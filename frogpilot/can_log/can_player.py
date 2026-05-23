#!/usr/bin/env python3
"""CAN Bus Player - rlogから全イベントを読み出してパブリッシュ（フルイベントリプレイ）

/data/media/0/realdata/ のrlogファイル（capnproto形式）から全イベントを
読み込み、オリジナルのタイミングを再現してパブリッシュする。

CAN_PLAYBACKモードでは、managerが他のシステムプロセスをスキップするため、
can_playerがrlog内のほぼ全イベントを安全にパブリッシュできる。
これにより、UIにcarState/controlsState/modelV2等が正しく表示される。

スキップするサービス:
  - initData: 通常のサービスではない
  - managerState: managerプロセスがパブリッシュするため衝突回避
  - Encode系: サイズが大きく再生に不要

使用例:
  python -m frogpilot.can_log.can_player /data/media/0/realdata/000001a3--c20ba54385
  python -m frogpilot.can_log.can_player --speed 2.0 /data/media/0/realdata/000001a3--c20ba54385
  python -m frogpilot.can_log.can_player --loop /data/media/0/realdata/000001a3--c20ba54385
"""

import os
import sys
import time
import argparse
import struct
import threading
from datetime import datetime, timezone
from pathlib import Path

import bz2
from cereal import log as capnp_log
import cereal.messaging as messaging

from openpilot.common.params import Params
from openpilot.common.swaglog import cloudlog

# 同一バッチとみなす最小タイムスタンプ差（秒）
BATCH_THRESHOLD_SEC = 0.001  # 1ms

# rlogファイル名（デバイス上はbz2圧縮）
RLOG_FILENAME = "rlog.bz2"
RLOG_FILENAME_UNCOMPRESSED = "rlog"

# フルイベントリプレイでスキップするサービス
# - initData: 初期化データ、通常サービスではない
# - managerState: managerプロセス自身がpublishするため衝突する
# - Encode系: サイズが大きく再生に不要
SKIP_SERVICES = {
  'initData',
  'managerState',
  'roadEncodeIdx', 'driverEncodeIdx', 'wideRoadEncodeIdx',
  'qRoadEncodeIdx', 'qDriverEncodeIdx', 'qWideEncodeIdx',
  'roadEncodeData', 'driverEncodeData', 'wideRoadEncodeData',
  'qRoadEncodeData',
  'livestreamWideRoadEncodeIdx', 'livestreamRoadEncodeIdx', 'livestreamDriverEncodeIdx',
  'livestreamWideRoadEncodeData', 'livestreamRoadEncodeData', 'livestreamDriverEncodeData',
}


def discover_segments(route_path: str) -> list[str]:
  """ルートディレクトリからセグメントディレクトリを検索

  2つのディレクトリ構造に対応:
    - フラット構造（commaデバイス）:
        /data/media/0/realdata/<route>--<dongle>--<segment>/rlog
        route_pathにいずれかのセグメントを指定すると、兄弟ディレクトリから
        同一ルートの全セグメントを自動検出する。
    - ネスト構造:
        /data/media/0/realdata/<route>/<segment>/rlog

  Args:
    route_path: ルートまたはセグメントディレクトリパス

  Returns:
    セグメントディレクトリパスのリスト（セグメント番号順）
  """
  route_dir = Path(route_path)
  if not route_dir.is_dir():
    return []

  # ヘルパー: ディレクトリにrlogが存在するか
  def _has_rlog(d: Path) -> bool:
    return (d / RLOG_FILENAME).exists() or (d / RLOG_FILENAME_UNCOMPRESSED).exists()

  # Case 1: フラット構造
  # ディレクトリ名が <route>--<dongle>--<segment_number> の形式かチェック
  dir_name = route_dir.name
  parts = dir_name.rsplit('--', 1)
  if len(parts) == 2 and parts[1].isdigit():
    route_prefix = parts[0]  # e.g. "00000007--33243391ae"
    parent = route_dir.parent
    segments: list[str] = []
    for entry in sorted(parent.iterdir()):
      if entry.is_dir() and entry.name.startswith(route_prefix + '--'):
        if _has_rlog(entry):
          segments.append(str(entry))
    if segments:
      return segments

  # Case 2: ネスト構造（セグメントがサブディレクトリとして存在）
  segments = []
  for entry in sorted(route_dir.iterdir()):
    if entry.is_dir() and _has_rlog(entry):
      segments.append(str(entry))

  # Case 3: route_path自体がrlogを含む単一セグメント
  if not segments and _has_rlog(route_dir):
    segments.append(str(route_dir))

  return segments


def _read_capnp_messages(data: bytes):
  """capnpのストリーミングフォーマットからメッセージの生バイトを抽出

  capnpのストリーミングフォーマットでは各メッセージが:
    - ヘッダー: セグメント数(4bytes) + 各セグメントのサイズ(4bytes×N)
    - パディングで8バイト境界
    - セグメントデータ
  という構造になっている。

  Yields:
    各メッセージの生バイト (bytes)
  """
  offset = 0
  while offset < len(data):
    # 最初の4バイト: (セグメント数 - 1)
    if offset + 4 > len(data):
      break
    seg_count = struct.unpack('<I', data[offset:offset+4])[0] + 1

    # 各セグメントのサイズ（8バイト単位）
    sizes_start = offset + 4
    if sizes_start + seg_count * 4 > len(data):
      break

    total_data_bytes = 0
    for i in range(seg_count):
      seg_words = struct.unpack('<I', data[sizes_start + i*4:sizes_start + i*4 + 4])[0]
      total_data_bytes += seg_words * 8

    # ヘッダーサイズ（8バイト境界にパディング）
    header_size = 4 + seg_count * 4
    header_size_padded = (header_size + 7) & ~7

    # メッセージ全体のサイズ
    msg_size = header_size_padded + total_data_bytes

    if offset + msg_size > len(data):
      break

    yield data[offset:offset+msg_size]
    offset += msg_size


def read_all_events_from_rlog(rlog_path: str) -> list[tuple]:
  """rlogファイルから全イベントを読み込む

  Args:
    rlog_path: rlogファイルパス

  Returns:
    [(logMonoTime, event_type, raw_bytes), ...] のリスト
  """
  with open(rlog_path, 'rb') as f:
    dat = f.read()

  # bz2圧縮チェック
  if dat.startswith(b'BZh9'):
    dat = bz2.decompress(dat)

  events = []
  for raw_msg in _read_capnp_messages(dat):
    try:
      with capnp_log.Event.from_bytes(raw_msg, traversal_limit_in_words=2**24) as msg:
        event_type = msg.which()
        log_mono_time = msg.logMonoTime
        events.append((log_mono_time, event_type, raw_msg))
    except Exception as e:
      cloudlog.exception(f"Failed to parse event: {e}")
      continue

  return events


def extract_recording_time(events: list[tuple]) -> int | None:
  """イベントリストから録画時刻を取得

  優先順位:
    1. clocks.wallTimeNanos（UNIXエポック時間、ナノ秒）
    2. gpsLocation.timestamp（UNIXエポック時間、秒→ナノ秒に変換）

  Args:
    events: イベントリスト

  Returns:
    wallTimeNanos（ナノ秒）、見つからなければNone
  """
  # 1. clocksイベントからwallTimeNanosを取得（最も正確）
  for log_mono_time, event_type, raw_msg in events:
    if event_type == 'clocks':
      try:
        with capnp_log.Event.from_bytes(raw_msg, traversal_limit_in_words=2**24) as msg:
          return msg.clocks.wallTimeNanos
      except Exception:
        continue

  # 2. フォールバック: gpsLocationイベントからtimestampを取得
  for log_mono_time, event_type, raw_msg in events:
    if event_type == 'gpsLocation':
      try:
        with capnp_log.Event.from_bytes(raw_msg, traversal_limit_in_words=2**24) as msg:
          gps_time = msg.gpsLocation.timestamp  # UNIX timestamp (秒、float)
          if gps_time > 0:
            cloudlog.info(f"CAN playback: using GPS time as fallback: {gps_time}")
            return int(gps_time * 1e9)  # ナノ秒に変換
      except Exception:
        continue

  return None


def load_all_events(route_path: str) -> tuple[list[tuple], set[str], int | None]:
  """ルート全体のイベントを全セグメントから読み込む

  ローディング進捗をCanPlaybackLoadingProgress Paramsに書き込む。

  Args:
    route_path: ルートディレクトリパス

  Returns:
    (events, service_names, recording_time_ns) のタプル
    - events: タイムスタンプ順にソートされたイベントのリスト
    - service_names: パブリッシュが必要なサービス名のセット
    - recording_time_ns: 録画開始時刻のwallTimeNanos（ナノ秒）、なければNone
  """
  params = Params()
  params.put("CanPlaybackLoadingProgress", "0")

  segments = discover_segments(route_path)
  if not segments:
    cloudlog.warning(f"No segments found in {route_path}")
    params.put("CanPlaybackLoadingProgress", "50")  # rlog完了=50%, デコードはスキップ
    return [], set(), None

  all_events = []
  total_segments = len(segments)
  for i, seg_path in enumerate(segments):
    # rlog読み込み進捗: 0-50%の範囲（残り50%はvideo_playerのデコード）
    progress = int((i / total_segments) * 50)
    params.put("CanPlaybackLoadingProgress", str(progress))

    seg_dir = Path(seg_path)
    rlog_path = seg_dir / RLOG_FILENAME
    if not rlog_path.exists():
      rlog_path = seg_dir / RLOG_FILENAME_UNCOMPRESSED
    if not rlog_path.exists():
      continue

    cloudlog.info(f"Loading events from {rlog_path} ({i+1}/{total_segments})")
    events = read_all_events_from_rlog(str(rlog_path))
    all_events.extend(events)

  # タイムスタンプでソート
  all_events.sort(key=lambda x: x[0])

  # パブリッシュが必要なサービス名を収集
  service_names = set()
  for _, event_type, _ in all_events:
    if event_type not in SKIP_SERVICES:
      service_names.add(event_type)

  # 録画日時をclocksイベントから取得
  recording_time_ns = extract_recording_time(all_events)

  # rlog読み込み完了 = 50%（残り50%はvideo_playerのフレームデコード）
  params.put("CanPlaybackLoadingProgress", "50")

  cloudlog.info(f"Loaded {len(all_events)} events, {len(service_names)} services to publish")
  cloudlog.info(f"Services: {sorted(service_names)}")

  return all_events, service_names, recording_time_ns


class CanPlayer:
  """rlogから全イベントを読み出し、フルイベントリプレイするクラス

  managerがCAN_PLAYBACKモードで他のシステムプロセスをスキップするため、
  can_playerがrlog内のほぼ全イベントを安全にパブリッシュできる。
  """

  def __init__(self, route_path: str, speed: float = 1.0, loop: bool = False):
    """
    Args:
      route_path: ルートディレクトリパス（/data/media/0/realdata/...）
      speed: 再生速度（1.0=実時間、2.0=2倍速）
      loop: ループ再生するか
    """
    self.route_path = route_path
    self.speed = speed
    self.loop = loop
    self.params = Params()

    self._paused = False
    self._stop = False
    self._current_time_ns = 0
    self._lock = threading.Lock()

    # ルート全体のイベントをロード（進捗をParamsに書き込み）
    self._events, self._service_names, self._recording_time_ns = load_all_events(route_path)
    if not self._events:
      raise ValueError(f"No events found in {route_path}")

    # PubMasterをフルイベントサービスで初期化
    cloudlog.info(f"CAN playback: publishing {len(self._service_names)} services: {sorted(self._service_names)}")
    self.pm = messaging.PubMaster(sorted(self._service_names))

    self._start_time_ns = self._events[0][0]
    self._end_time_ns = self._events[-1][0]
    self._event_count = len(self._events)
    self._duration = (self._end_time_ns - self._start_time_ns) / 1e9

    # 録画日時を設定
    # 1. clocks.wallTimeNanosから取得（最も正確）
    # 2. フォールバック: ファイルのmtime
    if self._recording_time_ns is not None:
      recording_dt = datetime.fromtimestamp(self._recording_time_ns / 1e9, tz=timezone.utc)
      self._recording_time_str = recording_dt.strftime('%Y-%m-%d %H:%M')
    else:
      # フォールバック: セグメントディレクトリの変更日時
      segments = discover_segments(route_path)
      if segments:
        recording_timestamp = os.path.getmtime(segments[0])
        recording_dt = datetime.fromtimestamp(recording_timestamp, tz=timezone.utc)
        self._recording_time_str = recording_dt.strftime('%Y-%m-%d %H:%M')
      else:
        self._recording_time_str = ""

  @property
  def duration_seconds(self) -> float:
    """ログの総時間（秒）"""
    return self._duration

  @property
  def start_time(self) -> float:
    """ログの開始UNIXタイムスタンプ（秒）"""
    return self._start_time_ns / 1e9

  @property
  def current_position_seconds(self) -> float:
    """現在の再生位置（秒、ログ先頭からの相対時間）"""
    with self._lock:
      if self._start_time_ns == 0:
        return 0.0
      return (self._current_time_ns - self._start_time_ns) / 1e9

  @property
  def real_time_str(self) -> str:
    """録画日時を文字列で返す (YYYY-MM-DD HH:MM)"""
    return self._recording_time_str

  @property
  def is_paused(self) -> bool:
    return self._paused

  @property
  def event_count(self) -> int:
    return self._event_count

  @property
  def message_count(self) -> int:
    """後方互換: event_countのエイリアス"""
    return self._event_count

  def pause(self):
    """再生を一時停止"""
    self._paused = True
    self._update_params_state()

  def resume(self):
    """再生を再開"""
    self._paused = False
    self._update_params_state()

  def toggle_pause(self):
    """一時停止/再開を切り替え"""
    self._paused = not self._paused

  def stop(self):
    """再生を停止"""
    self._stop = True
    self.params.put("CanPlaybackPlaying", "0")
    self.params.remove("CAN_PLAYBACK")

  def _update_params_state(self) -> None:
    """Paramsに現在の再生状態を書き込む"""
    with self._lock:
      position = (self._current_time_ns - self._start_time_ns) / 1e9 if self._start_time_ns else 0.0
    self.params.put("CanPlaybackPosition", str(position))
    self.params.put("CanPlaybackDuration", str(self._duration))
    self.params.put("CanPlaybackSpeed", str(self.speed))
    self.params.put("CanPlaybackPlaying", "0" if self._paused else "1")
    self.params.put("CanPlaybackRealTime", self._recording_time_str)

  def _check_params_commands(self) -> None:
    """ParamsからUI側のコマンドを確認して処理する"""
    # シークコマンドの確認
    seek_cmd = self.params.get("CanPlaybackSeek")
    if seek_cmd is not None:
      try:
        seek_pos = float(seek_cmd)
        cloudlog.info(f"CAN playback: seeking to {seek_pos:.2f}s")
        self.params.remove("CanPlaybackSeek")
      except (ValueError, TypeError):
        pass

    # 一時停止/再生コマンド
    pause_cmd = self.params.get("CanPlaybackPause")
    if pause_cmd == "1" and not self._paused:
      self._paused = True
      self.params.remove("CanPlaybackPause")
    elif pause_cmd == "0" and self._paused:
      self._paused = False
      self.params.remove("CanPlaybackPause")

    # 速度変更コマンド
    speed_cmd = self.params.get("CanPlaybackSpeedCmd")
    if speed_cmd is not None:
      try:
        new_speed = float(speed_cmd)
        if new_speed > 0:
          self.speed = new_speed
          cloudlog.info(f"CAN playback: speed changed to {new_speed}x")
        self.params.remove("CanPlaybackSpeedCmd")
      except (ValueError, TypeError):
        self.params.remove("CanPlaybackSpeedCmd")

  def run(self):
    """メイン再生ループ

    rlogから読み込んだ全イベントをオリジナルのタイミングでパブリッシュする。
    SKIP_SERVICESに含まれるイベントはパブリッシュしない。
    """
    # video_playerのデコード完了を待機（CanPlaybackLoadingProgress == 100）
    cloudlog.info("CAN playback: waiting for video decode to complete...")
    while not self._stop:
      progress = self.params.get("CanPlaybackLoadingProgress")
      if progress is not None:
        try:
          if int(progress) >= 100:
            break
        except (ValueError, TypeError):
          pass
      time.sleep(0.2)
    cloudlog.info("CAN playback: video decode complete, starting playback")

    # 初期状態をParamsに書き込む
    self._update_params_state()

    while not self._stop:
      log_start_ns = None
      playback_start = None
      total_paused_duration = 0.0
      last_params_update = time.monotonic()

      for log_mono_time, event_type, raw_bytes in self._events:
        if self._stop:
          break

        # UIコマンドの確認と状態同期（100ms間隔）
        now = time.monotonic()
        if now - last_params_update > 0.1:
          self._check_params_commands()
          self._update_params_state()
          last_params_update = now

        # 一時停止処理
        if self._paused:
          self._update_params_state()
          pause_begin = time.monotonic()
          while self._paused and not self._stop:
            time.sleep(0.05)
            self._check_params_commands()
          if self._stop:
            break
          total_paused_duration += time.monotonic() - pause_begin

        with self._lock:
          self._current_time_ns = log_mono_time

        # 最初のイベント：再生開始基準を設定
        if log_start_ns is None:
          log_start_ns = log_mono_time
          playback_start = time.monotonic()

        # タイミング計算
        effective_elapsed = time.monotonic() - playback_start - total_paused_duration
        log_elapsed_sec = (log_mono_time - log_start_ns) / 1e9
        target_elapsed = log_elapsed_sec / self.speed

        sleep_needed = target_elapsed - effective_elapsed

        if sleep_needed > BATCH_THRESHOLD_SEC:
          time.sleep(sleep_needed)

        # イベントをパブリッシュ（スキップ対象以外）
        if event_type in self._service_names:
          try:
            self.pm.send(event_type, raw_bytes)
          except Exception as e:
            cloudlog.error(f"CAN playback: error sending {event_type}: {e}")

      if not self.loop:
        break

      cloudlog.info("CAN playback loop: restarting from beginning")

    cloudlog.info("CAN playback finished")
    # Clean up CAN_PLAYBACK flag so manager returns to offroad
    self.params.remove("CAN_PLAYBACK")


def main():
  parser = argparse.ArgumentParser(
    description='CAN Bus Log Player - rlogから全イベントをパブリッシュ（フルイベントリプレイ）',
    formatter_class=argparse.RawDescriptionHelpFormatter,
    epilog="""
使用例:
  # ルートディレクトリを指定して再生
  python -m frogpilot.can_log.can_player /data/media/0/realdata/000001a3--c20ba54385

  # 2倍速再生
  python -m frogpilot.can_log.can_player --speed 2.0 /data/media/0/realdata/000001a3--c20ba54385

  # ループ再生
  python -m frogpilot.can_log.can_player --loop /data/media/0/realdata/000001a3--c20ba54385
""")
  parser.add_argument('route_path', help='Route directory path (e.g., /data/media/0/realdata/000001a3--c20ba54385)')
  parser.add_argument('--speed', type=float, default=1.0,
                      help='Playback speed multiplier (default: 1.0)')
  parser.add_argument('--loop', action='store_true',
                      help='Loop playback')
  args = parser.parse_args()

  if not os.path.isdir(args.route_path):
    print(f"Error: Directory not found: {args.route_path}")
    sys.exit(1)

  try:
    player = CanPlayer(args.route_path, speed=args.speed, loop=args.loop)
  except ValueError as e:
    print(f"Error: {e}")
    sys.exit(1)

  route_name = Path(args.route_path).name
  print("CAN Log Player (Full Event Replay)")
  print(f"  Route:      {route_name}")
  print(f"  Duration:   {player.duration_seconds:.2f}s")
  print(f"  Events:     {player.event_count}")
  print(f"  Services:   {len(player._service_names)}")
  print(f"  Speed:      {args.speed}x")
  print(f"  Loop:       {args.loop}")
  print(f"  Start time: {datetime.fromtimestamp(player.start_time, tz=timezone.utc).strftime('%Y-%m-%d %H:%M:%S')}")
  print(f"  Recorded:   {player.real_time_str}")
  print()

  try:
    player.run()
  except KeyboardInterrupt:
    player.stop()
    print("\nPlayback stopped.")


if __name__ == "__main__":
  main()
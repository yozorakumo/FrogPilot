#!/usr/bin/env python3
"""CAN Bus Player - rlogからCANメッセージを読み出してcanサービスにパブリッシュ

/data/media/0/realdata/ のrlogファイル（capnproto形式）からCANメッセージを
読み込み、オリジナルのタイミングを再現してcanサービスにパブリッシュする。

使用例:
  # ルートディレクトリを指定して再生
  python -m frogpilot.can_log.can_player /data/media/0/realdata/000001a3--c20ba54385

  # 2倍速再生
  python -m frogpilot.can_log.can_player --speed 2.0 /data/media/0/realdata/000001a3--c20ba54385

  # ループ再生
  python -m frogpilot.can_log.can_player --loop /data/media/0/realdata/000001a3--c20ba54385
"""

import os
import sys
import time
import argparse
import threading
from datetime import datetime, timezone
from pathlib import Path

import bz2
from cereal import log as capnp_log
import cereal.messaging as messaging

from openpilot.common.params import Params
from openpilot.selfdrive.pandad import can_list_to_can_capnp
from openpilot.common.swaglog import cloudlog

# 同一バッチとみなす最小タイムスタンプ差（秒）
BATCH_THRESHOLD_SEC = 0.001  # 1ms

# rlogファイル名（デバイス上はbz2圧縮）
RLOG_FILENAME = "rlog.bz2"
RLOG_FILENAME_UNCOMPRESSED = "rlog"


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


def read_can_messages_from_rlog(rlog_path: str) -> list[tuple]:
  """rlogファイルからCANメッセージを読み込む

  Args:
    rlog_path: rlogファイルパス

  Returns:
    [(logMonoTime, can_addr, busTime, dat_bytes, src_bus), ...] のリスト
  """
  with open(rlog_path, 'rb') as f:
    dat = f.read()

  # bz2圧縮チェック
  if dat.startswith(b'BZh9'):
    dat = bz2.decompress(dat)

  messages = []
  ents = capnp_log.Event.read_multiple_bytes(dat)

  for ent in ents:
    try:
      if ent.which() == 'can':
        log_mono_time = ent.logMonoTime
        for can_msg in ent.can:
          messages.append((
            log_mono_time,
            can_msg.address,
            can_msg.busTime,
            bytes(can_msg.dat),
            can_msg.src,
          ))
    except Exception:
      continue

  return messages


def load_route_can_data(route_path: str) -> list[tuple]:
  """ルート全体のCANデータを全セグメントから読み込む

  Args:
    route_path: ルートディレクトリパス

  Returns:
    タイムスタンプ順にソートされたCANメッセージのリスト
  """
  segments = discover_segments(route_path)
  if not segments:
    cloudlog.warning(f"No segments found in {route_path}")
    return []

  all_messages = []
  for seg_path in segments:
    seg_dir = Path(seg_path)
    rlog_path = seg_dir / RLOG_FILENAME
    if not rlog_path.exists():
      rlog_path = seg_dir / RLOG_FILENAME_UNCOMPRESSED
    if not rlog_path.exists():
      continue

    cloudlog.info(f"Loading CAN data from {rlog_path}")
    msgs = read_can_messages_from_rlog(str(rlog_path))
    all_messages.extend(msgs)

  # タイムスタンプでソート
  all_messages.sort(key=lambda x: x[0])
  return all_messages


class CanPlayer:
  """rlogからCANメッセージを読み出し、canサービスにパブリッシュするクラス"""

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
    self.pm = messaging.PubMaster(['can'])
    self.params = Params()

    self._paused = False
    self._stop = False
    self._current_time_ns = 0
    self._lock = threading.Lock()

    # ルート全体のCANデータをロード
    self._can_messages = load_route_can_data(route_path)
    if not self._can_messages:
      raise ValueError(f"No CAN messages found in {route_path}")

    self._start_time_ns = self._can_messages[0][0]
    self._end_time_ns = self._can_messages[-1][0]
    self._message_count = len(self._can_messages)
    self._duration = (self._end_time_ns - self._start_time_ns) / 1e9

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
    """ログの現実時間を文字列で返す"""
    with self._lock:
      ts = self._current_time_ns / 1e9
    return datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S.%f")

  @property
  def is_paused(self) -> bool:
    return self._paused

  @property
  def message_count(self) -> int:
    return self._message_count

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
    self.params.put("CanPlaybackRealTime", self.real_time_str)

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

  def _send_batch(self, batch: list):
    """CANメッセージのバッチをパブリッシュ

    Args:
      batch: can_list_to_can_capnp形式のメッセージリスト
    """
    if not batch:
      return
    can_bytes = can_list_to_can_capnp(batch, msgtype='can', valid=True)
    self.pm.send('can', can_bytes)

  def run(self):
    """メイン再生ループ

    rlogから読み込んだCANメッセージを、オリジナルのタイミングを再現しながら
    canサービスにパブリッシュする。
    """
    # 初期状態をParamsに書き込む
    self._update_params_state()

    while not self._stop:
      batch: list[list] = []
      log_start_ns = None
      playback_start = None
      total_paused_duration = 0.0
      last_params_update = time.monotonic()

      for msg in self._can_messages:
        if self._stop:
          break

        log_mono_time, can_addr, bus_time, dat_bytes, src_bus = msg

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

        # 最初のメッセージ：再生開始基準を設定
        if log_start_ns is None:
          log_start_ns = log_mono_time
          playback_start = time.monotonic()
          batch.append([can_addr, bus_time, dat_bytes, src_bus])
          continue

        # タイミング計算
        effective_elapsed = time.monotonic() - playback_start - total_paused_duration
        log_elapsed_sec = (log_mono_time - log_start_ns) / 1e9
        target_elapsed = log_elapsed_sec / self.speed

        sleep_needed = target_elapsed - effective_elapsed

        if sleep_needed > BATCH_THRESHOLD_SEC:
          # タイムギャップあり → 蓄積したバッチを送信して待機
          if batch:
            self._send_batch(batch)
            batch = []

          remaining = target_elapsed - (time.monotonic() - playback_start - total_paused_duration)
          if remaining > 0:
            time.sleep(remaining)

        batch.append([can_addr, bus_time, dat_bytes, src_bus])

      # 最後のバッチを送信
      if batch:
        self._send_batch(batch)

      if not self.loop:
        break

      cloudlog.info("CAN playback loop: restarting from beginning")

    cloudlog.info("CAN playback finished")
    # Clean up CAN_PLAYBACK flag so manager returns to offroad
    self.params.remove("CAN_PLAYBACK")


def main():
  parser = argparse.ArgumentParser(
    description='CAN Bus Log Player - rlogからCANメッセージを読み出してcanサービスにパブリッシュ',
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
  print("CAN Log Player (rlog)")
  print(f"  Route:      {route_name}")
  print(f"  Duration:   {player.duration_seconds:.2f}s")
  print(f"  Messages:   {player.message_count}")
  print(f"  Speed:      {args.speed}x")
  print(f"  Loop:       {args.loop}")
  print(f"  Start time: {datetime.fromtimestamp(player.start_time, tz=timezone.utc).strftime('%Y-%m-%d %H:%M:%S')}")
  print()

  try:
    player.run()
  except KeyboardInterrupt:
    player.stop()
    print("\nPlayback stopped.")


if __name__ == "__main__":
  main()
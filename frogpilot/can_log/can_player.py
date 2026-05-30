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

from __future__ import annotations

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


def is_valid_recording_time(dt: datetime) -> bool:
  """録画日時の妥当性をチェック

  未来の日時、または2015年以前は無効とみなす。

  Args:
    dt: チェックするdatetimeオブジェクト

  Returns:
    有効な場合はTrue、無効な場合はFalse
  """
  now = datetime.now(timezone.utc)
  # 未来の日時、または2015年以前は無効
  if dt > now or dt.year < 2015:
    return False
  return True


def _extract_segment_timestamps(seg_path: str) -> tuple[int | None, int | None]:
  """1つのセグメントからlogMonoTimeとGPS時刻を抽出

  Returns:
    (first_log_mono_ns, gps_time_ns) のタプル
    取得できない場合は(None, None)
  """
  seg_dir = Path(seg_path)
  rlog_path = seg_dir / RLOG_FILENAME
  if not rlog_path.exists():
    rlog_path = seg_dir / RLOG_FILENAME_UNCOMPRESSED
  if not rlog_path.exists():
    return None, None

  try:
    events = read_all_events_from_rlog(str(rlog_path))
  except Exception:
    return None, None

  first_log_mono = None
  gps_time_ns = None

  for log_mono_time, event_type, raw_msg in events:
    if first_log_mono is None:
      first_log_mono = log_mono_time

    if gps_time_ns is None:
      if event_type == 'clocks':
        try:
          with capnp_log.Event.from_bytes(raw_msg, traversal_limit_in_words=2**24) as msg:
            wall_time_ns = msg.clocks.wallTimeNanos
            dt = datetime.fromtimestamp(wall_time_ns / 1e9, tz=timezone.utc)
            if is_valid_recording_time(dt):
              gps_time_ns = wall_time_ns
              break
        except Exception:
          continue

      if event_type == 'gpsLocationExternal':
        try:
          with capnp_log.Event.from_bytes(raw_msg, traversal_limit_in_words=2**24) as msg:
            if msg.gpsLocationExternal.hasFix:
              ts = msg.gpsLocationExternal.unixTimestampMillis
              if ts > 0:
                dt = datetime.fromtimestamp(ts / 1000, tz=timezone.utc)
                if is_valid_recording_time(dt):
                  gps_time_ns = int(ts * 1e6)
                  break
        except Exception:
          continue

      if event_type == 'gpsLocation':
        try:
          with capnp_log.Event.from_bytes(raw_msg, traversal_limit_in_words=2**24) as msg:
            if msg.gpsLocation.hasFix:
              ts = msg.gpsLocation.unixTimestampMillis
              if ts > 0:
                dt = datetime.fromtimestamp(ts / 1000, tz=timezone.utc)
                if is_valid_recording_time(dt):
                  gps_time_ns = int(ts * 1e6)
                  break
        except Exception:
          continue

  return first_log_mono, gps_time_ns


def calculate_timestamp_corrections(segments: list[str]) -> dict[str, int]:
  """全セグメントのタイムスタンプ補正値を計算

  端末起動後、GPS fix取得前に記録されたセグメントはRTCデフォルト値により
  不正なタイムスタンプを持っている。この関数はGPS fixが得られたセグメントを
  基準にして起動時刻からのオフセットを計算し、全セグメントの正しい
  タイムスタンプを算出する。

  ロジック:
    1. 各セグメントの最初のイベントからlogMonoTimeを取得
    2. そのセグメント内のGPS fix後のclocksまたはgpsLocationから真正の時刻を取得
    3. オフセット = (GPS時刻 - logMonoTime_boot) を計算
    4. 最も古いオフセットを基準にして全セグメントに適用

  Args:
    segments: セグメントディレクトリパスのリスト

  Returns:
    {segment_path: corrected_timestamp_ns} の辞書
    補正できないセグメントは含まれない
  """
  if not segments:
    return {}

  segment_data = {}
  for seg_path in segments:
    first_log_mono, gps_time_ns = _extract_segment_timestamps(seg_path)
    if first_log_mono is not None:
      segment_data[seg_path] = {'first_log_mono': first_log_mono, 'gps_time': gps_time_ns}

  valid_offsets = []
  for seg_path, data in segment_data.items():
    if data['gps_time'] is not None:
      offset = data['gps_time'] - data['first_log_mono']
      valid_offsets.append(offset)

  if not valid_offsets:
    return {}

  reference_offset = min(valid_offsets)

  corrected_timestamps = {}
  for seg_path, data in segment_data.items():
    corrected_timestamps[seg_path] = data['first_log_mono'] + reference_offset

  return corrected_timestamps


def get_route_start_time(segments: list[str]) -> int | None:
  """ルートの開始時刻（GPS補正済み）を取得

  すべてのセグメントから最も古い補正済みタイムスタンプを返す。

  Args:
    segments: セグメントディレクトリパスのリスト

  Returns:
    ルート開始時刻のwallTimeNanos（ナノ秒）、なければNone
  """
  corrections = calculate_timestamp_corrections(segments)
  if corrections:
    return min(corrections.values())
  return None


def _find_valid_gps_timestamp_in_data(dat: bytes) -> int | None:
  """生のバイナリデータから直接GPSタイムスタンプを探す

  clocksイベントやgpsLocationExternalイベントのパースに失敗した場合にフォールバックとして使用。
  バイナリデータ内のunixTimestampMillis値（ミリ秒単位）を直接検索し、
  2023年以降のGPS fix確立後の最初の一致を返す。

  RTCデフォルト値は2022年以前のため、2023年以降でフィルタリングすることで
  GPS fix前の不正なタイムスタンプを除外する。

  Args:
    dat: rlogファイルの内容（解凍済み）

  Returns:
    wallTimeNanos（ナノ秒）、見つからなければNone
  """
  import struct

  # unixTimestampMillisの範囲（2020-01-01から2030-01-01）
  MIN_TS_MS = 1577836800000  # 2020-01-01
  MAX_TS_MS = 1767225600000  # 2030-01-01

  # 2023年以降でGPS fix後のタイムスタンプのみを収集
  candidates = []
  offset = 0
  while offset < len(dat) - 8:
    try:
      # リトルエンディアンの64ビット符号なし整数として読取
      val = struct.unpack('<Q', dat[offset:offset+8])[0]

      # 有効なunixTimestampMillis範囲内かチェック
      if MIN_TS_MS < val < MAX_TS_MS:
        try:
          dt = datetime.fromtimestamp(val / 1000, tz=timezone.utc)
          if is_valid_recording_time(dt):
            # 2023年以降を優先（GPS fix後のタイムスタンプ）
            if dt.year >= 2023:
              candidates.append((offset, val, dt))
        except (ValueError, OSError):
          pass
    except struct.error:
      pass

    offset += 1

  # 2023年以降で最初に見つかったGPS fix後のタイムスタンプを返す
  if candidates:
    # ファイルオフセット順でソートして最初のものを返す
    candidates.sort()
    return int(candidates[0][1] * 1e6)

  return None


def extract_recording_time(events: list[tuple], segments: list[str] = None) -> int | None:
  """イベントリストから録画時刻を取得

  優先順位:
    1. clocks.wallTimeNanos（UNIXエポック時間、ナノ秒）- 最も正確
       ※ RTCデフォルト値等原因で未来の日付になる場合は無効としてスキップ
    2. gpsLocationExternal.unixTimestampMillis（hasFix=true の最初のイベント）
    3. gpsLocation.unixTimestampMillis（hasFix=true の最初のイベント）
    4. 生のバイナリデータから直接GPSタイムスタンプを検索
    5. セグメントのディレクトリ名から日時を抽出（例: 2025-05-30--08-30-00）
    6. セグメントディレクトリのmtime

  GPS fixが未確定のイベント（hasFix=false）のタイムスタンプは無視する。
  これにより、RTCデフォルト値等原因でGPS時刻が不正な場合でも、
  正しいfix後のタイムスタンプが使用される。

  Args:
    events: イベントリスト
    segments: セグメントディレクトリパスのリスト（ディレクトリ名からの日時抽出用）

  Returns:
    wallTimeNanos（ナノ秒）、見つからなければNone
  """
  # 1. clocksイベントからwallTimeNanosを取得（最も正確）
  # 問題4修正: 未来の日付になる場合は無効としてスキップ
  for log_mono_time, event_type, raw_msg in events:
    if event_type == 'clocks':
      try:
        with capnp_log.Event.from_bytes(raw_msg, traversal_limit_in_words=2**24) as msg:
          wall_time_ns = msg.clocks.wallTimeNanos
          dt = datetime.fromtimestamp(wall_time_ns / 1e9, tz=timezone.utc)
          if is_valid_recording_time(dt):
            return wall_time_ns
          else:
            cloudlog.warning(f"CAN playback: clocks.wallTimeNanos is invalid (future or too old): {dt}")
      except Exception:
        continue

  # 2. gpsLocationExternal.unixTimestampMillisを使用（hasFix=true の最初のイベント）
  for log_mono_time, event_type, raw_msg in events:
    if event_type == 'gpsLocationExternal':
      try:
        with capnp_log.Event.from_bytes(raw_msg, traversal_limit_in_words=2**24) as msg:
          # GPS fixが有効な場合のみタイムスタンプを使用
          if msg.gpsLocationExternal.hasFix:
            ts = msg.gpsLocationExternal.unixTimestampMillis  # UNIX timestamp (ミリ秒)
            if ts > 0:
              dt = datetime.fromtimestamp(ts / 1000, tz=timezone.utc)
              if is_valid_recording_time(dt):
                cloudlog.info(f"CAN playback: using gpsLocationExternal time: {ts}")
                return int(ts * 1e6)  # ミリ秒→ナノ秒に変換
              else:
                cloudlog.warning(f"CAN playback: gpsLocationExternal time is invalid: {dt}")
      except Exception:
        continue

  # 3. フォールバック: gpsLocation.unixTimestampMillisを使用（hasFix=true の最初のイベント）
  for log_mono_time, event_type, raw_msg in events:
    if event_type == 'gpsLocation':
      try:
        with capnp_log.Event.from_bytes(raw_msg, traversal_limit_in_words=2**24) as msg:
          # GPS fixが有効な場合のみタイムスタンプを使用
          if msg.gpsLocation.hasFix:
            ts = msg.gpsLocation.unixTimestampMillis  # UNIX timestamp (ミリ秒)
            if ts > 0:
              dt = datetime.fromtimestamp(ts / 1000, tz=timezone.utc)
              if is_valid_recording_time(dt):
                cloudlog.info(f"CAN playback: using gpsLocation time: {ts}")
                return int(ts * 1e6)  # ミリ秒→ナノ秒に変換
              else:
                cloudlog.warning(f"CAN playback: gpsLocation time is invalid: {dt}")
      except Exception:
        continue

  # 4. フォールバック: 生のバイナリデータから直接GPSタイムスタンプを検索
  if segments:
    for seg_path in segments:
      rlog_path = os.path.join(seg_path, RLOG_FILENAME)
      if not os.path.exists(rlog_path):
        rlog_path = os.path.join(seg_path, RLOG_FILENAME_UNCOMPRESSED)
      if os.path.exists(rlog_path):
        try:
          with open(rlog_path, 'rb') as f:
            dat = f.read(8 * 1024 * 1024)  # 先頭8MBを読み込み
          if dat.startswith(b'BZh9'):
            dat = bz2.decompress(dat)
          ts_ns = _find_valid_gps_timestamp_in_data(dat)
          if ts_ns is not None:
            dt = datetime.fromtimestamp(ts_ns / 1e9, tz=timezone.utc)
            cloudlog.info(f"CAN playback: using raw GPS timestamp: {dt}")
            return ts_ns
        except Exception as e:
          cloudlog.warning(f"CAN playback: failed to extract raw GPS timestamp: {e}")
        break

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
    params.put("CanPlaybackLoadingProgress", "100")  # rlog完了=100%, デコードはスキップ
    return [], set(), None

  all_events = []
  total_segments = len(segments)

  # 問題2修正: rlog読み込み進捗をセグメントごと＋イベント読み込み中にも更新
  for i, seg_path in enumerate(segments):
    # セグメント処理開始時の進捗更新
    progress = int((i / total_segments) * 100)
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

    # イベント読み込み完了後に進捗を更新（セグメント処理途中での更新）
    # より細かく進捗を更新するために、読込後の進捗も通知
    progress = int(((i + 0.8) / total_segments) * 100)
    params.put("CanPlaybackLoadingProgress", str(progress))

  # タイムスタンプでソート
  all_events.sort(key=lambda x: x[0])

  # パブリッシュが必要なサービス名を収集
  service_names = set()
  for _, event_type, _ in all_events:
    if event_type not in SKIP_SERVICES:
      service_names.add(event_type)

  # 録画日時をclocksイベントから取得（segmentsを渡してディレクトリ名からも日時抽出）
  recording_time_ns = extract_recording_time(all_events, segments)

  # rlog読み込み完了 = 100%（video_playerのデコード進捗はCanPlaybackDecodeProgressで管理）
  params.put("CanPlaybackLoadingProgress", "100")

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
    self._current_time_ns = self._start_time_ns  # 初期位置を開始時刻に同期（負のposition防止）
    self._end_time_ns = self._events[-1][0]
    self._event_count = len(self._events)
    self._duration = (self._end_time_ns - self._start_time_ns) / 1e9

    # 録画日時を設定（GPS補正済み）
    # calculate_timestamp_correctionsを使ってRTCデフォルト値による不正なタイムスタンプを補正
    segments = discover_segments(route_path)
    if segments:
      corrected_time = get_route_start_time(segments)
      if corrected_time is not None:
        self._recording_time_ns = corrected_time
      else:
        self._recording_time_ns = None

    if self._recording_time_ns is not None:
      recording_dt = datetime.fromtimestamp(self._recording_time_ns / 1e9)
      self._recording_time_str = recording_dt.strftime('%Y-%m-%d %H:%M')
    else:
      if segments:
        recording_timestamp = os.path.getmtime(segments[0])
        recording_dt = datetime.fromtimestamp(recording_timestamp)
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
    """Paramsに現在の再生状態を書き込む（個別キー方式）"""
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
    # 前回の実行からの古い状態をリセット
    self.params.put("CanPlaybackPlaying", "0")
    cloudlog.info("CAN playback: reset CanPlaybackPlaying, starting immediately")

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
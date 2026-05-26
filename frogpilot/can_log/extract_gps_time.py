#!/usr/bin/env python3
"""rlogファイルからGPS録画時刻を抽出する軽量スクリプト

capnpログからclocks.wallTimeNanosまたはgpsLocation.unixTimestampMillisを抽出して
UNIXタイムスタンプ（秒）をstdoutに出力する。

使用例:
  python extract_gps_time.py /path/to/rlog.bz2
"""

import sys
import struct

try:
  import bz2
  from cereal import log as capnp_log
except ImportError:
  print("")
  sys.exit(0)


def read_capnp_messages(data):
  """capnpのストリーミングフォーマットからメッセージの生バイトを抽出"""
  offset = 0
  while offset < len(data):
    if offset + 4 > len(data):
      break
    seg_count = struct.unpack('<I', data[offset:offset+4])[0] + 1

    sizes_start = offset + 4
    if sizes_start + seg_count * 4 > len(data):
      break

    total_data_bytes = 0
    for i in range(seg_count):
      seg_words = struct.unpack('<I', data[sizes_start + i*4:sizes_start + i*4 + 4])[0]
      total_data_bytes += seg_words * 8

    header_size = 4 + seg_count * 4
    header_size_padded = (header_size + 7) & ~7

    msg_size = header_size_padded + total_data_bytes

    if offset + msg_size > len(data):
      break

    yield data[offset:offset+msg_size]
    offset += msg_size


def extract_gps_time(rlog_path):
  """rlogファイルからGPS録画時刻（UNIXタイムスタンプ秒）を抽出

  優先順位:
    1. clocks.wallTimeNanos（UNIXエポック時間、ナノ秒）- 最も正確
    2. gpsLocationExternal.unixTimestampMillis（hasFix=true の最初のイベント）
    3. gpsLocation.unixTimestampMillis（hasFix=true の最初のイベント）
    
  GPS fixが未確定のイベント（hasFix=false）のタイムスタンプは無視する。
  これにより、RTCデフォルト値等原因でGPS時刻が不正な場合でも、
  正しいfix後のタイムスタンプが使用される。
  """
  try:
    with open(rlog_path, 'rb') as f:
      dat = f.read(1024 * 1024)  # 先頭1MBのみ読み込み

    # bz2圧縮チェック
    if dat.startswith(b'BZh9'):
      dat = bz2.decompress(dat)

    for raw_msg in read_capnp_messages(dat):
      try:
        with capnp_log.Event.from_bytes(raw_msg, traversal_limit_in_words=2**24) as msg:
          event_type = msg.which()

          if event_type == 'clocks':
            wall_time = msg.clocks.wallTimeNanos
            if wall_time > 0:
              return str(wall_time / 1e9)  # ナノ秒→秒に変換

          elif event_type == 'gpsLocationExternal':
            # GPS fixが有効な場合のみタイムスタンプを使用
            # hasFix は UBX-NAV-PVT の flags bit 0 で判定
            # fix未確定期はRTCデフォルト値等原因で不正な日時が含まれる場合がある
            if msg.gpsLocationExternal.hasFix:
              ts = msg.gpsLocationExternal.unixTimestampMillis
              if ts > 0:
                return str(ts / 1000.0)  # ミリ秒→秒に変換

          elif event_type == 'gpsLocation':
            # GPS fixが有効な場合のみタイムスタンプを使用
            if msg.gpsLocation.hasFix:
              ts = msg.gpsLocation.unixTimestampMillis
              if ts > 0:
                return str(ts / 1000.0)  # ミリ秒→秒に変換

      except Exception:
        continue

    return ""  # 時刻が見つからなければ空文字

  except Exception as e:
    return ""  # エラー時は空文字


if __name__ == "__main__":
  if len(sys.argv) < 2:
    print("")
    sys.exit(0)

  rlog_path = sys.argv[1]
  timestamp = extract_gps_time(rlog_path)
  print(timestamp)
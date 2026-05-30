#!/usr/bin/env python3
"""Extract GPS timestamp corrections for routes

This script calculates the correct timestamps for all segments in a route
by computing the offset between logMonoTime (boot-relative) and wallTimeNanos (GPS time).

Usage:
  python extract_route_time.py <route_path>

Output:
  <corrected_timestamp_ns>
"""

import sys
import os
from pathlib import Path

try:
  import bz2
  from cereal import log as capnp_log
except ImportError:
  print("")
  sys.exit(0)


RLOG_FILENAME = "rlog.bz2"
RLOG_FILENAME_UNCOMPRESSED = "rlog"


def read_capnp_messages(data):
  """capnpのストリーミングフォーマットからメッセージの生バイトを抽出"""
  import struct
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


def is_valid_recording_time(dt):
  """録画日時の妥当性をチェック"""
  from datetime import datetime, timezone
  now = datetime.now(timezone.utc)
  if dt > now or dt.year < 2015:
    return False
  return True


def extract_segment_times(seg_path):
  """1つのセグメントからlogMonoTimeとGPS時刻を抽出"""
  seg_dir = Path(seg_path)
  rlog_path = seg_dir / RLOG_FILENAME
  if not rlog_path.exists():
    rlog_path = seg_dir / RLOG_FILENAME_UNCOMPRESSED
  if not rlog_path.exists():
    return None, None

  try:
    with open(rlog_path, 'rb') as f:
      dat = f.read(8 * 1024 * 1024)  # 8MB read

    if dat.startswith(b'BZh9'):
      dat = bz2.decompress(dat)

    first_log_mono = None
    gps_time_ns = None

    for raw_msg in read_capnp_messages(dat):
      try:
        with capnp_log.Event.from_bytes(raw_msg, traversal_limit_in_words=2**24) as msg:
          if first_log_mono is None:
            first_log_mono = msg.logMonoTime

          if gps_time_ns is None and msg.which() == 'clocks':
            wall_time_ns = msg.clocks.wallTimeNanos
            if wall_time_ns > 0:
              from datetime import datetime, timezone
              dt = datetime.fromtimestamp(wall_time_ns / 1e9, tz=timezone.utc)
              if is_valid_recording_time(dt):
                gps_time_ns = wall_time_ns

          if gps_time_ns is None and msg.which() == 'gpsLocationExternal':
            loc = msg.gpsLocationExternal
            if loc.hasFix and loc.unixTimestampMillis > 0:
              from datetime import datetime, timezone
              ts = loc.unixTimestampMillis
              dt = datetime.fromtimestamp(ts / 1000, tz=timezone.utc)
              if is_valid_recording_time(dt):
                gps_time_ns = int(ts * 1e6)

          if gps_time_ns is None and msg.which() == 'gpsLocation':
            loc = msg.gpsLocation
            if loc.hasFix and loc.unixTimestampMillis > 0:
              from datetime import datetime, timezone
              ts = loc.unixTimestampMillis
              dt = datetime.fromtimestamp(ts / 1000, tz=timezone.utc)
              if is_valid_recording_time(dt):
                gps_time_ns = int(ts * 1e6)

      except Exception:
        continue

    return first_log_mono, gps_time_ns

  except Exception:
    return None, None


def discover_segments(route_path):
  """Discover segment directories in a route"""
  route_dir = Path(route_path)
  if not route_dir.is_dir():
    return []

  def _has_rlog(d):
    return (d / RLOG_FILENAME).exists() or (d / RLOG_FILENAME_UNCOMPRESSED).exists()

  dir_name = route_dir.name
  parts = dir_name.rsplit('--', 1)
  if len(parts) == 2 and parts[1].isdigit():
    route_prefix = parts[0]
    parent = route_dir.parent
    segments = []
    for entry in sorted(parent.iterdir()):
      if entry.is_dir() and entry.name.startswith(route_prefix + '--'):
        if _has_rlog(entry):
          segments.append(str(entry))
    if segments:
      return segments

  segments = []
  for entry in sorted(route_dir.iterdir()):
    if entry.is_dir() and _has_rlog(entry):
      segments.append(str(entry))

  if not segments and _has_rlog(route_dir):
    segments.append(str(route_dir))

  return segments


def calculate_route_start_time(route_path):
  """Calculate the corrected start time for a route

  Returns:
    corrected_timestamp_ns (int) or empty string if unavailable
  """
  segments = discover_segments(route_path)
  if not segments:
    print("")
    return

  segment_data = []
  for seg_path in segments:
    first_log_mono, gps_time_ns = extract_segment_times(seg_path)
    if first_log_mono is not None:
      segment_data.append((seg_path, first_log_mono, gps_time_ns))

  valid_offsets = []
  for seg_path, first_mono, gps_time in segment_data:
    if gps_time is not None:
      offset = gps_time - first_mono
      valid_offsets.append(offset)

  if not valid_offsets:
    print("")
    return

  reference_offset = min(valid_offsets)

  earliest_timestamp = None
  for seg_path, first_mono, gps_time in segment_data:
    corrected = first_mono + reference_offset
    if earliest_timestamp is None or corrected < earliest_timestamp:
      earliest_timestamp = corrected

  print(str(earliest_timestamp))


if __name__ == "__main__":
  if len(sys.argv) < 2:
    print("")
    sys.exit(0)

  route_path = sys.argv[1]
  calculate_route_start_time(route_path)
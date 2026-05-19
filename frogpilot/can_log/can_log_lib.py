"""CAN Bus Log Format Library - バイナリCANログフォーマットの読み書き

ログフォーマット（1メッセージ = 22バイト）:
  [8bytes timestamp_nano] [4bytes can_addr] [1byte src_bus] [1byte datalen] [8bytes data]

リトルエンディアン、datalenが8未満の場合はdataの残りは0パディング。
gzip圧縮（.gz）に対応。圧縮ファイルは自動判定する。
"""

import gzip
import os
import struct
from dataclasses import dataclass
from typing import Optional

# 1レコードの固定サイズ
RECORD_SIZE = 22
# パックフォーマット: little-endian
# Q: uint64 (timestamp_nano), I: uint32 (can_addr), B: uint8 (src_bus), B: uint8 (datalen), 8s: 8bytes (data)
PACK_FMT = '<QIBB8s'


@dataclass
class CanLogEntry:
  """CANログの1エントリを表すデータクラス"""
  timestamp_nano: int    # ナノ秒精度のUNIXタイムスタンプ
  can_addr: int          # CAN ID (例: 0x165, 0x366)
  src_bus: int           # バス番号 (通常0)
  datalen: int           # データ長 (1-8)
  data: bytes            # CANデータバイト


class CanLogWriter:
  """CANログをバイナリファイルに書き込むクラス"""

  def __init__(self, filepath: str, compress: bool = False):
    """ログファイルを開く（バイナリ書き込みモード）

    Args:
      filepath: 出力ファイルパス
      compress: gzip圧縮を有効にするか（デフォルト: False）
    """
    self.filepath = filepath
    if compress and not filepath.endswith('.gz'):
      self.filepath = filepath + '.gz'
    else:
      self.filepath = filepath

    if self.filepath.endswith('.gz'):
      self._file = gzip.open(self.filepath, 'wb')
    else:
      self._file = open(self.filepath, 'wb')

  def write_entry(self, entry: CanLogEntry) -> None:
    """1エントリを書き込む

    Args:
      entry: 書き込むCanLogEntry
    """
    # dataを8バイトにパディング
    padded_data = entry.data[:entry.datalen].ljust(8, b'\x00')
    packed = struct.pack(PACK_FMT, entry.timestamp_nano, entry.can_addr,
                         entry.src_bus, entry.datalen, padded_data)
    self._file.write(packed)

  def flush(self) -> None:
    """バッファをフラッシュする"""
    self._file.flush()

  def close(self) -> None:
    """ファイルを閉じる"""
    if self._file and not self._file.closed:
      self._file.close()

  def __enter__(self) -> 'CanLogWriter':
    return self

  def __exit__(self, exc_type, exc_val, exc_tb) -> None:
    self.close()


class CanLogReader:
  """CANログをバイナリファイルから読み込むクラス"""

  def __init__(self, filepath: str):
    """ログファイルを開く（バイナリ読み込みモード）

    gzip圧縮ファイル（.gz拡張子）は自動的に判定して展開する。

    Args:
      filepath: 入力ファイルパス
    """
    self.filepath = filepath
    self._is_compressed = filepath.endswith('.gz')

    if self._is_compressed:
      self._file = gzip.open(filepath, 'rb')
    else:
      self._file = open(filepath, 'rb')

    self._message_count: int = 0
    self._start_time_nano: Optional[int] = None
    self._end_time_nano: Optional[int] = None
    self._index_entries()  # ファイル全体をスキャンしてメタデータを取得

  def _index_entries(self) -> None:
    """ファイル全体をスキャンしてメタデータ（メッセージ数、開始/終了時間）を取得

    非圧縮ファイル: seekで高速にインデックス構築
    圧縮ファイル: 全件スキャンが必要（seek非対応のため）
    """
    if self._is_compressed:
      # gzipファイルはseekが遅いため、全件スキャンでメタデータを取得
      self._file.seek(0)
      count = 0
      first_ts = None
      last_ts = None
      while True:
        record = self._file.read(RECORD_SIZE)
        if len(record) < RECORD_SIZE:
          break
        ts, _, _, _, _ = struct.unpack(PACK_FMT, record)
        if first_ts is None:
          first_ts = ts
        last_ts = ts
        count += 1

      self._message_count = count
      self._start_time_nano = first_ts
      self._end_time_nano = last_ts

      # 読み込み位置を先頭に戻す（gzipはseek(0)が可能）
      self._file.seek(0)
    else:
      # 非圧縮ファイル: seekで高速インデックス
      self._file.seek(0, 2)  # ファイル末尾へ
      file_size = self._file.tell()
      self._message_count = file_size // RECORD_SIZE

      if self._message_count == 0:
        self._start_time_nano = None
        self._end_time_nano = None
        return

      # 最初のエントリから開始時間を取得
      self._file.seek(0)
      first_record = self._file.read(RECORD_SIZE)
      if len(first_record) == RECORD_SIZE:
        ts, _, _, _, _ = struct.unpack(PACK_FMT, first_record)
        self._start_time_nano = ts

      # 最後のエントリから終了時間を取得
      last_offset = (self._message_count - 1) * RECORD_SIZE
      self._file.seek(last_offset)
      last_record = self._file.read(RECORD_SIZE)
      if len(last_record) == RECORD_SIZE:
        ts, _, _, _, _ = struct.unpack(PACK_FMT, last_record)
        self._end_time_nano = ts

      # 読み込み位置を先頭に戻す
      self._file.seek(0)

  def read_entry(self) -> Optional[CanLogEntry]:
    """1エントリを読み込む。EOFならNoneを返す

    Returns:
      CanLogEntry or None
    """
    record = self._file.read(RECORD_SIZE)
    if len(record) < RECORD_SIZE:
      return None

    timestamp_nano, can_addr, src_bus, datalen, data = struct.unpack(PACK_FMT, record)
    return CanLogEntry(
      timestamp_nano=timestamp_nano,
      can_addr=can_addr,
      src_bus=src_bus,
      datalen=datalen,
      data=data[:datalen],  # パディング部分を除去
    )

  def read_all(self) -> list[CanLogEntry]:
    """全エントリを読み込む

    Returns:
      CanLogEntryのリスト
    """
    self._file.seek(0)
    entries: list[CanLogEntry] = []
    while True:
      entry = self.read_entry()
      if entry is None:
        break
      entries.append(entry)
    return entries

  @property
  def duration_seconds(self) -> float:
    """ログの総時間（秒）"""
    if self._start_time_nano is not None and self._end_time_nano is not None:
      return (self._end_time_nano - self._start_time_nano) / 1e9
    return 0.0

  @property
  def start_time(self) -> float:
    """ログの開始UNIXタイムスタンプ（秒）"""
    if self._start_time_nano is not None:
      return self._start_time_nano / 1e9
    return 0.0

  @property
  def end_time(self) -> float:
    """ログの終了UNIXタイムスタンプ（秒）"""
    if self._end_time_nano is not None:
      return self._end_time_nano / 1e9
    return 0.0

  @property
  def message_count(self) -> int:
    """総メッセージ数"""
    return self._message_count

  def seek_to_time(self, target_seconds: float) -> None:
    """指定時刻にシーク（非圧縮: バイナリサーチ、圧縮: 線形サーチ）

    target_secondsはログ開始からの経過時間ではなく、絶対UNIXタイムスタンプ（秒）。

    Args:
      target_seconds: 目標のUNIXタイムスタンプ（秒）
    """
    if self._message_count == 0:
      return

    target_nano = int(target_seconds * 1e9)

    if self._is_compressed:
      # 圧縮ファイル: 先頭から線形サーチ
      self._file.seek(0)
      while True:
        pos = self._file.tell()
        record = self._file.read(RECORD_SIZE)
        if len(record) < RECORD_SIZE:
          break
        ts, _, _, _, _ = struct.unpack(PACK_FMT, record)
        if ts >= target_nano:
          self._file.seek(pos)
          return
      # 見つからなかった場合、ファイル末尾へ
      self._file.seek(0, 2)
    else:
      # 非圧縮ファイル: バイナリサーチで高速化
      left = 0
      right = self._message_count - 1

      while left <= right:
        mid = (left + right) // 2
        self._file.seek(mid * RECORD_SIZE)
        record = self._file.read(RECORD_SIZE)
        if len(record) < RECORD_SIZE:
          break

        ts, _, _, _, _ = struct.unpack(PACK_FMT, record)

        if ts < target_nano:
          left = mid + 1
        elif ts > target_nano:
          right = mid - 1
        else:
          # 完全一致: この位置にシーク
          self._file.seek(mid * RECORD_SIZE)
          return

      # 見つからなかった場合、leftの位置（target以上の最初のエントリ）にシーク
      seek_pos = min(left, self._message_count - 1)
      self._file.seek(seek_pos * RECORD_SIZE)

  def close(self) -> None:
    """ファイルを閉じる"""
    if self._file and not self._file.closed:
      self._file.close()

  def __enter__(self) -> 'CanLogReader':
    return self

  def __exit__(self, exc_type, exc_val, exc_tb) -> None:
    self.close()
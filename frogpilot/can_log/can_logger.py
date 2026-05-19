#!/usr/bin/env python3
"""CAN Bus Logger - canサービスをsubscribeしてバイナリファイルに記録

cereal.messagingのSubMasterを使用してCANメッセージを受信し、
独自のバイナリフォーマットでファイルに記録するデーモンプロセス。

CanData構造体 (cereal/log.capnp):
  address @0 :UInt32   - CAN ID
  busTime  @1 :UInt16  - バスタイム
  dat      @2 :Data    - CANデータ
  src      @3 :UInt8   - ソースバス
"""

import os
import time
from datetime import datetime
from pathlib import Path

from cereal.messaging import SubMaster
from frogpilot.can_log.can_log_lib import CanLogWriter, CanLogEntry

LOG_DIR = "/data/can_logs/"  # デバイス上の保存先
MAX_LOG_SIZE = 500 * 1024 * 1024  # 500MB上限
MAX_LOG_FILES = 20  # 古いログファイルの保持上限
COMPRESS_LOGS = True  # gzip圧縮をデフォルト有効


def get_log_filepath() -> str:
  """タイムスタンプベースのファイルパスを生成

  Returns:
    ログファイルのフルパス
  """
  timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
  os.makedirs(LOG_DIR, exist_ok=True)
  return os.path.join(LOG_DIR, f"can_log_{timestamp}.canbin")


def cleanup_old_logs() -> None:
  """古いログファイルを削除（MAX_LOG_FILES以上になったら古い順に削除）"""
  log_dir = Path(LOG_DIR)
  if not log_dir.exists():
    return

  # canbinファイルとcanbin.gzファイルを更新日時順でソート（古い順）
  log_files = sorted(
    list(log_dir.glob("can_log_*.canbin")) + list(log_dir.glob("can_log_*.canbin.gz")),
    key=lambda f: f.stat().st_mtime
  )

  while len(log_files) >= MAX_LOG_FILES:
    oldest = log_files.pop(0)
    try:
      oldest.unlink()
    except OSError:
      break


def main() -> None:
  """CANロガーのメインループ"""
  sm = SubMaster(["can"])
  filepath = get_log_filepath()
  writer = CanLogWriter(filepath, compress=COMPRESS_LOGS)

  try:
    while True:
      sm.update()

      if sm.updated["can"]:
        # canサービスからメッセージを読み取り
        can_data_list = sm["can"]
        log_mono_time = sm.logMonoTime["can"]

        # CanDataのリストを走査して各メッセージを記録
        for can_msg in can_data_list:
          can_data = can_msg.dat
          datalen = len(can_data)
          if datalen == 0:
            continue

          entry = CanLogEntry(
            timestamp_nano=log_mono_time,
            can_addr=can_msg.address,
            src_bus=can_msg.src,
            datalen=datalen,
            data=bytes(can_data),
          )
          writer.write_entry(entry)

        writer.flush()

        # ファイルサイズチェック（実際のファイルパスを使用）
        try:
          if os.path.getsize(writer.filepath) > MAX_LOG_SIZE:
            writer.close()
            cleanup_old_logs()
            filepath = get_log_filepath()
            writer = CanLogWriter(filepath, compress=COMPRESS_LOGS)
        except OSError:
          pass

  except KeyboardInterrupt:
    pass
  finally:
    writer.close()


if __name__ == "__main__":
  main()
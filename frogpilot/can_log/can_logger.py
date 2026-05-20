#!/usr/bin/env python3
"""CAN Bus Logger - DEPRECATED

このモジュールは非推奨です。CAN データは標準の loggerd によって
rlog ファイルに自動的に記録されます。

保存先: /data/media/0/realdata/{route}/{segment}/rlog

CAN再生には can_player.py を使用してください。
can_player.py は rlog から直接CANメッセージを読み出します。

以前の LOG_DIR = "/data/can_logs/" は使用されなくなりました。
"""

import sys

def main():
  print("DEPRECATED: can_logger.py is no longer needed.", file=sys.stderr)
  print("CAN data is automatically recorded by loggerd in rlog files.", file=sys.stderr)
  print("Use can_player.py to play back CAN data from rlog.", file=sys.stderr)
  print("See: /data/media/0/realdata/{route}/{segment}/rlog", file=sys.stderr)
  sys.exit(0)

if __name__ == "__main__":
  main()
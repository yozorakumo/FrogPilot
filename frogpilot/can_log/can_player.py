#!/usr/bin/env python3
"""CAN Bus Player - ログファイルからcanサービスにメッセージをパブリッシュ

CANログファイルを読み込み、オリジナルのタイミングを再現してcanサービスに
メッセージをパブリッシュする。tools/sim/lib/simulated_car.pyのパターンを参考。

使用例:
  # 実時間再生
  python -m frogpilot.can_log.can_player /path/to.can_log

  # 2倍速再生
  python -m frogpilot.can_log.can_player --speed 2.0 /path/to.can_log

  # ループ再生
  python -m frogpilot.can_log.can_player --loop /path/to.can_log
"""

import os
import sys
import time
import argparse
import threading
from datetime import datetime, timezone

import cereal.messaging as messaging

from openpilot.common.params import Params
from openpilot.selfdrive.pandad import can_list_to_can_capnp
from openpilot.common.swaglog import cloudlog

from frogpilot.can_log.can_log_lib import CanLogReader, CanLogEntry

# 同一バッチとみなす最小タイムスタンプ差（秒）
BATCH_THRESHOLD_SEC = 0.001  # 1ms


class CanPlayer:
  """CANログファイルを再生し、canサービスにメッセージをパブリッシュするクラス"""

  def __init__(self, filepath: str, speed: float = 1.0, loop: bool = False):
    """
    Args:
      filepath: CANログファイルパス
      speed: 再生速度（1.0=実時間、2.0=2倍速）
      loop: ループ再生するか
    """
    self.filepath = filepath
    self.speed = speed
    self.loop = loop
    self.pm = messaging.PubMaster(['can'])
    self.params = Params()

    self._paused = False
    self._stop = False
    self._current_time_ns = 0
    self._lock = threading.Lock()

    # ファイルメタデータを取得（インデックス付きリーダーで高速）
    with CanLogReader(filepath) as reader:
      self._duration = reader.duration_seconds
      self._start_time_ns = reader._start_time_nano or 0
      self._end_time_ns = reader._end_time_nano or 0
      self._message_count = reader.message_count

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
        # seek_posはログ先頭からの相対秒数 → 絶対タイムスタンプに変換
        abs_time = (self._start_time_ns / 1e9) + seek_pos
        cloudlog.info(f"CAN playback: seeking to {seek_pos:.2f}s (abs: {abs_time:.2f})")
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

  @staticmethod
  def _entry_to_can_msg(entry: CanLogEntry) -> list:
    """CanLogEntryをcan_list_to_can_capnp用の形式に変換

    can_list_to_can_capnp expects: [address, busTime, dat, src]
    """
    return [entry.can_addr, 0, bytes(entry.data[:entry.datalen]), entry.src_bus]

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

    ログエントリをストリーミングで読み込み、オリジナルのタイミングを
    再現しながらcanサービスにパブリッシュする。

    タイミング制御:
    - time.monotonic()を基準にwall clock時間を管理
    - 一時停止中の時間は再生タイミングに影響しない
    - 同一タイムスタンプ（1ms未満の差）のエントリはバッチ送信
    """
    # 初期状態をParamsに書き込む
    self._update_params_state()

    while not self._stop:
      with CanLogReader(self.filepath) as reader:
        batch: list[list] = []
        log_start_ns = None
        playback_start = None
        total_paused_duration = 0.0
        last_params_update = time.monotonic()

        while not self._stop:
          entry = reader.read_entry()
          if entry is None:
            break

          # UIコマンドの確認と状態同期（100ms間隔）
          now = time.monotonic()
          if now - last_params_update > 0.1:
            self._check_params_commands()
            self._update_params_state()
            last_params_update = now

          # 一時停止処理（ポーリング方式でスレッドセーフ）
          if self._paused:
            self._update_params_state()
            pause_begin = time.monotonic()
            while self._paused and not self._stop:
              time.sleep(0.05)
              # 一時停止中もコマンドを確認
              self._check_params_commands()
            if self._stop:
              break
            total_paused_duration += time.monotonic() - pause_begin

          with self._lock:
            self._current_time_ns = entry.timestamp_nano

          # 最初のエントリ：再生開始基準を設定
          if log_start_ns is None:
            log_start_ns = entry.timestamp_nano
            playback_start = time.monotonic()
            batch.append(self._entry_to_can_msg(entry))
            continue

          # タイミング計算
          # effective_elapsed: 一時停止を除いた実際の経過wall clock時間
          effective_elapsed = time.monotonic() - playback_start - total_paused_duration

          # target_elapsed: ログ内の経過時間を再生速度でスケーリング
          log_elapsed_sec = (entry.timestamp_nano - log_start_ns) / 1e9
          target_elapsed = log_elapsed_sec / self.speed

          sleep_needed = target_elapsed - effective_elapsed

          if sleep_needed > BATCH_THRESHOLD_SEC:
            # タイムギャップあり → 蓄積したバッチを送信して待機
            if batch:
              self._send_batch(batch)
              batch = []

            # 精度向上のため残り時間を再計算してスリープ
            remaining = target_elapsed - (time.monotonic() - playback_start - total_paused_duration)
            if remaining > 0:
              time.sleep(remaining)

          batch.append(self._entry_to_can_msg(entry))

        # 最後のバッチを送信
        if batch:
          self._send_batch(batch)

        if not self.loop:
          break

        cloudlog.info("CAN playback loop: restarting from beginning")

    cloudlog.info("CAN playback finished")


def main():
  parser = argparse.ArgumentParser(
    description='CAN Bus Log Player - ログファイルからcanサービスにメッセージをパブリッシュ',
    formatter_class=argparse.RawDescriptionHelpFormatter,
    epilog="""
使用例:
  # 実時間再生
  python -m frogpilot.can_log.can_player /path/to.can_log

  # 2倍速再生
  python -m frogpilot.can_log.can_player --speed 2.0 /path/to.can_log

  # ループ再生
  python -m frogpilot.can_log.can_player --loop /path/to.can_log

  card.pyと組み合わせて再生モードで起動:
  CAN_PLAYBACK=1 python -m frogpilot.can_log.can_player /path/to.can_log &
  CAN_PLAYBACK=1 selfdrive/card.py
""")
  parser.add_argument('filepath', help='CAN log file path (.can_log)')
  parser.add_argument('--speed', type=float, default=1.0,
                      help='Playback speed multiplier (default: 1.0)')
  parser.add_argument('--loop', action='store_true',
                      help='Loop playback')
  args = parser.parse_args()

  if not os.path.exists(args.filepath):
    print(f"Error: File not found: {args.filepath}")
    sys.exit(1)

  player = CanPlayer(args.filepath, speed=args.speed, loop=args.loop)

  print("CAN Log Player")
  print(f"  File:       {args.filepath}")
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
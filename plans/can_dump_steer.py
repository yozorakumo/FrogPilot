#!/usr/bin/env python3
"""STEER message byte-by-byte analysis for angle position identification."""
import time
from panda import Panda

p = Panda()
STEER_ID = 0x082

def parse_steer(data):
  """Parse each byte of STEER message."""
  bytes_list = [f'{b:02X}' for b in data]
  
  # Current DBC: STEER_ANGLE = 23|16@0+ (0.05,-1600) → reads byte2(byte1)
  raw_old = (data[2] << 8) | data[1]
  angle_old = raw_old * 0.05 - 1600
  
  # Byte3 as signed offset from 0x80
  byte3_signed = data[3] - 128 if data[3] < 256 else 0
  
  return bytes_list, raw_old, angle_old, byte3_signed

print("=== STEER Message Byte Analysis ===")
print("Format: byte0 byte1 byte2 byte3 byte4 byte5 byte6 byte7")
print("  DBC angle (byte2-1): 23|16@0+ (0.05,-1600)")
print("  byte3 as offset from 0x80 (128)")
print()

for label, dur in [
  ('Phase 1: Steering CENTERED (hold straight)', 8),
  ('Phase 2: Steering LEFT (turn wheel left ~90deg)', 8),
  ('Phase 3: Steering RIGHT (turn wheel right ~90deg)', 8),
  ('Phase 4: Steering LEFT FULL (turn wheel left as far as comfortable)', 8),
  ('Phase 5: Steering RIGHT FULL (turn wheel right as far as comfortable)', 8),
]:
  print(f"\n=== {label} ({dur}s) ===")
  start = time.time()
  first = True
  while time.time() - start < dur:
    msgs = p.can_recv()
    for m in msgs:
      if m[0] == STEER_ID:
        data = m[2]
        if len(data) >= 8:
          bytes_list, raw_old, angle_old, byte3_signed = parse_steer(data)
          print(f'  [{" ".join(bytes_list)}]  DBC_angle={angle_old:7.1f}°  byte3_offset={byte3_signed:+4d}')
          time.sleep(0.3)  # Slow down output
          break
    else:
      continue
    if first:
      first = False

print("\n=== DONE ===")
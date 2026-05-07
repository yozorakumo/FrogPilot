#!/usr/bin/env python3
"""
Gear investigation script for Mazda 2 DJ MT.
Captures CAN data from 0x165 (PEDALS), 0x166 (NEW_MSG_28), 0x09E (ID_9E)
while user changes gear positions and brake pressure.

Usage: PYTHONPATH=/data/openpilot python3 can_dump_gear_investigate.py
"""
import time
import sys
from panda import Panda

def decode_gear_from_pedals(data_bytes):
  """Decode GEAR signal from PEDALS (0x165): bit 48, 5 bits, Motorola (@1+)"""
  if len(data_bytes) < 7:
    return -1
  # GEAR: 48|5@1+ (Motorola byte order)
  # Bit 48 = byte 6 bit 0, 5 bits spanning bytes 6-7
  byte6 = data_bytes[6]
  byte7 = data_bytes[7] if len(data_bytes) > 7 else 0
  # Motorola: start at bit 48 (byte 6 bit 0), 5 bits
  gear = (byte6 & 0x1F)
  return gear

def decode_gear_pos_from_0x166(data_bytes):
  """Decode GEAR_POS from NEW_MSG_28 (0x166): bit 23, 4 bits, Intel (@0+)"""
  if len(data_bytes) < 4:
    return -1
  # GEAR_POS: 23|4@0+ (Intel byte order)
  # Bit 23 = byte 2 bit 7, 4 bits crossing to byte 3
  byte2 = data_bytes[2]
  byte3 = data_bytes[3]
  bit23 = (byte2 >> 7) & 1
  bits24_26 = byte3 & 0x7
  gear_pos = (bits24_26 << 1) | bit23
  return gear_pos

def decode_neutral_sw(data_bytes):
  """Decode NEUTRAL_SW from ID_9E (0x09E): bit 0"""
  if len(data_bytes) < 1:
    return -1
  return data_bytes[0] & 1

def decode_clutch_alt(data_bytes):
  """Decode CLUTCH_ALT from ID_9E (0x09E): bit 5"""
  if len(data_bytes) < 1:
    return -1
  return (data_bytes[0] >> 5) & 1

p = Panda()
print("=== Mazda 2 DJ MT Gear Investigation ===")
print("Please shift through: N -> 1st -> 2nd -> 3rd -> 4th -> 5th -> 6th -> R")
print("Also press/release brake pedal in each gear position")
print("Capturing for 30 seconds...\n")

targets = {0x165: "PEDALS", 0x166: "NEW_MSG_28", 0x09E: "ID_9E"}
start = time.time()
all_data = []

while time.time() - start < 30:
  msgs = p.can_recv()
  for msg in msgs:
    addr = msg[0]
    dat = msg[2]
    bus = msg[3]
    if addr in targets and bus == 0:
      if isinstance(dat, bytes):
        all_data.append((time.time() - start, addr, dat))

# Group by unique data pattern per address
print("--- Unique data patterns per CAN ID ---\n")
for addr in sorted(targets.keys()):
  label = targets[addr]
  patterns = {}
  for t, a, d in all_data:
    if a == addr:
      dh = d.hex()
      if dh not in patterns:
        patterns[dh] = {"count": 0, "first_time": t, "data": d}
      patterns[dh]["count"] += 1
      patterns[dh]["last_time"] = t
  
  print(f"ID 0x{addr:03X} ({label}) - {len(patterns)} unique patterns:")
  for dh, info in sorted(patterns.items(), key=lambda x: -x[1]["count"]):
    d = info["data"]
    bytes_str = " ".join(f"{b:02x}" for b in d)
    pct = info["count"] * 100 // sum(p["count"] for p in patterns.values())
    decoded = ""
    if addr == 0x165:
      gear = decode_gear_from_pedals(d)
      decoded = f"GEAR={gear}"
    elif addr == 0x166:
      gear_pos = decode_gear_pos_from_0x166(d)
      decoded = f"GEAR_POS={gear_pos}"
    elif addr == 0x09E:
      neutral = decode_neutral_sw(d)
      clutch = decode_clutch_alt(d)
      decoded = f"NEUTRAL_SW={neutral} CLUTCH_ALT={clutch}"
    print(f"  {bytes_str}  ({info['count']:4d} msgs, {pct:3d}%) t={info['first_time']:.1f}-{info['last_time']:.1f}s  {decoded}")
  print()

# Show timeline of changes
print("--- Timeline of data changes (first 50) ---\n")
prev = {}
count = 0
for t, addr, d in all_data:
  dh = d.hex()
  if addr not in prev or prev[addr] != dh:
    label = targets[addr]
    bytes_str = " ".join(f"{b:02x}" for b in d)
    decoded = ""
    if addr == 0x165:
      decoded = f"GEAR={decode_gear_from_pedals(d)}"
    elif addr == 0x166:
      decoded = f"GEAR_POS={decode_gear_pos_from_0x166(d)}"
    elif addr == 0x09E:
      decoded = f"NEUTRAL={decode_neutral_sw(d)} CLUTCH={decode_clutch_alt(d)}"
    print(f"  t={t:6.2f}s  0x{addr:03X} ({label:12s}): {bytes_str}  {decoded}")
    prev[addr] = dh
    count += 1
    if count >= 50:
      break

print("\n=== Done ===")
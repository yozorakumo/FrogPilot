#!/usr/bin/env python3
"""Gear signal capture - monitor gear + brake + clutch in real-time"""
import sys, time
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging

sub = messaging.sub_sock('can')
time.sleep(0.5)

DURATION = 30.0

# Track specific signals
prev = {}  # (addr, bus) -> data bytes

print('=== GEAR SIGNAL CAPTURE ===', flush=True)
print('Monitoring NEW_MSG_28(0x166) + PEDALS(0x165) + brake signals', flush=True)
print('', flush=True)

# Phase 1: Baseline (engine on, no pedal operation)
print('=== Phase 1: BASELINE (5s - do nothing) ===', flush=True)
start = time.time()
while time.time() - start < 5.0:
    msgs = messaging.recv_sock(sub, True)
    if msgs is None:
        continue
    for msg in msgs.can:
        prev[(msg.address, msg.src)] = bytes(msg.dat)

# Phase 2: Brake pedal test
print('', flush=True)
print('=== Phase 2: BRAKE TEST (10s) ===', flush=True)
print('Press and release brake pedal 3 times', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
print('>>> PRESS BRAKE NOW! <<<', flush=True)

start = time.time()
last_print = 0
while time.time() - start < 10.0:
    t = time.time() - start
    msgs = messaging.recv_sock(sub, True)
    if msgs is None:
        continue
    for msg in msgs.can:
        addr = msg.address
        bus = msg.src
        data = bytes(msg.dat)
        key = (addr, bus)

        old = prev.get(key)
        if old is not None:
            # NEW_MSG_28 (0x166 = 358) - track byte2 (GEAR_POS at 23|4@0+ = byte2 upper nibble)
            if addr == 0x166 and bus == 0 and len(data) >= 3 and len(old) >= 3:
                if data[2] != old[2]:
                    gear_val = (data[2] >> 4) & 0xF
                    old_gear = (old[2] >> 4) & 0xF
                    gear_name = {4: "FWD", 5: "FWD", 6: "REV"}.get(gear_val, f"UNK({gear_val})")
                    print(f'  [{t:6.2f}s] NEW_MSG_28 GEAR_POS: {old_gear} -> {gear_val} ({gear_name})  byte2: 0x{old[2]:02X} -> 0x{data[2]:02X}  full: {data.hex()}', flush=True)
                # Also track byte0 (CLUTCH_PEDAL at bit7)
                if data[0] != old[0]:
                    clutch = (data[0] >> 7) & 1
                    old_clutch = (old[0] >> 7) & 1
                    if clutch != old_clutch:
                        print(f'  [{t:6.2f}s] NEW_MSG_28 CLUTCH: {old_clutch} -> {clutch}  byte0: 0x{old[0]:02X} -> 0x{data[0]:02X}', flush=True)

            # PEDALS (0x165 = 357) - track GEAR_POS (55|8@0+ = byte6)
            if addr == 0x165 and bus == 0 and len(data) >= 7 and len(old) >= 7:
                if data[6] != old[6]:
                    print(f'  [{t:6.2f}s] PEDALS GEAR_POS: {old[6]} -> {data[6]}  byte6: 0x{old[6]:02X} -> 0x{data[6]:02X}', flush=True)
                # BRAKE_ON at byte0 bit4
                if data[0] != old[0]:
                    new_brake = (data[0] >> 4) & 1
                    old_brake = (old[0] >> 4) & 1
                    if new_brake != old_brake:
                        print(f'  [{t:6.2f}s] PEDALS BRAKE: {old_brake} -> {new_brake}  byte0: 0x{old[0]:02X} -> 0x{data[0]:02X}', flush=True)

        prev[key] = data

    if t - last_print >= 3.0:
        print(f'  [{t:5.1f}s] ...', flush=True)
        last_print = t

# Phase 3: Gear shift test
print('', flush=True)
print('=== Phase 3: GEAR SHIFT TEST (15s) ===', flush=True)
print('Shift: N -> 1st -> 2nd -> N (hold each for 2s)', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
print('>>> SHIFT NOW: N -> 1st -> 2nd -> N <<<', flush=True)

start = time.time()
last_print = 0
while time.time() - start < 15.0:
    t = time.time() - start
    msgs = messaging.recv_sock(sub, True)
    if msgs is None:
        continue
    for msg in msgs.can:
        addr = msg.address
        bus = msg.src
        data = bytes(msg.dat)
        key = (addr, bus)

        old = prev.get(key)
        if old is not None:
            if addr == 0x166 and bus == 0 and len(data) >= 3 and len(old) >= 3:
                if data[2] != old[2]:
                    gear_val = (data[2] >> 4) & 0xF
                    old_gear = (old[2] >> 4) & 0xF
                    gear_name = {4: "FWD", 5: "FWD", 6: "REV"}.get(gear_val, f"UNK({gear_val})")
                    print(f'  [{t:6.2f}s] NEW_MSG_28 GEAR_POS: {old_gear} -> {gear_val} ({gear_name})  byte2: 0x{old[2]:02X} -> 0x{data[2]:02X}', flush=True)
                if data[0] != old[0]:
                    clutch = (data[0] >> 7) & 1
                    old_clutch = (old[0] >> 7) & 1
                    if clutch != old_clutch:
                        print(f'  [{t:6.2f}s] NEW_MSG_28 CLUTCH: {old_clutch} -> {clutch}', flush=True)

            if addr == 0x165 and bus == 0 and len(data) >= 7 and len(old) >= 7:
                if data[6] != old[6]:
                    gear_map = {2: "6th", 3: "5th", 4: "4th", 5: "3rd", 7: "2nd", 13: "1st"}
                    name = gear_map.get(data[6], f"UNK({data[6]})")
                    print(f'  [{t:6.2f}s] PEDALS GEAR_POS: {old[6]} -> {data[6]} ({name})', flush=True)

        prev[key] = data

    if t - last_print >= 3.0:
        print(f'  [{t:5.1f}s] ...', flush=True)
        last_print = t

print('', flush=True)
print('=== DONE ===', flush=True)

# Print current values
print('\n=== CURRENT STATE ===', flush=True)
for key, data in sorted(prev.items()):
    addr, bus = key
    if addr in [0x165, 0x166] and bus == 0:
        print(f'  0x{addr:03X}: {data.hex()}', flush=True)
        if addr == 0x166:
            gear = (data[2] >> 4) & 0xF if len(data) > 2 else -1
            clutch = (data[0] >> 7) & 1 if len(data) > 0 else -1
            print(f'    GEAR_POS={gear}, CLUTCH={clutch}', flush=True)
        if addr == 0x165:
            gear = data[6] if len(data) > 6 else -1
            brake = (data[0] >> 4) & 1 if len(data) > 0 else -1
            print(f'    GEAR_POS={gear}, BRAKE={brake}', flush=True)

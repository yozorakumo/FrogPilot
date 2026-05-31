#!/usr/bin/env python3
"""Gear signal capture v2 - brake test + N/1st/2nd/Reverse test"""
import sys, time
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging

sub = messaging.sub_sock('can')
time.sleep(0.5)

prev = {}

def process(t, addr, bus, old, data):
    if old is None:
        return
    # NEW_MSG_28 (0x166 = 358)
    if addr == 0x166 and bus == 0 and len(data) >= 3 and len(old) >= 3:
        if data[2] != old[2]:
            gear_val = (data[2] >> 4) & 0xF
            old_gear = (old[2] >> 4) & 0xF
            gear_name = {4: "FWD", 5: "FWD", 6: "REV"}.get(gear_val, f"UNK({gear_val})")
            print(f'  [{t:6.2f}s] 0x166 GEAR_POS: {old_gear}->{gear_val} ({gear_name})  byte2: 0x{old[2]:02X}->0x{data[2]:02X}', flush=True)
        if data[0] != old[0]:
            clutch = (data[0] >> 7) & 1
            old_clutch = (old[0] >> 7) & 1
            if clutch != old_clutch:
                print(f'  [{t:6.2f}s] 0x166 CLUTCH: {old_clutch}->{clutch}', flush=True)

    # PEDALS (0x165 = 357)
    if addr == 0x165 and bus == 0 and len(data) >= 7 and len(old) >= 7:
        if data[6] != old[6]:
            gear_map = {2: "6th", 3: "5th", 4: "4th", 5: "3rd", 7: "2nd", 13: "1st"}
            name = gear_map.get(data[6], f"UNK({data[6]})")
            print(f'  [{t:6.2f}s] 0x165 GEAR_POS: {old[6]}->{data[6]} ({name})', flush=True)
        if data[0] != old[0]:
            new_brake = (data[0] >> 4) & 1
            old_brake = (old[0] >> 4) & 1
            if new_brake != old_brake:
                print(f'  [{t:6.2f}s] 0x165 BRAKE: {old_brake}->{new_brake}', flush=True)


def run_phase(name, duration, instructions):
    print(f'\n=== {name} ({duration}s) ===', flush=True)
    if instructions:
        print(instructions, flush=True)
    for i in range(3, 0, -1):
        print(f'  {i}...', flush=True)
        time.sleep(1)
    print('>>> GO! <<<', flush=True)
    start = time.time()
    last_print = 0
    while time.time() - start < duration:
        t = time.time() - start
        msgs = messaging.recv_sock(sub, True)
        if msgs is None:
            continue
        for msg in msgs.can:
            addr, bus = msg.address, msg.src
            data = bytes(msg.dat)
            key = (addr, bus)
            old = prev.get(key)
            process(t, addr, bus, old, data)
            prev[key] = data
        if t - last_print >= 3.0:
            print(f'  [{t:5.1f}s] ...', flush=True)
            last_print = t

# Phase 1: Baseline
run_phase("BASELINE", 5, "Do nothing - just sit in neutral")

# Phase 2: Brake test in neutral
run_phase("BRAKE TEST (in Neutral)", 12, "Press/release brake 3 times (stay in Neutral!)")

# Phase 3: Gear shifts
run_phase("GEAR SHIFT: N -> 1st -> 2nd -> N", 20, "Shift: N -> 1st(hold 3s) -> 2nd(hold 3s) -> N")

# Phase 4: Reverse
run_phase("REVERSE: N -> R -> N", 12, "Shift: N -> Reverse(hold 3s) -> N")

# Phase 5: Brake while in 1st gear
run_phase("BRAKE IN 1ST GEAR", 12, "Shift to 1st, then press/release brake 3 times")

print('\n=== DONE ===', flush=True)
print('\n=== CURRENT STATE ===', flush=True)
for key, data in sorted(prev.items()):
    addr, bus = key
    if addr in [0x165, 0x166] and bus == 0:
        print(f'  0x{addr:03X}: {data.hex()}', flush=True)

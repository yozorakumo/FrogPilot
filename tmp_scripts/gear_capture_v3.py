#!/usr/bin/env python3
"""Reverse gear + clutch gear hold capture v3"""
import sys, time
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging

sub = messaging.sub_sock('can')
time.sleep(0.5)

prev = {}
pedals_gear_log = []  # (time, gear_pos)
msg28_byte2_log = []  # (time, byte2_raw)

def log_signal(t, msg):
    if len(msg) < 1:
        return
    print(msg, flush=True)

def process(t, addr, bus, old, data):
    if old is None:
        return
    # NEW_MSG_28 (0x166)
    if addr == 0x166 and bus == 0 and len(data) >= 3 and len(old) >= 3:
        # Log byte2 changes (full 8-bit)
        if data[2] != old[2]:
            msg28_byte2_log.append((t, data[2]))
            print(f'  [{t:6.2f}s] 0x166 byte2: 0x{old[2]:02X}->0x{data[2]:02X} (raw={data[2]})', flush=True)
        if data[0] != old[0]:
            clutch = (data[0] >> 7) & 1
            old_clutch = (old[0] >> 7) & 1
            if clutch != old_clutch:
                print(f'  [{t:6.2f}s] 0x166 CLUTCH: {old_clutch}->{clutch}', flush=True)

    # PEDALS (0x165)
    if addr == 0x165 and bus == 0 and len(data) >= 7 and len(old) >= 7:
        if data[6] != old[6]:
            pedals_gear_log.append((t, data[6]))
            print(f'  [{t:6.2f}s] 0x165 PEDALS_GEAR: {old[6]}->{data[6]}', flush=True)
        if data[0] != old[0]:
            new_brake = (data[0] >> 4) & 1
            old_brake = (old[0] >> 4) & 1
            if new_brake != old_brake:
                print(f'  [{t:6.2f}s] 0x165 BRAKE: {old_brake}->{new_brake}', flush=True)

    # Also monitor 0x09E (MSG_11) - parking brake message that might have reverse
    if addr == 0x09E and bus == 0 and len(data) >= 2 and len(old) >= 2:
        if data != old:
            print(f'  [{t:6.2f}s] 0x09E: {old.hex()}->{data.hex()}', flush=True)

    # Monitor ALL IDs for any change during reverse
    # Log any message that changes during the test
    if addr not in [0x165, 0x166, 0x09E] and bus == 0 and data != old:
        pass  # Skip logging for other IDs to reduce noise


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

# Print initial state
print('=== INITIAL STATE ===', flush=True)
start = time.time()
while time.time() - start < 1.0:
    msgs = messaging.recv_sock(sub, True)
    if msgs is None:
        continue
    for msg in msgs.can:
        prev[(msg.address, msg.src)] = bytes(msg.dat)

for key, data in sorted(prev.items()):
    addr, bus = key
    if addr in [0x165, 0x166, 0x09E] and bus == 0:
        extra = ""
        if addr == 0x165:
            extra = f" PEDALS_GEAR={data[6]}" if len(data) > 6 else ""
            extra += f" BRAKE={((data[0]>>4)&1) if len(data)>0 else '?'}"
        if addr == 0x166:
            extra = f" byte2=0x{data[2]:02X} CLUTCH={((data[0]>>7)&1) if len(data)>0 else '?'}" if len(data) > 2 else ""
        print(f'  0x{addr:03X}: {data.hex()}{extra}', flush=True)

# Phase 1: N hold
run_phase("HOLD NEUTRAL", 5, "Stay in Neutral, hands off everything")

# Phase 2: N -> 1st -> hold
run_phase("N -> 1st -> HOLD", 10, "Clutch in -> shift to 1st -> CLUTCH STAY DOWN -> hold 5s")

# Phase 3: 1st -> release clutch slowly
run_phase("1st -> SLOWLY RELEASE CLUTCH", 10, "Slowly release clutch while in 1st gear")

# Phase 4: 1st -> N (clutch in)
run_phase("1st -> N (clutch in)", 10, "Clutch in -> shift to N -> release clutch")

# Phase 5: N -> Reverse
run_phase("N -> REVERSE", 15, "Clutch in -> shift to REVERSE -> hold 5s -> slowly release clutch -> hold 5s")

# Phase 6: R -> N
run_phase("REVERSE -> N", 10, "Clutch in -> shift to N -> release clutch")

print('\n=== DONE ===', flush=True)
print('\n=== FINAL STATE ===', flush=True)
for key, data in sorted(prev.items()):
    addr, bus = key
    if addr in [0x165, 0x166, 0x09E] and bus == 0:
        extra = ""
        if addr == 0x165:
            extra = f" PEDALS_GEAR={data[6]}" if len(data) > 6 else ""
        if addr == 0x166:
            extra = f" byte2=0x{data[2]:02X}" if len(data) > 2 else ""
        print(f'  0x{addr:03X}: {data.hex()}{extra}', flush=True)

print(f'\nPEDALS GEAR changes: {len(pedals_gear_log)}', flush=True)
for t, v in pedals_gear_log:
    print(f'  t={t:.2f}s gear={v}', flush=True)

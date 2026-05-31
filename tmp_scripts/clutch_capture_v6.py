#!/usr/bin/env python3
"""Clutch signal capture v6 - monitor ALL signals, single press test"""
import sys, time
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging

sub = messaging.sub_sock('can')
time.sleep(0.5)

DURATION = 12.0

# Track ALL bit transitions across ALL CAN IDs
transitions = {}  # (addr, bus, byte_idx, bit_idx) -> [(time, old_bit, new_bit), ...]
byte_values = {}  # (addr, bus, byte_idx) -> last value

# Countdown
print('=== CLUTCH CAPTURE v6 - SINGLE PRESS TEST ===', flush=True)
print('Press clutch exactly ONCE when told', flush=True)
print('', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)

print('', flush=True)
print('>>> PRESS CLUTCH ONCE NOW! (press and release) <<<', flush=True)
print('', flush=True)

start = time.time()
last_print = 0
while time.time() - start < DURATION:
    t = time.time() - start
    msgs = messaging.recv_sock(sub, True)
    if msgs is None:
        continue

    for msg in msgs.can:
        addr = msg.address
        bus = msg.src
        data = bytes(msg.dat)

        for i in range(len(data)):
            key = (addr, bus, i)
            old = byte_values.get(key)
            new = data[i]
            byte_values[key] = new

            if old is not None and old != new:
                diff = old ^ new
                for bit in range(8):
                    if diff & (1 << bit):
                        tkey = (addr, bus, i, bit)
                        if tkey not in transitions:
                            transitions[tkey] = []
                        old_bit = (old >> bit) & 1
                        new_bit = (new >> bit) & 1
                        transitions[tkey].append((t, old_bit, new_bit))

    if t - last_print >= 2.0:
        print(f'  [{t:5.1f}s] recording...', flush=True)
        last_print = t

elapsed = time.time() - start
print(f'\n=== DONE ({elapsed:.1f}s) ===', flush=True)

# Find signals with exactly 2 transitions (1 press + 1 release) 
# occurring after start (not baseline)
print('\n=== SIGNALS WITH EXACTLY 2 TRANSITIONS (single ON+OFF) ===', flush=True)
candidates = []
for tkey, entries in transitions.items():
    addr, bus, byte_idx, bit_idx = tkey
    total = len(entries)
    # Looking for exactly 2 transitions: 0->1 then 1->0 (or vice versa)
    if total == 2:
        t1, o1, n1 = entries[0]
        t2, o2, n2 = entries[1]
        # Must be after start (not in initial setup) and before 10s
        if t1 > 0.5 and t2 > t1 and t2 < 10.0:
            candidates.append((tkey, entries))

candidates.sort(key=lambda x: x[1][0][0])

print(f'Found {len(candidates)} signals:', flush=True)
for tkey, entries in candidates:
    addr, bus, byte_idx, bit_idx = tkey
    t1, o1, n1 = entries[0]
    t2, o2, n2 = entries[1]
    duration = t2 - t1
    print(f'  0x{addr:03X} bus={bus} byte[{byte_idx}] bit{bit_idx}: [{t1:.2f}s] {o1}->{n1}, [{t2:.2f}s] {o2}->{n2} (held {duration:.2f}s)', flush=True)

# Also find signals with 3-6 transitions in a narrow window
print('\n=== SIGNALS WITH 2-4 TRANSITIONS (possible clutch) ===', flush=True)
for tkey, entries in transitions.items():
    addr, bus, byte_idx, bit_idx = tkey
    total = len(entries)
    if 3 <= total <= 6:
        times = [t for t, _, _ in entries]
        # All transitions within a 5-second window
        if max(times) - min(times) < 5.0 and min(times) > 0.5:
            times_str = [f"[{t:.2f}s]{o}->{n}" for t, o, n in entries]
            print(f'  0x{addr:03X} bus={bus} byte[{byte_idx}] bit{bit_idx}: {total} transitions', flush=True)
            print(f'    {times_str}', flush=True)

print('\n=== 0x43D ALL CHANGES ===', flush=True)
for tkey, entries in sorted(transitions.items()):
    addr, bus, byte_idx, bit_idx = tkey
    if addr == 0x43D and bus == 0:
        times_str = [f"[{t:.2f}s]{o}->{n}" for t, o, n in entries]
        print(f'  byte[{byte_idx}] bit{bit_idx}: {len(entries)} transitions', flush=True)
        print(f'    {times_str}', flush=True)

print('\n=== 0x365 ALL CHANGES ===', flush=True)
for tkey, entries in sorted(transitions.items()):
    addr, bus, byte_idx, bit_idx = tkey
    if addr == 0x365 and bus == 0:
        times_str = [f"[{t:.2f}s]{o}->{n}" for t, o, n in entries]
        print(f'  byte[{byte_idx}] bit{bit_idx}: {len(entries)} transitions', flush=True)
        print(f'    {times_str}', flush=True)

print('\n=== 0x366 ALL CHANGES ===', flush=True)
for tkey, entries in sorted(transitions.items()):
    addr, bus, byte_idx, bit_idx = tkey
    if addr == 0x366 and bus == 0:
        times_str = [f"[{t:.2f}s]{o}->{n}" for t, o, n in entries]
        print(f'  byte[{byte_idx}] bit{bit_idx}: {len(entries)} transitions', flush=True)
        print(f'    {times_str}', flush=True)

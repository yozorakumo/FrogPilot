#!/usr/bin/env python3
"""Clutch signal capture with countdown - focuses on 0x43D and other key IDs"""
import sys, time
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging

sub = messaging.sub_sock('can')
time.sleep(0.5)

DURATION = 20.0
prev = {}
transitions = {}  # (addr, bus, byte_idx, bit_idx) -> [(time, old_bit, new_bit), ...]

# Countdown
for i in range(5, 0, -1):
    print(f'  Starting in {i}...', flush=True)
    time.sleep(1)

print('\n=== RECORDING STARTED ===', flush=True)
print('Press clutch 3 times NOW! (1 press per second)', flush=True)
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
        key = (addr, bus)

        if key in prev:
            old = prev[key]
            if old != data:
                for i in range(min(len(data), len(old))):
                    if old[i] != data[i]:
                        diff = old[i] ^ data[i]
                        for bit in range(8):
                            if diff & (1 << bit):
                                tkey = (addr, bus, i, bit)
                                if tkey not in transitions:
                                    transitions[tkey] = []
                                old_bit = (old[i] >> bit) & 1
                                new_bit = (data[i] >> bit) & 1
                                transitions[tkey].append((t, old_bit, new_bit))
        prev[key] = data

    # Print countdown every second
    if t - last_print >= 1.0:
        print(f'  [{t:5.1f}s] recording...', flush=True)
        last_print = t

elapsed = time.time() - start
print(f'\n=== RECORDING COMPLETE ({elapsed:.1f}s) ===', flush=True)

# Focus analysis on 0x43D
print('\n=== 0x43D ANALYSIS (TOP CLUTCH CANDIDATE) ===', flush=True)
for tkey in sorted(transitions.keys()):
    addr, bus, byte_idx, bit_idx = tkey
    if addr == 0x43D and bus == 0:
        entries = transitions[tkey]
        times_list = [f"{t:.2f}({o}->{n})" for t, o, n in entries]
        print(f'  0x43D byte[{byte_idx}] bit{bit_idx}: {len(entries)} transitions', flush=True)
        print(f'    {times_list}', flush=True)

# Show 0x43D full byte values over time
print('\n=== 0x43D FULL DATA VALUES OVER TIME ===', flush=True)
# Reconstruct from prev data
tkey_bytes = (0x43D, 0)
if tkey_bytes in prev:
    print(f'  Last data: {prev[tkey_bytes].hex()}', flush=True)

# Check 0x366 byte1 bit7 (current wrong definition)
print('\n=== 0x366 byte[1] bit7 (CURRENT WRONG DEFINITION) ===', flush=True)
tkey = (0x366, 0, 1, 7)
if tkey in transitions:
    entries = transitions[tkey]
    print(f'  {len(entries)} transitions', flush=True)
    for t, o, n in entries:
        print(f'    [{t:5.2f}s] {o}->{n}', flush=True)

# All candidate signals that have transitions only in 5-10s window
print('\n=== ALL SIGNALS WITH 5-10s CONCENTRATED CHANGES ===', flush=True)
for tkey, entries in sorted(transitions.items()):
    addr, bus, byte_idx, bit_idx = tkey
    total = len(entries)
    baseline = sum(1 for t, _, _ in entries if t < 5.0)
    action = sum(1 for t, _, _ in entries if 5.0 <= t <= 10.0)
    after = sum(1 for t, _, _ in entries if t > 10.0)
    if baseline == 0 and 4 <= action <= 12 and after <= 6:
        times_list = [f"{t:.2f}({o}->{n})" for t, o, n in entries]
        print(f'  0x{addr:03X} bus={bus} byte[{byte_idx}] bit{bit_idx}: total={total} action={action}', flush=True)
        print(f'    {times_list}', flush=True)

# Show 0x43D byte2 raw values at transition times
print('\n=== 0x43D BYTE[2] VALUE SEQUENCE ===', flush=True)
if (0x43D, 0) in prev:
    # Collect byte2 values from transitions
    byte2_changes = []
    for tkey, entries in transitions.items():
        addr, bus, byte_idx, bit_idx = tkey
        if addr == 0x43D and bus == 0 and byte_idx == 2:
            for t, o, n in entries:
                byte2_changes.append((t, byte_idx, bit_idx, o, n))
    byte2_changes.sort()
    
    # Reconstruct byte2 values
    val = 0
    for t, _, bit, o, n in byte2_changes:
        if n:
            val |= (1 << bit)
        else:
            val &= ~(1 << bit)
        print(f'  [{t:5.2f}s] bit{bit}: {o}->{n}  byte2=0x{val:02X} ({val:08b})', flush=True)

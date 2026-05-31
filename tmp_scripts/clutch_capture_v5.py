#!/usr/bin/env python3
"""Clutch signal capture v5 - real-time display of 0x43D byte2 values"""
import sys, time
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging

sub = messaging.sub_sock('can')
time.sleep(0.5)

DURATION = 15.0

# Countdown
print('=== CLUTCH CAPTURE v5 ===', flush=True)
print('Will show 0x43D byte[2] value in real-time', flush=True)
print('', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)

print('', flush=True)
print('>>> PRESS CLUTCH 3 TIMES NOW! <<<', flush=True)
print('', flush=True)

start = time.time()
prev_43d_byte2 = None
all_changes = []

while time.time() - start < DURATION:
    t = time.time() - start
    msgs = messaging.recv_sock(sub, True)
    if msgs is None:
        continue

    for msg in msgs.can:
        addr = msg.address
        bus = msg.src
        data = bytes(msg.dat)

        # Track 0x43D byte2 changes in real-time
        if addr == 0x43D and bus == 0 and len(data) > 2:
            cur = data[2]
            if prev_43d_byte2 is not None and cur != prev_43d_byte2:
                print(f'  [{t:6.2f}s] 0x43D byte[2]: 0x{prev_43d_byte2:02X} -> 0x{cur:02X} ({cur:08b})', flush=True)
                all_changes.append((t, prev_43d_byte2, cur))
            prev_43d_byte2 = cur

        # Also track 0x365 byte1
        if addr == 0x365 and bus == 0 and len(data) > 1:
            pass  # silently track

elapsed = time.time() - start
print(f'\n=== DONE ({elapsed:.1f}s) ===', flush=True)
print(f'Total 0x43D byte[2] changes: {len(all_changes)}', flush=True)

# Summary
print('\n=== SUMMARY ===', flush=True)
for t, old, new in all_changes:
    # Determine state
    if new == 0x20:
        state = "RELEASED"
    elif new == 0x1F:
        state = "PRESSED"
    elif new == 0x21:
        state = "LIGHT TOUCH"
    else:
        state = f"PARTIAL({new:08b})"
    print(f'  [{t:6.2f}s] 0x{old:02X} -> 0x{new:02X}  {state}', flush=True)

#!/usr/bin/env python3
"""Clutch FINAL verification - show top 3 candidates in real-time"""
import sys, time
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging

sub = messaging.sub_sock('can')
time.sleep(0.5)

DURATION = 12.0

# Candidates to monitor in real-time
CANDIDATES = [
    (0x166, 0, 0, 7, "0x166 byte0 bit7"),
    (0x130, 0, 7, 7, "0x130 byte7 bit7"),
    (0x366, 0, 0, 0, "0x366 byte0 bit0"),
    (0x43D, 0, 2, 5, "0x43D byte2 bit5"),
]

prev = {}  # (addr, bus) -> data bytes

print('=== CLUTCH FINAL VERIFICATION ===', flush=True)
print('Monitoring top 4 candidates in real-time', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)

print('', flush=True)
print('>>> PRESS CLUTCH ONCE NOW! <<<', flush=True)
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
        key = (addr, bus)

        if key in prev:
            old = prev[key]
            for addr_c, bus_c, byte_c, bit_c, name in CANDIDATES:
                if addr == addr_c and bus == bus_c and len(data) > byte_c and len(old) > byte_c:
                    old_byte = old[byte_c]
                    new_byte = data[byte_c]
                    old_bit = (old_byte >> bit_c) & 1
                    new_bit = (new_byte >> bit_c) & 1
                    if old_bit != new_bit:
                        print(f'  [{t:6.2f}s] {name}: {old_bit}->{new_bit}', flush=True)

        prev[key] = data

    if t - last_print >= 2.0:
        print(f'  [{t:5.1f}s] ...', flush=True)
        last_print = t

print(f'\n=== DONE ===', flush=True)

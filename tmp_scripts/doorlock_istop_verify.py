#!/usr/bin/env python3
"""Detailed verification of door lock and i-stop button signal candidates"""
import sys, time
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging

sub = messaging.sub_sock('can')
time.sleep(0.5)

def monitor_signals(label, duration, watch_list):
    """Monitor specific signals in real-time"""
    print(f'\n=== {label} ({duration}s) ===', flush=True)
    start = time.time()
    results = {k: [] for k in watch_list}
    
    while time.time() - start < duration:
        t = time.time() - start
        msgs = messaging.recv_sock(sub, True)
        if msgs is None:
            continue
        for msg in msgs.can:
            addr, bus = msg.address, msg.src
            if bus != 0:
                continue
            data = bytes(msg.dat)
            for (a, byte_idx, desc) in watch_list:
                if addr == a and byte_idx < len(data):
                    k = (a, byte_idx, desc)
                    if k not in results:
                        results[k] = []
                    val = data[byte_idx]
                    # Only record on change
                    if not results[k] or results[k][-1][1] != val:
                        results[k].append((t, val))
    
    for (a, byte_idx, desc) in watch_list:
        k = (a, byte_idx, desc)
        vals = results.get(k, [])
        print(f'  0x{a:03X} byte[{byte_idx}] ({desc}): {len(vals)} changes', flush=True)
        for t, v in vals[:20]:
            bits = f'{v:08b}'
            print(f'    t={t:.2f}s val=0x{v:02X} ({bits})', flush=True)
        if len(vals) > 20:
            print(f'    ... ({len(vals) - 20} more)', flush=True)
    
    return results

# ============================================================
# DOOR LOCK VERIFICATION
# Candidates: 0x3D2 byte[0], byte[1]
# ============================================================
print('='*60, flush=True)
print('DOOR LOCK VERIFICATION', flush=True)
print('='*60, flush=True)

door_watch = [
    (0x3D2, 0, "DOOR_byte0"),
    (0x3D2, 1, "DOOR_byte1"),
    (0x436, 0, "436_byte0"),
    (0x436, 1, "436_byte1"),
    (0x436, 2, "436_byte2"),
    (0x43D, 3, "43D_byte3"),
    (0x43D, 4, "43D_byte4"),
    (0x43F, 3, "43F_byte3"),
]

print('\nPhase 1: Doors UNLOCKED (baseline)', flush=True)
monitor_signals("UNLOCKED baseline", 3, door_watch)

print('\n>>> LOCK doors now! <<<', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
monitor_signals("LOCKED", 3, door_watch)

print('\n>>> UNLOCK doors now! <<<', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
monitor_signals("UNLOCKED again", 3, door_watch)

# ============================================================
# i-STOP BUTTON VERIFICATION
# Candidates: 0x130 byte[1]
# ============================================================
print('\n' + '='*60, flush=True)
print('i-STOP BUTTON VERIFICATION', flush=True)
print('='*60, flush=True)

istop_watch = [
    (0x130, 0, "130_byte0"),
    (0x130, 1, "130_byte1"),
    (0x130, 6, "130_byte6"),
    (0x130, 7, "130_byte7"),
    (0x091, 3, "091_byte3"),
    (0x091, 4, "091_byte4"),
]

print('\nPhase 1: i-stop current state (baseline)', flush=True)
monitor_signals("i-stop BASELINE", 3, istop_watch)

print('\n>>> PRESS i-stop button NOW! <<<', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
print('>>> PRESS! <<<', flush=True)
time.sleep(0.5)
monitor_signals("i-stop AFTER PRESS 1", 3, istop_watch)

print('\n>>> PRESS i-stop button AGAIN! <<<', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
print('>>> PRESS! <<<', flush=True)
time.sleep(0.5)
monitor_signals("i-stop AFTER PRESS 2", 3, istop_watch)

print('\n>>> PRESS i-stop button THIRD TIME! <<<', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
print('>>> PRESS! <<<', flush=True)
time.sleep(0.5)
monitor_signals("i-stop AFTER PRESS 3", 3, istop_watch)

print('\nDone!', flush=True)

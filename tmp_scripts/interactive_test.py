#!/usr/bin/env python3
"""Interactive CAN dump - monitors changes and waits for user action"""
import sys, time
sys.path.insert(0, '/data/openpilot')
from panda import Panda

p = Panda()
print('Panda connected:', p.get_type(), flush=True)

def monitor(duration, label):
    print(f'\n=== {label} ({duration}s) ===', flush=True)
    start = time.time()
    prev_data = {}
    changes = []
    
    while time.time() - start < duration:
        msgs = p.can_recv()
        for msg in msgs:
            addr = msg[0]
            bus = msg[3]
            data = msg[2]
            
            if bus != 0:
                continue
            
            key = (addr, bus)
            old_data = prev_data.get(key)
            
            if old_data != data:
                if old_data is not None:  # Only show changes (not first-seen)
                    changes.append((time.time()-start, addr, old_data, data))
                    print(f'  [{time.time()-start:5.1f}s] 0x{addr:03X}: {" ".join(f"{b:02X}" for b in old_data)} -> {" ".join(f"{b:02X}" for b in data)}', flush=True)
                prev_data[key] = data
    
    return changes

# ============================================================
print('='*60, flush=True)
print('INTERACTIVE CAN DUMP TEST', flush=True)
print('='*60, flush=True)

# Phase 1: Baseline
print('\n--- Phase 1: Baseline ---', flush=True)
monitor(3, "BASELINE")

# Phase 2: Door Lock
print('\n' + '='*60, flush=True)
print('>>> PHASE 2: LOCK the doors NOW <<<', flush=True)
print('>>> (Use key fob LOCK button) <<<', flush=True)
print('>>> Monitoring for 15 seconds <<<', flush=True)
print('='*60, flush=True)
lock_changes = monitor(15, "DOOR LOCK")

# Phase 3: Door Unlock
print('\n' + '='*60, flush=True)
print('>>> PHASE 3: UNLOCK the doors NOW <<<', flush=True)
print('>>> (Use key fob UNLOCK button) <<<', flush=True)
print('>>> Monitoring for 15 seconds <<<', flush=True)
print('='*60, flush=True)
unlock_changes = monitor(15, "DOOR UNLOCK")

# Phase 4: i-stop OFF
print('\n' + '='*60, flush=True)
print('>>> PHASE 4: PRESS i-stop button NOW <<<', flush=True)
print('>>> (To turn i-stop OFF) <<<', flush=True)
print('>>> Monitoring for 15 seconds <<<', flush=True)
print('='*60, flush=True)
istop_off_changes = monitor(15, "i-STOP OFF")

# Phase 5: i-stop ON
print('\n' + '='*60, flush=True)
print('>>> PHASE 5: PRESS i-stop button AGAIN <<<', flush=True)
print('>>> (To turn i-stop back ON) <<<', flush=True)
print('>>> Monitoring for 15 seconds <<<', flush=True)
print('='*60, flush=True)
istop_on_changes = monitor(15, "i-STOP ON")

# Summary
print('\n' + '='*60, flush=True)
print('SUMMARY', flush=True)
print('='*60, flush=True)
print(f'  Lock phase changes: {len(lock_changes)}', flush=True)
print(f'  Unlock phase changes: {len(unlock_changes)}', flush=True)
print(f'  i-stop OFF changes: {len(istop_off_changes)}', flush=True)
print(f'  i-stop ON changes: {len(istop_on_changes)}', flush=True)

# Show only unique addresses that changed
all_addrs = set()
for changes in [lock_changes, unlock_changes, istop_off_changes, istop_on_changes]:
    for _, addr, _, _ in changes:
        all_addrs.add(addr)

print(f'\n  Addresses that changed: {", ".join(f"0x{a:03X}" for a in sorted(all_addrs))}', flush=True)
print('\nDone!', flush=True)
p.close()

#!/usr/bin/env python3
"""Full CAN dump to identify door lock and i-stop signal changes"""
import sys, time
sys.path.insert(0, '/data/openpilot')
from panda import Panda

p = Panda()
print('Panda connected:', p.get_type(), flush=True)

def full_dump(duration, label, target_addrs=None):
    """Dump all CAN messages, highlighting changes in target addresses"""
    print(f'\n=== {label} ({duration}s) ===', flush=True)
    start = time.time()
    prev_data = {}
    
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
            
            # Show all changes for target addresses, first-seen for others
            is_target = target_addrs and addr in target_addrs
            
            if old_data != data:
                if is_target or old_data is None:
                    marker = " ***" if is_target else ""
                    print(f'  [{time.time()-start:5.1f}s] 0x{addr:03X} bus={bus}: {" ".join(f"{b:02X}" for b in data)}{marker}', flush=True)
                prev_data[key] = data

# ============================================================
print('='*60, flush=True)
print('FULL CAN DUMP WITH CHANGE DETECTION', flush=True)
print('='*60, flush=True)

# Phase 1: Baseline - capture all messages
print('\n--- Phase 1: Baseline (5s) ---', flush=True)
full_dump(5, "BASELINE")

# Phase 2: Door lock - user operates
print('\n--- Phase 2: DOOR LOCK (10s) ---', flush=True)
print('>>> Please LOCK the doors now (key fob or door switch) <<<', flush=True)
full_dump(10, "DOOR LOCK TEST")

# Phase 3: Door unlock - user operates
print('\n--- Phase 3: DOOR UNLOCK (10s) ---', flush=True)
print('>>> Please UNLOCK the doors now <<<', flush=True)
full_dump(10, "DOOR UNLOCK TEST")

# Phase 4: i-stop button
print('\n--- Phase 4: i-STOP BUTTON (10s) ---', flush=True)
print('>>> Please PRESS the i-stop button NOW <<<', flush=True)
full_dump(10, "i-STOP TEST")

# Phase 5: i-stop button again (toggle back)
print('\n--- Phase 5: i-STOP BUTTON TOGGLE BACK (10s) ---', flush=True)
print('>>> Please PRESS the i-stop button AGAIN <<<', flush=True)
full_dump(10, "i-STOP TOGGLE BACK")

print('\nDone!', flush=True)
p.close()

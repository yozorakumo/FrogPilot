#!/usr/bin/env python3
"""CAN signal scan for door lock and i-stop (idle stop) button signals"""
import sys, time
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging

sub = messaging.sub_sock('can')
time.sleep(0.5)

def get_snapshot(label, duration=4):
    """Get min/max/current for all CAN message bytes"""
    print(f'\n=== {label} ({duration}s) ===', flush=True)
    start = time.time()
    data_range = {}  # (addr, byte_idx) -> [min, max, current]
    
    while time.time() - start < duration:
        msgs = messaging.recv_sock(sub, True)
        if msgs is None:
            continue
        for msg in msgs.can:
            addr, bus = msg.address, msg.src
            if bus != 0:
                continue
            data = bytes(msg.dat)
            for byte_idx in range(len(data)):
                k = (addr, byte_idx)
                if k not in data_range:
                    data_range[k] = [data[byte_idx], data[byte_idx], data[byte_idx]]
                else:
                    data_range[k][0] = min(data_range[k][0], data[byte_idx])
                    data_range[k][1] = max(data_range[k][1], data[byte_idx])
                    data_range[k][2] = data[byte_idx]
    
    return data_range

def find_differences(before, after, label, max_changes=50):
    """Find bytes that changed between two snapshots"""
    changes = []
    for k in after:
        addr, byte_idx = k
        b_min, b_max, b_cur = before.get(k, (0, 0, 0))
        a_min, a_max, a_cur = after[k]
        
        # Check if range shifted
        if a_min > b_max or a_max < b_min or a_cur != b_cur:
            changes.append((addr, byte_idx, b_cur, a_cur, b_min, b_max, a_min, a_max))
    
    print(f'\n--- {label}: {len(changes)} bytes changed ---', flush=True)
    for addr, byte_idx, b_cur, a_cur, b_min, b_max, a_min, a_max in sorted(changes):
        range_b = f"[0x{b_min:02X}-0x{b_max:02X}]" if b_min != b_max else f"0x{b_cur:02X}"
        range_a = f"[0x{a_min:02X}-0x{a_max:02X}]" if a_min != a_max else f"0x{a_cur:02X}"
        print(f'  0x{addr:03X} byte[{byte_idx}]: {range_b} -> {range_a}', flush=True)
    return changes

# ============================================================
# PART 1: DOOR LOCK SIGNAL
# ============================================================
print('='*60, flush=True)
print('PART 1: DOOR LOCK SIGNAL DETECTION', flush=True)
print('='*60, flush=True)

print('\nPreparation: Ensure all doors are CLOSED and UNLOCKED.', flush=True)
print('Do NOT press any buttons yet.', flush=True)
for i in range(5, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)

# Phase 1: All doors unlocked (baseline)
baseline_unlocked = get_snapshot("BASELINE: All doors unlocked", 4)

# Phase 2: Lock all doors (using key fob or door switch)
print('\n>>> LOCK all doors (key fob LOCK button or door lock switch) <<<', flush=True)
for i in range(5, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
locked = get_snapshot("ALL DOORS LOCKED", 4)

# Phase 3: Unlock all doors
print('\n>>> UNLOCK all doors (key fob UNLOCK button) <<<', flush=True)
for i in range(5, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
unlocked = get_snapshot("ALL DOORS UNLOCKED", 4)

# Analysis
print('\n' + '='*60, flush=True)
print('DOOR LOCK ANALYSIS', flush=True)
print('='*60, flush=True)

# Find signals that changed when locked (vs baseline) and changed back when unlocked
lock_changes = find_differences(baseline_unlocked, locked, "Unlocked -> Locked")
unlock_changes = find_differences(locked, unlocked, "Locked -> Unlocked")

# Cross-reference: signals that toggled back
lock_addrs = set((addr, byte_idx) for addr, byte_idx, *_ in lock_changes)
unlock_addrs = set((addr, byte_idx) for addr, byte_idx, *_ in unlock_changes)
both = lock_addrs & unlock_addrs

print(f'\n--- DOOR LOCK CANDIDATES (changed in both lock & unlock) ---', flush=True)
for addr, byte_idx in sorted(both):
    b_cur = baseline_unlocked.get((addr, byte_idx), (0,0,0))[2]
    l_cur = locked.get((addr, byte_idx), (0,0,0))[2]
    u_cur = unlocked.get((addr, byte_idx), (0,0,0))[2]
    print(f'  ✅ 0x{addr:03X} byte[{byte_idx}]: Unlocked=0x{b_cur:02X} Locked=0x{l_cur:02X} Unlocked=0x{u_cur:02X}', flush=True)

# ============================================================
# PART 2: i-STOP (IDLE STOP) BUTTON SIGNAL
# ============================================================
print('\n\n' + '='*60, flush=True)
print('PART 2: i-STOP BUTTON SIGNAL DETECTION', flush=True)
print('='*60, flush=True)

print('\nPreparation: Note the current i-stop indicator status.', flush=True)
print('We will press the i-stop button to toggle it.', flush=True)
for i in range(5, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)

# Phase 1: i-stop current state
istop_baseline = get_snapshot("i-STOP BASELINE (current state)", 4)

# Phase 2: Press i-stop button
print('\n>>> PRESS the i-stop button NOW! <<<', flush=True)
for i in range(5, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
print('>>> PRESS i-STOP BUTTON! <<<', flush=True)
istop_after1 = get_snapshot("i-STOP AFTER FIRST PRESS", 4)

# Phase 3: Press i-stop button again (toggle back)
print('\n>>> PRESS the i-stop button AGAIN (toggle back) <<<', flush=True)
for i in range(5, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
print('>>> PRESS i-STOP BUTTON AGAIN! <<<', flush=True)
istop_after2 = get_snapshot("i-STOP AFTER SECOND PRESS", 4)

# Analysis
print('\n' + '='*60, flush=True)
print('i-STOP BUTTON ANALYSIS', flush=True)
print('='*60, flush=True)

press1_changes = find_differences(istop_baseline, istop_after1, "i-stop before -> after 1st press")
press2_changes = find_differences(istop_after1, istop_after2, "i-stop after 1st -> after 2nd press")

p1_addrs = set((addr, byte_idx) for addr, byte_idx, *_ in press1_changes)
p2_addrs = set((addr, byte_idx) for addr, byte_idx, *_ in press2_changes)
istop_both = p1_addrs & p2_addrs

print(f'\n--- i-STOP CANDIDATES (changed on both presses) ---', flush=True)
for addr, byte_idx in sorted(istop_both):
    b_cur = istop_baseline.get((addr, byte_idx), (0,0,0))[2]
    a1_cur = istop_after1.get((addr, byte_idx), (0,0,0))[2]
    a2_cur = istop_after2.get((addr, byte_idx), (0,0,0))[2]
    print(f'  ✅ 0x{addr:03X} byte[{byte_idx}]: Before=0x{b_cur:02X} Press1=0x{a1_cur:02X} Press2=0x{a2_cur:02X}', flush=True)

# ============================================================
# SUMMARY
# ============================================================
print('\n\n' + '='*60, flush=True)
print('SUMMARY', flush=True)
print('='*60, flush=True)

print('\nDOOR LOCK signals:', flush=True)
for addr, byte_idx in sorted(both):
    b_cur = baseline_unlocked.get((addr, byte_idx), (0,0,0))[2]
    l_cur = locked.get((addr, byte_idx), (0,0,0))[2]
    u_cur = unlocked.get((addr, byte_idx), (0,0,0))[2]
    print(f'  0x{addr:03X} byte[{byte_idx}]: Unlocked=0x{b_cur:02X} Locked=0x{l_cur:02X} Unlocked=0x{u_cur:02X}', flush=True)

print('\ni-STOP signals:', flush=True)
for addr, byte_idx in sorted(istop_both):
    b_cur = istop_baseline.get((addr, byte_idx), (0,0,0))[2]
    a1_cur = istop_after1.get((addr, byte_idx), (0,0,0))[2]
    a2_cur = istop_after2.get((addr, byte_idx), (0,0,0))[2]
    print(f'  0x{addr:03X} byte[{byte_idx}]: Before=0x{b_cur:02X} Press1=0x{a1_cur:02X} Press2=0x{a2_cur:02X}', flush=True)

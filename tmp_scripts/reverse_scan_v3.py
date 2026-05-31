#!/usr/bin/env python3
"""Simpler reverse scan: dump specific message bytes at each phase"""
import sys, time
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging

sub = messaging.sub_sock('can')
time.sleep(0.5)

def get_snapshot(label, duration=3):
    """Get stable CAN snapshot for key messages"""
    print(f'\n=== {label} ({duration}s) ===', flush=True)
    start = time.time()
    # Track min/max for each byte of key messages
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
                    data_range[k][2] = data[byte_idx]  # current
    
    return data_range

# Key messages to watch (based on previous results)
WATCH_ADDRS = [0x165, 0x166, 0x167, 0x09E, 0x09F, 0x050, 0x078, 0x130, 0x200, 0x202, 0x340, 0x365, 0x366, 0x420, 0x436, 0x45A, 0x4FE]

# Phase 1: Neutral baseline
print('=== PHASE 1: NEUTRAL (clutch NOT pressed) ===', flush=True)
print('Stay in Neutral. Do NOT press clutch.', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
neutral_no_clutch = get_snapshot("NEUTRAL no clutch", 4)

# Phase 2: Neutral, clutch pressed
print('\n>>> Press clutch now. Stay in Neutral. <<<', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
neutral_clutch = get_snapshot("NEUTRAL clutch pressed", 4)

# Phase 3: Reverse, clutch pressed
print('\n>>> Shift to REVERSE. KEEP CLUTCH PRESSED! <<<', flush=True)
for i in range(5, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
reverse_clutch = get_snapshot("REVERSE clutch pressed", 5)

# Phase 4: Back to Neutral, clutch still pressed
print('\n>>> Shift back to NEUTRAL. KEEP CLUTCH PRESSED! <<<', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
neutral_clutch2 = get_snapshot("NEUTRAL clutch pressed again", 4)

# Analysis
print('\n' + '='*60, flush=True)
print('=== REVERSE CANDIDATES ===', flush=True)
print('Bytes that changed ONLY in Reverse (not in any Neutral phase):', flush=True)
print('='*60, flush=True)

candidates = []
for k in reverse_clutch:
    addr, byte_idx = k
    if addr not in WATCH_ADDRS:
        continue
    
    rev_min, rev_max, rev_cur = reverse_clutch[k]
    
    # Get ranges for all neutral phases
    n1 = neutral_no_clutch.get(k, [0, 0, 0])
    n2 = neutral_clutch.get(k, [0, 0, 0])
    n3 = neutral_clutch2.get(k, [0, 0, 0])
    
    n_min = min(n1[0], n2[0], n3[0])
    n_max = max(n1[1], n2[1], n3[1])
    
    # Check if reverse range is outside neutral range
    if rev_min > n_max or rev_max < n_min:
        # This byte has different values in reverse!
        candidates.append((addr, byte_idx, rev_min, rev_max, n_min, n_max))
        print(f'  ✅ 0x{addr:03X} byte[{byte_idx}]: Neutral=[0x{n_min:02X}-0x{n_max:02X}] Reverse=[0x{rev_min:02X}-0x{rev_max:02X}]', flush=True)

if not candidates:
    print('  No clear candidates found. Checking bytes with any range difference...', flush=True)
    for k in reverse_clutch:
        addr, byte_idx = k
        if addr not in WATCH_ADDRS:
            continue
        rev_min, rev_max, rev_cur = reverse_clutch[k]
        n1 = neutral_no_clutch.get(k, [0, 0, 0])
        n2 = neutral_clutch.get(k, [0, 0, 0])
        n3 = neutral_clutch2.get(k, [0, 0, 0])
        
        # Show ranges if they differ
        all_neutral_ranges = f"N1=[0x{n1[0]:02X}-0x{n1[1]:02X}] N2=[0x{n2[0]:02X}-0x{n2[1]:02X}] N3=[0x{n3[0]:02X}-0x{n3[1]:02X}]"
        rev_range = f"R=[0x{rev_min:02X}-0x{rev_max:02X}]"
        
        if rev_range != f"R=[0x{n2[0]:02X}-0x{n2[1]:02X}]":  # Different from clutch-only neutral
            print(f'  0x{addr:03X} byte[{byte_idx}]: {all_neutral_ranges} {rev_range}', flush=True)

print('\n--- Full data for key messages ---', flush=True)
for addr in WATCH_ADDRS:
    print(f'\n  0x{addr:03X}:', flush=True)
    for byte_idx in range(8):
        k = (addr, byte_idx)
        n1 = neutral_no_clutch.get(k, None)
        n2 = neutral_clutch.get(k, None)
        rev = reverse_clutch.get(k, None)
        n3 = neutral_clutch2.get(k, None)
        if n1 and n2 and rev and n3:
            print(f'    byte[{byte_idx}]: N_noClutch=0x{n1[2]:02X} N_clutch=0x{n2[2]:02X} REVERSE=0x{rev[2]:02X} N_again=0x{n3[2]:02X}', flush=True)

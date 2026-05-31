#!/usr/bin/env python3
"""Full CAN scan for reverse signal - clutch stays pressed"""
import sys, time
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging

sub = messaging.sub_sock('can')
time.sleep(0.5)

prev = {}

def run_scan(name, duration):
    """Scan ALL CAN messages and track which bits change"""
    print(f'\n=== {name} ({duration}s) ===', flush=True)
    start = time.time()
    changes = {}  # (addr, byte_idx, bit_idx) -> [(t, old_val, new_val)]
    
    while time.time() - start < duration:
        t = time.time() - start
        msgs = messaging.recv_sock(sub, True)
        if msgs is None:
            continue
        for msg in msgs.can:
            addr, bus = msg.address, msg.src
            data = bytes(msg.dat)
            key = (addr, bus)
            
            if bus != 0:
                continue
                
            old = prev.get(key)
            if old is not None and len(old) == len(data):
                for byte_idx in range(len(data)):
                    if data[byte_idx] != old[byte_idx]:
                        xor = data[byte_idx] ^ old[byte_idx]
                        for bit_idx in range(8):
                            if xor & (1 << bit_idx):
                                k = (addr, byte_idx, bit_idx)
                                if k not in changes:
                                    changes[k] = []
                                changes[k].append((t, (old[byte_idx] >> bit_idx) & 1, (data[byte_idx] >> bit_idx) & 1))
            
            prev[key] = data
    
    return changes

def get_stable_state():
    """Get current state of all CAN messages"""
    start = time.time()
    while time.time() - start < 0.5:
        msgs = messaging.recv_sock(sub, True)
        if msgs is None:
            continue
        for msg in msgs.can:
            if msg.src == 0:
                prev[(msg.address, msg.src)] = bytes(msg.dat)
    return {k: v for k, v in prev.items() if k[1] == 0}

# Step 0: Get initial state
print('=== PREPARATION ===', flush=True)
print('Pull handbrake + press brake. Stay in Neutral.', flush=True)
for i in range(5, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)

state_n1 = get_stable_state()
print(f'Initial state captured: {len(state_n1)} messages', flush=True)

# Step 1: Baseline - clutch pressed, Neutral
print('\n>>> Press clutch and HOLD it down. Stay in Neutral. <<<', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)

baseline = run_scan("BASELINE (N, clutch pressed)", 6)

# Step 2: Shift to Reverse (clutch stays pressed!)
print('\n>>> While clutch still pressed: shift to REVERSE. KEEP CLUTCH PRESSED! <<<', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)

reverse = run_scan("REVERSE (clutch pressed)", 8)

# Step 3: Back to Neutral (clutch stays pressed)
print('\n>>> While clutch still pressed: shift back to NEUTRAL. <<<', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)

neutral2 = run_scan("NEUTRAL AGAIN (clutch pressed)", 6)

# Step 4: Release clutch
print('\n>>> Release clutch now. <<<', flush=True)
time.sleep(2)

state_n2 = get_stable_state()

# Analysis
print('\n' + '='*60, flush=True)
print('=== ANALYSIS ===', flush=True)
print('='*60, flush=True)

# Find signals that changed ONLY during reverse (not in baseline AND not in neutral2)
exclusive_reverse = {}
for k, v in reverse.items():
    baseline_v = baseline.get(k, [])
    neutral2_v = neutral2.get(k, [])
    
    # Get final values in reverse phase
    last_reverse_val = v[-1][2]  # Last new value in reverse
    
    # Check if this signal had changes in reverse that didn't happen in baseline/neutral2
    if len(v) > len(baseline_v) and len(v) > len(neutral2_v):
        exclusive_reverse[k] = {
            'n_baseline': len(baseline_v),
            'n_reverse': len(v),
            'n_neutral2': len(neutral2_v),
            'last_val': last_reverse_val,
            'transitions': v
        }

print(f'\nSignals with MORE activity in REVERSE than Neutral: {len(exclusive_reverse)}', flush=True)
for k, info in sorted(exclusive_reverse.items(), key=lambda x: -x[1]['n_reverse']):
    addr, byte_idx, bit_idx = k
    print(f'\n  0x{addr:03X} byte[{byte_idx}] bit{bit_idx}: baseline={info["n_baseline"]} reverse={info["n_reverse"]} neutral={info["n_neutral2"]} last={info["last_val"]}', flush=True)
    for t, old, new in info['transitions'][:10]:
        print(f'    t={t:.2f}s {old}->{new}', flush=True)

# Also find signals that changed to a STABLE new value during reverse
# and changed back during neutral2
print('\n--- Signals with stable transition in REVERSE ---', flush=True)
for k, v in reverse.items():
    addr, byte_idx, bit_idx = k
    baseline_v = baseline.get(k, [])
    neutral2_v = neutral2.get(k, [])
    
    # Skip if too many transitions (likely periodic noise)
    if len(v) > 20:
        continue
    
    # Check if value settled during reverse
    if len(v) >= 1:
        final_val = v[-1][2]
        initial_val = v[0][1]
        
        # Did it change back in neutral2?
        for t, old, new in neutral2_v:
            if old == final_val and new == initial_val:
                print(f'  0x{addr:03X} byte[{byte_idx}] bit{bit_idx}: {initial_val}->{final_val} (reverse) then {final_val}->{initial_val} (neutral) ✅', flush=True)
                break

# Compare initial vs final CAN states
print('\n--- CAN state changes: N1 vs N2 ---', flush=True)
for k in state_n1:
    addr, bus = k
    if k in state_n2:
        d1 = state_n1[k]
        d2 = state_n2[k]
        if d1 != d2:
            diff_bytes = []
            for i in range(min(len(d1), len(d2))):
                if d1[i] != d2[i]:
                    diff_bytes.append(f'byte{i}:0x{d1[i]:02X}->0x{d2[i]:02X}')
            if diff_bytes:
                print(f'  0x{addr:03X}: {", ".join(diff_bytes)}', flush=True)

# Show key message states
print('\n--- Key messages final state ---', flush=True)
for key, data in sorted(prev.items()):
    addr, bus = key
    if addr in [0x165, 0x166, 0x09E, 0x09F] and bus == 0:
        extra = ""
        if addr == 0x165 and len(data) > 6:
            extra = f" PEDALS_GEAR={data[6]}"
        if addr == 0x166 and len(data) > 2:
            extra = f" byte2=0x{data[2]:02X}"
        print(f'  0x{addr:03X}: {data.hex()}{extra}', flush=True)

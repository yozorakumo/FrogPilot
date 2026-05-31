#!/usr/bin/env python3
"""Full CAN scan for reverse gear signal detection"""
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
            
            if bus != 0:  # Only bus 0 (vehicle CAN)
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

# Step 1: Baseline in Neutral (brake + handbrake)
print('=== PREPARATION ===', flush=True)
print('1. Pull handbrake', flush=True)
print('2. Press and HOLD brake pedal', flush=True)
print('3. Start in Neutral', flush=True)
for i in range(5, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)

baseline = run_scan("BASELINE (Neutral, brake+handbrake)", 5)

# Clear baseline changes (these are normal periodic signals)
# We only care about NEW changes after gear shift

# Step 2: Shift to Reverse (clutch in, then release)
print('\nNow: Press clutch -> shift to REVERSE -> RELEASE CLUTCH', flush=True)
print('Keep brake pressed! Car should not move.', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
print('>>> SHIFT TO REVERSE AND RELEASE CLUTCH! <<<', flush=True)

reverse = run_scan("REVERSE (brake+handbrake, clutch released)", 8)

# Step 3: Back to Neutral
print('\nNow: Press clutch -> shift to Neutral -> release clutch', flush=True)
for i in range(3, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
print('>>> SHIFT TO NEUTRAL AND RELEASE CLUTCH! <<<', flush=True)

neutral2 = run_scan("NEUTRAL AGAIN (brake+handbrake)", 8)

# Analysis: Find signals that changed in REVERSE but NOT in baseline/neutral2
print('\n=== ANALYSIS ===', flush=True)

# Get all signals that changed during reverse phase
reverse_only = {}
for k, v in reverse.items():
    addr, byte_idx, bit_idx = k
    # Check if this signal changed differently in baseline or neutral2
    baseline_changes = baseline.get(k, [])
    neutral2_changes = neutral2.get(k, [])
    
    # Count transitions in each phase
    n_baseline = len(baseline_changes)
    n_reverse = len(v)
    n_neutral2 = len(neutral2_changes)
    
    # Interesting: changed more in reverse than in neutral
    if n_reverse > max(n_baseline, n_neutral2) + 2:
        reverse_only[k] = {
            'reverse': n_reverse,
            'baseline': n_baseline, 
            'neutral2': n_neutral2,
            'transitions': v[:5]  # First 5 transitions
        }

print(f'\nSignals with MORE activity during REVERSE: {len(reverse_only)}', flush=True)
for k, info in sorted(reverse_only.items()):
    addr, byte_idx, bit_idx = k
    print(f'  0x{addr:03X} byte[{byte_idx}] bit{bit_idx}: baseline={info["baseline"]} reverse={info["reverse"]} neutral={info["neutral2"]}', flush=True)
    for t, old, new in info['transitions']:
        print(f'    t={t:.2f}s {old}->{new}', flush=True)

# Also show all signals that changed ONLY in reverse (not in baseline AND not in neutral2)
exclusive = {}
for k, v in reverse.items():
    if k not in baseline and k not in neutral2:
        exclusive[k] = v

print(f'\nSignals that changed ONLY in REVERSE (not in baseline or neutral): {len(exclusive)}', flush=True)
for k, v in sorted(exclusive.items()):
    addr, byte_idx, bit_idx = k
    print(f'  0x{addr:03X} byte[{byte_idx}] bit{bit_idx}: {len(v)} transitions', flush=True)
    for t, old, new in v[:5]:
        print(f'    t={t:.2f}s {old}->{new}', flush=True)

# Show current state of key messages
print('\n=== CURRENT STATE ===', flush=True)
for key, data in sorted(prev.items()):
    addr, bus = key
    if bus == 0:
        print(f'  0x{addr:03X}: {data.hex()}', flush=True)

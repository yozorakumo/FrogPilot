#!/usr/bin/env python3
"""Analyze clutch capture data - find signals that change only during clutch operation (5-10s)"""
import sys, time, os
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging

OUTPUT = '/tmp/clutch_analysis.txt'
f = open(OUTPUT, 'w')

def log(msg):
    print(msg, flush=True)
    f.write(msg + '\n')

sub = messaging.sub_sock('can')
time.sleep(0.5)

DURATION = 20.0
# Track per (addr, bus, byte_idx, bit_idx): list of transition times
transitions = {}  # key -> [timestamps]
byte_values = {}  # (addr, bus, byte_idx) -> last value

start = time.time()
log('=== CLUTCH ANALYSIS v3 ===')
log(f'Starting {DURATION}s capture. Press clutch 3x starting at 5s.')

while time.time() - start < DURATION:
    t = time.time() - start
    msgs = messaging.recv_sock(sub, True)
    if msgs is None:
        continue

    for msg in msgs.can:
        addr = msg.address
        bus = msg.src
        data = bytes(msg.dat)

        for i in range(len(data)):
            key_base = (addr, bus, i)
            old = byte_values.get(key_base)
            new = data[i]
            byte_values[key_base] = new

            if old is not None and old != new:
                diff = old ^ new
                for bit in range(8):
                    if diff & (1 << bit):
                        tkey = (addr, bus, i, bit)
                        if tkey not in transitions:
                            transitions[tkey] = []
                        transitions[tkey].append(t)

elapsed = time.time() - start
log(f'\nCapture complete ({elapsed:.1f}s)')
log(f'Total unique (addr,bus,byte,bit) transitions tracked: {len(transitions)}')

# Analyze: find signals with transitions concentrated in 5-10s window
BASELINE_END = 5.0
ACTION_START = 5.0
ACTION_END = 10.0

log(f'\n=== SIGNALS WITH TRANSITIONS CONCENTRATED IN {ACTION_START}-{ACTION_END}s WINDOW ===')

results = []
for tkey, times in transitions.items():
    addr, bus, byte_idx, bit_idx = tkey
    total = len(times)
    if total < 2:  # Need at least 2 transitions (on and off)
        continue

    baseline_count = sum(1 for t in times if t < BASELINE_END)
    action_count = sum(1 for t in times if ACTION_START <= t <= ACTION_END)
    after_count = sum(1 for t in times if t > ACTION_END)

    # We want: minimal baseline changes, significant action changes
    # Clutch: 3 presses = 6 transitions ideally (3x on + 3x off)
    # But might be slightly more due to timing
    if baseline_count == 0 and 4 <= action_count <= 12 and after_count <= 4:
        ratio = action_count / total if total > 0 else 0
        results.append((tkey, total, baseline_count, action_count, after_count, times))

results.sort(key=lambda x: -x[4])  # Sort by action_count

log(f'\nFound {len(results)} candidate signals:')
for tkey, total, baseline, action, after, times in results:
    addr, bus, byte_idx, bit_idx = tkey
    log(f'\n  0x{addr:03X} bus={bus} byte[{byte_idx}] bit{bit_idx}: total={total} baseline={baseline} action={action} after={after}')
    log(f'    Transition times: {[f"{t:.2f}" for t in times]}')

# Also specifically check 0x366 byte1 bit7 (current CLUTCH_PEDAL definition)
log(f'\n=== CHECK CURRENT CLUTCH DEFINITION (0x366 byte1 bit7) ===')
tkey = (0x366, 0, 1, 7)
if tkey in transitions:
    times = transitions[tkey]
    log(f'  0x366 bus=0 byte[1] bit7: {len(times)} transitions')
    log(f'    Times: {[f"{t:.2f}" for t in times]}')
else:
    log(f'  0x366 bus=0 byte[1] bit7: NO transitions (always same value)')

# Check all bits of 0x366
log(f'\n=== ALL 0x366 BIT TRANSITIONS ===')
for tkey, times in sorted(transitions.items()):
    addr, bus, byte_idx, bit_idx = tkey
    if addr == 0x366 and bus == 0:
        log(f'  byte[{byte_idx}] bit{bit_idx}: {len(times)} transitions - times: {[f"{t:.2f}" for t in times[:20]]}')

# Check all bits of 0x165 (PEDALS)
log(f'\n=== ALL 0x165 BIT TRANSITIONS (PEDALS) ===')
for tkey, times in sorted(transitions.items()):
    addr, bus, byte_idx, bit_idx = tkey
    if addr == 0x165 and bus == 0:
        log(f'  byte[{byte_idx}] bit{bit_idx}: {len(times)} transitions - times: {[f"{t:.2f}" for t in times[:20]]}')

# Check 0x357 (GEAR)
log(f'\n=== ALL 0x357 BIT TRANSITIONS ===')
for tkey, times in sorted(transitions.items()):
    addr, bus, byte_idx, bit_idx = tkey
    if addr == 0x357 and bus == 0:
        log(f'  byte[{byte_idx}] bit{bit_idx}: {len(times)} transitions - times: {[f"{t:.2f}" for t in times[:20]]}')

f.close()
log(f'\nAnalysis saved to {OUTPUT}')

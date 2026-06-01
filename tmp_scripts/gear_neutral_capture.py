#!/usr/bin/env python3
"""Capture GEAR_POS values for each gear position including neutral"""
import sys, time
sys.path.insert(0, '/data/openpilot')
from panda import Panda

p = Panda()
print('Panda connected:', p.get_type(), flush=True)

def capture_gears(label, duration=8):
    """Capture GEAR_POS and related signals for a period"""
    print(f'\n=== {label} ({duration}s) ===', flush=True)
    print('  Time   GEAR_POS  CLUTCH  REVERSE  RAW_PEDALS_0x165_byte2', flush=True)
    start = time.time()
    seen_gears = set()
    
    while time.time() - start < duration:
        msgs = p.can_recv()
        for msg in msgs:
            addr = msg[0]
            bus = msg[3]
            data = msg[2]
            
            if bus != 0:
                continue
            
            if addr == 0x165:  # PEDALS
                byte2 = data[2] if len(data) > 2 else 0
                byte3 = data[3] if len(data) > 3 else 0
                
                # GEAR_POS is bits 0-3 of byte2 (from DBC: 4-bit signal)
                gear_pos = byte2 & 0x0F
                reverse = byte3 & 0x01
                
                if gear_pos not in seen_gears:
                    seen_gears.add(gear_pos)
                    t = time.time() - start
                    raw_hex = ' '.join(f'{b:02X}' for b in data)
                    print(f'  [{t:5.1f}s] GEAR_POS={gear_pos:2d} (0x{gear_pos:01X})  REV={reverse}  RAW={raw_hex}', flush=True)
    
    print(f'  Unique GEAR_POS values seen: {sorted(seen_gears)}', flush=True)
    return seen_gears

# ============================================================
print('='*60, flush=True)
print('GEAR POSITION CAPTURE FOR EACH GEAR', flush=True)
print('='*60, flush=True)

# Phase 1: Neutral (engine running, car stopped, no clutch)
print('\n>>> Put shifter in NEUTRAL, release clutch <<<', flush=True)
time.sleep(3)
neutral_gears = capture_gears("NEUTRAL (engine on, clutch released)", 8)

# Phase 2: 1st gear (clutch pressed first, then engage)
print('\n>>> Press clutch, shift to 1st gear, release clutch <<<', flush=True)
time.sleep(3)
gear1 = capture_gears("1st GEAR (clutch released)", 5)

# Phase 3: 2nd gear
print('\n>>> Shift to 2nd gear <<<', flush=True)
time.sleep(3)
gear2 = capture_gears("2nd GEAR", 5)

# Phase 4: 3rd gear
print('\n>>> Shift to 3rd gear <<<', flush=True)
time.sleep(3)
gear3 = capture_gears("3rd GEAR", 5)

# Phase 5: 4th gear
print('\n>>> Shift to 4th gear <<<', flush=True)
time.sleep(3)
gear4 = capture_gears("4th GEAR", 5)

# Phase 6: 5th gear
print('\n>>> Shift to 5th gear <<<', flush=True)
time.sleep(3)
gear5 = capture_gears("5th GEAR", 5)

# Phase 7: 6th gear
print('\n>>> Shift to 6th gear <<<', flush=True)
time.sleep(3)
gear6 = capture_gears("6th GEAR", 5)

# Phase 8: Reverse
print('\n>>> Shift to REVERSE <<<', flush=True)
time.sleep(3)
gearR = capture_gears("REVERSE", 5)

# Phase 9: Press clutch while in neutral
print('\n>>> Put in NEUTRAL, press clutch <<<', flush=True)
time.sleep(3)
neutral_clutch = capture_gears("NEUTRAL + CLUTCH PRESSED", 5)

# Phase 10: Press clutch while in 1st
print('\n>>> Put in 1st, press clutch <<<', flush=True)
time.sleep(3)
gear1_clutch = capture_gears("1st + CLUTCH PRESSED", 5)

# Summary
print('\n' + '='*60, flush=True)
print('SUMMARY', flush=True)
print('='*60, flush=True)
print(f'  Neutral:        GEAR_POS = {sorted(neutral_gears)}', flush=True)
print(f'  Neutral+Clutch: GEAR_POS = {sorted(neutral_clutch)}', flush=True)
print(f'  1st:            GEAR_POS = {sorted(gear1)}', flush=True)
print(f'  1st+Clutch:     GEAR_POS = {sorted(gear1_clutch)}', flush=True)
print(f'  2nd:            GEAR_POS = {sorted(gear2)}', flush=True)
print(f'  3rd:            GEAR_POS = {sorted(gear3)}', flush=True)
print(f'  4th:            GEAR_POS = {sorted(gear4)}', flush=True)
print(f'  5th:            GEAR_POS = {sorted(gear5)}', flush=True)
print(f'  6th:            GEAR_POS = {sorted(gear6)}', flush=True)
print(f'  Reverse:        GEAR_POS = {sorted(gearR)}', flush=True)

print('\nDone!', flush=True)
p.close()

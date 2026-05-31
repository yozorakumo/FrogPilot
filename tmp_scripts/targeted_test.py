#!/usr/bin/env python3
"""Targeted CAN dump - only shows specific addresses"""
import sys, time
sys.path.insert(0, '/data/openpilot')
from panda import Panda

TARGET_ADDRS = {0x436, 0x130, 0x420, 0x0_420}

p = Panda()
print('Panda connected:', p.get_type(), flush=True)
print('Monitoring: 0x436 (DOOR_LOCK_FB), 0x130 (ISTOP_STATUS), 0x420 (BCM)', flush=True)

def monitor_targeted(duration, label):
    print(f'\n=== {label} ({duration}s) ===', flush=True)
    start = time.time()
    prev = {}
    
    while time.time() - start < duration:
        msgs = p.can_recv()
        for msg in msgs:
            addr = msg[0]
            bus = msg[3]
            data = msg[2]
            
            if bus != 0 or addr not in TARGET_ADDRS:
                continue
            
            old = prev.get(addr)
            if old != data:
                name = {0x436: "DOOR_LOCK_FB", 0x130: "ISTOP_STATUS", 0x420: "BCM"}.get(addr, f"0x{addr:03X}")
                change = ""
                if old is not None:
                    diffs = []
                    for i in range(len(data)):
                        if i < len(old) and old[i] != data[i]:
                            ob, nb = old[i], data[i]
                            bit_diffs = [str(b) for b in range(8) if (ob ^ nb) & (1 << b)]
                            diffs.append(f"byte{i}:0x{ob:02X}->0x{nb:02X}(bits{','.join(bit_diffs)})")
                    change = " | " + ", ".join(diffs)
                print(f'  [{time.time()-start:5.1f}s] {name}: {" ".join(f"{b:02X}" for b in data)}{change}', flush=True)
                prev[addr] = data

# ============================================================
print('='*60, flush=True)
print('TARGETED CAN MONITOR', flush=True)
print('='*60, flush=True)

# Baseline
monitor_targeted(5, "BASELINE")

# Lock
print('\n>>> LOCK the doors NOW (key fob) <<<', flush=True)
monitor_targeted(15, "DOOR LOCK")

# Unlock
print('\n>>> UNLOCK the doors NOW (key fob) <<<', flush=True)
monitor_targeted(15, "DOOR UNLOCK")

# i-stop OFF
print('\n>>> PRESS i-stop button NOW (turn OFF) <<<', flush=True)
monitor_targeted(15, "i-STOP OFF")

# i-stop ON
print('\n>>> PRESS i-stop button AGAIN (turn ON) <<<', flush=True)
monitor_targeted(15, "i-STOP ON")

print('\nDone!', flush=True)
p.close()

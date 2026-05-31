#!/usr/bin/env python3
"""Test i-stop cancel commands on various CAN IDs"""
import sys, time
sys.path.insert(0, '/data/openpilot')
from panda import Panda

p = Panda()
print('Panda connected:', p.get_type(), flush=True)

def send_and_monitor_istop(addr, bus, data, label, monitor_duration=3):
    """Send a CAN message and monitor for 0x130 changes"""
    print(f'\n  Trying: 0x{addr:03X} bus={bus} data={" ".join(f"{b:02X}" for b in data)} ({label})', flush=True)
    
    # Get baseline
    baseline = None
    start = time.time()
    while time.time() - start < 0.5:
        msgs = p.can_recv()
        for msg in msgs:
            if msg[0] == 0x130 and msg[3] == 0:
                baseline = msg[2]
    
    if baseline:
        istop_state = "OFF" if baseline[1] & 0x02 else "ON"
        print(f'  Baseline: {" ".join(f"{b:02X}" for b in baseline)} (i-stop {istop_state})', flush=True)
    
    # Send command
    for _ in range(3):
        p.can_send(addr, bytes(data), bus)
        time.sleep(0.1)
    
    # Monitor for changes
    start = time.time()
    changed = False
    while time.time() - start < monitor_duration:
        msgs = p.can_recv()
        for msg in msgs:
            if msg[0] == 0x130 and msg[3] == 0:
                data_rcv = msg[2]
                if baseline and data_rcv[1] != baseline[1]:
                    new_istop = "OFF" if data_rcv[1] & 0x02 else "ON"
                    old_istop = "OFF" if baseline[1] & 0x02 else "ON"
                    print(f'  *** ISTOP CHANGED: byte1 0x{baseline[1]:02X} ({old_istop}) -> 0x{data_rcv[1]:02X} ({new_istop})', flush=True)
                    changed = True
    
    if not changed:
        print(f'  No i-stop change detected', flush=True)
    return changed

# ============================================================
print('='*60, flush=True)
print('TEST: i-stop cancel commands', flush=True)
print('='*60, flush=True)

# Get current i-stop state
print('\n>>> Current i-stop state: <<<', flush=True)
time.sleep(0.5)
start = time.time()
while time.time() - start < 1:
    msgs = p.can_recv()
    for msg in msgs:
        if msg[0] == 0x130 and msg[3] == 0:
            d = msg[2]
            istop = "OFF (disabled)" if d[1] & 0x02 else "ON (enabled)"
            print(f'  0x130: {" ".join(f"{b:02X}" for b in d)} (i-stop {istop})', flush=True)
            break

# Test 1: Try 0x130 itself (the i-stop status address)
print('\n--- Test 1: Send on 0x130 ---', flush=True)
send_and_monitor_istop(0x130, 0, [0x00, 0x02, 0x00, 0x30, 0x01, 0x01, 0x00, 0x00], "0x130 byte1=0x02 (ISTOP_OFF=1)")
send_and_monitor_istop(0x130, 0, [0x00, 0x00, 0x00, 0x30, 0x01, 0x01, 0x00, 0x00], "0x130 byte1=0x00 (ISTOP_OFF=0)")

# Test 2: Try BCM (0x420) with i-stop related data
print('\n--- Test 2: BCM (0x420) with i-stop commands ---', flush=True)
send_and_monitor_istop(0x420, 0, [0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], "BCM byte1=0x02")
send_and_monitor_istop(0x420, 0, [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02], "BCM byte7=0x02")

# Test 3: Try various addresses that might control i-stop
print('\n--- Test 3: Scan likely i-stop control IDs ---', flush=True)
for addr in [0x131, 0x132, 0x133, 0x134, 0x135, 0x136, 0x137, 0x138, 0x139]:
    send_and_monitor_istop(addr, 0, [0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], f"0x{addr:03X} byte1=0x02")

# Test 4: UDS diagnostic for i-stop
print('\n--- Test 4: UDS diagnostic for i-stop ---', flush=True)
# Try UDS on various diagnostic addresses
for diag_addr in [0x7E0, 0x760, 0x762]:
    # UDS RoutineControl to disable i-stop
    send_and_monitor_istop(diag_addr, 0, [0x31, 0x01, 0x85, 0x00, 0x00, 0x00, 0x00, 0x00], f"UDS RoutineControl 0x{diag_addr:03X}")

# Test 5: Try 0x0FD and 0x0FE (known Mazda addresses near i-stop)
print('\n--- Test 5: Nearby Mazda addresses ---', flush=True)
for addr in [0x12E, 0x12F, 0x131, 0x13A, 0x13B, 0x13C, 0x13D, 0x13E, 0x13F]:
    send_and_monitor_istop(addr, 0, [0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], f"0x{addr:03X} byte0=0x02")

print('\nDone!', flush=True)
p.close()

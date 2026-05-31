#!/usr/bin/env python3
"""Brute-force scan for door lock and i-stop command CAN IDs"""
import sys, time, struct
sys.path.insert(0, '/data/openpilot')
from panda import Panda

p = Panda()
print('Panda connected:', p.get_type(), flush=True)

def send_and_monitor(addr, bus, data, label, monitor_duration=2):
    """Send a CAN message and monitor for 0x436 changes"""
    print(f'\n  Trying: 0x{addr:03X} bus={bus} data={" ".join(f"{b:02X}" for b in data)} ({label})', flush=True)
    
    # Get baseline
    baseline_door = None
    start = time.time()
    while time.time() - start < 0.3:
        msgs = p.can_recv()
        for msg in msgs:
            if msg[0] == 0x436 and msg[3] == 0:
                baseline_door = msg[2]
    
    # Send command
    p.can_send(addr, bytes(data), bus)
    time.sleep(0.1)
    p.can_send(addr, bytes(data), bus)
    time.sleep(0.1)
    p.can_send(addr, bytes(data), bus)
    
    # Monitor for changes
    start = time.time()
    changed = False
    while time.time() - start < monitor_duration:
        msgs = p.can_recv()
        for msg in msgs:
            if msg[0] == 0x436 and msg[3] == 0:
                if baseline_door and msg[2] != baseline_door:
                    print(f'  *** RESPONSE! 0x436 changed: {" ".join(f"{b:02X}" for b in baseline_door)} -> {" ".join(f"{b:02X}" for b in msg[2])}', flush=True)
                    changed = True
    return changed

# ============================================================
print('='*60, flush=True)
print('SCAN: Looking for door lock command CAN ID', flush=True)
print('='*60, flush=True)

# Wait for door to be unlocked
print('\n>>> Please ensure doors are UNLOCKED before starting <<<', flush=True)
time.sleep(2)

# Test 1: Try sending on BCM TX ID (0x420) with different data
print('\n--- Test 1: BCM (0x420) with lock commands ---', flush=True)
send_and_monitor(0x420, 0, [0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], "BCM lock byte0=0x0A")
send_and_monitor(0x420, 0, [0x82, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], "BCM lock byte0=0x82")
send_and_monitor(0x420, 0, [0x8A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], "BCM lock byte0=0x8A")

# Test 2: Try nearby BCM IDs
print('\n--- Test 2: Scan BCM area (0x421-0x42F) ---', flush=True)
for addr in range(0x421, 0x430):
    send_and_monitor(addr, 0, [0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], f"lock 0x{addr:03X}")

# Test 3: Try door lock feedback ID (0x436) - maybe BCM reads from here?
print('\n--- Test 3: DOOR_LOCK_FB (0x436) with lock data ---', flush=True)
send_and_monitor(0x436, 0, [0x14, 0x01, 0x80, 0x36, 0xFF, 0x64, 0x00, 0x01], "0x436 lock")

# Test 4: Try common Mazda BCM command IDs
print('\n--- Test 4: Known Mazda BCM command IDs ---', flush=True)
# Some Mazda models use 0x430-0x43F for BCM commands
for addr in [0x430, 0x431, 0x432, 0x433, 0x434, 0x435]:
    send_and_monitor(addr, 0, [0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], f"lock 0x{addr:03X}")

# Test 5: Try UDS diagnostic commands
print('\n--- Test 5: UDS diagnostic commands ---', flush=True)
# UDS RoutineControl (0x31) to BCM on 0x760/0x762
# Try BCM diagnostic address 0x760
for diag_addr in [0x760, 0x762]:
    # UDS TesterPresent
    send_and_monitor(diag_addr, 0, [0x3E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], f"TesterPresent 0x{diag_addr:03X}")
    # UDS RoutineControl (start routine) - door lock
    send_and_monitor(diag_addr, 0, [0x31, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00], f"RoutineControl lock 0x{diag_addr:03X}")
    # UDS IOControl - door lock
    send_and_monitor(diag_addr, 0, [0x2F, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00], f"IOControl lock 0x{diag_addr:03X}")

# Test 6: Try keyless entry module ID
print('\n--- Test 6: Keyless/Door module IDs ---', flush=True)
for addr in [0x43A, 0x43B, 0x43C, 0x43D, 0x43E, 0x43F]:
    send_and_monitor(addr, 0, [0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], f"lock 0x{addr:03X}")

# Test 7: Try CAN bus 1 (MS-CAN)
print('\n--- Test 7: Try different bus numbers ---', flush=True)
send_and_monitor(0x420, 1, [0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], "BCM lock bus=1")
send_and_monitor(0x420, 128, [0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00], "BCM lock bus=128")

print('\n>>> Scan complete. If no responses found, commands may require MS-CAN bus <<<', flush=True)
print('\nDone!', flush=True)
p.close()

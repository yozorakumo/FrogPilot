#!/usr/bin/env python3
"""Send door lock/unlock and i-stop cancel CAN commands via Panda on Mazda 2 DJ MT"""
import sys, time
sys.path.insert(0, '/data/openpilot')
from panda import Panda

p = Panda()
print('Panda connected:', p.get_type(), flush=True)

def send_can(addr, bus, data):
    """Send CAN message via Panda"""
    p.can_send(addr, data, bus)
    print(f'  Sent: 0x{addr:03X} bus={bus} data={" ".join(f"{b:02X}" for b in data)}', flush=True)

def monitor(duration, label):
    print(f'\n=== {label} ({duration}s) ===', flush=True)
    start = time.time()
    last_door = None
    last_istop = None
    
    while time.time() - start < duration:
        msgs = p.can_recv()
        for msg in msgs:
            addr = msg[0]    # address
            bus = msg[3]     # bus
            data = msg[2]    # data bytes
            
            if bus != 0:
                continue
            
            if addr == 0x436:  # DOOR_LOCK_FB
                key = data[1] if len(data) > 1 else None
                if key != last_door:
                    last_door = key
                    locked = "LOCKED" if data[1] & 0x01 else "UNLOCKED"
                    b2 = data[2] if len(data) > 2 else 0
                    print(f'  [{time.time()-start:5.1f}s] DOOR_LOCK_FB: byte1=0x{data[1]:02X} ({locked}) byte2=0x{b2:02X}', flush=True)
            
            elif addr == 0x130:  # ISTOP_STATUS
                key = data[1] if len(data) > 1 else None
                if key != last_istop:
                    last_istop = key
                    istop = "OFF (disabled)" if data[1] & 0x02 else "ON (enabled)"
                    print(f'  [{time.time()-start:5.1f}s] ISTOP_STATUS: byte1=0x{data[1]:02X} ({istop})', flush=True)

# ============================================================
print('='*60, flush=True)
print('CAN COMMAND TEST: Door Lock/Unlock + i-stop Cancel', flush=True)
print('='*60, flush=True)

# Baseline
monitor(3, "BASELINE - current state")

# ---- TEST 1: Door Lock via BCM (0x420) ----
print('\n>>> TEST 1: DOOR LOCK (BCM 0x420) <<<', flush=True)
# BCM message: byte0=0x00, byte1=DOOR_LOCK_STATUS(2=lock), byte2=0x00, DOOR_LOCK_ALL=1
# From DBC: BCM has DOOR_LOCK_STATUS (byte0 bits 0-2) and DOOR_LOCK_ALL (byte0 bit 3)
# Wait, let me check the DBC definition...
# BO_ 304 BCM: 8 XXX
# SG_ DOOR_LOCK_STATUS : 0|3@1+ (1,0) [0|7] "" XXX
# SG_ DOOR_LOCK_ALL : 3|1@1+ (1,0) [0|1] "" XXX
# So DOOR_LOCK_STATUS is bits 0-2 of byte0, DOOR_LOCK_ALL is bit 3 of byte0
# Lock: DOOR_LOCK_STATUS=2, DOOR_LOCK_ALL=1 → byte0 = 0b00001010 = 0x0A
send_can(0x420, 0, bytes([0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))
time.sleep(0.3)
send_can(0x420, 0, bytes([0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))
time.sleep(0.3)
send_can(0x420, 0, bytes([0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))

monitor(3, "AFTER LOCK COMMAND")

# ---- TEST 2: Door Unlock via BCM (0x420) ----
print('\n>>> TEST 2: DOOR UNLOCK (BCM 0x420) <<<', flush=True)
# Unlock: DOOR_LOCK_STATUS=1, DOOR_LOCK_ALL=0 → byte0 = 0b00000001 = 0x01
send_can(0x420, 0, bytes([0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))
time.sleep(0.3)
send_can(0x420, 0, bytes([0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))
time.sleep(0.3)
send_can(0x420, 0, bytes([0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))

monitor(3, "AFTER UNLOCK COMMAND")

# ---- TEST 3: i-stop Cancel via BCM (0x420) ----
print('\n>>> TEST 3: i-STOP CANCEL <<<', flush=True)
# Try different approaches:
# Approach 1: Send 0x420 with just DOOR_LOCK_STATUS=0
send_can(0x420, 0, bytes([0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))
time.sleep(0.5)
monitor(3, "AFTER i-STOP CANCEL ATTEMPT 1")

# Approach 2: Try sending on the i-stop address (0x130)
print('\n>>> TEST 3b: i-STOP CANCEL via 0x130 <<<', flush=True)
# Send ISTOP_OFF=1 on 0x130 byte1 bit1
send_can(0x130, 0, bytes([0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))
time.sleep(0.5)
monitor(3, "AFTER i-STOP CANCEL ATTEMPT 2")

print('\n>>> Please press i-stop button manually now <<<', flush=True)
for i in range(5, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
monitor(3, "AFTER MANUAL i-STOP PRESS")

print('\nDone!', flush=True)
p.close()

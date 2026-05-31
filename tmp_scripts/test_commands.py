#!/usr/bin/env python3
"""Send door lock/unlock and i-stop cancel CAN commands on Mazda 2 DJ MT"""
import sys, time, struct
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging
from opendbc.can.packer import CANPacker

packer = CANPacker('mazda_2_dj_mt')

# Subscribe to CAN bus
sub = messaging.sub_sock('can')
# Publish to CAN bus  
pub = messaging.pub_sock('sendcan')

time.sleep(0.5)

def send_can(addr, bus, data):
    """Send a single CAN message"""
    msg = messaging.new_message('sendcan', 1)
    send = msg.sendcan[0]
    send.address = addr
    send.bus = bus
    send.dat = bytes(data)
    send.src = 0
    pub.send(msg.to_bytes())
    print(f'  Sent: addr=0x{addr:03X} bus={bus} data={" ".join(f"{b:02X}" for b in data)}', flush=True)

def monitor(duration, label):
    print(f'\n=== {label} ({duration}s) ===', flush=True)
    start = time.time()
    last_door = None
    last_istop = None
    
    while time.time() - start < duration:
        msgs = messaging.recv_sock(sub, True)
        if msgs is None:
            continue
        for msg in msgs.can:
            addr, bus = msg.address, msg.src
            if bus != 0:
                continue
            data = bytes(msg.dat)
            
            if addr == 0x436:  # DOOR_LOCK_FB
                key = data[1]
                if key != last_door:
                    last_door = key
                    locked = "LOCKED" if data[1] & 0x01 else "UNLOCKED"
                    print(f'  [{time.time()-start:5.1f}s] DOOR_LOCK_FB: byte1=0x{data[1]:02X} ({locked}) byte2=0x{data[2]:02X}', flush=True)
            
            elif addr == 0x130:  # ISTOP_STATUS
                key = data[1]
                if key != last_istop:
                    last_istop = key
                    istop = "OFF (disabled)" if data[1] & 0x02 else "ON (enabled)"
                    print(f'  [{time.time()-start:5.1f}s] ISTOP_STATUS: byte1=0x{data[1]:02X} ({istop})', flush=True)

# ============================================================
print('='*60, flush=True)
print('CAN COMMAND TEST: Door Lock/Unlock + i-stop Cancel', flush=True)
print('='*60, flush=True)

# Baseline
monitor(3, "BASELINE")

# ---- TEST 1: Door Lock ----
print('\n>>> Sending DOOR LOCK (BCM: DOOR_LOCK_STATUS=2, DOOR_LOCK_ALL=1) <<<', flush=True)
lock_msg = packer.make_can_msg("BCM", 0, {"DOOR_LOCK_STATUS": 2, "DOOR_LOCK_ALL": 1})
addr, bus, data, src = lock_msg
send_can(addr, bus, data)
time.sleep(0.3)
send_can(addr, bus, data)  # repeat
time.sleep(0.3)
send_can(addr, bus, data)  # repeat
time.sleep(0.5)

monitor(3, "AFTER LOCK COMMAND")

# ---- TEST 2: Door Unlock ----
print('\n>>> Sending DOOR UNLOCK (BCM: DOOR_LOCK_STATUS=1, DOOR_LOCK_ALL=0) <<<', flush=True)
unlock_msg = packer.make_can_msg("BCM", 0, {"DOOR_LOCK_STATUS": 1, "DOOR_LOCK_ALL": 0})
addr, bus, data, src = unlock_msg
send_can(addr, bus, data)
time.sleep(0.3)
send_can(addr, bus, data)
time.sleep(0.3)
send_can(addr, bus, data)
time.sleep(0.5)

monitor(3, "AFTER UNLOCK COMMAND")

# ---- TEST 3: i-stop Cancel ----
print('\n>>> Sending i-STOP CANCEL (ISTOP_OFF=1 on BCM) <<<', flush=True)
# Try sending ISTOP_OFF=1 via a dedicated message
# Method 1: Use BCM message with ISTOP_OFF signal
try:
    istop_msg = packer.make_can_msg("BCM_ISTOP", 0, {"ISTOP_OFF": 1})
    addr, bus, data, src = istop_msg
    send_can(addr, bus, data)
except Exception as e:
    print(f'  BCM_ISTOP not found, trying raw: {e}', flush=True)
    # Method 2: Raw CAN message on BCM address
    # i-stop button is on 0x130 - try sending feedback to 0x420 with istop signal
    send_can(0x420, 0, [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])

time.sleep(0.5)
monitor(3, "AFTER i-STOP CANCEL")

print('\n>>> Now press i-stop button manually <<<', flush=True)
for i in range(5, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
monitor(3, "AFTER MANUAL i-STOP PRESS")

print('\nDone!', flush=True)

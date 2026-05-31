#!/usr/bin/env python3
"""Test door lock/unlock and i-stop cancel CAN commands on Mazda 2 DJ MT"""
import sys, time
sys.path.insert(0, '/data/openpilot')

from opendbc.can.packer import CANPacker
from opendbc.can.parser import CANParser
import cereal.messaging as messaging

packer = CANPacker('mazda_2_dj_mt')
sub = messaging.sub_sock('can')
send_sock = messaging.pub_sock('can')

time.sleep(0.5)

def send_can_msg(msg):
    """Send a CAN message"""
    send_sock.send(msg.to_bytes())

def read_signal(msg_name, signal_name, duration=2):
    """Read a signal value"""
    parser = CANParser('mazda_2_dj_mt', [(msg_name, 10)], bus=0)
    time.sleep(duration)
    msgs = messaging.recv_sock(sub, True)
    if msgs:
        for m in msgs.can:
            pass
    # Create fresh parser
    parser = CANParser('mazda_2_dj_mt', [(msg_name, 10)], bus=0)
    start = time.time()
    while time.time() - start < duration:
        msgs = messaging.recv_sock(sub, True)
        if msgs:
            for m in msgs.can:
                pass
    return parser.vl[msg_name][signal_name]

def monitor_signals(duration, label):
    """Monitor door lock and i-stop signals"""
    print(f'\n=== {label} ({duration}s) ===', flush=True)
    start = time.time()
    door_values = set()
    istop_values = set()
    
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
                door_values.add(data[1])
                locked = "LOCKED" if data[1] & 0x01 else "UNLOCKED"
                confirm = "CONFIRM" if data[2] & 0x80 else "no-confirm"
                print(f'  t={time.time()-start:.2f}s DOOR_LOCK_FB: byte1=0x{data[1]:02X} ({locked}) byte2=0x{data[2]:02X} ({confirm})', flush=True)
            elif addr == 0x130:  # ISTOP_STATUS
                istop_values.add(data[1])
                istop = "OFF (disabled)" if data[1] & 0x02 else "ON (enabled)"
                print(f'  t={time.time()-start:.2f}s ISTOP_STATUS: byte1=0x{data[1]:02X} ({istop})', flush=True)
    
    return door_values, istop_values

# ============================================================
# TEST 1: Door Lock/Unlock via BCM (0x420)
# ============================================================
print('='*60, flush=True)
print('TEST 1: DOOR LOCK/UNLOCK COMMANDS', flush=True)
print('='*60, flush=True)

print('\n--- Current state (baseline) ---', flush=True)
monitor_signals(3, "BASELINE")

# Send door LOCK command
print('\n>>> Sending DOOR LOCK command via BCM <<<', flush=True)
lock_msg = packer.make_can_msg("BCM", 0, {"DOOR_LOCK_STATUS": 2, "DOOR_LOCK_ALL": 1})
send_can_msg(lock_msg)
time.sleep(0.5)
# Send again for reliability
send_can_msg(lock_msg)
time.sleep(1)

print('\n--- After LOCK command ---', flush=True)
monitor_signals(3, "AFTER LOCK")

# Send door UNLOCK command
print('\n>>> Sending DOOR UNLOCK command via BCM <<<', flush=True)
unlock_msg = packer.make_can_msg("BCM", 0, {"DOOR_LOCK_STATUS": 1, "DOOR_LOCK_ALL": 0})
send_can_msg(unlock_msg)
time.sleep(0.5)
send_can_msg(unlock_msg)
time.sleep(1)

print('\n--- After UNLOCK command ---', flush=True)
monitor_signals(3, "AFTER UNLOCK")

# ============================================================
# TEST 2: i-stop Cancel
# ============================================================
print('\n\n' + '='*60, flush=True)
print('TEST 2: i-STOP CANCEL', flush=True)
print('='*60, flush=True)

print('\n--- Current i-stop state ---', flush=True)
monitor_signals(3, "i-stop BASELINE")

# Note: The i-stop cancel command needs to be determined.
# Currently sending a BCM message as placeholder.
# The actual i-stop cancel may require a different message or UDS command.
print('\n>>> NOTE: i-stop cancel command is a placeholder <<<', flush=True)
print('>>> Actual i-stop button press signal needs separate CAN message <<<', flush=True)
print('>>> Use the physical i-stop button for now <<<', flush=True)

print('\n--- Please press the i-stop button manually ---', flush=True)
for i in range(5, 0, -1):
    print(f'  {i}...', flush=True)
    time.sleep(1)
monitor_signals(3, "AFTER i-stop button press")

print('\nDone!', flush=True)

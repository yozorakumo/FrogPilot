#!/usr/bin/env python3
"""Monitor door lock (0x436) and i-stop (0x130) signals in real-time"""
import sys, time
sys.path.insert(0, '/data/openpilot')
import cereal.messaging as messaging

sub = messaging.sub_sock('can')
time.sleep(0.5)

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
                key = ('door', data[1], data[2])
                if key != last_door:
                    last_door = key
                    locked = "LOCKED" if data[1] & 0x01 else "UNLOCKED"
                    confirm = "LOCKED" if data[2] & 0x80 else "UNLOCKED"
                    print(f'  [{time.time()-start:5.1f}s] DOOR: byte1=0x{data[1]:02X} ({locked}) byte2=0x{data[2]:02X} ({confirm})', flush=True)
            
            elif addr == 0x130:  # ISTOP_STATUS
                key = ('istop', data[1])
                if key != last_istop:
                    last_istop = key
                    istop = "OFF (disabled)" if data[1] & 0x02 else "ON (enabled)"
                    print(f'  [{time.time()-start:5.1f}s] ISTOP: byte1=0x{data[1]:02X} ({istop})', flush=True)

print('='*60, flush=True)
print('REAL-TIME MONITOR: Door Lock + i-stop Status', flush=True)
print('='*60, flush=True)
print('\nWatching 0x436 (DOOR_LOCK_FB) and 0x130 (ISTOP_STATUS)', flush=True)
print('Lock/unlock doors or press i-stop button to see changes.', flush=True)

# Monitor continuously in 30-second blocks
for i in range(6):
    monitor(30, f"Block {i+1}")
    if i < 5:
        print('\n  --- continuing ---', flush=True)

print('\nDone!', flush=True)

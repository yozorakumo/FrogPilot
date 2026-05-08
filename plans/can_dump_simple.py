import time
from panda import Panda

p = Panda()
targets = [0x09A, 0x091, 0x082, 0x086, 0x240]
labels = {0x09A: 'BLINK_INFO', 0x091: 'TURN_SWITCH', 0x082: 'STEER', 0x086: 'STEER2', 0x240: 'STEER_TORQUE'}

states = [
    ('=== Phase 1: No turn signal (10 sec) ===', 10),
    ('=== Phase 2: LEFT turn signal (10 sec) ===', 10),
    ('=== Phase 3: RIGHT turn signal (10 sec) ===', 10),
    ('=== Phase 4: HAZARD (10 sec) ===', 10),
]

for label, duration in states:
    print(label)
    start = time.time()
    last_print = {}
    while time.time() - start < duration:
        msgs = p.can_recv()
        for m in msgs:
            addr = m[0]
            if addr in targets:
                data = m[2]
                data_hex = data.hex()
                key = addr
                now = time.time()
                if key not in last_print or now - last_print[key] >= 0.2:
                    name = labels.get(addr, 'UNKNOWN')
                    elapsed = time.time() - start
                    print(f'  t={elapsed:5.1f}s 0x{addr:03X} ({name:14s}) Data: {data_hex}')
                    last_print[key] = now
    print()
    time.sleep(1)

print('=== DONE ===')
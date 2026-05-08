import time
from panda import Panda

p = Panda()
targets = [0x09A, 0x091, 0x082, 0x086, 0x240]
labels = {0x09A: 'BLINK_INFO', 0x091: 'TURN_SWITCH', 0x082: 'STEER', 0x086: 'STEER2', 0x240: 'STEER_TORQUE'}

states = [
    ('=== No turn signal ===', 3),
    ('=== LEFT turn signal ===', 3),
    ('=== RIGHT turn signal ===', 3),
    ('=== HAZARD ===', 3),
]

for label, duration in states:
    print(label)
    print('>>> Please activate the signal NOW <<<')
    start = time.time()
    seen = set()
    while time.time() - start < duration:
        msgs = p.can_receive()
        for m in msgs:
            if m[0] in targets and m[0] not in seen:
                seen.add(m[0])
                name = labels.get(m[0], 'UNKNOWN')
                print(f'  0x{m[0]:03X} ({name}) Data: {m[3].hex()}')
    time.sleep(0.5)
    seen.clear()
    print()

print('=== DONE ===')
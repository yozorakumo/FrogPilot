import time
from panda import Panda
p = Panda()
print('--- CAN Bus Dump (3 seconds) ---')
start = time.time()
seen = {}
while time.time() - start < 3:
    msgs = p.can_recv()
    for msg in msgs:
        addr = msg[0]
        bus = msg[3] & 0x7
        key = (addr, bus)
        if key not in seen:
            seen[key] = True
            print(f'CAN ID: 0x{addr:03X} ({addr:4d}) Bus: {bus}')
print(f'\nTotal unique messages: {len(seen)}')
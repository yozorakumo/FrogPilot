import sys
import time
sys.path.insert(0, '/data/openpilot')
from panda import Panda

p = Panda()
print('Panda type:', p.get_type())
print('Panda serial:', p.get_serial())

# Panda health
h = p.health()
print('\n--- Panda Health ---')
for k, v in h.items():
    print(f'  {k}: {v}')

# CAN bus dump (5 seconds)
print('\n--- CAN Bus Dump (5 seconds) ---')
start = time.time()
seen = {}
while time.time() - start < 5:
    msgs = p.can_recv()
    for msg in msgs:
        addr = msg[0]   # address
        bus = msg[3]    # bus number
        data = msg[2]   # data bytes
        key = (addr, bus)
        if key not in seen:
            seen[key] = data.hex()
            print(f'CAN ID: 0x{addr:03X} ({addr:4d}) Bus: {bus} Data: {data.hex()}')

print(f'\nTotal unique (addr, bus) pairs: {len(seen)}')
print('\n--- Sorted by CAN ID ---')
for (addr, bus), data in sorted(seen.items()):
    print(f'CAN ID: 0x{addr:03X} ({addr:4d}) Bus: {bus}')
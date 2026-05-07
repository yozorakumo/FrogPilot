import sys
import time
from collections import Counter
from panda import Panda

p = Panda()

label = sys.argv[1] if len(sys.argv) > 1 else "UNKNOWN"

# Focus on IDs that might carry gear info
targets = [0x165, 0x166, 0x130, 0x09E]

print(f'=== {label} ===')
start = time.time()
id_data = {}

while time.time() - start < 2:
    msgs = p.can_recv()
    for msg in msgs:
        addr = msg[0]
        data = msg[2]
        bus = msg[3]
        if addr in targets and bus == 0:
            if isinstance(data, bytes):
                data_hex = data.hex()
            elif isinstance(data, int):
                data_hex = f'{data:016X}'
            else:
                data_hex = str(data)
            if addr not in id_data:
                id_data[addr] = Counter()
            id_data[addr][data_hex] += 1

# Show most common pattern for each ID
for addr in sorted(id_data.keys()):
    counter = id_data[addr]
    total = sum(counter.values())
    most_common = counter.most_common(3)
    print(f'ID 0x{addr:03X} ({total} msgs):')
    for data_hex, count in most_common:
        # Show byte-by-byte
        bytes_str = ' '.join(data_hex[i:i+2] for i in range(0, len(data_hex), 2))
        print(f'  {bytes_str}  ({count}/{total} = {count*100//total}%)')
print(f'=== {label} done ===')
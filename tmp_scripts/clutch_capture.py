import sys, time
sys.path.insert(0, '/data/openpilot')
from panda import Panda

p = Panda()
print('=== CLUTCH SIGNAL CAPTURE ===', flush=True)
print('Capturing ALL CAN changes for 25 seconds...', flush=True)
print('Procedure: wait 5s, then press clutch 3 times (1 press per second), then wait', flush=True)
print('', flush=True)

DURATION = 25.0
prev = {}       # (addr, bus) -> last_data_bytes
changes = {}    # (addr, bus) -> count
log = []        # (timestamp, addr, bus, byte_idx, old_val, new_val)

start = time.time()
print(f'[0.0s] === STARTING CAPTURE - Press clutch 3 times starting at 5s mark ===', flush=True)

while time.time() - start < DURATION:
    t = time.time() - start
    msgs = p.can_recv()
    for msg in msgs:
        addr = msg[0]
        bus = msg[3]
        data = msg[2]
        key = (addr, bus)

        if key in prev:
            old = prev[key]
            if old != data:
                for i in range(len(data)):
                    if i < len(old) and old[i] != data[i]:
                        changes[key] = changes.get(key, 0) + 1
                        log.append((t, addr, bus, i, old[i], data[i]))
                        # Print every change in real-time
                        print(f'[{t:6.2f}s] 0x{addr:03X} bus={bus} byte[{i}]: 0x{old[i]:02X} -> 0x{data[i]:02X}  bits: {old[i]:08b} -> {data[i]:08b}', flush=True)
        prev[key] = data

elapsed = time.time() - start
print(f'\n=== CAPTURE COMPLETE ({elapsed:.1f}s) ===', flush=True)

# Summary: sort by change count
print('\n=== TOP CHANGING SIGNALS (sorted by change count) ===', flush=True)
sorted_changes = sorted(changes.items(), key=lambda x: -x[1])
for (addr, bus), cnt in sorted_changes[:20]:
    print(f'  0x{addr:03X} ({addr:4d}) bus={bus}: {cnt} byte-level changes', flush=True)

# Show bit-level detail for top candidates
print('\n=== BIT-LEVEL DETAIL FOR TOP 5 ===', flush=True)
for (addr, bus), cnt in sorted_changes[:5]:
    print(f'\n  --- 0x{addr:03X} bus={bus} ({cnt} changes) ---', flush=True)
    entries = [(t, a, b, bi, ov, nv) for t, a, b, bi, ov, nv in log if a == addr and b == bus]
    for t, a, b, bi, ov, nv in entries[:40]:
        diff_bits = ov ^ nv
        bit_positions = [i for i in range(8) if diff_bits & (1 << i)]
        print(f'    [{t:6.2f}s] byte[{bi}]: 0x{ov:02X}->0x{nv:02X} changed bits: {bit_positions}', flush=True)

# Also show current value of 0x366 specifically
print('\n=== CURRENT VALUES FOR KEY IDs ===', flush=True)
for check_addr in [0x366, 0x165, 0x357, 0x158, 0x28]:
    for bus in [0, 1, 128]:
        key = (check_addr, bus)
        if key in prev:
            data = prev[key]
            print(f'  0x{check_addr:03X} bus={bus}: last data = {data.hex()}', flush=True)

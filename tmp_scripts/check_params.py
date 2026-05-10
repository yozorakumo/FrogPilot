#!/usr/bin/env python3
import os
import struct

# Read params directly from filesystem
def read_param(name):
    path = f'/data/params/d/{name}'
    try:
        with open(path, 'r') as f:
            return f.read().strip()
    except:
        return None

def read_param_bool(name):
    val = read_param(name)
    if val is None:
        return 'NOT SET'
    return val == '1'

# Check relevant params
print('=== Relevant Params ===')
print('ExperimentalLongitudinalEnabled:', read_param_bool('ExperimentalLongitudinalEnabled'))
print('DisableOpenpilotLongitudinal:', read_param_bool('DisableOpenpilotLongitudinal'))
print('ExperimentalMode:', read_param_bool('ExperimentalMode'))
print('ExperimentalModeConfirmed:', read_param_bool('ExperimentalModeConfirmed'))
print('CarFingerprint:', read_param('CarFingerprint'))

# Check if CarParamsPersistent exists and its size
cp_path = '/data/params/d/CarParamsPersistent'
if os.path.exists(cp_path):
    size = os.path.getsize(cp_path)
    print(f'\n CarParamsPersistent exists, size: {size} bytes')
else:
    print('\nCarParamsPersistent NOT FOUND')

# Check if CarParams exists
cp_path2 = '/data/params/d/CarParams'
if os.path.exists(cp_path2):
    size = os.path.getsize(cp_path2)
    print(f'CarParams exists, size: {size} bytes')
else:
    print('CarParams NOT FOUND')
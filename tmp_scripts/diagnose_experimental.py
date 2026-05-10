#!/usr/bin/env python3
"""Diagnostic script to check why Experimental Mode is greyed out."""
import sys
import os

# Add openpilot paths
sys.path.insert(0, '/data/openpilot')
sys.path.insert(0, '/data/openpilot/pyextra')

from openpilot.common.params import Params

params = Params()

print("=" * 60)
print("EXPERIMENTAL MODE DIAGNOSTIC")
print("=" * 60)

# 1. Check git info
print("\n--- GIT INFO ---")
os.system("cd /data/openpilot && git log --oneline -3")
os.system("cd /data/openpilot && git branch --show-current")

# 2. Check if interface.py has the fix
print("\n--- INTERFACE.PY CHECK ---")
with open('/data/openpilot/selfdrive/car/mazda/interface.py', 'r') as f:
    content = f.read()
    if 'experimentalLongitudinalAvailable' in content:
        print("✅ FIX APPLIED: experimentalLongitudinalAvailable found")
    elif 'alphaLongitudinalAvailable' in content:
        print("❌ FIX NOT APPLIED: Still using alphaLongitudinalAvailable!")
    else:
        print("⚠️ NEITHER field found - check interface.py")

# 3. Check CarParamsPersistent
print("\n--- CARPARAMS CHECK ---")
cp_bytes = params.get("CarParamsPersistent")
if cp_bytes:
    try:
        from cereal import car
        CP = car.CarParams.from_bytes(cp_bytes)
        print(f"carFingerprint: {CP.carFingerprint}")
        print(f"carName: {CP.carName}")
        print(f"experimentalLongitudinalAvailable: {CP.experimentalLongitudinalAvailable}")
        print(f"openpilotLongitudinalControl: {CP.openpilotLongitudinalControl}")
        print(f"pcmCruise: {CP.pcmCruise}")
    except Exception as e:
        print(f"Error parsing CarParams: {e}")
else:
    print("❌ CarParamsPersistent is EMPTY!")

# 4. Check relevant params
print("\n--- PARAMS CHECK ---")
print(f"ExperimentalLongitudinalEnabled: {params.get_bool('ExperimentalLongitudinalEnabled')}")
print(f"ExperimentalMode: {params.get_bool('ExperimentalMode')}")
print(f"DisableOpenpilotLongitudinal: {params.get_bool('DisableOpenpilotLongitudinal')}")
print(f"CarMake: {params.get('CarMake', encoding='utf-8')}")
print(f"CarModel: {params.get('CarModel', encoding='utf-8')}")

# 5. Try generating CarParams directly
print("\n--- DIRECT PARAMS GENERATION ---")
try:
    from openpilot.selfdrive.car.mazda.values import CAR
    from openpilot.selfdrive.car.interfaces import CarInterfaceBase
    from openpilot.selfdrive.car import gen_empty_fingerprint
    
    # Test with MAZDA_2_DJ_MT
    CP_test = CarInterfaceBase.get_params(CAR.MAZDA_2_DJ_MT, gen_empty_fingerprint(), [], False, None)
    print(f"Direct generation - experimentalLongitudinalAvailable: {CP_test.experimentalLongitudinalAvailable}")
    print(f"Direct generation - openpilotLongitudinalControl: {CP_test.openpilotLongitudinalControl}")
    
    # Test with experimental_long=True
    CP_test2 = CarInterfaceBase.get_params(CAR.MAZDA_2_DJ_MT, gen_empty_fingerprint(), [], True, None)
    print(f"With experimental_long=True - openpilotLongitudinalControl: {CP_test2.openpilotLongitudinalControl}")
except Exception as e:
    print(f"Error: {e}")
    import traceback
    traceback.print_exc()

print("\n" + "=" * 60)
print("DIAGNOSTIC COMPLETE")
print("=" * 60)
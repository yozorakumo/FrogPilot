#!/usr/bin/env python3
import sys
sys.path.insert(0, '/data/openpilot')

from openpilot.selfdrive.car.mazda.values import CAR
from openpilot.selfdrive.car.interfaces import CarInterface

# Test without experimental_long
CP = CarInterface.get_params(CAR.MAZDA_2_DJ_MT, {}, [], False, False, None)
print('=== Without experimental_long ===')
print('experimentalLongitudinalAvailable:', CP.experimentalLongitudinalAvailable)
print('openpilotLongitudinalControl:', CP.openpilotLongitudinalControl)

# Test with experimental_long
CP2 = CarInterface.get_params(CAR.MAZDA_2_DJ_MT, {}, [], True, False, None)
print('\n=== With experimental_long=True ===')
print('experimentalLongitudinalAvailable:', CP2.experimentalLongitudinalAvailable)
print('openpilotLongitudinalControl:', CP2.openpilotLongitudinalControl)

# Check CarParamsPersistent from params
from openpilot.common.params import Params
params = Params()
cp_bytes = params.get("CarParamsPersistent")
if cp_bytes:
    import capnp
    from cereal import car
    aligned = bytearray(len(cp_bytes))
    aligned[:] = cp_bytes
    msg = capnp._lib.capnp.read_packed_message_flat(aligned)
    CP_stored = msg.get_root_as_struct()
    print('\n=== Stored CarParamsPersistent ===')
    try:
        print('experimentalLongitudinalAvailable:', CP_stored.experimentalLongitudinalAvailable)
    except Exception as e:
        print('Error reading experimentalLongitudinalAvailable:', e)
    try:
        print('openpilotLongitudinalControl:', CP_stored.openpilotLongitudinalControl)
    except Exception as e:
        print('Error reading openpilotLongitudinalControl:', e)
else:
    print('\nNo CarParamsPersistent found')

# Check relevant params
print('\n=== Relevant Params ===')
print('ExperimentalLongitudinalEnabled:', params.get_bool("ExperimentalLongitudinalEnabled") if params.get("ExperimentalLongitudinalEnabled") else 'NOT SET')
print('DisableOpenpilotLongitudinal:', params.get_bool("DisableOpenpilotLongitudinal") if params.get("DisableOpenpilotLongitudinal") else 'NOT SET')
print('ExperimentalMode:', params.get_bool("ExperimentalMode") if params.get("ExperimentalMode") else 'NOT SET')
print('CarFingerprint:', params.get("CarFingerprint", encoding='utf-8'))
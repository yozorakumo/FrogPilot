from openpilot.selfdrive.car.mazda.values import Buttons, MazdaFlags


def create_steering_control(packer, CP, frame, apply_steer, lkas):

  tmp = apply_steer + 2048

  lo = tmp & 0xFF
  hi = tmp >> 8

  b1 = int(lkas["BIT_1"])
  er1 = 0
  lnv = int(lkas["LINE_NOT_VISIBLE"])
  ldw = int(lkas["LDW"])
  er2 = int(lkas["ERR_BIT_2"])

  steering_angle = int(lkas.get("STEERING_ANGLE", 0))
  b2 = int(lkas.get("ANGLE_ENABLED", 0))

  tmp = steering_angle + 2048
  ahi = tmp >> 10
  amd = (tmp & 0x3FF) >> 2
  amd = (amd >> 4) | (( amd & 0xF) << 4)
  alo = (tmp & 0x3) << 2

  ctr = frame % 16
  # bytes:     [    1  ] [ 2 ] [             3               ]  [           4         ]
  csum = 249 - ctr - hi - lo - (lnv << 3) - er1 - (ldw << 7) - ( er2 << 4) - (b1 << 5)

  # bytes      [ 5 ] [ 6 ] [    7   ]
  csum = csum - ahi - amd - alo - b2

  if ahi == 1:
    csum = csum + 15

  if csum < 0:
    if csum < -256:
      csum = csum + 512
    else:
      csum = csum + 256

  csum = csum % 256

  values = {}
  if CP.flags & MazdaFlags.GEN1:
    values = {
      "LKAS_REQUEST": apply_steer,
      "CTR": ctr,
      "ERR_BIT_1": er1,
      "LINE_NOT_VISIBLE" : lnv,
      "LDW": ldw,
      "BIT_1": b1,
      "ERR_BIT_2": er2,
      "STEERING_ANGLE": steering_angle,
      "ANGLE_ENABLED": b2,
      "CHKSUM": csum
    }

  return packer.make_can_msg("CAM_LKAS", 0, values)


def create_alert_command(packer, cam_msg: dict, ldw: bool, steer_required: bool):
  # LANE_LINES exists in all Mazda CAM_LANEINFO DBC definitions
  values = {
    "LANE_LINES": cam_msg.get("LANE_LINES", 0),
  }

  # Check if this DBC has the full CAM_LANEINFO signal set.
  # mazda_2017.dbc defines LINE_VISIBLE and many other signals in CAM_LANEINFO,
  # but mazda_2_dj_mt.dbc only defines LANE_LINES.
  # Sending undefined signals to the packer causes massive log spam (26+ errors/sec)
  # which can trigger UI watchdog timeouts and device reboots.
  has_full_laneinfo = "LINE_VISIBLE" in cam_msg

  if has_full_laneinfo:
    values.update({s: cam_msg.get(s, 0) for s in [
      "LINE_VISIBLE",
      "LINE_NOT_VISIBLE",
      "BIT1",
      "BIT2",
      "BIT3",
      "NO_ERR_BIT",
      "S1",
      "S1_HBEAM",
      "TJA",
      "TJA_TRANSITION",
    ]})
    values.update({
      "HANDS_WARN_3_BITS": 0b111 if steer_required else 0,
      "HANDS_ON_STEER_WARN": steer_required,
      "HANDS_ON_STEER_WARN_2": steer_required,
      "LDW_WARN_LL": 0,
      "LDW_WARN_RL": 0,
    })

  return packer.make_can_msg("CAM_LANEINFO", 0, values)


def create_button_cmd(packer, CP, counter, button):

  can = int(button == Buttons.CANCEL)
  res = int(button == Buttons.RESUME)

  if CP.flags & MazdaFlags.GEN1:
    values = {
      "CAN_OFF": can,
      "CAN_OFF_INV": (can + 1) % 2,

      "SET_P": 0,
      "SET_P_INV": 1,

      "RES": res,
      "RES_INV": (res + 1) % 2,

      "SET_M": 0,
      "SET_M_INV": 1,

      "DISTANCE_LESS": 0,
      "DISTANCE_LESS_INV": 1,

      "DISTANCE_MORE": 0,
      "DISTANCE_MORE_INV": 1,

      "MODE_X": 0,
      "MODE_X_INV": 1,

      "MODE_Y": 0,
      "MODE_Y_INV": 1,

      "BIT1": 1,
      "BIT2": 1,
      "BIT3": 1,
      "CTR": (counter + 1) % 16,
    }

    return packer.make_can_msg("CRZ_BTNS", 0, values)

# NOTE: Door lock and i-stop cancel commands are NOT functional on Mazda 2 DJ MT.
# The BCM (0x420) is a status TX message and does not accept commands on HS-CAN.
# These functions are kept as placeholders for future MS-CAN/UDS implementation.

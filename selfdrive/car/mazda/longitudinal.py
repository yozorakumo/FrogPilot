from __future__ import annotations

import logging
from enum import StrEnum

from panda.python.uds import SERVICE_TYPE
from openpilot.selfdrive.car import carlog
from openpilot.selfdrive.car.isotp_parallel_query import IsoTpParallelQuery

# Mazda 2 DJ MT longitudinal control constants
RADAR_ADDR = 0x764
RADAR_BUS = 0

CRZ_INFO_ADDR = 0x21B
CRZ_CTRL_ADDR = 0x21C

CRZ_INFO_TEMPLATE = bytes.fromhex("01ffe20006800000")

LONG_COMMAND_STEP = 2
TESTER_PRESENT_STEP = 50

ACCEL_CMD_MAX = 2000.0
ACCEL_CMD_MIN = -2000.0

HOLD_BRAKE_CMD_TARGET = -1024.0
HOLD_LATCHED_CMD_TARGET = -1.0
NEAR_STOP_BRAKE_CMD_TARGET = -750.0
NEAR_STOP_ENTRY_SPEED = 1.0

ACTIVE_STOP_CHECKSUM_BIAS = 0x04

# Speed-dependent accel scaling
ACCEL_SCALE_UP_BP = (0.0, 4.2, 11.1, 22.2)
ACCEL_SCALE_UP_V = (1000.0, 1000.0, 950.0, 800.0)

ACCEL_SCALE_DOWN_BP = (0.0, 1.4, 5.6, 22.2)
ACCEL_SCALE_DOWN_V = (1200.0, 1000.0, 925.0, 950.0)


class MazdaLongitudinalProfile(StrEnum):
  STANDBY = "standby"
  ENGAGED_CRUISE = "engaged_cruise"
  ENGAGED_FOLLOW = "engaged_follow"
  STOP_GO_HOLD = "stop_go_hold"
  STOP_GO_HOLD_LATCHED = "stop_go_hold_latched"


CRZ_CTRL_TEMPLATES: dict[MazdaLongitudinalProfile, bytes] = {
  MazdaLongitudinalProfile.STANDBY: bytes.fromhex("02018b0000000000"),
  MazdaLongitudinalProfile.ENGAGED_CRUISE: bytes.fromhex("0a018b2000001000"),
  MazdaLongitudinalProfile.ENGAGED_FOLLOW: bytes.fromhex("0a018b4000001000"),
  MazdaLongitudinalProfile.STOP_GO_HOLD: bytes.fromhex("0a018b6000001000"),
  MazdaLongitudinalProfile.STOP_GO_HOLD_LATCHED: bytes.fromhex("0a018b8000001000"),
}

# --- Signal definitions from mazda_2_dj_mt.dbc ---
# Format: (start_bit, size, factor, offset, is_intel)
# All signals here are Motorola byte order (is_intel=False)

CRZ_INFO_SIGNALS = {
  "ACCEL_CMD": (17, 13, 1.0, -4096.0, False),
  "ACC_ACTIVE": (33, 1, 1.0, 0.0, False),
  "ACC_SET_ALLOWED": (34, 1, 1.0, 0.0, False),
  "CRZ_ENDED": (36, 1, 1.0, 0.0, False),
  "STOPPING_MAYBE": (42, 1, 1.0, 0.0, False),
  "CTR1": (51, 4, 1.0, 0.0, False),
  "STOPPING_MAYBE2": (52, 1, 1.0, 0.0, False),
  "RESUME_UNLATCHING_MAYBE": (54, 1, 1.0, 0.0, False),
}

CRZ_CTRL_SIGNALS = {
  "CRZ_ACTIVE": (3, 1, 1.0, 0.0, False),
  "ACC_ACTIVE_2": (52, 1, 1.0, 0.0, False),
  "RADAR_HAS_LEAD": (23, 1, 1.0, 0.0, False),
  "RADAR_LEAD_RELATIVE_DISTANCE": (31, 3, 1.0, 0.0, False),
  "ACC_GAS_MAYBE2": (29, 1, 1.0, 0.0, False),
}


def _set_motorola_signal(raw: bytearray, start_bit: int, size: int, value: int) -> None:
  """Encode a value into a CAN byte array using Motorola (big-endian) bit ordering.

  DBC Motorola bit numbering:
    bit 0 = byte 0 MSB (bit 7), bit 7 = byte 0 LSB (bit 0),
    bit 8 = byte 1 MSB (bit 7), etc.
  """
  for j in range(size):
    bit_value = (value >> (size - 1 - j)) & 1  # MSB first
    bit_pos = start_bit + j
    byte_index = bit_pos // 8
    bit_within_byte = 7 - (bit_pos % 8)
    if bit_value:
      raw[byte_index] |= (1 << bit_within_byte)
    else:
      raw[byte_index] &= ~(1 << bit_within_byte)


def _patch_signal(signals: dict, raw: bytes, signal_name: str, value: float) -> bytes:
  """Patch a signal value into raw CAN bytes using signal definition."""
  start_bit, size, factor, offset, is_intel = signals[signal_name]
  raw_value = int(round((value - offset) / factor))
  # Handle two's complement for unsigned representation
  if raw_value < 0:
    raw_value = (1 << size) + raw_value
  dat = bytearray(raw)
  if is_intel:
    # Intel byte order: LSB first
    for j in range(size):
      bit_value = (raw_value >> j) & 1
      bit_pos = start_bit + j
      byte_index = bit_pos // 8
      bit_within_byte = bit_pos % 8
      if bit_value:
        dat[byte_index] |= (1 << bit_within_byte)
      else:
        dat[byte_index] &= ~(1 << bit_within_byte)
  else:
    _set_motorola_signal(dat, start_bit, size, raw_value)
  return bytes(dat)


def _compute_inverted_sum_checksum(raw: bytes, checksum_index: int = 7) -> int:
  return (0xFF - (sum(raw[i] for i in range(len(raw)) if i != checksum_index) & 0xFF)) & 0xFF


def _update_crz_info_checksum(raw: bytes, bias: int = 0) -> bytes:
  dat = bytearray(raw)
  dat[7] = (_compute_inverted_sum_checksum(dat) + bias) & 0xFF
  return bytes(dat)


def clip(value: float, lower: float, upper: float) -> float:
  return min(max(value, lower), upper)


def _interp_scale(v_ego: float, bp: tuple[float, ...], values: tuple[float, ...]) -> float:
  if v_ego <= bp[0]:
    return values[0]
  if v_ego >= bp[-1]:
    return values[-1]
  for i in range(1, len(bp)):
    if v_ego <= bp[i]:
      x0, x1 = bp[i - 1], bp[i]
      y0, y1 = values[i - 1], values[i]
      ratio = (v_ego - x0) / (x1 - x0)
      return y0 + (y1 - y0) * ratio
  return values[-1]


def accel_to_accel_cmd(accel: float, v_ego: float) -> int:
  if accel >= 0.0:
    scale = _interp_scale(v_ego, ACCEL_SCALE_UP_BP, ACCEL_SCALE_UP_V)
  else:
    scale = _interp_scale(v_ego, ACCEL_SCALE_DOWN_BP, ACCEL_SCALE_DOWN_V)
  return int(round(clip(accel * scale, ACCEL_CMD_MIN, ACCEL_CMD_MAX)))


def hold_brake_accel() -> float:
  return HOLD_BRAKE_CMD_TARGET / ACCEL_SCALE_DOWN_V[0]


def hold_latched_accel() -> float:
  return HOLD_LATCHED_CMD_TARGET / ACCEL_SCALE_DOWN_V[0]


def near_stop_brake_accel(v_ego: float) -> float:
  ratio = clip(v_ego / NEAR_STOP_ENTRY_SPEED, 0.0, 1.0)
  target = HOLD_BRAKE_CMD_TARGET + (NEAR_STOP_BRAKE_CMD_TARGET - HOLD_BRAKE_CMD_TARGET) * ratio
  return target / ACCEL_SCALE_DOWN_V[0]


def build_crz_info(accel, counter, long_active, hold_request, v_ego,
                   hold_latched=False, acc_set_allowed=True, resume_unlatching=False) -> bytes:
  stopping_active = hold_request and not hold_latched
  raw = _patch_signal(CRZ_INFO_SIGNALS, CRZ_INFO_TEMPLATE, "ACCEL_CMD", accel_to_accel_cmd(accel, v_ego))
  raw = _patch_signal(CRZ_INFO_SIGNALS, raw, "ACC_ACTIVE", int(long_active))
  raw = _patch_signal(CRZ_INFO_SIGNALS, raw, "ACC_SET_ALLOWED", int(acc_set_allowed))
  raw = _patch_signal(CRZ_INFO_SIGNALS, raw, "CRZ_ENDED", 0)
  raw = _patch_signal(CRZ_INFO_SIGNALS, raw, "STOPPING_MAYBE", int(stopping_active))
  raw = _patch_signal(CRZ_INFO_SIGNALS, raw, "STOPPING_MAYBE2", int(stopping_active))
  raw = _patch_signal(CRZ_INFO_SIGNALS, raw, "RESUME_UNLATCHING_MAYBE", int(resume_unlatching))
  raw = _patch_signal(CRZ_INFO_SIGNALS, raw, "CTR1", counter % 16)
  checksum_bias = ACTIVE_STOP_CHECKSUM_BIAS if stopping_active else 0
  return _update_crz_info_checksum(raw, bias=checksum_bias)


def select_profile(long_active, lead_visible, hold_request, crz_hold_latched) -> MazdaLongitudinalProfile:
  if not long_active:
    return MazdaLongitudinalProfile.STANDBY
  if hold_request and crz_hold_latched:
    return MazdaLongitudinalProfile.STOP_GO_HOLD_LATCHED
  if hold_request:
    return MazdaLongitudinalProfile.STOP_GO_HOLD
  if lead_visible:
    return MazdaLongitudinalProfile.ENGAGED_FOLLOW
  return MazdaLongitudinalProfile.ENGAGED_CRUISE


def build_crz_ctrl(long_active, lead_visible, hold_request, hold_latched,
                   crz_hold_latched=False, crz_hold_passive=False, crz_resume_active=False) -> bytes:
  lead_visible = lead_visible or hold_request or hold_latched or crz_hold_latched or crz_hold_passive
  raw = CRZ_CTRL_TEMPLATES[select_profile(long_active, lead_visible, hold_request, crz_hold_latched)]
  raw = _patch_signal(CRZ_CTRL_SIGNALS, raw, "CRZ_ACTIVE", int(long_active))
  raw = _patch_signal(CRZ_CTRL_SIGNALS, raw, "ACC_ACTIVE_2", int(long_active and not crz_hold_passive))
  raw = _patch_signal(CRZ_CTRL_SIGNALS, raw, "RADAR_HAS_LEAD", int(lead_visible))
  if crz_hold_passive or crz_hold_latched:
    raw = _patch_signal(CRZ_CTRL_SIGNALS, raw, "RADAR_LEAD_RELATIVE_DISTANCE", 4)
    raw = _patch_signal(CRZ_CTRL_SIGNALS, raw, "ACC_GAS_MAYBE2", 0)
  elif hold_request or crz_resume_active:
    raw = _patch_signal(CRZ_CTRL_SIGNALS, raw, "RADAR_LEAD_RELATIVE_DISTANCE", 3)
    raw = _patch_signal(CRZ_CTRL_SIGNALS, raw, "ACC_GAS_MAYBE2", 1)
  return raw


def create_longitudinal_messages(bus, accel, counter, long_active, lead_visible, *,
                                 hold_request=False, crz_ctrl_hold_request=None,
                                 hold_latched=False, crz_hold_latched=False,
                                 crz_hold_passive=False, crz_resume_active=False,
                                 crz_info_resume_unlatching=False, v_ego=0.0) -> list[list]:
  """Create longitudinal CAN messages. Returns list of [addr, 0, dat, bus]."""
  if crz_ctrl_hold_request is None:
    crz_ctrl_hold_request = hold_request
  return [
    [CRZ_INFO_ADDR, 0, build_crz_info(accel, counter, long_active, hold_request, v_ego,
                                        hold_latched=hold_latched,
                                        resume_unlatching=crz_info_resume_unlatching), bus],
    [CRZ_CTRL_ADDR, 0, build_crz_ctrl(long_active, lead_visible, crz_ctrl_hold_request, hold_latched,
                                        crz_hold_latched=crz_hold_latched,
                                        crz_hold_passive=crz_hold_passive,
                                        crz_resume_active=crz_resume_active), bus],
  ]


def create_radar_tester_present(bus=RADAR_BUS) -> list:
  """Create a UDS tester present message for the radar with suppress response."""
  # Service 0x3E (TESTER_PRESENT), subfunction 0x80 (suppressPosResponse)
  dat = bytes([0x02, SERVICE_TYPE.TESTER_PRESENT, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00])
  return [RADAR_ADDR, 0, dat, bus]


def _uds_request(can_recv, can_send, bus, addr, request, response, *, timeout=0.1) -> bool:
  query = IsoTpParallelQuery(can_send, can_recv, bus, [(addr, None)], [request], [response])
  return len(query.get_data(timeout)) > 0


def enter_radar_programming_session(can_recv, can_send, bus=RADAR_BUS, addr=RADAR_ADDR, retry=5) -> bool:
  # UDS Diagnostic Session Control: service 0x10, session type 0x02 (PROGRAMMING)
  request = bytes([SERVICE_TYPE.DIAGNOSTIC_SESSION_CONTROL, 0x02])
  response = bytes([SERVICE_TYPE.DIAGNOSTIC_SESSION_CONTROL + 0x40, 0x02])
  for attempt in range(retry):
    try:
      if _uds_request(can_recv, can_send, bus, addr, request, response):
        carlog.warning(f"mazda radar programming session enabled on {hex(addr)}")
        return True
    except Exception:
      carlog.exception("mazda radar programming session exception")
    carlog.error(f"mazda radar programming session retry ({attempt + 1})")
  carlog.error("mazda radar programming session failed")
  return False


def request_radar_default_session(can_recv, can_send, bus=RADAR_BUS, addr=RADAR_ADDR) -> bool:
  # UDS Diagnostic Session Control: service 0x10, session type 0x01 (DEFAULT)
  request = bytes([SERVICE_TYPE.DIAGNOSTIC_SESSION_CONTROL, 0x01])
  response = bytes([SERVICE_TYPE.DIAGNOSTIC_SESSION_CONTROL + 0x40, 0x01])
  try:
    return _uds_request(can_recv, can_send, bus, addr, request, response)
  except Exception:
    carlog.exception("mazda radar default session exception")
    return False
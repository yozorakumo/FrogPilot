from cereal import car, custom
from openpilot.common.conversions import Conversions as CV
from opendbc.can.can_define import CANDefine
from opendbc.can.parser import CANParser
from openpilot.selfdrive.car.interfaces import CarStateBase
from openpilot.selfdrive.car.mazda.values import DBC, LKAS_LIMITS, MazdaFlags

class CarState(CarStateBase):
  def __init__(self, CP, FPCP):
    super().__init__(CP, FPCP)

    can_define = CANDefine(DBC[CP.carFingerprint]["pt"])
    if not (CP.flags & MazdaFlags.MT):
      self.shifter_values = can_define.dv["GEAR"]["GEAR"]

    self.crz_btns_counter = 0
    self.acc_active_last = False
    self.low_speed_alert = False
    self.lkas_allowed_speed = False
    self.lkas_disabled = False
    self.steering_angle_prev = 0.0

    self.prev_distance_button = 0
    self.distance_button = 0

    self.prev_cancel_button = False
    self.cancel_button = False
    self.prev_main_button = False
    self.main_button = False

  def update(self, cp, cp_cam, frogpilot_toggles):

    ret = car.CarState.new_message()
    fp_ret = custom.FrogPilotCarState.new_message()

    self.prev_distance_button = self.distance_button
    self.distance_button = cp.vl["CRZ_BTNS"]["DISTANCE_LESS"]

    # Button state tracking for longitudinal control
    self.prev_cancel_button = self.cancel_button
    self.cancel_button = cp.vl["CRZ_BTNS"]["CAN_OFF"] == 1
    self.prev_main_button = self.main_button
    self.main_button = bool(cp.vl["CRZ_BTNS"]["MODE_X"] and cp.vl["CRZ_BTNS"]["MODE_Y"])

    ret.wheelSpeeds = self.get_wheel_speeds(
      cp.vl["WHEEL_SPEEDS"]["FL"],
      cp.vl["WHEEL_SPEEDS"]["FR"],
      cp.vl["WHEEL_SPEEDS"]["RL"],
      cp.vl["WHEEL_SPEEDS"]["RR"],
    )
    ret.vEgoRaw = (ret.wheelSpeeds.fl + ret.wheelSpeeds.fr + ret.wheelSpeeds.rl + ret.wheelSpeeds.rr) / 4.
    ret.vEgo, ret.aEgo = self.update_speed_kf(ret.vEgoRaw)

    # Match panda speed reading
    speed_kph = cp.vl["ENGINE_DATA"]["SPEED"]
    ret.standstill = speed_kph <= .1

    if self.CP.flags & MazdaFlags.MT:
      # GEAR_POS値からクラッチ・ニュートラル状態を判定
      # GEAR_POS値の意味: 2=6速, 3=5速, 4=4速, 5=3速, 7=2速, 13=1速
      # 中間値: 14=ニュートラル/クラッチ, 6/8/9/10/11/12=シフト遷移中
      gear_pos = int(cp.vl["PEDALS"]["GEAR_POS"])
      GEAR_VALUES = {2: 6, 3: 5, 4: 4, 5: 3, 7: 2, 13: 1}  # GEAR_POS -> ギア番号
      VALID_GEAR_POS = set(GEAR_VALUES.keys())

      ret.clutchPressed = gear_pos not in VALID_GEAR_POS  # クラッチが踏まれている = ギアが確定していない
      # ニュートラル判定はGEAR_POS==14（ギア変更時に必ず経由する中間値）

      # GearShifter enum has no per-gear values; all engaged gears map to 'drive'
      # Actual gear number (1-6) is stored in fp_ret.gearStep below
      gear_map = {
        2: car.CarState.GearShifter.drive,   # 6速
        3: car.CarState.GearShifter.drive,   # 5速
        4: car.CarState.GearShifter.drive,   # 4速
        5: car.CarState.GearShifter.drive,   # 3速
        7: car.CarState.GearShifter.drive,   # 2速
        13: car.CarState.GearShifter.drive,  # 1速
      }
      ret.gearShifter = gear_map.get(gear_pos, car.CarState.GearShifter.neutral)
      # Map raw CAN GEAR_POS values to actual gear numbers (1-6), 0 for neutral/unknown
      fp_ret.gearStep = GEAR_VALUES.get(gear_pos, 0)
    else:
      can_gear = int(cp.vl["GEAR"]["GEAR"])
      ret.gearShifter = self.parse_gear_shifter(self.shifter_values.get(can_gear, None))

    ret.genericToggle = bool(cp.vl["BLINK_INFO"]["HIGH_BEAMS"])
    ret.leftBlindspot = cp.vl["BSM"]["LEFT_BS_STATUS"] != 0
    ret.rightBlindspot = cp.vl["BSM"]["RIGHT_BS_STATUS"] != 0
    left_blink = cp.vl["BLINK_INFO"]["LEFT_BLINK"] == 1
    right_blink = cp.vl["BLINK_INFO"]["RIGHT_BLINK"] == 1
    hazard_switch = cp.vl["TURN_SWITCH"]["HAZARD"] == 1

    # ハザードはTURN_SWITCHのHAZARDシグナルで判定
    hazard = hazard_switch or (left_blink and right_blink)
    if hazard:
      ret.leftBlinker, ret.rightBlinker = self.update_blinker_from_lamp(40, True, True)
    else:
      ret.leftBlinker, ret.rightBlinker = self.update_blinker_from_lamp(40, left_blink, right_blink)

    steer_angle = cp.vl["STEER"]["STEER_ANGLE"]
    # Guard against sensor error values (e.g., 0xFFFE = 1676.70°)
    # Mazda 2 DJ steering angle sensor outputs invalid marker values
    # during hazard/right blinker activation
    if abs(steer_angle) > 360:  # Physical steering range is approximately ±500°
      steer_angle = self.steering_angle_prev
    self.steering_angle_prev = steer_angle
    ret.steeringAngleDeg = steer_angle
    ret.steeringTorque = cp.vl["STEER_TORQUE"]["STEER_TORQUE_SENSOR"]
    ret.steeringPressed = abs(ret.steeringTorque) > LKAS_LIMITS.STEER_THRESHOLD

    ret.steeringTorqueEps = cp.vl["STEER_TORQUE"]["STEER_TORQUE_MOTOR"]
    ret.steeringRateDeg = cp.vl["STEER_RATE"]["STEER_ANGLE_RATE"]

    # TODO: this should be from 0 - 1.
    ret.brakePressed = cp.vl["PEDALS"]["BRAKE_ON"] == 1
    ret.brake = cp.vl["BRAKE"]["BRAKE_PRESSURE"]

    ret.seatbeltUnlatched = cp.vl["SEATBELT"]["DRIVER_SEATBELT"] == 0
    ret.doorOpen = any([cp.vl["DOORS"]["FL"], cp.vl["DOORS"]["FR"],
                        cp.vl["DOORS"]["BL"], cp.vl["DOORS"]["BR"]])

    # TODO: this should be from 0 - 1.
    ret.gas = cp.vl["ENGINE_DATA"]["PEDAL_GAS"]
    ret.gasPressed = ret.gas > 0

    # Either due to low speed or hands off
    lkas_blocked = cp.vl["STEER_RATE"]["LKAS_BLOCK"] == 1

    if self.CP.minSteerSpeed > 0:
      # LKAS is enabled at 52kph going up and disabled at 45kph going down
      # wait for LKAS_BLOCK signal to clear when going up since it lags behind the speed sometimes
      if speed_kph > LKAS_LIMITS.ENABLE_SPEED and not lkas_blocked:
        self.lkas_allowed_speed = True
      elif speed_kph < LKAS_LIMITS.DISABLE_SPEED:
        self.lkas_allowed_speed = False
    else:
      self.lkas_allowed_speed = True

    # TODO: the signal used for available seems to be the adaptive cruise signal, instead of the main on
    #       it should be used for carState.cruiseState.nonAdaptive instead
    if self.CP.openpilotLongitudinalControl:
      # Alpha-longitudinal mode: PEDALS-based cruise state detection
      acc_armed = cp.vl["PEDALS"]["ACC_OFF"] == 1
      acc_active = cp.vl["PEDALS"]["ACC_ACTIVE"] == 1
      ret.cruiseState.available = acc_armed or acc_active
      ret.cruiseState.enabled = acc_active
    else:
      ret.cruiseState.available = cp.vl["CRZ_CTRL"]["CRZ_AVAILABLE"] == 1
      ret.cruiseState.enabled = cp.vl["CRZ_CTRL"]["CRZ_ACTIVE"] == 1
    ret.cruiseState.standstill = cp.vl["PEDALS"]["STANDSTILL"] == 1
    ret.cruiseState.speed = cp.vl["CRZ_EVENTS"]["CRZ_SPEED"] * CV.KPH_TO_MS

    if ret.cruiseState.enabled:
      if not self.lkas_allowed_speed and self.acc_active_last:
        self.low_speed_alert = True
      else:
        self.low_speed_alert = False

    # Check if LKAS is disabled due to lack of driver torque when all other states indicate
    # it should be enabled (steer lockout). Don't warn until we actually get lkas active
    # and lose it again, i.e, after initial lkas activation
    ret.steerFaultTemporary = self.lkas_allowed_speed and lkas_blocked

    self.acc_active_last = ret.cruiseState.enabled

    self.crz_btns_counter = cp.vl["CRZ_BTNS"]["CTR"]

    # camera signals
    self.lkas_disabled = cp_cam.vl["CAM_LANEINFO"]["LANE_LINES"] == 0
    self.cam_lkas = cp_cam.vl["CAM_LKAS"]
    self.cam_laneinfo = cp_cam.vl["CAM_LANEINFO"]
    ret.steerFaultPermanent = cp_cam.vl["CAM_LKAS"]["ERR_BIT_1"] == 1

    # FrogPilot CarState functions
    self.lkas_previously_enabled = self.lkas_enabled
    self.lkas_enabled = not self.lkas_disabled

    return ret, fp_ret

  @staticmethod
  def get_can_parser(CP, FPCP):
    messages = [
      # sig_address, frequency
      ("BLINK_INFO", 10),
      ("TURN_SWITCH", 10),
      ("STEER", 67),
      ("STEER_RATE", 83),
      ("STEER_TORQUE", 83),
      ("WHEEL_SPEEDS", 100),
    ]

    if CP.flags & MazdaFlags.GEN1:
      messages += [
        ("ENGINE_DATA", 100),
      ]

    if CP.flags & MazdaFlags.GEN1:
      messages += [
        ("CRZ_CTRL", 50),
        ("CRZ_EVENTS", 50),
        ("CRZ_BTNS", 10),
        ("PEDALS", 50),
        ("BRAKE", 50),
        ("SEATBELT", 10),
        ("DOORS", 10),
        ("BSM", 10),
      ]

    if CP.flags & MazdaFlags.GEN1 and not (CP.flags & MazdaFlags.MT):
      messages += [
        ("GEAR", 20),
      ]

    if CP.flags & MazdaFlags.GEN1 and CP.flags & MazdaFlags.MT:
      messages += [
        ("NEW_MSG_28", 50),
      ]

    return CANParser(DBC[CP.carFingerprint]["pt"], messages, 0)

  @staticmethod
  def get_cam_can_parser(CP, FPCP):
    messages = []

    if CP.flags & MazdaFlags.GEN1:
      messages += [
        # sig_address, frequency
        ("CAM_LANEINFO", 2),
        ("CAM_LKAS", 16),
      ]

    return CANParser(DBC[CP.carFingerprint]["pt"], messages, 2)

#!/usr/bin/env python3
from cereal import car, custom
from openpilot.common.conversions import Conversions as CV
from openpilot.selfdrive.car.mazda.values import CAR, LKAS_LIMITS
from openpilot.selfdrive.car import create_button_events, get_safety_config
from openpilot.selfdrive.car.interfaces import CarInterfaceBase
from openpilot.selfdrive.car.mazda.longitudinal import enter_radar_programming_session

ButtonType = car.CarState.ButtonEvent.Type
FrogPilotButtonType = custom.FrogPilotCarState.ButtonEvent.Type
EventName = car.CarEvent.EventName

MAZDA_LONG_SAFETY_PARAM = 1

class CarInterface(CarInterfaceBase):

  @staticmethod
  def _get_params(ret, candidate, fingerprint, car_fw, experimental_long, docs, frogpilot_toggles):
    ret.carName = "mazda"
    ret.safetyConfigs = [get_safety_config(car.CarParams.SafetyModel.mazda)]
    ret.radarUnavailable = True

    ret.dashcamOnly = candidate not in (CAR.MAZDA_CX5_2022, CAR.MAZDA_CX9_2021)

    ret.steerActuatorDelay = 0.1
    ret.steerLimitTimer = 0.8

    CarInterfaceBase.configure_torque_tune(candidate, ret.lateralTuning)

    if candidate not in (CAR.MAZDA_CX5_2022, ):
      ret.minSteerSpeed = LKAS_LIMITS.DISABLE_SPEED * CV.KPH_TO_MS

    ret.centerToFront = ret.wheelbase * 0.41

    ret.enableBsm = True

    if candidate == CAR.MAZDA_2_DJ_MT:
      ret.mass = 1030.
      ret.wheelbase = 2.57
      ret.steerRatio = 14.8
      ret.transmissionType = car.CarParams.TransmissionType.manual
      ret.safetyConfigs[0].safetyParam = 1  # MAZDA_PARAM_2_DJ_MT for panda safety

    # Alpha longitudinal control for Mazda 2 DJ MT
    # DISABLED: longitudinal.py import paths need fixing for FrogPilot
    # ret.alphaLongitudinalAvailable = candidate == CAR.MAZDA_2_DJ_MT
    ret.alphaLongitudinalAvailable = False
    ret.openpilotLongitudinalControl = False

    if ret.openpilotLongitudinalControl:
      ret.pcmCruise = True
      ret.safetyConfigs = [get_safety_config(car.CarParams.SafetyModel.mazda, MAZDA_LONG_SAFETY_PARAM)]
      ret.radarUnavailable = True
      ret.startingState = True
      ret.startAccel = 1.2
      ret.vEgoStarting = 0.15
      ret.vEgoStopping = 0.5
      ret.longitudinalActuatorDelay = 0.36
      ret.longitudinalTuning.kpBP = [20.]
      ret.longitudinalTuning.kpV = [1.2, 1.0, 0.8]
      ret.longitudinalTuning.kiBP = [20.]
      ret.longitudinalTuning.kiV = [0.18, 0.12, 0.08]
      ret.centerToFront = ret.wheelbase * 0.41

    return ret

  @staticmethod
  def init(CP, logcan, sendcan):
    if CP.openpilotLongitudinalControl:
      enter_radar_programming_session(logcan, sendcan)

  @staticmethod
  def deinit(CP, logcan, sendcan):
    if CP.openpilotLongitudinalControl:
      return

  # returns a car.CarState
  def _update(self, c, frogpilot_toggles):
    ret, fp_ret = self.CS.update(self.cp, self.cp_cam, frogpilot_toggles)

     # TODO: add button types for inc and dec
    ret.buttonEvents = [
      *create_button_events(self.CS.distance_button, self.CS.prev_distance_button, {1: ButtonType.gapAdjustCruise}),
      *create_button_events(self.CS.lkas_enabled, self.CS.lkas_previously_enabled, {1: FrogPilotButtonType.lkas}),
    ]

    # Longitudinal button events
    if self.CP.openpilotLongitudinalControl:
      ret.buttonEvents += create_button_events(self.CS.cancel_button, self.CS.prev_cancel_button, ButtonType.cancel)
      ret.buttonEvents += create_button_events(self.CS.main_button, self.CS.prev_main_button, ButtonType.mainCruise)

    # events
    events = self.create_common_events(ret)

    if self.CS.lkas_disabled:
      events.add(EventName.lkasDisabled)
    elif self.CS.low_speed_alert:
      events.add(EventName.belowSteerSpeed)

    if ret.clutchPressed:
      events.add(EventName.clutchPressed)

    ret.events = events.to_msg()

    return ret, fp_ret

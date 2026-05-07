from cereal import car
from opendbc.can.packer import CANPacker
from openpilot.common.conversions import Conversions as CV
from openpilot.selfdrive.car import apply_driver_steer_torque_limits
from openpilot.selfdrive.car.interfaces import CarControllerBase
from openpilot.selfdrive.car.mazda import mazdacan
from openpilot.selfdrive.car.mazda.values import CarControllerParams, Buttons, CAR

VisualAlert = car.CarControl.HUDControl.VisualAlert


class CarController(CarControllerBase):
  def __init__(self, dbc_name, CP, VM):
    self.CP = CP
    self.apply_steer_last = 0
    self.packer = CANPacker(dbc_name)
    self.brake_counter = 0
    self.frame = 0
    
    # FrogPilot variables
    self.doors_locked = False

  def update(self, CC, CS, now_nanos, frogpilot_toggles):
    can_sends = []

    apply_steer = 0

    if CC.latActive:
      # calculate steer and also set limits due to driver torque
      new_steer = int(round(CC.actuators.steer * CarControllerParams.STEER_MAX))
      apply_steer = apply_driver_steer_torque_limits(new_steer, self.apply_steer_last,
                                                     CS.out.steeringTorque, CarControllerParams)

    if CC.cruiseControl.cancel:
      # If brake is pressed, let us wait >70ms before trying to disable crz to avoid
      # a race condition with the stock system, where the second cancel from openpilot
      # will disable the crz 'main on'. crz ctrl msg runs at 50hz. 70ms allows us to
      # read 3 messages and most likely sync state before we attempt cancel.
      self.brake_counter = self.brake_counter + 1
      if self.frame % 10 == 0 and not (CS.out.brakePressed and self.brake_counter < 7):
        # Cancel Stock ACC if it's enabled while OP is disengaged
        # Send at a rate of 10hz until we sync with stock ACC state
        can_sends.append(mazdacan.create_button_cmd(self.packer, self.CP, CS.crz_btns_counter, Buttons.CANCEL))
    else:
      self.brake_counter = 0
      if CC.cruiseControl.resume and self.frame % 5 == 0:
        # Mazda Stop and Go requires a RES button (or gas) press if the car stops more than 3 seconds
        # Send Resume button when planner wants car to move
        can_sends.append(mazdacan.create_button_cmd(self.packer, self.CP, CS.crz_btns_counter, Buttons.RESUME))

    self.apply_steer_last = apply_steer

    # send HUD alerts
    if self.frame % 50 == 0:
      ldw = CC.hudControl.visualAlert == VisualAlert.ldw
      steer_required = CC.hudControl.visualAlert == VisualAlert.steerRequired
      # TODO: find a way to silence audible warnings so we can add more hud alerts
      steer_required = steer_required and CS.lkas_allowed_speed
      can_sends.append(mazdacan.create_alert_command(self.packer, CS.cam_laneinfo, ldw, steer_required))

    # send steering command
    can_sends.append(mazdacan.create_steering_control(self.packer, self.CP,
                                                      self.frame, apply_steer, CS.cam_lkas))

    new_actuators = CC.actuators.as_builder()
    new_actuators.steer = apply_steer / CarControllerParams.STEER_MAX
    new_actuators.steerOutputCan = apply_steer

    # FrogPilot Mazda carcontroller functions
    if self.CP.carFingerprint == CAR.MAZDA_2_DJ_MT:
      if not self.doors_locked:
        if frogpilot_toggles.experimental_mode and frogpilot_toggles.mazda_auto_lock_speed and CS.out.vEgo >= frogpilot_toggles.mazda_lock_speed * CV.KPH_TO_MS:
          can_sends.append(mazdacan.create_door_lock_command(self.packer, True))
          self.doors_locked = True
      elif self.doors_locked:
        if frogpilot_toggles.experimental_mode and frogpilot_toggles.mazda_auto_unlock_speed and CS.out.vEgo <= frogpilot_toggles.mazda_unlock_speed * CV.KPH_TO_MS:
          can_sends.append(mazdacan.create_door_lock_command(self.packer, False))
          self.doors_locked = False
        elif frogpilot_toggles.experimental_mode and frogpilot_toggles.mazda_auto_unlock_park_brake and CS.out.parkingBrake:
          can_sends.append(mazdacan.create_door_lock_command(self.packer, False))
          self.doors_locked = False

    self.frame += 1
    return new_actuators, can_sends

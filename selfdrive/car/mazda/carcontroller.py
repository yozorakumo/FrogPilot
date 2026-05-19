from cereal import car
from opendbc.can.packer import CANPacker
from openpilot.common.conversions import Conversions as CV
from openpilot.selfdrive.car import apply_driver_steer_torque_limits
from openpilot.selfdrive.car.interfaces import CarControllerBase
from openpilot.selfdrive.car.mazda import mazdacan
from openpilot.selfdrive.car.mazda.values import CarControllerParams, Buttons, CAR
from openpilot.selfdrive.car.mazda.longitudinal import (
  LONG_COMMAND_STEP, NEAR_STOP_ENTRY_SPEED, RADAR_BUS, TESTER_PRESENT_STEP,
  create_longitudinal_messages, create_radar_tester_present,
  hold_brake_accel, hold_latched_accel, near_stop_brake_accel,
)

VisualAlert = car.CarControl.HUDControl.VisualAlert

DT_CTRL = 0.01  # 100Hz
CRZ_CTRL_LATCH_FRAMES = int(round(2.0 / DT_CTRL))
CRZ_CTRL_PASSIVE_FRAMES = int(round(9.6 / DT_CTRL))
CRZ_CTRL_RESUME_REACTIVATE_FRAMES = int(round(0.08 / DT_CTRL))
CRZ_INFO_RESUME_PHASE_FRAMES = int(round(0.20 / DT_CTRL))
HOLD_REQUEST_FRAMES = int(round(6.0 / DT_CTRL))
RESUME_RELEASE_FRAMES = int(round(0.5 / DT_CTRL))


class CarController(CarControllerBase):
  def __init__(self, dbc_name, CP, VM):
    self.CP = CP
    self.apply_steer_last = 0
    self.packer = CANPacker(dbc_name)
    self.brake_counter = 0
    self.frame = 0

    # Longitudinal state variables
    self.long_counter = 0
    self.standstill_hold_frames = 0
    self.stop_intent_latched = False
    self.resume_release_frames = 0
    self.resume_crz_latched_frames = 0
    self.resume_phase_frames = 0
    self.resume_ctrl_active_prev = False
    self.virtual_resume_sent_latched = False
    self.resume_button_prev = False

    # FrogPilot variables
    self.doors_locked = False

  def update(self, CC, CS, now_nanos, frogpilot_toggles):
    can_sends = []

    apply_steer = 0

    if CC.latActive and not CS.out.steerFaultTemporary:
      # calculate steer and also set limits due to driver torque
      new_steer = int(round(CC.actuators.steer * CarControllerParams.STEER_MAX))
      apply_steer = apply_driver_steer_torque_limits(new_steer, self.apply_steer_last,
                                                     CS.out.steeringTorque, CarControllerParams)

    # Longitudinal control
    if self.CP.openpilotLongitudinalControl:
      long_active = CC.longActive

      # Stop-and-Go state management
      stopping = long_active and CS.out.vEgo < 0.5
      starting = long_active and CS.out.vEgo < 0.5 and not stopping

      # Lead visibility (always True for radar emulation when active)
      lead_visible = long_active

      # Standstill hold management
      if CS.out.standstill:
        self.standstill_hold_frames += 1
      else:
        self.standstill_hold_frames = 0

      standstill_hold_request = self.standstill_hold_frames > HOLD_REQUEST_FRAMES

      # Stop intent latch
      if stopping:
        self.stop_intent_latched = True
      if starting or not long_active:
        self.stop_intent_latched = False

      # Hold state management
      hold_request = standstill_hold_request and long_active
      hold_latched = self.stop_intent_latched and CS.out.standstill

      # CRZ_CTRL state transitions
      crz_hold_latched = self.resume_crz_latched_frames > 0
      crz_hold_passive = False

      if hold_latched:
        self.resume_crz_latched_frames = CRZ_CTRL_LATCH_FRAMES
      elif self.resume_crz_latched_frames > 0:
        self.resume_crz_latched_frames -= 1

      # Resume handling
      resume_requested = long_active and CS.out.vEgo < 0.5 and not CS.out.standstill
      if resume_requested:
        self.resume_release_frames = RESUME_RELEASE_FRAMES

      release_brake = self.resume_release_frames > 0
      if release_brake:
        self.resume_release_frames -= 1

      release_hold_requested = release_brake or (long_active and CS.out.vEgo > 0.5)

      # CRZ_CTRL resume active
      crz_ctrl_resume_active = release_brake and long_active

      # CRZ_INFO resume unlatching
      crz_info_resume_unlatching = release_brake and long_active

      # Accel calculation
      if not long_active:
        accel = 0.0
      else:
        accel = CC.actuators.accel

      if release_brake:
        accel = max(accel, 0.0)
      elif CS.out.standstill:
        accel = hold_latched_accel() if hold_latched else hold_brake_accel()
      elif self.stop_intent_latched and not release_hold_requested and stopping and CS.out.vEgo < NEAR_STOP_ENTRY_SPEED:
        accel = min(accel, near_stop_brake_accel(CS.out.vEgo))

      # Send tester present at ~2Hz
      if self.frame % TESTER_PRESENT_STEP == 0:
        can_sends.append(create_radar_tester_present(RADAR_BUS))

      # Send longitudinal messages at ~50Hz
      if self.frame % LONG_COMMAND_STEP == 0:
        can_sends.extend(create_longitudinal_messages(
          RADAR_BUS, accel, self.long_counter, long_active, lead_visible,
          hold_request=hold_request,
          crz_ctrl_hold_request=standstill_hold_request,
          hold_latched=hold_latched,
          crz_hold_latched=crz_hold_latched,
          crz_hold_passive=crz_hold_passive,
          crz_resume_active=crz_ctrl_resume_active,
          crz_info_resume_unlatching=crz_info_resume_unlatching,
          v_ego=CS.out.vEgo,
        ))
        self.long_counter += 1

    else:
      # Stock cruise control button commands
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

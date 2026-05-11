// CAN msgs we care about
#define MAZDA_LKAS          0x243
#define MAZDA_LKAS_HUD      0x440
#define MAZDA_CRZ_CTRL      0x21c
#define MAZDA_CRZ_BTNS      0x09d
#define MAZDA_STEER_TORQUE  0x240
#define MAZDA_ENGINE_DATA   0x202
#define MAZDA_PEDALS        0x165
#define MAZDA_BCM           0x420
#define MAZDA_CRZ_INFO      0x21b
#define MAZDA_RADAR_UDS     0x764

// CAN bus numbers
#define MAZDA_MAIN 0
#define MAZDA_AUX  1
#define MAZDA_CAM  2

enum { MAZDA_PARAM_LONGITUDINAL = 1 };

const SteeringLimits MAZDA_STEERING_LIMITS = {
  .max_steer = 800,
  .max_rate_up = 10,
  .max_rate_down = 25,
  .max_rt_delta = 300,
  .max_rt_interval = 250000,
  .driver_torque_factor = 1,
  .driver_torque_allowance = 15,
  .type = TorqueDriverLimited,
};

const LongitudinalLimits MAZDA_LONG_LIMITS = {
  .max_accel = 2000,
  .min_accel = -2000,
  .inactive_accel = 0,
};

const CanMsg MAZDA_TX_MSGS[] = {{MAZDA_LKAS, 0, 8}, {MAZDA_CRZ_BTNS, 0, 8}, {MAZDA_LKAS_HUD, 0, 8}, {MAZDA_BCM, 0, 8}};

const CanMsg MAZDA_LONG_TX_MSGS[] = {
  {MAZDA_LKAS, 0, 8},
  {MAZDA_CRZ_BTNS, 0, 8},
  {MAZDA_LKAS_HUD, 0, 8},
  {MAZDA_BCM, 0, 8},
  {MAZDA_CRZ_INFO, 0, 8},
  {MAZDA_CRZ_CTRL, 0, 8},
  {MAZDA_RADAR_UDS, 0, 8},
};

RxCheck mazda_rx_checks[] = {
  {.msg = {{MAZDA_CRZ_CTRL,     0, 8, .frequency = 50U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_CRZ_BTNS,     0, 8, .frequency = 10U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_STEER_TORQUE, 0, 8, .frequency = 83U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_ENGINE_DATA,  0, 8, .frequency = 100U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_PEDALS,       0, 8, .frequency = 50U}, { 0 }, { 0 }}},
};

// Longitudinal RX checks: CRZ_CTRL removed (we send it ourselves), PEDALS used for cruise state
RxCheck mazda_long_rx_checks[] = {
  {.msg = {{MAZDA_CRZ_BTNS,     0, 8, .frequency = 10U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_STEER_TORQUE, 0, 8, .frequency = 83U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_ENGINE_DATA,  0, 8, .frequency = 100U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_PEDALS,       0, 8, .frequency = 50U}, { 0 }, { 0 }}},
};

static bool mazda_longitudinal = false;

// track msgs coming from OP so that we know what CAM msgs to drop and what to forward
static void mazda_rx_hook(const CANPacket_t *to_push) {
  if ((int)GET_BUS(to_push) == MAZDA_MAIN) {
    int addr = GET_ADDR(to_push);

    // sample speed: scale by 0.01 to get kph
    if (addr == MAZDA_ENGINE_DATA) {
      int speed = (GET_BYTE(to_push, 2) << 8) | GET_BYTE(to_push, 3);
      vehicle_moving = speed > 10; // moving when speed > 0.1 kph
    }

    // sample steering torque
    if (addr == MAZDA_STEER_TORQUE) {
      int torque_driver_new = GET_BYTE(to_push, 0) - 127U;
      // update array of samples
      update_sample(&torque_driver, torque_driver_new);
    }

    // enter controls on rising edge of ACC, exit controls on ACC off
    if (!mazda_longitudinal && (addr == MAZDA_CRZ_CTRL)) {
      bool cruise_engaged = GET_BYTE(to_push, 0) & 0x8U;
      pcm_cruise_check(cruise_engaged);
    }

    // gas pressed
    if (addr == MAZDA_ENGINE_DATA) {
      gas_pressed = (GET_BYTE(to_push, 4) || (GET_BYTE(to_push, 5) & 0xF0U));
    }

    // brake pressed
    if (addr == MAZDA_PEDALS) {
      brake_pressed = (GET_BYTE(to_push, 0) & 0x10U);

      // longitudinal: get cruise state from PEDALS instead of CRZ_CTRL
      if (mazda_longitudinal) {
        bool cruise_engaged = GET_BIT(to_push, 3);   // PEDALS.ACC_ACTIVE (bit 3)
        bool acc_armed = GET_BIT(to_push, 2);          // PEDALS.ACC_OFF (bit 2)
        acc_main_on = acc_armed;

        if (acc_armed && !cruise_engaged_prev && !brake_pressed && !brake_pressed_prev) {
          pcm_cruise_check(cruise_engaged);
        }
      }
    }

    // CRZ_BTNS cancel handling for longitudinal
    if (mazda_longitudinal && (addr == MAZDA_CRZ_BTNS)) {
      bool cancel = GET_BIT(to_push, 0);  // CRZ_BTNS.CAN_OFF (bit 0)
      if (cancel) {
        controls_allowed = false;
      }
    }

    // stock ECU detection via LKAS message
    generic_rx_checks(addr == MAZDA_LKAS);
  }
}

static bool mazda_tx_hook(const CANPacket_t *to_send) {
  bool tx = true;
  int bus = GET_BUS(to_send);
  // Check if msg is sent on the main BUS
  if (bus == MAZDA_MAIN) {
    int addr = GET_ADDR(to_send);

    // steer cmd checks
    if (addr == MAZDA_LKAS) {
      int desired_torque = (((GET_BYTE(to_send, 0) & 0x0FU) << 8) | GET_BYTE(to_send, 1)) - 2048U;

      if (steer_torque_cmd_checks(desired_torque, -1, MAZDA_STEERING_LIMITS)) {
        tx = false;
      }
    }

    // cruise buttons check
    if (addr == MAZDA_CRZ_BTNS) {
      // allow resume spamming while controls allowed, but
      // only allow cancel while controls not allowed
      bool cancel_cmd = (GET_BYTE(to_send, 0) == 0x1U);
      if (!controls_allowed && !cancel_cmd) {
        tx = false;
      }
    }

    // longitudinal: CRZ_INFO acceleration checks
    if (mazda_longitudinal && (addr == MAZDA_CRZ_INFO)) {
      // ACCEL_CMD: 17|13@0+ (1,-4096) in DBC → 13-bit Motorola, offset -4096
      uint32_t accel_raw = ((uint32_t)(to_send->data[2] & 0x3U) << 11) |
                           ((uint32_t)to_send->data[3] << 3) |
                           ((uint32_t)to_send->data[4] >> 5);
      int desired_accel = (int)accel_raw - 4096;
      if (longitudinal_accel_checks(desired_accel, MAZDA_LONG_LIMITS)) {
        tx = false;
      }
    }

    // longitudinal: CRZ_CTRL cruise active check
    if (mazda_longitudinal && (addr == MAZDA_CRZ_CTRL)) {
      bool cruise_active = GET_BIT(to_send, 3);  // CRZ_ACTIVE (bit 3)
      if (!controls_allowed && cruise_active) {
        tx = false;
      }
    }

    // longitudinal: RADAR UDS whitelist (only allow tester present and session control)
    // ISO-TP single frame: data[0]=PCI, data[1]=SID, data[2]=SubFunction
    if (mazda_longitudinal && (addr == MAZDA_RADAR_UDS)) {
      bool tester_present = (to_send->data[1] == 0x3EU) && (to_send->data[2] == 0x80U);
      bool session_control = (to_send->data[1] == 0x10U) && ((to_send->data[2] == 0x01U) || (to_send->data[2] == 0x02U));
      if (!(tester_present || session_control)) {
        tx = false;
      }
    }
  }

  return tx;
}

static int mazda_fwd_hook(int bus, int addr) {
  int bus_fwd = -1;

  if (bus == MAZDA_MAIN) {
    bus_fwd = MAZDA_CAM;
  } else if (bus == MAZDA_CAM) {
    bool block = (addr == MAZDA_LKAS) || (addr == MAZDA_LKAS_HUD);
    if (!block) {
      bus_fwd = MAZDA_MAIN;
    }
  } else {
    // don't fwd
  }

  return bus_fwd;
}

static safety_config mazda_init(uint16_t param) {
  mazda_longitudinal = GET_FLAG(param, MAZDA_PARAM_LONGITUDINAL);
  acc_main_on = false;

  safety_config ret;
  if (mazda_longitudinal) {
    ret = BUILD_SAFETY_CFG(mazda_long_rx_checks, MAZDA_LONG_TX_MSGS);
  } else {
    ret = BUILD_SAFETY_CFG(mazda_rx_checks, MAZDA_TX_MSGS);
  }
  return ret;
}

const safety_hooks mazda_hooks = {
  .init = mazda_init,
  .rx = mazda_rx_hook,
  .tx = mazda_tx_hook,
  .fwd = mazda_fwd_hook,
};

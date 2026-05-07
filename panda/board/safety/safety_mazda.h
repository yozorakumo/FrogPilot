// CAN msgs we care about - standard Mazda (mazda_2017.dbc)
#define MAZDA_LKAS          0x243
#define MAZDA_LKAS_HUD      0x440
#define MAZDA_CRZ_CTRL      0x21c
#define MAZDA_CRZ_BTNS      0x09d
#define MAZDA_STEER_TORQUE  0x240
#define MAZDA_ENGINE_DATA   0x202
#define MAZDA_PEDALS        0x165

// CAN msgs - Mazda 2 DJ MT (mazda_2_dj_mt.dbc)
#define MAZDA_2_DJ_MT_LKAS       0x268
#define MAZDA_2_DJ_MT_LKAS_HUD   0x485
#define MAZDA_2_DJ_MT_CRZ_CTRL   0x162
#define MAZDA_2_DJ_MT_CRZ_BTNS   0x470
#define MAZDA_2_DJ_MT_PEDALS     0x315
#define MAZDA_2_DJ_MT_BCM        0x420
// STEER_TORQUE (0x240) and ENGINE_DATA (0x202) are shared with standard Mazda

// CAN bus numbers
#define MAZDA_MAIN 0
#define MAZDA_AUX  1
#define MAZDA_CAM  2

// safety param flags
const uint16_t MAZDA_PARAM_2_DJ_MT = 1U;

bool mazda_2_dj_mt = false;

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

const CanMsg MAZDA_TX_MSGS[] = {{MAZDA_LKAS, 0, 8}, {MAZDA_CRZ_BTNS, 0, 8}, {MAZDA_LKAS_HUD, 0, 8}};

const CanMsg MAZDA_2_DJ_MT_TX_MSGS[] = {
  {MAZDA_2_DJ_MT_LKAS, 0, 8},
  {MAZDA_2_DJ_MT_CRZ_BTNS, 0, 8},
  {MAZDA_2_DJ_MT_LKAS_HUD, 0, 8},
  {MAZDA_2_DJ_MT_BCM, 0, 8},
};

RxCheck mazda_rx_checks[] = {
  {.msg = {{MAZDA_CRZ_CTRL,     0, 8, .frequency = 50U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_CRZ_BTNS,     0, 8, .frequency = 10U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_STEER_TORQUE, 0, 8, .frequency = 83U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_ENGINE_DATA,  0, 8, .frequency = 100U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_PEDALS,       0, 8, .frequency = 50U}, { 0 }, { 0 }}},
};

RxCheck mazda_2_dj_mt_rx_checks[] = {
  {.msg = {{MAZDA_2_DJ_MT_CRZ_CTRL,   0, 8, .frequency = 50U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_2_DJ_MT_CRZ_BTNS,   0, 8, .frequency = 10U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_STEER_TORQUE,        0, 8, .frequency = 83U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_ENGINE_DATA,         0, 8, .frequency = 100U}, { 0 }, { 0 }}},
  {.msg = {{MAZDA_2_DJ_MT_PEDALS,     0, 8, .frequency = 50U}, { 0 }, { 0 }}},
};

// track msgs coming from OP so that we know what CAM msgs to drop and what to forward
static void mazda_rx_hook(const CANPacket_t *to_push) {
  if ((int)GET_BUS(to_push) == MAZDA_MAIN) {
    int addr = GET_ADDR(to_push);

    // sample speed: scale by 0.01 to get kph (ENGINE_DATA 0x202 is shared)
    if (addr == MAZDA_ENGINE_DATA) {
      int speed = (GET_BYTE(to_push, 2) << 8) | GET_BYTE(to_push, 3);
      vehicle_moving = speed > 10; // moving when speed > 0.1 kph
    }

    // sample steering torque (STEER_TORQUE 0x240 is shared)
    // Standard Mazda: 8-bit signal at byte 0 with offset -127
    // Mazda 2 DJ MT: 16-bit signal at bytes 0-1 with no offset
    if (addr == MAZDA_STEER_TORQUE) {
      int torque_driver_new;
      if (mazda_2_dj_mt) {
        torque_driver_new = (int16_t)((GET_BYTE(to_push, 0) << 8) | GET_BYTE(to_push, 1));
      } else {
        torque_driver_new = GET_BYTE(to_push, 0) - 127U;
      }
      // update array of samples
      update_sample(&torque_driver, torque_driver_new);
    }

    // enter controls on rising edge of ACC, exit controls on ACC off
    // Standard Mazda CRZ_CTRL (0x21C): CRZ_ACTIVE at bit 3 (byte 0)
    // Mazda 2 DJ MT CRZ_CTRL (0x162): CRZ_ACTIVE at bit 28 (byte 3, bit 4)
    if (mazda_2_dj_mt) {
      if (addr == MAZDA_2_DJ_MT_CRZ_CTRL) {
        bool cruise_engaged = GET_BYTE(to_push, 3) & 0x10U;
        pcm_cruise_check(cruise_engaged);
      }
    } else {
      if (addr == MAZDA_CRZ_CTRL) {
        bool cruise_engaged = GET_BYTE(to_push, 0) & 0x8U;
        pcm_cruise_check(cruise_engaged);
      }
    }

    // gas pressed (ENGINE_DATA 0x202 is shared)
    // Standard Mazda: PEDAL_GAS at bits 39-50 (byte 4 + byte 5 upper nibble)
    // Mazda 2 DJ MT: PEDAL_GAS at bits 39-46 (byte 4 only)
    if (addr == MAZDA_ENGINE_DATA) {
      if (mazda_2_dj_mt) {
        gas_pressed = GET_BYTE(to_push, 4) != 0U;
      } else {
        gas_pressed = (GET_BYTE(to_push, 4) || (GET_BYTE(to_push, 5) & 0xF0U));
      }
    }

    // brake pressed
    // Standard Mazda PEDALS (0x165): BRAKE_ON at bit 4 (byte 0, bit 4)
    // Mazda 2 DJ MT PEDALS (0x315): BRAKE_ON at bit 16 (byte 2, bit 0)
    if (mazda_2_dj_mt) {
      if (addr == MAZDA_2_DJ_MT_PEDALS) {
        brake_pressed = GET_BYTE(to_push, 2) & 0x1U;
      }
    } else {
      if (addr == MAZDA_PEDALS) {
        brake_pressed = (GET_BYTE(to_push, 0) & 0x10U);
      }
    }

    // stock ECU detection via LKAS message
    bool stock_lkas = mazda_2_dj_mt ? (addr == MAZDA_2_DJ_MT_LKAS) : (addr == MAZDA_LKAS);
    generic_rx_checks(stock_lkas);
  }
}

static bool mazda_tx_hook(const CANPacket_t *to_send) {
  bool tx = true;
  int bus = GET_BUS(to_send);
  // Check if msg is sent on the main BUS
  if (bus == MAZDA_MAIN) {
    int addr = GET_ADDR(to_send);

    // steer cmd checks
    // Standard Mazda CAM_LKAS (0x243): LKAS_REQUEST 12-bit at bits 3-14
    // Mazda 2 DJ MT CAM_LKAS (0x268): LKAS_REQUEST 16-bit at bits 7-22
    int lkas_addr = mazda_2_dj_mt ? MAZDA_2_DJ_MT_LKAS : MAZDA_LKAS;
    if (addr == lkas_addr) {
      int desired_torque;
      if (mazda_2_dj_mt) {
        desired_torque = (int16_t)((GET_BYTE(to_send, 0) << 8) | GET_BYTE(to_send, 1));
      } else {
        desired_torque = (((GET_BYTE(to_send, 0) & 0x0FU) << 8) | GET_BYTE(to_send, 1)) - 2048U;
      }

      if (steer_torque_cmd_checks(desired_torque, -1, MAZDA_STEERING_LIMITS)) {
        tx = false;
      }
    }

    // cruise buttons check
    // Standard Mazda CRZ_BTNS (0x09D): CAN_OFF at bit 0 (byte 0, bit 0)
    // Mazda 2 DJ MT CRZ_BTNS (0x470): CAN_OFF at bit 8 (byte 1, bit 0)
    int crz_btns_addr = mazda_2_dj_mt ? MAZDA_2_DJ_MT_CRZ_BTNS : MAZDA_CRZ_BTNS;
    if (addr == crz_btns_addr) {
      // allow resume spamming while controls allowed, but
      // only allow cancel while controls not allowed
      bool cancel_cmd;
      if (mazda_2_dj_mt) {
        cancel_cmd = (GET_BYTE(to_send, 1) & 0x1U) != 0U;
      } else {
        cancel_cmd = (GET_BYTE(to_send, 0) == 0x1U);
      }
      if (!controls_allowed && !cancel_cmd) {
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
    bool block;
    if (mazda_2_dj_mt) {
      block = (addr == MAZDA_2_DJ_MT_LKAS) || (addr == MAZDA_2_DJ_MT_LKAS_HUD);
    } else {
      block = (addr == MAZDA_LKAS) || (addr == MAZDA_LKAS_HUD);
    }
    if (!block) {
      bus_fwd = MAZDA_MAIN;
    }
  } else {
    // don't fwd
  }

  return bus_fwd;
}

static safety_config mazda_init(uint16_t param) {
  mazda_2_dj_mt = GET_FLAG(param, MAZDA_PARAM_2_DJ_MT);

  safety_config ret;
  if (mazda_2_dj_mt) {
    ret = BUILD_SAFETY_CFG(mazda_2_dj_mt_rx_checks, MAZDA_2_DJ_MT_TX_MSGS);
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

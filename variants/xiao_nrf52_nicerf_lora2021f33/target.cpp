#include <Arduino.h>
#include "target.h"
#include <helpers/ArduinoHelpers.h>
#include <helpers/radiolib/NiceRF_LoRa2021F33.h>

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  // The XIAO nRF52840 has no physical user button - see variants/xiao_nrf52.
  // Pin -1 keeps the object defined for UITask but inert (no phantom power-off).
  MomentaryButton user_btn(-1, 1000, true);
#endif

XiaoNrf52Board board;

// The LR2021 needs its own SPI pin set, so P_LORA_SCLK/MISO/MOSI are defined in
// platformio.ini and CustomLR2021::std_init() applies them via SPI::setPins().
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);

WRAPPER_CLASS radio_driver(radio, board);

VolatileRTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);

EnvironmentSensorManager sensors;

bool radio_init() {
  rtc_clock.begin(Wire);

#ifdef PIN_LORA_CE
  // Always start from a cold module. Without this the LR2021 survives an MCU
  // reset in a state findChip() cannot reach and init fails with -2 on every
  // boot that is not a fresh power-up - see nicerf_lora2021f33_power_cycle().
  nicerf_lora2021f33_power_cycle();
#endif

  nicerf_lora2021f33_pre_init(radio);        // PA table must be set before begin() applies TX power
  bool radio_ok = radio.std_init(&SPI);
#ifdef PIN_LORA_CE
  // And retry if it still failed. The chip is not always freed by one cycle:
  // on this board it has stayed unreachable across three in a row and then come
  // back later on its own, so a retry is worth the boot time.
  for (int attempt = 1; attempt <= 2 && !radio_ok; attempt++) {
    Serial.printf("[LR2021] init failed, power-cycling the module (%d/2)\r\n", attempt);
    nicerf_lora2021f33_power_cycle();
    radio_ok = radio.std_init(&SPI);
  }
#endif
  if (!radio_ok) return false;
  nicerf_lora2021f33_post_init(radio);       // front-end RF switch: needs an initialised chip

#if defined(LR2021_PRAM_UPD) && defined(RADIOLIB_GODMODE)
  // load the firmware patch. Must come after std_init(), because begin() ->
  // findChip() resets the chip and would wipe it. See the note in NiceRF_LoRa2021F33.h.
  {
    int16_t st = nicerf_lora2021f33_pram_load(radio);
    bool ok = false; uint16_t ver = 0;
    nicerf_lora2021f33_pram_status(radio, &ok, &ver);
    Serial.printf("[LR2021] pram load: rc=%d -> loaded=%s version=0x%04X\r\n",
                  (int)st, ok ? "YES" : "NO", (unsigned)ver);
  }
#endif

#if defined(NICERF_LORA2021F33_SIMO) && defined(RADIOLIB_GODMODE)
  // DC-DC. Measured on this module: 20.5 -> 16.0 mA total in Rx (module part
  // ~11 -> ~6.5 mA, ~41%), so the SIMO inductor really is fitted. The chip
  // resets to SIMO_OFF, hence setting it on every init.
  // Load the PRAM first - the datasheet lists "DCDC (SIMO) impact on
  // sensitivity" for sub-GHz LoRa as a limitation the patch fixes.
  {
    int16_t st = nicerf_lora2021f33_set_simo(radio, true);
    Serial.printf("[LR2021] simo: rc=%d\r\n", (int)st);
  }
#endif

  nicerf_lora2021f33_report(radio);          // module identity + supply/temperature

  return true;
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng); // create new random identity
}

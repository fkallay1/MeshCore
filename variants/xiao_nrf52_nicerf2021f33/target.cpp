#include <Arduino.h>
#include "target.h"
#include <helpers/ArduinoHelpers.h>
#include <helpers/radiolib/NiceRF2021F33.h>

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

  nicerf2021f33_pre_init(radio);        // PA table must be set before begin() applies TX power
  if (!radio.std_init(&SPI)) return false;
  nicerf2021f33_post_init(radio);       // front-end RF switch: needs an initialised chip
  nicerf2021f33_report(radio);          // module identity + supply/temperature

  return true;
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng); // create new random identity
}

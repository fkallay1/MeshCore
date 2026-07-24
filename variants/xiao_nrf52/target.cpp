#include <Arduino.h>
#include "target.h"
#include <helpers/ArduinoHelpers.h>

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
#endif

#ifdef PIN_USER_BTN
  // The XIAO nRF52840 has no physical user button. PIN_USER_BTN (PIN_BUTTON1=0,
  // the D0 edge pin) reads LOW when the LoRa radio is attached, which the
  // MomentaryButton (reverse=true => LOW == pressed) interprets as a held
  // press, so the repeater UITask fires a 1s long-press and powers the board
  // off ~1s after boot. Use pin -1 so the button object stays defined (UITask
  // needs it) but is inert (check() returns NONE) — no phantom power-off.
  MomentaryButton user_btn(-1, 1000, true);
#endif

XiaoNrf52Board board;

RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);

WRAPPER_CLASS radio_driver(radio, board);

VolatileRTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);

EnvironmentSensorManager sensors;

bool radio_init() {
  rtc_clock.begin(Wire);

  return radio.std_init(&SPI);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng); // create new random identity
}
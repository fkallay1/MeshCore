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
  //en: CE (module pin 5) enables the module's own LDO. It has an internal
  //en: pull-up, so the module powers up even unconnected, but driving it makes
  //en: the state deterministic and gives us a real power-down for battery use.
  //en: NiceRF: when CE is low, NSS and RESET must be low too or current leaks
  //en: in through the ESD diodes.
  pinMode(PIN_LORA_CE, OUTPUT);
  digitalWrite(PIN_LORA_CE, HIGH);
  delay(5);   //en: let the module LDO settle before the first SPI transaction
#endif

  nicerf_lora2021f33_pre_init(radio);        // PA table must be set before begin() applies TX power
  if (!radio.std_init(&SPI)) return false;
  nicerf_lora2021f33_post_init(radio);       // front-end RF switch: needs an initialised chip
  //en: which TCXO voltage the chip took - 0.0 means the configured one was rejected and
  //en: std_init() fell back, i.e. this module runs off a plain crystal.
  //sk: ktore napatie TCXO cip vzal - 0.0 znamena, ze nastavene odmietol a std_init()
  //sk: spadol na fallback, cize tento modul bezi na obycajnom krystale.
  Serial.print("[LR2021] tcxo="); Serial.print(radio.getTcxoUsed(), 2); Serial.println(" V");

#if defined(LR2021_PRAM_UPD) && defined(RADIOLIB_GODMODE)
  //en: load the firmware patch. Must come after std_init(), because begin() ->
  //en: findChip() resets the chip and would wipe it. See the note in NiceRF_LoRa2021F33.h.
  {
    int16_t st = nicerf_lora2021f33_pram_load(radio);
    bool ok = false; uint16_t ver = 0;
    nicerf_lora2021f33_pram_status(radio, &ok, &ver);
    Serial.printf("[LR2021] pram load: rc=%d -> loaded=%s version=0x%04X\r\n",
                  (int)st, ok ? "YES" : "NO", (unsigned)ver);
  }
#endif

#if defined(NICERF_LORA2021F33_SIMO) && defined(RADIOLIB_GODMODE)
  //en: DC-DC. Measured on this module: 20.5 -> 16.0 mA total in Rx (module part
  //en: ~11 -> ~6.5 mA, ~41%), so the SIMO inductor really is fitted. The chip
  //en: resets to SIMO_OFF, hence setting it on every init.
  //en: Load the PRAM first - the datasheet lists "DCDC (SIMO) impact on
  //en: sensitivity" for sub-GHz LoRa as a limitation the patch fixes.
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

#ifdef FK_NICERF_LORA2021F33_TEST
/*
  Bench test commands, so the module can be poked without a rebuild.
  Everything here is behind FK_NICERF_LORA2021F33_TEST and never ships enabled.

    fk info        - chip identity, supply, temperature, errors, PRAM state
    fk simo on|off - switch the chip's internal regulator to DC-DC / LDO
    fk ce on|off   - power the whole module down via its LDO enable pin
    fk pram        - report PRAM state
    fk pram load   - (re)load the firmware patch, if built with LR2021_PRAM_UPD
*/
bool nicerfTestCliCommand(char* command, char* reply) {
  if (memcmp(command, "fk ", 3) != 0) return false;
  const char* arg = command + 3;

  if (memcmp(arg, "info", 4) == 0) {
    //en: GetVBat/GetTemp need standby - read while the radio is in Rx they return
    //en: 2 mV / 0.0 C.
    //en: KNOWN QUIRK, unexplained: once the radio has been receiving, VBat reads
    //en: 2454 mV here instead of the 3312 mV the boot report gets, while the
    //en: temperature stays correct. Not caused by the PRAM, not by SIMO and not by
    //en: the standby flavour - all three were tested and ruled out with same-boot
    //en: A/B runs. Trust the boot-time reading; treat this one as indicative only.
    radio.standby(RADIOLIB_LR2021_STANDBY_XOSC);
    nicerf_lora2021f33_report(radio);
    uint8_t maj = 0, min = 0; uint16_t vbat = 0, err = 0;
    radio.getVersion(&maj, &min);
    radio.getVbat(13, &vbat);
    radio.getErrors(&err);
    float t = radio.getTemperature(RADIOLIB_LR2021_TEMP_SOURCE_VBE, 13);
    radio.startReceive();
    sprintf(reply, "fw=%u.%u vbat=%umV temp=%.1fC err=0x%04X", (unsigned)maj,
            (unsigned)min, (unsigned)vbat, t, (unsigned)err);
    return true;
  }

#if defined(RADIOLIB_GODMODE)
  if (memcmp(arg, "simo ", 5) == 0) {
    bool on = (memcmp(arg + 5, "on", 2) == 0);
    int16_t st = nicerf_lora2021f33_set_simo(radio, on);   //en: leaves the chip in STDBY_RC
    radio.standby(RADIOLIB_LR2021_STANDBY_XOSC);      //en: VBat only reads right with the XOSC up
    //en: measure BEFORE re-arming Rx - the ADC is only valid in standby
    uint8_t maj = 0, min = 0; uint16_t vbat = 0, err = 0;
    radio.getVersion(&maj, &min);
    radio.getVbat(13, &vbat);
    radio.getErrors(&err);
    float t = radio.getTemperature(RADIOLIB_LR2021_TEMP_SOURCE_VBE, 13);
    radio.startReceive();   //en: wrapper's idle() is protected; its state already says
                            //en: RX, which matches reality once the chip receives again
    sprintf(reply, "simo=%s rc=%d | fw=%u.%u vbat=%umV temp=%.1fC err=0x%04X",
            on ? "on" : "off", (int)st, (unsigned)maj, (unsigned)min,
            (unsigned)vbat, t, (unsigned)err);
    return true;
  }

  if (memcmp(arg, "pram", 4) == 0) {
    if (memcmp(arg + 4, " load", 5) == 0) {
#ifdef LR2021_PRAM_UPD
      int16_t st = nicerf_lora2021f33_pram_load(radio);
      bool ok = false; uint16_t ver = 0;
      nicerf_lora2021f33_pram_status(radio, &ok, &ver);
      radio.startReceive();   //en: setRegMode/PRAM left us in standby - re-arm Rx directly
                            //en: (wrapper's idle() is protected; its state already says RX,
                            //en:  which matches reality once the chip is receiving again)
      sprintf(reply, "pram load rc=%d -> loaded=%s ver=0x%04X", (int)st,
              ok ? "YES" : "NO", (unsigned)ver);
#else
      strcpy(reply, "not built with LR2021_PRAM_UPD");
#endif
    } else {
      bool ok = false; uint16_t ver = 0;
      nicerf_lora2021f33_pram_status(radio, &ok, &ver);
      sprintf(reply, "pram loaded=%s ver=0x%04X", ok ? "YES" : "NO", (unsigned)ver);
    }
    return true;
  }
#endif  // RADIOLIB_GODMODE

#ifdef PIN_LORA_CE
  if (memcmp(arg, "ce ", 3) == 0) {
    bool on = (memcmp(arg + 3, "on", 2) == 0);
    digitalWrite(PIN_LORA_CE, on ? HIGH : LOW);
    delay(20);
    //en: with CE low the module LDO is off; whatever the chip still answers is
    //en: coming parasitically through the ESD diodes of the driven SPI pins
    uint8_t maj = 0, min = 0;
    int16_t st = radio.getVersion(&maj, &min);
    sprintf(reply, "ce=%s | getVersion rc=%d fw=%u.%u %s", on ? "HIGH" : "LOW",
            (int)st, (unsigned)maj, (unsigned)min,
            (maj == 0x01 && min == 0x18) ? "(chip answers)" : "(no valid answer)");
    return true;
  }
#endif

  strcpy(reply, "fk: info | simo on|off | ce on|off | pram [load]");
  return true;
}
#endif  // FK_NICERF_LORA2021F33_TEST

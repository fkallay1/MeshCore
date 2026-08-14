#pragma once

#include <RadioLib.h>

/* -----------------------------------------------------------------------------
   NiceRF LoRa2021F33-2G4 — Semtech LR2021 with an external dual-band front-end.

   Board-independent support for the module: RF switch wiring, PA drive table and
   the init sequence. A board variant only supplies the MCU pins and includes this.

   Front-end control (confirmed by NiceRF support and their demo code V1.1,
   LoRa/Core/Src/lr2021.c). None of these are module pins - they are LR2021 DIOs
   routed to the front-end on the module PCB, so the chip drives them itself:

     DIO9  IRQ, brought out on module pin 18   -> LR2021_IRQ_DIO=9
     DIO6  sub-GHz PA enable,  RF switch active during TX_LF
     DIO5  2.4 GHz LNA enable, RF switch active during RX_HF
             (LOW = bypass ON, costs 12 dB of sensitivity - see datasheet note)
     DIO8  2.4 GHz PA enable,  RF switch active during TX_HF
     DIO7  2.4 GHz FEM supply, demo drives it as a static GPIO HIGH; expressed
             here as an RF switch pin that is HIGH in every mode

   Two RadioLib details this file works around:

   1) LR2021::setRfSwitchTable() writes the mode mask into the CHIP's per-DIO RF
      switch config, so the LR2021 switches the front-end in hardware. That is why
      the HF rows matter even though the driver only ever calls MODE_RX/MODE_TX at
      runtime. BUT it indexes the accumulated config with the table column instead
      of the DIO number (LR2021_config.cpp, setDioRfSwitchConfig(dioNum + 5,
      dioConfigs[i])). The two agree only while pins[i] == DIO(5+i), so this pin
      array MUST stay dense and ordered from DIO5. Listing DIO6 alone would write
      a zero config and the sub-GHz PA would never be enabled.

   2) RadioLib's built-in PA table is tuned for Semtech's reference design, whose
      lowest step already drives this module to roughly +19 dBm at the antenna.
      NICERF2021F33_PA_TABLE_LF below re-maps the requested power so that it means
      dBm AT THE MODULE OUTPUT, using the fixed PA drive (duty 7 / 6 slices) that
      NiceRF's own demo uses. Define NICERF2021F33_STOCK_PA_TABLE to opt out.

   2.4 GHz is prepared but NOT enabled: without NICERF2021F33_ENABLE_24G the HF
   DIOs are left untouched (RADIOLIB_NC) and only the sub-GHz path is programmed.
   ----------------------------------------------------------------------------- */

#define NICERF2021F33_IRQ_DIO       9      // LR2021 DIO carrying IRQ to module pin 18
#define NICERF2021F33_TCXO_VOLTAGE  3.3f   // NiceRF: LR20XX_SYSTEM_TCXO_CTRL_3_3V

static const uint32_t nicerf2021f33_rfswitch_dios[Module::RFSWITCH_MAX_PINS] = {
  RADIOLIB_LR2021_DIO5,      // must stay at index 0 (see note 1 above)
  RADIOLIB_LR2021_DIO6,      // must stay at index 1
#ifdef NICERF2021F33_ENABLE_24G
  RADIOLIB_LR2021_DIO7,
  RADIOLIB_LR2021_DIO8,
#else
  RADIOLIB_NC,
  RADIOLIB_NC,
#endif
  RADIOLIB_NC,
};

static const Module::RfSwitchMode_t nicerf2021f33_rfswitch_table[] = {
  //                       DIO5  DIO6  DIO7  DIO8
  { LR2021::MODE_STBY,   { LOW,  LOW,  HIGH, LOW  } },
  { LR2021::MODE_RX,     { LOW,  LOW,  HIGH, LOW  } },  // sub-GHz RX has no external LNA
  { LR2021::MODE_TX,     { LOW,  HIGH, HIGH, LOW  } },  // sub-GHz PA
  { LR2021::MODE_RX_HF,  { HIGH, LOW,  HIGH, LOW  } },  // 2.4 GHz LNA (bypass OFF)
  { LR2021::MODE_TX_HF,  { LOW,  LOW,  HIGH, HIGH } },  // 2.4 GHz PA
  END_OF_MODE_TABLE,
};

#ifndef NICERF2021F33_STOCK_PA_TABLE
/*
   Sub-GHz PA table, indexed by RadioLib as table[power + 9] for power -9..+22.

   paVal is the LR2021 SetTxParams value in HALF-dBm steps - the same "Register
   Value" column as the datasheet power tables. paDutyCycle/paSlices are held at
   the fixed 7/6 that NiceRF's demo uses for the whole sub-GHz range, which is
   also the configuration their published power figures were measured with.

   NICERF2021F33_PAVAL_FOR_OUT[] maps a target MODULE OUTPUT of 10..30 dBm to the
   register value, interpolated from the datasheet's 868/915 MHz table
   (register -11/-5/1/7/13/19/25/31/37/44 -> 10.4/13.3/16.2/18.9/21.4/24.0/
   26.5/28.3/29.3/29.8 dBm).

   Interpolated, not measured, BUT cross-checked on hardware: supply current at
   requested 14/18/20/22 dBm came out 200/230/268/310 mA against the datasheet's
   186/213/243/277 mA for the same register values (the offset is the XIAO's own
   draw). The curve tracks, so the table is good to roughly +-1 dB. Still measure
   before trusting an absolute number near a regulatory limit.

   NICERF2021F33_PA_OFFSET shifts what a requested dBm means. It defaults to 8,
   which lines the dial up with how MeshCore behaves on other high-power boards
   (RAK3401 "1W" and friends): the requested number is nominal, reality is higher,
   and 22 is the top of the scale.

       set tx 6  -> ~14 dBm out (EU 868 default sub-band limit)
       set tx 14 -> ~22 dBm out
       set tx 22 -> ~30 dBm out (module maximum, ~1 W)

   The offset exists because RadioLib hard-limits an LF request to -9..+22
   (checkOutputPower) while the module reaches ~29.8 dBm, so without a shift the
   dial would stop at 22 dBm out and leave 8 dB unused.

   Why not simply drop this table and let RadioLib's stock one do the job, the way
   RAK3401 does? Because that board's SKY66122 FEM adds ~8 dB, while this module's
   PA adds ~16 dB at low drive and saturates near 30 dBm (datasheet: chip -5.5 dBm
   -> 10.4 out, +3.5 -> 18.9, +12.5 -> 26.5, +22 -> 29.8). With the stock table the
   request equals the CHIP output, so anything from ~10 dBm up would already push
   the PA into compression and the whole dial would collapse onto the maximum.
   Keeping the chip drive low and calibrated is what makes the steps mean anything.
   Anything above register ~37 buys +0.5 dB for +87 mA - not worth it.
*/
#ifndef NICERF2021F33_PA_OFFSET
  #define NICERF2021F33_PA_OFFSET 8
#endif

//en: index 0 = 10 dBm module output ... index 20 = 30 dBm
static const int8_t NICERF2021F33_PAVAL_FOR_OUT[21] = {
  -11, -10, -8, -6, -4, -1,  1,  3,  5,  7, 10,   // 10..20 dBm
   12,  14, 17, 19, 21, 24, 27, 30, 35, 44        // 21..30 dBm
};

static LR2021PaTableEntry_t NICERF2021F33_PA_TABLE_LF[32];

//en: expected module output (dBm) for a requested power, after clamping
static inline int nicerf2021f33_expected_out(int requested) {
  int out = requested + (NICERF2021F33_PA_OFFSET);
  if (out < 10) out = 10;
  if (out > 30) out = 30;
  return out;
}

static inline void nicerf2021f33_build_pa_table() {
  for (int i = 0; i < 32; i++) {
    int out = nicerf2021f33_expected_out(i - 9);
    NICERF2021F33_PA_TABLE_LF[i].paDutyCycle = 7;
    NICERF2021F33_PA_TABLE_LF[i].paSlices    = 6;
    NICERF2021F33_PA_TABLE_LF[i].paVal       = NICERF2021F33_PAVAL_FOR_OUT[out - 10];
  }
}
#endif  // NICERF2021F33_STOCK_PA_TABLE


/* Call BEFORE std_init(): begin() already applies LORA_TX_POWER, so the table has
   to be in place by then. Pure setter, no SPI traffic. */
template <class T> static inline void nicerf2021f33_pre_init(T& radio) {
#ifndef NICERF2021F33_STOCK_PA_TABLE
  nicerf2021f33_build_pa_table();
  radio.setPaTable(NICERF2021F33_PA_TABLE_LF, false);
#endif
}

/* Call AFTER std_init(): programming the DIO functions needs a live chip. */
template <class T> static inline void nicerf2021f33_post_init(T& radio) {
  radio.setRfSwitchTable(nicerf2021f33_rfswitch_dios, nicerf2021f33_rfswitch_table);
}

/*
   Everything the chip can tell us about itself. The LR2021 has no UID / serial
   number command - GetVersion (0x0101) is the only identification there is, so
   the module type below comes from the build config, not from the hardware.
   GetVBat is the useful one: it reports the supply the CHIP actually sees. With
   VCC wired and the module's internal LDO regulating it reads ~3.3 V; a module
   left on VCC-less parasitic feed through the GPIO ESD diodes will answer SPI
   but cannot run its PA, and this is the reading that tells the two apart.
*/
template <class T> static inline void nicerf2021f33_report(T& radio) {
  uint8_t  major = 0, minor = 0;
  uint16_t vbat_mv = 0, errors = 0;
  radio.getVersion(&major, &minor);
  radio.getVbat(13, &vbat_mv);
  radio.getErrors(&errors);
  float temp = radio.getTemperature(RADIOLIB_LR2021_TEMP_SOURCE_VBE, 13);

  Serial.printf("[LR2021] NiceRF LoRa2021F33-2G4  fw=%u.%u  vbat=%umV  temp=%.1fC  errors=0x%04X\r\n",
                (unsigned)major, (unsigned)minor, (unsigned)vbat_mv, temp, (unsigned)errors);
  Serial.printf("[LR2021] irq=DIO%d  tcxo=%.1fV  freq=%.3fMHz  band=%s  rfsw=DIO5/DIO6%s\r\n",
                (int)NICERF2021F33_IRQ_DIO, (double)NICERF2021F33_TCXO_VOLTAGE,
                (double)LORA_FREQ, (LORA_FREQ > 1500.0f) ? "HF(2G4)" : "LF(sub-GHz)",
#ifdef NICERF2021F33_ENABLE_24G
                "/DIO7/DIO8");
#else
                " (2G4 off)");
#endif

#ifndef NICERF2021F33_STOCK_PA_TABLE
  //en: what a requested dBm actually becomes: register value (= chip drive) and
  //en: the module output the datasheet predicts for it. RadioLib refuses any
  //en: request outside -9..+22 outright, so 22 is always the top of the dial.
  int idx = (int)LORA_TX_POWER + 9;
  if (idx < 0) idx = 0;
  if (idx > 31) idx = 31;
  int8_t pv = NICERF2021F33_PA_TABLE_LF[idx].paVal;
  Serial.printf("[LR2021] pa=nicerf(duty%u/slices%u) offset=%+d  tx=%ddBm req -> paVal=%d (%.1fdBm chip) -> ~%d dBm module out\r\n",
                (unsigned)NICERF2021F33_PA_TABLE_LF[idx].paDutyCycle,
                (unsigned)NICERF2021F33_PA_TABLE_LF[idx].paSlices,
                (int)NICERF2021F33_PA_OFFSET,
                (int)LORA_TX_POWER, (int)pv, (double)pv / 2.0,
                nicerf2021f33_expected_out((int)LORA_TX_POWER));
  Serial.printf("[LR2021] dial: tx=-9 -> ~%d dBm ... tx=22 -> ~%d dBm out\r\n",
                nicerf2021f33_expected_out(-9), nicerf2021f33_expected_out(22));
#else
  Serial.printf("[LR2021] pa=radiolib-default  tx=%ddBm req\r\n", (int)LORA_TX_POWER);
#endif
}

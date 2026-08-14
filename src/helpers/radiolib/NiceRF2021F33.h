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

   The mapping was interpolated from the datasheet's 868/915 MHz table
   (register -11/-5/1/7/13/19/25/31/37/44 -> 10.4/13.3/16.2/18.9/21.4/24.0/
   26.5/28.3/29.3/29.8 dBm) so that a requested power of N dBm lands near N dBm
   at the antenna port. Below ~10 dBm the module cannot go any lower, so those
   steps all clamp to the bottom register value.

   INTERPOLATED, NOT MEASURED - verify against a power meter before trusting the
   absolute numbers, especially near a regulatory limit.
*/
static const LR2021PaTableEntry_t NICERF2021F33_PA_TABLE_LF[32] = {
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  // -9 dBm requested -> ~10.4 dBm out (floor)
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  // -8
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  // -7
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  // -6
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  // -5
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  // -4
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  // -3
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  // -2
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  // -1
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  //  0
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  //  1
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  //  2
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  //  3
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  //  4
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  //  5
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  //  6
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  //  7
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  //  8
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  //  9
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -11 },  // 10 dBm -> ~10.4
  { .paDutyCycle = 7, .paSlices = 6, .paVal = -10 },  // 11
  { .paDutyCycle = 7, .paSlices = 6, .paVal =  -8 },  // 12
  { .paDutyCycle = 7, .paSlices = 6, .paVal =  -6 },  // 13
  { .paDutyCycle = 7, .paSlices = 6, .paVal =  -4 },  // 14  <- EU 868 ERP limit band
  { .paDutyCycle = 7, .paSlices = 6, .paVal =  -1 },  // 15
  { .paDutyCycle = 7, .paSlices = 6, .paVal =   1 },  // 16
  { .paDutyCycle = 7, .paSlices = 6, .paVal =   3 },  // 17
  { .paDutyCycle = 7, .paSlices = 6, .paVal =   5 },  // 18
  { .paDutyCycle = 7, .paSlices = 6, .paVal =   7 },  // 19
  { .paDutyCycle = 7, .paSlices = 6, .paVal =  10 },  // 20
  { .paDutyCycle = 7, .paSlices = 6, .paVal =  12 },  // 21
  { .paDutyCycle = 7, .paSlices = 6, .paVal =  14 },  // 22 -> ~22 dBm out (~160 mA more than 14 dBm)
};
#endif

/* Call BEFORE std_init(): begin() already applies LORA_TX_POWER, so the table has
   to be in place by then. Pure setter, no SPI traffic. */
template <class T> static inline void nicerf2021f33_pre_init(T& radio) {
#ifndef NICERF2021F33_STOCK_PA_TABLE
  radio.setPaTable(const_cast<LR2021PaTableEntry_t*>(NICERF2021F33_PA_TABLE_LF), false);
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
  //en: show what the requested dBm actually turns into at the chip - the module
  //en: output is that plus the external PA gain, per the datasheet power table
  int idx = (int)LORA_TX_POWER + 9;
  if (idx < 0) idx = 0;
  if (idx > 31) idx = 31;
  int8_t pv = NICERF2021F33_PA_TABLE_LF[idx].paVal;
  Serial.printf("[LR2021] pa=nicerf(duty%u/slices%u)  tx=%ddBm req -> paVal=%d (%.1fdBm chip drive)\r\n",
                (unsigned)NICERF2021F33_PA_TABLE_LF[idx].paDutyCycle,
                (unsigned)NICERF2021F33_PA_TABLE_LF[idx].paSlices,
                (int)LORA_TX_POWER, (int)pv, (double)pv / 2.0);
#else
  Serial.printf("[LR2021] pa=radiolib-default  tx=%ddBm req\r\n", (int)LORA_TX_POWER);
#endif
}

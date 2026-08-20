#pragma once

#include <RadioLib.h>
#include "MeshCore.h"

//en: This class carries the stale-reply guard (see getPacketLength below), so the
//en: heartbeat can print its counter without asking which chip is fitted.
//sk: Tato trieda ma guard pretecenej odpovede (viď getPacketLength nizsie), takze
//sk: heartbeat vie vypisat jeho pocitadlo bez toho, aby sa pytal na cip.
#ifndef FK_RADIO_HAS_STALE_GUARD
#define FK_RADIO_HAS_STALE_GUARD 1
#endif

class CustomLR2021 : public LR2021 {
  uint32_t _preambleMillis = 66;
  uint32_t _maxPayloadMillis = 3934;
  uint32_t _activityAt = 0;
  bool _headerSeen = false;
  bool _rx_boosted = false;
  uint32_t _stale_pktlen_reads = 0;
  float _tcxo_used = 0.0f;

  public:
    CustomLR2021(Module *mod) : LR2021(mod) { irqDioNum = LR2021_IRQ_DIO; }

    bool std_init(SPIClass* spi = NULL)
    {
      
  #ifdef LR2021_TCXO_VOLTAGE
      float tcxo = LR2021_TCXO_VOLTAGE;
  #else
      float tcxo = 1.6f;
  #endif

  #ifdef LORA_CR
      uint8_t cr = LORA_CR;
  #else
      uint8_t cr = 5;
  #endif

  #if defined(P_LORA_SCLK)
    #ifdef NRF52_PLATFORM
      if (spi) { spi->setPins(P_LORA_MISO, P_LORA_SCLK, P_LORA_MOSI); spi->begin(); }
    #elif defined(RP2040_PLATFORM)
      if (spi) {
        spi->setMISO(P_LORA_MISO);
        //spi->setCS(P_LORA_NSS); // Setting CS results in freeze
        spi->setSCK(P_LORA_SCLK);
        spi->setMOSI(P_LORA_MOSI);
        spi->begin();
      }
    #else
      if (spi) spi->begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);
    #endif
  #endif
      int status = begin(LORA_FREQ, LORA_BW, LORA_SF, cr, RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE, LORA_TX_POWER, 16, tcxo);
      // if radio init fails with -707/-706, try again with tcxo voltage set to 0.0f
      if (status == RADIOLIB_ERR_SPI_CMD_FAILED || status == RADIOLIB_ERR_SPI_CMD_INVALID) {
        tcxo = 0.0f;
        status = begin(LORA_FREQ, LORA_BW, LORA_SF, cr, RADIOLIB_LR2021_LORA_SYNC_WORD_PRIVATE, LORA_TX_POWER, 16, tcxo);
      }
      if (status != RADIOLIB_ERR_NONE) {
        Serial.print("ERROR: radio init failed: ");
        Serial.println(status);
        return false;  // fail
      }
    
      _tcxo_used = tcxo;   //en: which value the chip actually accepted (see the retry above)
      setCRC(2);
      explicitHeader();

      
    #ifdef LR2021_RX_BOOSTED_GAIN
      setRxBoostedGainMode(LR2021_RX_BOOSTED_GAIN);
    #endif

      return true;  // success
    }
    
    float getFreqMHz() const { return freqMHz; }

    //en: TCXO voltage that begin() succeeded with. std_init() retries with 0.0f when the
    //en: configured value returns -706/-707, so the two can differ - and a module with a
    //en: plain crystal only accepts 0.0f. Worth printing at boot: otherwise a silently
    //en: fallen-back board looks identical to one that never needed a TCXO.
    //sk: Napatie TCXO, s ktorym begin() preslo. std_init() pri -706/-707 skusi znova s
    //sk: 0.0f, takze sa to moze lisit - a modul s obycajnym krystalom vezme len 0.0f.
    //sk: Vyplati sa to vypisat pri boote: inak doska, ktora ticho spadla na fallback,
    //sk: vyzera rovnako ako tá, ktora TCXO nikdy nepotrebovala.
    float getTcxoUsed() const { return _tcxo_used; }

    bool getRxBoostedGainMode() const { return _rx_boosted; }

    int16_t startReceive() override {
      // include the PREAMBLE_DETECTED irq bit in reported flags
      return LR2021::startReceive(RADIOLIB_LR2021_RX_TIMEOUT_INF, RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED), RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
    }

    //en: Read the received length ourselves, keeping the status word.
    //en: A "get" on this family is two SPI transactions (LRxxxx::SPIcommand): send the
    //en: opcode, then read the reply. SPItransferStream() waits 1 us before polling
    //en: BUSY, so when BUSY has not risen yet the reply is read too early and the chip
    //en: answers with its default [stat 2B][irq 4B] stream instead. RadioLib strips the
    //en: status and hands the rest back as data, so getRxPktLength() returns irq[31:16]
    //en: - exactly 4 with RX_DONE set. Setting the status width to 0 keeps both status
    //en: bytes in our own buffer, the same trick LRxxxx::getIrqStatus uses.
    //sk: Precitaj prijatu dlzku sami a podrz si status slovo.
    //sk: Citanie ("get") je na tejto rodine dvojtransakcne (LRxxxx::SPIcommand): posli
    //sk: opcode, potom precitaj odpoved. SPItransferStream() pocka 1 us nez zacne polovat
    //sk: BUSY, takze ak BUSY nestuplo, odpoved sa cita priskoro a cip posle svoj default
    //sk: stream [stat 2B][irq 4B]. RadioLib status odstrihne a zvysok vrati ako data,
    //sk: takze getRxPktLength() vrati irq[31:16] - s nastavenym RX_DONE presne 4. Sirka
    //sk: statusu na 0 nam oba status bajty ponecha, rovnaky trik pouziva
    //sk: LRxxxx::getIrqStatus.
    int16_t readRxPktLenWithStatus(bool wait, uint8_t* stat, uint16_t* val) {
      int16_t st = mod->SPIwriteStream(RADIOLIB_LR2021_CMD_GET_RX_PKT_LENGTH, NULL, 0, wait, false);
      Module::BitWidth_t sw = mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_STATUS];
      Module::BitWidth_t cw = mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_CMD];
      mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_STATUS] = Module::BITS_0;
      mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_CMD]    = Module::BITS_0;
      uint8_t buff[4] = { 0 };
      st = mod->SPIreadStream(RADIOLIB_LRXXXX_CMD_NOP, buff, sizeof(buff), wait, false);
      mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_STATUS] = sw;
      mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_CMD]    = cw;
      if (stat) *stat = buff[0];
      if (val)  *val  = ((uint16_t)buff[2] << 8) | (uint16_t)buff[3];
      return st;
    }

    //en: Trust the length only when the chip says the reply is ours. The command status
    //en: field of stat1 has four values and CMD_DAT means "successfully processed, data
    //en: is being transmitted" - the only correct one for the read half of a get. A
    //en: reply produced by the race reports CMD_OK instead, i.e. "nothing to collect",
    //en: which is exactly the case where the status stream comes back. The Rx FIFO is
    //en: still intact at this point (readData() runs later), so re-reading recovers the
    //en: frame instead of losing it.
    //en: Measured on hardware: with the BUSY wait skipped on purpose in the live RX
    //en: path, 11 of 11 bogus reads reported CMD_OK and every frame was recovered; on
    //en: genuine frames whose length happened to equal irq[31:16] the status correctly
    //en: reported CMD_DAT. Judging by that value alone - as an earlier version did -
    //en: therefore misfires on real frames, which is why the status decides here.
    //sk: Dlzke ver len vtedy, ked cip povie, ze odpoved je nasa. Pole command status v
    //sk: stat1 ma styri hodnoty a CMD_DAT znamena "successfully processed, data is being
    //sk: transmitted" - jedina spravna pre citaciu polovicu get prikazu. Odpoved z
    //sk: pretecenia hlasi CMD_OK, teda "nic na vyzdvihnutie", a to je presne pripad, kedy
    //sk: sa vrati status stream. Rx FIFO je v tom momente jeste cele (readData() bezi az
    //sk: potom), takze opakovane citanie ramec zachrani namiesto straty.
    //sk: Odmerane na zeleze: s umyselne preskocenym cakanim na BUSY v zivej RX ceste
    //sk: hlasilo 11 z 11 chybnych citani CMD_OK a kazdy ramec sa zachranil; na
    //sk: hodnovernych ramcoch, ktorych dlzka sa nahodou rovnala irq[31:16], status
    //sk: spravne hlasil CMD_DAT. Rozhodovat len podla tej hodnoty - ako to robila
    //sk: predosla verzia - teda strieľa aj na dobrych ramcoch, a preto tu rozhoduje status.
    size_t getPacketLength(bool update = true) override {
#ifdef FK_RADIO_SPI_DIAG
      return fkDiagPktLen(update);
#else
      uint8_t  stat = 0;
      uint16_t val  = 0;
      for (int i = 0; i < 3; i++) {
        readRxPktLenWithStatus(true, &stat, &val);
        if ((stat & 0x0E) == RADIOLIB_LRXXXX_STAT_1_CMD_DAT) return val;
        _stale_pktlen_reads++;
      }
      //en: never end up worse than the plain library read
      //sk: nikdy neskonci horsie nez holym kniznicnym citanim
      return LR2021::getPacketLength(update);
#endif
    }

    //en: how many stale answers had to be re-read (0 = the race never hit)
    //sk: koľko zastaralých odpovedí sa muselo prečítať znova (0 = preteka nenastala)
    uint32_t getStalePktLenReads() const { return _stale_pktlen_reads; }


#ifdef FK_RADIO_SPI_DIAG
    //en: Ring buffer of guard events. NOTHING is printed from here - this sits in the
    //en: RX hot path and a Serial write would stall reception (the classic trap in this
    //en: codebase). 'fk spifix' dumps it later, from the CLI.
    //sk: Kruhovy buffer udalosti guardu. Odtialto sa NIC netlaci - sme v horucej RX
    //sk: ceste a zapis do Serialu by zastavil prijem (klasicka pasca tohto kodu).
    //sk: Vypise to az 'fk spifix' z CLI.
    struct FkSpiEvent {
      uint16_t fp;      //en: fingerprint = irq[31:16] before the read
      uint16_t first;   //en: value the first read returned
      uint16_t final;   //en: value finally used
      uint8_t  stat;    //en: stat1 of the read that produced 'first'
      uint8_t  off;     //en: unused on LR2021 (no offset in the reply), kept so the CLI is shared
      uint8_t  tries;   //en: how many reads it took
      uint8_t  rule;    //en: bit0 = CMD_DAT rule fired, bit1 = fingerprint rule fired
      uint8_t  inj;     //en: 1 = the BUSY wait was skipped on purpose
    };
    static const uint8_t FK_SPI_EVENTS = 8;
    FkSpiEvent _ev[FK_SPI_EVENTS];
    uint8_t _ev_count = 0, _ev_write = 0, _ev_total = 0;

    //en: A/B of the two rules on live traffic, on the real failing read.
    //en:  - CMD_DAT rule (proposed upstream): a read reply is ours only when stat1's
    //en:    command status says "data is being transmitted". Needs the status byte of
    //en:    the very read that produced the value, which is why the read is done here
    //en:    rather than through LR2021::getPacketLength().
    //en:  - fingerprint rule (what we ship): value == irq[31:16]. Cannot tell a real
    //en:    68 from a stale 68, so it can fire on a genuine frame.
    //en: Whatever happens, fall back to the library read so we never end up worse.
    //sk: A/B oboch pravidiel na zivej premavke, na tom skutocne chybnom citani.
    //sk:  - pravidlo CMD_DAT (navrhnute upstreamu): odpoved je nasa len ked command
    //sk:    status v stat1 hlasi "data is being transmitted". Potrebuje status bajt
    //sk:    prave toho citania, ktore hodnotu vyrobilo - preto sa cita tu a nie cez
    //sk:    LR2021::getPacketLength().
    //sk:  - pravidlo odtlacku (to, co posielame): hodnota == irq[31:16]. Nerozlisi
    //sk:    realnu 68 od zastaralej 68, takze moze vystrelit aj na dobrom ramci.
    //sk: V kazdom pripade sa nakoniec spadne na kniznicne citanie, aby sme na tom
    //sk: nikdy neboli horsie.
    //en: Experiment switches, settable at runtime from the CLI so the board does not
    //en: have to be reflashed between runs.
    //en:  _fk_inject  = skip the BUSY wait on every Nth length read (0 = off). Forces
    //en:                the failure in the real RX path, which is the only way to see
    //en:                whether the guard actually rescues a live frame.
    //en:  _fk_pretype = call getPacketType() first, reproducing the library's two
    //en:                back-to-back read commands. Tests whether that sequence is
    //en:                what triggers the race in the first place.
    //sk: Prepinace pokusu, nastavitelne za behu z CLI, aby sa doska nemusela medzi
    //sk: behmi reflashovat.
    //sk:  _fk_inject  = preskoc cakanie na BUSY pri kazdom n-tom citani dlzky (0 = vyp).
    //sk:                Vynuti chybu v realnej RX ceste - inak sa neda zistit, ci guard
    //sk:                zivy ramec naozaj zachrani.
    //sk:  _fk_pretype = zavolaj najprv getPacketType(), cim sa napodobnia kniznicne dva
    //sk:                citacie prikazy hned za sebou. Testuje, ci prave tato sekvencia
    //sk:                pretecenie spusta.
    uint16_t _fk_inject = 0, _fk_inject_cnt = 0;
    bool _fk_pretype = false;

    size_t fkDiagPktLen(bool update) {
      uint8_t  stat0 = 0, stat = 0, tries = 0;
      uint16_t fp = 0, val = 0, first = 0;
      bool cmd_flagged = false, fp_flagged = false;

      bool inject = false;
      if (_fk_inject && ++_fk_inject_cnt >= _fk_inject) { _fk_inject_cnt = 0; inject = true; }
#if RADIOLIB_GODMODE
      if (_fk_pretype) { uint8_t t = 0; (void)getPacketType(&t); }
#endif

      for (tries = 1; tries <= 4; tries++) {
        fp = (uint16_t)(getIrqStatus() >> 16);
        readRxPktLenWithStatus(inject && tries == 1 ? false : true, &stat, &val);
        if (tries == 1) { first = val; stat0 = stat; }
        if (fp != 0 && val == fp) fp_flagged = true;
        if (((stat >> 1) & 0x03) == 0x03) break;   //en: CMD_DAT -> reply belongs to us
        cmd_flagged = true;
      }

      size_t len = val;
      if (((stat >> 1) & 0x03) != 0x03) len = LR2021::getPacketLength(update);

      if (cmd_flagged || fp_flagged) {
        _stale_pktlen_reads++;
        _ev_total++;
        FkSpiEvent& e = _ev[_ev_write];
        e.fp = fp; e.first = first; e.final = (uint16_t)len;
        e.stat = stat0; e.off = 0; e.tries = tries > 4 ? 4 : tries;
        e.rule = (cmd_flagged ? 1 : 0) | (fp_flagged ? 2 : 0);
        e.inj = inject ? 1 : 0;
        _ev_write = (uint8_t)((_ev_write + 1) % FK_SPI_EVENTS);
        if (_ev_count < FK_SPI_EVENTS) _ev_count++;
      }
      return len;
    }
#endif

    bool isReceiving() {
      uint32_t irq = getIrqStatus();
      bool preamble = irq & RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED;  // bit 5
      bool header   = irq & RADIOLIB_LR2021_IRQ_LORA_HEADER_VALID;  // bit 6
      bool hdrErr   = irq & RADIOLIB_LR2021_IRQ_LORA_HDR_CRC_ERROR; // bit 9
      uint32_t now  = millis();
      if (hdrErr) {
        clearIrqFlags(RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED | RADIOLIB_LR2021_IRQ_LORA_HEADER_VALID | RADIOLIB_LR2021_IRQ_LORA_HDR_CRC_ERROR);
        _activityAt = 0;
        _headerSeen = false;
        return false;
      }
      if (!header && _headerSeen) {
        // something cleared the header flag, reset our state.
        _activityAt = 0; _headerSeen = false;
        return false;
      }

      if (header) {
        if (!_headerSeen) { _headerSeen = true; _activityAt = now; };
        if (now - _activityAt > _maxPayloadMillis) {
          MESH_DEBUG_PRINTLN("Clearing header IRQ after %ums", _maxPayloadMillis);
          clearIrqFlags(RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED | RADIOLIB_LR2021_IRQ_LORA_HEADER_VALID | RADIOLIB_LR2021_IRQ_LORA_HDR_CRC_ERROR);
          _activityAt = 0; _headerSeen = false;
          return false;
        }
        return true;
      }
      if (preamble) {
        if (_activityAt == 0) _activityAt = now;
        if (now - _activityAt > _preambleMillis) {
          clearIrqFlags(RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED);
          _activityAt = 0;
          MESH_DEBUG_PRINTLN("Clearing preamble IRQ after %ums", _preambleMillis);
          return false;
        }
        return true;
      }
      _activityAt = 0; _headerSeen = false;
      return false;
    }
    
    void setPreambleMillis(uint32_t preambleMillis) {
      _preambleMillis = preambleMillis;
      MESH_DEBUG_PRINTLN("Set _preambleMillis=%u", _preambleMillis);
    }
    void setMaxPayloadMillis(uint32_t payloadMillis) {
      _maxPayloadMillis = payloadMillis;
      MESH_DEBUG_PRINTLN("Set _maxPayloadMillis=%u", _maxPayloadMillis);
    }


    uint8_t getSpreadingFactor() const { return spreadingFactor; }
};
#pragma once

#include <RadioLib.h>
#include "MeshCore.h"

class CustomLR2021 : public LR2021 {
  uint32_t _preambleMillis = 66;
  uint32_t _maxPayloadMillis = 3934;
  uint32_t _activityAt = 0;
  bool _headerSeen = false;
  bool _rx_boosted = false;
  uint32_t _stale_pktlen_reads = 0;

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
    
      setCRC(2);
      explicitHeader();

      
    #ifdef LR2021_RX_BOOSTED_GAIN
      setRxBoostedGainMode(LR2021_RX_BOOSTED_GAIN);
    #endif

      return true;  // success
    }
    
    float getFreqMHz() const { return freqMHz; }

    bool getRxBoostedGainMode() const { return _rx_boosted; }

    int16_t startReceive() override {
      // include the PREAMBLE_DETECTED irq bit in reported flags
      return LR2021::startReceive(RADIOLIB_LR2021_RX_TIMEOUT_INF, RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED), RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
    }

    //en: Guard against a stale SPI response corrupting the received length.
    //en: A "get" on this chip family is two transactions (LRxxxx::SPIcommand): send
    //en: the opcode, then read the answer. Module::SPItransferStream() waits 1 us and
    //en: then polls BUSY - if BUSY has not risen yet the wait is skipped and the read
    //en: comes too early. The chip then answers with its default [stat 2B][irq 4B]
    //en: stream, and getRxPktLength() blindly parses the first two payload bytes, i.e.
    //en: irq[31:16]. With RX_DONE (bit 18) set that is exactly 4, so a 50 or 133 byte
    //en: frame was reported as len=4; readData() read 4 bytes, its clearRxFifo() threw
    //en: the rest away and tryParsePacket() rejected the frame - a silent loss. Worse,
    //en: irq[31:16] can also be a plausible length (12 with TX_DONE, 68 with CRC_ERROR),
    //en: which passes unnoticed as a garbage packet.
    //en: The Rx FIFO is still intact here (readData() runs later), so re-reading the
    //en: length recovers the frame. getIrqStatus() cannot suffer the same race - it IS
    //en: that default stream (see LRxxxx::getIrqStatus) - which makes irq[31:16] an
    //en: exact fingerprint of a stale answer. A genuine frame whose length happens to
    //en: match only costs a few extra reads and is returned unchanged.
    //sk: Ochrana pred zastaralou SPI odpovedou, ktorá pokazí prijatú dĺžku.
    //sk: Čítanie ("get") je na tejto rodine čipov dvojtransakčné (LRxxxx::SPIcommand):
    //sk: pošli opcode, potom prečítaj odpoveď. Module::SPItransferStream() počká 1 us a
    //sk: potom poluje na BUSY - ak BUSY ešte nestúplo, čakanie sa preskočí a čítanie
    //sk: príde priskoro. Čip vtedy odpovie svojím default streamom [stat 2B][irq 4B] a
    //sk: getRxPktLength() slepo rozparsuje prvé dva bajty, teda irq[31:16]. S nastaveným
    //sk: RX_DONE (bit 18) je to presne 4, takže 50 alebo 133 bajtový rámec sa ohlásil ako
    //sk: len=4; readData() prečítal 4 bajty, jeho clearRxFifo() zvyšok zahodil a
    //sk: tryParsePacket() rámec odmietol - tichá strata. Horšie, irq[31:16] môže dať aj
    //sk: hodnovernú dĺžku (12 s TX_DONE, 68 s CRC_ERROR) a prejde nepovšimnuté ako smeť.
    //sk: Rx FIFO je tu ešte celé (readData() beží až potom), takže opakované čítanie
    //sk: dĺžky rámec zachráni. getIrqStatus() tou istou pretekou trpieť nemôže - ono samo
    //sk: JE ten default stream (viď LRxxxx::getIrqStatus) - a preto je irq[31:16] presný
    //sk: odtlačok zastaralej odpovede. Skutočný rámec, ktorého dĺžka sa náhodou zhoduje,
    //sk: stojí len pár čítaní navyše a vráti sa nezmenený.
    size_t getPacketLength(bool update = true) override {
      size_t len = 0;
      for (int i = 0; i < 4; i++) {
        uint16_t stale = (uint16_t)(getIrqStatus() >> 16);
        len = LR2021::getPacketLength(update);
        //en: stale == 0 means there is nothing to confuse the length with, so a zero
        //en: length is a genuine "no packet" answer - do not waste reads on it.
        //sk: stale == 0 znamena, ze dlzku nie je s cim zamenit, teda nulova dlzka je
        //sk: skutocne "ziadny paket" - necitaj to znova zbytocne.
        if (stale == 0 || len != stale) break;
        _stale_pktlen_reads++;
      }
      return len;
    }

    //en: how many stale answers had to be re-read (0 = the race never hit)
    //sk: koľko zastaralých odpovedí sa muselo prečítať znova (0 = preteka nenastala)
    uint32_t getStalePktLenReads() const { return _stale_pktlen_reads; }

#ifdef FK_LR2021_SPI_DIAG
    //en: Diagnostics for the stale-reply race (see getPacketLength above). Reads
    //en: GetRxPktLength the way LRxxxx::SPIcommand does - two transactions - but keeps
    //en: the status word, and can deliberately skip the BUSY wait to provoke the early
    //en: read. Both status bytes stay visible by setting the status width to 0, exactly
    //en: how LRxxxx::getIrqStatus reads the default stream.
    //en: stat bits 3:1 = command status: 0 FAIL, 1 PERR, 2 OK, 3 DAT ("data is being
    //en: transmitted"). If a stale reply reports OK rather than DAT, the driver could
    //en: reject it from the status alone - which is the fix proposed upstream.
    //sk: Diagnostika pretecenej odpovede (viď getPacketLength vyššie). Číta
    //sk: GetRxPktLength tak, ako to robí LRxxxx::SPIcommand - dvoma transakciami - ale
    //sk: podrží si status slovo a vie úmyselne preskočiť čakanie na BUSY, aby predčasné
    //sk: čítanie vyprovokovalo. Oba status bajty ostanú viditeľné tým, že sa šírka
    //sk: statusu nastaví na 0 - presne ako číta default stream LRxxxx::getIrqStatus.
    //sk: stat bity 3:1 = command status: 0 FAIL, 1 PERR, 2 OK, 3 DAT ("data is being
    //sk: transmitted"). Ak zastaralá odpoveď hlási OK a nie DAT, driver ju vie odmietnuť
    //sk: už zo statusu - a to je oprava navrhnutá upstreamu.
    int16_t fkRawPktLen(bool wait, uint8_t* stat, uint16_t* val) {
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
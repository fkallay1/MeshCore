#pragma once

#include <RadioLib.h>
#include "MeshCore.h"

//en: This class carries the stale-reply guard, so the heartbeat can print its counter
//en: without asking which chip is fitted. CustomLR2021 defines the same flag.
//sk: Tato trieda ma guard pretecenej odpovede, takze heartbeat vie vypisat jeho
//sk: pocitadlo bez toho, aby sa pytal, ktory cip je na doske. CustomLR2021 definuje
//sk: ten isty flag.
#ifndef FK_RADIO_HAS_STALE_GUARD
#define FK_RADIO_HAS_STALE_GUARD 1
#endif

class CustomLR1110 : public LR1110 {
  uint32_t _preambleMillis = 66;
  uint32_t _maxPayloadMillis = 3934;
  uint32_t _activityAt = 0;
  bool _headerSeen = false;
  bool _rx_boosted = false;
  uint32_t _stale_pktlen_reads = 0;

  public:
    CustomLR1110(Module *mod) : LR1110(mod) { }

    //en: Read the receive buffer status ourselves, keeping the status word. Mirrors
    //en: CustomLR2021::readRxPktLenWithStatus - see the reasoning there. The reply here
    //en: is [stat 2B][len 1B][offset 1B], so a stale one yields irq[31:24] as the length
    //en: and irq[23:16] as the offset; that offset is what shifts the payload.
    //sk: Precitaj stav prijimacieho buffra sami a podrz si status slovo. Zrkadli to
    //sk: CustomLR2021::readRxPktLenWithStatus - odovodnenie je tam. Odpoved ma tu tvar
    //sk: [stat 2B][len 1B][offset 1B], takze zastarala da ako dlzku irq[31:24] a ako
    //sk: offset irq[23:16]; prave ten offset posuva payload.
    int16_t readRxPktLenWithStatus(bool wait, uint8_t* stat, uint16_t* val, uint8_t* off = NULL) {
      int16_t st = mod->SPIwriteStream(RADIOLIB_LR11X0_CMD_GET_RX_BUFFER_STATUS, NULL, 0, wait, false);
      Module::BitWidth_t sw = mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_STATUS];
      Module::BitWidth_t cw = mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_CMD];
      mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_STATUS] = Module::BITS_0;
      mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_CMD]    = Module::BITS_0;
      uint8_t buff[4] = { 0 };
      st = mod->SPIreadStream(RADIOLIB_LRXXXX_CMD_NOP, buff, sizeof(buff), wait, false);
      mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_STATUS] = sw;
      mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_CMD]    = cw;
      if (stat) *stat = buff[0];
      if (val)  *val  = buff[2];
      if (off)  *off  = buff[3];
      return st;
    }

    //en: how many stale replies had to be read again (0 = the race never hit)
    //sk: kolko zastaralych odpovedi sa muselo precitat znova (0 = preteka nenastala)
    uint32_t getStalePktLenReads() const { return _stale_pktlen_reads; }

    //en: Trust the length only when stat1 says CMD_DAT, i.e. that a reply really is
    //en: being sent. Measured on LR2021 hardware: a stale reply always reported CMD_OK
    //en: and a genuine one CMD_DAT, so the status decides rather than the value - the
    //en: value alone cannot tell a real length from a coincidental one.
    //sk: Dlzke ver len ked stat1 hlasi CMD_DAT, teda ze sa odpoved naozaj posiela.
    //sk: Odmerane na LR2021: zastarala odpoved vzdy hlasila CMD_OK a skutocna CMD_DAT,
    //sk: takze rozhoduje status a nie hodnota - tá sama nerozlisi skutocnu dlzku od
    //sk: nahodnej zhody.
    size_t getPacketLength(bool update) override {
#ifdef FK_RADIO_SPI_DIAG
      size_t len = fkDiagPktLen(update);
#else
      uint8_t  stat = 0;
      uint16_t val  = 0;
      size_t   len  = 0;
      for (int i = 0; i < 3; i++) {
        readRxPktLenWithStatus(true, &stat, &val);
        len = val;
        if ((stat & 0x0E) == RADIOLIB_LRXXXX_STAT_1_CMD_DAT) break;
        _stale_pktlen_reads++;
        if (i == 2) len = LR1110::getPacketLength(update);   //en: fall back to the library read
      }
#endif
      if (len == 0 && getIrqStatus() & RADIOLIB_LR11X0_IRQ_HEADER_ERR) {
        // we've just received a corrupted packet
        // this may have triggered a bug causing subsequent packets to be shifted
        // call standby() to return radio to known-good state
        // recvRaw will call startReceive() to restart rx
        MESH_DEBUG_PRINTLN("LR1110: got header err, calling standby()");
        standby();
      }
      return len;
    }
    
    float getFreqMHz() const { return freqMHz; }

    int16_t setRxBoostedGainMode(bool en) {
      _rx_boosted = en;
      return LR1110::setRxBoostedGainMode(en);
    }

    bool getRxBoostedGainMode() const { return _rx_boosted; }

    int16_t startReceive() override {
      // include the PREAMBLE_DETECTED irq bit in reported flags.
      return LR1110::startReceive(RADIOLIB_LR11X0_RX_TIMEOUT_INF, RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED), RADIOLIB_IRQ_RX_DEFAULT_MASK, 0);
    }

#ifdef FK_RADIO_SPI_DIAG
    //en: Same diagnostic as CustomLR2021 - identical member names on purpose, so the
    //en: shared 'fk stale|spifix|inject|pretype' CLI in MyMesh works for both chips.
    //en: NOTHING is printed from here; we are in the RX hot path.
    //sk: Ta ista diagnostika ako v CustomLR2021 - nazvy clenov su zamerne rovnake, aby
    //sk: zdielane CLI 'fk stale|spifix|inject|pretype' v MyMesh fungovalo pre oba cipy.
    //sk: Odtialto sa NIC netlaci, sme v horucej RX ceste.
    struct FkSpiEvent {
      uint16_t fp;      //en: fingerprint = irq[31:24] before the read
      uint32_t irq;     //en: whole IRQ word at that moment - says WHY the read happened
      uint16_t first;   //en: value the first read returned
      uint16_t final;   //en: value finally used
      uint8_t  stat;    //en: stat1 of the read that produced 'first'
      uint8_t  off;     //en: buffer offset the same read returned
      uint8_t  tries;
      uint8_t  rule;    //en: bit0 = CMD_DAT rule fired, bit1 = fingerprint rule fired
      uint8_t  inj;     //en: 1 = the BUSY wait was skipped on purpose
    };
    static const uint8_t FK_SPI_EVENTS = 8;
    FkSpiEvent _ev[FK_SPI_EVENTS];
    uint8_t _ev_count = 0, _ev_write = 0, _ev_total = 0;
    uint16_t _fk_inject = 0, _fk_inject_cnt = 0;
    bool _fk_pretype = false;

    size_t fkDiagPktLen(bool update) {
      uint8_t  stat0 = 0, stat = 0, off = 0, off0 = 0, tries = 0;
      uint16_t fp = 0, val = 0, first = 0;
      uint32_t irqw = 0, irq0 = 0;
      bool cmd_flagged = false, fp_flagged = false;

      bool inject = false;
      if (_fk_inject && ++_fk_inject_cnt >= _fk_inject) { _fk_inject_cnt = 0; inject = true; }
#if RADIOLIB_GODMODE
      if (_fk_pretype) { uint8_t t = 0; (void)getPacketType(&t); }
#endif
      for (tries = 1; tries <= 4; tries++) {
        irqw = getIrqStatus();
        fp = (uint16_t)(irqw >> 24);
        readRxPktLenWithStatus(inject && tries == 1 ? false : true, &stat, &val, &off);
        if (tries == 1) { first = val; stat0 = stat; off0 = off; irq0 = irqw; }
        if (val == fp && (irqw & RADIOLIB_LR11X0_IRQ_RX_DONE)) fp_flagged = true;
        if ((stat & 0x0E) == RADIOLIB_LRXXXX_STAT_1_CMD_DAT) break;
        cmd_flagged = true;
      }

      size_t len = val;
      if ((stat & 0x0E) != RADIOLIB_LRXXXX_STAT_1_CMD_DAT) len = LR1110::getPacketLength(update);

      //en: PROBE: a length of 0 while RX_DONE is set, with the status saying the reply is
      //en: genuine (CMD_DAT), is a state this chip reaches routinely - unlike LR2021.
      //en: Two readings are possible: either nothing is waiting, or the length register
      //en: has not been updated yet at the moment RX_DONE fires. Only a re-read can tell
      //en: them apart, so do it here and record how many it took. If a re-read ever
      //en: returns non-zero, the retry rule is right and the status check alone is not
      //en: enough on this family.
      //sk: SONDA: dlzka 0 pri nastavenom RX_DONE, ked status hovori, ze odpoved je
      //sk: platna (CMD_DAT), je stav, do ktoreho sa tento cip dostava bezne - na rozdiel
      //sk: od LR2021. Su dva vyklady: alebo naozaj nic neceka, alebo sa register dlzky v
      //sk: momente RX_DONE este nedopisal. Rozlisi to len opakovane citanie, takze ho tu
      //sk: sprav a zaznamenaj, kolko pokusov trvalo. Ak niekdy vrati nenulu, pravidlo s
      //sk: opakovanim je spravne a kontrola statusu sama na tejto rodine nestaci.
      uint8_t probe = 0;
      if (len == 0 && (irqw & RADIOLIB_LR11X0_IRQ_RX_DONE)) {
        for (probe = 1; probe <= 3; probe++) {
          uint8_t st2 = 0, of2 = 0;
          uint16_t v2 = 0;
          readRxPktLenWithStatus(true, &st2, &v2, &of2);
          if (v2) { len = v2; off = of2; break; }
        }
        if (len == 0) probe = 0;   //en: nothing came out of it
      }

      if (cmd_flagged || fp_flagged) {
        _stale_pktlen_reads++;
        _ev_total++;
        FkSpiEvent& e = _ev[_ev_write];
        e.fp = fp; e.irq = irq0; e.first = first; e.final = (uint16_t)len;
        e.stat = stat0; e.off = off0;
        //en: probe>0 = a re-read DID produce a length after a zero; tries stays the CMD_DAT count
        e.tries = probe ? (uint8_t)(100 + probe) : (tries > 4 ? 4 : tries);
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
      bool preamble = irq & RADIOLIB_LR11X0_IRQ_PREAMBLE_DETECTED;      // bit 4
      bool header   = irq & RADIOLIB_LR11X0_IRQ_SYNC_WORD_HEADER_VALID; // bit 5
      bool hdrErr   = irq & RADIOLIB_LR11X0_IRQ_HEADER_ERR;             // bit 6
      uint32_t now  = millis();
      if (hdrErr) {
        clearIrqState(RADIOLIB_LR11X0_IRQ_PREAMBLE_DETECTED | RADIOLIB_LR11X0_IRQ_SYNC_WORD_HEADER_VALID | RADIOLIB_LR11X0_IRQ_HEADER_ERR);
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
          clearIrqState(RADIOLIB_LR11X0_IRQ_PREAMBLE_DETECTED | RADIOLIB_LR11X0_IRQ_SYNC_WORD_HEADER_VALID | RADIOLIB_LR11X0_IRQ_HEADER_ERR);
          _activityAt = 0; _headerSeen = false;
          return false;
        }
        return true;
      }
      if (preamble) {
        if (_activityAt == 0) _activityAt = now;
        if (now - _activityAt > _preambleMillis) {
          clearIrqState(RADIOLIB_LR11X0_IRQ_PREAMBLE_DETECTED);
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

#pragma once

#include <RadioLib.h>
#include "MeshCore.h"

class CustomLR1110 : public LR1110 {
  uint32_t _preambleMillis = 66;
  uint32_t _maxPayloadMillis = 3934;
  uint32_t _activityAt = 0;
  bool _headerSeen = false;
  bool _rx_boosted = false;

  public:
    CustomLR1110(Module *mod) : LR1110(mod) { }

    size_t getPacketLength(bool update) override {
      size_t len = LR1110::getPacketLength(update);
      //en: Guard against a stale SPI reply being parsed as the received length.
      //en: A "get" is two transactions (LRxxxx::SPIcommand): send the opcode, then read
      //en: the reply. Module::SPItransferStream() waits 1 us and then polls BUSY, so if
      //en: BUSY has not risen yet the wait is skipped and the reply is read too early.
      //en: The chip answers with its default [stat 2B][irq 4B] stream instead, and
      //en: getRxBufferStatus() takes the length from the first payload byte, i.e.
      //en: irq[31:24] - zero for every RX-relevant flag, since those all live in the
      //en: two low bytes. The Rx buffer is still intact at this point (readData() runs
      //en: later), so reading the length again recovers the packet instead of dropping
      //en: it. getIrqStatus() cannot be hit by the same race: it IS that default stream
      //en: (see LRxxxx::getIrqStatus), which makes irq[31:24] an exact fingerprint.
      //en: RX_DONE has to be checked too, because a zero sentinel is also the honest
      //en: answer when no packet is waiting.
      //sk: Ochrana pred tym, aby sa zastarala SPI odpoved rozparsovala ako dlzka.
      //sk: Citanie ("get") je dvojtransakcne (LRxxxx::SPIcommand): posli opcode, potom
      //sk: precitaj odpoved. Module::SPItransferStream() pocka 1 us a potom poluje na
      //sk: BUSY, takze ak BUSY nestuplo, cakanie sa preskoci a odpoved sa cita priskoro.
      //sk: Cip vtedy posle svoj default stream [stat 2B][irq 4B] a getRxBufferStatus()
      //sk: vezme dlzku z prveho bajtu payloadu, teda irq[31:24] - nula pre kazdy RX
      //sk: priznak, lebo tie vsetky sedia v dvoch dolnych bajtoch. Rx buffer je v tomto
      //sk: momente jeste cely (readData() bezi az potom), takze opakovane citanie dlzky
      //sk: paket zachrani namiesto zahodenia. getIrqStatus() tou istou pretekou trpiet
      //sk: nemoze - ono samo JE ten default stream (vid LRxxxx::getIrqStatus), preto je
      //sk: irq[31:24] presny odtlacok. RX_DONE treba overit tiez, lebo nulovy sentinel
      //sk: je aj cestna odpoved vtedy, ked ziadny paket neceka.
      uint32_t irq = getIrqStatus();
      for (int i = 0; i < 3 && len == (size_t)(irq >> 24)
                            && (irq & RADIOLIB_LR11X0_IRQ_RX_DONE); i++) {
        len = LR1110::getPacketLength(update);
        irq = getIrqStatus();
      }
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

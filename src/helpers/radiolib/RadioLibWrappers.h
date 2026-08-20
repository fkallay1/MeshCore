#pragma once

#include <Mesh.h>
#include <RadioLib.h>

#ifdef USE_CC310_HW_CRYPTO
#include <Adafruit_nRFCrypto.h>
#endif
struct PacketMillis {
  uint32_t preambleMillis;  // preamble-detect -> header-valid deadline
  uint32_t payloadMillis;   // header-valid   -> rx-done deadline
};

class RadioLibWrapper : public mesh::Radio {
protected:
  PhysicalLayer* _radio;
  mesh::MainBoard* _board;
  uint32_t n_recv, n_sent, n_recv_errors;
  int16_t _noise_floor, _threshold;
  bool _cad_enabled;
  uint16_t _num_floor_samples;
  int32_t _floor_sample_sum;
  uint8_t _preamble_sf;
#ifdef FKPR_RADIO_WATCHDOG
  //en: passive liveness stats, filled from the noise-floor sampler in loop() -
  //en: that sampler already reads getCurrentRSSI() ~32x per second, so there is
  //en: no reason for the watchdog to run its own blocking burst.
  //sk: pasivna statistika zivota, plni ju vzorkovac noise-floor v loop() -
  //sk: ten uz cita getCurrentRSSI() ~32x za sekundu, takze watchdog nema dovod
  //sk: robit vlastnu blokujucu davku.
  int16_t  _wd_rssi_min, _wd_rssi_max;
  uint32_t _wd_samples;
#endif

  void idle();
  void startRecv();
  float packetScoreInt(float snr, int sf, int packet_len);
  virtual bool isReceivingPacket() =0;
  virtual void doResetAGC();

public:
  RadioLibWrapper(PhysicalLayer& radio, mesh::MainBoard& board) : _radio(&radio), _board(&board), _preamble_sf(0) { n_recv = n_sent = 0; }

  void begin() override;
  virtual void powerOff() { _radio->sleep(); }
  int recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int len_bytes) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override;
  bool isChannelActive();

  bool isReceiving() override {
    if (isReceivingPacket()) return true;

    return isChannelActive();
  }

  virtual void setParams(float freq, float bw, uint8_t sf, uint8_t cr) = 0;
  uint32_t getRngSeed();
  void setTxPower(int8_t dbm);


  virtual float getCurrentRSSI() =0;

  //en: Is the chip still answering on SPI? Default test is RSSI plausibility,
  //en: which works where the radio reports a wide RSSI range (LR2021 returns
  //en: -255 dBm once its supply is gone). Radios whose RSSI cannot express an
  //en: impossible value - SX126x is one - override this with something better.
  virtual bool isChipResponding() { return getCurrentRSSI() > -150.0f; }
  virtual uint8_t getSpreadingFactor() const { return LORA_SF; }
  static uint16_t preambleLengthForSF(uint8_t sf) { return sf <= 8 ? 32 : 16; }
  void updatePreamble(uint8_t sf) { _preamble_sf = sf; _radio->setPreambleLength(preambleLengthForSF(sf)); }
  PacketMillis calcMaxPacketMillis(uint8_t sf, float bw, uint8_t cr, uint8_t preambleSymbols);
  virtual int16_t performChannelScan();

  int getNoiseFloor() const override { return _noise_floor; }
#ifdef FKPR_RADIO_WATCHDOG
  //en: read the accumulated window and start a fresh one. 'samples' is the more
  //en: reliable signal of the two: it only advances while the chip is actually
  //en: armed in Rx, so a stalled receiver shows up even on a dead-quiet channel.
  //sk: precitaj nazbierane okno a zacni nove. 'samples' je spolahlivejsi z tych
  //sk: dvoch: rastie len kym je cip naozaj v RX, takze zaseknuty prijimac sa
  //sk: prejavi aj na uplne tichom kanali.
  void takeRssiWindow(int* out_min, int* out_max, uint32_t* out_samples) {
    *out_samples = _wd_samples;
    *out_min = _wd_samples ? _wd_rssi_min : 0;
    *out_max = _wd_samples ? _wd_rssi_max : 0;
    _wd_rssi_min = 32767; _wd_rssi_max = -32768; _wd_samples = 0;
  }
#endif
  void triggerNoiseFloorCalibrate(int threshold) override;
  void setCADEnabled(bool enable) override { _cad_enabled = enable; }
  void resetAGC() override;

  void loop() override;

  uint32_t getPacketsRecv() const { return n_recv; }
  uint32_t getPacketsRecvErrors() const { return n_recv_errors; }
  uint32_t getPacketsSent() const { return n_sent; }
#ifdef FK_DEBUG
  // [FK_DEBUG] ISR udalosti (RxDone+TxDone). missed ≈ getIsrEvents()-n_sent-n_recv-n_recv_errors
  uint32_t getIsrEvents() const;
#endif
  void resetStats() { n_recv = n_sent = n_recv_errors = 0; }

  virtual float getLastRSSI() const override;
  virtual float getLastSNR() const override;

  float packetScore(float snr, int packet_len) override { return packetScoreInt(snr, 10, packet_len); }  // assume sf=10

  virtual bool setRxBoostedGainMode(bool) { return false; }
  virtual bool getRxBoostedGainMode() const { return false; }
  
  virtual bool configSideDetectors(const uint8_t sideDetSFs[], uint8_t num, float bw) { return false; }
};

/**
 * \brief  an RNG impl using the noise from the LoRa radio as entropy.
 *         NOTE: this is VERY SLOW!  Use only for things like creating new LocalIdentity
*/
class RadioNoiseListener : public mesh::RNG {
  PhysicalLayer* _radio;
public:
  RadioNoiseListener(PhysicalLayer& radio): _radio(&radio) { }

  void random(uint8_t* dest, size_t sz) override {
#ifdef USE_CC310_HW_CRYPTO
    nRFCrypto.Random.generate(dest, (uint16_t)sz);
    for (int i = 0; i < sz; i++) {
      dest[i] ^= _radio->randomByte() ^ (::random(0, 256) & 0xFF); // combine with Radio's entropy
    }
#else
    for (int i = 0; i < sz; i++) {
      dest[i] = _radio->randomByte() ^ (::random(0, 256) & 0xFF);
    }
#endif
  }
};

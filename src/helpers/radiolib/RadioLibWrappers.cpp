
#define RADIOLIB_STATIC_ONLY 1
#include "RadioLibWrappers.h"

#define STATE_IDLE       0
#define STATE_RX         1
#define STATE_TX_WAIT    3
#define STATE_TX_DONE    4
#define STATE_INT_READY 16

#define NUM_NOISE_FLOOR_SAMPLES  64
#define SAMPLING_THRESHOLD  14

static volatile uint8_t state = STATE_IDLE;

#ifdef FK_DEBUG
// [FK_DEBUG] Počítadlo ISR udalostí (RxDone + TxDone). state |= INT_READY je len
// bit, takže ak dorazia dva pakety pred prečítaním, druhý prepíše FIFO a strata
// je ticho. Tento counter pripočíta KAŽDÉ IRQ → derivovaný odhad zahodených:
//   missed ≈ isr_event_count - n_sent(TX) - n_recv - n_recv_errors
static volatile uint32_t isr_event_count = 0;
#endif

// this function is called when a complete packet
// is transmitted by the module
static
#if defined(ESP8266) || defined(ESP32)
  ICACHE_RAM_ATTR
#endif
void setFlag(void) {
  // we sent a packet, set the flag
#ifdef FK_DEBUG
  isr_event_count++;
#endif
  state |= STATE_INT_READY;
}

void RadioLibWrapper::begin() {
  _radio->setPacketReceivedAction(setFlag);  // this is also SentComplete interrupt
  _preamble_sf = getSpreadingFactor();
  _radio->setPreambleLength(preambleLengthForSF(_preamble_sf)); // longer preamble for lower SF improves reliability
  state = STATE_IDLE;

  if (_board->getStartupReason() == BD_STARTUP_RX_PACKET) {  // received a LoRa packet (while in deep sleep)
    setFlag(); // LoRa packet is already received
  }

  _noise_floor = 0;
  _threshold = 0;
  _cad_enabled = false;

  // start average out some samples
  _num_floor_samples = 0;
  _floor_sample_sum = 0;
#ifdef FKPR_RADIO_WATCHDOG
  _wd_rssi_min = 32767; _wd_rssi_max = -32768; _wd_samples = 0;
#endif
}

uint32_t RadioLibWrapper::getRngSeed() {
  return _radio->random(0x7FFFFFFF);
}

#ifdef FK_DEBUG
uint32_t RadioLibWrapper::getIsrEvents() const { return isr_event_count; }
#endif

void RadioLibWrapper::setTxPower(int8_t dbm) {
#if defined(USE_LR2021)
  //en: LR2021 only: setOutputPower() writes PA config + TxParams, which are
  //en: standby-only commands. This wrapper deliberately keeps state == STATE_RX
  //en: after readData ("LR2021 stays in Rx", see recvRaw), so unlike the SX126x
  //en: path nothing ever calls startReceive() again on its own. Writing PA config
  //en: from Rx could therefore leave the receiver down for good - a 'set tx' while
  //en: listening made the radio deaf until reboot. Drop to standby first and let
  //en: checkRecv() re-arm Rx, the same way resetAGC() and applySideDetectorConfig()
  //en: already do. Dispatcher's stuck-radio check cannot catch this: it reads
  //en: isInRecvMode(), which is our own state flag, not the chip.
  //sk: Len LR2021: setOutputPower() zapisuje PA config a TxParams, čo sú príkazy
  //sk: platné len v standby. Tento wrapper po readData zámerne drží
  //sk: state == STATE_RX („LR2021 stays in Rx", viď recvRaw), takže na rozdiel od
  //sk: SX126x cesty už nikto sám od seba nezavolá startReceive(). Zápis PA configu
  //sk: počas Rx tak mohol zhodiť prijímač natrvalo - „set tx" počas počúvania
  //sk: spravil z rádia hluchú dosku až do rebootu. Najprv teda standby a RX nech
  //sk: znova nahodí checkRecv(), rovnako ako to už robí resetAGC() aj
  //sk: applySideDetectorConfig(). Kontrola zaseknutého rádia v Dispatcheri to
  //sk: nezachytí: číta isInRecvMode(), čo je náš vlastný príznak, nie stav čipu.
  idle();
#endif
  _radio->setOutputPower(dbm);
}

void RadioLibWrapper::idle() {
  _radio->standby();
  state = STATE_IDLE;   // need another startReceive()
}

void RadioLibWrapper::triggerNoiseFloorCalibrate(int threshold) {
  _threshold = threshold;
  if (_num_floor_samples >= NUM_NOISE_FLOOR_SAMPLES) {  // ignore trigger if currently sampling
    _num_floor_samples = 0;
    _floor_sample_sum = 0;
  }
}

void RadioLibWrapper::doResetAGC() {
  _radio->sleep();  // warm sleep to reset analog frontend
}

void RadioLibWrapper::resetAGC() {
  // make sure we're not mid-receive of packet!
  if ((state & STATE_INT_READY) != 0 || isReceivingPacket()) return;

  doResetAGC();
  state = STATE_IDLE;   // trigger a startReceive()

  // Reset noise floor sampling so it reconverges from scratch.
  // Without this, a stuck _noise_floor of -120 makes the sampling threshold
  // too low (-106) to accept normal samples (~-105), self-reinforcing the
  // stuck value even after the receiver has recovered.
  _noise_floor = 0;
  _num_floor_samples = 0;
  _floor_sample_sum = 0;
}

void RadioLibWrapper::loop() {
  if (state == STATE_RX && _num_floor_samples < NUM_NOISE_FLOOR_SAMPLES) {
    if (!isReceivingPacket()) {
      int rssi = getCurrentRSSI();
#ifdef FKPR_RADIO_WATCHDOG
      //en: record BEFORE the threshold filter below - that filter drops the upper
      //en: half of the spread, and the spread is the whole point of this test.
      //sk: zaznamenaj PRED prahovym filtrom nizsie - ten odreze hornu polovicu
      //sk: rozptylu, a prave rozptyl je zmyslom tohto testu.
      if (rssi < _wd_rssi_min) _wd_rssi_min = rssi;
      if (rssi > _wd_rssi_max) _wd_rssi_max = rssi;
      _wd_samples++;
#endif
      if (rssi < _noise_floor + SAMPLING_THRESHOLD) {  // only consider samples below current floor + sampling THRESHOLD
        _num_floor_samples++;
        _floor_sample_sum += rssi;
      }
    }
  } else if (_num_floor_samples >= NUM_NOISE_FLOOR_SAMPLES && _floor_sample_sum != 0) {
    _noise_floor = _floor_sample_sum / NUM_NOISE_FLOOR_SAMPLES;
    if (_noise_floor < -120) {
      _noise_floor = -120;    // clamp to lower bound of -120dBi
    }
    _floor_sample_sum = 0;

    #ifdef MESH_DEBUG_NOISE_FLOOR
    MESH_DEBUG_PRINTLN("RadioLibWrapper: noise_floor = %d", (int)_noise_floor);
    #endif
  }
}

void RadioLibWrapper::startRecv() {
  #if defined(USE_LR2021)
  _radio->standby(); // without this LR2021 can throw -706 when calling startReceive after hardware CAD when side detectors are enabled
  #endif
  int err = _radio->startReceive();
  if (err == RADIOLIB_ERR_NONE) {
    state = STATE_RX;
  } else {
    MESH_DEBUG_PRINTLN("RadioLibWrapper: error: startReceive(%d)", err);
  }
}

bool RadioLibWrapper::isInRecvMode() const {
  return (state & ~STATE_INT_READY) == STATE_RX;
}

int RadioLibWrapper::recvRaw(uint8_t* bytes, int sz) {
  int len = 0;
  if (state & STATE_INT_READY) {
    len = _radio->getPacketLength();
    if (len > 0) {
      if (len > sz) { len = sz; }
      int err = _radio->readData(bytes, len);
      if (err != RADIOLIB_ERR_NONE) {
        MESH_DEBUG_PRINTLN("RadioLibWrapper: error: readData(%d)", err);
      #ifdef FK_DEBUG
        Serial.print(F("[RADIO] readData ERR=")); Serial.print(err);
        Serial.print(F(" len=")); Serial.print(len);
        if (err == RADIOLIB_ERR_CRC_MISMATCH) Serial.print(F(" (CRC_MISMATCH)"));
        Serial.println();
      #endif
        len = 0;
        n_recv_errors++;
      } else {
      //  Serial.print("  readData() -> "); Serial.println(len);
        n_recv++;
      }
    }
    #if defined(USE_LR2021)
    //en: Only claim we are still in Rx when a packet was actually read. The TX-done
    //en: interrupt sets the same STATE_INT_READY bit, so after forwarding a packet this
    //en: block runs with nothing to read: getPacketLength() returns leftovers, readData()
    //en: fails with -24, and pinning STATE_RX here told the wrapper it was receiving while
    //en: the chip sat in STDBY_RC after the transmit. Nothing re-arms after that and the
    //en: radio is deaf until reboot - and the Dispatcher's stuck-radio check cannot see it,
    //en: because it reads isInRecvMode(), which is this very flag.
    //sk: Tvrd, ze sme v Rx, len ked sa naozaj precital paket. Prerusenie „TX done" nastavuje
    //sk: ten isty bit STATE_INT_READY, takze po preposlani paketu sa tento blok vykona bez
    //sk: toho, aby bolo co citat: getPacketLength() vrati zvysky, readData() zlyha s -24, a
    //sk: pripnutie STATE_RX tu povedalo wrapperu, ze prijima, hoci cip bol po vysielani
    //sk: v STDBY_RC. Potom uz nikto prijem nenahodi a radio je hluche az do rebootu - a
    //sk: kontrola zaseknuteho radia v Dispatcheri to nevidi, lebo cita prave tento priznak.
    state = _fk_lenstate ? ((len > 0) ? STATE_RX : STATE_IDLE) : STATE_RX;   //en: runtime A/B, see _fk_lenstate
    #else
    state = STATE_IDLE;   // need another startReceive()
    #endif
  }

  //en: Mask STATE_INT_READY out of the comparison, the way isInRecvMode() already does.
  //en: The ISR ORs that bit in, so if a second packet arrives between the assignment
  //en: above and this test, state becomes STATE_RX|STATE_INT_READY - which is not equal
  //en: to STATE_RX, so we would call startReceive() on a receiver that is already in Rx.
  //en: LR2021 answers that with -706 and the wrapper is left in STATE_IDLE for good.
  //sk: Vymaskuj STATE_INT_READY z porovnania, rovnako ako to uz robi isInRecvMode().
  //sk: ISR ten bit priraduje cez OR, takze ak medzi priradenim vyssie a tymto testom
  //sk: pride dalsi paket, state je STATE_RX|STATE_INT_READY - a to sa nerovna STATE_RX,
  //sk: takze by sme zavolali startReceive() na prijimaci, ktory uz v Rx je. LR2021 na to
  //sk: odpovie -706 a wrapper ostane v STATE_IDLE natrvalo.
  //en: Measure the race instead of waiting for its absence: evaluate both conditions and
  //en: count the times they disagree. Those are exactly the moments that used to call
  //en: startReceive() on a receiver already in Rx and kill it. _fk_mask=false restores the
  //en: old broken behaviour for a proper A/B in the same traffic.
  //sk: Namiesto cakania na neprítomnost meraj samotny subeh: vyhodnot obe podmienky a
  //sk: pocitaj, kolkokrat sa nezhoduju. Prave to su okamihy, ktore predtym volali
  //sk: startReceive() na uz bezicom prijme a zabijali ho. _fk_mask=false vrati povodne
  //sk: chybne spravanie pre poctive A/B v tej istej prevadzke.
  {
    uint8_t st = state;
    bool masked   = ((st & ~STATE_INT_READY) != STATE_RX);
    bool unmasked = (st != STATE_RX);
    if (unmasked && !masked) {
      _n_race++;
      Serial.printf("[FK] race: prerusenie v okne (state=0x%02X), maska zachytila #%lu\r\n",
                    st, (unsigned long)_n_race);
    }
    if (_fk_mask ? masked : unmasked) {
    int err = _radio->startReceive();
#if defined(USE_LR2021)
    //en: LR2021 rejects SetRx with -706 unless it is in standby. startRecv() guards
    //en: against that, this inline re-arm never did - and this is the path taken after
    //en: a transmit, because recvRaw() otherwise pins state to STATE_RX and never comes
    //en: here. On every other chip this same line is the normal post-packet path and is
    //en: correct, which is why the omission is invisible until an LR2021 replies to
    //en: something. When it hits, state stays IDLE, the next loop retries identically,
    //en: and the receiver is down until a reboot.
    //en: The retry is instrumented rather than silent: we want to see in the log that
    //en: the error really happens, not just that the symptom went away.
    //sk: LR2021 odmietne SetRx s -706, ak nie je v standby. startRecv() to osetruje,
    //sk: toto vnutorne nahodenie nikdy nie - a prave sem sa program dostane po vysielani,
    //sk: lebo inak recvRaw() drzi state na STATE_RX a sem vobec nepride. Na kazdom inom
    //sk: cipe je ten isty riadok bezna cesta po prijatom pakete a je spravny, preto to
    //sk: chybalo nepovsimnute, kym LR2021 na nieco neodpovie. Ked to nastane, state
    //sk: ostane IDLE, dalsie kolo skusi to iste a prijem je mrtvy az do rebootu.
    //sk: Opakovanie je zamerne s vypisom, nie ticho: chceme v logu vidiet, ze ta chyba
    //sk: naozaj nastava, nie len ze symptom zmizol.
    if (err != RADIOLIB_ERR_NONE) {
      _n_rearm_failed++;
      _radio->standby();
      err = _radio->startReceive();
      if (err == RADIOLIB_ERR_NONE) { _n_rearm_fixed++; }

      //en: Never log this unconditionally. recvRaw() runs on every loop() iteration, so
      //en: once the chip stops accepting SetRx for good the message turns into tens of
      //en: thousands of lines a minute: it buries every other message, and the serial
      //en: writes themselves slow the loop to a crawl. Report the first failure of a run
      //en: and then at most one line per five seconds, carrying the running count.
      //sk: Toto nikdy nelogovat bez podmienky. recvRaw() bezi v kazdom kole loop(), takze
      //sk: ked cip prestane SetRx prijimat natrvalo, sprava sa zmeni na desiatky tisic
      //sk: riadkov za minutu: pochova kazdu inu spravu a samotne zapisy na seriovu linku
      //sk: spomalia slucku na plazenie. Vypis prve zlyhanie serie a potom najviac jeden
      //sk: riadok za pat sekund, aj s poctom.
      if (err == RADIOLIB_ERR_NONE) {
        if (_n_rearm_run) {
          Serial.printf("[FK] nahodenie prijmu opat preslo po %lu zlyhaniach\r\n",
                        (unsigned long)_n_rearm_run);
        }
        _n_rearm_run = 0;
      } else {
        uint32_t now = millis();
        if (_n_rearm_run == 0 || (uint32_t)(now - _t_rearm_msg) >= 5000) {
          _t_rearm_msg = now;
          Serial.printf("[FK] nahodenie prijmu odmietnute: %d (za sebou %lu)\r\n",
                        err, (unsigned long)_n_rearm_run + 1);
        }
        _n_rearm_run++;
      }
    }
#endif
    if (err == RADIOLIB_ERR_NONE) {
      //en: Any successful re-arm ends the run - not only one that needed the retry.
      //en: Clearing it only in the retry branch left the count standing after a
      //en: recovery put the receiver back in Rx, because this block then stops being
      //en: entered at all, and the guard kept firing on a radio that was already fine.
      //sk: Seriu ukoncuje kazde uspesne nahodenie, nie len to, ktore potrebovalo
      //sk: opakovanie. Nulovanie iba vo vetve opakovania nechalo pocet visiet aj po
      //sk: zotaveni, ktore prijem vratilo do Rx - tento blok sa uz potom nevykonava
      //sk: vobec a strazca strielal na radiu, ktore bolo v poriadku.
      _n_rearm_run = 0;
      state = STATE_RX;
    } else {
      MESH_DEBUG_PRINTLN("RadioLibWrapper: error: startReceive(%d)", err);
    }
    }
  }
  return len;
}

uint32_t RadioLibWrapper::getEstAirtimeFor(int len_bytes) {
  return _radio->getTimeOnAir(len_bytes) / 1000;
}

bool RadioLibWrapper::startSendRaw(const uint8_t* bytes, int len) {
  _board->onBeforeTransmit();
  int err = _radio->startTransmit((uint8_t *) bytes, len);
  if (err == RADIOLIB_ERR_NONE) {
    state = STATE_TX_WAIT;
    return true;
  }
  MESH_DEBUG_PRINTLN("RadioLibWrapper: error: startTransmit(%d)", err);
  idle();   // trigger another startRecv()
  _board->onAfterTransmit();
  return false;
}

bool RadioLibWrapper::isSendComplete() {
  if (state & STATE_INT_READY) {
    state = STATE_IDLE;
    n_sent++;
    return true;
  }
  return false;
}

void RadioLibWrapper::onSendFinished() {
  _radio->finishTransmit();
  _board->onAfterTransmit();
  state = STATE_IDLE;
}

int16_t RadioLibWrapper::performChannelScan() {
  return _radio->scanChannel();
}

bool RadioLibWrapper::isChannelActive() {
  // int.thresh: RSSI-based interference detection (relative to noise floor)
  if (_threshold != 0 && getCurrentRSSI() > _noise_floor + _threshold) return true;

  // cad: hardware channel activity detection
  if (_cad_enabled) {
    int16_t result = performChannelScan();
    // scanChannel() triggers DIO interrupt (CAD done) which sets STATE_INT_READY
    // via setFlag() ISR. Clear it before restarting RX so recvRaw() doesn't
    // try to read a non-existent packet and count a spurious recv error.
    state = STATE_IDLE;
    startRecv();
    if (result != RADIOLIB_CHANNEL_FREE) return true;
  }

  return false;
}

float RadioLibWrapper::getLastRSSI() const {
  return _radio->getRSSI();
}
float RadioLibWrapper::getLastSNR() const {
  return _radio->getSNR();
}

// Approximate SNR threshold per SF for successful reception (based on Semtech datasheets)
static float snr_threshold[] = {
    -7.5,  // SF7 needs at least -7.5 dB SNR
    -10,   // SF8 needs at least -10 dB SNR
    -12.5, // SF9 needs at least -12.5 dB SNR
    -15,  // SF10 needs at least -15 dB SNR
    -17.5,// SF11 needs at least -17.5 dB SNR
    -20   // SF12 needs at least -20 dB SNR
};

float RadioLibWrapper::packetScoreInt(float snr, int sf, int packet_len) {
  if (sf < 7) return 0.0f;

  if (snr < snr_threshold[sf - 7]) return 0.0f;    // Below threshold, no chance of success

  auto success_rate_based_on_snr = (snr - snr_threshold[sf - 7]) / 10.0;
  auto collision_penalty = 1 - (packet_len / 256.0);   // Assuming max packet of 256 bytes

  return max(0.0, min(1.0, success_rate_based_on_snr * collision_penalty));
}

PacketMillis RadioLibWrapper::calcMaxPacketMillis(uint8_t sf, float bw, uint8_t cr, uint8_t preambleSymbols) {
  // based on RadioLib's calculateTimeOnAir()
  uint32_t tsym_us = ((uint32_t)10000 << sf) / (bw * 10);
  uint32_t sfCoeff1_x4 = (sf == 5 || sf == 6) ? 25 : 17; // 6.25 : 4.25, semtech magic numbers to account for sync word + sfd

  // preamble + syncword + sfd + header
  uint32_t preamble_us = (((preambleSymbols + 8) * 4 + sfCoeff1_x4) * tsym_us) / 4;

  // airtime for max packet at current radio settings
  uint32_t total_us   = _radio->getTimeOnAir(MAX_TRANS_UNIT);
  // airtime for payload only (no preamble, header or SOF)
  uint32_t payload_us = total_us > preamble_us ? total_us - preamble_us : 4000 - preamble_us; // fallback to 4 secs at worst case
  // rescale payload_us for max possible CR
  if (cr >= 5 && cr < 8) { payload_us = (payload_us * 8) / cr; }

  return PacketMillis {(preamble_us + 999) / 1000, (payload_us + 999) / 1000};
}

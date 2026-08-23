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
      //en: guard OFF - one bool test, then straight to the library. Nothing is read,
      //en: recorded or pre-called, so this is the untouched measured configuration.
      //sk: guard OFF - jeden test bool a rovno do kniznice. Nic sa necita, nezapisuje
      //sk: ani nepredradzuje, takze toto je nedotknuta merana konfiguracia.
      if (!_fk_guard) return LR2021::getPacketLength(update);
      return _fk_pretype ? fkDiagPktLen(update) : fkGuardPktLen(update);
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
      uint32_t irq;     //en: whole IRQ word at that moment - says WHY the read happened
      uint16_t first;   //en: value the first read returned
      uint16_t final;   //en: value finally used
      uint8_t  stat;    //en: stat1 of the read that produced 'first'
      uint8_t  off;     //en: unused on LR2021 (no offset in the reply), kept so the CLI is shared
      uint8_t  tries;   //en: how many reads it took
      uint8_t  rule;    //en: bit0 = CMD_DAT rule fired, bit1 = fingerprint rule fired
      uint8_t  inj;     //en: 1 = the BUSY wait was skipped on purpose
      uint8_t  mode;    //en: guard mode the event was recorded under (1=A, 2=B, 3=C)
      uint16_t us;      //en: microseconds from the first read to the one that was used
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
    //en:  _fk_guard_mode decides what separates two attempts, which is the whole point
    //en:  of the experiment: measured on a live episode, retrying the same opcode
    //en:  back-to-back recovered 1 of 13 reads, while the library fallback - which
    //en:  issues getPacketType() before reading the length - recovered 12 of 12. So the
    //en:  question is whether the cure is the elapsed time or the intervening command.
    //en:    1 = A, attempts back to back (nothing in between)
    //en:    2 = B, idle delay of _fk_gap_us between attempts, no command
    //en:    3 = C, one getPacketType() between attempts
    //en:  A/B/C are switchable from the CLI so all three run inside one episode.
    //sk:  _fk_guard_mode urcuje, co oddeluje dva pokusy, a o to v tomto pokuse ide:
    //sk:  na zivej epizode opakovanie toho isteho opkodu za sebou zachranilo 1 z 13
    //sk:  citani, kym kniznicny fallback - ktory pred citanim dlzky vola getPacketType()
    //sk:  - zachranil 12 z 12. Otazka teda je, ci lieci uplynuty cas alebo vlozeny
    //sk:  prikaz.
    //sk:    1 = A, pokusy hned za sebou (nic medzi nimi)
    //sk:    2 = B, medzi pokusmi necinne cakanie _fk_gap_us, ziadny prikaz
    //sk:    3 = C, medzi pokusmi jeden getPacketType()
    //sk:  A/B/C sa prepinaju z CLI, takze vsetky tri prebehnu v jednej epizode.
    uint8_t  _fk_guard_mode = 1;
    uint16_t _fk_gap_us = 60;
    //en:  _fk_max_tries caps the attempts. The shipped guard stops at 3, which is why we
    //en:  cannot tell whether a fourth or fifth read of the same opcode would still help
    //en:  - and that is exactly the number the upstream patch has to choose. Settable
    //en:  from the CLI so the recovery curve can be walked without reflashing.
    //sk:  _fk_max_tries ohranicuje pocet pokusov. Posielany guard sa zastavi na 3, takze
    //sk:  nevieme, ci by stvrte alebo piate citanie toho isteho opkodu jeste pomohlo - a
    //sk:  presne toto cislo si ma upstream patch vybrat. Nastavitelne z CLI, aby sa krivka
    //sk:  zotavenia dala prejst bez noveho flashu.
    uint8_t  _fk_max_tries = 3;
    uint16_t _fk_zero = 0, _fk_zero_cnt = 0;
    bool _fk_pretype = false;
    //en:  _fk_guard = master switch for our stale-reply guard, 'fk guard on|off'.
    //en:              OFF is a single bool test and then the plain library read, so the
    //en:              timing of the measured configuration is left alone. ON runs the
    //en:              guard and records every recovery into the ring buffer.
    //sk:  _fk_guard = hlavny prepinac nasho guardu, 'fk guard on|off'.
    //sk:              OFF je jediny test bool a potom hole kniznicne citanie, takze
    //sk:              casovanie meranej konfiguracie zostava nedotknute. ON pusti guard
    //sk:              a kazdu zachranu zapise do kruhoveho buffra.
    bool _fk_guard = false;

    //en: The shipped guard, plus a record of every recovery. Trusts the length only
    //en: when stat1 says CMD_DAT; otherwise re-reads (Rx FIFO is still whole at that
    //en: point) and finally falls back to the library read, so it never ends up worse.
    //en: Nothing is printed from here - we are in the hot Rx path; 'fk spifix' dumps it.
    //sk: Ostry guard plus zaznam kazdej zachrany. Dlzke veri len ked stat1 hlasi
    //sk: CMD_DAT; inak precita znova (Rx FIFO je v tom momente jeste cele) a nakoniec
    //sk: spadne na kniznicne citanie, takze nikdy neskonci horsie. Odtialto sa NIC
    //sk: netlaci - sme v horucej RX ceste; vypise to 'fk spifix'.
    size_t fkGuardPktLen(bool update) {
      uint8_t  stat = 0, stat0 = 0, tries = 0;
      uint16_t val = 0, first = 0;
      size_t   len = 0;
      bool inject = false;
      if (_fk_inject && ++_fk_inject_cnt >= _fk_inject) { _fk_inject_cnt = 0; inject = true; }
      uint32_t t0 = micros();
      for (tries = 1; tries <= _fk_max_tries; tries++) {
        //en: what separates two attempts is the variable under test - see _fk_guard_mode.
        //en: Nothing is inserted before the first read, so 'first' stays comparable
        //en: across modes.
        //sk: co oddeluje dva pokusy, je tu meranou premennou - vid _fk_guard_mode. Pred
        //sk: prvym citanim sa nevklada nic, aby 'first' ostal medzi modmi porovnatelny.
        if (tries > 1) {
          if (_fk_guard_mode == 2) {
            delayMicroseconds(_fk_gap_us);
          } else if (_fk_guard_mode == 3) {
            uint8_t pt = 0; (void)getPacketType(&pt);
          }
        }
        readRxPktLenWithStatus(inject && tries == 1 ? false : true, &stat, &val);
        if (tries == 1) { first = val; stat0 = stat; }
        if ((stat & 0x0E) == RADIOLIB_LRXXXX_STAT_1_CMD_DAT) { len = val; break; }
        _stale_pktlen_reads++;
      }
      //en: exhausted - fall back to the library read, which never ends up worse. 'tries'
      //en: is clamped to 255 so the record still says "the cap was not enough".
      //sk: vycerpane - spadni na kniznicne citanie, ktore nikdy neskonci horsie. 'tries'
      //sk: sa zarazi na 255, takze zo zaznamu je vidno, ze strop nestacil.
      if (tries > _fk_max_tries) { len = LR2021::getPacketLength(update); tries = 255; }
      uint32_t us = micros() - t0;

      //en: record only when the guard actually did something - reading the IRQ word
      //en: afterwards is safe, getIrqStatus() cannot be hit by this race.
      //sk: zaznamenaj len ked guard naozaj zasiahol - precitanie IRQ slova az potom je
      //sk: bezpecne, getIrqStatus() tato preteka zasiahnut nemoze.
      if (tries > 1) {
        uint32_t irqw = getIrqStatus();
        FkSpiEvent& e = _ev[_ev_write];
        e.fp = (uint16_t)(irqw >> 16); e.irq = irqw;
        e.first = first; e.final = (uint16_t)len;
        e.stat = stat0;  e.off = 0;
        e.tries = tries; e.rule = 1; e.inj = inject ? 1 : 0;
        e.mode = _fk_guard_mode; e.us = (uint16_t)(us > 65535 ? 65535 : us);
        _ev_write = (uint8_t)((_ev_write + 1) % FK_SPI_EVENTS);
        if (_ev_count < FK_SPI_EVENTS) _ev_count++;
        _ev_total++;
      }
      return len;
    }

    size_t fkDiagPktLen(bool update) {
      uint8_t  stat0 = 0, stat = 0, tries = 0;
      uint16_t fp = 0, val = 0, first = 0;
      uint32_t irqw = 0, irq0 = 0;
      bool cmd_flagged = false, fp_flagged = false;

      bool inject = false;
      if (_fk_inject && ++_fk_inject_cnt >= _fk_inject) { _fk_inject_cnt = 0; inject = true; }
      //en: force the len==0 bail-out path in recvRaw() - it skips readData(), so nothing
      //en: clears the IRQ flags. On this wrapper LR2021 also does not re-arm Rx, so the
      //en: question is whether the flags (and the DIO line) stay asserted afterwards.
      //sk: vynut cestu s nulovou dlzkou v recvRaw() - tam sa readData() preskoci, takze
      //sk: IRQ priznaky nikto nevycisti. LR2021 sa navyse v tomto wrapperi nerearmuje,
      //sk: takze otazka je, ci priznaky (a linka DIO) ostanu svietit.
      if (_fk_zero && ++_fk_zero_cnt >= _fk_zero) { _fk_zero_cnt = 0; return 0; }
#if RADIOLIB_GODMODE
      if (_fk_pretype) { uint8_t t = 0; (void)getPacketType(&t); }
#endif

      for (tries = 1; tries <= 4; tries++) {
        irqw = getIrqStatus();
        fp = (uint16_t)(irqw >> 16);
        readRxPktLenWithStatus(inject && tries == 1 ? false : true, &stat, &val);
        if (tries == 1) { first = val; stat0 = stat; irq0 = irqw; }
        //en: same condition as CustomLR1110 on purpose, so both chips are comparable -
        //en: a zero fingerprint is recorded too, it just does not drive a retry.
        //sk: zamerne ta ista podmienka ako v CustomLR1110, aby boli oba cipy
        //sk: porovnatelne - nulovy odtlacok sa tiez zaznamena, len nespusti opakovanie.
        if (val == fp && (irqw & RADIOLIB_LR2021_IRQ_RX_DONE)) fp_flagged = true;
        if (((stat >> 1) & 0x03) == 0x03) break;   //en: CMD_DAT -> reply belongs to us
        cmd_flagged = true;
      }

      size_t len = val;
      if (((stat >> 1) & 0x03) != 0x03) len = LR2021::getPacketLength(update);

      if (cmd_flagged || fp_flagged) {
        _stale_pktlen_reads++;
        _ev_total++;
        FkSpiEvent& e = _ev[_ev_write];
        e.fp = fp; e.irq = irq0; e.first = first; e.final = (uint16_t)len;
        e.stat = stat0; e.off = 0; e.tries = tries > 4 ? 4 : tries;
        e.mode = 0; e.us = 0;   //en: not the A/B/C harness
        e.rule = (cmd_flagged ? 1 : 0) | (fp_flagged ? 2 : 0);
        e.inj = inject ? 1 : 0;
        _ev_write = (uint8_t)((_ev_write + 1) % FK_SPI_EVENTS);
        if (_ev_count < FK_SPI_EVENTS) _ev_count++;
      }
      return len;
    }
#endif

#ifdef FK_RADIO_SPI_DIAG
    //en: Is BUSY observable at all? SPItransferStream() waits 1 us after CS-high and then
    //en: polls BUSY for a LOW level - so if the line has not risen yet, the wait does
    //en: nothing and the reply is read too early. Watching for the RISE instead would
    //en: prevent that, but only if the MCU can actually catch the line high. This issues
    //en: the opcode with the wait skipped and then samples BUSY in a tight loop:
    //en:   hi    = how many of the first samples read HIGH
    //en:   fall  = sample index where it went low (0 = never seen high)
    //en: If hi is always 0, BUSY rises and falls faster than we can sample, and no amount
    //en: of polling can fix the handshake - the status byte stays the only signal.
    //sk: Da sa BUSY vobec zachytit? SPItransferStream() pocka po CS-high 1 us a potom
    //sk: poluje na NIZKU uroven - ak linka este nestupla, cakanie nerobi nic a odpoved sa
    //sk: cita priskoro. Sledovat NASTUP by tomu predislo, ale len ak MCU vie linku
    //sk: zachytit vysoko. Toto posle opcode s preskocenym cakanim a potom vzorkuje BUSY v
    //sk: tesnej slucke:
    //sk:   hi    = kolko z prvych vzoriek bolo VYSOKO
    //sk:   fall  = index vzorky, kde spadla (0 = nikdy nevidena vysoko)
    //sk: Ak je hi vzdy 0, BUSY stupa a padá rychlejsie nez vieme vzorkovat a handshake sa
    //sk: polovanim opravit neda - status bajt ostava jediny signal.
    void fkBusyProbe(uint16_t* hi, uint16_t* fall, uint16_t samples = 400) {
      uint32_t pin = mod->getGpio();
      *hi = 0; *fall = 0;
      if (pin == RADIOLIB_NC) return;
      mod->SPIwriteStream(RADIOLIB_LR2021_CMD_GET_RX_PKT_LENGTH, NULL, 0, false, false);
      for (uint16_t i = 1; i <= samples; i++) {
        if (mod->hal->digitalRead(pin)) { (*hi)++; }
        else if (*hi) { *fall = i; break; }
      }
    }
#endif

    //en: current IRQ word and the level of the IRQ line, for checking whether a
    //en: zero-length read left the flags (and the line) asserted.
    //sk: aktualne IRQ slovo a uroven IRQ linky, na overenie, ci nulove citanie
    //sk: nechalo priznaky (a linku) svietit.
    void fkIrqState(uint32_t* irq, uint8_t* dio) {
      if (irq) *irq = getIrqStatus();
      uint32_t p = mod->getIrq();
      if (dio) *dio = (p == RADIOLIB_NC) ? 2 : (uint8_t)mod->hal->digitalRead(p);
    }

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
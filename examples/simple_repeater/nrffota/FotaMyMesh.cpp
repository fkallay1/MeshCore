// =====================================================================
// FotaMyMesh.cpp — FOTA časť triedy MyMesh (telá metód + lokálne helpery).
//
// Drží celú FOTA integráciu MIMO examples/simple_repeater/MyMesh.cpp, aby
// bol diff repeatera oproti upstream (branch dev) minimálny — v MyMesh.cpp
// ostávajú len tenké 1–3 riadkové hooky, v MyMesh.h jeden súvislý blok
// deklarácií. Všetko tu je gated -D WITH_LORA_FOTA.
//
// Diagnostické výpisy idú cez FOTA_DEBUG_PRINT/PRINTLN (FotaDebug.h),
// zapnuté -D FOTA_DEBUG=1 vo FOTA env; CLI odpovede (reply) sú funkčný
// výstup a ostávajú sprintf.
// =====================================================================
#ifdef WITH_LORA_FOTA

#include "../MyMesh.h"
#include "FotaMesh.h"
#include "FotaReceiver.h"
#include "FotaPatcher.h"
#include "FotaBuffer.h"   // zdieľaný scratch — parkovisko snapshotu deferred CLI
#include "FotaDebug.h"

#if defined(FK_DEBUG_STACKTRACE) && defined(INCLUDE_pxTaskGetStackStart) && INCLUDE_pxTaskGetStackStart == 1
#include <task.h>                 // pxTaskGetStackStart — AKTUÁLNE použitie loop-stacku (FK_DEBUG_STACKTRACE)
#endif

extern RADIO_CLASS radio;   // surový RadioLib SX1262 (z target.cpp) — pre AGC register read ('fota agc')

#if __has_include("build_info.h")
  #include "build_info.h"   // DOČASNÉ: test_nrf-fota/gen_build_info.py (pre-script)
#endif
#ifndef FW_BUILD_NUMBER
  #define FW_BUILD_NUMBER 0
#endif

// Oneskorenie CLI odpovede — musí sedieť s CLI_REPLY_DELAY_MILLIS v MyMesh.cpp
// (konštanta je tam súkromná; nedá sa includnúť bez ďalšieho hooku).
#define FOTA_CLI_REPLY_DELAY_MILLIS  600

// Rozpoznaj FOTA CLI príkaz a vráť smerník na argumenty (časť ZA 'fota'/'ota',
// vrátane vedúcej medzery), alebo NULL ak to nie je FOTA príkaz. Jediný zdroj
// pravdy pre detekciu — používa LoRa (defer) aj Serial (inline) cesta.
// FOTA-CLI-ALIAS: keď klienti prejdú na 'fota', zmaž 'ota' vetvu.
static const char* fota_args_of(const char* cmd) {
  if (memcmp(cmd, "fota", 4) == 0 && (cmd[4] == 0 || cmd[4] == ' ')) return cmd + 4;
  if (memcmp(cmd, "ota",  3) == 0 && (cmd[3] == 0 || cmd[3] == ' ')) return cmd + 3;  // FOTA-CLI-ALIAS
  return nullptr;
}

// Názov PAYLOAD_TYPE pre čitateľný RAW log.
__attribute__((unused))
static const char* payload_type_name(uint8_t t) {
  switch (t) {
    case PAYLOAD_TYPE_REQ:        return "REQ";
    case PAYLOAD_TYPE_RESPONSE:   return "RESPONSE";
    case PAYLOAD_TYPE_TXT_MSG:    return "TXT_MSG";
    case PAYLOAD_TYPE_ACK:        return "ACK";
    case PAYLOAD_TYPE_ADVERT:     return "ADVERT";
    case PAYLOAD_TYPE_GRP_TXT:    return "GRP_TXT";
    case PAYLOAD_TYPE_GRP_DATA:   return "GRP_DATA";
    case PAYLOAD_TYPE_ANON_REQ:   return "ANON_REQ";
    case PAYLOAD_TYPE_PATH:       return "PATH";
    case PAYLOAD_TYPE_TRACE:      return "TRACE";
    case PAYLOAD_TYPE_MULTIPART:  return "MULTIPART";
    case PAYLOAD_TYPE_CONTROL:    return "CONTROL";
    case PAYLOAD_TYPE_RAW_CUSTOM: return "RAW_CUSTOM";
    default:                      return "?";
  }
}

// Snapshot kontextu klienta pre odloženú CLI odpoveď. Parkuje sa v zdieľanom
// FotaBuffer (~182 B v 512 B okne), takže MyMesh nerastie o permanentný člen.
struct FotaCliDefer {
  uint8_t  dest_pub[PUB_KEY_SIZE];     // 32 — komu odpovedať (Identity)
  uint8_t  secret[PUB_KEY_SIZE];       // 32 — shared secret na šifrovanie odpovede
  uint8_t  out_path[MAX_PATH_SIZE];    // 64 — známa out-path (ak je)
  uint32_t sender_timestamp;           // ts prijatého príkazu — odpoveď musí mať INÝ (CLI dedup)
  uint8_t  out_path_len;               // OUT_PATH_UNKNOWN → flood reply
  uint8_t  path_hash_size;             // pre flood reply
  char     fargs[48];                  // argumenty (" verify", " agc", …)
};

// ── FK_DEBUG_STACKTRACE: dočasné meranie stacku — na ZÁVER CELÉ vyhodiť ─────
#if defined(FK_DEBUG_STACKTRACE) && defined(INCLUDE_pxTaskGetStackStart) && INCLUDE_pxTaskGetStackStart == 1
// AKTUÁLNE použitie loop-tasku stacku [B] (na rozdiel od uxTaskGetStackHighWaterMark,
// ktorý je historické MINIMUM voľného). Hrubý odhad: SP ≈ adresa lokálu; pxTaskGetStackStart
// vráti spodok (najnižšiu adresu) stacku tasku. Total = LOOP_STACK_SZ (256*4 slov) = 4096 B.
#define FOTA_LOOP_STACK_TOTAL_B  4096u
__attribute__((unused))
static uint32_t loop_stack_used_now() {
  uint8_t marker;
  uint8_t* base = pxTaskGetStackStart(nullptr);
  if (!base) return 0;
  uint32_t free_now = (uint32_t)&marker - (uint32_t)base;   // voľné pod aktuálnym SP
  return (free_now < FOTA_LOOP_STACK_TOTAL_B) ? (FOTA_LOOP_STACK_TOTAL_B - free_now) : 0;
}
#endif

// =====================================================================
// RAW log — každý surový (CRC-OK) rámec, EŠTE PRED dekódovaním/dešifrovaním.
// Odpoveď na otázku "prichádzajú na repeater hocijaké pakety?" — ak rawrx
// rastie ale GRP_DATA/onGroupDataRecv nie, chyba je v dekódovaní, nie v RF.
// Volané z MyMesh::logRxRaw (hook).
// =====================================================================
void MyMesh::fotaLogRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
  _fota_raw_rx++;
  _fota_raw_last_len  = (uint32_t)len;
  _fota_raw_last_rssi = rssi;
  _fota_raw_last_snr  = snr;
  FOTA_DEBUG_PRINT("[FOTA] RAW #%lu len=%d rssi=%d snr=%.1f",
                   (unsigned long)_fota_raw_rx, len, (int)rssi, snr);
  if (len > 0) {
    // PAYLOAD_TYPE = (hdr >> 2) & 0x0F (NIE dolné 4 bity — tie sú route+časť typu);
    // route = hdr & 0x03.  (4=ADVERT, 7=ANON_REQ, 2=TXT, 1=RESPONSE, 5=GRP_TXT, 6=GRP_DATA)
    uint8_t pt    = (raw[0] >> 2) & 0x0F;
    uint8_t route = raw[0] & 0x03;
    // Replikácia tryParsePacket() offsetov, aby sme vedeli vypísať CESTU a rozoznať
    // FOTA bez dešifrovania:  [hdr][?transport 4B][path_len][path…][payload].
    int i = 1;
    if (route == ROUTE_TYPE_TRANSPORT_FLOOD || route == ROUTE_TYPE_TRANSPORT_DIRECT) i += 4;
    int path_count = -1, path_off = i + 1, path_byte_len = 0, payload_off = -1;
    if (i < len) {
      uint8_t plen  = raw[i];
      uint8_t hsize = (plen >> 6) + 1;       // path hash size (1 alebo 2 B)
      path_count    = plen & 63;             // počet hopov (0 = zero-hop / čerstvý flood)
      path_byte_len = path_count * hsize;
      payload_off   = path_off + path_byte_len;
    }
    // Naša FOTA? GRP_DATA na našom FOTA kanáli — channel_hash je 1. bajt payloadu
    // (Mesh.cpp: channel_hash = payload[0]); čitateľné už tu, pred dešifrovaním.
    bool is_fota = (pt == PAYLOAD_TYPE_GRP_DATA && _fota_ready
                    && payload_off >= 0 && payload_off < len
                    && raw[payload_off] == _fota_channel.hash[0]);
    FOTA_DEBUG_PRINT(" type=%u(%s%s) route=%u hdr=0x%X",
                     (unsigned)pt, payload_type_name(pt), is_fota ? "/FOTA" : "",
                     (unsigned)route, (unsigned)raw[0]);
    // Cesta: počet hopov + hash bajty.  path[0] = zero-hop alebo čerstvý flood od
    // zdroja; každý preposielajúci repeater pripojí svoj hash → path[N] dlhšia.
    if (path_count >= 0) {
      FOTA_DEBUG_PRINT(" path[%d]", path_count);
      if (path_byte_len > 0 && (path_off + path_byte_len) <= len) {
        FOTA_DEBUG_PRINT("=");
        for (int k = 0; k < path_byte_len; k++) FOTA_DEBUG_PRINT("%02X", (unsigned)raw[path_off + k]);
      }
    }
  }
  FOTA_DEBUG_PRINT(" first=");
  int n8 = len < 8 ? len : 8;
  for (int k = 0; k < n8; k++) FOTA_DEBUG_PRINT("%02X", (unsigned)raw[k]);
  FOTA_DEBUG_PRINTLN("");
}

// =====================================================================
// GRP_DATA kanál — repeater "subscribne" jediný FOTA kanál; keď sa
// channel_hash zhoduje, MeshCore dešifruje GRP_DATA cez fota_channel.secret
// a zavolá onGroupDataRecv().
// =====================================================================
int MyMesh::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) {
  if (_fota_ready && max_matches > 0 && hash[0] == _fota_channel.hash[0]) {
    channels[0] = _fota_channel;
    return 1;
  }
  return 0;
}

// Dešifrovaný GRP_DATA payload: [ts 4B LE][fota_type 1B][...]. FOTA payload
// začína za 4B timestampom (zhodné s fota_sender.py meshcore_grp_data_packet).
void MyMesh::onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel,
                             uint8_t* data, size_t len) {
  if (type != PAYLOAD_TYPE_GRP_DATA) return;
  if (channel.hash[0] != _fota_channel.hash[0]) return;   // nie náš FOTA kanál
  // Zjednotený FOTA formát: štandardný GRP_DATA plaintext = [data_type 2B][len 1B][ts 4B][fota_payload].
  // Odlúpni [data_type][len]; ak data_type != FOTA_MAGIC, nie je to FOTA. Po odlúpnutí má
  // buffer tvar [ts 4B][fota_payload] — zvyšok pipeline (loop +4) ostáva nezmenený.
  if (len < 3 + 5) return;                               // [dt2][len1] + [ts4][type1]
  uint16_t dtype = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
  if (dtype != FOTA_MAGIC) return;                        // nie náš FOTA data_type
  // data[2] = pravá dĺžka [ts4][fota_payload]. MACThenDecrypt vracia AES-padovanú
  // (16B) dĺžku, preto NEporovnávaj s len; použi data[2] na strhnutie paddingu.
  uint8_t inner = data[2];
  if (inner < 5 || (size_t)(3 + inner) > len) return;    // sanity vs padded buffer
  data += 3; len = inner;                                // → presné [ts4][fota_payload]
#ifdef FOTA_GDR_DIAG
  FOTA_DEBUG_PRINTLN("[DIAG] GDR fota_type=0x%X len=%d pending=%d",
                     (unsigned)data[4], (int)len, _fota_pending_len);
#endif
  // Odlož payload — pomalé CustomLFS I/O sa spraví v loop() PO tom, čo dispatcher
  // re-armne rádio do RX. FS zápis priamo tu oneskoroval re-arm a rádio po prvom
  // pakete prestávalo prijímať. Ak ešte čaká predošlý, tento zahodíme (loop ho
  // stihne spracovať skôr ako príde ďalší LoRa paket pri SF7).
  if (_fota_pending_len == 0) {
    int n = (int)len;
    if (n > (int)sizeof(_fota_pending)) n = (int)sizeof(_fota_pending);
    memcpy(_fota_pending, data, n);
    _fota_pending_len  = n;
    _fota_pending_rssi = (float)radio_driver.getLastRSSI();
    _fota_pending_snr  = packet->getSNR();
  } else {
    FOTA_DEBUG_PRINTLN("[FOTA] WARN pending busy, paket zahodený");
  }
}

// =====================================================================
// Inicializácia (hooky z MyMesh::begin)
// =====================================================================
// PRED mesh::Mesh::begin(): prečítaj flasher debug marker čo najskôr po
// boote (GPREGRET2/RESETREAS, pred SoftDevice) + vynuluj FOTA stav.
void MyMesh::fotaEarlyInit() {
  fota_check_flasher_debug();
  _fota_ready = false;
  _fota_pending_len = 0;
  _fota_cli_pending = false;
  _fota_cli_buf = nullptr;
  _fota_apply_deadline = 0;
  _fota_raw_rx = 0;
  _fota_raw_last_len = 0;
  _fota_raw_last_rssi = _fota_raw_last_snr = 0;
}

// NA KONCI MyMesh::begin(): mount FOTA FS + kanál + boot banner.
void MyMesh::fotaBegin() {
  fota_init();                       // mount CustomLFS @ 0xD4000 + resume
  fota_build_channel(_fota_channel); // FOTA GRP_DATA kanál (#-konvencia z FOTA_CHANNEL_NAME)
  _fota_ready = true;
  fota_print_flasher_debug();        // ak sa práve vrátil z flashera
  FOTA_DEBUG_PRINTLN("[FOTA] build #%lu  freq=%.3f sf=%u bw=%.1f",
                     (unsigned long)FW_BUILD_NUMBER, _prefs.freq, (unsigned)_prefs.sf, _prefs.bw);
}

// =====================================================================
// CLI — Serial (inline) aj LoRa (deferred) cesta
// =====================================================================
// Serial/inline cesta (hook z MyMesh::handleCommand). Vracia false ak to
// nie je FOTA príkaz ('fota …' / legacy 'ota …'). Beží na plytkom stacku.
// LoRa cesta sem nepríde — onPeerDataRecv ju odloží cez fotaHandleLoRaCli.
bool MyMesh::fotaHandleCliCommand(const char* command, char* reply) {
  const char* fargs = fota_args_of(command);
  if (!fargs) return false;
  runFotaCli(fargs, reply);
  return true;
}

// LoRa cesta (hook z MyMesh::onPeerDataRecv). Vracia false ak to nie je FOTA
// príkaz. NEspracúva inline — sme hlboko v RX callstacku (verify by pretiekol
// 4 kB loop-task stack a TICHO prepísal susedný heap = stav rádia). Snapshot
// klienta → zdieľaný FotaBuffer, spracuje a odpovie loop() (fotaLoop).
bool MyMesh::fotaHandleLoRaCli(const ClientInfo* client, const uint8_t* secret,
                               const char* command, char* reply,
                               uint8_t path_hash_size, uint32_t sender_timestamp) {
  // Echo prijatého príkazu z LoRa admin CLI (diagnostika — operátor pri doske
  // vidí, čo bolo zadané vzdialene; retry duplikáty filtruje volajúci).
  // Párové značky: [LoRa->CLI] = prišlo z LoRa, [CLI->LoRa] = posielaná odpoveď.
  FOTA_DEBUG_PRINTLN("[LoRa->CLI] %s", command);
  const char* fargs = fota_args_of(command);
  if (!fargs) return false;
  if (_fota_cli_pending) {
    strcpy(reply, "FOTA: zaneprázdnené, skús neskôr");
  } else if (deferFotaCli(client, secret, fargs, path_hash_size, sender_timestamp)) {
#ifdef FOTA_INFO_MSG
    // Voliteľný medzi-paket "spracúvam". DEFAULT VYP: cez repeater idú dva
    // pakety (tento + výsledok z loop()) tesne za sebou a druhý — podstatný
    // — sa môže stratiť. Bez flagu pošleme len jeden paket: finálny výsledok.
    strcpy(reply, "FOTA: spracúvam, výsledok o chvíľu...");
#else
    *reply = 0;   // žiadny medzi-paket; odpoveď príde len raz, z loop()
#endif
  } else {
    strcpy(reply, "FOTA: defer zlyhal (buffer)");
  }
  return true;
}

// Telo FOTA CLI (agc diagnostika + fota_handle_command). Volané z handleCommand
// (Serial, inline) aj z loop() (odložená LoRa cesta) — VŽDY na plytkom stacku.
void MyMesh::runFotaCli(const char* fargs, char* reply) {
  if (strcmp(fargs, " agc") == 0) {
    // AGC/gain diagnostika rádia (READ-ONLY — nemení konfiguráciu rádia, žiadny
    // dopad na kompatibilitu s inými MeshCore zariadeniami). Pri point-blank
    // (RSSI ~-23) overuje či sa receiver nedesenzitizoval / aký má gain mód.
    uint8_t rxgain = 0;
    radio.readRegister(0x08AC, &rxgain, 1);   // RADIOLIB_SX126X_REG_RX_GAIN
    float inst_rssi = radio.getRSSI(false);   // okamžité RSSI kanála (GetRssiInst)
    const char* gm = (rxgain == 0x96) ? "boosted" : (rxgain == 0x94 ? "power-save" : "?");
    FOTA_DEBUG_PRINTLN("[FOTA] AGC rxgain_reg=0x%X %s  boost_pref=%s  inst_rssi=%.1fdBm  nf=%d  agc_reset=%lus(0=vyp)",
                       (unsigned)rxgain, gm,
                       radio_driver.getRxBoostedGainMode() ? "on" : "off",
                       inst_rssi, (int)_radio->getNoiseFloor(),
                       (unsigned long)(((uint32_t)_prefs.agc_reset_interval) * 4));
    sprintf(reply, "AGC gain=0x%02X(%s) boost=%s rssi=%ddBm nf=%d agc_reset=%lus",
            rxgain, gm, radio_driver.getRxBoostedGainMode() ? "on" : "off",
            (int)inst_rssi, (int)_radio->getNoiseFloor(),
            (unsigned long)(((uint32_t)_prefs.agc_reset_interval) * 4));
  } else {
    // POZOR: AGC auto-reset (set agc.reset.interval > 0) NEKOMBINOVAŤ s FOTA flashom!
    // Ak agc resety (radio.sleep+calibrate) bežia počas FOTA session, nasledujúci
    // 'fota flash' zlyhá (flasher sa zastaví po "Komprimovany format", repeater
    // nabehne na OLD). Pri agc_reset=0 funguje príjem aj flash spoľahlivo.
    // (Overené 2026-06-15: agc=0 #28→#29 PASS; agc=8 #28→#29 aj #30→#31 FAIL.)
    fota_handle_command(fargs, reply);   // LoRa-FOTA: status|verify|flash|clear|id|...
  }
}

// Zaparkuj snapshot klienta do zdieľaného FotaBuffer a označ čakajúci príkaz.
// false = buffer nedostupný (už požičaný). Buffer drží snapshot až kým ho loop()
// neprečíta a neuvoľní (potom ho fota_patch_to_file môže požičať na hpatch cache).
bool MyMesh::deferFotaCli(const ClientInfo* client, const uint8_t* secret,
                          const char* fargs, uint8_t path_hash_size,
                          uint32_t sender_timestamp) {
  uint8_t* buf = fota_get_buffer(sizeof(FotaCliDefer));
  if (!buf) return false;
  FotaCliDefer* s = (FotaCliDefer*)buf;
  memcpy(s->dest_pub, client->id.pub_key, PUB_KEY_SIZE);
  memcpy(s->secret, secret, PUB_KEY_SIZE);
  s->sender_timestamp = sender_timestamp;
  s->out_path_len   = client->out_path_len;
  s->path_hash_size = path_hash_size;
  if (client->out_path_len != OUT_PATH_UNKNOWN && client->out_path_len <= MAX_PATH_SIZE) {
    memcpy(s->out_path, client->out_path, client->out_path_len);
  }
  strncpy(s->fargs, fargs, sizeof(s->fargs) - 1);
  s->fargs[sizeof(s->fargs) - 1] = 0;
  _fota_cli_buf     = buf;
  _fota_cli_pending = true;
  return true;
}

// Pošli CLI textovú odpoveď klientovi zo snapshotu (z loop(), po dobehnutí
// odloženého príkazu). Vyfaktorované z onPeerDataRecv TXT_MSG vetvy.
void MyMesh::sendDeferredCliReply(const uint8_t* dest_pub, const uint8_t* secret,
                                  const uint8_t* out_path, uint8_t out_path_len,
                                  uint8_t path_hash_size, const char* text,
                                  uint32_t sender_timestamp) {
  int text_len = strlen(text);
  if (text_len <= 0) return;
  if (text_len > 160) text_len = 160;
  // Symetria k "[LoRa->CLI]" echu — operátor pri doske vidí aj odchádzajúcu
  // odpoveď. (Upstream ani predrefaktorový kód odpoveď nevypisovali.)
  FOTA_DEBUG_PRINTLN("[CLI->LoRa] %s", text);
  uint8_t temp[166];
  uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
  if (timestamp <= sender_timestamp) {
    // Odpoveď musí mať timestamp VYŠŠÍ než príkaz, inak ju companion (seen-table
    // dedup podľa ts) odfiltruje ako duplikát a nezobrazí. Pokrýva DVA prípady:
    //  1) zosynced čas + rýchla odpoveď v kľude → reply_ts == sender_ts (kolízia),
    //  2) ZLÝ čas repeatera (po reboote 2024) → reply_ts < sender_ts → companion by
    //     odpoveď zoradil do minulosti / odfiltroval. V oboch ho ťaháme nad sender_ts.
    // (Inline cesta cez CommonCLI si čas synchronizuje sama; FOTA cesta nie.)
    timestamp = sender_timestamp + 1;
  }
  memcpy(temp, &timestamp, 4);
  temp[4] = (TXT_TYPE_CLI_DATA << 2);
  memcpy(&temp[5], text, text_len);
  mesh::Identity id(dest_pub);
  auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, id, secret, temp, 5 + text_len);
  if (!pkt) return;
  if (out_path_len == OUT_PATH_UNKNOWN) {
    sendFloodReply(pkt, FOTA_CLI_REPLY_DELAY_MILLIS, path_hash_size);
  } else {
    sendDirect(pkt, (uint8_t*)out_path, out_path_len, FOTA_CLI_REPLY_DELAY_MILLIS);
  }
}

// =====================================================================
// fotaLoop — deferred práca (hook z MyMesh::loop, PO mesh::Mesh::loop()).
// =====================================================================
void MyMesh::fotaLoop() {
  // Odložené FOTA spracovanie — mesh::Mesh::loop() vyššie už re-armol rádio do RX,
  // takže pomalé CustomLFS I/O tu už nezablokuje príjem ďalšieho paketu.
  if (_fota_pending_len > 0) {
    int n = _fota_pending_len;
    fota_print_pkt(_fota_pending + 4, n - 4, _fota_pending_rssi, _fota_pending_snr);
    fota_process(_fota_pending + 4, n - 4);
    _fota_pending_len = 0;   // uvoľni buffer až po spracovaní
  }

  // Odložené FOTA CLI (z LoRa) — tu beží na PLYTKOM stacku ako Serial cesta.
  // Snapshot skopíruj zo zdieľaného FotaBuffer na (plytký) loop stack, buffer
  // UVOĽNI (aby ho fota_patch_to_file mohol požičať na hpatch cache), a až POTOM
  // spusti príkaz. Výsledok pošli klientovi. (flash → fota_apply nevráti sa.)
  if (_fota_cli_pending && _fota_cli_buf) {
    FotaCliDefer snap;
    memcpy(&snap, _fota_cli_buf, sizeof(snap));
    fota_put_buffer(_fota_cli_buf);
    _fota_cli_buf     = nullptr;
    _fota_cli_pending = false;

    char reply[166];
    reply[0] = 0;
    runFotaCli(snap.fargs, reply);   // ťažká práca (verify=hpatch+SHA) na plytkom stacku
    sendDeferredCliReply(snap.dest_pub, snap.secret, snap.out_path,
                         snap.out_path_len, snap.path_hash_size, reply,
                         snap.sender_timestamp);
  }

  // Odložený flash: 'fota flash' nastaví fota_apply_pending() a pošle ACK „accepted".
  // Skutočný flash (fota_apply, NEVRÁTI sa) spustíme AŽ keď ACK reálne odíde z
  // outbound queue — inak by reboot prišiel skôr než sa ACK odvysiela a odosielateľ
  // by nič nedostal (presne to sa stalo). Safety net: deadline, aby flash nečakal
  // donekonečna pri inej premávke v queue.
  if (fota_apply_pending()) {
    if (_fota_apply_deadline == 0) _fota_apply_deadline = futureMillis(6000);
    if (_mgr->getOutboundTotal() == 0 || millisHasNowPassed(_fota_apply_deadline)) {
      fota_clear_apply_pending();
      _fota_apply_deadline = 0;
      FOTA_DEBUG_PRINTLN("[FOTA] ACK odoslaný — spúšťam flash");
      fota_apply();   // NEVRÁTI sa pri úspechu (skok na flasher + reboot)
      FOTA_DEBUG_PRINTLN("[FOTA] flash zlyhal pred skokom (pozri vyššie)");
    }
  }

#ifdef FK_DEBUG
  // DOČASNÉ: heartbeat s build# (na detekciu verzie pri FOTA teste cez Serial)
  static unsigned long s_next_build_print = 0;
  if (s_next_build_print == 0 || millisHasNowPassed(s_next_build_print)) {
    s_next_build_print = futureMillis(5000);
    FOTA_DEBUG_PRINT("[FOTA] AALIVE build #%lu  freq=%.3f sf=%u rawrx=%lu rxpkts=%lu rxerr=%lu",
                     (unsigned long)FW_BUILD_NUMBER, _prefs.freq, (unsigned)_prefs.sf,
                     (unsigned long)_fota_raw_rx,
                     (unsigned long)radio_driver.getPacketsRecv(),
                     (unsigned long)radio_driver.getPacketsRecvErrors());
#ifdef FK_DEBUG_STACKTRACE
    // [FK_DEBUG_STACKTRACE — na ZÁVER vyhodiť] stack loop-tasku (4096 B celkom).
    // stk_minfree = historické MINIMUM voľného (najhlbší bod od bootu; dominuje Ed25519
    // verify advertu). nowused = aktuálne (plytké, idle) použitie.
    FOTA_DEBUG_PRINT(" stk_minfree=%uB",
                     (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
#if defined(INCLUDE_pxTaskGetStackStart) && INCLUDE_pxTaskGetStackStart == 1
    FOTA_DEBUG_PRINT(" nowused=%luB/4096", (unsigned long)loop_stack_used_now());
#endif
#endif // FK_DEBUG_STACKTRACE
    // Odhad zahodených/prepísaných paketov v rádiu (RxDone IRQ bez prečítania):
    //   miss = isr_events - TX_sent - rx_ok - rx_crc_err
    FOTA_DEBUG_PRINTLN(" isr=%lu miss=%ld nf=%d",
                       (unsigned long)radio_driver.getIsrEvents(),
                       (long)radio_driver.getIsrEvents()
                         - (long)radio_driver.getPacketsSent()
                         - (long)radio_driver.getPacketsRecv()
                         - (long)radio_driver.getPacketsRecvErrors(),
                       (int)_radio->getNoiseFloor());
  }
#endif // FK_DEBUG
}

#endif  // WITH_LORA_FOTA

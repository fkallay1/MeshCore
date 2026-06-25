// =====================================================================
// OtaReceiver.cpp — OTA prijímač (MeshCore port z FK_lora-sniffer)
//
// Spracováva dešifrovaný OTA payload, ukladá chunky do CustomLFS append-logu,
// po COMPLETE zostaví patch.bin a overí SHA256. Reboot-resilient (meta+bitmap).
// =====================================================================
#ifdef WITH_LORA_OTA
#include "OtaReceiver.h"
#include "OtaFs.h"
#include <Arduino.h>
#include <SHA256.h>          // rweather/Crypto — rovnaká dep ako mesh::Utils
#include <Ed25519.h>         // rweather/Crypto —Ed25519::verify()

// Globálna inštancia OtaFS — CustomLFS na 0xD4000 (92kB)
CustomLFS OtaFS(OTA_FS_FLASH_ADDR, OTA_FS_FLASH_SIZE, OTA_FS_BLOCK_SIZE);

static bool verify_header_signature(const uint8_t* sig,
                                    const uint8_t* msg, size_t msg_len,
                                    uint8_t key_id) {
    for (int i = 0; i < s_author_count; i++) {
        if (s_authors[i].id == key_id) {
            return Ed25519::verify(sig, s_authors[i].pub_key, msg, msg_len);
        }
    }
    Serial.print(F("[OTA] UNKNOWN key_id=0x"));
    Serial.println(key_id, HEX);
    return false;
}

// =====================================================================
// RAM stav
// =====================================================================
static OtaState   ota;
static uint16_t   s_bitmap_dirty = 0;

// Offset tabuľka pre assembly krok: byte offset DATA v recv.log pre každý chunk.
// 4B × 1024 = 4KB BSS, acceptable pre nRF52840 (248KB RAM).
static uint32_t s_log_data_offset[OTA_MAX_CHUNKS];

// =====================================================================
// Veľkosť bežiaceho FW image — z linker symbolov (žiadna zmena ld scriptu,
// žiadny post-build). nrf52_common.ld exportuje __etext (= LMA .data, t.j.
// koniec .text/.exidx vo flashi), __data_start__/__data_end__ (VMA .data
// v RAM) a __flash_arduino_start (= ORIGIN(FLASH), reálny app base z aktívneho
// ld). .data má v RAM rovnakú veľkosť ako jej flash LMA-obraz, takže:
//   image_end = __etext + (__data_end__ - __data_start__)
//   fw_size   = image_end - __flash_arduino_start
// Zhoduje sa s firmware.bin z DFU zipu (= old_fw_size od sendera). Overené.
//
// Base berieme z linker symbolu __flash_arduino_start (NIE hardcoded
// APP_FLASH_START) — symbol vždy odráža reálny link base aktívneho ld scriptu
// (v6=0x26000, v7=0x27000) a je tak robustnejší zdroj pravdy než makro.
// APP_FLASH_START/MAX makrá ostávajú pre compile-time kontexty (symbol tam
// nie je konštantný výraz). Hodnoty sú link-time konštanty (relokácie) — pri
// kompilácii neznáme, ale to runtime aritmetike nevadí.
// =====================================================================
extern "C" {
    extern char __etext;
    extern char __data_start__;
    extern char __data_end__;
    extern char __flash_arduino_start;   // = ORIGIN(FLASH) = app base
}

static inline uint32_t fw_image_size(void) {
    uint32_t data_size = (uint32_t)(uintptr_t)&__data_end__
                       - (uint32_t)(uintptr_t)&__data_start__;
    uint32_t image_end = (uint32_t)(uintptr_t)&__etext + data_size;
    return image_end - (uint32_t)(uintptr_t)&__flash_arduino_start;
}

// Cross-check: zodpovedá deklarovaná old_fw_size reálne bežiacemu FW?
// Lacná brána pred drahým SHA256 — ak veľkosť nesedí, base FW je iný.
static bool ota_fw_size_matches(uint32_t fw_size) {
    uint32_t self = fw_image_size();
    if (fw_size == self) return true;
    Serial.print(F("[OTA] base FW: old_fw_size ")); Serial.print(fw_size);
    Serial.print(F(" != bežiace ")); Serial.println(self);
    return false;
}

// =====================================================================
// Base FW cache — rýchla validácia bez reštartu SHA256 výpočtu
// =====================================================================
static bool ota_base_fw_validated(uint32_t fw_size, const uint8_t* prefix) {
    // Cross-check oproti bežiacemu FW (z linker symbolov) — odmietni hneď
    // bez SHA256, ak patch cieli na iný base.
    if (!ota_fw_size_matches(fw_size)) return false;
    // Ak je cache plná a veľkosť sedí → porovnaj prefix
    if (ota.base_fw_size == fw_size) {
        return memcmp(ota.base_fw_sha256, prefix, 4) == 0;
    }
    // Veľkosť sa zmenila alebo cache prázdna → re-počítaj
    if (fw_size > APP_FLASH_MAX) {
        Serial.print(F("[OTA] base FW: fw_size ")); Serial.print(fw_size);
        Serial.println(F(" > APP_FLASH_MAX"));
        return false;
    }
    SHA256 sha;
    sha.update((const void*)APP_FLASH_START, fw_size);
    uint8_t h[32];
    sha.finalize(h, sizeof(h));
    ota.base_fw_size = fw_size;
    memcpy(ota.base_fw_sha256, h, 32);
    Serial.print(F("[OTA] base FW cached: size=")); Serial.print(fw_size);
    Serial.print(F("B sha256="));
    for (int i = 0; i < 4; i++) { if (h[i] < 0x10) Serial.print('0'); Serial.print(h[i], HEX); }
    Serial.println(F("..."));
    return memcmp(ota.base_fw_sha256, prefix, 4) == 0;
}

// Overenie base FW cez kompletný SHA256 (pre HEADER s full old_sha256)
static bool ota_base_fw_check_full(uint32_t fw_size, const uint8_t* sha256_full) {
    if (!ota_fw_size_matches(fw_size)) return false;
    if (fw_size > APP_FLASH_MAX) return false;
    SHA256 sha;
    sha.update((const void*)APP_FLASH_START, fw_size);
    uint8_t h[32];
    sha.finalize(h, sizeof(h));
    ota.base_fw_size = fw_size;
    memcpy(ota.base_fw_sha256, h, 32);
    return memcmp(h, sha256_full, 32) == 0;
}

// =====================================================================
// Interné pomocné funkcie
// =====================================================================
static void ota_clear() {
    memset(&ota, 0, sizeof(ota));
    ota.status = OTA_ST_IDLE;
    s_bitmap_dirty = 0;
}

static void ota_set_error(uint8_t code) {
    ota.status   = OTA_ST_ERROR;
    ota.err_code = code;
    Serial.print(F("[OTA] CHYBA=0x")); Serial.println(code, HEX);
}

// Kernighan bit count
static uint16_t bitmap_popcount() {
    uint16_t n = 0, bytes = (ota.total_chunks + 7u) / 8u;
    for (uint16_t i = 0; i < bytes; i++) {
        uint8_t b = ota.bitmap[i];
        while (b) { n++; b &= b - 1u; }
    }
    return n;
}

// =====================================================================
// CustomLFS — meta.bin
// =====================================================================
static bool save_meta() {
    OtaMetaPersist mp;
    mp.magic        = OTA_META_MAGIC;
    mp.status       = ota.status;
    mp.err_code     = ota.err_code;
    mp.total_chunks = ota.total_chunks;
    mp.patch_size   = ota.patch_size;
    memcpy(mp.patch_sha256, ota.patch_sha256, 32);
    memcpy(mp.new_sha256,   ota.new_sha256,   32);
    mp.old_fw_size = ota.old_fw_size;
    memcpy(mp.old_sha256,   ota.old_sha256,   32);
    mp.ota_prot_inf = ota.ota_prot_inf;
    mp.meta_recv    = ota.meta_recv;
    mp.sig_recv     = ota.sig_recv;
    mp.hdr_key_id   = ota.hdr_key_id;
    memcpy(mp.hdr_sig, ota.hdr_sig, 64);
    mp.crc16 = ota_crc16((const uint8_t*)&mp, (uint16_t)(sizeof(mp) - 2u));

    OtaFS.remove(OTA_FS_META);
    File f(OtaFS);
    if (!f.open(OTA_FS_META, FILE_O_WRITE)) {
        Serial.println(F("[OTA] meta: zápis zlyhal")); return false;
    }
    bool ok = (f.write((const uint8_t*)&mp, sizeof(mp)) == (int)sizeof(mp));
    f.close();
    return ok;
}

static bool load_meta(OtaMetaPersist* out) {
    File f(OtaFS);
    if (!f.open(OTA_FS_META, FILE_O_READ)) return false;
    bool ok = (f.read((uint8_t*)out, sizeof(*out)) == (int)sizeof(*out));
    f.close();
    if (!ok) return false;
    if (out->magic != OTA_META_MAGIC) return false;
    uint16_t crc = ota_crc16((const uint8_t*)out, (uint16_t)(sizeof(*out) - 2u));
    return crc == out->crc16;
}

// =====================================================================
// CustomLFS — bitmap.bin
// =====================================================================
static void save_bitmap() {
    if (ota.total_chunks == 0) return;
    uint16_t nbytes = (ota.total_chunks + 7u) / 8u;
    OtaFS.remove(OTA_FS_BITMAP);
    File f(OtaFS);
    if (!f.open(OTA_FS_BITMAP, FILE_O_WRITE)) return;
    f.write(ota.bitmap, nbytes);
    f.close();
    s_bitmap_dirty = 0;
}

static bool load_bitmap() {
    if (ota.total_chunks == 0) return false;
    uint16_t nbytes = (ota.total_chunks + 7u) / 8u;
    File f(OtaFS);
    if (!f.open(OTA_FS_BITMAP, FILE_O_READ)) return false;
    bool ok = (f.read(ota.bitmap, nbytes) == (int)nbytes);
    f.close();
    return ok;
}

// =====================================================================
// CustomLFS — recv.log (append log)
// Každý záznam: [idx 2B LE][data_len 2B LE][data N]
// =====================================================================
static bool log_append(uint16_t idx, const uint8_t* data, uint16_t data_len) {
    File f(OtaFS);
    if (!f.open(OTA_FS_LOG, FILE_O_WRITE)) {
        Serial.println(F("[OTA] log: zápis zlyhal")); return false;
    }
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(idx);       hdr[1] = (uint8_t)(idx >> 8);
    hdr[2] = (uint8_t)(data_len);  hdr[3] = (uint8_t)(data_len >> 8);
    f.write(hdr, 4);
    f.write(data, data_len);
    f.close();
    return true;
}

// =====================================================================
// Assembly z recv.log — spoločné pomocné funkcie
// =====================================================================
// Presná dĺžka chunku i (bez AES paddingu): plné chunky = OTA_CHUNK_DATA_MAX,
// posledný = zvyšok z patch_size.
static uint16_t chunk_exp_len(uint16_t i) {
    if (i < ota.total_chunks - 1u) return OTA_CHUNK_DATA_MAX;
    uint32_t rem = ota.patch_size - (uint32_t)(ota.total_chunks - 1u) * OTA_CHUNK_DATA_MAX;
    return (rem > OTA_CHUNK_DATA_MAX) ? OTA_CHUNK_DATA_MAX : (uint16_t)rem;
}

// Prechod 1: naplň s_log_data_offset[] z recv.log (posledný výskyt idx vyhrá).
static void build_log_offsets(File& log_r) {
    memset(s_log_data_offset, 0xFF, sizeof(s_log_data_offset));
    uint32_t log_pos = 0;
    uint8_t  hdr[4];
    log_r.seek(0);
    while (log_r.read(hdr, 4) == 4) {
        uint16_t idx      = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
        uint16_t data_len = (uint16_t)(hdr[2] | ((uint16_t)hdr[3] << 8));
        if (idx < ota.total_chunks) s_log_data_offset[idx] = log_pos + 4;
        log_pos += 4u + data_len;
        log_r.seek(log_pos);
    }
}

// Zostaví patch z recv.log priamo do RAM (buf, kapacita cap).
// Vráti zostavenú veľkosť (== ota.patch_size) alebo 0 pri chybe/chýbajúcom chunku.
static uint32_t assemble_log_to_buf(uint8_t* buf, uint32_t cap) {
    if (ota.total_chunks == 0 || ota.patch_size == 0 || ota.patch_size > cap) return 0;
    File log_r(OtaFS);
    if (!log_r.open(OTA_FS_LOG, FILE_O_READ)) return 0;
    build_log_offsets(log_r);
    uint32_t out_pos = 0;
    for (uint16_t i = 0; i < ota.total_chunks; i++) {
        if (s_log_data_offset[i] == 0xFFFFFFFFu) { log_r.close(); return 0; }
        uint16_t exp_len = chunk_exp_len(i);
        if (out_pos + exp_len > cap) { log_r.close(); return 0; }
        log_r.seek(s_log_data_offset[i]);
        if (log_r.read(buf + out_pos, exp_len) != (int)exp_len) { log_r.close(); return 0; }
        out_pos += exp_len;
    }
    log_r.close();
    return out_pos;
}

#ifndef USE_PATCHBIN_FILE
// RAM mód: SHA256 patchu streamovo z recv.log (bez patch.bin, bez veľkého buffra).
static bool verify_log_sha() {
    File log_r(OtaFS);
    if (!log_r.open(OTA_FS_LOG, FILE_O_READ)) {
        Serial.println(F("[OTA] log: čítanie zlyhal")); return false;
    }
    build_log_offsets(log_r);
    SHA256 sha;
    uint8_t buf[OTA_CHUNK_DATA_MAX];
    for (uint16_t i = 0; i < ota.total_chunks; i++) {
        if (s_log_data_offset[i] == 0xFFFFFFFFu) {
            Serial.print(F("[OTA] chýba chunk ")); Serial.println(i);
            log_r.close(); return false;
        }
        uint16_t exp_len = chunk_exp_len(i);
        log_r.seek(s_log_data_offset[i]);
        if (log_r.read(buf, exp_len) != (int)exp_len) { log_r.close(); return false; }
        sha.update(buf, exp_len);
    }
    log_r.close();
    uint8_t hash[32];
    sha.finalize(hash, sizeof(hash));
    if (memcmp(hash, ota.patch_sha256, 32) != 0) {
        Serial.print(F("[OTA] SHA256 NESÚHLASÍ  got="));
        for (int i = 0; i < 8; i++) { if (hash[i] < 0x10) Serial.print('0'); Serial.print(hash[i], HEX); }
        Serial.println(F("..."));
        return false;
    }
    return true;
}
#endif

// =====================================================================
// Assembly + SHA256 verifikácia (po COMPLETE)
//   default:           over SHA streamovo z recv.log, NEpíš patch.bin (úspora FS)
//   USE_PATCHBIN_FILE: zostav recv.log → patch.bin (FS) + over SHA, zmaž recv.log
// =====================================================================
static bool assemble_and_verify() {
#ifndef USE_PATCHBIN_FILE
    Serial.println(F("[OTA] Overujem patch SHA256 (RAM, bez patch.bin)..."));
    if (!verify_log_sha()) return false;
    Serial.println(F("[OTA] patch SHA256 OK (recv.log ostáva ako zdroj)"));
    return true;
#else
    Serial.println(F("[OTA] Zostavujem patch.bin..."));
    File log_r(OtaFS);
    if (!log_r.open(OTA_FS_LOG, FILE_O_READ)) {
        Serial.println(F("[OTA] log: čítanie zlyhal")); return false;
    }
    build_log_offsets(log_r);

    OtaFS.remove(OTA_FS_PATCH);
    File out_f(OtaFS);
    if (!out_f.open(OTA_FS_PATCH, FILE_O_WRITE)) { log_r.close(); return false; }

    SHA256 sha;
    bool   ok = true;
    uint8_t buf[OTA_CHUNK_DATA_MAX];
    for (uint16_t i = 0; i < ota.total_chunks && ok; i++) {
        if (s_log_data_offset[i] == 0xFFFFFFFFu) {
            Serial.print(F("[OTA] chýba chunk ")); Serial.println(i); ok = false; break;
        }
        uint16_t exp_len = chunk_exp_len(i);
        log_r.seek(s_log_data_offset[i]);
        int n = log_r.read(buf, exp_len);
        if (n != (int)exp_len) { ok = false; break; }
        sha.update(buf, (size_t)n);
        out_f.write(buf, (size_t)n);
    }
    log_r.close();
    out_f.close();
    if (!ok) { Serial.println(F("[OTA] zostava zlyhala")); return false; }

    uint8_t hash[32];
    sha.finalize(hash, sizeof(hash));
    if (memcmp(hash, ota.patch_sha256, 32) != 0) {
        Serial.print(F("[OTA] SHA256 NESÚHLASÍ  got="));
        for (int i = 0; i < 8; i++) { if (hash[i] < 0x10) Serial.print('0'); Serial.print(hash[i], HEX); }
        Serial.println(F("..."));
        return false;
    }
    Serial.println(F("[OTA] patch.bin SHA256 OK"));
    OtaFS.remove(OTA_FS_LOG);   // recv.log cleanup — patch.bin je odteraz zdroj
    Serial.println(F("[OTA] recv.log zmazaný (patch.bin je zdroj)"));
    return true;
#endif
}

// =====================================================================
// ota_acquire_patch_ram — patch do čerstvo malloc-nutého RAM buffra.
//   default:           zostaví z recv.log (žiadny patch.bin, žiadny 2× FS)
//   USE_PATCHBIN_FILE: prečíta /ota/patch.bin
// Caller uvoľní cez free(). *out_size = veľkosť. NULL pri chybe/malloc zlyhaní.
// =====================================================================
uint8_t* ota_acquire_patch_ram(uint32_t* out_size) {
#ifdef USE_PATCHBIN_FILE
    File f(OtaFS);
    if (!f.open(OTA_FS_PATCH, FILE_O_READ)) { Serial.println(F("[OTA] patch.bin chýba")); return nullptr; }
    uint32_t sz = (uint32_t)f.size();
    if (sz == 0 || sz > OTA_FS_FLASH_SIZE) { f.close(); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) { f.close(); Serial.print(F("[OTA] malloc ")); Serial.print(sz); Serial.println(F("B zlyhal")); return nullptr; }
    bool ok = ((uint32_t)f.read(buf, sz) == sz);
    f.close();
    if (!ok) { free(buf); Serial.println(F("[OTA] čítanie patch.bin zlyhalo")); return nullptr; }
    *out_size = sz;
    return buf;
#else
    uint32_t sz = ota.patch_size;
    if (sz == 0 || sz > OTA_FS_FLASH_SIZE) { Serial.println(F("[OTA] neplatná patch_size")); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) {
        Serial.print(F("[OTA] malloc ")); Serial.print(sz);
        Serial.println(F("B zlyhal (RAM assembly) — pre veľké patche skús -D USE_PATCHBIN_FILE"));
        return nullptr;
    }
    if (assemble_log_to_buf(buf, sz) != sz) {
        free(buf); Serial.println(F("[OTA] RAM assembly z recv.log zlyhala")); return nullptr;
    }
    *out_size = sz;
    return buf;
#endif
}

// =====================================================================
// Resume po reboote
// =====================================================================
static void try_resume() {
    OtaMetaPersist mp;
    if (!load_meta(&mp)) return;

    if (!(mp.status & (OTA_ST_RECEIVING | OTA_ST_COMPLETE | OTA_ST_VERIFIED | OTA_ST_DONE))) return;
    if (mp.total_chunks == 0 || mp.total_chunks > OTA_MAX_CHUNKS) return;

    ota.total_chunks = mp.total_chunks;
    ota.patch_size   = mp.patch_size;
    memcpy(ota.patch_sha256, mp.patch_sha256, 32);
    memcpy(ota.new_sha256,   mp.new_sha256,   32);
    ota.old_fw_size = mp.old_fw_size;
    memcpy(ota.old_sha256,   mp.old_sha256,   32);
    ota.ota_prot_inf = mp.ota_prot_inf;
    ota.meta_recv    = mp.meta_recv;
    ota.sig_recv     = mp.sig_recv;
    ota.hdr_key_id   = mp.hdr_key_id;
    memcpy(ota.hdr_sig, mp.hdr_sig, 64);
    ota.status   = mp.status;
    ota.err_code = mp.err_code;

    load_bitmap();
    ota.recv_count = bitmap_popcount();

    Serial.print(F("[OTA] RESUME "));
    Serial.print(ota.recv_count); Serial.print('/');
    Serial.print(ota.total_chunks); Serial.print(F(" chunks  st=0x"));
    Serial.println(ota.status, HEX);
}

// =====================================================================
// Verejné API
// =====================================================================
void ota_init() {
    ota_clear();
    if (!OtaFS.begin()) {
        // Po flasheri je 0xD4000 prepísaný raw patch dátami — reformátuj
        Serial.println(F("[OTA] FS poškodený (post-flash?), reformátujem..."));
        OtaFS.format();
        if (!OtaFS.begin()) {
            Serial.println(F("[OTA] FS: format+begin zlyhalo — FS nedostupný"));
        }
    }
    OtaFS.mkdir(OTA_FS_DIR);
    try_resume();
    Serial.println(F("[OTA] init  (CustomLFS 92kB @ 0xD4000)"));
}

const OtaState* ota_get_state() { return &ota; }

void ota_print_status() {
    Serial.print(F("[OTA] "));
    Serial.print(ota.recv_count); Serial.print('/'); Serial.print(ota.total_chunks);
    Serial.print(F("  st=0x")); Serial.print(ota.status, HEX);
    Serial.print(F("  size=")); Serial.print(ota.patch_size);
    if (ota.err_code) { Serial.print(F("  err=0x")); Serial.print(ota.err_code, HEX); }
    Serial.println();
}

void ota_send_nack() {
    if (ota.total_chunks == 0) return;
    uint16_t missing[OTA_NACK_MAX_IDX];
    uint8_t  cnt = 0;
    for (uint16_t i = 0; i < ota.total_chunks && cnt < OTA_NACK_MAX_IDX; i++)
        if (!OTA_BIT_GET(ota.bitmap, i))
            missing[cnt++] = i;

    Serial.print(F("[OTA] NACK missing=")); Serial.print(cnt);
    if (cnt) {
        Serial.print(F("  [")); Serial.print(missing[0]);
        if (cnt > 1) { Serial.print(F("..")); Serial.print(missing[cnt-1]); }
        Serial.print(']');
    }
    Serial.println();
}

// Postav STATUS paket (6B). Vždy dostupný ak je session.
int ota_build_status(uint8_t* out) {
    if (ota.total_chunks == 0) return 0;
    OtaStatusPkt* p = (OtaStatusPkt*)out;
    p->type         = OTA_PKT_STATUS;
    p->recv_count   = ota.recv_count;
    p->total_chunks = ota.total_chunks;
    p->status       = ota.status;
    return (int)sizeof(OtaStatusPkt);
}

// Postav NACK paket (2 + count*2). Vracia 0 ak nič nechýba.
int ota_build_nack(uint8_t* out) {
    if (ota.total_chunks == 0) return 0;
    OtaNackPkt* p = (OtaNackPkt*)out;
    p->type  = OTA_PKT_NACK;
    p->count = 0;
    for (uint16_t i = 0; i < ota.total_chunks && p->count < OTA_NACK_MAX_IDX; i++)
        if (!OTA_BIT_GET(ota.bitmap, i))
            p->idx[p->count++] = i;
    if (p->count == 0) return 0;
    return 2 + (int)p->count * 2;
}

// =====================================================================
// Spracovanie OTA_HEADER
// =====================================================================
// Zrekonštruuje 102 B META z uložených polí (MUSÍ byť bajt-identické s OtaHeaderPkt
// a s tým, čo podpísal sender — inak Ed25519 verify zlyhá).
static void rebuild_meta(uint8_t out[102]) {
    out[0] = OTA_PKT_HEADER;
    out[1] = ota.ota_prot_inf;
    memcpy(out + 2,  &ota.patch_size, 4);
    memcpy(out + 6,  ota.patch_sha256, 32);
    memcpy(out + 38, ota.new_sha256, 32);
    memcpy(out + 70, ota.old_sha256, 32);
}

// Keď máme META aj SIG → over podpis a "promuj" hlavičku (nastav total_chunks).
// Bezpečnostný invariant: total_chunks (a teda completion/flash) sa nastaví LEN po
// úspešnom overení podpisu nad rekonštruovanou 102 B META.
static void try_verify_header() {
    if (!(ota.meta_recv && ota.sig_recv)) return;
    if (ota.total_chunks > 0) return;            // už promované

    uint8_t meta[102];
    rebuild_meta(meta);
    bool ok;
#ifdef OTA_ALLOW_UNSIGNED
    bool is_unsigned = (ota.hdr_sig[0] == 0 && ota.hdr_sig[1] == 0 &&
                        ota.hdr_sig[2] == 0 && ota.hdr_sig[3] == 0);
    if (is_unsigned) { Serial.println(F("[OTA] HEADER: UNSIGNED (OTA_ALLOW_UNSIGNED)")); ok = true; }
    else
#endif
    ok = verify_header_signature(ota.hdr_sig, meta, 102u, ota.hdr_key_id);

    if (!ok) {
        Serial.println(F("[OTA] HEADER: INVALID signature — rejecting"));
        ota_set_error(OTA_ERR_SIGNATURE);
        return;
    }

    uint32_t tc = (ota.patch_size + OTA_CHUNK_DATA_MAX - 1u) / OTA_CHUNK_DATA_MAX;
    if (tc == 0 || tc > OTA_MAX_CHUNKS) {
        Serial.print(F("[OTA] HEADER: zlé total_chunks=")); Serial.println(tc); return;
    }
    ota.total_chunks = (uint16_t)tc;
    ota.recv_count   = bitmap_popcount();
    ota.status       = OTA_ST_RECEIVING;
    save_bitmap();
    save_meta();
    Serial.print(F("[OTA] HEADER OK (META+SIG overené) chunks=")); Serial.print(tc);
    Serial.print(F("  mám ")); Serial.print(ota.recv_count); Serial.println(F(" chunkov"));

    // Chunky mohli doraziť pred hlavičkou → over COMPLETE hneď
    if (ota.recv_count >= ota.total_chunks) {
        ota.status |= OTA_ST_COMPLETE;
        save_meta();
        Serial.println(F("[OTA] COMPLETE — assembly + SHA256..."));
        if (assemble_and_verify()) {
            ota.status |= OTA_ST_VERIFIED; save_meta();
            Serial.println(F("[OTA] VERIFIED — 'ota verify'=dry-run | 'ota flash'=flash+reboot"));
        } else {
            ota_set_error(OTA_ERR_SHA256); save_meta();
        }
    }
}

// OTA_PKT_HEADER = META (metadáta patchu, podpisované). Idempotentné (opätovné
// prijatie len prepíše rovnaké polia). Verify+promócia spraví try_verify_header.
static void handle_meta(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(OtaHeaderPkt)) { Serial.println(F("[OTA] META: krátky")); return; }
    const OtaHeaderPkt* pkt = (const OtaHeaderPkt*)plain;

    // Base FW gating cez cache (META.old_sha256 musí sedieť s bežiacim FW)
    if (ota.base_fw_size > 0 && !ota_base_fw_check_full(ota.base_fw_size, pkt->old_sha256)) {
        Serial.println(F("[OTA] META: base FW nezhoda — iný FW beží na zariadení"));
        ota_set_error(OTA_ERR_BASEFW);
        return;
    }

    // Re-send identickej META? Spočítaj PRED prípadným ota_clear (ten zeruje
    // patch_sha256). Ak je to DUP, preskočíme save_meta() — flash-zápis blokuje
    // RX cestu (nRF52 NVMC halt) a spôsobí stratu nasledujúceho SIG/APPLY paketu.
    bool dup_meta = ota.meta_recv && (ota.status & OTA_ST_RECEIVING)
                 && ota.patch_size == pkt->patch_size
                 && memcmp(ota.patch_sha256, pkt->patch_sha256, 32) == 0;

    bool partial = (ota.status & OTA_ST_RECEIVING) && ota.total_chunks == 0;
    bool other_patch = (ota.status & OTA_ST_RECEIVING) && ota.total_chunks > 0 &&
                       memcmp(ota.patch_sha256, pkt->patch_sha256, 32) != 0;
    if (!(ota.status & OTA_ST_RECEIVING) || other_patch) {
        // Nová session (alebo iný patch beží) — vyčisti FS
        OtaFS.remove(OTA_FS_LOG);
        OtaFS.remove(OTA_FS_PATCH);
        OtaFS.remove(OTA_FS_BITMAP);
        ota_clear();
        ota.status = OTA_ST_RECEIVING;
        ota.total_chunks = 0;
    }
    (void)partial;   // partial chunky sa zachovajú (merge), nič nemažeme

    ota.ota_prot_inf = pkt->ota_prot_inf;
    ota.patch_size   = pkt->patch_size;
    memcpy(ota.patch_sha256, pkt->patch_sha256, 32);
    memcpy(ota.new_sha256,   pkt->new_sha256,   32);
    memcpy(ota.old_sha256,   pkt->old_sha256,   32);
    ota.meta_recv = 1;
    if (!dup_meta) save_meta();   // DUP re-send → žiadny flash zápis (nestalluj RX)
    Serial.print(F("[OTA] META prijaté patch_size=")); Serial.print(pkt->patch_size);
    Serial.print(F("B  patch_sha256="));
    for (int i = 0; i < 6; i++) { if (pkt->patch_sha256[i] < 0x10) Serial.print('0'); Serial.print(pkt->patch_sha256[i], HEX); }
    Serial.print(F("..."));
    Serial.println(dup_meta ? F("  meta_recv=1 DUP → skip save")
                            : F("  NEW/CHANGED → save"));
    try_verify_header();
}

// OTA_PKT_HDR_SIG = SIG (Ed25519 podpis META). Gating cez old_sha256.
static void handle_sig(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(OtaHdrSigPkt)) { Serial.println(F("[OTA] SIG: krátky")); return; }
    const OtaHdrSigPkt* pkt = (const OtaHdrSigPkt*)plain;

    if (ota.meta_recv) {
        if (memcmp(ota.old_sha256, pkt->old_sha256, 32) != 0) {
            Serial.println(F("[OTA] SIG: old_sha256 nezhoda s META — drop")); return;
        }
    } else if (ota.base_fw_size > 0 && !ota_base_fw_check_full(ota.base_fw_size, pkt->old_sha256)) {
        Serial.println(F("[OTA] SIG: base FW nezhoda — drop")); return;
    }
    if (!(ota.status & OTA_ST_RECEIVING)) { ota.status = OTA_ST_RECEIVING; ota.total_chunks = 0; }

    // Re-send identického SIG? DUP → preskoč save_meta() (rovnaký dôvod ako META).
    bool dup_sig = ota.sig_recv && ota.hdr_key_id == pkt->key_id
                && memcmp(ota.hdr_sig, pkt->signature, 64) == 0;
    ota.hdr_key_id = pkt->key_id;
    memcpy(ota.hdr_sig, pkt->signature, 64);
    ota.sig_recv = 1;
    if (!dup_sig) save_meta();   // DUP re-send → žiadny flash zápis (nestalluj RX)
    Serial.print(F("[OTA] SIG prijaté key_id=0x")); Serial.print(pkt->key_id, HEX);
    Serial.println(dup_sig ? F("  sig_recv=1 DUP → skip save")
                           : F("  NEW/CHANGED → save"));
    try_verify_header();
}

// =====================================================================
// Spracovanie OTA_CHUNK
// =====================================================================
static void handle_chunk(const uint8_t* plain, int plen) {
    if (plen < 13) return;  // min: type(1)+idx(2)+crc(2)+old_fw_size(4)+prefix(4) = 13B

    const OtaChunkPkt* pkt = (const OtaChunkPkt*)plain;
    uint16_t idx = pkt->chunk_idx;
    uint16_t rx_crc = pkt->crc16;
    const uint8_t* data = pkt->data;
    uint16_t data_len = (uint16_t)(plen - (int)(sizeof(OtaChunkPkt) - OTA_CHUNK_DATA_MAX));

    // Base FW validácia — over, že chunk je pre aktuálny FW na zariadení
    if (!ota_base_fw_validated(pkt->old_fw_size, pkt->old_sha256_prefix)) {
        Serial.println(F("[OTA] CHUNK: base FW nezhoda — drop"));
        return;
    }

    // Session init alebo merge:
    //  - žiadna session → vytvor partial (total_chunks=0, base z chunku);
    //    skoré chunky sa rovno bufferujú, HEADER ich neskôr "promuje".
    //  - existujúca session s INÝM base FW → ignoruj (nemiešaj patche).
    if (!(ota.status & OTA_ST_RECEIVING)) {
        OtaFS.remove(OTA_FS_LOG);
        OtaFS.remove(OTA_FS_PATCH);
        OtaFS.remove(OTA_FS_BITMAP);
        ota_clear();
        ota.old_fw_size = pkt->old_fw_size;
        memcpy(ota.old_sha256, pkt->old_sha256_prefix, 4);  // zvyšok doplní HEADER
        ota.status = OTA_ST_RECEIVING;
        ota.total_chunks = 0;  // čaká HEADER (alebo promóciu)
        save_meta();
        Serial.println(F("[OTA] CHUNK: partial session z chunku (čaká HEADER)"));
    } else if (memcmp(ota.old_sha256, pkt->old_sha256_prefix, 4) != 0) {
        return;  // chunk patrí inému base FW než bežiaca session
    }
    // HEADER nenesie old_fw_size — session ho preberá z chunku (base už overený
    // vyššie cez ota_base_fw_validated). Bez tohto by ostal 0 po HEADER ceste.
    ota.old_fw_size = pkt->old_fw_size;

    // Hranica idx: kým nepoznáme total_chunks (pred HEADER), bufferuj až po MAX.
    uint16_t max_idx = (ota.total_chunks > 0) ? ota.total_chunks : (uint16_t)OTA_MAX_CHUNKS;
    if (idx >= max_idx) {
        if (ota.total_chunks > 0) ota_set_error(OTA_ERR_OVERFLOW);
        return;
    }

    // Presná dĺžka chunku — AES-ECB dopĺňa plaintext na 16B blok; padding treba
    // strhnúť, inak CRC (sender ráta cez presnú dĺžku) nesedí. Posledný chunk má
    // dĺžku z patch_size, tú poznáme až po HEADER — preto sa posledný chunk PRED
    // HEADER neuloží (CRC zlyhá na paddingu) a príde znova v ďalšom cykle.
    uint16_t exp_len = OTA_CHUNK_DATA_MAX;
    if (ota.total_chunks > 0 && idx == (uint16_t)(ota.total_chunks - 1u)) {
        uint32_t rem = ota.patch_size - (uint32_t)(ota.total_chunks - 1u) * OTA_CHUNK_DATA_MAX;
        exp_len = (rem > OTA_CHUNK_DATA_MAX) ? OTA_CHUNK_DATA_MAX : (uint16_t)rem;
    }
    if (data_len > exp_len) data_len = exp_len;

    uint16_t calc_crc = ota_crc16(data, data_len);
    if (calc_crc != rx_crc) {
        Serial.print(F("[OTA] CRC ERR idx=")); Serial.print(idx);
        Serial.print(F("  exp=0x")); Serial.print(rx_crc, HEX);
        Serial.print(F("  got=0x")); Serial.println(calc_crc, HEX);
        return;
    }

    if (OTA_BIT_GET(ota.bitmap, idx)) return;  // duplikát s OK CRC

    if (!log_append(idx, data, data_len)) {
        ota_set_error(OTA_ERR_STORAGE); return;
    }

    OTA_BIT_SET(ota.bitmap, idx);
    ota.recv_count++;

    s_bitmap_dirty++;
    bool complete = (ota.total_chunks > 0) && (ota.recv_count >= ota.total_chunks);
    if (s_bitmap_dirty >= OTA_BITMAP_SAVE_EVERY || complete)
        save_bitmap();

    if (ota.recv_count % 20 == 0 || complete) {
        Serial.print(F("[OTA] ")); Serial.print(ota.recv_count);
        Serial.print('/'); Serial.println(ota.total_chunks);
    }

    if (complete) {
        ota.status |= OTA_ST_COMPLETE;
        save_meta();
        Serial.println(F("[OTA] COMPLETE — assembly + SHA256..."));

        if (assemble_and_verify()) {
            ota.status |= OTA_ST_VERIFIED;
            save_meta();
            Serial.println(F("[OTA] VERIFIED — 'ota verify'=dry-run | 'ota flash'=flash+reboot"));
        } else {
            ota_set_error(OTA_ERR_SHA256);
            save_meta();
        }
    }
}

// =====================================================================
// Spracovanie OTA_APPLY
// =====================================================================
static void handle_apply(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(OtaApplyPkt)) return;
    const OtaApplyPkt* pkt = (const OtaApplyPkt*)plain;
    if (memcmp(pkt->sha256, ota.patch_sha256, 32) != 0) {
        Serial.println(F("[OTA] APPLY: SHA256 nesúhlasí")); return;
    }
    ota_apply();
}

// =====================================================================
// Výpis OTA paketu
// =====================================================================
void ota_print_pkt(const uint8_t* plain, int plen, float rssi, float snr) {
    if (plen < 1) return;
    uint8_t type = plain[0];

    Serial.print(F("[OTA] "));

    switch (type) {
        case OTA_PKT_HEADER: {   // META
            if (plen < (int)sizeof(OtaHeaderPkt)) { Serial.println(F("META (krátky)")); return; }
            const OtaHeaderPkt* p = (const OtaHeaderPkt*)plain;
            uint32_t ps; memcpy(&ps, &p->patch_size, 4);
            uint32_t tc = (ps + OTA_CHUNK_DATA_MAX - 1u) / OTA_CHUNK_DATA_MAX;
            Serial.print(F("META  v")); Serial.print(p->ota_prot_inf);
            Serial.print(F("  size="));   Serial.print(ps); Serial.print('B');
            Serial.print(F("  chunks~")); Serial.print(tc);
            Serial.print(F("  patch="));
            for (int i = 0; i < 4; i++) { if (p->patch_sha256[i] < 0x10) Serial.print('0'); Serial.print(p->patch_sha256[i], HEX); }
            Serial.print(F("...  new="));
            for (int i = 0; i < 4; i++) { if (p->new_sha256[i] < 0x10) Serial.print('0'); Serial.print(p->new_sha256[i], HEX); }
            Serial.print(F("..."));
            break;
        }
        case OTA_PKT_HDR_SIG: {  // SIG
            if (plen < (int)sizeof(OtaHdrSigPkt)) { Serial.println(F("SIG (krátky)")); return; }
            const OtaHdrSigPkt* p = (const OtaHdrSigPkt*)plain;
            Serial.print(F("SIG  v")); Serial.print(p->ota_prot_inf);
            Serial.print(F("  key_id=0x")); Serial.print(p->key_id, HEX);
            Serial.print(F("  sig="));
            for (int i = 0; i < 4; i++) { if (p->signature[i] < 0x10) Serial.print('0'); Serial.print(p->signature[i], HEX); }
            Serial.print(F("...  old="));
            for (int i = 0; i < 4; i++) { if (p->old_sha256[i] < 0x10) Serial.print('0'); Serial.print(p->old_sha256[i], HEX); }
            Serial.print(F("..."));
            break;
        }
        case OTA_PKT_CHUNK: {
            if (plen < (int)(sizeof(OtaChunkPkt) - OTA_CHUNK_DATA_MAX)) { Serial.println(F("CHUNK (krátky)")); return; }
            const OtaChunkPkt* p = (const OtaChunkPkt*)plain;
            uint16_t dlen = (uint16_t)(plen - (int)(sizeof(OtaChunkPkt) - OTA_CHUNK_DATA_MAX));
            uint16_t calc  = ota_crc16(p->data, dlen);
            bool crc_ok    = (calc == p->crc16);

            Serial.print(F("CHUNK  idx="));  Serial.print(p->chunk_idx);
            if (ota.total_chunks > 0) { Serial.print('/'); Serial.print(ota.total_chunks); }
            Serial.print(F("  len="));       Serial.print(dlen); Serial.print('B');
            Serial.print(F("  crc=0x"));     Serial.print(p->crc16, HEX);
            Serial.print(crc_ok ? F("  OK") : F("  BAD"));
            if (p->chunk_idx < OTA_MAX_CHUNKS && OTA_BIT_GET(ota.bitmap, p->chunk_idx)) Serial.print(F(" DUP"));
            Serial.print(F("  base="));
            for (int i = 0; i < 4; i++) { if (p->old_sha256_prefix[i] < 0x10) Serial.print('0'); Serial.print(p->old_sha256_prefix[i], HEX); }
            break;
        }
        case OTA_PKT_APPLY: {
            if (plen < (int)sizeof(OtaApplyPkt)) { Serial.println(F("APPLY (krátky)")); return; }
            const OtaApplyPkt* p = (const OtaApplyPkt*)plain;
            bool sha_ok = (memcmp(p->sha256, ota.patch_sha256, 32) == 0);
            Serial.print(F("APPLY  sha256="));
            for (int i = 0; i < 8; i++) { if (p->sha256[i] < 0x10) Serial.print('0'); Serial.print(p->sha256[i], HEX); }
            Serial.print(sha_ok ? F("...  OK") : F("...  NESEDÍ"));
            break;
        }
        default:
            Serial.print(F("? type=0x")); Serial.print(type, HEX);
    }

    if (rssi != 0.0f) {
        Serial.print(F("  RSSI=")); Serial.print(rssi, 1);
        Serial.print(F(" SNR="));  Serial.print(snr, 1);
    }
    Serial.println();
}

// =====================================================================
// Dispatch
// =====================================================================
bool ota_process(const uint8_t* plain, int plen) {
    if (plen < 1) return false;
    switch (plain[0]) {
        case OTA_PKT_HEADER:  handle_meta(plain, plen);  return true;
        case OTA_PKT_HDR_SIG: handle_sig(plain, plen);   return true;
        case OTA_PKT_CHUNK:   handle_chunk(plain, plen);  return true;
        case OTA_PKT_APPLY:   handle_apply(plain, plen);  return true;
        default:             return false;
    }
}

// =====================================================================
// ota_apply — spustí skutočný flash (ota_flash_via_flasher, NEVRÁTI SA pri úspechu)
// =====================================================================
bool ota_apply() {
    if (!(ota.status & OTA_ST_VERIFIED)) {
        Serial.println(F("[OTA] APPLY: nie je verifikované — spusti príjem chunkov")); return false;
    }
    uint8_t prev_status = ota.status;   // pre obnovu ak flash zlyhá (base-check a pod.)
    ota.status = OTA_ST_APPLYING;
    save_meta();

    extern bool ota_flash_via_flasher();
    if (ota_flash_via_flasher()) return true;  // NEVRÁTI SA pri úspechu

    // Flash zlyhal pred skokom (napr. base FW != old). Obnov VERIFIED.
    ota.status = prev_status;
    save_meta();
    return false;
}

// =====================================================================
// ota_clear_session — vymaže OTA súbory z FS, resetuje RAM stav
// =====================================================================
void ota_clear_session() {
    Serial.println(F("[OTA] Mazem OTA session..."));
    int removed = 0;
    const char* files[] = { OTA_FS_META, OTA_FS_BITMAP, OTA_FS_LOG, OTA_FS_PATCH };
    for (int i = 0; i < 4; i++) {
        if (OtaFS.remove(files[i])) {
            Serial.print(F("[OTA] rm "));
            Serial.println(files[i]);
            removed++;
        }
    }
    ota_clear();
    Serial.print(F("[OTA] Hotovo — vymazaných ")); Serial.print(removed);
    Serial.println(F(" súborov"));
}

#endif  // WITH_LORA_OTA

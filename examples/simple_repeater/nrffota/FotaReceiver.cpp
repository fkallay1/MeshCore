// =====================================================================
// FotaReceiver.cpp — OTA prijímač (MeshCore port z FK_lora-sniffer)
//
// Spracováva dešifrovaný OTA payload, ukladá chunky do CustomLFS append-logu,
// po COMPLETE zostaví patch.bin a overí SHA256. Reboot-resilient (meta+bitmap).
// =====================================================================
#ifdef WITH_LORA_FOTA
#include "FotaReceiver.h"
#include "FotaFs.h"
#include "FwId.h"             // fw_id_trailer (build#, image_size, sha256)
#include <Arduino.h>
#include <SHA256.h>          // rweather/Crypto — rovnaká dep ako mesh::Utils
#include <Ed25519.h>         // rweather/Crypto —Ed25519::verify()

// Globálna inštancia FotaFS — CustomLFS na 0xD4000 (92kB)
CustomLFS FotaFS(FOTA_FS_FLASH_ADDR, FOTA_FS_FLASH_SIZE, FOTA_FS_BLOCK_SIZE);

static bool verify_header_signature(const uint8_t* sig,
                                    const uint8_t* msg, size_t msg_len,
                                    uint8_t key_id) {
    for (int i = 0; i < s_author_count; i++) {
        if (s_authors[i].id == key_id) {
            return Ed25519::verify(sig, s_authors[i].pub_key, msg, msg_len);
        }
    }
    Serial.print(F("[FOTA] UNKNOWN key_id=0x"));
    Serial.println(key_id, HEX);
    return false;
}

// =====================================================================
// RAM stav
// =====================================================================
static FotaState   ota;
static uint16_t   s_bitmap_dirty = 0;

// Offset tabuľka pre assembly krok: byte offset DATA v recv.log pre každý chunk.
// 4B × 1024 = 4KB BSS, acceptable pre nRF52840 (248KB RAM).
static uint32_t s_log_data_offset[FOTA_MAX_CHUNKS];

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

// Reálny app base z linker symbolu (= ORIGIN(FLASH) aktívneho ld scriptu):
// v6=0x26000, v7=0x27000. Toto je zdroj pravdy pre device-side SHA — NIE makro
// APP_FLASH_START, ktoré je pri zlej/chýbajúcej board konfigurácii (napr. v7
// board bez FOTA_SOFTDEVICE_V7) nesprávne a hash by sa počítal z inej oblasti.
// (Flasher je standalone bez linker symbolov → tam makro ostáva, viď flash_layout.h.)
static inline uint32_t fw_flash_base(void) {
    return (uint32_t)(uintptr_t)&__flash_arduino_start;
}

// Exportované pre FotaPatcher / FotaMesh — jeden zdroj pravdy pre app base a
// veľkosť bežiaceho FW (z linker symbolov, nie z makra).
uint32_t fota_running_fw_base(void) { return fw_flash_base(); }
uint32_t fota_running_fw_size(void) { return fw_image_size(); }

static void print_sha_full(const uint8_t* h) {
    for (int i = 0; i < 32; i++) { if (h[i] < 0x10) Serial.print('0'); Serial.print(h[i], HEX); }
    Serial.println();
}

// "ota id" — vypíš FW identitu a dopočítaj plný SHA256 bežiaceho FW.
// running sha256 sa počíta nad [base, +image_size) AS-IS (vrátane vyplneného
// traileru) → ZHODUJE sa s old_sha256 v .otapkg.json (to PC počíta nad rovnakým
// app image). Trailer.sha256 je iný hash (self-hash so sha[]=0) — len referencia.
void fota_print_fw_id(char* reply) {
    uint32_t base      = fw_flash_base();
    uint32_t link_size = fw_image_size();
    uint32_t timg      = fw_id_trailer.image_size;
    uint32_t build     = fw_id_trailer.build_number;

    Serial.println(F("[FOTA] === FW identity ==="));
    Serial.print(F("[FOTA] build #            = ")); Serial.println((unsigned long)build);
    Serial.print(F("[FOTA] app base           = 0x")); Serial.println(base, HEX);
    Serial.print(F("[FOTA] image_size trailer = ")); Serial.println((unsigned long)timg);
    Serial.print(F("[FOTA] image_size linker  = ")); Serial.println((unsigned long)link_size);
    if (timg != link_size)
        Serial.println(F("[FOTA] !! POZOR: trailer != linker veľkosť — zlá board konfig?"));
    Serial.print(F("[FOTA] trailer sha256     = ")); print_sha_full(fw_id_trailer.sha256);

    uint8_t h[32]; memset(h, 0, sizeof(h));
    if (timg && timg <= (APP_FLASH_END - base)) {
        SHA256 sha;
        sha.update((const void*)base, timg);
        sha.finalize(h, sizeof(h));
        Serial.print(F("[FOTA] running sha256     = ")); print_sha_full(h);
        Serial.println(F("[FOTA] ^ porovnaj s old_sha256 v .otapkg.json"));
    } else {
        Serial.println(F("[FOTA] running sha256: image_size neplatná"));
    }
    if (reply) {
        sprintf(reply, "id b#%lu sz=%lu sha=%02X%02X%02X%02X",
                (unsigned long)build, (unsigned long)timg, h[0], h[1], h[2], h[3]);
    }
}

// Cross-check: zodpovedá deklarovaná old_fw_size reálne bežiacemu FW?
// Lacná brána pred drahým SHA256 — ak veľkosť nesedí, base FW je iný.
static bool fota_fw_size_matches(uint32_t fw_size) {
    uint32_t self = fw_image_size();
    if (fw_size == self) return true;
    Serial.print(F("[FOTA] base FW: old_fw_size ")); Serial.print(fw_size);
    Serial.print(F(" != bežiace ")); Serial.println(self);
    return false;
}

// =====================================================================
// Base FW cache — rýchla validácia bez reštartu SHA256 výpočtu
// =====================================================================
static bool fota_base_fw_validated(uint32_t fw_size, const uint8_t* prefix) {
    // Cross-check oproti bežiacemu FW (z linker symbolov) — odmietni hneď
    // bez SHA256, ak patch cieli na iný base.
    if (!fota_fw_size_matches(fw_size)) return false;
    // Ak je cache plná a veľkosť sedí → porovnaj prefix
    if (ota.base_fw_size == fw_size) {
        return memcmp(ota.base_fw_sha256, prefix, 4) == 0;
    }
    // Veľkosť sa zmenila alebo cache prázdna → re-počítaj
    if (fw_size > (APP_FLASH_END - fw_flash_base())) {
        Serial.print(F("[FOTA] base FW: fw_size ")); Serial.print(fw_size);
        Serial.println(F(" > app okno"));
        return false;
    }
    SHA256 sha;
    sha.update((const void*)fw_flash_base(), fw_size);
    uint8_t h[32];
    sha.finalize(h, sizeof(h));
    ota.base_fw_size = fw_size;
    memcpy(ota.base_fw_sha256, h, 32);
    Serial.print(F("[FOTA] base FW cached: size=")); Serial.print(fw_size);
    Serial.print(F("B sha256="));
    for (int i = 0; i < 4; i++) { if (h[i] < 0x10) Serial.print('0'); Serial.print(h[i], HEX); }
    Serial.println(F("..."));
    return memcmp(ota.base_fw_sha256, prefix, 4) == 0;
}

// Overenie base FW cez kompletný SHA256 (pre HEADER s full old_sha256)
static bool fota_base_fw_check_full(uint32_t fw_size, const uint8_t* sha256_full) {
    if (!fota_fw_size_matches(fw_size)) return false;
    if (fw_size > (APP_FLASH_END - fw_flash_base())) return false;
    SHA256 sha;
    sha.update((const void*)fw_flash_base(), fw_size);
    uint8_t h[32];
    sha.finalize(h, sizeof(h));
    ota.base_fw_size = fw_size;
    memcpy(ota.base_fw_sha256, h, 32);
    return memcmp(h, sha256_full, 32) == 0;
}

// Base FW gating pre META/SIG — tie nesú plný old_sha256[32] ale NIE old_fw_size.
// Over ho voči bežiacemu FW VŽDY (aj keď HEADER/SIG príde PRED akýmkoľvek chunkom):
//  - ak už máme cache (base_fw_size>0, naplnené chunkom/skorším META) → porovnaj lacno
//    bez nového SHA (base_fw_sha256 == SHA bežiaceho FW; base_fw_size je vždy ==
//    fw_image_size, lebo fota_fw_size_matches to gat­uje pred cache zápisom);
//  - inak doráta SHA nad celým bežiacim image (fw_image_size). Pre legitímny patch
//    platí old_fw_size == fw_image_size (invariant z FwId.h), takže to sedí.
// Bez tohto sa pri HEADER-first / SIG-first zakladala session pre CUDZÍ patch.
static bool fota_meta_base_ok(const uint8_t* old_sha256_full) {
    if (ota.base_fw_size > 0) {
        return memcmp(ota.base_fw_sha256, old_sha256_full, 32) == 0;
    }
    return fota_base_fw_check_full(fw_image_size(), old_sha256_full);
}

// =====================================================================
// Interné pomocné funkcie
// =====================================================================
static void fota_clear() {
    memset(&ota, 0, sizeof(ota));
    ota.status = FOTA_ST_IDLE;
    s_bitmap_dirty = 0;
}

static void fota_set_error(uint8_t code) {
    ota.status   = FOTA_ST_ERROR;
    ota.err_code = code;
    Serial.print(F("[FOTA] CHYBA=0x")); Serial.println(code, HEX);
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
    FotaMetaPersist mp;
    mp.magic        = FOTA_META_MAGIC;
    mp.status       = ota.status;
    mp.err_code     = ota.err_code;
    mp.total_chunks = ota.total_chunks;
    mp.patch_size   = ota.patch_size;
    memcpy(mp.patch_sha256, ota.patch_sha256, 32);
    memcpy(mp.new_sha256,   ota.new_sha256,   32);
    mp.old_fw_size = ota.old_fw_size;
    memcpy(mp.old_sha256,   ota.old_sha256,   32);
    mp.fota_prot_inf = ota.fota_prot_inf;
    mp.meta_recv    = ota.meta_recv;
    mp.sig_recv     = ota.sig_recv;
    mp.hdr_key_id   = ota.hdr_key_id;
    memcpy(mp.hdr_sig, ota.hdr_sig, 64);
    mp.crc16 = fota_crc16((const uint8_t*)&mp, (uint16_t)(sizeof(mp) - 2u));

    FotaFS.remove(FOTA_FS_META);
    File f(FotaFS);
    if (!f.open(FOTA_FS_META, FILE_O_WRITE)) {
        Serial.println(F("[FOTA] meta: zápis zlyhal")); return false;
    }
    bool ok = (f.write((const uint8_t*)&mp, sizeof(mp)) == (int)sizeof(mp));
    f.close();
    return ok;
}

static bool load_meta(FotaMetaPersist* out) {
    File f(FotaFS);
    if (!f.open(FOTA_FS_META, FILE_O_READ)) return false;
    bool ok = (f.read((uint8_t*)out, sizeof(*out)) == (int)sizeof(*out));
    f.close();
    if (!ok) return false;
    if (out->magic != FOTA_META_MAGIC) return false;
    uint16_t crc = fota_crc16((const uint8_t*)out, (uint16_t)(sizeof(*out) - 2u));
    return crc == out->crc16;
}

// =====================================================================
// CustomLFS — bitmap.bin
// =====================================================================
static void save_bitmap() {
    if (ota.total_chunks == 0) return;
    uint16_t nbytes = (ota.total_chunks + 7u) / 8u;
    FotaFS.remove(FOTA_FS_BITMAP);
    File f(FotaFS);
    if (!f.open(FOTA_FS_BITMAP, FILE_O_WRITE)) return;
    f.write(ota.bitmap, nbytes);
    f.close();
    s_bitmap_dirty = 0;
}

static bool load_bitmap() {
    if (ota.total_chunks == 0) return false;
    uint16_t nbytes = (ota.total_chunks + 7u) / 8u;
    File f(FotaFS);
    if (!f.open(FOTA_FS_BITMAP, FILE_O_READ)) return false;
    bool ok = (f.read(ota.bitmap, nbytes) == (int)nbytes);
    f.close();
    return ok;
}

// =====================================================================
// CustomLFS — recv.log (append log)
// Každý záznam: [idx 2B LE][data_len 2B LE][data N]
// =====================================================================
static bool log_append(uint16_t idx, const uint8_t* data, uint16_t data_len) {
    File f(FotaFS);
    if (!f.open(FOTA_FS_LOG, FILE_O_WRITE)) {
        Serial.println(F("[FOTA] log: zápis zlyhal")); return false;
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
// Presná dĺžka chunku i (bez AES paddingu): plné chunky = FOTA_CHUNK_DATA_MAX,
// posledný = zvyšok z patch_size.
static uint16_t chunk_exp_len(uint16_t i) {
    if (i < ota.total_chunks - 1u) return FOTA_CHUNK_DATA_MAX;
    uint32_t rem = ota.patch_size - (uint32_t)(ota.total_chunks - 1u) * FOTA_CHUNK_DATA_MAX;
    return (rem > FOTA_CHUNK_DATA_MAX) ? FOTA_CHUNK_DATA_MAX : (uint16_t)rem;
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
    File log_r(FotaFS);
    if (!log_r.open(FOTA_FS_LOG, FILE_O_READ)) return 0;
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
    File log_r(FotaFS);
    if (!log_r.open(FOTA_FS_LOG, FILE_O_READ)) {
        Serial.println(F("[FOTA] log: čítanie zlyhal")); return false;
    }
    build_log_offsets(log_r);
    SHA256 sha;
    uint8_t buf[FOTA_CHUNK_DATA_MAX];
    for (uint16_t i = 0; i < ota.total_chunks; i++) {
        if (s_log_data_offset[i] == 0xFFFFFFFFu) {
            Serial.print(F("[FOTA] chýba chunk ")); Serial.println(i);
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
        Serial.print(F("[FOTA] SHA256 NESÚHLASÍ  got="));
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
    Serial.println(F("[FOTA] Overujem patch SHA256 (RAM, bez patch.bin)..."));
    if (!verify_log_sha()) return false;
    Serial.println(F("[FOTA] patch SHA256 OK (recv.log ostáva ako zdroj)"));
    return true;
#else
    Serial.println(F("[FOTA] Zostavujem patch.bin..."));
    File log_r(FotaFS);
    if (!log_r.open(FOTA_FS_LOG, FILE_O_READ)) {
        Serial.println(F("[FOTA] log: čítanie zlyhal")); return false;
    }
    build_log_offsets(log_r);

    FotaFS.remove(FOTA_FS_PATCH);
    File out_f(FotaFS);
    if (!out_f.open(FOTA_FS_PATCH, FILE_O_WRITE)) { log_r.close(); return false; }

    SHA256 sha;
    bool   ok = true;
    uint8_t buf[FOTA_CHUNK_DATA_MAX];
    for (uint16_t i = 0; i < ota.total_chunks && ok; i++) {
        if (s_log_data_offset[i] == 0xFFFFFFFFu) {
            Serial.print(F("[FOTA] chýba chunk ")); Serial.println(i); ok = false; break;
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
    if (!ok) { Serial.println(F("[FOTA] zostava zlyhala")); return false; }

    uint8_t hash[32];
    sha.finalize(hash, sizeof(hash));
    if (memcmp(hash, ota.patch_sha256, 32) != 0) {
        Serial.print(F("[FOTA] SHA256 NESÚHLASÍ  got="));
        for (int i = 0; i < 8; i++) { if (hash[i] < 0x10) Serial.print('0'); Serial.print(hash[i], HEX); }
        Serial.println(F("..."));
        return false;
    }
    Serial.println(F("[FOTA] patch.bin SHA256 OK"));
    FotaFS.remove(FOTA_FS_LOG);   // recv.log cleanup — patch.bin je odteraz zdroj
    Serial.println(F("[FOTA] recv.log zmazaný (patch.bin je zdroj)"));
    return true;
#endif
}

// =====================================================================
// fota_acquire_patch_ram — patch do čerstvo malloc-nutého RAM buffra.
//   default:           zostaví z recv.log (žiadny patch.bin, žiadny 2× FS)
//   USE_PATCHBIN_FILE: prečíta /ota/patch.bin
// Caller uvoľní cez free(). *out_size = veľkosť. NULL pri chybe/malloc zlyhaní.
// =====================================================================
uint8_t* fota_acquire_patch_ram(uint32_t* out_size) {
#ifdef USE_PATCHBIN_FILE
    File f(FotaFS);
    if (!f.open(FOTA_FS_PATCH, FILE_O_READ)) { Serial.println(F("[FOTA] patch.bin chýba")); return nullptr; }
    uint32_t sz = (uint32_t)f.size();
    if (sz == 0 || sz > FOTA_FS_FLASH_SIZE) { f.close(); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) { f.close(); Serial.print(F("[FOTA] malloc ")); Serial.print(sz); Serial.println(F("B zlyhal")); return nullptr; }
    bool ok = ((uint32_t)f.read(buf, sz) == sz);
    f.close();
    if (!ok) { free(buf); Serial.println(F("[FOTA] čítanie patch.bin zlyhalo")); return nullptr; }
    *out_size = sz;
    return buf;
#else
    uint32_t sz = ota.patch_size;
    if (sz == 0 || sz > FOTA_FS_FLASH_SIZE) { Serial.println(F("[FOTA] neplatná patch_size")); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) {
        Serial.print(F("[FOTA] malloc ")); Serial.print(sz);
        Serial.println(F("B zlyhal (RAM assembly) — pre veľké patche skús -D USE_PATCHBIN_FILE"));
        return nullptr;
    }
    if (assemble_log_to_buf(buf, sz) != sz) {
        free(buf); Serial.println(F("[FOTA] RAM assembly z recv.log zlyhala")); return nullptr;
    }
    *out_size = sz;
    return buf;
#endif
}

// =====================================================================
// Resume po reboote
// =====================================================================
static void try_resume() {
    FotaMetaPersist mp;
    if (!load_meta(&mp)) return;

    if (!(mp.status & (FOTA_ST_RECEIVING | FOTA_ST_COMPLETE | FOTA_ST_VERIFIED | FOTA_ST_DONE))) return;
    if (mp.total_chunks == 0 || mp.total_chunks > FOTA_MAX_CHUNKS) return;

    ota.total_chunks = mp.total_chunks;
    ota.patch_size   = mp.patch_size;
    memcpy(ota.patch_sha256, mp.patch_sha256, 32);
    memcpy(ota.new_sha256,   mp.new_sha256,   32);
    ota.old_fw_size = mp.old_fw_size;
    memcpy(ota.old_sha256,   mp.old_sha256,   32);
    ota.fota_prot_inf = mp.fota_prot_inf;
    ota.meta_recv    = mp.meta_recv;
    ota.sig_recv     = mp.sig_recv;
    ota.hdr_key_id   = mp.hdr_key_id;
    memcpy(ota.hdr_sig, mp.hdr_sig, 64);
    ota.status   = mp.status;
    ota.err_code = mp.err_code;

    load_bitmap();
    ota.recv_count = bitmap_popcount();

    Serial.print(F("[FOTA] RESUME "));
    Serial.print(ota.recv_count); Serial.print('/');
    Serial.print(ota.total_chunks); Serial.print(F(" chunks  st=0x"));
    Serial.println(ota.status, HEX);
}

// =====================================================================
// Verejné API
// =====================================================================
void fota_init() {
    fota_clear();
    if (!FotaFS.begin()) {
        // Po flasheri je 0xD4000 prepísaný raw patch dátami — reformátuj
        Serial.println(F("[FOTA] FS poškodený (post-flash?), reformátujem..."));
        FotaFS.format();
        if (!FotaFS.begin()) {
            Serial.println(F("[FOTA] FS: format+begin zlyhalo — FS nedostupný"));
        }
    }
    FotaFS.mkdir(FOTA_FS_DIR);
    try_resume();
    Serial.println(F("[FOTA] init  (CustomLFS 92kB @ 0xD4000)"));
}

const FotaState* fota_get_state() { return &ota; }

void fota_print_status() {
    Serial.print(F("[FOTA] "));
    Serial.print(ota.recv_count); Serial.print('/'); Serial.print(ota.total_chunks);
    Serial.print(F("  st=0x")); Serial.print(ota.status, HEX);
    Serial.print(F("  size=")); Serial.print(ota.patch_size);
    if (ota.err_code) { Serial.print(F("  err=0x")); Serial.print(ota.err_code, HEX); }
    Serial.println();
}

void fota_send_nack() {
    if (ota.total_chunks == 0) return;
    uint16_t missing[FOTA_NACK_MAX_IDX];
    uint8_t  cnt = 0;
    for (uint16_t i = 0; i < ota.total_chunks && cnt < FOTA_NACK_MAX_IDX; i++)
        if (!FOTA_BIT_GET(ota.bitmap, i))
            missing[cnt++] = i;

    Serial.print(F("[FOTA] NACK missing=")); Serial.print(cnt);
    if (cnt) {
        Serial.print(F("  [")); Serial.print(missing[0]);
        if (cnt > 1) { Serial.print(F("..")); Serial.print(missing[cnt-1]); }
        Serial.print(']');
    }
    Serial.println();
}

// Rozsah na počítanie chýbajúcich chunkov [*lo .. *hi].
//  - HEADER známy (total_chunks>0): [0 .. total_chunks-1].
//  - HEADER neznámy (total_chunks==0): okno [najnižší .. najvyšší prijatý] z bitmapy
//    (chunky pod najnižším prijatým nevieme bez HEADER-a spoľahlivo nárokovať).
// Vracia false = "zero info yet" (žiaden chunk a žiaden HEADER).
static bool fota_missing_range(uint16_t* lo, uint16_t* hi) {
    if (ota.total_chunks > 0) { *lo = 0; *hi = (uint16_t)(ota.total_chunks - 1u); return true; }
    // HEADER neznámy — počítaj diery od chunku 0 po NAJVYŠŠÍ prijatý (chunky pod
    // najnižším prijatým reálne existujú a chýbajú, preto počítame od 0).
    int fhi = -1;
    for (uint16_t i = 0; i < FOTA_MAX_CHUNKS; i++)
        if (FOTA_BIT_GET(ota.bitmap, i)) fhi = (int)i;
    if (fhi < 0) return false;   // žiaden chunk
    *lo = 0; *hi = (uint16_t)fhi; return true;
}

// Vypočíta chýbajúce chunky v rozsahu z fota_missing_range().
// Návratová hodnota: celkový počet chýbajúcich; -1 = "zero info yet".
// out[] (ak != NULL) sa naplní prvými max_out indexmi, *out_n = koľko ich tam je.
int fota_calc_missing(uint16_t* out, int max_out, int* out_n) {
    if (out_n) *out_n = 0;
    uint16_t lo, hi;
    if (!fota_missing_range(&lo, &hi)) return -1;
    int n = 0, total = 0;
    for (uint16_t i = lo; ; i++) {
        if (!FOTA_BIT_GET(ota.bitmap, i)) {
            if (out && n < max_out) out[n++] = i;
            total++;
        }
        if (i == hi) break;   // bezpečné aj pre uint16_t (hi môže byť 0/65535)
    }
    if (out_n) *out_n = n;
    return total;
}

// Vypíše chýbajúce CHUNKY na Serial (bez prefixu/newline; H/S a riadok rieši volajúci).
// limit<=0 → všetky; inak prvých 'limit' (+zvyšok ako "+N"). Nič netlačí ak niet rozsahu.
void fota_print_missing(int limit) {
    uint16_t lo, hi;
    if (!fota_missing_range(&lo, &hi)) return;
    int shown = 0, total = 0;
    for (uint16_t i = lo; ; i++) {
        if (!FOTA_BIT_GET(ota.bitmap, i)) {
            total++;
            if (limit <= 0 || shown < limit) { Serial.print(i); Serial.print(' '); shown++; }
        }
        if (i == hi) break;
    }
    if (limit > 0 && total > shown) { Serial.print('+'); Serial.print(total - shown); }
}

// ---- Odložená žiadosť o flash (ACK „accepted" musí odísť PRED rebootom) ----
static bool s_apply_pending = false;
void fota_request_apply()      { s_apply_pending = true; }
bool fota_apply_pending()      { return s_apply_pending; }
void fota_clear_apply_pending(){ s_apply_pending = false; }

// Postav STATUS paket (6B). Vždy dostupný ak je session.
int fota_build_status(uint8_t* out) {
    if (ota.total_chunks == 0) return 0;
    FotaStatusPkt* p = (FotaStatusPkt*)out;
    p->type         = FOTA_PKT_STATUS;
    p->recv_count   = ota.recv_count;
    p->total_chunks = ota.total_chunks;
    p->status       = ota.status;
    return (int)sizeof(FotaStatusPkt);
}

// Postav NACK paket (2 + count*2). Vracia 0 ak nič nechýba.
int fota_build_nack(uint8_t* out) {
    if (ota.total_chunks == 0) return 0;
    FotaNackPkt* p = (FotaNackPkt*)out;
    p->type  = FOTA_PKT_NACK;
    p->count = 0;
    for (uint16_t i = 0; i < ota.total_chunks && p->count < FOTA_NACK_MAX_IDX; i++)
        if (!FOTA_BIT_GET(ota.bitmap, i))
            p->idx[p->count++] = i;
    if (p->count == 0) return 0;
    return 2 + (int)p->count * 2;
}

// =====================================================================
// Spracovanie FOTA_HEADER
// =====================================================================
// Zrekonštruuje 102 B META z uložených polí (MUSÍ byť bajt-identické s FotaHeaderPkt
// a s tým, čo podpísal sender — inak Ed25519 verify zlyhá).
static void rebuild_meta(uint8_t out[102]) {
    out[0] = FOTA_PKT_HEADER;
    out[1] = ota.fota_prot_inf;
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
#ifdef FOTA_ALLOW_UNSIGNED
    bool is_unsigned = (ota.hdr_sig[0] == 0 && ota.hdr_sig[1] == 0 &&
                        ota.hdr_sig[2] == 0 && ota.hdr_sig[3] == 0);
    if (is_unsigned) { Serial.println(F("[FOTA] HEADER: UNSIGNED (FOTA_ALLOW_UNSIGNED)")); ok = true; }
    else
#endif
    ok = verify_header_signature(ota.hdr_sig, meta, 102u, ota.hdr_key_id);

    if (!ok) {
        Serial.println(F("[FOTA] HEADER: INVALID signature — rejecting"));
        fota_set_error(FOTA_ERR_SIGNATURE);
        return;
    }

    uint32_t tc = (ota.patch_size + FOTA_CHUNK_DATA_MAX - 1u) / FOTA_CHUNK_DATA_MAX;
    if (tc == 0 || tc > FOTA_MAX_CHUNKS) {
        Serial.print(F("[FOTA] HEADER: zlé total_chunks=")); Serial.println(tc); return;
    }
    ota.total_chunks = (uint16_t)tc;
    ota.recv_count   = bitmap_popcount();
    ota.status       = FOTA_ST_RECEIVING;
    save_bitmap();
    save_meta();
    Serial.print(F("[FOTA] HEADER OK (META+SIG overené) chunks=")); Serial.print(tc);
    Serial.print(F("  mám ")); Serial.print(ota.recv_count); Serial.println(F(" chunkov"));

    // Chunky mohli doraziť pred hlavičkou → over COMPLETE hneď
    if (ota.recv_count >= ota.total_chunks) {
        ota.status |= FOTA_ST_COMPLETE;
        save_meta();
        Serial.println(F("[FOTA] COMPLETE — assembly + SHA256..."));
        if (assemble_and_verify()) {
            ota.status |= FOTA_ST_VERIFIED; save_meta();
            Serial.println(F("[FOTA] VERIFIED — 'ota verify'=dry-run | 'ota flash'=flash+reboot"));
        } else {
            fota_set_error(FOTA_ERR_SHA256); save_meta();
        }
    }
}

// FOTA_PKT_HEADER = META (metadáta patchu, podpisované). Idempotentné (opätovné
// prijatie len prepíše rovnaké polia). Verify+promócia spraví try_verify_header.
static void handle_meta(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(FotaHeaderPkt)) { Serial.println(F("[FOTA] META: krátky")); return; }
    const FotaHeaderPkt* pkt = (const FotaHeaderPkt*)plain;

    // Base FW gating — META.old_sha256 MUSÍ sedieť s bežiacim FW, inak patch nepatrí
    // tomuto zariadeniu. Platí AJ keď HEADER príde pred prvým chunkom (vtedy
    // base_fw_size==0 a SHA sa doráta nad fw_image_size). Drop (nie ERROR) — cudzí
    // paket nesmie zhodiť ani založiť NAŠU session. Vypíš (ako pri chunku).
    if (!fota_meta_base_ok(pkt->old_sha256)) {
        Serial.println(F("[FOTA] META: base FW nezhoda — patch nie je pre toto zariadenie, drop"));
        return;
    }

    // Re-send identickej META? Spočítaj PRED prípadným fota_clear (ten zeruje
    // patch_sha256). Ak je to DUP, preskočíme save_meta() — flash-zápis blokuje
    // RX cestu (nRF52 NVMC halt) a spôsobí stratu nasledujúceho SIG/APPLY paketu.
    bool dup_meta = ota.meta_recv && (ota.status & FOTA_ST_RECEIVING)
                 && ota.patch_size == pkt->patch_size
                 && memcmp(ota.patch_sha256, pkt->patch_sha256, 32) == 0;

    bool partial = (ota.status & FOTA_ST_RECEIVING) && ota.total_chunks == 0;
    bool other_patch = (ota.status & FOTA_ST_RECEIVING) && ota.total_chunks > 0 &&
                       memcmp(ota.patch_sha256, pkt->patch_sha256, 32) != 0;
    if (!(ota.status & FOTA_ST_RECEIVING) || other_patch) {
        // Nová session (alebo iný patch beží) — vyčisti FS
        FotaFS.remove(FOTA_FS_LOG);
        FotaFS.remove(FOTA_FS_PATCH);
        FotaFS.remove(FOTA_FS_BITMAP);
        fota_clear();
        ota.status = FOTA_ST_RECEIVING;
        ota.total_chunks = 0;
    }
    (void)partial;   // partial chunky sa zachovajú (merge), nič nemažeme

    ota.fota_prot_inf = pkt->fota_prot_inf;
    ota.patch_size   = pkt->patch_size;
    memcpy(ota.patch_sha256, pkt->patch_sha256, 32);
    memcpy(ota.new_sha256,   pkt->new_sha256,   32);
    memcpy(ota.old_sha256,   pkt->old_sha256,   32);
    ota.meta_recv = 1;
    if (!dup_meta) save_meta();   // DUP re-send → žiadny flash zápis (nestalluj RX)
    Serial.print(F("[FOTA] META prijaté patch_size=")); Serial.print(pkt->patch_size);
    Serial.print(F("B  patch_sha256="));
    for (int i = 0; i < 6; i++) { if (pkt->patch_sha256[i] < 0x10) Serial.print('0'); Serial.print(pkt->patch_sha256[i], HEX); }
    Serial.print(F("..."));
    Serial.println(dup_meta ? F("  meta_recv=1 DUP → skip save")
                            : F("  NEW/CHANGED → save"));
    try_verify_header();
}

// FOTA_PKT_HDR_SIG = SIG (Ed25519 podpis META). Gating cez old_sha256.
static void handle_sig(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(FotaHdrSigPkt)) { Serial.println(F("[FOTA] SIG: krátky")); return; }
    const FotaHdrSigPkt* pkt = (const FotaHdrSigPkt*)plain;

    if (ota.meta_recv) {
        if (memcmp(ota.old_sha256, pkt->old_sha256, 32) != 0) {
            Serial.println(F("[FOTA] SIG: old_sha256 nezhoda s META — drop")); return;
        }
    } else if (!fota_meta_base_ok(pkt->old_sha256)) {
        // SIG prišiel pred META — over base FW VŽDY (aj pri base_fw_size==0), inak by
        // SIG-first založil session pre cudzí patch. (SIG.old_sha256 = "gating patrí mne".)
        Serial.println(F("[FOTA] SIG: base FW nezhoda — patch nie je pre toto zariadenie, drop")); return;
    }
    if (!(ota.status & FOTA_ST_RECEIVING)) { ota.status = FOTA_ST_RECEIVING; ota.total_chunks = 0; }

    // Re-send identického SIG? DUP → preskoč save_meta() (rovnaký dôvod ako META).
    bool dup_sig = ota.sig_recv && ota.hdr_key_id == pkt->key_id
                && memcmp(ota.hdr_sig, pkt->signature, 64) == 0;
    ota.hdr_key_id = pkt->key_id;
    memcpy(ota.hdr_sig, pkt->signature, 64);
    ota.sig_recv = 1;
    if (!dup_sig) save_meta();   // DUP re-send → žiadny flash zápis (nestalluj RX)
    Serial.print(F("[FOTA] SIG prijaté key_id=0x")); Serial.print(pkt->key_id, HEX);
    Serial.println(dup_sig ? F("  sig_recv=1 DUP → skip save")
                           : F("  NEW/CHANGED → save"));
    try_verify_header();
}

// =====================================================================
// Spracovanie FOTA_CHUNK
// =====================================================================
static void handle_chunk(const uint8_t* plain, int plen) {
    if (plen < 13) return;  // min: type(1)+idx(2)+crc(2)+old_fw_size(4)+prefix(4) = 13B

    const FotaChunkPkt* pkt = (const FotaChunkPkt*)plain;
    uint16_t idx = pkt->chunk_idx;
    uint16_t rx_crc = pkt->crc16;
    const uint8_t* data = pkt->data;
    uint16_t data_len = (uint16_t)(plen - (int)(sizeof(FotaChunkPkt) - FOTA_CHUNK_DATA_MAX));

    // Base FW validácia — over, že chunk je pre aktuálny FW na zariadení
    if (!fota_base_fw_validated(pkt->old_fw_size, pkt->old_sha256_prefix)) {
        Serial.println(F("[FOTA] CHUNK: base FW nezhoda — drop"));
        return;
    }

    // Session init alebo merge:
    //  - žiadna session → vytvor partial (total_chunks=0, base z chunku);
    //    skoré chunky sa rovno bufferujú, HEADER ich neskôr "promuje".
    //  - existujúca session s INÝM base FW → ignoruj (nemiešaj patche).
    if (!(ota.status & FOTA_ST_RECEIVING)) {
        FotaFS.remove(FOTA_FS_LOG);
        FotaFS.remove(FOTA_FS_PATCH);
        FotaFS.remove(FOTA_FS_BITMAP);
        fota_clear();
        ota.old_fw_size = pkt->old_fw_size;
        memcpy(ota.old_sha256, pkt->old_sha256_prefix, 4);  // zvyšok doplní HEADER
        ota.status = FOTA_ST_RECEIVING;
        ota.total_chunks = 0;  // čaká HEADER (alebo promóciu)
        save_meta();
        Serial.println(F("[FOTA] CHUNK: partial session z chunku (čaká HEADER)"));
    } else if (memcmp(ota.old_sha256, pkt->old_sha256_prefix, 4) != 0) {
        return;  // chunk patrí inému base FW než bežiaca session
    }
    // HEADER nenesie old_fw_size — session ho preberá z chunku (base už overený
    // vyššie cez fota_base_fw_validated). Bez tohto by ostal 0 po HEADER ceste.
    ota.old_fw_size = pkt->old_fw_size;

    // Hranica idx: kým nepoznáme total_chunks (pred HEADER), bufferuj až po MAX.
    uint16_t max_idx = (ota.total_chunks > 0) ? ota.total_chunks : (uint16_t)FOTA_MAX_CHUNKS;
    if (idx >= max_idx) {
        if (ota.total_chunks > 0) fota_set_error(FOTA_ERR_OVERFLOW);
        return;
    }

    // Presná dĺžka chunku — AES-ECB dopĺňa plaintext na 16B blok; padding treba
    // strhnúť, inak CRC (sender ráta cez presnú dĺžku) nesedí. Posledný chunk má
    // dĺžku z patch_size, tú poznáme až po HEADER — preto sa posledný chunk PRED
    // HEADER neuloží (CRC zlyhá na paddingu) a príde znova v ďalšom cykle.
    uint16_t exp_len = FOTA_CHUNK_DATA_MAX;
    if (ota.total_chunks > 0 && idx == (uint16_t)(ota.total_chunks - 1u)) {
        uint32_t rem = ota.patch_size - (uint32_t)(ota.total_chunks - 1u) * FOTA_CHUNK_DATA_MAX;
        exp_len = (rem > FOTA_CHUNK_DATA_MAX) ? FOTA_CHUNK_DATA_MAX : (uint16_t)rem;
    }
    if (data_len > exp_len) data_len = exp_len;

    uint16_t calc_crc = fota_crc16(data, data_len);
    if (calc_crc != rx_crc) {
        Serial.print(F("[FOTA] CRC ERR idx=")); Serial.print(idx);
        Serial.print(F("  exp=0x")); Serial.print(rx_crc, HEX);
        Serial.print(F("  got=0x")); Serial.println(calc_crc, HEX);
        return;
    }

    if (FOTA_BIT_GET(ota.bitmap, idx)) return;  // duplikát s OK CRC

    if (!log_append(idx, data, data_len)) {
        fota_set_error(FOTA_ERR_STORAGE); return;
    }

    FOTA_BIT_SET(ota.bitmap, idx);
    ota.recv_count++;

    s_bitmap_dirty++;
    bool complete = (ota.total_chunks > 0) && (ota.recv_count >= ota.total_chunks);
    if (s_bitmap_dirty >= FOTA_BITMAP_SAVE_EVERY || complete)
        save_bitmap();

    if (ota.recv_count % 20 == 0 || complete) {
        Serial.print(F("[FOTA] ")); Serial.print(ota.recv_count);
        Serial.print('/'); Serial.println(ota.total_chunks);
    }

    if (complete) {
        ota.status |= FOTA_ST_COMPLETE;
        save_meta();
        Serial.println(F("[FOTA] COMPLETE — assembly + SHA256..."));

        if (assemble_and_verify()) {
            ota.status |= FOTA_ST_VERIFIED;
            save_meta();
            Serial.println(F("[FOTA] VERIFIED — 'ota verify'=dry-run | 'ota flash'=flash+reboot"));
        } else {
            fota_set_error(FOTA_ERR_SHA256);
            save_meta();
        }
    }
}

// =====================================================================
// Spracovanie FOTA_APPLY
// =====================================================================
static void handle_apply(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(FotaApplyPkt)) return;
    const FotaApplyPkt* pkt = (const FotaApplyPkt*)plain;
    if (memcmp(pkt->sha256, ota.patch_sha256, 32) != 0) {
        Serial.println(F("[FOTA] APPLY: SHA256 nesúhlasí")); return;
    }
    fota_apply();
}

// =====================================================================
// Výpis OTA paketu
// =====================================================================
void fota_print_pkt(const uint8_t* plain, int plen, float rssi, float snr) {
    if (plen < 1) return;
    uint8_t type = plain[0];

    Serial.print(F("[FOTA] "));

    switch (type) {
        case FOTA_PKT_HEADER: {   // META
            if (plen < (int)sizeof(FotaHeaderPkt)) { Serial.println(F("META (krátky)")); return; }
            const FotaHeaderPkt* p = (const FotaHeaderPkt*)plain;
            uint32_t ps; memcpy(&ps, &p->patch_size, 4);
            uint32_t tc = (ps + FOTA_CHUNK_DATA_MAX - 1u) / FOTA_CHUNK_DATA_MAX;
            Serial.print(F("META  v")); Serial.print(p->fota_prot_inf);
            Serial.print(F("  size="));   Serial.print(ps); Serial.print('B');
            Serial.print(F("  chunks~")); Serial.print(tc);
            Serial.print(F("  patch="));
            for (int i = 0; i < 4; i++) { if (p->patch_sha256[i] < 0x10) Serial.print('0'); Serial.print(p->patch_sha256[i], HEX); }
            Serial.print(F("...  new="));
            for (int i = 0; i < 4; i++) { if (p->new_sha256[i] < 0x10) Serial.print('0'); Serial.print(p->new_sha256[i], HEX); }
            Serial.print(F("..."));
            break;
        }
        case FOTA_PKT_HDR_SIG: {  // SIG
            if (plen < (int)sizeof(FotaHdrSigPkt)) { Serial.println(F("SIG (krátky)")); return; }
            const FotaHdrSigPkt* p = (const FotaHdrSigPkt*)plain;
            Serial.print(F("SIG  v")); Serial.print(p->fota_prot_inf);
            Serial.print(F("  key_id=0x")); Serial.print(p->key_id, HEX);
            Serial.print(F("  sig="));
            for (int i = 0; i < 4; i++) { if (p->signature[i] < 0x10) Serial.print('0'); Serial.print(p->signature[i], HEX); }
            Serial.print(F("...  old="));
            for (int i = 0; i < 4; i++) { if (p->old_sha256[i] < 0x10) Serial.print('0'); Serial.print(p->old_sha256[i], HEX); }
            Serial.print(F("..."));
            break;
        }
        case FOTA_PKT_CHUNK: {
            if (plen < (int)(sizeof(FotaChunkPkt) - FOTA_CHUNK_DATA_MAX)) { Serial.println(F("CHUNK (krátky)")); return; }
            const FotaChunkPkt* p = (const FotaChunkPkt*)plain;
            uint16_t dlen = (uint16_t)(plen - (int)(sizeof(FotaChunkPkt) - FOTA_CHUNK_DATA_MAX));
            uint16_t calc  = fota_crc16(p->data, dlen);
            bool crc_ok    = (calc == p->crc16);

            Serial.print(F("CHUNK  idx="));  Serial.print(p->chunk_idx);
            if (ota.total_chunks > 0) { Serial.print('/'); Serial.print(ota.total_chunks); }
            Serial.print(F("  len="));       Serial.print(dlen); Serial.print('B');
            Serial.print(F("  crc=0x"));     Serial.print(p->crc16, HEX);
            Serial.print(crc_ok ? F("  OK") : F("  BAD"));
            if (p->chunk_idx < FOTA_MAX_CHUNKS && FOTA_BIT_GET(ota.bitmap, p->chunk_idx)) Serial.print(F(" DUP"));
            Serial.print(F("  base="));
            for (int i = 0; i < 4; i++) { if (p->old_sha256_prefix[i] < 0x10) Serial.print('0'); Serial.print(p->old_sha256_prefix[i], HEX); }
            break;
        }
        case FOTA_PKT_APPLY: {
            if (plen < (int)sizeof(FotaApplyPkt)) { Serial.println(F("APPLY (krátky)")); return; }
            const FotaApplyPkt* p = (const FotaApplyPkt*)plain;
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
bool fota_process(const uint8_t* plain, int plen) {
    if (plen < 1) return false;
    switch (plain[0]) {
        case FOTA_PKT_HEADER:  handle_meta(plain, plen);  return true;
        case FOTA_PKT_HDR_SIG: handle_sig(plain, plen);   return true;
        case FOTA_PKT_CHUNK:   handle_chunk(plain, plen);  return true;
        case FOTA_PKT_APPLY:   handle_apply(plain, plen);  return true;
        default:             return false;
    }
}

// =====================================================================
// fota_apply — spustí skutočný flash (fota_flash_via_flasher, NEVRÁTI SA pri úspechu)
// =====================================================================
bool fota_apply() {
    if (!(ota.status & FOTA_ST_VERIFIED)) {
        Serial.println(F("[FOTA] APPLY: nie je verifikované — spusti príjem chunkov")); return false;
    }
    uint8_t prev_status = ota.status;   // pre obnovu ak flash zlyhá (base-check a pod.)
    ota.status = FOTA_ST_APPLYING;
    save_meta();

    extern bool fota_flash_via_flasher();
    if (fota_flash_via_flasher()) return true;  // NEVRÁTI SA pri úspechu

    // Flash zlyhal pred skokom (napr. base FW != old). Obnov VERIFIED.
    ota.status = prev_status;
    save_meta();
    return false;
}

// =====================================================================
// fota_clear_session — vymaže OTA súbory z FS, resetuje RAM stav
// =====================================================================
void fota_clear_session() {
    Serial.println(F("[FOTA] Mazem OTA session..."));
    int removed = 0;
    const char* files[] = { FOTA_FS_META, FOTA_FS_BITMAP, FOTA_FS_LOG, FOTA_FS_PATCH };
    for (int i = 0; i < 4; i++) {
        if (FotaFS.remove(files[i])) {
            Serial.print(F("[FOTA] rm "));
            Serial.println(files[i]);
            removed++;
        }
    }
    fota_clear();
    Serial.print(F("[FOTA] Hotovo — vymazaných ")); Serial.print(removed);
    Serial.println(F(" súborov"));
}

#endif  // WITH_LORA_FOTA

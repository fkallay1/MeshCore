// =====================================================================
// FotaReceiver.cpp — FOTA prijímač (MeshCore port z FK_lora-sniffer)
//
// Spracováva dešifrovaný FOTA payload, ukladá chunky do CustomLFS append-logu,
// po COMPLETE zostaví patch.bin a overí SHA256. Reboot-resilient (meta+bitmap).
// =====================================================================
#ifdef WITH_LORA_FOTA
#include "FotaReceiver.h"
#include "FotaFs.h"
#include "FwId.h"             // fw_id_trailer (build#, image_size, sha256)
#include "FotaDebug.h"
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
    FOTA_DEBUG_PRINTLN("[FOTA] UNKNOWN key_id=0x%X", (unsigned)key_id);
    return false;
}

// =====================================================================
// RAM stav
// =====================================================================
static FotaState   fota;
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

__attribute__((unused))
static void print_sha_full(const uint8_t* h) {
    for (int i = 0; i < 32; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)h[i]); }
    FOTA_DEBUG_PRINTLN("");
}

// "fota id" — vypíš FW identitu a dopočítaj plný SHA256 bežiaceho FW.
// running sha256 sa počíta nad [base, +image_size) AS-IS (vrátane vyplneného
// traileru) → ZHODUJE sa s old_sha256 v .fotapkg.json (to PC počíta nad rovnakým
// app image). Trailer.sha256 je iný hash (self-hash so sha[]=0) — len referencia.
void fota_print_fw_id(char* reply) {
    uint32_t base      = fw_flash_base();
    uint32_t link_size = fw_image_size();
    uint32_t timg      = fw_id_trailer.image_size;
    uint32_t build     = fw_id_trailer.build_number;

    FOTA_DEBUG_PRINTLN("[FOTA] === FW identity ===");
    FOTA_DEBUG_PRINTLN("[FOTA] build #            = %lu", (unsigned long)build);
    FOTA_DEBUG_PRINTLN("[FOTA] app base           = 0x%lX", (unsigned long)base);
    FOTA_DEBUG_PRINTLN("[FOTA] image_size trailer = %lu", (unsigned long)timg);
    FOTA_DEBUG_PRINTLN("[FOTA] image_size linker  = %lu", (unsigned long)link_size);
    if (timg != link_size)
        FOTA_DEBUG_PRINTLN("[FOTA] !! POZOR: trailer != linker veľkosť — zlá board konfig?");
    FOTA_DEBUG_PRINT("[FOTA] trailer sha256     = "); print_sha_full(fw_id_trailer.sha256);

    uint8_t h[32]; memset(h, 0, sizeof(h));
    if (timg && timg <= (APP_FLASH_END - base)) {
        SHA256 sha;
        sha.update((const void*)base, timg);
        sha.finalize(h, sizeof(h));
        FOTA_DEBUG_PRINT("[FOTA] running sha256     = "); print_sha_full(h);
        FOTA_DEBUG_PRINTLN("[FOTA] ^ porovnaj s old_sha256 v .fotapkg.json");
    } else {
        FOTA_DEBUG_PRINTLN("[FOTA] running sha256: image_size neplatná");
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
    FOTA_DEBUG_PRINTLN("[FOTA] base FW: old_fw_size %lu != bežiace %lu",
                       (unsigned long)fw_size, (unsigned long)self);
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
    if (fota.base_fw_size == fw_size) {
        return memcmp(fota.base_fw_sha256, prefix, 4) == 0;
    }
    // Veľkosť sa zmenila alebo cache prázdna → re-počítaj
    if (fw_size > (APP_FLASH_END - fw_flash_base())) {
        FOTA_DEBUG_PRINTLN("[FOTA] base FW: fw_size %lu > app okno", (unsigned long)fw_size);
        return false;
    }
    SHA256 sha;
    sha.update((const void*)fw_flash_base(), fw_size);
    uint8_t h[32];
    sha.finalize(h, sizeof(h));
    fota.base_fw_size = fw_size;
    memcpy(fota.base_fw_sha256, h, 32);
    FOTA_DEBUG_PRINT("[FOTA] base FW cached: size=%luB sha256=", (unsigned long)fw_size);
    for (int i = 0; i < 4; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)h[i]); }
    FOTA_DEBUG_PRINTLN("...");
    return memcmp(fota.base_fw_sha256, prefix, 4) == 0;
}

// Overenie base FW cez kompletný SHA256 (pre HEADER s full old_sha256)
static bool fota_base_fw_check_full(uint32_t fw_size, const uint8_t* sha256_full) {
    if (!fota_fw_size_matches(fw_size)) return false;
    if (fw_size > (APP_FLASH_END - fw_flash_base())) return false;
    SHA256 sha;
    sha.update((const void*)fw_flash_base(), fw_size);
    uint8_t h[32];
    sha.finalize(h, sizeof(h));
    fota.base_fw_size = fw_size;
    memcpy(fota.base_fw_sha256, h, 32);
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
    if (fota.base_fw_size > 0) {
        return memcmp(fota.base_fw_sha256, old_sha256_full, 32) == 0;
    }
    return fota_base_fw_check_full(fw_image_size(), old_sha256_full);
}

// =====================================================================
// Interné pomocné funkcie
// =====================================================================
static void fota_clear() {
    memset(&fota, 0, sizeof(fota));
    fota.status = FOTA_ST_IDLE;
    s_bitmap_dirty = 0;
}

static void fota_set_error(uint8_t code) {
    fota.status   = FOTA_ST_ERROR;
    fota.err_code = code;
    FOTA_DEBUG_PRINTLN("[FOTA] CHYBA=0x%X", (unsigned)code);
}

// Kernighan bit count
static uint16_t bitmap_popcount() {
    uint16_t n = 0, bytes = (fota.total_chunks + 7u) / 8u;
    for (uint16_t i = 0; i < bytes; i++) {
        uint8_t b = fota.bitmap[i];
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
    mp.status       = fota.status;
    mp.err_code     = fota.err_code;
    mp.total_chunks = fota.total_chunks;
    mp.patch_size   = fota.patch_size;
    memcpy(mp.patch_sha256, fota.patch_sha256, 32);
    memcpy(mp.new_sha256,   fota.new_sha256,   32);
    mp.old_fw_size = fota.old_fw_size;
    memcpy(mp.old_sha256,   fota.old_sha256,   32);
    mp.fota_prot_inf = fota.fota_prot_inf;
    mp.meta_recv    = fota.meta_recv;
    mp.sig_recv     = fota.sig_recv;
    mp.hdr_key_id   = fota.hdr_key_id;
    memcpy(mp.hdr_sig, fota.hdr_sig, 64);
    mp.crc16 = fota_crc16((const uint8_t*)&mp, (uint16_t)(sizeof(mp) - 2u));

    FotaFS.remove(FOTA_FS_META);
    File f(FotaFS);
    if (!f.open(FOTA_FS_META, FILE_O_WRITE)) {
        FOTA_DEBUG_PRINTLN("[FOTA] meta: zápis zlyhal"); return false;
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
    if (fota.total_chunks == 0) return;
    uint16_t nbytes = (fota.total_chunks + 7u) / 8u;
    FotaFS.remove(FOTA_FS_BITMAP);
    File f(FotaFS);
    if (!f.open(FOTA_FS_BITMAP, FILE_O_WRITE)) return;
    f.write(fota.bitmap, nbytes);
    f.close();
    s_bitmap_dirty = 0;
}

static bool load_bitmap() {
    if (fota.total_chunks == 0) return false;
    uint16_t nbytes = (fota.total_chunks + 7u) / 8u;
    File f(FotaFS);
    if (!f.open(FOTA_FS_BITMAP, FILE_O_READ)) return false;
    bool ok = (f.read(fota.bitmap, nbytes) == (int)nbytes);
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
        FOTA_DEBUG_PRINTLN("[FOTA] log: zápis zlyhal"); return false;
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
    if (i < fota.total_chunks - 1u) return FOTA_CHUNK_DATA_MAX;
    uint32_t rem = fota.patch_size - (uint32_t)(fota.total_chunks - 1u) * FOTA_CHUNK_DATA_MAX;
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
        if (idx < fota.total_chunks) s_log_data_offset[idx] = log_pos + 4;
        log_pos += 4u + data_len;
        log_r.seek(log_pos);
    }
}

// Zostaví patch z recv.log priamo do RAM (buf, kapacita cap).
// Vráti zostavenú veľkosť (== fota.patch_size) alebo 0 pri chybe/chýbajúcom chunku.
static uint32_t assemble_log_to_buf(uint8_t* buf, uint32_t cap) {
    if (fota.total_chunks == 0 || fota.patch_size == 0 || fota.patch_size > cap) return 0;
    File log_r(FotaFS);
    if (!log_r.open(FOTA_FS_LOG, FILE_O_READ)) return 0;
    build_log_offsets(log_r);
    uint32_t out_pos = 0;
    for (uint16_t i = 0; i < fota.total_chunks; i++) {
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
        FOTA_DEBUG_PRINTLN("[FOTA] log: čítanie zlyhal"); return false;
    }
    build_log_offsets(log_r);
    SHA256 sha;
    uint8_t buf[FOTA_CHUNK_DATA_MAX];
    for (uint16_t i = 0; i < fota.total_chunks; i++) {
        if (s_log_data_offset[i] == 0xFFFFFFFFu) {
            FOTA_DEBUG_PRINTLN("[FOTA] chýba chunk %u", (unsigned)i);
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
    if (memcmp(hash, fota.patch_sha256, 32) != 0) {
        FOTA_DEBUG_PRINT("[FOTA] SHA256 NESÚHLASÍ  got=");
        for (int i = 0; i < 8; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)hash[i]); }
        FOTA_DEBUG_PRINTLN("...");
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
    FOTA_DEBUG_PRINTLN("[FOTA] Overujem patch SHA256 (RAM, bez patch.bin)...");
    if (!verify_log_sha()) return false;
    FOTA_DEBUG_PRINTLN("[FOTA] patch SHA256 OK (recv.log ostáva ako zdroj)");
    return true;
#else
    FOTA_DEBUG_PRINTLN("[FOTA] Zostavujem patch.bin...");
    File log_r(FotaFS);
    if (!log_r.open(FOTA_FS_LOG, FILE_O_READ)) {
        FOTA_DEBUG_PRINTLN("[FOTA] log: čítanie zlyhal"); return false;
    }
    build_log_offsets(log_r);

    FotaFS.remove(FOTA_FS_PATCH);
    File out_f(FotaFS);
    if (!out_f.open(FOTA_FS_PATCH, FILE_O_WRITE)) { log_r.close(); return false; }

    SHA256 sha;
    bool   ok = true;
    uint8_t buf[FOTA_CHUNK_DATA_MAX];
    for (uint16_t i = 0; i < fota.total_chunks && ok; i++) {
        if (s_log_data_offset[i] == 0xFFFFFFFFu) {
            FOTA_DEBUG_PRINTLN("[FOTA] chýba chunk %u", (unsigned)i); ok = false; break;
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
    if (!ok) { FOTA_DEBUG_PRINTLN("[FOTA] zostava zlyhala"); return false; }

    uint8_t hash[32];
    sha.finalize(hash, sizeof(hash));
    if (memcmp(hash, fota.patch_sha256, 32) != 0) {
        FOTA_DEBUG_PRINT("[FOTA] SHA256 NESÚHLASÍ  got=");
        for (int i = 0; i < 8; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)hash[i]); }
        FOTA_DEBUG_PRINTLN("...");
        return false;
    }
    FOTA_DEBUG_PRINTLN("[FOTA] patch.bin SHA256 OK");
    FotaFS.remove(FOTA_FS_LOG);   // recv.log cleanup — patch.bin je odteraz zdroj
    FOTA_DEBUG_PRINTLN("[FOTA] recv.log zmazaný (patch.bin je zdroj)");
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
    if (!f.open(FOTA_FS_PATCH, FILE_O_READ)) { FOTA_DEBUG_PRINTLN("[FOTA] patch.bin chýba"); return nullptr; }
    uint32_t sz = (uint32_t)f.size();
    if (sz == 0 || sz > FOTA_FS_FLASH_SIZE) { f.close(); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) { f.close(); FOTA_DEBUG_PRINTLN("[FOTA] malloc %lu B zlyhal", (unsigned long)sz); return nullptr; }
    bool ok = ((uint32_t)f.read(buf, sz) == sz);
    f.close();
    if (!ok) { free(buf); FOTA_DEBUG_PRINTLN("[FOTA] čítanie patch.bin zlyhalo"); return nullptr; }
    *out_size = sz;
    return buf;
#else
    uint32_t sz = fota.patch_size;
    if (sz == 0 || sz > FOTA_FS_FLASH_SIZE) { FOTA_DEBUG_PRINTLN("[FOTA] neplatná patch_size"); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) {
        FOTA_DEBUG_PRINTLN("[FOTA] malloc %lu B zlyhal (RAM assembly) — pre veľké patche skús -D USE_PATCHBIN_FILE", (unsigned long)sz);
        return nullptr;
    }
    if (assemble_log_to_buf(buf, sz) != sz) {
        free(buf); FOTA_DEBUG_PRINTLN("[FOTA] RAM assembly z recv.log zlyhala"); return nullptr;
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

    fota.total_chunks = mp.total_chunks;
    fota.patch_size   = mp.patch_size;
    memcpy(fota.patch_sha256, mp.patch_sha256, 32);
    memcpy(fota.new_sha256,   mp.new_sha256,   32);
    fota.old_fw_size = mp.old_fw_size;
    memcpy(fota.old_sha256,   mp.old_sha256,   32);
    fota.fota_prot_inf = mp.fota_prot_inf;
    fota.meta_recv    = mp.meta_recv;
    fota.sig_recv     = mp.sig_recv;
    fota.hdr_key_id   = mp.hdr_key_id;
    memcpy(fota.hdr_sig, mp.hdr_sig, 64);
    fota.status   = mp.status;
    fota.err_code = mp.err_code;

    load_bitmap();
    fota.recv_count = bitmap_popcount();

    FOTA_DEBUG_PRINTLN("[FOTA] RESUME %u/%u chunks  st=0x%02X", (unsigned)fota.recv_count, (unsigned)fota.total_chunks, (unsigned)fota.status);
}

// =====================================================================
// Verejné API
// =====================================================================
void fota_init() {
    fota_clear();
    if (!FotaFS.begin()) {
        // Po flasheri je 0xD4000 prepísaný raw patch dátami — reformátuj
        FOTA_DEBUG_PRINTLN("[FOTA] FS poškodený (post-flash?), reformátujem...");
        FotaFS.format();
        if (!FotaFS.begin()) {
            FOTA_DEBUG_PRINTLN("[FOTA] FS: format+begin zlyhalo — FS nedostupný");
        }
    }
    FotaFS.mkdir(FOTA_FS_DIR);
    try_resume();
    FOTA_DEBUG_PRINTLN("[FOTA] init  (CustomLFS 92kB @ 0xD4000)");
}

const FotaState* fota_get_state() { return &fota; }

void fota_print_status() {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "[FOTA] %u/%u  st=0x%02X  size=%lu",
                     (unsigned)fota.recv_count, (unsigned)fota.total_chunks,
                     (unsigned)fota.status, (unsigned long)fota.patch_size);
    if (fota.err_code && n > 0 && n < (int)sizeof(buf) - 16) {
        snprintf(buf + n, sizeof(buf) - n, "  err=0x%02X", (unsigned)fota.err_code);
    }
    FOTA_DEBUG_PRINTLN("%s", buf);
}

void fota_send_nack() {
    if (fota.total_chunks == 0) return;
    uint16_t missing[FOTA_NACK_MAX_IDX];
    uint8_t  cnt = 0;
    for (uint16_t i = 0; i < fota.total_chunks && cnt < FOTA_NACK_MAX_IDX; i++)
        if (!FOTA_BIT_GET(fota.bitmap, i))
            missing[cnt++] = i;

    FOTA_DEBUG_PRINT("[FOTA] NACK missing=%u", (unsigned)cnt);
    if (cnt) {
        FOTA_DEBUG_PRINT("  [%u", (unsigned)missing[0]);
        if (cnt > 1) { FOTA_DEBUG_PRINT("..%u", (unsigned)missing[cnt-1]); }
        FOTA_DEBUG_PRINT("]");
    }
    FOTA_DEBUG_PRINTLN("");
}

// Rozsah na počítanie chýbajúcich chunkov [*lo .. *hi].
//  - HEADER známy (total_chunks>0): [0 .. total_chunks-1].
//  - HEADER neznámy (total_chunks==0): okno [najnižší .. najvyšší prijatý] z bitmapy
//    (chunky pod najnižším prijatým nevieme bez HEADER-a spoľahlivo nárokovať).
// Vracia false = "zero info yet" (žiaden chunk a žiaden HEADER).
static bool fota_missing_range(uint16_t* lo, uint16_t* hi) {
    if (fota.total_chunks > 0) { *lo = 0; *hi = (uint16_t)(fota.total_chunks - 1u); return true; }
    // HEADER neznámy — počítaj diery od chunku 0 po NAJVYŠŠÍ prijatý (chunky pod
    // najnižším prijatým reálne existujú a chýbajú, preto počítame od 0).
    int fhi = -1;
    for (uint16_t i = 0; i < FOTA_MAX_CHUNKS; i++)
        if (FOTA_BIT_GET(fota.bitmap, i)) fhi = (int)i;
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
        if (!FOTA_BIT_GET(fota.bitmap, i)) {
            if (out && n < max_out) out[n++] = i;
            total++;
        }
        if (i == hi) break;   // bezpečné aj pre uint16_t (hi môže byť 0/65535)
    }
    if (out_n) *out_n = n;
    return total;
}

// Vypíše chýbajúce CHUNKY na Serial (bez prefixu/newline; H/S a riadok rieši volajúci).
// Súvislý beh chýbajúcich sa zlúči do rozsahu "od-do" (napr. "4-11"), jednotlivý ako "5".
// 'limit' = strop v TOKENOCH (jednotlivé číslo = 1 token, rozsah "od-do" = 2); <=0 = bez stropu.
// Beh sa NEoreže — vypíše sa celý; po vyčerpaní tokenov sa zvyšok zhrnie do "+N" (počet
// zvyšných chýbajúcich CHUNKOV). Nič netlačí ak niet rozsahu.
void fota_print_missing(int limit) {
    uint16_t lo, hi;
    if (!fota_missing_range(&lo, &hi)) return;
    int total = 0, shown = 0, tokens = 0;
    bool in_run = false; uint16_t rs = 0, re = 0;
    for (uint16_t i = lo; ; i++) {
        bool missing = !FOTA_BIT_GET(fota.bitmap, i);
        if (missing) {
            total++;
            if (!in_run) { rs = re = i; in_run = true; } else re = i;
        }
        if (in_run && (!missing || i == hi)) {     // koniec behu: vypíš ho celý (ak je budget)
            if (limit <= 0 || tokens < limit) {
                if (re != rs) { FOTA_DEBUG_PRINT("%u-%u ", (unsigned)rs, (unsigned)re); tokens += 2; }
                else          { FOTA_DEBUG_PRINT("%u ", (unsigned)rs); tokens += 1; }
                shown += (int)(re - rs + 1);
            }
            in_run = false;
        }
        if (i == hi) break;
    }
    if (limit > 0 && total > shown) { FOTA_DEBUG_PRINT("+%d", total - shown); }
}

// Naformátuje chýbajúce CHUNKY do 'out' ako rozsahy s vedúcou medzerou (" 5", " 4-11").
// 'limit' = strop v TOKENOCH (číslo = 1, rozsah = 2); <=0 = bez stropu. Beh sa NEoreže.
// Po vyčerpaní tokenov ALEBO pri zaplnení out sa zvyšok zhrnie do " +N" (počet chunkov).
// Vracia počet znakov. Bez veľkého stack-bufferu — píše priamo do 'out' (LoRa reply ~160 B).
int fota_format_missing(char* out, int out_sz, int limit) {
    if (out_sz <= 0) return 0;
    out[0] = 0;
    uint16_t lo, hi;
    if (!fota_missing_range(&lo, &hi)) return 0;
    char* p = out;
    char* cap = out + out_sz - 12;                 // rezerva na " +NNNNN"
    int total = 0, shown = 0, tokens = 0;
    bool full = false;                             // buffer plný (zvyšok do "+N")
    bool in_run = false; uint16_t rs = 0, re = 0;
    for (uint16_t i = lo; ; i++) {
        bool missing = !FOTA_BIT_GET(fota.bitmap, i);
        if (missing) {
            total++;
            if (!in_run) { rs = re = i; in_run = true; } else re = i;
        }
        if (in_run && (!missing || i == hi)) {
            if (!full && (limit <= 0 || tokens < limit)) {
                int w = (re == rs) ? snprintf(p, cap - p, " %u", (unsigned)rs)
                                   : snprintf(p, cap - p, " %u-%u", (unsigned)rs, (unsigned)re);
                if (w < 0 || p + w >= cap) full = true;     // nezmestí → zvyšok do "+N"
                else { p += w; tokens += (re == rs) ? 1 : 2; shown += (int)(re - rs + 1); }
            }
            in_run = false;
        }
        if (i == hi) break;
    }
    if (total > shown) p += snprintf(p, out + out_sz - p, " +%d", total - shown);
    return (int)(p - out);
}

// ---- Odložená žiadosť o flash (ACK „accepted" musí odísť PRED rebootom) ----
static bool s_apply_pending = false;
void fota_request_apply()      { s_apply_pending = true; }
bool fota_apply_pending()      { return s_apply_pending; }
void fota_clear_apply_pending(){ s_apply_pending = false; }

// Postav STATUS paket (6B). Vždy dostupný ak je session.
int fota_build_status(uint8_t* out) {
    if (fota.total_chunks == 0) return 0;
    FotaStatusPkt* p = (FotaStatusPkt*)out;
    p->type         = FOTA_PKT_STATUS;
    p->recv_count   = fota.recv_count;
    p->total_chunks = fota.total_chunks;
    p->status       = fota.status;
    return (int)sizeof(FotaStatusPkt);
}

// Postav NACK paket (2 + count*2). Vracia 0 ak nič nechýba.
int fota_build_nack(uint8_t* out) {
    if (fota.total_chunks == 0) return 0;
    FotaNackPkt* p = (FotaNackPkt*)out;
    p->type  = FOTA_PKT_NACK;
    p->count = 0;
    for (uint16_t i = 0; i < fota.total_chunks && p->count < FOTA_NACK_MAX_IDX; i++)
        if (!FOTA_BIT_GET(fota.bitmap, i))
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
    out[1] = fota.fota_prot_inf;
    memcpy(out + 2,  &fota.patch_size, 4);
    memcpy(out + 6,  fota.patch_sha256, 32);
    memcpy(out + 38, fota.new_sha256, 32);
    memcpy(out + 70, fota.old_sha256, 32);
}

// Keď máme META aj SIG → over podpis a "promuj" hlavičku (nastav total_chunks).
// Bezpečnostný invariant: total_chunks (a teda completion/flash) sa nastaví LEN po
// úspešnom overení podpisu nad rekonštruovanou 102 B META.
static void try_verify_header() {
    if (!(fota.meta_recv && fota.sig_recv)) return;
    if (fota.total_chunks > 0) return;            // už promované

    uint8_t meta[102];
    rebuild_meta(meta);
    bool ok;
#ifdef FOTA_ALLOW_UNSIGNED
    bool is_unsigned = (fota.hdr_sig[0] == 0 && fota.hdr_sig[1] == 0 &&
                        fota.hdr_sig[2] == 0 && fota.hdr_sig[3] == 0);
    if (is_unsigned) { FOTA_DEBUG_PRINTLN("[FOTA] HEADER: UNSIGNED (FOTA_ALLOW_UNSIGNED)"); ok = true; }
    else
#endif
    ok = verify_header_signature(fota.hdr_sig, meta, 102u, fota.hdr_key_id);

    if (!ok) {
        FOTA_DEBUG_PRINTLN("[FOTA] HEADER: INVALID signature — rejecting");
        fota_set_error(FOTA_ERR_SIGNATURE);
        return;
    }

    uint32_t tc = (fota.patch_size + FOTA_CHUNK_DATA_MAX - 1u) / FOTA_CHUNK_DATA_MAX;
    if (tc == 0 || tc > FOTA_MAX_CHUNKS) {
        FOTA_DEBUG_PRINTLN("[FOTA] HEADER: zlé total_chunks=%lu", (unsigned long)tc); return;
    }
    fota.total_chunks = (uint16_t)tc;
    fota.recv_count   = bitmap_popcount();
    fota.status       = FOTA_ST_RECEIVING;
    save_bitmap();
    save_meta();
    FOTA_DEBUG_PRINTLN("[FOTA] HEADER OK (META+SIG overené) chunks=%lu  mám %u chunkov", (unsigned long)tc, (unsigned)fota.recv_count);

    // Chunky mohli doraziť pred hlavičkou → over COMPLETE hneď
    if (fota.recv_count >= fota.total_chunks) {
        fota.status |= FOTA_ST_COMPLETE;
        save_meta();
        FOTA_DEBUG_PRINTLN("[FOTA] COMPLETE — assembly + SHA256...");
        if (assemble_and_verify()) {
            fota.status |= FOTA_ST_VERIFIED; save_meta();
            FOTA_DEBUG_PRINTLN("[FOTA] VERIFIED — 'fota verify'=dry-run | 'fota flash'=flash+reboot");
        } else {
            fota_set_error(FOTA_ERR_SHA256); save_meta();
        }
    }
}

// FOTA_PKT_HEADER = META (metadáta patchu, podpisované). Idempotentné (opätovné
// prijatie len prepíše rovnaké polia). Verify+promócia spraví try_verify_header.
static void handle_meta(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(FotaHeaderPkt)) { FOTA_DEBUG_PRINTLN("[FOTA] META: krátky"); return; }
    const FotaHeaderPkt* pkt = (const FotaHeaderPkt*)plain;

    // Base FW gating — META.old_sha256 MUSÍ sedieť s bežiacim FW, inak patch nepatrí
    // tomuto zariadeniu. Platí AJ keď HEADER príde pred prvým chunkom (vtedy
    // base_fw_size==0 a SHA sa doráta nad fw_image_size). Drop (nie ERROR) — cudzí
    // paket nesmie zhodiť ani založiť NAŠU session. Vypíš (ako pri chunku).
    if (!fota_meta_base_ok(pkt->old_sha256)) {
        FOTA_DEBUG_PRINTLN("[FOTA] META: base FW nezhoda — patch nie je pre toto zariadenie, drop");
        return;
    }

    // Re-send identickej META? Spočítaj PRED prípadným fota_clear (ten zeruje
    // patch_sha256). Ak je to DUP, preskočíme save_meta() — flash-zápis blokuje
    // RX cestu (nRF52 NVMC halt) a spôsobí stratu nasledujúceho SIG/APPLY paketu.
    bool dup_meta = fota.meta_recv && (fota.status & FOTA_ST_RECEIVING)
                 && fota.patch_size == pkt->patch_size
                 && memcmp(fota.patch_sha256, pkt->patch_sha256, 32) == 0;

    bool partial = (fota.status & FOTA_ST_RECEIVING) && fota.total_chunks == 0;
    bool other_patch = (fota.status & FOTA_ST_RECEIVING) && fota.total_chunks > 0 &&
                       memcmp(fota.patch_sha256, pkt->patch_sha256, 32) != 0;
    if (!(fota.status & FOTA_ST_RECEIVING) || other_patch) {
        // Nová session (alebo iný patch beží) — vyčisti FS
        FotaFS.remove(FOTA_FS_LOG);
        FotaFS.remove(FOTA_FS_PATCH);
        FotaFS.remove(FOTA_FS_BITMAP);
        fota_clear();
        fota.status = FOTA_ST_RECEIVING;
        fota.total_chunks = 0;
    }
    (void)partial;   // partial chunky sa zachovajú (merge), nič nemažeme

    fota.fota_prot_inf = pkt->fota_prot_inf;
    fota.patch_size   = pkt->patch_size;
    memcpy(fota.patch_sha256, pkt->patch_sha256, 32);
    memcpy(fota.new_sha256,   pkt->new_sha256,   32);
    memcpy(fota.old_sha256,   pkt->old_sha256,   32);
    fota.meta_recv = 1;
    if (!dup_meta) save_meta();   // DUP re-send → žiadny flash zápis (nestalluj RX)
    FOTA_DEBUG_PRINT("[FOTA] META prijaté patch_size=%lu B  patch_sha256=", (unsigned long)pkt->patch_size);
    for (int i = 0; i < 6; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)pkt->patch_sha256[i]); }
    FOTA_DEBUG_PRINTLN("...  %s", dup_meta ? "meta_recv=1 DUP → skip save" : "NEW/CHANGED → save");
    try_verify_header();
}

// FOTA_PKT_HDR_SIG = SIG (Ed25519 podpis META). Gating cez old_sha256.
static void handle_sig(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(FotaHdrSigPkt)) { FOTA_DEBUG_PRINTLN("[FOTA] SIG: krátky"); return; }
    const FotaHdrSigPkt* pkt = (const FotaHdrSigPkt*)plain;

    if (fota.meta_recv) {
        if (memcmp(fota.old_sha256, pkt->old_sha256, 32) != 0) {
            FOTA_DEBUG_PRINTLN("[FOTA] SIG: old_sha256 nezhoda s META — drop"); return;
        }
    } else if (!fota_meta_base_ok(pkt->old_sha256)) {
        // SIG prišiel pred META — over base FW VŽDY (aj pri base_fw_size==0), inak by
        // SIG-first založil session pre cudzí patch. (SIG.old_sha256 = "gating patrí mne".)
        FOTA_DEBUG_PRINTLN("[FOTA] SIG: base FW nezhoda — patch nie je pre toto zariadenie, drop"); return;
    }
    if (!(fota.status & FOTA_ST_RECEIVING)) { fota.status = FOTA_ST_RECEIVING; fota.total_chunks = 0; }

    // Re-send identického SIG? DUP → preskoč save_meta() (rovnaký dôvod ako META).
    bool dup_sig = fota.sig_recv && fota.hdr_key_id == pkt->key_id
                && memcmp(fota.hdr_sig, pkt->signature, 64) == 0;
    fota.hdr_key_id = pkt->key_id;
    memcpy(fota.hdr_sig, pkt->signature, 64);
    fota.sig_recv = 1;
    if (!dup_sig) save_meta();   // DUP re-send → žiadny flash zápis (nestalluj RX)
    FOTA_DEBUG_PRINTLN("[FOTA] SIG prijaté key_id=0x%X  %s", (unsigned)pkt->key_id,
        dup_sig ? "sig_recv=1 DUP → skip save" : "NEW/CHANGED → save");
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
        FOTA_DEBUG_PRINTLN("[FOTA] CHUNK: base FW nezhoda — drop");
        return;
    }

    // Session init alebo merge:
    //  - žiadna session → vytvor partial (total_chunks=0, base z chunku);
    //    skoré chunky sa rovno bufferujú, HEADER ich neskôr "promuje".
    //  - existujúca session s INÝM base FW → ignoruj (nemiešaj patche).
    if (!(fota.status & FOTA_ST_RECEIVING)) {
        FotaFS.remove(FOTA_FS_LOG);
        FotaFS.remove(FOTA_FS_PATCH);
        FotaFS.remove(FOTA_FS_BITMAP);
        fota_clear();
        fota.old_fw_size = pkt->old_fw_size;
        memcpy(fota.old_sha256, pkt->old_sha256_prefix, 4);  // zvyšok doplní HEADER
        fota.status = FOTA_ST_RECEIVING;
        fota.total_chunks = 0;  // čaká HEADER (alebo promóciu)
        save_meta();
        FOTA_DEBUG_PRINTLN("[FOTA] CHUNK: partial session z chunku (čaká HEADER)");
    } else if (memcmp(fota.old_sha256, pkt->old_sha256_prefix, 4) != 0) {
        return;  // chunk patrí inému base FW než bežiaca session
    }
    // HEADER nenesie old_fw_size — session ho preberá z chunku (base už overený
    // vyššie cez fota_base_fw_validated). Bez tohto by ostal 0 po HEADER ceste.
    fota.old_fw_size = pkt->old_fw_size;

    // Hranica idx: kým nepoznáme total_chunks (pred HEADER), bufferuj až po MAX.
    uint16_t max_idx = (fota.total_chunks > 0) ? fota.total_chunks : (uint16_t)FOTA_MAX_CHUNKS;
    if (idx >= max_idx) {
        if (fota.total_chunks > 0) fota_set_error(FOTA_ERR_OVERFLOW);
        return;
    }

    // Presná dĺžka chunku — AES-ECB dopĺňa plaintext na 16B blok; padding treba
    // strhnúť, inak CRC (sender ráta cez presnú dĺžku) nesedí. Posledný chunk má
    // dĺžku z patch_size, tú poznáme až po HEADER — preto sa posledný chunk PRED
    // HEADER neuloží (CRC zlyhá na paddingu) a príde znova v ďalšom cykle.
    uint16_t exp_len = FOTA_CHUNK_DATA_MAX;
    if (fota.total_chunks > 0 && idx == (uint16_t)(fota.total_chunks - 1u)) {
        uint32_t rem = fota.patch_size - (uint32_t)(fota.total_chunks - 1u) * FOTA_CHUNK_DATA_MAX;
        exp_len = (rem > FOTA_CHUNK_DATA_MAX) ? FOTA_CHUNK_DATA_MAX : (uint16_t)rem;
    }
    if (data_len > exp_len) data_len = exp_len;

    uint16_t calc_crc = fota_crc16(data, data_len);
    if (calc_crc != rx_crc) {
        FOTA_DEBUG_PRINTLN("[FOTA] CRC ERR idx=%u  exp=0x%04X  got=0x%04X", (unsigned)idx, (unsigned)rx_crc, (unsigned)calc_crc);
        return;
    }

    if (FOTA_BIT_GET(fota.bitmap, idx)) return;  // duplikát s OK CRC

    if (!log_append(idx, data, data_len)) {
        fota_set_error(FOTA_ERR_STORAGE); return;
    }

    FOTA_BIT_SET(fota.bitmap, idx);
    fota.recv_count++;

    s_bitmap_dirty++;
    bool complete = (fota.total_chunks > 0) && (fota.recv_count >= fota.total_chunks);
    if (s_bitmap_dirty >= FOTA_BITMAP_SAVE_EVERY || complete)
        save_bitmap();

    if (fota.recv_count % 20 == 0 || complete) {
        FOTA_DEBUG_PRINTLN("[FOTA] %u/%u", (unsigned)fota.recv_count, (unsigned)fota.total_chunks);
    }

    if (complete) {
        fota.status |= FOTA_ST_COMPLETE;
        save_meta();
        FOTA_DEBUG_PRINTLN("[FOTA] COMPLETE — assembly + SHA256...");

        if (assemble_and_verify()) {
            fota.status |= FOTA_ST_VERIFIED;
            save_meta();
            FOTA_DEBUG_PRINTLN("[FOTA] VERIFIED — 'fota verify'=dry-run | 'fota flash'=flash+reboot");
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
    if (memcmp(pkt->sha256, fota.patch_sha256, 32) != 0) {
        FOTA_DEBUG_PRINTLN("[FOTA] APPLY: SHA256 nesúhlasí"); return;
    }
    fota_apply();
}

// =====================================================================
// Výpis FOTA paketu
// =====================================================================
void fota_print_pkt(const uint8_t* plain, int plen, float rssi, float snr) {
    if (plen < 1) return;
    uint8_t type = plain[0];

    switch (type) {
        case FOTA_PKT_HEADER: {   // META
            if (plen < (int)sizeof(FotaHeaderPkt)) { FOTA_DEBUG_PRINTLN("[FOTA] META (krátky)"); return; }
            const FotaHeaderPkt* p = (const FotaHeaderPkt*)plain;
            uint32_t ps; memcpy(&ps, &p->patch_size, 4);
            uint32_t tc = (ps + FOTA_CHUNK_DATA_MAX - 1u) / FOTA_CHUNK_DATA_MAX;
            FOTA_DEBUG_PRINT("[FOTA] META  v%u  size=%lu B  chunks~%lu  patch=", (unsigned)p->fota_prot_inf, (unsigned long)ps, (unsigned long)tc);
            for (int i = 0; i < 4; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)p->patch_sha256[i]); }
            FOTA_DEBUG_PRINT("...  new=");
            for (int i = 0; i < 4; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)p->new_sha256[i]); }
            FOTA_DEBUG_PRINTLN("...");
            return;
        }
        case FOTA_PKT_HDR_SIG: {  // SIG
            if (plen < (int)sizeof(FotaHdrSigPkt)) { FOTA_DEBUG_PRINTLN("[FOTA] SIG (krátky)"); return; }
            const FotaHdrSigPkt* p = (const FotaHdrSigPkt*)plain;
            FOTA_DEBUG_PRINT("[FOTA] SIG  v%u  key_id=0x%X  sig=", (unsigned)p->fota_prot_inf, (unsigned)p->key_id);
            for (int i = 0; i < 4; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)p->signature[i]); }
            FOTA_DEBUG_PRINT("...  old=");
            for (int i = 0; i < 4; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)p->old_sha256[i]); }
            FOTA_DEBUG_PRINTLN("...");
            return;
        }
        case FOTA_PKT_CHUNK: {
            if (plen < (int)(sizeof(FotaChunkPkt) - FOTA_CHUNK_DATA_MAX)) { FOTA_DEBUG_PRINTLN("[FOTA] CHUNK (krátky)"); return; }
            const FotaChunkPkt* p = (const FotaChunkPkt*)plain;
            uint16_t dlen = (uint16_t)(plen - (int)(sizeof(FotaChunkPkt) - FOTA_CHUNK_DATA_MAX));
            uint16_t calc  = fota_crc16(p->data, dlen);
            bool crc_ok    = (calc == p->crc16);

            FOTA_DEBUG_PRINT("[FOTA] CHUNK  idx=%u", (unsigned)p->chunk_idx);
            if (fota.total_chunks > 0) { FOTA_DEBUG_PRINT("/%u", (unsigned)fota.total_chunks); }
            FOTA_DEBUG_PRINT("  len=%u B  crc=0x%04X %s", (unsigned)dlen, (unsigned)p->crc16, crc_ok ? "OK" : "BAD");
            if (p->chunk_idx < FOTA_MAX_CHUNKS && FOTA_BIT_GET(fota.bitmap, p->chunk_idx)) FOTA_DEBUG_PRINT(" DUP");
            FOTA_DEBUG_PRINT("  base=");
            for (int i = 0; i < 4; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)p->old_sha256_prefix[i]); }
            FOTA_DEBUG_PRINTLN("");
            return;
        }
        case FOTA_PKT_APPLY: {
            if (plen < (int)sizeof(FotaApplyPkt)) { FOTA_DEBUG_PRINTLN("[FOTA] APPLY (krátky)"); return; }
            const FotaApplyPkt* p = (const FotaApplyPkt*)plain;
            bool sha_ok = (memcmp(p->sha256, fota.patch_sha256, 32) == 0);
            FOTA_DEBUG_PRINT("[FOTA] APPLY  sha256=");
            for (int i = 0; i < 8; i++) { FOTA_DEBUG_PRINT("%02X", (unsigned)p->sha256[i]); }
            FOTA_DEBUG_PRINTLN("%s", sha_ok ? "...  OK" : "...  NESEDÍ");
            return;
        }
        default:
            FOTA_DEBUG_PRINTLN("[FOTA] ? type=0x%X", (unsigned)type);
            return;
    }
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
    if (!(fota.status & FOTA_ST_VERIFIED)) {
        FOTA_DEBUG_PRINTLN("[FOTA] APPLY: nie je verifikované — spusti príjem chunkov"); return false;
    }
    uint8_t prev_status = fota.status;   // pre obnovu ak flash zlyhá (base-check a pod.)
    fota.status = FOTA_ST_APPLYING;
    save_meta();

    extern bool fota_flash_via_flasher();
    if (fota_flash_via_flasher()) return true;  // NEVRÁTI SA pri úspechu

    // Flash zlyhal pred skokom (napr. base FW != old). Obnov VERIFIED.
    fota.status = prev_status;
    save_meta();
    return false;
}

// =====================================================================
// fota_clear_session — vymaže FOTA súbory z FS, resetuje RAM stav
// =====================================================================
void fota_clear_session() {
    FOTA_DEBUG_PRINTLN("[FOTA] Mazem FOTA session...");
    int removed = 0;
    const char* files[] = { FOTA_FS_META, FOTA_FS_BITMAP, FOTA_FS_LOG, FOTA_FS_PATCH };
    for (int i = 0; i < 4; i++) {
        if (FotaFS.remove(files[i])) {
            FOTA_DEBUG_PRINTLN("[FOTA] rm %s", files[i]);
            removed++;
        }
    }
    fota_clear();
    FOTA_DEBUG_PRINTLN("[FOTA] Hotovo — vymazaných %d súborov", removed);
}

#endif  // WITH_LORA_FOTA

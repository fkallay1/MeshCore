// =====================================================================
// OtaPatcher.cpp — aplikácia OTA patchu (MeshCore port z FK_lora-sniffer)
// =====================================================================
#ifdef WITH_LORA_OTA
#include "OtaPatcher.h"
#include "OtaFs.h"
#include "OtaState.h"
#include <Arduino.h>
#include <SHA256.h>          // rweather/Crypto
#include <nrf.h>             // NRF_NVMC, NVMC_CONFIG_WEN_*

// HPatchLite — vendorovaná v nrfota/hpatchlite/ (include path z build_flags)
#if __has_include("hpatch_lite.h")
  #include "hpatch_lite.h"
  #define OTA_HAS_HPATCH 1
#else
  #define OTA_HAS_HPATCH 0
#endif

// Streaming DEFLATE decompressor pre ZLIB dry-run
#include "puff_stream.h"

// flasher_code.h — generuj: python nrfota/tools/build_flasher.py
#if __has_include("flasher_code.h")
  #include "flasher_code.h"
  #define OTA_HAS_FLASHER 1
#else
  #define OTA_HAS_FLASHER 0
#endif

extern const OtaState* ota_get_state();

static void print_sha16(const uint8_t* h) {
    for (int i = 0; i < 16; i++) { if (h[i] < 0x10) Serial.print('0'); Serial.print(h[i], HEX); }
}

// Overí, že aktuálne bežiaci FW (app flash @ APP_FLASH_START) zodpovedá 'old'
// z ktorého ota_sender.py vygeneroval patch. Ak base nesedí → NEPREPISOVAŤ.
static bool ota_verify_old_fw() {
    const OtaState* st = ota_get_state();
    bool all_zero = true;
    for (int i = 0; i < 32 && all_zero; i++) if (st->old_sha256[i]) all_zero = false;
    if (all_zero || st->old_fw_size == 0) {
        Serial.println(F("[OLD] old_sha256 neznámy — kontrola base preskočená"));
        return true;
    }
    if (st->old_fw_size > APP_FLASH_MAX) {
        Serial.print(F("[OLD] CHYBA: old_fw_size ")); Serial.print(st->old_fw_size);
        Serial.println(F(" > APP_FLASH_MAX")); return false;
    }
    SHA256 sha; sha.reset();
    sha.update((const void*)APP_FLASH_START, st->old_fw_size);
    uint8_t h[32]; sha.finalize(h, sizeof(h));
    Serial.print(F("[OLD] base app flash SHA256=")); print_sha16(h); Serial.println(F("..."));
    Serial.print(F("[OLD] očakávaný old_sha256 =")); print_sha16(st->old_sha256); Serial.println(F("..."));
    if (memcmp(h, st->old_sha256, 32) != 0) {
        Serial.println(F("[OLD] BASE NESEDÍ — bežiaci FW != old z patchu! NEPREPISUJEM."));
        Serial.println(F("[OLD]   Vygeneruj patch voči aktuálnemu firmvéru."));
        return false;
    }
    Serial.println(F("[OLD] base FW sedí s patchom"));
    return true;
}

#if OTA_HAS_FLASHER
// ── NVMC (priamy prístup po sd_softdevice_disable) ─────────────────────
static void nvmc_erase_page(uint32_t addr) {
    while (!NRF_NVMC->READY);
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een;
    __DMB(); __ISB();
    NRF_NVMC->ERASEPAGE = addr;
    while (!NRF_NVMC->READY);
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
    __DMB();
}

static void nvmc_write_words(uint32_t dst_addr, const uint32_t* src, uint32_t word_count) {
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
    __DMB(); __ISB();
    volatile uint32_t* dst = (volatile uint32_t*)dst_addr;
    for (uint32_t i = 0; i < word_count; i++) {
        dst[i] = src[i];
        while (!NRF_NVMC->READY);
    }
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
    __DMB();
}

static bool ensure_flasher_written() {
    if (memcmp((const void*)FLASHER_CODE_ADDR, flasher_code, FLASHER_CODE_SIZE) == 0) {
        Serial.println(F("[FLASH] Flasher je aktuálny"));
        return true;
    }
    Serial.println(F("[FLASH] Zapisujem flasher do 0xEB000..."));
    nvmc_erase_page(FLASHER_CODE_ADDR);
    nvmc_write_words(FLASHER_CODE_ADDR, (const uint32_t*)flasher_code, (FLASHER_CODE_SIZE + 3) / 4);
    if (memcmp((const void*)FLASHER_CODE_ADDR, flasher_code, FLASHER_CODE_SIZE) != 0) {
        Serial.println(F("[FLASH] CHYBA: overenie flasher zlyhalo!"));
        return false;
    }
    Serial.println(F("[FLASH] Flasher zapísaný OK"));
    return true;
}
#endif

// ================================================================
// HPatchLite callbacky
// Patch formát: HPatchLite inplaceB (hdiffi -inplaceB old.bin new.bin patch.bin)
// ================================================================
#if OTA_HAS_HPATCH

// Sekvenčné čítanie z File (hpatch_lite_open + hpatch_lite_patch zdieľajú handle)
static hpi_BOOL patch_file_read(hpi_TInputStreamHandle h,
                                hpi_byte* out, hpi_size_t* size) {
    if (*size == 0) return hpi_TRUE;
    File* f = (File*)h;
    int n = f->read(out, (uint32_t)*size);
    if (n <= 0) { *size = 0; return hpi_FALSE; }
    *size = (hpi_size_t)n;
    return hpi_TRUE;
}

// SHA256-only listener pre test mód (hpatchi_listener_t musí byť prvý člen)
typedef struct {
    hpatchi_listener_t base;   // MUSÍ byť prvý
    SHA256   sha;
    uint32_t written;
} ShaListener;

static hpi_BOOL sha_read_old(hpatchi_listener_t* l,
                             hpi_pos_t pos, hpi_byte* out, hpi_size_t size) {
    (void)l;
    if ((uint32_t)pos + (uint32_t)size > APP_FLASH_MAX) return hpi_FALSE;
    memcpy(out, (const void*)(APP_FLASH_START + (uint32_t)pos), size);
    return hpi_TRUE;
}
static hpi_BOOL sha_write_new(hpatchi_listener_t* l,
                              const hpi_byte* data, hpi_size_t size) {
    ShaListener* s = (ShaListener*)l;
    s->sha.update(data, size);
    s->written += size;
    return hpi_TRUE;
}

// Magic komprimovaného patch formátu: 'Z','L','I','B' (LE uint32)
#define ZPATCH_MAGIC  0x42494C5Au

// HPatchLite read_diff callback — streaming DEFLATE z RAM buffra
static hpi_BOOL patch_zlib_read(hpi_TInputStreamHandle h,
                                hpi_byte* out, hpi_size_t* size) {
    if (*size == 0) return hpi_TRUE;
    puff_stream_t* ps = (puff_stream_t*)h;
    uint32_t n = puff_stream_read(ps, (uint8_t*)out, (uint32_t)*size);
    *size = (hpi_size_t)n;
    return (n > 0 || ps->state == PS_DONE) ? hpi_TRUE : hpi_FALSE;
}

// ── TEST MÓD: SHA256-only, nič nezapisuje do flash ────────────────────
bool ota_patch_to_file() {
    const OtaState* st = ota_get_state();
    Serial.println(F("[PATCH] Test: SHA256 verify (bez flash)..."));
    ota_verify_old_fw();   // len informatívne v dry-rune (neblokuje test)

    File patch_f(OtaFS);
    if (!patch_f.open(OTA_FS_PATCH, FILE_O_READ)) {
        Serial.println(F("[PATCH] patch.bin chýba"));
        return false;
    }
    uint32_t patch_size = (uint32_t)patch_f.size();

    // Detekuj komprimovaný formát (magic 'ZLIB' v prvých 4 bajtoch)
    uint32_t magic = 0;
    patch_f.read((uint8_t*)&magic, 4);
    if (magic == ZPATCH_MAGIC) {
        // ── ZLIB streaming dry-run ──────────────────────────────────────
        uint32_t uncomp_sz = 0, new_fw_sz = 0;
        patch_f.read((uint8_t*)&uncomp_sz, 4);
        patch_f.read((uint8_t*)&new_fw_sz, 4);
        uint32_t comp_sz = patch_size - 12;

        Serial.print(F("[PATCH] ZLIB: compressed=")); Serial.print(comp_sz);
        Serial.print(F("B  raw=")); Serial.print(uncomp_sz);
        Serial.print(F("B  new_fw=")); Serial.print(new_fw_sz); Serial.println('B');

        uint8_t* comp_buf = (uint8_t*)malloc(comp_sz);
        if (!comp_buf) {
            patch_f.close();
            Serial.print(F("[PATCH] malloc(")); Serial.print(comp_sz);
            Serial.println(F("B) zlyhalo"));
            return false;
        }
        uint32_t rd = (uint32_t)patch_f.read(comp_buf, comp_sz);
        patch_f.close();
        if (rd != comp_sz) {
            free(comp_buf);
            Serial.println(F("[PATCH] Chyba čítania patch.bin"));
            return false;
        }

        puff_stream_t* ps = (puff_stream_t*)malloc(sizeof(puff_stream_t));
        if (!ps) {
            free(comp_buf);
            Serial.println(F("[PATCH] malloc puff_stream zlyhalo"));
            return false;
        }
        puff_stream_init(ps, comp_buf, comp_sz);

        hpi_compressType compress_type = hpi_compressType_no;
        hpi_pos_t new_size = 0, uncomp_size_hpi = 0;
        hpi_size_t extra_safe = 0;
        if (!hpatchi_inplace_open(ps, patch_zlib_read,
                                  &compress_type, &new_size,
                                  &uncomp_size_hpi, &extra_safe)) {
            Serial.print(F("[PATCH] Neplatny HPatchLite header (ZLIB) ps_err="));
            Serial.println(ps->error);
            free(ps); free(comp_buf);
            return false;
        }
        Serial.print(F("[PATCH] hpatchi: new_size=")); Serial.print((uint32_t)new_size);
        Serial.print(F("B  extra_safe=")); Serial.print((uint32_t)extra_safe); Serial.println('B');

        ShaListener sl;
        sl.written = 0;
        sl.base.diff_data = ps;
        sl.base.read_diff = patch_zlib_read;
        sl.base.read_old  = sha_read_old;
        sl.base.write_new = sha_write_new;

        uint8_t cache[2048];
        bool ok = (bool)hpatch_lite_patch(&sl.base, new_size, cache, sizeof(cache));
        int ps_err = ps->error;
        free(ps);
        free(comp_buf);

        if (!ok) {
            if (ps_err) {
                Serial.print(F("[PATCH] Dekompresia zlyhal: err=")); Serial.println(ps_err);
            } else {
                Serial.println(F("[PATCH] HPatchLite ZLYHALO (ZLIB)"));
            }
            return false;
        }

        uint8_t result_sha[32];
        sl.sha.finalize(result_sha, sizeof(result_sha));
        Serial.print(F("[PATCH] SHA256=")); print_sha16(result_sha); Serial.println(F("..."));

        bool all_zero = true;
        for (int i = 0; i < 32 && all_zero; i++) if (st->new_sha256[i]) all_zero = false;
        if (all_zero) {
            Serial.println(F("[PATCH] Ocakavany SHA256 nezname — overuj manualne"));
            return true;
        }
        if (memcmp(result_sha, st->new_sha256, 32) != 0) {
            Serial.print(F("[PATCH] SHA256 NESEDI  exp=")); print_sha16(st->new_sha256); Serial.println(F("..."));
            return false;
        }
        Serial.println(F("[PATCH] ZLIB patch overeny!"));
        return true;
    }
    patch_f.seek(0);

    // Pôvodný formát: nekomprimovaný HPatchLite
    hpi_compressType compress_type = hpi_compressType_no;
    hpi_pos_t new_size = 0, uncomp_size = 0;
    hpi_size_t extra_safe = 0;
    if (!hpatchi_inplace_open(&patch_f, patch_file_read,
                              &compress_type, &new_size,
                              &uncomp_size, &extra_safe)) {
        patch_f.close();
        Serial.println(F("[PATCH] Neplatny format patchu (hpatchi_inplace_open)"));
        return false;
    }
    if (compress_type != hpi_compressType_no) {
        patch_f.close();
        Serial.println(F("[PATCH] Komprimovany patch nie je podporovany"));
        return false;
    }

    Serial.print(F("[PATCH] new=")); Serial.print((uint32_t)new_size);
    Serial.print(F("B  patch=")); Serial.print(patch_size);
    Serial.print(F("B  extraSafe=")); Serial.print((uint32_t)extra_safe); Serial.println('B');

    ShaListener sl;
    sl.written = 0;
    sl.base.diff_data = &patch_f;
    sl.base.read_diff = patch_file_read;
    sl.base.read_old  = sha_read_old;
    sl.base.write_new = sha_write_new;

    uint8_t cache[2048];
    bool ok = (bool)hpatch_lite_patch(&sl.base, new_size, cache, sizeof(cache));
    patch_f.close();

    if (!ok) { Serial.println(F("[PATCH] HPatchLite ZLYHALO")); return false; }

    uint8_t result_sha[32];
    sl.sha.finalize(result_sha, sizeof(result_sha));
    Serial.print(F("[PATCH] SHA256=")); print_sha16(result_sha); Serial.println(F("..."));

    bool all_zero = true;
    for (int i = 0; i < 32 && all_zero; i++)
        if (st->new_sha256[i]) all_zero = false;

    if (all_zero) {
        Serial.println(F("[PATCH] Očakávaný SHA256 neznámy — overuj manuálne"));
        return true;
    }
    if (memcmp(result_sha, st->new_sha256, 32) != 0) {
        Serial.print(F("[PATCH] SHA256 NESEDÍ  exp="));
        print_sha16(st->new_sha256); Serial.println(F("..."));
        return false;
    }
    Serial.println(F("[PATCH] OK — patch overený!"));
    return true;
}

// ── PRODUKČNÝ MÓD: patch→RAM → jump flasher@0xEB000 ──────────────────
bool ota_flash_via_flasher() {
#if !OTA_HAS_FLASHER
    Serial.println(F("[FLASHER] flasher_code.h chýba."));
    Serial.println(F("[FLASHER] Spusti: python nrfota/tools/build_flasher.py"));
    return false;
#else
    // ── 0: overenie base FW ──
    if (!ota_verify_old_fw()) {
        Serial.println(F("[FLASHER] PRERUŠENÉ — base FW nesedí, neriskujem prepis."));
        return false;
    }

    // ── 1: otvor patch.bin, zisti new_fw_size z hlavičky ──
    File patch_f(OtaFS);
    if (!patch_f.open(OTA_FS_PATCH, FILE_O_READ)) {
        Serial.println(F("[FLASHER] patch.bin chýba"));
        return false;
    }
    uint32_t patch_size = (uint32_t)patch_f.size();
    if (patch_size == 0 || patch_size > OTA_FS_FLASH_SIZE) {
        Serial.print(F("[FLASHER] Neplatná veľkosť patchu: ")); Serial.println(patch_size);
        patch_f.close();
        return false;
    }

    uint32_t new_fw_size = 0;
    {
        uint32_t magic = 0;
        patch_f.read((uint8_t*)&magic, 4);
        if (magic == ZPATCH_MAGIC) {
            uint32_t uncomp_sz = 0;
            patch_f.read((uint8_t*)&uncomp_sz, 4);
            patch_f.read((uint8_t*)&new_fw_size, 4);
            patch_f.seek(0);
            Serial.print(F("[FLASHER] Komprimovany format: staged=")); Serial.print(patch_size);
            Serial.print(F("B  raw=")); Serial.print(uncomp_sz);
            Serial.print(F("B  new_fw=")); Serial.print(new_fw_size); Serial.println('B');
        } else {
            patch_f.seek(0);
            hpi_compressType compress_type = hpi_compressType_no;
            hpi_pos_t new_fw_size64 = 0, uncomp_size = 0;
            hpi_size_t extra_safe = 0;
            if (!hpatchi_inplace_open(&patch_f, patch_file_read,
                                      &compress_type, &new_fw_size64,
                                      &uncomp_size, &extra_safe)) {
                patch_f.close();
                Serial.println(F("[FLASHER] Neplatny format patchu"));
                return false;
            }
            if (compress_type != hpi_compressType_no) {
                patch_f.close();
                Serial.println(F("[FLASHER] Komprimovany HPatchLite nie je podporovany"));
                return false;
            }
            new_fw_size = (uint32_t)new_fw_size64;
            Serial.print(F("[FLASHER] Nekomprimovany format: new_fw=")); Serial.print(new_fw_size);
            Serial.print(F("B  patch=")); Serial.print(patch_size); Serial.println('B');
            patch_f.seek(0);
        }
    }

    // ── 2: načítaj patch.bin do RAM (~50kB) ──
    uint8_t* patch_buf = (uint8_t*)malloc(patch_size);
    if (!patch_buf) {
        patch_f.close();
        Serial.print(F("[FLASHER] malloc(")); Serial.print(patch_size);
        Serial.println(F("B) zlyhalo — nedostatok RAM"));
        return false;
    }
    patch_f.seek(0);
    uint32_t rd = (uint32_t)patch_f.read(patch_buf, patch_size);
    patch_f.close();
    if (rd != patch_size) {
        free(patch_buf);
        Serial.println(F("[FLASHER] Chyba čítania patch.bin"));
        return false;
    }
    Serial.print(F("[FLASHER] Patch v RAM (")); Serial.print(patch_size); Serial.println(F("B)"));

    const uint32_t PATCH_RAM_ADDR = 0x20000000u;   // zhodné s flasher.c
    if (patch_size > 0x20000u) {   // 128kB — limit RAM oblasti pre patch (flasher.ld)
        free(patch_buf);
        Serial.println(F("[FLASHER] Patch > 128kB — nezmestí sa do RAM oblasti"));
        return false;
    }

    // ── 3: zavrieť CustomLFS (len odmount — FS dáta vo flash ZOSTANÚ) ──
    OtaFS.end();

    // ── 4: BYE + disable SoftDevice (USB CDC zmizne) ──
    typedef void(*flasher_fn_t)(uint32_t, uint32_t, uint32_t);
    Serial.print(F("[FLASHER] → 0x")); Serial.print(FLASHER_CODE_ADDR, HEX);
    Serial.println(F(" [BYE] (streaming)"));
    Serial.flush();

    extern uint32_t sd_softdevice_disable(void);
    sd_softdevice_disable();

    // ── 5: flasher kód do 0xEB000 (nvmc, až po sd_disable) ──
    if (!ensure_flasher_written()) {
        NVIC_SystemReset();
    }

    // ── 6: komprimovaný patch do RAM @ PATCH_RAM_ADDR. Po tomto bode ŽIADNE
    //       Serial (USB buffer mohol byť prepísaný). FS sa NEdotýka. ──
    memmove((void*)PATCH_RAM_ADDR, patch_buf, patch_size);

    // ── 7: skok na flasher — NEVRÁTI SA. ──
    ((flasher_fn_t)(FLASHER_CODE_ADDR | 1u))(PATCH_RAM_ADDR, patch_size, new_fw_size);
    while (1);
    return false;  // unreachable
#endif  // OTA_HAS_FLASHER
}

#else  // OTA_HAS_HPATCH == 0

bool ota_patch_to_file() {
    Serial.println(F("[PATCH] HPatchLite nie je nainštalovaná (nrfota/hpatchlite/)."));
    return false;
}
bool ota_flash_via_flasher() {
    Serial.println(F("[FLASHER] HPatchLite nie je nainštalovaná."));
    return false;
}

#endif  // OTA_HAS_HPATCH

// ── Flasher debug marker — NRF_POWER->GPREGRET2 ──────────────────────────
#define NRF_POWER_GPREGRET2 (*(volatile uint32_t*)0x40000514u)
#define NRF_POWER_RESETREAS (*(volatile uint32_t*)0x40000400u)
static uint8_t  s_flasher_step  = 0;
static uint32_t s_gpret2_raw    = 0;
static uint32_t s_resetreas_raw = 0;

void ota_check_flasher_debug() {
    s_gpret2_raw = NRF_POWER_GPREGRET2 & 0xFFu;
    if (s_gpret2_raw != 0u) {
        NRF_POWER_GPREGRET2 = 0u;          // vymaž (SD ešte nebeží → priamy zápis OK)
        s_flasher_step = (uint8_t)s_gpret2_raw;
    }
    s_resetreas_raw = NRF_POWER_RESETREAS;
    NRF_POWER_RESETREAS = s_resetreas_raw; // write-1-to-clear
}

static void print_step(uint8_t step) {
    Serial.print(F("[FLASHER-DBG] Step=0x"));
    if (step < 0x10u) Serial.print('0');
    Serial.print(step, HEX);
    Serial.print(F("  "));
    switch (step) {
        case 0xFF: Serial.println(F("STARTED — crashed before ZLIB check")); break;
        case 0x01: Serial.println(F("ZLIB detected OK")); break;
        case 0x02: Serial.println(F("puff() OK")); break;
        case 0x03: Serial.println(F("hpatchi_inplace_open OK")); break;
        case 0x04: Serial.println(F("hpatchi_inplaceB OK")); break;
        case 0x05: Serial.println(F("flush last page OK")); break;
        case 0x06: Serial.println(F("RESET issued — patch complete!")); break;
        case 0xE0: Serial.println(F("ERR: uncomp_size 0 or > max")); break;
        case 0xE1: Serial.println(F("ERR: puff() failed")); break;
        case 0xE2: Serial.println(F("ERR: puff destlen mismatch")); break;
        case 0xE3: Serial.println(F("ERR: hpatchi_inplace_open failed")); break;
        case 0xE4: Serial.println(F("ERR: compress_type != no")); break;
        case 0xE5: Serial.println(F("ERR: extra_safe > MAX")); break;
        case 0xE6: Serial.println(F("ERR: hpatchi_inplaceB failed")); break;
        case 0xFE: Serial.println(F("FAIL — flasher zlyhal a resetoval sa")); break;
        default:
            if (step >= 0x20u && step <= 0x3Fu) {
                uint32_t kb = (uint32_t)(step - 0x20u) * 16u;
                Serial.print(F("puff progress: dekomprimovaných ~"));
                Serial.print(kb); Serial.println(F("kB"));
            } else {
                Serial.println(F("(unknown)"));
            }
            break;
    }
}

// ── Flash trace log — flasher appenduje eventy do FLASH_TRACE_ADDR ──────
#define FLASH_TRACE_MAX  512u
static void ota_print_flasher_trace() {
    const volatile uint32_t* t = (const volatile uint32_t*)FLASH_TRACE_ADDR;
    if (t[0] == 0xFFFFFFFFu) {
        Serial.println(F("[FLASHER-TRACE] (prázdny — flasher nezapísal trace)"));
        return;
    }
    Serial.println(F("[FLASHER-TRACE] sekvencia eventov flashera:"));
    for (uint32_t i = 0; i < FLASH_TRACE_MAX; i++) {
        uint32_t code = t[i];
        if (code == 0xFFFFFFFFu) break;
        if (code == 0xD0u) { Serial.print(F("  [chk] FNV-1a výstupu (nový FW) = 0x")); Serial.println(t[i + 1], HEX); i++; continue; }
        if (code == 0xD1u) { Serial.print(F("  [vfy] FNV-1a zapísanej flash    = 0x")); Serial.println(t[i + 1], HEX); i++; continue; }
        if (code == 0xD2u) { Serial.println(F("  [vfy] VERIFY OK — flash == hpatchi výstup")); continue; }
        if (code == 0xEAu) { Serial.println(F("  [vfy] VERIFY FAIL — skok do DFU!")); continue; }
        Serial.print(F("  ["));
        if (i < 10) Serial.print(' ');
        Serial.print(i); Serial.print(F("] "));
        print_step((uint8_t)(code & 0xFFu));
    }
}

void ota_print_flasher_debug() {
    Serial.print(F("[FLASHER-DBG] GPREGRET2=0x")); Serial.print(s_gpret2_raw, HEX);
    Serial.print(F("  RESETREAS=0x")); Serial.print(s_resetreas_raw, HEX);
    Serial.print(F(" ("));
    if (s_resetreas_raw & 0x01u) Serial.print(F("PIN "));
    if (s_resetreas_raw & 0x02u) Serial.print(F("WDT! "));
    if (s_resetreas_raw & 0x04u) Serial.print(F("SREQ "));
    if (s_resetreas_raw & 0x08u) Serial.print(F("LOCKUP! "));
    if (s_resetreas_raw == 0u)   Serial.print(F("power-on/none"));
    Serial.println(')');
    if (s_flasher_step != 0u) {
        Serial.print(F("[FLASHER-DBG] posledný krok: "));
        print_step(s_flasher_step);
        s_flasher_step = 0;
    }
    ota_print_flasher_trace();
}

// ── DEBUG: dekomprimuj patch.bin cez puff_stream, vypíš FNV celého raw ──────
void ota_debug_decompress() {
    Serial.print(F("[DBG] app flash @0x")); Serial.print(APP_FLASH_START, HEX); Serial.print(F("[0:16]= "));
    const uint8_t* app = (const uint8_t*)APP_FLASH_START;
    for (int i = 0; i < 16; i++) { if (app[i] < 0x10) Serial.print('0'); Serial.print(app[i], HEX); Serial.print(' '); }
    Serial.println();

    File f(OtaFS);
    if (!f.open(OTA_FS_PATCH, FILE_O_READ)) { Serial.println(F("[DBG] patch.bin chýba")); return; }
    uint32_t sz = (uint32_t)f.size();
    uint32_t magic = 0, uncomp = 0, newfw = 0;
    f.read((uint8_t*)&magic, 4); f.read((uint8_t*)&uncomp, 4); f.read((uint8_t*)&newfw, 4);
    if (magic != 0x42494C5Au) { Serial.println(F("[DBG] nie ZLIB formát")); f.close(); return; }
    uint32_t comp_sz = sz - 12;
    uint8_t* comp = (uint8_t*)malloc(comp_sz);
    if (!comp) { Serial.println(F("[DBG] malloc comp fail")); f.close(); return; }
    uint32_t rd = (uint32_t)f.read(comp, comp_sz); f.close();
    if (rd != comp_sz) { Serial.println(F("[DBG] read fail")); free(comp); return; }

    puff_stream_t* ps = (puff_stream_t*)malloc(sizeof(puff_stream_t));
    if (!ps) { Serial.println(F("[DBG] malloc ps fail")); free(comp); return; }
    puff_stream_init(ps, comp, comp_sz);

    uint32_t fnv = 2166136261u, total = 0;
    uint8_t buf[256];
    for (;;) {
        uint32_t n = puff_stream_read(ps, buf, sizeof(buf));
        if (n == 0) break;
        for (uint32_t i = 0; i < n; i++) { fnv ^= (uint32_t)buf[i]; fnv *= 16777619u; }
        total += n;
    }
    int err = ps->error;
    free(ps); free(comp);

    Serial.print(F("[DBG] puff_stream raw=")); Serial.print(total);
    Serial.print(F("B (exp ")); Serial.print(uncomp); Serial.print(F(")"));
    Serial.print(F("  FNV=0x")); Serial.print(fnv, HEX);
    Serial.print(F("  err=")); Serial.print(err);
    Serial.println(total == uncomp && err == 0 ? F("  [dekompr OK]") : F("  [DEKOMPR CHYBA!]"));
}

#endif  // WITH_LORA_OTA

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

// Globálna inštancia OtaFS — CustomLFS na 0xD4000 (92kB)
CustomLFS OtaFS(OTA_FS_FLASH_ADDR, OTA_FS_FLASH_SIZE, OTA_FS_BLOCK_SIZE);

// =====================================================================
// RAM stav
// =====================================================================
static OtaState   ota;
static uint16_t   s_bitmap_dirty = 0;

// Offset tabuľka pre assembly krok: byte offset DATA v recv.log pre každý chunk.
// 4B × 1024 = 4KB BSS, acceptable pre nRF52840 (248KB RAM).
static uint32_t s_log_data_offset[OTA_MAX_CHUNKS];

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
// Assembly + SHA256 verifikácia (po COMPLETE)
// =====================================================================
static bool assemble_and_verify() {
    Serial.println(F("[OTA] Zostavujem patch.bin..."));

    // --- Prechod 1: offset tabuľka ---
    memset(s_log_data_offset, 0xFF, sizeof(s_log_data_offset));

    File log_r(OtaFS);
    if (!log_r.open(OTA_FS_LOG, FILE_O_READ)) {
        Serial.println(F("[OTA] log: čítanie zlyhal")); return false;
    }

    uint32_t log_pos = 0;
    uint8_t  hdr[4];
    while (log_r.read(hdr, 4) == 4) {
        uint16_t idx      = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
        uint16_t data_len = (uint16_t)(hdr[2] | ((uint16_t)hdr[3] << 8));
        if (idx < ota.total_chunks)
            s_log_data_offset[idx] = log_pos + 4;
        log_pos += 4u + data_len;
        log_r.seek(log_pos);
    }

    // --- Prechod 2: zostavenie patch.bin + SHA256 ---
    OtaFS.remove(OTA_FS_PATCH);
    File out_f(OtaFS);
    if (!out_f.open(OTA_FS_PATCH, FILE_O_WRITE)) {
        log_r.close(); return false;
    }

    SHA256 sha;
    bool   ok = true;
    uint8_t buf[OTA_CHUNK_DATA_MAX];

    for (uint16_t i = 0; i < ota.total_chunks && ok; i++) {
        if (s_log_data_offset[i] == 0xFFFFFFFFu) {
            Serial.print(F("[OTA] chýba chunk ")); Serial.println(i);
            ok = false; break;
        }

        uint16_t exp_len;
        if (i < ota.total_chunks - 1u) {
            exp_len = OTA_CHUNK_DATA_MAX;
        } else {
            uint32_t rem = ota.patch_size - (uint32_t)(ota.total_chunks - 1u) * OTA_CHUNK_DATA_MAX;
            exp_len = (rem > OTA_CHUNK_DATA_MAX) ? OTA_CHUNK_DATA_MAX : (uint16_t)rem;
        }

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
    return true;
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
// Spracovanie OTA_BEGIN
// =====================================================================
static void handle_begin(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(OtaBeginPkt)) { Serial.println(F("[OTA] BEGIN: krátky")); return; }
    const OtaBeginPkt* pkt = (const OtaBeginPkt*)plain;

    if (pkt->total_chunks == 0 || pkt->total_chunks > OTA_MAX_CHUNKS) {
        Serial.print(F("[OTA] BEGIN: neplatné chunks=")); Serial.println(pkt->total_chunks); return;
    }

    // Rovnaká session?
    if ((ota.status & (OTA_ST_RECEIVING | OTA_ST_COMPLETE | OTA_ST_VERIFIED)) &&
        ota.total_chunks == pkt->total_chunks &&
        ota.patch_size   == pkt->patch_size   &&
        memcmp(ota.patch_sha256, pkt->patch_sha256, 32) == 0)
    {
        Serial.print(F("[OTA] BEGIN: rovnaká session, mám "));
        Serial.print(ota.recv_count); Serial.print('/');
        Serial.print(ota.total_chunks); Serial.println(F(" chunkov"));
        return;
    }

    // Nová session
    OtaFS.remove(OTA_FS_LOG);
    OtaFS.remove(OTA_FS_PATCH);
    OtaFS.remove(OTA_FS_BITMAP);
    ota_clear();

    ota.total_chunks = pkt->total_chunks;
    ota.patch_size   = pkt->patch_size;
    memcpy(ota.patch_sha256, pkt->patch_sha256, 32);
    memcpy(ota.new_sha256,   pkt->new_sha256,   32);
    ota.old_fw_size = pkt->old_fw_size;
    memcpy(ota.old_sha256,   pkt->old_sha256,   32);
    ota.status = OTA_ST_RECEIVING;
    save_meta();

    Serial.print(F("[OTA] BEGIN  chunks=")); Serial.print(pkt->total_chunks);
    Serial.print(F("  patch_size="));        Serial.print(pkt->patch_size); Serial.println('B');
    Serial.print(F("[OTA]   old_fw_size=")); Serial.print(pkt->old_fw_size);
    Serial.print(F("B  old_sha256="));
    for (int i = 0; i < 6; i++) { if (pkt->old_sha256[i] < 0x10) Serial.print('0'); Serial.print(pkt->old_sha256[i], HEX); }
    Serial.println(F("..."));
    Serial.print(F("[OTA]   patch_sha256="));
    for (int i = 0; i < 6; i++) { if (pkt->patch_sha256[i] < 0x10) Serial.print('0'); Serial.print(pkt->patch_sha256[i], HEX); }
    Serial.println(F("..."));
    Serial.print(F("[OTA]   new_sha256="));
    for (int i = 0; i < 6; i++) { if (pkt->new_sha256[i] < 0x10) Serial.print('0'); Serial.print(pkt->new_sha256[i], HEX); }
    Serial.println(F("..."));
}

// =====================================================================
// Spracovanie OTA_CHUNK
// =====================================================================
static void handle_chunk(const uint8_t* plain, int plen) {
    if (!(ota.status & OTA_ST_RECEIVING)) { Serial.println(F("[OTA] CHUNK: bez session")); return; }
    if (plen < 5) return;

    uint16_t idx;
    uint16_t rx_crc;
    memcpy(&idx,    plain + 1, 2);
    memcpy(&rx_crc, plain + 3, 2);
    const uint8_t* data    = plain + 5;
    uint16_t       data_len = (uint16_t)(plen - 5);

    if (idx >= ota.total_chunks) { ota_set_error(OTA_ERR_OVERFLOW); return; }

    // Presná dĺžka chunku je deterministická z idx/patch_size — NESPOLIEHAJ sa na
    // dĺžku paketu. AES-ECB dopĺňa plaintext na 16B blok; padding treba strhnúť,
    // inak CRC (počítané senderom cez presnú dĺžku) nesedí.
    uint16_t exp_len;
    if (idx < ota.total_chunks - 1u) {
        exp_len = OTA_CHUNK_DATA_MAX;
    } else {
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
    bool complete = (ota.recv_count >= ota.total_chunks);
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
        case OTA_PKT_BEGIN: {
            if (plen < (int)sizeof(OtaBeginPkt)) { Serial.println(F("BEGIN (krátky)")); return; }
            const OtaBeginPkt* p = (const OtaBeginPkt*)plain;
            uint16_t tc; memcpy(&tc, &p->total_chunks, 2);
            uint32_t ps; memcpy(&ps, &p->patch_size,   4);
            Serial.print(F("BEGIN  chunks=")); Serial.print(tc);
            Serial.print(F("  size="));        Serial.print(ps); Serial.print('B');
            Serial.print(F("  patch="));
            for (int i = 0; i < 4; i++) { if (p->patch_sha256[i] < 0x10) Serial.print('0'); Serial.print(p->patch_sha256[i], HEX); }
            Serial.print(F("...  new="));
            for (int i = 0; i < 4; i++) { if (p->new_sha256[i] < 0x10) Serial.print('0'); Serial.print(p->new_sha256[i], HEX); }
            Serial.print(F("..."));
            break;
        }
        case OTA_PKT_CHUNK: {
            if (plen < 5) { Serial.println(F("CHUNK (krátky)")); return; }
            uint16_t idx;    memcpy(&idx,    plain + 1, 2);
            uint16_t rx_crc; memcpy(&rx_crc, plain + 3, 2);
            uint16_t dlen = (uint16_t)(plen - 5);
            uint16_t calc  = ota_crc16(plain + 5, dlen);
            bool crc_ok    = (calc == rx_crc);

            Serial.print(F("CHUNK  idx="));  Serial.print(idx);
            if (ota.total_chunks > 0) { Serial.print('/'); Serial.print(ota.total_chunks); }
            Serial.print(F("  len="));       Serial.print(dlen); Serial.print('B');
            Serial.print(F("  crc=0x"));     Serial.print(rx_crc, HEX);
            Serial.print(crc_ok ? F("  OK") : F("  BAD"));
            if (idx < OTA_MAX_CHUNKS && OTA_BIT_GET(ota.bitmap, idx)) Serial.print(F(" DUP"));
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
        case OTA_PKT_BEGIN: handle_begin(plain, plen); return true;
        case OTA_PKT_CHUNK: handle_chunk(plain, plen); return true;
        case OTA_PKT_APPLY: handle_apply(plain, plen); return true;
        default:            return false;
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

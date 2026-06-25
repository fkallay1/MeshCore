#pragma once
// =====================================================================
// FotaProtocol.h — on-air protokol OTA-over-LoRa (MeshCore port)
//
// Port z FK_lora-sniffer/src/fota_proto.h. Portable: žiadne Arduino
// závislosti, použiteľné aj na PC strane (fota_sender.py / ctypes).
//
// V MeshCore prichádzajú OTA pakety zabalené v GRP_DATA (AES-128-ECB +
// HMAC-SHA256), repeater ich dešifruje cez mesh::Utils::MACThenDecrypt
// a v onGroupDataRecv() odovzdá do FotaReceiver. Plaintext má tvar:
//   [ts 4B LE][fota_type 1B][...]   — OTA payload začína na fota_type.
// =====================================================================
#include <stdint.h>

// =====================================================================
// Typy OTA paketov (prvý bajt OTA payloadu — za 4B timestampom GRP_DATA)
// =====================================================================
#define FOTA_PKT_HEADER    0x10   // PC → zariadenie: META (metadáta patchu, podpisované)
#define FOTA_PKT_CHUNK    0x11   // PC → zariadenie: jeden chunk patch dát
#define FOTA_PKT_APPLY    0x12   // PC → zariadenie: aplikuj patch
#define FOTA_PKT_HDR_SIG   0x13   // PC → zariadenie: SIG (Ed25519 podpis META) — 2. časť HEADER
#define FOTA_PKT_STATUS   0x20   // zariadenie → PC: stav prijímania
#define FOTA_PKT_NACK     0x21   // zariadenie → PC: chýbajúce chunky

// Zjednotený OTA formát v0 (bridge aj companion) — viď
// docs/superpowers/specs/2026-06-23-fota-companion-mcpy-design.md
#define FOTA_MAGIC         0x07A0 // GRP_DATA data_type pre OTA (gating diskriminátor)
#define FOTA_PROT_INF_V0   0x00   // verzia OTA protokolu/štruktúr

// Max dát v jednom FOTA_CHUNK. Cez štandardný GRP_DATA (sendGroupData) je
// data_len ≤ MAX_GROUP_DATA_LENGTH(165); data = [ts 4B] + chunk(13 + DATA),
// teda DATA ≤ 148. Volíme 144 s rezervou (zhodné s fota_sender.py FOTA_CHUNK_DATA).
#define FOTA_CHUNK_DATA_MAX  144

// Max chunkov, ktoré zmestíme do jedného FOTA_NACK
#define FOTA_NACK_MAX_IDX    60

// =====================================================================
// On-air štruktúry (packed, bez paddingu)
// =====================================================================

// FOTA_PKT_HEADER — META, 102 B = CELÁ podpisovaná správa (Ed25519). Zmestí sa do
// GRP_DATA (data_len = 4B ts + 102 = 106 ≤ 165). total_chunks sa NEposiela
// (odvodí sa z patch_size/FOTA_CHUNK_DATA_MAX), old_sha256_prefix sa NEposiela
// (= old_sha256[:4]) — obe sú funkciou podpísaných polí → integrita zachovaná.
typedef struct __attribute__((packed)) {
    uint8_t  type;             // FOTA_PKT_HEADER
    uint8_t  fota_prot_inf;     // FOTA_PROT_INF_V0 — verzia protokolu/štruktúr
    uint32_t patch_size;       // LE [bajty]; total_chunks = ceil(patch_size/FOTA_CHUNK_DATA_MAX)
    uint8_t  patch_sha256[32]; // SHA256 patch.bin (na overenie príjmu)
    uint8_t  new_sha256[32];   // SHA256 nového firmware po aplikácii patchu
    uint8_t  old_sha256[32];   // SHA256 starého fw — base gating + overenie pred prepisom
} FotaHeaderPkt;                // = 102 B

// FOTA_PKT_HDR_SIG — SIG, 99 B (2. časť HEADER). data_len = 4 + 99 = 103 ≤ 165.
// Podpis kryje LEN 102 B META; key_id mimo podpisu (zlý key_id → verify zlyhá).
typedef struct __attribute__((packed)) {
    uint8_t  type;             // FOTA_PKT_HDR_SIG
    uint8_t  fota_prot_inf;     // FOTA_PROT_INF_V0 (zhodné s META; META je autoritatívne)
    uint8_t  old_sha256[32];   // gating "patrí mne" (== META.old_sha256, pre-filter)
    uint8_t  key_id;           // ktorý autor podpísal
    uint8_t  signature[64];    // Ed25519 podpis nad 102 B META
} FotaHdrSigPkt;                // = 99 B

// FOTA_CHUNK — 13 + data_len B (max 13+144 = 157 B)
// +8B oproti pôvodnému: old_fw_size + old_sha256_prefix pre session izoláciu
// GRP_DATA data = 4B ts + 157B = 161B ≤ 165B ✅
typedef struct __attribute__((packed)) {
    uint8_t  type;                 // FOTA_PKT_CHUNK
    uint16_t chunk_idx;            // LE, 0-based
    uint16_t crc16;                // CRC16/CCITT len nad data[]
    uint32_t old_fw_size;          // LE — veľkosť base FW pre validáciu
    uint8_t  old_sha256_prefix[4]; // prvých 4B SHA256 base FW — rýchla kontrola
    uint8_t  data[FOTA_CHUNK_DATA_MAX];  // reálna dĺžka z pktlen-13
} FotaChunkPkt;

// FOTA_APPLY — 33 B
typedef struct __attribute__((packed)) {
    uint8_t  type;          // FOTA_PKT_APPLY
    uint8_t  sha256[32];    // potvrdenie — musí súhlasiť s prijatým
} FotaApplyPkt;

// FOTA_STATUS — 6 B
typedef struct __attribute__((packed)) {
    uint8_t  type;          // FOTA_PKT_STATUS
    uint16_t recv_count;    // počet prijatých chunkov
    uint16_t total_chunks;  // celkový počet
    uint8_t  status;        // FOTA_ST_* flags (z FotaState.h)
} FotaStatusPkt;

// FOTA_NACK — 2 + count*2 B
typedef struct __attribute__((packed)) {
    uint8_t  type;          // FOTA_PKT_NACK
    uint8_t  count;         // počet záznamov v idx[]
    uint16_t idx[FOTA_NACK_MAX_IDX];  // chýbajúce chunk_idx (len prvých count)
} FotaNackPkt;

// =====================================================================
// CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF) — zhodné s fota_sender.py
// =====================================================================
static inline uint16_t fota_crc16(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000u) ? ((uint16_t)(crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

#pragma once
// =====================================================================
// OtaProtocol.h — on-air protokol OTA-over-LoRa (MeshCore port)
//
// Port z FK_lora-sniffer/src/ota_proto.h. Portable: žiadne Arduino
// závislosti, použiteľné aj na PC strane (ota_sender.py / ctypes).
//
// V MeshCore prichádzajú OTA pakety zabalené v GRP_DATA (AES-128-ECB +
// HMAC-SHA256), repeater ich dešifruje cez mesh::Utils::MACThenDecrypt
// a v onGroupDataRecv() odovzdá do OtaReceiver. Plaintext má tvar:
//   [ts 4B LE][ota_type 1B][...]   — OTA payload začína na ota_type.
// =====================================================================
#include <stdint.h>

// =====================================================================
// Typy OTA paketov (prvý bajt OTA payloadu — za 4B timestampom GRP_DATA)
// =====================================================================
#define OTA_PKT_BEGIN    0x10   // PC → zariadenie: spusti novú OTA session
#define OTA_PKT_CHUNK    0x11   // PC → zariadenie: jeden chunk patch dát
#define OTA_PKT_APPLY    0x12   // PC → zariadenie: aplikuj patch
#define OTA_PKT_STATUS   0x20   // zariadenie → PC: stav prijímania
#define OTA_PKT_NACK     0x21   // zariadenie → PC: chýbajúce chunky

// Max dát v jednom OTA_CHUNK. GRP_DATA plaintext max = MAX_PACKET_PAYLOAD
// (184) − chan_hash(1) − MAC(2) − ts(4) − chunk_hdr(5) = 172, zaokrúhlene
// nadol na bezpečnú hodnotu zhodnú s ota_sender.py (OTA_CHUNK_DATA=150).
#define OTA_CHUNK_DATA_MAX  150

// Max chunkov, ktoré zmestíme do jedného OTA_NACK
#define OTA_NACK_MAX_IDX    60

// =====================================================================
// On-air štruktúry (packed, bez paddingu)
// =====================================================================

// OTA_BEGIN — 111 B
typedef struct __attribute__((packed)) {
    uint8_t  type;             // OTA_PKT_BEGIN
    uint16_t total_chunks;     // LE
    uint32_t patch_size;       // LE [bajty]
    uint8_t  patch_sha256[32]; // SHA256 patch.bin (na overenie príjmu)
    uint8_t  new_sha256[32];   // SHA256 nového firmware po aplikácii patchu
    uint32_t old_fw_size;      // LE — veľkosť STARÉHO fw (z ktorého bol patch generovaný)
    uint8_t  old_sha256[32];   // SHA256 starého fw — overenie že base na zariadení sedí
} OtaBeginPkt;

// OTA_CHUNK — 5 + data_len B (max 155 B)
typedef struct __attribute__((packed)) {
    uint8_t  type;          // OTA_PKT_CHUNK
    uint16_t chunk_idx;     // LE, 0-based
    uint16_t crc16;         // CRC16/CCITT len nad data[]
    uint8_t  data[OTA_CHUNK_DATA_MAX];  // reálna dĺžka z pktlen-5
} OtaChunkPkt;

// OTA_APPLY — 33 B
typedef struct __attribute__((packed)) {
    uint8_t  type;          // OTA_PKT_APPLY
    uint8_t  sha256[32];    // potvrdenie — musí súhlasiť s prijatým
} OtaApplyPkt;

// OTA_STATUS — 6 B
typedef struct __attribute__((packed)) {
    uint8_t  type;          // OTA_PKT_STATUS
    uint16_t recv_count;    // počet prijatých chunkov
    uint16_t total_chunks;  // celkový počet
    uint8_t  status;        // OTA_ST_* flags (z OtaState.h)
} OtaStatusPkt;

// OTA_NACK — 2 + count*2 B
typedef struct __attribute__((packed)) {
    uint8_t  type;          // OTA_PKT_NACK
    uint8_t  count;         // počet záznamov v idx[]
    uint16_t idx[OTA_NACK_MAX_IDX];  // chýbajúce chunk_idx (len prvých count)
} OtaNackPkt;

// =====================================================================
// CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF) — zhodné s ota_sender.py
// =====================================================================
static inline uint16_t ota_crc16(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000u) ? ((uint16_t)(crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

#pragma once
// =====================================================================
// OtaFs.h — OTA filesystem glue (MeshCore port)
//
// Dedikovaný CustomLFS na 0xD4000 (92kB) — oddelený od MeshCore InternalFS
// (ten ostáva na Adafruit defaulte 0xED000-0xF4000 pre identity/prefs/ACL).
// Rovnaký prístup ako companion_radio (CustomLFS ExtraFS(0xD4000,...)).
//
// Flasher beží z 0xEB000 — MIMO app flash aj InternalFS, takže môže
// bezpečne prepisovať 0x26000-0xD4000 bez sebazničenia.
//
// Port z FK_lora-sniffer/src/ota_fs.h.
// =====================================================================

#include <CustomLFS.h>
using namespace Adafruit_LittleFS_Namespace;

// Flash adresy (per-board, freestanding-safe) — single source
#include "flash_layout.h"

#define FLASHER_META_MAGIC  0x464C5348u      // "FLSH"

// Metadata flashera (zapísané pred skokom, čítané flasherom z FLASHER_META_ADDR)
typedef struct __attribute__((packed)) {
    uint32_t magic;          // FLASHER_META_MAGIC
    uint32_t src_addr;       // zdrojová adresa v RAM (nový FW buffer)
    uint32_t fw_size;        // veľkosť nového FW v bajtoch
    uint32_t dst_addr;       // cieľová flash adresa (APP_FLASH_START)
    uint8_t  new_sha256[32]; // SHA256 pre overenie po zápise
} FlasherMeta;

// Globálna inštancia — definovaná v OtaReceiver.cpp, extern v ostatných
extern CustomLFS OtaFS;

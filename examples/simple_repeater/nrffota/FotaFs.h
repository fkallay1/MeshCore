#pragma once
// =====================================================================
//en: FotaFs.h — FOTA filesystem glue (MeshCore port)
//en:
//en: Dedicated CustomLFS at 0xD4000 (92kB) — separate from MeshCore InternalFS
//en: (which stays at the Adafruit default 0xED000-0xF4000 for identity/prefs/ACL).
//en: Same approach as companion_radio (CustomLFS ExtraFS(0xD4000,...)).
//en:
//en: The flasher runs from 0xEB000 — OUTSIDE both the app flash and InternalFS,
//en: so it can safely rewrite 0x26000-0xD4000 without destroying itself.
//en:
//en: Port of FK_lora-sniffer/src/fota_fs.h.
//sk: FotaFs.h — FOTA filesystem glue (MeshCore port)
//sk:
//sk: Dedikovaný CustomLFS na 0xD4000 (92kB) — oddelený od MeshCore InternalFS
//sk: (ten ostáva na Adafruit defaulte 0xED000-0xF4000 pre identity/prefs/ACL).
//sk: Rovnaký prístup ako companion_radio (CustomLFS ExtraFS(0xD4000,...)).
//sk:
//sk: Flasher beží z 0xEB000 — MIMO app flash aj InternalFS, takže môže
//sk: bezpečne prepisovať 0x26000-0xD4000 bez sebazničenia.
//sk:
//sk: Port z FK_lora-sniffer/src/fota_fs.h.
// =====================================================================

#include <CustomLFS.h>
using namespace Adafruit_LittleFS_Namespace;

//en: Flash addresses (per-board, freestanding-safe) — single source
#include "flash_layout.h"

#define FLASHER_META_MAGIC  0x464C5348u      //en: "FLSH"

//en: Flasher metadata (written before the jump, read by the flasher from FLASHER_META_ADDR)
typedef struct __attribute__((packed)) {
    uint32_t magic;          //en: FLASHER_META_MAGIC
    uint32_t src_addr;       //en: source address in RAM (new FW buffer)
    uint32_t fw_size;        //en: size of the new FW in bytes
    uint32_t dst_addr;       //en: destination flash address (APP_FLASH_START)
    uint8_t  new_sha256[32]; //en: SHA256 for verification after the write
} FlasherMeta;

//en: Global instance — defined in FotaReceiver.cpp, extern elsewhere
extern CustomLFS FotaFS;

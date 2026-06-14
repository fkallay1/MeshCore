#pragma once
// =====================================================================
// OtaState.h — RAM stav OTA session + perzistentná hlavička (MeshCore port)
//
// Port z FK_lora-sniffer/src/ota.h.
// =====================================================================
#include "OtaProtocol.h"

// =====================================================================
// Limity
// =====================================================================
#define OTA_MAX_CHUNKS     1024                        // absolútny max
#define OTA_BITMAP_BYTES   ((OTA_MAX_CHUNKS + 7) / 8)  // 128 B

// =====================================================================
// Status bity (OtaState.status + OtaMetaPersist.status)
// =====================================================================
#define OTA_ST_IDLE       0x00   // žiadna session
#define OTA_ST_RECEIVING  0x01   // prijímame chunky
#define OTA_ST_COMPLETE   0x02   // všetky chunky prijaté
#define OTA_ST_VERIFIED   0x04   // SHA256 overená
#define OTA_ST_APPLYING   0x08   // aplikujeme patch
#define OTA_ST_DONE       0x10   // hotovo, čaká reboot
#define OTA_ST_ERROR      0x80   // chyba — detail v err_code

// Kódy chýb
#define OTA_ERR_NONE      0x00
#define OTA_ERR_SHA256    0x01   // hash nesedí
#define OTA_ERR_CRC16     0x02   // chunk CRC chyba
#define OTA_ERR_STORAGE   0x03   // LittleFS I/O
#define OTA_ERR_PATCH     0x04   // hdiffpatch zlyhalo
#define OTA_ERR_OVERFLOW  0x05   // chunk_idx out of range

// =====================================================================
// Perzistentná hlavička  /ota/meta.bin  (114 B)
//
// Zapisuje sa len pri stavových prechodoch (BEGIN / COMPLETE / VERIFIED / DONE).
// CRC16 pokrýva všetko okrem seba — ak CRC nesedí, zápis bol prerušený
// a session sa zahodí.
// =====================================================================
#define OTA_META_MAGIC  0x4F544101u   // "OTA\x01"

typedef struct __attribute__((packed)) {
    uint32_t magic;             // OTA_META_MAGIC
    uint8_t  status;            // OTA_ST_* — stav v čase zápisu
    uint8_t  err_code;          // OTA_ERR_*
    uint16_t total_chunks;
    uint32_t patch_size;        // [B]
    uint8_t  patch_sha256[32];  // SHA256 patch.bin (na overenie príjmu)
    uint8_t  new_sha256[32];    // SHA256 nového firmware (na overenie aplikácie)
    uint32_t old_fw_size;       // veľkosť starého fw (base patchu) — prežije reboot/resume
    uint8_t  old_sha256[32];    // SHA256 starého fw — overenie base pred prepisom
    uint16_t crc16;             // CRC16 všetkého vyššie
} OtaMetaPersist;

// =====================================================================
// RAM stav  (závisí od OtaMetaPersist, bitmap extra)
// Celková veľkosť cca 176 B
// =====================================================================
typedef struct {
    uint16_t total_chunks;              // 0 = žiadna session
    uint32_t patch_size;
    uint8_t  patch_sha256[32];          // SHA256 patch.bin
    uint8_t  new_sha256[32];            // SHA256 nového firmware
    uint32_t old_fw_size;               // veľkosť starého fw (base patchu), 0 = neznáme
    uint8_t  old_sha256[32];            // SHA256 starého fw — overenie base pred prepisom

    uint16_t recv_count;               // prepočítané z bitmap pri resume
    uint8_t  status;                   // OTA_ST_*
    uint8_t  err_code;                 // OTA_ERR_*

    // bitová mapa: bit N = 1 → chunk N prijatý a zapísaný (128 B pokryje 1024 chunkov)
    uint8_t  bitmap[OTA_BITMAP_BYTES];
} OtaState;

// =====================================================================
// LittleFS cesty  (spoločné pre receiver aj patcher)
// =====================================================================
#define OTA_FS_DIR     "/ota"
#define OTA_FS_META    "/ota/meta.bin"
#define OTA_FS_BITMAP  "/ota/bitmap.bin"
#define OTA_FS_LOG     "/ota/recv.log"
#define OTA_FS_PATCH   "/ota/patch.bin"

// =====================================================================
// Bitmap makrá (bez runtime overhead)
// =====================================================================
#define OTA_BIT_SET(bm, n)  ((bm)[(n) >> 3] |=  (uint8_t)(1u << ((n) & 7u)))
#define OTA_BIT_CLR(bm, n)  ((bm)[(n) >> 3] &= (uint8_t)(~(1u << ((n) & 7u))))
#define OTA_BIT_GET(bm, n)  (((bm)[(n) >> 3] >> ((n) & 7u)) & 1u)

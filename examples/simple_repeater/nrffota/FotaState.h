#pragma once
// =====================================================================
// FotaState.h — RAM stav OTA session + perzistentná hlavička (MeshCore port)
//
// Port z FK_lora-sniffer/src/ota.h.
// =====================================================================
#include "FotaProtocol.h"

// =====================================================================
// Limity
// =====================================================================
#define FOTA_MAX_CHUNKS     1024                        // absolútny max
#define FOTA_BITMAP_BYTES   ((FOTA_MAX_CHUNKS + 7) / 8)  // 128 B

// =====================================================================
// Status bity (FotaState.status + FotaMetaPersist.status)
// =====================================================================
#define FOTA_ST_IDLE       0x00   // žiadna session
#define FOTA_ST_RECEIVING  0x01   // prijímame chunky
#define FOTA_ST_COMPLETE   0x02   // všetky chunky prijaté
#define FOTA_ST_VERIFIED   0x04   // SHA256 overená
#define FOTA_ST_APPLYING   0x08   // aplikujeme patch
#define FOTA_ST_DONE       0x10   // hotovo, čaká reboot
#define FOTA_ST_ERROR      0x80   // chyba — detail v err_code

// Kódy chýb
#define FOTA_ERR_NONE      0x00
#define FOTA_ERR_SHA256    0x01   // hash nesedí
#define FOTA_ERR_CRC16     0x02   // chunk CRC chyba
#define FOTA_ERR_STORAGE   0x03   // LittleFS I/O
#define FOTA_ERR_PATCH     0x04   // hdiffpatch zlyhalo
#define FOTA_ERR_OVERFLOW    0x05   // chunk_idx out of range
#define FOTA_ERR_SIGNATURE   0x06   // Ed25519 podpis HEADER paket nie je validny
#define FOTA_ERR_BASEFW      0x07   // base FW nezhoda — chunk/HEADER pre iný FW

// =====================================================================
// Perzistentná hlavička  /ota/meta.bin  (114 B)
//
// Zapisuje sa len pri stavových prechodoch (HEADER / COMPLETE / VERIFIED / DONE).
// CRC16 pokrýva všetko okrem seba — ak CRC nesedí, zápis bol prerušený
// a session sa zahodí.
// =====================================================================
#define FOTA_META_MAGIC  0x4F544101u   // "OTA\x01"

typedef struct __attribute__((packed)) {
    uint32_t magic;             // FOTA_META_MAGIC
    uint8_t  status;            // FOTA_ST_* — stav v čase zápisu
    uint8_t  err_code;          // FOTA_ERR_*
    uint16_t total_chunks;
    uint32_t patch_size;        // [B]
    uint8_t  patch_sha256[32];  // SHA256 patch.bin (na overenie príjmu)
    uint8_t  new_sha256[32];    // SHA256 nového firmware (na overenie aplikácie)
    uint32_t old_fw_size;       // veľkosť starého fw (base patchu) — prežije reboot/resume
    uint8_t  old_sha256[32];    // SHA256 starého fw — overenie base pred prepisom
    // Zjednotený formát v0 — rozdelený HEADER (META+SIG), perzistuje cez reboot:
    uint8_t  fota_prot_inf;      // verzia protokolu z META
    uint8_t  meta_recv;         // META prijaté
    uint8_t  sig_recv;          // SIG prijaté
    uint8_t  hdr_key_id;        // key_id z SIG
    uint8_t  hdr_sig[64];       // Ed25519 podpis z SIG (over po prijatí META+SIG)
    uint16_t crc16;             // CRC16 všetkého vyššie
} FotaMetaPersist;

// =====================================================================
// RAM stav  (závisí od FotaMetaPersist, bitmap extra)
// Celková veľkosť cca 176 B
// =====================================================================
typedef struct {
    uint16_t total_chunks;              // 0 = žiadna session
    uint32_t patch_size;
    uint8_t  patch_sha256[32];          // SHA256 patch.bin
    uint8_t  new_sha256[32];            // SHA256 nového firmware
    uint32_t old_fw_size;               // veľkosť starého fw (base patchu), 0 = neznáme
    uint8_t  old_sha256[32];            // SHA256 starého fw — overenie base pred prepisom

    // Base FW cache — pre rýchlu validáciu bez reštartu SHA256 výpočtu
    uint32_t base_fw_size;              // 0 = nevalidovaný
    uint8_t  base_fw_sha256[32];        // SHA256 aktuálneho FW na zariadení

    uint16_t recv_count;               // prepočítané z bitmap pri resume
    uint8_t  status;                   // FOTA_ST_*
    uint8_t  err_code;                 // FOTA_ERR_*

    // Zjednotený formát v0 — rozdelený HEADER (META+SIG):
    uint8_t  fota_prot_inf;             // verzia z META
    uint8_t  meta_recv;                // META prijaté
    uint8_t  sig_recv;                 // SIG prijaté
    uint8_t  hdr_key_id;               // key_id z SIG
    uint8_t  hdr_sig[64];              // Ed25519 podpis z SIG (over po prijatí META+SIG)

    // bitová mapa: bit N = 1 → chunk N prijatý a zapísaný (128 B pokryje 1024 chunkov)
    uint8_t  bitmap[FOTA_BITMAP_BYTES];
} FotaState;

// =====================================================================
// LittleFS cesty  (spoločné pre receiver aj patcher)
// =====================================================================
#define FOTA_FS_DIR     "/ota"
#define FOTA_FS_META    "/ota/meta.bin"
#define FOTA_FS_BITMAP  "/ota/bitmap.bin"
#define FOTA_FS_LOG     "/ota/recv.log"
#define FOTA_FS_PATCH   "/ota/patch.bin"

// =====================================================================
// Autorizacna tabulka pre Ed25519 overovanie (definovana v FotaReceiver_signkey.cpp)
// =====================================================================
#define FOTA_MAX_AUTHORS  8

typedef struct {
    uint8_t  id;           // key_id
    uint8_t  pub_key[32];  // Ed25519 public key
} FotaAuthorEntry;

// Externá deklarácia tabuľky autorov
extern const FotaAuthorEntry s_authors[];
extern const int s_author_count;

// =====================================================================
// Bitove makra (bez runtime overhead)
// =====================================================================
#define FOTA_BIT_SET(bm, n)  ((bm)[(n) >> 3] |=  (uint8_t)(1u << ((n) & 7u)))
#define FOTA_BIT_CLR(bm, n)  ((bm)[(n) >> 3] &= (uint8_t)(~(1u << ((n) & 7u))))
#define FOTA_BIT_GET(bm, n)  (((bm)[(n) >> 3] >> ((n) & 7u)) & 1u)

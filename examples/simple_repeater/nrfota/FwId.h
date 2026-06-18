#pragma once
// =====================================================================
// FwId.h — FW identity trailer zapečený do firmvéru pri builde.
//
// Štruktúra leží v .rodata (flash) — BEZ zmeny ld scriptu. Nájde sa podľa
// 8-bajtového magicu (FWID_MAGIC). Polia image_size a sha256 vyplní POST-build
// skript test_nrf-ota/gen_fw_trailer.py priamo vo firmware.hex (pred tým, než
// z neho PackageDfu vyrobí .zip / uf2conv .uf2 — takže ich nesú všetky artefakty).
//
// KONVENCIA SHA256 (musí sedieť skript aj device-side overenie):
//   sha256 = SHA256( celý app image [APP_FLASH_START .. +image_size)
//                    so sha256[] poľom vynulovaným )
// build_number aj image_size SÚ súčasťou hashovanej oblasti (vyplnené pred
// výpočtom), takže device ich pred hashom NEnuluje — nuluje len sha256[].
//
// image_size sa zhoduje s firmware.bin z DFU zipu (= old_fw_size od sendera)
// a s fw_image_size() z linker symbolov (__etext + SIZEOF(.data) - APP_FLASH_START).
// =====================================================================
#include <stdint.h>

#define FWID_MAGIC      "FKFWID01"   // 8 bajtov, bez NUL terminátora
#define FWID_MAGIC_LEN  8

typedef struct __attribute__((packed)) {
    char     magic[8];      // "FKFWID01"
    uint32_t image_size;    // LE — veľkosť app image (vyplní post-build)
    uint32_t build_number;  // LE — FW_BUILD_NUMBER (compile-time)
    uint8_t  sha256[32];    // SHA256 image so sha256[]=0 (vyplní post-build)
} FwIdTrailer;               // 48 B

// Inštancia (definícia v FwId.cpp). const → .rodata → flash.
extern const FwIdTrailer fw_id_trailer;

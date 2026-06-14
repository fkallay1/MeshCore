#pragma once
// =====================================================================
// OtaPatcher.h — Aplikácia OTA patchu (MeshCore port z FK_lora-sniffer)
//
// ┌─────────────────────────────────────────────────────────────────┐
// │ TEST (overenie príjmu):  ota_patch_to_file()                    │
// │   • old=XIP flash, patch=CustomLFS/patch.bin                    │
// │   • HPatchLite streaming → SHA256 only (nič nezapisuje)         │
// │                                                                 │
// │ PRODUKCIA — streaming in-place flasher:  ota_flash_via_flasher()│
// │   • new_fw_size z patch hlavičky                                │
// │   • malloc(patch_size) ~50kB (jednorazovo, tesne pred skokom)   │
// │   • OtaFS.end() + sd_softdevice_disable()                       │
// │   • flasher kód do 0xEB000 (mimo app flash + InternalFS)        │
// │   • skok na flasher@0xEB001: streaming HDiffPatch in-place      │
// │     old=XIP, patch=RAM, new=APP_FLASH_START (2-page okno)       │
// │   • flasher: SystemReset — NEVRÁTI SA                           │
// └─────────────────────────────────────────────────────────────────┘
//
// Závislosť: HPatchLite (nrfota/hpatchlite/), puff_stream, flasher_code.h
//   flasher_code.h generuj: python nrfota/tools/build_flasher.py
// =====================================================================

#include "OtaState.h"
#include <stdint.h>

#include "flash_layout.h"   // APP_FLASH_START/MAX, FLASH_PAGE_SIZE

// Verifikuje príjem: aplikuje patch → SHA256 (bez zápisu do flash).
// Vracia true ak patch OK a SHA256 sedí.
bool ota_patch_to_file();

// FS-region flasher: patch→RAM→jump na flasher@0xEB000.
// TÁTO FUNKCIA SA NEVRÁTI ak uspeje. Vracia false len pri chybe (pred skokom).
bool ota_flash_via_flasher();

// DEBUG: dekomprimuj patch.bin cez puff_stream, vypíš FNV celého raw výstupu.
void ota_debug_decompress();

// Prečítaj debug marker flashera z GPREGRET2/RESETREAS. Volaj raz v setup()
// PRED SoftDevice enable. Hodnota sa vytlačí cez ota_print_flasher_debug().
void ota_check_flasher_debug();

// Vytlač uložený flasher debug marker ak existuje (volaj pri banneri/connect).
void ota_print_flasher_debug();

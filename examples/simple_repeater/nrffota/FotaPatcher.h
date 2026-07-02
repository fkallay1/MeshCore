#pragma once
// =====================================================================
// FotaPatcher.h — Aplikácia FOTA patchu (MeshCore port z FK_lora-sniffer)
//
// ┌─────────────────────────────────────────────────────────────────┐
// │ TEST (overenie príjmu):  fota_patch_to_file()                    │
// │   • old=XIP flash, patch=CustomLFS/patch.bin                    │
// │   • HPatchLite streaming → SHA256 only (nič nezapisuje)         │
// │                                                                 │
// │ PRODUKCIA — streaming in-place flasher:  fota_flash_via_flasher()│
// │   • new_fw_size z patch hlavičky                                │
// │   • malloc(patch_size) ~50kB (jednorazovo, tesne pred skokom)   │
// │   • FotaFS.end() + sd_softdevice_disable()                       │
// │   • flasher kód do 0xEB000 (mimo app flash + InternalFS)        │
// │   • skok na flasher@0xEB001: streaming HDiffPatch in-place      │
// │     old=XIP, patch=RAM, new=APP_FLASH_START (2-page okno)       │
// │   • flasher: SystemReset — NEVRÁTI SA                           │
// └─────────────────────────────────────────────────────────────────┘
//
// Závislosť: HPatchLite (nrffota/hpatchlite/), puff_stream, flasher_code.h
//   flasher_code.h generuj: python nrffota/tools/build_flasher.py
// =====================================================================

#include "FotaState.h"
#include <stdint.h>
#include <stddef.h>   // size_t

#include "flash_layout.h"   // APP_FLASH_START/MAX, FLASH_PAGE_SIZE

// Verifikuje príjem: aplikuje patch → SHA256 (bez zápisu do flash).
// Vracia true ak patch OK a SHA256 sedí.
// err/err_sz (voliteľné): krátky ASCII dôvod FAIL (príp. poznámka pri OK) pre CLI.
bool fota_patch_to_file(char* err = nullptr, size_t err_sz = 0);

// FS-region flasher: patch→RAM→jump na flasher@0xEB000.
// TÁTO FUNKCIA SA NEVRÁTI ak uspeje. Vracia false len pri chybe (pred skokom).
bool fota_flash_via_flasher();

// DEBUG: dekomprimuj patch.bin cez puff_stream, vypíš FNV celého raw výstupu.
void fota_debug_decompress();

// Prečítaj debug marker flashera z GPREGRET2/RESETREAS. Volaj raz v setup()
// PRED SoftDevice enable. Hodnota sa vytlačí cez fota_print_flasher_debug().
void fota_check_flasher_debug();

// Vytlač uložený flasher debug marker ak existuje (volaj pri banneri/connect).
void fota_print_flasher_debug();

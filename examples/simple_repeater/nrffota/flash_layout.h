#pragma once
// =====================================================================
// flash_layout.h — OTA flash mapa pre MeshCore nRF52840 repeater.
//
// Iba NUMERICKÉ makrá → freestanding-safe (kompiluje sa aj do standalone
// flashera v tools/build_flasher.py, ktorý beží bez Arduino/newlib).
//
// PREDPOKLAD: app je linkovaná cez boards/nrf52840_s140_v6_extrafs.ld
// (alebo _v7_extrafs.ld), kde app flash KONČÍ na 0xD4000 — rovnako ako
// companion_radio. To uvoľní okno 0xD4000-0xF4000 (128kB) pre FS.
//
// Layout (zhodný princíp s FK_lora-sniffer, prispôsobený MeshCore):
//   APP_FLASH_START - 0xD4000 : aplikačný kód (repeater FW)
//   0xD4000 - 0xEB000         : OTA FS (CustomLFS, 92kB — recv.log/patch.bin/meta/bitmap)
//   0xEB000 - 0xEC000         : flasher kód (4kB ARM Thumb2)
//   0xEC000 - 0xED000         : flasher metadata + trace log (4kB)
//   0xED000 - 0xF4000         : MeshCore InternalFS (28kB, NEDOTKNUTÝ — identity/prefs/ACL)
//   0xF4000+                  : bootloader
//
// Flasher beží z 0xEB000 — MIMO aplikačnej flash (0x26000-0xD4000) aj
// MIMO InternalFS (0xED000+), takže môže bezpečne prepisovať app flash
// bez sebazničenia a bez poškodenia identity/prefs.
//
// Jediný rozdiel medzi boardmi je APP_FLASH_START (veľkosť SoftDevice):
//   s140 v6  → 0x26000  (ProMicro, Heltec T096, ...)  — default
//   s140 v7  → 0x27000  (Seeed XIAO nRF52840, ...)
//
// POZOR: APP_FLASH_START makro je dnes LEGACY/compile-time fallback. Bežiaci FW
// (FotaReceiver/FotaPatcher) ani flasher ho NEPOUŽÍVAJÚ — app base sa berie z
// linker symbolu __flash_arduino_start (ORIGIN(FLASH) aktívneho ld) cez
// fota_running_fw_base() a odovzdáva flasheru runtime. Preto NETREBA board flag.
// APP_FLASH_END (0xD4000) je board-nezávislé (FS okno).
// =====================================================================

#if defined(BOARD_XIAO) || defined(FOTA_SOFTDEVICE_V7)
  // SoftDevice s140 v7.x
  #define APP_FLASH_START      0x27000u
#elif defined(FOTA_APP_FLASH_START)
  // explicitný override z build_flags
  #define APP_FLASH_START      FOTA_APP_FLASH_START
#else
  // SoftDevice s140 v6.1.1 (default — ProMicro a väčšina nRF52840 boardov)
  #define APP_FLASH_START      0x26000u
#endif

// Spoločné pre všetky boardy (FS okno nezávisí od veľkosti SoftDevice)
#define APP_FLASH_END          0xD4000u
#define APP_FLASH_MAX          (APP_FLASH_END - APP_FLASH_START)
#define FLASH_PAGE_SIZE        4096u

#define FOTA_FS_FLASH_ADDR      0xD4000u
#define FOTA_FS_FLASH_SIZE      (0xEB000u - 0xD4000u)   // 92kB (23 stránok × 4096)
#define FOTA_FS_BLOCK_SIZE      128u                    // LittleFS blok = 128B

#define FLASHER_CODE_ADDR      0xEB000u                // 4kB ARM Thumb2 kód flashera
#define FLASHER_META_ADDR      0xEC000u                // 4kB metadata + trace log
#define FLASH_TRACE_ADDR       FLASHER_META_ADDR

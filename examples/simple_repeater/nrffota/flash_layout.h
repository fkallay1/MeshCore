#pragma once
// =====================================================================
//en: flash_layout.h — FOTA flash map for the MeshCore nRF52840 repeater.
//sk: flash_layout.h — FOTA flash mapa pre MeshCore nRF52840 repeater.
//
//en: NUMERIC macros only → freestanding-safe (also compiled into the
//en: standalone flasher in tools/build_flasher.py, which runs without
//en: Arduino/newlib).
//sk: Iba NUMERICKÉ makrá → freestanding-safe (kompiluje sa aj do standalone
//sk: flashera v tools/build_flasher.py, ktorý beží bez Arduino/newlib).
//
//en: ASSUMPTION: the app is linked via boards/nrf52840_s140_v6_extrafs.ld
//en: (or _v7_extrafs.ld), where the app flash ENDS at 0xD4000 — same as
//en: companion_radio. That frees the 0xD4000-0xF4000 window (128kB) for FS.
//sk: PREDPOKLAD: app je linkovaná cez boards/nrf52840_s140_v6_extrafs.ld
//sk: (alebo _v7_extrafs.ld), kde app flash KONČÍ na 0xD4000 — rovnako ako
//sk: companion_radio. To uvoľní okno 0xD4000-0xF4000 (128kB) pre FS.
//
//en: Layout (same principle as FK_lora-sniffer, adapted to MeshCore):
//en:   APP_FLASH_START - 0xD4000 : application code (repeater FW)
//en:   0xD4000 - 0xEB000         : FOTA FS (CustomLFS, 92kB — recv.log/patch.bin/meta/bitmap)
//en:   0xEB000 - 0xEC000         : flasher code (4kB ARM Thumb2)
//en:   0xEC000 - 0xED000         : flasher metadata + trace log (4kB)
//en:   0xED000 - 0xF4000         : MeshCore InternalFS (28kB, UNTOUCHED — identity/prefs/ACL)
//en:   0xF4000+                  : bootloader
//sk: Layout (zhodný princíp s FK_lora-sniffer, prispôsobený MeshCore):
//sk:   APP_FLASH_START - 0xD4000 : aplikačný kód (repeater FW)
//sk:   0xD4000 - 0xEB000         : FOTA FS (CustomLFS, 92kB — recv.log/patch.bin/meta/bitmap)
//sk:   0xEB000 - 0xEC000         : flasher kód (4kB ARM Thumb2)
//sk:   0xEC000 - 0xED000         : flasher metadata + trace log (4kB)
//sk:   0xED000 - 0xF4000         : MeshCore InternalFS (28kB, NEDOTKNUTÝ — identity/prefs/ACL)
//sk:   0xF4000+                  : bootloader
//
//en: The flasher runs from 0xEB000 — OUTSIDE the application flash
//en: (0x26000-0xD4000) and OUTSIDE InternalFS (0xED000+), so it can safely
//en: rewrite the app flash without destroying itself or identity/prefs.
//sk: Flasher beží z 0xEB000 — MIMO aplikačnej flash (0x26000-0xD4000) aj
//sk: MIMO InternalFS (0xED000+), takže môže bezpečne prepisovať app flash
//sk: bez sebazničenia a bez poškodenia identity/prefs.
//
//en: The only difference between boards is APP_FLASH_START (SoftDevice size):
//en:   s140 v6  → 0x26000  (ProMicro, Heltec T096, ...)  — default
//en:   s140 v7  → 0x27000  (Seeed XIAO nRF52840, ...)
//sk: Jediný rozdiel medzi boardmi je APP_FLASH_START (veľkosť SoftDevice):
//sk:   s140 v6  → 0x26000  (ProMicro, Heltec T096, ...)  — default
//sk:   s140 v7  → 0x27000  (Seeed XIAO nRF52840, ...)
//
//en: NOTE: the APP_FLASH_START macro is LEGACY / compile-time fallback today.
//en: Neither the running FW (FotaReceiver/FotaPatcher) nor the flasher USE it
//en: — the app base is taken from the linker symbol __flash_arduino_start
//en: (ORIGIN(FLASH) of the active ld) via fota_running_fw_base() and passed
//en: to the flasher at runtime. Hence no board flag is needed.
//en: APP_FLASH_END (0xD4000) is board-independent (FS window).
//sk: POZOR: APP_FLASH_START makro je dnes LEGACY/compile-time fallback. Bežiaci FW
//sk: (FotaReceiver/FotaPatcher) ani flasher ho NEPOUŽÍVAJÚ — app base sa berie z
//sk: linker symbolu __flash_arduino_start (ORIGIN(FLASH) aktívneho ld) cez
//sk: fota_running_fw_base() a odovzdáva flasheru runtime. Preto NETREBA board flag.
//sk: APP_FLASH_END (0xD4000) je board-nezávislé (FS okno).
// =====================================================================

#if defined(BOARD_XIAO) || defined(FOTA_SOFTDEVICE_V7)
  //en: SoftDevice s140 v7.x
  #define APP_FLASH_START      0x27000u
#elif defined(FOTA_APP_FLASH_START)
  //en: explicit override from build_flags
  #define APP_FLASH_START      FOTA_APP_FLASH_START
#else
  //en: SoftDevice s140 v6.1.1 (default — ProMicro and most nRF52840 boards)
  #define APP_FLASH_START      0x26000u
#endif

//en: Common to all boards (the FS window does not depend on SoftDevice size)
#define APP_FLASH_END          0xD4000u
#define APP_FLASH_MAX          (APP_FLASH_END - APP_FLASH_START)
#define FLASH_PAGE_SIZE        4096u

#define FOTA_FS_FLASH_ADDR      0xD4000u
#define FOTA_FS_FLASH_SIZE      (0xEB000u - 0xD4000u)   //en: 92kB (23 pages x 4096)
#define FOTA_FS_BLOCK_SIZE      128u                    //en: LittleFS block = 128B

#define FLASHER_CODE_ADDR      0xEB000u                //en: 4kB ARM Thumb2 flasher code
#define FLASHER_META_ADDR      0xEC000u                //en: 4kB metadata + trace log
#define FLASH_TRACE_ADDR       FLASHER_META_ADDR

#pragma once
// =====================================================================
// FotaDebug.h — diagnostické výpisy FOTA modulu (vzor: MESH_DEBUG_PRINT
// v src/MeshCore.h). Zapína -D FOTA_DEBUG=1 (FOTA env v platformio.ini);
// bez flagu sa výpisy vôbec nekompilujú (žiadny flash/CPU overhead).
//
// Použitie: printf štýl, text správy vrátane "[FOTA] " prefixu si dáva
// volajúci (naše správy majú rôzne prefixy: [FOTA], [DBG], [FLASHER-DBG]).
// POZOR: CLI odpovede (sprintf do reply bufferu) sem NEpatria — to je
// funkčný výstup pre klienta, nie diagnostika.
// =====================================================================
#if FOTA_DEBUG && ARDUINO
  #include <Arduino.h>
  // "\r\n" (CRLF) ako Serial.println — samotné "\n" robí v termináli "schodíky"
  // (nový riadok bez návratu vozíka). Newline NEvkladaj do formátu ručne,
  // ukonči riadok cez FOTA_DEBUG_PRINTLN.
  #define FOTA_DEBUG_PRINT(F, ...)   Serial.printf(F, ##__VA_ARGS__)
  #define FOTA_DEBUG_PRINTLN(F, ...) Serial.printf(F "\r\n", ##__VA_ARGS__)
#else
  #define FOTA_DEBUG_PRINT(...) {}
  #define FOTA_DEBUG_PRINTLN(...) {}
#endif

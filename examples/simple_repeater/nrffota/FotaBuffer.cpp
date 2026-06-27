// =====================================================================
// FotaBuffer.cpp — implementácia zdieľaného FOTA scratchu.
// Dnes: jeden statický buffer v .bss. Swap na malloc/free = len tieto 2 fn.
// =====================================================================
#ifdef WITH_LORA_FOTA
#include "FotaBuffer.h"
#include <Arduino.h>   // Serial — diagnostika nesprávneho použitia

// Jediné miesto, ktoré drží "odkiaľ" je pamäť. Aligned(4) pre prípadné
// budúce word-orientované použitie.
static uint8_t s_fota_buf[FOTA_BUF_CAP] __attribute__((aligned(4)));
static bool    s_fota_buf_in_use = false;

uint8_t* fota_get_buffer(uint32_t need) {
    if (need > FOTA_BUF_CAP) {
        Serial.print(F("[FOTA] buffer: need ")); Serial.print(need);
        Serial.print(F("B > cap ")); Serial.print(FOTA_BUF_CAP); Serial.println('B');
        return nullptr;
    }
    if (s_fota_buf_in_use) {
        Serial.println(F("[FOTA] buffer: už požičaný (reentrancia?)"));
        return nullptr;
    }
    s_fota_buf_in_use = true;
    return s_fota_buf;
    // SWAP na heap: `return (uint8_t*)malloc(need);` (zruš in_use logiku)
}

void fota_put_buffer(uint8_t* p) {
    if (p != s_fota_buf) {
        Serial.println(F("[FOTA] buffer: put cudzí smerník — ignorujem"));
        return;
    }
    s_fota_buf_in_use = false;
    // SWAP na heap: `free(p);`
}

#endif // WITH_LORA_FOTA

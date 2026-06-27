#pragma once
// =====================================================================
// FotaBuffer.h — zdieľaný scratch buffer pre FOTA (borrow / release).
//
// Účel: oddeliť "ODKIAĽ" je pamäť od "AKO sa používa". Dnes vracia smerník
// na jeden statický buffer v .bss — call-sites (napr. hpatch cache vo
// fota_patch_to_file) ho len požičajú/vrátia. Ak raz budeme chcieť malloc/
// free (alebo recyklovať iný existujúci buffer), zmena je IBA tu + v .cpp;
// volajúce miesta sa nemenia.
//
// POZOR: FOTA operácie nie sú reentrantné — v jednom okamihu je buffer
// požičaný max. raz. Dvojitá výpožička = bug (get vráti NULL).
// =====================================================================
#include <stdint.h>

// Kapacita zdieľaného scratchu [B]. Dimenzované na najväčšieho konzumenta:
// hpatch_lite_patch() read-cache vo verify (min. nutné = hpi_kMinCacheSize = 2 B;
// 512 = pohodlná rezerva pre throughput dry-runu). Flasher má vlastný ~20 kB
// blob v .bss SVOJHO relokovateľného obrazu — s týmto NESÚVISÍ.
#define FOTA_BUF_CAP   512u

// Požičaj scratch s kapacitou >= need bajtov.
// Vráti smerník, alebo NULL ak: need > FOTA_BUF_CAP, alebo je buffer už
// požičaný. Po dokončení práce VŽDY zavolaj fota_put_buffer().
uint8_t* fota_get_buffer(uint32_t need);

// Vráť scratch požičaný cez fota_get_buffer(). 'p' musí byť presne ten
// smerník, ktorý get vrátil (inak sa volanie ignoruje).
void fota_put_buffer(uint8_t* p);

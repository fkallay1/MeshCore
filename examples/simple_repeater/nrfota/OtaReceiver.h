#pragma once
// =====================================================================
// OtaReceiver.h — OTA prijímač: spracovanie paketov, CustomLFS, reboot-resilient
//
// Port z FK_lora-sniffer/src/ota_receiver.h. Spracováva už DEŠIFROVANÝ OTA
// payload (MeshCore GRP_DATA → mesh::Utils::MACThenDecrypt → onGroupDataRecv).
// Žiadna dynamická pamäť (statický stav + statické buffre).
// =====================================================================
#include "OtaState.h"

// Zapisuj bitmap každých N nových chunkov (kompromis wear vs. strata pri reboote)
#define OTA_BITMAP_SAVE_EVERY  8

// =====================================================================
// Verejné API
// =====================================================================

// Inicializácia — mount CustomLFS @ 0xD4000, pokus o resume po reboote.
void ota_init();

// Spracuj jeden OTA paket (plaintext, začína typovým bajtom OTA_PKT_*).
// Vracia true ak bol paket rozpoznaný ako OTA.
bool ota_process(const uint8_t* plain, int plen);

// Manuálne spustenie aplikácie patchu (ak je OTA_ST_VERIFIED).
bool ota_apply();

// Diagnostika cez Serial.
void ota_print_status();
void ota_send_nack();

// Výpis dekódovaného OTA paketu (diagnostika pred spracovaním).
// plain = pointer na OTA paket (začína typovým bajtom OTA_PKT_*).
void ota_print_pkt(const uint8_t* plain, int plen, float rssi, float snr);

// Stav session (len na čítanie).
const OtaState* ota_get_state();

// Reálny app base a veľkosť bežiaceho FW z linker symbolov (v6=0x26000,
// v7=0x27000) — zdroj pravdy pre device-side SHA/verify namiesto makra.
uint32_t ota_running_fw_base(void);
uint32_t ota_running_fw_size(void);

// Výpis FW identity (build#, image_size, trailer sha256) + dopočítaný plný
// SHA256 bežiaceho FW — na porovnanie s old_sha256 v .otapkg.json. reply =
// krátka odpoveď pre LoRa; detaily idú na Serial.
void ota_print_fw_id(char* reply);

// Vymaže všetky OTA súbory z CustomLFS + resetuje RAM stav.
void ota_clear_session();

// Načíta patch do čerstvo malloc-nutého RAM buffra (caller uvoľní free()).
//   default:           zostaví z /ota/recv.log priamo do RAM (bez patch.bin)
//   -D USE_PATCHBIN_FILE: prečíta /ota/patch.bin
// *out_size = veľkosť patchu. Vracia NULL pri chybe (vrátane malloc zlyhania).
uint8_t* ota_acquire_patch_ram(uint32_t* out_size);

// Postav STATUS/NACK paket do out[] (pre odoslanie späť cez LoRa OTA kanál).
// Vracia dĺžku payloadu, alebo 0 ak nie je čo poslať.
int ota_build_status(uint8_t* out);
int ota_build_nack(uint8_t* out);

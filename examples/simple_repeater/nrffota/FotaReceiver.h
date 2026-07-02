#pragma once
// =====================================================================
// FotaReceiver.h — FOTA prijímač: spracovanie paketov, CustomLFS, reboot-resilient
//
// Port z FK_lora-sniffer/src/fota_receiver.h. Spracováva už DEŠIFROVANÝ FOTA
// payload (MeshCore GRP_DATA → mesh::Utils::MACThenDecrypt → onGroupDataRecv).
// Žiadna dynamická pamäť (statický stav + statické buffre).
// =====================================================================
#include "FotaState.h"

// Zapisuj bitmap každých N nových chunkov (kompromis wear vs. strata pri reboote)
#define FOTA_BITMAP_SAVE_EVERY  8

// =====================================================================
// Verejné API
// =====================================================================

// Inicializácia — mount CustomLFS @ 0xD4000, pokus o resume po reboote.
void fota_init();

// Spracuj jeden FOTA paket (plaintext, začína typovým bajtom FOTA_PKT_*).
// Vracia true ak bol paket rozpoznaný ako FOTA.
bool fota_process(const uint8_t* plain, int plen);

// Manuálne spustenie aplikácie patchu (ak je FOTA_ST_VERIFIED).
bool fota_apply();

// Diagnostika cez Serial.
void fota_print_status();
void fota_send_nack();

// Chýbajúce chunky. fota_calc_missing vráti celkový počet (-1 = zero info yet) a
// naplní out[] prvými max_out indexmi (*out_n; oba môžu byť NULL = len počet).
// fota_print_missing vypíše na Serial, fota_format_missing zapíše do bufferu (LoRa reply)
// — obe ako rozsahy "od-do"; 'limit' = strop v TOKENOCH (číslo=1, rozsah=2), <=0 = všetky.
// Rozsah: HEADER známy → [0..total-1]; inak okno prijatých.
int  fota_calc_missing(uint16_t* out, int max_out, int* out_n);
void fota_print_missing(int limit);
int  fota_format_missing(char* out, int out_sz, int limit);

// Odložený flash: 'fota flash' najprv pošle ACK „accepted", flash (fota_apply) sa
// spustí z loop() AŽ keď ACK reálne odíde (inak reboot skôr než sa ACK odvysiela).
void fota_request_apply();
bool fota_apply_pending();
void fota_clear_apply_pending();

// Výpis dekódovaného FOTA paketu (diagnostika pred spracovaním).
// plain = pointer na FOTA paket (začína typovým bajtom FOTA_PKT_*).
void fota_print_pkt(const uint8_t* plain, int plen, float rssi, float snr);

// Stav session (len na čítanie).
const FotaState* fota_get_state();

// Reálny app base a veľkosť bežiaceho FW z linker symbolov (v6=0x26000,
// v7=0x27000) — zdroj pravdy pre device-side SHA/verify namiesto makra.
uint32_t fota_running_fw_base(void);
uint32_t fota_running_fw_size(void);

// Výpis FW identity (build#, image_size, trailer sha256) + dopočítaný plný
// SHA256 bežiaceho FW — na porovnanie s old_sha256 v .fotapkg.json. reply =
// krátka odpoveď pre LoRa; detaily idú na Serial.
void fota_print_fw_id(char* reply);

// Vymaže všetky FOTA súbory z CustomLFS + resetuje RAM stav.
void fota_clear_session();

// Načíta patch do čerstvo malloc-nutého RAM buffra (caller uvoľní free()).
//   default:           zostaví z /ota/recv.log priamo do RAM (bez patch.bin)
//   -D USE_PATCHBIN_FILE: prečíta /ota/patch.bin
// *out_size = veľkosť patchu. Vracia NULL pri chybe (vrátane malloc zlyhania).
uint8_t* fota_acquire_patch_ram(uint32_t* out_size);

// Postav STATUS/NACK paket do out[] (pre odoslanie späť cez LoRa FOTA kanál).
// Vracia dĺžku payloadu, alebo 0 ak nie je čo poslať.
int fota_build_status(uint8_t* out);
int fota_build_nack(uint8_t* out);

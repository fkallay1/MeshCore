// =====================================================================
// FotaMesh.cpp — glue medzi MeshCore a OTA modulom.
// =====================================================================
#ifdef WITH_LORA_FOTA
#include "FotaMesh.h"
#include "FotaReceiver.h"
#include "FotaPatcher.h"
#include <Utils.h>
#include <Arduino.h>
#include <string.h>

void fota_build_channel(mesh::GroupChannel& ch) {
    // MeshCore #-konvencia: secret = SHA256(FOTA_CHANNEL_NAME)[0:16] (meno VRÁTANE '#',
    // zhodné s meshcore_py set_channel device.py:216). secret obsahuje 0x00 →
    // NEhashovať ako string. AES kľúč = secret[:16], HMAC kľúč = secret[:32].
    const char* name = FOTA_CHANNEL_NAME;
    uint8_t full[32];
    mesh::Utils::sha256(full, sizeof(full), (const uint8_t*)name, (int)strlen(name));
    memset(ch.secret, 0, PUB_KEY_SIZE);
    memcpy(ch.secret, full, 16);

    // channel hash = SHA256(secret)[0..PATH_HASH_SIZE] (zhodné s companion addChannel)
    uint8_t h[32];
    mesh::Utils::sha256(h, sizeof(h), ch.secret, 16);
    memcpy(ch.hash, h, PATH_HASH_SIZE);

    Serial.print(F("[FOTA] kanál ")); Serial.print(name);
    Serial.print(F(" hash=0x"));
    if (ch.hash[0] < 0x10) Serial.print('0');
    Serial.println(ch.hash[0], HEX);   // očakávané 0xA4 pre #fkotanrf
}

void fota_handle_command(const char* args, char* reply) {
    while (*args == ' ') args++;

    if (*args == 0 || strcmp(args, "status") == 0) {
        const FotaState* st = fota_get_state();
        sprintf(reply, "FOTA %u/%u st=0x%02X size=%lu err=0x%02X",
                (unsigned)st->recv_count, (unsigned)st->total_chunks,
                (unsigned)st->status, (unsigned long)st->patch_size,
                (unsigned)st->err_code);
        fota_print_status();
    } else if (strcmp(args, "verify") == 0 || strcmp(args, "dryrun") == 0) {
        bool ok = fota_patch_to_file();
        strcpy(reply, ok ? "FOTA dry-run OK" : "FOTA dry-run FAIL");
    } else if (strcmp(args, "flash") == 0 || strcmp(args, "apply") == 0) {
        // fota_apply() sa pri úspechu NEVRÁTI (skok na flasher + reboot)
        fota_apply();
        strcpy(reply, "FOTA flash FAIL (pozri serial)");
    } else if (strcmp(args, "clear") == 0) {
        fota_clear_session();
        strcpy(reply, "FOTA cleared");
    } else if (strcmp(args, "decompress") == 0 || strcmp(args, "decomp") == 0) {
        fota_debug_decompress();
        strcpy(reply, "FOTA decompress -> serial");
    } else if (strcmp(args, "nack") == 0) {
        fota_send_nack();
        strcpy(reply, "FOTA nack -> serial");
    } else if (strcmp(args, "dbg") == 0) {
        fota_print_flasher_debug();
        strcpy(reply, "FOTA dbg -> serial");
    } else if (strcmp(args, "id") == 0 || strcmp(args, "fwid") == 0) {
        fota_print_fw_id(reply);   // build#, image_size, plný running sha256 -> serial
    } else {
        strcpy(reply, "FOTA: status|verify|flash|clear|decompress|nack|dbg|id");
    }
}

#endif  // WITH_LORA_FOTA

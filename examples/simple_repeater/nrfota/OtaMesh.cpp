// =====================================================================
// OtaMesh.cpp — glue medzi MeshCore a OTA modulom.
// =====================================================================
#ifdef WITH_LORA_OTA
#include "OtaMesh.h"
#include "OtaReceiver.h"
#include "OtaPatcher.h"
#include <Utils.h>
#include <Arduino.h>
#include <string.h>

void ota_build_channel(mesh::GroupChannel& ch) {
    const char* psk = OTA_CHANNEL_PSK;
    int plen = (int)strlen(psk);

    // secret = psk doplnené nulami na PUB_KEY_SIZE (32B) — AES kľúč = secret[:16],
    // HMAC kľúč = secret[:32] (zhodné s ota_sender.py psk.ljust(32))
    memset(ch.secret, 0, PUB_KEY_SIZE);
    memcpy(ch.secret, psk, plen > PUB_KEY_SIZE ? PUB_KEY_SIZE : plen);

    // channel hash = sha256(psk)[0..PATH_HASH_SIZE] (sha256 nad RAW psk bajtmi)
    uint8_t full[32];
    mesh::Utils::sha256(full, sizeof(full), (const uint8_t*)psk, plen);
    memcpy(ch.hash, full, PATH_HASH_SIZE);

    Serial.print(F("[OTA] kanál hash=0x"));
    if (ch.hash[0] < 0x10) Serial.print('0');
    Serial.print(ch.hash[0], HEX);
    Serial.print(F("  psk_len=")); Serial.println(plen);
}

void ota_handle_command(const char* args, char* reply) {
    while (*args == ' ') args++;

    if (*args == 0 || strcmp(args, "status") == 0) {
        const OtaState* st = ota_get_state();
        sprintf(reply, "OTA %u/%u st=0x%02X size=%lu err=0x%02X",
                (unsigned)st->recv_count, (unsigned)st->total_chunks,
                (unsigned)st->status, (unsigned long)st->patch_size,
                (unsigned)st->err_code);
        ota_print_status();
    } else if (strcmp(args, "verify") == 0 || strcmp(args, "dryrun") == 0) {
        bool ok = ota_patch_to_file();
        strcpy(reply, ok ? "OTA dry-run OK" : "OTA dry-run FAIL");
    } else if (strcmp(args, "flash") == 0 || strcmp(args, "apply") == 0) {
        // ota_apply() sa pri úspechu NEVRÁTI (skok na flasher + reboot)
        ota_apply();
        strcpy(reply, "OTA flash FAIL (pozri serial)");
    } else if (strcmp(args, "clear") == 0) {
        ota_clear_session();
        strcpy(reply, "OTA cleared");
    } else if (strcmp(args, "decompress") == 0 || strcmp(args, "decomp") == 0) {
        ota_debug_decompress();
        strcpy(reply, "OTA decompress -> serial");
    } else if (strcmp(args, "nack") == 0) {
        ota_send_nack();
        strcpy(reply, "OTA nack -> serial");
    } else if (strcmp(args, "dbg") == 0) {
        ota_print_flasher_debug();
        strcpy(reply, "OTA dbg -> serial");
    } else {
        strcpy(reply, "OTA: status|verify|flash|clear|decompress|nack|dbg");
    }
}

#endif  // WITH_LORA_OTA

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
        char reason[48]; reason[0] = 0;
        bool ok = fota_patch_to_file(reason, sizeof(reason));
        const char* tag = ok ? "OK" : "FAIL";
        if (reason[0]) sprintf(reply, "FOTA dry-run %s: %s", tag, reason);
        else           sprintf(reply, "FOTA dry-run %s", tag);
    } else if (strcmp(args, "flash") == 0 || strcmp(args, "apply") == 0) {
        // NEFLASHUJ tu: fota_apply() sa pri úspechu NEVRÁTI (skok na flasher + reboot),
        // takže by sa ACK nikdy neodvysielal. Najprv pošli „accepted", flash spustí
        // loop() AŽ keď ACK reálne odíde z outbound queue (fota_apply_pending()).
        if (fota_get_state()->status & FOTA_ST_VERIFIED) {
            fota_request_apply();
            strcpy(reply, "FOTA flash accepted");
        } else {
            strcpy(reply, "FOTA flash: nie je VERIFIED (najprv prijmi chunky + verify)");
        }
    } else if (strcmp(args, "clear") == 0) {
        fota_clear_session();
        strcpy(reply, "FOTA cleared");
    } else if (strcmp(args, "decompress") == 0 || strcmp(args, "decomp") == 0) {
        fota_debug_decompress();
        strcpy(reply, "FOTA decompress -> serial");
    } else if (strcmp(args, "nack") == 0) {
        fota_send_nack();
        strcpy(reply, "FOTA nack -> serial");
    } else if (strcmp(args, "miss") == 0 || strcmp(args, "missall") == 0) {
        // Chýbajúce: na začiatku H (META) a S (SIG) ak chýbajú, potom chunky (od 0)
        // ako rozsahy — súvislý beh "od-do" (napr. "4-11"), jednotlivý ako "5".
        // miss = strop FOTA_MISS_OUTTOKENS tokenov (číslo = 1, rozsah = 2; H/S sa NErátajú a vypíšu sa vždy);
        // missall = všetky (capnuté len na dĺžku LoRa paketu). Zvyšné chunky ako "+N".
        // Oba ukážu CELKOVÝ počet. "Zero info yet" len ak neprišlo vôbec nič.
        bool show_all = (args[4] == 'a');               // "missall" má 'a' na args[4]
        const FotaState* st = fota_get_state();
        bool miss_h = !st->meta_recv;
        bool miss_s = !st->sig_recv;
        int chunk_missing = fota_calc_missing(NULL, 0, NULL);   // len celkový počet

        bool any_info = st->meta_recv || st->sig_recv || st->recv_count > 0 || st->total_chunks > 0;
        if (!any_info) {
            Serial.println(F("[FOTA] miss Zero info yet"));
            strcpy(reply, "FOTA miss: Zero info yet");
        } else {
            int chunk_total = (chunk_missing < 0) ? 0 : chunk_missing;
            int hs = (miss_h ? 1 : 0) + (miss_s ? 1 : 0);
            int total = hs + chunk_total;
            int tok_lim = show_all ? 0 : FOTA_MISS_OUTTOKENS;   // 0 = všetky; inak tokenový strop (H/S mimo)

            // Serial: plný detail (H/S vždy + chunky ako rozsahy)
            Serial.print(F("[FOTA] miss ")); Serial.print(total);
            if (st->total_chunks > 0) { Serial.print('/'); Serial.print(st->total_chunks); }
            else                        Serial.print(F(" (pred HEADER)"));
            Serial.print(F(": "));
            if (miss_h) Serial.print(F("H "));
            if (miss_s) Serial.print(F("S "));
            fota_print_missing(tok_lim);
            Serial.println();

            // Reply (LoRa aj Serial CLI): počet + H/S + zoznam rozsahov, capnutý na dĺžku paketu
            char* p = reply;
            p += sprintf(p, "FOTA miss=%d", total);
            if (st->total_chunks > 0) p += sprintf(p, "/%u", (unsigned)st->total_chunks);
            else                       p += sprintf(p, "(no hdr)");
            if (total > 0) *p++ = ':';
            if (miss_h) p += sprintf(p, " H");
            if (miss_s) p += sprintf(p, " S");
            int avail = 158 - (int)(p - reply);          // strop pre LoRa (~160 B)
            if (avail > 8) p += fota_format_missing(p, avail, tok_lim);
            *p = 0;
        }
    } else if (strcmp(args, "dbg") == 0) {
        fota_print_flasher_debug();
        strcpy(reply, "FOTA dbg -> serial");
    } else if (strcmp(args, "id") == 0 || strcmp(args, "fwid") == 0) {
        fota_print_fw_id(reply);   // build#, image_size, plný running sha256 -> serial
    } else {
        strcpy(reply, "FOTA: status|verify|flash|clear|decompress|nack|miss|missall|dbg|id");
    }
}

#endif  // WITH_LORA_FOTA

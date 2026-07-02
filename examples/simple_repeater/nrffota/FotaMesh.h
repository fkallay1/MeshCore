#pragma once
// =====================================================================
// FotaMesh.h — glue medzi MeshCore (GRP_DATA kanál, CLI) a FOTA modulom.
//
// FOTA pakety prichádzajú ako MeshCore GRP_DATA na dedikovanom kanáli.
// MyMesh override-ne searchChannelsByHash()/onGroupDataRecv() a odovzdá
// dešifrovaný payload sem. Ovládanie cez "fota ..." príkazy (serial aj LoRa CLI;
// legacy alias "ota ..." — viď FOTA-CLI-ALIAS v MyMesh.cpp).
// =====================================================================
#include <Mesh.h>   // mesh::GroupChannel, PUB_KEY_SIZE, PATH_HASH_SIZE

// Default FOTA kanál — MeshCore #-konvencia (secret = SHA256(name)[0:16]).
// Override cez build_flags:  -D FOTA_CHANNEL_NAME='"#mojkanal"'
// Zhodné s meshcore_py set_channel(idx, name) aj fota_sender.py fota_channel_secret().
#ifndef FOTA_CHANNEL_NAME
  #define FOTA_CHANNEL_NAME "#fkotanrf"
#endif

// "fota miss" strop výpisu chýbajúcich v TOKENOCH (jednotlivé číslo = 1, rozsah = 2;
// H/S sa do tokenov nerátajú). "fota missall" ignoruje strop (limit 0). Override cez
// build_flags:  -D FOTA_MISS_OUTTOKENS=30
#ifndef FOTA_MISS_OUTTOKENS
  #define FOTA_MISS_OUTTOKENS 20
#endif

// Postav FOTA GroupChannel z mena (secret = SHA256(name)[0:16] doplnené nulami na
// 32B, hash = SHA256(secret)[0]) — zhodné s companion set_channel.
void fota_build_channel(mesh::GroupChannel& ch);

// Spracuj "fota ..." CLI príkaz (status|verify|flash|clear|decompress|nack|miss|missall|dbg|id).
// args = text za "fota"/"ota". reply = výstupný buffer (serial/LoRa odpoveď).
void fota_handle_command(const char* args, char* reply);

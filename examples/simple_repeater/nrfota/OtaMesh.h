#pragma once
// =====================================================================
// OtaMesh.h — glue medzi MeshCore (GRP_DATA kanál, CLI) a OTA modulom.
//
// OTA pakety prichádzajú ako MeshCore GRP_DATA na dedikovanom kanáli (PSK).
// MyMesh override-ne searchChannelsByHash()/onGroupDataRecv() a odovzdá
// dešifrovaný payload sem. Ovládanie cez "ota ..." príkazy (serial aj LoRa CLI).
// =====================================================================
#include <Mesh.h>   // mesh::GroupChannel, PUB_KEY_SIZE, PATH_HASH_SIZE

// Default OTA kanál — MeshCore #-konvencia (secret = SHA256(name)[0:16]).
// Override cez build_flags:  -D OTA_CHANNEL_NAME='"#mojkanal"'
// Zhodné s meshcore_py set_channel(idx, name) aj ota_sender.py ota_channel_secret().
#ifndef OTA_CHANNEL_NAME
  #define OTA_CHANNEL_NAME "#fkotanrf"
#endif

// Postav OTA GroupChannel z mena (secret = SHA256(name)[0:16] doplnené nulami na
// 32B, hash = SHA256(secret)[0]) — zhodné s companion set_channel.
void ota_build_channel(mesh::GroupChannel& ch);

// Spracuj "ota ..." CLI príkaz (status|verify|flash|clear|decompress|nack|dbg).
// args = text za "ota". reply = výstupný buffer (serial/LoRa odpoveď).
void ota_handle_command(const char* args, char* reply);

#pragma once
// =====================================================================
// OtaMesh.h — glue medzi MeshCore (GRP_DATA kanál, CLI) a OTA modulom.
//
// OTA pakety prichádzajú ako MeshCore GRP_DATA na dedikovanom kanáli (PSK).
// MyMesh override-ne searchChannelsByHash()/onGroupDataRecv() a odovzdá
// dešifrovaný payload sem. Ovládanie cez "ota ..." príkazy (serial aj LoRa CLI).
// =====================================================================
#include <Mesh.h>   // mesh::GroupChannel, PUB_KEY_SIZE, PATH_HASH_SIZE

// Default OTA kanál PSK — MUSÍ sa zhodovať s ota_sender.py --psk.
// Override cez build_flags:  -D OTA_CHANNEL_PSK='"moj-tajny-kluc"'
#ifndef OTA_CHANNEL_PSK
  #define OTA_CHANNEL_PSK "meshcore-ota"
#endif

// Postav OTA GroupChannel z PSK (hash = sha256(psk)[0], secret = psk doplnené
// nulami na 32B) — zhodné s meshcore_grp_data_packet() v ota_sender.py.
void ota_build_channel(mesh::GroupChannel& ch);

// Spracuj "ota ..." CLI príkaz (status|verify|flash|clear|decompress|nack|dbg).
// args = text za "ota". reply = výstupný buffer (serial/LoRa odpoveď).
void ota_handle_command(const char* args, char* reply);

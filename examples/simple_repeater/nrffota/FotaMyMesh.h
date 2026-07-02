#pragma once
// =====================================================================
// FotaMyMesh.h — FOTA členy a deklarácie metód triedy MyMesh.
//
// POZOR: NEinclude-ovať samostatne! Tento súbor sa vkladá VNÚTRI tela
// `class MyMesh { ... }` v examples/simple_repeater/MyMesh.h (protected
// sekcia) — obsahuje surové member deklarácie bez obalu triedy. Zmysel:
// jediný 3-riadkový #include hook v MyMesh.h namiesto ~38-riadkového
// bloku (minimálny diff vs upstream). Telá metód: FotaMyMesh.cpp.
//
// Kľúčové návrhy (detaily pri definíciách): deferred spracovanie paketov
// aj CLI (RX callstack je hlboký, 4 kB loop stack; ťažká práca beží až
// z loop() po re-arme rádia), deferred flash (ACK musí odísť pred rebootom).
// =====================================================================

  mesh::GroupChannel _fota_channel;
  bool _fota_ready;
  uint8_t _fota_pending[MAX_PACKET_PAYLOAD];  // odložený FOTA paket pre loop()
  int     _fota_pending_len;                  // 0 = nič nečaká
  float   _fota_pending_rssi, _fota_pending_snr;
  volatile uint32_t _fota_raw_rx;             // RAW rámce (pred dekódom/dešifrou)
  uint32_t          _fota_raw_last_len;
  float             _fota_raw_last_rssi, _fota_raw_last_snr;
  bool     _fota_cli_pending;                 // odložený LoRa CLI príkaz
  uint8_t* _fota_cli_buf;                     // požičaný FotaBuffer so snapshotom
  unsigned long _fota_apply_deadline;         // safety net pre odložený flash
  int  searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) override;
  void onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel, uint8_t* data, size_t len) override;
  void fotaLogRxRaw(float snr, float rssi, const uint8_t raw[], int len);
  void fotaEarlyInit();
  void fotaBegin();
  void fotaLoop();
  bool fotaHandleCliCommand(const char* command, char* reply);
  bool fotaHandleLoRaCli(const ClientInfo* client, const uint8_t* secret,
                         const char* command, char* reply,
                         uint8_t path_hash_size, uint32_t sender_timestamp);
  void runFotaCli(const char* fargs, char* reply);
  bool deferFotaCli(const ClientInfo* client, const uint8_t* secret,
                    const char* fargs, uint8_t path_hash_size,
                    uint32_t sender_timestamp);
  void sendDeferredCliReply(const uint8_t* dest_pub, const uint8_t* secret,
                            const uint8_t* out_path, uint8_t out_path_len,
                            uint8_t path_hash_size, const char* text,
                            uint32_t sender_timestamp);

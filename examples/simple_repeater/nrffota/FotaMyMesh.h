#pragma once
// =====================================================================
//en: FotaMyMesh.h — FOTA member variables and method declarations of the
//en: MyMesh class.
//sk: FotaMyMesh.h — FOTA členy a deklarácie metód triedy MyMesh.
//
//en: WARNING: do NOT include standalone! This file is pasted INSIDE the
//en: body of `class MyMesh { ... }` in examples/simple_repeater/MyMesh.h
//en: (protected section) — it contains raw member declarations without a
//en: class wrapper. Purpose: a single 3-line #include hook in MyMesh.h
//en: instead of a ~38-line block (minimal diff vs upstream). Method
//en: bodies: FotaMyMesh.cpp.
//sk: POZOR: NEinclude-ovať samostatne! Tento súbor sa vkladá VNÚTRI tela
//sk: `class MyMesh { ... }` v examples/simple_repeater/MyMesh.h (protected
//sk: sekcia) — obsahuje surové member deklarácie bez obalu triedy. Zmysel:
//sk: jediný 3-riadkový #include hook v MyMesh.h namiesto ~38-riadkového
//sk: bloku (minimálny diff vs upstream). Telá metód: FotaMyMesh.cpp.
//
//en: Key designs (details at the definitions): deferred handling of both
//en: packets and CLI (the RX callstack is deep, 4 kB loop stack; heavy work
//en: runs from loop() after the radio is re-armed), deferred flash (the ACK
//en: must leave before reboot).
//sk: Kľúčové návrhy (detaily pri definíciách): deferred spracovanie paketov
//sk: aj CLI (RX callstack je hlboký, 4 kB loop stack; ťažká práca beží až
//sk: z loop() po re-arme rádia), deferred flash (ACK musí odísť pred rebootom).
// =====================================================================

  mesh::GroupChannel _fota_channel;
  bool _fota_ready;
  uint8_t _fota_pending[MAX_PACKET_PAYLOAD];  //en: deferred FOTA packet for loop()
  int     _fota_pending_len;                  //en: 0 = nothing pending
  float   _fota_pending_rssi, _fota_pending_snr;
  volatile uint32_t _fota_raw_rx;             //en: RAW frames received (before decode/decrypt)
  volatile uint32_t _fota_raw_tx;             //en: RAW frames sent (handed to the radio)
  uint32_t          _fota_raw_last_len;
  float             _fota_raw_last_rssi, _fota_raw_last_snr;
  bool     _fota_cli_pending;                 //en: deferred LoRa CLI command
  uint8_t* _fota_cli_buf;                     //en: borrowed FotaBuffer with the snapshot
  unsigned long _fota_apply_deadline;         //en: safety net for the deferred flash
  int  searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) override;
  void onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel, uint8_t* data, size_t len) override;
  void fotaLogRxRaw(float snr, float rssi, const uint8_t raw[], int len);
  void fotaLogTxRaw(const uint8_t raw[], int len);
  void fotaEarlyInit();
  void fotaBegin();
  void fotaLoop();
  //en: non-const command — scrubs line-editing artifacts (backspace, arrows) in-place
  //sk: non-const command — čistí artefakty editovania riadku (backspace, šípky) in-place
  bool fotaHandleCliCommand(char* command, char* reply);
  //en: v0-prefix FOTA: return pubkeys of ACL admins matching a 4 B prefix.
  //en: The free function is the strong override of the FotaReceiver hook — friend,
  //en: because this block lands in the protected section of MyMesh.
  //sk: v0-prefix FOTA: vráť pubkey ACL adminov so zhodným 4 B prefixom.
  //sk: Free funkcia je silná verzia hooku z FotaReceiver — friend, lebo tento
  //sk: blok sa vkladá do protected sekcie MyMesh.
  int fotaAclAdminPubkeys(const uint8_t prefix[4], const uint8_t* out_keys[], int max);
  friend int fota_acl_admin_pubkeys(const uint8_t prefix[4], const uint8_t* out_keys[], int max);
#if FOTA_DEBUG
  //en: Serial-only debug CLI (getacl / getpath|setpath by pub_key prefix)
  //sk: Serial-only debug CLI (getacl / getpath|setpath cez pub_key prefix)
  bool fotaHandleSerialPathCli(const char* fargs, char* reply);
#endif
  //en: non-const client — getpath/setpath/missall-with-path write ACL out_path
  //sk: non-const client — getpath/setpath/missall-s-cestou zapisujú ACL out_path
  bool fotaHandleLoRaCli(ClientInfo* client, const uint8_t* secret,
                         const char* command, char* reply,
                         uint8_t path_hash_size, uint32_t sender_timestamp);
  void runFotaCli(const char* fargs, char* reply);
  bool deferFotaCli(const ClientInfo* client, const uint8_t* secret,
                    const char* fargs, uint8_t path_hash_size,
                    uint32_t sender_timestamp, const char* tag);
  void sendDeferredCliReply(const uint8_t* dest_pub, const uint8_t* secret,
                            const uint8_t* out_path, uint8_t out_path_len,
                            uint8_t path_hash_size, const char* text,
                            uint32_t sender_timestamp);

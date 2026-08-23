#include "MyMesh.h"
#include <algorithm>

/* ------------------------------ Config -------------------------------- */

#ifndef LORA_FREQ
  #define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
  #define LORA_BW 250
#endif
#ifndef LORA_SF
  #define LORA_SF 10
#endif
#ifndef LORA_CR
  #define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER 20
#endif

#ifndef ADVERT_NAME
  #define ADVERT_NAME "repeater"
#endif
#ifndef ADVERT_LAT
  #define ADVERT_LAT 0.0
#endif
#ifndef ADVERT_LON
  #define ADVERT_LON 0.0
#endif

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY 300
#endif

//en: FK fork: optional separate delay for FLOOD replies (login PATH+RESPONSE,
//en: flood fallbacks of REQ responses). Lets the reply wait out the echo storm
//en: of the request's own flood in multi-hop meshes — at 300 ms the reply
//en: launches into a channel still busy with re-floods of the request and
//en: rarely survives the trip back. Direct replies keep SERVER_RESPONSE_DELAY.
//en: Enable per-env: -D FK_SERVER_FLOOD_RESPONSE_DELAY=<ms>; without the flag
//en: behaviour is identical to upstream.
//sk: FK fork: voliteľný samostatný delay pre FLOOD odpovede (login
//sk: PATH+RESPONSE, flood fallbacky REQ odpovedí). Odpoveď počká, kým dobehne
//sk: echo búrka floodu samotného requestu vo viac-hopovom meshi — pri 300 ms
//sk: odpoveď štartuje do kanála ešte obsadeného re-floodmi requestu a spiatočnú
//sk: cestu zriedka prežije. Direct odpovede ostávajú na SERVER_RESPONSE_DELAY.
//sk: Zapnutie per-env: -D FK_SERVER_FLOOD_RESPONSE_DELAY=<ms>; bez flagu je
//sk: správanie identické s upstreamom.
#ifdef FK_SERVER_FLOOD_RESPONSE_DELAY
  #define FK_FLOOD_RESP_DELAY   FK_SERVER_FLOOD_RESPONSE_DELAY
#else
  #define FK_FLOOD_RESP_DELAY   SERVER_RESPONSE_DELAY
#endif

#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY 200
#endif

#define FIRMWARE_VER_LEVEL       2

#define REQ_TYPE_GET_STATUS         0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05
#define REQ_TYPE_GET_NEIGHBOURS     0x06
#define REQ_TYPE_GET_OWNER_INFO     0x07     // FIRMWARE_VER_LEVEL >= 2

#define RESP_SERVER_LOGIN_OK        0 // response to ANON_REQ

#define ANON_REQ_TYPE_REGIONS      0x01
#define ANON_REQ_TYPE_OWNER        0x02
#define ANON_REQ_TYPE_BASIC        0x03   // just remote clock

#define CLI_REPLY_DELAY_MILLIS      600

#define LAZY_CONTACTS_WRITE_DELAY    5000

void MyMesh::putNeighbour(const mesh::Identity &id, uint32_t timestamp, float snr) {
#if MAX_NEIGHBOURS // check if neighbours enabled
  // find existing neighbour, else use least recently updated
  uint32_t oldest_timestamp = 0xFFFFFFFF;
  NeighbourInfo *neighbour = &neighbours[0];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    // if neighbour already known, we should update it
    if (id.matches(neighbours[i].id)) {
      neighbour = &neighbours[i];
      break;
    }

    // otherwise we should update the least recently updated neighbour
    if (neighbours[i].heard_timestamp < oldest_timestamp) {
      neighbour = &neighbours[i];
      oldest_timestamp = neighbour->heard_timestamp;
    }
  }

  // update neighbour info
  neighbour->id = id;
  neighbour->advert_timestamp = timestamp;
  neighbour->heard_timestamp = getRTCClock()->getCurrentTime();
  neighbour->snr = (int8_t)(snr * 4);
#endif
}

uint8_t MyMesh::handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood) {
  ClientInfo* client = NULL;
  if (data[0] == 0) {   // blank password, just check if sender is in ACL
    client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    if (client == NULL) {
    #if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Login, sender not in ACL");
    #endif
    }
  }
  if (client == NULL) {
    uint8_t perms;
    if (strcmp((char *)data, _prefs.password) == 0) { // check for valid admin password
      perms = PERM_ACL_ADMIN;
    } else if (strcmp((char *)data, _prefs.guest_password) == 0) { // check guest password
      perms = PERM_ACL_GUEST;
    } else {
#if MESH_DEBUG
      MESH_DEBUG_PRINTLN("Invalid password: %s", data);
#endif
#ifdef FK_DEBUG
      Serial.printf("[FK] LOGIN src=%02X: invalid password\r\n", (unsigned)sender.pub_key[0]);
#endif
      return 0;
    }

    client = acl.putClient(sender, 0);  // add to contacts (if not already known)
    if (sender_timestamp <= client->last_timestamp) {
      MESH_DEBUG_PRINTLN("Possible login replay attack!");
#ifdef FK_DEBUG
      Serial.printf("[FK] LOGIN src=%02X: replay ts=%lu last=%lu\r\n",
                    (unsigned)sender.pub_key[0], (unsigned long)sender_timestamp,
                    (unsigned long)client->last_timestamp);
#endif
      return 0;  // FATAL: client table is full -OR- replay attack
    }

    MESH_DEBUG_PRINTLN("Login success!");
    client->last_timestamp = sender_timestamp;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->permissions &= ~0x03;
    client->permissions |= perms;
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

    if (perms != PERM_ACL_GUEST) {   // keep number of FS writes to a minimum
      dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
    }
  }

  if (is_flood) {
    client->out_path_len = OUT_PATH_UNKNOWN;  // need to rediscover out_path
  }
#ifdef FK_DEBUG
  Serial.printf("[FK] LOGIN src=%02X: accepted %s, out_path=%s\r\n",
                (unsigned)sender.pub_key[0], is_flood ? "flood" : "direct",
                client->out_path_len == OUT_PATH_UNKNOWN ? "unknown" : "known");
#endif

  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  reply_data[4] = RESP_SERVER_LOGIN_OK;
  reply_data[5] = 0;  // Legacy: was recommended keep-alive interval (secs / 16)
  reply_data[6] = client->isAdmin() ? 1 : 0;
  reply_data[7] = client->permissions;
  getRNG()->random(&reply_data[8], 4);   // random blob to help packet-hash uniqueness
  reply_data[12] = FIRMWARE_VER_LEVEL;  // New field

  return 13;  // reply length
}

uint8_t MyMesh::handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)

    return 8 + region_map.exportNamesTo((char *) &reply_data[8], sizeof(reply_data) - 12, REGION_DENY_FLOOD);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    sprintf((char *) &reply_data[8], "%s\n%s", _prefs.node_name, _prefs.owner_info);

    return 8 + strlen((char *) &reply_data[8]);   // reply length
  }
  return 0;
}

uint8_t MyMesh::handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data) {
  if (anon_limiter.allow(rtc_clock.getCurrentTime())) {
    // request data has: {reply-path-len}{reply-path}
    reply_path_len = *data++;
    if (!mesh::Packet::isValidPathLen(reply_path_len)) return 0;  // reject - bad encoding

    mesh::Packet::writePath(reply_path, data, reply_path_len);
    // data += (uint8_t)reply_path_len * reply_path_hash_size;

    memcpy(reply_data, &sender_timestamp, 4);   // prefix with sender_timestamp, like a tag
    uint32_t now = getRTCClock()->getCurrentTime();
    memcpy(&reply_data[4], &now, 4);     // include our clock (for easy clock sync, and packet hash uniqueness)
    reply_data[8] = 0;  // features
#ifdef WITH_RS232_BRIDGE
    reply_data[8] |= 0x01;  // is bridge, type UART
#elif WITH_ESPNOW_BRIDGE
    reply_data[8] |= 0x03;  // is bridge, type ESP-NOW
#endif
    if (_prefs.disable_fwd) {   // is this repeater currently disabled
      reply_data[8] |= 0x80;  // is disabled
    }
    // TODO:  add some kind of moving-window utilisation metric, so can query 'how busy' is this repeater
    return 9;   // reply length
  }
  return 0;
}

int MyMesh::handleRequest(ClientInfo *sender, uint32_t sender_timestamp, uint8_t *payload, size_t payload_len) {
  // uint32_t now = getRTCClock()->getCurrentTimeUnique();
  // memcpy(reply_data, &now, 4);   // response packets always prefixed with timestamp
  memcpy(reply_data, &sender_timestamp, 4); // reflect sender_timestamp back in response packet (kind of like a 'tag')

  if (payload[0] == REQ_TYPE_GET_STATUS) {  // guests can also access this now
    RepeaterStats stats;
    stats.batt_milli_volts = board.getBattMilliVolts();
    stats.curr_tx_queue_len = _mgr->getOutboundTotal();
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv = radio_driver.getPacketsRecv();
    stats.n_packets_sent = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs = uptime_millis / 1000;
    stats.n_sent_flood = getNumSentFlood();
    stats.n_sent_direct = getNumSentDirect();
    stats.n_recv_flood = getNumRecvFlood();
    stats.n_recv_direct = getNumRecvDirect();
    stats.err_events = _err_flags;
    stats.last_snr = (int16_t)(radio_driver.getLastSNR() * 4);
    stats.n_direct_dups = ((SimpleMeshTables *)getTables())->getNumDirectDups();
    stats.n_flood_dups = ((SimpleMeshTables *)getTables())->getNumFloodDups();
    stats.total_rx_air_time_secs = getReceiveAirTime() / 1000;
    stats.n_recv_errors = radio_driver.getPacketsRecvErrors();
    memcpy(&reply_data[4], &stats, sizeof(stats));

    return 4 + sizeof(stats); //  reply_len
  }
  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t perm_mask = ~(payload[1]); // NEW: first reserved byte (of 4), is now inverse mask to apply to permissions

    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);

    // query other sensors -- target specific
    if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
      perm_mask = 0x00;  // just base telemetry allowed
    }
    sensors.querySensors(perm_mask, telemetry);

	// This default temperature will be overridden by external sensors (if any)
    float temperature = board.getMCUTemperature();
    if(!isnan(temperature)) { // Supported boards with built-in temperature sensor. ESP32-C3 may return NAN
      telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature); // Built-in MCU Temperature
    }

    uint8_t tlen = telemetry.getSize();
    memcpy(&reply_data[4], telemetry.getBuffer(), tlen);
    return 4 + tlen; // reply_len
  }
  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    uint8_t res1 = payload[1];   // reserved for future  (extra query params)
    uint8_t res2 = payload[2];
    if (res1 == 0 && res2 == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < acl.getNumClients() && ofs + 7 <= sizeof(reply_data) - 4; i++) {
        auto c = acl.getClientByIdx(i);
        if (c->permissions == 0) continue;  // skip deleted entries
        memcpy(&reply_data[ofs], c->id.pub_key, 6); ofs += 6;  // just 6-byte pub_key prefix
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }
  if (payload[0] == REQ_TYPE_GET_NEIGHBOURS) {
    uint8_t request_version = payload[1];
    if (request_version == 0) {

      // reply data offset (after response sender_timestamp/tag)
      int reply_offset = 4;

      // get request params
      uint8_t count = payload[2]; // how many neighbours to fetch (0-255)
      uint16_t offset;
      memcpy(&offset, &payload[3], 2); // offset from start of neighbours list (0-65535)
      uint8_t order_by = payload[5]; // how to order neighbours. 0=newest_to_oldest, 1=oldest_to_newest, 2=strongest_to_weakest, 3=weakest_to_strongest
      uint8_t pubkey_prefix_length = payload[6]; // how many bytes of neighbour pub key we want
      // we also send a 4 byte random blob in payload[7...10] to help packet uniqueness

      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS count=%d, offset=%d, order_by=%d, pubkey_prefix_length=%d", count, offset, order_by, pubkey_prefix_length);

      // clamp pub key prefix length to max pub key length
      if(pubkey_prefix_length > PUB_KEY_SIZE){
        pubkey_prefix_length = PUB_KEY_SIZE;
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS invalid pubkey_prefix_length=%d clamping to %d", pubkey_prefix_length, PUB_KEY_SIZE);
      }

      // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
      int16_t neighbours_count = 0;
#if MAX_NEIGHBOURS
      NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
      for (int i = 0; i < MAX_NEIGHBOURS; i++) {
        auto neighbour = &neighbours[i];
        if (neighbour->heard_timestamp > 0) {
          sorted_neighbours[neighbours_count] = neighbour;
          neighbours_count++;
        }
      }

      // sort neighbours based on order
      if (order_by == 0) {
        // sort by newest to oldest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting newest to oldest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp > b->heard_timestamp; // desc
        });
      } else if (order_by == 1) {
        // sort by oldest to newest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting oldest to newest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->heard_timestamp < b->heard_timestamp; // asc
        });
      } else if (order_by == 2) {
        // sort by strongest to weakest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting strongest to weakest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr > b->snr; // desc
        });
      } else if (order_by == 3) {
        // sort by weakest to strongest
        MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS sorting weakest to strongest");
        std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
          return a->snr < b->snr; // asc
        });
      }
#endif

      // build results buffer
      int results_count = 0;
      int results_offset = 0;
      uint8_t results_buffer[130];
      for(int index = 0; index < count && index + offset < neighbours_count; index++){
        
        // stop if we can't fit another entry in results
        int entry_size = pubkey_prefix_length + 4 + 1;
        if(results_offset + entry_size > sizeof(results_buffer)){
          MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS no more entries can fit in results buffer");
          break;
        }

#if MAX_NEIGHBOURS
        // add next neighbour to results
        auto neighbour = sorted_neighbours[index + offset];
        uint32_t heard_seconds_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
        memcpy(&results_buffer[results_offset], neighbour->id.pub_key, pubkey_prefix_length); results_offset += pubkey_prefix_length;
        memcpy(&results_buffer[results_offset], &heard_seconds_ago, 4); results_offset += 4;
        memcpy(&results_buffer[results_offset], &neighbour->snr, 1); results_offset += 1;
        results_count++;
#endif

      }

      // build reply
      MESH_DEBUG_PRINTLN("REQ_TYPE_GET_NEIGHBOURS neighbours_count=%d results_count=%d", neighbours_count, results_count);
      memcpy(&reply_data[reply_offset], &neighbours_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_count, 2); reply_offset += 2;
      memcpy(&reply_data[reply_offset], &results_buffer, results_offset); reply_offset += results_offset;

      return reply_offset;
    }
  } else if (payload[0] == REQ_TYPE_GET_OWNER_INFO) {
    sprintf((char *) &reply_data[4], "%s\n%s\n%s", FIRMWARE_VERSION, _prefs.node_name, _prefs.owner_info);
    return 4 + strlen((char *) &reply_data[4]);
  }
  return 0; // unknown command
}

mesh::Packet *MyMesh::createSelfAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  uint8_t app_data_len = _cli.buildAdvertData(ADV_TYPE_REPEATER, app_data);

  return createAdvert(self_id, app_data, app_data_len);
}

File MyMesh::openAppend(const char *fname) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(fname, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(fname, "a");
#else
  return _fs->open(fname, "a", true);
#endif
}

static uint8_t max_loop_minimal[] =  { 0, /* 1-byte */  4, /* 2-byte */  2, /* 3-byte */  1 };
static uint8_t max_loop_moderate[] = { 0, /* 1-byte */  2, /* 2-byte */  1, /* 3-byte */  1 };
static uint8_t max_loop_strict[] =   { 0, /* 1-byte */  1, /* 2-byte */  1, /* 3-byte */  1 };

bool MyMesh::isLooped(const mesh::Packet* packet, const uint8_t max_counters[]) {
  uint8_t hash_size = packet->getPathHashSize();
  uint8_t hash_count = packet->getPathHashCount();
  uint8_t n = 0;
  const uint8_t* path = packet->path;
  while (hash_count > 0) {      // count how many times this node is already in the path
    if (self_id.isHashMatch(path, hash_size)) n++;
    hash_count--;
    path += hash_size;
  }
  return n >= max_counters[hash_size];
}

void MyMesh::sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size) {
  TransportKey req_scope;
  bool is_wildcard = recv_pkt_region != NULL && recv_pkt_region->isWildcard();
  bool req_scope_known = recv_pkt_region != NULL && !is_wildcard
                      && region_map.getTransportKeysFor(*recv_pkt_region, &req_scope, 1) > 0;

  switch (mesh::chooseReplyScope(req_scope_known, is_wildcard, !default_scope.isNull())) {
    case mesh::REPLY_SCOPE_REQUEST:
      sendFloodScoped(req_scope, packet, delay_millis, path_hash_size);   // reply with same scope as request
      break;
    case mesh::REPLY_SCOPE_DEFAULT:
      // requester's scope is unknown: DIRECT request (no transport codes), or code matched no Region.
      // un-scoped would be dropped at hop 0 by repeaters running flood.max.unscoped=0
      sendFloodScoped(default_scope, packet, delay_millis, path_hash_size);
      break;
    case mesh::REPLY_SCOPE_NONE:
      sendFlood(packet, delay_millis, path_hash_size);  // send un-scoped
      break;
  }
}

bool MyMesh::allowPacketForward(const mesh::Packet *packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood()
      && mesh::isFloodHopLimitExceeded(packet, _prefs.flood_max, _prefs.flood_max_unscoped, _prefs.flood_max_advert)) {
    return false;
  }
  if (packet->isRouteFlood() && recv_pkt_region == NULL) {
    MESH_DEBUG_PRINTLN("allowPacketForward: unknown transport code, or wildcard not allowed for FLOOD packet");
    return false;
  }
  if (packet->isRouteFlood() && _prefs.loop_detect != LOOP_DETECT_OFF) {
    const uint8_t* maximums;
    if (_prefs.loop_detect == LOOP_DETECT_MINIMAL) {
      maximums = max_loop_minimal;
    } else if (_prefs.loop_detect == LOOP_DETECT_MODERATE) {
      maximums = max_loop_moderate;
    } else {
      maximums = max_loop_strict;
    }
    if (isLooped(packet, maximums)) {
      MESH_DEBUG_PRINTLN("allowPacketForward: FLOOD packet loop detected!");
      return false;
    }
  }
  return true;
}

const char *MyMesh::getLogDateTime() {
  static char tmp[32];
  uint32_t now = getRTCClock()->getCurrentTime();
  DateTime dt = DateTime(now);
  sprintf(tmp, "%02d:%02d:%02d - %d/%d/%d U", dt.hour(), dt.minute(), dt.second(), dt.day(), dt.month(),
          dt.year());
  return tmp;
}

void MyMesh::logRxRaw(float snr, float rssi, const uint8_t raw[], int len) {
#ifdef WITH_LORA_FOTA
  fotaLogRxRaw(snr, rssi, raw, len);   //en: RAW diagnostics + FOTA tag (nrffota/FotaMyMesh.cpp)
#endif
#if MESH_PACKET_LOGGING
  Serial.print(getLogDateTime());
  Serial.print(" RAW: ");
  mesh::Utils::printHex(Serial, raw, len);
  Serial.println();
#endif
}

void MyMesh::logTxRaw(const uint8_t raw[], int len) {
#ifdef WITH_LORA_FOTA
  fotaLogTxRaw(raw, len);   //en: TX RAW diagnostics + FOTA tag (nrffota/FotaMyMesh.cpp)
#endif
  //en: MESH_PACKET_LOGGING already prints TX in Dispatcher::checkSend() — no dup here.
}

void MyMesh::logRx(mesh::Packet *pkt, int len, float score) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 1) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": RX, len=%d (type=%d, route=%s, payload_len=%d) SNR=%d RSSI=%d score=%d", len,
               pkt->getPayloadType(), pkt->isRouteDirect() ? "D" : "F", pkt->payload_len,
               (int)_radio->getLastSNR(), (int)_radio->getLastRSSI(), (int)(score * 1000));

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTx(mesh::Packet *pkt, int len) {
#ifdef WITH_BRIDGE
  if (_prefs.bridge_pkt_src == 0) {
    bridge.sendPacket(pkt);
  }
#endif

  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX, len=%d (type=%d, route=%s, payload_len=%d)", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);

      if (pkt->getPayloadType() == PAYLOAD_TYPE_PATH || pkt->getPayloadType() == PAYLOAD_TYPE_REQ ||
          pkt->getPayloadType() == PAYLOAD_TYPE_RESPONSE || pkt->getPayloadType() == PAYLOAD_TYPE_TXT_MSG) {
        f.printf(" [%02X -> %02X]\n", (uint32_t)pkt->payload[1], (uint32_t)pkt->payload[0]);
      } else {
        f.printf("\n");
      }
      f.close();
    }
  }
}

void MyMesh::logTxFail(mesh::Packet *pkt, int len) {
  if (_logging) {
    File f = openAppend(PACKET_LOG_FILE);
    if (f) {
      f.print(getLogDateTime());
      f.printf(": TX FAIL!, len=%d (type=%d, route=%s, payload_len=%d)\n", len, pkt->getPayloadType(),
               pkt->isRouteDirect() ? "D" : "F", pkt->payload_len);
      f.close();
    }
  }
}

int MyMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}

uint32_t MyMesh::getRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}
uint32_t MyMesh::getDirectRetransmitDelay(const mesh::Packet *packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.direct_tx_delay_factor);
  return getRNG()->nextInt(0, 5*t + 1);
}

mesh::DispatcherAction MyMesh::onRecvPacket(mesh::Packet* pkt) {
  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) {
      recv_pkt_region = NULL;
    } else {
      recv_pkt_region =  &region_map.getWildcard();
    }
  } else {
    recv_pkt_region = NULL;
  }
  return Mesh::onRecvPacket(pkt);
}

void MyMesh::onAnonDataRecv(mesh::Packet *packet, const uint8_t *secret, const mesh::Identity &sender,
                            uint8_t *data, size_t len) {
  if (packet->getPayloadType() == PAYLOAD_TYPE_ANON_REQ) { // received an initial request by a possible admin
                                                           // client (unknown at this stage)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    data[len] = 0;  // ensure null terminator
    uint8_t reply_len;

    reply_path_len = 0xFF;
    if (data[4] == 0 || data[4] >= ' ') {   // is password, ie. a login request
      reply_len = handleLoginReq(sender, secret, timestamp, &data[4], packet->isRouteFlood());
    } else if (data[4] == ANON_REQ_TYPE_REGIONS && packet->isRouteDirect()) {
      reply_len = handleAnonRegionsReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_OWNER && packet->isRouteDirect()) {
      reply_len = handleAnonOwnerReq(sender, timestamp, &data[5]);
    } else if (data[4] == ANON_REQ_TYPE_BASIC && packet->isRouteDirect()) {
      reply_len = handleAnonClockReq(sender, timestamp, &data[5]);
    } else {
      reply_len = 0;  // unknown/invalid request type
    }

    if (reply_len == 0) return;   // invalid request

    // a DIRECT login can reply via the stored out_path, as onPeerDataRecv() does for REQ
    ClientInfo* client = acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    bool have_out_path = client != NULL && client->out_path_len != OUT_PATH_UNKNOWN;

    auto route = mesh::chooseReplyRoute(packet->isRouteFlood(), reply_path_len != 0xFF, have_out_path);

    if (route == mesh::REPLY_ROUTE_PATH_RETURN) {
      // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
      mesh::Packet* path = createPathReturn(sender, secret, packet->path, packet->path_len,
                                            PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
      if (path) sendFloodReply(path, FK_FLOOD_RESP_DELAY, packet->getPathHashSize());
#ifdef FK_ANON_FLOOD_DIRECT_FALLBACK
      fkAnonFallbackArm(sender, secret, packet, reply_data, reply_len);
#endif
      return;
    }

    mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, secret, reply_data, reply_len);
    if (reply == NULL) return;

    if (route == mesh::REPLY_ROUTE_DIRECT_SUPPLIED) {
      sendDirect(reply, reply_path, reply_path_len, SERVER_RESPONSE_DELAY);
    } else if (route == mesh::REPLY_ROUTE_DIRECT_OUT_PATH) {
      sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
    } else {
      sendFloodReply(reply, FK_FLOOD_RESP_DELAY, packet->getPathHashSize());
    }
  }
}

#ifdef FK_ANON_FLOOD_DIRECT_FALLBACK
//en: ── FK fork: direct fallback of the login-handshake reply ────────────────
//en: The flooded PATH+RESPONSE login reply often dies in busy multi-hop meshes
//en: (uplink floods arrive, the reply flood rarely survives the trip back).
//en: After sending the flood reply we park a pending record; if the client's
//en: reciprocal PATH does not arrive within the window (=> client->out_path in
//en: the ACL is still OUT_PATH_UNKNOWN), the reply is re-sent DIRECT along the
//en: REVERSED inbound request path, and that reversed path is stored as the
//en: provisional out_path. The direct copy gets a fresh random blob
//en: (reply[8..11]) so its packet hash differs and dedup does not drop it.
//en: Worst case equals upstream behaviour (flood only, fallback lost too).
//en: Flag: -D FK_ANON_FLOOD_DIRECT_FALLBACK=<ms window after the flood TX>;
//en: without the flag none of this code is compiled.
//sk: ── FK fork: direct fallback odpovede login handshaku ────────────────────
//sk: Floodovaná PATH+RESPONSE login odpoveď v rušnom viac-hopovom meshi často
//sk: zomrie (uplink floody dolietajú, spiatočný flood zriedka prežije).
//sk: Po odoslaní flood odpovede si odparkujeme pending záznam; ak recipročný
//sk: PATH od klienta nepríde do okna (=> client->out_path v ACL je stále
//sk: OUT_PATH_UNKNOWN), odpoveď sa pošle znova DIRECT po OTOČENEJ ceste
//sk: prichádzajúceho requestu a otočená cesta sa zapíše ako provizórna
//sk: out_path. Direct kópia dostane čerstvý random blob (reply[8..11]), aby
//sk: mala iný packet hash a dedup ju nezahodil. Worst case = upstream
//sk: správanie (len flood, aj fallback stratený).
//sk: Flag: -D FK_ANON_FLOOD_DIRECT_FALLBACK=<ms okno po TX floodu>;
//sk: bez flagu sa tento kód vôbec nekompiluje.
#define FK_ANON_PENDING_SLOTS  2
struct FkAnonPending {
  unsigned long deadline;              //en: 0 = slot free  //sk: 0 = slot voľný
  uint8_t  pub_key[PUB_KEY_SIZE];
  uint8_t  secret[PUB_KEY_SIZE];
  uint8_t  fwd_path[MAX_PATH_SIZE];    //en: as received (client→repeater) — goes into the payload
                                       //sk: ako prišla (klient→repeater) — ide do payloadu
  uint8_t  rev_path[MAX_PATH_SIZE];    //en: reversed (repeater→client) — transport route of the direct copy
                                       //sk: otočená (repeater→klient) — transportná trasa direct kópie
  uint8_t  path_len_enc;               //en: encoded ((hash_size-1)<<6 | count)  //sk: enkódované
  uint8_t  reply[16];                  //en: login reply is 13 B  //sk: login odpoveď má 13 B
  uint8_t  reply_len;
};
static FkAnonPending s_fk_anon[FK_ANON_PENDING_SLOTS];

void MyMesh::fkAnonFallbackArm(const mesh::Identity& sender, const uint8_t* secret,
                               const mesh::Packet* packet, const uint8_t* reply, uint8_t reply_len) {
  uint8_t hs     = packet->getPathHashSize();
  uint8_t cnt    = packet->getPathHashCount();
  uint8_t nbytes = packet->getPathByteLen();
  //en: zero-hop: direct copy would use the same single link as the flood — nothing to gain
  //sk: zero-hop: direct kópia by šla tou istou jedinou linkou ako flood — niet čo získať
  if (cnt == 0 || nbytes > MAX_PATH_SIZE) {
#ifdef FK_DEBUG
    Serial.printf("[FK] LOGIN fallback src=%02X: not armed (hops=%u bytes=%u)\r\n",
                  (unsigned)sender.pub_key[0], (unsigned)cnt, (unsigned)nbytes);
#endif
    return;
  }
  if (reply_len > sizeof(s_fk_anon[0].reply)) {
#ifdef FK_DEBUG
    Serial.printf("[FK] LOGIN fallback src=%02X: reply too long (%u)\r\n",
                  (unsigned)sender.pub_key[0], (unsigned)reply_len);
#endif
    return;
  }

  //en: free slot, else evict the one expiring soonest (oldest handshake)
  //sk: voľný slot, inak vytlač ten s najskorším deadlinom (najstarší handshake)
  FkAnonPending* e = &s_fk_anon[0];
  for (int i = 0; i < FK_ANON_PENDING_SLOTS; i++) {
    if (s_fk_anon[i].deadline == 0) { e = &s_fk_anon[i]; break; }
    if (s_fk_anon[i].deadline < e->deadline) e = &s_fk_anon[i];
  }

  memcpy(e->pub_key, sender.pub_key, PUB_KEY_SIZE);
  memcpy(e->secret, secret, PUB_KEY_SIZE);
  memcpy(e->fwd_path, packet->path, nbytes);
  for (int i = 0; i < cnt; i++) {   //en: reverse hash-sized groups  //sk: otoč po skupinách veľkosti hashu
    memcpy(&e->rev_path[i * hs], &packet->path[(cnt - 1 - i) * hs], hs);
  }
  e->path_len_enc = packet->path_len;
  memcpy(e->reply, reply, reply_len);
  e->reply_len = reply_len;
  //en: window counts from the (possibly FK-delayed) flood TX, not from now
  //sk: okno sa počíta od (prípadne FK-oneskoreného) TX floodu, nie od teraz
  e->deadline = futureMillis(FK_FLOOD_RESP_DELAY + FK_ANON_FLOOD_DIRECT_FALLBACK);
#ifdef FK_DEBUG
  Serial.printf("[FK] LOGIN fallback src=%02X: armed hops=%u fire_in=%d ms\r\n",
                (unsigned)sender.pub_key[0], (unsigned)cnt,
                (int)(FK_FLOOD_RESP_DELAY + FK_ANON_FLOOD_DIRECT_FALLBACK));
#endif
}

void MyMesh::fkAnonFallbackLoop() {
  for (int i = 0; i < FK_ANON_PENDING_SLOTS; i++) {
    FkAnonPending* e = &s_fk_anon[i];
    if (e->deadline == 0 || !millisHasNowPassed(e->deadline)) continue;
    e->deadline = 0;

    ClientInfo* c = acl.getClient(e->pub_key, PUB_KEY_SIZE);
    if (c == NULL) {   //en: evicted meanwhile  //sk: medzitým vytlačený z ACL
#ifdef FK_DEBUG
      Serial.printf("[FK] LOGIN fallback src=%02X: cancelled (ACL entry missing)\r\n",
                    (unsigned)e->pub_key[0]);
#endif
      continue;
    }
    if (c->out_path_len != OUT_PATH_UNKNOWN) {
      //en: reciprocal PATH arrived — the flood reply made it, nothing to do
      //sk: recipročný PATH prišiel — flood odpoveď sa doručila, netreba nič
#ifdef FK_DEBUG
      Serial.printf("[FK] LOGIN fallback src=%02X: cancelled (reciprocal PATH received)\r\n",
                    (unsigned)e->pub_key[0]);
#endif
      continue;
    }

    //en: fresh random blob (reply_data[8..11] of the login reply) → different
    //en: packet hash, otherwise nodes that saw the flood copy would drop this one
    //sk: čerstvý random blob (reply_data[8..11] login odpovede) → iný packet
    //sk: hash, inak by uzly, ktoré videli flood kópiu, túto zahodili
    getRNG()->random(&e->reply[8], 4);
    mesh::Packet* p = createPathReturn(e->pub_key, e->secret, e->fwd_path, e->path_len_enc,
                                       PAYLOAD_TYPE_RESPONSE, e->reply, e->reply_len);
    if (p == NULL) {
#ifdef FK_DEBUG
      Serial.printf("[FK] LOGIN fallback src=%02X: createPathReturn failed\r\n",
                    (unsigned)e->pub_key[0]);
#endif
      continue;
    }

    //en: provisional out_path (repeater→client); confirmed by the client's first direct packet
    //sk: provizórna out_path (repeater→klient); potvrdí ju prvý direct paket od klienta
    uint8_t nbytes = ((e->path_len_enc >> 6) + 1) * (e->path_len_enc & 63);
    memcpy(c->out_path, e->rev_path, nbytes);
    c->out_path_len = e->path_len_enc;

    sendDirect(p, e->rev_path, e->path_len_enc, 0);
#ifdef FK_DEBUG
    Serial.printf("[FK] LOGIN fallback src=%02X: DIRECT resend hops=%u route=",
                  (unsigned)e->pub_key[0], (unsigned)(e->path_len_enc & 63));
    for (uint8_t i = 0; i < nbytes; i++) Serial.printf("%02X", (unsigned)e->rev_path[i]);
    Serial.printf("\r\n");
#endif
  }
}
#endif // FK_ANON_FLOOD_DIRECT_FALLBACK

int MyMesh::searchPeersByHash(const uint8_t *hash) {
  int n = 0;
  for (int i = 0; i < acl.getNumClients(); i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i; // store the INDEXES of matching contacts (for subsequent 'peer' methods)
    }
  }
  return n;
}

void MyMesh::getPeerSharedSecret(uint8_t *dest_secret, int peer_idx) {
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    // lookup pre-calculated shared_secret
    memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
  } else {
    MESH_DEBUG_PRINTLN("getPeerSharedSecret: Invalid peer idx: %d", i);
  }
}

static bool isShare(const mesh::Packet *packet) {
  if (packet->hasTransportCodes()) {
    return packet->transport_codes[0] == 0 && packet->transport_codes[1] == 0;  // codes { 0, 0 } means 'send to nowhere'
  }
  return false;
}

void MyMesh::onAdvertRecv(mesh::Packet *packet, const mesh::Identity &id, uint32_t timestamp,
                          const uint8_t *app_data, size_t app_data_len) {
  mesh::Mesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len); // chain to super impl

  // if this a zero hop advert (and not via 'Share'), add it to neighbours
  if (packet->getPathHashCount() == 0 && !isShare(packet)) {
    AdvertDataParser parser(app_data, app_data_len);
    if (parser.isValid() && parser.getType() == ADV_TYPE_REPEATER) { // just keep neigbouring Repeaters
      putNeighbour(id, timestamp, packet->getSNR());
    }
  }
}

void MyMesh::onPeerDataRecv(mesh::Packet *packet, uint8_t type, int sender_idx, const uint8_t *secret,
                            uint8_t *data, size_t len) {
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("onPeerDataRecv: invalid peer idx: %d", i);
    return;
  }
  ClientInfo* client = acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_REQ) { // request (from a Known admin client!)
    uint32_t timestamp;
    memcpy(&timestamp, data, 4);

    if (timestamp > client->last_timestamp) { // prevent replay attacks
      int reply_len = handleRequest(client, timestamp, &data[4], len - 4);
      if (reply_len == 0) return; // invalid command

      client->last_timestamp = timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      if (packet->isRouteFlood()) {
        // let this sender know path TO here, so they can use sendDirect(), and ALSO encode the response
        mesh::Packet *path = createPathReturn(client->id, secret, packet->path, packet->path_len,
                                              PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
        if (path) sendFloodReply(path, FK_FLOOD_RESP_DELAY, packet->getPathHashSize());
      } else {
        mesh::Packet *reply =
            createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
        if (reply) {
          if (client->out_path_len != OUT_PATH_UNKNOWN) { // we have an out_path, so send DIRECT
            sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
          } else {
            sendFloodReply(reply, FK_FLOOD_RESP_DELAY, packet->getPathHashSize());
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  } else if (type == PAYLOAD_TYPE_TXT_MSG && len > 5 && client->isAdmin()) { // a CLI command
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4); // timestamp (by sender's RTC clock - which could be wrong)
    uint8_t flags = (data[4] >> 2);        // message attempt number, and other flags

    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: unsupported text type received: flags=%02x", (uint32_t)flags);
    } else if (sender_timestamp >= client->last_timestamp) { // prevent replay attacks
      bool is_retry = (sender_timestamp == client->last_timestamp);
      client->last_timestamp = sender_timestamp;
      client->last_activity = getRTCClock()->getCurrentTime();

      // len can be > original length, but 'text' will be padded with zeroes
      data[len] = 0; // need to make a C string again, with null terminator

      if (flags == TXT_TYPE_PLAIN) { // for legacy CLI, send Acks
        uint32_t ack_hash; // calc truncated hash of the message timestamp + text + sender pub_key, to prove
                           // to sender that we got it
        mesh::Utils::sha256((uint8_t *)&ack_hash, 4, data, 5 + strlen((char *)&data[5]), client->id.pub_key,
                            PUB_KEY_SIZE);

        mesh::Packet *ack = createAck(ack_hash);
        if (ack) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(ack, TXT_ACK_DELAY, packet->getPathHashSize());
          } else {
            sendDirect(ack, client->out_path, client->out_path_len, TXT_ACK_DELAY);
          }
        }
      }

      uint8_t temp[166];
      char *command = (char *)&data[5];
      char *reply = (char *)&temp[5];
      if (is_retry) {
        *reply = 0;
      }
#ifdef WITH_LORA_FOTA
      else if (fotaHandleLoRaCli(client, secret, command, reply,
                                 packet->getPathHashSize(), sender_timestamp)) {
        //en: FOTA CLI from LoRa — deferred to loop(); reply filled by
        //en: fotaHandleLoRaCli (nrffota/FotaMyMesh.cpp).
      }
#endif
      else {
        handleCommand(sender_timestamp, command, reply);
      }
      int text_len = strlen(reply);
      if (text_len > 0) {
        uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
        if (timestamp == sender_timestamp) {
          // WORKAROUND: the two timestamps need to be different, in the CLI view
          timestamp++;
        }
        memcpy(temp, &timestamp, 4);        // mostly an extra blob to help make packet_hash unique
        temp[4] = (TXT_TYPE_CLI_DATA << 2); // NOTE: legacy was: TXT_TYPE_PLAIN

        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + text_len);
        if (reply) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(reply, CLI_REPLY_DELAY_MILLIS, packet->getPathHashSize());
          } else {
            sendDirect(reply, client->out_path, client->out_path_len, CLI_REPLY_DELAY_MILLIS);
          }
        }
      }
    } else {
      MESH_DEBUG_PRINTLN("onPeerDataRecv: possible replay attack detected");
    }
  }
}

bool MyMesh::onPeerPathRecv(mesh::Packet *packet, int sender_idx, const uint8_t *secret, uint8_t *path,
                            uint8_t path_len, uint8_t extra_type, uint8_t *extra, uint8_t extra_len) {
  // TODO: prevent replay attacks
  int i = matching_peer_indexes[sender_idx];

  if (i >= 0 && i < acl.getNumClients()) { // get from our known_clients table (sender SHOULD already be known in this context)
    MESH_DEBUG_PRINTLN("PATH to client, path_len=%d", (uint32_t)path_len);
    auto client = acl.getClientByIdx(i);

    // store a copy of path, for sendDirect()
    client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len);
    client->last_activity = getRTCClock()->getCurrentTime();
  } else {
    MESH_DEBUG_PRINTLN("onPeerPathRecv: invalid peer idx: %d", i);
  }

  // NOTE: no reciprocal path send!!
  return false;
}

#define CTL_TYPE_NODE_DISCOVER_REQ   0x80
#define CTL_TYPE_NODE_DISCOVER_RESP  0x90

void MyMesh::onControlDataRecv(mesh::Packet* packet) {
  uint8_t type = packet->payload[0] & 0xF0;    // just test upper 4 bits
  if (type == CTL_TYPE_NODE_DISCOVER_REQ && packet->payload_len >= 6
      && !_prefs.disable_fwd && discover_limiter.allow(rtc_clock.getCurrentTime())
  ) {
    int i = 1;
    uint8_t  filter = packet->payload[i++];
    uint32_t tag;
    memcpy(&tag, &packet->payload[i], 4); i += 4;
    uint32_t since;
    if (packet->payload_len >= i+4) {   // optional since field
      memcpy(&since, &packet->payload[i], 4); i += 4;
    } else {
      since = 0;
    }

    if ((filter & (1 << ADV_TYPE_REPEATER)) != 0 && _prefs.discovery_mod_timestamp >= since) {
      bool prefix_only = packet->payload[0] & 1;
      uint8_t data[6 + PUB_KEY_SIZE];
      data[0] = CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_REPEATER;   // low 4-bits for node type
      data[1] = packet->_snr;   // let sender know the inbound SNR ( x 4)
      memcpy(&data[2], &tag, 4);     // include tag from request, for client to match to
      memcpy(&data[6], self_id.pub_key, PUB_KEY_SIZE);
      auto resp = createControlData(data, prefix_only ? 6 + 8 : 6 + PUB_KEY_SIZE);
      if (resp) {
        sendZeroHop(resp, getRetransmitDelay(resp)*4);  // apply random delay (widened x4), as multiple nodes can respond to this
      }
    }
  } else if (type == CTL_TYPE_NODE_DISCOVER_RESP && packet->payload_len >= 6) {
    uint8_t node_type = packet->payload[0] & 0x0F;
    if (node_type != ADV_TYPE_REPEATER) {
      return;
    }
    if (packet->payload_len < 6 + PUB_KEY_SIZE) {
      MESH_DEBUG_PRINTLN("onControlDataRecv: DISCOVER_RESP pubkey too short: %d", (uint32_t)packet->payload_len);
      return;
    }

    if (pending_discover_tag == 0 || millisHasNowPassed(pending_discover_until)) {
      pending_discover_tag = 0;
      return;
    }
    uint32_t tag;
    memcpy(&tag, &packet->payload[2], 4);
    if (tag != pending_discover_tag) {
      return;
    }

    mesh::Identity id(&packet->payload[6]);
    if (id.matches(self_id)) {
      return;
    }
    putNeighbour(id, rtc_clock.getCurrentTime(), packet->getSNR());
  }
}

//en: (FOTA: searchChannelsByHash / onGroupDataRecv overrides live in nrffota/FotaMyMesh.cpp)

void MyMesh::sendNodeDiscoverReq() {
  uint8_t data[10];
  data[0] = CTL_TYPE_NODE_DISCOVER_REQ; // prefix_only=0
  data[1] = (1 << ADV_TYPE_REPEATER);
  getRNG()->random(&data[2], 4); // tag
  memcpy(&pending_discover_tag, &data[2], 4);
  pending_discover_until = futureMillis(60000);
  uint32_t since = 0;
  memcpy(&data[6], &since, 4);

  auto pkt = createControlData(data, sizeof(data));
  if (pkt) {
    sendZeroHop(pkt);
  }
}

MyMesh::MyMesh(mesh::MainBoard &board, mesh::Radio &radio, mesh::MillisecondClock &ms, mesh::RNG &rng,
               mesh::RTCClock &rtc, mesh::MeshTables &tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      region_map(key_store), temp_map(key_store),
      _cli(board, rtc, sensors, region_map, acl, &_prefs, this),
      telemetry(MAX_PACKET_PAYLOAD - 4),
      discover_limiter(4, 120),  // max 4 every 2 minutes
      anon_limiter(4, 180)   // max 4 every 3 minutes
#if defined(WITH_RS232_BRIDGE)
      , bridge(&_prefs, WITH_RS232_BRIDGE, _mgr, &rtc)
#endif
#if defined(WITH_ESPNOW_BRIDGE)
      , bridge(&_prefs, _mgr, &rtc)
#endif
{
  last_millis = 0;
  uptime_millis = 0;
  next_local_advert = next_flood_advert = 0;
  dirty_contacts_expiry = 0;
  set_radio_at = revert_radio_at = 0;
  _logging = false;
  region_load_active = false;
  recv_pkt_region = NULL;

#if MAX_NEIGHBOURS
  memset(neighbours, 0, sizeof(neighbours));
#endif

  // defaults
  _prefs.airtime_factor = 1.0;
  _prefs.rx_delay_base = 0.0f;   // turn off by default, was 10.0;
  _prefs.tx_delay_factor = 0.5f; // was 0.25f
  _prefs.direct_tx_delay_factor = 0.3f; // was 0.2
  StrHelper::strncpy(_prefs.node_name, ADVERT_NAME, sizeof(_prefs.node_name));
  _prefs.node_lat = ADVERT_LAT;
  _prefs.node_lon = ADVERT_LON;
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.advert_interval = 1;        // default to 2 minutes for NEW installs
  _prefs.flood_advert_interval = 47; // 47 hours
  _prefs.flood_max = 64;
  _prefs.flood_max_unscoped = 64;
  _prefs.flood_max_advert = 8;
  _prefs.interference_threshold = 0; // disabled
  _prefs.cad_enabled = 0;            // hardware CAD before TX (off by default; 'set cad on')

  // bridge defaults
  _prefs.bridge_enabled = 1;    // enabled
  _prefs.bridge_delay   = 500;  // milliseconds
  _prefs.bridge_pkt_src = 0;    // logTx
  _prefs.bridge_baud = 115200;  // baud rate
  _prefs.bridge_channel = 1;    // channel 1

  StrHelper::strncpy(_prefs.bridge_secret, "LVSITANOS", sizeof(_prefs.bridge_secret));

  // GPS defaults
  _prefs.gps_enabled = 0;
  _prefs.gps_interval = 0;
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS;

  _prefs.adc_multiplier = 0.0f; // 0.0f means use default board multiplier

#if defined(USE_SX1262) || defined(USE_SX1268)
#ifdef SX126X_RX_BOOSTED_GAIN
  _prefs.rx_boosted_gain = SX126X_RX_BOOSTED_GAIN;
#else
  _prefs.rx_boosted_gain = 1; // enabled by default;
#endif
#endif
  _prefs.radio_fem_rxgain = 1;
  _prefs.radio_fem_txgain = 0;

  pending_discover_tag = 0;
  pending_discover_until = 0;

  memset(default_scope.key, 0, sizeof(default_scope.key));
}

void MyMesh::begin(FILESYSTEM *fs) {
#ifdef WITH_LORA_FOTA
  fotaEarlyInit();   //en: read flasher debug marker ASAP after boot + reset FOTA state
#endif
  mesh::Mesh::begin();
  _fs = fs;
  // load persisted prefs
  _cli.loadPrefs(_fs);
  acl.load(_fs, self_id);
  // TODO: key_store.begin();
  region_map.load(_fs);

  // establish default-scope
  {
    RegionEntry* r = region_map.getDefaultRegion();
    if (r) {
      region_map.getTransportKeysFor(*r, &default_scope, 1);
    } else {
#ifdef DEFAULT_FLOOD_SCOPE_NAME
      r = region_map.findByName(DEFAULT_FLOOD_SCOPE_NAME);
      if (r == NULL) {
        r = region_map.putRegion(DEFAULT_FLOOD_SCOPE_NAME, 0);  // auto-create the default scope region
        if (r) { r->flags = 0; }   // Allow-flood
      }
      if (r) {
        region_map.setDefaultRegion(r);
        region_map.getTransportKeysFor(*r, &default_scope, 1);
      }
#endif
    }
  }

#if defined(WITH_BRIDGE)
  if (_prefs.bridge_enabled) {
    bridge.begin();
  }
#endif

  radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_driver.setTxPower(_prefs.tx_power_dbm);

  radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);
  MESH_DEBUG_PRINTLN("RX Boosted Gain Mode: %s",
                     radio_driver.getRxBoostedGainMode() ? "Enabled" : "Disabled");
  board.setLoRaFemLnaEnabled(_prefs.radio_fem_rxgain);
  board.setLoRaFemPaGainEnabled(_prefs.radio_fem_txgain);

  updateAdvertTimer();
  updateFloodAdvertTimer();

  board.setAdcMultiplier(_prefs.adc_multiplier);

#if ENV_INCLUDE_GPS == 1
  applyGpsPrefs();
#endif

#ifdef WITH_LORA_FOTA
  fotaBegin();       //en: mount FOTA FS + channel + boot banner (nrffota/FotaMyMesh.cpp)
#endif
}

void MyMesh::sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size) {
  if (scope.isNull()) {
    sendFlood(pkt, delay_millis, path_hash_size);
  } else {
    uint16_t codes[2];
    codes[0] = scope.calcTransportCode(pkt);
    codes[1] = 0;  // REVISIT: set to 'home' Region, for sender/return region?
    sendFlood(pkt, codes, delay_millis, path_hash_size);
  }
}

void MyMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) {
  set_radio_at = futureMillis(2000); // give CLI reply some time to be sent back, before applying temp radio params
  pending_freq = freq;
  pending_bw = bw;
  pending_sf = sf;
  pending_cr = cr;

  revert_radio_at = futureMillis(2000 + timeout_mins * 60 * 1000); // schedule when to revert radio params
}

bool MyMesh::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return SPIFFS.format();
#else
#error "need to implement file system erase"
  return false;
#endif
}

void MyMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  mesh::Packet *pkt = createSelfAdvert();
  if (pkt) {
    if (flood) {
      sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);
    } else {
      sendZeroHop(pkt, delay_millis);
    }
  } else {
    MESH_DEBUG_PRINTLN("ERROR: unable to create advertisement packet!");
  }
}

void MyMesh::updateAdvertTimer() {
  if (_prefs.advert_interval > 0) { // schedule local advert timer
    next_local_advert = futureMillis(((uint32_t)_prefs.advert_interval) * 2 * 60 * 1000);
  } else {
    next_local_advert = 0; // stop the timer
  }
}

void MyMesh::updateFloodAdvertTimer() {
  if (_prefs.flood_advert_interval > 0) { // schedule flood advert timer
    next_flood_advert = futureMillis(((uint32_t)_prefs.flood_advert_interval) * 60 * 60 * 1000);
  } else {
    next_flood_advert = 0; // stop the timer
  }
}

void MyMesh::dumpLogFile() {
#if defined(RP2040_PLATFORM)
  File f = _fs->open(PACKET_LOG_FILE, "r");
#else
  File f = _fs->open(PACKET_LOG_FILE);
#endif
  if (f) {
    while (f.available()) {
      int c = f.read();
      if (c < 0) break;
      Serial.print((char)c);
    }
    f.close();
  }
}

void MyMesh::setTxPower(int8_t power_dbm) {
  radio_driver.setTxPower(power_dbm);
}

bool MyMesh::setRxBoostedGain(bool enable) {
  return radio_driver.setRxBoostedGainMode(enable);
}

#if defined(USE_LR2021)
bool MyMesh::configSideDetectors(const uint8_t sideDetSFs[], uint8_t num, float bw) {
  return radio_driver.configSideDetectors(sideDetSFs, num, bw);
}
#endif

void MyMesh::formatNeighborsReply(char *reply) {
  char *dp = reply;

#if MAX_NEIGHBOURS
  // create copy of neighbours list, skipping empty entries so we can sort it separately from main list
  int16_t neighbours_count = 0;
  NeighbourInfo* sorted_neighbours[MAX_NEIGHBOURS];
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    auto neighbour = &neighbours[i];
    if (neighbour->heard_timestamp > 0) {
      sorted_neighbours[neighbours_count] = neighbour;
      neighbours_count++;
    }
  }

  // sort neighbours newest to oldest
  std::sort(sorted_neighbours, sorted_neighbours + neighbours_count, [](const NeighbourInfo* a, const NeighbourInfo* b) {
    return a->heard_timestamp > b->heard_timestamp; // desc
  });

  for (int i = 0; i < neighbours_count && dp - reply < 134; i++) {
    NeighbourInfo *neighbour = sorted_neighbours[i];

    // add new line if not first item
    if (i > 0) *dp++ = '\n';

    char hex[10];
    // get 4 bytes of neighbour id as hex
    mesh::Utils::toHex(hex, neighbour->id.pub_key, 4);

    // add next neighbour
    uint32_t secs_ago = getRTCClock()->getCurrentTime() - neighbour->heard_timestamp;
    sprintf(dp, "%s:%d:%d", hex, secs_ago, neighbour->snr);
    while (*dp)
      dp++; // find end of string
  }
#endif
  if (dp == reply) { // no neighbours, need empty response
    strcpy(dp, "-none-");
    dp += 6;
  }
  *dp = 0; // null terminator
}

void MyMesh::removeNeighbor(const uint8_t *pubkey, int key_len) {
#if MAX_NEIGHBOURS
  for (int i = 0; i < MAX_NEIGHBOURS; i++) {
    NeighbourInfo *neighbour = &neighbours[i];
    if (memcmp(neighbour->id.pub_key, pubkey, key_len) == 0) {
      neighbours[i] = NeighbourInfo(); // clear neighbour entry
    }
  }
#endif
}

void MyMesh::startRegionsLoad() {
  temp_map.resetFrom(region_map);   // rebuild regions in a temp instance
  memset(load_stack, 0, sizeof(load_stack));
  load_stack[0] = &temp_map.getWildcard();
  region_load_active = true;
}

bool MyMesh::saveRegions() {
  return region_map.save(_fs);
}

void MyMesh::onDefaultRegionChanged(const RegionEntry* r) {
  if (r) {
    region_map.getTransportKeysFor(*r, &default_scope, 1);
  } else {
    memset(default_scope.key, 0, sizeof(default_scope.key));
  }
}

void MyMesh::formatStatsReply(char *reply) {
  StatsFormatHelper::formatCoreStats(reply, board, *_ms, _err_flags, _mgr);
}

void MyMesh::formatRadioStatsReply(char *reply) {
  StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver, getTotalAirTime(), getReceiveAirTime());
}

void MyMesh::formatPacketStatsReply(char *reply) {
  StatsFormatHelper::formatPacketStats(reply, radio_driver, getNumSentFlood(), getNumSentDirect(), 
                                       getNumRecvFlood(), getNumRecvDirect());
}

void MyMesh::saveIdentity(const mesh::LocalIdentity &new_id) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif
  store.save("_main", new_id);
}

void MyMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables *)getTables())->resetStats();
}

void MyMesh::handleCommand(uint32_t sender_timestamp, char *command, char *reply) {
  if (region_load_active) {
    if (StrHelper::isBlank(command)) {  // empty/blank line, signal to terminate 'load' operation
      region_map = temp_map;  // copy over the temp instance as new current map
      region_load_active = false;

      sprintf(reply, "OK - loaded %d regions", region_map.getCount());
    } else {
      char *np = command;
      while (*np == ' ') np++;   // skip indent
      int indent = np - command;

      char *ep = np;
      while (RegionMap::is_name_char(*ep)) ep++;
      if (*ep) { *ep++ = 0; }  // set null terminator for end of name

      while (*ep && *ep != 'F') ep++;  // look for (optional) flags

      if (indent > 0 && indent < 8 && strlen(np) > 0) {
        auto parent = load_stack[indent - 1];
        if (parent) {
          auto old = region_map.findByName(np);
          auto nw = temp_map.putRegion(np, parent->id, old ? old->id : 0);  // carry-over the current ID (if name already exists)
          if (nw) {
            nw->flags = old ? old->flags : (*ep == 'F' ? 0 : REGION_DENY_FLOOD);   // carry-over flags from curr

            load_stack[indent] = nw;  // keep pointers to parent regions, to resolve parent_id's
          }
        }
      }
      reply[0] = 0;
    }
    return;
  }

  while (*command == ' ') command++; // skip leading spaces

  if (strlen(command) > 4 && command[2] == '|') { // optional prefix (for companion radio CLI)
    memcpy(reply, command, 3);                    // reflect the prefix back
    reply += 3;
    command += 3;
  }

  // handle ACL related commands
  if (memcmp(command, "setperm ", 8) == 0) {   // format:  setperm {pubkey-hex} {permissions-int8}
    char* hex = &command[8];
    char* sp = strchr(hex, ' ');   // look for separator char
    if (sp == NULL) {
      strcpy(reply, "Err - bad params");
    } else {
      *sp++ = 0;   // replace space with null terminator

      uint8_t pubkey[PUB_KEY_SIZE];
      int hex_len = min(sp - hex, PUB_KEY_SIZE*2);
      if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
        uint8_t perms = atoi(sp);
        if (acl.applyPermissions(self_id, pubkey, hex_len / 2, perms)) {
          dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);   // trigger acl.save()
          strcpy(reply, "OK");
        } else {
          strcpy(reply, "Err - invalid params");
        }
      } else {
        strcpy(reply, "Err - bad pubkey");
      }
    }
  } else if (sender_timestamp == 0 && strcmp(command, "get acl") == 0) {
    Serial.println("ACL:");
    for (int i = 0; i < acl.getNumClients(); i++) {
      auto c = acl.getClientByIdx(i);
      if (c->permissions == 0) continue;  // skip deleted (or guest) entries

      Serial.printf("%02X ", c->permissions);
      mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
      Serial.printf("\n");
    }
    reply[0] = 0;
  } else if (memcmp(command, "discover.neighbors", 18) == 0) {
    const char* sub = command + 18;
    while (*sub == ' ') sub++;
    if (*sub != 0) {
      strcpy(reply, "Err - discover.neighbors has no options");
    } else {
      sendNodeDiscoverReq();
      strcpy(reply, "OK - Discover sent");
    }
#ifdef WITH_LORA_FOTA
  } else if (fotaHandleCliCommand(command, reply)) {
    //en: FOTA CLI ('fota …' / legacy 'ota …') — nrffota/FotaMyMesh.cpp
#endif
#ifdef FK_RADIO_DIAG_CLI
  } else if (radioDiagCliCommand(command, reply)) {
    //en: radio-agnostic bench diagnostics ('fk rssi|hammer|reinit')
#endif
#ifdef FK_NICERF_LORA2021F33_TEST
  } else if (nicerfTestCliCommand(command, reply)) {
    //en: bench test CLI ('fk …') for the NiceRF LR2021 module — variant target.cpp
#endif
  } else{
    _cli.handleCommand(sender_timestamp, command, reply);  // common CLI commands
  }
}

#ifdef FKPR_RADIO_WATCHDOG
/*
  Detect a radio that has stopped answering, and re-initialise it.

  MeshCore has no such check today. Dispatcher::loop() watches for a radio 'stuck'
  outside Rx, but it asks isInRecvMode(), which reports the wrapper's own `state`
  flag rather than the chip - and on LR2021 that flag is pinned to STATE_RX, so
  the test can never fire. Even when it does, it only raises
  ERR_EVENT_STARTRX_TIMEOUT and never attempts recovery. A radio that dies
  quietly therefore leaves the node running, printing heartbeats and forwarding
  nothing, until someone power-cycles it.

  The liveness test is RadioLibWrapper::isChipResponding(), so each radio can
  answer in the way that actually works for it:
    - LR2021 (default test): a lost supply makes getCurrentRSSI() return
      -255 dBm; real thermal noise never approaches -150.
    - SX126x: that test would be useless, because its instantaneous RSSI is a
      single byte and can only express 0 .. -127.5 dBm - a dead SPI bus reads as
      0 dBm or -127.5 dBm, both of which look legal. It checks the status byte
      instead (0x00 / 0xFF are not valid chipMode/cmdStatus combinations).

  Three consecutive bad reads (i.e. ~90 s) are required before acting, so a
  single glitch cannot trigger a re-init.
*/
/*
  ACTIVE probe - on-demand only, used by 'fk rssi'/'fk hammer'/'fk reinit'.

  The watchdog does NOT use this: RadioLibWrapper::loop() already reads
  getCurrentRSSI() about 32 times a second for its noise floor (Dispatcher
  re-arms the sampler every NOISE_FLOOR_CALIB_INTERVAL = 2 s, 64 samples a
  round), so min/max is collected there for free - see takeRssiWindow(). A
  blocking burst would add nothing but ~n ms of stalled loop.

  It stays for the bench, where it is genuinely useful: it answers immediately
  instead of after a 30 s window, and it reads the chip directly, so it still
  reports something when the receiver is NOT armed in Rx and the passive
  sampler is therefore collecting nothing at all.
*/
void MyMesh::radioSampleRssi(int n, int* out_min, int* out_max) {
  int lo = 9999, hi = -9999;
  for (int i = 0; i < n; i++) {
    int r = (int) radio_driver.getCurrentRSSI();
    if (r < lo) lo = r;
    if (r > hi) hi = r;
    delay(1);
  }
  *out_min = lo; *out_max = hi;
}

#ifdef FK_RADIO_DIAG_CLI
/*
  Bench diagnostics, radio-agnostic (works on SX126x and LR2021 alike):
    fk rssi [n]    - ACTIVE burst of n RSSI samples (default 32), min/max/spread
    fk win         - read the PASSIVE window the watchdog uses (and reset it).
                     n=0 means the receiver was never armed in Rx since the last
                     read - which is what the watchdog treats as dead.
    fk hammer [n]  - n rapid standby+startReceive cycles. This is what wedged an
                     SX1262 hard enough that RadioLib's 1 ms reset pulse could not
                     recover it - only sitting in the bootloader did. Use it to
                     reproduce that state deliberately.
    fk reinit      - run the full recovery by hand
  Module-specific commands (simo/ce/pram) are left to the variant handler.
*/
//en: the raw RadioLib object - the SPI diagnostics below talk to the chip directly
//sk: surovy RadioLib objekt - SPI diagnostika nizsie hovori priamo s cipom
extern RADIO_CLASS radio;

bool MyMesh::radioDiagCliCommand(char* command, char* reply) {
  if (memcmp(command, "fk ", 3) != 0) return false;
  const char* arg = command + 3;

#ifdef FK_RADIO_SPI_DIAG
  //en: 'fk zero <n>' - make every n-th length read return 0 (0 = off), which sends
  //en: recvRaw() down its bail-out path: no readData(), so nothing clears the IRQ flags.
  //en: 'fk irq' - current IRQ word, the level of the IRQ line and the wrapper state, to
  //en: see what that bail-out left behind.
  //sk: 'fk zero <n>' - kazde n-te citanie dlzky vrati 0 (0 = vyp), cim posle recvRaw()
  //sk: do vetvy bez readData(), takze IRQ priznaky nikto nevycisti.
  //sk: 'fk irq' - aktualne IRQ slovo, uroven IRQ linky a stav wrappera, aby bolo vidiet,
  //sk: co po tom bail-oute zostalo.
  if (memcmp(arg, "zero", 4) == 0) {
    const char* n = arg + 4;
    while (*n == ' ') n++;
    if (*n) radio._fk_zero = (uint16_t)atoi(n);
    radio._fk_zero_cnt = 0;
    sprintf(reply, "zero=%u (0 = vyp) - kazde n-te citanie dlzky vrati 0",
            (unsigned)radio._fk_zero);
    return true;
  }
  if (memcmp(arg, "irq", 3) == 0) {
    uint32_t irq = 0;
    uint8_t dio = 2;
    radio.fkIrqState(&irq, &dio);
    sprintf(reply, "irq=0x%lX dio=%s rawrx=%lu rxerr=%lu",
            (unsigned long)irq,
            dio == 2 ? "n/a" : (dio ? "HIGH" : "low"),
            (unsigned long)radio_driver.getPacketsRecv(),
            (unsigned long)radio_driver.getPacketsRecvErrors());
    return true;
  }
#endif

#ifdef FK_RADIO_SPI_DIAG
  //en: 'fk busy [n]' - can the MCU see BUSY high after an opcode? n runs, default 8.
  //sk: 'fk busy [n]' - vidi MCU BUSY vysoko po opcode? n behov, default 8.
  if (memcmp(arg, "busy", 4) == 0) {
    int runs = (arg[4] == ' ') ? atoi(arg + 5) : 8;
    if (runs < 1 || runs > 64) runs = 8;
    int seen = 0, sum_hi = 0, sum_fall = 0;
    for (int k = 0; k < runs; k++) {
      uint16_t hi = 0, fall = 0;
      radio.fkBusyProbe(&hi, &fall);
      if (hi) { seen++; sum_hi += hi; sum_fall += fall; }
      Serial.print("[FK]   busy run "); Serial.print(k);
      Serial.print(" hi="); Serial.print(hi);
      Serial.print(" fall="); Serial.println(fall);
    }
    sprintf(reply, "busy videny vysoko %d/%d, priemer hi=%d fall=%d",
            seen, runs, seen ? sum_hi / seen : 0, seen ? sum_fall / seen : 0);
    return true;
  }
#endif

#ifdef FK_RADIO_SPI_DIAG
  //en: 'fk inject <n>' - skip the BUSY wait on every n-th length read (0 = off), and
  //en: 'fk pretype on|off' - call getPacketType() before the length read, which is what
  //en: the library path does. Both are experiment switches, see CustomLR2021.
  //sk: 'fk inject <n>' - preskoc cakanie na BUSY pri kazdom n-tom citani dlzky (0 = vyp),
  //sk: a 'fk pretype on|off' - zavolaj pred citanim dlzky getPacketType(), tak ako to
  //sk: robi kniznicna cesta. Oboje su prepinace pokusu, vid CustomLR2021.
  if (memcmp(arg, "inject", 6) == 0) {
    const char* n = arg + 6;
    while (*n == ' ') n++;
    if (*n) radio._fk_inject = (uint16_t)atoi(n);
    radio._fk_inject_cnt = 0;
    sprintf(reply, "inject=%u (0 = vyp), pretype=%s",
            (unsigned)radio._fk_inject, radio._fk_pretype ? "on" : "off");
    return true;
  }
  //en: 'fk guard on|off' - master switch for the stale-reply guard. OFF leaves the
  //en: read path untouched (one bool test, then the library read), so the measured
  //en: configuration is not disturbed; ON runs the guard and records every recovery,
  //en: dumpable with 'fk spifix'.
  //sk: 'fk guard on|off' - hlavny prepinac guardu pretecenej odpovede. OFF nechava
  //sk: citaciu cestu nedotknutu (jeden test bool a kniznicne citanie), takze merana
  //sk: konfiguracia zostava cista; ON pusti guard a zaznamena kazdu zachranu, ktoru
  //sk: potom vypise 'fk spifix'.
  //en: 'fk guard a|b|c' also picks what separates two attempts: a = back to back,
  //en: b = idle gap only, c = one getPacketType() in between. 'on' keeps the mode.
  //sk: 'fk guard a|b|c' zaroven vyberie, co oddeluje dva pokusy: a = hned za sebou,
  //sk: b = len necinna pauza, c = jeden getPacketType() medzi nimi. 'on' mod nemeni.
  if (memcmp(arg, "guard", 5) == 0) {
    const char* n = arg + 5;
    while (*n == ' ') n++;
    if (*n == 'a' || *n == 'b' || *n == 'c') {
      radio._fk_guard_mode = (uint8_t)(*n - 'a' + 1);
      radio._fk_guard = true;
    } else if (*n) {
      radio._fk_guard = (memcmp(n, "on", 2) == 0);
    }
    sprintf(reply, "guard=%s mode=%c tries=%u gap=%uus, spifix=%lu, pretype=%s, inject=%u",
            radio._fk_guard ? "ON" : "off",
            (char)('a' + radio._fk_guard_mode - 1),
            (unsigned)radio._fk_max_tries,
            (unsigned)radio._fk_gap_us,
            (unsigned long)radio.getStalePktLenReads(),
            radio._fk_pretype ? "on" : "off",
            (unsigned)radio._fk_inject);
    return true;
  }
  //en: 'fk gap <us>' - the idle delay mode B inserts between attempts. Set it to what
  //en: mode C's getPacketType() actually costs (read it off the us= field) so the two
  //en: modes differ only in whether a command was issued.
  //sk: 'fk gap <us>' - necinna pauza, ktoru vklada mod B medzi pokusy. Nastav ju na to,
  //sk: co realne stoji getPacketType() v mode C (odcitaj z polozky us=), aby sa oba mody
  //sk: lisili len tym, ci sa vydal prikaz.
  //en: 'fk tries <n>' - how many reads the guard may take before falling back (1..8).
  //en: tries=255 in the dump means the cap was not enough and the library read rescued it.
  //sk: 'fk tries <n>' - kolko citani smie guard spravit, nez spadne na fallback (1..8).
  //sk: tries=255 vo vypise znamena, ze strop nestacil a zachranilo to kniznicne citanie.
  if (memcmp(arg, "tries", 5) == 0) {
    const char* n = arg + 5;
    while (*n == ' ') n++;
    if (*n) {
      int v = atoi(n);
      if (v < 1) v = 1;
      if (v > 8) v = 8;
      radio._fk_max_tries = (uint8_t)v;
    }
    sprintf(reply, "tries=%u (max citani nez fallback)", (unsigned)radio._fk_max_tries);
    return true;
  }
  if (memcmp(arg, "gap", 3) == 0) {
    const char* n = arg + 3;
    while (*n == ' ') n++;
    if (*n) radio._fk_gap_us = (uint16_t)atoi(n);
    sprintf(reply, "gap=%uus (mode b)", (unsigned)radio._fk_gap_us);
    return true;
  }
  if (memcmp(arg, "pretype", 7) == 0) {
    const char* n = arg + 7;
    while (*n == ' ') n++;
    if (*n) radio._fk_pretype = (memcmp(n, "on", 2) == 0);
    sprintf(reply, "pretype=%s, inject=%u",
            radio._fk_pretype ? "on" : "off", (unsigned)radio._fk_inject);
    return true;
  }
#endif

#ifdef FK_RADIO_SPI_DIAG
  //en: 'fk spifix' - dump the guard's ring buffer (see the radio class).
  //en: rule tells which rule flagged the read: CMD = the status said the reply was not
  //en: ours (the fix proposed upstream), FP = the value equalled irq[31:16] (the
  //en: heuristic we ship). FP alone on a frame whose final length equals first is a
  //en: false positive - the length was right all along.
  //sk: 'fk spifix' - vypis kruhoveho buffra guardu (vid the radio class).
  //sk: rule hovori, ktore pravidlo citanie oznacilo: CMD = status hlasil, ze odpoved
  //sk: nie je nasa (oprava navrhnuta upstreamu), FP = hodnota sa rovnala irq[31:16]
  //sk: (heuristika, ktoru posielame). Samotne FP na ramci, kde final == first, je
  //sk: falosny poplach - dlzka bola spravna od zaciatku.
  if (memcmp(arg, "spifix", 6) == 0) {
    Serial.print("[FK] spifix total="); Serial.print(radio._ev_total);
    Serial.print(" v buffri="); Serial.println(radio._ev_count);
    for (uint8_t k = 0; k < radio._ev_count; k++) {
      //en: oldest first
      uint8_t i = (uint8_t)((radio._ev_write + RADIO_CLASS::FK_SPI_EVENTS - radio._ev_count + k)
                            % RADIO_CLASS::FK_SPI_EVENTS);
      const RADIO_CLASS::FkSpiEvent& e = radio._ev[i];
      Serial.print(e.inj ? "[FK]  *fp=" : "[FK]   fp=");
      Serial.print(e.fp);
      Serial.print(" first=");        Serial.print(e.first);
      Serial.print(" final=");        Serial.print(e.final);
      Serial.print(" irq=0x");        Serial.print(e.irq, HEX);
      Serial.print(" stat=0x");       Serial.print(e.stat, HEX);
      Serial.print(" cmd=");          Serial.print((e.stat >> 1) & 0x03);
      Serial.print(" tries=");        Serial.print(e.tries);
      if (e.mode) {
        Serial.print(" mode=");       Serial.print((char)('a' + e.mode - 1));
        Serial.print(" us=");         Serial.print(e.us);
      }
      Serial.print(" rule=");
      if (e.rule & 1) Serial.print("CMD");
      if (e.rule == 3) Serial.print("+");
      if (e.rule & 2) Serial.print("FP");
      if (!(e.rule & 1) && (e.rule & 2) && e.first == e.final) Serial.print(" (falosny)");
      Serial.println("");
    }
    sprintf(reply, "spifix total=%lu, v buffri %u udalosti (detail na Serial)",
            (unsigned long)radio._ev_total, (unsigned)radio._ev_count);
    return true;
  }
#endif

#ifdef FK_RADIO_SPI_DIAG
  //en: 'fk stale' - A/B test of the two-transaction read behind every "get" command.
  //en: Eight reads with the BUSY wait deliberately skipped, then one proper read as a
  //en: reference. fp = top half of the IRQ word, which is what a stale reply returns
  //en: instead of the length. Prints per-read detail over Serial.
  //sk: 'fk stale' - A/B test dvojtransakcneho citania, ktore stoji za kazdym "get"
  //sk: prikazom. Osem citani s umyselne preskocenym cakanim na BUSY, potom jedno
  //sk: poriadne ako referencia. fp = horna polovica IRQ slova, teda to, co zastarala
  //sk: odpoved vrati namiesto dlzky. Detail kazdeho citania ide na Serial.
  if (memcmp(arg, "stale", 5) == 0) {
    uint32_t irq = radio.getIrqStatus();
    uint16_t fp  = (uint16_t)(irq >> 16);
    uint8_t  st  = 0;
    uint16_t v   = 0;
    int nStale = 0, nDat = 0, nOk = 0;
    Serial.printf("[FK] stale test: irq=%08lX fp=%u\n", (unsigned long)irq, (unsigned)fp);
    for (int i = 0; i < 8; i++) {
      radio.readRxPktLenWithStatus(false, &st, &v);
      uint8_t cs = (st >> 1) & 3;
      if (cs == 3) nDat++; else if (cs == 2) nOk++;
      if (v == fp) nStale++;
      Serial.printf("[FK]   nowait #%d stat=%02X cmd=%u val=%u%s\n",
                    i, (unsigned)st, (unsigned)cs, (unsigned)v, v == fp ? "  <- fp" : "");
    }
    radio.readRxPktLenWithStatus(true, &st, &v);
    Serial.printf("[FK]   wait     stat=%02X cmd=%u val=%u\n",
                  (unsigned)st, (unsigned)((st >> 1) & 3), (unsigned)v);
    sprintf(reply, "fp=%u | nowait: fp-hits=%d/8 DAT=%d OK=%d | wait: cmd=%u len=%u",
            (unsigned)fp, nStale, nDat, nOk, (unsigned)((st >> 1) & 3), (unsigned)v);
    return true;
  }
#endif

  if (memcmp(arg, "rssi", 4) == 0) {
    int n = (arg[4] == ' ') ? atoi(arg + 5) : 32;
    if (n < 2 || n > 200) n = 32;
    int lo = 0, hi = 0;
    radioSampleRssi(n, &lo, &hi);
    sprintf(reply, "rssi n=%d min=%d max=%d spread=%d %s", n, lo, hi, hi - lo,
            (hi == lo) ? "CONSTANT -> chip not measuring" : "varying -> alive");
    return true;
  }

  if (memcmp(arg, "win", 3) == 0) {
    //en: NOTE: this consumes the window, so the watchdog check right after it
    //en: sees only what accumulated since. At ~32 samples/s that refills in well
    //en: under a second, so it cannot cause a false 'dead' verdict in practice.
    int lo = 0, hi = 0;
    uint32_t n = 0;
    radio_driver.takeRssiWindow(&lo, &hi, &n);
    sprintf(reply, "win n=%lu min=%d max=%d spread=%d %s", (unsigned long)n, lo, hi,
            n ? (hi - lo) : -1,
            (n == 0) ? "NO SAMPLES -> not armed in Rx" : (hi == lo) ? "CONSTANT -> chip not measuring" : "varying -> alive");
    return true;
  }

  if (memcmp(arg, "hammer", 6) == 0) {
    int n = (arg[6] == ' ') ? atoi(arg + 7) : 50;
    if (n < 1 || n > 2000) n = 50;
    //en: isChipResponding() is exactly the operation that wedged an SX1262 when it
    //en: ran every 30 s (standby -> register read -> re-arm Rx). Hammering it
    //en: compresses days of that into seconds.
    int ok = 0;
    for (int i = 0; i < n; i++) { if (radio_driver.isChipResponding()) ok++; }
    int lo = 0, hi = 0;
    radioSampleRssi(16, &lo, &hi);
    sprintf(reply, "hammer x%d (%d ok) | rssi min=%d max=%d spread=%d", n, ok, lo, hi, hi - lo);
    return true;
  }

  if (memcmp(arg, "reinit", 6) == 0) {
    bool ok = radio_init();
    if (ok) {
      radio_driver.begin();
      radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
      radio_driver.setTxPower(_prefs.tx_power_dbm);
      radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);
    }
    int lo = 0, hi = 0;
    radioSampleRssi(16, &lo, &hi);
    sprintf(reply, "reinit %s | rssi min=%d max=%d spread=%d", ok ? "OK" : "FAILED", lo, hi, hi - lo);
    return true;
  }

  return false;   //en: not ours - let the variant handler have it
}
#endif

void MyMesh::radioWatchdogLoop() {
  if (!millisHasNowPassed(next_radio_check)) return;
  next_radio_check = futureMillis(FKPR_RADIO_WATCHDOG);

  //en: The first call lands on the first loop() after boot, when the sampler has
  //en: had no chance to collect anything yet - that reads as 'no samples' and
  //en: would score a strike against a radio that is merely still starting up.
  //en: Arm here and start judging one full window later.
  //sk: Prve volanie padne na prvy loop() po boote, ked vzorkovac este nemal sancu
  //sk: nic nazbierat - to sa cita ako 'ziadne vzorky' a pripisalo by cierny bod
  //sk: radiu, ktore sa len rozbieha. Tu sa len naarmuj a posudzuj o cele okno neskor.
  if (!radio_wd_armed) { radio_wd_armed = true; return; }

  //en: Read the window the noise-floor sampler filled since the last check and
  //en: start a fresh one. This costs no SPI traffic of its own, which also means
  //en: it cannot repeat the mistake of the earlier attempts: the standby ->
  //en: register read -> re-arm Rx sequence they used is exactly what wedged an
  //en: SX1262 hard enough that only a power cycle recovered it.
  //sk: Precitaj okno, ktore od minulej kontroly naplnil vzorkovac noise-floor, a
  //sk: zacni nove. Nestoji to ziadnu vlastnu SPI komunikaciu, takze to ani
  //sk: nemoze zopakovat chybu skorsich pokusov: sekvencia standby -> citanie
  //sk: registra -> znovu do RX bola prave to, co zaseklo SX1262 tak, ze pomohol
  //sk: az power cycle.
  int lo = 0, hi = 0;
  uint32_t samples = 0;
  radio_driver.takeRssiWindow(&lo, &hi, &samples);
  int spread = samples ? (hi - lo) : -1;

  //en: Two independent failure signatures:
  //en:  samples == 0 - the sampler never ran, so the receiver was not armed in Rx
  //en:                 for the whole window. Holds even on a dead-quiet channel.
  //en:  spread  == 0 - it ran, but every read returned the same byte. A live
  //en:                 receiver cannot do that: thermal noise plus the 0.5 dB
  //en:                 step guarantee movement. A dead SPI slave reads 0x00 or
  //en:                 0xFF forever (and LR2021 gave -255 dBm with its supply cut).
  bool dead = (samples == 0) || (spread == 0);

#ifdef FK_RADIO_DIAG_ONLY
  //en: OBSERVATION MODE - measure and report, never act. Used to learn what a
  //en: healthy radio actually looks like on each chip before trusting the rule.
  Serial.printf("[FK] rssi-window n=%lu min=%d max=%d spread=%d%s\r\n",
                (unsigned long)samples, lo, hi, spread,
                dead ? ((samples == 0) ? "  <== NO SAMPLES" : "  <== CONSTANT") : "");
  return;
#endif

  if (!dead) { radio_dead_count = 0; return; }

  radio_dead_count++;
  MESH_DEBUG_PRINTLN("radioWatchdog: radio not answering (%d/3)", (int)radio_dead_count);
  Serial.printf("[FK] radio watchdog: %s (n=%lu min=%d max=%d) (%d/3)\r\n",
                (samples == 0) ? "receiver never armed in Rx" : "RSSI constant - chip not measuring",
                (unsigned long)samples, lo, hi, (int)radio_dead_count);
  if (radio_dead_count < 3) return;

  radio_dead_count = 0;
  Serial.println(F("[FK] radio not responding - re-initialising"));
  if (!radio_init()) {
    Serial.println(F("[FK] radio re-init FAILED - will retry"));
    return;
  }

  //en: radio_init() alone is not enough to get back on the air:
  //en:  - RadioLib's begin() drops the packet-received callback, so the wrapper
  //en:    has to re-attach it (that is what its begin() does, and it also puts
  //en:    the state back to IDLE so recvRaw() re-arms Rx);
  //en:  - std_init() configures the radio from the COMPILE-TIME defaults, so the
  //en:    runtime prefs have to be applied again or the node silently reverts to
  //en:    the built-in frequency/SF/power.
  radio_driver.begin();
  radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_driver.setTxPower(_prefs.tx_power_dbm);
  radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);
  board.setLoRaFemLnaEnabled(_prefs.radio_fem_rxgain);
  board.setLoRaFemPaGainEnabled(_prefs.radio_fem_txgain);

  Serial.printf("[FK] radio re-initialised OK (%.3fMHz sf=%d bw=%.1f tx=%d)\r\n",
                (double)_prefs.freq, (int)_prefs.sf, (double)_prefs.bw,
                (int)_prefs.tx_power_dbm);
}
#endif

void MyMesh::loop() {
#ifdef WITH_BRIDGE
  bridge.loop();
#endif

  mesh::Mesh::loop();

#ifdef FKPR_RADIO_WATCHDOG
  radioWatchdogLoop();
#endif

#ifdef FK_ANON_FLOOD_DIRECT_FALLBACK
  fkAnonFallbackLoop();   //en: direct resend of the login reply when the flood copy got lost
                          //sk: direct re-send login odpovede, keď sa flood kópia stratila
#endif

  if (next_flood_advert && millisHasNowPassed(next_flood_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    uint32_t delay_millis = 0;
    if (pkt) sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);

    updateFloodAdvertTimer(); // schedule next flood advert
    updateAdvertTimer();      // also schedule local advert (so they don't overlap)
  } else if (next_local_advert && millisHasNowPassed(next_local_advert)) {
    mesh::Packet *pkt = createSelfAdvert();
    if (pkt) sendZeroHop(pkt);

    updateAdvertTimer(); // schedule next local advert
  }

  if (set_radio_at && millisHasNowPassed(set_radio_at)) { // apply pending (temporary) radio params
    set_radio_at = 0;                                     // clear timer
    radio_driver.setParams(pending_freq, pending_bw, pending_sf, pending_cr);
    MESH_DEBUG_PRINTLN("Temp radio params");
  }

  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) { // revert radio params to orig
    revert_radio_at = 0;                                        // clear timer
    radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
    MESH_DEBUG_PRINTLN("Radio params restored");
  }

  // is pending dirty contacts write needed?
  if (dirty_contacts_expiry && millisHasNowPassed(dirty_contacts_expiry)) {
    acl.save(_fs);
    dirty_contacts_expiry = 0;
  }

  // update uptime
  uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;

#ifdef WITH_LORA_FOTA
  fotaLoop();   //en: deferred FOTA packet/CLI/flash + heartbeat (nrffota/FotaMyMesh.cpp)
#endif
}

// To check if there is pending work
bool MyMesh::hasPendingWork() const {
#if defined(WITH_BRIDGE)
  if (bridge.isRunning()) return true;  // bridge needs WiFi radio, can't sleep
#endif
  return _mgr->getOutboundTotal() > 0;
}

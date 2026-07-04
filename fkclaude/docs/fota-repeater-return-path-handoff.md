# Handoff: naimportovanie „return path" do repeatera z appky

> **Účel dokumentu:** návod pre inú session, ktorá upraví **appku** (Flutter
> `mc_fotanrf_flutterapp` a/alebo companion firmware / Python sender), aby vedela
> repeateru **poslať cestu (PATH paket)**, ktorou má repeater **odpovedať `sendDirect`**
> namiesto floodu. Rieši problém „2-hop repeater floodí všetky odpovede, reverzná cesta
> sa nikdy nezaloží" (viď pamäť `fota_reverse_path_never_established`).
>
> Dokument je **samostatný** — čitateľ nemusí poznať kontext session, v ktorej vznikol.
> Všetky odkazy sú `súbor:riadok` v repo `MeshCore` (branch `features/nrf-fota`,
> stav k 2026-07-04).

> **STAV (2026-07-04): IMPLEMENTOVANÉ — Možnosť A.** Companion má nový fork-only
> `CMD_SEND_RETURN_PATH = 0x70` (`examples/companion_radio/MyMesh.cpp`, wire
> `[0x70][pub_key 32B][path_len][path]`, handler = `createPathReturn` + direct/flood send).
> Appka (`fkallay1/meshcore-open`, commit `fbdf1cc`): checkbox „Najprv poslať cestu (PATH)"
> pri „Get missing Chunks" — pošle PATH (forward cesta **reverznutá**, 1B hashe) + 1,5 s
> pauzu pred `fota missall`. Companion treba preflashovať; smer cesty over per §6.

---

## 1. Problém (prečo to teraz nefunguje)

Repeater (`examples/simple_repeater`) si pre každého klienta pamätá **spätnú cestu**
`client->out_path` (v ACL tabuľke, `ClientACL`). Pri každej odpovedi sa rozhoduje takto —
`examples/simple_repeater/MyMesh.cpp:722` (ACK) a `:758` (CLI reply):

```cpp
if (client->out_path_len == OUT_PATH_UNKNOWN) {         // 0xFF
    sendFloodReply(reply, CLI_REPLY_DELAY_MILLIS, packet->getPathHashSize());  // ← flood
} else {
    sendDirect(reply, client->out_path, client->out_path_len, CLI_REPLY_DELAY_MILLIS);  // ← direct
}
```

`out_path` sa naplní **iba** v `onPeerPathRecv()` — `MyMesh.cpp:771`:

```cpp
bool MyMesh::onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret,
                            uint8_t* path, uint8_t path_len, ...) {
  int i = matching_peer_indexes[sender_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    auto client = acl.getClientByIdx(i);
    client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len);  // ← uloží
    client->last_activity = getRTCClock()->getCurrentTime();
  }
  return false;   // NOTE: no reciprocal path send!!  (repeater NEpošle späť SVOJU cestu)
}
```

Čiže **`out_path` sa nastaví len keď repeater dostane PATH paket** (`PAYLOAD_TYPE_PATH`).
Náš repeater **sám** cestu nezaloží:
- Príchodzí flooded CLI/telemetry REQ ide do `onPeerDataRecv` (nie PATH vetva) → repeater
  **neukladá** akumulovanú flood-cestu ako `out_path`.
- Automatická reciprocal-return cesta (`Mesh.cpp:169-172`) sa spustí **iba** pri príchodzom
  `PAYLOAD_TYPE_PATH`, nie pri REQ/TXT.

Preto pri 2-hop klientovi, ktorý talkuje direct z advertu, repeater floodí odpovede →
flood zomiera kolíziami (CAD off) → klient odpoveď nedostane.

**Riešenie, ktoré chceme:** admin/klient **explicitne pošle repeateru PATH paket** so
spätnou cestou → `onPeerPathRecv` ju uloží → repeater odteraz odpovedá `sendDirect`.

---

## 2. Ako sa PATH paket konštruuje (core mechanizmus)

Vyrába ho `Mesh::createPathReturn()` — `src/Mesh.cpp:437`:

```cpp
Packet* Mesh::createPathReturn(const uint8_t* dest_hash, const uint8_t* secret,
                               const uint8_t* path, uint8_t path_len,
                               uint8_t extra_type, const uint8_t* extra, size_t extra_len) {
  packet->header = (PAYLOAD_TYPE_PATH << PH_TYPE_SHIFT);   // ROUTE_TYPE_* sa doplní pri odoslaní
  int len = 0;
  memcpy(&packet->payload[len], dest_hash, PATH_HASH_SIZE); len += PATH_HASH_SIZE;  // dest hash (1B)
  len += self_id.copyHashTo(&packet->payload[len]);                                 // src  hash (1B)
  {
    uint8_t data[MAX_PACKET_PAYLOAD]; int data_len = 0;
    data[data_len++] = path_len;                                    // 1B: path_len (viď kódovanie nižšie)
    memcpy(&data[data_len], path, hash_count*hash_size);            // path bajty (hash_count × 1B)
    data_len += hash_count*hash_size;
    if (extra_len > 0) { data[data_len++] = extra_type; memcpy(&data[data_len], extra, extra_len); ... }
    else               { data[data_len++] = 0xFF; getRNG()->random(&data[data_len], 4); data_len += 4; }  // dummy blob (unique packet_hash)
    len += Utils::encryptThenMAC(secret, &packet->payload[len], data, data_len);    // MAC+šifra
  }
  packet->payload_len = len;
}
```

### Wire formát PATH paketu (payload po hlavičke)

```
[dest_hash 1B][src_hash 1B][ MAC(2B) + encrypt( [path_len 1B][path hashes N×1B][extra_type 1B alebo 0xFF][extra/rand] ) ]
                            └────────────── Utils::encryptThenMAC(secret, …) ──────────────────────────────┘
```

Konštanty (`src/MeshCore.h`):
- `PATH_HASH_SIZE = 1` → **hash uzla = prvý bajt jeho `pub_key`** (`Identity.h:19-20`:
  `memcpy(dest, pub_key, PATH_HASH_SIZE)`).
- `MAX_PATH_SIZE = 64`, `PUB_KEY_SIZE = 32`, `OUT_PATH_UNKNOWN = 0xFF`.
- **`path_len` kódovanie** (rovnaké ako všade v MeshCore): horné 2 bity = `hash_size-1`
  (0 → 1-bajtové hashe), dolných 6 bitov = `hash_count` (počet hopov). Pre 1-bajtové hashe
  je `path_len` = počet hopov (0–63).
- `secret` = **párový ECDH shared secret** medzi klientom a repeaterom
  (`ECDH(client_priv, repeater_pub)`) — TEN ISTÝ, akým sa šifruje admin login / CLI TXT.
  **NIE** kanálový `#fkotanrf` secret (ten je len pre GRP_DATA/FOTA).

### Ako to repeater prijme

`src/Mesh.cpp:126-179` (`PAYLOAD_TYPE_PATH` vetva): dešifruje payload párovým secretom,
rozparsuje `path_len`+`path` a zavolá `onPeerPathRecv(pkt, j, secret, path, path_len, …)`.
Ak vráti `true` **a** paket prišiel floodom, `Mesh.cpp:171` pošle aj reciprocal
(`createPathReturn(&src_hash, secret, pkt->path, …)` cez `sendDirect`). Náš repeater vracia
`false` (žiaden reciprocal — nevadí, my chceme len uložiť `out_path`).

---

## 3. Smer cesty (KRITICKÉ — ľahko sa pomýli)

`path` bajty vo `createPathReturn(dest, secret, path, …)` = **cesta, ktorou má PRÍJEMCA
(dest = repeater) doraziť k ODOSIELATEĽOVI (src = klient)**. Čiže do PATH paketu vkladáš
**spätnú cestu repeater → klient** = zoznam prvých bajtov `pub_key` medzi-repeaterov na trase.

Analógia klient-strany: `BaseChatMesh::handleReturnPathRetry` — `src/helpers/BaseChatMesh.cpp:360`:
```cpp
mesh::Packet* rpath = createPathReturn(contact.id, contact.getSharedSecret(self_id), path, path_len, 0, NULL, 0);
if (rpath) sendDirect(rpath, contact.out_path, contact.out_path_len, 3000);
//                          └ posiela sa DIRECT po ceste klient→repeater (forward),
//   payload `path` = cesta repeater→klient (reverzná)
```

Prakticky: appka už **pozná forward cestu** klient → repeater (je to `out_path` daného
kontaktu, ktorým klient posiela pakety repeateru). Cesta do PATH paketu = **tá istá cesta
v opačnom poradí hopov**.

> ⚠️ **Over na doske:** MeshCore miestami zaobchádza s cestou symetricky (reciprocal na
> `Mesh.cpp:171` posiela `pkt->path` bez otočenia). Poradie hopov (rovnaké vs. reverznuté)
> **empiricky over** cez `FOTA_DEBUG` log (viď §6) — pri jednom medzi-repeateri je to jedno,
> pri 2+ hopoch to treba potvrdiť.

---

## 4. Čo v appke chýba (a možnosti riešenia)

Companion firmware (`examples/companion_radio/MyMesh.cpp`) **nemá žiadny CMD**, ktorý by
poslal PATH paket kontaktu. Prehľad relevantných CMD:

| CMD | # | Čo robí | Použiteľné na náš cieľ? |
|---|---|---|---|
| `CMD_ADD_UPDATE_CONTACT` | 9 | nastaví **lokálny** `contact.out_path` (forward klient→repeater) | ❌ nastaví len companion, nič nepošle repeateru |
| `CMD_RESET_PATH` | 13 | `recipient->out_path_len = OUT_PATH_UNKNOWN` (lokálne) | ❌ |
| `CMD_SEND_RAW_DATA` | 25 | `createRawData` → **RAW_CUSTOM** paket, `sendDirect` | ❌ typ hlavičky je fixný RAW_CUSTOM, nie PATH |
| `CMD_SEND_PATH_DISCOVERY_REQ` | 52 | flood telemetry REQ (`MyMesh.cpp:1591`) | ⚠️ štandardný spôsob, ale s naším `simple_repeater` handshake nedokončí (repeater floodí reply a cestu neukladá) |

Takže na **explicitné** vloženie cesty treba jednu z troch ciest:

### Možnosť A — nový companion CMD (odporúčané pre Flutter appku)
Pridať do `examples/companion_radio/MyMesh.cpp` nový príkaz, napr. `CMD_SEND_RETURN_PATH`
(voľné číslo, napr. 64+; skontroluj kolízie v zozname `#define CMD_*` na začiatku súboru):

```cpp
} else if (cmd_frame[0] == CMD_SEND_RETURN_PATH && len >= 1 + PUB_KEY_SIZE + 1) {
    uint8_t* pub_key   = &cmd_frame[1];
    uint8_t  path_len  = cmd_frame[1 + PUB_KEY_SIZE];
    uint8_t* path      = &cmd_frame[1 + PUB_KEY_SIZE + 1];
    ContactInfo* rcpt  = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (rcpt) {
      // párový secret: ContactInfo ho má cachovaný (vzor: BaseChatMesh.cpp:363).
      // Ekvivalent z hola: self_id.calcSharedSecret(secret, rcpt->id) (Identity.h:74).
      const uint8_t* secret = rcpt->getSharedSecret(self_id);
      mesh::Packet* p = createPathReturn(rcpt->id, secret, path, path_len, 0, NULL, 0);
      if (p) {
        // pošli DIRECT po forward ceste (rcpt->out_path) ak je známa, inak flood:
        if (rcpt->out_path_len == OUT_PATH_UNKNOWN) sendFlood(p);
        else sendDirect(p, rcpt->out_path, rcpt->out_path_len);
        writeOKFrame();
      } else writeErrFrame(ERR_CODE_TABLE_FULL);
    } else writeErrFrame(ERR_CODE_NOT_FOUND);
}
```
Flutter strana: pridať frame `CMD_SEND_RETURN_PATH` + UI na zadanie/výber cesty (zoznam
hash bajtov medzi-repeaterov). Vyžaduje **flash companion firmware aj build appky**.

### Možnosť B — Python sender (bez zmeny companion FW)
Ak sa PATH posiela cez náš vlastný bridge/sender (`meshcore_py` / `ota_sender_mcpy.py`),
ktorý vie skladať surové mesh pakety, **skonštruuj PATH paket priamo v Pythone** (rovnaká
krypto pipeline ako pri GRP_DATA, len iný payload/typ):
- `header = PAYLOAD_TYPE_PATH << PH_TYPE_SHIFT` (+ route bity pri odoslaní),
- payload = `dest_hash(1B=repeater_pub[0]) + src_hash(1B=self_pub[0]) + encryptThenMAC(secret, path_len + path + 0xFF + rand4)`,
- `secret = ECDH(self_priv, repeater_pub)` (párový, nie kanálový),
- odoslať cez companion `CMD_SEND_RAW_DATA`? **NIE** (ten prepíše typ na RAW_CUSTOM) — treba
  buď priamy rádiový bridge, alebo Možnosť A. (Preto je B praktická len ak existuje cesta,
  ktorá pošle bajty paketu 1:1 do éteru.)

> Pozri existujúci FOTA sender, ako skladá GRP_DATA + `encryptThenMAC` — tá istá util
> (`src/helpers/Utils` ekvivalent v Pythone). Rozdiel je len secret (párový vs. kanálový)
> a formát vnútra.

### Možnosť C — fix na strane repeatera (nie „appka", ale najrobustnejšie; zváž)
V `simple_repeater/MyMesh.cpp` `onPeerDataRecv`: keď `client->out_path_len == OUT_PATH_UNKNOWN`
a paket prišiel **floodom**, adoptuj `packet->path` (reverznutú) ako `client->out_path`.
Tým sa reverzná cesta založí sama pri prvom flood-CLI, bez akéhokoľvek zásahu do appky.
Nevýhoda: mení firmware repeatera (a treba dať pozor na správne poradie/validáciu hopov).
Uvádzam pre úplnosť — používateľ chcel riešenie cez appku, čiže primárne A/B.

---

## 5. Autorizácia / bezpečnosť
- `onPeerPathRecv` prijme cestu, len ak je odosielateľ **známy klient v ACL**
  (`matching_peer_indexes[sender_idx] >= 0`). Admin session to spĺňa (klient je autentifikovaný
  párovým secretom). Cudzí uzol PATH nevloží.
- Cesta sa **nevaliduje** (`// TODO: prevent replay attacks`) — repeater uloží, čo dostane.
  Čiže admin môže vnútiť ľubovoľnú cestu (to je pre nás feature, nie bug).

---

## 6. Ako otestovať (na doske, cez FOTA_DEBUG)
FOTA env majú `-D FOTA_DEBUG=1` → Serial diagnostika. Postup:
1. Nahraj repeater FOTA build, otvor Serial monitor.
2. Z appky/senderu pošli PATH paket na repeater (Možnosť A/B).
3. V logu sľaduj:
   - príchod PATH: `[FOTA] RX RAW … type=8(?)…` (PAYLOAD_TYPE_PATH = 8) + `MESH_DEBUG` riadok
     `PATH to client, path_len=N` z `onPeerPathRecv` (`MyMesh.cpp:777`),
   - následne pošli admin CLI príkaz a over, že **odpoveď ide direct**: v `[FOTA] TX RAW`
     riadku má byť `route=` direct (nie flood) a `path[…]` sedí s vloženou cestou.
4. Kontrola dosahu: 2-hop klient dostane CLI odpoveď (predtým flood → strata).

`[FOTA] RX RAW` / `[FOTA] TX RAW` logy: `examples/simple_repeater/nrffota/FotaMyMesh.cpp`
(`fota_log_raw_line`, tlačí type/route/path). PAYLOAD_TYPE mená: `payload_type_name()` tamtiež.

---

## 7. Zhrnutie kľúčových odkazov
| Vec | Miesto |
|---|---|
| Voľba flood vs. direct pri odpovedi | `examples/simple_repeater/MyMesh.cpp:722`, `:758` |
| Uloženie `out_path` z PATH paketu | `examples/simple_repeater/MyMesh.cpp:771` (`onPeerPathRecv`) |
| Konštrukcia PATH paketu | `src/Mesh.cpp:437` (`createPathReturn`) |
| Príjem/parse PATH + reciprocal | `src/Mesh.cpp:126-179` |
| Klient-strana vzor (reciprocal send) | `src/helpers/BaseChatMesh.cpp:360` (`handleReturnPathRetry`) |
| Companion CMD zoznam | `examples/companion_radio/MyMesh.cpp:6-62` |
| `CMD_SEND_PATH_DISCOVERY_REQ` (52) | `examples/companion_radio/MyMesh.cpp:1591` |
| Konštanty (PATH_HASH_SIZE=1 …) | `src/MeshCore.h:8,18,22`; `src/Identity.h:19` |
| Súvis. pamäť | `fota_reverse_path_never_established`, `fota_cli_reply_timestamp_collision` |

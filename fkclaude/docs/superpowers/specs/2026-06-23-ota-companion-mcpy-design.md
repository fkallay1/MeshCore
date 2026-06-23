# OTA cez MeshCore companion (meshcore_py) — dizajn

- **Dátum:** 2026-06-23
- **Vetva:** `features/nrf-ota`
- **Autor:** Fedor Kallay + Claude
- **Súvisí:** `fkclaude/fcl_readme_nrf-ota.md`, `examples/simple_repeater/nrfota/`, pamäte `ota_meshcore_integration`, `ota_sender_scope`, `e2e_ota_session_20260621`

## 1. Cieľ

Nahradiť doterajší vlastný **FK_lora-sniffer bridge** (custom firmware, raw-TX cez serial rámce `[0xAB][0xCD][len][data]`) **štandardným MeshCore companionom** (`companion_radio`), ovládaným z PC cez knižnicu **meshcore_py**. Zachovať existujúci OTA delta-patch systém na `simple_repeater/nrfota/`.

Konkrétne deliverables:
1. **Zjednotený on-air OTA formát** (verzia protokolu `ota_prot_inf`), ktorý funguje rovnako cez bridge aj cez companion.
2. **`test_nrf-ota/ota_sender_mcpy.py`** — nový sender, ktorý cez meshcore_py posiela OTA pakety cez companion (serial, COM3), GRP_DATA, zerohop/flood/direct.
3. Úprava **`simple_repeater/nrfota`** receivera na nový formát (rozdelený HEADER na META + SIG, versioning, odvodené polia).
4. Úprava **`ota_sender.py`** (bridge) na **rovnaký** nový on-air formát.
5. Úprava runnera **`ota_test_lora_repeater.py`** — prepínač `--sender {bridge,mcpy}`.
6. **e2e direct (zerohop) test** cez companion: COM3 (companion, SK) → COM5 (repeater, SK).

### Non-goals
- Žiadne zmeny v `companion_radio` firmware (stock).
- `region` scope cez companion (vyžaduje `CMD_SET_FLOOD_SCOPE_KEY`) — neskôr; teraz stačí zerohop/flood/direct.
- BLE companion — používame USB-serial variantu.

## 2. Kľúčové zistenia (overené v kóde)

- **`Mesh.cpp:237`**: `onGroupDataRecv(pkt, type, channel, data, len)` dostáva **celý dešifrovaný plaintext** `data` (dĺžka `len`) — jadro **neodlupuje** `[data_type][len]`. (Companion-only `onChannelDataRecv` to robí, ale `simple_repeater` ho nepoužíva.) → receiver si `[data_type][len]` odlúpne sám.
- **`CMD_SEND_RAW_PACKET (65)`** zamietnuté: volá `sendPacket()`, nie `sendFloodScoped()` → flood/region forwarding cez repeatre nefunguje korektne. Preto **GRP_DATA** cez `CMD_SEND_CHANNEL_DATA (62)`.
- **`CMD_SEND_CHANNEL_DATA (62)`** handler (`companion_radio/MyMesh.cpp:1137`): frame `[62][channel_idx][path_len][path…][data_type 2B LE][payload…]`; `path_len=0`→zerohop (`sendDirect(pkt,path,0)`), `0xFF`→flood (`sendFloodScoped`), `N`→direct. On-air plaintext = `[data_type 2B][data_len 1B][payload]`, šifrované kanálovým kľúčom (`BaseChatMesh.cpp:508-537`).
- **Veľkostný rozpočet:** `MAX_PACKET_PAYLOAD=184`, `CIPHER_BLOCK_SIZE=16`, `MAX_GROUP_DATA_LENGTH = 184−16−3 = 165` (firmware tvrdo limituje `data_len`). Náš `data = [ts 4B] + ota_payload`, teda **`ota_payload ≤ 161 B`**.
- Companion env pre XIAO nRF52840: **`Xiao_nrf52_companion_radio_usb`**. Root `build_flags` defaultujú SK preset (`LORA_FREQ=869.618`, `LORA_BW=62.5`, `LORA_SF=8`, companion `LORA_CR=5`).

## 3. Zjednotený on-air formát

Obe cesty (bridge raw aj companion GRP_DATA) produkujú **bajt-identický** dešifrovaný plaintext:

```
[data_type 2B LE = OTA_MAGIC][data_len 1B = 4 + len(ota_payload)][ts 4B LE][ota_payload …]
```

- `OTA_MAGIC` — 16-bit konštanta ≠ `DATA_TYPE_RESERVED (0x0000)`, slúži ako **gating diskriminátor** (receiver spracuje len pakety s týmto `data_type`). Návrh: `OTA_MAGIC = 0x07A0` (definované v zdieľanej hlavičke).
- `ts` (4B) — **nonce na unikátnosť** (AES-ECB je deterministické, bez IV → rovnaký plaintext = rovnaký ciphertext = mesh dedup `hasSeen` by resend zahodil). Sender ho mení per-transmission. Je **mimo** `ota_payload` (kompatibilné s pôvodným „[ts][type]" rozložením — receiver ho po `[data_type][len]` skipuje).
- `ota_payload[0]` = OTA packet type.

**Prečo identický:** companion `sendGroupData` zostaví `[data_type][len][data]` a šifruje; bridge `ota_sender.py` zostaví **ten istý** plaintext sám a šifruje (rovnaký kanálový kľúč, AES-128-ECB + HMAC-SHA256) → ten istý ciphertext. Route header (`zerohop`/`flood`) môže byť medzi transportmi mierne odlišný, ale to je smerovanie, nie OTA payload — receiver ho nerieši.

## 4. OTA payload štruktúry (verzia `ota_prot_inf = 0x00`)

Všetky `packed`. `ota_prot_inf` (1B hneď za `type`) = verzia protokolu pre budúcu evolúciu.

### 4.1 `OTA_PKT_HEADER` — META (type 0x10) — 102 B

| offset | veľk. | pole | pozn. |
|---|---|---|---|
| 0 | 1 | `type` | 0x10 |
| 1 | 1 | `ota_prot_inf` | 0x00 |
| 2 | 4 | `patch_size` | LE; `total_chunks = ceil(patch_size / OTA_CHUNK_DATA)` |
| 6 | 32 | `patch_sha256` | overenie zostaveného patchu |
| 38 | 32 | `new_sha256` | SHA256 nového FW |
| 70 | 32 | `old_sha256` | SHA256 base FW (gating + base check) |

**Celých 102 B = podpisovaná správa** (Ed25519). `total_chunks` a `old_sha256_prefix` **odstránené** — sú odvoditeľné z `patch_size` resp. `old_sha256[0:4]`, ktoré sú v podpisovanej časti → integrita zachovaná.

`data_len = 4 + 102 = 106 ≤ 165` ✓

### 4.2 `OTA_PKT_HDR_SIG` — SIG (type 0x13, nový) — 99 B

| offset | veľk. | pole | pozn. |
|---|---|---|---|
| 0 | 1 | `type` | 0x13 |
| 1 | 1 | `ota_prot_inf` | 0x00 (zhodné s META; META je autoritatívne) |
| 2 | 32 | `old_sha256` | gating „patrí mne" (pre-filter, == META.old_sha256) |
| 34 | 1 | `key_id` | ktorý autor podpísal |
| 35 | 64 | `signature` | Ed25519 nad **102 B META** |

`data_len = 4 + 99 = 103 ≤ 165` ✓

### 4.3 `OTA_PKT_CHUNK` (type 0x11) — štruktúra nezmenená, `data ≤ 144 B`

| offset | veľk. | pole |
|---|---|---|
| 0 | 1 | `type` 0x11 |
| 1 | 2 | `chunk_idx` LE |
| 3 | 2 | `crc16` (nad `data[]`) |
| 5 | 4 | `old_fw_size` LE |
| 9 | 4 | `old_sha256_prefix` |
| 13 | ≤144 | `data` |

`OTA_CHUNK_DATA` znížené **150 → 144** (rezerva): `data_len = 4 + (13 + 144) = 161 ≤ 165` ✓. `total_chunks` sa odvodzuje z `OTA_CHUNK_DATA = 144`.

### 4.4 `OTA_PKT_APPLY` (type 0x12) — nezmenené (33 B). `data_len = 4 + 33 = 37` ✓

### 4.6 OTA kanál — `#fkotanrf`

Jeden zdroj pravdy = **názov kanála `#fkotanrf`** (MeshCore `#`-konvencia: secret odvodený z mena).

| veličina | hodnota | odvodenie |
|---|---|---|
| názov | `#fkotanrf` | |
| secret (16 B PSK) | `2382c5b811d390667e6a7800c03338ca` | `SHA256("#fkotanrf")[0:16]` (meno **vrátane `#`**) |
| gating `hash[0]` | `0xA4` | `SHA256(secret)[0]` |

- **Companion:** `set_channel(idx, "#fkotanrf")` — meshcore_py si secret odvodí sám (`device.py:216`). Companion uloží `channel.hash = SHA256(secret)[0] = 0xA4`.
- **Bridge `ota_sender.py`:** derivuje rovnaký secret (z `--channel-name "#fkotanrf"`, alebo `--psk 2382…ca`), šifruje sám.
- **Receiver (`ota_build_channel`):** secret obsahuje `0x00` (12. bajt) → **nehashovať ako string**. Zmena: `secret = SHA256(OTA_CHANNEL_NAME)[0:16]`, `secret[16:32]=0`, `hash = SHA256(secret, 16)[0]`. AES kľúč = `secret[:16]`, HMAC kľúč = `secret[:32]`. `OTA_CHANNEL_NAME` (build define) = `"#fkotanrf"`.
- Nahrádza pôvodné `OTA_CHANNEL_PSK = "meshcore-ota-key"`.

### 4.5 Podpis

`signature = Ed25519(privkey, META[0:102])`, kde `META` je presný 102-bajtový buffer (4.1). Pubkey v `OtaReceiver_signkey.cpp` (`s_authors[key_id]`) nezmenený. `key_id` mimo podpisovanej časti je bezpečné — zmena `key_id` → overenie proti inému pubkey zlyhá → drop.

## 5. Receiver zmeny (`simple_repeater/nrfota`)

### 5.1 `onGroupDataRecv` demux (`MyMesh.cpp`)
1. Existujúce: `type == PAYLOAD_TYPE_GRP_DATA`, `channel.hash[0] == _ota_channel.hash[0]`.
2. **Nové:** `len ≥ 3`, `data_type = data[0] | data[1]<<8`; ak `data_type != OTA_MAGIC` → return (nie náš). Sanity: `data[2] == len-3`.
3. `plain = data + 3; plen = len - 3` → `[ts 4B][ota_payload]`.
4. Existujúce: skip `ts` (4B), dispatch na `ota_payload[0]`.

### 5.2 Stavový automat hlavičky (META + SIG)
- Session dostane flagy `meta_recv`, `sig_recv`.
- `OTA_PKT_HEADER` (META): ulož `patch_size`, `patch_sha256`, `new_sha256`, `old_sha256`, `ota_prot_inf` + **uchovaj 102 B META buffer** (na overenie podpisu). `total_chunks = ceil(patch_size/144)`. `meta_recv=1`.
- `OTA_PKT_HDR_SIG` (SIG): ulož `key_id`, `signature` (+ `old_sha256` pre pre-filter). `sig_recv=1`.
- **„Header kompletný" = `meta_recv && sig_recv && verify_ok`**, kde `verify_ok = Ed25519_verify(signature, META[0:102], s_authors[key_id])`. Až potom sa session „promuje" (ako dnešná HEADER promócia — merge buffrovaných chunkov, zachovanie logu/bitmapy).
- Chunky pred kompletným headerom sa bufferujú (existujúca out-of-order logika); `old_fw_size` preberá z chunku (existujúca lekcia).
- **Perzistencia** (`meta.bin`): ukladať META polia + 102 B buffer + SIG (`key_id`+`signature`) + flagy, aby promócia prežila reboot a doplnenie druhého paketu po reboote.

### 5.3 `old_sha256` gating
- Pre-filter: porovnaj `old_sha256` z META/SIG proti **cache `base_fw_sha256`** (per-boot, už existuje) — **nie** prepočítavať SHA256 nad 444 kB appky per-paket (hot-RX stall, viď pamäť). Nezhoda → drop (paket nie je pre tento FW).
- Autoritatívny base-check pre flash používa **podpísaný** `old_sha256` z META.

### 5.4 Status (`OtaStatusPkt` / `ota status`)
- Reportovať čiastkový stav hlavičky: `meta_recv`, `sig_recv`, `verify_ok` (napr. nové bity v `status`, alebo rozšírený výpis). VERIFIED (`OTA_ST_VERIFIED`) ostáva = všetky chunky + header kompletný + SHA256 patchu OK.

### 5.5 `OtaProtocol.h`
- Pridať `OTA_PKT_HDR_SIG 0x13`, `OTA_MAGIC` (0x07A0), `OTA_PROT_INF_V0 0x00`.
- Prepísať `OtaHeaderPkt` na META (4.1), pridať `OtaHdrSigPkt` (4.2).
- `OTA_CHUNK_DATA_MAX 150 → 144` (alebo nová konštanta `OTA_CHUNK_DATA`); zladiť odvodenie `total_chunks` a `exp_len`.

### 5.6 OTA kanál (`OtaMesh.cpp ota_build_channel`)
- `OTA_CHANNEL_NAME` (build define) = `"#fkotanrf"` (nahrádza `OTA_CHANNEL_PSK`).
- `secret = SHA256(OTA_CHANNEL_NAME)[0:16]` (= `2382…ca`), `secret[16:32]=0`, `hash = SHA256(secret,16)[0]` (= `0xA4`). Viac §4.6. Pozor na `0x00` v secrete — nepoužívať `strlen`.

## 6. Sender zmeny

Zdieľaná logika sa vyčlení tak, aby ju používali obaja senderi (DRY, bez duplicity):

**Zdieľaný builder (funkcie v `ota_sender.py`, importované do `ota_sender_mcpy.py`):**
- `make_patch()` (hdiffi + zlib) — nezmenené.
- `build_meta_payload()` → 102 B META (`ota_payload`).
- `build_sig_payload(meta, privkey, key_id)` → 99 B SIG; `signature = Ed25519(meta)`.
- `build_ota_chunk()` (`OTA_CHUNK_DATA=144`), `build_ota_apply()` — upravené veľkosti/typy.
- `crc16`, `sign_ota_header` (premenovať na `sign_meta`), Ed25519 load.

### 6.1 `ota_sender.py` (bridge) — zmena wire-formátu
- `build_grpdata_payload(psk, ota_payload, ts)` → plaintext = `[OTA_MAGIC 2B][len 1B = 4+len(ota_payload)][ts 4B][ota_payload]`, potom AES-ECB+HMAC ako dnes.
- HEADER sa posiela ako **dva** pakety (META + SIG) namiesto jedného 172 B.
- `wrap_meshcore_packet` (route/header) nezmenený; bridge transport (`[0xAB][0xCD]` rámec) nezmenený.
- Starý 172 B `OtaHeaderPkt` formát sa **retiruje** (jeden formát).

### 6.2 `ota_sender_mcpy.py` (companion, nový) — async
- `mc = await MeshCore.create_serial(port, baud)` (default COM3).
- **SK preset runtime:** `await mc.commands.set_radio(869.618, 62.5, 8, 5)`.
- **OTA kanál:** `await mc.commands.set_channel(channel_idx, "#fkotanrf")` — secret si meshcore_py odvodí (`SHA256("#fkotanrf")[0:16]`), companion `hash[0]=0xA4` == receiver. `channel_idx` cez `--channel-idx`, default 1. (Viď §4.6.)
- **Per OTA paket:** zostav `data = [ts 4B] + ota_payload`; frame = `bytes([62, channel_idx, path_len]) + path + struct.pack('<H', OTA_MAGIC) + data`; pošli `await mc.commands.send(frame, [EventType.OK, EventType.ERROR])` (companion `writeOKFrame` → `EventType.OK`).
  - **Companion šifruje** (má kanál) → `ota_sender_mcpy` **nerobí AES/HMAC** (na rozdiel od bridge).
  - `path_len`: `zerohop=0` (default test), `flood=0xFF`, `direct=N` + path hashe.
- Poradie/pacing/redundancia (`--delay`, `--header-every`, packetorder) — analogicky k `ota_sender.py`; HEADER = teraz META **a** SIG (oba treba doručiť).
- Status/NACK od zariadenia **nečíta** cez companion — VERIFIED rieši runner pollovaním `ota status` na COM5 (ako dnes).

### 6.3 meshcore_py poznámky
- `create_serial` spraví app-start handshake (vráti `None` ak zariadenie neodpovedá).
- `set_radio(freq_MHz, bw_kHz, sf, cr)` — `device.py:70`.
- `set_channel(idx, name, secret)` — `device.py:206`; secret 16 B raw.
- `mc.commands.send(data, expected)` — `commands/base.py:155` (subscribe-before-send, vracia Event).

## 7. Runner zmeny (`ota_test_lora_repeater.py`)

- **`--sender {bridge,mcpy}`** (default `bridge` = spätná kompatibilita).
  - `bridge`: ako dnes — FK_lora bridge na COM3 (preset CZ), `OTA_SENDER = ota_sender.py`.
  - `mcpy`: companion na COM3 (preset SK), `OTA_SENDER = ota_sender_mcpy.py`.
- **`--preset {cz,sk}`** (odvodené od sendera, override možný): `bridge→cz`, `mcpy→sk`.
- `phase_baseline` pre `mcpy`:
  - build+upload **`Xiao_nrf52_companion_radio_usb`** → COM3 (SK default v build_flags).
  - repeater (`ProMicro_repeater_ota` / `Xiao_nrf52_repeater` OTA) na COM5, preladený na **SK** (CLI `set radio 869.618,62.5,8,5` + reboot; prefs prežijú).
- `phase_run` pre `mcpy`: zostaví `ota_sender_mcpy.py` príkaz (`--port COM3 --channel-name "#fkotanrf" --channel-idx 1 --scope zerohop --delay …`, `--privkey test_key.der --keyid 1`). Companion šifruje sám (kanál v `set_channel`).
- `broadcast_until_verified` / `ota flash` / `evaluate` — bez zmeny logiky.

## 8. e2e testy (zerohop) — OBE cesty

**HW (jedna mašina):** **COM3 = Seeed XIAO nRF52840**, **COM5 = ProMicro nRF52840 repeater** (OTA DUT, `ProMicro_repeater_ota`). COM3 sa **reflashuje podľa cesty** (jeden kus HW, dve roly). Reflash všetkého je očakávaný.

### 8a. Bridge cesta (regresia nového formátu)
- COM3 ← **FK_lora `Xiao_bridge`** (preset **CZ**). Repeater COM5 ← `ProMicro_repeater_ota` (CZ).
- `baseline --sender bridge` → `run --sender bridge` (`ota_sender.py`, nový zjednotený formát) → VERIFIED → `ota flash` → `build #NEW`.
- Cieľ: overiť, že zjednotený formát + META/SIG split nerozbil bridge cestu.

### 8b. Companion cesta (hlavný cieľ)
- COM3 ← **`Xiao_nrf52_companion_radio_usb`** (preset **SK**, runtime `set_radio`). Repeater COM5 ← `ProMicro_repeater_ota`, preladený na **SK** (CLI `set radio 869.618,62.5,8,5` + reboot).
- `baseline --sender mcpy` → `run --sender mcpy` (`ota_sender_mcpy.py`, zerohop) → poll `ota status` COM5 → VERIFIED → `ota flash` → `build #NEW`.

**Akceptačné kritérium (obe):** repeater nabehne na NEW build# cez OTA — pri 8b výhradne cez MeshCore companion (žiadny FK_lora bridge).

> Pozn. preset: 8a beží na CZ (mimo živej SK siete, ako doterajší runner), 8b na SK. Repeater sa medzi behmi preladí (prefs prežijú; medzi behmi `ota clear`).

## 9. Riziká / otvorené

- **`mc.commands.send()` pre custom frame**: overiť, že base `send()` je dostupné na `mc.commands` a že `RESP_CODE_OK`→`EventType.OK` mapovanie funguje; fallback `mc.connection_manager.send(frame)` (fire-and-forget).
- **`set_radio` jednotky/perzistencia**: potvrdiť MHz/kHz a `savePrefs` (aby preset prežil reboot companionu).
- **Pacing**: companion má vlastný Dispatcher (CAD/duty-cycle). Pre zerohop 1-hop by `--delay` malo stačiť (pamäť: 1-hop otočka ~1.5 s); multi-hop mimo rozsahu tohto testu.
- **`OTA_MAGIC` kolízia**: zvoliť hodnotu mimo bežných MeshCore `data_type` (companion repeater nemá iné kanály okrem OTA → nízke riziko).
- **Re-test bridge e2e**: keďže `ota_sender.py` mení wire-formát, treba pre istotu pretestovať aj `--sender bridge` cestu (ak je FK_lora bridge dostupný).
- **`test_key.der`** je gitignored — na čerstvom klone chýba (existujúca lekcia).

## 10. Testovacia stratégia

- **Offline (host):** unit test zdieľaného buildera — overiť, že `ota_sender.py` (bridge) aj `ota_sender_mcpy.py` (companion) produkujú **identický dešifrovaný plaintext** pre META/SIG/chunk (re-implementovať companion `sendGroupData` wrapping v teste a porovnať bajty). Overiť `data_len ≤ 165` pre všetky typy. Overiť Ed25519 podpis/overenie nad 102 B META.
- **HW e2e:** sekcia 8.

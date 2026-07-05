# nrffota — LoRa-FOTA (delta-patch) pre MeshCore repeater (nRF52840)

Port funkčného FOTA systému z projektu **FK_lora-sniffer** do MeshCore repeatera.
Umožňuje aktualizovať firmvér repeatera **cez LoRa** prenosom malého delta-patchu
(rozdiel medzi starým a novým FW), nie celého firmvéru.

> Toto je **iné** ako vstavané MeshCore OTA (`NRF52Board::startOTAUpdate`), ktoré
> len reštartuje do Nordic DFU a FW sa nahráva cez BLE. Tu ide o patch cez LoRa.

## Ako to funguje

```
PC (fota_sender.py)                     Repeater (nRF52840)
  hdiffi -inplaceB old new patch         onGroupDataRecv()  ← MeshCore GRP_DATA
  zlib(-9, wbits=-9) → staged            fota_process()
  GRP_DATA (AES-128-ECB + HMAC) ──LoRa──▶ chunky → CustomLFS append-log
  HEADER / CHUNK / APPLY                   COMPLETE → assemble patch.bin + SHA256
                                          'fota flash' → flasher@0xEB000:
                                            HPatchLite inplaceB (old=XIP, new=app flash)
                                            streaming DEFLATE (puff_stream)
                                            NVMC zápis + verify → reset
```

- **Transport:** FOTA pakety idú ako MeshCore `PAYLOAD_TYPE_GRP_DATA` na dedikovanom
  kanáli (`#fkotanrf`, #-konvencia). MeshCore ich dešifruje (`Utils::MACThenDecrypt`, AES-128-ECB +
  HMAC-SHA256) a `MyMesh::onGroupDataRecv()` ich odovzdá do `fota_process()`.
  Žiadna zmena jadra MeshCore. Celá integrácia do repeatera žije v
  `FotaMyMesh.cpp` (telá FOTA metód MyMesh vrátane overridov
  `searchChannelsByHash()`/`onGroupDataRecv()`); v samotnom `MyMesh.cpp`
  je len 6 tenkých `#ifdef WITH_LORA_FOTA` hookov (~30 riadkov diff vs upstream).
- **Krypto:** `mesh::Utils::sha256` + `rweather/Crypto` (rovnaká dep ako MeshCore).
- **FS:** dedikovaný `CustomLFS` na 0xD4000 (oddelený od MeshCore `InternalFS`).
- **Patch formát:** HPatchLite `inplaceB` zabalený v `[ZLIB][uncomp][new_fw][deflate]`.

## Flash mapa

**Kanonický zdroj: [`flash_layout.h`](flash_layout.h)** (jediné miesto s adresami;
stručne aj v AGENTS.md). V skratke: app končí na 0xD4000, FOTA FS 92 kB @ 0xD4000,
flasher @ 0xEB000, MeshCore InternalFS @ 0xED000 NEDOTKNUTÝ.
Strop patchu ~40 kB (recv.log + patch.bin súčasne). Pre s140 v7 (XIAO/SenseCap) je
app base `0x27000` — nič netreba nastavovať: FW ho zistí z linker symbolu
(`fota_running_fw_base()`) a odovzdá flasheru runtime (jeden board-agnostický blob).

## Build

```bash
# 1) vygeneruj flasher blob (raz, resp. po zmene flasher/flasher.c)
python examples/simple_repeater/nrffota/tools/build_flasher.py

# 2) build FOTA repeatera
pio run -e ProMicro_repeater_fota
```

FOTA je zapnuté build-flagom `-D WITH_LORA_FOTA=1` (viď `variants/promicro/platformio.ini`).
Bez tohto flagu sú všetky `nrffota/` súbory inertné → stock repeater buildy sú
nedotknuté. Kanál: `-D FOTA_CHANNEL_NAME='"#fkotanrf"'` — MeshCore #-konvencia
(secret = SHA256(meno)[0:16]), musí sa zhodovať so senderom.

Diagnostické výpisy na Serial (`[FOTA] …`, `[FLASHER-DBG] …`) sú za flagom
`-D FOTA_DEBUG=1` (makrá `FOTA_DEBUG_PRINT/PRINTLN` vo [`FotaDebug.h`](FotaDebug.h),
vzor MESH_DEBUG). FOTA envy ho majú default zapnutý; bez neho sa výpisy vôbec
nekompilujú — CLI odpovede (`reply`) fungujú vždy.

## Ovládanie (Serial alebo LoRa admin CLI)

| Príkaz            | Akcia                                                      |
|-------------------|------------------------------------------------------------|
| `fota status`     | stav session (recv/total, flags, veľkosť)                  |
| `fota verify`     | dry-run: aplikuj patch → SHA256, **nič nezapisuje**        |
| `fota flash`      | **OSTRÝ** flash + reboot (nevráti sa pri úspechu)          |
| `fota clear`      | vymaž FOTA session z FS                                    |
| `fota miss`       | chýbajúce chunky, čiarkami (`H,S,0-4,6,8,9`; dvojica `a,b`; strop 20 tokenov) |
| `fota missall [cesta]` | všetky chýbajúce (bez stropu); voliteľná cesta (repeater→klient hopy, 2/4/6-hex tokeny) sa uloží do ACL → odpovede idú direct |
| `fota getpath`    | LoRa: vypíš ACL spätnú cestu volajúceho klienta            |
| `fota setpath <cesta>` | LoRa: ulož spätnú cestu klienta do ACL (odpoveď už ide ňou) |
| `fota getacl`     | Serial+FOTA_DEBUG: výpis ACL s cestami (`getpath`/`setpath <pubkey-prefix> …` majú serial varianty tiež) |
| `fota nack`       | vypíš chýbajúce chunky (NACK formát)                       |
| `fota decompress` | debug: dekomprimuj patch.bin cez puff_stream, vypíš FNV    |
| `fota dbg`        | vypíš flasher debug marker (GPREGRET2/RESETREAS + trace)   |
| `fota id`         | FW identita (build#, veľkosť, running SHA256)              |
| `fota agc`        | read-only diagnostika rádia (RxGain, RSSI, noise floor)    |

Miss total v odpovedi: `/T` overený; `/~T(noS)` odhad z META pred SIG; `(noH)`/`(noHS)`
bez META — vtedy zoznam končí `N-??` (N = najvyšší prijatý + 1; appka rozvinie z totalu
balíka). Legacy prefix `ota …` stále funguje (alias, viď `FOTA-CLI-ALIAS` vo FotaMyMesh.cpp).
Na Serial sa píšu priamo (`fota status`). Cez LoRa idú ako admin CLI príkazy (rovnaká cesta
ako ostatné MeshCore CLI cez `onPeerDataRecv` TXT); voliteľný companion tag `NN|` pred
príkazom sa strippe a zrkadlí v odpovedi.

## Posielanie patchu z PC

Pozri `test_nrf-fota/fota_sender.py --mode meshcore` (bridge) alebo
`test_nrf-fota/fota_sender_mcpy.py` (cez companion). Generuje patch
(`hdiffi -inplaceB` + zlib) a vysiela HEADER/CHUNK/APPLY ako GRP_DATA.

## Bezpečnosť flashera

- Pred prepisom overí, že bežiaci FW == `old` z patchu (SHA256). Ak nesedí → nepíše.
- Po zápise prečíta flash späť (FNV-1a) a pri nezhode skočí do DFU bootloadera
  namiesto bootu pokazeného FW (zariadenie sa dá obnoviť cez USB).
- `fota verify` (dry-run) prejde celý patch bez zápisu — testuj ho pred `fota flash`.

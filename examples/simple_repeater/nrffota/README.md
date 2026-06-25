# nrffota — LoRa-OTA (delta-patch) pre MeshCore repeater (nRF52840)

Port funkčného OTA systému z projektu **FK_lora-sniffer** do MeshCore repeatera.
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
                                          'ota flash' → flasher@0xEB000:
                                            HPatchLite inplaceB (old=XIP, new=app flash)
                                            streaming DEFLATE (puff_stream)
                                            NVMC zápis + verify → reset
```

- **Transport:** OTA pakety idú ako MeshCore `PAYLOAD_TYPE_GRP_DATA` na dedikovanom
  kanáli (PSK). MeshCore ich dešifruje (`Utils::MACThenDecrypt`, AES-128-ECB +
  HMAC-SHA256) a `MyMesh::onGroupDataRecv()` ich odovzdá do `fota_process()`.
  Žiadna zmena jadra MeshCore — len override `searchChannelsByHash()` +
  `onGroupDataRecv()`.
- **Krypto:** `mesh::Utils::sha256` + `rweather/Crypto` (rovnaká dep ako MeshCore).
- **FS:** dedikovaný `CustomLFS` na 0xD4000 (oddelený od MeshCore `InternalFS`).
- **Patch formát:** HPatchLite `inplaceB` zabalený v `[ZLIB][uncomp][new_fw][deflate]`.

## Flash mapa (nRF52840, extrafs.ld, s140 v6)

```
0x26000 - 0xD4000 : aplikačný kód repeatera (712 kB)
0xD4000 - 0xEB000 : OTA FS (CustomLFS, 92 kB — recv.log/patch.bin/meta/bitmap)
0xEB000 - 0xEC000 : flasher kód (4 kB, beží mimo app flash)
0xEC000 - 0xED000 : flasher trace/meta (4 kB)
0xED000 - 0xF4000 : MeshCore InternalFS (28 kB — identity/prefs/ACL, NEDOTKNUTÝ)
0xF4000+          : bootloader
```
Strop patchu ~40 kB (recv.log + patch.bin súčasne). Pre s140 v7 (XIAO/SenseCap) je
app base `0x27000` — nič netreba nastavovať: FW ho zistí z linker symbolu
(`fota_running_fw_base()`) a odovzdá flasheru runtime (jeden board-agnostický blob).

## Build

```bash
# 1) vygeneruj flasher blob (raz, resp. po zmene flasher/flasher.c)
python examples/simple_repeater/nrffota/tools/build_flasher.py

# 2) build OTA repeatera
pio run -e ProMicro_repeater_fota
```

OTA je zapnuté build-flagom `-D WITH_LORA_FOTA=1` (viď `variants/promicro/platformio.ini`).
Bez tohto flagu sú všetky `nrffota/` súbory inertné → stock repeater builды sú
nedotknuté. PSK kanála: `-D FOTA_CHANNEL_PSK='"..."'` (musí sa zhodovať so senderom).

## Ovládanie (Serial alebo LoRa admin CLI)

| Príkaz           | Akcia                                                      |
|------------------|------------------------------------------------------------|
| `ota status`     | stav session (recv/total, flags, veľkosť)                  |
| `ota verify`     | dry-run: aplikuj patch → SHA256, **nič nezapisuje**        |
| `ota flash`      | **OSTRÝ** flash + reboot (nevráti sa pri úspechu)          |
| `ota clear`      | vymaž OTA session z FS                                     |
| `ota decompress` | debug: dekomprimuj patch.bin cez puff_stream, vypíš FNV    |
| `ota nack`       | vypíš chýbajúce chunky                                     |
| `ota dbg`        | vypíš flasher debug marker (GPREGRET2/RESETREAS + trace)   |

Na Serial sa píšu priamo (`ota status`). Cez LoRa idú ako admin CLI príkazy
(rovnaká cesta ako ostatné MeshCore CLI cez `onPeerDataRecv` TXT).

## Posielanie patchu z PC

Pozri `FK_lora-sniffer/tools/fota_sender.py --mode meshcore --psk meshcore-ota`.
Generuje patch (`hdiffi -inplaceB` + zlib) a vysiela HEADER/CHUNK/APPLY ako GRP_DATA.

## Bezpečnosť flashera

- Pred prepisom overí, že bežiaci FW == `old` z patchu (SHA256). Ak nesedí → nepíše.
- Po zápise prečíta flash späť (FNV-1a) a pri nezhode skočí do DFU bootloadera
  namiesto bootu pokazeného FW (zariadenie sa dá obnoviť cez USB).
- `ota verify` (dry-run) prejde celý patch bez zápisu — testuj ho pred `ota flash`.

# LoRa-OTA pre MeshCore repeater (nRF52840) — kompletný popis

Aktualizácia firmvéru MeshCore **repeatera cez LoRa** prenosom malého **delta-patchu**
(rozdiel starý→nový FW), namiesto nahrávania celého firmvéru. Port funkčného systému
z projektu **FK_lora-sniffer**. Toto je **iné** ako vstavané MeshCore „OTA"
(`NRF52Board::startOTAUpdate`), ktoré len reštartuje do Nordic DFU a FW sa nahráva cez BLE.

---

## 1. Čo bolo spravené

| Oblasť | Súbory | Popis |
|--------|--------|-------|
| OTA modul | [examples/simple_repeater/nrfota/](examples/simple_repeater/nrfota/) | celý OTA kód (príjem + patchovanie), MeshCore jadro nezmenené |
| Integrácia | [MyMesh.h](examples/simple_repeater/MyMesh.h) / [MyMesh.cpp](examples/simple_repeater/MyMesh.cpp) | 3 malé `#ifdef WITH_LORA_OTA` zásahy |
| Build env | [variants/promicro/platformio.ini](variants/promicro/platformio.ini) | `ProMicro_repeater_ota` (extrafs.ld + flag + CZ) |
| Test | [test_nrf-ota/](test_nrf-ota/) | end-to-end LoRa test + nástroje z FK_lora |

Detailný popis OTA modulu samotného: [examples/simple_repeater/nrfota/README.md](examples/simple_repeater/nrfota/README.md).

---

## 2. Architektúra

```
PC (test_nrf-ota/ota_sender.py)              REPEATER (nRF52840, MeshCore)
  hdiffi -inplaceB old new patch               MyMesh::onGroupDataRecv()  ← GRP_DATA (dešifr.)
  zlib(-9,wbits=-9) → staged [ZLIB|..|deflate] ota_process()  (skip 4B ts → OTA typ)
  GRP_DATA (AES-128-ECB + HMAC) ──┐            chunky → CustomLFS append-log (/ota/recv.log)
                                  │ LoRa       COMPLETE → assemble patch.bin + SHA256 → VERIFIED
  BRIDGE (XIAO, FK_lora) ─────────┘            'ota flash' → flasher@0xEB000:
  [0xAB CD len] serial → raw LoRa TX             HPatchLite inplaceB (old=XIP, new=app flash)
                                                 streaming DEFLATE (puff_stream) + NVMC + verify
                                                 reset → boot NEW
```

### Transport (GRP_DATA kanál)
OTA pakety idú ako MeshCore `PAYLOAD_TYPE_GRP_DATA` na dedikovanom kanáli (PSK).
- `MyMesh::searchChannelsByHash()` — keď sa `channel_hash` zhoduje s OTA kanálom, vráti ho
  → MeshCore dešifruje payload cez `Utils::MACThenDecrypt` (AES-128-ECB + HMAC-SHA256).
- `MyMesh::onGroupDataRecv()` — dostane plaintext `[ts 4B][ota_type][...]`, preskočí 4B
  timestamp a zavolá `ota_process()`.
- **Žiadna zmena jadra MeshCore** — `onGroupDataRecv`/`searchChannelsByHash` sú existujúce virtuálne hooky.

Kanál: `hash = sha256(psk)[0]`, `secret = psk doplnené nulami na 32B`. PSK je 16/32-bajtový
(default `"meshcore-ota-key"`). Zhodné s `ota_sender.py --mode meshcore --psk <hex>`.

### Krypto / FS — znovupoužité z MeshCore
- **SHA256**: `mesh::Utils::sha256` + `rweather/Crypto` (existujúca dep, žiadna nová).
- **AES+HMAC**: `mesh::Utils::MACThenDecrypt` (rovnaké ako bežné MeshCore pakety).
- **FS**: `CustomLFS` (oltaco, existujúca dep) — dedikovaný región @ 0xD4000.

### Flash mapa (extrafs.ld, app končí 0xD4000)
```
0x26000–0xD4000  aplikačný kód repeatera (712 kB)
0xD4000–0xEB000  OTA FS (CustomLFS, 92 kB — recv.log/patch.bin/meta/bitmap)
0xEB000–0xEC000  flasher kód (4 kB, beží MIMO app flash aj InternalFS)
0xEC000–0xED000  flasher trace/meta (4 kB)
0xED000–0xF4000  MeshCore InternalFS (identity/prefs/ACL — NEDOTKNUTÝ)
0xF4000+         bootloader
```
Flasher na **0xEB000** (nie 0xF2000 ako sniffer) — vyhne sa MeshCore InternalFS.
Strop patchu ~40 kB (recv.log + patch.bin súčasne v 92 kB FS).

### Flasher (in-place patch, bezpečnosť)
1. Overí že bežiaci FW == `old` z patchu (SHA256). Ak nesedí → NEPÍŠE.
2. Načíta patch do RAM, `sd_softdevice_disable()`, zapíše flasher do 0xEB000, skočí naň.
3. Flasher: HPatchLite inplaceB streaming (old=XIP flash, patch z RAM cez puff_stream,
   new=app flash in-place), NVMC zápis po stránkach.
4. Po zápise prečíta flash späť (FNV-1a); pri nezhode skok do DFU (nie boot pokazeného FW).
5. `NVIC_SystemReset` → boot nového FW.

---

## 3. Build

```bash
# 1) flasher blob (raz / po zmene flasher.c)
python examples/simple_repeater/nrfota/tools/build_flasher.py     # → nrfota/flasher_code.h

# 2) OTA repeater
pio run -e ProMicro_repeater_ota
```
- Gated `-D WITH_LORA_OTA=1`. Bez flagu sú nrfota súbory inertné → stock buildy nedotknuté.
- OTA env používa `boards/nrf52840_s140_v6_extrafs.ld` (app končí 0xD4000) + **CZ preset**
  (`LORA_FREQ=869.525`, `SF=7`) aby sa nerušila SK sieť a zhodovalo sa s FK_lora bridge.
- Pre XIAO/s140 v7: pridaj `-D OTA_SOFTDEVICE_V7` a regeneruj flasher `BOARD_FLASHER=xiao`.

---

## 4. Ovládanie (Serial alebo LoRa admin CLI)

`ota status | verify | flash | clear | decompress | nack | dbg`

- `ota verify` = dry-run (aplikuje patch → SHA256, **nič nezapíše**)
- `ota flash`  = **OSTRÝ** flash + reboot (nevráti sa pri úspechu)

Cez Serial píš priamo (`ota status`). Cez LoRa idú ako admin CLI príkazy (existujúca
MeshCore cesta). Flasher sa púšťa **manuálne** (`ota flash`) — auto-APPLY cez LoRa je tiež
možný (`ota_sender --reboot`), ale default je manuálne spustenie po `ota verify`.

---

## 5. Build number (dočasné testovacie lešenie)

`test_nrf-ota/gen_build_info.py` (pre-script OTA env-u) pri každom builde inkrementuje
`test_nrf-ota/build_number.txt` a generuje `build_info.h` s `FW_BUILD_NUMBER`. Repeater
ho vypisuje na boote a v heartbeate `[OTA] AALIVE build #N`. Slúži na:
- detekciu, či po OTA flash beží NOVÁ verzia (build# sa zvýšil),
- zaručenie že OLD != NEW (build# je súčasť kódu → patch nie je prázdny).

---

## 6. End-to-end test cez LoRa

**Topológia:** `PC ─USB─ XIAO bridge (COM3) ─LoRa─ ProMicro repeater (COM5) ─USB─ PC`

Bridge = FK_lora `gateway_fw` (`[0xAB CD len]` serial → raw LoRa TX). Repeater = MeshCore OTA.
Oba na **CZ presete** (869.525/SF7), aby sa počuli a nerušili SK sieť.

```bash
PENV=~/.platformio/penv/Scripts/python.exe   # má pyserial + platformio

# 1) baseline: flash bridge (CZ) na COM3 + repeater OLD (CZ) na COM5
$PENV test_nrf-ota/ota_test_lora_repeater.py baseline --bridge-port COM3 --target-port COM5
#    (ak XIAO už beží ako CZ bridge:  pridaj --skip-bridge)

# 2) run: build NEW, patch OLD→NEW, broadcast cez bridge, flash, verify
$PENV test_nrf-ota/ota_test_lora_repeater.py run --bridge-port COM3 --target-port COM5
#    voliteľne: --cycles 4 --drop 0.2  (simulácia straty + kumulácia naprieč cyklami)
```

Test:
1. `baseline` — postaví+nahrá bridge (CZ) a repeater OLD (CZ), uloží OLD app obraz + build#.
2. `run` — postaví NEW, vyrobí patch (`hdiffi` + zlib), broadcastuje cez bridge ako GRP_DATA,
   po VERIFIED spustí `ota verify` (dry-run) a `ota flash`, po reboote overí `build #NEW` +
   `[FLASHER-DBG]` marker + FNV-1a checksum.

**Predpoklady:** COM5/COM3 voľné (zatvor Serial Monitor), `hdiffi.exe` + `pyserial` +
`pycryptodome` v penv pythone (`pip install -r test_nrf-ota/requirements.txt`).

### Stav testu
> Implementácia + oba buildy (OTA aj stock repeater) overené ako SUCCESS.
> HW end-to-end test pripravený; výsledok sa doplní po behu na COM3/COM5.

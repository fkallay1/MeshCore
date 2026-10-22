# LoRa-FOTA pre MeshCore (nRF52840) — technický popis

Detailný technický popis FOTA-over-LoRa systému pre MeshCore repeater na nRF52840.
Určené pre údržbu a budúci vývoj. Užívateľský/prehľadový popis je v
[fcl_readme_nrf-fota.md](fcl_readme_nrf-fota.md).

> **TL;DR:** Aktualizácia firmvéru repeatera **malým delta-patchom cez LoRa** (nie celý FW,
> nie BLE-DFU). Patch sa prijme cez šifrovaný GRP_DATA kanál, uloží do dedikovaného
> CustomLFS regiónu, overí (SHA256), a aplikuje **standalone in-place flasherom** bežiacim
> mimo aplikačného flashu (HPatchLite + streaming DEFLATE + NVMC). Celé jadro MeshCore
> zostáva **nezmenené** — integrácia je cez 3 existujúce virtuálne hooky.

---

## 1. Prehľad a princíp

Klasické FOTA pre embedded buď posiela celý FW (650 kB → minúty/hodiny na LoRa), alebo
využíva bootloader DFU (MeshCore má `NRF52Board::startOTAUpdate` → Nordic DFU cez **BLE**,
čo je úplne iná cesta). Tento systém posiela len **rozdiel** medzi starým a novým FW:

```
hdiffi -inplaceB old.bin new.bin patch.bin     # HPatchLite "inplaceB" formát
zlib(level=9, wbits=-9) patch  →  ~0.5 kB pre malú zmenu (z 442 kB FW)
```

Pre malú zmenu kódu je patch rádovo **stovky bajtov** namiesto stoviek kB. To je
realizovateľné cez LoRa GRP_DATA pakety (max 184 B payload) v pár chunkoch.

Tok dát:

```
PC (test_nrf-fota/fota_sender.py)                REPEATER (nRF52840, MeshCore)
  hdiffi -inplaceB old new patch                 MyMesh::onGroupDataRecv()  ← GRP_DATA (dešifr.)
  zlib(-9,wbits=-9) → staged [ZLIB|..|deflate]   buffer → loop(): fota_process()
  GRP_DATA (AES-128-ECB + HMAC) ──┐              chunky → CustomLFS append-log (/ota/recv.log)
                                  │ LoRa         COMPLETE → assemble patch.bin + SHA256 → VERIFIED
  BRIDGE (XIAO, FK_lora) ─────────┘              'ota flash' → flasher@0xEB000:
  [0xAB CD len] serial → raw LoRa TX               HPatchLite inplaceB (old=XIP, new=app flash)
                                                   streaming DEFLATE (puff_stream) + NVMC + verify
                                                   NVIC_SystemReset → boot NEW
```

---

## 2. Komponenty a súbory

Všetko nové je v `examples/simple_repeater/nrffota/` (podadresár), guardované `#ifdef WITH_LORA_FOTA`
(resp. `FOTA_FLASHER_BUILD` pre flasher). **Bez flagu sú súbory inertné → stock buildy nedotknuté.**

| Súbor | Účel |
|-------|------|
| `nrffota/FotaProtocol.h` | typy paketov (META/SIG/CHUNK/APPLY/STATUS/NACK), wire formát |
| `nrffota/FotaState.h` | stav session (recv_count, total_chunks, status, sha, err_code) |
| `nrffota/FotaFs.h` | CustomLFS mount @ 0xD4000, cesty `/ota/*` |
| `nrffota/FotaReceiver.{h,cpp}` | príjem chunkov, append-log, bitmap, assemble, SHA256 verify, NACK |
| `nrffota/FotaReceiver_signkey.cpp` | Ed25519 author pubkeys (podpis HEADERu) |
| `nrffota/FotaPatcher.{h,cpp}` | dry-run (`fota_patch_to_file`) a ostrý flash (`fota_apply`) |
| `nrffota/FotaMesh.{h,cpp}` | glue: kanál (#-konvencia), `fota_handle_command()` (status/verify/flash/...) |
| `nrffota/FotaMyMesh.cpp` | **FOTA časť triedy MyMesh** — telá metód/overridov + deferred CLI/paket/flash logika (od 2026-07-03; v MyMesh.cpp len tenké hooky) |
| `nrffota/FotaDebug.h` | `FOTA_DEBUG_PRINT/PRINTLN` makrá (printf; gated `-D FOTA_DEBUG=1`, vzor MESH_DEBUG) |
| `nrffota/FotaTexts.h` | **centrálny katalóg CLI textov** `FOTA_TXT_*` (EN default / SK cez `-D FOTA_LANG_SK=1`) + komentárový katalóg debug hlášok; viď §7.1 |
| `nrffota/FotaBuffer.{h,cpp}` | zdieľaný 512 B scratch (borrow/release) — hpatch cache + deferred CLI snapshot |
| `nrffota/FwId.{h,cpp}` | FW identity trailer (build#, image_size, sha256) v .rodata |
| `nrffota/puff_stream.{c,h}` | standalone streaming DEFLATE dekompresor (bez libc/setjmp) |
| `nrffota/hpatchlite/*` | lokálna kópia HPatchLite (inplaceB patcher) |
| `nrffota/flasher/flasher.{c,ld}` | standalone in-place flasher, ORIGIN 0xEB000 |
| `nrffota/flasher_code.h` | vygenerovaný blob flashera (4096 B, board-agnostický) — `tools/build_flasher.py` |
| `nrffota/flash_layout.h` | numerické flash adresy (zdieľané FW aj flasher) — **kanonická flash mapa** |

Integrácia do jadra repeatera (od 2026-07-03 minimalizovaná — telá presunuté do
`nrffota/FotaMyMesh.cpp`, ten sa kompiluje ako súčasť `examples/simple_repeater`):
- `examples/simple_repeater/MyMesh.h` — jeden súvislý `#ifdef WITH_LORA_FOTA` blok
  (stav `_fota_*` + deklarácie metód/overridov).
- `examples/simple_repeater/MyMesh.cpp` — **tenké hooky** (~30 riadkov diff vs upstream):
  `fotaLogRxRaw()` v `logRxRaw`, `fotaLogTxRaw()` v `logTxRaw`, `fotaHandleLoRaCli()` v `onPeerDataRecv`,
  `fotaEarlyInit()`+`fotaBegin()` v `begin`, `fotaHandleCliCommand()` v `handleCommand`,
  `fotaLoop()` v `loop`.
  - **`logTxRaw` je nový core hook** v `src/Dispatcher.{h,cpp}` (prázdny default; volaný v
    `checkSend()` hneď po `startSendRaw`, zrkadlo `logRxRaw`) — viď RAW log §7.
- `variants/promicro/platformio.ini`, `variants/sensecap_solar/platformio.ini`,
  `variants/xiao_nrf52/platformio.ini`, `variants/t1000-e/platformio.ini`,
  `variants/rak3401/platformio.ini`, `variants/rak4631/platformio.ini` — envy
  `ProMicro_repeater_fota` (v6) / `SenseCap_Solar_repeater_fota` (v7) /
  `Xiao_nrf52_repeater_fota` (v7) / `t1000e_repeater_fota` (v7, **LR1110**) /
  `RAK_3401_repeater_fota` (v6, „RAK 1W") / `RAK_4631_repeater_fota` (v6) — všetky
  `WITH_LORA_FOTA`+`FOTA_DEBUG`, `extra_scripts` s `create-uf2.py` + 3× `gen_` (jednotné;
  RAK4631 navyše dedí `fix_bsec_lib.py` z base). RAK envy majú `custom_fota_device`
  (rak3401/rak4631), lebo prvý segment PIOENV „rak" by v archíve kolidoval.
  Pozn. LR1110: `fota agc` na T1000-E vracia skrátený `FOTA_TXT_AGC_NOREG_FMT`
  (bez SX126x RX_GAIN registra) — `#ifdef USE_SX1262` v `FotaMyMesh.cpp: runFotaCli()`.

Testovacia infraštruktúra v `test_nrf-fota/`:
- `fota_sender.py` — generuje patch (hdiffi+zlib), vysiela cez bridge (mode `meshcore`/`direct`).
- `fota_test_lora_repeater.py` — end-to-end `baseline`/`run`.
- `gen_build_info.py` — pre-script: inkrementuje `build_number.txt` → `build_info.h` (`FW_BUILD_NUMBER`).
- `hdiffi.exe`, `requirements.txt`.

---

## 3. Transport — GRP_DATA kanál

FOTA pakety idú ako MeshCore `PAYLOAD_TYPE_GRP_DATA` (0x06) na **dedikovanom kanáli** určenom PSK.
Žiadna zmena jadra — využívame existujúce virtuálne hooky:

```cpp
// MyMesh::searchChannelsByHash() — keď channel_hash sedí s FOTA kanálom, vráť ho.
//   → MeshCore dešifruje payload cez Utils::MACThenDecrypt (AES-128-ECB + HMAC-SHA256, 2B MAC)
// MyMesh::onGroupDataRecv()    — dostane plaintext [ts 4B LE][fota_type 1B][...].
```

### Odvodenie kanála (musí byť bajtovo zhodné PC ↔ device)

| | zariadenie (`FotaMesh.cpp`) | `fota_sender.py` |
|---|---|---|
| psk (16 B) | `SHA256(FOTA_CHANNEL_NAME)[0:16]` — meno vrátane `#` (`"#fkotanrf"`) | `fota_channel_secret(name)` |
| `secret` (32 B) | psk doplnené `0x00` na `PUB_KEY_SIZE` | `psk.ljust(32, b'\0')` |
| AES kľúč | `secret[:16]` | `psk32[:16]` |
| HMAC kľúč | `secret[:32]` | `psk32` (32 B) |
| `hash` (`PATH_HASH_SIZE`=1) | `sha256(psk)[0]` (= `0xA4` pre `#fkotanrf`) | `hashlib.sha256(psk).digest()[0]` |

> **Kanál pozn. (od 2026-06-23, zjednotený formát v0):** kanál sa odvodzuje MeshCore
> **#-konvenciou z mena** (`FOTA_CHANNEL_NAME "#fkotanrf"`), zhodne s `meshcore_py
> set_channel` — pôvodný textový `FOTA_CHANNEL_PSK "meshcore-ota-key"` je ZRUŠENÝ.
> `fota_sender.py --psk <hex>` stále berie surové psk bajty; správnu hodnotu vráti
> `fota_channel_secret()`. Krypto sa rieši **až po prijatí** surového rámca, takže PSK
> nikdy nebol príčinou problémov s príjmom.

### Wire formát plaintextu
`[ts 4B LE][fota_type 1B][payload...]` — FOTA payload začína za 4 B timestampom
(`onGroupDataRecv` preskočí `data+4`). Typy: HEADER=META (0x10), SIG (0x13), CHUNK (0x11),
APPLY (0x12), (STATUS/NACK 0x20/0x21 = spätný kanál).

**SIG podpis (build ≥ 335):** META (102 B) je podpísaná Ed25519. SIG paket:
`[type][prot_inf][old_sha256 32][key_id][signature 64]`. Pri `key_id=0` (v0-prefix,
default) nasledujú +4 B = prefix pubkey podpisovateľa → 103 B; `verify_header_signature`
podľa prefixu hľadá v `s_authors[]` a potom cez hook `fota_acl_admin_pubkeys()` v ACL
adminoch (`MyMesh::fotaAclAdminPubkeys`, len `PERM_ACL_ADMIN`). `key_id ≥ 1` = legacy
99 B, `s_authors[key_id-1]` (staré FW). Prefix + podpis persistujú v `meta.bin`
(`hdr_signer_prefix`, `FOTA_META_MAGIC` v2). Hook: default stub (0 kandidátov) sa
kompiluje len na platforme bez glue; MeshCore silná verzia = `FotaMyMesh.cpp`,
ZephCore silná verzia = `FotaRepeaterMesh.cpp` (`s_fota_self`, port 2026-07-16 —
ACL-admin podpis funguje aj na ZephCore). Podrobne: `fcl_readme_nrf-fota.md` §4b.

### Deferred spracovanie (dôležité pre RX)
`onGroupDataRecv()` (volané z recv cesty dispatchera) **len skopíruje payload** do
`_fota_pending[]` a nastaví `_fota_pending_len`. Ťažké CustomLFS I/O (`fota_process`) sa robí až
vo `fotaLoop()` (hook v `MyMesh::loop()`) **PO** `mesh::Mesh::loop()` — t.j. po tom, čo dispatcher
re-armne rádio do RX. Dôvod: FS zápis priamo v recv callbacku oneskoroval re-arm rádia.

---

## 4. Krypto / FS — znovupoužité z MeshCore

- **SHA256**: `mesh::Utils::sha256` + `rweather/Crypto` (existujúca dep, žiadna nová).
- **AES-128-ECB + HMAC-SHA256**: `mesh::Utils::MACThenDecrypt` (rovnaké ako bežné MeshCore pakety).
- **FS**: `CustomLFS` (oltaco, existujúca dep) — dedikovaný región @ 0xD4000, **oddelený** od
  MeshCore InternalFS (identity/prefs/ACL).

### 4.1 CustomLFS — ako zapisuje a na čo si dať pozor
`CustomLFS` (lib `oltaco/CustomLFS#0.2.2` z `[nrf52_base]` lib_deps, aktivácia `-D EXTRAFS=1`) je
wrapper okolo `Adafruit_LittleFS` s konfigurovateľným regiónom (`CustomLFS(flash_addr, size, block_size)`),
rovnaký engine ako InternalFS. `CustomLFS.h` sám ťahá `using namespace Adafruit_LittleFS_Namespace`.
- **littlefs v1** (`read_size=prog_size=block_size=128`), engine je COW/log-structured: nič sa
  neprepisuje na mieste, zmena bloku = nový blok + atomický commit metadát → power-loss safe.
- **Zápis do flash nie je okamžitý:** file cache 128 B + `flash_nrf5x` 4 KB page-cache v RAM;
  reálny NVMC erase+program až pri flushi stránky / `close()` (= sync). `_flash_erase` nerobí
  page-erase, zapisuje 0xFF do page-cache. `FILE_O_WRITE` = RDWR|CREAT + seek na koniec (append).
- **Random zápis do súboru je drahý** (COW bloku + prepis CTZ reťazca) → FOTA stavia na append-logu
  `recv.log`, nie na zápise chunkov na offset.
- **Pri zapnutom SoftDevice** ide `flash_nrf5x` cez SD flash API (`sd_flash_write`, async okolo
  rádia) → LittleFS zápisy fungujú počas príjmu. Raw NVMC so zapnutým SD nejde → flasher robí
  `sd_disable` a má vlastné `nvmc_*`.
- **Nikdy 2 inštancie LittleFS nad tým istým regiónom** — stav (alokátor, cache, metadáta) je
  per-inštancia → druhá má stale pohľad → alokuje tie isté bloky → korupcia. Ani jedna inštancia
  nie je reentrantná (multi-task prístup = mutex).

### 4.2 RAM layout nRF52840 (Adafruit core + FreeRTOS)
```
0x20000000  SoftDevice static RAM (24 kB; RAM ORIGIN = 0x20006000)
0x20006000  .data → .bss → .heap (rastie nahor)
0x2003F800  MSP stack 2 kB — len ISR + SoftDevice
0x20040000  __StackTop
```
`setup()/loop()` aj FOTA bežia ako **FreeRTOS task so stackom malloc-nutým z heapu** — loop-task
stack = `LOOP_STACK_SZ = 256*4` slov = **4 KB** (xTaskCreate hĺbka je v slovách). `const`
globály a literály sú v `.rodata` (flash, bez RAM). Lokálne pole `uint8_t buf[4096]` pretečie
loop-stack; Cortex-M4 nemá HW stack-limit (MSPLIM až M33) → tichá korupcia susednej RAM, žiadny
trap (FreeRTOS to chytí len watermarkom pri context-switchi). Pravidlo: veľké buffery `static`,
`malloc` alebo globál — nikdy na stack (`s_log_data_offset[1024]` je static, patch buffer malloc).
Dôsledok pre deštruktívny assemble do 0x20000000 (neimplementovaný Tier-2): podrezal by vlastný
task-stack v heape → musel by bežať s vlastným vysokým SP v flasher kontexte. Viď §8.9.

### 4.3 Veľkosť bežiaceho obrazu a FwId trailer (linker symboly)
Veľkosť FW image za behu bez zmeny ld scriptu: `nrf52_common.ld` exportuje `__etext`,
`__data_start__`, `__data_end__`, `__flash_arduino_start` (= ORIGIN(FLASH), 0x26000 v6 / 0x27000 v7),
`__flash_arduino_end` (0xD4000). Koniec flash image = `__etext + SIZEOF(.data)`;
`fw_image_size() = (__etext + (__data_end__ − __data_start__)) − __flash_arduino_start`.
Overené: == `firmware.bin` z DFU zipu == PIO flash report (444664 B, 2026-06-18) == `old_fw_size`
od sendera → lacná brána pred SHA256 pri base-FW kontrole (CHUNK aj HEADER). Linker symboly nie sú
compile-time konštanty (relokácia) — vždy brať `&symbol`, nedajú sa do `static const`/`#define`/
array-size. **Base FW SHA počítať tiež od linker symbolu**, nie od makra `APP_FLASH_START` — na v7
(SenseCap, 0x27000) makro 0x26000 dalo hash z inej oblasti a patch sa zamietal (2026-06-25).

`FwIdTrailer` (`nrffota/FwId.{h,cpp}`): magic `FKFWID01`, image_size, build_number, sha256 (48 B)
v `.rodata.fwid`; `__attribute__((used))` **nestačí** proti `--gc-sections` (GCC 7.2.1 nemá
`retain`) → `-Wl,-u,fw_id_trailer` v build_flags, inak magic v hexe nie je. `gen_fw_trailer.py`
(POST-build, čistý Python bez intelhex) nájde magic, vyplní `image_size` a `sha256`
(= SHA256 obrazu so sha-poľom = 0) a re-emituje hex **pred** PackageDfu (genpkg počíta CRC16 z binu).
Čítanie traileru pri boote / self-integrity check je uvažované, neimplementované.

---

## 5. Flash mapa (extrafs.ld — app končí 0xD4000)

**Kanonický zdroj adries: [`nrffota/flash_layout.h`](../examples/simple_repeater/nrffota/flash_layout.h)**
(jediné miesto, kde sa mapa udržiava — README modulu aj tento doc naň len odkazujú).
V skratke: app 0x26000(v6)/0x27000(v7)–0xD4000 · FOTA FS 92 kB @ 0xD4000 · flasher 4 kB
@ 0xEB000 · flasher trace @ 0xEC000 · MeshCore InternalFS @ 0xED000 **NEDOTKNUTÝ** ·
bootloader @ 0xF4000.

- Flasher na **0xEB000** (nie 0xF2000 ako pôvodný FK_lora sniffer) — aby sa vyhol MeshCore
  InternalFS na 0xED000.
- Strop patchu ~40 kB (recv.log + patch.bin musia byť súčasne v 92 kB FS).
- Envy `ProMicro_repeater_fota`, `RAK_3401_repeater_fota` a `RAK_4631_repeater_fota`
  používajú `boards/nrf52840_s140_v6_extrafs.ld` (712704 B); `SenseCap_Solar_repeater_fota`,
  `Xiao_nrf52_repeater_fota` a `t1000e_repeater_fota` používajú `..._v7_extrafs.ld` (708608 B).
- Pre XIAO / s140 v7 (app base 0x27000): nič netreba — board-agnostické (base z linker symbolu, runtime do flashera). Jeden `flasher_code.h`.

---

## 6. Flasher (in-place patch, bezpečnostné poistky)

`fota_apply()` (`FotaPatcher.cpp`), beží v aplikácii pred skokom na flasher:
1. **Overí base FW** = `old` z patchu (SHA256 bežiaceho app flashu vs `old_sha256` v BEGIN).
   Ak nesedí → **NEPÍŠE** ("BASE NESEDÍ — NEPREPISUJEM"). Zabráni prepisu nekompatibilným patchom.
2. Načíta patch (komprimovaný staged formát) do RAM, vypíše `[FLASHER] Komprimovany format...`.
3. `sd_softdevice_disable()`, skopíruje flasher blob do 0xEB000, skočí naň (`[BYE]`).

Flasher (`flasher/flasher.c`, standalone ARM Thumb2, beží z 0xEB000 mimo app flash):
4. HPatchLite **inplaceB streaming**: old = XIP flash @ app start, patch = raw DEFLATE z RAM cez
   `puff_stream`, new = app flash **in-place**. NVMC zápis po stránkach.
5. Po zápise prečíta flash späť (FNV-1a checksum); pri nezhode skok do DFU (nie boot pokazeného FW).
6. `NVIC_SystemReset()` → boot nového FW.

Komprimovaný staged formát patchu: `[magic 'ZLIB' 4B][uncomp_size 4B][new_fw_size 4B][raw DEFLATE]`.
`puff_stream` dekomprimuje on-the-fly počas patchovania (512 B okno, wbits=-9), takže do RAM sa
nezmestí ani celý patch ani celý FW.

---

## 7. Build a ovládanie

```bash
# 1) flasher blob (raz / po zmene flasher.c)
python examples/simple_repeater/nrffota/tools/build_flasher.py   # → nrffota/flasher_code.h

# 2) FOTA repeater
pio run -e ProMicro_repeater_fota
```

Príkazy (serial CLI alebo LoRa admin CLI cez `handleCommand`):
```
fota status | verify | flash | clear | decompress | nack | miss | missall | dbg | agc | id
```
(legacy prefix `ota` stále funguje — alias `FOTA-CLI-ALIAS` vo FotaMyMesh.cpp, používa ho
Flutter appka; po jej migrácii na `fota` sa alias zmaže.)
- `fota verify` = dry-run (aplikuje patch v RAM → SHA256, **nič nezapíše**).
- `fota flash`  = **OSTRÝ** flash + reboot (pri úspechu sa nevráti).
- `fota miss`   = chýbajúce chunky ako rozsahy „od-do" (napr. `H S 2 4-11 28 32-34 +N`),
  strop `FOTA_MISS_OUTTOKENS` tokenov (číslo=1, rozsah=2; H/S sa nerátajú a vypíšu sa vždy).
- `fota missall`= ako `miss`, ale **všetky** chýbajúce (capnuté len dĺžkou LoRa paketu).
- `fota dbg`    = flasher debug (GPREGRET2/RESETREAS + trace z 0xEC000).
- `fota agc`    = **read-only** AGC/gain diagnostika (viď §8.3).
- `fota clear`  = zmaže FOTA session (`/ota/*`).

Diagnostické Serial výpisy (`[FOTA] …`, `[FLASHER-DBG] …`, heartbeat `AALIVE`) idú cez
makrá `FOTA_DEBUG_PRINT/PRINTLN` (`nrffota/FotaDebug.h`) gated **`-D FOTA_DEBUG=1`**
(FOTA envy default zapnuté). Bez flagu sa vôbec nekompilujú; CLI odpovede (reply buffer)
fungujú vždy — sú to funkčné výstupy, nie diagnostika.

Pod **`-D FK_DEBUG=1`** navyše (od b3523fe2, build ≥ 352) core vypisuje **tiché drop
brány GRP_DATA** (`src/Mesh.cpp`, `src/Dispatcher.cpp`): `[FK] GRP_DATA xx: DEDUP (seen)`
(seen-table — RAW paket ukáže, spracovanie nebeží), `[FK] GRP_DATA xx: MAC fail
(foreign/corrupt)` / `no matching channel`, a `[FK] RX DROP: packet pool empty!`
(Dispatcher bez voľného paketu — dovtedy viditeľné len pod MESH_DEBUG). Len GRP_DATA —
GRP_TXT kópie chatov by log spamovali. Dôvod vzniku: flood chunky na 200 km teste
v RAW logu bez `[FOTA] CHUNK` spracovania (viď `fcl_remote_e2e_rpi.md` §2.2).

Od buildu **#355** FK_DEBUG dekóduje aj **PATH pakety** (`src/Mesh.cpp`) — RAW log
ukazuje len šifrovaný obal, tieto výpisy pomenúvajú obsah a drop brány (diagnóza
„PATH odišiel, ale nedošiel" na oboch smeroch):

- `[FK] PATH RX src=XX ret_path[N]=AABBCC extra_t=.. extra_len=.. flood(->reciprocal)|direct`
  — úspešne dešifrovaný PATH pre nás: aká spiatočná cesta nám bola oznámená; pri
  flood variante repeater následne pošle recipročný PATH (direct).
- `[FK] PATH RX XX->YY: unknown peer | MAC fail (corrupt?)` — PATH adresovaný nám,
  ale odosielateľ nie je v ACL / dešifrovanie zlyhalo (poškodenie pri nízkom SNR).
- `[FK] PATH RX XX->YY: DEDUP (seen)` — PATH pre nás zahodený seen-table dedupom
  (napr. neskoršia kópia po viacerých cestách).
- `[FK] PATH TX to=XX ret_path[N]=AABBCC extra_t=.. extra_len=..` — obsah KAŽDÉHO
  odchádzajúceho PATH (jediné hrdlo `createPathReturn`; extra_t=255 = bez extra,
  len anti-dedup blob). Páruj s nasledujúcim `TX RAW type=8` riadkom.

FK_DEBUG ďalej dekóduje aj **LOGIN/ANON_REQ handshake** (`src/Mesh.cpp` ANON_REQ
vetva + `examples/simple_repeater/MyMesh.cpp` `handleLoginReq`/`fkAnonFallback*`)
— vidno OBE strany handshaku vrátane fallbacku z §7.0b:

- `[FK] ANON_REQ XX->YY: MAC fail (corrupt?)` / `DEDUP (seen)` — dorazil, ale
  dešifrovanie zlyhalo / neskoršia kópia floodu zahodená.
- `[FK] LOGIN src=XX: invalid password` / `replay ts=.. last=..` — zlé heslo /
  replay attack (ts ≤ posledný známy).
- `[FK] LOGIN src=XX: accepted flood|direct, out_path=known|unknown`.
- `[FK] LOGIN fallback src=XX: not armed (hops=.. bytes=..)` / `reply too long (..)`
  — zero-hop (niet čo získať) alebo odpoveď nezmestná do fallback bufferu.
- `[FK] LOGIN fallback src=XX: armed hops=.. fire_in=.. ms` — fallback naparkovaný.
- `[FK] LOGIN fallback src=XX: cancelled (ACL entry missing | reciprocal PATH received)`
  — buď klienta medzitým vytlačilo z ACL, alebo recipročný PATH prišiel (flood OK).
- `[FK] LOGIN fallback src=XX: DIRECT resend hops=.. route=AABBCC` — recipročný
  PATH neprišiel včas, odpoveď sa poslala direct po otočenej ceste.

### 7.0b FK fork flagy pre login handshake v rušnom meshi (2026-07-17, 0d9192db)

Problém (200 km trasa): uplink floody dolietajú, ale **flood odpoveď repeatera
(login PATH+RESPONSE) takmer nikdy neprežije cestu späť** — štartuje 300 ms po
requeste rovno do jeho echo búrky a na ďalších hopoch ju dorazí pozadová
prevádzka. Manuálne cesty (`fota setpath`) fungujú, štandardný handshake nie.
Dva voliteľné flagy v `examples/simple_repeater/MyMesh.{h,cpp}` (bez flagu sa
kód nekompiluje, správanie = upstream):

- **`-D FK_SERVER_FLOOD_RESPONSE_DELAY=<ms>`** — samostatný delay LEN pre flood
  odpovede (login PATH, RESPONSE/REQ flood fallbacky); odpoveď počká, kým echo
  búrka requestu utíchne. Direct odpovede ostávajú na 300 ms.
- **`-D FK_ANON_FLOOD_DIRECT_FALLBACK=<ms>`** — ak do okna po flood odpovedi
  nepríde recipročný PATH (klientova `out_path` v ACL ostala UNKNOWN), login
  odpoveď sa pošle znova **DIRECT po otočenej ceste requestu** a otočená cesta
  sa zapíše ako provizórna out_path. Direct kópia má čerstvý random blob
  (`reply[8..11]`) → iný packet hash, dedup ju nezahodí. 2 pending sloty,
  kontrola vo `fkAnonFallbackLoop()` z `MyMesh::loop()`. Worst case = upstream.

Zapnuté v `t1000e_repeater_fota` (1500/2000 ms). Pozn.: klient↔klient handshake
(TXT/ACK) má rovnakú zraniteľnosť, ale beží medzi telefónmi — tam to neovplyvníme.
ZephCore mirror (RepeaterMesh.cpp) zatiaľ NEportnutý — až po HW validácii.

### 7.1 Texty — FotaTexts.h (od buildu 324)

Správa textov je rozdelená do troch tried s rôznym režimom:

1. **CLI odpovede (reply buffer)** — centralizované v `nrffota/FotaTexts.h` ako
   `FOTA_TXT_*` makrá; pri každom makre je komentár, kde sa používa (súbor/funkcia).
   - **Jazyk:** default **angličtina**; `-D FOTA_LANG_SK=1` (nastavené vo všetkých
     3 FOTA envoch) prepne na slovenčinu. Upstream/EN build = jednoducho bez flagu.
   - **Strojovo parsované formáty** (`FOTA %u/%u st=…`, `miss=`, `id b#…`, výpisy
     ciest, `FOTA flash accepted`…) sú jazykovo **neutrálne** — definované len raz,
     NElokalizovať (parsuje ich Flutter appka a `test_nrf-fota` skripty).
   - Pravidlo: statická hláška = `const char*` na makro (bez kopírovania);
     formátovaná = formátovacie makro + `snprintf` priamo do reply.
2. **Chybové dôvody helperov** — `fota_parse_path_arg` / `fota_client_by_prefix`
   vracajú `const char**` smerník na literál z FotaTexts.h (žiadne `strcpy` do
   lokálnych bufferov, žiadny `char err[48]` na stacku). Dry-run dôvody
   (`fota_patch_to_file`) idú cez `set_err` do 48 B buffera volajúceho — texty
   `FOTA_TXT_VFY_*` držať krátke.
3. **Debug hlášky (`FOTA_DEBUG_*`)** — ostávajú **inline po anglicky** na mieste
   volania (nelokalizujú sa, bez flagu sa nekompilujú). Referenčný katalóg
   EN↔SK + miesta použitia je ako komentár na konci `FotaTexts.h`.

POZOR pri zmene textov: `"FNV-1a of output"` (FLASHER-TRACE) a `"build #"`
parsuje `fota_test_lora_repeater.py` (regex akceptuje aj staré SK `výstupu`).

**Serial CLI a preklepy (build ≥ 318):** reader v `main.cpp` bufferuje každý bajt, takže
backspace/šípky by normálne skončili ako „unknown command". `fotaHandleCliCommand`
(FotaMyMesh.cpp, `fota_scrub_cli_line`) preto buffer pred parsovaním vyčistí in-place:
aplikuje backspace/DEL, odstráni ANSI sekvencie šípok (CSI/SS3) a zahodí riadiace znaky —
a keďže čistí aj keď vráti `false`, opravené preklepy fungujú aj pre common CLI príkazy.
Nepokryté ostávajú len `setperm` / `get acl` / `discover.neighbors` (matchujú sa v
`MyMesh::handleCommand` pred naším hookom) a `load` mód regiónov. Šípky kurzor neposúvajú,
len sa neutralizujú.

---

### 7.2 `fota flash` cez LoRa — ACK pred rebootom (build ≥ 192)
`fota flash` sa už nespúšťa priamo vo `fota_handle_command` (`fota_apply` pri úspechu nevráti →
ACK by nikdy neodišiel). Ak je session VERIFIED → reply `FOTA flash accepted` + `fota_request_apply()`;
skutočný `fota_apply()` spustí `MyMesh::loop()` až keď `_mgr->getOutboundTotal()==0` (ACK reálne
odišiel) alebo po 6 s safety-net (`_fota_apply_deadline`). Bez VERIFIED → „FOTA flash: nie je
VERIFIED". `fota miss`/`missall` sémantika (tokeny, rozsahy, H/S, `+N`, `N-??`) je v
[fcl_readme_nrf-fota.md](fcl_readme_nrf-fota.md) §4; strop `FOTA_MISS_OUTTOKENS 20` (override `-D`),
`missall` = limit 0. `chunk_idx` je `uint16_t`; `total_chunks` sa v chunku zámerne neposiela
(odvodí sa z podpísaného `patch_size`, nastaví sa až po overení podpisu).

---

## 8. Vyriešené problémy (technický rozbor pre budúcnosť)

### 8.1 Build / integrácia
- **`macro names must be identifiers`** — `-U LORA_FREQ` v `build_flags` sa SCons rozbil na holé
  prázdne `-U`. Fix: nepoužívať `-U`, len `-D LORA_FREQ=...` (GCC: posledná definícia vyhráva,
  redefinition warning potlačený `-w`).
- **Inertné súbory bez flagu** — všetky `nrffota/*` guardované `#ifdef WITH_LORA_FOTA`;
  `flasher.c` guard `FOTA_FLASHER_BUILD`; `puff_stream.c`/`hpatch_lite.c` guard
  `WITH_LORA_FOTA || FOTA_FLASHER_BUILD`. Rekurzívny `build_src_filter` ich zoberie, ale
  `--gc-sections` ich v stock builde zahodí.
- **Flash mapa** — flasher presunutý z 0xF2000 (sniffer) na 0xEB000, aby sa vyhol MeshCore
  InternalFS (0xED000).

### 8.1b Zápis na 0x40000514 mrazil T1000-E pri pripojení USB (2026-07-16, fix 8863ea76)
- **Symptóm:** FOTA build na T1000-E (batéria) úplne zamrzol (aj LoRa) pri pripojení USB
  za behu; na PC to maskoval otvorený terminál len zhodou okolností nie — spúšťačom bol
  USBDETECTED event. Stock build OK. ProMicro/Solar bez symptómu — board bez batérie
  „USB plug za behu" nikdy nezažije (bootuje vždy s VBUS), bug bol latentný na všetkých.
- **Príčina:** `fota_check_flasher_debug()` čítal+mazal „GPREGRET2" na **0x40000514** —
  tá adresa ale NIE JE GPREGRET2 (skutočný = 0x40000520); je to REZERVOVANÝ priestor
  POWER periférie vedľa POFCON (0x510). Zápis rozbil stav POWER periférie → zamrznutie
  pri najbližšom POWER evente (USB plug).
- **Fix:** register prístup odstránený úplne — MC flasher marker do registra aj tak už
  nezapisuje (`fmark`=no-op, kroky nesie flash trace log; ZC má RAM breadcrumb).
  RESETREAS (0x400, správna adresa) ostáva. + `fota_print_flasher_trace()` sanity check:
  stale/cudzí trace región (čerstvá doska, ne-MeshCore FW — napr. po ZephCore) už
  nevypíše 512 riadkov smetí, zastaví sa na prvom nevalidnom kóde.
- **Diagnostika:** HW bisect D1–D6 (postupné vypínanie: debug flagy → boot hooky →
  FS mount → register hook); D5/D6 pár izoloval register hook ako jedinú premennú.

### 8.2 Príjem / patchovanie chunkov
- **Meshcore chunk CRC vs AES padding** — AES-ECB doplní 159→160 B; bez strhnutia paddingu
  CRC nesedel. Fix v `handle_chunk`: presná dĺžka chunku z `idx`/`patch_size` + clamp.
  (V logu sa stále môže objaviť "CRC BAD", ale finálny `patch.bin SHA256 OK` potvrdzuje dáta.)
- **Reboot-resilient session** — bitmap sa ukladá každých 8 chunkov / pri complete; CustomLFS
  session prežije aj DFU reflash app flashu (overené).
- **`fota_apply` stav pri zlyhaní** — pri base-mismatch obnoví predošlý status (neostane APPLYING).
- **Out-of-order session (od 04a95a8d, 2026-06-18)** — chunky pred HEADER sa bufferujú do partial
  session (`total_chunks=0`), HEADER ich „promuje" (zachová log + bitmap, `recv_count` z
  `bitmap_popcount`). Lekcia: HEADER nenesie `old_fw_size` → `handle_chunk` ho musí doplniť z chunku,
  inak guard zahodí všetky chunky po HEADER (0/4, odhalené e2e). Posledný chunk sa pred HEADER
  neuloží (CRC bez známej presnej dĺžky), príde v ďalšom kole. Sender `--packetorder
  {normal,hbegin,hmiddle,hend}` určuje pozíciu HEADER; `hend` (chunky prvé) pomáha pri slabom RF,
  lebo HEADER/META prežíva reboot, chunky < 8 nie.
- **recv.log → patch (RAM-assembly, 5d4f23df)** — `/ota/recv.log` je append-žurnál
  `[idx 2B][len 2B][data]` (reboot-odolný, znesie duplicity, dedup cez bitmap). Pri COMPLETE
  `verify_log_sha()` streamuje SHA256 z logu; patch sa pri dry-rune aj flashi zostaví
  `fota_acquire_patch_ram()` (malloc, offset tabuľka sa prebuduje pri každom acquire, caller `free()`).
  `-D USE_PATCHBIN_FILE` = stará cesta cez `patch.bin` (+ zmaže recv.log). Deštruktívny assemble
  do 0x20000000 (Tier-2) nie je implementovaný (§4.2).
- **`ota verify` (dry-run) PRED `ota flash` → heap hardfault** (historicky pred `5d4f23df`) —
  `fota_patch_to_file` robil `malloc` + streaming rekonštrukciu celého ~442 kB FW; následný
  `fota_apply` hardfaultol na `malloc(patch_size)` (flasher sa zastavil po `[FLASHER] Komprimovany
  format`, repeater nabehol na OLD). **Stav po `5d4f23df` (RAM-assembly):** obe cesty idú cez
  `fota_acquire_patch_ram` s korektným `free()`; commit deklaruje „dry-run pred flashom bez
  hardfaultu". **Standalone `ota verify` overený OK (2026-06-21)** — beží opakovane, bez hardfaultu,
  rekonštruuje bit-presne. ⚠️ **Sekvencia verify→flash v JEDNOM boote NEBOLA v 2026-06-21 session
  priamo retestovaná** (medzi verify a flash sa rebootovalo). Preto `--verify-first` v teste ostáva
  **default vyp** ako poistka; ak treba dry-run v teste, najprv over verify→flash same-boot na HW.

### 8.3 Príjem na rádiu — DÔLEŽITÝ rozbor (2026-06)
Symptóm: repeater po čase **prestal prijímať čokoľvek** (`rawrx=0`), dlhodobo hluchý.

**Postup diagnostiky:**
1. Pridaný `logRxRaw()` override → počítadlo `rawrx` (surové CRC-OK rámce **PRED** dekódom)
   v `[FOTA] AALIVE` heartbeate. Odlíši "rádio nepočuje nič" (RF/PHY) od "počuje, dekód zlyhá".
   - **2026-07-04:** pribudol symetrický **TX** log. Výpis `[FOTA] RAW` premenovaný na
     **`[FOTA] RX RAW`**; nový **`[FOTA] TX RAW`** loguje každý ODOSLANÝ rámec (vlastné
     adverty/ACK aj preposlané) cez nový core hook `logTxRaw` (Dispatcher `checkSend` →
     `fotaLogTxRaw`). Zdieľané telo `fota_log_raw_line(dir,…)` (rovnaký formát type/route/path,
     TX bez rssi/snr). Nové počítadlo `rawtx` v heartbeate (`rawrx=… rawtx=…`). Vše za `FOTA_DEBUG`.
   - **Filter cesty `FK_DEBUG_MAXPATH`** (2026-07-12, predtým napevno `path_count>4`):
     vypíše sa len rámec s `path_count <= FK_DEBUG_MAXPATH`; **TX má limit +1**, aby forward
     vypísaného RX (path narastie o náš hash) bol v logu tiež. Nedefinované → default 64
     (nad max 63 hopov) = **bez filtra**. Zapnutie per env: `-D FK_DEBUG_MAXPATH=4`.
     POZOR: počítadlá `rawrx`/`rawtx` (čísla `#N` v riadkoch) sa inkrementujú aj pre
     odfiltrované rámce — diery v číslovaní = potlačené výpisy, nie strata paketov.
2. `nf=-120` v hluchom stave = **clampnutá dolná hranica** noise floor
   ([RadioLibWrappers.cpp:97](../src/helpers/radiolib/RadioLibWrappers.cpp#L97)) → rádio JE v RX a
   vzorkuje, kanál tichý.
3. **Kontrolný test s pôvodným FK_lora snifferom** na tých istých doskách/anténe/bridge:
   `fota_test_lora.py` PREŠIEL (#115→#116) → **HW v poriadku** (RSSI -23, SNR +11).
4. **`ota agc`** (read-only): číta SX1262 register RxGain 0x08AC (0x96 boosted / 0x94 power-save),
   okamžité RSSI (`getRSSI(false)`), noise floor, `agc_reset_interval`. Prístup k surovému
   RadioLib objektu cez `extern RADIO_CLASS radio;` v `MyMesh.cpp`.

**Záver (po dôkladnom teste — pozor na predčasné závery!):**
- **NIE je to preamble.** Pri `agc_reset=0` funguje príjem na **štandardných preamble 32**
  (MeshCore `preambleLengthForSF`: SF≤8→32). Skoršia domnienka, že bridge TX preamble 32==RX 32
  je "hraničné" a treba 64, bola **nesprávna** — preamble 64 "fungoval" len náhodou, lebo reflash
  medzitým resetol zaseknuté rádio. **Repeater radio config sme NEMENILI → plná kompatibilita
  s inými MeshCore zariadeniami.**
- **Trvalá hluchota = jednorazový zaseknutý receiver** ("stuck noise floor", self-reinforcing,
  [RadioLibWrappers.cpp:78](../src/helpers/radiolib/RadioLibWrappers.cpp#L78)). MeshCore má naň
  recovery `resetAGC()` (sleep+calibrate, `SX126xReset.h`), ale volá sa len periodicky cez
  `getAGCResetInterval()`, ktorý je **default 0 (vypnuté)**. Zaseknutý stav vyčistí
  power-cycle / DFU reflash.

### 8.4 AGC auto-reset ROZBÍJA FOTA flash (kritická interakcia)
`set agc.reset.interval N` (N>0) zapne periodický `resetAGC()` (`radio.sleep(true)` + `calibrate`).
**Nekombinovať s FOTA flashom!** Ak agc resety bežia počas FOTA session, nasledujúci `ota flash`
zlyhá — flasher sa zastaví hneď po `[FLASHER] Komprimovany format`, repeater nabehne na OLD FW.

Empiricky overené:
| agc_reset | flash výsledok |
|-----------|----------------|
| 0 | #28→#29 PASS, #32→#33 PASS |
| 8 s | #28→#29 FAIL, #30→#31 FAIL |

Guard "vypni agc pri `ota flash` príkaze" **NEFUNGOVAL** (škoda vzniká agc resetmi počas session,
nie pri samotnom `fota_apply`, ktorý je blokujúci) → odstránený, nahradený varovným komentárom v
`MyMesh.cpp` ota vetve. **Odporúčanie: `agc_reset_interval = 0` (MeshCore default).** Príjem to
nepotrebuje; zaseknutý receiver (zriedkavý) sa rieši rebootom.

> Mechanizmus (hypotéza): `sleep+calibrate` počas session zanechá rádio/SoftDevice v stave, ktorý
> rozbije prípravu flashu (`sd_softdevice_disable` + NVMC). Detail neoverený — vyriešené tým, že
> agc reset ostáva vypnutý.

### 8.5 Bridge (FK_lora gateway_fw) — RF spoj
- **Preamble**: bridge musí TX-ovať preamble zhodnú s MeshCore RX = 32 pre SF≤8
  (`(LORA_SF<=8)?32:16`). Pôvodne mal 16 → obojstranná hluchota (oprava z prv. session).
- **TX výkon**: 22 dBm (10 dBm bol pre marginálny spoj málo).
- **RXEN**: bridge drží `RXEN HIGH` natrvalo + `setDio2AsRfSwitch(true)` (rovnako ako FK_lora
  sniffer, ktorý prijíma OK). Skúšaný `setRfSwitchPins()` (ako MeshCore) — **nebol potrebný**,
  vrátené späť.
- **LIVE heartbeat** (každé 3 s, ASCII `[LIVE] preset=... tx=N rx=M`) — bezpečné voči binárnemu
  frame-parseru PC (`[0xCC 0xDD]` sa v ASCII nevyskytne).

### 8.6 Flasher hang pri `ota flash` — IRQ počas NVMC okna (VYRIEŠENÉ 2026-06-21)
Symptóm: po `ota flash` sa flasher zastavil hneď po `[FLASHER] Patch v RAM`, zariadenie
zamrzlo (USB enumerované ako app PID, ale 0 bajtov serial, 1200-touch nezabral → nutný
fyzický reset), nový FW nenabehol. Reprodukovateľné v session s množstvom rebootov/RX
pred flashom; `#90→#91` prešiel (rádio idle).

**Diagnostika:** `ota verify` (dry-run, app-side puff+hpatchi z recv.log, BEZ zápisu)
zrekonštruoval patch **bit-presne** (SHA == cieľový FW) → príjem, assembly, dekompresia aj
HPatchLite sú správne; chyba je **výlučne vo flash-write ceste**. NIE regres z RAM-assembly
ani z `fw_image_size` linker symbolu (oboje overené správne). Pozn.: `ota dbg` trace je
**vždy „prázdny" + GPREGRET2=0x1 aj pri ÚSPECHU** (`fmark`=no-op, ftrace nečitateľný cez app)
→ tieto diagnostiky sú NEinformatívne; reálny signál = „nabehne nový build #".

**Root cause:** `fota_flash_via_flasher()` volal `sd_softdevice_disable()` a hneď
`ensure_flasher_written()` (NVMC zápis flasher blobu) **s povolenými prerušeniami a rádiom
armnutým v RX**. Flasher si robí `cpsid i` až PO skoku. Keď počas NVMC okna prišlo rádio
DIO1 / SysTick prerušenie → skok cez VTOR do app handlera (SD už disabled, FS odmountovaný)
→ fault/hang.

**Fix:** po `sd_softdevice_disable()` pridané `__disable_irq()` PRED `ensure_flasher_written()`
([FotaPatcher.cpp](../examples/simple_repeater/nrffota/FotaPatcher.cpp), commit `d24c6792`).
`sd_disable` ostáva s IRQ povolenými (SVC sa dokončí); chránime kritické NVMC okno + skok.
**Overené na HW: 2 čisté flash cykly (#101→#102, #102→#103, neskôr #104→#105).** Toto je
pravdepodobne aj príčina §8.4 (agc sleep+calibrate = rádio v zlom stave → DIO ISR rozbije flash).

### 8.7 CAD / LBT a FOTA — držať vypnuté (default)
MeshCore má pred TX „listen-before-talk" gate v `Dispatcher::checkSend()` cez
`_radio->isReceiving()`. Tá vetví na dve úrovne v `RadioLibWrapper::isChannelActive()`
([RadioLibWrappers.cpp:207](../src/helpers/radiolib/RadioLibWrappers.cpp#L207)):
1. **RSSI prah** voči noise floor (`_threshold`) — lacné, rádio **neopúšťa RX**.
2. **Hardvérové CAD** (`_cad_enabled`) — synchrónne `_radio->scanChannel()` (na SX1262
   blokujúce CAD: RX→CAD→RX prepnutie + čakanie na CAD-done DIO). Po scane si vetva sama
   čistí CAD-done IRQ a re-armuje RX (`state = STATE_IDLE; startRecv()`).

**`setCADEnabled()` NIE je RadioLib API** — je to MeshCore `mesh::Radio` virtuál, len uloží
bool (`RadioLibWrappers.h:54`). Default `getCADEnabled()=false` (`Dispatcher.h:171`); u repeatera
runtime pref `_prefs.cad_enabled = 0` (`MyMesh.cpp:1103`, prepínateľné `set cad on`).

**Pre FOTA: nechaj CAD vypnuté (default).** Kód sám je korektný (upratanie IRQ + re-arm je
nutné), ale CAD beží len na **TX ceste** (re-flood chunku, STATUS/NACK, CLI reply) a zapnuté
pridáva RX→CAD→RX mode-churn. SX1262 buffruje ~1 paket; naše tiché straty boli práve
„rádio/CPU zaneprázdnené → zhltne back-to-back chunk" (§8.6, §8.8).
Ďalšie okno mimo RX = vyššie riziko straty chunku, plus blokujúci scan stalluje `loop()`, kde
robíme odložené NVMC zápisy. Kolíznu ochranu z veľkej časti dáva už RSSI-prah (vetva 1), ktorá
RX neopúšťa. Konzistentné s §8.4 (AGC reset ostáva vypnutý): drž MeshCore defaulty.
Ak by si chcel agresívnejšiu ochranu, `set cad on` je runtime (bez rebuildu) — A/B meraj
`getPacketsRecvErrors()` / RAW counter pred a po.

---

### 8.8 Strata SIG/APPLY pri re-sende — `save_meta()` flash zápis v RX ceste (VYRIEŠENÉ 2026-06-25)
Symptóm (2026-06-24, build #128, env `ProMicro_repeater_ota`): „APPLY na už-VERIFIED session sa
ignoruje". Z RAW logu: pri SF8 chýbal rámec APPLY, pri SF7 s krátkym delay chýbal SIG → session
ostala `total_chunks=0/st=0x1` a „APPLY: nie je verifikované" bolo korektné.

**Mechanizmus:** `handle_meta` aj `handle_sig` volali `save_meta()` (CustomLFS flash zápis) pri
KAŽDOM prijatí, aj na identickom re-sende. nRF52 NVMC pri erase/write zastaví CPU na desiatky ms;
RadioLib nečíta paket v prerušení (ISR len nastaví flag, čítanie je v `loop()` cez `recvRaw`),
SX1262 má RX buffer na **1 paket** a žiadny overflow IRQ ani príznak → paket, ktorý priletí počas
zápisu predošlého, sa ticho prepíše (nestúpne `rawrx` ani `rxerr`). Obeťou bol vždy paket hneď za
META (SIG) alebo za SIG (APPLY) — podľa SF a delay to vyzeralo rôzne.

**Fix (build #130+, HW overené #135):** deDUP — `handle_meta`: `dup_meta` (meta_recv && rovnaký
patch_size+sha, počítané PRED clear/prepisom) → `if(!dup_meta) save_meta()`; `handle_sig`:
`dup_sig` (sig_recv && rovnaký key_id+hdr_sig) → `if(!dup_sig) save_meta()`. Re-send
identického META/SIG už nerobí flash zápis → konverguje za ~2 dávky. `hasSeen`/dispatch sa
nemenili (sú OK). Redundancia APPLY v appke by nič neriešila.

**Diagnostika pod `FK_DEBUG`:** ISR counter v `setFlag` → AALIVE `isr=` a `miss = isr−sent−recv−recverr`
(odhad zahodených; rastie pri strate = FIFO prepis/stall, `miss=0` a paket chýba = strata na
vzduchu); `[RADIO] readData ERR (CRC_MISMATCH)` = paket dorazil poškodený (RF), nie stall.
Pri ladení pomohol SF7 log: APPLY dorazil vždy (RAW len=53), chýbal SIG.

**Vedľajší nález:** `logRxRaw` tlačil `type=raw[0]&0x0F` (zavádzajúce — route + časť typu); reálny
payload type je `(raw[0]>>2)&0x0F` (`Packet.h`). Opravené, pridaný `route=`. Pravidlo odloženého
spracovania (§3) je kritické: akýkoľvek dlhý blocking v RX ceste = riziko tichej straty.

### 8.9 `fota verify` cez LoRa CLI zhodil rádio — pretečenie 4 KB loop stacku (VYRIEŠENÉ 2026-06-27)
Symptóm (2026-06-26): `fota verify` cez **vzdialené LoRa CLI** spoľahlivo zhodil rádio, ale Serial
bežal ďalej (CPU žije, nie HardFault); cez Serial bol `fota verify` OK. Padlo aj bez patchu.

**Root cause = tichý stack overflow loop-tasku.** Loop-task stack je presne 4096 B (`LOOP_STACK_SZ`,
viď §4.2). LoRa CLI vchádza do `handleCommand` hlboko v RX ceste (`Dispatcher::loop → Mesh::onRecvPacket`
(`data[184]`+`secret[32]`) `→ onPeerDataRecv` (`temp[166]`) `→ handleCommand`) = ~600–900 B navyše
oproti Serial ceste; `fota_patch_to_file()` mal `cache[2048]` NA STACKU (pol stacku). FreeRTOS
task stacky sú z heapu → pretečenie prepísalo susedný heap = stav RadioLib/SX1262 → rádio mŕtve.
`configCHECK_FOR_STACK_OVERFLOW=1` je príliš slabé (kontrola SP len pri context-switchi).
Aj iné LoRa CLI príkazy sú na hrane (4 KB je systémovo málo).

**Fix (commit 27d5374b, 3 časti):**
1. **Všetky LoRa FOTA CLI príkazy odložené z RX hooku do `loop()`** — `fota_args_of(cmd)` detektor,
   `struct FotaCliDefer` (~178 B snapshot klienta) zaparkovaný v zdieľanom `FotaBuffer`;
   `onPeerDataRecv` → `deferFotaCli()` + okamžité „spracúvam…" (ak pending → „zaneprázdnené");
   `loop()` → `runFotaCli()` + `sendDeferredCliReply()`. Serial cesta beží inline (plytký stack).
2. **`nrffota/FotaBuffer.{h,cpp}`** — jeden static `s_fota_buf[512]` v .bss, borrow/release
   (`fota_get_buffer`/`fota_put_buffer`, guard proti dvojitej výpožičke); `fota_patch_to_file`
   používa scratch namiesto `cache[2048]` (hpatch min. cache = 2 B, 512 B stačí; flasher to už robil
   správne staticky). Reuse flasherovho buffra nejde (samostatný relokovateľný blob).
3. **Heartbeat `stackfree=<N>B/4096`** = `uxTaskGetStackHighWaterMark` → historické minimum voľného
   stacku od bootu (FreeRTOS plní stack vzorom), odhalí najhlbší bod aj z plytkého heartbeatu;
   pri reálnom pretečení sa zasekne ~0.

**HW meranie (2026-06-27, ProMicro #178):** min. voľný stack 1860 B po boote, 728 B hneď po prvom
ADVERTe = Ed25519 verify prichádzajúceho advertu (~1,1 kB, inherentné MeshCore). Odložený verify
ostáva pod týmto floorom → fix OK, rezerva 728 B. `fota verify` reply odvtedy nesie dôvod
(`set_err`, texty `FOTA_TXT_VFY_*`).

**Ako vyhodiť dočasné meranie:** stack kód je pod `#ifdef FK_DEBUG_STACKTRACE` (helper
`loop_stack_used_now`, `<task.h>`, výpisy `stk_minfree`/`nowused`); build flagy
`-D FK_DEBUG_STACKTRACE=1` a `-D INCLUDE_pxTaskGetStackStart=1` (promicro + sensecap fota env).
Vyhodiť = odstrániť flagy + guardované bloky. Toto nie je NVMC-IRQ problém z §8.6 (verify nič
nezapisuje).

### 8.10 FOTA CLI odpoveď cez LoRa sa v kľude nezobrazila — kolízia timestampu (VYRIEŠENÉ 2026-06-28)
`ota status` a iné FOTA CLI cez LoRa sa vracali oveľa menej často než `ver`/`get radio` (1 hop,
kľud). Príčina: deferovaná FOTA cesta (`sendDeferredCliReply`) nemala workaround inline cesty
(`if (timestamp == sender_timestamp) timestamp++`); idle repeater odpovedal v tej istej epoch
sekunde → `reply_ts == sender_ts` → companion/appka odpoveď zahodila ako duplikát (seen-table
podľa ts, §11). Pomalé príkazy (`verify`) prekročili sekundu, preto chodili; iná CLI aktivita
posunie `last_unique` → bez kolízie. `is_retry` gate (rovnaký ts) to zosilnil — retry neodpovie.
**Fix (3037cd90):** `sender_timestamp` ide cez snapshot `FotaCliDefer`; v `sendDeferredCliReply`
`if (timestamp <= sender_timestamp) timestamp = sender_timestamp + 1;` (pokrýva kolíziu aj zlý
čas). Overené: v kľude `ota status` chodí ako `ver` (#197 ProMicro, #198 SenseCap).
Fakty okolo času: repeater si čas cez LoRa nastaví LEN explicitným `clock sync`/`time`
(`CommonCLI.cpp`, admin-only — guest nespustí žiaden CLI); ProMicro/SenseCap nemajú HW RTC
(`VolatileRTCClock`, po reboote 15. 5. 2024; zlý čas odpoveď neskryje, len ju companion zoradí do
minulosti); MeshCore zámerne nesyncuje čas z ľubovoľného paketu (replay ochrana). Dva CLI príkazy
tesne za sebou: prehodený starší padne na replay guard; FOTA defer je single-slot („zaneprázdnené").
Otvorené: voliteľný auto-sync hodín z autentifikovaného FOTA admin príkazu (len dopredu).

### 8.11 Demux companion formátu — `MACThenDecrypt` vracia AES-padovanú dĺžku (2026-06-23)
Pri zjednotení formátu bridge/companion (`[FOTA_MAGIC 2B][len 1B][ts 4B][payload]`, companion
`sendGroupData` pridáva `[data_type][len]`, jadro dáva `onGroupDataRecv` celý plaintext) zlyhal
demux: `Mesh::MACThenDecrypt` vracia dĺžku zaokrúhlenú na 16 B blok, nie pravú dĺžku plaintextu.
Porovnanie `data[2] == len-3` preto zahodilo KAŽDÝ paket (symptóm: `rawrx++`, type=10, hash 0xA4,
ale `ota status` 0/0). Správne: `inner = data[2]` (companion data_len) je autoritatívna vnútorná
dĺžka → `data += 3; len = inner`. MAC používa 32 B secret (`[16 derived][16 zero]`) — companion
`CMD_SET_CHANNEL` aj receiver `fota_build_channel` zerujú `[16:32]` zhodne. `CMD_SEND_RAW_PACKET`
(65) sa nepoužíva — volá `sendPacket`, nie `sendFloodScoped` (flood/region cez repeatre nefunguje).

---

## 9. Známe obmedzenia / TODO
- **Fire-and-forget príjem** niekedy potrebuje viac broadcast cyklov (strata paketov na začiatku).
  Automatický `fota_test_lora_repeater.py run` preto občas skončí pred VERIFIED a flash zlyhá na
  timingu (manuálny flash s hotovou VERIFIED session vždy prejde). → viď
  [fcl_readme_verified_pooling.md](fcl_readme_verified_pooling.md) (spevnenie VERIFIED-pollingu).
- **agc_reset držať na 0** (viď §8.4).
- **CAD/LBT držať vypnuté** (`cad_enabled = 0`, default) — viď §8.7.
- Cieľovo: globálny build flag pre všetky nRF52840 boardy (teraz dedikovaný env).
- XIAO/SenseCap (v7) ako FOTA cieľ: nič špeciálne — jeden board-agnostický `flasher_code.h` (app base runtime z linker symbolu); HOTOVÉ 2026-06-25.
- `build_number.txt` / `gen_build_info.py` sú **dočasné testovacie lešenie** (build# vo FW na
  detekciu verzie po flashi a na zaručenie OLD≠NEW).
  - Git konvencia (2026-07-12): v repe je neutrálna hodnota **300** (rovnaká vo
    `features/nrf-fota` aj `features/nrf-fota-dualguard`, aby checkout medzi vetvami
    do súboru nesiahal); lokálne reálne číslo drží
    `git update-index --skip-worktree test_nrf-fota/build_number.txt`. Po novom klone
    flag nastaviť znova. Build bez skriptov je bezpečný: `__has_include("build_info.h")`
    + fallback `FW_BUILD_NUMBER 0` (FotaMyMesh.cpp).

---

## 10. História verzií (testovacie buildy)
`#11→#12` prvý dokázaný FOTA flash cez LoRa (prv. session). `#23→#24`, `#28→#29`, `#32→#33`
opakované PASS pri `agc_reset=0`. Build# je dočasné testovacie počítadlo (`gen_build_info.py`).

---

## 11. VYRIEŠENÉ — po `fota clear` sa re-send zobrazil len ako RAW (2026-06-23 → vyriešené)

Symptóm pri E2E cez Flutter appku: po `fota clear` + re-send tej istej dávky sa pakety
už nedispatchli cez FOTA logiku, v logu len `[FOTA] RAW #N ... type=10`.

**Skutočná príčina NEBOLA vo firmvéri** (pôvodná hypotéza o odregistrovaní kanála bola
nesprávna): MeshCore **seen-table dedup** — `packet_hash = SHA256(typ‖payload)`; Flutter
appka posielala **byte-identické** pakety (`tsBase=0`), takže repeater ich korektne
zahodil ako duplikáty ešte pred dešifrovaním. `fota clear` čistí len FOTA session,
seen-table zámerne nie. **Fix vo Flutter appke**: unikátny `ts` (epoch sekundy) pre každý
paket. Detail: GOTCHA blok v [fcl_readme_nrf-fota.md](fcl_readme_nrf-fota.md) §2 Transport.
Firmware dedup je korektný a nemení sa.

---

## 12. ZephCore port (2026-07) — duálne guardy a sync

FOTA je od 2026-07 naportované aj do ZephCore (`D:\FkDev\FkProj\VSC\ZephCore`,
vetva `features/nrf-fota`). Kľúčové dohody:

- **Zdieľané súbory sú byte-identické** medzi
  `examples/simple_repeater/nrffota/` (tu) a `ZephCore/zephcore/app/nrffota/` +
  `test_nrf-fota/` ↔ `ZephCore/test_nrf-fota/`. Kontrola/prenos:
  `python test_nrf-fota/fota_mczc_scr_sync.py [--copy]` (obojstranný — beží z
  ktoréhokoľvek repa; zdroj pravdy je MeshCore, kopíruje sa vždy MC→ZC).
- **Platformové rozdiely = duálne guardy** `#if defined(FOTA_MESHCORE_BUILD)` /
  `#elif defined(FOTA_ZEPHCORE_BUILD)` priamo v zdieľaných súboroch. Shim
  hlavičky: `FotaFs.h` (CustomLFS File ↔ Zephyr fs_*), `FotaDebug.h`
  (Serial.printf ↔ printk), `FotaCrypto.h` (rweather ↔ PSA+Monocypher),
  `flash_layout.h` (mapy oboch platforiem). FOTA envy tu majú
  `-D FOTA_MESHCORE_BUILD=1`.
- **Per-projekt glue (nesyncuje sa):** `FotaMyMesh.{h,cpp}` (tu) ↔
  `FotaRepeaterMesh.{h,cpp}` (ZephCore). `FotaMesh.{h,cpp}` je zdieľané.
- **`flasher_code.h` je per-repo generovaný** — MeshCore default
  (`build_flasher.py`, ORIGIN 0xEB000, blob bitovo NEZMENENÝ voči odladenému),
  ZephCore `--origin 0x20020000 --platform zephcore` (flasher beží z RAM,
  blob si patch presúva na PATCH_RAM_ADDR sám — `FLASHER_COPY_PATCH`;
  8 kB CODE limit namiesto 4 kB).
- **ZephCore mapa:** app 0x26000/0x27000–0xD0000, FOTA dáta v zdieľanom
  `/lfs/fota/*` (0xD4000, 128 kB), flasher v RAM — flash mapa ZephCore sa
  NEMENÍ. Cesta flash-rezidentného flashera (budúce power-loss recovery)
  ostáva v kóde za `FOTA_FLASHER_IN_FLASH`, trace za `FOTA_FLASHER_TRACE`.
- **`gen_fw_trailer.py` je dual-mode** (PIO post-action aj CLI `--hex/--bin/--uf2`)
  a hex gap-fill je odteraz **0xFF** (zhoda s erased flashom a objcopy binom).
- Pri úprave FOTA kódu TU: ak sa týka zdieľaného súboru, píš obe guard vetvy
  a po commite spusti sync v ZephCore + tamojší build
  (`west build -b promicro_sx1262 zephcore -- -DEXTRA_CONF_FILE="boards/common/repeater.conf;boards/common/fota.conf"`).

### 12.1 HW e2e na ZephCore (2026-07-08, PASS) — čo si vynútil Zephyr

Prvý HW beh odhalil tri ZephCore-špecifické prekážky (všetky opravené, detaily
v commitoch `9c84dea1`/ZephCore `2808509`):

1. **`__rom_region_end` ≠ koniec image** — linker span je len horný odhad
   (368640 vs reálnych 220508 B); `fw_image_size()` v ZEPHCORE vetve preferuje
   presnú veľkosť z FwId traileru (size-gate inak odmietal HEADER).
2. **Kernel RAM + MPU vs RAM flasher** — Zephyr image siaha za 0x20020000
   (`_image_ram_end` ≈ 0x20025000), kopírovanie blobu tam rozbíjalo kernel.
   Flasher okno je preto na VRCHU RAM (0x2003E000–0x20040000), rezervované
   DTS overlayom (`fota.overlay`: sram0 248 kB); stack flashera začína na code
   origine (FLASHER_STACK_TOP defsym) a rastie dole do mŕtvej app RAM. Navyše
   Zephyr ARM MPU: okno mimo sram0 = write fault a SRAM je execute-never →
   pred kopiou/skokom `MPU->CTRL = 0`.
3. **Breadcrumby bez GPREGRET2** — GPREGRET2 prepisuje Adafruit bootloader
   (vždy 0x1) a vrchné kB RAM maže jeho startup stack (SP=0x20040000). RAM
   marker preto na 0x20036000 (mŕtva zóna) + flash trace na 0xCF000 (posledná
   stránka app okna, fakticky voľná — zephcore blob sa buildí s FLASHER_DEBUG=1
   počas stabilizácie).

Výsledok: 2 čisté cykly #289→#290 (DFU baseline) a #290→#291 (čisto FOTA),
patch ~340 B, bežiaca SHA po flashi bit-presná. Companion (mcpy sender) občas
po sende nič neodvysielal (session 0/0 / 1/0) — rieši opakovaný send/reboot
companiona; DUT rádio bolo vždy OK (advert obojsmerne overený).

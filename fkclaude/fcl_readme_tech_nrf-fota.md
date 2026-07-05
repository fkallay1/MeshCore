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
  `variants/xiao_nrf52/platformio.ini` — envy `ProMicro_repeater_fota` (v6) /
  `SenseCap_Solar_repeater_fota` (v7) / `Xiao_nrf52_repeater_fota` (v7) — všetky
  `WITH_LORA_FOTA`+`FOTA_DEBUG`, `extra_scripts` s `create-uf2.py` + 3× `gen_` (jednotné).

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
- Env `ProMicro_repeater_fota` používa `boards/nrf52840_s140_v6_extrafs.ld` (712704 B);
  `SenseCap_Solar_repeater_fota` a `Xiao_nrf52_repeater_fota` používajú `..._v7_extrafs.ld` (708608 B).
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

### 8.2 Príjem / patchovanie chunkov
- **Meshcore chunk CRC vs AES padding** — AES-ECB doplní 159→160 B; bez strhnutia paddingu
  CRC nesedel. Fix v `handle_chunk`: presná dĺžka chunku z `idx`/`patch_size` + clamp.
  (V logu sa stále môže objaviť "CRC BAD", ale finálny `patch.bin SHA256 OK` potvrdzuje dáta.)
- **Reboot-resilient session** — bitmap sa ukladá každých 8 chunkov / pri complete; CustomLFS
  session prežije aj DFU reflash app flashu (overené).
- **`fota_apply` stav pri zlyhaní** — pri base-mismatch obnoví predošlý status (neostane APPLYING).
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
     TX bez rssi/snr). Nové počítadlo `rawtx` v heartbeate (`rawrx=… rawtx=…`). Filter
     `path_count>4` platí pre oba smery. Vše za `FOTA_DEBUG`.
2. `nf=-120` v hluchom stave = **clampnutá dolná hranica** noise floor
   ([RadioLibWrappers.cpp:97](src/helpers/radiolib/RadioLibWrappers.cpp#L97)) → rádio JE v RX a
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
  [RadioLibWrappers.cpp:78](src/helpers/radiolib/RadioLibWrappers.cpp#L78)). MeshCore má naň
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
([FotaPatcher.cpp](examples/simple_repeater/nrffota/FotaPatcher.cpp), commit `d24c6792`).
`sd_disable` ostáva s IRQ povolenými (SVC sa dokončí); chránime kritické NVMC okno + skok.
**Overené na HW: 2 čisté flash cykly (#101→#102, #102→#103, neskôr #104→#105).** Toto je
pravdepodobne aj príčina §8.4 (agc sleep+calibrate = rádio v zlom stave → DIO ISR rozbije flash).

### 8.7 CAD / LBT a FOTA — držať vypnuté (default)
MeshCore má pred TX „listen-before-talk" gate v `Dispatcher::checkSend()` cez
`_radio->isReceiving()`. Tá vetví na dve úrovne v `RadioLibWrapper::isChannelActive()`
([RadioLibWrappers.cpp:207](src/helpers/radiolib/RadioLibWrappers.cpp#L207)):
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
„rádio/CPU zaneprázdnené → zhltne back-to-back chunk" (§8.6, [[fota_apply_resend_dispatch_20260624]]).
Ďalšie okno mimo RX = vyššie riziko straty chunku, plus blokujúci scan stalluje `loop()`, kde
robíme odložené NVMC zápisy. Kolíznu ochranu z veľkej časti dáva už RSSI-prah (vetva 1), ktorá
RX neopúšťa. Konzistentné s [[agc_keep_standard]]: drž MeshCore defaulty.
Ak by si chcel agresívnejšiu ochranu, `set cad on` je runtime (bez rebuildu) — A/B meraj
`getPacketsRecvErrors()` / RAW counter pred a po.

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

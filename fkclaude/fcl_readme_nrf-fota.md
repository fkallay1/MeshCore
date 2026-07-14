# LoRa-FOTA pre MeshCore repeater (nRF52840) — kompletný popis

Aktualizácia firmvéru MeshCore **repeatera cez LoRa** prenosom malého **delta-patchu**
(rozdiel starý→nový FW), namiesto nahrávania celého firmvéru. Port funkčného systému
z projektu **FK_lora-sniffer**. Toto je **iné** ako vstavané MeshCore „FOTA"
(`NRF52Board::startOTAUpdate`), ktoré len reštartuje do Nordic DFU a FW sa nahráva cez BLE.

---

## 1. Čo bolo spravené

| Oblasť | Súbory | Popis |
|--------|--------|-------|
| FOTA modul | [examples/simple_repeater/nrffota/](../examples/simple_repeater/nrffota/) | celý FOTA kód (príjem + patchovanie + integrácia `FotaMyMesh.cpp`), MeshCore jadro nezmenené |
| Integrácia | [MyMesh.h](../examples/simple_repeater/MyMesh.h) / [MyMesh.cpp](../examples/simple_repeater/MyMesh.cpp) | 6 tenkých `#ifdef WITH_LORA_FOTA` hookov (~30 riadkov); telá v `nrffota/FotaMyMesh.cpp` |
| Build env | [variants/promicro/platformio.ini](../variants/promicro/platformio.ini), [variants/sensecap_solar/platformio.ini](../variants/sensecap_solar/platformio.ini), [variants/xiao_nrf52/platformio.ini](../variants/xiao_nrf52/platformio.ini) | `ProMicro_repeater_fota` (v6), `SenseCap_Solar_repeater_fota` (v7), `Xiao_nrf52_repeater_fota` (v7) — extrafs.ld + `WITH_LORA_FOTA` + `FOTA_DEBUG` |
| Test | [test_nrf-fota/](../test_nrf-fota/) | end-to-end LoRa test + nástroje z FK_lora |

Detailný popis FOTA modulu samotného: [examples/simple_repeater/nrffota/README.md](examples/simple_repeater/nrffota/README.md).

**Súvisiace dokumenty:**
- [fcl_readme_tech_nrf-fota.md](fcl_readme_tech_nrf-fota.md) — detailný technický popis (architektúra,
  flasher, krypto, **vyriešené problémy** vrátane AGC-vs-flash interakcie) pre údržbu/budúcnosť.
- [docs/conv_claude_20260615.md](docs/conv_claude_20260615.md) — záznam debugovacej cesty (archívna história debugovania).
- [fcl_readme_verified_pooling.md](fcl_readme_verified_pooling.md) — spevnenie VERIFIED-pollingu v teste.
- [fcl_readme_scope_multihop.md](fcl_readme_scope_multihop.md) — scope/route voľby odosielateľa
  (`--scope` flood/zerohop/region/direct), multi-hop direct (pacing, distinct-hop pravidlo),
  bridge LBT + adaptívny resend. Overené VERIFIED až po 2-hop na živej SK sieti.
- [fcl_e2e_runbook_nrf-fota.md](fcl_e2e_runbook_nrf-fota.md) — **runbook**: porty/zariadenia/úlohy + ako spustiť a overiť e2e.

---

## 2. Architektúra

```
PC (test_nrf-fota/fota_sender.py)              REPEATER (nRF52840, MeshCore)
  hdiffi -inplaceB old new patch               MyMesh::onGroupDataRecv()  ← GRP_DATA (dešifr.)
  zlib(-9,wbits=-9) → staged [ZLIB|..|deflate] fota_process()  (skip 4B ts → FOTA typ)
  GRP_DATA (AES-128-ECB + HMAC) ──┐            chunky → CustomLFS append-log (/ota/recv.log)
                                  │ LoRa       COMPLETE → assemble patch.bin + SHA256 → VERIFIED
  BRIDGE (XIAO, FK_lora) ─────────┘            'fota flash' → flasher@0xEB000:
  [0xAB CD len] serial → raw LoRa TX             HPatchLite inplaceB (old=XIP, new=app flash)
                                                 streaming DEFLATE (puff_stream) + NVMC + verify
                                                 reset → boot NEW
```

### Transport (GRP_DATA kanál)
FOTA pakety idú ako MeshCore `PAYLOAD_TYPE_GRP_DATA` na dedikovanom kanáli (PSK).
- `MyMesh::searchChannelsByHash()` — keď sa `channel_hash` zhoduje s FOTA kanálom, vráti ho
  → MeshCore dešifruje payload cez `Utils::MACThenDecrypt` (AES-128-ECB + HMAC-SHA256).
- `MyMesh::onGroupDataRecv()` — dostane plaintext `[ts 4B][fota_type][...]`, preskočí 4B
  timestamp a zavolá `fota_process()`.
- **Žiadna zmena jadra MeshCore** — `onGroupDataRecv`/`searchChannelsByHash` sú existujúce virtuálne hooky.

> **GOTCHA — dedup vs. re-send (2026-06-23):** GRP_DATA sa dešifruje a routuje na FOTA len ak
> `!_tables->hasSeen(pkt)` (`Mesh.cpp:227`). `hasSeen` je cyklická tabuľka 160 hashov, kde
> `packet_hash = SHA256(typ‖payload)` (`Packet.cpp:41`). Ak sender pošle **byte-identické** pakety
> (rovnaký patch + rovnaký `ts`), repeater ich zahodí ako duplikáty — `logRxRaw` vypíše len `[FOTA] RAW`,
> `onGroupDataRecv` sa NEzavolá. `fota clear` čistí len FOTA receiver, NIE seen-table. **Sender preto MUSÍ
> dať každému paketu unikátny `ts`** (py sendery: `int(time.time())`+`ts+=1`). Symptóm „po fota clear +
> re-send len RAW" bol presne toto — bug bol vo Flutter appke (`tsBase=0`), nie vo firmvéri. Firmware
> dedup je korektný; ak by raz bolo treba znášať identické re-sendy, je možný „bezstavový FOTA routing"
> (doručiť aj pri `hasSeen`, retransmit ponechať pod dedupom) — neimplementované, netreba.

Kanál (od 2026-06-23, #-konvencia): meno `FOTA_CHANNEL_NAME "#fkotanrf"`,
`psk = SHA256(mena)[0:16]` (vrátane `#`), `secret = psk doplnené nulami na 32B`,
`hash = sha256(psk)[0]` (= `0xA4`). Zhodné s `meshcore_py set_channel` aj
`fota_sender.fota_channel_secret()`; sender: `--mode meshcore --psk <hex secretu>`.
(Pôvodný textový PSK `"meshcore-ota-key"` je zrušený.)

### Krypto / FS — znovupoužité z MeshCore
- **SHA256**: `mesh::Utils::sha256` + `rweather/Crypto` (existujúca dep, žiadna nová).
- **AES+HMAC**: `mesh::Utils::MACThenDecrypt` (rovnaké ako bežné MeshCore pakety).
- **FS**: `CustomLFS` (oltaco, existujúca dep) — dedikovaný región @ 0xD4000.

### Flash mapa
**Kanonický zdroj: [`nrffota/flash_layout.h`](../examples/simple_repeater/nrffota/flash_layout.h)**
(detailný rozbor: [tech doc §5](fcl_readme_tech_nrf-fota.md)). V skratke: app končí na 0xD4000,
FOTA FS 92 kB @ 0xD4000, flasher @ 0xEB000 (vyhne sa MeshCore InternalFS @ 0xED000).
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
python examples/simple_repeater/nrffota/tools/build_flasher.py     # → nrffota/flasher_code.h

# 2) FOTA repeater
pio run -e ProMicro_repeater_fota
```
- Gated `-D WITH_LORA_FOTA=1`. Bez flagu sú nrffota súbory inertné → stock buildy nedotknuté.
- FOTA env používa `boards/nrf52840_s140_v6_extrafs.ld` (app končí 0xD4000) + **CZ preset**
  (`LORA_FREQ=869.525`, `SF=7`) aby sa nerušila SK sieť a zhodovalo sa s FK_lora bridge.
- Pre XIAO/s140 v7 (app base 0x27000): nič netreba — FOTA je board-agnostické (app base z linker symbolu `fota_running_fw_base()`, odovzdaný flasheru runtime). Jeden `flasher_code.h`.

---

## 4. Ovládanie (Serial alebo LoRa admin CLI)

`fota status | verify | flash | clear | decompress | nack | miss | missall [cesta] | getpath | setpath | getacl | dbg | agc | id`
(legacy prefix `ota` stále funguje — alias pre Flutter appku, `FOTA-CLI-ALIAS` vo FotaMyMesh.cpp)

- `fota verify` = dry-run (aplikuje patch → SHA256, **nič nezapíše**)
- `fota flash`  = **OSTRÝ** flash + reboot (nevráti sa pri úspechu)
- `fota miss`   = chýbajúce chunky, čiarkami oddelené (`H,S,0-4,6,8,9`; dvojica ako `a,b`,
  dlhší beh `a-b`; strop 20 tokenov, H/S vždy). Total: `/T` overený; `/~T(noS)` = odhad
  z META pred SIG-om (od 2026-07-06 report kompletný hneď po META); `(noH)`/`(noHS)` = bez
  META, vtedy chvost končí markerom `N-??` (N = najvyšší prijatý + 1; appka si ho rozvinie
  z totalu balíka, prípadne zahodí, ak N presahuje total)
- `fota missall [cesta]` = všetky chýbajúce (bez tokenového stropu, len limit LoRa paketu).
  Voliteľná `cesta` (`nn,nn` / `nnnn,…` / `nnnnnn,…` — šírka tokenu = 1/2/3 B hop hash,
  poradie repeater→klient) sa najprv uloží do ACL `out_path` → odpoveď aj všetky ďalšie
  CLI odpovede idú `sendDirect` namiesto floodu
- `fota getpath` / `fota setpath <cesta>` (len LoRa — viažu sa na ACL záznam volajúceho) =
  vypíš / ulož spätnú cestu klienta. Rieši [[fota_reverse_path_never_established]] („repeater
  floodí odpovede, reverzná cesta sa nezaloží"). POZOR: flood login ACL cestu MAŽE — treba ju
  poslať znova (Flutter appka ju pridáva do missall automaticky, ťupka „Poslať cestu v dopyte")
- Serial debug varianty (gated `#if FOTA_DEBUG`): `fota getacl` (výpis ACL s cestami),
  `fota getpath <pubkey-prefix-hex>`, `fota setpath <pubkey-prefix-hex> <cesta>`
- LoRa CLI spracúva voliteľný companion tag `NN|` pred príkazom a zrkadlí ho v odpovedi
  (Flutter appka podľa neho páruje odpovede; bez toho tagované príkazy padali do inline
  CommonCLI cesty a obchádzali defer)

Cez Serial píš priamo (`fota status`). Cez LoRa idú ako admin CLI príkazy (existujúca
MeshCore cesta). Flasher sa púšťa **manuálne** (`fota flash`) — auto-APPLY cez LoRa je tiež
možný (`fota_sender --reboot`), ale default je manuálne spustenie po `fota verify`.

**Preklepy na Serial CLI (build ≥ 318):** backspace/DEL aj šípky sa pred parsovaním
vyčistia (`fota_scrub_cli_line` vo FotaMyMesh.cpp) — opravený preklep už nekončí ako
„unknown command", a to aj pre bežné (nie-FOTA) príkazy. Výnimka: `setperm`, `get acl`,
`discover.neighbors` a `load` mód regiónov (spracujú sa pred naším hookom). Šípky kurzor
neposúvajú, len sa neutralizujú.

Diagnostické výpisy (`[FOTA] …`, heartbeat `AALIVE`) sú za flagom `-D FOTA_DEBUG=1`
(`nrffota/FotaDebug.h`, vzor MESH_DEBUG) — FOTA envy ho majú default zapnutý.
Debug výpisy sú **po anglicky** (nelokalizujú sa).

**Jazyk CLI odpovedí (build ≥ 324):** texty odpovedí sú centrálne v `nrffota/FotaTexts.h`
(`FOTA_TXT_*`). Default je angličtina; naše FOTA envy majú `-D FOTA_LANG_SK=1` → odpovede
po slovensky (ako doteraz). Strojovo parsované formáty (`FOTA n/n st=…`, `miss=`, `id b#…`,
`FOTA flash accepted`…) sú v oboch jazykoch rovnaké — appka aj test skripty fungujú bez
ohľadu na jazyk.

---

## 4b. Podpis FOTA balíka — v0-prefix + ACL (build ≥ 335)

FOTA HEADER (META) je podpísaný Ed25519. Repeater overuje podpis dvomi cestami:

**Formáty SIG paketu:**
- **v0-prefix (nový, default) — `key_id=0`:** za 99B SIG nasledujú 4 B = prefix
  (prvé 4 B) Ed25519 pubkey podpisovateľa → spolu **103 B**. Repeater podľa prefixu
  nájde kľúč a overí (1 verify).
- **legacy — `key_id ≥ 1`:** pôvodných 99 B, mapovanie `key_id N → s_authors[N-1]`.
  Ostáva pre **staré FW** (build < 335), ktoré v0-prefix nepoznajú. Nový sender
  vyrába legacy formát cez `--keyid 1`.

**Kde repeater hľadá kľúč (len pri key_id=0):**
1. `s_authors[]` vo `FotaReceiver_signkey.cpp` (5 zakompilovaných kľúčov, adresované
   prefixom): index 0 = `test_key.der`, index 1–4 = `fota_signkey1..4.der`.
2. Ak prefix nesedí so žiadnym builtin → **ACL admini** repeatera (`ClientACL`, len
   záznamy s `PERM_ACL_ADMIN`; Read/Write a nižšie sa nekvalifikujú). Zhoda podľa
   prvých 4 B identity pubkey. Revokácia = vyhodenie admina z ACL (`acl` clear/login).

Log: `HEADER signer=builtin[i]` (zakompilovaný) alebo `HEADER signer=ACL admin`;
neúspech `signer prefix XXXXXXXX not found/valid (authors+ACL)`.

**Podpisovanie (PC sendre `fota_sender.py` / `fota_sender_mcpy.py` / `fota_export_pkg.py`
/ `gen_fotapkg.py`):**
- `--privkey <kľúč.der>` — DER súbor (seed sa interne expanduje).
- `--privkey-hex <128 hex>` — **companion identity kľúč** (dlhý hex, ktorý zobrazí
  companion/appka; je to expandovaný 64 B kľúč, nie seed).
- default `--keyid 0` (v0-prefix); `--keyid 1` = legacy pre staré FW. Bez privkey
  sa auto-prepne na legacy `key_id=1` s nulovým podpisom (repeater bez
  `FOTA_ALLOW_UNSIGNED` ho odmietne).
- `.fotapkg.json` blok `signed` má navyše `signer_prefix` (8 hex) a `key_id`.

**`fota_keytool.py`** (v `test_nrf-fota/`):
```
python fota_keytool.py gen test_nrf-fota\fota_signkey1.der   # nový keypair + C snippet pre s_authors
python fota_keytool.py der2hex test_nrf-fota\test_key.der    # seed -> expandovaný 128-hex (companion formát) + pub
python fota_keytool.py pub  <kľúč.der | 128hex>              # pubkey / prefix / C snippet
```
`.der → hex` ide (der2hex). **`hex → .der` NEJDE** — companion hex je `SHA512(seed)`
s clampingom (jednosmerná funkcia), seed sa z neho spätne nedá získať; preto
`hex2der` neexistuje. Kľúče `fota_signkey1..4.der` sú gitignored (ako `test_key.der`).

**Jazyk PC nástrojov:** používateľské texty (help, chyby, výpisy `keytool`/podpisu) sú
v `test_nrf-fota/fota_texts.py` (obdoba `FotaTexts.h`). **Default = angličtina**;
`set FOTA_LANG=sk` prepne na slovenčinu. Strojovo parsované výpisy (`[patch] … sha256=`,
`[fotapkg] …`, počítadlá chunkov) cez katalóg NEidú — ostávajú bajt-stabilné (rovnaké
pravidlo ako v FW). Migrácia je prvá tranža (keytool + podpisové hlášky + help kľúčov);
zvyšné diagnostické `[patch]`/`[mcpy]` hlášky sa dajú presúvať do katalógu postupne.

**Stav — OBE CESTY OVERENÉ NA HW (2026-07-15, build #334, Xiao COM3 companion →
ProMicro repeater COM5):**
- **builtin:** patch podpísaný `test_key.der` (key_id=0, prefix C22F8AE0) → repeater
  `HEADER signer=builtin[0]` → `HEADER OK` → `VERIFIED`.
- **ACL admin:** patch podpísaný companion identity hexom (prefix BA3DC5DA, nie v
  `s_authors`, ale `acl[3] ba3dc5da perm=0x03 admin`) → repeater prešiel do ACL vetvy →
  `HEADER signer=ACL admin` → `HEADER OK`.
- Zmena `FOTA_META_MAGIC` (v2) korektne zahodí starý `meta.bin` bez crashu.

**ACL-admin test — postup (na zopakovanie):**
1. Cez appku (alebo companion) sa prihlás na repeater ako admin (`ADMIN_PASSWORD`,
   default `"password"`) → vznikne ACL admin záznam s identitou companiona. Over cez
   Serial `fota getacl` (hľadaj `perm=0x03 admin` s prefixom companiona).
2. Zisti privátny identity kľúč companiona ako dlhý hex (128 hex = expandovaný;
   appka ho vie zobraziť). Prefix odvodíš cez `fota_keytool.py pub <hex>`.
3. Pošli patch podpísaný týmto hexom: `fota_sender_mcpy.py … --privkey-hex <hex>`
   (default key_id=0). Base FW patchu musí sedieť s bežiacim FW repeatera (napr.
   old = práve bežiaci build, bez `--reboot` sa neflashne).
4. Očakávaj na repeateri: `HEADER signer=ACL admin` → `HEADER OK`. Negatívny test:
   kľúč, ktorý nie je ani builtin ani ACL admin → `signer prefix … not found`.

---

## 5. Build number (dočasné testovacie lešenie)

`test_nrf-fota/gen_build_info.py` (pre-script FOTA env-u) pri každom builde inkrementuje
`test_nrf-fota/build_number.txt` a generuje `build_info.h` s `FW_BUILD_NUMBER`. Repeater
ho vypisuje na boote a v heartbeate `[FOTA] AALIVE build #N`. Slúži na:
- detekciu, či po FOTA flash beží NOVÁ verzia (build# sa zvýšil),
- zaručenie že OLD != NEW (build# je súčasť kódu → patch nie je prázdny).

---

## 6. End-to-end test cez LoRa

**Topológia:** `PC ─USB─ XIAO bridge (COM3) ─LoRa─ ProMicro repeater (COM5) ─USB─ PC`

Bridge = FK_lora `gateway_fw` (`[0xAB CD len]` serial → raw LoRa TX). Repeater = MeshCore FOTA.
Oba na **CZ presete** (869.525/SF7), aby sa počuli a nerušili SK sieť.

```bash
PENV=~/.platformio/penv/Scripts/python.exe   # má pyserial + platformio

# 1) baseline: flash bridge (CZ) na COM3 + repeater OLD (CZ) na COM5
$PENV test_nrf-fota/fota_test_lora_repeater.py baseline --bridge-port COM3 --target-port COM5
#    (ak XIAO už beží ako CZ bridge:  pridaj --skip-bridge)

# 2) run: build NEW, patch OLD→NEW, broadcast cez bridge, flash, verify
$PENV test_nrf-fota/fota_test_lora_repeater.py run --bridge-port COM3 --target-port COM5
#    voliteľne: --cycles 4 --drop 0.2  (simulácia straty + kumulácia naprieč cyklami)
```

Test:
1. `baseline` — postaví+nahrá bridge (CZ) a repeater OLD (CZ), uloží OLD app obraz + build#.
2. `run` — postaví NEW, vyrobí patch (`hdiffi` + zlib), broadcastuje cez bridge ako GRP_DATA,
   po VERIFIED spustí `fota flash` (dry-run `fota verify` je opt-in `--verify-first`),
   po reboote overí `build #NEW` + `[FLASHER-DBG]` marker + FNV-1a checksum.

**Predpoklady:** COM5/COM3 voľné (zatvor Serial Monitor), `hdiffi.exe` + `pyserial` +
`pycryptodome` v penv pythone (`pip install -r test_nrf-fota/requirements.txt`).

### Stav — OVERENÉ NA HW

E2E cez LoRa **funguje a je opakovane overené**: prvý dôkaz #11→#12 (2026-06-14),
opakované PASS pri `agc_reset=0`, cez companion (mcpy) #158→#159 (2026-06-26),
samobežný runner #109→#111. Overené aj poistky: base-FW check odmietne cudzí patch,
FOTA FS prežije DFU reflash.

**História ladenia** (preamble 32 pre SF≤8, TX výkon bridge, zaseknutý noise floor,
AGC-vs-flash interakcia — **nechať `agc_reset=0`**) je detailne rozobraná v
[tech doc §8](fcl_readme_tech_nrf-fota.md) (8.3 príjem na rádiu, 8.4 AGC, 8.5 bridge RF);
plný chronologický záznam: [docs/conv_claude_20260615.md](docs/conv_claude_20260615.md).

**Diagnostika (gated `WITH_LORA_FOTA` + `FOTA_DEBUG`):**
- `fotaLogRxRaw()` → `[FOTA] RX RAW` + `rawrx` v `[FOTA] AALIVE` heartbeate = surové CRC-OK
  rámce PRED dekódom (odlíši „rádio nepočuje nič" od „počuje, dekód zlyhá").
- `fotaLogTxRaw()` → `[FOTA] TX RAW` + `rawtx` = každý ODOSLANÝ rámec (vlastné adverty/ACK aj
  preposlané); rovnaký formát ako RX (type/route/path), bez rssi/snr. Core hook `logTxRaw`.
- `fota agc` (serial/CLI) → SX1262 RxGain register (0x08AC: 0x96 boosted / 0x94 power-save),
  okamžité RSSI, noise floor, `agc_reset_interval`. **Read-only — nemení config rádia.**
- `onGroupDataRecv()` len buffruje, ťažké CustomLFS I/O sa robí vo `fotaLoop()` po re-arme rádia.

Postup testu (fire-and-forget, príjem niekedy potrebuje pár cyklov kvôli strate paketov):
```bash
PENV=~/.platformio/penv/Scripts/python.exe
$PENV test_nrf-fota/fota_test_lora_repeater.py baseline --skip-bridge   # OLD + clear + reboot
$PENV test_nrf-fota/fota_test_lora_repeater.py run --skip-bridge --cycles 4
# ak run skončí pred VERIFIED: znova broadcast (fota_sender) a potom 'fota flash' manuálne
```

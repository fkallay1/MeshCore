# fcl_fable_fota_infos.md — Fable 5: audit FOTA projektu + dočistenie rename

**Dátum:** 2026-07-03 · **Branch:** `features/nrf-fota` · **Autor:** Claude Fable 5
**Zadanie:** prejsť celý FOTA kód a poznámky, dohľadať a opraviť zvyšky po OTA→FOTA
rename, navrhnúť vyčistenie/sprehľadnenie.

---

## 1. Celkový stav projektu (zhrnutie po prečítaní kódu + docs)

LoRa-FOTA (delta-patch OTA pre nRF52840 repeatre) je vo **funkčnom, overenom stave**:

- **FW modul** `examples/simple_repeater/nrffota/` — čistá architektúra:
  `FotaProtocol.h` (wire) → `FotaReceiver` (session/FS/krypto) → `FotaPatcher` (hpatch
  + flasher) → `FotaMesh` (CLI + kanál) → 3 malé `#ifdef WITH_LORA_FOTA` zásahy v MyMesh.
  Deferred spracovanie (RX len buffruje, FS I/O v `loop()`), deferred CLI aj deferred
  APPLY (ACK odíde pred rebootom) — hlavné historické pasce (NVMC stall, stack overflow,
  AGC-vs-flash) sú vyriešené a zdokumentované.
- **PC strana** `test_nrf-fota/` — `fota_sender.py` (bridge), `fota_sender_mcpy.py`
  (companion), `gen_fotapkg.py` + hook (auto `.fotapkg.json` po builde), e2e test
  `fota_test_lora_repeater.py`, pytest 9 testov formátu.
- **Docs** — `nrffota/README.md` (modul), `fkclaude/fcl_readme_nrf-fota.md` (užívateľský),
  `fcl_readme_tech_nrf-fota.md` (technický), runbooky, rename handoff.

Rename OTA→FOTA (2026-06-25) bol urobený dobre v identifikátoroch **verejného API**,
ale ostala vrstva zvyškov v členských premenných, pomocných funkciách, stringoch
a hlavne v dokumentácii — vrátane niekoľkých **fakticky nesprávnych** údajov. Všetko
nižšie je opravené.

## 2. Opravené rename chyby (tento audit, 2026-07-03)

### 2a. Fakticky nesprávne (najzávažnejšie)
| Kde | Problém | Fix |
|---|---|---|
| `nrffota/README.md` | radil `-D FOTA_CHANNEL_PSK='"..."'` — **makro už neexistuje** (od 2026-06-23 #-konvencia) | `-D FOTA_CHANNEL_NAME='"#fkotanrf"'` |
| `fcl_readme_nrf-fota.md`, `fcl_readme_tech_nrf-fota.md` | popisovali starý kanál z textového PSK `"meshcore-ota-key"` | popis #-konvencie (psk = SHA256(mena)[0:16], hash 0xA4) |
| `fcl_readme_scope_multihop.md` | príklad `PSK=$(python -c "print(b'meshcore-ota-key'.hex())")` → **vysielal by na zlom kanáli**; `-DOTA_GDR_DIAG` → flag sa volá `FOTA_GDR_DIAG` | psk cez `fota_channel_secret()`; flag opravený |
| `fota_test_lora_repeater.py` | mŕtva konštanta `FOTA_PSK_STR = "meshcore-ota-key"` (nepoužitá, mätúca) | zmazaná (psk sa berie runtime zo sendera) |
| `fcl_readme_tech_nrf-fota.md` | typy paketov „BEGIN, CHUNK, APPLY" — BEGIN neexistuje | HEADER=META (0x10), SIG (0x13), CHUNK, APPLY |
| `FotaProtocol.h`, `fota_sender.py` | odkaz na spec `docs/superpowers/specs/...` — spec je v `fkclaude/docs/...` | cesta opravená |
| `.gitignore` + git | patterny na staré názvy (`ota_patch*.bin`, `*.otapkg.json`) → **`fota_patch.bin` bol omylom commitnutý** ako binárka; nové artefakty sa neignorovali | patterny prepísané na `fota_*`/`*.fotapkg.json`; `git rm --cached fota_patch.bin`; staré artefakty (`ota_patch*.bin` v roote aj v test_nrf-fota/) zmazané z disku |
| `push_fotapkg.bat` | komentár tvrdil, že spúšťa `push_otapkg.py` | opravený |
| `emit_fota_golden.py` | usage v docstringu odkazoval na `emit_ota_golden.py` | opravený |
| `gen_build_info.py` | komentár `-DWITH_LORA_OTA` (flag je `WITH_LORA_FOTA`) | opravený |

### 2b. Identifikátory (kód funguje rovnako, len konzistentné mená)
- **MyMesh.h/.cpp:** `_ota_channel/_ota_ready/_ota_pending*/_ota_raw_*` → `_fota_*`
  (v súbore už bol `_fota_apply_deadline` — mix je preč).
- **FotaReceiver.cpp:** statická stavová premenná `ota` → `fota` (~90 miest).
- **fota_sender.py:** `sign_ota_header/direct_ota_packet/build_ota_chunk/build_ota_apply/send_ota`
  → `*_fota_*` / `send_fota`; upravené importy v `fota_sender_mcpy.py` a `emit_fota_golden.py`.
- **gen_fotapkg_hook.py:** env premenná `OTAPKG_SKIP` → **`FOTAPKG_SKIP`**
  (legacy `OTAPKG_SKIP` stále akceptovaná).

### 2c. Užívateľské stringy a komentáre
- Serial hinty: `'ota verify'/'ota flash'` → `'fota …'` (FotaReceiver VERIFIED hláška),
  `.otapkg.json` → `.fotapkg.json` (výpis `fota id`), `[OTA]` → `[FOTA]` prefixy
  v `fota_sender.py`, `[DIAG] GDR otatype=` → `fota_type=`, „Mazem OTA session" → FOTA.
- Prózové komentáre „OTA modul/kanál/paket/…" → FOTA vo všetkých `nrffota/*` súboroch,
  MyMesh, python skriptoch a `AGENTS.md` (2 riadky). Banner generovaného
  `flasher_code.h` + jeho šablóna v `build_flasher.py`.
- `nrffota/README.md`: CLI tabuľka prepnutá na `fota …` prefix + doplnené chýbajúce
  príkazy (`miss`, `missall`, `id`, `agc`) + poznámka o legacy aliase; odkaz na sender
  opravený z `FK_lora-sniffer/tools/...` na `test_nrf-fota/fota_sender.py`.

### 2d. Overenie
- `pio run -e ProMicro_repeater_fota` → **SUCCESS** (build #209; hook potvrdil nový
  `FOTAPKG_SKIP`). Pozn.: verifikačný build bumpol build# — vedľajší efekt lešenia.
- `pytest test_nrf-fota/tests` → **9/9 PASS**; `py_compile` všetkých editovaných .py OK.
- Na HW (LoRa e2e) som netestoval — zmeny sú rename/komenty/stringy, wire nezmenený.

## 3. Zámerne PONECHANÉ ako „ota" (nemeniť!)
Rovnaké ako v [docs/fota-rename-handoff.md](docs/fota-rename-handoff.md) §5:
- **Wire:** kanál `#fkotanrf` (hash 0xA4), `FOTA_MAGIC 0x07A0`, payload typy 0x10–0x21.
- **Device FS cesty:** `/ota`, `/ota/meta.bin`, `/ota/recv.log`, … (zmena by osirela
  session pri upgrade) + meta magic `0x4F544101` („OTA\x01").
- **CLI alias `ota …`** — miesta označené `FOTA-CLI-ALIAS` (MyMesh.cpp), kým Flutter
  appka nemigruje na `fota …`.
- **Upstream:** `esp32_ota` lib_deps sekcie, `NRF52Board::startOTAUpdate` (BLE DFU) —
  cudzie OTA, netýka sa nás.
- E2E test zámerne posiela `ota status/clear/flash` — **testuje tým legacy alias**,
  ktorý reálne používa Flutter appka. Regex `OTA \d+/\d+` matchuje aj nový „FOTA …"
  výstup (substring), takže funguje pre oba tvary.

## 4. Návrh vyčistenia / sprehľadnenia (na schválenie, zoradené podľa úžitku)

1. **Dokončiť CLI migráciu `ota` → `fota`** (až keď Flutter appka prejde na `fota …`):
   zmazať vetvy `FOTA-CLI-ALIAS` v MyMesh.cpp, prepnúť e2e test na `fota …`
   (+ `find_ota_status` → `find_fota_status`, regex na `FOTA`). Jednorazová, nízke riziko.
2. **Dedup dokumentácie** — flash mapa je dnes na 4 miestach (flash_layout.h,
   nrffota/README.md, fcl_readme_nrf-fota.md, AGENTS.md) a časom sa rozchádzajú
   (práve som jednu zosúlaďoval). Návrh: kanonicky **len `flash_layout.h`** (komentár)
   + AGENTS.md; README/fcl len linkovať.
3. **Zoštíhliť `fcl_readme_nrf-fota.md`** — sekcie „Kľúčové nálezy z ladenia 2026-06-14/15"
   sú cenná história, ale patria do tech docu/archívu; užívateľský readme by mal mať
   len aktuálny stav + odkazy. (Presun, nie mazanie.)
4. **`conv_claude_20260615.md` presunúť z rootu do `fkclaude/docs/`** (root má byť bez
   pomocných súborov; fcl_readme naň linkuje — link opraviť).
5. **Rozhodnúť osud build# lešenia** (`gen_build_info.py`, heartbeat `AALIVE`): pre
   produkčné buildy by build# mal ostať (FW identita), ale heartbeat interval/verbozita
   by mohli byť za flagom (čiastočne už je: `FOTA_INFO_MSG`).
6. **Zjednotiť rádio defaulty senderov** — `fota_sender_mcpy.py`/`fota_export_pkg.py`
   majú default 869.618/SF8, e2e test stavia CZ preset 869.525/SF7. Funguje (explicitné
   argumenty), ale default v dvoch skriptoch ≠ default FW envu → footgun pri ručnom použití.
7. (Kozmetika, voliteľné) zarovnanie stĺpcov `FOTA_PKT_*` definícií v FotaProtocol.h
   a `fota_sender.py` — po rename sa posunuli o 1 znak. Nechal som tak (no-reformat pravidlo).
8. (Nápad) pridať pytest golden test na `fota miss`/`missall` reply formát (rozsahy,
   tokenový strop) — dnes pokryté len ručne.

## 5. ČASŤ 2 (2026-07-03, ten istý deň) — realizácia cleanupu + FOTA_DEBUG + presun do nrffota

Používateľ schválil „urob všetko" z §4 + zadal navyše FOTA_DEBUG makrá a presun kódu.
**NEcommitnuté — čaká na review.** Stav: build oba FOTA envy PASS, pytest 9/9 PASS.

### 5a. FOTA_DEBUG makrá (nové)
- **`nrffota/FotaDebug.h`** — `FOTA_DEBUG_PRINT/PRINTLN(fmt, ...)` = `Serial.printf`,
  vzor `MESH_DEBUG_PRINT` (MeshCore.h); gated `#if FOTA_DEBUG && ARDUINO`, bez flagu
  sa nekompilujú. **Všetky** diagnostické `Serial.print*` vo FOTA kóde skonvertované
  (FotaReceiver ~47, FotaPatcher ~53, FotaMesh, FotaBuffer, FotaMyMesh) — printf štýl,
  texty správ byte-zhodné (e2e parsuje `[FLASHER-DBG]`, `FOTA n/m st=0x`, `AALIVE`).
  CLI reply buffre (sprintf) nezmenené — funkčný výstup. `Serial.flush()` pred skokom
  do flashera ponechaný (funkčný). Oba FOTA envy: **`-D FOTA_DEBUG=1`** (default ZAP).
- **CRLF fix (po HW teste)**: `FOTA_DEBUG_PRINTLN` lepí **`"\r\n"`** ako `Serial.println`
  — samotné `"\n"` (štýl MESH_DEBUG) robilo na termináli „rosypaný" výpis (schodíky bez
  návratu vozíka). Newline sa do formátov nedáva ručne — riadok vždy končí cez PRINTLN.
  CLI reply cesta cez Serial je ako v pôvodnom kóde: reply plní sprintf, tlačí ho
  upstream `main.cpp` (`  -> reply`) — **nezávislé od FOTA_DEBUG**.

### 5b. Presun FOTA kódu z MyMesh do nrffota (minimalizácia diffu vs upstream)
- **`nrffota/FotaMyMesh.cpp`** (nový) — telá VŠETKÝCH FOTA metód MyMesh + lokálne
  helpery (fota_args_of, payload_type_name, FotaCliDefer, stacktrace meranie):
  `fotaLogRxRaw, searchChannelsByHash, onGroupDataRecv, fotaEarlyInit, fotaBegin,
  fotaHandleCliCommand, fotaHandleLoRaCli, runFotaCli, deferFotaCli,
  sendDeferredCliReply, fotaLoop`. Kompiluje sa automaticky (`nrffota/*.cpp` vo filtri).
- **MyMesh.cpp**: diff vs `dev` klesol zo **408 na ~30 riadkov** = 6 tenkých hookov
  (logRxRaw, onPeerDataRecv else-if, begin ×2, handleCommand else-if, loop).
  LoRa CLI echo presunuté do fotaHandleLoRaCli (predtým negated aj v stock buildoch)
  a doplnená symetria: `[LoRa->CLI] <príkaz>` (prijaté) + `[CLI->LoRa] <odpoveď>`
  (odosielané, v sendDeferredCliReply — upstream odpoveď nikdy nevypisoval, ani
  s MESH_DEBUG; platí len pre FOTA príkazy, štandardné CLI nechané bez zmeny).
- **MyMesh.h**: diff vs upstream = **4 riadky** — celý FOTA blok (stav `_fota_*` +
  deklarácie metód) je v `nrffota/FotaMyMesh.h`, ktorý sa `#include`-uje **vnútri
  tela `class MyMesh`** (preprocessor vlepí member deklarácie; súbor má výrazné
  varovanie, že sa nesmie includovať samostatne). Include `nrffota/FotaMesh.h`
  z headera odstránený (netreba).
- Pozn.: `FOTA_CLI_REPLY_DELAY_MILLIS 600` vo FotaMyMesh.cpp musí sedieť
  s `CLI_REPLY_DELAY_MILLIS` v MyMesh.cpp (private define, zámerná duplicita s komentom).

### 5c. Cleanup z §4 (zrealizované)
- **e2e test** `fota_test_lora_repeater.py` → posiela **`fota …`** príkazy,
  `find_ota_status`→`find_fota_status`, regex `FOTA\s+`; FW alias `ota` OSTÁVA
  (Flutter appka) — zmaže sa až po jej migrácii.
- **Docs dedup**: flash mapa kanonicky **len `flash_layout.h`** (+ stručne AGENTS.md);
  nrffota/README.md, fcl_readme aj tech doc §5 už len linkujú + one-liner.
- **fcl_readme_nrf-fota.md zoštíhlený**: historické „Kľúčové nálezy/Ladenie 2026-06-14/15"
  nahradené krátkym stavom + odkazmi na tech §8.3–8.5 a docs/conv_claude (obsah tam už bol).
- **`conv_claude_20260615.md`** presunutý z rootu → `fkclaude/docs/` (link opravený).
- **Tech doc**: §2 tabuľka doplnená (FotaMyMesh/FotaDebug/FotaBuffer/FwId/signkey),
  integrácia prepísaná na 6 hookov, §11 „OTVORENÝ BUG" prepísaný na **VYRIEŠENÉ**
  (skutočná príčina = Flutter tsBase=0/seen-table, nie kanál), CLI na `fota`.
- **Sender defaulty**: komentáre vo fota_sender_mcpy.py + fota_export_pkg.py
  (869.618/SF8 = FK pracovný kanál ≠ CZ e2e/FW env 869.525/SF7 — zadávaj explicitne).

### 5d. Incident počas práce
Časť práce bežala omylom na inom modeli — po ňom dohľadané a opravené: neplatný
„build PASS" (bežal počas rozpracovaných editov; přebuildované po dokončení),
kompilačná chyba `FOTA_DEBUG_PRINTLN(buf)` (makro lepí literál — fix `("%s", buf)`).

### 5e. Zostáva / na zváženie
- HW e2e test po refaktore (kód je len presunutý/print-makrá, wire nezmenený — ale
  pred nasadením odporúčam jeden `run` cyklus).
- `%f`/`%.3f` v printf: upstream ho používa (MESH_DEBUG v RAK senzoroch), na nRF52
  by mal fungovať — pri prvom HW teste over výstup `AALIVE freq=…` (ak by bol prázdny,
  Adafruit printf bez float supportu → prepnúť na celočíselný výpis).
- Body §4/5 (build# lešenie) a §4/8 (pytest na miss formát) zostávajú otvorené.

## 6. Dotknuté súbory (audit, časť 1)
FW: `MyMesh.{h,cpp}`, `nrffota/{FotaReceiver.{h,cpp},FotaMesh.{h,cpp},FotaProtocol.h,`
`FotaState.h,FotaFs.h,FotaPatcher.{h,cpp},FotaReceiver_signkey.cpp,flash_layout.h,`
`puff_stream.c,flasher/flasher.ld,flasher_code.h,tools/build_flasher.py,README.md}`
PC: `test_nrf-fota/{fota_sender.py,fota_sender_mcpy.py,fota_export_pkg.py,`
`fota_test_lora_repeater.py,gen_fotapkg.{py,bat},gen_fotapkg_hook.py,gen_build_info.py,`
`gen_fw_trailer.py,push_fotapkg.{py,bat},tools/emit_fota_golden.py}`
Ostatné: `.gitignore` (+ `git rm --cached fota_patch.bin`, zmazané stale `ota_patch*.bin`),
`AGENTS.md`, `fkclaude/{fcl_readme_nrf-fota.md,fcl_readme_tech_nrf-fota.md,fcl_readme_scope_multihop.md}`.

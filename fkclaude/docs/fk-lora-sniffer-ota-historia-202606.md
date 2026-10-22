# FK_lora-sniffer OTA — história a fakty z júna 2026 (pred portom do MeshCore)

> Archív. Tieto poznámky vznikli pri práci na repe `D:\FkDev\FkProj\VSC\FK_lora-sniffer`
> (sniffer + bridge `gateway_fw` pre nRF52840 ProMicro/XIAO), ale sessions bežali z adresára
> MeshCore, takže ležali len v pamäti tohto projektu. FK_lora-sniffer nemá vlastný `fkclaude/`;
> keď ho dostane, tento súbor sa tam presunie. Bridge (`gateway_fw`) je zároveň testovací
> nástroj MeshCore FOTA — viď [fcl_readme_scope_multihop.md](../fcl_readme_scope_multihop.md) §3.
>
> Čo z toho platí pre MeshCore FOTA dnes, je v
> [fcl_readme_tech_nrf-fota.md](../fcl_readme_tech_nrf-fota.md) (§5 flash mapa, §6 flasher,
> §8 vyriešené problémy). Cesty `C:\Users\...` a OneDrive v texte sú historické — od 2026-06-21
> je všetko na `D:\FkDev` (portable prostredie), penv python
> `D:\FkDev\.platformio\penv\Scripts\python.exe`.

## Sniffer OTA — architektúra (jún 2026)

Streaming in-place patch pre nRF52840 ProMicro sniffer. Nový FW ~650 kB, RAM ~50–100 kB voľnej,
LittleFS len 120 kB → celý FW sa do RAM nezmestí, patch sa musí aplikovať streamovaním.

### Flash mapa sniffera (iná než MeshCore port)

```
0x26000–0xD4000  app flash (712 kB)
0xD4000–0xF2000  LittleFS 120 kB (30 × 4 kB) — prijíma patch.bin
0xF2000–0xF3000  flasher kód (4 kB, standalone C + HPatchLite)
0xF3000–0xF4000  flash trace log (bývalá FLASHER_META)
0xF4000+         bootloader
```

MeshCore port má FOTA FS 92 kB @ 0xD4000, flasher @ 0xEB000, trace @ 0xEC000 — zmenšené, aby
MeshCore InternalFS @ 0xED000 prežil (tech §5).

### Tok patchu

PC: `hdiffi -inplaceB old.bin new.bin patch.bin` (HPatchLite inplaceB — bez tohto flagu flasher
nefunguje, HDiffPatch a HPatchLite formáty sú nekompatibilné), potom
`zlib.compress(patch, level=9, wbits=-9)` → staged `[magic 'ZLIB' 4B][uncomp_size 4B][new_fw_size 4B][raw DEFLATE]`.
Zariadenie (`ota_patcher.cpp → ota_flash_via_flasher()`): `hpatchi_inplace_open()` → patch do RAM
→ `sd_softdevice_disable()` → flasher blob zapísať → skok `flasher_entry(...)`. Flasher
(`flasher/flasher.c`, ARM Thumb2 mimo app flash): puff_stream dekompresia on-the-fly, HPatchLite
inplaceB (old = XIP app flash, new = in-place), priamy NVMC, FNV-1a verify po zápise,
`NVIC_SystemReset()`. Po reboote `ota_init()` zistí poškodený LittleFS → format.

Pôvodná (nestreamovaná) verzia dekomprimovala do `DECOMP_BUF @ 0x20000000` (max 216 kB) a stagovala
patch do 0xD4000 (ničila FS) — nahradená streamingom 14. 6. 2026 (commit 9740e9f dev-stream):
patch v RAM 0x20000000 (memmove), `read_diff = puff_stream_read`, temp_cache/FlashCtx v .bss,
flasher.ld RAM 0x20020000/128 kB. HW overené na 650 kB FW (build #74, #76): FS prežil,
FNV == Python zlib, `extra_safe` pre 650 kB patch = 0.

### Ladenie flashera — čo sa naučilo

- **Flasher je bez Serial** (USB CDC ide cez SoftDevice, ktorý je vypnutý). Debug krok sa odkladal do
  retenčného registra GPREGRET2. POZOR: adresa `0x40000514` použitá v tomto období **nie je
  GPREGRET2** (skutočný = 0x40000520) — zápis tam rozbil POWER perifériu, viď tech §8.1b. Register
  sa číta priamo (`*(volatile uint32_t*)`), nie cez `sd_power_gpregret_get()` (vracia
  `NRF_ERROR_INVALID_STATE` pred SD enable).
- **WDT:** flasher kŕmi RR[0..7] @0x40010600 hodnotou 0x6E524635; čisto-CPU fáza (puff) ho musí
  kŕmiť tiež (`wdt_feed()` z `outbyte()` každých 8 kB).
- **RESETREAS** (0x40000400, write-1-to-clear): bit0 PIN, 1 DOG, 2 SREQ, 3 LOCKUP — vypisovať pri
  on-connect banneri.
- **Flash trace** (0xF3000): flasher appenduje 32-bit event kódy cez NVMC, FW číta sekvenciu.
  Flasher kód musí byť < 4096 B, inak prepíše trace stránku (ftrace_init ju eraseuje → zmaže kód);
  `build_flasher.py` exituje pri > 4096 B. Pri `FLASHER_DEBUG=0` sa trace **neeraseuje** → čítanie
  starého trace zavádza („FNV nesedí", „beží starý build"); spoľahlivý je banner build#.
- **IRQ crash:** flasher bežal s aktívnymi IRQ z OS/SoftDevice kontextu; SysTick/RADIO/USB
  prerušenie skočilo cez VTOR do app handlera bez platného stavu → reset. Fix `__asm("cpsid i")`
  na začiatku `flasher_main`. Dôsledok: FAIL cesta nesmie byť `while(1)` (zariadenie visí bez USB)
  → SystemReset + ftrace(0xFE). MeshCore port neskôr narazil na to isté okno **pred** skokom
  (zápis flasher blobu s povolenými IRQ) — tech §8.6.
- **RAM konflikt:** DECOMP_BUF @0x20000000 prekrýval flasher .bss+stack @0x20020000 pri dekompresii
  > 128 kB → chaotické trace hodnoty. Fix: RAM ORIGIN flashera 0x20036000 (40 kB), DECOMP_BUF_MAX
  216 kB; neskôr streaming problém odstránil úplne.
- **FLASHER_DRYRUN** (prejde celý proces bez zápisu) chránil pred brickom pri ladení; ostrý zápis
  chránený FNV verify + pri nezhode skok do DFU (`GPREGRET 0x4000051C = 0x57` UF2 magic + reset).
  Dry-run FNV je pre inplaceB nespoľahlivý (nezapisuje → `flash_read_old` číta zlé dáta).
- **Oživenie:** RESET tlačidlo (single = reštart, double-tap = DFU/UF2 disk); pri poškodenom app flashi
  nabehne len bootloader → DFU re-flash cez USB (`ota_test.py baseline`).

### Koreňová príčina „patch nefunguje" (13. 6. 2026) — infraštruktúra, nie flasher

1. **`gen_build_info.py` bežal viackrát za build** (top-level kód SCons skriptu sa vykoná pri každom
   načítaní) → `build_info.h` sa menil počas buildu → nedeterministický firmware. Fix: guard cez
   SCons env `_BUILD_INFO_DONE`.
2. **`old_firmware.bin` bol desynchronizovaný** s tým, čo DFU reálne nahráva (`.pio/build/<env>/firmware.zip`)
   → patch zo zlého base. Fix: old/new brať priamo z `firmware.zip` (autoritatívny obraz), nie z
   `tools/firmware.bin`. Platí dodnes: base vždy z archívu toho, čo na zariadení beží.

Diagnostika, ktorá to odhalila: SHA256 base-check app flashu vs `old_sha256`, dry-run cez
nezávislú cestu (`puff_stream + hpatch_lite_patch`) dával tiež zlý SHA → chyba nebola v jednej
implementácii.

### Build a prostredie (historické pasce)

- **OneDrive ničil `.git` aj `.pio`** (index.lock, zmiznuté `.git`, `MissingPackageManifestError`
  počas buildu). Dočasný fix `workspace_dir` mimo OneDrive; trvalé riešenie = presun všetkého na
  `D:\FkDev` (21. 6. 2026). Obnova `.git` po strate: `git init -b <vetva>` + `remote add` + `fetch` +
  `git reset --mixed origin/<vetva>` (NIE `--soft` — nechá index prázdny → falošné „D").
- **PlatformIO na Windows a cp1250:** diakritika v `library.json` manifeste rozbila JSON parsing
  (`pio pkg list` → UnicodeEncodeError); inline komentár `; ...` za hodnotou `build_flags` rozbíja
  parsing — komentár na samostatný riadok. `lib/ed25519/` potreboval vlastný `library.json`
  (ASCII). To isté cp1250 neskôr postihlo FOTA hook (tech, `gen_fotapkg_hook.py`).
- `gen_build_info.py` (pre-build) inkrementuje `build_number.txt` a generuje `build_info.h`
  (`FW_BUILD_NUMBER`, `FW_BUILD_TIME`) → `build #N` v banneri a AALIVE; dôsledok OLD ≠ NEW pri
  každom builde, test sa dá spustiť bez zmeny zdroja. `build_info.h`, `build_number.txt`,
  `*_firmware.bin` v `.gitignore`.
- Repo FK_lora-sniffer: GitHub `fkallay1/FK_lora-sniffer`, vetvy `main` (záloha) a `dev-stream`
  (pracovná).
- `unzip_bin.py` (post-build) rotácia: predošlý `fw_new.bin` → `tools/fw_old.bin`, aktuálny build
  z `firmware.zip` → `tools/fw_new.bin` (vyberá najnovší zip podľa mtime) → `fw_old` vždy = to, čo
  beží. Manuálny postup: `pio run -t upload` → uprav kód → `pio run` →
  `ota_sender.py --old tools/fw_old.bin --new tools/fw_new.bin --port COM5 --mode serial-direct --reboot`.
- `tools/ota_test.py baseline|run` (penv python): dvojfázový HW-in-the-loop test, serial-direct
  injekcia `[0xAB][0xCD][len][data]` bez LoRa; čaká na reboot cez `list_ports`, číta banner build#.

### Multi-board (ProMicro + XIAO) a test cez LoRa (14. 6. 2026, commit c9455ec)

- `shared/board_config.h` dispatcher podľa `-DBOARD_PROMICRO`/`-DBOARD_XIAO`; `boards/xiao.h`:
  D1=DIO1, D2=RESET, D3=BUSY, D4=NSS, D5=RXEN, D8=SCK, D9=MISO, D10=MOSI, bez POWER_EN
  (`SX126X_POWER_EN=-1`). `shared/flash_layout.h` per board — jediný rozdiel XIAO
  `APP_FLASH_START 0x27000` (s140 v7.3) vs `0x26000` (v6). Flasher sa rebuildol byte-identicky.
- Envy `{ProMicro,Xiao}_{sniffer,bridge}` cez `[common]`/`[sniffer_common]`/`[bridge_common]`;
  z MeshCore skopírované board json, `nrf52840_s140_v7*.ld`, `variants/xiao_nrf52/` (BSP nemá XIAO).
- `ota_test_lora.py baseline|run --bridge-port COM3 --target-port COM5 [--cycles N --drop P]`:
  fire-and-forget broadcast cez bridge, prijímač nič nevysiela späť. HW overené: direct mód,
  `--drop 0.3` (kumulácia naprieč cyklami), meshcore mód (šifrovaný GRP_DATA) — LittleFS prežil
  každý flash. Porty: COM5 = ProMicro sniffer, COM3 = XIAO bridge.
- Presety `shared/lora_presets.h`: SK = 869.618/SF8 (default), CZ = 869.525/SF7 (test bez rušenia
  SK siete); bridge je compile-time (`-DLORA_PRESET_CZ`), musí sedieť s targetom.
- Bugy nájdené testom: `ota_apply` pri zlyhaní nechával APPLYING (fix obnoví prev_status, 209b6b4);
  meshcore chunk CRC vs AES padding (159→160 B — presná dĺžka z idx/patch_size, aab1a9c);
  `pycryptodome` chýbal v penv (sender ticho poslal 0 paketov → `tools/requirements.txt`);
  detekcia build# pasívne z AALIVE (reboot 'b' resetoval recv_count — bitmap sa pre < 8 chunkov
  ukladá len pri complete).
- XIAO ako OTA-flash **cieľ** ostal neoverený (flasher_code.h bol ProMicro; pred flashom XIAO
  `BOARD_FLASHER=xiao python tools/build_flasher.py`). V MeshCore porte to rieši jeden
  board-agnostický flasher (app base runtime).

### Sniffer `main.cpp` — ovládanie a recovery (stav jún 2026)

Sériové klávesy: `a/m/d` filter ALL/MSG/DEC, `l` ALIVE heartbeat, `r` radio reinit, `s` soft restart
(SPI+NRESET+begin), `b` hard reboot, `o` OTA status, `P` OTA apply (SHA256 verify), `Q` OTA test
patch (bez flashu), `W` OTA flash, `C` OTA clear session (maže `/ota/*`).

Radio recovery (4 úrovne): L1 `radio.begin()` (CRC/header chyby) → L2 AGC reset
(`sleep + standby RC + calibrate`, stuck receiver so živým SPI) → L3 `SPI.end() + NRESET + SPI.begin()`
(zaseknutý bus) → L4 `SX126X_POWER_EN LOW 200 ms → HIGH` (čip neodpovedá, `-2`); auto-retry
s backoffom 5→10→20→40→60 s. Serial reconnect: pri otvorení monitora banner + force ALIVE.

Opravené bugy: dvojité `printHdr()` pri GRP_TXT/GRP_DATA; RAM meranie (`sbrk`/`mallinfo`/
`xPortGetFreeHeapSize` nefungovalo — odstránené); zostatok FS po OTA → `ota_clear_session()`;
ALIVE crash, keď LittleFS nezomountoval (`lfs_ptr` null guard); `ota_init()` ignoroval výsledok
druhého `OtaFS.begin()` po formáte.

**„Nič sa neukáže na sériovej konzole":** (1) hard fault v ALIVE handleri po 5 s (null `lfs_ptr`)
→ crash loop; (2) `while (!Serial && millis() < 3000)` je kratšie než USB re-enumerácia po
nrfutil flashi (2–4 s) → setup správy preč; reconnect banner v `loop()` to zachráni. Diagnóza:
otvor terminál **pred** RST. `(bool)Serial` závisí od DTR — terminál bez DTR banner nedostane.

## Port OTA do MeshCore (14. 6. 2026) a RF ladenie (15. 6. 2026)

Port do `examples/simple_repeater/nrfota/` (vetva `features/nrf-ota`, od 25. 6. `nrffota/` na
`features/nrf-fota`): GRP_DATA kanál, `searchChannelsByHash` + `onGroupDataRecv` hooky, CustomLFS
@ 0xD4000, flasher @ 0xEB000, odložené spracovanie v `loop()`. Platný popis: tech §1–§6.
Ladenie príjmu 15. 6. (mŕtvy RF spoj → asymetria → preamble omyl → AGC reset ako skutočná príčina
interakcie s flashom, `agc_reset=0`): [conv_claude_20260615.md](conv_claude_20260615.md) §3–§6
a tech §8.3–§8.5. Spevnenie testu (`broadcast_until_verified`, `--verify-first` opt-in):
[fcl_readme_verified_pooling.md](../fcl_readme_verified_pooling.md).

Poučenie z „preamble" omylu: reflash/power-cycle vyčistil jednorazovo zaseknutý receiver
(„stuck noise floor −120") a zmena preamble vyzerala ako fix. Pri RF diagnostike meniť jednu
premennú a overiť A/B/A — viď aj [lr2021-aba-rlwait-20260903.md](lr2021-aba-rlwait-20260903.md).

## Heltec V3 FK varianty (apríl 2026)

Vlastné `variants/heltec_v3_fk/` (BLE_PIN_CODE, debug flagy, bez displeja) existovali len na
vetve `test-stareho-stashu` (commity c6718592, b1888610 — záloha zo stashu). Na pracovných vetvách
`features/*` ani `test/lr2021-*` nie sú. Osobné konfigurácie, netreba generalizovať.

## Debug na ProMicro: SoftDevice reset pri breakpointe (apríl 2026)

MeshCore `companion_radio` na ProMicro nRF52840 sa pri zastavení debuggerom resetoval:
`RESETREAS = 0x04` (SREQ) = SoftDevice S140 zavolal `NVIC_SystemReset()` — pri zastavenom CPU
nestihne BLE timer interrupt → SD assert. WDT nebežal. Fault handler je v
`framework-arduinoAdafruit-nrf52/libraries/Bluefruit52Lib/src/bluefruit.cpp` (`nrf_error_cb`):
bez `CFG_DEBUG` prázdny (SD resetuje), s `CFG_DEBUG=3` vypíše detaily a uvisne vo `while(1) yield()`.
Možnosti: `Bluefruit.Advertising.stop()` pred breakpointom; `CFG_DEBUG=3`; flag bez BLE
(`DEBUG_NO_BLE` — SoftDevice sa nespustí, Serial voľný); `DEBUG_LOG_ONLY` (bez
`serial_iface.enable()`, `Serial.begin()` priamo, mesh beží). Plánovaný env
`promicro_companion_debug` sa nedokončil. RESETREAS čítať hneď v `setup()`, uložiť do GPREGRET,
vypísať až keď je Serial aktívny (COM port existuje až po `serial_iface.enable()`/`Serial.begin()`).

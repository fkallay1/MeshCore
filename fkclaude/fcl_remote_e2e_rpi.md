# Vzdialený 200 km test — stav, problémy a plán e2e/flash cez RPi

*(poznámky pre ďalšiu session; stav k 2026-07-17 večer, MC branch `features/nrf-fota` @ b3523fe2)*

## 1. Zostava

| Kus | Kde | Stav |
|---|---|---|
| **T1000-E repeater** (cieľ testu) | vzdialené stanovište ~200 km | beží **build #347** (fix 0x514, BEZ FK flagov); serial visí na RPi |
| **RPi (Zero?)** | pri vzdialenom repeateri | Fedor naň chodí cez **PuTTY (SSH)** a serial číta cez **picocom** |
| **Telefón + companion** (vysielač) | lokálne u Fedora | Flutter appka (meshcore-open fork), adb funguje z tohto PC |
| **Kontrolný ProMicro repeater** | lokálne u Fedora (2 m od vysielača) | **build #334** (spred 0x514 fixu — latentný USB-freeze bug, pri USB napájaní sa neprejaví; časom povýšiť) |
| RDP transfer `\\tsclient\D\10_RDP_Transfer` | — | **ku koncu session nefunkčný** (Access denied) — flash builds na vzdialené PC cezeň |

## 2. Otvorené problémy (kde sme skončili)

### 2.1 Login handshake cez flood na 200 km neprechádza
- Uplink floody dolietajú (ANON_REQ s path[3..10] na repeateri vidno), **spiatočný
  flood (PATH+RESPONSE) takmer nikdy nepríde** — 300 ms po requeste štartuje do jeho
  echo búrky + pozadová prevádzka. Manuálne cesty (setpath) fungujú.
- **Riešenie implementované, ČAKÁ NA HW TEST**: FK flagy (commit 0d9192db, build #348+):
  `FK_SERVER_FLOOD_RESPONSE_DELAY=1500` + `FK_ANON_FLOOD_DIRECT_FALLBACK=2000`
  (detaily `fcl_readme_tech_nrf-fota.md` §7.0b). Vzdialený repeater ich ešte NEMÁ (beží #347).
- Pri teste sledovať v logu: `FK anon fallback: armed / direct resend (N hops) / handshake OK`.

### 2.2 Flood chunky v RAW logu bez spracovania (mystérium)
- Na vzdialenom T1000-E: `RX RAW type=6 chah=A4 route=1 path[10]` **bez** `[FOTA] CHUNK` riadkov.
- Vylúčené: kľúč/formát (kontrolný ProMicro tie isté chunky dešifruje vždy — base
  mismatch drop na ňom je SPRÁVNY, chunky sú pre T1000-E base), appka (tsBase =
  epoch pri každom Send, `fota_screen.dart:493` — cross-attempt dedup fix z júna platí),
  region gating (REGION_DENY_FLOOD blokuje len forwarding, nie lokálne spracovanie).
- Zostávajúci kandidáti (všetci v builde #347 TICHÍ): **seen-table dedup** (neskoršie
  kópie toho istého chunku — na ProMicro logu vidno presne tento vzor: path[0] kópia
  spracovaná, path[1] kópie len RAW), **MAC fail** (poškodenie pri SNR −10), **packet
  pool empty** (print len pod MESH_DEBUG, ktorý je vypnutý).
- **Riešenie: diagnostický build #352** (commit b3523fe2) — pod `FK_DEBUG` vypisuje:
  `[FK] GRP_DATA A4: DEDUP (seen)` / `MAC fail (foreign/corrupt)` / `no matching channel`
  a `[FK] RX DROP: packet pool empty!`. Len GRP_DATA (GRP_TXT by spamoval).
  **#352 ešte nie je na zariadení.**

### 2.3 Vedľajšie zistenia
- Užívateľ omylom posielal `.rev.` (rollback) balík → base mismatch — zvážiť: pushovať
  do telefónu len upgrade balík, alebo v appke zvýrazniť smer (TODO do app handoff).
- GRP_DATA v sieti takmer nechodí (len naše) — RAW tag `GRP_DATA/FOTA` je ale len zhoda
  hash bajtu PRED dešifrovaním, skutočné „moje" rozhoduje MAC.
- upgrade #347→#348 na vzdialenom repeateri NEBOL dokončený (skúšal sa cez flood).

## 3. Artefakty pripravené v `test_nrf-fota/builds/`

| Súbor | Obsah |
|---|---|
| `t1000e.fw_348.uf2` | #348 = #347 + FK flagy (fallback+delay) |
| `t1000e.fw_352.uf2 / .hex / .zip` | #352 = #348 + FK diag výpisy; **.zip = DFU balík na serial flash cez RPi** |
| `fotapkg_json/347-348.*.json` | podpísaný patch (+rollback) — pushnutý v telefóne |
| `fotapkg_json/348-352.*.json` | podpísaný patch (+rollback) |
| CHÝBA `347→352` | ak sa má #352 nasadiť cez FOTA priamo z #347, vygenerovať: `python test_nrf-fota/gen_fotapkg.py --old test_nrf-fota/builds/t1000e.fw_347.bin --new test_nrf-fota/builds/t1000e.fw_352.bin --device t1000e` |

## 4. Plán: vzdialený e2e test cez RPi (Claude ovláda obe strany)

**Čo Claude potrebuje od Fedora (zatiaľ nedodané):**
1. SSH prístup na RPi z tohto PC: IP/hostname (VPN?), user, auth (heslo raz → nainštalujem kľúč).
2. Názov serial zariadenia na RPi (`/dev/ttyACM0`?).
3. Lokálny vysielač pre automatizáciu: companion/bridge na USB tohto PC + COM port
   (telefón viem len adb push, appku neovládam).

**Setup na RPi (zdieľaný prístup k serialu, žiadne bitky o port):**
```bash
tmux new -s rptr -d "picocom -b 115200 --imap lfcrlf --logfile /home/pi/rptr.log /dev/ttyACM0"
# Claude číta:   ssh pi@<rpi> "tail -f /home/pi/rptr.log"
# Claude píše:   ssh pi@<rpi> "tmux send-keys -t rptr 'fota status' Enter"
# Fedor pozerá:  ssh + tmux attach -t rptr
```

**Priebeh e2e:** poslať patch (fota_sender_mcpy / appka) → na RPi logu sledovať RAW +
`[FK]` diagnostiky + CHUNK → `fota miss`/doposlanie → `verify` → `flash` → z heartbeatu
overiť nový build#. Poznámka: FOTA flash = soft reset, bez rizika 0x514 triedy problémov.

## 5. Vzdialený FLASH cez RPi (rýchly, bez LoRa)

Adafruit bootloader podporuje **DFU cez serial** — presne to robí lokálny „NoBuild upload"
task. Na RPi:

```bash
# jednorazovo
pip3 install adafruit-nrfutil

# prenos balíka (z tohto PC)
scp test_nrf-fota/builds/t1000e.fw_352.zip pi@<rpi>:/tmp/

# flash (picocom NAJPRV odpojiť od portu! napr. tmux kill-session -t rptr)
adafruit-nrfutil dfu serial --package /tmp/t1000e.fw_352.zip -p /dev/ttyACM0 -b 115200 --singlebank --touch 1200
# --touch 1200 = 1200bps touch prepne bežiacu appku do bootloadera automaticky (netreba tlačidlo)
```

- `.zip` DFU balík vzniká pri každom builde (`.pio/build/<env>/firmware.zip`); pre #352 je
  odložený v `builds/t1000e.fw_352.zip`. Pre iný build bez rebuil-du: adafruit-nrfutil
  `dfu genpkg --dev-type 0x0052 --sd-req 0x0123 --application <hex>` (0x0123 = s140 v7;
  viď NoBuild task/`fcl` poznámky — zip→hex hack).
- Riziko: pri prerušenom DFU ostane zariadenie v bootloaderi — DFU sa dá zopakovať
  (bootloader beží ďalej), NIE je to brick. Horšie je len úplné odpojenie napájania
  RPi↔repeater — na to nikto na mieste nie je, takže flashovať len so stabilným napájaním.
- POZOR: kým appka nebeží (počas DFU), repeater nerepeatuje — okno ~1 min.

## 6. Prvé kroky ďalšej session

1. Vypýtať SSH údaje na RPi (bod 4) → setup tmux+picocom logfile.
2. Nasadiť **#352** na vzdialený repeater — ideálne DFU cez RPi (§5), inak FOTA
   (vygenerovať 347→352 patch, §3).
3. Zopakovať flood chunk test → prečítať `[FK]` výpisy → pomenovať bránu.
4. Otestovať login handshake s FK flagmi (fallback) na 200 km.
5. Po validácii: FK flagy zapnúť aj pre ostatné FOTA envy + zvážiť ZC mirror; povýšiť
   kontrolný ProMicro (#334 → aktuál).

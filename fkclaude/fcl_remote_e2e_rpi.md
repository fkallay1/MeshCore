# Vzdialený 200 km test — stav, problémy a plán e2e/flash cez RPi

*(poznámky pre ďalšiu session; stav k 2026-07-17 neskoro večer, MC branch `features/nrf-fota` @ b3523fe2)*

## 1. Zostava

| Kus | Kde | Stav |
|---|---|---|
| **T1000-E repeater** (cieľ testu) | vzdialené stanovište ~200 km | beží **build #352** (diag build; flashnutý DFU cez RPi 2026-07-17; predtým tam bol #348 — FOTA upgrade 347→348 sa teda DOKONČIL) |
| **RPi (WPSD hotspot `wpsd1ba`)** | pri vzdialenom repeateri | **10.21.0.103** cez WireGuard (Wg1; NIE .104!), user `pi-star`; SSH kľúč nainštalovaný → z tohto PC funguje `ssh rpi` bez hesla; Fedor naň chodí cez **PuTTY (SSH)** |
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

## 4. Vzdialený prístup cez RPi — NASTAVENÉ (2026-07-17)

**SSH:** RPi = `10.21.0.103` (WireGuard Wg1; pozor, NIE .104), user `pi-star`, host `wpsd1ba`
(Debian 13, WPSD hotspot). Kľúč `D:\FkDev\cli_home\.ssh\id_ed25519` je v authorized_keys;
ssh config alias je v `C:\Users\globesy\.ssh\config` (Windows ssh číta REÁLNY profil, nie
cli_home!) → z tohto PC stačí **`ssh rpi`** / `scp <súbor> rpi:/tmp/`.

- Serial: `/dev/ttyACM0` (T1000-E), user je v skupine `dialout`, sudo bez hesla, root FS rw.
- Nainštalované: `tmux`, `picocom` (bol), **adafruit-nrfutil v venv `~/nrfvenv`**
  (Debian 13 = externally-managed pip, preto venv): `~/nrfvenv/bin/adafruit-nrfutil`.
- POZOR pri generovaní kľúča z PowerShellu: `-N '""'` nastaví passphrase doslova `""`
  (kľúč sa v BatchMode neodomkne, sshd loguje „Connection reset [preauth]") — správne
  je `cmd /c 'ssh-keygen ... -N ""'`.

**Bežiaci setup (zdieľaný prístup k serialu, žiadne bitky o port):**
```bash
# tmux session "rptr": picocom v reštart-slučke (prežije FOTA flash / USB re-enum),
# log sa appenduje do /home/pi-star/rptr.log
tmux new -s rptr -d 'while true; do picocom -b 115200 --imap lfcrlf --logfile /home/pi-star/rptr.log /dev/ttyACM0; echo PICOCOM-EXIT, restart o 3 s; sleep 3; done'
# ~/.tmux.conf: history-limit 100000 + mouse on (nastavené 2026-07-18)
# Claude číta:   ssh rpi "tail -f /home/pi-star/rptr.log"     (alebo tail -50)
# Claude píše:   ssh rpi "tmux send-keys -t rptr 'fota status' Enter"
# Fedor pozerá:  PuTTY → 10.21.0.103, pi-star → `tmux attach -t rptr` (interaktívne,
#                dá sa písať CLI; ODPOJIŤ = Ctrl+B, potom D — NIE Ctrl+A/Ctrl+X, to zabije picocom)
#                alebo len na čítanie: `tail -f ~/rptr.log` (bezpečné paralelne s Claude)
# SCROLLBACK v tmux: koliesko myši (mouse on), alebo Ctrl+B [ → PgUp/šípky, koniec q
#                (v copy-mode ostávajú klávesy lokálne — do zariadenia NEIDÚ);
#                šípky MIMO copy-mode idú do CLI zariadenia! Celá história: less ~/rptr.log
# viacero tmux attach naraz je OK; NIKDY druhý picocom priamo na /dev/ttyACM0
# POZOR: pri DFU flashi slučka picocom hneď reštartuje a drží port — na DFU treba
#        tmux kill-session -t rptr (nie len čakať na exit picocomu)
```

**Priebeh e2e:** poslať patch (fota_sender_mcpy / appka) → na RPi logu sledovať RAW +
`[FK]` diagnostiky + CHUNK → `fota miss`/doposlanie → `verify` → `flash` → z heartbeatu
overiť nový build#. Poznámka: FOTA flash = soft reset, bez rizika 0x514 triedy problémov.

## 5. Vzdialený FLASH cez RPi (rýchly, bez LoRa) — OVERENÉ 2026-07-17 (#348→#352)

Adafruit bootloader podporuje **DFU cez serial** — presne to robí lokálny „NoBuild upload"
task. Postup (funkčný, ~40 s):

```bash
# prenos balíka (z tohto PC)
scp test_nrf-fota/builds/t1000e.fw_352.zip rpi:/tmp/

# flash (picocom NAJPRV odpojiť od portu!)
ssh rpi "tmux kill-session -t rptr; ~/nrfvenv/bin/adafruit-nrfutil dfu serial --package /tmp/t1000e.fw_352.zip -p /dev/ttyACM0 -b 115200 --singlebank --touch 1200"
# --touch 1200 = 1200bps touch prepne bežiacu appku do bootloadera automaticky (netreba tlačidlo)

# obnoviť logovanie + overiť build# (heartbeat AALIVE chodí ~každých 30 s)
ssh rpi "sleep 6; tmux new -s rptr -d 'picocom -b 115200 --imap lfcrlf --logfile /home/pi-star/rptr.log /dev/ttyACM0'"
ssh rpi "sleep 45; grep AALIVE ~/rptr.log | tail -2"
```

Pozn.: `ver` vracia stále „v1.16.0 (Build: 6 Jun 2026)" — dátum sa nemení, build#
vidno len v `AALIVE` heartbeate (reboot poznať podľa resetu `rawrx` počítadla).

## 5a. AUTOMATIZOVANÉ: orchestrátor `fota_remote_e2e.py` (2026-07-18)

Celý postup z §5/§5b je zabalený do **`test_nrf-fota/fota_remote_e2e.py`** + skill
**`/remote-e2e`** (`.claude/skills/remote-e2e/SKILL.md`). Zariadenia/role/cesty:
`test_nrf-fota/fota_devices.json` (target/sender/monitor; transport com/rpi; kind →
build env). Beží aj bez Claude:

```bash
PENV="D:/FkDev/.platformio/penv/Scripts/python.exe"
$PENV test_nrf-fota/fota_remote_e2e.py setup                      # bootstrap všetkého
$PENV test_nrf-fota/fota_remote_e2e.py status                     # build#, sha, fota stav
$PENV test_nrf-fota/fota_remote_e2e.py flash-dfu --latest         # DFU flash cez RPi/COM
$PENV test_nrf-fota/fota_remote_e2e.py e2e --rebuild --monitor promicro-local   # plný test
```

Časové pečiatky: lokálne logy `test_nrf-fota/logs/<dev>.log` (čas PC),
na RPi `rptr.log.ts.log` (čas RPi/NTP, tmux okno `ts`) — porovnávanie TX↔RX naprieč
stanovišťami.

**Druhé stanovište `xiao-5km` (192.168.91.32, host `wpsd`) — NASTAVENÉ 2026-07-18:**
kľúč nasadený, venv je symlink `~/nrfvenv → ~/meshcore-cli/env` (premenovanie by
rozbilo shebangy), tmux+picocom+ts cez `setup`. **E2E PASS #361→#362 za 163 s** —
`probe-path` našiel cestu (**1 hop cez `21`**, flood sondy počuli aj 2132/2162/2173/6321),
direct doručenie **5/5 na prvý prechod**. Pôvodný #263 povýšený DFU (`flash-dfu --latest`),
lebo patch 263→361 vyžadoval extraSafe 24 KB → starý 4 KB flasher v #263 by ho odmietol
(0xE5) a 4 KB-kompat variant mal 455 chunkov (~hodina).

Nové subcommandy: `build -d a,b|all`, `flash-dfu -d all`, `probe-path -d <dev>`
(flood sonda → návrh path_to z RAW logu targetu; toleruje starý formát bez /FOTA tagu).
Skill `/build-flash` = priamy build+flash bez FOTA. POZOR: upstream merge rozbil
`Xiao_nrf52_repeater*` envy (repeater_btn PR nepridal xiao variantu `user_btn`) —
opravené u nás 6614aea5, kandidát na ďalší upstream PR.

## 5b. FOTA cez LoRa na 200 km — pracovný postup (2026-07-18, dnes robí orchestrátor)

Vysielač = companion na **COM3** tohto PC (`fota_sender_mcpy.py`). Živá sieť →
**`--delay 5`** (1 paket / 5 s, nikdy rýchlejšie!). Známe cesty: tam=`632139779C`,
späť=`C0777363`. Repeater = T1000-E, build# viď AALIVE.

```bash
PENV="D:/FkDev/.platformio/penv/Scripts/python.exe"

# 1. prvý prechod (celý patch): flood — na 200 km prešlo len ~20 % chunkov
$PENV test_nrf-fota/fota_sender_mcpy.py --old test_nrf-fota/builds/t1000e.fw_352.bin \
    --new test_nrf-fota/builds/t1000e.fw_355.bin --port COM3 --delay 5 \
    --scope flood --privkey test_nrf-fota/test_key.der

# 2. stav + chýbajúce chunky (cez RPi tmux CLI)
ssh rpi "tmux send-keys -t rptr 'fota missall' Enter; sleep 3; tail -4 ~/rptr.log"

# 3. doposlanie LEN chýbajúcich, direct po známej ceste (doručuje ovela lepšie než flood)
#    --chunks berie priamo výstup missall; META/SIG sa pri --chunks preskakuje
#    (--with-header ich pridá)
$PENV test_nrf-fota/fota_sender_mcpy.py --old ... --new ... --port COM3 --delay 5 \
    --scope direct --path 632139779C --chunks "1-6,9,10,12-14,..."

# 4. opakovať missall→--chunks kým 50/50, potom verify+flash cez tmux CLI
```

**Výsledok 2026-07-18 (patch 352→355, 7120 B / 50 chunkov):** flood 1. prechod 10/50
(~20 %), direct kolá ~10–40 % (kolíše), spolu 7 kôl ≈ 25 min → 50/50, `fota verify`
dry-run OK (SHA sedela). Doručenie potvrdené aj FK diagom: 1× `MAC fail
(foreign/corrupt)` (poškodený chunk), žiadny DEDUP/pool drop — mystérium §2.2 sa
pri delay=5 s neprejavilo.

**POZOR — CLI lockup bug (opravený v #356, eb0dffd5):** ak sa do serial CLI dostane
≥159 znakov bez `\r` (napr. **šípky stlačené v attachnutom tmux mimo copy-mode —
ESC[A sekvencie idú rovno do picocomu/zariadenia!**), CLI sa zasekne až do rebootu
(buffer-full vetva dávala `\r` na zlý index). Presne to zožralo náš `fota flash`
→ #355 sme nakoniec flashli cez serial DFU (§5; 1200 bps touch beží mimo CLI,
funguje aj so zaseknutým CLI). Skrollovať v tmux len cez copy-mode (`Ctrl+B [`,
odchod `q`) — tam šípky ostávajú lokálne.

- `.zip` DFU balík vzniká pri každom builde (`.pio/build/<env>/firmware.zip`); pre #352 je
  odložený v `builds/t1000e.fw_352.zip`. Pre iný build bez rebuil-du: adafruit-nrfutil
  `dfu genpkg --dev-type 0x0052 --sd-req 0x0123 --application <hex>` (0x0123 = s140 v7;
  viď NoBuild task/`fcl` poznámky — zip→hex hack).
- Riziko: pri prerušenom DFU ostane zariadenie v bootloaderi — DFU sa dá zopakovať
  (bootloader beží ďalej), NIE je to brick. Horšie je len úplné odpojenie napájania
  RPi↔repeater — na to nikto na mieste nie je, takže flashovať len so stabilným napájaním.
- POZOR: kým appka nebeží (počas DFU), repeater nerepeatuje — okno ~1 min.

## 6. Ďalšie kroky

1. ~~SSH setup + tmux/picocom logfile~~ HOTOVO (§4).
2. ~~Nasadiť #352~~ → HOTOVO; 2026-07-18 už beží **#355** (FOTA doručenie+verify cez
   LoRa OK, finálny flash cez DFU kvôli CLI lockup bugu — viď §5b).
3. ~~Flood chunk test~~ — FK diag bežal: pri delay=5 s žiadne DEDUP/pool dropy, 1×
   MAC fail; §2.2 mystérium bolo pravdepodobne spôsobené rýchlym posielaním (dedup
   neskorších kópií) — potvrdiť pri rýchlejšom teste, ak ešte treba.
4. Otestovať login handshake s FK flagmi (fallback + delay, sú v #348+) na 200 km —
   sledovať `FK anon fallback: armed / direct resend (N hops) / handshake OK`.
   V #355 je aj `[FK] PATH RX/TX` dekódovanie (tech doc §7.0a) — uvidno obe strany
   handshaku.
5. ~~Nasadiť fix CLI lockup~~ HOTOVO — **2026-07-18 prvý KOMPLETNÝ 200 km e2e**:
   FOTA 355→**358** (#358 = merged upstream/dev ab156acc + CLI fix + PATH decode),
   44 chunkov direct po `632139779C`, 1. prechod 50 %, spolu 6 kôl `--chunks`
   ≈ 12 min → 44/44 → `fota verify` OK → **`fota flash` cez CLI** → AALIVE #358.
   POZOR: FOTA flash (USB re-enumerácia) zhodí picocom aj tmux server — po flashi
   session založiť nanovo (§4).
6. Po validácii: FK flagy zapnúť aj pre ostatné FOTA envy + zvážiť ZC mirror; povýšiť
   kontrolný ProMicro (#334 → aktuál).

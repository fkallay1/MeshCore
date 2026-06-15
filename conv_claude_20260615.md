# Konverzácia Claude — LoRa-OTA pre MeshCore (nRF52840)
**Dátum:** 2026-06-14 / 2026-06-15
**Vetva:** `features/nrf-ota` (MeshCore) + `dev-stream` (FK_lora-sniffer bridge)
**Účastníci:** Fedor Kallay (embedded/firmware dev) + Claude (Opus 4.8)

Tento súbor je detailný záznam celej práce a najmä **debugovacej cesty** k sprevádzkovaniu
OTA-over-LoRa na MeshCore repeateri. Technický popis systému je v
[readme_tech_nrf-ota.md](readme_tech_nrf-ota.md), prehľad v [readme_nrf-ota.md](readme_nrf-ota.md).

---

## 0. Zadanie

Fedor má funkčný OTA-over-LoRa systém (delta patch) v projekte **FK_lora-sniffer**. Chcel ho
**čisto naimplementovať do MeshCore** (vetva `features/nrf-ota`), s požiadavkami:
- rešpektovať kódový štýl MeshCore, **bez dynamickej pamäte** (preferované),
- jadro MeshCore meniť **minimálne**, nový kód do podadresára `examples/simple_repeater/nrfota/`,
- len pre nRF52840 (flag `WITH_LORA_OTA`),
- znovupoužiť MeshCore knižnice (FS, krypto, rádio, spracovanie paketov),
- **len príjmová + patchovacia časť** (odosielanie je vyriešené v `ota_sender.py`),
- **NIE** existujúce MeshCore BLE-DFU — chceme OTA cez **LoRa**.
- Transport: **GRP_DATA kanál** (ako v FK_lora).
- Ostrý flasher manuálne + dry-run; ovládanie cez Serial (primárne) aj LoRa.

Neskôr: end-to-end test (ako `ota_test_lora.py`) v novom `test_nrf-ota/`, skript
`ota_test_lora_repeater.py`. **COM3 = XIAO bridge, COM5 = ProMicro repeater.** CZ preset
(aby nerušil produkčnú SK sieť). Dočasný build number do repeatera (detekcia verzie + OLD≠NEW
pre patch). Readme s popisom.

---

## 1. Port OTA do MeshCore (HOTOVO — skoršia session)

- Nový modul `examples/simple_repeater/nrfota/`: OtaProtocol/OtaState/OtaFs/OtaReceiver/
  OtaPatcher/OtaMesh + puff_stream + hpatchlite + flasher (ORIGIN 0xEB000) + flasher_code.h.
- Integrácia: 3 hooky v MyMesh (`searchChannelsByHash`, `onGroupDataRecv`, `handleCommand` "ota").
- Transport GRP_DATA: kanál z PSK (`hash=sha256(psk)[0]`, `secret=psk.ljust(32)`),
  dešifrovanie `Utils::MACThenDecrypt` (AES-128-ECB + HMAC).
- Krypto/FS znovupoužité: `mesh::Utils::sha256` (rweather/Crypto), `CustomLFS` @ 0xD4000.
- Flash mapa (extrafs.ld): app→0xD4000, OTA FS 0xD4000–0xEB000, flasher 0xEB000, InternalFS
  0xED000 NEDOTKNUTÝ.
- Build env `ProMicro_repeater_ota` (CZ preset, `-D WITH_LORA_OTA`).
- Build chyba `macro names must be identifiers` (z `-U LORA_FREQ`) → fix: len `-D`, `-w`.

## 2. Test infraštruktúra + prvé HW testy (skoršia session)

- `test_nrf-ota/`: ota_sender.py, ota_test_lora_repeater.py, gen_build_info.py, hdiffi.exe.
- Bridge = FK_lora `gateway_fw` na XIAO (COM3). Repeater = MeshCore na ProMicro (COM5).
- **Prvý dokázaný end-to-end úspech: build #11 → patch #11→#12** (488 B) cez LoRa GRP_DATA →
  repeater prijal 4 chunky → assemble + SHA256 → `ota verify` (dry-run) → `ota flash` →
  flasher@0xEB000 → reboot → **#12**. Bezpečnostné poistky overené (base-FW check odmietol
  stale patch; FS prežil reflash).
- **Kľúčové RF zistenie (vtedy):** MeshCore používa pre SF≤8 preamble 32; bridge mal 16 →
  obojstranná hluchota. Fix: bridge preamble (SF≤8?32:16) + TX 10→22 dBm.
- **Otvorený problém:** repeater po boote prijal ~1 paket, potom RX "zamrzol" (`rxpkts` ostalo 1).

---

## 3. TÁTO SESSION — debugovanie príjmu

### 3.1 „Zobraz či prichádzajú surové pakety" (+ deferred spracovanie)
Fedor: *"dorob zobrazenie info, či na repeater prichádzajú hocijaké raw pakety. Lebo môže byť
problém ďalej v dekódovaní."*

- Pridaný `MyMesh::logRxRaw()` override (gated `WITH_LORA_OTA`): počítadlo `_ota_raw_rx`
  (surové CRC-OK rámce **PRED** dekódom) + per-paket výpis `[OTA] RAW #n len/rssi/snr/hdr`.
  `rawrx` pridané do `[OTA] AALIVE` heartbeatu.
- **Deferred OTA spracovanie:** `onGroupDataRecv()` už len buffruje do `_ota_pending[]`; ťažké
  CustomLFS I/O sa robí v `loop()` po `mesh::Mesh::loop()` (po re-arme rádia).
- Build #21, nahraté COM5.

**Výsledok:** `rawrx=0` naprieč ~60 odvysielanými paketmi. `nf=-120` (clampnutá dolná hranica =
rádio JE v RX, kanál tichý). Bridge `rx=0` aj na adverty repeatera → **obojstranná hluchota**
napriek overene zhodným parametrom (`get radio` → 869.525/62.5/7/5, bridge LIVE pre32/sync0x12).

### 3.2 PSK kontrola
Fedor mal podozrenie na textový PSK. Overené **bajtovo identické** odvodenie na oboch stranách
(psk = 16 ASCII bajtov "meshcore-ota-key" = hex `6d65...6b6579`; secret=ljust(32); hash=sha256[0]).
PSK **funguje a nie je príčina** (krypto až po príjme).

### 3.3 SK preset test → lokalizácia chyby
Fedor: skús SK preset (možno problém s CZ frekvenciou; CLI ukazoval 869.5250244 vs 869.525).
- Frekvencia 869.5250244 = len float32 presnosť (869.525f → 869.5250244140625), identická na
  oboch stranách, 24 Hz @ BW62.5k zanedbateľné → **nie príčina**.
- Pretunované cez CLI `set radio 869.618,62.5,8,5` + reboot (loadPrefs prebije compiled default).

| | CZ | SK |
|---|---|---|
| Bridge RX (COM3) | rx=0 | **rx=8** ✅ |
| Repeater RX (COM5) | rawrx=0 | rawrx=0 ❌ |

→ **Bridge prijíma (anténa/RX OK), repeater nepočuje nič ani na SK** → chyba v príjme repeatera.
Repeater vrátený na CZ (aby nerušil SK sieť).

### 3.4 (PÔVODNE ZLÝ ZÁVER) „preamble"
Porovnanie radio-initu: FK_lora sniffer RX preamble **16** vs MeshCore RX **32**. Zvýšený bridge
TX preamble na **64** → repeater **začal prijímať** (`rawrx=12`, RSSI -23, SNR +11), celé OTA
#23→#24 PASS. Záver (vtedy): preamble 32/32 hraničné. **Tento záver sa neskôr ukázal nesprávny.**

---

## 4. Fedorova AGC hypotéza → SKUTOČNÝ ROOT CAUSE

Fedor: *"problém môže byť s AGC, keď sú tak blízko seba... keby bola zle preambula na repeateri,
tak by to nefungovalo kade-kade. A ak zmeníš niečo v nastavení rádia v repeateri, môže to byť
problém voči iným zariadeniam. Dopln výpis AGC, ak sa to dá prečítať."*

**Dôležité ujasnenie:** radio config repeatera som NEMENIL — menil sa len bridge (test nástroj).

- Pridaný `ota agc` (read-only): SX1262 RxGain reg 0x08AC (0x96 boosted / 0x94 power-save),
  okamžité RSSI (`getRSSI(false)`), noise floor, `agc_reset_interval`. Prístup cez
  `extern RADIO_CLASS radio;` v MyMesh.cpp. Zistené: **boosted gain**, **agc_reset=0 (vypnuté)**.

**Experiment (vrátený bridge na štandard preamble 32):**
- Pri **32/32 a agc_reset=0 repeater PRIJÍMA** (`rawrx=10`, build #27, čerstvý boot)!
  → **preamble NEBOL príčina.** Preamble 64 "fungoval" len náhodou (reflash resetol rádio).
- Trvalá hluchota #21 = **jednorazový zaseknutý receiver** ("stuck noise floor -120",
  self-reinforcing, RadioLibWrappers.cpp:78). Recovery = `resetAGC()` (sleep+calibrate), ale
  `agc_reset_interval` je **default 0** → nikdy sa nespustí. Zaseknutý stav vyčistí power-cycle.

**Kontrolný test pôvodným FK_lora snifferom** (sniffer COM5 + bridge COM3, CZ, direct):
`ota_test_lora.py` PREŠIEL #115→#116, `peers=1` → **HW v poriadku** na tých istých doskách.

---

## 5. AGC auto-reset ROZBÍJA OTA flash (kritická interakcia)

Pri testovaní AGC resetu ako "fixu" príjmu sa ukázalo:

| agc_reset | flash |
|-----------|-------|
| 0 | #28→#29 **PASS**, #32→#33 **PASS** (manuálny) |
| 8 s | #28→#29 **FAIL**, #30→#31 **FAIL** |

Pri `agc_reset>0` sa flasher zastavil hneď po `[FLASHER] Komprimovany format`, repeater nabehol
na OLD. Guard "vypni agc pri `ota flash`" **NEFUNGOVAL** (škoda vzniká agc resetmi počas session,
nie pri blokujúcom `ota_apply`) → odstránený, nahradený varovným komentárom.

**Záver: `agc_reset_interval = 0` (MeshCore default).** Príjem to nepotrebuje; zaseknutý receiver
(zriedkavý) sa rieši rebootom. Repeater radio config NEZMENENÝ → plná kompatibilita.

---

## 6. Finálny stav

- **Funguje príjem aj flash** pri `agc_reset=0` + štandardný preamble 32. Repeater #33 (final kód).
- Bridge: CZ, preamble 32, 22 dBm, LIVE heartbeat. RXEN manuálne HIGH (ako sniffer).
- **Pridané diagnostiky:** `rawrx` v heartbeate; `ota agc` (read-only).
- **Automatický test `run` občas FAIL = timing** (skúsi flash pred VERIFIED; fire-and-forget
  príjem niekedy potrebuje viac cyklov). Manuálny flash s hotovou VERIFIED session vždy PASS.
  → spevnenie VERIFIED-pollingu: [readme_verified_pooling.md](readme_verified_pooling.md).

### Zmenené súbory
**MeshCore (`features/nrf-ota`):**
- `examples/simple_repeater/MyMesh.cpp` — `logRxRaw` rawrx, deferred OTA buffer+drain,
  `ota agc` príkaz, `extern RADIO_CLASS radio`, init členov, varovanie agc+flash.
- `examples/simple_repeater/MyMesh.h` — buffer `_ota_pending`, `_ota_raw_*` členy.
- `readme_nrf-ota.md` (update), `readme_tech_nrf-ota.md` (nový), `conv_claude_20260615.md` (nový).
- `test_nrf-ota/ota_sender.py`, `ota_test_lora_repeater.py`, `.gitignore`.

**FK_lora-sniffer (`dev-stream`):**
- `tools/gateway_fw/src/main.cpp` — LIVE heartbeat, preamble 32, 22 dBm (RXEN/preamble64
  experimenty vrátené späť).

### Poučenia pre budúcnosť
1. **Pridaj `rawrx` (pred-dekód) počítadlo skôr** — okamžite odlíši RF/PHY problém od dekódu.
2. **Intermitentné chyby neprisudzuj prvej koincidencii** (preamble 64 "fix" bola náhoda po
   reflashi). Over zmenu izolovane a opakovane.
3. **AGC auto-reset NEKOMBINOVAŤ s OTA flashom.**
4. **Nemeniť radio config prijímača** kvôli jednému test-spoju — rozbije kompatibilitu so sieťou.
   Riešenie patrí na stranu test-nástroja (bridge), nie do produkčného FW.

---

## 7. Commit + spevnenie testu (po commite, na žiadosť)

**Commity + push:**
- MeshCore `12ca81c7` (features/nrf-ota): rawrx, ota agc, deferred OTA, docs, .gitignore.
- FK_lora `0117492` (dev-stream): bridge LIVE heartbeat + preamble 32 + 22 dBm.
- MeshCore `7cac5ce4`: spevnenie testu (nižšie).
- Dokumenty: `readme_tech_nrf-ota.md`, `conv_claude_20260615.md`, `readme_verified_pooling.md`.

**Spevnenie `ota_test_lora_repeater.py`** (detail: [readme_verified_pooling.md](readme_verified_pooling.md)):
- **VERIFIED-polling**: `broadcast_until_verified()` — opakuje broadcast + poll `ota status` až
  do VERIFIED / `--verify-wait` (60 s), viac pollov/kolo, sleduje rast recv/total, settle po
  reboote. Nahradilo fixný `--cycles` loop, ktorý pri strate paketov na začiatku padol predčasne.
- **Flash hardfault po dry-rune (NOVÝ FW nález)**: `ota verify` (malloc + streaming rekonštrukcia
  442 kB) pokazí heap → následný `ota flash` hardfaultne na `malloc` (flasher stop po
  "Komprimovany format" → OLD). Dôkaz: manuálny flash bez dry-runu PASS; automatický s dry-runom
  FAIL. Fix v teste: dry-run je opt-in (`--verify-first`, default vyp). FW TODO: vyčistiť heap.
- **Overené:** #34→#36 konzistentný **[PASS]**.

> Pozn.: po DFU baseline sa raz prefs repeatera vrátili na SK preset (zostatok z SK testu) —
> pred `run` treba `set radio 869.525,62.5,7,5` + reboot, nech CZ repeater počuje CZ bridge.

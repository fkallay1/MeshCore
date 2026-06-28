# E2E FOTA test — runbook (MeshCore nRF52840 repeater, LoRa delta-patch)

Samostatný návod ako spustiť a overiť end-to-end FOTA test cez LoRa. Použiteľný ako vstup pre
Claude Code session alebo pre človeka. Prehľad systému: [fcl_readme_nrf-fota.md](fcl_readme_nrf-fota.md),
technika: [fcl_readme_tech_nrf-fota.md](fcl_readme_tech_nrf-fota.md), spevnenie testu:
[fcl_readme_verified_pooling.md](fcl_readme_verified_pooling.md).

## Topológia
```
PC ──USB(COM3)── XIAO bridge ──LoRa(CZ)── ProMicro repeater ──USB(COM5)── PC
```

## Zariadenia a ich úloha
| Port | HW (VID:PID) | Úloha | Firmware |
|------|--------------|-------|----------|
| **COM3** | Seeed XIAO nRF52840 (`2886:8044`) | **Bridge / vysielač** — PC pošle cez USB rámec `[0xAB 0xCD len data]`, XIAO ho odvysiela ako surový LoRa paket. Naťukáva FOTA pakety do éteru. | FK_lora `gateway_fw` (env `Xiao_bridge`), **CZ preset** |
| **COM5** | Adafruit nRF52840 ProMicro (`239A:00B3`) | **Repeater / cieľ (DUT)** — MeshCore, prijíma FOTA `GRP_DATA` cez LoRa, skladá patch, overí SHA256/podpis, flashne nový FW. Tu sa testuje. | MeshCore env `ProMicro_repeater_fota` (`WITH_LORA_FOTA`), **CZ preset** |

> Porty identifikuj podľa **VID:PID** (robustné), nie podľa čísla — COM3/COM5 sa môžu pri prepojení zmeniť.
> Detekcia (PowerShell): `Get-CimInstance Win32_SerialPort | Select DeviceID,PNPDeviceID`.
> COM4 (Intel AMT SOL) ignoruj — nie je to naše zariadenie.

**Obe zariadenia musia byť na CZ presete:** `869.525 MHz, SF7, BW62.5, preamble 32`
(aby sa počuli navzájom a nerušili SK produkčnú sieť).

## Prostredie (stav 2026-06, mimo OneDrive)
- Projekt: `D:\FkDev\FkProj\VSC\MeshCore`
- Penv python (pyserial + pycryptodome + platformio): `D:\FkDev\.platformio\penv\Scripts\python.exe`
- FK_lora bridge zdroj (len ak reflashuješ bridge): `D:\FkDev\FkProj\VSC\FK_lora-sniffer`
  (runner ho berie z env premennej `PLATFORMIO_SETTING_PROJECTS_DIR`, fallback = súrodenec MeshCore)

## Predpoklady (must-have)
1. **COM3 aj COM5 voľné** — zatvor Serial Monitor / iné terminály.
2. `pip install -r test_nrf-fota/requirements.txt` v penv (pyserial, pycryptodome).
3. `test_nrf-fota/hdiffi.exe` prítomný.
4. **`test_nrf-fota/test_key.der` prítomný** ⚠️ — je **GITIGNORED**, na čerstvom klone/po presune CHÝBA.
   Bez neho ide nepodpísaný HEADER → repeater odmietne (CHYBA=0x6) → nikdy VERIFIED. Musí to byť
   kľúč, ktorého pubkey = `c22f8ae0…5b51` (zakompilovaný v
   `examples/simple_repeater/nrffota/FotaReceiver_signkey.cpp`, `key_id=1`). Skopíruj z funkčného prostredia.
5. XIAO už beží ako CZ FK_lora bridge → použi `--skip-bridge`. Inak vynechaj `--skip-bridge` —
   runner bridge postaví+nahrá (potrebuje FK_lora-sniffer projekt).
6. Repeater FW musí obsahovať **flasher IRQ fix** (commit `d24c6792`), inak `ota flash` zamrzne.

## Spustenie (dvojfázové)
```bash
PENV="D:/FkDev/.platformio/penv/Scripts/python.exe"

# Fáza 1 — baseline: build+DFU OLD repeater (COM5), ulož OLD obraz, ota clear, reboot
"$PENV" test_nrf-fota/fota_test_lora_repeater.py baseline --skip-bridge --target-port COM5

# Fáza 2 — run: build NEW, patch OLD→NEW, broadcast cez bridge → VERIFIED → ota flash → reboot → over
"$PENV" test_nrf-fota/fota_test_lora_repeater.py run --skip-bridge \
        --target-port COM5 --bridge-port COM3 --verify-wait 220
```
Runner je **samobežný**: default `--privkey test_key.der`, `--packetorder hend`, reboot pred každým
broadcast kolom. Žiadne ďalšie flagy netreba.

## Čo robia fázy
- **baseline** — postaví+nahrá OLD repeater (DFU cez COM5), uloží OLD app obraz (`lora_old.bin`) +
  build#, vyčistí FOTA session (`ota clear`), reboot. `--skip-bridge` predpokladá, že XIAO už beží ako CZ bridge.
- **run** — postaví NEW (`lora_new.bin`), vyrobí delta-patch OLD→NEW (`hdiffi -inplace` + zlib),
  broadcastuje cez bridge ako **podpísaný** GRP_DATA, čaká na VERIFIED (poll `ota status` cez COM5),
  spustí `ota flash`, po reboote overí build# == NEW + FNV-1a.

## Ako vyzerá ÚSPECH (PASS)
- run skončí: **`[PASS] Repeater nabehol na build #N — FOTA cez LoRa funguje! 🎉`**
- Medzistavy: `ota status` → **`4/4 st=0x07`** (VERIFIED); po `ota flash` sa objaví
  **`[FOTA] AALIVE build #N`** (NEW build#, vyšší než OLD); FNV-1a očakávaný == flasher výstup.

## Časté problémy / diagnostika
- **Zasekne sa na 2-3/4 chunkoch, „VERIFIED nedosiahnuté":** slabý RF. SX1262 po čerstvom boote
  chytí len ~3-4 rámce (stuck receiver, §8.3). Cieľ RSSI ≈ **-23** (v logu `RAW … rssi=`).
  Pri -26 to chce viac kôl → zvýš `--verify-wait` (300+), priblíž dosky / dotiahni antény (U.FL).
- **`total=0` / „HEADER INVALID signature, CHYBA=0x6":** chýba/nesedí `test_key.der`.
- **Po `ota flash` repeater mlčí (USB zombie, treba fyzický reset):** flasher zamrzol (chýba IRQ fix
  alebo iný flash problém). Obnova: **dvojklik RESET** na ProMicro → UF2 disk `PROMICRO` →
  `pio run -e ProMicro_repeater_fota -t upload` (DFU).
- **`ota dbg` trace „prázdny" + GPREGRET2=0x1 aj pri úspechu** — neinformatívne, normálne.
  Reálny signál úspechu = nabehne nový build#.

## Manuálne CLI príkazy na repeateri (COM5, serial)
`ota status | verify | flash | clear | nack | miss | missall | dbg | agc`
- `ota status` — recv/total/stav (`st=0x07` = VERIFIED).
- `ota verify` — dry-run rekonštrukcia patchu + SHA256, **bez zápisu** (bezpečné).
- `ota flash` — **OSTRÝ** flash + reboot (gatuje na VERIFIED).
- `ota miss` / `missall` — chýbajúce chunky ako rozsahy „od-do" (miss = strop 20 tokenov, missall = všetky).
- `ota clear` — zmaže FOTA session (`/ota/*`).

---
name: build-flash
description: Use when Fedor asks to build and/or directly flash firmware to his MeshCore devices (USB/serial DFU, NOT over-the-air) — "postav firmware", "flashni companion", "flashni všetky zariadenia", "povýš ProMicro". Uses test_nrf-fota/fota_remote_e2e.py (build / flash-dfu) with devices from fota_devices.json.
---

# Build + priamy flash zariadení (bez FOTA/LoRa)

Nástroj: **`test_nrf-fota/fota_remote_e2e.py`** (penv python
`D:\FkDev\.platformio\penv\Scripts\python.exe`, z koreňa repa). Zariadenia:
`test_nrf-fota/fota_devices.json` — `-d` berie meno, zoznam `a,b,c` alebo `all`.

## Príkazy

| Požiadavka | Príkaz |
|---|---|
| „postav FW pre X" (len preklad) | `build -d X` |
| „postav všetko" | `build -d all` (envy sa deduplikujú) |
| „flashni X najnovším buildom" | `flash-dfu -d X --latest` |
| „flashni X čerstvým buildom" | `flash-dfu -d X --rebuild` |
| „flashni X konkrétny build" | `flash-dfu -d X --build 358` |
| „flashni všetky" | `flash-dfu -d all --latest` (ide postupne) |
| „flashni companion (COM3)" | `flash-dfu -d companion --rebuild` |

Funguje pre lokálne COM zariadenia aj vzdialené za RPi (DFU cez SSH) — transport
rieši config. Po flashi repeatra skript sám overí build# cez `fota id`.

## Pravidlá a pasce

- **COM port musí byť voľný** (zavri Serial Monitor/putty). Na RPi skript sám
  zastaví picocom session a po flashi ju obnoví. Lokálne konflikty rieši **serial
  hub** (`hub -d <dev>`, `fota_serial_hub.py`): hub drží port trvalo, orchestrátor
  aj Fedor (PuTTY Raw `localhost:<hub_port>`) idú cez neho a DFU si port vypýta
  automaticky (PAUSE/RESUME). Ak hub beží, nič ďalšie neriešiš.
- **Vzdialený DFU len pri stabilnom napájaní** — prerušené DFU nechá zariadenie
  v bootloaderi (dá sa zopakovať, nie je to brick, ale bez FW nerepeatuje).
- **`--rebuild` na FOTA envoch bumpne build#** (auto-hook navyše vygeneruje fotapkg
  posledných 2 buildov). Flash bez bumpu = `--latest`/`--build N` z archívu;
  úplne bez prekladu z .pio je aj VSCode „NoBuild upload" task.
- **Companion a iné ne-FOTA envy** nemajú builds/ archív (`pio_zip` v kinds) —
  flashuje sa `.pio/build/<env>/firmware.zip`, typicky s `--rebuild`; overenie
  `fota id` sa preskakuje (nie je repeater FW).
- **ESP32 (Heltec)**: kind s `"method": "pio-upload"` — flash cez `pio run -t upload
  --upload-port COMx`, len lokálne. nRF52 ide serial DFU (`--touch 1200`, netreba tlačidlo).
- Nové zariadenie/typ: pridaj do `fota_devices.json` (devices + kinds env mapa).
- Kontrolný ProMicro (COM5) beží starý #334 — pri najbližšom flashi ho povýš na aktuál.

## Súvisiace

- FOTA cez LoRa (vzdialený update vzduchom) = skill `remote-e2e` (send/e2e).
- Runbook vzdialeného pracoviska: `fkclaude/fcl_remote_e2e_rpi.md`.

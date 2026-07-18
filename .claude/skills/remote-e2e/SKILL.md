---
name: remote-e2e
description: Use when Fedor asks to flash/test/check a MeshCore repeater remotely or locally — "flashni remote repeater", "sprav e2e test", "aký build tam beží", "pošli FOTA patch", "pozri log repeatera". Operates test_nrf-fota/fota_remote_e2e.py (devices in fota_devices.json).
---

# Vzdialené/lokálne FOTA operácie na repeateroch

Všetku mechaniku rieši **`test_nrf-fota/fota_remote_e2e.py`** (spúšťaj penv pythonom
`D:\FkDev\.platformio\penv\Scripts\python.exe`, z koreňa repa). Zariadenia a role sú
v `test_nrf-fota/fota_devices.json` — pred akciou over, že zariadenie tam je; nové
zariadenie najprv doplň do configu (kind → env/prefix mapa je tamtiež).

## Preklad požiadaviek na subcommandy

| Fedor povie | Sprav |
|---|---|
| „aký je stav / aký build tam beží" | `status -d <dev>` |
| „flashni <dev> aktuálnym buildom" | `flash-dfu -d <dev> --latest` (nič nekompiluje) |
| „flashni <dev> čerstvým buildom" | `flash-dfu -d <dev> --rebuild` (bumpne build#) |
| „pošli FOTA patch / oper cez LoRa" | `send -d <dev>` (doručenie + miss-loop, bez flashu) |
| „sprav celý e2e test" | `e2e -d <dev>` (send → verify → flash cez CLI → over build#) |
| „pozri čo vypisuje" | `log -d <dev>` alebo `cmd -d <dev> "<cli príkaz>"` |
| prvý raz / po zmene RPi | `setup -d <dev>` (bootstrap: tmux/picocom/nrfvenv/ts-okno) |

`--monitor <dev>` pri send/e2e zapne počítanie RAW/CHUNK na kontrolnom zariadení.

## Kľúčové pravidlá a úsudok (toto skript nevie)

- **Živá sieť = delay 5 s/paket, nikdy rýchlejšie** (default v configu). Kratší delay
  len na izolovanom CZ testovacom presete.
- **DFU (`flash-dfu`) vs. LoRa (`e2e`)**: DFU je rýchla istota (funguje aj pri mŕtvom
  CLI — 1200 bps touch beží mimo aplikácie), ale flashuj len pri stabilnom napájaní
  (prerušené DFU = zariadenie čaká v bootloaderi; DFU sa dá zopakovať, nie je to brick).
  LoRa e2e je test našej FOTA — používaj, keď je cieľom testovať.
- **Scope**: na 200 km je `direct` po known path ~2× lepší než `flood` (50 % vs 20 %
  na prechod). Ak direct neprináša nič po 2 kolách, skús `--scope flood`. `path_to`
  musí byť v configu (z RAW logu / od Fedora).
- **old/new bin**: skript sám berie old podľa bežiaceho build# z `builds/` a robí sha
  krížovú kontrolu. Ak zlyhá „nemám builds/...bin", build archív chýba — nikdy neposielaj
  patch s nesedeným base (repeater ho aj tak odmietne, len sa míňa éter).
- **Interpretácia logu**: `MAC fail (foreign/corrupt)` = poškodenie pri nízkom SNR
  (nie kľúče); `DEDUP (seen)` = duplicitná kópia; `pool empty` = preťaženie; RAW bez
  CHUNK pri route=2 = tranzitná direct kópia (nespracúva sa — normálne).
- **Mŕtve CLI** (žiadne echo na príkazy, RX RAW ale beží): na buildoch <356 to je
  buffer-lockup bug (šípky v tmux mimo copy-mode!) — jediná záchrana je reboot cez
  `flash-dfu`. Od #356 je fix (overflow sa spracuje ako unknown command).
- **Po FOTA flashi** spadne picocom aj tmux na RPi — skript session obnovuje sám;
  ručne: `ensure` logika je v `setup`.
- **Časy**: lokálne logy `test_nrf-fota/logs/<dev>.log` (čas PC), RPi `<log>.ts.log`
  (čas RPi, NTP) — porovnateľné na ms pre analýzu odoslanie→doručenie.

## Kontext a hlbšie detaily

- Runbook 200 km testu: `fkclaude/fcl_remote_e2e_rpi.md` (RPi prístup, WireGuard,
  DFU postup, pasce, história).
- FOTA systém: `fkclaude/fcl_readme_nrf-fota.md`, tech detaily `fcl_readme_tech_nrf-fota.md`.
- Sender (volá ho skript): `test_nrf-fota/fota_sender_mcpy.py` (companion na COM3,
  CMD_SEND_CHANNEL_DATA, `--chunks` formát z `fota missall`).
- Po dokončenej práci aktualizuj `fcl_remote_e2e_rpi.md` (stav zariadení, build#).

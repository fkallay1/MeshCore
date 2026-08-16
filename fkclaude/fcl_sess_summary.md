# Priebežné summary zo sessions

Čo sa v ktorej session robilo — jedna vec, jeden riadok (téma → záver), pod ňou
odkaz do dokumentácie. Nadpis sekcie = názov session. **Nové sekcie sa pridávajú
navrch**, aby bolo posledné navrchu.

Odkazy sú vo formáte `[súbor § sekcia](súbor#Lriadok)` — vo VS Code sú klikacie
a skočia priamo na riadok. Čísla riadkov platia k dátumu zápisu; ak sa dokument
posunie, hľadaj podľa názvu sekcie.

---

## 45 Merge upstream 1.17.1; Novy variant xiao_nrf42_nicerf2021f33; Riesenie WatchDog radia; Nove cli prikazy fk xxx, pre testovanie

*(2026-08-15 → 2026-08-17, vetva `features/nrf-fota`)*

- **Vetvy** — `upstream/dev` → `dev` → `features/nrf-fota` zosynchronizované, upstream 1.17.1 zmergnutý
  - ↳ [.claude/skills/sync-upstream/SKILL.md](../.claude/skills/sync-upstream/SKILL.md)
- **Skill `/sync-upstream`** — postavený, lebo vyrovnanie vetiev prišlo tretíkrát
  - ↳ [.claude/skills/sync-upstream/SKILL.md](../.claude/skills/sync-upstream/SKILL.md)
- **Voľba základu pre NiceRF variant** — odvodené z `xiao_nrf52`, nie z `meshtracker_x1`
  - ↳ [fcl_readme_nicerf_lora2021.md § Prečo odvodené z xiao_nrf52](fcl_readme_nicerf_lora2021.md#L6)
- **Nový variant `xiao_nrf52_nicerf2021f33`** — DIO mapa, RF switch, TCXO 3,3 V, PA tabuľka; na železe vysiela aj prijíma
  - ↳ [fcl_readme_nicerf_lora2021.md § Zapojenie](fcl_readme_nicerf_lora2021.md#L27)
- **Sémantika výkonu** — ciferník s `PA_OFFSET=8` podľa konvencie RAK 1W (14 → ~22 dBm)
  - ↳ [fcl_readme_nicerf_lora2021.md § Ciferník výkonu](fcl_readme_nicerf_lora2021.md#L70)
- **Upstream PR #3218** — chyba `setTxPower` na LR2021 odoslaná; PR bez zmienky o AI
  - ↳ [docs/lr2021-txpower-pr-description.md](docs/lr2021-txpower-pr-description.md) · [fcl_readme_nicerf_lora2021.md § Chyba v RadioLib](fcl_readme_nicerf_lora2021.md#L206)
- **PRAM patch pre LR2021** — 2240 B sa nahráva a verifikuje pri štarte
  - ↳ [fcl_readme_nicerf_lora2021.md § PRAM](fcl_readme_nicerf_lora2021.md#L504)
- **DC-DC / SIMO** — cievka na module je osadená, spotreba v RX klesla o ~41 %
  - ↳ [fcl_readme_nicerf_lora2021.md § ODMERANÉ: modul cievku MÁ](fcl_readme_nicerf_lora2021.md#L637)
- **CE pin** — funguje ako skutočný vypínač modulu, parazitné napájanie ho neudrží
  - ↳ [fcl_readme_nicerf_lora2021.md § Test CE](fcl_readme_nicerf_lora2021.md#L492)
- **Testovacie CLI `fk …`** — `info / simo / ce / pram`, aby sa nemuselo stále preflashovať
  - ↳ [fcl_readme_nicerf_lora2021.md § Testovacie CLI](fcl_readme_nicerf_lora2021.md#L400)
- **FOTA balíčky 1.16 → aktuál** — veľkosti sa zmestili (patch 31–37 kB, extraSafe pod 32 kB)
  - ↳ [fcl_readme_fota_extrasafe.md § Aktuálne riešenie](fcl_readme_fota_extrasafe.md#L34)
- **Watchdog rádia** — tri pokusy o detekciu boli mylné, funguje až pasívne RSSI okno z noise-floor vzorkovača
  - ↳ [fcl_readme_nicerf_lora2021.md § Watchdog rádia](fcl_readme_nicerf_lora2021.md#L317)
- **`isChipResponding()` na SX1262** — vadné, 0 z 6000; watchdog ho už nepoužíva
  - ↳ [fcl_readme_nicerf_lora2021.md § Watchdog rádia](fcl_readme_nicerf_lora2021.md#L317)
- **Metrika `n`** — nie je konštanta 960, pri nekonvergujúcom floore vyskočí (`n=92950`)
  - ↳ [fcl_readme_nicerf_lora2021.md § Diagnostika rádia](fcl_readme_nicerf_lora2021.md#L434)
- **Nové CLI `fk win`** — prečíta pasívne okno na požiadanie; sériové CLI vyžaduje `\r`, nie `\n`
  - ↳ [fcl_readme_nicerf_lora2021.md § Diagnostika rádia](fcl_readme_nicerf_lora2021.md#L414)
- **Filter `FK_DEBUG_MAXPATH=4`** — skrýval prevádzku (uzol chodí cez 9–13 hopov), odstránený
  - ↳ [fcl_readme_nrf-fota.md § Diagnostika](fcl_readme_nrf-fota.md#L293)
- **I2C displej** — pridaný výpis `display.begin()`; zlyhanie bolo hardvérové, nie firmvérové
  - ↳ [fcl_readme_nicerf_lora2021.md § Boot diagnostika](fcl_readme_nicerf_lora2021.md#L441)
- **Hypotéza parazitného napájania cez J-Link** — A/B na 8 power-cykloch nerozhodol, rozhodne až multimeter
  - ↳ [fcl_readme_nicerf_lora2021.md § Parazitné napájanie cez J-Link](fcl_readme_nicerf_lora2021.md#L477)
- **Rozdelenie flagov nicerf variantu** — testovacie flagy presunuté zo zdieľaného bloku do FOTA envu
  - ↳ [fcl_readme_nicerf_lora2021.md § Envy — kam patria naše flagy](fcl_readme_nicerf_lora2021.md#L260)
- **Zlý env pri flashi** — nahraný `_repeater` namiesto `_repeater_fota`, doska sa tvárila zaseknuto
  - ↳ [fcl_readme_nicerf_lora2021.md § Envy](fcl_readme_nicerf_lora2021.md#L250)
- **Mŕtve rádio na ProMicre** — buildy 391/437/444 zlyhali rovnako, takže nie regresia
  - ↳ [fcl_readme_nicerf_lora2021.md § Boot diagnostika](fcl_readme_nicerf_lora2021.md#L441)
- **Boot diagnostika** — pull test odhalil prerušený vodič MISO; po prepichnutí doska ide
  - ↳ [fcl_readme_nicerf_lora2021.md § Boot diagnostika](fcl_readme_nicerf_lora2021.md#L441)
- **`/auto-mode-setup`** — vysvetlené čo mení a prečo sa bije s `CLAUDE.md`; nič sa neaplikovalo
  - ↳ bez projektovej dokumentácie — vec CLI, nie repozitára

**Otvorené na ďalej:** watchdog ostáva v `LORA_RADIO_DIAG_ONLY` (zasahovacia vetva
neoverená proti skutočnej poruche) · parazitné napájanie cez J-Link čaká na meranie
multimetrom · krížový test adverts medzi ProMicro a nicerf nebol dokončený.

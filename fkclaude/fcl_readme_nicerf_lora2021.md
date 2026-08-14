# XIAO nRF52840 + NiceRF LoRa2021F33-2G4 (Semtech LR2021)

Variant `xiao_nrf52_nicerf2021f33` — primárne **868 MHz**, 2,4 GHz je pripravené,
ale **zámerne nezapnuté** (jeden `-D`, viď koniec dokumentu).

## Prečo odvodené z `xiao_nrf52` a nie z `meshtracker_x1`

Board vrstva MeshCore je od rádia úplne oddelená — `XiaoNrf52Board` ani
`MeshTrackerX1Board` neobsahujú nič rádio-špecifické. Z `meshtracker_x1` (jediný
LR2021 + nRF52840 variant v strome) by sa reálne prebralo ~10 build flagov, zvyšok
je pre XIAO balast (GPS, SPA06 baro, DRV2605, buzzer, iný board json). Naopak
`xiao_nrf52` už má správny board json, SoftDevice v7, funkčný FOTA env a náš fix
inertného `user_btn`.

Preto:

| Vrstva | Odkiaľ |
|---|---|
| Board (`XiaoNrf52Board`, `variant.cpp`) | požičané z `variants/xiao_nrf52` cez `-I` + `build_src_filter`, **žiadna kópia** |
| Rádio (LR2021 flagy, SPI piny) | vzor `variants/meshtracker_x1` |
| Front-end modulu (RF switch, PA, DIO mapa) | `src/helpers/radiolib/NiceRF2021F33.h` — **board-nezávislé**, ProMicro variant to použije bezo zmeny |

**Pasca pri kopírovaní:** `-I variants/xiao_nrf52_nicerf2021f33` musí byť **pred**
`-I variants/xiao_nrf52`, inak sa vezme SX1262-ový `target.h` a build padne na
`'radio_driver' was not declared`.

## Zapojenie

Drží sa rozloženia SX1262 z `xiao_nrf52`, aby sa nemenili zvyky:

| Modul (18 pinov) | XIAO | Pozn. |
|---|---|---|
| 18 IRQ | D1 | LR2021 **DIO9** (potvrdil NiceRF support) → `LR2021_IRQ_DIO=9` |
| 17 RESET | D2 | |
| 14 BUSY | D3 | |
| 13 NSS | D4 | |
| 12 SCK | D8 | `P_LORA_SCLK` musí byť definované, inak `std_init()` nezavolá `SPI::setPins()` |
| 16 MISO | D9 | |
| 15 MOSI | D10 | |
| 1 VCC | vlastný zdroj 3,0–5,5 V | **nie z XIAO** — viď nižšie |
| 5 CE | HIGH | interný pull-up; dole len pri úplnom vypnutí (vtedy aj NSS/RESET low) |
| 9 ANT | sub-GHz anténa | |
| 10 ANT-2G4 | 2,4 GHz anténa | zatiaľ nevyužité |

Front-end riadi **LR2021 sám** cez svoje DIO — z MCU tam nejde nič navyše:

```
DIO9  IRQ (pin 18)
DIO6  sub-GHz PA enable    RF switch, aktívne počas TX_LF   <- pre 868 zásadné
DIO5  2,4 GHz LNA enable   RF switch, aktívne počas RX_HF   (LOW = bypass, -12 dB)
DIO8  2,4 GHz PA enable    RF switch, aktívne počas TX_HF
DIO7  2,4 GHz FEM power    demo ho drží staticky HIGH
```

## Napájanie — najväčšie obmedzenie

Modul berie **<800 mA @868 MHz/1 W** a **<1200 mA @433 MHz/2 W** (pri 5 V).
XIAO to zo svojho 3V3 regulátora nedá. Vlastný zdroj + poriadny bulk kondenzátor,
zem spoločná. Pri 3,3 V dá modul 26,2 dBm @868 pri 540 mA.

Pozn.: NiceRF demo nastavuje `LR20XX_SYSTEM_REG_MODE_DCDC`, ale v RadioLib
diskusii #1772 zistili, že **VDCC1/VDCC2 nie sú na module vôbec zapojené** a NiceRF
potvrdil, že modul podporuje LDO režim. RadioLib `setRegMode()` sám od seba nevolá,
takže čip ostáva vo východzom režime — netreba nič robiť.

## Výstupný výkon — pozor

`LORA_TX_POWER` v MeshCore ide do RadioLibu ako dBm a ten cez **PA tabuľku**
odvodí `paDutyCycle`/`paSlices`/`paVal` (`paVal` je register `SetTxParams`
v **polovičných dBm** — presne stĺpec „Register Value" z datasheetu).

Problém: východzia LF tabuľka RadioLibu je ladená na Semtech referenčný dizajn.
Na tomto module **už jej najnižší stupeň (power = −9) tlačí ~19 dBm na anténu** —
nad limitom 14 dBm ERP pre väčšinu EU868 subpásiem.

Preto `NiceRF2021F33.h` obsahuje vlastnú `NICERF2021F33_PA_TABLE_LF`, kde
**`LORA_TX_POWER` znamená dBm na výstupe modulu** (default 14). PA drive je fixný
7/6 — presne ako v NiceRF demo, čiže konfigurácia, pri ktorej merali svoje tabuľky.

⚠️ Tabuľka je **interpolovaná z datasheetu, nie meraná** (body 868/915:
register −11/−5/1/7/13/19/25/31/37/44 → 10,4/13,3/16,2/18,9/21,4/24,0/26,5/28,3/
29,3/29,8 dBm). Pred spoľahnutím sa na absolútne číslo premerať.
`-D NICERF2021F33_STOCK_PA_TABLE` prepne späť na RadioLib tabuľku.

## Chyba v RadioLib, ktorú header obchádza

`LR2021::setRfSwitchTable()` **nezapisuje GPIO na MCU** — programuje RF-switch
konfiguráciu do samotného čipu (per-DIO maska režimov), takže LR2021 prepína
front-end hardvérovo. Preto majú HF riadky zmysel, hoci driver za behu volá len
`MODE_RX`/`MODE_TX`.

Lenže index je pomiešaný (`LR2021_config.cpp`):

```c
dioConfigs[dioNum] |= ...            // akumuluje sa podľa čísla DIO
setDioRfSwitchConfig(dioNum + 5, dioConfigs[i]);   // ...zapisuje sa podľa stĺpca
```

Sedí to len kým `pins[i] == DIO(5+i)`. **Pole pinov preto musí ísť husto od DIO5.**
Keby sme uviedli samotné DIO6, zapíše sa nulová konfigurácia a sub-GHz PA sa nikdy
nezapne (v diskusii #1772 si takto niekto pravdepodobne spálil LF PA).

Náš header to drží: `{DIO5, DIO6, NC, NC, NC}`, pri 2,4 GHz `{DIO5, DIO6, DIO7, DIO8, NC}`.

## Zapnutie 2,4 GHz (zatiaľ NEROBIŤ)

V `variants/xiao_nrf52_nicerf2021f33/platformio.ini` odkomentovať:

```ini
  -D NICERF2021F33_ENABLE_24G=1
  -D LORA_FREQ=2445.0
  -D LORA_BW=203.0
```

Checklist predtým:
1. Anténa na pin 10 (ANT-2G4), nie na pin 9.
2. `MAX_LORA_TX_POWER` dole — RadioLib klampuje HF výkon čipu na **−19…+12 dBm**
   a NiceRF odporúča nejsť nad **+4** (vyššie = nižší výstup, vyšší prúd, riziko
   poškodenia PA pri dlhom vysielaní).
3. Naša PA tabuľka je len sub-GHz; 2,4 GHz spadne na HF tabuľku RadioLibu.
4. MeshCore CLI validuje frekvenciu 150–2500 MHz a BW ≤ 500 kHz; RadioLib LR2021
   mapuje aj 203 a 406 kHz, 812 kHz už CLI neprepustí.
5. 1 W na 2,4 GHz je ďaleko nad EU limitom 100 mW EIRP.
6. 2,4 GHz uzly tvoria **samostatnú sieť** — s 868 MHz uzlami sa nespoja.

Kompiluje sa to už teraz (overené `PLATFORMIO_BUILD_FLAGS=-D NICERF2021F33_ENABLE_24G=1`),
len to nie je odskúšané na železe.

## Envy

| Env | Popis |
|---|---|
| `Xiao_nrf52_nicerf2021f33_repeater` | bežný repeater |
| `Xiao_nrf52_nicerf2021f33_repeater_fota` | repeater s LoRa-FOTA, `custom_fota_device = xiaonicerf` |

`custom_fota_device` je nutné — hook odvodzuje meno zariadenia z `PIOENV.split("_")[0]`,
čo by dalo `xiao` a miešalo by sa to s archívom SX1262 XIAO.

## Neoverené / otvorené

- Celé je to zatiaľ **len skompilované, nie odskúšané na železe.**
- PA tabuľka — interpolovaná, treba premerať.
- DIO7 riešime ako RF-switch pin HIGH vo všetkých režimoch (demo používa
  `GPIO_HIGH` funkciu). Ekvivalentné by to malo byť, ale netestované.
- ProMicro variant: stačí `variants/promicro_nicerf2021f33/` rovnakým strihom
  (target.h/.cpp + ini, board trieda požičaná z `variants/promicro`).

## Zdroje

- Datasheet Rev 1.2 — <https://www.nicerf.com/lora-module/lora2021f33-2g4.html>
- NiceRF demo kód V1.1 (`LoRa/Core/Src/lr2021.c` — DIO mapa, TCXO, PA cfg)
- RadioLib diskusia o tomto module: <https://github.com/jgromes/RadioLib/discussions/1772>
- MeshCore issue #2740 (LR2021 + ESP32-C3, tiež IRQ na DIO9):
  <https://github.com/meshcore-dev/MeshCore/issues/2740>

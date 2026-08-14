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

## Ciferník výkonu (`set tx`)

Rovnaká konvencia ako ostatné výkonné dosky v MeshCore (RAK3401 „1W" a spol.):
**číslo je nominálne, PA si pripočíta svoje, 22 = maximum.** Zabezpečuje to
`NICERF2021F33_PA_OFFSET`, default **8**:

| `set tx` | výstup modulu | prúd (merané, aj s XIAO) |
|---|---|---|
| 6 | ~14 dBm | ~200 mA |
| 14 | ~22 dBm | ~310 mA |
| 22 | ~30 dBm (1 W) | ~800 mA (odhad z datasheetu) |

`set tx 23` až `set tx 30` **nerobia nič** — RadioLib odmietne LF požiadavku nad
+22 (`checkOutputPower`) a ticho nechá pôvodnú hodnotu. CLI pritom odpovie OK.

### Nameraná krivka (2026-08-15, XIAO + modul, offset +8)

| `set tx` | výstup | paVal | datasheet @5 V | merané | pomer |
|---|---|---|---|---|---|
| 15 | ~23 dBm | 17 | ~312 mA | 359 mA | 1,15× |
| 16 | ~24 | 19 | ~336 | 392 | 1,17× |
| 17 | ~25 | 21 | ~368 | 427 | 1,16× |
| 18 | ~26 | 24 | ~416 | 491 | 1,18× |
| 19 | ~27 | 27 | ~468 | 560 | 1,20× |
| 20 | ~28 | 30 | ~522 | 635 | 1,22× |
| 21 | ~29 | 35 | ~605 | 780 | 1,29× |
| 22 | ~30 | 44 | ~724 | 950 | 1,31× |

**Rastúci pomer = prepad napájania**, nie odber XIAO (ten je konštanta a pomer by
klesal). Datasheetové prúdy sú pri 5 V; jeho tabuľka napätie/výkon dáva pri 4,0 V
už len 28,2 dBm @626 mA a pri 3,3 V 26,2 dBm @540 mA. Pomer 1,15–1,31 sedí na
VCC okolo 4,3–4,5 V pod záťažou. **Merať napätie priamo na pine 1 modulu počas
TX** — ak tam pri ~1 A nie je aspoň 4,5 V, horné stupne aj tak nedávajú, čo majú.

**Prevádzkové optimum je `set tx 18–19`** (~26–27 dBm, 0,49–0,56 A). Z 19 na 22
je +70 % prúdu za asi +2 dB. Navyše 27 dBm ERP je presne strop pásma
869,4–869,65 pri 10 % duty. `set tx 22` (0,95 A) je laboratórna hodnota — je to
dvojnásobok toho, čo garantuje USB2.

Krivka je hladká a monotónna, čo je zároveň dobrá správa o prispôsobení antény
(pri zlom matchi býva odber rozhádzaný).

**Prečo nie čisto RAK štýlom** (stock tabuľka RadioLibu, číslo = budenie čipu):
RAK-ov SKY66122 pridáva ~8 dB, tento PA ~16 dB pri nízkom budení a saturuje sa
okolo 30 dBm. So stock tabuľkou by už `set tx 14` znamenalo 14 dBm z čipu →
~29 dBm z modulu, a celý ciferník od ~10 vyššie by bol zlepený na strope.
Preto držíme budenie čipu nízko a kalibrovane — až vtedy majú kroky význam.

Zisk PA z datasheetu (register → budenie čipu → výstup):

| register | čip | výstup | zisk |
|---|---|---|---|
| −11 | −5,5 dBm | 10,4 dBm | 15,9 dB |
| 7 | 3,5 dBm | 18,9 dBm | 15,4 dB |
| 25 | 12,5 dBm | 26,5 dBm | 14,0 dB |
| 44 | 22 dBm | 29,8 dBm | 7,8 dB |

Nad register ~37 kúpiš +0,5 dB za +87 mA — nemá zmysel.

**Limity pásma:** 869,4–869,65 MHz dovoľuje 500 mW ERP (27 dBm) pri 10 % duty
cycle, väčšina ostatných 868 subpásiem len 25 mW ERP (14 dBm).

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

Tabuľka je **interpolovaná z datasheetu** (body 868/915: register
−11/−5/1/7/13/19/25/31/37/44 → 10,4/13,3/16,2/18,9/21,4/24,0/26,5/28,3/29,3/
29,8 dBm), ale **overená na HW meraním prúdu** (2026-08-14, ešte pri offsete 0):

| požadované | paVal | očakávaný prúd modulu | merané (aj s XIAO) |
|---|---|---|---|
| 14 | −4 | ~186 mA | 200 mA |
| 18 | 5 | ~213 mA | 230 mA |
| 20 | 10 | ~243 mA | 268 mA |
| 22 | 14 | ~277 mA | 310 mA |

Prírastky sedia (+27/+30/+34 očakávané vs +30/+38/+42 merané), rozdiel je odber
XIAO (~15–20 mA). Tabuľka je teda dobrá **na ~±1 dB**. Pre absolútne číslo pri
regulačnom limite stále platí: premerať prístrojom.

`-D NICERF2021F33_STOCK_PA_TABLE` prepne späť na RadioLib tabuľku (viď vyššie,
prečo tu nie je dobrý nápad).

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

## Iné modulácie (LR-FHSS, FLRC, GFSK, OOK)

Modul aj RadioLib ich vedia (`beginLRFHSS()`, `beginFLRC()`, `beginGFSK()`,
`beginOOK()`), ale pre MeshCore to **nie je prepínač**:

- **LR-FHSS sa nedá prijímať.** RadioLib `LR2021.cpp:339`: pri
  `PACKET_TYPE_LR_FHSS` vráti `startReceive()` rovno `RADIOLIB_ERR_WRONG_MODEM`
  („this modem cannot receive"). Je to uplink-only modulácia do LoRaWAN brány.
- **FLRC prijímať vie**, ale MeshCore je na LoRa naviazané hlbšie: `Dispatcher`
  počíta airtime a duty-cycle z LoRa parametrov, robí CAD, kalibruje šumový
  podklad z LoRa RSSI; CLI `set radio` má tvar `freq,bw,sf,cr` a prefs držia
  presne tieto štyri. Bola by to nová modemová vetva, nie konfigurácia. A nespojí
  sa s ničím — celá sieť by musela bežať to isté.

**LR-FHSS ako jednosmerný uplink do vlastnej brány** je reálna možnosť: uložiť
LoRa stav → `beginLRFHSS()` → odoslať → obnoviť cez `std_init()` +
`nicerf2021f33_post_init()`. RF switch to nerieši (`MODE_TX` je nezávislé od
modemu) a PA tabuľka platí tiež. Uzol je počas toho pár sekúnd hluchý.

⚠️ **Ale najprv brána:** SX1302 (WM1302) LR-FHSS demodulovať vie len po firmware
update, ktorý **Semtech nedistribuuje verejne** — treba si oň napísať. Bez neho
nemá zmysel stavať vysielaciu stranu. A na to, aby paket vyliezol v LoRaWAN
network serveri, treba okolo neho poriadne LoRaWAN rámcovanie (DevAddr, MIC,
čítače), inak je to pre server neznámy paket.

## Multi-SF (side detectors)

4. generácia LoRa IP v LR2021 vie popri hlavnom SF sledovať ďalšie SF súčasne.
MeshCore to má v CLI: `set extra.sf 8,9,10` / `get extra.sf` (oddeľovač čiarka,
ukladá sa do prefs). Pravidlá z `CustomLR2021Wrapper::configSideDetectors()`:

- max **3** vedľajšie SF, hodnoty 5–12,
- každé musí byť **vyššie** než primárne SF (kód odmieta `<= sf`; komentár
  v zdrojáku tvrdí opak a je zavádzajúci),
- rozpätie najviac **+4** nad primárnym,
- pri primárnom **SF ≥ 10 je povolený len jeden** vedľajší detektor,
- LDRO sa zapne samo pri symbole ≥ 16 ms, sync word 0x12.

Uzol tak **prijíma** aj iné SF, ale **vysiela stále na svojom** primárnom —
nie je to plnohodnotný SF bridging, odpovede idú primárnym SF.

Známa vrtochovitosť je ošetrená v `RadioLibWrappers.cpp`: so zapnutými side
detektormi vracal LR2021 `-706` pri `startReceive()` po hardvérovom CAD, preto sa
tam volá `standby()` navyše.

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

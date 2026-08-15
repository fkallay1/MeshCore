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

Modul má **vlastný LDO** (VCC → 3,3 V pre čip, riadi ho pin CE), zatiaľ čo
**sub-GHz PA aj 2,4 GHz FEM visia priamo na VCC**. Cez ten LDO tečie len čip,
preto `vbat` hlási ~3,3 V bez ohľadu na to, čo je na VCC.

Pozn.: NiceRF demo nastavuje `LR20XX_SYSTEM_REG_MODE_DCDC` a **je to správne** —
odmerali sme, že modul cievku pre SIMO má (viď sekciu DC-DC). Skoršie tvrdenie
z RadioLib diskusie #1772, že „VDCC1/VDCC2 nie sú zapojené", bolo zavádzajúce:
cievka patrí na **LXA/LXB**, VDCC/VPAX sú len prepojky potrebné v oboch režimoch.

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

Merané celkovo na napájaní. **Odber samotného XIAO je 9,5 mA** (odmerané
odpojením VCC modulu), preto stĺpec „modul" = merané − 9,5 mA. Datasheetové
prúdy platia pre modul samotný pri 5 V.

| `set tx` | výstup | paVal | datasheet | merané | modul | pomer |
|---|---|---|---|---|---|---|
| 6 | ~14 dBm | −4 | ~186 mA | 204 mA | 194,5 | 1,05 |
| 7 | ~15 | −1 | — | 212 | 202,5 | — |
| 8 | ~16 | 1 | ~197 | 218 | 208,5 | 1,06 |
| 9 | ~17 | 3 | — | 227 | 217,5 | — |
| 10 | ~18 | 5 | ~213 | 236 | 226,5 | 1,06 |
| 11 | ~19 | 7 | ~221 | 250 | 240,5 | 1,09 |
| 12 | ~20 | 10 | ~243 | 273 | 263,5 | 1,08 |
| 13 | ~21 | 12 | ~265 | 292 | 282,5 | 1,07 |
| 14 | ~22 | 14 | ~277 | 315 | 305,5 | **1,10** |
| 15 | ~23 | 17 | ~312 | 345 | 335,5 | 1,08 |
| 17 | ~25 | 21 | ~368 | 427 | 417,5 | 1,13 |
| 19 | ~27 | 27 | ~468 | 560 | 550,5 | 1,18 |
| 21 | ~29 | 35 | ~605 | 780 | 770,5 | 1,27 |
| 22 | ~30 | 44 | ~724 | 950 | 940,5 | **1,30** |

**Do ~0,35 A je zhoda s datasheetom výborná (5–10 %)** — zvyšok vysvetlí kľudový
odber LDO modulu a to, že VCC nie je presne 5 V ako pri ich meraní.

**Nad ~0,5 A pomer rastie na 1,18–1,30** a to už je konzistentné s prepadom
napájania: pri takom prúde je úbytok na kábloch a USB ceste výrazný, a
datasheetová tabuľka napätie/výkon dáva pri 4,0 V už len 28,2 dBm @626 mA
a pri 3,3 V 26,2 dBm @540 mA. Ak by sa niekedy jazdilo na hornom konci
ciferníka, **zmerať VCC priamo na pine 1 modulu počas TX** (krátka špička →
treba ADC delič alebo niekoľkosekundovú CW nosnú). Pri `tx ≤ 15` netreba.

**Prevádzkové optimum:** `set tx 6` (~14 dBm) pre bežné 868 subpásma,
`set tx 19` (~27 dBm) je strop pásma 869,4–869,65 pri 10 % duty. Z 19 na 22
je +70 % prúdu za asi +2 dB — nemá zmysel. `tx 22` (0,95 A) je laboratórna
hodnota, dvojnásobok toho, čo garantuje USB2.

Krivka je hladká a monotónna cez 10 bodov, čo je dobrá správa aj o stabilite PA,
aj o prispôsobení 17 cm antény (pri zlom matchi býva odber rozhádzaný).

**Overenie posunu +8:** rovnaký `paVal` dáva rovnaký prúd pred aj po zmene
offsetu — `tx 14`→`tx 6` (0,200/0,204 A), `18`→`10` (0,230/0,236),
`20`→`12` (0,268/0,273), `22`→`14` (0,310/0,315). Rozdiel konštantných 4–6 mA
naprieč rozsahom, čiže **posun je čisté prečíslovanie, fyzika identická**.

### RX boosted gain — na LR2021 doskách vypnutý, zapnúť ručne

`rx_boosted_gain` má v prefs default **0** a repeater ho na 1 prepína len
v bloku `#if defined(USE_SX1262) || defined(USE_SX1268)` (`MyMesh.cpp:1128`),
do ktorého LR2021 nespadá. Žiadny LR2021 variant nedefinuje ani
`LR2021_RX_BOOSTED_GAIN`. **Takže všetky SX1262 repeatre ho majú zapnutý
a LR2021 dosky nie** — vrátane upstream `meshtracker_x1` a `meshnology_w12`.
Vyzerá to na prehliadnutie, nie zámer; kandidát na upstream.

Zapnutie za behu (uloží sa do prefs, prežije reštart): `set radio.rxgain on`

**Cena odmeraná 2026-08-15: 20 mA → 18–18,5 mA po vypnutí, čiže ~1,5–2 mA.**
Prínos sa na stole nedá zmerať (susedia dorážajú na −43 až −68 dBm, čo je
45–70 dB nad hranicou šumu; RSSI aj `nf` ostali identické) — prejaví sa až na
slabých linkách, typicky 1–3 dB citlivosti. Ten prúdový rozdiel je zároveň
jediný dôkaz, že sa boost naozaj aplikoval.

⚠️ Pri natrvalo do variantu **pozor na semantiku**: `LR2021::setRxBoostedGainMode()`
berie **úroveň 0–7**, nie bool, a `CustomLR2021::std_init()` posiela hodnotu
definu priamo ako úroveň. Takže `-D LR2021_RX_BOOSTED_GAIN=1` = úroveň 1;
ekvivalent CLI zapnutia je **`=7`** (wrapper posiela `LR2021_RX_BOOST_LEVEL`,
default 7).

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

## Testovacie CLI (`fk …`) — flag `FK_NICERF2021F33_TEST`

Aby sa dal modul skúšať bez neustáleho preflashovania. Vyžaduje aj
`-D RADIOLIB_GODMODE=1` (RadioLib má `setRegMode`, `writeRegMem32`,
`activatePram` a spol. privátne). **Nikdy nenechať zapnuté v ostrom builde.**

| príkaz | čo robí |
|---|---|
| `fk info` | identita čipu, VBAT, teplota, chyby, stav PRAM |
| `fk simo on\|off` | prepne vnútorný regulátor čipu na DC-DC / LDO |
| `fk ce on\|off` | vypne/zapne celý modul cez jeho LDO enable (pin CE) |
| `fk pram` | stav PRAM (magic + verzia) |
| `fk pram load` | (znova) nahrá patch, ak je build s `LR2021_PRAM_UPD` |

⚠️ **`getVbat()` a `getTemp()` sú platné LEN v standby.** Merané počas príjmu
vracajú 2 mV a 0,0 °C. Objavené tvrdo: boot report (beží pred nahodením RX)
čítal správne, neskorší dotaz nie — a chvíľu to vyzeralo ako pokazený čip.
Preto `fk info` aj `fk simo` merajú v standby a až potom vrátia RX.

### Test CE — parazitné napájanie modul neudrží

```
fk ce off -> ce=LOW  | getVersion rc=0 fw=0.1  (no valid answer)
fk ce on  -> ce=HIGH | getVersion rc=0 fw=1.24 (chip answers)
```

Pri CE dole čip **neodpovedá** — vracia nezmysel. Čiže prúd tečúci cez ESD
diódy zo SPI pinov ho neudrží funkčný a CE je skutočný vypínač. (Ten včerajší
jav, keď čip bez VCC raz odpovedal na `getVersion`, bol hraničný a nereprodukovateľný.)
Pozor: NiceRF žiada pri CE dole stiahnuť aj NSS a RESET, inak tečie leakage.

## PRAM (firmvérový patch čipu) — ✅ OVERENÉ NA HW 2026-08-15

LR2021 má **patch RAM**: Semtech dodáva binárnu záplatu, ktorú hostiteľ nahráva do
čipu. **Je volatilná** — stratí sa pri resete a pri studenom štarte, prežije len
spánok s retenciou. ZephCore to v 1.17.1 pridal, MeshCore nie.

**Upstream to nerobí a čip beží nezáplatovaný.** RadioLib API má (`activatePram()`,
`checkPramLoaded()`, `getPramVersion()`, správne konštanty), ale **sám ho nikdy
nevolá**, blob nedodáva, a tie metódy sú **privátne** (za `#if !RADIOLIB_GODMODE`),
takže ich `CustomLR2021` ani nemá ako zavolať. Platí to aj pre upstream
`meshtracker_x1` a `meshnology_w12`. Overené na HW: bez `LR2021_PRAM_UPD`
hlási boot report `pram: loaded=no`.

**U nás to už funguje** (`-D LR2021_PRAM_UPD=1`, blob v
`src/helpers/radiolib/lr20xx_pram_lr2021.h`, 560 slov = **2240 B**):

```
[LR2021] pram: loaded=YES version=0x0313
```

Magic slovo `0x600DB002` sa prečíta späť, čiže čip patch prijal. Rádio beží
ďalej normálne a **odber sa nezmenil** (viď meraciu maticu v sekcii DC-DC).

### ⚠️ Nevysvetlená vlastnosť merania VBAT

Overené A/B na jednom builde a v jednom behu (2026-08-15):

| kde sa meria | VBAT | teplota |
|---|---|---|
| boot report (rádio ešte nikdy nebolo v RX) | **3312 mV** | správna |
| `fk info` (rádio už bežalo v príjme) | **2454 mV** | správna |

**Nespôsobuje to ani PRAM, ani SIMO, ani druh standby** — všetky tri som
postupne podozrieval a všetky tri vylúčil meraním:
- bez PRAM a s vypnutým SIMO: boot 3312, `fk info` 2454
- s PRAM a zapnutým SIMO: boot 3312, `fk info` 2454
- `standby(STDBY_XOSC)` namiesto `standby()` (STDBY_RC): bez zmeny

Rozhoduje teda **to, či rádio predtým bežalo v príjme**, nie konfigurácia.
Teplota aj chybové príznaky sú pritom v oboch prípadoch v poriadku, takže **nič
nie je pokazené** — je to vlastnosť merania, nie porucha.

**Praktické pravidlo: verte boot hodnote (3312 mV), `fk info` brať orientačne.**

### Mechanizmus (Semtech `lr20xx_patch.c`, Clear BSD)

1. zápis blobu na `0x801000` po blokoch 32 slov (`write_regmem32`)
2. príkaz `0x012D` = enable PRAM
3. kontrola magic slova na `0x800FF8`, očakáva sa `0x600DB002`;
   typ a verzia na `0x800FFC`

Čitateľné je len `{ is_pram_loaded, pram_type, pram_version }` — PRAM **nie je**
zdroj informácií o čipe, je to kódová pamäť. Sériové číslo LR2021 nemá vôbec.

**Blob aj loader máme lokálne** v NiceRF demo balíku
(`lr20xx_driver/inc/lr20xx_pram_lr2021.h`, `src/lr20xx_pram_load.c`,
`src/lr20xx_patch.c`), driver **v2.0.2**. NiceRF ho vo svojom demo nahráva ako
**prvý krok** inicializácie (`lr2021.c:92`). Veľkosť **406 slov = 1 624 B**
(ZephCore uvádza 2 240 B flashu aj s loaderom, +80 nA v retenčnom spánku).
Max. veľkosť PRAM nie je v našich materiáloch nikde uvedená.

**Verzia drivera je aktuálna — netreba nič sťahovať** (overené 2026-08-15):
NiceRF demo aj ZephCore majú zhodne **v2.0.2** a PRAM blob je **bajt na bajt
identický** (406 slov, sha256 prefix `88d560ee582f3515`). Oficiálny
`Lora-net/usp` má paradoxne staršiu v1.3.4 (11/2025); samostatný
`lr20xx_driver` repozitár neexistuje (Lora-net ho má len pre sx126x, llcc68,
lr1110, lr1121). **Chýba nám len novšia revízia datasheetu** — pýtať od NiceRF.

### Čo patch opravuje

README drivera v2.0.2 enumeruje: 4× Bluetooth LE (Coded PHY access address,
frequency drift, 2 Mbps preamble, blocking), 3× RTToF (PLL frequency step, RSSI
computation, extended mode stuck) a **DCDC (SIMO) impact on sensitivity** pre
sub-GHz FSK/FLRC/OOK/**LoRa**/Z-Wave.

**Ani jedno sa nás netýka** — BLE ani ranging nepoužívame a DC-DC nemáme
(viď nižšie). Ale: v2.0.2 v tom istom vydaní **zmazal** workaround funkcie pre
BLE, RTToF a DC-DC, lebo ich patch nahradil.

### Datasheet Rev 2.1 — doslovné znenie (STIAHNUTÉ 2026-08-15)

Náš pôvodný **Rev 1.1 (10/14/25) je zastaraný** — o PRAM v ňom nie je nič
(§22.3 je „RTToF PLL Frequency Step Truncation"). Aktuálny je
**LR20xx Datasheet Rev 2.1 (13/04/26, 243 strán, LR2021/LR2022/LR2012)**,
zmenový záznam uvádza *„Added Section 22.3 Firmware Patch RAM (PRAM)"*:

> While not strictly required, using the chip without the PRAM can create
> performance issues and unexpected bugs. **The use of the PRAM is therefore
> highly recommended.**
>
> - The PRAM is lost after a reset or a cold start.
> - The PRAM is preserved during a sleep with retention.
> - The sleep current with retention will increase by **80 nA** when the PRAM
>   is loaded.

§22 úvod: *„Most of the workarounds are implemented in the Firmware Patch RAM,
loaded directly by the drivers during initialization."* — teda širšie než
enumerovaný zoznam nižšie, a vysvetľuje, prečo v Rev 2.1 zmizli BLE workaroundy
zo zoznamu funkcií.

§22.3.1 predpisuje nahrávať **po resete a po prebudení z deep sleep („cold
sleep")**, presne v poradí `GetVersion` → `WriteRegMem32` od `0x801000` →
opcode `0x012D` s bajtom `0x00`; §22.3.2 kontrola `0x800FF8` == `0x600DB002`,
verzia z `0x800FFC` ako `((val >> 8) & 0xffff)`.

### Ako to robí ZephCore (vzor, ak sa do toho pustíme)

Nahráva **len v resetových cestách**, nie pri každom boote: obraz prežije spánok
s retenciou, a ich driver iný spánok nepoužíva. Postup v `lr20xx_load_pram()`:
`get_version` → kontrola **0x01/0x18** (existujú dva obrazy, druhý pre
LR2012/LR2022) → `load_pram` → `enable_pram` → **spätné čítanie magic slova**.
Zlyhanie je len `LOG_WRN`, init nepadne. Volá sa z `lr20xx_hardware_reset()`,
čiže hneď po resete a **pred** konfiguráciou (rovnako ako NiceRF demo).

**Náš problém s poradím:** RadioLib `begin()` je monolit a `findChip()` čip
resetuje (až 10×), takže patch by šiel až **po** `std_init()`, teda po
konfigurácii. Či to čipu prekáža, treba overiť na železe. Plus by bolo treba
`-D RADIOLIB_GODMODE=1` kvôli prístupu k privátnym metódam.

**Pozor:** MeshCore `resetAGC()` robí `_radio->sleep()`. Ak by to bol spánok bez
retencie, patch sa stratí. U nás zatiaľ nehrozí (`agc_reset=0`, viď
[[agc_keep_standard]]), ale pri zapnutí AGC resetu to overiť.

## DC-DC (SIMO) — prečo ho nemáme a nepotrebujeme

SIMO (Single Inductor Multiple Output) je spínaný menič, ktorým si čip vyrába
vnútorné napájacie vetvy účinnejšie než lineárnym regulátorom — nižší odber,
hlavne v RX a pri nižších TX výkonoch. Datasheet: *„The LR2021 uses an internal
SIMO converter and voltage regulation system to supply VR_PA."*

Podmienka (§3.6): **externá cievka 2,2 µH**, DCR max 0,5 Ω, Isat min 200 mA,
rezonančná frekvencia min 20 MHz, na pinoch **VDCC1/VDCC2** (~1,55 V, max 20 mA).

### ✅ ODMERANÉ 2026-08-15: modul cievku MÁ, DC-DC funguje

Všetko s **rovnakým** stavom ostatných nastavení (RX boost zapnutý, `tx=15`,
SF7/BW62,5). Odber samotného XIAO je 9,5 mA.

| konfigurácia | celkom | modul (−9,5 mA) |
|---|---|---|
| LDO, bez PRAM | 20,0 mA | ~10,5 |
| LDO, s PRAM | 20,5 mA | ~11,0 |
| SIMO, bez PRAM | 15,5–16 mA | ~6,25 |
| **SIMO, s PRAM** | **16,0 mA** | **~6,5** |

**SIMO ušetrí 4,5 mA** (20,5 → 16,0), na module je to pokles ~11 → ~6,5 mA,
teda **~41 %**. Datasheet sľubuje „až 50 %", takže sedíme.

Keby cievka chýbala, prúd by klesnúť nemohol — menič spínajúci do prázdna by ho
naopak zvýšil. **Tým je otázka zodpovedaná bez čakania na NiceRF.**

Vysvetľuje to aj ich špecifikáciu „RX <8 mA": to je hodnota **so SIMO** (ich demo
ho zapína). V LDO režime má modul ~11 mA a ich číslo by nesplnil.

**PRAM nič nestojí** — 20,0 vs 20,5 mA v LDO a 15,75 vs 16,0 v SIMO, oboje
v rozptyle merania. Datasheetových „+80 nA" sa týka retenčného spánku.

**Príjem sa nezhoršil**: `nf=-112/-113`, RSSI −38/−55/−69, SNR 12,8–14,2,
`rxerr=0`, `miss=0` — identické s LDO režimom. (Pozor: silné signály nezistia
stratu 1–3 dB citlivosti, to by ukázala až slabá linka.)

**Zapnutie natrvalo:** `-D NICERF2021F33_SIMO=1` (zapína sa pri každom
`radio_init()`, lebo **čip sa resetuje do LDO**). Zapínať **spolu s
`LR2021_PRAM_UPD`** — datasheet uvádza „DCDC (SIMO) impact on sensitivity" pre
sub-GHz LoRa ako vec, ktorú patch opravuje.

### Pôvodná analýza (prečo sme to museli merať)

⚠️ **Skoršie tvrdenie „VDCC1/VDCC2 nie sú vyvedené, takže SIMO sa nedá zapnúť"
bolo NESPRÁVNE.** Stálo na parafráze fóra, nie na dokumente, a bolo aj vecne
mimo: cievka nepatrí na VDCC, ale na piny **LXA/LXB**. VDCC1↔VDCC2 a
VPAX1↔VPAX2 sú len externé prepojky, ktoré potrebujú **oba** režimy.

LXA/LXB sú vnútri modulu, von nevedú, a NiceRF vnútornú schému nezverejňuje —
bloková schéma v ich datasheete (str. 4) ukazuje len `VCC → LDO → 3,3 V` a FEM,
nie okolie čipu. **Z dokumentov sa teda nedá zistiť, či tam cievka je.**

Indície, že **je**:
- ich demo kód nastavuje `LR20XX_SYSTEM_REG_MODE_DCDC`,
- modul udáva RX prúd **<8 mA**, čip má v SIMO **5,7 mA** (v LDO by bol vyšší),
- datasheet čipu: *„All performance given with SIMO used to power the chip"*.

**Zistiť sa to dá testom** (nepotrebuje prepájanie): `standby()` →
`setRegMode(SIMO_NORMAL=0x02)` → či čip ďalej beží + zmerať RX prúd. Potrebuje
`-D RADIOLIB_GODMODE=1` (`setRegMode` je privátna) a príkaz je platný **len
v Standby RC**. Riziko nízke (bez cievky niet indukčnosti na zákmit, menič len
nevytvorí napätia), ale je to nedeklarovaná konfigurácia.

### Čo hovorí datasheet Rev 2.1 (§23.1)

> The DC-DC configuration is **strongly recommended for battery-operated
> applications** as it reduces power consumption by **up to 50 %** compared to
> LDO mode, as its efficiency exceeds 85 %. … PA_LF operation at +22 dBm
> requires a supply voltage greater than 2.2 V when running on the SIMO.

Cievka podľa §3.6: **2,2 µH**, DCR max 0,5 Ω, Isat min 200 mA, rezonančná
frekvencia min 20 MHz.

**Default je LDO** — `SetRegMode` má `0x00: SIMO_OFF … (Default)`, a náš firmware
`setRegMode()` nevolá vôbec. Takže dnes beží čip v LDO režime.

Pozor na súvislosť s PRAM: driver README uvádza *„DCDC (SIMO) impact on
sensitivity"* pre sub-GHz LoRa ako limitáciu, ktorú **opravuje práve PRAM**
(a datasheet čipu inde píše *„The sensitivity is increased in LDO mode"*).
Čiže **ak sa pôjde do DC-DC kvôli batérii, PRAM prestáva byť voliteľná.**

### Dva stupne napájania — nezamieňať

```
VCC → LDO modulu (riadi CE) → 3,3 V → VBAT čipu → [LDO regulátory | SIMO] → VDCC/VPAX
      ↑ vypnutie = modul bez napájania      ↑ toto prepína SetRegMode
```

Sub-GHz PA aj 2,4 GHz FEM visia **priamo na VCC**, nie na tom 3,3 V rozvode —
cez LDO tečie len čip. Preto `vbat` hlási ~3,3 V bez ohľadu na to, čo je na VCC.

**„Vypnúť LDO a nechať bežať DC-DC" nie je možný stav** — SIMO je *za* tým LDO
a bez VBAT nemá z čoho vyrábať. A parazitné napájanie cez ESD diódy zo SPI pinov
nie je náhrada: prúd je o dva rády mimo (GPIO nRF52840 dá 2–15 mA, rádio
potrebuje 5,7–8 mA v RX a stovky mA v TX), napätie kolíše s dátami, a je to
prevádzka mimo medzných hodnôt (datasheet tab. 3-1: *„Stresses above the values
listed below may cause permanent device failure"*) s rizikom latch-up.
NiceRF preto pri CE dole žiada stiahnuť aj NSS a RESET.

## Neoverené / otvorené

- ~~Má modul cievku pre SIMO?~~ **VYRIEŠENÉ — má, DC-DC ušetrí ~41 %.**
- **VBAT: 3312 mV pri boote vs 2454 mV po tom, čo rádio bežalo v RX** — príčina
  neznáma, PRAM/SIMO/standby vylúčené meraním. Nič nie je pokazené, ale to číslo
  sa nedá brať vážne mimo boot reportu.
- **RadioLib `setRegMode()` posiela 5 argumentových bajtov**, kým datasheet
  Rev 2.1 (tab. 6-26) definuje **jediný** (`simo_usage`). Čip tie štyri navyše
  zjavne ignoruje (SIMO preukázateľne funguje a prúd klesol podľa datasheetu),
  ale je to rozpor — overiť proti najnovšej vetve RadioLibu a prípadne nahlásiť.
- **Sensitivity v SIMO režime** — na silných signáloch sa strata 1–3 dB nedá
  zmerať. Overiť na slabej linke (5 km alebo 200 km uzol).
- **PA tabuľka je interpolovaná, nie meraná** wattmetrom. Nepriamo overená
  prúdom (zhoda 5–10 % do 0,35 A), ale absolútny výkon nikto nemeral.
- **Prepad VCC nad ~0,5 A** — nemeraný, treba ADC delič alebo CW nosnú.
  Pri `tx ≤ 15` netreba riešiť.
- **PRAM sa nenahráva** — rozhodnutie odložené, viď sekciu vyššie.
- DIO7 riešime ako RF-switch pin HIGH vo všetkých režimoch (demo používa
  `GPIO_HIGH` funkciu). Ekvivalentné by to malo byť, ale netestované.
- **CE (pin 5) nie je zapojený**, drží ho interný pull-up. Pre batériu ho
  pripojiť na voľný pin — **D5 je voľný** (D1–D4, D8–D10 zabraté rádiom, D6/D7
  deklarované ako I2C, D0 = PIN_BUTTON1). Pri CE dole musia ísť dole aj NSS
  a RESET, inak tečie cez ESD diódy.
- ProMicro variant: stačí `variants/promicro_nicerf2021f33/` rovnakým strihom
  (target.h/.cpp + ini, board trieda požičaná z `variants/promicro`).

## Zdroje

- **LR20xx datasheet Rev 2.1** (13/04/26, aktuálny) —
  <https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/8924/600_62785538.LR20xxDatasheet_V2_1.pdf>
- Modul LoRa2021F33-2G4 datasheet Rev 1.2 —
  <https://www.nicerf.com/lora-module/lora2021f33-2g4.html>
- NiceRF demo kód V1.1, driver **v2.0.2** (`LoRa/Core/Src/lr2021.c` — DIO mapa,
  TCXO, PA cfg; `Drivers/lr20xx_driver/README.md` — zoznam limitácií;
  `inc/lr20xx_pram_lr2021.h` — PRAM blob). **Je to najnovšia verzia** — ZephCore
  má tú istú a blob je bajt na bajt identický; `Lora-net/usp` má staršiu v1.3.4.
- ZephCore (vzor pre PRAM) — <https://github.com/liquidraver/ZephCore>
- RadioLib diskusia o tomto module: <https://github.com/jgromes/RadioLib/discussions/1772>
- MeshCore issue #2740 (LR2021 + ESP32-C3, tiež IRQ na DIO9):
  <https://github.com/meshcore-dev/MeshCore/issues/2740>

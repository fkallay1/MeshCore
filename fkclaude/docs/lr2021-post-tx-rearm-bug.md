# LR2021: nahodenie príjmu môže zlyhať (−706) — chýbajúca maska STATE_INT_READY

Nájdené a potvrdené 28. 8. 2026. **Pôvodný názov hovoril „po vysielaní" — to bolo
nesprávne, viď opravu na konci.** **Netýka sa to pretečených SPI odpovedí** — je to
samostatná chyba v MeshCore, ktorá skončí tichým a trvalým výpadkom príjmu.

## Symptóm

Repeater po vysielaní prestane prijímať a **sám sa nezotaví**. V logu:

```
[FK] rssi-window n=0 min=0 max=0 spread=-1  <== NO SAMPLES
AALIVE ... rawrx=2983 isr=3440    (obe zamrznuté)
```

Čip je pritom v poriadku — SPI odpovedá, `fk stat` vracia `cmd=3=DAT`, `fk busy` 8/8.
Len je v **STDBY_RC namiesto Rx** a nikto ho nevráti. Prúd klesne z ~16 mA na ~10 mA.
`fk reinit` dosku okamžite oživí.

## Príčina

`RadioLibWrapper::recvRaw()` má na konci nahodenie príjmu **bez ochrany**, ktorú má
`startRecv()`:

```cpp
void RadioLibWrapper::startRecv() {
#if defined(USE_LR2021)
  _radio->standby();          // <-- bez tohto LR2021 hádže -706
#endif
  int err = _radio->startReceive();
```

```cpp
// recvRaw(), koniec:
if (state != STATE_RX) {
  int err = _radio->startReceive();   // <-- tu standby() CHÝBA
```

LR2021 odmietne `SetRx`, keď už v Rx je, s `RADIOLIB_ERR_SPI_CMD_INVALID (−706)`.
`state` ostane `IDLE`, ďalšie kolo skúsi to isté a zlyhá rovnako — natrvalo.

### Prečo len LR2021 a prečo tak zriedka

Po prijatí paketu sa `state` nastavuje odlišne:

```cpp
#if defined(USE_LR2021)
  state = STATE_RX;     // LR2021 v Rx zostáva
#else
  state = STATE_IDLE;   // ostatné čipy potrebujú nové startReceive
#endif
```

Pre **ostatné čipy** je ten chybný riadok **normálna cesta po každom pakete** a je správny,
lebo ochranu nepotrebujú. Pre **LR2021** je takmer nedosiahnuteľný — dostane sa naň len keď
`state` zmenilo niečo iné, typicky `onSendFinished()` po vysielaní.

Preto to nikto nenašiel: riadok vyzerá správne, lebo pre väčšinu hardvéru správny je.

## Dôkaz

Build #336 s inštrumentáciou (prvý pokus ponechaný, pri chybe výpis + `standby()` a znova):

```
02:12:26.302  RX #185 RESPONSE
02:12:26.917  [FK] startReceive() po TX zlyhalo: -706 -> standby + retry
02:12:26.917  [FK]   retry -> 0 (zabralo)
02:12:27.233  RX #186 ...            <- príjem pokračuje
```

Bez opravy by rádio v tom momente umrelo. Výskyt nastal 25 minút po nasadení buildu.

Predtým, na builde bez opravy, zamrzlo **2 z ~20 vysielaní** (01:27 RESPONSE, 01:37 PATH),
zvyšné prešli — chyba je súbežná, nie deterministická, čo sedí s tým, že závisí od toho,
či je čip v danom okamihu v Rx.

## Oprava

Doplniť ten istý `standby()`, prípadne v `recvRaw()` volať rovno `startRecv()`, aby logika
existovala na jednom mieste.

**Týka sa každej LR2021 dosky**, najviac tých so zapnutým preposielaním — repeater
odpovedá na prijatú prevádzku neustále, takže príležitostí má veľa.

## Súvisiace

Watchdog (`FKPR_RADIO_WATCHDOG`) tento stav **spoľahlivo deteguje** — hlási
`rssi-window ... spread=-1 <== NO SAMPLES`. Zachytil oba spontánne výskyty. Beží však
s `FK_RADIO_DIAG_ONLY=1`, takže len pozoruje. Zapnutie naostro by dosku zotavilo do 30 s
aj bez tejto opravy, a chráni aj pred inými príčinami tichého výpadku.


## OPRAVA DIAGNÓZY (28. 8., po štyroch výskytoch)

Prvá verzia tohto dokumentu tvrdila, že chyba nastáva **po vysielaní**. **Nie je to tak.**
V behu #336 sa **nevysielalo ani raz** (`rawtx=2` je z bootu, ďalšie `TX RAW` v logu nie sú)
a napriek tomu nastali štyri zlyhania. Tri zo štyroch prišli **do pol sekundy po prijatom
rámci**.

### Skutočná príčina: chýbajúca maska v porovnaní

```cpp
state |= STATE_INT_READY;              // riadok 35, v ISR
...
state = STATE_RX;                      // riadok 196, po spracovaní paketu
...
if (state != STATE_RX) {               // riadok 202 — BEZ MASKY
  int err = _radio->startReceive();
```

Ak **medzi priradením a porovnaním pribehne prerušenie z ďalšieho paketu**, `state` bude
`STATE_RX | STATE_INT_READY`, čo sa nerovná `STATE_RX` — a zavolá sa `startReceive()` na
prijímači, ktorý už v Rx je. LR2021 odpovie **−706** a wrapper ostane v `STATE_IDLE`.

Že sa ten bit má maskovať, vie samotný kód o kúsok vyššie:

```cpp
bool RadioLibWrapper::isInRecvMode() const {
  return (state & ~STATE_INT_READY) == STATE_RX;   // <-- tu maska JE
}
```

### Prečo to sedí na všetko

Nastáva **hneď po prijatí**, lebo treba ďalšie prerušenie v mikrosekundovom okne — a
v meshi chodia pakety v dávkach retransmisií. Je **zriedkavé** z rovnakého dôvodu. A je to
**len LR2021**, lebo ostatné čipy majú na tom mieste `STATE_IDLE` a nahodenie naozaj
potrebujú.

### Správna oprava

```cpp
if ((state & ~STATE_INT_READY) != STATE_RX) {
```

`standby()` pred opakovaním, ktorý bol v prvej verzii, je len **náplasť** — zbytočné
volanie sa ňou vykoná a až potom opraví. S maskou sa nevykoná vôbec.

Nasadené v builde #339, inštrumentácia ponechaná: ak sa počítadlo zlyhaní za porovnateľný
čas (4 výskyty za 5,7 h) neposunie, je to potvrdené.

## Doplnenie 28. 8. 2026 dopoludnia — čip sa resetuje sám

Meranie na buildoch #340 až #344 prinieslo pozorovanie, ktoré doterajší výklad
nepokrýva a je od neho nezávislé.

### Čo bolo namerané

O 10:04 doska ohluchla natrvalo. Kým som ju reštartoval, odpovedala cez CLI:

| údaj | zdravý stav (dni dozadu, aj 09:31 v ten deň) | pri poruche 10:04 |
|---|---|---|
| `vbat` | 3311–3313 mV | **3 mV** |
| `temp` | ~19 °C | **0,0 °C** |
| `errors` | 0x0000 | **0x0001** |
| PRAM | `loaded=YES version=0x0313` | **`loaded=no version=0x1CFD`** |
| ResetSource | `NRESET` | **`ANALOG (POR/BRN)`** |

PRAM je v RAM čipu, takže `loaded=no` znamená skutočný reset, nie chybu čítania.
SPI pritom odpovedalo správne (`cmd=3=DAT`), len `SetRx` čip odmietal (−707,
`SPI_CMD_FAILED`), lebo bol v čerstvom nenakonfigurovanom stave.

O 09:31, teda pol hodiny predtým, ten istý príkaz vracal `NRESET` — pole sa mení,
takže to nebol zvyšok z bootu.

`fk reinit` príjem okamžite vrátil (po 7 minútach hluchoty). Hneď po ďalšom
vysielaní `vbat` klesol z 3313 na **2455 mV** a pribudol bit `errors=0x0200`.

### Väzba na vysielanie — platila, potom prestala

V behu #340 prišlo **všetkých päť zlyhaní do 0,3–2,5 s po vysielaní**. Od buildu
#343 doska padá **aj s vypnutým `repeat`**, teda bez vysielania. Väzba na TX teda
nie je celý obraz.

### Čo je vylúčené

**Anténa a filter.** Úrovne príjmu sú pred poruchou aj po nej nerozlíšiteľné:

| build | medián RSSI | najlepší RSSI | medián SNR |
|---|---|---|---|
| #339 (pred) | −47 dBm | −34 | 13,2 |
| #340 (počas) | −47 dBm | −34 | 13,2 |
| #342 / #343 (po) | −48 dBm | −34 | 13,5 |

Poškodená vstupná cesta by citlivosť zrazila o desiatky dB. Vysielací výkon sa
počas celého merania nemenil, `_prefs.tx_power_dbm` bolo trvalo 7.

**Pozor na informačný riadok pri boote:** `tx=14dBm req -> ~22 dBm module out`
počíta z konštanty `LORA_TX_POWER` z prekladu, nie z behovej hodnoty. Skutočne
nastavený výkon hlási až `radio re-initialised OK (... tx=7)`.

### Dve chyby v mojej vlastnej oprave

1. **Zaplavenie logu.** Nahodenie sa opakovalo v každom kole `loop()` a zakaždým
   vypisovalo — za 6,5 minúty 177 641 riadkov, čo samo spomalilo slučku. Výpis je
   odvtedy obmedzený na jeden riadok za 5 s.
2. **Počítadlo série sa nenulovalo.** `_n_rearm_run` klesalo na nulu len vtedy, keď
   uspelo *opakovanie*. Po zotavení sa tá vetva už nevykonala, hodnota ostala visieť
   a strážca strieľal každých 10 s na rádiu, ktoré bolo v poriadku (`run=467`
   nemenné cez tri zásahy). Nuluje sa teraz pri **každom** úspešnom nahodení.

### Otvorené

Zmena `state = (len > 0) ? STATE_RX : STATE_IDLE` je od buildu #344 za behovým
prepínačom `fk lenstate on|off`, **východzie je pôvodné správanie**, aby sa dalo
zmerať, či súčasné padanie nespôsobuje práve ona.

## Overené na železe 28. 8. 2026 predpoludním — obe podoby sa zotavia samy

Porucha chodí **v epizódach**. Mimo epizódy je vysielanie neškodné (build #345:
24 minút, 118 prijatých, 31 vyslaných, nula zásahov). Vnútri epizódy zabije príjem
prakticky každé vysielanie.

Preto sa nedá nič usudzovať z čistého okna hneď po flashi či reštarte — to je tá
istá pasca, ktorá ma pri tomto probléme oklamala už dvakrát.

### Dôkaz väzby na vysielanie

Build #344, jediná zmenená premenná bol `repeat`:

| fáza | trvanie | prijaté | vyslané | zlyhania |
|---|---|---|---|---|
| A — `repeat off` | 25,3 min | **277** | 1 | 1 |
| B — `repeat on` | prvý paket | 1 | 1 | 1 |

Odstupy zlyhaní od vysielania: 0,609 s a 0,318 s. Po prepočítaní sedia aj staršie
buildy (#341: 6 TX / 6 zlyhaní, #342: 16 / 13, #343: 5 / 7) — väzba je zhruba jedna
k jednej s vysielaním, príjem nespôsobuje nič.

Zachytený aj typický spúšťač z praxe:

```
11:37:01.9  RX #147   <- prisiel LoRa CLI prikaz „get repeat"
11:37:02.5  TX #40    <- doska odoslala odpoved
11:37:02.8  -707      <- 0,308 s po tom vysielani
11:37:04.7  zotavenie -> vbat=3316mV, pram loaded=YES, prijem spat
```

To vysvetľuje staršie hlásenie „skúšal som sa naň cez LoRa prihlásiť a prestalo to
prijímať" — prihlásenie donúti repeater odpovedať a odpoveď je vysielanie.

### Dve podoby, dva detektory

| podoba | čo ju zachytí | čas do zotavenia |
|---|---|---|
| `SetRx` odmietnutý (−707) | strážca nahodenia (`radioRearmGuard`) | ~2 s |
| čip ticho prestane merať, RSSI konštantne −255 | pravidlo watchdogu cez RSSI | ~60 s |

Druhú podobu strážca nahodenia **nikdy neuvidí**: wrapper si myslí, že je v Rx,
takže sa o nahodenie ani nepokúsi, a bez pokusu nie je čo odmietnuť.

Preto bol zrušený `FK_RADIO_DIAG_ONLY` — pravidlo cez RSSI je jediné, čo tú podobu
chytí. Výpis okna ostal, len sa už nekončí návratom. Overené na skutočnej poruche:

```
11:47:41  CONSTANT (1/3)
11:48:11  CONSTANT (2/3)
11:48:41  CONSTANT (3/3) -> zotavenie
11:48:41  vbat=3315mV, pram loaded=YES, errors=0x0000
11:49:11  rssi-window n=1024 max=-113 spread=142   (prijimac znovu meria)
```

### Čo to nerieši

Príčinu. Uzol sa vracia k životu sám namiesto toho, aby ostal hluchý do reštartu,
čo je pre vzdialený beh podstatné — ale vnútri epizódy stojí každá administrátorská
odpoveď cez LoRa dve sekundy hluchoty.

Ďalší krok, keď bude doska po ruke: **osciloskop na 3V3 pri module počas vysielania.**
Softvérovou náhradou je zníženie vysielacieho výkonu a porovnanie počtu zlyhaní na
vysielanie vnútri tej istej epizódy.

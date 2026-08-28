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

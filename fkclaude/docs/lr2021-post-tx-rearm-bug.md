# LR2021: po vysielaní môže zlyhať nahodenie príjmu (−706)

Nájdené a potvrdené 28. 8. 2026. **Netýka sa to pretečených SPI odpovedí** — je to
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

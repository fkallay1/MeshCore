# Runbook — test jgromesovej opravy (RadioLib issue 1857)

Pripravené 21. 8. 2026 ~03:20. **Dosky sa pri príprave nedotkli** — `fk pretype on`
beží ďalej, flashovanie až po vyhodnotení o 08:00.

## Čo sa testuje

Vetva `1857-lrxxxx-read-busy`, commit `623de00c821317320392f4f42bf8a5a5aa38e5bf`,
`src/modules/LR11x0/LR_common.cpp` +10/-0: poll BUSY v `LRxxxx::SPIcommand()` medzi
zápisom opcode a čítaním odpovede, s timeoutom → `RADIOLIB_ERR_SPI_CMD_TIMEOUT`.

## Prečo musí ísť náš guard von

`readRxPktLenWithStatus()` v `CustomLR1110.h` / `CustomLR2021.h` volá
`mod->SPIwriteStream()` a `mod->SPIreadStream()` **priamo**, teda mimo
`LRxxxx::SPIcommand()`. Jeho fix je vnútri `SPIcommand()`, takže na našu cestu vôbec
nedosiahne. Bez vypnutia guardu by test nemeral nič.

**Vedľajší nález na neskôr:** slabé čakanie je v `Module::SPItransferStream()`
(`delayMicroseconds(1)` + poll). Jeho oprava v `SPIcommand()` pokryje všetky *jeho*
get príkazy, ale nie cudzí kód, ktorý si strímy skladá sám — ako náš wrapper. Ak
guard raz zahodíme, treba to overiť.

## Pripravený build

Worktree `…/scratchpad/mc-rl1857`, vetva `test/rl1857-busy-wait` (z `features/nrf-fota`).
Tri zmeny:

1. `platformio.ini` — pin RadioLib `6d893483…` → `623de00c8…`
2. `platformio.ini` — do globálnych `build_flags` pridané `-D FK_STALE_GUARD_OFF=1`
3. `CustomLR1110.h` + `CustomLR2021.h` — nová vetva `#ifdef FK_STALE_GUARD_OFF`, ktorá
   dĺžku číta kniznično (`LR1110::/LR2021::getPacketLength`), takže ide cez
   `SPIcommand()`. Ostatná logika (HEADER_ERR → `standby()`) **zostáva nedotknutá**.

Postavené a OK:

| env | doska | port / DFU |
|---|---|---|
| `t1000e_repeater_fota` | T1000-E (LR11x0) | COM22 / COM23 |
| `Xiao_nrf52_nicerf_lora2021f33_repeater_fota` | XIAO NiceRF (LR2021) | COM21 / **COM20** |

XIAO: `flash-dfu` na COM20 zlyháva (drží jeden port), ide to ručne cez
`adafruit-nrfutil`.

## Ako sa to bude merať

`spifix=` je pri `FK_STALE_GUARD_OFF` **štrukturálne 0** — `_stale_pktlen_reads` sa
nikdy neinkrementuje. Ako signál je teda nepoužiteľné a nesmie sa s ním
argumentovať. Merať treba pôvodný symptóm:

* **LR2021 (XIAO):** `len=4` v RAW logu na rámcoch, ktoré sú dlhšie
* **LR11x0 (T1000-E):** dĺžka 0 → tiché zahodenie, teda rastúci rozdiel `isr` − `rxpkts`
* obe: `rxerr`, `miss`, a či sa počítadlá nezasekli

### POZOR — `spifix=425` na T1000-E NIE je kontrolná hodnota

Overené 21. 8. ~03:30 z dumpov v logu (22:14, 01:33, 01:37 — 24 z 425 udalostí,
teda ~6 %): **všetky sú falošné poplachy.** Každá vyzerá takto:

```
fp=0 first=0 final=0 irq=0x38 stat=0x7 cmd=3 tries=1 rule=FP (falosny)
```

* `irq=0x38` / `0xB8` = header error / CRC error → dĺžka **naozaj je 0**
* `cmd=3` = `CMD_DAT` → hlavné pravidlo (status) ich neoznačilo
* `first == final` → guard do dĺžky nezasiahol
* `rule=FP` sám: `fp = irq>>24` je pre `0x38`/`0xB8` **nula** a skutočná dĺžka tiež,
  takže `val == fp` platí čírou zhodou

Na LR11x0 je dĺžka **jednobajtová** z `irq[31:24]`, preto je tá zhoda triviálna; na
LR2021 je dvojbajtová a nevzniká — odtiaľ XIAO `spifix=0`.

Vysvetľuje to aj `miss=427` ≈ `spifix=425` — **je to tá istá populácia** pokazených
rámcov, nie dve nezávislé čísla.

**Ostrá cesta (to, čo je v PR 3261) sa takto nechová**: počíta len pravidlo statusu
(`if (stat == CMD_DAT) break; _stale_pktlen_reads++;`), takže tieto udalosti by
nezapočítala vôbec. Inflácia je artefakt DIAG buildu, kde je podmienka
`if (cmd_flagged || fp_flagged)`.

**Dôsledok:** skutočných pretečených odpovedí je vo videnej vzorke **nula** (žiadna
`rule=CMD`). Kontrola pre test jgromesovej opravy teda z tohto behu **neexistuje** —
nedá sa povedať „bolo 425, teraz je 0". Ak má test niečo dokázať, treba buď nájsť
podmienky, pri ktorých `rule=CMD` naozaj vzniká (`pretype off`, rušný mesh,
súbežný re-flood), alebo použiť `fk inject`, ktorý pretečenie vyrobí umelo.

Symptóm je dávkový: v kľude sa hodiny neukáže, prichádza pri súbežnom re-floode.
Preto to chce soak, nie krátky beh.

## Rozhodovacie pravidlo pre ráno (Fedor, 21. 8. ~03:40)

Kontrolný bod je **08:00** a rozhoduje jediná otázka: **vznikla aspoň jedna udalosť
`rule=CMD`?** (teda skutočná pretečená odpoveď, nie FP falošný poplach)

* **NIE** → RadioLib sa **ešte netestuje**. `fk pretype on` beží ďalej do **10:30** —
  ráno je v meshi rušnejšie a dávkový symptóm má väčšiu šancu sa ukázať. O 08:0x
  zapísať nový baseline, aby sa dal počítať prírastok 08:00 → 10:30. Potom to isté
  vyhodnotenie znova.
* **ÁNO** → máme kontrolu, ide sa rovno testovať.

Dôvod: bez jedinej `rule=CMD` nie je proti čomu merať a výsledok testu by bol
nefalzifikovateľný — „po oprave nula" nič nedokazuje, keď bola nula aj predtým.

## Prostredie (aby sa nehľadalo znova)

* huby: promicro `:7455`, t1000e `:7422`, xiao `:7421`
* hub spúšťať pythonom `D:/FkDev/FkProj/VSC/ZephCore/.venv/Scripts/python.exe` —
  `D:\FkDev\Tools\python312` **nemá pyserial** a hub pod ním tichozlyhá
* `fota_remote_e2e.py hub --device <name>` / `cmd --device <name> "<cli>"`

## A/B balíky — tri ramená (21. 8. 12:12)

Worktree `…/scratchpad/mc-rl1857`, vetva `test/rl1857-busy-wait`, rebasnutá na
`features/nrf-fota` (obsahuje opravu `CustomLR1110` buff[1]/buff[2]).
Balíky v `…/scratchpad/ab/`. Všetky majú `-D FK_STALE_GUARD_OFF=1`, čiže dĺžku
čítajú knižničnou cestou cez `LRxxxx::SPIcommand()`.

| balík | RadioLib | firmware.bin | Δ proti CTRL | sha256[:8] |
|---|---|---|---|---|
| `CTRL_t1000e` | pin `6d893483` | 429 252 B | — | `BA5B42A2` |
| **`ISO_t1000e`** | pin **+ len jeho commit** | 429 316 B | **+64 B** | `55AFDB22` |
| `TREAT_t1000e` | jeho vetva (= master +1) | 429 316 B | +64 B | `4550C505` |
| `CTRL_XIAO` | pin `6d893483` | 492 492 B | — | `74628575` |
| **`ISO_XIAO`** | pin **+ len jeho commit** | 492 556 B | **+64 B** | `41A1D064` |
| `TREAT_XIAO` | jeho vetva | 492 636 B | **+144 B** | `CC7480B1` |

### Merať treba CTRL vs ISO, nie CTRL vs TREAT

Jeho vetva `1857-lrxxxx-read-busy` stojí na **masteri**, nie na našom pine — medzi
nimi je **14 commitov** (13 cudzích + jeho), 12 súborov. A nie sú nevinné:

```
09e0bfdd [LR2021] Allow to call packet length mode for LoRa
e3e6fdc7 [LR2021] Fix configFifoIrq()
a686bc8c [LR2021] fix CR constants, add missing header mode
b9e2c46d [LR11xx] fix actual coding rate return
f2be22b4 [LR2021] Make Rx stat readout methods public
3e8ada07 [LR2021] Fix bit width config for Vbat/temperature
267d4e95 [LR2021] Fix temperature readout
```

Medzi dotknutými súbormi je `LR2021_cmds_chip_control.cpp` (+18/-5) — tam žije
`getRxPktLength()`, teda presne funkcia, ktorej výsledok meriame. Že to nie je
teoretická obava: `TREAT_XIAO` je o **80 B väčší** než `ISO_XIAO`, čo je
skompilovaný dopad tých cudzích commitov.

`ISO` je preto **náš pin + cherry-pick `623de00c`**, diff proti pinu je
`src/modules/LR11x0/LR_common.cpp | 10 ++++++++++` a nič iné. Lokálny klon je
`…/scratchpad/radiolib-iso`, vetva `iso1857`; `platformio.ini` v testovacom
worktree naň ukazuje cez `symlink://` (PlatformIO vyrobí `.pio-link`, zdroj sa
nekopíruje).

`TREAT` sa dá použiť ako **tretie rameno** na otázku „mení niečo tých 13
commitov?", ale nikdy ako dôkaz o jeho oprave.

Flash: `adafruit-nrfutil dfu serial --package <zip> -p <dfu_port> -b 115200
--singlebank --touch 1200`, alebo balík skopírovať ako `firmware.zip` do
`.pio/build/<env>/` a použiť `flash-dfu`. Po flashi overiť podľa `sha=` z bootu.

**POZOR na `flash-dfu` bez `--rebuild`** — vezme najnovší ARCHÍVNY zip z `builds/`
a na t1000e flashne stovky buildov starý firmware (21. 8. nahralo #370 namiesto
#495). Trap je zdokumentovaný priamo v `resolve_zip()`.


## ODVOLANÉ 21. 8. 19:50 — teória „predradený príkaz maskuje preteku"

Tvrdil som, že knižničná cesta (`LR2021/LR11x0::getPacketLength()` si predradia
`getPacketType()`) aj náš DIAG cyklus (`getIrqStatus()` pred každým čítaním) sú na
preteku slepé, a že to vysvetľuje nuly. **Neplatí.** Pôvodné pozorovanie z buildu
#445 (`fcl_readme_nicerf_lora2021.md`) vzniklo práve na knižničnej ceste — vtedy
guard neexistoval, `getPacketLength()` šla priamo do `LR2021::getPacketLength()`,
teda do cesty s predradeným `getPacketType()`. Dôvod dnešných núl teda **nie je
vysvetlený**.

## Prečo sú nuly — pravdepodobne len málo rámcov

Pôvodné pozorovanie: `RX RAW #47097 len=4 … path[13] rssi=-24`, ten istý rámec videl
ProMicro ako `len=50`. Vždy presne 4, či mal rámec 38, 50, 126 alebo 133 B. Dialo sa
to **v zhlukoch, keď to isté reflooduje niekoľko repeaterov naraz**.

Kľúčové: bolo to pri RAW počítadle **47 097**. Dnešné fázy majú 1 400 – 9 400 rámcov.
Ak je prirodzená frekvencia rádu jednotiek na desaťtisíce rámcov, dnešné nuly nie sú
dôkaz proti ničomu — len sme nenabrali dosť prevádzky.

Zo záznamov guardu na LR2021 sú **všetky** zachytené udalosti injektované
(`*` v dumpe = `e.inj`): 22× `rule=CMD+FP` s `*`, a 2× `rule=FP (falosny)` na
68 B rámcoch. Prirodzenú preteku guard **nikdy nezachytil** — jediný prirodzený
dôkaz je to pozorovanie z #445 z čias, keď guard neexistoval.

**Dôsledok pre test:** zmysluplné A/B potrebuje desaťtisíce rámcov na rameno, teda
dni, alebo injekciu — ktorú jgromes výslovne nechcel. To treba povedať aj jemu.


## FAZA REPRO-445 (od 21. 8. 20:03) + kontrolny bod 22. 8. 08:07

Fedorov navrh: preflashnut XIAO na **povodny build #445**, teda ten, na ktorom sa
20. 8. symptom pozoroval. Nema guard ani DIAG, takze komolena dlzka **padne do RAW
logu neopravena** a vidi sa priamo, nie cez pocitadlo.

DFU balik pre #445 sa musel vyrobit — archiv ma len `.bin` a `.uf2`. Format je
trivialny: `firmware.dat` = 14 B, `<H device_type=0x52><H dev_rev=0xFFFF>
<I app_ver=0xFFFFFFFF><H sd_len=1><H sd=0x0123><H crc16>`, kde crc16 je
`binascii.crc_hqx(bin, 0xFFFF)`. Overene na existujucom balíku (crc sedelo), potom
zlozene pre #445 do `test_nrf-fota/builds/xiaonicerf.fw_445.zip` a flashnute cez
`flash-dfu --build 445`.

| doska | build | uloha | detektor |
|---|---|---|---|
| XIAO | **#445** | reprodukcia, bez guardu | `len=4 ` v RAW logu |
| T1000-E | #303 | NODIAG guard, opravene citanie | `spifix=[1-9]` |
| ProMicro | #498 | referencia | krizove parovanie podla `first=` |

### Ktore dlzky su a nie su symptom

Detektor som zuzoval **trikrat** a dvakrat plano vyrusil. Overene z logov:

* `len=68` — bezna dlzka (24x za den), plus znamy FP falosny poplach guardu
* `len=12` — **legitimny ACK**: 1 hdr + 1 pathlen + 4 cesta + 6 payload. Ten isty ACK
  vidno v hopoch 12 → 13 → 14, kazdy hop pripise bajt do cesty
* `readData ERR (CRC_MISMATCH)` — obycajny pokazeny ramec, v logoch ich su tisice
* **`len=4` je jedina nemozna hodnota** a zaroven presne `irq[31:16]` pri `RX_DONE`

### Statisticka sila — nula NIE JE vysledok

Povodne pozorovanie bolo pri RAW **#47097**, readme uvadza 4 konkretne ramce →
rádovo **1 udalost na ~12 000 ramcov**. XIAO nabiera **~930 ramcov/h**, cize:

* 3,6 h = 3 344 ramcov → ocakavanych **0,3** udalosti
* 1 ocakavana udalost = **~13 h**
* pocet z povodneho pozorovania = **~50 h, teda dva dni**

Preto sa nesmie tvrdit „nereprodukovali sme to", kym nie je vzorka dost velka.

Citlivost dosiek (merane 21. 8.): XIAO 934 ramcov/h a chybovost 1,2 %; T1000-E a
ProMicro 732/h, ProMicro 4,6 %. **XIAO je najcitlivejsi** — symptom sa ukaze najskor
tam, ale jeho ramec nemusi mat s cim sparovat, ak ho ostatne dosky nezachytili.

## Po teste

Report do RadioLib issue 1857 — sľúbené, viď `PRs/rl-1857-comment-test-offer.md`.
Worktree potom `git worktree remove`, vetvu `test/rl1857-busy-wait` zmazať.

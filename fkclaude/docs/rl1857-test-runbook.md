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

## Po teste

Report do RadioLib issue 1857 — sľúbené, viď `PRs/rl-1857-comment-test-offer.md`.
Worktree potom `git worktree remove`, vetvu `test/rl1857-busy-wait` zmazať.

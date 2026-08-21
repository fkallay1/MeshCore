# Odpoveď jgromesovi na RadioLib 1857 — prisľúbenie testu jeho vetvy

**ODOSLANÉ 2026-08-21** ako komentár v RadioLib issue 1857. Cieľ: `jgromes/RadioLib`, issue 1857.

Kontext: jgromes 20. 8. potvrdil príčinu z datasheetu a pushol vlastný fix na vetvu
`1857-lrxxxx-read-busy` (1 commit `623de00c8`, `src/modules/LR11x0/LR_common.cpp`,
+10/-0) — poll BUSY v `LRxxxx::SPIcommand()` medzi zápisom príkazu a čítaním
výsledku, s timeoutom a `RADIOLIB_ERR_SPI_CMD_TIMEOUT`. Žiada overenie na železe
bez úprav SPI konfigurácie.

Prečo to vieme otestovať:

* pin RadioLib v `platformio.ini` je git commit, takže sa dá ukázať priamo na jeho
  vetvu (`...RadioLib.git#623de00c8...`) — netreba release
* železo máme na obe rodiny: LR2021 (XIAO+NiceRF) a LR11x0 (T1000-E)
* **musíme si vypnúť vlastný guard** — náš `getPacketLength()` v `CustomLR2021.h` /
  `CustomLR1110.h` číta mimo `SPIcommand()`, takže jeho fix by sa v našej ceste
  vôbec neprejavil a `spifix=` by sa nezmenil. Test musí ísť cez knižničnú cestu.
* symptóm je dávkový (v kľude hodiny nič, pri súbežnom re-floode ~tretina rámcov),
  takže to chce dlhší soak v živom meshi, nie krátky beh

POZOR: obe dosky (COM21, COM22) práve držia pasívny pokus `fk pretype on`, ktorý
potrebuje hodiny bez rebootu. Flashovať až po jeho dokončení — preto v texte
žiadny termín.

Príkaz (telo = len časť za posledným nadpisom Body; `rindex`, nie `index`):

```bash
D:/FkDev/GHcli/bin/gh.exe issue comment 1857 --repo jgromes/RadioLib --body-file body.md
```

Pravidlá: bez zmienky o AI, bez mriežkových čísel, bez URL (ich CONTRIBUTING žiada
krátkosť; odkaz tu netreba, sme v tom istom issue).

## Body

Thank you — that is the right layer, and it covers far more than the length read: the same race reaches `getRSSI()`, `getSNR()` and the rest, and those have no fingerprint to test against, so they could not be guarded from outside the driver at all.

I will test the branch on both families, on an LR2021 board and on a T1000-E, with the SPI configuration untouched. One thing worth saying about the method: the guard I had been running reads the length outside `SPIcommand()`, so it would hide the result — I will take it out for the test and let the library path do the work, then watch for the original symptom instead, a length of 4 on LR2021 and 0 on LR11x0 with frames silently dropped.

It will need a soak rather than a short run. In quiet periods the race does not show for hours; it appears in bursts when several repeaters re-flood the same packet at once, which is where about a third of frames were being lost. I will report back once it has had enough live traffic to mean something.

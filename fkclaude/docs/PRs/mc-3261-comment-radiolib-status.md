# Odpoveď oltacovi na PR 3261 + doplnenie odkazu do tela PR

**ODOSLANÉ 2026-08-21.** Telo PR 3261 doplnené o odkaz + komentár odoslaný (odstup 2 min medzi krokmi). oltaco sa 21. 8. 2026 spýtal na PR 3261: *„Nice work, have you opened
an issue/PR at RadioLib?"*

Stav, z ktorého odpoveď vychádza:

* RadioLib issue **1857** je otvorené (podané 20. 8.), a **jgromes už reagoval**:
  potvrdil to z datasheetu, zredukoval príčinu na chýbajúci BUSY wait medzi dvoma
  polovicami read transakcie, a **pushol vlastný kandidát fixu** na vetvu
  `1857-lrxxxx-read-busy`. Žiada overenie na železe bez SPI sabotáže.
* RadioLib **PR sme nepodali** a už ani netreba — fix si píše sám.
* Telo PR 3261 issue **spomína slovami, ale bez odkazu** (riadok „That part has been
  reported to RadioLib as an issue"). To je tá diera, kvôli ktorej sa pýta.
* Telo PR už obsahuje aj argument o pine, takže ho v komentári neopakujeme, len naň
  ukážeme. Pin sedí na commite z **11. 7.**, jgromesova vetva je z **20. 8.**

Dva kroky:

**1. Doplniť odkaz do tela PR 3261** — jediná zmena, vloženie URL:

```
...reported to RadioLib as an issue (https://github.com/jgromes/RadioLib/issues/1857), with...
```

**2. Poslať komentár** (telo = len časť za posledným nadpisom Body; `rindex`, nie `index`):

```bash
D:/FkDev/GHcli/bin/gh.exe pr comment 3261 --repo meshcore-dev/MeshCore --body-file body.md
```

Pravidlá: bez zmienky o AI, bez mriežkových čísel (`#1857` by sa v MeshCore vyhodnotilo
voči ich repu), URL na iný repo je v komentári v poriadku a je tu žiadaná.

## Body

Yes — https://github.com/jgromes/RadioLib/issues/1857. The description mentioned it only in words, which was not much use; I have put the link in there as well.

jgromes agreed and put the cause more sharply than I had: the read transaction does not wait for BUSY between sending the opcode and reading the reply, which the LR1110 datasheet requires. He has pushed a candidate fix to the `1857-lrxxxx-read-busy` branch and asked me to verify it on hardware without the SPI sabotage the measurements used. I will report back there once it has run in a live mesh.

What that means for this PR is in the description already: the proper fix belongs in the driver's transport layer, and once it lands this guard becomes redundant and can be dropped — but not before the pinned RadioLib commit is moved past it, and the pin currently sits on 11 July.

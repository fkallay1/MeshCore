# Handoff: rozhýbať upstream PR 2978 (serial CLI lockup)

Vstup pre novú session. Predchádzajúca session riešila niečo iné (SPI preteka na
LR11x0/LR2021) a nechala tam rozbehnuté veci — viď **Nedotýkať sa** na konci.

## Úloha

Upstream PR **2978** v `meshcore-dev/MeshCore` visí od 18. 7. 2026 bez jediného
CI checku. Treba ho rozhýbať: rebase vetvy na aktuálny `upstream/dev` a push, čo by
malo spustiť build workflow. Nič iné sa v tom PR meniť nemá — zmena je v poriadku
a odsúhlasená nebola len preto, že sa na ňu nikto nepozrel.

## Čo ten PR opravuje

Vetva `fix/serial-cli-buffer-lockup`, jeden commit `f76c4656`, titulok
*„fix(repeater/sensor/room_server/secure_chat cli): permanent lockup without CR"*.

Podstata (z commit message): pri plnom buffri sa vynútené `''` zapíše na
`command[sizeof-1]`, ale kontrola dokončeného riadku testuje `command[len-1]`
(čo je `sizeof-2`, keď je buffer plný). Riadok sa teda nikdy nedokončí, podmienka
čítacej slučky `len < sizeof-1` ostane navždy nepravdivá a CLI prestane čítať
serial až do rebootu. Zápis na `[sizeof-1]` navyše prepíše NUL terminátor, takže
ďalší `strlen()` čita za buffer (UB). Jeden riadok, opakovaný v štyroch
firmwaroch.

## Prečo naň nikdy nebežalo CI

PR má **0 checkov** (`gh pr view 2978 --json statusCheckRollup` vracia prázdno),
`mergeable: MERGEABLE`, posledná aktivita 9. 8. — vtedy tam Fedor pridal nudge
komentár s tým, že build sa nikdy nespustil a vyzerá to na schvaľovanie workflowu
pre prvého prispievateľa.

**Čo sa odvtedy zmenilo:** 17. 8. 2026 im prešiel Fedorov PR **3218** (LR2021
setTxPower) a **ten mal 15 checkov**. Čiže tá bariéra pre prvého prispievateľa už
padla a nový push na vetvu by CI spustiť mal. To je celá hypotéza za touto úlohou.

## Postup

1. `git fetch upstream`
2. rebase `fix/serial-cli-buffer-lockup` na `upstream/dev` — vetva je lokálne aj na
   `origin`; ak by boli konflikty, ide o štyri rovnaké jednoriadkové zmeny v CLI
   čítacích slučkách, takže sa rieši ručne bez drámy
3. overiť buildom aspoň jeden env za každý dotknutý example, napr.
   `ProMicro_repeater_fota`, `Heltec_ct62_sensor`, `Heltec_E290_room_server` a
   jeden `secure_chat` env (`pio project config | grep 'env:'` na zoznam);
   `FIRMWARE_VERSION=v1.0.0` je povinná premenná
4. `git push --force-with-lease origin fix/serial-cli-buffer-lockup`
5. po ~2 minútach skontrolovať `gh pr checks 2978 --repo meshcore-dev/MeshCore`
6. ak checky nabehnú, netreba komentovať; ak stále nič, **až potom** krátky
   komentár do PR, že rebase prebehol a build sa naň stále nespúšťa

## Prostredie

* remotes: `origin` = `fkallay1/MeshCore`, `upstream` = `meshcore-dev/MeshCore`
* `gh` je na `D:\FkDev\GHcliin\gh.exe` a **nie je na PATH**
* na upstream vetvy sa robieva git worktree v scratchpade (`git worktree add …`,
  po vybavení `git worktree remove`)
* commit správy viacriadkovo cez `git commit -F <súbor>`, nie `-m` (PowerShell
  rozbíja úvodzovky)

## Konvencie, ktoré treba dodržať

* **Žiadna zmienka o AI** — ani `Co-Authored-By` v commite na upstream vetve, ani
  „Generated with" v tele PR či komentára
* **Nikdy `#číslo` ani URL v commit správe.** Fork zdieľa s upstreamom repository
  network, takže `#2978` sa vyhodnotí voči ich repu a vytvorí tam nezmazateľnú
  krížovú referenciu. Stalo sa to už dvakrát. Bezpečne: `PR 2978` bez mriežky;
  plné URL len do **obsahu súborov**
* pred pushom na upstream vetvu: `git diff upstream/dev..HEAD | grep -c "FK"`
  musí byť **0** (naše flagy majú prefix `FK_`/`FKPR_` a upstream nesmú vidieť),
  rovnako `grep -c "//sk:"` = 0
* popis PR/komentára **ukázať Fedorovi na schválenie** pred odoslaním
* do `--body-file` ide **len telo**, nie interná hlavička draftu; pri extrakcii
  hľadať nadpis cez `rindex`, nie `index`

## Nedotýkať sa

* **NEFLASHOVAŤ dosky.** Na COM21 (XIAO/LR2021) aj COM22 (T1000-E/LR1110) beží
  pasívny pokus `fk pretype on`, ktorý potrebuje hodiny bez rebootu. Každý flash ho
  vynuluje. Hub loguje do `test_nrf-fota/logs/*.log`.
* PR **3261** a RadioLib issue **1857** sú odoslané, podložené meraniami a čakajú na
  ľudí — bez nového dôvodu ich neupravovať
* žiadne ďalšie prekrsťovanie flagov

## Kde si dočítať kontext

Pamäť: `upstream-pr-lr2021-txpower` (konvencie pre upstream PR), `fk-flag-naming`
(dve vrstvy flagov a kontrola pred PR), `gh-cli-path`, `powershell-commit-quoting`.

# Zadanie: preniesť docs commity z testovacej vetvy na `features/nrf-fota`

**Cieľ:** dostať dokumentáciu tam, kam patrí, a nestratiť ju.

## Stav (3. 9. 2026)

Pracovná vetva je `test/lr2021-runtime-ab` a **osem docs commitov skončilo na nej**
namiesto na `features/nrf-fota`:

```
435f193a docs(session-46): summary session 46 + ako citat spifix
765ef600 docs(lr2021): 6 okien po 30 min - efekt jgromesovho patchu zmizol
d5e340dc docs(session-48): stav PRs, carlhodderove zistenia, kontrolor odkazov
3f80c328 docs(lr2021): A/B/A jgromesovho BUSY patchu - nerozhodnute
6591f82b docs(prs): doplnena zaverecna veta o podpore LR2021 v MeshCore
90c6fb52 docs(prs): komentar do MeshCore 2740 odoslany
7403d9d6 docs(prs): skrateny zaver komentara pre MeshCore 2740
93529ea3 docs(prs): drafty komentarov pre RadioLib 1857 a MeshCore 2740
```

`test/lr2021-runtime-ab` je `[origin/test/lr2021-runtime-ab: ahead 2]`, teda časť je
navyše ani nepushnutá.

## Čo je v nich cenné

- `fkclaude/docs/lr2021-aba-rlwait-20260903.md` — **oba A/B behy a záver**, že jedno
  striedanie klame (p 0,0014 → 0,65). Najhodnotnejší výstup z tej session.
- `fkclaude/docs/PRs/rl-1857-comment-what-we-saw.md` — draft pre jgromesa, **neposlaný**.
- `fkclaude/docs/PRs/mc-2740-comment-nicerf-questions.md` — **odoslané** 3. 9.,
  komentár `5517533661` v MeshCore issue 2740.
- `test_nrf-fota/start_hubs.bat` — nový launcher hubov.
- `fkclaude/docs/handoff/` — tieto zadania.

## Postup

Buď `git cherry-pick` tých commitov na `features/nrf-fota` (čistejšie, zachová správy),
alebo `git checkout test/lr2021-runtime-ab -- fkclaude/ test_nrf-fota/start_hubs.bat`
a jeden súhrnný commit.

## Pasce

- **Nikdy `git checkout dev`** — zahodí `skip-worktree` na
  `test_nrf-fota/build_number.txt` (na `dev` ten súbor neexistuje).
- Vyrovnanie vetiev má vlastný skill: `/sync-upstream`.
- Do commit message nikdy `#číslo` ani URL — sieť forkov vyrobí trvalé krížové odkazy.
- Toto je presne ten problém, ktorý rieši zadanie **fkclaude-samostatne-repo**. Ak sa
  Fedor rozhodne pre samostatný repo, tento prenos treba spraviť **pred** tým.

## Hotovo keď

Doky sú na `features/nrf-fota` a pushnuté.

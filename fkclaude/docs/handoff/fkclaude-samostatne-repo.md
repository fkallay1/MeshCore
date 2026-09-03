# Zadanie: presunúť `fkclaude/` do samostatného súkromného repa

**Cieľ:** rozhodnúť a prípadne postaviť. **Zatiaľ sa nič nemenilo** — je to Fedorovo
rozhodnutie.

## Prečo to vzniklo

Fedor: *„ci by sa nedal cely adresar fkclaude spravovat inde a nie v repo, lebo takto
si to vsetci mozu pozriet v mojom forku"*. Predpokladal, že jgromes odtiaľ usúdil AI.

**Ten predpoklad je pravdepodobne nesprávny** a treba to vedieť: carlhodderova veta
*„That 'table' should've given it away, let alone the bolded sentences"* ukazuje na
**formátovanie komentára**, nie na repo.

## Čo je naozaj vidieť (overené 3. 9. 2026)

- `CLAUDE.md` **v koreni repa**, prvý riadok: *„This file provides guidance to Claude
  Code (claude.ai/code)"*. **To je hlasnejšie než celý `fkclaude/`.**
- Na verejnej `features/nrf-fota` je **29 trackovaných súborov** so zmienkou o Claude
  alebo ChatGPT — medzi nimi `flasher.c`, `FotaProtocol.h`, `.gitignore`.
- `fkclaude/` = 39 súborov, len na `features/nrf-fota` a `features/fota_lr2021_testing`.
- **Vetvy, ktoré ukazujeme upstreamu, sú čisté:** `fix/lr2021-lr1110-stale-spi-reply`
  aj `fix/lr2021-set-tx-power-rx-stall` majú 0 výskytov. Recenzent PR nevidí nič.

## Druhý dôvod, silnejší než utajenie

Docs commity opakovane padajú na tú vetvu, na ktorej práve stojíme — 3. 9. ich osem
skončilo na `test/lr2021-runtime-ab` namiesto `features/nrf-fota`. Samostatné repo
tento problém **odstráni úplne**, doky prestanú závisieť od vetvy.

## Navrhované riešenie (Fedorov nápad, odporúčaný)

**Jedno centrálne súkromné repo pre všetky projekty**, nie repo na projekt:

```
D:\FkDev\fkclaude\                       <- sukromny repo
    MeshCore\  fcl_*.md  docs\  tools\
    ZephCore\
D:\FkDev\FkProj\VSC\MeshCore\fkclaude    -> junction
```

```
mklink /J "D:\FkDev\FkProj\VSC\MeshCore\fkclaude" "D:\FkDev\fkclaude\MeshCore"
```

Jeden commit na míľnik namiesto N. Cesta ostane `fkclaude/`, takže **žiadny skill ani
odkaz sa nerozbije** — premenovanie by stálo 45 výskytov v 19 trackovaných súboroch,
24 pamäťových súborov a 8 miest v globálnom `CLAUDE.md`.

Junction je **vec Windowsu, nie gitu** — `mklink` je vstavaný príkaz `cmd.exe`, junction
je reparse point na NTFS. Git doň jednoducho vojde ako do adresára. Admin práva netreba.

## Overené na sucho (skutočné testy, nie domnienky)

| test | výsledok |
|---|---|
| git vidí súbory cez junction | áno, ako obyčajné blob-y `100644` |
| `.git/info/exclude` ho skryje | áno, `git status` čistý |
| zápis cez junction ide do centrálu | áno |
| **`git clean -fdx`** | zmaže **len junction**, obsah cieľa prežije |
| nová vetva / prepínanie vetiev | junction sa nedotkne |
| **`git worktree add`** | nový adresár → **treba nový `mklink`** |
| `.git/info/exclude` v worktree | **dedí sa**, leží v common dir |

## Jediná pasca

Checkout **starého commitu**, kde je `fkclaude/` ešte trackovaný, zapíše staré súbory
**cez junction do živých dokov, mlčky.** Ignorovaný súbor git prepíše bez varovania —
netrackovaný by odmietol (`error: The following untracked working tree files would be
overwritten`). Tým, že si cestu vylúčime, vzdáme sa aj tej ochrany. Overené oboma smermi.

Škoda je ale **viditeľná a vratná**, lebo centrál je sám git repo — `git status` ju
ukáže ako ` M` a `git checkout -- .` vráti. Podmienka: doky commitnúť **pred** skokom
do histórie. Chytí to `git bisect`, porovnávanie so starým stavom, worktree na starý commit.

## Postup, ak sa Fedor rozhodne áno

1. Súkromný repo, napr. `fkallay1/fk.devdocs`.
2. `git rm -r --cached fkclaude/` na oboch feature vetvách (súbory na disku ostanú).
3. Vylúčiť cez **`.git/info/exclude`**, nie `.gitignore` — ten sa commituje a sám by
   prezradil, že tam ten adresár je.
4. `cd fkclaude && git init && git remote add ... && git push`
5. Junction podľa vyššie.
6. `CLAUDE.md` riešiť **zvlášť** — je nápadnejší a presúvať sa nedá, len prestať trackovať.
7. Zapísať do skillu: junction aj `exclude` sú vec pracovnej kópie, po novom klone
   (a po `git clean -fdx`) ich treba nastaviť znova — rovnaký typ veci ako
   `skip-worktree` na `test_nrf-fota/build_number.txt`.

## Čo už je vonku, vonku zostane

Mazací commit históriu neodstráni. **Ani prepis histórie** — vo fork network ostávajú
objekty dosiahnuteľné cez SHA aj z URL priestoru iných repozitárov v sieti. Jediné, čo
naozaj zaberie, je zmazať fork celý, čo by zabilo otvorené PR (3261, 2978). Dopredu to
zmysel má, spätne nie.

## Zvážená a odmietnutá alternatíva

Doky do už existujúceho súkromného `fk.claude` (`D:\FkDev\cli_home`), kde žijú pamäte —
`SessionEnd` hook by ich commitoval sám. Odmietnuté: adresáre sú tam pomenované
zmrzačenou cestou (`D--FkDev-FkProj-VSC-MeshCore`), čo sa rozsype pri presune projektu,
a automatický commit berie kontrolu nad správami.

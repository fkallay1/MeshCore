# Nedoriešené veci — vstupné info pre nové session

Stav k **9. 9. 2026**. Každý súbor je samostatné zadanie: nová session nepotrebuje nič
z predchádzajúcej konverzácie, všetko podstatné je v ňom.

## Odporúčané poradie

| # | zadanie | prečo v tomto poradí |
|---|---|---|
| 1 | [docs-commity-na-zlej-vetve](docs-commity-na-zlej-vetve.md) | osem docs commitov visí na testovacej vetve, časť nepushnutá — najprv ich zachrániť |
| 2 | [nicerf-variant-cista-vetva](nicerf-variant-cista-vetva.md) | dvaja ľudia si o variant napísali menovite; čisto technické, bez rizika tónu |
| 3 | [promicro-ozivenie-swd](promicro-ozivenie-swd.md) | doska je mimo prevádzky; zip je pripravený, treba len sondu |
| 4 | [jgromes-komentar-1857](jgromes-komentar-1857.md) | dlžíme odpoveď, issue je živé a označené `bug` |
| 5 | [zephcorews-compilecommands-regresia](zephcorews-compilecommands-regresia.md) | denne zdržuje prácu, oprava je jeden riadok |
| 6 | [fkclaude-samostatne-repo](fkclaude-samostatne-repo.md) | rozhodnutie; spraviť **po** bode 1 |
| 7 | [meshcore-pr-3261](meshcore-pr-3261.md) | závisí od bodu 4; CI runy z 20. 8. expirujú na červený krížik ~19. 9. |
| 8 | [pram-pr-do-radiolib](pram-pr-do-radiolib.md) | hotové, čaká na napísanie PR |
| 9 | [meshcore-nenahlasene-nalezy](meshcore-nenahlasene-nalezy.md) | nič nehorí |
| 10 | [promicro-hang-po-prvom-tx](promicro-hang-po-prvom-tx.md) | vyrieši sa možno sám pri bode 3 |
| 11 | [worktree-upratanie](worktree-upratanie.md) | kozmetika |

## Čo je naopak hotové

- **MeshCore issue 2740** — komentár o našom NiceRF porte a otázky na carlhoddera
  **odoslaný** 3. 9. Telo v `fkclaude/docs/PRs/mc-2740-comment-nicerf-questions.md`.
- **A/B test jgromesovho patchu** — dva behy, vyhodnotené a zapísané v
  `fkclaude/docs/lr2021-aba-rlwait-20260903.md`. Záver: **nerozhodnuté**, porucha
  sa nevyskytla ani s vypnutou opravou.
- **Dňová pečiatka v logoch** — `fota_serial_hub.py` píše `03-09:29:37.763`, takže
  hľadanie podľa času už nemôže trafiť iný deň. Staré logy v `test_nrf-fota/logs/archive/`.
- **`test_nrf-fota/start_hubs.bat`** — huby nemali žiadny launcher.
- **VS Code**: `"window.openFoldersInNewWindow": "on"`, `start_VSCode.bat` berie cestu
  ako argument.

## Trvalé pravidlá, ktoré platia vo všetkých týchto zadaniach

- **Žiadna zmienka o AI** v ničom, čo ide von — komentáre, PR, commit messages.
- **Neposielať ani neupravovať zverejnené, kým Fedor neodsúhlasí finálne znenie.**
  „Poslal by som to" ani „doplň tam vetu X" nie je súhlas: ukáž celý text a čakaj.
- **Do commit message nikdy `#číslo` ani URL** — sieť forkov vyrobí trvalé krížové
  odkazy. Stalo sa to už dvakrát.
- Von len **krátka próza v prvej osobe** — žiadne tabuľky, žiadne tučné písmo.
  jgromes aj carlhodder podľa nich rozpoznali strojový text.
- Telá GH komunikácie do `fkclaude/docs/PRs/`, názov `<repo>-<číslo>-<druh>-<téma>.md`.
- `gh` je `D:\FkDev\GHcli\bin\gh.exe`, **nie je na PATH**.
- Pri meraniach: **jedno A/B/A striedanie klame** — overuj obe hranice prepnutia
  a maj referenčnú dosku, na ktorej sa nič neprepína.

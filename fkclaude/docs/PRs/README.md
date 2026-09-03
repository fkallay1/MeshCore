# PRs — drafty a stav komunikácie cez GitHub

Sem ide **všetko, čo sa posiela na GitHub alebo odtiaľ prišlo**: telá PR, telá issues,
komentáre a odpovede. Interné handoffy a session poznámky tu **nie sú** — tie zostávajú
v `fkclaude/docs/`.

## Názov súboru

```
<repo>-<číslo>-<druh>-<téma>.md
```

* **`<repo>`** — `mc` = `meshcore-dev/MeshCore`, `rl` = `jgromes/RadioLib`. Číslo samo
  nestačí: `1857` v RadioLib a `1857` v MeshCore sú dve rôzne veci.
* **`<číslo>`** — číslo PR/issue z GitHubu, **hneď za repom**, nie na konci. Tak sa
  všetky doky k jednému PR zoradia vedľa seba (viď dva `mc-3261-*`).
* **`<druh>`** — `pr`, `issue`, `comment`.
* Ešte neodoslané a bez čísla: `<repo>-draft-<druh>-<téma>.md`, po odoslaní premenovať
  a opraviť odkazy.

## Obsah súboru

Prvý riadok pod nadpisom je **stav**: `**ODOSLANÉ <dátum>: <URL>**` alebo `**NEODOSLANÉ.**`
Po odoslaní stav prepísať hneď — inak sa dvakrát rieši, či to už išlo von.

Telo, ktoré ide na GitHub, je **za posledným nadpisom `## Body`**. Extrahovať cez
`rindex`, nie `index`, inak sa do tela dostane interná hlavička.

## Pravidlá pre telo

* žiadna zmienka o AI
* **žiadne `#číslo`** — fork zdieľa s upstreamom repository network, `#2978` sa vyhodnotí
  voči ich repu a zanechá tam nezmazateľnú krížovú referenciu. Bezpečne `PR 2978`.
* plné URL sú v poriadku v tele PR/komentára, v **commit správach nie**
* pred odoslaním: `git diff upstream/dev..HEAD | grep -c "FK"` = 0, `grep -c "//sk:"` = 0

## Kontrola stavu cez `gh`

`gh` je na `D:\FkDev\GHcli\bin\gh.exe` a **nie je na PATH**.

Upstream repozitár je **`meshcore-dev/MeshCore`**, nie `ripplebiz/MeshCore` —
`--repo ripplebiz/MeshCore` vráti prázdny zoznam bez chyby, čo vyzerá ako
„žiadne PR". Najistejšie je `gh search prs --author "@me"`, ktoré prejde všetky
repozitáre naraz.

### Červený krížik po 30 dňoch nie je padnutý build

Neschválený run (viď gate nižšie) GitHub po **presne 30 dňoch** zruší a označí
`conclusion: failure`. Na PR to vyzerá ako „náš kód nepreložil", pritom sa nič
nepreložilo.

Odmerané 28. 8. 2026 na PR 2978: tri runy s `created_at: 2026-07-29` majú
`updated_at: 2026-08-28` a `failure`.

Rozlíšenie od skutočného padnutého buildu:

```
gh api "repos/meshcore-dev/MeshCore/actions/runs/<ID>/jobs" --jq '.jobs|length'
gh api "repos/meshcore-dev/MeshCore/actions/runs/<ID>" --jq '.created_at, .updated_at'
```

**`jobs = 0` + rozdiel created/updated presne 30 dní = expirácia.** Skutočný fail
má joby aj logy. Nehľadať chybu v kóde a nerobiť „fix" push; krížik odstráni len
nový push (vyrobí čerstvé `action_required` runy) alebo to, že to maintainer
odklikne.

### `gh pr checks` na našich PR nikdy nič nezobrazí

Actions sa pre PR z forku na upstreame nespúšťajú samé — runy skončia so
`conclusion: action_required` a `gh pr checks` napíše „no checks reported",
`statusCheckRollup` vráti prázdne pole. Vyzerá to, akoby workflow nikdy nevznikol.
Stav sa číta len takto:

```
gh api "repos/meshcore-dev/MeshCore/actions/runs?branch=<vetva>" \
  --jq '.workflow_runs[] | "\(.name) \(.status)/\(.conclusion) \(.created_at)"'
```

Approval potrebuje **každý mimo org** (obchádzajú to len členovia). Nie je to
bariéra pre prvého prispievateľa a mergnutím PR 3218 nepadla — nemá zmysel na ňu
čakať ani ju „odblokovať" rebasom.

### `gh issue view --json comments` vie vrátiť staré dáta

Odmerané 3. 9. 2026 na RadioLib issue 1857: prvé volanie vrátilo **3 komentáre**
a `updatedAt: 2026-08-21`, druhé identické volanie o pár minút neskôr **7
komentárov** a `2026-09-02`. Medzi nimi sa nič neposielalo. Štyri komentáre —
vrátane jgromesovej kritiky — teda **chýbali bez akéhokoľvek varovania**.

Pri kontrole stavu preto čítať komentáre cez REST, ktorý toto neukázal:

```
gh api "repos/<owner>/<repo>/issues/<číslo>/comments?per_page=50" \
  --jq '.[] | "\(.created_at) \(.user.login) \(.body)"' | head -40
```

Platí aj pre PR — komentáre PR sú v `issues/<číslo>/comments`.

## Aktuálny obsah

Stav overený 3. 9. 2026.

| súbor | čo to je | stav |
|---|---|---|
| `mc-2739-comment-nicerf-lora2021.md` | komentár k PR 2739 (NiceRF LoRa2021 + ESP32-C3) | odoslané 20. 8. |
| `mc-2740-comment-nicerf-questions.md` | otázky carlhodderovi + oznámenie nášho portu (issue 2740) | odoslané 3. 9., **odpovedal** |
| `mc-2978-pr-serial-cli-lockup.md` | telo PR 2978 (serial CLI lockup bez CR) | odoslané 18. 7., rebase 20. 8., čaká na approve; runy expirovali 28. 8. |
| `mc-3218-pr-lr2021-txpower.md` | telo PR 3218 (LR2021 `setTxPower`) | **mergnuté** 17. 8. |
| `mc-3261-pr-lr-stale-spi-reply.md` | telo PR 3261 (zastaralá SPI odpoveď ako dĺžka) | odoslané 20. 8., bez review |
| `mc-3261-comment-radiolib-status.md` | odpoveď oltacovi + doplnenie odkazu do tela | odoslané 21. 8. |
| `mc-draft-pr-fota-upstream.md` | telo PR pre FOTA (LoRa delta-patch OTA) | **neodoslané** |
| `rl-1857-issue-stale-spi-reply.md` | telo RadioLib issue 1857 | odoslané 20. 8. |
| `rl-1857-comment-lr11x0-measurements.md` | doplnenie po meraní na LR11x0 | odoslané 20. 8. |
| `rl-1857-comment-test-offer.md` | prisľúbenie testu vetvy `1857-lrxxxx-read-busy` | odoslané 21. 8. |
| `rl-1857-comment-ten-day-result.md` | výsledok po desiatich dňoch na železe | odoslané 1. 9. |
| `rl-1857-comment-test-result.md` | skorší výsledok testu jeho opravy | neposlané, nahradené predchádzajúcim |
| `rl-1857-comment-short-reply.md` | odpoveď po jgromesovej kritike (dlhšia, s omluvou) | **na schválenie**, neposlané |
| `rl-1857-comment-what-we-saw.md` | odpoveď po jgromesovej kritike (kratšia, vecná) | **na schválenie**, neposlané |

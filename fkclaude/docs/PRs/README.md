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

## Aktuálny obsah

| súbor | čo to je | stav |
|---|---|---|
| `mc-2739-comment-nicerf-lora2021.md` | komentár k PR 2739 (NiceRF LoRa2021 + ESP32-C3) | odoslané 20. 8. |
| `mc-2978-pr-serial-cli-lockup.md` | telo PR 2978 (serial CLI lockup bez CR) | odoslané 18. 7., čaká na approve |
| `mc-3218-pr-lr2021-txpower.md` | telo PR 3218 (LR2021 `setTxPower`) | **mergnuté** 17. 8. |
| `mc-3261-pr-lr-stale-spi-reply.md` | telo PR 3261 (zastaralá SPI odpoveď ako dĺžka) | odoslané 20. 8. |
| `mc-3261-comment-radiolib-status.md` | odpoveď oltacovi + doplnenie odkazu do tela | odoslané 21. 8. |
| `mc-draft-pr-fota-upstream.md` | telo PR pre FOTA (LoRa delta-patch OTA) | **neodoslané** |
| `rl-1857-issue-stale-spi-reply.md` | telo RadioLib issue 1857 | odoslané 20. 8. |
| `rl-1857-comment-lr11x0-measurements.md` | doplnenie po meraní na LR11x0 | odoslané 20. 8. |
| `rl-1857-comment-test-offer.md` | prisľúbenie testu vetvy `1857-lrxxxx-read-busy` | odoslané 21. 8. |

# Zadanie: tri nenahlásené nálezy v MeshCore rádiovej obálke

**Cieľ:** rozhodnúť, čo s nimi — nahlásiť, opraviť PR-kom, alebo nechať u nás.

## Nálezy (nájdené pri ladení LR2021, žiadny nie je nahlásený)

1. **Chýbajúca maska `STATE_INT_READY`.** Stavy obálky sú `STATE_IDLE=0`, `STATE_RX=1`
   a `STATE_INT_READY`, ktorý ISR **priORuje** k stavu. Porovnania stavu, ktoré ten bit
   nemaskujú, sa preto pri súbehu s prerušením správajú nečakane.

2. **Natvrdo držaný `STATE_RX`.** V `recvRaw()` sa stav nastavoval na `STATE_RX`
   bez ohľadu na to, či sa paket naozaj prečítal. U nás je to obídené cez
   `_fk_lenstate`:
   `state = _fk_lenstate ? ((len > 0) ? STATE_RX : STATE_IDLE) : STATE_RX;`

3. **`isChipResponding()` / `chipResponds()` je na SX1262 vadné.** Nameraných
   **0 z 6000** — hlási nedostupný čip aj na zdravom rádiu. Preto sa v našom
   watchdogu nepoužíva a detekcia stojí na pasívnom RSSI okne.

## Kontext, ktorý treba mať

Súvisiace veci sú v pamäti: `radio_watchdog_passive_rssi`,
`lr2021_post_tx_rearm`, `fota_raw_log_path`.

Náš watchdog (`FKPR_RADIO_WATCHDOG`) je overený na železe vrátane zotavenia — 62
zotavení, nula zlyhaní — a je zapnutý na **všetkých** FOTA variantoch po tom, čo
ProMicro (SX1262) sedel hluchý 8,5 h, kým watchdog správne hlásil, ale nekonal.

## Postup

1. Pre každý nález rozhodnúť: upstream issue, upstream PR, alebo len naša vetva.
2. Bod 3 je najlepšie doložený (0/6000) a najmenej sporný — kandidát na samostatný
   krátky issue.
3. Body 1 a 2 spolu súvisia a chcú si sadnúť s upstream kódom, či sa medzitým nezmenil.

## Pasce

- Písať krátko a vecne, bez tabuliek a tučného písma — v tomto projekte nás už raz
  zhodili za „strojový" text.
- Telá do `fkclaude/docs/PRs/`, názov `mc-<číslo>-<druh>-<téma>.md`.
- Text vopred Fedorovi na schválenie.

## Hotovo keď

Pre každý z troch nálezov je rozhodnuté a rozhodnutie zapísané.

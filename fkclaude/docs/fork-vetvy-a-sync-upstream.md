# Fork `fkallay1/MeshCore` — vetvy, build_number a história syncov s upstreamom

> Stavové údaje k postupu v skille `sync-upstream`.
> Skill drží **ako** (kroky, pasce), tento dokument drží **čo a kedy** (inventár vetiev, hodnoty,
> konfliktné miesta, história). Pri každom syncu sem doplniť riadok do histórie a prípadne nové
> konfliktné miesto.

## Vetvy forku

Remotes: `upstream` = `meshcore-dev/MeshCore`, `origin` = `fkallay1/MeshCore`.

| Vetva | Úloha | Pozn. |
|---|---|---|
| `main` | sleduje `upstream/main` | neudržiavaná |
| `dev` | **čistá kópia `upstream/dev`**, bez vlastných commitov | fast-forward bez checkoutu (`git fetch . upstream/dev:dev`) |
| `features/nrf-fota` | hlavná pracovná vetva (LoRa-FOTA) | merge cieľ pre `dev` |
| `features/fota_lr2021_testing` | FOTA + LR2021 (NiceRF) testovanie | merguje sa do nej `features/nrf-fota`, nie `dev` |
| `test/lr2021-runtime-ab` | A/B testy rádia, **nesie najnovšie `fkclaude/` docs a skilly** | reťaz `features/nrf-fota` → `features/fota_lr2021_testing` → `test/lr2021-runtime-ab` (od 2026-10-02) |
| `feat/nicerf-lora2021f33-xiao` | čistá PR vetva variantu NiceRF (základňa `upstream/dev`) | **nemergovať `dev`**, len rebase; detail [nicerf-cista-vetva-20260909.md](nicerf-cista-vetva-20260909.md) |
| `test/nicerf-lora2021f33-hw` | FK diagnostika nad čistou vetvou (env `…_repeater_diag`) | worktree `D:\FkDev\wt\nicerf` |
| `fix/lr2021-lr1110-stale-spi-reply` | upstream PR 3261 | PR vetva |
| `fix/lr2021-set-tx-power-rx-stall` | re-arm fix po TX na LR2021 | PR vetva |
| `fix/serial-cli-buffer-lockup` | upstream PR 2978 (prekonaný upstream #3395) | PR vetva |
| `fix/repeater-btn-boot-level` | user_btn na xiao variante | kandidát na PR |
| `features/nrf-fota-dualguard` | zmergnutá do `features/nrf-fota` (3dd39996, 2026-07-13) | zachovaná na origine |
| `features/nrf-ota`, `features/nrf-debug`, `test/lr2021-pram`, `test/rl1857-*`, `test-stareho-stashu` | historické / testovacie | `test-stareho-stashu` nesie Heltec V3 FK varianty |

Reťaz merge: `upstream/dev` → `dev` → `features/nrf-fota` → `features/fota_lr2021_testing` →
`test/lr2021-runtime-ab`. PR vetvy (`fix/*`, `feat/*`) sa prípadne **rebasujú** na `upstream/dev`,
nikdy sa do nich nemerguje `dev`.

Worktree len do krátkych ciest `D:\FkDev\wt\<meno>` (worktree v session scratchpade prekročí
MAX_PATH a PlatformIO build padne na „chýbajúci" `PhysicalLayer.h`).

### Overovacia dávka buildov

Po merge do `features/nrf-fota`: `ProMicro_repeater_fota`, `RAK_3401_repeater_fota`,
`RAK_4631_repeater_fota`, `SenseCap_Solar_repeater_fota`, `t1000e_repeater_fota`,
`Xiao_nrf52_repeater_fota`, `Xiao_nrf52_companion_radio_usb` (~7 min). Po merge do LR2021 vetiev
navyše `Xiao_nrf52_nicerf_lora2021f33_repeater[_fota]` + `t1000e_repeater_fota`.

## build_number.txt — hodnoty v repe

`test_nrf-fota/build_number.txt` je lokálne `skip-worktree`, v repe je neutrálna hodnota, aby
checkout medzi vetvami do súboru nesiahal (stav 2026-10-22, `git show <vetva>:…`):

| Vetva | Hodnota v repe |
|---|---|
| `features/nrf-fota` | 310 |
| `features/nrf-fota-dualguard` | 300 |
| `features/fota_lr2021_testing` | 300 |
| `test/lr2021-runtime-ab` | 300 |
| `feat/nicerf-lora2021f33-xiao`, `dev` | súbor neexistuje |

Lokálne reálne číslo (2026-10-22 na `test/lr2021-runtime-ab`): 350. Rozdiel 310 vs 300 znamená, že
prepnutie `features/nrf-fota` ↔ LR2021 vetvy hlási „would be overwritten by checkout" — postup je
v skille (uložiť, `--no-skip-worktree`, checkout, vrátiť, `--skip-worktree`). Pôvodná konvencia
(2026-07-12): 300 v `features/nrf-fota` (3b535495) aj `dualguard` (7f0b896f), zvolené nad históriou
oboch vetiev (nrf-fota došla po 269, dualguard po 298).

## Známe konfliktné miesta pri merge upstreamu

Rastie s každým syncom. Riešenia sú overené buildom.

| Súbor | Čo sa deje | Riešenie |
|---|---|---|
| `examples/simple_repeater/MyMesh.cpp` `onAnonDataRecv()` | Upstream (v1.17.1) prešiel na `mesh::chooseReplyRoute()` (`src/helpers/RoutingPolicy.h`), pribudol `REPLY_ROUTE_DIRECT_OUT_PATH`, sentinel `reply_path_len` `-1` → `0xFF` (typ `uint8_t`) | Vziať upstream štruktúru, navrstviť naše: `FK_FLOOD_RESP_DELAY` vo flood vetvách + `fkAnonFallbackArm()` v `REPLY_ROUTE_PATH_RETURN`. Pozor na mŕtve `reply_path_len < 0`. |
| `variants/xiao_nrf52/target.{cpp,h}` | **Tichý zlý auto-merge** (git nehlási konflikt): upstream pridal `MomentaryButton user_btn(PIN_USER_BTN,1000,true,true)` do `#ifdef DISPLAY_CLASS`, my máme inertnú `(-1,1000,true)` (commit `7b651a41`) v `#ifdef PIN_USER_BTN` → `redefinition`, build FAIL | Jedna deklarácia v upstream bloku `DISPLAY_CLASS` s **našimi** parametrami (pin −1 = žiadny fantómový power-off), duplicitný `extern` v `target.h` preč. |
| `examples/simple_*/main.cpp` serial CLI plný buffer | Upstream #3395 (2026-09-11) opravil rovnako ako náš PR #2978 (`sizeof-2`) + drží NUL | Vziať upstream (vyriešené `fb318e9d`, ďalej nekonfliktuje). |
| `src/helpers/radiolib/CustomLR1110.h` | Upstream pridal `begin()` s `setRegulatorDCDC()`, my máme stale-SPI guard | Oboje; `begin()` hneď za konštruktor, aby náš komentár ostal nad `getPacketLength()`. |
| `variants/rak4631/platformio.ini` | Upstream pridal ethernet env, my máme FOTA env (2026-07-18, ab156acc) | Keep-both. |
| `.vscode/extensions.json` | Upstream ho prestal trackovať (`b40968a0`), my držíme kvôli NoBuild task rozšíreniam | Riešené raz „keep ours" (`b9ce15c1`); ďalšie merge už nekonfliktujú (base ho nemá). |

Po merge vždy skontrolovať, či upstream nesiahol na FOTA háky — `onGroupDataRecv`,
`searchChannelsByHash`, `allowPacketForward`, `logRxRaw` (grep v `examples/simple_repeater/nrffota/`
a `MyMesh.cpp`). Upstream merge 795989b9 rozbil `Xiao_nrf52_repeater*` (repeater_btn PR bez
`user_btn` v xiao variante) — opravené u nás 6614aea5.

## História syncov

| Dátum | Čo prišlo | Konflikty / poznámka |
|---|---|---|
| 2026-07-13/14 | „38 Vyrovnanie vetiev" — prvý sync podľa runbooku | `.vscode/extensions.json` keep ours (`b9ce15c1`); dualguard zmergnutá (3dd39996) |
| 2026-07-18 | upstream/dev 61 commitov (ab156acc) → build #358 | `variants/rak4631/platformio.ini` (FOTA env vs ethernet env, keep-both); + CLI lockup fix + PATH decode |
| 2026-08-14 | upstream v1.17.1, 136 commitov | `onAnonDataRecv()` RoutingPolicy; tichý zlý merge `xiao_nrf52/target.*`; skill `sync-upstream` vznikol |
| 2026-09 | upstream #3395 (serial CLI buffer) | vzaté upstream (`fb318e9d`), náš PR 2978 prekonaný |
| 2026-10-02 | rozšírenie reťaze o `features/fota_lr2021_testing` → `test/lr2021-runtime-ab`; čistá vetva rebasnutá na upstream dev fad0ffb7 (17a9de5b, force-push) | — |

Upstream ide ~20–30 commitov/deň — „behind X commits" na GitHube deň po syncu je normálne;
syncovať pri reálnej potrebe, nie denne.

## Súvisiace

- Docs a skilly sú trackované na vetvách; od 3. 9. 2026 sú všetky docs commity len na
  `test/lr2021-runtime-ab` — handoff [handoff/docs-commity-na-zlej-vetve.md](handoff/docs-commity-na-zlej-vetve.md)
  a [handoff/fkclaude-samostatne-repo.md](handoff/fkclaude-samostatne-repo.md) (nevyriešené).
- Worktree upratanie: [handoff/worktree-upratanie.md](handoff/worktree-upratanie.md).

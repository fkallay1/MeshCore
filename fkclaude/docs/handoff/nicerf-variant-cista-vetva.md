# Zadanie: NiceRF LoRa2021F33 variant von — čistá vetva

**Cieľ:** vypublikovať náš variant **XIAO nRF52840 + NiceRF LoRa2021F33-2G4** na čistej
vetve z `upstream/dev` — tak, aby si ho vedeli vziať dvaja ľudia, ktorí si o to napísali,
a aby z toho mohol vzniknúť PR do `meshcore-dev/MeshCore`.

## Prečo — kto o to konkrétne požiadal

V MeshCore issue **2740**:

* **felixfelix-bot (4. 9. 2026):** *„your PA-table / RF-switch / SIMO offer is very
  welcome, we will happily take the whole variant"* — a osobitne vyzdvihli SIMO erratum
  (re-apply po `SetLoraModulationParams`). Lietajú balóny (~30 km) so štyrmi plain
  NiceRF LoRa2021 modulmi, takže PA verziu nemajú.
* **carlhodder (3. 9. 2026):** odpovedal na naše otázky detailne — jeho štyri dosky
  (F33-2G4 na Seeed aj na ProMicro klone, Waveshare Core2021-XF, plain LoRa2021), F33
  **má TCXO**, Waveshare osadzuje NTC (100K, beta odhadom 4250K), **SIMO induktor je
  osadený na oboch NiceRF variantoch** (pozrel pod tienením, priložil fotky). Dlžíme mu
  aspoň poďakovanie a čísla naspäť.
* **My sami** sme to ponúkli v komentári z 2. 9. („Happy to share … or the whole
  variant, if that is useful") — telo v `fkclaude/docs/PRs/mc-2740-comment-nicerf-questions.md`.

**Nekoliduje to s PR 2739.** c03rad0r ho 4. 9. rebasoval na 7 súborov (+553): **plain**
modul (160 mW, výstup čipu) na **ESP32-C3** SuperMini + opt-in `EspIdfHal`. My máme **PA
verziu** (F33-2G4, ~30 dBm) na **nRF52840**. Náš variant je nadstavba, nie konkurencia —
tak ho treba aj napísať.

## Stav: variant je hotový a odskúšaný

Beží na doske `xiao-nicerf` (COM21), build **#317**: k 7. 9. **106 h** uptime,
**47 689** rámcov, `rxerr` 4,34 %, `spifix=0`, `miss=2`, účet ISR presne sedí
(47 689 RX + 11 498 TX + 2 072 chýb = `isr` 61 261).

**Čistý env sa dnes prekladá** — `FIRMWARE_VERSION=v1.17.1 pio run -e
Xiao_nrf52_nicerf_lora2021f33_repeater` → SUCCESS za 47 s (overené 9. 9. 2026 na
`test/lr2021-runtime-ab`).

Súbory dnes na vetve `test/lr2021-runtime-ab`:

| súbor | riadkov | kontaminácia |
|---|---|---|
| `variants/xiao_nrf52_nicerf_lora2021f33/platformio.ini` | 225 | 8× `FK_`, 3× `FKPR_`, `;sk:` — **všetko iba v FOTA env** |
| `variants/xiao_nrf52_nicerf_lora2021f33/target.cpp` | 202 | 3× `FK_`, 9× `//sk:` |
| `variants/xiao_nrf52_nicerf_lora2021f33/target.h` | 30 | 1× `FK_` |
| `src/helpers/radiolib/NiceRF_LoRa2021F33.h` | 290 | **čistý** (0× FK, 0× `//sk:`) |
| `src/helpers/radiolib/lr20xx_pram_lr2021.h` | 90 | čistý, ale **do PR nepatrí** |

## Čo do PR patrí

Odhad **~550 riadkov v 4 súboroch**:

1. **`variants/xiao_nrf52_nicerf_lora2021f33/{platformio.ini, target.cpp, target.h}`** —
   z ini len základná sekcia `[Xiao_nrf52_nicerf_lora2021f33]` (r. 47–125) a env
   `[env:Xiao_nrf52_nicerf_lora2021f33_repeater]` (r. 126–138). **Oba sú už dnes bez
   jediného FK flagu** — celá kontaminácia ini sedí v FOTA envе nižšie.
2. **`src/helpers/radiolib/NiceRF_LoRa2021F33.h`** — to je jadro, čo od nás chcú:
   PA tabuľka (`NICERF_LORA2021F33_PAVAL_FOR_OUT`, 21 hodnôt, floor ~19 dBm),
   RF switch (`nicerf_lora2021f33_rfswitch_dios` / `_table`), SIMO
   (`nicerf_lora2021f33_set_simo`) a `nicerf_lora2021f33_report()`.

### Čo do PR NEPATRÍ

* **`[env:…_repeater_fota]`** (r. 148–225) — FOTA je náš samostatný projekt a práve v tom
  bloku sedia všetky FK flagy: `FKPR_RADIO_WATCHDOG`, `FK_RADIO_DIAG_CLI`,
  `FK_NICERF_LORA2021F33_TEST`, `FK_RADIO_SPI_DIAG`, `FK_LRXXXX_STALE_COUNTER`.
* **Náš `CustomLR2021.h`** (571 riadkov proti upstream 130). Celý ten prírastok je
  diagnostika — `fkRawRead()`, `fkSetSpiHz()`, `_fk_rdgap_us`, stale-reply guard,
  `getTcxoUsed()`. **Upstream verzia stačí:** TCXO fallback na `0.0f` pri −706/−707
  v nej už je. Jediné, čo z našej variant naozaj používa, je `getTcxoUsed()` v jednom
  `Serial.print` v `target.cpp` (r. 51) — ten riadok pre PR vyhodiť. (Ak by ho Fedor
  chcel zachovať, je to dvojriadkový getter, ale do prvého PR to nemiešať.)
* **`lr20xx_pram_lr2021.h` a `LR2021_PRAM_UPD`** — PRAM má nahrávať RadioLib, nie
  MeshCore (viď zadanie `pram-pr-do-radiolib.md`). Doska **beží bez neho**: v FOTA env je
  `LR2021_PRAM_UPD` zakomentovaný (r. 205) aj `NICERF_LORA2021F33_SIMO` (r. 206).
  V `NiceRF_LoRa2021F33.h` je include za `#ifdef LR2021_PRAM_UPD`, ale keď header do PR
  ide a `lr20xx_pram_lr2021.h` nie, ten `#ifdef` by bol pasca — kto flag zapne, dostane
  chybu prekladu. **Pre PR blok `LR2021_PRAM_UPD` z headera vyhodiť** (aj
  `nicerf_lora2021f33_pram_status`), u nás na vetve ostáva.
* **`variants/xiao_nrf52/target.cpp`** — náš variant má vlastný `user_btn(-1, 1000, true)`,
  takže PR to nepotrebuje. Je to však reálna chyba: na XIAO s `DISPLAY_CLASS` číta
  `PIN_USER_BTN` (D0) LOW, keď je pripojené rádio, `MomentaryButton(reverse=true)` to
  vezme ako držané stlačenie a repeater sa ~1 s po boote sám vypne. **Samostatný
  kandidát** — dopísať do `meshcore-nenahlasene-nalezy.md`.

## Čo sa medzitým udialo v upstream/dev — zohľadniť

Stav zistený **9. 9. 2026**. Naša vetva `test/lr2021-runtime-ab` obsahuje upstream po
`65aa1138` (1. 9. 2026, merge PR 3319), odvtedy v `upstream/dev` pribudli **štyri**
commity. Jeden z nich je pre nás dôležitý:

### LR2021 startReceive - upstream opravil zle makro (8. 9.)

`974f00de` (oltaco, PR 3379) v `src/helpers/radiolib/CustomLR2021.h`:

```
-  RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED)
+  RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED)
```

`RADIOLIB_LR2021_IRQ_PREAMBLE_DETECTED` je **maska** (`0x01UL << 5`, teda 32), nie číslo
bitu — `1UL << 32` je nedefinovaný posun. Správne je generické
`RADIOLIB_IRQ_PREAMBLE_DETECTED` (= 2).

**Naša kópia `CustomLR2021.h` má ten starý riadok** (r. 96). Z toho vyplývajú tri veci:

1. **Pre čistú vetvu to je nulová práca** — náš `CustomLR2021.h` do PR nejde, berieme
   upstream súbor a ten je už opravený. Len sa treba vetviť z **aktuálneho**
   `upstream/dev`, nie z merge-base.
2. **Na testovacej vetve to pretiahnuť do našej kópie.** Preamble bit sa
   nedostal do reportovaných flagov, takže vetva na preambulu v `isReceiving()` bola
   podľa všetkého mŕtva — a **všetky naše LR2021 čísla** (rxerr 4,34 % na xiao-nicerf,
   obe A/B/A série, šesť okien po 30 min) sú namerané s tým rozbitým posunom. Dotýka sa
   to aj toho, čo hlásime do RadioLib 1857 a do 2740.
3. **Nevydávať to za dokázanú príčinu, kým sa nezmeria.** Je to hypotéza s jasným
   mechanizmom, nie výsledok. Kto to bude merať, nech si prečíta
   `aba_single_switch_misleads` — jedno striedanie tu už raz klamalo.

Zvyšné tri commity nás netýkajú (companion CLI fix `5d82ed35` / PR 3366 a dva merge
commity). **Pin RadioLibu sa nezmenil** — v `platformio.ini` od 1. 9. nie je ani jeden
commit, stále beží `6d8934836` z 11. 7.

### Otvorené upstream PR, ktoré treba sledovať

| PR | kto | čo | prečo nás zaujíma |
|---|---|---|---|
| **2739** | c03rad0r | NiceRF LoRa2021 (plain, ESP32-C3), 7 súborov / +553, naposledy 4. 9. | náš PR má byť nadstavba, nie konkurencia; poslať po ňom |
| **3074** | omegaconjecture | Lierda AM36 Pico, ESP32-S3 + LR2021, 5 súborov, od 9. 8. bez pohybu | **precedens** — ako upstream posudzuje LR2021 variant; prečítať review skôr, než napíšeme svoj |
| **3331** | strasharo | `xiao_nrf52` repeater: preskočiť I2C probing RTC/senzorov pri boote | **týka sa nás** — náš variant ťahá `-I variants/xiao_nrf52` a náš `target.cpp` je z neho odvodený; ak to padne, zvážiť to isté u nás |
| **3191** | jpmartineau | `companion_radio_serial` env pre XIAO | len aby sme sa nezrazili v tom istom súbore |

## Postup

0. **Najprv znovu prejsť upstream** — `git fetch upstream && git log --oneline
   65aa1138..upstream/dev` a pozrieť `src/helpers/radiolib`, `variants`, `platformio.ini`.
   Sekcia vyššie je stav k 9. 9.; ak medzitým pribudlo niečo k LR2021, patrí to sem.
1. **Vetva z upstreamu, vo worktree** — nie prepnutím hlavného repa, na
   `test/lr2021-runtime-ab` beží meranie:
   ```
   git fetch upstream
   git worktree add ../../scratchpad/mc-nicerf-variant -b feat/nicerf-lora2021f33-xiao upstream/dev
   ```
2. **Cherry-pick nepoužívať** — na dirty vetve sú tie súbory premiešané s FOTA a FK.
   Skopírovať a ručne vyčistiť.
3. **Strip:** `fkclaude/tools/strip_lang_comments.py` na `//sk:` (target.cpp 9×) a `;sk:`
   v ini; potom ručne FK — v `target.cpp` 3× (`fk_pram_enable`, `FK_NICERF_LORA2021F33_TEST`
   blok s CLI `fk …`) a v `target.h` 1× (prototyp `nicerfTestCliCommand`).
4. **Kontrola pred pushom:**
   ```
   git diff upstream/dev..HEAD | grep -cE '\bFK|//sk:|;sk:'                 # = 0
   grep -rinE '\b(claude|chatgpt|co-authored|generated with)' <zmenené>     # = 0
   pio project config | grep -i nicerf                                      # jediný env
   ```
5. **Build:** `FIRMWARE_VERSION=v1.17.1 pio run -e Xiao_nrf52_nicerf_lora2021f33_repeater`.
6. **Flash na železo — najprv sa spýtať Fedora.** `xiao-nicerf` (COM21) je zapojený ako
   FOTA repeater a beží na ňom 106-hodinový čistý test (`spifix=0`), ktorý je zároveň
   argument pre RadioLib 1857 aj pre rozhodnutie o PR 3261. Preflashovaním sa to okno
   stratí.
7. **Von:** pushnúť na `origin`, link dať do issue 2740. Telo komentára do
   `fkclaude/docs/PRs/mc-2740-comment-nicerf-variant.md`, **vopred na schválenie**.
   PR do upstreamu až keď dopadne c03rad0rov 2739 — dva LR2021 varianty naraz v review
   je zbytočný šum.

## Otvorené otázky pre Fedora

* **Rovno PR, alebo najprv len vetva + link?** felixfelix chce „the whole variant", nie
  nutne merge; carlhodderovi stačí vetva. PR sa dá poslať aj o dva týždne.
* **SIMO:** nechať vypnuté ako dnes, alebo do PR pridať zapnuté? (~41 % nižší RX prúd,
  ~11 → ~6,5 mA, ale erratum vyžaduje re-apply po `SetLoraModulationParams` — pre balóny
  je to zásadné, pre nás nie.)
* **2,4 GHz blok** (r. 110–112, zakomentovaný) — nechať v PR ako komentovanú možnosť,
  alebo vyhodiť?
* **Kedy pretiahnuť `974f00de` (preamble makro) na testovaciu vetvu?** Súvisí s bodom
  o flashi: buď hneď, a tým sa zavrie porovnávacie okno, alebo až po dobehnutí merania —
  ale potom ďalšie hodiny logov ležia na známej chybe.

## Pasce

* `-I variants/xiao_nrf52_nicerf_lora2021f33` **musí** byť pred `-I variants/xiao_nrf52` —
  oba adresáre majú `target.h` a náš musí vyhrať.
* PA tabuľka má floor ~19 dBm; `LORA_TX_POWER=14` dáva ~22 dBm **na výstupe modulu** —
  nezamieňať s výstupom čipu, na tom sa v 2740 dá ľahko rozísť.
* `LR2021_IRQ_DIO=9` (modulový pin 18); indexy RF switchu začínajú od **DIO5**.
* Žiadna zmienka o AI nikde; do commit message ani `#číslo`, ani URL.
* `gh` je `D:\FkDev\GHcli\bin\gh.exe`, nie je na PATH.

## Hotovo keď

Vetva `feat/nicerf-lora2021f33-xiao` existuje na `origin`, prekladá sa, `grep` na FK aj
`//sk:` vracia 0, a v issue 2740 je (po Fedorovom schválení) komentár s odkazom.

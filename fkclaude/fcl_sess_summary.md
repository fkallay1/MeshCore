# Priebežné summary zo sessions

Čo sa v ktorej session robilo — jedna vec, jeden riadok (téma → záver), pod ňou
odkaz do dokumentácie. Nadpis sekcie = názov session. **Nové sekcie sa pridávajú
navrch**, aby bolo posledné navrchu.

Odkazy sú vo formáte `[súbor § sekcia](súbor#kotva)`, kde kotva je odvodená
z **názvu nadpisu**, nie z čísla riadku — vo VS Code sú klikacie a prežijú
posunutie dokumentu. Rozbijú sa len ak sa nadpis premenuje; vtedy to VS Code
sám podčiarkne ako neplatný odkaz.

---

## 48 Kontrola PRs

*(2026-09-03, vetva `test/lr2021-runtime-ab`)*

- **„Fail" na PR 2978** — nie padnutý build: neschválený run po presne 30 dňoch expiruje na `failure`, 0 jobov, nič sa nepreložilo
  - ↳ [docs/PRs/README.md § Červený krížik po 30 dňoch](docs/PRs/README.md#červený-krížik-po-30-dňoch-nie-je-padnutý-build)
- **Ako sa stav CI vôbec dá prečítať** — `gh pr checks` a `statusCheckRollup` na našich PR nezobrazia nič, len `gh api actions/runs`
  - ↳ [docs/PRs/README.md § gh pr checks nikdy nič nezobrazí](docs/PRs/README.md#gh-pr-checks-na-našich-pr-nikdy-nič-nezobrazí)
- **`gh issue view --json comments` vrátil staré dáta** — 3 komentáre namiesto 7, chýbala jgromesova kritika; komentáre čítať cez REST
  - ↳ [docs/PRs/README.md § gh issue view vie vrátiť staré dáta](docs/PRs/README.md#gh-issue-view---json-comments-vie-vrátiť-staré-dáta)
- **Zlý upstream v príkaze** — `--repo ripplebiz/MeshCore` vráti prázdno bez chyby; správne je `meshcore-dev/MeshCore`, najistejšie `gh search prs`
  - ↳ [docs/PRs/README.md § Kontrola stavu cez gh](docs/PRs/README.md#kontrola-stavu-cez-gh)
- **Prehľad frontov** — 3218 mergnuté, 3261 bez review, 2978 v chvoste, bootloader 47–50 odložené na ďalší release, RadioLib 1857 živé
  - ↳ [docs/PRs/README.md § Aktuálny obsah](docs/PRs/README.md#aktuálny-obsah)
- **carlhodder odpovedal v issue 2740** — druhý človek s LR2021, má štyri dosky a prvá je identická s našou
  - ↳ [fcl_readme_nicerf_lora2021.md § Jeho zostava](fcl_readme_nicerf_lora2021.md#jeho-zostava--tri-z-týchto-štyroch-bežia)
- **SIMO erratum** — vraj sa treba nastaviť znova po `SetLoraModulationParams` / `SetRxPath`; my ho zapíname raz v `radio_init()`, čiže tiché vypadnutie do LDO
  - ↳ [fcl_readme_nicerf_lora2021.md § SIMO sa stráca po zmene modulácie](fcl_readme_nicerf_lora2021.md#simo-sa-stráca-po-zmene-modulácie--neoverené)
- **Náš mechanizmus potvrdený nezávisle** — vidí payload celý `05` a sám navrhol „CMD_OK status byte repeated"; u nás praská dĺžka, u neho payload
  - ↳ [fcl_readme_nicerf_lora2021.md § Rovnaký príznak, iná cesta](fcl_readme_nicerf_lora2021.md#rovnaký-príznak-pokazených-paketov-iná-cesta)
- **Otvorená otázka na nás** — či `SPIreadStream` pri `CMD_OK` vracal `ERR_NONE`; odpoveď je áno a je to dôkaz, prečo sa chyba nikde nehlási
  - ↳ [fcl_readme_nicerf_lora2021.md § Otázka, ktorú nám položil](fcl_readme_nicerf_lora2021.md#otázka-ktorú-nám-položil--treba-odpovedať)
- **Tip na „rádio ohluchne"** — generické `RADIOLIB_IRQ_*` sú počty posunov, nie masky; modulové konštanty maskou sú
  - ↳ [fcl_readme_nicerf_lora2021.md § Jeho tipy](fcl_readme_nicerf_lora2021.md#jeho-tipy-na-rádio-ohluchne-po-vysielaní)
- **A/B/A prehodnotené** — `rlwait` pravdepodobne prepínal redundantné čakanie, takže `stale=0` v oboch fázach nič nedokazuje
  - ↳ [docs/lr2021-aba-rlwait-20260903.md § Doplnené 3. 9.](docs/lr2021-aba-rlwait-20260903.md#doplnené-3-9-rlwait-pravdepodobne-neprepínal-nič)
- **Kontrolór odkazov** — `fkclaude/tools/check_doc_links.py` overí súbor + kotvu v celom `fkclaude/`; našiel 10 rozbitých odkazov z čias renamu OTA→FOTA, opravené
  - ↳ bez projektovej dokumentácie — nástroj sám má hlavičku s použitím

**Otvorené na ďalej:** odpovedať carlhodderovi (otázka na `ERR_NONE`, náš `fk stale` recept) · zmerať odber pred/po prepnutí SF, či SIMO erratum platí · overiť, či `waitForGpio` je nastavené na ceste `getPacketLength()` · dva neposlané drafty pre 1857 čakajú na schválenie · reprodukcia na nezmenenej knižnici, ktorú jgromes žiada, stále nie je · na PR 2978 sa mesiac nič nedeje

## 46 Problem na NicRF2021 - ze RAW RX paekty sa komolili - zla dlzka avypisany obsah v Serial logging - problem casovania SPI - PR do meshocore, Issue do RadioLib

*(2026-08-19 → 2026-09-03, vetva `features/nrf-fota`)*

- **Príznak `len=4` v RAW logu** — nie chyba výpisu: druhé SPI čítanie prišlo priskoro, vrátilo default stream a ako dĺžka sa čítala horná polovica IRQ slova
  - ↳ [fcl_readme_nicerf_lora2021.md § Zastaralá SPI odpoveď](fcl_readme_nicerf_lora2021.md#zastaralá-spi-odpoveď--komolená-dĺžka-paketu)
- **Mechanizmus dokázaný injekciou** — preskočené čakanie na BUSY vyrobí chybu na požiadanie: 32/32 predčasných čítaní hlásilo `CMD_OK`, všetky poriadne `CMD_DAT`
  - ↳ [fcl_readme_nicerf_lora2021.md § Odmerané na železe](fcl_readme_nicerf_lora2021.md#odmerané-na-železe--fk-stale)
- **Guard v MeshCore pre oba čipy** — `CustomLR2021` aj `CustomLR1110` veria dĺžke len pri `CMD_DAT`; koreň je v RadioLibe, náš override je obchvat, nie oprava
  - ↳ [fcl_readme_nicerf_lora2021.md § Kde je koreň](fcl_readme_nicerf_lora2021.md#kde-je-koreň)
- **Issue do RadioLib a PR do MeshCore** — 1857 a 3261 odoslané; bez zmienky o AI a bez `#čísla` či URL v commitoch (kvôli krížovým referenciám do upstreamu)
  - ↳ [docs/PRs/rl-1857-issue-stale-spi-reply.md](docs/PRs/rl-1857-issue-stale-spi-reply.md) · [docs/PRs/mc-3261-pr-lr-stale-spi-reply.md](docs/PRs/mc-3261-pr-lr-stale-spi-reply.md)
- **Vyvrátené vlastné tvrdenie** — prvá verzia PR tvrdila, že nulová dĺžka na LR11x0 nenastáva; železo ukázalo 25 núl za 3 minúty, PR prepísaný a omyl priznaný komentárom
  - ↳ [docs/PRs/mc-3261-comment-radiolib-status.md](docs/PRs/mc-3261-comment-radiolib-status.md)
- **`rule=FP` je falošný poplach** — 400+ udalostí na T1000-E bola len zhoda nuly so sebou, ani jedna `rule=CMD`; to ukázalo, že chyba je na našej strane čítania
  - ↳ [fcl_readme_nicerf_lora2021.md § Ako čítať spifix](fcl_readme_nicerf_lora2021.md#ako-čítať-spifix--rulefp-je-falošný-poplach) · [docs/lr20xx-spi-read-protocol.md § Rámcovanie čítacieho príkazu](docs/lr20xx-spi-read-protocol.md#rámcovanie-čítacieho-príkazu)
- **`fk pretype on` bez efektu** — extra `getPacketType()` pred čítaním dĺžky nezmenil nič na žiadnej doske; hypotéza časovacieho zákmitu tým nepotvrdená
  - ↳ bez projektovej dokumentácie — vetva zanikla, čísla vysvetlila až oprava offsetu
- **Premenovanie modulu podľa datasheetu** — `NiceRF_LoRa2021F33` všade (header, flagy, variant, JSON, doky), s poznámkou NiceRF = G-NiceRF, aby to nebudilo dojem klonu
  - ↳ [fcl_readme_nicerf_lora2021.md § Datasheet modulu](fcl_readme_nicerf_lora2021.md#datasheet-modulu--v12-2026-08-stiahnuté-2026-08-20)
- **Konvencia flagov `FK_` / `FKPR_`** — testovacie vs. kandidáti na PR; pred každým PR musí `grep -c "FK"` na diffe vrátiť 0
  - ↳ [fcl_readme_nicerf_lora2021.md § Kam patria naše flagy](fcl_readme_nicerf_lora2021.md#kam-patria-naše-flagy)
- **Watchdog overený na železe** — detektor (`spread=0` v troch oknách po sebe pri zamrznutom `rawrx`) aj zotavenie (`fk ce off/on` + re-init) na LR2021
  - ↳ [fcl_readme_nicerf_lora2021.md § Watchdog rádia](fcl_readme_nicerf_lora2021.md#watchdog-rádia--flag-fkpr_radio_watchdog-nezávislý-od-typu-rádia)
- **Zaseknutý príjem na LR2021 reprodukovaný** — `irq=0x40111`, DIO trvalo HIGH, bez samozotavenia; dva moje pokusy o opravu zlyhali, dosku zachránilo UF2 cez bootloader
  - ↳ [docs/lr2021-post-tx-rearm-bug.md § Symptóm](docs/lr2021-post-tx-rearm-bug.md#symptóm)
- **T1000-E pribudol ako tretia doska** — prepísaný na repeater FOTA FW, apka COM22 / bootloader COM23; dva rôzne COM sú zámer, aby sa stav dal rozlíšiť podľa portu
  - ↳ bez projektovej dokumentácie — zariadenia sú v `test_nrf-fota/fota_devices.json`
- **`--rebuild` flashoval starý archív** — `resolve_zip()` bral najnovší zip z `builds/` namiesto `.pio/build/<env>/firmware.zip`; T1000-E dostal build o 106 starší, opravené
  - ↳ bez projektovej dokumentácie — oprava v `test_nrf-fota/fota_remote_e2e.py`
- **Handoff pre PR 2978** — rebase odovzdaný samostatnej session vrátane varovania neflashovať dosky, aby si nezhodila bežiace pasívne meranie
  - ↳ [docs/handoff-pr2978-rebase.md § Prečo naň nikdy nebežalo CI](docs/handoff-pr2978-rebase.md#prečo-naň-nikdy-nebežalo-ci)

**Otvorené na ďalej:** T1000-E beží od 21. 8. RX-only (`set repeat off`, oba advert intervaly 0, vidno ako `rawtx=1`) — pozostatok môjho merania, vrátiť dvoma príkazmi, ak to nedrží bežiace A/B · meranie „TX si sám ruší príjem" sa už netreba dokončiť, príčinu núl vysvetlila oprava offsetu

## 45 Merge upstream 1.17.1; Novy variant xiao_nrf42_nicerf_lora2021f33; Riesenie WatchDog radia; Nove cli prikazy fk xxx, pre testovanie, Repozitar na GB so zalohou globlaneho .claude

*(2026-08-15 → 2026-08-17, vetva `features/nrf-fota`)*

- **Vetvy** — `upstream/dev` → `dev` → `features/nrf-fota` zosynchronizované, upstream 1.17.1 zmergnutý
  - ↳ [.claude/skills/sync-upstream/SKILL.md](../.claude/skills/sync-upstream/SKILL.md)
- **Skill `/sync-upstream`** — postavený, lebo vyrovnanie vetiev prišlo tretíkrát
  - ↳ [.claude/skills/sync-upstream/SKILL.md](../.claude/skills/sync-upstream/SKILL.md)
- **Voľba základu pre NiceRF variant** — odvodené z `xiao_nrf52`, nie z `meshtracker_x1`
  - ↳ [fcl_readme_nicerf_lora2021.md § Prečo odvodené z xiao_nrf52](fcl_readme_nicerf_lora2021.md#prečo-odvodené-z-xiao_nrf52-a-nie-z-meshtracker_x1)
- **Nový variant `xiao_nrf52_nicerf_lora2021f33`** — DIO mapa, RF switch, TCXO 3,3 V, PA tabuľka; na železe vysiela aj prijíma
  - ↳ [fcl_readme_nicerf_lora2021.md § Zapojenie](fcl_readme_nicerf_lora2021.md#zapojenie)
- **Sémantika výkonu** — ciferník s `PA_OFFSET=8` podľa konvencie RAK 1W (14 → ~22 dBm)
  - ↳ [fcl_readme_nicerf_lora2021.md § Ciferník výkonu](fcl_readme_nicerf_lora2021.md#ciferník-výkonu-set-tx)
- **Upstream PR #3218** — chyba `setTxPower` na LR2021 odoslaná; PR bez zmienky o AI
  - ↳ [docs/PRs/mc-3218-pr-lr2021-txpower.md](docs/PRs/mc-3218-pr-lr2021-txpower.md) · [fcl_readme_nicerf_lora2021.md § Chyba v RadioLib](fcl_readme_nicerf_lora2021.md#chyba-v-radiolib-ktorú-header-obchádza)
- **PRAM patch pre LR2021** — 2240 B sa nahráva a verifikuje pri štarte
  - ↳ [fcl_readme_nicerf_lora2021.md § PRAM](fcl_readme_nicerf_lora2021.md#pram-firmvérový-patch-čipu--overené-na-hw-2026-08-15)
- **DC-DC / SIMO** — cievka na module je osadená, spotreba v RX klesla o ~41 %
  - ↳ [fcl_readme_nicerf_lora2021.md § DC-DC (SIMO)](fcl_readme_nicerf_lora2021.md#dc-dc-simo--prečo-ho-nemáme-a-nepotrebujeme)
- **CE pin** — funguje ako skutočný vypínač modulu, parazitné napájanie ho neudrží
  - ↳ [fcl_readme_nicerf_lora2021.md § Test CE](fcl_readme_nicerf_lora2021.md#test-ce--parazitné-napájanie-modul-neudrží)
- **Testovacie CLI `fk …`** — `info / simo / ce / pram`, aby sa nemuselo stále preflashovať
  - ↳ [fcl_readme_nicerf_lora2021.md § Testovacie CLI](fcl_readme_nicerf_lora2021.md#testovacie-cli-fk---flag-fk_nicerf_lora2021f33_test)
- **FOTA balíčky 1.16 → aktuál** — veľkosti sa zmestili (patch 31–37 kB, extraSafe pod 32 kB)
  - ↳ [fcl_readme_fota_extrasafe.md § Aktuálne riešenie](fcl_readme_fota_extrasafe.md#3-aktuálne-riešenie-build--265-commit-na-featuresnrf-fota)
- **Watchdog rádia** — tri pokusy o detekciu boli mylné, funguje až pasívne RSSI okno z noise-floor vzorkovača
  - ↳ [fcl_readme_nicerf_lora2021.md § Watchdog rádia](fcl_readme_nicerf_lora2021.md#watchdog-rádia--flag-fkpr_radio_watchdog-nezávislý-od-typu-rádia)
- **`isChipResponding()` na SX1262** — vadné, 0 z 6000; watchdog ho už nepoužíva
  - ↳ [fcl_readme_nicerf_lora2021.md § Watchdog rádia](fcl_readme_nicerf_lora2021.md#watchdog-rádia--flag-fkpr_radio_watchdog-nezávislý-od-typu-rádia)
- **Metrika `n`** — nie je konštanta 960, pri nekonvergujúcom floore vyskočí (`n=92950`)
  - ↳ [fcl_readme_nicerf_lora2021.md § Metrika n nie je konštanta](fcl_readme_nicerf_lora2021.md#metrika-n-nie-je-konštanta)
- **Nové CLI `fk win`** — prečíta pasívne okno na požiadanie; sériové CLI vyžaduje `\r`, nie `\n`
  - ↳ [fcl_readme_nicerf_lora2021.md § Diagnostika rádia](fcl_readme_nicerf_lora2021.md#diagnostika-rádia--flag-fk_radio_diag_cli-nezávislý-od-typu-rádia)
- **Filter `FK_DEBUG_MAXPATH=4`** — skrýval prevádzku (uzol chodí cez 9–13 hopov), odstránený
  - ↳ [fcl_readme_nrf-fota.md § FK_DEBUG_MAXPATH môže skryť prevádzku](fcl_readme_nrf-fota.md#fk_debug_maxpath-môže-skryť-prevádzku)
- **I2C displej** — pridaný výpis `display.begin()`; zlyhanie bolo hardvérové, nie firmvérové
  - ↳ [fcl_readme_nicerf_lora2021.md § Boot diagnostika](fcl_readme_nicerf_lora2021.md#boot-diagnostika-pri-zlyhaní-radio_init--flag-fk_debug)
- **Hypotéza parazitného napájania cez J-Link** — A/B na 8 power-cykloch nerozhodol, rozhodne až multimeter
  - ↳ [fcl_readme_nicerf_lora2021.md § Parazitné napájanie cez J-Link](fcl_readme_nicerf_lora2021.md#parazitné-napájanie-cez-j-link--neuzavreté)
- **Rozdelenie flagov nicerf variantu** — testovacie flagy presunuté zo zdieľaného bloku do FOTA envu
  - ↳ [fcl_readme_nicerf_lora2021.md § Kam patria naše flagy](fcl_readme_nicerf_lora2021.md#kam-patria-naše-flagy)
- **Zlý env pri flashi** — nahraný `_repeater` namiesto `_repeater_fota`, doska sa tvárila zaseknuto
  - ↳ [fcl_readme_nicerf_lora2021.md § Envy](fcl_readme_nicerf_lora2021.md#envy)
- **Mŕtve rádio na ProMicre** — buildy 391/437/444 zlyhali rovnako, takže nie regresia
  - ↳ [fcl_readme_nicerf_lora2021.md § Boot diagnostika](fcl_readme_nicerf_lora2021.md#boot-diagnostika-pri-zlyhaní-radio_init--flag-fk_debug)
- **Boot diagnostika** — pull test odhalil prerušený vodič MISO; po prepichnutí doska ide
  - ↳ [fcl_readme_nicerf_lora2021.md § Boot diagnostika](fcl_readme_nicerf_lora2021.md#boot-diagnostika-pri-zlyhaní-radio_init--flag-fk_debug)
- **`/auto-mode-setup`** — vysvetlené čo mení a prečo sa bije s `CLAUDE.md`; nič sa neaplikovalo
  - ↳ bez projektovej dokumentácie — vec CLI, nie repozitára
- **Priebežné summary zo sessions** — vznikol tento súbor + globálny skill, ktorý ho dopĺňa na konci každej session
  - ↳ bez projektovej dokumentácie — skill je v `~/.claude/skills/session-summary/`, formát popísaný v hlavičke tohto súboru
- **Odkazy cez kotvy z nadpisov** — nahradili čísla riadkov, prežijú posunutie dokumentu a VS Code neplatné sám podčiarkne
  - ↳ bez projektovej dokumentácie — pravidlo v globálnom `~/.claude/CLAUDE.md`
- **Formát nadpisov v `fcl_*`** — žiadne emoji, cieľ odkazu dostane vlastný nadpis; odstránené emoji zo 4 nadpisov
  - ↳ bez projektovej dokumentácie — pravidlo v globálnom `~/.claude/CLAUDE.md`, platí aj pre session z iného repa
- **Záloha `~/.claude` do súkromného repa `fk.claude`** — `git init` priamo v ňom, whitelist `.gitignore`, `SessionEnd` hook commituje len svoj projekt
  - ↳ bez projektovej dokumentácie — README priamo v repozitári `fk.claude`
- **Lokálne povolenia projektov neboli nikde zálohované** — sedia v gitignorovanom `.claude/settings.local.json` (MeshCore ich má 33), nie v globálnom `settings.json`
  - ↳ bez projektovej dokumentácie — `project-settings/` + `tools/fk_claude_restore.ps1` v repe `fk.claude`
- **Claude Code maže transkripty po 30 dňoch** — `cleanupPeriodDays`, bez akéhokoľvek upozornenia; vypnuté nastavením na 3650
  - ↳ bez projektovej dokumentácie — README v repe `fk.claude`, sekcia o mazaní transkriptov
- **Záchrana zmazaných sessions** — zo starej konfigurácie na `C:`, z 7z celozálohy a z dvoch `.claude_bkp`; MeshCore 6 → 43, meshcore-open 0 → 7
  - ↳ bez projektovej dokumentácie — README v repe `fk.claude`, tabuľka zdrojov
- **Undelete nemá zmysel** — `D:` je NVMe SSD s TRIM, po zmazaní vracia radič nuly; rozhoduje sa v okamihu mazania, nie neskoršími zápismi
  - ↳ bez projektovej dokumentácie — README v repe `fk.claude`
- **Pamäť je jedna na projekt, nie na session** — viaže sa na absolútnu cestu, nie na git repo; archívna pamäť sa preto nikdy nekopíruje späť
  - ↳ bez projektovej dokumentácie — README v repe `fk.claude`, sekcia o pamäti
- **Obnova zmazaných sessions z webu** — sessions pripojené cez Remote Control majú transkript na serveri; zo 6 lokálnych je 58
  - ↳ bez projektovej dokumentácie — `~/.claude/docs/session-recovery.md` + skill `/session-recovery`
- **`claude --teleport` na tieto sessions nefunguje** — pýta sa endpointu `teleport-events`, ktorý vracia prázdno; obsah je na `events` (odmerané)
  - ↳ bez projektovej dokumentácie — `~/.claude/docs/session-recovery.md`
- **Remote Control synchronizuje jednosmerne** — lokál → web; prázdny stub s `bridge-session` sa pripojí, ale históriu nestiahne
  - ↳ bez projektovej dokumentácie — `~/.claude/docs/session-recovery.md`
- **Nástroje na obnovu** — `fk_session_list.py` (čo chýba) a `fk_session_fetch.py` (stiahne a skonvertuje vrátane prepojenia)
  - ↳ bez projektovej dokumentácie — `~/.claude/tools/` v repe `fk.claude`
- **Párovanie len cez `cse_` ID** — moja chyba: zhoda názvu dávala falošné „máme"; jedna session mohla byť bridgnutá viackrát, každá epizóda má vlastný výrez
  - ↳ bez projektovej dokumentácie — `~/.claude/docs/session-recovery.md`

**Otvorené na ďalej:** watchdog ostáva v `FK_RADIO_DIAG_ONLY` (zasahovacia vetva
neoverená proti skutočnej poruche) · parazitné napájanie cez J-Link čaká na meranie
multimetrom · krížový test adverts medzi ProMicro a nicerf nebol dokončený ·
sessions, ktoré nikdy neboli na webe, sa obnoviť nedajú — ostávajú len zadania
z `history.jsonl` · transkripty v zálohe rastú (~8 MB za session), časom zvážiť
politiku orezávania · zvážiť `/bug` na teleport (číta z `teleport-events`, ktorý
pre Remote Control zrkadlá vracia prázdno).

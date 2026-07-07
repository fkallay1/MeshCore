# FOTA extraSafeSize — prečo patch degeneroval na ~celý FW a ako je to vyriešené

Súvis: [fcl_readme_nrf-fota.md](fcl_readme_nrf-fota.md) (užívateľský popis FOTA),
[fcl_readme_tech_nrf-fota.md](fcl_readme_tech_nrf-fota.md) (technický rozbor),
[fcl_HowTo_BuildApp&fotapkg.md](fcl_HowTo_BuildApp&fotapkg.md) (runbook generovania balíkov).

---

## 1. Symptóm (objavené 2026-07-06)

Fotapkg **sensecap 232→265** vyšiel **323 KB** (≈ komprimovaný celý FW), zatiaľ čo
rollback 265→232 len **8,7 KB**. Taký balík je nepoužiteľný — nezmestí sa do FOTA FS
(92 KB) ani do RAM patch regiónu (128 KB) a cez LoRa by sa posielal večnosť.

Poznámka k čítaniu výstupu generátora: **raw** hdiffi patch je VŽDY ≈ veľkosť nového FW
(formát nesie subtrakčný stream cez celé pokrytie — pri dobrej zhode samé nuly).
Kvalitu diffu ukazuje až **komprimovaná** veľkosť (dobrý patch ~1–2 %, degenerovaný ~60 %+).

## 2. Koreňová príčina

Flasher patchuje **in-place** (HPatchLite inplaceB): nový obraz zapisuje priamo cez
starý, zdola nahor. Keď FW medzi buildmi **narastie**, obsah sa posunie dopredu —
`new[p] = old[p − posun]` — a patcher by musel čítať staré dáta, ktoré už prepísal.
Formát to rieši ring bufferom veľkosti **extraSafeSize** (zápis sa oneskorí o toľko
bajtov). hdiffi dostane strop cez `-inplace-N`; keď posun (≈ rast FW) presiahne N,
hdiffi zhody nemôže použiť a emituje celý obraz ako literály → 323 KB.

- 232→265: rast **+5392 B** > strop 4096 → degenerát (323 KB).
- 265→232: FW sa **zmenšuje** → referencie ukazujú dopredu na ešte neprepísané dáta
  → maličký patch. Preto tá asymetria upgrade/rollback — nie je to chyba dát.
- Empiricky sedelo na všetky páry: 238→265 (+4896 B) = 55 KB, hopy s rastom < 4096 B = jednotky KB.
- S `-inplace-8192` vyšiel ten istý pár 232→265 na **12,4 KB** — potvrdené experimentom.

## 3. Aktuálne riešenie (build > 265, commit na features/nrf-fota)

**Limit zdvihnutý 4096 → 32768 B** a zavedený ako jediná zdieľaná konštanta
`FOTA_MAX_EXTRA_SAFE` v `nrffota/flash_layout.h`. Zmeny:

| Vrstva | Zmena |
|--------|-------|
| `flasher/flasher.c` | `MAX_EXTRA_SAFE = FOTA_MAX_EXTRA_SAFE` (32 KB); temp_cache 20→48 KB v **.bss flashera** — žije v RAM len POČAS flashovania (región 128 KB @ 0x20020000, celá RAM je po `sd_softdevice_disable` voľná) → **bežiacu appku nestojí ani bajt RAM ani flash** (kód flashera nenarástol: zmenili sa 2 immediate hodnoty, binárka ostala 4096/4096 B) |
| `flasher_code.h` | pregenerovaný (`tools/build_flasher.py`) |
| `FotaPatcher.cpp` — **verify** | `ota verify` teraz **zlyhá s jasným dôvodom** `extraSafe X > max Y, flash zlyha`, ak patch žiada viac než limit. Predtým verify prešiel (SHA dry-run extra_safe nepotrebuje — číta nedotknutý starý FW z flashe) a problém vybuchol až vo flasheri ako 0xE5 + reset |
| `FotaPatcher.cpp` — **apply** | rovnaká predkontrola v `fota_flash_via_flasher()` v OBOCH vetvách (ZLIB aj nekomprimovaná) — apply sa zastaví ešte PRED `FotaFS.end()`/`sd_disable`/skokom, čiže bez rebootu a so zrozumiteľným logom |
| `fota_sender.py` (gen) | dvojkandidátna stratégia + výpis `extraSafeSize` (viď nižšie) |

**Dvojkandidátna stratégia generátora** (`make_patch`): hdiffi si totiž pokojne zvolí
extra_safe > 4096 aj pri malej zmene, keď mu to strop dovolí (zmerané: rast +144 B →
extra_safe 12,6 KB) — a taký patch by odmietli staré flashery. Preto generátor spraví
oba varianty a vyberá:

1. `-inplace-4096` — kompatibilný so VŠETKÝMI flashermi → **vyhráva, ak komprimovaný ≤ 32 KB** (norma sú jednotky KB),
2. `-inplace-32768` — záchrana pre veľké rasty; použije sa len keď kandidát 1 degeneroval,
   s výpisom `[POZOR] ... staré flashery tento patch odmietnu (0xE5)`.

Zmerané po zmene: 262→265 = 575 B (extra_safe 0, kompatibilné všade);
232→265 = **13,3 KB** (extra_safe 13512, len nové flashery) — z pôvodných 323 KB.

## 4. Prechodové obdobie — na čo si dať pozor

Flasher **inštaluje bežiaca appka pri každom apply** (`ensure_flasher_written()` —
porovná 0xEB000 s embedovaným blobom). Verzia flashera teda sleduje bežiaci FW:

- Zariadenie na builde ≤ 265 má **starý flasher (4 KB limit)** → prvý hop naň musí mať
  extra_safe ≤ 4096 (generátor to preferuje automaticky; pri degenerovanom páre treba
  reťaz hopov cez `builds/` archív, kde každý hop rastie ≤ ~4 KB — viď HowTo §2).
- Po JEDNOM úspešnom update na build s novým flasherom platí 32 KB limit
  a ďalšie hopy znesú rast do ~32 KB.
- Staré `ota verify` (build ≤ 265) predkontrolu nemá — na starom zariadení verify
  povie OK aj patchu, ktorý flasher odmietne 0xE5 (bezpečné — odmietne PRED zápisom,
  len to stojí reboot).

## 5. Potenciálne budúce riešenie — smerový flag (patch zdola-hore / zhora-dole)

Idea (zatiaľ NEimplementované): balík by niesol informáciu, ktorým smerom flashovať.

- Zápis **od konca nadol** obracia geometriu: rast FW prestane byť problém úplne
  (referencie na staré dáta ukazujú pod zápisový smer), problémom sa stane zmenšenie —
  presne zrkadlovo k dnešku. Upgrade (rastie) → reverse, rollback (zmenšuje sa) → forward;
  generátor by vyrobil oba a vybral menší → **oba smery vždy malé**, bez stropu.
- HPatchLite reverse nepodporuje → trik: pri generovaní bajtovo otočiť oba obrazy
  a diffnúť štandardne; flasher by čítal starý FW odzadu, skladal stránky odzadu
  a mazal/písal od najvyššej stránky nadol. hpatchi jadro sa nemení — smer žije len
  v listener callbackoch + flush + FNV verifikácii.
- Flag ide prirodzene do 12 B ZLIB wrapper hlavičky (iný magic, napr. `RLIB`) —
  starý flasher neznámy magic čisto odmietne pred prvým zápisom (fail-safe zadarmo).
- `ota verify`: SHA sa počíta nad výstupným streamom — pri reverse tečie odzadu,
  balík by musel niesť hash zrkadleného streamu (najčistejšie redefinovať `new_sha256`
  ako „hash výstupu patchera").
- **Blocker: flasher binárka je 4096/4096 B plná** (tvrdý strop — na 0xEC000 nadväzuje
  trace stránka). Druhá cesta zápisu = najprv treba kód zoštíhliť pod ~3,7 KB
  (kandidáti: debug/ftrace vetvy), alebo presunúť trace stránku (chirurgia flash mapy).
- So zdvihnutým limitom 32 KB je tlak na toto riešenie malý — má zmysel, až keby
  pravidelne vznikali rasty > 32 KB medzi susednými nasadzovanými buildmi.

## 5b. Potenciálne budúce riešenie — resume po páde + fail-safe boot (nápad 2026-07-07)

Pozorovanie: „posunutý zápis" (dekomprimuj dopredu do RAM, píš so sklzom, staré dáta
ostávajú vo flashi) **už existuje** — je to presne extraSafe ring buffer v
`hpatchi_inplaceB` (zápis oneskorený o extraSafeSize; pri malom extra_safe si knižnica
sklz sama zväčší na polovicu voľnej cache, hpatch_lite.c:361). Samotný posun teda
nezmenšuje patch; pridaná hodnota nápadu je **obnova po páde**.

**Dnešné fail scenáre flashera:**
| Kedy zlyhá | Čo sa stane |
|------------|-------------|
| pred prvým zápisom (hlavička/extra_safe/base SHA) | reset, stará appka nedotknutá ✓ |
| po dopísaní, FNV readback nesedí | `GPREGRET=0x57` → DFU bootloader, USB obnova ✓ |
| **uprostred zápisu** (power loss / fault) | **DIERA**: stránka 0 (vektory nového FW) je zapísaná ako prvá → bootloader skočí do roztrhaného obrazu → crash-loop; von len double-tap reset + USB |

**Návrh (neimplementované): stránka 0 naposledy + resume stub**
1. Pred patchovaním zapísať na app_base minimálny **stub** (reset vector → flasher@0xEB000);
   skutočnú stránku 0 držať v RAM a zapísať až po úspešnej FNV verifikácii.
2. Reset uprostred → bootloader → stub → flasher: CRC kontrolného bloku v RAM
   (nRF52 RAM **prežije soft reset** — WDT/fault/SystemReset; komprimovaný patch
   @0x20000000 tiež) → platný = **pokračuj od poslednej potvrdenej stránky**
   (rozpísanú re-erase + dopíš z ringu); neplatný (power loss, RAM preč) =
   `GPREGRET=0x57` → DFU — žiadny crash-loop, definovaný stav.
3. Power loss počas zápisu samotného stubu → nevalidná stránka 0 → bootloader ostane
   v DFU. Fail-safe v každom bode.
**Doplnok — staging patchu do voľnej app flashe (nápad 2026-07-07 #2):** appka namiesto
`memmove` do RAM zapíše staged patch cez NVMC **nad koniec FW** (app región v7 = 708 KB,
FW ~475 KB → ~228 KB voľných; staged patch 13–92 KB sa zmestí; ak nie → RAM fallback =
dnešná cesta). Flash je memory-mapped a `flasher_entry` už berie `patch_addr` ako
parameter → **flasher sa pre samotný staging takmer nemení**. Patch tým prežije výpadok
napájania — čo je chýbajúci diel pre plný resume. POZOR: samotný staging resume nedáva —
ring (nezapísaný výstup `[w, p)`) bol v RAM a jeho re-produkcia replayom potrebuje čítať
starý pás `[w−extraSafe, w)`, ktorý je už prepísaný — staré dáta `≥ w` vo flashi problém
nie sú, chýba presne pás pod write-pointerom. (Pri SOFT resete ring + stav streamu v RAM
prežijú → úroveň B „len pokračuj" funguje bez logu.) Ring sa pritom NESMIE preventívne
flushovať — budúca produkcia legitímne číta až extraSafe pod aktuálny bod, sklz je
korektnostná podmienka formátu, nie optimalizácia. Replay po výpadku potrebuje navyše:
- **progress bitmap** (1 bit na potvrdenú stránku; NVMC bit-clear bez erase = lacné),
- **rotujúci log starého okna** — pred prepísaním stránok odzálohovať ich STARÝ obsah
  do tej istej voľnej oblasti. Replay potom: diff stream z flashe, staré dáta ≥ w
  z flashe, zničené okno z logu, hlbšie garbage (výstup sa zahadzuje).
  Cena: každá stránka zapísaná 2× (~+20 s).
  **Hĺbka logu = `extra_safe` z hlavičky patchu, NIE fixných 32 KB** (formát garantuje
  čítania ≥ q − extra_safe): pri es=0 (väčšina bežných patchov) log NETREBA vôbec
  (resume = čistý replay, wear navyše 0); plný log traffic len pri veľkých rastoch.
  **Wear čísla**: traffic ≈ starý FW (~475 KB ≈ 116 stránok) na OTA (keď es>0);
  fixný 40 KB log = ~12 cyklov/OTA → ~800 OTA do 10k spec; log rozprestretý cez
  ~160 KB voľnej plochy + rotácia štart offsetu per OTA (build#) = ~3 cykly/OTA
  → tisíce OTA. Porovnanie: app stránky = 1 cyklus/OTA tak či tak.
  **Formát**: log stránka = 1 stará stránka (4096 B); index stránka = pole slov
  (slot→app stránka), zapisované postupne bez erase (1 slovo = 1 zápis, erase 1×/OTA);
  progress rovnako 1 slovo = 1 potvrdená stránka (obíde limit opakovaných zápisov
  do slova). Pri resume: index+progress → RAM tabuľka.

**Úrovne ambície (každá stavia na predošlej):**
| Úroveň | Čo pridá | Efekt pri výpadku |
|--------|----------|-------------------|
| A | flash staging + stub stránka 0 + bitmap | žiadny crash-loop: boot → stub → flasher → nedokončené → DFU (definovaný stav) |
| B | + kontrolný blok v RAM s CRC | resume po soft resete (WDT/fault) — pokračuje kde skončil |
| C | + old-window log | plný resume aj po výpadku napájania — dopatchuje sa sám |

- Odhad: +1,5–3 KB kódu flashera (úroveň C horný okraj) → potrebuje 8 KB región.
  Trace stránku pri tom radšej PRESUNÚŤ než zrušiť (pri fail scenároch je najcennejšia)
  — napr. flasher 0xEB000–0xED000 (8 KB) a trace ukrojiť z FOTA FS (92→88 KB).
- Najťažšia časť: testovanie power-fail scenárov (rezanie napájania v definovaných
  fázach zápisu), nie kód.

## 6. Diagnostický ťahák

| Prejav | Význam |
|--------|--------|
| upgrade balík ~stovky KB, rollback maličký | rast FW > extra_safe strop generátora (dnes by už nemalo nastať — eskaluje na 32 KB) |
| `[POZOR] extraSafeSize > 4096` pri gen | balík pôjde len na zariadenia s novým flasherom (build > 265) |
| `ota verify` → `extraSafe X > max Y, flash zlyha` | patch žiada viac než limit flashera TOHTO FW — apply by skončil 0xE5; pregeneruj balík alebo chodť reťazou |
| flasher trace 0xE5 (`FM_ERR_SAFE`) | starý flasher odmietol patch s extra_safe nad svoj limit — bez zápisu, appka nedotknutá |

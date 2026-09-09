# NiceRF LoRa2021F33 variant na čistej vetve — 9. 9. 2026

Zadanie: `fkclaude/docs/handoff/nicerf-variant-cista-vetva.md`. Cieľ bol vypublikovať
variant XIAO nRF52840 + NiceRF LoRa2021F33-2G4 na vetve z aktuálneho `upstream/dev`,
tak aby si ju vedeli vziať felixfelix-bot a carlhodder z MeshCore issue 2740.

## Výsledok

Dve vetvy na `origin`:

| vetva | čo je v nej |
|---|---|
| **`feat/nicerf-lora2021f33-xiao`** | 5 nových súborov, 652 riadkov, **žiadny upstream súbor nezmenený**. Toto ide von. |
| **`test/nicerf-lora2021f33-hw`** | tenká vrstva nad ňou: `fk …` CLI, watchdog, `FK_SERIAL_WAIT_DTR`, env `…_repeater_diag` |

Základňa je `017fd096` (`upstream/dev` k 9. 9.), teda **vrátane** `974f00de` — opravy
zlého makra `PREAMBLE_DETECTED`. Tá istá oprava je pretiahnutá aj do našej kópie
`CustomLR2021.h` na `test/lr2021-runtime-ab` (commit `a067e5f2`).

Envy v publikovanej vetve:

* `Xiao_nrf52_nicerf_lora2021f33_repeater` — základný
* `Xiao_nrf52_nicerf_lora2021f33_repeater_simo` — to isté so zapnutým DC-DC

PRAM (`LR2021_PRAM_UPD=1`) je zapnutá v základnej sekcii, teda platí pre oba envy.
Fedorovo rozhodnutie: RadioLib PRAM nenahráva a nevieme, kedy sa to tam dostane, takže
to musí fungovať proti tomu, čo MeshCore pinuje. `RADIOLIB_GODMODE=1` je v globálnych
`build_flags` upstreamu, čiže netreba nič navyše. Aby to bolo odolné dopredu,
`nicerf_lora2021f33_pram_load()` najprv prečíta magic na `0x800FF8` a keď je patch už
aktívny, nerobí nič — budúca RadioLib, ktorá si PRAM nahrá sama, nedostane druhú kópiu.

### Overené na železe

Doska `xiao-nicerf` (COM21), čistý env postavený proti **pinnutému** RadioLibu
`6d8934836`, flashnutý cez DFU:

```
[LR2021] pram load: rc=0 -> loaded=YES version=0x0313
[LR2021] NiceRF LoRa2021F33-2G4  fw=1.24  vbat=3310mV  temp=19.1C  errors=0x0000
[LR2021] pram: loaded=YES version=0x0313
[LR2021] irq=DIO9  tcxo=3.3V  freq=869.618MHz  band=LF(sub-GHz)  rfsw=DIO5/DIO6 (2G4 off)
[LR2021] pa=nicerf(duty7/slices6) offset=+8  tx=14dBm req -> paVal=14 (7.0dBm chip) -> ~22 dBm module out
```

Po 503 s: `recv` 30, `sent` 9 (8 flood + 1 direct), `recv_errors` 1, `nf` −120,
`last_rssi` −28, `last_snr` 13,25. Repeater teda prijíma aj preposiela.

## Prečo čistý build najprv vôbec nenabootoval

Prvý flash čistého envu skončil na `ERROR: radio init failed: -2`
(`RADIOLIB_ERR_CHIP_NOT_FOUND`) a upstream `main.cpp` v tom prípade robí `halt()`.
Zopakovalo sa to štyrikrát za sebou, aj proti záplatovanému RadioLibu.

### Náš fork to celý čas maskoval

Príčina nie je v knižnici ani vo variante. Náš `examples/simple_repeater/main.cpp`
má FK smyčku, ktorú upstream nemá: pri zlyhaní inicializácie **odoberie modulu
napájanie cez CE a skúsi znova, až trikrát**. V archívnom logu
`test_nrf-fota/logs/archive/xiao-nicerf_20260903-092925.log` je to desaťkrát, vždy
v tejto podobe:

```
ERROR: radio init failed: -2
[FK] radio init failed - power-cycling module, attempt 1/3
[FK] module power-down: CE low, driven lines low, 300 ms
[FK] module power-up: CE high
[LR2021] pram load: rc=0 -> loaded=YES version=0x0313
[FK] radio init after power-cycle: OK
```

Čiže **doska sa mesiace rozbiehala len vďaka tomu power-cyklu** a nikto si to
nevšimol, lebo výsledok bol vždy „OK". Publikovaná vetva by na upstreame `halt()`la.

### Fix patrí do variantu

CE je enable LDO modulu (pin 5) a má vnútorný pull-up, takže modul zostáva napájaný
aj cez reset MCU — a LR2021 vie zostať v stave, z ktorého ho reset vnútri
`findChip()` nevytiahne. Vylieči to len odobranie napájania.

Preto je v `NiceRF_LoRa2021F33.h` funkcia `nicerf_lora2021f33_power_cycle()` a
`radio_init()` ju volá **pred prvým** `std_init()`, plus dva opakované pokusy, keby
to nepomohlo. Všetko vo variante, žiadny upstream súbor sa nedotkol. Sekvencia je
prevzatá z toho, čo v našom `main.cpp` funguje: najprv ide dole každá linka, ktorú
budíme, výstupy modulu sa nechajú ako vstupy — inak modul prinapája cez ESD diódy
svojich IO pinov a napájanie v skutočnosti nezmizne (varuje pred tým NiceRF).

### Čo nie je dokázané

Pri jednej epizóde **nepomohli ani tri power-cykly za sebou** — modul sa vrátil až
neskôr sám. Takže „power-cycle vždy vylieči −2" tvrdiť nemožno; platí len, že bez
neho variant na upstreame nenabootuje vôbec. Sedí to s
`lr2021_busy_unobservable_in_episode` aj s `aba_single_switch_misleads`: porucha je
epizodická a jedno striedanie klame.

## Čo sa pri tom zistilo o našich vlastných meraniach

Testovacia vetva **nestavia proti pinnutému RadioLibu**. Od 21. 8. 2026
(`24c86be5`) je v `platformio.ini` symlink na lokálny checkout
`scratchpad/radiolib-ab` = jgromesov master + jeho BUSY patch (`440610d71`) +
dva naše FK patche (odmietnutie odpovede bez `CMD_DAT`, opakovanie len samotnej
odpovede). Všetky LR2021 čísla od 21. 8. sú teda namerané proti tomuto, nie proti
tomu, čo beží MeshCore.

Naviac naše `RadioLibWrappers` majú aj **nepodmienené** zmeny, nielen za `#ifdef`.
Najdôležitejšia je opakované nahodenie príjmu, keď `startReceive()` na LR2021
zlyhá — a tá v publikovanej vetve **nie je**. Je to upstream chyba, nie vec
variantu, a patrí do samostatného PR (vetva `fix/lr2021-set-tx-power-rx-stall`).
Preto čistý env postavený z `test/nicerf-lora2021f33-hw` nie je ten istý binár ako
z `feat/…` — líšia sa o 744 B. **Publikovaný firmvér overuj vždy z `feat/`.**

## Finálne čísla 133-hodinového okna

Zapísané pred flashom, lebo sa tým okno zavrelo. Build #317, FOTA env, PRAM
vypnutá, proti `radiolib-ab`:

| | |
|---|---|
| uptime | 479 350 s (133 h 9 min) |
| `recv` / `sent` | 65 069 / 15 890 |
| `recv_errors` | 2 900 (**4,27 %**) |
| airtime TX / RX | 4 636 s / 18 802 s |
| `nf` / `last_rssi` | −120 / −28 dBm |
| `spifix` | 0 |
| `fk win` | n=704 min=−125 max=−120 spread=5 → alive |

Pripomienka: aj tieto čísla sú namerané s rozbitým posunom `PREAMBLE_DETECTED`,
čiže s mŕtvou vetvou na preambulu v `isReceiving()`.

## Rozhodnutia, ktoré padli

* **SIMO** dostalo vlastný env namiesto flagu v jednom. Zapnuté to nie je defaultne
  z dvoch dôvodov: čip sa resetuje do LDO a `SetRegMode` sa musí zopakovať po
  `SetLoraModulationParams` (erratum), takže behová zmena SF (`tempradio`, `set sf`)
  ho zhodí späť na LDO až do ďalšieho initu. To v kóde ošetrené nie je — dopísané ako
  známe obmedzenie do komentára env.
* **2,4 GHz** ostáva zakomentovaný s celým popisom. Pre felixfelixa je to popis
  zapojenia pinu 10 a nič nestojí.
* **`getTcxoUsed()`** vypadol. Nie je v upstream `CustomLR2021.h` a do prvého PR sa
  kvôli jednému `Serial.print` neťahá zmena jadra.
* **FK CLI a watchdog** sú na `test/nicerf-lora2021f33-hw`, nie v publikovanej vetve —
  žijú v `RadioLibWrappers`, `CustomLR2021.h` a `MyMesh.cpp`, čiže v upstream
  súboroch, a `grep` na FK by nulu nevrátil.

## Pasce, ktoré stáli čas

### Worktree v session scratchpade prekročí MAX_PATH

Prvý worktree bol v `cli_home/temp/claude/<projekt>/<session>/scratchpad/…` a build
padal na `fatal error: ../../protocols/PhysicalLayer/PhysicalLayer.h: No such file or
directory` — pritom ten súbor tam bol. Nenormalizovaná cesta k nemu má **presne 260
znakov**, čo je windowsový MAX_PATH. Worktree pre PlatformIO patrí na krátku cestu;
tento je `D:\FkDev\wt\nicerf`.

### Heredoc cez Bash tool zožerie jednu úroveň backslashov

`python - <<'PY'` so `\\r\\n` v zdroji nedá `\r\n`, ale skutočný CR+LF — a v C
literáli je z toho `missing terminating " character`. Escape do generovaného kódu
skladaj z `chr(92)`, alebo to doplň ručne.

## Čo ešte nie je hotové

1. **Komentár do issue 2740** — telo patrí do
   `fkclaude/docs/PRs/mc-2740-comment-nicerf-variant.md` a **vopred na schválenie**.
   Von až potom.
2. **PR do upstreamu** až keď dopadne c03rad0rov 2739 (plain modul na ESP32-C3).
   Dva LR2021 varianty naraz v review je zbytočný šum.
3. **Chýbajúci re-arm fix** — bez `fix/lr2021-set-tx-power-rx-stall` môže publikovaný
   variant po neúspešnom `startReceive()` ticho prestať prijímať. Rozhodnúť, či to
   spomenieme v 2740 ako známu závislosť.
4. **Dlhý beh čistého envu** — teraz beží 8 minút. Kým z toho nie sú hodiny, „overené"
   znamená len „nabootuje, prijíma a preposiela".

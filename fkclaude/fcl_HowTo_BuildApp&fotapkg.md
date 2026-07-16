# HowTo — build FOTA firmware + vygeneruj fotapkg + pushni do telefónu

Runbook pre opakovanú úlohu „urob build pre <board>, vyrob otapkg <A>→<aktuálny>,
pushni do telefónu". Cieľ: nič nehľadať — všetky cesty, názvy a gotchas na jednom mieste.

Súvis: [fcl_readme_nrf-fota.md](fcl_readme_nrf-fota.md) (užívateľský popis FOTA),
[fcl_readme_tech_nrf-fota.md](fcl_readme_tech_nrf-fota.md) (technický rozbor).

---

## 0. Kľúčové cesty a fakty (toto sa vždy hľadalo)

| Vec | Kde |
|-----|-----|
| `pio` | `D:\FkDev\.platformio\penv\Scripts\pio` |
| penv python (pre skripty, má pyserial + deps) | `D:\FkDev\.platformio\penv\Scripts\python.exe` |
| **Podpisový kľúč Ed25519** | `test_nrf-fota/test_key.der` — **ak existuje → otapkg SIGNED**, inak UNSIGNED. `gen_fotapkg.py` ho berie ako default `--privkey`. (Je gitignored → na čerstvom klone chýba.) |
| adb (pre push do telefónu) | `D:\FkDev\00_Downloads\scrcpy-win64-v4.0\adb.exe` (fallback `adb` z PATH) |
| Archív buildov (app image .bin + .uf2) | `test_nrf-fota/builds/` — názov `<device>.fw_<N>.bin` (gitignored) |
| Vygenerované balíky | `test_nrf-fota/fotapkg_json/` — `<old>-<new>.<device>.fotapkg.json` (+ `.rev.` = rollback) |
| Globálny build counter | `test_nrf-fota/build_number.txt` (bumpuje ho `pre:gen_build_info.py` pri každom builde). **V repe je neutrálna hodnota 300** (obe FOTA vetvy); lokálne reálne číslo drží `git update-index --skip-worktree test_nrf-fota/build_number.txt` → git ho nehlási ani necommituje. Po novom klone flag nastaviť znova; ak checkout hlási „would be overwritten", dočasne `--no-skip-worktree`. |
| FOTA env názvy | `ProMicro_repeater_fota`, `SenseCap_Solar_repeater_fota`, `Xiao_nrf52_repeater_fota`, `t1000e_repeater_fota`, `RAK_3401_repeater_fota` (= „RAK 1W"), `RAK_4631_repeater_fota` |

**`device` v názvoch** = `PIOENV.split("_")[0].lower()` → `ProMicro_repeater_fota` → `promicro`,
`SenseCap_Solar_repeater_fota` → `sensecap`, `Xiao_nrf52_repeater_fota` → `xiao`, `t1000e_repeater_fota` → `t1000e`.
**Výnimka:** ak by prvý segment kolidoval, env nastaví `custom_fota_device` v `platformio.ini`
(hook `gen_fotapkg_hook.py` ho preferuje) — `RAK_3401_repeater_fota` → `rak3401`, `RAK_4631_repeater_fota` → `rak4631`.

---

## 1. Build (a čo sa stane automaticky)

```bash
"D:/FkDev/.platformio/penv/Scripts/pio" run -e SenseCap_Solar_repeater_fota
# alebo -e ProMicro_repeater_fota
```

Reťaz `extra_scripts` v `variants/*/platformio.ini` urobí pri KAŽDOM builde:
1. `pre:gen_build_info.py` — bump `build_number.txt` (207→208…) + `build_info.h`.
2. `post:gen_fw_trailer.py` — zapíše FwId trailer (magic `FKFWID01`, build#, sha256) do `firmware.hex`.
3. `post:gen_fotapkg_hook.py` — **archivuje** `builds/<device>.fw_<N>.bin(+.uf2)` a **auto-vygeneruje
   otapkg medzi POSLEDNÝMI DVOMA buildmi toho istého device** → `fotapkg_json/<old>-<N>.<device>.fotapkg.json`
   + `.rev.` (rollback). Nefatálne (zlyhanie nezhodí build).

> **Dôsledok:** ak chceš „`<A>` → aktuálny" a `<A>` je posledný archivovaný build daného device,
> stačí zbuildovať a hook to vyrobí sám. (Napr. archív mal `sensecap.fw_205`, build dá 208 →
> hook vyrobí `205-208.sensecap.fotapkg.json`.)

> **Overovací build bez fotapkg:** `FOTAPKG_SKIP=1 pio run -e …` — hook krok 3 sa preskočí
> (archív aj pkg), build# sa ale bumpne vždy (krok 1). Legacy meno `OTAPKG_SKIP` tiež funguje.

Pozn.: FOTA envy majú `-D FOTA_DEBUG=1` — diagnostické `[FOTA]`/`[FLASHER-DBG]` výpisy na
Serial (makrá v `nrffota/FotaDebug.h`). Vypnutie = zakomentovať flag v ini; CLI odpovede
fungujú aj bez neho. Komentáre v FOTA zdrojákoch sú dvojjazyčné `//en:`+`//sk:`
(strip: `python fkclaude/tools/strip_lang_comments.py --keep en|sk <cesty>`).

---

## 2. Ručné generovanie otapkg (ľubovoľný pár buildov)

Keď chceš iný pár než „posledné dva" (napr. base build NIE je posledný v archíve):

```bash
"D:/FkDev/.platformio/penv/Scripts/python.exe" test_nrf-fota/gen_fotapkg.py \
  --old test_nrf-fota/builds/sensecap.fw_205.bin \
  --new test_nrf-fota/builds/sensecap.fw_208.bin \
  --device sensecap
```

Ďalšie režimy:
- `--auto --device sensecap` → 2 najnovšie buildy z archívu.
- `--from-hex <firmware.hex> --device <d>` → extrahuje+archivuje z HEX, potom gen posledné dva.
- `--privkey ''` → vynúť UNSIGNED; inak sa použije `test_key.der` ak existuje.
- `--privkey-hex <128 hex>` → podpíš **companion identity kľúčom** (dlhý hex z appky).
- `--keyid 0` (default) = v0-prefix formát; `--keyid 1` = legacy pre **staré FW** (< build 335).

**Podpisové kľúče — `fota_keytool.py`** (viac v `fcl_readme_nrf-fota.md` §4b):
```
python test_nrf-fota\fota_keytool.py gen test_nrf-fota\fota_signkey1.der   # nový kľúč + C snippet do s_authors
python test_nrf-fota\fota_keytool.py der2hex test_nrf-fota\test_key.der    # .der -> companion 128-hex + pub/prefix
python test_nrf-fota\fota_keytool.py pub  <kľúč.der | 128hex>              # pub / prefix / C snippet
```
`.der → hex` áno; `hex → .der` NIE (companion hex = SHA512(seed), jednosmerné).
Kľúče `fota_signkey1..4.der` sú gitignored.

Výstup: `fotapkg_json/<old>-<new>.<device>.fotapkg.json` (upgrade) + `.rev.` (rollback).
Používaj **penv python** (`gen_fotapkg.py` importuje `fota_export_pkg`/`fota_sender` — potrebuje ich deps).

> **GOTCHA — rast FW medzi buildmi vs extraSafeSize** (plný rozbor:
> [fcl_readme_fota_extrasafe.md](fcl_readme_fota_extrasafe.md)). In-place patch znesie
> posun obsahu (≈ rast FW) len do extraSafeSize; nad ním degeneruje na ~celý FW (~300 KB).
> Od buildu > 265 je limit flashera 32 KB (`FOTA_MAX_EXTRA_SAFE`, flash_layout.h) a gen
> robí dvoch kandidátov: preferuje `-inplace-4096` (kompatibilný so VŠETKÝMI flashermi),
> pri degeneráte eskaluje na `-inplace-32768` + `[POZOR]` — taký balík odmietnu STARÉ
> flashery (build ≤ 265, err 0xE5; `ota verify` nového FW to povie vopred).
> (Rollback maličký a upgrade veľký = tento jav, nie chyba dát.)
> **Pre zariadenia na starom builde:** prvý hop musí mať extra_safe ≤ 4096 — pri
> degenerovanom páre použi reťaz hopov cez `builds/` archív s rastom ≤ ~4 KB na hop
> (napr. 232→238→246→265; balíky sa aplikujú postupne).

---

## 3. Push do telefónu

**DÔLEŽITÉ: rob to cez PowerShell, NIE Git Bash** — Git Bash mangle-uje `/sdcard/...` na
`C:/Program Files/Git/...` a push zlyhá. ([[fotapkg_push_to_phone]])

```powershell
$adb = "D:\FkDev\00_Downloads\scrcpy-win64-v4.0\adb.exe"; if (-not (Test-Path $adb)) { $adb = "adb" }
$pkg = "D:\FkDev\FkProj\VSC\MeshCore\test_nrf-fota\fotapkg_json\205-208.sensecap.fotapkg.json"
& $adb devices                       # over, že je zariadenie "device"
& $adb push $pkg /sdcard/Download/
& $adb shell ls -l /sdcard/Download/205-208.sensecap.fotapkg.json   # overenie
```

Alternatíva bez ciest: **double-click `test_nrf-fota/push_fotapkg.bat`** → natívny multi-select
dialóg (default v `fotapkg_json/`), pushne na `/sdcard/Download/`. (Beží aj na penv pythone bez tkinter.)

---

## 4. Rýchly checklist „build 205 → aktuálny na SenseCap + push"

1. `pio run -e SenseCap_Solar_repeater_fota` → build# sa bumpne, hook vyrobí `205-<N>.sensecap.fotapkg.json`
   (ak bol `205` posledný sensecap build v archíve; inak §2 ručne).
2. Over v logu riadok `[fotapkg] 205-<N>.sensecap.fotapkg.json ... [signed]`.
3. PowerShell adb push balíka na `/sdcard/Download/` (§3).

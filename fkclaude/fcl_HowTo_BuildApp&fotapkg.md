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
| Globálny build counter | `test_nrf-fota/build_number.txt` (bumpuje ho `pre:gen_build_info.py` pri každom builde) |
| FOTA env názvy | `ProMicro_repeater_fota`, `SenseCap_Solar_repeater_fota` |

**`device` v názvoch** = `PIOENV.split("_")[0].lower()` → `ProMicro_repeater_fota` → `promicro`,
`SenseCap_Solar_repeater_fota` → `sensecap`.

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

Výstup: `fotapkg_json/<old>-<new>.<device>.fotapkg.json` (upgrade) + `.rev.` (rollback).
Používaj **penv python** (`gen_fotapkg.py` importuje `fota_export_pkg`/`fota_sender` — potrebuje ich deps).

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

# VS Code: pomalý C++ hover/F12 — diagnóza a oprava (2026-07-12)

Poznámka z debug session (Claude Code + Fedor). Symptóm: hover na symbol ukazoval
„Loading…" niekoľko sekúnd, F12 (Go to Definition) rovnako pomalé, v každom súbore.

## Príčiny (tri vrstvy, všetky reálne)

1. **`C_Cpp.default.compileCommands` vo workspace settings** (`MeshCoreWS`/`ZephCoreWS`
   `.code-workspace`): v multi-root sa `${workspaceFolder}` vyhodnotí pre KAŽDÝ folder —
   MeshCore `compile_commands.json` nemá, takže cpptools konfigurácia visela.
   → presunuté na folder-úroveň `ZephCore/.vscode/settings.json`.
2. **Indexer žul 223 013 súborov / 138 692 C++ zdrojov** — dominovali `ZephCore/modules`
   (153k) a `ZephCore/zephyr` (60k). → `C_Cpp.files.exclude` + `files.watcherExclude`
   + `search.exclude` v oboch workspace súboroch (zephyr, modules, tools, .pio, build*,
   .west, .venv) → 12 429 súborov. IntelliSense ZephCore to neobmedzuje (hlavičky idú
   cez compile_commands includes), len fulltext symbol-search v Zephyr zdrojoch.
3. **`.BROWSE.VC.DB` mala 13,7 GB** v `%LOCALAPPDATA%\Microsoft\vscode-cpptools\` —
   nafúknutá z opakovaného indexovania; hlavný `cpptools.exe` do nej trvalo zapisoval
   (60 % jadra) a všetky requesty čakali za ním. → zmazať celý adresár pri vypnutom VSC:
   `Remove-Item -Recurse -Force "$env:LOCALAPPDATA\Microsoft\vscode-cpptools"` — DB sa
   postaví nanovo (s exclusions ostane malá). **Toto bol hlavný vinník.**

## Trvalé nastavenia

- Portable user settings (`D:\FkDev\VSCode\data\user-data\User\settings.json`):
  ```jsonc
  "C_Cpp.intelliSenseCachePath": "${env:DEV_ROOT}/cache/vscode-ipch",
  "C_Cpp.default.browse.databaseFilename": "${env:DEV_ROOT}/cache/vscode-cpptools-db/${workspaceFolderBasename}.vc.db"
  ```
  Dôvod: štartovací bat presmerúva USERPROFILE/HOME/TEMP, ale **nie LOCALAPPDATA**
  (zámerne — globálne presmerovanie by rozbilo cache iných nástrojov), takže natívne
  procesy cpptools písali na C:. Toto ich presmeruje cielene, cez `${env:DEV_ROOT}`.
- `meshcore/.vscode/settings.json`: `.pio` mimo indexera/watchera/search.
- Zvyškový `meshcore/.vscode/zephyr-ide.json` odstránený (Zephyr IDE patrí len ZephCore).
- `ZephCore/.vscode/compile_commands.json`: opravené staré cesty `D:\FkDev\zephyr_sdk\gnu`
  → `D:\FkDev\zephyr-sdk\zephyr-sdk-1.0.1\gnu`.

## Runbook pri recidíve

1. `Ctrl+Shift+P` → **C/C++: Log Diagnostics** — pozri `Current database path` a či má TU
   správny compiler/definy.
2. Skontroluj veľkosť `*.vc.db` (teraz `D:\FkDev\cache\vscode-cpptools-db\`) — gigabajty
   = zmazať, regeneruje sa.
3. Popup „Enumerated N files" pri štarte: N nad ~20k = do workspace pribudol ťažký strom,
   doplň exclusions.
4. `Get-Process cpptools` — trvalé CPU na hlavnom procese = stále indexuje/prepisuje DB.

Pozn.: ipch cache (`vscode-ipch`) ostáva takmer prázdna — cpptools ju zapisuje lenivo;
pri zdravom stave (TU parse ~0,5 s) to nevadí.

Bonus nález: v štartovacom bat-ku je west-install vetva rozbitá (`python -m curl` +
zlá URL) — správne `curl -o "%DEV_ROOT%\python312\get-pip.py" https://bootstrap.pypa.io/get-pip.py`.
Prejaví sa až na čerstvom stroji bez west.

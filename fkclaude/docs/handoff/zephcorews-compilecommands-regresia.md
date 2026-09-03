# Zadanie: odstrániť `compileCommands` zo `ZephCoreWS.code-workspace`

**Cieľ:** vrátiť opravu zo 14. 7. 2026, ktorá sa 24. 7. stratila.

## Nález (3. 9. 2026)

`D:\FkDev\FkProj\VSC\ZephCoreWS.code-workspace` (ten, ktorý Fedor používa) má
v `settings`:

```json
"C_Cpp.default.compileCommands": "${workspaceFolder}/.vscode/compile_commands.json"
```

Fedorova vlastná poznámka (`zephcore_env_sdk` v pamäti) hovorí:

> POZOR na `C_Cpp.default.compileCommands` — **NESMIE** byť v .code-workspace settings
> (multi-root → rozbije IntelliSense MeshCore folderu), patrí len do
> `ZephCore/.vscode/settings.json` (regresia odstránená 2026-07-14).

Dátumy sedia: `MeshCoreWS.code-workspace` je zo **14. 7.** a ten kľúč **nemá**,
`ZephCoreWS.code-workspace` je z **24. 7.** a **má** ho. Vrátilo sa to desať dní po oprave.

## Prečo to škodí

V multi-root workspace platí `${workspaceFolder}` **pre každý priečinok zvlášť**.
Overené: `compile_commands.json` má **len ZephCore**; MeshCore, FK_lora-sniffer,
meshcore_py, meshcore-open, bootloader a esp32-s3 ho nemajú. Šesť zo siedmich teda
ukazuje do prázdna → cpptools konfiguráciu nikdy nerozrieši → hover visí na „Loading".

## Postup

1. Zisti, prečo sa to 24. 7. vrátilo — či to nebolo zámerné kvôli niečomu inému.
   **Bez toho nemazať.**
2. Zmaž ten jeden riadok zo `ZephCoreWS.code-workspace`. Záloha do scratchpadu.
3. Nič sa nestratí: `ZephCore/.vscode/settings.json` ten kľúč **má** (overené), takže
   ZephCore mu ostane; ostatných šesť priečinkov prestane ukazovať do prázdna.

## Bokom, na zváženie

Sú tam **tri** workspace súbory a `MeshCore.code-workspace` (7. 7.) má **0 nastavení** —
teda ani tie výnimky z júla. Otvoriť ho omylom = návrat pred opravu. Kandidát na zmazanie.

`MeshCoreWS` a `ZephCoreWS` majú tie isté priečinky, líšia sa poradím (ZephCore prvý,
lebo Zephyr IDE berie root z prvého) a tým jedným kľúčom. Poradie sa mení priamo v poli
`folders` v tom súbore — myšou to VS Code preusporiadať nedovolí, preto Fedor robil kópiu.

## Hotovo keď

`ZephCoreWS` ten kľúč nemá, ZephCore build/IntelliSense stále funguje a hover v MeshCore
je rýchly.

# Zadanie: PR nahrávania PRAM do RadioLibu

**Cieľ:** poslať upstream do jgromes/RadioLib to, že LR2021 si má patch RAM nahrať
sama knižnica.

## Prečo

Fedorova úvaha: **ak Semtech predpisuje nahranie PRAM ako súčasť reset sekvencie,
má to robiť RadioLib, nie my.** Datasheet §22.3. Dnes to robíme v MeshCore, čo je
zlá vrstva.

## Stav — hotové, netreba stavať znova

Worktree `scratchpad/radiolib-pram`, vetva **`pram`**, commit **`f7ede7aba`**
„[LR2021] Load the firmware patch RAM during init". Postavené na jgromesovom **mastri
+ jeho BUSY patchi** (`440610d71`), teda nie na našej testovacej zostave.

Obsah:
- nový `src/modules/LR2021/LR2021_pram.h` — obraz PRAM, 560 slov / 2240 B, Clear BSD
- `LR2021::loadPram()` v `LR2021_cmds_chip_control.cpp`
- volanie v `modSetup()` **po `standby()` a pred TCXO** (Semtechom predpísané poradie)
- vypínateľné cez `RADIOLIB_EXCLUDE_LR2021_PRAM` v `BuildOpt.h`

Fakty: base `0x801000`, aktivačný opkód `0x012D`, magic `0x600DB002` na `0x800FF8`,
verzia na `0x800FFC`, `RADIOLIB_LRXXXX_SPI_MAX_READ_WRITE_LEN` = 128 B = 32 slov na zápis.

**Overené na železe:** `pram: loaded=YES version=0x0313` s vypnutým naším
`LR2021_PRAM_UPD`, teda knižnica to naozaj nahrala sama, v správnom poradí a bez GODMODE.

## Čo zostáva

1. Ak bude PRAM nahrávať RadioLib, **naše nahrávanie z MeshCore má ísť preč** —
   Fedorova poznámka. Vyriešiť, ako to spraviť kompatibilne so staršou knižnicou.
2. Napísať telo PR do `fkclaude/docs/PRs/` podľa konvencie
   (`rl-<číslo>-pr-<téma>.md`, číslo doplniť po vytvorení).
3. Text vopred Fedorovi na schválenie. Žiadna zmienka o AI.

## Pasce

- Do **commit message** nikdy `#číslo` ani URL — sieť forkov vyrobí trvalé krížové
  odkazy. Stalo sa to už dvakrát.
- CI upstreamu vyžaduje schválenie pre každého mimo organizácie (`action_required`),
  nám nikdy nezbehne samo. Overovať cez `gh api actions/runs`, nie `gh pr checks`.

## Hotovo keď

PR je otvorený a Fedor pozná jeho číslo.

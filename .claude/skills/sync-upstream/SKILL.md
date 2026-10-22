---
name: sync-upstream
description: Use when Fedor asks to sync the fork with upstream MeshCore — "vyrovnanie vetiev", "sprav merge upstream/dev", "zosynchronizuj s upstreamom", "natiahni upstream do nrf-fota". Fast-forwards dev from upstream/dev without checkout, merges into features/nrf-fota, resolves the known conflict spots, verifies with the FOTA build batch, pushes.
---

# Vyrovnanie vetiev (upstream → dev → features/nrf-fota)

Fork `fkallay1/MeshCore`, remotes: `upstream` = `meshcore-dev/MeshCore`, `origin` = fork.
Vetvy: `main` sleduje upstream/main (neudržiavaná), **`dev` je čistá kópia
`upstream/dev` bez vlastných commitov**, `features/nrf-fota` je pracovná vetva.

## Postup

```bash
# 1) Kontrola rozpracovaného — PRED čímkoľvek
git status --porcelain -uall          # hlavný strom musí byť čistý
git stash list
git worktree list                     # aj v worktree vetvách: git -C <cesta> status --porcelain
git branch -vv                        # vetvy bez [origin/...] = nepushnuté commity

# 2) Fetch + koľko toho je
git fetch upstream --prune && git fetch origin --prune
git rev-list --count dev..upstream/dev     # koľko commitov pribudlo
git rev-list --count upstream/dev..dev     # MUSÍ byť 0 (dev nemá vlastné commity)

# 3) Fast-forward dev BEZ checkoutu
git fetch . upstream/dev:dev

# 4) Merge do pracovnej vetvy (byť na features/nrf-fota)
git merge dev --no-edit

# 5) Verify build (POVINNÉ, viď pasce) — ~7 min
pio run -e ProMicro_repeater_fota -e RAK_3401_repeater_fota -e RAK_4631_repeater_fota \
        -e SenseCap_Solar_repeater_fota -e t1000e_repeater_fota -e Xiao_nrf52_repeater_fota \
        -e Xiao_nrf52_companion_radio_usb

# 6) Commit (viacriadková správa cez -F, nie -m) + push
git commit -F <scratchpad_súbor>
git push origin features/nrf-fota dev
```

## Pasce

- **`git fetch . upstream/dev:dev` — nikdy `git checkout dev`.** Checkout by zahodil
  skip-worktree `test_nrf-fota/build_number.txt` (na `dev` ten súbor neexistuje).
- **Čistý `git status` po merge NIE JE dôkaz, že to preloží.** Git dokáže bez konfliktu
  spojiť dva pridané riadky do jedného `#ifdef` bloku (2026-08-14: dve deklarácie
  `user_btn` v `variants/xiao_nrf52/target.cpp` → `redefinition`). **Vždy buildovať.**
- Build FOTA envov **bumpne build#** a auto-hook vygeneruje fotapkg posledných 2 buildov —
  to je pri vyrovnávaní legitímne, netreba to riešiť.
- Konflikt `.vscode/extensions.json` (upstream ho prestal trackovať `b40968a0`, my držíme
  kvôli NoBuild task rozšíreniam) sa riešil RAZ „keep ours" (`b9ce15c1`); ďalšie merge
  už nekonfliktujú.
- **Prepnutie medzi vlastnými vetvami zablokuje `build_number.txt`** („would be overwritten
  by checkout") — vetvy majú v repe rôzne hodnoty (fkclaude/docs/fork-vetvy-a-sync-upstream.md#build_numbertxt--hodnoty-v-repe). Postup:
  `cp $F ../bn_save.txt; git update-index --no-skip-worktree $F; git checkout -- $F;
  git checkout <vetva>; cp ../bn_save.txt $F; git update-index --skip-worktree $F`.
- **Ďalšie vlastné vetvy** (inventár a reťaz: fkclaude/docs/fork-vetvy-a-sync-upstream.md#vetvy-forku): do LR2021 vetiev
  merguj `features/nrf-fota` (nie dev) a buildni aj `Xiao_nrf52_nicerf_lora2021f33_repeater[_fota]`
  + `t1000e_repeater_fota`. **Nemergovať** dev do PR vetiev (`fix/*`, `feat/nicerf-*`) — tie sa
  prípadne rebasujú.
- Upstream ide ~20–30 commitov/deň — „behind X commits" na GitHube deň po syncu je
  normálne. Syncovať pri reálnej potrebe, nie denne.

## Známe konfliktné miesta

Zoznam súborov, čo sa v nich deje a overené riešenia: fkclaude/docs/fork-vetvy-a-sync-upstream.md#známe-konfliktné-miesta-pri-merge-upstreamu
— pri každom syncu ho doplň (rastie). História syncov: fkclaude/docs/fork-vetvy-a-sync-upstream.md#história-syncov.

Po merge vždy skontrolovať, či upstream nesiahol na FOTA háky —
`onGroupDataRecv`, `searchChannelsByHash`, `allowPacketForward`, `logRxRaw`
(grep v `examples/simple_repeater/nrffota/` a `MyMesh.cpp`).

## Report Fedorovi

Vždy uviesť: koľko commitov prišlo, ktoré konflikty a ako riešené, výsledok buildov,
a **čo zostalo rozpracované** (stashe, nepushnuté vetvy vo worktree).

## Súvisiace

- Build/flash zariadení = skill `build-flash`, FOTA cez LoRa = skill `remote-e2e`.
- `test_nrf-fota/build_number.txt` je `skip-worktree` (v repe neutrálna hodnota, per vetva viď doc
  vyššie) — po novom klone flag nastaviť znova: `git update-index --skip-worktree test_nrf-fota/build_number.txt`.

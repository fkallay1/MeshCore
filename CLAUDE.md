# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Authoritative docs

- **`AGENTS.md`** (repo root) is the primary build/architecture reference — read it first. It covers the PlatformIO build system, `build_src_filter` mechanism, per-platform FS, NRF52 specifics, LoRa-OTA, and style notes. Do not duplicate it; extend it when something is missing.
- **`fkclaude/`** holds the maintainer's (Fedor Kallay) working docs, prefixed `fcl_*`. Read **all** `fcl_*.md` files at the start of every session (they are the working context, not reference material to look up ad hoc); `fcl_sess_summary.md` is the index of past sessions. The main ones:
  - `fcl_readme_nrf-fota.md` / `fcl_readme_tech_nrf-fota.md` — the LoRa delta-patch FOTA system for nRF52840 repeaters (a custom feature, separate from MeshCore's built-in BLE DFU "OTA").
  - `fcl_readme_fota_extrasafe.md`, `fcl_readme_scope_multihop.md`, `fcl_readme_verified_pooling.md`, `fcl_e2e_runbook_nrf-fota.md`, `fcl_remote_e2e_rpi.md` — FOTA limits, scopes, test hardening and the remote test bench.
  - `fcl_readme_nicerf_lora2021.md`, `fcl_rozvoj_wm1302_koncentrator.md` — LR2021 board variant and concentrator plans.
  - `fkclaude/docs/` — helper docs, PR texts (`docs/PRs/`), branch/sync notes (`docs/fork-vetvy-a-sync-upstream.md`).
- **Cross-project tooling lives in `D:\FkDev\FkProj\AIToolsCommon\tools\`** — notably `check_doc_links.py` (verifies links/anchors in any project's `fkclaude/`; used by the `/session-summary` skill). It used to sit in `fkclaude/tools/` here; moved out 2026-09-12 so the other projects can reach it. `fkclaude/tools/strip_lang_comments.py` stays local — the bilingual `//en:`/`//sk:` comment convention is specific to this fork.
- **Auxiliary / Claude-generated docs go under `fkclaude/docs/`, never the upstream tree.** The upstream `docs/` directory and the repo root are off-limits for helper material. In particular, brainstorming specs and implementation plans (the superpowers skills' default `docs/superpowers/specs|plans/`) MUST be written to `fkclaude/docs/superpowers/specs|plans/` — this user preference overrides the skills' default location. Do not create new top-level directories or files of a helper nature; consolidate them under `fkclaude/`.

## Common commands

```bash
# List every build env (there are ~100, one per board variant)
pio project config | grep 'env:' | sed 's/env://'

# Build / flash a single env
pio run -e <env_name>
pio run -e <env_name> --target upload

# Unit tests (host-native, googletest) — only src/Utils.cpp is wired in
pio test -e native --verbose

# Build via the wrapper script (Git Bash / WSL) — FIRMWARE_VERSION env var is REQUIRED
sh build.sh list
sh build.sh build-firmware <target>
sh build.sh build-matching-firmwares <string-match>   # outputs .bin/.uf2 to out/
```

There is no lint step and no hardware CI; embedded targets are verified manually on-device.

## Architecture: the three-layer packet stack

The core library (`src/`) is a layered class hierarchy. Applications subclass the top layer:

1. **`Dispatcher`** (`Dispatcher.cpp/.h`) — lowest level. Owns the `Radio`, `PacketManager`, and clock abstractions. Detects raw incoming packets, manages the prioritized outbound/inbound queues, CAD, airtime/duty-cycle budgeting, and AGC/noise-floor calibration. `onRecvPacket()` is pure-virtual for the next layer. Retransmit decisions are returned as a `DispatcherAction` (`ACTION_RELEASE`, `ACTION_RETRANSMIT(pri)`, etc.).
2. **`Mesh`** (`Mesh.cpp/.h`) — recognizes `PAYLOAD_TYPE_*`, routes/forwards/holds packets (`routeRecvPacket`), handles flood vs. direct, path stripping, ACKs, and decryption. Exposes virtual hooks for subclasses: `searchPeersByHash`, `getPeerSharedSecret`, `onGroupDataRecv`, `searchChannelsByHash`, `allowPacketForward`, etc.
3. **`MyMesh`** (per example, e.g. `examples/simple_repeater/MyMesh.cpp`) — the application. Each example's `main.cpp` instantiates a `MyMesh`, then calls `the_mesh.begin()` and `the_mesh.loop()`. `handleCommand()` dispatches CLI commands (Serial or LoRa admin channel).

The radio is abstracted behind `Radio` (Dispatcher.h) and implemented via RadioLib wrappers in `src/helpers/radiolib/`. Board/platform support lives in `src/helpers/` (`ESP32Board`, `NRF52Board`, etc.).

**Key pattern — deferred processing:** receive hooks like `onGroupDataRecv()` must only copy data into a buffer; heavy work (flash I/O, decompression) happens later in `loop()` after the radio is re-armed. Violating this stalls reception.

## Conventions specific to this fork

- The maintainer commits and pushes milestones autonomously on dev/feature branches; push promptly so nothing lives only on this machine. Working branches are `features/nrf-fota` (FOTA), `test/lr2021-runtime-ab` (LR2021 tests, where `fkclaude/` docs are currently committed) and `feat/nicerf-lora2021f33-xiao`; `main` is upstream-tracking, `dev` is the upstream PR base. Branch map and sync procedure: `fkclaude/docs/fork-vetvy-a-sync-upstream.md`, skill `sync-upstream`.
- Commit messages and anything pushed to GitHub: plain English, no issue numbers (`#NN`) or URLs (this is a fork — GitHub turns them into permanent cross-references in upstream), no tool attribution lines.
- Embedded discipline (from README contributing notes): no dynamic allocation outside `setup()`/`begin()`; keep code concise and low-layer; **do not** reformat existing code (creates noise diffs).
- OTA-specific hard constraints (see `fkclaude/` + AGENTS.md): keep `agc_reset_interval = 0` during OTA flash, and the flasher lives at flash `0xEB000` to avoid MeshCore's InternalFS at `0xED000`.

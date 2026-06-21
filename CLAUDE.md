# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Authoritative docs

- **`AGENTS.md`** (repo root) is the primary build/architecture reference — read it first. It covers the PlatformIO build system, `build_src_filter` mechanism, per-platform FS, NRF52 specifics, LoRa-OTA, and style notes. Do not duplicate it; extend it when something is missing.
- **`fkclaude/`** holds the maintainer's (Fedor Kallay) working docs, prefixed `fcl_*`. Read these at the start of a session when touching their subject area:
  - `fcl_readme_nrf-ota.md` / `fcl_readme_tech_nrf-ota.md` — the LoRa delta-patch OTA system for nRF52840 repeaters (a custom feature, separate from MeshCore's built-in BLE DFU "OTA").
  - `fcl_readme_verified_pooling.md` — hardening of the OTA end-to-end test.

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

- The maintainer commits and pushes milestones autonomously on dev/feature branches (OneDrive corrupts `.git`, so work is pushed promptly). Branches like `features/nrf-ota` are the working branches; `main` is upstream-tracking, `dev` is the upstream PR base.
- Embedded discipline (from README contributing notes): no dynamic allocation outside `setup()`/`begin()`; keep code concise and low-layer; **do not** reformat existing code (creates noise diffs).
- OTA-specific hard constraints (see `fkclaude/` + AGENTS.md): keep `agc_reset_interval = 0` during OTA flash, and the flasher lives at flash `0xEB000` to avoid MeshCore's InternalFS at `0xED000`.

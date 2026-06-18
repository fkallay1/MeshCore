# MeshCore — AGENTS.md

## What this is
MeshCore: Arduino-based LoRa mesh networking firmware library for nRF52, ESP32, RP2040, STM32. Built with PlatformIO.

## Build system
- **PlatformIO**, not standalone Make/CMake. Root `platformio.ini` uses `extra_configs = variants/*/platformio.ini` to pull per-board configs.
- **Required**: `FIRMWARE_VERSION` env var before any build (checked in `build.sh`).
- **Run a single env**: `pio run -e <env_name>`
- **Build and flash**: `pio run -e <env_name> && pio run -e <env_name> --target upload`
- **List all envs**: `pio project config | grep 'env:' | sed 's/env://'`

### Build script (Git Bash / WSL)
```
sh build.sh list                                    # list all envs
sh build.sh build-firmware <target>                  # build single target
sh build.sh build-matching-firmwares <string-match>  # build matching targets
sh build.sh build-companion-firmwares                 # all companion radios
sh build.sh build-repeater-firmwares                  # all repeaters
sh build.sh build-room-server-firmwares               # all room servers
sh build.sh build-firmwares                           # companion + repeaters + room servers
```
- Output `.bin` / `.uf2` files go to `out/` subdirectory.
- `DISABLE_DEBUG=1` strips all debug logging flags before build.

### Adding a new device variant
1. Create `variants/<name>/platformio.ini` with env sections extending `<platform>_base` (e.g. `nrf52_base`, `esp32_base`).
2. Add board JSON to `boards/` if it isn't in PlatformIO registry.
3. Put variant-specific `target.h` / `target.cpp` in the variant directory.
4. Reference it in `build_src_filter` with `+<../variants/<name>>`.

The `build_src_filter` mechanism (in root `platformio.ini` and `build_as_lib.py`) controls what `src/` files are compiled. Key CPPDEFINES filter variants and examples at build time:
- `MC_VARIANT` → includes `variants/<name>/`
- `BUILD_EXAMPLE` → includes `examples/<name>/*.cpp`
- `EXCLUDE_FROM_EXAMPLE` → excludes a file from an example
- `DISPLAY_CLASS` → pulls the correct display driver from `helpers/ui/`
- Platform defines: `ESP32_PLATFORM`, `NRF52_PLATFORM`, `STM32_PLATFORM`, `RP2040_PLATFORM`

### Debug logging
Disabled by default (`-w -DNDEBUG`). Uncomment `; -D MESH_DEBUG=1` or `; -D MESH_PACKET_LOGGING=1` in the per-variant `platformio.ini`.

## Architecture notes
- **Core library**: `src/` — `Mesh.cpp`, `Dispatcher.cpp`, `Packet.cpp`, `Identity.cpp`, `Utils.cpp`, `helpers/`.
- **Examples** (applications that link against core): `examples/simple_repeater/`, `examples/simple_sensor/`, `examples/simple_room_server/`, `examples/companion_radio/`, `examples/simple_secure_chat/`, `examples/kiss_modem/`.
- **Each example entrypoint** is `main.cpp`. It creates a `MyMesh` (extending `Mesh`) instance, then calls `the_mesh.begin()` and `the_mesh.loop()` with example-specific behavior (CLI commands, buttons, displays).
- **`MyMesh::handleCommand()`** — CLI command dispatch (Serial or LoRa admin channel).
- **`MyMesh::onGroupDataRecv()`** — virtual hook for received packet handling.

### FS per platform
- ESP32: SPIFFS at `/identity`
- nRF52/STM32: `InternalFS`
- RP2040: `LittleFS`
- nRF52 OTA: `CustomLFS` at 0xD4000 (separate from internal FS)

### NRF52 platform specifics
- Custom Adafruit nRF52 Arduino fork used (see `platformio.ini` nrf52_base for URL).
- Softdevice: s140 v6 (default) or v7 (XIAO boards, needs `-D OTA_SOFTDEVICE_V7`).
- Linker scripts in `boards/`: `nrf52840_s140_v6.ld`, `nrf52840_s140_v6_extrafs.ld` (extra FS, app ends at 0xD4000).
- OTA repeater variants use `_extrafs.ld` + `board_upload.maximum_size = 712704`.

### LoRa radio config
- Uses RadioLib wrapper (`src/helpers/radiolib/`).
- Radio class and wrapper defined per variant: `RADIO_CLASS`, `WRAPPER_CLASS`.
- SX1262: `USE_SX1262` + `CustomSX1262` radio + `CustomSX1262Wrapper`.
- Preamble: SF≤8 → 32, SF>8 → 16 (set in `RadioLibWrappers`).

## LoRa-OTA (nRF52840 only)
Located in `examples/simple_repeater/nrfota/`, gated by `-D WITH_LORA_OTA=1`.

**Build order**:
1. `python examples/simple_repeater/nrfota/tools/build_flasher.py` — generates `nrfota/flasher_code.h` (only needed once or after `flasher.c` changes).
2. `pio run -e ProMicro_repeater_ota` — builds with OTA support.

CLI commands: `ota status | verify | flash | clear | decompress | nack | dbg | agc`

**Critical constraints**:
- `agc_reset_interval` must be 0 (MeshCore default) — **NEVER use non-zero value with OTA flash**. AGC resets during OTA session cause `ota flash` to fail (flasher stops at "Komprimovany format").
- `ota verify` (dry-run) before `ota flash` causes heap hardfault on some boards — skip dry-run for reliability, or use `--verify-first` opt-in.
- Flash layout: app 0x26000–0xD4000, OTA FS 0xD4000–0xEB000, flasher 0xEB000–0xEC000, reserved 0xEC000–0xED000, MeshCore InternalFS 0xED000–0xF4000, bootloader 0xF4000+.

**End-to-end tests** in `test_nrf-ota/` require `hdiffi.exe`, `pyserial`, `pycryptodome` in PlatformIO's virtualenv.

## Testing
- Root `platformio.ini` has `[env:native]` section with googletest. Currently only `src/Utils.cpp` is included:
  ```
  pio test -e native
  ```
- No automated integration/hardware tests in CI (embedded platform).
- LoRa-OTA end-to-end tests are manual CLI in `test_nrf-ota/`.

## Docs
- mkdocs config at root `mkdocs.yml` → docs served at `docs/` (GitHub Pages via `meshcore-dev.github.io/meshcore/`).

## Style notes
- Use `extern RADIO_CLASS radio;` for direct radio access (e.g. in `MyMesh.cpp`).
- `#ifdef` guards are preferred over compile-time flags for optional features.
- Deferred processing pattern: `onGroupDataRecv()` copies data to buffer, heavy work happens in `loop()`.

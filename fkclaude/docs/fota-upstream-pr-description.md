# Upstream PR description — LoRa delta-patch FOTA (nRF52840 repeaters)

> Meta (SK): Draft popisu pre Pull Request do `meshcore-dev/MeshCore`. Text nižšie je
> pripravený na priame vloženie do GitHub PR (telo od „---" po koniec). Pred odoslaním
> PR: strip `//sk:` komentárov na špeciálnej vetve (viď [[fota_rename]] konvencie),
> over linky a čísla buildov. Inšpirované štruktúrou upstream PR #2864.

Suggested PR title:

**FOTA over LoRa: delta-patch firmware updates for nRF52840 repeaters (no core changes, opt-in build flag)**

---

## TL;DR

This PR adds **firmware-over-the-air updates via LoRa** for nRF52840-based repeaters.
Instead of transferring the whole firmware (~450 kB — impractical over LoRa), it sends a
**binary delta patch** between the old and the new firmware — typically **a few hundred
bytes to a few kB** for an incremental release. The patch travels over a standard
encrypted MeshCore group-data channel, is verified on the device (SHA-256 + Ed25519
package signature + running-firmware check), and applied in-place by a small standalone
flasher. The update can be driven **from a phone app through a completely stock,
unmodified companion device** — no special gateway hardware is needed.

Everything is **opt-in and isolated**: the feature lives in
`examples/simple_repeater/nrffota/` behind `-D WITH_LORA_FOTA=1`, hooks into `MyMesh`
via ~30 lines of thin `#ifdef` glue, and adds one empty-default core hook. Stock builds
are byte-for-byte unaffected.

## Motivation

Repeaters are the one MeshCore node class that is routinely placed somewhere you really
don't want to climb back to — rooftops, masts, remote hills, solar boxes. The built-in
update path (Nordic BLE DFU via `startOTAUpdate`) requires being within Bluetooth range,
which defeats the purpose for exactly these nodes.

At the same time, most firmware updates between consecutive releases change only a small
fraction of the image. A binary diff of two consecutive builds is often under 1 kB — small
enough to deliver over the mesh itself, through the repeaters' own radio, from wherever
the operator happens to be.

## The workflow (operator's view)

The intended day-to-day flow is deliberately boring:

1. **Build** the new firmware. Post-build hooks archive the application image and
   automatically generate a **FOTA package** (`.fotapkg.json`) between the previous and
   the current build — a self-contained JSON with the compressed delta patch, metadata
   (old/new SHA-256, sizes, build IDs) and an Ed25519 signature. A matching **rollback
   package** (new→old) is generated alongside it.
2. **Copy the package to a phone** (it's a single small JSON file).
3. In the **phone app**, connect to any nearby MeshCore **companion** node over BLE/USB —
   a completely standard companion build, nothing FOTA-specific in it — pick the package,
   and send. The app streams the patch as encrypted channel packets, watches the
   repeater's status replies, and re-sends only the chunks the repeater reports missing.
4. On the repeater, once the transfer verifies (`VERIFIED`), issue `fota flash` (remote
   CLI or automatic apply). The device reboots into the new firmware. If anything is
   wrong — wrong base firmware, corrupted patch, failed post-write verification — the
   device refuses to write, or falls back to DFU rather than booting a broken image.

Because the transport is ordinary MeshCore packets, the operator can update a repeater
that is **one or more hops away**, from the couch, through the mesh.

## How it works

**Delta patch.** The sender runs [HPatchLite](https://github.com/sisong/HPatchLite)
(`hdiffi -inplaceB`) on the old/new application images and compresses the patch with
raw DEFLATE (zlib level 9). For a typical incremental build the result is a few hundred
bytes; the receiver-side cap is ~40 kB.

**Transport.** Patches are chunked into standard `PAYLOAD_TYPE_GRP_DATA` packets on a
dedicated channel (regular `#name`-derived PSK; AES + HMAC exactly like any other
MeshCore channel — no new crypto). The receiver integrates through **existing virtual
hooks only**: `searchChannelsByHash()` recognizes the FOTA channel,
`onGroupDataRecv()` copies the payload into a buffer, and all heavy work (filesystem
I/O, assembly) is deferred to `loop()` after the radio is re-armed — reception is never
stalled.

**Session & reliability.** Received chunks go into an append-log in a dedicated
LittleFS region (separate from InternalFS — identity/prefs/ACL are untouched). The
session is reboot-resilient (bitmap checkpointing) and even survives a DFU reflash of
the application. Transfers are resumable: the `fota miss` / `fota missall` CLI replies
report missing chunk ranges, and the sender re-sends only those. A repeater reachable
only via multiple hops can pin a return path (`fota setpath`) so status replies travel
directly instead of flooding.

**Verification chain.** Before anything is written:
- the package header carries an **Ed25519 signature** checked against an author
  public-key allowlist compiled into the firmware;
- the assembled patch is verified by **SHA-256**;
- the patch's expected *old* firmware hash is compared against the **actually running
  firmware** — a patch built for a different base is rejected outright;
- `fota verify` offers a full **dry-run**: the new image is reconstructed in RAM and
  hashed without writing a single byte.

**Applying.** `fota flash` disables the SoftDevice and interrupts, places a ~4 kB
standalone flasher outside the application area, and jumps to it. The flasher streams
the patch (on-the-fly DEFLATE via a no-libc `puff` port, 512 B window) through
HPatchLite's in-place mode — old data is read XIP from flash, new data is written
page-by-page over it, so neither the full image nor the full patch ever needs to fit in
RAM, and **no second firmware slot is required**. After writing, it reads the flash back
and verifies a checksum; on mismatch it jumps to DFU instead of booting a corrupted
image. On success it resets into the new firmware.

**Remote control.** The full CLI (`fota status | verify | flash | clear | miss |
missall | setpath | …`) works identically over Serial and over the existing LoRa admin
channel, so the whole cycle can be driven remotely.

## Isolation & footprint

- All FOTA code lives in `examples/simple_repeater/nrffota/` (receiver, patcher,
  flasher, protocol, vendored HPatchLite + puff), guarded by `#ifdef WITH_LORA_FOTA`.
  Without the flag the files are inert and stock builds are unaffected.
- `MyMesh.{h,cpp}` gets ~30 lines of thin hook calls; the actual bodies live in
  `nrffota/FotaMyMesh.cpp`.
- The only core change is a new `logTxRaw()` hook in `Dispatcher` (empty default,
  mirror of the existing `logRxRaw`) used for TX-side diagnostics.
- Three opt-in build environments are added: `ProMicro_repeater_fota`,
  `SenseCap_Solar_repeater_fota`, `Xiao_nrf52_repeater_fota`. They use an `extrafs`
  linker script variant that ends the application at 0xD4000, reserving a region for
  the FOTA filesystem and flasher while leaving MeshCore's InternalFS untouched.

The design has already proven portable: the same shared FOTA sources (dual
`FOTA_MESHCORE_BUILD` / `FOTA_ZEPHCORE_BUILD` guards) have been **ported to and
HW-validated on ZephCore**, the Zephyr-based MeshCore implementation, with a
byte-identical protocol on the wire.

## Building a FOTA package

Automatic (the common case): every build of a FOTA environment archives the app image
and generates the package between the last two builds of that board, signed if the
Ed25519 key is present:

```
pio run -e SenseCap_Solar_repeater_fota
# → fotapkg_json/<old>-<new>.<device>.fotapkg.json  (+ .rev. rollback package)
```

Manual, for an arbitrary pair of archived builds:

```
python test_nrf-fota/gen_fotapkg.py --old builds/sensecap.fw_205.bin \
       --new builds/sensecap.fw_208.bin --device sensecap
```

The generator produces two patch candidates (a conservative in-place profile compatible
with all deployed flashers, and an escalated one for large-growth updates) and always
emits the rollback package, so a bad update can be undone over the air the same way it
arrived.

## Sending — the app is the primary path

- **Phone app (primary):** a Flutter app based on
  [zjs81/meshcore-open](https://github.com/zjs81/meshcore-open) with an added FOTA
  sender module — [fkallay1/meshcore-open](https://github.com/fkallay1/meshcore-open),
  branch `feature/nrf-ota-sender`. It connects to a **stock companion** (BLE/USB/TCP),
  sends the package over the standard channel-data command, tracks progress from the
  repeater's status replies, and re-sends missing chunks. The longer-term goal is for
  FOTA sending to land in the standard MeshCore app(s); alternatively it can ship as a
  small standalone app, since everything needed on the radio side is already in stock
  companion firmware.
- **Python script:** `test_nrf-fota/fota_sender_mcpy.py` does the same through a
  companion via `meshcore_py` — handy for scripting and CI-style use.

No dedicated gateway hardware is required — any regular companion node acts as the
entry point into the mesh.¹

## Scopes & multi-hop

The sender can control how far packets propagate, so an update doesn't have to flood
the whole network: `zerohop` (direct neighbours only, the default), `flood`, `region`
(transport-coded flood confined to a named region), and `direct` (an explicit hop path
to the target). Multi-hop direct requires nothing from intermediate repeaters beyond
stock forwarding behaviour.

## Platform support & testing

HW-validated end-to-end (full receive → verify → flash → boot-new-firmware cycles) on
three nRF52840 repeater targets: **Seeed ProMicro-compatible (nRF52840 + SX1262)**,
**SenseCap Solar Node**, and **Seeed XIAO nRF52840**. Companions used during testing:
**XIAO nRF52840** and **Seeed T1000-E** — both running **unmodified stock companion
firmware**.

- **Zero-hop:** extensively tested — dozens of complete flash cycles across a build
  counter spanning ~280 test builds, including an automated end-to-end harness
  (`test_nrf-fota/fota_test_lora_repeater.py`) with packet-loss injection and
  multi-round chunk accumulation.
- **Defined multi-hop path:** validated on a **live production mesh** (869.618 MHz /
  SF8): direct 1-hop and 2-hop paths through regular stock repeaters, ending in a
  verified transfer and successful flash; region-scoped delivery likewise.
- **Failure paths tested:** patch for a different base firmware is rejected without
  writing; post-write verification mismatch diverts to DFU; the receive session
  survives reboots and DFU reflashes; dry-run `fota verify` reconstructs the image
  bit-exactly; rollback packages restore the previous build over the air.

## Notes / scope

- nRF52840 only for now (SoftDevice s140 v6 and v7 layouts both supported; the
  application base address is taken from a linker symbol at runtime, so one flasher
  blob serves all boards).
- The patch is applied **in-place** — no second slot, at the cost of an upper bound on
  how much the image may *grow* per update before the patch degenerates; the tooling
  detects this and falls back to a chain of smaller hops through archived builds.
- The delivery is deliberately fire-and-forget + selective repair (`miss`/`missall`)
  rather than a windowed ARQ — simple, radio-friendly, and proven sufficient in
  practice.
- Build-number scaffolding under `test_nrf-fota/` is test tooling, not a release
  mechanism; happy to trim or relocate any of it per maintainer preference.

---

¹ For fully automated regression tests we also drive a raw-LoRa serial bridge, but that
is test infrastructure only — no user-facing flow depends on it.

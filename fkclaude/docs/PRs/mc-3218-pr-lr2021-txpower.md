# PR — LR2021 setTxPower

**ODOSLANÉ 2026-08-15: https://github.com/meshcore-dev/MeshCore/pull/3218** (open)

Vetva: `fix/lr2021-set-tx-power-rx-stall` (z `upstream/dev`), na `origin`.
Worktree `…/scratchpad/mc-pr-lr2021` — po zmergovaní/zavretí PR zmazať cez
`git worktree remove`.

Použitý príkaz (telo = len časť za `## Body`, nie tento súbor celý):

```bash
D:/FkDev/GHcli/bin/gh.exe pr create \
  --repo meshcore-dev/MeshCore \
  --base dev \
  --head fkallay1:fix/lr2021-set-tx-power-rx-stall \
  --title "Fix LR2021 occasionally stopping reception after a TX power change" \
  --body-file fkclaude/docs/PRs/mc-3218-pr-lr2021-txpower.md
```

(`gh` treba najskôr prihlásiť — viď poznámku o `gh auth login`.)

---

## Title

```
Fix LR2021 occasionally stopping reception after a TX power change
```

## Body

**Type: bug**

### Symptom

On an LR2021 board, changing the transmit power at runtime (`set tx <dbm>`)
could stop the radio from receiving. Nothing recovered it — the node kept
running and still transmitted its own adverts, but never received another
packet until it was rebooted. It happened only occasionally, a few times over
several hours of use.

### Cause

`RadioLibWrapper::setTxPower()` calls `setOutputPower()` and nothing else:

```cpp
void RadioLibWrapper::setTxPower(int8_t dbm) {
  _radio->setOutputPower(dbm);
}
```

On LR2021 that writes the PA config and TxParams, which are standby-only
commands, and the receiver is never re-armed afterwards.

This only bites on LR2021 because of the platform split in `recvRaw()`:

```cpp
#if defined(USE_LR2021)
  state = STATE_RX;     // LR2021 stays in Rx after readData
#else
  state = STATE_IDLE;   // need another startReceive()
#endif
```

On SX126x the state falls back to `STATE_IDLE`, so the next `recvRaw()` calls
`startReceive()` regardless and the write is harmless. On LR2021 the wrapper
keeps `STATE_RX` forever, so nothing calls `startReceive()` on its own. If the
PA config write knocks the receiver down, it stays down.

`setTxPower()` is the only setter in the wrapper that does not do this —
`idle()`, `resetAGC()` and `applySideDetectorConfig()` all set
`state = STATE_IDLE` to trigger a fresh `startReceive()`.

The stuck-radio check in `Dispatcher::loop()` cannot catch this either:
`isInRecvMode()` reports the wrapper's own `state` flag rather than the chip's
actual mode, so it still believes the radio is in Rx. And even when it does
trip, it only raises `ERR_EVENT_STARTRX_TIMEOUT` without attempting recovery.

### Fix

On LR2021, drop to standby via `idle()` before writing the PA config, and let
`checkRecv()` re-arm Rx — the same pattern the other setters already use.
Other radios are left untouched, since they re-arm on their own.

### Affected boards

`meshtracker_x1` and `meshnology_w12` (both build with `USE_LR2021`).

### Testing

Verified on hardware with an LR2021 repeater (Seeed XIAO nRF52840 with a NiceRF
LoRa2021F33-2G4 module, 869.618 MHz, SF7, BW 62.5):

- Before the fix the receiver occasionally stopped after a `set tx`, with no
  way back other than a reboot.
- After the fix, six consecutive power changes (15/18/21/14/19/14) left the
  radio fully functional: it kept receiving and forwarding mesh traffic
  (`rawrx`/`rxpkts` increasing, no missed IRQs). Noise-floor sampling also kept
  updating, which is an independent confirmation that Rx was re-armed, since
  that sampling only runs while `state == STATE_RX`.

`MeshTracker_X1_repeater` builds clean.

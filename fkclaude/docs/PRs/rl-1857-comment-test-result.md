# RadioLib issue 1857 — komentár po odskúšaní jeho opravy

**Stav: NA SCHVÁLENIE. Neposlané.**

Odmerané 20.–24. 8. 2026. Pravidlá: žiadna zmienka o AI, bez mriežkových čísel,
ich CONTRIBUTING žiada stručnosť.

Čísla v tele:
* regresia: 3505 : 3589 a 3533 rámcov proti SX1262 referencii za 10,6 h → 0,977 a 0,984
  (referencia na starom pine 1,013); žiadne timeouty ani zaseknutia za 21 h
* jeho oprava zavrela cestu, na ktorú mieri: 0 z 3024 proti 574 z 2380
* druhá cesta: kontrola bez nášho patchu 1247 z 1512 rámcov (82,5 %) za 3,4 h
* náš patch: 0 z 136 proti 26 z 32 na tej istej doske, 90 sekúnd medzi fázami
* koľko čítaní: zo 360 udalostí štvrté stačí v 96,9 %, piate pokryje ~99,7 %
* `GetStatus` naozaj maže ResetSource — overené na železe, preto sa opkód neopakuje

## Body

Tested, and I owe you numbers rather than impressions.

**No regression.** Against an SX1262 reference receiver hearing the same traffic over
10.6 hours of directly comparable measurement, the LR2021 board received 3505 frames to
the reference's 3589 and the LR11x0 board 3533 — ratios of 0.977 and 0.984, where the
LR11x0 board on the pinned commit gave 1.013 over 22.5 hours. All within 1.5 standard
errors, so no throughput cost is measurable. Across 21 hours: no
`RADIOLIB_ERR_SPI_CMD_TIMEOUT`, no BUSY timeout messages, no stalls, receive-error counts
and stack headroom unchanged. Nothing is discarded that was not being discarded before.

**And it closes the path it targets.** In one nine-hour run the LR2021 board had not a
single bad length read across 3024 frames, where the same board on the pinned commit had
574 out of 2380.

**A second path survives it, and I have now characterised it.** On a build with your
commit active, the board still reads a wrong length on part of the received frames. Over
3.4 hours that was 1247 of 1512 frames — 82.5%, steady across the whole run. On every one
of those reads the command status was `CMD_OK`, never `CMD_DAT`.

The wrong value is not arbitrary. It is `irq[31:16]`, the top half of the IRQ word that
the chip streams when no reply is pending, and it tracks the flags exactly:

| length read | bits | flags |
|---|---|---|
| 4 | 18 | `RX_DONE` |
| 5 | 16 + 18 | `ERROR` + `RX_DONE` |
| 68 | 18 + 22 | `RX_DONE` + `CRC_ERROR` |

It also is not specific to the length read. With the board in that state I read
`GetVersion` six times in a row from the CLI and got the status stream every time, never
the version — so a wrapper cannot guard against it, there is nothing to compare against.

**What fixes it.** Treat a read reply whose command status is not `CMD_DAT` as not ours
and fetch it again. The LR20xx datasheet defines `CMD_DAT` as "the latest command was a
successfully processed read, and data is currently transmitted instead of IrqStatus", and
the LR1121 manual says clocking NOPs returns "either the response to the last command, or
the status information if no response is pending", pointing at `stat1`/`CMD_DAT` to tell
the two apart. So the status is documented as the discriminator for exactly this case.

Measured on the same board, in the same episode, 90 seconds between the two phases, with
nothing else changed:

| build | frames | bad length reads |
|---|---|---|
| your commit + the retry below | 136 | **0** |
| your commit alone | 32 | **26** |

The control build reported its first bad read on frame #2, sixteen seconds after boot.

On how many attempts are needed: over 360 recorded events the fourth read succeeded in
96.9%, a fifth covered all but about 11, and a cap of eight was reached at most twice. So
five leaves roughly one attempt of headroom.

```c
int16_t LRxxxx::SPIcommand(uint16_t cmd, bool write, uint8_t* data, size_t len,
                           const uint8_t* out, size_t outLen) {
  int16_t state = RADIOLIB_ERR_UNKNOWN;
  if(!write) {
    // send the 16-bit command once - a prepared reply stays pending until clocked out
    state = this->mod->SPIwriteStream(cmd, out, outLen, true, false);
    RADIOLIB_ASSERT(state);

    for(uint8_t attempt = 0; attempt < RADIOLIB_LRXXXX_READ_ATTEMPTS; attempt++) {
      // wait for BUSY to go low   <-- your change, unchanged
      ...

      // read the result without command
      this->mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_CMD] = Module::BITS_0;
      state = this->mod->SPIreadStream(RADIOLIB_LRXXXX_CMD_NOP, data, len, true, false);
      this->mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_CMD] = Module::BITS_16;
      RADIOLIB_ASSERT(state);

      // the reply is only ours when the status says data is being sent
      if(replyStatusWas(RADIOLIB_LRXXXX_STAT_1_CMD_DAT)) { break; }
    }
  } else {
    ...
```

Only the NOP transaction is repeated, never the opcode, and that part matters: re-issuing
is not free of side effects. `GetStatus` clears the reset source — I confirmed that on the
board, a read reported `ResetSource = NRESET` and after one `GetStatus` it read `cleared`
— and `GetAndClearIrqStatus` clears the interrupt flags on every issue. The radio FIFO
read goes through `SPIreadStream` directly, so it never enters this loop.

The piece I left as `replyStatusWas()` is the one I would not decide for you: the status
byte is consumed inside `SPIreadStream`, and `SPIparseStatus` is shared with the write
path where `CMD_OK` is the correct answer. Whether that check belongs in `SPIparseStatus`
behind a flag, or inline here with the status width temporarily set to zero, is a call
about your own structure.

I have not suggested closing this, since the underlying reason the chip stops serving
replies is still open on my side — a real power-down of the module does not clear the
state, and it correlates with the chip being in Rx, but that is as far as I can take it.
Happy to run the same comparison again on both families if you change anything, including
a check that nothing starts getting dropped that was not being dropped before. And if you
would rather have this as a pull request than as a description, say the word.

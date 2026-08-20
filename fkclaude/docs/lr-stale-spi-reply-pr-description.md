# PR — zastaralá SPI odpoveď čítaná ako dĺžka paketu (LR2021 + LR1110)

**NEODOSLANÉ.** Vetva `fix/lr2021-lr1110-stale-spi-reply` (z `upstream/dev`), na
`origin` už je. Worktree `…/scratchpad/mc-pr-lr-stale` — po vybavení PR zmazať cez
`git worktree remove`.

Komentáre sú na vetve stripnuté na EN (`fkclaude/tools/strip_lang_comments.py
--keep en`) a počítadlo `_stale_pktlen_reads` je z upstream verzie vyhodené —
upstream ho nemá kto čítať, v našom forku ostáva a tlačí sa v `AALIVE` ako
`spifix=`.

Príkaz (telo = **len časť za posledným nadpisom Body**, nie tento súbor celý; pri
extrakcii zachovať prázdne riadky a hľadať nadpis cez `rindex`, nie `index` — inak
sa do tela dostane aj táto interná hlavička, presne to sa už raz stalo):

```bash
python -c "import io;p='fkclaude/docs/lr-stale-spi-reply-pr-description.md';s=io.open(p,encoding='utf-8').read();i=s.rindex(chr(35)*2+' Body')+7;io.open('body.md','w',encoding='utf-8',newline='').write(s[i:].lstrip(chr(10)))"

D:/FkDev/GHcli/bin/gh.exe pr create   --repo meshcore-dev/MeshCore   --base dev   --head fkallay1:fix/lr2021-lr1110-stale-spi-reply   --title "Fix packet loss when a stale SPI reply is read as the received length"   --body-file body.md
```

**Stav:** Testing sekcia je už podložená — chyba sa v reálnej RX ceste vynúti
(`fk inject 4`, viď `fcl_readme_nicerf_lora2021.md`) a 11 z 11 prípadov sa
zachránilo. Guard rozhoduje podľa `CMD_DAT`, nie podľa odtlačku IRQ; odtlačok sa
meraním ukázal ako falošne strieľajúci na hodnoverných 68 B rámcoch. LR1110 je
overený len buildom, čo je v texte priznané.

---

## Title

```
Fix packet loss when a stale SPI reply is read as the received length
```

## Body

**Type: bug**

### Symptom

On an LR2021 repeater, the raw receive log occasionally reported a length of 4 for
frames that were 38, 50, 126 or 133 bytes long - always exactly 4, regardless of the
real length. The frames were then rejected as corrupt and never reached the mesh
layer. A second repeater on the same channel, an SX1262 board, logged the same frames
with correct lengths at the same moment, so it was not an RF problem.

It came in bursts. When several repeaters re-flood the same packet at once and the
chip is busy, roughly a third of the frames were lost this way. In quiet periods it
did not happen for hours.

The loss is silent: `readData()` returns success, so it is not counted as a receive
error, only as a frame that was received and then thrown away.

### Cause

On the LR11x0 and LR2021 families a read command is two SPI transactions
(`LRxxxx::SPIcommand()`): send the opcode, then read the reply.
`Module::SPItransferStream()` waits `delayMicroseconds(1)` after the first
transaction and then polls BUSY for a low level - so if BUSY has not risen within
that microsecond, the wait is skipped and the reply is read before the chip has
prepared it.

The chip then sends its default `[stat 2B][irq 4B]` stream, and the length is parsed
straight out of that. The status byte is valid, so nothing reports an error.

On LR2021 the length is built from two bytes, so `getRxPktLength()` returns
`irq[31:16]`. With RX_DONE (bit 18) set that is exactly 4 - the observed value. With
TX_DONE or CRC_ERROR also set it becomes 12 or 68, which is plausible enough to pass
as a real length and produce a garbage packet instead of an obviously short one.

On LR11x0 the length is a single byte, so `getRxBufferStatus()` returns `irq[31:24]`,
and every RX-relevant flag of that family lives in the two low bytes - so the length
comes back as **0**. `recvRaw()` skips the packet on its `len > 0` test, and the frame
is dropped when Rx is re-armed. No log line, no error counter, nothing but a gap in
the `isr` versus `recv` counts.

`getIrqStatus()` cannot be caught by this race, because it *is* that default stream.
That also makes the top bytes of the IRQ word an exact fingerprint of a stale reply,
which is what the fix uses.

### Fix

The chip already says whether a reply is on its way: the command status field of stat1
is `CMD_DAT` ("successfully processed, data is being transmitted") for the read half of
a get, and `CMD_OK` ("nothing to collect") when the status stream comes back instead.
RadioLib consumes that byte internally and only rejects `CMD_FAIL` and `CMD_PERR`, so
the distinction is lost. Reading the length here directly keeps the status byte visible
- the same trick `LRxxxx::getIrqStatus` uses to read the IRQ word - and the length is
trusted only when the status says the reply is ours. The Rx FIFO is still intact at that
point, so re-reading recovers the frame; if it never settles, the plain library read is
used so this can never end up worse.

An earlier version of this compared the returned length against `irq[31:16]` instead,
which needs no status byte. That turned out to misfire: 68 is both a common frame length
here and what `irq[31:16]` reads when RX_DONE and CRC_ERROR are set together, so it
flagged genuine frames. The status reported `CMD_DAT` on exactly those, which is why it
decides now.

On LR11x0 the stale value is 0 and a zero-length frame does not occur, so the zero is
unambiguous and the guard there stays a re-read on `len == 0 && RX_DONE` - no status byte
needed. The existing `len == 0 && HEADER_ERR` handling is left untouched and still runs
if re-reading does not help.

Both overrides use only public API and protected members of the base class, so no
`RADIOLIB_GODMODE` is needed.

### What this does not cover

This guards the path through `recvRaw()`. `LR11x0::readData()` reads the length and
the buffer offset again through the two-argument
`getPacketLength(bool update, uint8_t* offset)`, which is not virtual and therefore
cannot be intercepted from a subclass. If the race is lost there, the payload comes
out empty, or shifted by a bogus offset - which may well be the "packets shifted"
symptom the existing comment in `CustomLR1110::getPacketLength()` refers to.

The same race also affects every other read on these chips - `getRSSI()`, `getSNR()`,
`getRssiInst()`, `getVbat()`, `getTemp()` - so a wrong RSSI or SNR can reach
`packetScore()` without any indication. Those cannot be guarded from here either,
because a wrong RSSI has no fingerprint to test against. A proper fix belongs in the
driver's transport layer.

### Affected boards

LR2021: `meshtracker_x1`, `meshnology_w12`.

LR11x0: `t1000-e`, `wio_wm1110`, `thinknode_m3`, `thinknode_m7`, `thinknode_m9`,
`minewsemi_me25ls01`.

### Testing

LR2021 on hardware: Seeed XIAO nRF52840 with a NiceRF LoRa2021 module, 869.618 MHz,
SF7, BW 62.5, in a live mesh with several repeaters, against a second receiver (SX1262)
on the same channel as a reference.

The failure is a timing race, so rather than wait for it, it was forced: a debug build
skips the BUSY wait on every fourth length read, in the real receive path.

- 11 of 11 sabotaged reads returned exactly 4 - the original symptom - and every one of
  them reported `CMD_OK`.
- The re-read returned the true length every time (63, 65, 67, 73, ...).
- Over the same window the node logged the same frames as the reference receiver, with
  no bogus lengths and no receive errors, although a quarter of its length reads had
  been broken on purpose.
- On genuine frames whose length happened to equal `irq[31:16]`, the status reported
  `CMD_DAT` and the guard correctly stayed out of the way.

LR11x0 is not verified on hardware - the mechanism is derived from the driver code and
the affected code path is shared with LR2021. Anyone with an LR11x0 board can check it
cheaply: a length of 0 while RX_DONE is set is the signature, and reading the length
again immediately afterwards returns the real value.

Builds clean: `t1000e_repeater`, `wio_wm1110_repeater`, `MeshTracker_X1_repeater`.

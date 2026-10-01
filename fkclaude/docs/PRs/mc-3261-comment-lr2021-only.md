# PR 3261 — rebase na dev + vypustenie LR11x0 polovice

**NEODOSLANÉ — čaká na Fedorovo schválenie** (2026-10-02).

Stav pred odoslaním:

* PR bol voči `dev` v konflikte (`CustomLR1110.h`, upstream tam pridal `begin()` s DC/DC).
* LR11x0 polovica PR číta **zlý bajt** (`buff[2]` = offset namiesto `buff[1]` = dĺžka,
  LR11x0 má 1 status bajt, nie 2) — pamäť `lr1110_pktlen_wrong_byte`. Merania T1000-E v
  tele PR („4 of 4 … 84, 20 and 196 byte frames", „25 times in three minutes") vznikli
  cez to chybné čítanie, sú teda neplatné.
* `LR11x0::getPacketLength()` volá `getPacketType()` **pred** čítaním dĺžky, takže
  knižničná cesta je na LR11x0 náhodou chránená. Najčistejšie = LR11x0 časť vypustiť.
* jgromes 23. 9. zavrel RadioLib issue 1857 aj PR 1858 („my money is on a hardware
  issue"). oltacov argument „počkať na RadioLib" teda padá.

Pripravené lokálne (NEpushnuté): vetva `fix/lr2021-lr1110-stale-spi-reply` = `899c60cd`
na `dev` `fad0ffb7`, len `CustomLR2021.h` (+48). Build `MeshTracker_X1_repeater` +
`meshnology_w12_repeater` OK. Kombinácia s #3512 a NiceRF vetvou sa aplikuje čisto a
preloží (worktree `D:\FkDev\wt\combo`).

Poradie krokov po schválení:

1. `git push --force-with-lease origin fix/lr2021-lr1110-stale-spi-reply`
2. zmeniť titulok a telo (`gh pr edit 3261 --title ... --body-file body.md`)
3. o 2 min neskôr komentár nižšie

## Title

Fix LR2021 packet loss when a stale SPI reply is read as the length

## Comment

Rebased onto current dev and narrowed to LR2021 only.

The LR11x0 half had a bug of its own. LR11x0 uses a one byte status, not two like LR2021, so the reply to GetRxBufferStatus is status, length, offset, and my read took the offset as the length. The T1000-E numbers I posted earlier came through that read, so please disregard them. With the correct byte the library path turns out to be covered already, because LR11x0::getPacketLength() calls getPacketType() before it reads the length, which keeps the race from reaching it. So there is nothing left to fix on LR11x0 here and CustomLR1110.h is back to what dev has.

On waiting for RadioLib: the issue and the candidate fix there were closed on 23 September without a change to the driver, so on LR2021 this guard is the only thing that keeps those frames for now.

This sits fine next to #3512. That PR drops a frame when the FIFO level does not match the length, this one makes sure the length is the real one in the first place, so with both a stale length read is recovered instead of dropped. They touch different methods and apply together cleanly.

## Body

On an LR2021 repeater the raw receive log occasionally reported a length of 4 for frames that were 38, 50, 126 or 133 bytes long, always exactly 4. The frames were then rejected as corrupt and never reached the mesh layer. A second receiver (SX1262) on the same channel logged the same frames with correct lengths at the same moment, so it was not RF. It came in bursts when several repeaters re-flooded at once; in quiet periods it did not happen for hours. The loss is silent, because readData() returns success.

A read command on this chip is two SPI transactions, the opcode and then the reply. SPItransferStream() waits 1 us after the first one before it polls BUSY, so if BUSY has not risen by then the wait is skipped and the reply is read before the chip has prepared it. The chip then sends its default status and IRQ stream, and getRxPktLength() returns irq[31:16], which is exactly 4 with RX_DONE set. With CRC_ERROR also set it becomes 68, which passes as a plausible length.

The chip does say whether a reply is on its way. The command status field of stat1 is CMD_DAT for the read half of a get and CMD_OK when the status stream comes back instead. RadioLib consumes that byte and only rejects CMD_FAIL and CMD_PERR. This change reads the length directly with the status width set to 0, the same trick LRxxxx::getIrqStatus uses, and trusts it only when the status is CMD_DAT. The Rx FIFO is still intact at that point, so a re-read recovers the frame. If it never settles, the plain library read is used, so it cannot end up worse than today. No RADIOLIB_GODMODE needed.

An earlier version compared the length against irq[31:16] instead. That misfired, because 68 is both a common frame length and what those bytes read with RX_DONE and CRC_ERROR set, so the status decides now.

Tested on a XIAO nRF52840 with a NiceRF LoRa2021 module, 869.618 MHz, SF7, BW 62.5, in a live mesh next to an SX1262 reference receiver. Since it is a timing race, it was forced: a debug build skipped the BUSY wait on every fourth length read in the real receive path. 11 of 11 sabotaged reads returned 4 and reported CMD_OK, the re-read returned the true length each time, and the node logged the same frames as the reference. Genuine frames whose length happened to equal irq[31:16] reported CMD_DAT and were left alone.

This only guards the length read in recvRaw(). The same race can hit other reads such as RSSI and SNR, which have no fingerprint to test against; that belongs in the driver.

Affected boards: meshtracker_x1, meshnology_w12. Builds clean: MeshTracker_X1_repeater, meshnology_w12_repeater.

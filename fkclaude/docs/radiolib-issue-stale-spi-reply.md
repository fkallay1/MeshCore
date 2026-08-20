# RadioLib issue — zastaralá odpoveď na „get" príkaz

**Interné, NEKOPÍROVAŤ do issue.** Cieľ: repozitár `jgromes/RadioLib`, šablóna
*Bug report*. Pod čiarou `---- TELO ----` je presne to, čo ide do issue.

Pravidlá (viď pamäť o upstream PR):
- Žiadna zmienka o AI, žiadny odkaz na nás, **žiadne URL** a **žiadne `#číslo`** —
  mriežka aj plná URL v tele issue vytvárajú krížové odkazy.
- Text má byť krátky; ich CONTRIBUTING sekcia „Less is more" na dlhé steny reaguje
  podráždene. Toto je už zostrihané na minimum, ktoré ešte nesie dôkaz.
- Ich CONTRIBUTING má aj sekciu „AI Use", ktorá žiada označenie AI-asistovaných
  príspevkov. Fedor rozhodol, že zmienka tam nebude.

Náš guard v MeshCore: `src/helpers/radiolib/CustomLR2021.h`,
`src/helpers/radiolib/CustomLR1110.h`. Vetva PR: `fix/lr2021-lr1110-stale-spi-reply`.

Titulok:

    [LR11x0][LR2021] Reply to a get command is sometimes read before the chip has it ready

---- TELO ----

**Describe the bug**

On the LR11x0 and LR2021 families a read command is two SPI transactions: send the
opcode, then read the reply. When the second transaction happens before the chip has
the reply ready, the chip sends its default `[stat 2B][irq 4B]` stream instead, and
`LRxxxx::SPIcommand()` returns those bytes as if they were the reply. The status byte
itself is valid, so `SPIparseStatus()` reports success and the caller has no way to
tell that the payload is not an answer to its command.

The received packet length is where this hurts most.

`LR2021::getRxPktLength()` builds a 16-bit value from the first two payload bytes, so
it returns `irq[31:16]`. With RX_DONE (bit 18) set that is exactly 4. With TX_DONE or
CRC_ERROR also set it becomes 12 or 68 - plausible enough to pass as a real length.

`LR11x0::getRxBufferStatus()` takes the length from `buff[0]` and the buffer offset
from `buff[1]`, so it returns `irq[31:24]` and `irq[23:16]`. All RX-relevant flags of
this family live in the two low bytes, so the length comes back as 0; and if
CMD_ERROR (bit 22) is set, the offset comes back as 0x40, which shifts the payload.

`readData()` then reads that many bytes and clears the Rx buffer, so the packet is
destroyed. Nothing reports an error, because the read itself succeeded.

Measured on an LR2021 at SF7 in a live LoRa mesh: frames of 50 and 133 bytes were
reported as `len=4`. In dense bursts, where several nodes answer at once and the chip
is busy, this hit roughly a third of the frames. A second receiver on the same channel
logged the same frames with correct lengths.

Every other `get` is exposed the same way - `getRSSI()`, `getSNR()`, `getRssiInst()`,
`getVbat()`, `getTemp()` - so a caller can also get a wrong RSSI or SNR without any
indication. `getIrqStatus()` is the one exception, because it IS that default stream;
that is also what makes the mechanism easy to confirm, since the bogus length is
always the top bytes of the IRQ word.

As for why the reply is read early: `Module::SPItransferStream()` does
`delayMicroseconds(1)` after the first transaction and then polls BUSY for a low
level. If BUSY has not risen within that microsecond, the loop exits immediately and
the read follows.

**To Reproduce**

The race itself is timing dependent, but it can be provoked deterministically by doing
what the driver does and skipping the BUSY wait on purpose. Setting the status width to
0 keeps both status bytes visible, the same way `LRxxxx::getIrqStatus()` reads the
default stream:

```c++
// two transactions, BUSY wait deliberately skipped
mod->SPIwriteStream(RADIOLIB_LR2021_CMD_GET_RX_PKT_LENGTH, NULL, 0, false, false);
mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_STATUS] = Module::BITS_0;
mod->spiConfig.widths[RADIOLIB_MODULE_SPI_WIDTH_CMD]    = Module::BITS_0;
uint8_t buff[4] = { 0 };
mod->SPIreadStream(RADIOLIB_LRXXXX_CMD_NOP, buff, sizeof(buff), false, false);
// buff[0..1] = status word, buff[2..3] = the reply - or the top half of the IRQ word
```

Measured on the board, eight reads with the wait skipped followed by one proper read,
repeated four times. `cmd` is the command status field of stat1, bits 3:1:

```
nowait  stat=04  cmd=2 (CMD_OK)   val=0    <- top half of the IRQ word, not the length
nowait  stat=04  cmd=2 (CMD_OK)   val=0
...   32 reads, all identical
wait    stat=06  cmd=3 (CMD_DAT)  val=50   <- the real length of the last packet
```

So 32 out of 32 early reads returned the status stream, and every one of them reported
`CMD_OK`; the properly waited reads reported `CMD_DAT` and the correct length. The
fingerprint is 0 in this bench test because the IRQ word had already been cleared by
the preceding `readData()`. In the live failure the RX_DONE flag is still pending, which
is where the value 4 comes from.

Reading the length again after a stale reply returns the correct value, which also
confirms that the Rx buffer still holds the packet at that point.

**Expected behavior**

A read command either returns the reply to the command that was sent, or fails with a
status code. It should not be possible to receive the default status stream in place of
the requested data without any indication.

**Possible directions**

The chip already reports whether a reply is on its way, and the measurement above shows
it discriminates cleanly. The command status field in stat1 has four values, and
`LRxxxx::SPIparseStatus()` currently only rejects two of them:

```c++
if((in & 0b00001110) == RADIOLIB_LRXXXX_STAT_1_CMD_PERR) { ... }
else if((in & 0b00001110) == RADIOLIB_LRXXXX_STAT_1_CMD_FAIL) { ... }
```

`CMD_OK` ("successfully processed") and `CMD_DAT` ("successfully processed, data is
being transmitted") are both accepted as success. On the read transaction of a get
command, `CMD_DAT` is the only correct one - and a stale reply reported `CMD_OK` in all
32 measured cases. Checking for it would catch this without any timing change, and
would cover every get command rather than just the length.

The callback only receives the status byte, so it cannot tell a read from a write on
its own; it would need either a flag in the SPI config saying a data reply is expected,
or a separate check in the read branch of `LRxxxx::SPIcommand()`.

An alternative, or an addition, is the handshake itself: wait for BUSY to actually rise
before waiting for it to fall, bounded by a short timeout for the case where the command
has already completed by the time sampling starts.

Either way it would be good if the failure were visible to the caller, rather than
arriving as data that looks legitimate.

And if touching the transport is not wanted at all, the length read alone can be made
safe from the outside, because that one has a fingerprint to test against: compare the
value returned against the top bytes of the IRQ word and, when they match, read it
again. That is what the application this was found in does now, and it recovers the
frames. It is a workaround rather than a fix - it costs an extra status read per packet
and it does nothing for the other get commands, which have no fingerprint - but it is
cheap and it needs no driver change.

**Additional info**

 - MCU: nRF52840
 - Arduino core: Adafruit nRF52 Arduino core
 - Wireless module type: LR2021 (LR11x0 affected by the same code path)
 - Build environment: PlatformIO
 - Library version: 7.7.1, git 6d8934836678d8894e3d556550475b37dce3e2b6

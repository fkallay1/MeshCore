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

It is a timing race, so a plain example sketch does not show it reliably - it needs the
chip to be busy, in practice a second node transmitting back to back. What does make it
visible without any special hardware is instrumenting the length read itself, since a
stale reply has an exact fingerprint:

```c++
// LR2021: a length equal to the top half of the IRQ word is a stale reply
uint16_t fingerprint = (uint16_t)(radio.getIrqStatus() >> 16);
size_t len = radio.getPacketLength();
if (len == fingerprint && fingerprint != 0) {
  Serial.print(F("stale reply, len="));
  Serial.println(len);          // 4 when only RX_DONE is set
}
```

Reading the length again at that point returns the correct value, which confirms both
that the Rx buffer still holds the packet and that the first read was simply too early.

**Expected behavior**

A read command either returns the reply to the command that was sent, or fails with a
status code. It should not be possible to receive the default status stream in place of
the requested data without any indication.

**Possible directions**

Rather than guess at the right fix, a few options that seem open, in case it helps the
discussion: wait for BUSY to actually rise before waiting for it to fall, with a bounded
timeout for the case where the command has already completed; or make the reply read
verifiable, so a caller can tell a real answer from the status stream; or, at minimum,
document that these reads can return stale data so applications can guard the ones that
matter.

**Additional info**

 - MCU: nRF52840
 - Arduino core: Adafruit nRF52 Arduino core
 - Wireless module type: LR2021 (LR11x0 affected by the same code path)
 - Build environment: PlatformIO
 - Library version: 7.7.1, git 6d8934836678d8894e3d556550475b37dce3e2b6

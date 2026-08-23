> **ZABLOKOVANE — NEPOSIELAT (24. 8. 00:20)**
>
> Meranie na zivej epizode s ostrym guardom vyvratilo dve tvrdenia v tele:
>
> 1. **„every natural occurrence needed three reads, the third succeeds" je artefakt**
>    diagnostickej cesty. `fkDiagPktLen` volal `getIrqStatus()` **pred kazdym** citanim
>    dlzky, takze medzi opakovaniami bol vzdy iny prikaz. Ostry `fkGuardPktLen` cita
>    trikrat za sebou bez niceho medzitym.
> 2. **Opakovanie toho isteho opkodu za sebou pomohlo len 1 z 13 udalosti.** Zvysnych 12
>    zachranil az fallback `LR2021::getPacketLength()`, ktory ma v sebe `getPacketType()`
>    pred citanim dlzky — teda **iny prikaz medzi pokusmi**. Status hlasil `CMD_OK`
>    (`stat=0x5`, cmd=2) pri vsetkych 13 prvych citaniach.
>
> Dosledok: navrhovany patch (opakuj to iste citanie, breakni na CMD_DAT) by opravil
> ~8 % tychto pripadov, nie vsetky. Telo treba prepisat az po tom, ako otestujeme
> variantu s vlozenym prikazom medzi pokusmi.

# Výsledok testu jgromesovej vetvy — komentár do RadioLib 1857

**NEODOSLANÉ.** Čaká na Fedorovo schválenie.

Zadanie od Fedora (23. 8.): **žiadne dohady o HW.** Napísať, že jeho oprava je správna
a že nič nezahadzuje; že náš problém úplne nevyriešila a testujeme ďalej; a že stav
nezhoršila — porovnávali sme tri dosky.

Odmerané, čo je v tele:
* regresia: XIAO(ISO):ProMicro = 0,995 (0,7 σ), T1000-E(ISO):ProMicro = 0,990 (0,9 σ),
  referencia na starom pine 1,013; nula timeoutov na oboch LR doskách
* ISO pri nf -117: 0 udalostí / 3024 rámcov za 8,8 h
* ISO pri nf -113: 74,8 % rámcov s dĺžkou 4 (1044 z 1395) — jeho oprava to nezastavila
* injekcia na builde s jeho fixom: prvé čítanie 4, opakované vytiahlo 80/34/36, status
  hlásil CMD_OK — mechanizmus potvrdený

Pravidlá: bez zmienky o AI, bez mriežkových čísel, ich CONTRIBUTING žiada krátkosť.

## Body

Sorry this took a while — I wanted numbers rather than impressions. About 21 hours of runtime on an LR11x0 board and 15 on an LR2021 one, 9179 and 5742 frames received. The build is your commit cherry-picked onto the commit we pin, so those ten lines are the only difference from what it is compared against.

**No regression.** Over 10.6 hours of directly comparable measurement, against an SX1262 reference receiver hearing the same traffic, the LR2021 board received 3505 frames to the reference's 3589 and the LR11x0 board 3533 — ratios of 0.977 and 0.984, where the LR11x0 board on the pinned commit gave 1.013 over 22.5 hours. All within 1.5 standard errors, so no throughput cost is measurable. Across the whole 21 hours: no `RADIOLIB_ERR_SPI_CMD_TIMEOUT`, no BUSY timeout messages, no stalls, no lost frames, receive-error counts and stack headroom unchanged.

**And it closes the path it targets.** In one nine-hour run the LR2021 board had not a single bad length read across 3024 frames, where the same board on the pinned commit had 574 out of 2380.

**There is a second path to the same wrong value, though, and I have only managed to characterise it today.** On the same build, with your commit active, the board still reads a length of 4 on part of the received frames under conditions I cannot yet reproduce on demand. What I can now say precisely is how those reads look, because I put a switchable re-read in our own wrapper and cycled it while everything else stayed fixed:

```
re-read off   104 frames    12 bad   11.5 %
re-read on    269 frames     1 bad    0.4 %
re-read off   416 frames   303 bad   72.8 %
re-read on     24 frames     0 bad    0.0 %
re-read off    91 frames    73 bad   80.2 %
re-read on     39 frames     0 bad    0.0 %
```

On every one of those reads the status byte was `CMD_OK`, not `CMD_DAT`, and re-reading returned the true length — between 29 and 147 bytes on the samples I captured. So the reply is identifiable as not-ours from the status alone, and the data is still there to be fetched.

That suggests something you could do inside the driver and that would cover far more than the length read: on a read command, where data is expected, treat a reply whose command status is not `CMD_DAT` as invalid and read again. `SPIparseStatus` currently rejects only `CMD_FAIL` and `CMD_PERR`, so `CMD_OK` passes through. Done there it would also protect `getRSSI()`, `getSNR()` and the rest, which cannot be guarded from outside the driver at all — a wrong RSSI has no signature to test against.

One number worth having if you try it: every single natural occurrence I captured needed **three** reads, never two — the first returns 4, the second still fails, the third succeeds. Forced failures recover on the second, so the real thing is more persistent than a single mistimed read. That count was measured with the opcode re-issued each time, which is what our wrapper did; I am repeating it now with only the reply re-clocked, and will say if the number changes. Three would therefore be the bare minimum here and I would leave more headroom, say five; one of our re-read runs still leaked a single frame, which is what running at exactly the limit looks like.

If you do implement something along those lines, I am happy to run the same comparison again on both families — including a check that nothing starts getting dropped that was not being dropped before.

Sketching what that could look like on top of your branch, in `LRxxxx::SPIcommand()`:

```c
if(!write) {
  // send the 16-bit command once; the prepared reply stays pending until clocked out
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

    // a read reply is only ours when the command status says data is being sent;
    // CMD_OK means the command was accepted but this is not the reply we asked for
    if(replyStatusWas(RADIOLIB_LRXXXX_STAT_1_CMD_DAT)) { break; }
  }
}
```

Only the NOP transaction is repeated, not the opcode, and that part matters: re-issuing
the opcode is not free of side effects. `GetStatus` clears the reset source and
`GetAndClearIrqStatus` clears the interrupt flags on every issue, so a retry there would
discard state. The LR2021 datasheet says a pending read result stays retrievable in a
later frame — section 6.7.1 notes that a `GetStatus` issued after a read returns that
read's result — so re-clocking the reply is enough. The radio FIFO read bypasses
`SPIcommand()` entirely, so it never enters this loop.

The part I have deliberately left as `replyStatusWas()` is the one I would not want to decide for you: the status byte is consumed inside `SPIreadStream` and `SPIparseStatus` is shared with the write path, where `CMD_OK` is the correct answer — so whether that check belongs in `SPIparseStatus` behind a flag, or inline here with the status width temporarily set to zero, is a call about your own structure. In our wrapper we did it inline, but we only had one command to worry about.

On whether the check is safe to make: the LR20xx datasheet defines `CMD_DAT` as "the latest command was a successfully processed read, and data is currently transmitted instead of IrqStatus", and the LR1121 manual says that clocking NOPs returns "either the response to the last command, or the status information if no response is pending", pointing the reader at `stat1`/`CMD_DAT` to tell the two apart. So the status is documented as the discriminator for exactly this, which is what makes it look worth using rather than a workaround. The one case it cannot cover is the single-frame FIFO read, where the status precedes the opcode — but that path does not go through `SPIcommand()` anyway.

I have not suggested closing this, since the symptom in the title still reaches us by that second route — whether it stays here or moves to a fresh issue is your call. And if you would rather I put the change together as a pull request instead of describing it, say the word and I will.

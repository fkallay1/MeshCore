# RadioLib issue 1857 — komentár po desiatich dňoch na železe

**ODOSLANÉ 1. 9. 2026:** https://github.com/jgromes/RadioLib/issues/1857#issuecomment-5500032693

Nová verzia oproti `rl-1857-comment-test-result.md` (ten ostáva nedotknutý na porovnanie).

Čo je v tele a čo sa oproti starému draftu zmenilo:

* **pridané: odvolanie našich LR11x0 čísel.** Komentár z 20. 8. („4 of 4 … recovering 84,
  20 and 196 byte frames") vznikol cez naše chybné čítanie — `CustomLR1110` indexovala
  odpoveď ako LR2021, teda s dvoma status bajtmi, hoci LR11x0 má jeden. Tie čísla teda
  neplatia a nechať ich tam nespravené by bolo horšie než ich odvolať.
* **pridané: priznanie, že nevieme oddeliť jeho opravu od našej.** Bežíme na oboch
  naraz, takže „zlé dĺžky zmizli" nedokazuje, ktorá z nich to spravila. Počítadlo
  `lrxxxx_stale_reads` je odvtedy zapnuté a číslo dodáme.
* **ostáva: žiadna regresia**, teraz s dlhším behom (62 h v jednom kuse, 35 078 rámcov).
* **ostáva: dôkaz, že CMD_DAT rozlišuje** — 8 z 8 pretečených odpovedí malo `cmd≠3`.
* **ostáva: výhrada k našej doske** — epizódy mali aj fyzickú zložku, takže naše absolútne
  miery nie sú typické pre ten súčiastku.
* **ponechané, ale skrátené: „How I was testing".** Nadpis hovorí, že je to na
  preskočenie, takže dĺžka nikomu neprekáža — a práve pri odvolávaní čísel má zmysel
  ukázať, ako sme merali tie ostatné. Vypustený je z nej **zoznam vylúčených príčin**:
  napájanie aj anténny konektor sa neskôr ukázali ako veci, ktoré chybovosť menili, takže
  tvrdiť, že sú vylúčené, by bolo v rozpore s tým, čo dnes vieme.
* **vypustené:** tabuľky s hodinami SPI a pauzami. Sú už v skorších komentároch;
  opakovať ich by rozmazalo to podstatné.

Pravidlá: žiadna zmienka o AI, žiadne mriežkové čísla.

---

## Telo

Ten days of running your fix, and one correction I owe you.

**No regression.** The LR2021 board has been on your commit continuously since it landed.
The longest single uninterrupted run was 62 hours: 35 078 frames received, 7 586
transmitted, no `RADIOLIB_ERR_SPI_CMD_TIMEOUT`, no BUSY timeout messages, no stalls. Two
other boards — an SX1262 and an LR11x0 — sat alongside it hearing the same traffic as
references and stayed within a few percent of it throughout. Nothing is being discarded
that was not being discarded before.

**A correction to my earlier LR11x0 numbers — please disregard them.** In my second
comment I reported "4 of 4 sabotaged reads came back as a length of 0 … recovering 84, 20
and 196 byte frames" on a T1000-E. That measurement went through my own debug read, and
that read was wrong: it indexed the reply as if LR11x0 returned two status bytes like
LR2021, when it returns one. So it was reporting the buffer start offset where I thought I
was reading the payload length. The giveaway was that 100 % of the lengths it produced were
divisible by four — FIFO offsets are aligned, payload lengths are not.

I found this the day after posting and fixed it on my side, but the numbers are still
standing in the issue, so: the LR11x0 half of that comment is not evidence of anything. I
have not re-measured that board since. Everything in the same comment that came from the
LR2021 board — the BUSY duration histogram over 40 runs in particular — was taken through
a different path and is unaffected.

**What I cannot yet tell you, and want to be straight about.** I have been running your
fix *together with* the `CMD_DAT` retry I proposed earlier, so I cannot currently say
whether your fix alone is sufficient. The bad length reads are gone, but either change
explains that. I have now enabled the retry counter so I can report how often it still
fires on top of your BUSY wait — that is the number that decides whether anything further
is warranted, and I will post it once it has run long enough to mean something. If it
stays at zero, that is the useful answer and I will say so.

**One measurement that stands on its own.** With the BUSY wait deliberately skipped, eight
consecutive length reads all returned the stale value, and every one of them carried a
command status of `CMD_OK` rather than `CMD_DAT`. The same read with the wait in place
returned `CMD_DAT` and the correct length:

```
nowait 0..7     stat=05  cmd=2  val=69     <- stale, irq[31:16]
wait            stat=07  cmd=3  val=40     <- correct
```

Eight out of eight. Whatever ends up being enough as a fix, that bit gives a cheap way to
*detect* the condition rather than only avoid it — which matters here because the failure
is otherwise silent: a wrong length is a valid value, `readData()` succeeds, and no error
counter anywhere moves.

**A caveat about my board.** The episodes on this unit turned out to have a physical
component as well. I could provoke the fault on demand by disconnecting the BUSY line, and
the rate changed after reseating connectors — at one point three quarters of the traffic
was being lost, and at other times the same board ran for hours clean. So please do not
read my absolute failure rates as representative of the part. The mechanism is real and
reproducible; the frequencies are mine.

### How I was testing, if you want to read this long story

Two weeks, three boards side by side — the LR2021 one under test, an LR11x0 one on the
same firmware, and an SX1262 one as a reference receiver, all hearing the same live mesh.
Most of that time I was measuring the wrong thing, and the way it went wrong is worth
passing on because anyone trying to reproduce this will hit it too.

The main mistake was mine and it is the one I would warn you about: **I twice concluded I
had found the fix because a phase came back clean, and both times it was only that the
fault takes a while to reappear after any intervention.** A plain reboot buys two clean
frames. A real power-down of the module buys about twenty. Later I watched the same board
run for nine hours with the fault present and then go quiet for a day with nothing
changed. Nothing was fixed in any of those cases; the onset was delayed, or the episode
simply ended.

So a clean window proves nothing here, however long it is. After that I stopped trusting
any measurement that was not bracketed by a control on either side, and every number I
have quoted in this issue is A/B/A for that reason.

The other thing that helped was making everything switchable at runtime instead of
rebuilding for each idea — the guard, the retry count, the gap between attempts, the SPI
clock, the delay after the opcode. That let a whole sweep run inside a single episode of a
fault that comes and goes on its own. Before that, each comparison spanned different
episodes and the numbers moved for reasons that had nothing to do with what I was testing.

I will not repeat the list of causes I ruled out early, because two of them did not stay
ruled out: the board's antenna connector and its supply both turned out to influence the
rate later on. That is part of why I am flagging my absolute frequencies as mine rather
than yours.

The BUSY probe was the last thing I tried and the one I should have tried first.

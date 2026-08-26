# RadioLib issue 1857 — komentár po odskúšaní jeho opravy

**Stav: NA SCHVÁLENIE. Neposlané.**

Merané 20.–26. 8. 2026, tri dosky, 149 065 prijatých rámcov, 8 buildov na XIAO.
Pravidlá: žiadna zmienka o AI, bez mriežkových čísel.

Čo je v tele:
* regresia: 0,977 a 0,984 proti SX1262 referencii, kontrola 1,013; 21 h bez timeoutov
* BUSY sonda: prvé tri pokusy `hi=0`, ďalšie `hi≈15` — čip BUSY vôbec nezdvihne
* hodiny SPI: 250k/500k = 0 %, 1 MHz = 33 %, 2 MHz = 78–89 % (tri kontroly)
* pauza po opkóde pri 2 MHz: 0 µs = 70 a 83,5 %, 8–48 µs = **0 % z 532 rámcov**
* náš CMD_DAT patch: 0 zo 136 proti 26 z 32, tá istá doska, 90 s medzi fázami
* na konci „how I was testing" — Fedor si vyžiadal aj popis slepých uličiek

## Body

Tested. It took longer than I hoped, but it ended somewhere useful: I think I can now
tell you what the chip actually does, not just that something goes wrong.

**No regression from your commit.** Against an SX1262 reference receiver hearing the same
traffic over 10.6 hours, the LR2021 board received 3505 frames to the reference's 3589 and
the LR11x0 board 3533 — ratios of 0.977 and 0.984, where the LR11x0 board on the pinned
commit gave 1.013 over 22.5 hours. All within 1.5 standard errors. Across 21 hours: no
`RADIOLIB_ERR_SPI_CMD_TIMEOUT`, no BUSY timeout messages, no stalls. Nothing is discarded
that was not being discarded before.

**And it closes the path it targets** — in one nine-hour run the LR2021 board had not a
single bad length read across 3024 frames, where the same board on the pinned commit had
574 out of 2380.

### What was still getting through

A second path survives it: the board reads `irq[31:16]` as the packet length on most
frames. The value is never random — it tracks the flags exactly, 4 for `RX_DONE`, 5 for
`ERROR + RX_DONE`, 68 for `RX_DONE + CRC_ERROR`. Over 18814 recorded reads the command
status was `CMD_OK` every single time, and `CMD_PERR` never once, so the opcode always
arrives intact and the chip always processes it — it simply has no reply ready.

### The cause, as far as I can measure it

I sent the read opcode with the BUSY wait skipped and then sampled the pin in a tight
loop, 400 samples, eight times in a row:

```
busy run 0  hi=0   fall=0      <- BUSY never seen high
busy run 1  hi=0   fall=0
busy run 2  hi=0   fall=0
busy run 3  hi=16  fall=17     <- high for ~15 samples, then falls
busy run 4  hi=15  fall=16
busy run 5  hi=15  fall=16
busy run 6  hi=15  fall=16
busy run 7  hi=15  fall=16
```

Four invocations in a row, the same pattern each time. So the line is not too fast to
catch — when it works it stays high for fifteen samples out of four hundred. **In stretches
of consecutive transactions the chip does not assert BUSY at all**, and a host that waits
for BUSY to go low therefore returns immediately and clocks the reply out before there is
one. How long those stretches are varies: in the run above it was the first three probes
every time, while in a later session all eight probes saw the line and a run of five blind
ones turned up only afterwards. I would not read a fixed number into it.

The line itself is clean. Sampling BUSY while nothing at all is being sent, with the main
loop stopped so no other transaction can raise it, gave 882841 samples across three windows
with zero highs and zero edges — so this is the chip declining to assert the line, not
interference making us misread it.

That also explains the count I kept seeing elsewhere: reads one, two and three come back
as status, the fourth is correct. Same three.

### Two measurements that follow from it

**SPI clock.** Slowing the clock lengthens the opcode frame and gives the chip more time.
At the stock 2 MHz the opcode takes 8 us; three interleaved control phases at 2 MHz gave
78.4%, 78.6% and 89.0% bad reads, so the episode was live throughout:

| SPI clock | opcode frame | frames | bad |
|---|---|---|---|
| 250 kHz | 64 us | 101 | **0.0%** |
| 500 kHz | 32 us | 108 | **0.0%** |
| 1 MHz | 16 us | 105 | 33.3% |
| 2 MHz | 8 us | 305 | 78.4 / 78.6 / 89.0% |

**A delay between the opcode and the reply frame**, at the stock 2 MHz, with the read
issued exactly once so nothing masks the failure:

| delay | frames | first read failed |
|---|---|---|
| 0 us | 110 | 70.0% |
| **8 us** | 108 | **0.0%** |
| 16 us | 116 | **0.0%** |
| 24 us | 102 | **0.0%** |
| 32 us | 103 | **0.0%** |
| 48 us | 103 | **0.0%** |
| 0 us (control) | 103 | 83.5% |

532 frames with a delay, not one bad read, bracketed by two controls at zero.

### What I would suggest

Wait for BUSY to go **high** after the read opcode, with a short timeout, before the
existing wait for it to go low. When the chip raises the line you catch it and wait
exactly as long as needed; when it does not raise it at all, the timeout expires and that
expiry is itself the delay that fixes the read. Both cases end up correct, and nothing is
slowed down that does not need to be.

A plain fixed delay works too — 8 us was already enough here — but it pays the cost on
every read, and I would not want to guess a number that holds for every board.

### A safety net, if you want one as well

Independently of the above, a reply whose command status is not `CMD_DAT` is not the reply
that was asked for. The LR20xx datasheet defines `CMD_DAT` as "the latest command was a
successfully processed read, and data is currently transmitted instead of IrqStatus", and
the LR1121 manual says clocking NOPs returns "either the response to the last command, or
the status information if no response is pending", pointing at `stat1`/`CMD_DAT` to tell
them apart. Repeating just the NOP transaction on a non-`CMD_DAT` reply, up to five times,
measured on the same board in the same episode with 90 seconds between the two phases:

| build | frames | bad length reads |
|---|---|---|
| your commit + that retry | 136 | **0** |
| your commit alone | 32 | **26** |

The control build reported its first bad read on frame #2, sixteen seconds after boot, and
over 3.4 hours it ran at 82.5% of 1512 frames.

Only the NOP transaction should be repeated, never the opcode: `GetStatus` clears the
reset source — I watched a read report `ResetSource = NRESET` and read `cleared` right
after one `GetStatus` — and `GetAndClearIrqStatus` clears the interrupt flags on every
issue. The radio FIFO read goes through `SPIreadStream` directly, so it never enters that
path.

I am happy to put either of these together as a pull request, and to re-run the whole
comparison on both families afterwards, including a check that nothing starts getting
dropped that was not being dropped before.

### How I was testing, if you want to read this long story

Seven days, three boards side by side — the LR2021 one under test, an LR11x0 one on the
same firmware, and an SX1262 one as a reference receiver, all hearing the same live mesh —
149065 frames logged in total and eight firmware builds on the board under test. Most of
that time I was measuring the wrong thing.

The first mistake was mine and it is worth naming: I twice concluded I had found the fix
because a phase came back clean, and both times it was only that the fault takes a while
to reappear after any intervention. A plain reboot buys two clean frames, a real
power-down of the module buys about twenty. Nothing is fixed; the onset is delayed. After
that I stopped trusting any clean window that was not bracketed by a control on either
side, and every measurement above is A/B/A for that reason.

Things that turned out not to be it: the noise floor, which was constant through both
states; our own retransmit traffic; the antenna and its connector; the module supply — a
genuine power-down, with `ResetSource` reading `ANALOG (POR/BRN)` afterwards to prove the
rail had actually collapsed, did not clear it; and the firmware, since the fault survives
reflashing the identical image and appears on the very first frame afterwards. Wiring and
signal integrity looked plausible for a while, especially once the clock dependence turned
up, but the content of the bad replies argues against it: they are well-formed status
streams carrying the real IRQ word, and in the same second the 130-byte payload reads back
perfectly.

What finally moved it was making everything switchable at runtime instead of rebuilding
for each idea — the guard, the retry count, the gap between attempts, the SPI clock, and
finally the delay after the opcode — so a whole sweep could run inside one episode of a
fault that comes and goes on its own. Before that, each comparison spanned different
episodes and the numbers moved for reasons that had nothing to do with what I was testing.

The BUSY probe was the last thing I tried and the one I should have tried first.

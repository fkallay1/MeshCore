# RadioLib issue 1857 — NOVÝ NÁVRH B: cez surové bajty

**Stav: NA SCHVÁLENIE. Neposlané.** Vyrobené 20. 9. 2026 ako druhá alternatíva.

Alternatíva k `rl-1857-comment-plain-report.md` (návrh A). Staré drafty
`rl-1857-comment-what-we-saw.md` a `rl-1857-comment-short-reply.md` **ostávajú
nedotknuté na porovnanie**.

## Čím sa líši od návrhu A

Rovnaké fakty a rovnaká dĺžka, ale **otvára sa surovými bajtmi, nie opisom**. Dôvod je
doložený: v RadioLib issue 1861 (eggaroonie, LR2021, chyby `-24`, zatvorené 28. 8. ako
vyriešené) jgromes vec rozlúskol z jedného IRQ slova — `0x00040171` rozobral na bity a
z toho určil príčinu. Na ten tvar reaguje. Prózu o mechanizme nám naopak vytkol.

Návrh A je bezpečnejší (nič netvrdí o vnútri čipu), návrh B je informačne hutnejší
a rýchlejšie sa z neho dá nesúhlasiť — čo je v tomto prípade skôr výhoda.

Navyše oproti A priznáva, že **naše chybové miery sú z buildov s vlastnou chybou**
aplikácie (zlý posun pri `PREAMBLE_DETECTED`, opravené inde 8. 9.). Počítadla stavového
bajtu sa to netýka. Ak by to bolo príliš, tento odsek sa dá vypustiť bez toho, aby sa
zvyšok rozsypal — ale potom sa nesmie citovať ani miera 4,27 %.

## Fakty, z ktorých text vychádza (overené 20. 9.)

* Dvojica meraní, obe už raz zverejnené 1. 9.:
  `nowait  stat=05 cmd=2 val=69` (stavový prúd, `val` = irq[31:16]) a
  `wait    stat=07 cmd=3 val=40` (skutočná odpoveď). 8 z 8.
* 133 h 9 min okno, build #317: 65 069 prijatých, 2 900 chýb (4,27 %), `spifix` = 0.
* Posledná epizóda 30. 8. 2026.
* A/B: 6 okien po 30 min, A 2,61 % vs B 2,97 %, p ≈ 0,65.
* Odvolané LR11x0 čísla (chybné indexovanie odpovede) — už odvolané 1. 9., neopakovať.

## Pravidlá dodržané

Žiadna zmienka o AI. Žiadne tabuľky, tučné písmo ani nadpisy v tele. Prvá osoba
jednotného čísla. Žiadne `#číslo`. Bloky s bajtmi nechať ako sú (odsadené štyrmi
medzerami), ostatné odseky pri odosielaní spojiť do jedného riadku.

---

## Body

Sorry for the wall of text earlier. Here is the report again, in raw bytes rather than
in my reading of them.

With the BUSY wait deliberately skipped, the reply to the get-packet-length command
comes back as

    stat=05  cmd=2  val=69

and the same read with the wait in place comes back as

    stat=07  cmd=3  val=40

The first is the chip's default status stream: command status CMD_OK, and what I parsed
as a length is the top half of the IRQ word. The second is the real reply, CMD_DAT, with
the true length. Eight out of eight went that way. The symptom on the air is episodes,
hours apart, of frames read at the wrong size and discarded, and nothing anywhere reports
an error, because a wrong length is still a valid length.

You asked me to check whether the physical BUSY connection is reliable, and you were
right to. I can provoke the fault on demand by disconnecting that line, and the rate on
this board changed after I reseated connectors. How well that line can be observed from
the host also varies within a single episode, so I would not draw conclusions from my
probe of it. Part of what I measured is my own hardware.

I cannot tell you whether your branch fixes it, and I owe you a correction. When I
reported no regression after ten days, I wrote that the board had been on your commit
continuously; it was running your commit together with two changes of my own. Since then
it has run five and a half days, 65 069 frames received, with my detector at zero, and I
switched the wait off for three separate half-hour windows inside that run without the
condition appearing. The last episode I saw was on 30 August. I also ran an A/B on it
twice: the first pass looked like a clear effect, three alternations later the effect was
gone and the noise had landed in the other phase. One more caveat on my own numbers - my
application had an unrelated bug in those builds, so the receive error rates are not
clean. The status byte counter is unaffected by it.

If you would rather close this until I can reproduce it with an unmodified library, that
is fine by me. The part worth keeping is the discriminator above: whatever turns out to
be enough as a fix, the command status byte is a cheap way to detect the condition rather
than only avoid it.

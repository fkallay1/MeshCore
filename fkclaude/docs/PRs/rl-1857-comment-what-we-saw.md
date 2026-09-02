# RadioLib issue 1857 — krátky komentár: čo sme vlastne riešili

**Stav: NA SCHVÁLENIE. Neposlané.**

Nahrádza `rl-1857-comment-short-reply.md` (ten bol dlhší a viac sa venoval omluve).
Tu je jadro: **povedať jgromesovi, aký problém sme riešili**, lebo napísal, že mu to
z našich textov nebolo jasné.

Štyri odstavce, žiadne tabuľky, žiadne tučné písmo, žiadne nadpisy.

Obsah:
* aký je príznak na doske — epizódy pokazených paketov
* čo sme zistili — je to stavový bajt namiesto dát, teda `CMD_OK` miesto `CMD_DAT`
* že to nejako súvisí s BUSY, bez tvrdenia o mechanizme
* že carlhodderov príznak vyzerá podobne, len zjavne zriedkavejšie

Pravidlá: žiadna zmienka o AI, žiadne odkazy s mriežkou.

---

## Telo

Sorry, my earlier posts were far too long and I buried the actual report. Let me say
plainly what I was trying to describe.

On my board I get episodes - not constant, they come and go over hours - where received
packets are corrupted. What I traced it to is that a read command sometimes returns the
chip status stream instead of the data I asked for. The command status in that reply is
`CMD_OK` rather than `CMD_DAT`, so the chip is telling me the reply is not ready, and what
I get back instead is the status and IRQ word. My code was then reading the IRQ word as if
it were the packet length, which is why the lengths were nonsense. In the worst episode
about three quarters of the traffic was lost this way, and nothing anywhere reports an
error, because a wrong length is still a valid length.

It is somehow connected to BUSY, though I cannot tell you the mechanism. During those
episodes I sampled the pin right after issuing a read opcode and it never went high at
all - zero highs out of roughly two thousand samples, no edges. Outside the episodes the
same probe sees it asserted normally. I can also provoke the fault on demand by
disconnecting the BUSY line, and the rate on this board changed after I reseated
connectors, so part of what I measured is my own hardware.

@carlhodder, what you describe looks close to the same thing to me. The all-`05` payload
you see is the value I read as the first status byte on one of these bad replies - you get
it in place of the payload, I get it in place of the length. Yours sounds a lot less
frequent than mine, which would fit if my board has a marginal BUSY connection on top of
whatever the underlying condition is.

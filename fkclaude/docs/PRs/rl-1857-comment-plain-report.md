# RadioLib issue 1857 — NOVÝ NÁVRH A: obyčajný report

**Stav: NA SCHVÁLENIE. Neposlané.** Vyrobené 20. 9. 2026 ako nová alternatíva.

Nenahrádza `rl-1857-comment-what-we-saw.md` — ten **ostáva nedotknutý na porovnanie**.
Rovnako ostáva `rl-1857-comment-short-reply.md` (ešte staršia verzia).

## Čím sa líši od `what-we-saw`

1. **Vyhodené tvrdenie o nepozorovateľnom BUSY.** Starý draft písal „zero highs out of
   roughly two thousand samples, no edges". To už neplatí — v tej istej epizóde sme
   namerali 0/16 aj 16/16 sedem minút od seba (viď `lr2021_busy_unobservable_in_episode`).
   Nový text hovorí len to, čo vieme doložiť: poruchu vieme vyvolať odpojením BUSY
   a miera sa zmenila po preložení konektorov.
2. **Priznanie, že jeho opravu nevieme posúdiť** — chýbalo. S finálnymi číslami
   133-hodinového okna (65 069 rámcov, detektor 0) a s tým, že sme jeho čakanie
   trikrát vypli a porucha aj tak neprišla.
3. **Odvolanie vety „continuously since it landed"** — doska bežala na jeho oprave
   **plus našich dvoch commitoch**, nie na jeho samotnej.
4. **Ponuka issue zavrieť.** Zamrznuté issue je horšie než zatvorené; nech rozhodne on.
5. Dĺžka: päť krátkych odsekov, ~250 slov. Starý draft mal štyri, ale dlhšie a s
   nepodloženým odsekom.

## Fakty, z ktorých text vychádza (overené 20. 9.)

* 133 h 9 min okno, build #317: **65 069 prijatých**, 2 900 chýb (4,27 %), `spifix` = **0**.
* Poslednú epizódu sme videli **30. 8. 2026**.
* Druhý A/B beh, 6 okien po 30 min: A 2,61 % vs B 2,97 %, p ≈ 0,65 — bez efektu.
  Prvý beh s jedným striedaním dal p ≈ 0,0014, teda opačný záver. Detaily
  v `fkclaude/docs/lr2021-aba-rlwait-20260903.md`.
* Pri úmyselne preskočenom čakaní: **8 z 8** čítaní vrátilo stavový prúd s `CMD_OK`,
  to isté čítanie s čakaním vrátilo `CMD_DAT` a správnu dĺžku.
* Jeho PR 1858 je stále otvorený a nezmergovaný.

## Pravidlá dodržané

Žiadna zmienka o AI. Žiadne tabuľky, tučné písmo ani nadpisy v tele. Prvá osoba
jednotného čísla. Žiadne `#číslo`. Pri odosielaní odseky spojiť do jedného riadku.

---

## Body

Sorry for the wall of text earlier. Let me put the report in plain terms.

On my board I get episodes, coming and going over hours, where received frames are
corrupted or dropped. What I traced it to is that a read command sometimes returns the
chip status stream instead of the reply I asked for. The command status in that reply
says CMD_OK rather than CMD_DAT, and my code was then reading the IRQ word as if it were
the packet length, which is where the nonsense lengths came from. Nothing reports an
error, because a wrong length is still a valid length.

You asked me to check whether the physical BUSY connection is reliable, and you were
right to. I can provoke the fault on demand by disconnecting that line, and the rate on
this board changed after I reseated connectors. How well the line can be observed from
the host also varies within a single episode, so I would not draw conclusions from my
probe of it. Part of what I measured is my own hardware, and my failure rates should not
be read as representative of the part.

I also cannot tell you whether your branch fixes it, and I should correct something I
said earlier. When I reported no regression after ten days, I wrote that the board had
been on your commit continuously, but it was running your commit together with two
changes of my own. Since then it has run five and a half days, 65 069 frames received,
and the condition did not occur once - my detector counted zero. I then switched the
wait off for three separate half-hour windows inside the same run and it still did not
occur. The last episode I saw was on 30 August.

So the run says nothing about your fix either way, and I did not want the earlier comment
left standing as if it did. I also ran an A/B on it twice: the first pass looked like a
clear effect, three alternations later that effect was gone and the noise had simply
landed in the other phase.

If you would rather close this until I can reproduce it with an unmodified library, that
is fine by me. The one thing I would keep is that the command status byte tells the two
cases apart: with the wait deliberately skipped, eight out of eight reads came back as
the status stream reporting CMD_OK, while the same read with the wait in place reported
CMD_DAT and the correct length.

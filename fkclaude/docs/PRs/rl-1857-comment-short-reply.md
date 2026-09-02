# RadioLib issue 1857 — krátka odpoveď po jgromesovej kritike

**Stav: NA SCHVÁLENIE. Neposlané.**

Kontext: jgromes napísal, že z našich predchádzajúcich komentárov nerozumie, čo reportujeme,
a že texty vyzerajú ako od stroja (carlhodder to potvrdil — „that table should've given it
away, let alone the bolded sentences"). Zároveň dal za pravdu carlhodderovi, že knižnica
na BUSY už čaká, a teda že jeho oprava 1858 problém pravdepodobne nerieši.

Overené: **carlhodder má pravdu.** `Module::SPItransferStream()` s `waitForGpio` čaká na
BUSY pred prenosom aj po ňom. Naše pôvodné rámovanie „knižnica nečaká" bolo nesprávne.

Táto odpoveď je preto zámerne krátka, bez tabuliek, bez tučného písma a bez sekcií — teda
opak toho, čo sme posielali doteraz.

Čo je v tele:
* priznanie k dĺžke a štýlu predchádzajúcich príspevkov, jednou vetou, bez omáčky
* súhlas s carlhodderom vo vecnej časti
* jediné meranie, na ktorom nám záleží: BUSY sa v epizódach nezdvihne vôbec
* prepojenie na carlhodderov príznak — `0x05` je ten istý status bajt
* prísľub reprodukovať na nezmenenej knižnici

Pravidlá: žiadna zmienka o AI, žiadne odkazy s mriežkou.

---

## Telo

Fair point about the length and the formatting of my earlier posts, and I am sorry for the
noise. I will keep this short.

carlhodder is right and my framing was wrong: `SPItransferStream()` does wait for BUSY both
before and after the transfer when `waitForGpio` is set, so "the library does not wait" was
simply incorrect.

The one thing I did measure that I think still matters: on my board, during the bad
episodes, BUSY never goes high at all. I sampled the pin in a tight loop right after issuing
a read opcode and got zero highs out of roughly two thousand samples, with no edges. When
that happens the existing wait returns immediately, because there is nothing to wait for.
Outside those episodes the same probe sees the line asserted normally.

That fits your suggestion about the physical connection, and I should have led with it: I can
reproduce the fault on demand by disconnecting the BUSY line, and the rate changed after I
reseated connectors on that board. So at least part of what I was measuring is my hardware,
and my failure rates should not be read as typical.

@carlhodder, one thing that may connect our two cases: the all-`05` payload you see is the
same value I read as the first status byte on a stale reply. I get it in place of the packet
length, you get it in place of the payload, which would be the same reply-not-ready
condition reached through the FIFO read path instead of the length read.

To be accurate about what I am running: it is your commit plus the two retry commits I
described, not a clean library, and I should have said so earlier rather than writing that
the board had been on your fix. Over the last 24 hours it took 14284 frames and made 3007
transmissions and the retry never fired once - which I read as the fault simply not
occurring in that window, since the last bad episode on this board was three days ago. It
says nothing about whether your change helps.

I will put an unmodified copy on it as you asked and come back only if I have something
reproducible and short to report.

---

## Poznámka k oprave 2. 9.

Prvá verzia tela tvrdila „running 24 hours on a clean library". **Nebola to pravda** —
doska beží na `radiolib-cmddat`, teda jgromesova oprava **plus obe naše retry commity**.
Prezradzuje to už len to, že vieme prečítať `rlretry`; to počítadlo v čistej knižnici
neexistuje.

Zároveň opravené čítanie výsledku: `rlretry=0` neznamená „jeho oprava stačí", ale že za
14 284 rámcov **neprišla ani jedna pretečená odpoveď** — teda že porucha nenastala.
Poslednú epizódu sme videli 30. 8. O účinnosti jeho opravy to nevypovedá.

Reprodukciu na nezmenenej knižnici, ktorú žiada, sme **ešte neurobili**.

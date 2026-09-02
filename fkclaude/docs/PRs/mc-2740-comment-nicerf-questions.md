# MeshCore issue 2740 — otázky na carlhoddera + oznámenie nášho portu

**Stav: NA SCHVÁLENIE. Neposlané.**

## Kam to ide a prečo tam

**Priamo napísať sa mu nedá** — GitHub nemá privátne správy a `carlhodder` nemá
zverejnený email ani web (overené cez API).

Preto **MeshCore issue 2740** — „[Feature request] NiceRF LoRa2021 (LR2021 Gen 4) variant
support". Je to jediné miesto v MeshCore, kde sa zapája, tému má presne našu, a je to
**mimo jgromesovho issue**, čo je po jeho reakcii vítané. Sú tam aj `c03rad0r` (NiceRF
s ESP32-C3, chce prispieť variant) a `axhoff` (tlačí na LR2021 kvôli MeshTracker X1).

## Otázka na cievku je VRÁTENÁ

Pôvodne som ju vyhodil s tým, že je zodpovedaná dvakrát. **Druhý dôkaz neplatí.**

NiceRF nám odpísal, že *„current consumption specifications are provided for two operating
modes: DC-DC and LDO"* — ale **v datasheete `LoRa2021F33-2G4` to nie je.** Je tam jediná
hodnota `ReceiveCurrent @Sub-GHz <8 mA` a o DC-DC ani slovo. A ten graf, čo poslali
(*„PA LF Tx Power vs VBAT for +14, +17, +22 dBm"*, prah 2,2 V), je Semtechova krivka
**vnútorného** PA čipu.

Porovnanie ukazuje, že odpovedali o **inom module**:

| | `LoRa2021` (bez PA) | `LoRa2021F33-2G4` (náš) |
|---|---|---|
| napájanie | **1,8 – 3,6 V** | **3,0 – 5,5 V** |
| výkon | **až 22 dBm** | až **31 dBm** |
| prijímací prúd | < 7 mA | < 8 mA |
| externý PA | nie | **áno** |

Graf do 22 dBm aj prah 2,2 V sedia na modul bez PA. Náš má minimum 3,0 V, takže sa naň
nevzťahuje ani jedno.

**Zostáva teda len náš vlastný nepriamy dôkaz:** SIMO zrazilo odber modulu z ~11 na ~6,5 mA
(41 %) a menič spínajúci do prázdna by prúd nezrazil. Silná indícia, ale nie pohľad na dosku.

Text je v prvej osobe jednotného čísla („I"), nie „we" — píše to Fedor za seba.

## Čo je v tele

1. oznámenie portu na XIAO nRF52840
2. **rozlíšenie dvoch modulov** — bez toho sa nedá porovnávať nič
3. dve vety, **ako sme sa k nemu dostali** (cez RadioLib vlákno) a že tú istú potiaž máme
   občas aj my — bez detailov, tie sú v druhom vlákne
4. otázka, **ktorý modul má** + náš tip, že základný (podľa jeho zmienky o NTC a XTAL)
5. **cievka pre SIMO** — či je osadená, prípadne fotka
6. aké MCU a či je chybovosť na štyroch doskách podobná
7. zaseknuté prerušenia z júna
8. **TCXO** — náš má 0,5 ppm, čo je odpoveď na jeho júnovú obavu o vonkajšie jednotky
9. **že aktuálny MeshCore + RadioLib beží dobre** — bez zmienky o tej CRC chybe, lebo
   on sám v tom komentári napísal, že ju medzitým opravili

### Prečo detail o pokazených čítaniach v tele nie je

O tom „všetko 05" písal **v RadioLib 1857**, nie tu — v MeshCore 2740 hlásil v júni len
zrelosť RadioLibu, hodiny a zaseknuté prerušenia. Rozpisovať to tu by tému rozdelilo na
dve miesta a zduplikovalo text z `rl-1857-comment-what-we-saw.md`, čo je presne to, za čo
nás jgromes zhodil. Notifikáciu z `@carlhodder` dostane, takže si to spojí.

Pravidlá: žiadna zmienka o AI, žiadne odkazy s mriežkou.

## Zapracované Fedorove zmeny (3. 9.)

* „Worth flagging" → **„Worth mentioning"** (gramatika, jeho „Worth mention" nesedí)
* **vypustená** veta o 800 mA a „completely different class" — bola priostrá
* doplnený **tip, že má základný modul** podľa NTC a XTAL, s gramatickou opravou
* pri cievke **vypustený odsek o odpovedi NiceRF** — do komentára nepatrí, stačí, že my
  vieme, prečo ju nepoužívame
* **„the inductor is there" → „should be there"** — presnejšie, je to odvodenie
* **„at the similar rate" → „at a similar rate"**, „most of the traffic is lost" →
  **„some traffic is corrupted"** (miernejšie a pravdivejšie)
* **vypustená veta o CRC chybe** — carlhodder v tom istom komentári napísal, že ju
  „just last night the main branch patched", takže hlásiť mu, že sme ju nevideli, by
  bolo zbytočné
* fotka **rozšírená na obe verzie** modulu
* **skrátený záver** — pôvodne končil na „and they are intermittent". Že sú epizódy
  občasné, je v tele povedané už dvakrát („not constant, episodes that come and go over
  hours" a „hours of nothing, then episodes"), takže tretie zopakovanie inými slovami
  nič nepridávalo. Nosné je **„a separate thing"** — teda že to s RadioLibom nesúvisí.

### Ako je vyriešená otázka na cievku

Zaujíma nás **naša doska**, teda PA verzia — preto sa pýtame *„Do you know whether…"*.
Takto sa dá odpovedať, aj keď PA verziu nemá; formulácia „vidíš tam cievku?" by bola
neodpovedateľná. Fotka je otvorená pre **obe** verzie, lebo aj tá základná nám povie,
akú má NiceRF prax pri osádzaní tohto čipu.

---

## Telo

I have ported the NiceRF LoRa2021F33-2G4 to MeshCore, on a Seeed XIAO nRF52840, and have
been testing it as a repeater in a live mesh for a few weeks. Happy to share whatever is
useful - the PA table for that module, the RF switch mapping, and the SIMO and patch RAM handling - or the whole variant, if that is useful.

Worth mentioning that NiceRF sell two different parts here, and it matters for comparing
notes: the plain LoRa2021 that @c03rad0r describes, 19.72 by 15 mm, and the LoRa2021F33-2G4
I have, which carries an external PA and goes to about 30 dBm.

@carlhodder, I found your comments here after we crossed paths in the RadioLib thread on bad
reads on this chip, so the context may look familiar. I get the same kind of trouble on my
board - not constant, episodes that come and go over hours - and I will keep that discussion there rather than repeating it here.

A few questions if you do not mind.

Which of the two are your four boards - the plain module or the PA version? Going by your
mention of the NTC and the crystal I would guess the plain one, but I may be wrong. That is
the first thing I would want to know before reading anything across from your setup to mine.

Do you know whether the SIMO inductor is populated on the PA version? The datasheet for my
module says nothing about the DC-DC regulator at all, just a single receive current figure, so
I have no documentation to go on. My own measurement says it should be there - enabling SIMO
took the module from about 11 mA to about 6.5 mA in receive, and a converter switching into
nothing would not do that - but that is an inference. Since you had a shield off, a photo
would help, of either version - even the plain one would tell me something about how NiceRF
lay these out.

What MCUs are your four boards on, and do you see the corrupted packets at a similar rate on
all four? Mine is a single board and the rate on it varies - hours of nothing, then episodes
where some traffic is corrupted.

Did you get anywhere with the sticky interrupt problem you described in June, where preamble
valid and header valid stay set without an RX done? I have been chasing something with a
similar shape - the receiver goes deaf shortly after a transmission and does not come back on
its own - and I am wondering whether they are the same thing seen from two sides.

On the temperature point you raised in June: mine, the PA version, is specified with an industrial TCXO at
0.5 ppm, which is presumably why I have not run into the drift you were worried about for
outdoor units. If the plain module is on a plain crystal, that difference is worth knowing
for anyone choosing between the two.

And on your first point, about waiting for the RadioLib implementation to settle: for what it
is worth, it looks fine from here now. The episodes I mentioned above are a separate thing.

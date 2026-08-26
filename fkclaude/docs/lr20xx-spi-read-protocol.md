# LR20xx / LR11xx: čítacie SPI príkazy, BUSY a CMD_DAT

Podložené primárnymi dokumentmi, nie odvodením z RadioLibu. Vzniklo pri RadioLib
issue 1857 / MeshCore PR 3261, keď bolo treba rozhodnúť, či je opakované čítanie
správna oprava alebo hack.

## Zdroje a kde ich mám

| dokument | čo z neho | ako som ho získal |
|---|---|---|
| `LR2021/LR2022/LR2012 Final Datasheet Rev. 2.1` (243 s., 13/04/26) | §5.4.1.2, §6.7.1 | DigiKey `mm.digikey.com/.../600_62785538.LR20xxDatasheet_V2_1.pdf` |
| `LR1121 Transceiver User Manual Rev 1.2` (129 s.) | §3.2, §3.4, §3.5 | `files.waveshare.com/wiki/Core1121/UserManual_LR1121_v1_2.pdf` |
| `TheClams/lr2021` (Rust driver, `master`) | `src/lib.rs`, `src/status.rs` | GitHub |

Mouser oba PDF blokuje (vráti HTML „Access denied"), DigiKey a Waveshare nie.
Stiahnuté kópie aj textové výťahy sú v scratchpade session, nie v repe.

## Rámcovanie čítacieho príkazu

LR2021 DS §5.4.1.2: hostiteľ pošle 16-bitový opkód, čip **asertuje BUSY na
zostupnej hrane NSS**. Keď má dáta pripravené, BUSY deasertuje a hostiteľ ich
vyčíta druhým rámcom nulami.

Rozdiel v počte status bajtov, ktorý bol zdrojom chyby v `CustomLR1110.h`:

| | príkazový rámec | čítací rámec |
|---|---|---|
| **LR2021** (DS §5.4.1.2) | `Stat(15:8) Stat(7:0) Irq(31:24)` | `Stat(15:8) Stat(7:0) Rsp1 …` |
| **LR11x0** (UM §3.4) | `Stat1 Stat2 IrqStat(31:24) …` | `Stat1 Rsp0 Rsp1 …` |

LR1121 UM §3.4 to hovorí priamo: *„Stat1 and Stat2 are always sent when the host
issues a command. **Only Stat1 is sent back when retrieving data** from the
LR1121."* Preto má LR2021 v RadioLibe `BITS_16` a LR11x0 `BITS_8`, a preto bola
dĺžka na LR1110 v `buff[2]` namiesto `buff[1]` chybná.

### Nikde nie je špecifikovaná latencia asercie BUSY

Ani jeden dokument nedáva parameter typu `t_BUSY` — len „asserted on the falling
edge of NSS". Hostiteľ, ktorý po príkazovom rámci hneď číta BUSY, teda **nemá
dokumentovanú garanciu**, že nízka hodnota znamená „dáta hotové" a nie „ešte
neasertované". RadioLibových `delayMicroseconds(1)` je odhad bez opory v doke.
To je presne to, čo jgromes svojou opravou odstránil.

## CMD_DAT je dokumentovaný rozlišovač

LR2021 DS §6.7.1, definícia CommandStatus:

> `0x3: CMD_DAT` – The latest command was a successfully processed read, and data
> is currently transmitted **instead of IrqStatus**

Čiže CMD_DAT nie je „nejaký príznak" — je to jediný spôsob, ako sa dá odlíšiť
„toto sú tvoje dáta" od „toto je IrqStatus". Naša korupcia (dĺžka 4 = `irq[31:16]`)
je presne ten druhý prípad.

LR1121 UM §3.4.1 to pre celú rodinu hovorí ešte explicitnejšie:

> If a sequence of zeros (NOP) is written to the MOSI signal, the LR1121 clocks
> out either the response to the last command, **or the status information if no
> response is pending**. […] See stat1/CMD_DAT for more information.

Manuál teda čitateľa na CMD_DAT sám odkazuje ako na spôsob, ako zistiť, ktorú
z tých dvoch vecí práve dostal.

### Stat, 16 bitov (LR2021 DS tab. 6-38)

| bity | 15:12 | 11:9 | 8 | 7:4 | 3 | 2:0 |
|---|---|---|---|---|---|---|
| pole | `0x0` | CommandStatus | InterruptStatus | ResetSource | rfu | ChipMode |

CommandStatus sú bity 3:1 **prvého** (MSB) bajtu — RadioLib má `statusPos = 0`,
takže `SPIparseStatus` dostáva ten správny bajt a naša podmienka
`(stat & 0x0E) == CMD_DAT` je na oboch rodinách korektná.

Horný nibble musí byť `0x0`, takže status s nenulovým horným nibblom nie je
platný status — použiteľná sanity kontrola.

## Kontrolu nerobí ani RadioLib, ani Semtechov vlastný driver

RadioLib `SPIparseStatus` odmieta len `CMD_FAIL` a `CMD_PERR`, takže `CMD_OK`
prejde ako platná odpoveď. Rust driver `TheClams/lr2021` má **tú istú dieru**:

```rust
pub fn check(&self) -> Result<(), Lr2021Error> {
    match self {
        CmdStatus::Unknown => Err(..), CmdStatus::Fail => Err(..), CmdStatus::PErr => Err(..),
        CmdStatus::Ok | CmdStatus::Data => Ok(()),   // Ok aj Data prejdú
    }
}
```

Jeho `cmd_rd()` navyše čaká na BUSY **low** s timeoutom 1 ms rovnako ako RadioLib,
takže ak BUSY ešte nestúpol, vráti sa okamžite — teda tá istá race. V polling
vetve je zakomentovaný `Timer::after_micros(5)`, čo naznačuje, že sa s tým
časovaním niekto hral. Nie je to teda chyba RadioLibu, je to diera vo vzore.

## Prečo opakovať len druhý rámec, nie opkód

LR2021 DS §6.7.1 o GetStatus:

> If the previous transaction was a read command, GetStatus returns the result of
> the read command.

Pripravená odpoveď teda **nezmizne** — ostáva vyzdvihnuteľná ďalším rámcom. Preto
stačí znova naklokovať nuly a nie je to nič, čo by menilo stav čipu.

Opakovanie **opkódu** naopak vedľajšie efekty má, a to bola chyba v našej prvej
verzii patchu:

* `GetStatus` (0x0100) čistí ResetSource — druhé vydanie zahodí informáciu o tom,
  čo čip resetovalo
* `GetAndClearIrqStatus` (0x0117) čistí IRQ príznaky pri každom vydaní — retry by
  zahodil príznaky, ktoré medzi pokusmi vznikli. V RadioLibe to zatiaľ nič nevolá,
  takže je to latentné, nie aktívne

`readRadioRxFifo` obchádza `SPIcommand` úplne (`SPIreadStream` so status width 0),
takže FIFO čítanie sa do cyklu nedostane a nehrozí dvojité vyzdvihnutie dát.

**Zmerané 24. 8. 2026 na živej epizóde (ostrý guard, 13 udalostí):** opakovanie
**toho istého opkódu** za sebou zachránilo len **1 z 13**. Zvyšných 12 zachránil až
fallback `LR2021::getPacketLength()`, ktorý pred čítaním dĺžky volá `getPacketType()` —
teda **iný príkaz medzi pokusmi**. Status hlásil `CMD_OK` (`stat=0x5`) pri všetkých 13
prvých čítaniach.

Staršie „vždy tri čítania, tretie uspeje" bol artefakt diagnostickej cesty:
`fkDiagPktLen` volal `getIrqStatus()` **pred každým** čítaním dĺžky, takže medzi
opakovaniami bol vždy iný príkaz. Ostrý guard číta trikrát za sebou bez ničoho medzi.

Dôsledok pre patch: „opakuj to isté čítanie a breakni na CMD_DAT" opraví ~8 % týchto
prípadov. Potrebná je varianta s vloženým príkazom — a tú dáva §6.7.1 v dokumentovanej
forme: `GetStatus` po čítaní vráti výsledok toho čítania a je to iný opkód. Netestované.

## Diagnostické opkódy pre LR2021

| príkaz | opkód | odpoveď (2. rámec) |
|---|---|---|
| GetStatus | `0x0100` | jednorámcový: `Stat(15:8) Stat(7:0) IrqStatus(31:0)` |
| GetVersion | `0x0101` | `Stat(15:8) Stat(7:0) FWMajor FWMinor` |
| GetErrors | `0x0110` | chybové príznaky |
| ClearErrors | `0x0111` | — |

**LR2021 má FWMajor `0x01`, FWMinor `0x18`** (DS tab. 6-40; LR2022/LR2012 majú
`0x02`/`0x00`). To je konkrétna známa hodnota, proti ktorej sa dá overiť, či čip
skutočne odpovedá — na rozdiel od RadioLibovho `findChip()`, ktoré vráti len
`RADIOLIB_ERR_CHIP_NOT_FOUND` (-2) bez toho, aby povedalo prečo.

Pre našu „Radio init FAILED" má hodnotu najmä **ResetSource** (Stat bity 7:4):

| hodnota | význam |
|---|---|
| `0x0` | Cleared |
| `0x1` | Analog — POR / **Brown-Out** |
| `0x2` | NRESET pin |

Ak čip po zlyhaní init odpovie a ResetSource hlási `0x1`, videl brown-out — to je
priamy test hypotézy o parazitnom napájaní, nie dohad. ChipMode (bity 2:0) k tomu
povie, v akom režime sa nachádza.

Surové bajty statusu rozlíšia aj to, čo `-2` zliepa do jednej hodnoty: `0x00` =
MISO držané nízko (čip nenapájaný alebo drží zbernicu), `0xFF` = MISO plávajúce
(žiadny driver). RadioLib obe hlási ako CHIP_NOT_FOUND.

## A/B/A na živej epizóde: vložený príkaz zlyhanie ruší, nie zmierňuje

Meranie 24. 8. 2026, 00:14–02:12, XIAO/LR2021 na builde #311 (jgromesova oprava
aktívna), jediná menená premenná je cesta guardu. Epizóda syndrómu bola po celý čas
živá — potvrdené tým, že sa zlyhania po prepnutí späť vrátili na tú istú úroveň.

| fáza | cesta | rámce | udalostí guardu | podiel |
|---|---|---|---|---|
| 00:14–00:35 | ostrá (`pretype off`) | 142 | 119 | 83,8 % |
| 00:35–01:24 | **diag (`pretype on`)** | 100 | **0** | **0 %** |
| 01:24–02:12 | ostrá (kontrola) | 131 | 103 | 78,6 % |

Ostrá cesta číta dĺžku trikrát za sebou, nič medzi tým. Diag cesta pred cyklom zavolá
`getPacketType()` a potom **pred každým** čítaním `getIrqStatus()`.

**Nula z 100 proti 103 zo 131.** To nie je zmiernenie — s vloženým príkazom nezlyhalo
ani jedno *prvé* čítanie, takže sa nezaznamenala žiadna udalosť. Chyba teda nie je
o opakovaní; je o tom, čo čítaniu bezprostredne predchádza.

Doplňujúce čísla z ostrých fáz: keď sa zlyhanie stane, opakovanie toho istého opkódu
ho vyrieši v ~30 % (845 z 2848 videných záznamov malo `tries=3`), zvyšok potrebuje
fallback. **Neopravený neostal ani jeden rámec** (`final == first` nula krát, a na
výstupe nula `len=4` vo všetkých troch fázach).

Zatiaľ neoddelené: či lieči ten vložený príkaz alebo len čas, ktorý zaberie. Na to je
build 907 s režimami `a` (nič) / `b` (len pauza) / `c` (vložený príkaz) a nastaviteľným
stropom pokusov.

**Dôsledok pre patch:** navrhovať „opakuj čítanie" je liečenie následku. Ak sa potvrdí,
že rozhoduje predchádzajúca transakcia, správna oprava je iná — a `CMD_DAT` kontrola
ostáva ako detekcia, nie ako oprava.

## Hypotéza: rozhoduje počet transakcií od RX_DONE, nie čas

Meranie 24. 8. 2026, 02:19–03:30, build #317, guard v troch režimoch, strop pokusov 8.
Kľúčová veličina je **zlyhaných čítaní na udalosť** — z počítadiel presne, bez
závislosti na kruhovom buffri.

| režim | čo je medzi pokusmi | zlyhaní/udalosť | uspeje čítanie č. |
|---|---|---|---|
| `a` | nič | **3,03** | 4. |
| `c` | `getPacketType()` | **1,00** | 2. |
| `b` | pauza 60 µs | **3,86** (malá vzorka) | 4.–5. |

Pauza sa teda nechová ako príkaz. A ak sa to spočíta ako **transakcie od RX_DONE**,
sedí všetko, čo sme kedy namerali:

| konfigurácia | poradie transakcií | výsledok |
|---|---|---|
| guard OFF | `getPacketType`, dĺžka | zlyhá (2027 z 5996 rámcov = 34 %) |
| režim `a` | dĺžka, dĺžka, dĺžka, **dĺžka** | uspeje 4. |
| režim `c` | dĺžka, `getPacketType`, **dĺžka** | uspeje 3. |
| diag (`pretype on`) | `getPacketType`, `getIrqStatus`, **dĺžka** | uspeje 1. čítanie, 0 zo 100 |

Vo všetkých prípadoch, kde to uspeje, je čítanie **tretia alebo ďalšia** transakcia po
RX_DONE. Kde je druhá (guard off), zlyháva — a to nie vždy, ale v tretine prípadov, čo
je presne to, čo čakať na hranici.

To vysvetľuje aj prečo diag cesta „predchádzala" zlyhaniu, kým `getPacketType` samotný
v `LR2021::getPacketLength()` nie: diag má pred čítaním **dva** príkazy, knižnica jeden.

Nie je to potvrdené — potrebuje to väčšiu vzorku pre `b` a najmä test s pauzou rádovo
milisekundy. Ale ak to platí, oprava „opakuj čítanie" funguje len ako spôsob, ako tie
transakcie nasekať, a čistejšia oprava je vydať pred prvým čítaním dĺžky jednu neškodnú
transakciu navyše. Otázka pre datasheet ostáva otvorená: nič v ňom takúto podmienku
nespomína.

## Náš RadioLib patch overený na železe

24. 8. 2026, 05:26–06:17, tá istá doska, tá istá epizóda, 90 sekúnd medzi fázami.
Guard **vypnutý** v oboch, takže v ceste je len knižnica.

| build | RadioLib | rámce | `len=4` |
|---|---|---|---|
| #317 | iso1857 **+ náš patch** (opakuje len odpoveď, 5 pokusov) | 136 | **0** |
| #321 | iso1857 sám | 32 | **26 = 81,2 %** |

Kontrolný build hlásil prvý `len=4` už na **rámci #2, 16 sekúnd po bootnutí**. Pri
podiele 81 % je pravdepodobnosť, že 136 rámcov vyjde čistých náhodou, mimo akúkoľvek
diskusiu.

Patch teda funguje — a funguje vo variante, ktorá **opakuje iba druhý (NOP) rámec** a
opkód nechá na pokoji, takže nemá vedľajšie efekty `GetStatus` ani
`GetAndClearIrqStatus`. To bola otevřená otázka od chvíle, keď sme zistili, že náš
MeshCore guard opakuje celý príkaz, a teda ho nevalidoval.

Zapadá to do hypotézy o počte transakcií: preklokovanie odpovede **je** transakcia,
takže päť pokusov nasype dosť transakcií na to, aby odpoveď prišla správne.

### Prehľad všetkých fáz noci

| čas | konfigurácia | rámce | udalostí | zlyhaní/udalosť | `len=4` |
|---|---|---|---|---|---|
| 02:19 | guard `a`, strop 8 | 428 | 360 | 3,03 | 0 |
| 02:48 | guard `c` (vložený príkaz) | 90 | 69 | **1,00** | 0 |
| 03:24 | guard `b` (pauza 60 µs) | 57 | 46 | 3,96 | 0 |
| 04:25 | guard `a` (kontrola) | 117 | 91 | **3,05** | 0 |
| 05:26 | guard off, **náš patch** | 136 | 0 | — | **0** |
| 06:15 | guard off, **bez patchu** | 32 | — | — | **26** |

## Príčina nájdená: čip v tom stave BUSY vôbec nezdvihne

26. 8. 2026. `fkBusyProbe` pošle opkód s **preskočeným** čakaním a vzorkuje BUSY v tesnej
slučke (400 vzoriek). Osem pokusov za sebou, štyri opakovania celého testu, vždy rovnako:

| pokus | `hi` | `fall` |
|---|---|---|
| 0, 1, 2 | **0** | 0 — BUSY nikdy nevidený hore |
| 3–7 | 15–16 | 16–17 — hore ~15 vzoriek, potom padne |

Linka teda **nie je prirýchla na zachytenie** — keď funguje, drží 15 vzoriek. Pri prvých
troch transakciách ju čip jednoducho nezdvihne. Hostiteľ, ktorý čaká na jej **pokles**, sa
preto vráti okamžite a vyčíta odpoveď skôr, než existuje.

To je tá istá trojka ako pri čítaní dĺžky (1., 2., 3. vráti status, 4. je správne), takže
takmer isto ide o jeden jav.

### Potvrdenie: pauza po opkóde (2 MHz, jedno čítanie)

| pauza | rámce | zlyhaní 1. čítania |
|---|---|---|
| 0 µs | 110 | 70,0 % |
| **8 µs** | 108 | **0,0 %** |
| 16 / 24 / 32 / 48 µs | 424 | **0,0 %** |
| 0 µs (kontrola) | 103 | 83,5 % |

532 rámcov s pauzou, ani jedna chyba, zovreté dvomi kontrolami.

### Odporúčaná oprava

Čakať na **stúpnutie** BUSY s krátkym timeoutom, až potom na pokles. Keď čip linku zdvihne,
zachytí sa a čaká sa presne tak dlho, ako treba; keď ju nezdvihne, vyprší timeout — a ten
timeout **je** tá pauza, ktorá vec opravuje. Oba prípady skončia správne a nič sa
nespomaľuje zbytočne. Pevná pauza funguje tiež (8 µs stačilo), ale platí sa pri každom
čítaní.

Kontrola `CMD_DAT` ostáva ako záchranná sieť, nezávisle overená: 0 zo 136 proti 26 z 32.

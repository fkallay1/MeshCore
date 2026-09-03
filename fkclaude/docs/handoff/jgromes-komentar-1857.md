# Zadanie: komentár pre jgromesa do RadioLib issue 1857

**Cieľ:** dopísať a po Fedorovom schválení odoslať krátky komentár. Text je rozpísaný,
teraz k nemu máme aj výsledok testu.

## Kontext

jgromes nám napísal, že z našich predchádzajúcich komentárov **nechápe, čo reportujeme**,
a že texty vyzerajú ako od stroja. carlhodder to potvrdil („that table should've given it
away, let alone the bolded sentences"). Odvtedy píšeme **krátku prózu bez tabuliek,
bez tučného písma, bez nadpisov, v prvej osobe jednotného čísla.**

Draft: `fkclaude/docs/PRs/rl-1857-comment-what-we-saw.md` — **NA SCHVÁLENIE, neposlané.**
Štyri odstavce: aký je príznak, že je to `CMD_OK` namiesto `CMD_DAT`, že to súvisí s BUSY
(bez tvrdenia o mechanizme), a že carlhodderovo „všetko 05" je ten istý stavový bajt.

## Čo treba do textu doplniť

1. **Oprava tvrdenia z 1. 9.** Komentár `#issuecomment-5500032693` hovorí
   *„continuously since it landed"* — nie je to presné, doska bežala na jeho oprave
   **plus našich dvoch commitoch**.
2. **Výsledok A/B/A** (podrobne v `fkclaude/docs/lr2021-aba-rlwait-20260903.md`):
   jeho opravu sme vypli na 90 minút, detekcia bežala, **`stale=0`** — ani jedna
   pretečená odpoveď. Chybovosť s opravou 2,61 %, bez nej 2,97 %, p ≈ 0,65, a všetko
   sú obyčajné CRC/hlavičkové chyby z éteru, nie SPI.
3. **Formulácia musí byť „nevieme", nie „nepomáha".** Test nedokazuje neúčinnosť —
   dokazuje, že porucha sa v tom okne nevyskytla. Poslednú epizódu sme videli 30. 8.

## Pravidlá (Fedorove, záväzné)

- Žiadna zmienka o AI, nikde.
- **Neposielať ani neupravovať zverejnené, kým Fedor neodsúhlasí finálne znenie.**
  „Poslal by som to" ani „doplň tam vetu X" **nie je súhlas** — ukáž celý text a čakaj.
- Pred odoslaním over, či sa súbor medzitým nezmenil — Fedor doň píše súbežne.
- Odsadenie: pri odosielaní spoj odseky do jedného riadku, GitHub mäkký zlom vykreslí
  ako koniec riadku a natvrdo zalomený text vyzerá rozstrapkane.
- `gh` je `D:\FkDev\GHcli\bin\gh.exe`, nie je na PATH.

## Hotovo keď

Fedor text schváli a je odoslaný; do hlavičky súboru sa dopíše URL komentára.

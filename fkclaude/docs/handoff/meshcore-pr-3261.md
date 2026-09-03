# Zadanie: rozhodnúť, čo s MeshCore PR 3261

**Cieľ:** nechať, prepracovať, alebo zavrieť.

## Stav (overené 3. 9. 2026)

**PR 3261 je OPEN** — „Fix LR11x0 and LR2021 packet loss when a stale SPI reply is read
as the received packet length". Vetva `fix/lr2021-lr1110-stale-spi-reply`,
commit `152c6d5d`, pushnutá.

Pre porovnanie: **3218 MERGED** (LR2021 setTxPower), **2978 OPEN** (serial CLI lockup).

## Prečo je to otázne

1. **Naše LR11x0 čísla v tom PR sme odvolali.** Komentár z 20. 8. („4 of 4 … recovering
   84, 20 and 196 byte frames") vznikol cez naše chybné čítanie — `CustomLR1110`
   indexovala odpoveď ako LR2021 (dva status bajty), hoci LR11x0 má jeden, takže sme
   čítali offset v bufferi namiesto dĺžky. Prezradilo to, že 100 % dĺžok bolo deliteľných
   štyrmi. Odvolané v komentári z 1. 9.
2. **A/B/A z 2.–3. 9. nič nepotvrdil.** `stale=0` za 4032 aj za 1777 rámcov, vrátane
   90 minút s **vypnutým** čakaním na BUSY. Porucha sa nedá vyvolať ani tak. Poslednú
   epizódu sme videli 30. 8. Podrobne v `fkclaude/docs/lr2021-aba-rlwait-20260903.md`.
3. **jgromesov BUSY patch `440610d71`** rieši vec na úrovni knižnice. Ak sa dostane do
   RadioLibu, náš guard v MeshCore môže byť zbytočný.

## Argument pre ponechanie

Príznak je **tichý**: zlá dĺžka je platná hodnota, `readData()` uspeje a žiadne
počítadlo sa nepohne. Rozlíšenie `CMD_DAT` vs `CMD_OK` dáva lacný spôsob, ako stav
**zistiť**, nie len obísť. To meranie stojí samo osebe a je zopakovateľné:
s úmyselne preskočeným čakaním malo **8 z 8** pretečených odpovedí `cmd≠3`, kým to isté
čítanie s čakaním vrátilo `cmd=3` a správnu dĺžku.

## Postup

1. Počkať, ako dopadne jgromesov patch v RadioLibe (viď zadanie **jgromes-komentar-1857**).
2. Podľa toho buď PR prepísať tak, aby bol o **detekcii** (počítadlo pretečených
   odpovedí), nie o oprave, alebo zavrieť s odkazom na knižničné riešenie.
3. Nech je rozhodnutie akékoľvek, do PR napísať krátky poctivý komentár.

## Pasce

- **Text vopred Fedorovi na schválenie**, aj pri zatváraní. „Poslal by som" nie je súhlas.
- Krátka próza, žiadne tabuľky ani tučné písmo — za to nás už raz zhodili.
- CI upstreamu vyžaduje schválenie pre každého mimo organizácie (`action_required`),
  nám nikdy nezbehne samo; po 30 dňoch expiruje na červený fail s 0 jobmi. Overovať cez
  `gh api actions/runs`, nie `gh pr checks`.

## Hotovo keď

PR je buď aktualizovaný, alebo zavretý s vysvetlením.

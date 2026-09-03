# Zadanie: upratať worktree a presunúť ich zo scratchpadov

**Cieľ:** dostať worktree na stále miesto a zrušiť tie, čo doslúžili.

## Stav (3. 9. 2026)

Šesť worktree, **všetky v session scratchpadoch**, ktoré sú svojou povahou dočasné.
Jeden je dokonca pod scratchpadom **cudzieho projektu**:

| kde | vetva |
|---|---|
| `…/9c1a50a7…/scratchpad/mc-pr-lr-stale` | `fix/lr2021-lr1110-stale-spi-reply` |
| `…/cfa39a1e…/scratchpad/mc-pr-lr2021` | `fix/lr2021-set-tx-power-rx-stall` |
| `…/6a27aa02…/scratchpad/mc-rl1857` | `test/rl1857-busy-wait` |
| `…/6a27aa02…/scratchpad/mc-nodiag` | `test/rl1857-nodiag` |
| `…/156e0822…/scratchpad/mc-pr` | `fix/serial-cli-buffer-lockup` |
| `…/D--FkDev-FkProj-VSC-esp32-s3-USBIP/…/scratchpad/mc-pr-wt` | `fix/repeater-btn-boot-level` |

Vytvoril ich Claude, Fedor o nich nevedel — pýtal si vetvy, nie worktree.

## Dohoda z 3. 9.

Fedor s worktree súhlasí (*„ak je pre mna vyhodnjesie pouzivat worktee, tak to mozeme"*),
ale platí:

1. Nový worktree **ohlásiť dopredu aj s dôvodom**, nie potichu.
2. Dávať ich na **stále miesto**, nie do scratchpadu:
   `D:\FkDev\FkProj\VSC\MeshCore-wt\<meno>` — súrodenec repa. Dovnútra repa nepatria,
   musel by sa ignorovať.
3. Keď vetva doslúži, worktree zrušiť — **vetvu to nezmaže**, len uvoľní adresár.

## Čo treba vedieť

- Worktree je **len na disku, na GitHub sa neukladá nič.** GitHub pozná commity a vetvy;
  to, či je vetva rozbalená v jednom adresári alebo v piatich, nikto nevidí a čerstvý
  klon nemá žiadny.
- Vetvu vybratú v inom worktree **nejde vybrať v hlavnom strome** — git to odmietne
  (`fatal: … is already checked out at …`), v `git branch -vv` to poznáš podľa `+`.
  Toto Fedora mohlo miasť.
- Každý worktree je pre PlatformIO **samostatný projekt** — vlastný `.pio`, prvý build
  od nuly. Miesto problém nie je (157 MB oproti 1801 MB v hlavnom strome), čas áno.
- Otvárať vo VS Code **len zvnútra bežiaceho VS Code** (`Ctrl+Shift+N`, potom
  Open Folder), nikdy `code.exe` nasucho — prostredie nastavuje `start_VSCode.bat`
  (HOME, USERPROFILE, PLATFORMIO_CORE_DIR, ZEPHYR_SDK_INSTALL_DIR, PATH, TEMP) a nové
  okno ho zdedí len od už bežiaceho procesu.
- Od 3. 9. je v user settings `"window.openFoldersInNewWindow": "on"`, takže Open Folder
  už nenahradí okno s otvoreným workspace; a `start_VSCode.bat` prijíma cestu ako
  argument (`%*`), takže vie otvoriť konkrétny priečinok aj pri studenom štarte.
- Multi-root workspace pri tomto projekte **neodporúčať** — IntelliSense už raz trpela.

## Postup

1. Zisti, ktoré vetvy sú vybavené: **PR 3218 je MERGED** (`fix/lr2021-set-tx-power-rx-stall`),
   `fix/repeater-btn-boot-level` nie je ani pushnutá, `test/rl1857-nodiag` bol jednorazový.
2. `git worktree remove <cesta>` pre doslúžené, potom `git worktree prune`.
3. Zvyšné presunúť do `MeshCore-wt\`.

## Hotovo keď

`git worktree list` ukazuje len hlavný strom a worktree na stálom mieste.

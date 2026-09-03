# Zadanie: prečo build z testovacej vetvy zavesí ProMicro pri prvom TX

**Cieľ:** zistiť, či je to firmware alebo doska. Samostatný nález, nesúvisí s LR2021.

## Pozorovanie (3. 9. 2026)

ProMicro naflashovaný buildom **#318** z vetvy `test/lr2021-runtime-ab`:

```
03-10:19:06  -> id b#318 sz=427692 sha=7825D854
03-10:19:15  [FOTA] TX RAW #1 len=132 type=4(ADVERT) srch=18 route=2
             ...a odvtedy nič, ani CLI
```

Nabehne, odpovie, odvysiela **prvý ADVERT** a zamrzne celá slučka.

## Prečo to nie je zrejmé

**Build #500 (fungoval) aj #318 (zamrzne) majú presne rovnakú veľkosť 427 692 B.**
Kód je teda takmer identický a „iná vetva" ako vysvetlenie príliš nesedí. Fedorova
poznámka: *„je divne, ak by zrazu bez dotyku prestal fungovat"*.

## Hypotézy na overenie

1. **Doska, nie firmware** — ak sa #500 po flashi (viď [promicro-ozivenie-swd])
   zasekne rovnako, firmware z hry vypadáva. Sedelo by to s hypotézou o parazitnom
   napájaní cez SWD (`promicro_jlink_phantom_power` v pamäti).
2. **Regresia v testovacej vetve** — tá nesie runtime A/B RadioLib (symlink na
   `radiolib-ab`) a FK diagnostiku. Zmeny sú v `LR11x0/LR_common.cpp`, čo SX1262
   nepoužíva, ale `RADIOLIB_GODMODE=1` a build flagy sa líšiť môžu.
3. **Zaseknutie v ceste po TX** — pripomína `lr2021_post_tx_rearm`, len na inom čipe.

## Postup

1. Naflashuj #500 (samostatné zadanie) a nechaj bežať — ak zamrzne pri prvom ADVERTe
   rovnako, je to doska.
2. Ak #500 pobeží, postav ProMicro z `features/nrf-fota` a porovnaj s #318.
3. Diff build flagov medzi vetvami pre `ProMicro_repeater_fota`.

## Hotovo keď

Vieme povedať „doska" alebo „firmware", a v druhom prípade ktorá zmena to spôsobuje.

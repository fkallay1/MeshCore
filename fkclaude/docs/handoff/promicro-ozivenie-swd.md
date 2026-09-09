# Zadanie: oživiť ProMicro cez SWD

> **VYRIEŠENÉ 3. 9. 2026 večer — bez SWD.** Doska sa medzitým dostala do UF2
> bootloadera. Serial DFU (`adafruit-nrfutil`) na nej zlyháva deterministicky
> (start + init packet prejdú, prvý dátový paket skončí na
> `WriteFile failed (PermissionError(13, ...))`, opakovanie nepomôže), ale
> bootloader vystavuje removable disk **`F:` / label `PROMICRO`**. Skopírovanie
> `test_nrf-fota/builds/promicro.fw_500.uf2` na `F:\` prešlo na prvý pokus.
>
> Po nábehu: build **#500** odpovedá cez hub 7455 (`clock`, `ver`), AALIVE riadky
> pribúdajú v `test_nrf-fota/logs/promicro-local.log`, hodiny nastavené.
>
> Rádio nabehlo mŕtve (`rssi-window n=960 min=-127 max=-127 spread=0`), ale
> `FKPR_RADIO_WATCHDOG` ho po 3 oknách sám reinicializoval
> (`radio re-initialised OK`, nf=-105) a pakety odvtedy tečú — ďalší dôkaz, že
> zotavenie funguje na železe. `promicro.fw_500.zip` už netreba.
>
> Poznatok uložený v pamäti ako `promicro-uf2-recovery`.

**Cieľ:** dostať na ProMicro (COM5) späť funkčný firmware. Doska je zaseknutá a
softvérovo sa do DFU nedostane.

## Stav (3. 9. 2026, overené)

Na doske je build **#318** (naflashovaný 3. 9. o 10:19 z vetvy `test/lr2021-runtime-ab`).
Nabehol, odpovedal na `id`, o 10:19:15 odvysielal **prvý ADVERT a hneď po ňom zamrzol** —
nielen rádio, ale celá slučka: CLI neodpovedá vôbec.

Predtým na nej bežal build **#500** a fungoval.

- `flash-dfu` končí na `Target is not in DFU mode`
- 1200-baud touch prejde (port sa otvorí), ale **USB identita sa nezmení** → doska do
  bootloadera nevstúpi. Ten touch obsluhuje bežiaci firmware a ten je mŕtvy.
- SWD sonda **nie je pripojená** — overené `nrfjprog --ids`, `pyocd list`,
  USB `VID_1366` (všetko prázdne). Fedor ju zabudol zapojiť.

## Pripravené

`test_nrf-fota/builds/promicro.fw_500.zip` — **vyrobený a overený**, netreba ho robiť znova.
Vznikol z `promicro.fw_500.uf2`: adresa **0x26000** vytiahnutá z UF2 hlavičky (nie
odhadnutá), payload sa zhoduje s `.bin`, `device_type 82`, `sd_req 0x123` — rovnako ako
referenčný `firmware.zip` z PlatformIO.

Hex na tej istej adrese je v scratchpade session ako `promicro_fw500.hex` (ak zmizol,
vyrobí sa z UF2 rovnakým postupom).

## Postup

1. Fedor pripojí J-Link/SWD na ProMicro.
2. Over sondu: `nrfjprog --ids` alebo `pyocd list`.
3. Flash (v poradí spoľahlivosti):
   - `nrfjprog --program <hex> --sectorerase --verify --reset`
   - `pio run -e ProMicro_repeater_fota -t upload --upload-protocol jlink`
   - `pyocd` / `openocd` ako záloha
4. Hub na COM5 predtým pozastaviť: pošli `~~HUB:PAUSE~~` na `localhost:7455`,
   po flashi `~~HUB:RESUME~~`. (Cez SWD to možno netreba, ale port drží hub.)
5. Po nábehu nastaviť hodiny: `time <epoch_sekundy>` (UTC).

## Pasce

- **`239A:00B3` nerozlišuje nič** — táto doska má ten istý USB PID v aplikácii aj
  v bootloaderi. Nevyvodzuj z neho stav dosky (ja som to spravil dvakrát a obe razy zle).
- `pyserial` je **len v `ZephCore\.venv`**, nie v `Tools\python312`.
- Fedor má vlastnú poznámku, že **SWD na ProMicre bolo podozrivé z parazitného
  napájania** (mŕtvy I2C displej, zaseknuté rádio). Pripojenie sondy teda samo osebe
  môže správanie zmeniť — všímaj si to.

## Hotovo keď

Doska odpovedá na `clock` cez hub 7455 a v `logs/promicro-local.log` pribúdajú AALIVE riadky.

# Draft PR — Seeed XIAO nRF52840 + NiceRF LoRa2021F33-2G4

**NEODOSLANÉ — čaká na Fedorovo schválenie** (2026-10-02).

Vetva `feat/nicerf-lora2021f33-xiao` na `origin` (`ec11e6bf`), rebasnutá na upstream
`dev` `fad0ffb7`, 3 commity (variant, power-cyklus, companion envy). Worktree
`D:\FkDev\wt\nicerf-pr`. Buildy OK: `_repeater`, `_repeater_simo`, `_companion_radio_ble`,
`_companion_radio_usb`.

Na železe overené: repeater a repeater_simo (XIAO + F33 v živej sieti). **Companion envy
sú len preložené**, na železe nie — pred odoslaním buď otestovať, alebo nechať v texte
„build tested" (tak je napísané nižšie).

Vzťah k #2739 (c03rad0r): iný modul (plain LoRa2021 bez PA, kryštál) a iný hostiteľ
(ESP32-C3); spoločný súbor žiadny. V texte sa naň odkazuje len slovne, nie číslom
(krížové odkazy, rovnaké pravidlo ako pri 2739 komentári).

Odoslanie:

```bash
python -c "import io;p='fkclaude/docs/PRs/mc-draft-pr-nicerf-lora2021f33.md';s=io.open(p,encoding='utf-8').read();i=s.rindex(chr(35)*2+' Body')+7;io.open('body.md','w',encoding='utf-8',newline='').write(s[i:].lstrip(chr(10)))"

D:/FkDev/GHcli/bin/gh.exe pr create --repo meshcore-dev/MeshCore --base dev --head fkallay1:feat/nicerf-lora2021f33-xiao --title "Add Seeed XIAO nRF52840 + NiceRF LoRa2021F33 variant" --body-file body.md
```

## Body

Adds a variant for a Seeed XIAO nRF52840 with a NiceRF LoRa2021F33-2G4, which is a Semtech LR2021 behind an external dual-band front end with a PA, rated about 1 W at 868 MHz. It is a different module from the plain LoRa2021 in the open ESP32-C3 variant: that one has no PA and a crystal, this one has a PA, a TCXO and front-end lines that the chip has to drive.

The board layer is reused from variants/xiao_nrf52 unchanged. Only target.cpp and target.h are new, plus a small module header in src/helpers/radiolib/NiceRF_LoRa2021F33.h that another board could reuse. It covers four things that are specific to the module:

The RF switch. The PA enable is LR2021 DIO6, active in TX_LF, and without it the module transmits on the bare chip output. LR2021::setRfSwitchTable() indexes its config by table column rather than DIO number, so the pin array has to stay dense and ordered from DIO5, which is noted in the header.

A PA table, so that the TX power setting means roughly dBm at the module output. RadioLib's stock table is tuned for Semtech's reference design and its lowest step already drives this module to about 19 dBm. The table is interpolated from the datasheet and cross-checked against supply current, not measured with an RF meter, and NICERF_LORA2021F33_STOCK_PA_TABLE falls back to the stock one.

The firmware patch RAM, which the datasheet calls highly recommended and RadioLib does not load yet. It is loaded after begin(), because findChip() resets the chip, and skipped when a patch is already active.

A power cycle of the module before init. CE has an internal pull-up, so the module stays powered across an MCU reset and can end up where begin() returns CHIP_NOT_FOUND on every boot until it loses power. The cycle drives CE low with every line we own held low first, because the vendor warns the module can be fed through its IO pins otherwise, and retries up to twice.

There is also an env with the chip's DC-DC regulator enabled. The inductor is fitted even though the vendor does not document it, receive current drops from 20.5 to 16.0 mA. It is kept separate because the regulator mode has to be re-applied after the modulation parameters are set, so a runtime SF change drops it back to LDO until the next init. Once the pinned RadioLib is moved to 7.8, which has DC-DC support for LR2021, this can use that instead of the direct writes.

2.4 GHz is wired on the module and prepared in the header but left commented out.

Envs: Xiao_nrf52_nicerf_lora2021f33_repeater, _repeater_simo, _companion_radio_ble, _companion_radio_usb. The two repeaters have been running on hardware in a live mesh at 869.618 MHz, SF7, BW 62.5. The companion envs are build tested only.

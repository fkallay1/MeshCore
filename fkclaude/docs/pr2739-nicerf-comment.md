# Komentár k upstream PR 2739 (NiceRF LoRa2021 + ESP32-C3)

**NEODOSLANÉ.** Cieľ: `meshcore-dev/MeshCore`, PR 2739 od `c03rad0r`.

Príkaz (telo = len časť za posledným nadpisom Body; `rindex`, nie `index`):

```bash
python -c "import io;p='fkclaude/docs/pr2739-nicerf-comment.md';s=io.open(p,encoding='utf-8').read();i=s.rindex(chr(35)*2+' Body')+7;io.open('body.md','w',encoding='utf-8',newline='').write(s[i:].lstrip(chr(10)))"

D:/FkDev/GHcli/bin/gh.exe pr comment 2739 --repo meshcore-dev/MeshCore --body-file body.md
```

Pravidlá: bez zmienky o AI, bez URL a bez mriežkových čísel (krížové odkazy).
Modul sa označuje **F33**, nie F233.

## Body

We are running what looks like the same module on a different MCU - a Seeed XIAO
nRF52840 with the NiceRF LoRa2021F33-2G4 (18-pin, dual-band) - so a couple of things we
measured here may be useful, and there is some overlap worth flagging early.

**The front-end DIOs do need programming.** The description says "No DIO2-as-RF-switch
(NiceRF handles internally)". They are internal in the sense that they are not brought
out to the MCU, but they are LR2021 DIOs wired to the front-end on the module PCB, and
the chip has to be told to drive them - `setRfSwitchTable()` writes that mode mask into
the chip, it does not touch MCU GPIOs. On the sub-GHz path the one that matters is DIO6,
the PA enable, active during TX_LF. Without it the external PA never turns on and you
transmit with the bare chip output. NiceRF's own demo code (LoRa/Core/Src/lr2021.c)
programs DIO6 for sub-GHz and DIO5/DIO7/DIO8 for the 2.4 GHz side, and their support
confirmed that mapping for us.

One trap if you add it: `LR2021::setRfSwitchTable()` indexes the accumulated config by
the table column instead of the DIO number, so the pin array has to stay dense and
ordered from DIO5 - listing DIO6 on its own writes a zero config and the PA stays off
anyway.

**The TCXO may be the cause of your -707.** We configure 3.3 V (matching
`LR20XX_SYSTEM_TCXO_CTRL_3_3V` in NiceRF's demo) and `begin()` succeeds with it - we
print the value that was accepted at boot and it is 3.30 V, so no fallback was taken.
That suggests the module has a TCXO fed from the chip rather than a plain crystal. Worth
noting that -706/-707 is also exactly what a wrong TCXO configuration returns, which is
why the LR2021 support already in `dev` retries `begin()` with 0.0 V on those two codes.
Since you get -707 with `tcxoVoltage = 0`, trying 3.3 V looks like a cheap thing to rule
out before attributing it to the Arduino SPI driver. It may well be both - just that one
is a one-line test.

Separately, and unrelated to the zeros you see: there is a race in the LR11x0/LR2021
read path where the reply to a get command can be read before the chip has prepared it,
and the chip then returns its status stream instead. It shows up as a bogus received
length. We reported it upstream to RadioLib with a reproduction. Mentioning it only
because it lives in the same code path you had to work around.

**A note on the power dial.** With RadioLib's stock PA table the lowest step already
drives this module to roughly +19 dBm at the antenna, so `LORA_TX_POWER=22` means about
+30 dBm (1 W) once the PA is enabled - around 800 mA, and above the limit of most 868
sub-bands. We ended up remapping it so the number means dBm at the module output, which
keeps `set tx` consistent with MeshCore's other high-power boards.

**What we are preparing.** Two pieces, and the first one is deliberately
board-independent so it can be shared: a `NiceRF2021F33` support header under
`src/helpers/radiolib/` holding the module specifics only - RF switch table, the remapped
PA drive table, the PRAM patch loader and a boot report - plus a variant for the XIAO
nRF52840. The header takes no MCU assumptions, so your ESP32-C3 board should be able to
include it unchanged and get the PA enabled without duplicating any of it. Happy to
coordinate so we are not solving the same module twice.

Last practical thing: `src/helpers/radiolib/CustomLR2021.h` and `CustomLR2021Wrapper.h`
have landed in `dev` in the meantime, so a rebase will drop those two files from this PR
and leave the variant, the board json and the HAL.

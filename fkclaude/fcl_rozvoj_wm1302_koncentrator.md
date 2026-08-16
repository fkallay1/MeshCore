# Rozvojová úvaha: koncentrátorový repeater (WM1302) pre kapacitu uplinku

Stav k **2026-08-15**: nič sa nestavia, toto je zápis overených faktov a rozhodnutí,
aby sa k tomu dalo vrátiť bez opakovania rešerše. Nadväzuje na
[`fcl_readme_nicerf_lora2021.md`](fcl_readme_nicerf_lora2021.md).

## Cieľ

Veľa companionov vysiela naraz, navzájom sa rušia a repeater ich nestíha prijímať.
Pôvodná úvaha: **companiony vysielajú LR-FHSS** (odolné voči kolíziám),
**downlink ostáva LoRa**. Repeater by musel byť koncentrátor.

## Čo je overené

### LR2021 vie LR-FHSS len vysielať

Datasheet LR2021 (v demo balíku NiceRF, `61979758.LR2021_V1_1_datasheet.pdf`):

> „…including LoRa, (G)FSK, (G)MSK, FLRC, 4-FSK, O-QPSK and **LR-FHSS (transmit only)**."
>
> „**LR-FHSS is implemented as a transmit-only mode.**"

Potvrdzuje to Semtechov driver v demo kóde — má len `lr20xx_radio_lr_fhss_init`,
`build_frame`, `set_sync_word`, `get_time_on_air_in_ms`, `get_hop_sequence_count`.
Žiadny demodulátor. RadioLib to má rovnako (`LR2021.cpp:339` vráti pri
`PACKET_TYPE_LR_FHSS` rovno `RADIOLIB_ERR_WRONG_MODEM`).

**Dôsledok:** prijímač LR-FHSS *musí* byť koncentrátor. Koncová doska to nikdy
nezvládne, nech je akokoľvek nová.

### Verejný sx1302_hal LR-FHSS nepodporuje

Tri nezávislé kontroly v `Lora-net/sx1302_hal`:

1. `libloragw/inc/loragw_hal.h` — `struct lgw_pkt_rx_s.modulation` môže byť len
   `MOD_CW` (0x08), `MOD_LORA` (0x10), `MOD_FSK` (0x20). **Neexistuje spôsob, ako
   LR-FHSS paket vôbec ohlásiť.** Polia `bandwidth`/`datarate`/`coderate` sú
   komentované ako LoRa-only.
2. `packet_forwarder/global_conf.json.sx1250.EU868` — žiadny FHSS kľúč.
3. `packet_forwarder/src/lora_pkt_fwd.c` — reťazec „fhss" sa v ňom **nevyskytuje
   ani raz**. Parsované kľúče v `SX130x_conf`: `com_type`, `com_path`,
   `lorawan_public`, `clksrc`, `full_duplex`, `antenna_gain`, `fine_timestamp`,
   `sx1261_conf`, `radio_N`, `chan_multiSF_All`, `chan_multiSF_N`, `chan_Lora_std`,
   `chan_FSK`. Nič viac.

Kremík SX1302/SX1303 to zvládne (Semtech to deklaruje), ale treba neverejný
firmware/HAL build, ktorý Semtech dáva výrobcom brán. **Neoverené, či ho pre
WM1302 vôbec vieme získať** — to je jediná otvorená otázka LR-FHSS vetvy.

### Pozor na vymyslené konfigurácie

LLM (Gemini) vygeneroval „funkčný" `global_conf.json` so sekciou `lrfhss_conf`
(`operating_mode`, `grid: 3900`, `nb_channel: 35`). **Sekcia neexistuje** a
forwarder ju ticho ignoruje — neznáme kľúče nespôsobia chybu, takže o tom
nedostaneš žiadny signál. V tom istom súbore boli ďalšie tri chyby:

- `chan_multi_SF_0` namiesto `chan_multiSF_0` → tých 8 kanálov by sa ticho
  nenakonfigurovalo,
- `tx_lut_0..15` na najvyššej úrovni = schéma starého SX1301; v sx1302_hal je to
  pole `tx_gain_lut` **vnútri** objektu `radio_N`,
- `"tx_freq_min": 86300000` = 86,3 MHz, chýba nula.

Fyzikálne parametre (mriežka 3,9 kHz, 137 kHz, DR8/DR9) boli pritom správne — a
práve to robí takú odpoveď nebezpečnou. **Konfiguráciu pre bránu si vždy over
proti parseru, nie proti tomu, ako vyzerá.**

## Čo existuje a dá sa použiť

MeshCore ekosystém má Python vetvu — `meshcore-dev/MeshCore` (tento repo) o
koncentrátoroch nevie, ale:

| Projekt | Čo je | Koncentrátor |
|---|---|---|
| [pyMC_core](https://rightup.github.io/pyMC_core/) | Python reimplementácia MeshCore, RPi + SPI rádio | jednorádiové SX1262 |
| [pyMC_Repeater / openHop](https://github.com/pyMC-dev/pyMC_Repeater) | repeater démon nad pyMC_core | **explicitne nepodporuje** SX1302/SX1303 |
| [**pyMC_WM1303**](https://github.com/hansvanmeer/pyMC_WM1303) | modul robiaci z SX1302/SX1303 **viackanálovú MeshCore bránu** | **áno, toto je ono** |
| [meshpoint](https://github.com/KMX415/meshpoint) | SX1302 base station | pre Meshtastic; MeshCore rieši cez USB companion, early alpha |

**pyMC_WM1303** dáva 6 súbežných kanálov:

- **A–D** — multi-SF demodulátory SX1302, **všetky SF naraz**, napevno 125 kHz
- **E** — pomocné rádio SX1261/SX1262, jedno SF, pružne 62,5–500 kHz
- **F** — širokopásmový demodulátor SX1302, jedno SF, 125–500 kHz

Vysiela tiež (per-kanálové TX fronty, round-robin, CAD cez SX1261/62). Stojí na
`libloragw` HAL v2.10 + `lora_pkt_fwd`, čiže **na verejnej vetve — žiadny
neverejný firmware netreba**. Testované na SenseCAP M1 (Pi 4 + WM1302/WM1303).

## Blokátory a podmienky

| Vec | Stav |
|---|---|
| SX1261/62 na doske (tvrdá podmienka pre CAD) | ✅ **WM1302 ho má** (LBT) — potvrdené 2026-08-15 |
| Šírka pásma multi-SF kanálov | ⚠️ **napevno 125 kHz**, naša sieť beží na **62,5 kHz** |
| LR-FHSS demodulácia | ❌ neverejný firmware, neriešené |
| MeshCore (C++) podpora koncentrátora | ❌ neexistuje, ale netreba — ide sa cez Python vetvu |

### Šírka pásma je hlavné rozhodnutie

Na 62,5 kHz by z celého koncentrátora fungoval len kanál E (jedno rádio, žiadny
zisk). **Aby dávali A–D zmysel, musela by celá sieť prejsť na 125 kHz.** To je
rozhodnutie o sieti, nie o jednom uzle — treba ho spraviť vedome a naraz.

### Mesh vs. hviezda

- **Všetky uzly na jednej frekvencii** → mesh ostáva mesh (uzly sa počujú
  navzájom), brána získa súbežné dekódovanie **rôznych SF**. Rovnaké SF v rovnakom
  čase sa stále zrazia.
- **Uzly rozsypané po kanáloch A–D** → skutočný kapacitný zisk, ale uzly na
  rôznych kanáloch sa navzájom nepočujú a z toho hopu je **hviezda**.

Dá sa kombinovať (spoločná mesh frekvencia + zberné kanály), ale treba to navrhnúť,
nie na to naraziť.

## Záver

**Pôvodný cieľ sa dá dosiahnuť bez LR-FHSS.** Zisk pochádza z paralelných
demodulátorov koncentrátora, nie z modulácie. LR-FHSS by pridal odolnosť pri
hustotách rádovo vyšších, než máme, a je zablokovaný na firmware, ktorý nemáme.

### Ďalší krok, keď na to príde rad

1. Rozhodnúť sa o **125 kHz** pre celú sieť (bez toho nemá zmysel pokračovať).
2. Skúsiť `pyMC_WM1303` na Pi **tak, ako je** — nič nepísať, kým nevidno, ako sa
   správa s našimi uzlami.
3. Až potom riešiť, či a čo dopisovať.

LR-FHSS vetvu otvárať len ak by Semtech/Seeed vydal HAL s demoduláciou. Otázka na
nich má znieť takto (nie „podporuje WM1302 LR-FHSS", na to odpovedia „áno"):

> Does Semtech provide an SX1302/SX1303 Corecell HAL and concentrator firmware
> build with LR-FHSS demodulation support, and under what terms is it available?
> The public `Lora-net/sx1302_hal` has no LR-FHSS support in `loragw_hal.h`
> (only MOD_CW/MOD_LORA/MOD_FSK) and the packet forwarder does not parse any
> LR-FHSS configuration.

Ak by sa raz LR-FHSS otvorilo: vysielacia strana v LR2021 je pár hodín práce
(uložiť LoRa stav → `beginLRFHSS()` → odoslať → obnoviť cez `std_init()` +
`nicerf2021f33_post_init()`; RF switch aj PA tabuľka platia bez zmeny, uzol je
počas toho pár sekúnd hluchý). Overiť sa to dá aj bez brány — SDR záznam a
[jumanamirza/LR-FHSS-receiver](https://github.com/jumanamirza/LR-FHSS-receiver)
(MATLAB, dekóduje LR-FHSS z sx126x/lr1110, čiže aj z LR2021).

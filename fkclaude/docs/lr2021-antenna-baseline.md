# Východisková citlivosť XIAO + NiceRF LR2021 — pred výmenou antény

Zmerané **30. 8. 2026, 07:25–20:31** (13 hodín, build #303, DC-DC vypnuté,
bez jediného výpadku). Slúži ako porovnávacia základňa pri skúšaní inej antény.

## Referenčný vzdialený vysielač: `216D`

V logu sa objavuje v troch podobách podľa dĺžky hashu v ceste — `21` (1 bajt),
`216D` (2 bajty), `216DD2` (3 bajty). Pri vyhodnocovaní treba brať **všetky tri**.

Je to jediný uzol v dosahu, ktorý je na hranici citlivosti; všetko ostatné je
o 50 dB silnejšie (mediány −69 až −47 dBm).

## Namerané hodnoty

| doska | čip | rámcov od `216D` | medián RSSI | medián SNR |
|---|---|---|---|---|
| **XIAO NiceRF** | LR2021 | **1 322** | **−117 dBm** | **+1,8 dB** |
| ProMicro | SX1262 | 35 | −115 dBm | −8,5 dB |
| T1000-E | LR1110 | **0** | — | — |

Rozpis podľa dĺžky hashu na XIAO:

| podoba | rámcov | medián RSSI | medián SNR | min SNR |
|---|---|---|---|---|
| `216D` | 918 | −117 dBm | 1,8 dB | −3,8 dB |
| `21` | 375 | −117 dBm | 1,5 dB | −12,2 dB |
| `216DD2` | 29 | −118 dBm | 0,0 dB | −1,8 dB |

**Poznámka k ProMicro:** v tomto okne bol 8,5 hodiny hluchý (viď watchdog), takže
jeho počet je podhodnotený. Medián SNR −8,5 dB podhodnotený nie je.

## Ako to zmerať znova

Rovnaké okno by malo byť aspoň niekoľko hodín a bez výpadkov — inak sa porovnáva
citlivosť s dostupnosťou. Skupinovať treba podľa **posledného skoku v ceste**, nie
podľa `srch`: `srch` je pôvodca paketu, kdežto RSSI a SNR patria tomu, kto nás
naozaj dosiahol.

```python
m = re.search(r'path\[(\d+)\]=([0-9A-F]+).*?rssi=(-?\d+) snr=(-?[\d.]+)', riadok)
hops, path = int(m.group(1)), m.group(2)
w = len(path)//hops                 # znakov na jeden hash
if path[-w:] in ('21','216D','216DD2'):   # posledny skok
    ...
```

Porovnávať sa dá dvoma spôsobmi a oba majú zmysel:

- **medián SNR** — priamo hovorí o zisku antény, nezávisí od prevádzky
- **počet prijatých rámcov od `216D`** — hovorí, koľko z toho reálne prejde;
  ale iba pri rovnako dlhom a rovnako čistom okne

Pri limitnom SNR okolo +1,8 dB je demodulačná hranica SF7 asi −7,5 dB, takže
rezerva je približne **9 dB**. Anténa horšia o 9 dB by ten uzol stratila úplne —
tak, ako ho dnes nepočuje T1000-E.

## Šumové pozadie — brať `nf=` z AALIVE, nič iné

Doska si šumové pozadie počíta sama a hlási ho v riadku AALIVE ako `nf=`:

```
[FOTA]   AALIVE build #303 ... isr=11496 miss=0 nf=-120
```

**To je to číslo.** Nepočítať si vlastnú štatistiku z `rssi-window` — tá má
`min` aj `max` a ani jedno z nich šumové pozadie nie je:

| zdroj | typicky | čo to je |
|---|---|---|
| `rssi-window min=` | −123 dBm | najtichší okamih z 960 vzoriek, spodný chvost |
| **`nf=` z AALIVE** | **−119 dBm** | **priemer — šumové pozadie** |
| `rssi-window max=` | −118 dBm | najhlučnejší okamih |

Pre poriadok: žiadny prijatý rámec nemá RSSI −123. Najslabšie dekódované sú
okolo −121 dBm, teda **pod** úrovňou šumu — LoRa dekóduje aj pri zápornom SNR,
preto má `216D` SNR okolo nuly.

## SNR sa nedá čítať bez RSSI

V logu sú rámce s `rssi=-44 snr=1.8` — silný signál a pritom nízke SNR. Sú to
pakety s dlhou cestou (8 až 13 skokov), ktoré sa v sieti šíria naraz cez viacero
uzlov a zrazia sa samy so sebou. Nízke SNR teda neznamená automaticky slabý
signál; treba pozerať obe hodnoty.

## Výsledok prvej výmeny: Mikrotik omni + 50 cm kábel navyše, filter ponechaný

Merané **30. 8. 2026**, dve rovnako dlhé okná tesne za sebou (aby sa vylúčil
denný chod), build #303:

| | pred (kolineár) | po (Mikrotik omni) | rozdiel |
|---|---|---|---|
| `nf` | −119 dBm | −120 dBm | −1 dB |
| `216D` rámcov za hodinu | 79 | **121** | +53 % |
| `216D` RSSI medián | −117 dBm | −117 dBm | 0 |
| `216D` SNR medián | +0,5 dB | **+1,0 dB** | +0,5 dB |
| SNR kvartily | −0,5 / 0,5 / 1,2 | 0,8 / 1,0 / 1,5 | dolný +1,3 dB |
| celkovo prijatých | 692 | 707 | bez zmeny |

**Zlepšenie nie je ziskom antény** — RSSI sa nezmenilo. Klesol šum a zúžilo sa
rozdelenie SNR (zmizli najhoršie prípady), takže pri rovnakom signáli prejde viac
rámcov. A to napriek 50 cm kábla navyše, ktorý stojí okolo 0,5 dB.

Jedna hodina pri rozdiele 0,5 dB nie je nezvratná — nočný pokles šumu môže robiť
časť toho zlepšenia. Overiť dlhším oknom porovnateľným s dňom.

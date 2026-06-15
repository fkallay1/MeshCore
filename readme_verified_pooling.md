# Spevnenie testu `ota_test_lora_repeater.py` — VERIFIED-polling + flash

Popis úprav, ktoré spoľahlivo dotiahli end-to-end OTA test (`test_nrf-ota/ota_test_lora_repeater.py`)
na konzistentný **[PASS]**. Pred úpravami `run` občas zlyhal — buď nedosiahol VERIFIED (príjem),
alebo zlyhal flash. Obe príčiny sú nižšie + ako sa riešia.

> Kontext a celkový technický popis: [readme_tech_nrf-ota.md](readme_tech_nrf-ota.md).

---

## Problém 1 — príjem niekedy nedosiahol VERIFIED (timing)

**Pôvodné správanie:** `run` odvysielal fixný počet kôl (`--cycles`, default 4), po každom kole
raz skontroloval `ota status`, a nakoniec spravil **jednu** kontrolu — ak nebol VERIFIED, hneď
`[FAIL]`.

**Prečo zlyhával:** príjem je fire-and-forget a na začiatku (čerstvý boot / nečinné RX okno)
sa **stratí pár paketov**. Chunky sa kumulujú naprieč kolami, ale 4 kolá nemuseli stačiť a
jednorazová finálna kontrola padla skôr, než session dozrela. (Manuálne dovysielanie potom
VERIFIED dosiahlo — čiže to bol len timing, nie chyba príjmu.)

**Riešenie — trpezlivý `broadcast_until_verified()`:**
- Opakuje **broadcast + poll `ota status`** v slučke až do VERIFIED alebo vypršania `--verify-wait`
  (default **60 s**), nie fixný počet kôl.
- Po každom broadcaste spraví viac `ota status` pollov (`--poll-tries`, default 2; každý
  `--poll-secs`, default 5 s) — VERIFIED riadok sa tak nepremešká kvôli krátkemu oknu.
- Sleduje **rast `recv/total`** (`best_recv`) → vidno, či príjem žije, aj keď ešte nie je VERIFIED.
- Po `reboot` repeatera pridaný **settle** (`--reboot-settle`, default 4 s), nech rádio stihne
  armnúť RX, než príde prvý broadcast (inak sa prvé kolo často stratilo).

**Nové argumenty:**
| arg | default | popis |
|-----|---------|-------|
| `--verify-wait` | 60 | max sekúnd opakovať broadcast+poll kým príjem dosiahne VERIFIED |
| `--poll-tries` | 2 | počet `ota status` pollov po každom broadcast kole |
| `--poll-secs` | 5 | sekundy čítania serialu na jeden `ota status` poll |
| `--reboot-settle` | 4 | sekundy po reboote pred prvým broadcastom (RX arm) |
| `--cycles` | 4 | *(legacy)* — príjem teraz riadi `--verify-wait`, nie fixný počet kôl |

---

## Problém 2 — flash občas hardfaultol PO dry-rune (heap)

**Pozorovanie:** automatický `run` robil `ota verify` (dry-run) a hneď `ota flash`. Flash často
zlyhal — flasher sa zastavil hneď po `[FLASHER] Komprimovany format`, **bez** ďalšej hlášky, a
repeater nabehol na **OLD** build. Naproti tomu **manuálny `ota flash` bez predošlého dry-runu
vždy prešiel** (#28→#29, #32→#33).

**Príčina:** `ota verify` (`ota_patch_to_file`) spraví `malloc` + **streaming rekonštrukciu celého
~442 kB FW** (puff_stream + hpatch_lite_patch + 2 kB stack cache). Po ňom nasledujúci
`ota flash` (`ota_apply`) **hardfaultne na `malloc(patch_size)`** — t.j. heap je po dry-rune v
stave, ktorý rozbije ďalšiu alokáciu (hardfault, nie čistý `malloc==NULL`, preto žiadna hláška;
zariadenie sa resetne → boot OLD). Detail v
[readme_tech_nrf-ota.md](readme_tech_nrf-ota.md) §8.2.

**Riešenie v teste:** dry-run pred flashom je teraz **opt-in** (`--verify-first`, default **vyp**).
`run` ide po VERIFIED rovno na `ota flash`. Bezpečnosť to neoslabuje — `ota flash` si **sám**
overuje base-FW (SHA256 bežiaceho flashu vs `old_sha256` z patchu) a odmietne flash pri nezhode;
a samotný VERIFIED stav už potvrdil SHA256 zostaveného patchu. Plný dry-run sa dá kedykoľvek
spustiť manuálne (`ota verify`) alebo v teste cez `--verify-first`.

| arg | default | popis |
|-----|---------|-------|
| `--verify-first` | (vyp) | spustiť `ota verify` (dry-run) pred ostrým flashom; default vyp kvôli heap hardfaultu |

> **TODO (firmware):** opraviť heap stav tak, aby `ota verify` + `ota flash` bezpečne fungovali
> za sebou (napr. nealokovať/uvoľniť čistejšie v `ota_patch_to_file`, alebo medzi nimi `malloc`
> arénu zresetovať). Dovtedy default = bez dry-runu.

---

## Výsledok
Po oboch úpravách `run` dosiahol konzistentný **[PASS]** (#34→#36, FNV-1a sedí, repeater nabehol
na NEW build). Príklad spustenia:

```bash
PENV=~/.platformio/penv/Scripts/python.exe
$PENV test_nrf-ota/ota_test_lora_repeater.py baseline --bridge-port COM3 --target-port COM5
$PENV test_nrf-ota/ota_test_lora_repeater.py run      --bridge-port COM3 --target-port COM5 --verify-wait 70
# voliteľne pridať bezpečnostný dry-run pred flashom (pozor na heap hardfault):
#   ... run ... --verify-first
```

Pozn.: pri reflashe OLD (baseline) sa raz prefs repeatera vrátili na SK preset — pred `run` over
že repeater je na CZ (`set radio 869.525,62.5,7,5` + reboot), inak sa s CZ bridge nepočujú.

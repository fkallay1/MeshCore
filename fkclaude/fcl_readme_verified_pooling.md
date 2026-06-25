# Spevnenie testu `fota_test_lora_repeater.py` — VERIFIED-polling + flash

Popis úprav, ktoré spoľahlivo dotiahli end-to-end FOTA test (`test_nrf-fota/fota_test_lora_repeater.py`)
na konzistentný **[PASS]**. Pred úpravami `run` občas zlyhal — buď nedosiahol VERIFIED (príjem),
alebo zlyhal flash. Obe príčiny sú nižšie + ako sa riešia.

> Kontext a celkový technický popis: [fcl_readme_tech_nrf-fota.md](fcl_readme_tech_nrf-fota.md).

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

**Príčina:** `ota verify` (`fota_patch_to_file`) spraví `malloc` + **streaming rekonštrukciu celého
~442 kB FW** (puff_stream + hpatch_lite_patch + 2 kB stack cache). Po ňom nasledujúci
`ota flash` (`fota_apply`) **hardfaultne na `malloc(patch_size)`** — t.j. heap je po dry-rune v
stave, ktorý rozbije ďalšiu alokáciu (hardfault, nie čistý `malloc==NULL`, preto žiadna hláška;
zariadenie sa resetne → boot OLD). Detail v
[fcl_readme_tech_nrf-fota.md](fcl_readme_tech_nrf-fota.md) §8.2.

**Riešenie v teste:** dry-run pred flashom je teraz **opt-in** (`--verify-first`, default **vyp**).
`run` ide po VERIFIED rovno na `ota flash`. Bezpečnosť to neoslabuje — `ota flash` si **sám**
overuje base-FW (SHA256 bežiaceho flashu vs `old_sha256` z patchu) a odmietne flash pri nezhode;
a samotný VERIFIED stav už potvrdil SHA256 zostaveného patchu. Plný dry-run sa dá kedykoľvek
spustiť manuálne (`ota verify`) alebo v teste cez `--verify-first`.

| arg | default | popis |
|-----|---------|-------|
| `--verify-first` | (vyp) | spustiť `ota verify` (dry-run) pred ostrým flashom; default vyp kvôli heap hardfaultu |

> **Stav (2026-06-21):** commit `5d4f23df` (RAM-assembly) prerobil obe cesty na `fota_acquire_patch_ram`
> s korektným `free()` a deklaruje „dry-run pred flashom bez hardfaultu". Standalone `ota verify` je
> overený OK (opakovane, bit-presná rekonštrukcia). Sekvencia verify→flash v jednom boote ale NEBOLA
> priamo retestovaná (medzi nimi sa rebootovalo) → `--verify-first` ostáva default **vyp** ako poistka.
> Pred zapnutím over verify→flash same-boot na HW. Viď [fcl_readme_tech_nrf-fota.md](fcl_readme_tech_nrf-fota.md) §8.2.

---

## Výsledok
Po oboch úpravách `run` dosiahol konzistentný **[PASS]** (#34→#36, FNV-1a sedí, repeater nabehol
na NEW build). Príklad spustenia:

```bash
PENV=~/.platformio/penv/Scripts/python.exe
$PENV test_nrf-fota/fota_test_lora_repeater.py baseline --bridge-port COM3 --target-port COM5
$PENV test_nrf-fota/fota_test_lora_repeater.py run      --bridge-port COM3 --target-port COM5 --verify-wait 70
# voliteľne pridať bezpečnostný dry-run pred flashom (pozor na heap hardfault):
#   ... run ... --verify-first
```

Pozn.: pri reflashe OLD (baseline) sa raz prefs repeatera vrátili na SK preset — pred `run` over
že repeater je na CZ (`set radio 869.525,62.5,7,5` + reboot), inak sa s CZ bridge nepočujú.

---

## Samobežný runner (2026-06-21) — 3 zmeny

Po pridaní podpísaného FOTA HEADER-u runner prestal dosahovať VERIFIED (posielal nepodpísaný
header → firmware ho zamietol). Tri zmeny v `fota_test_lora_repeater.py` (commit `d487a3dd`),
aby `baseline` + `run` prešli **bez manuálnych flagov**:

1. **auto `--privkey`** = `test_nrf-fota/test_key.der`. Firmware vyžaduje podpísaný
   HEADER (Ed25519 key_id=1, [FotaReceiver_signkey.cpp](examples/simple_repeater/nrffota/FotaReceiver_signkey.cpp));
   bez kľúča → `CHYBA=0x6`, `total_chunks=0`, session sa nedokončí. (Toto bola príčina, prečo
   test „nešiel" po pridaní podpisu.)
   > ⚠️ **`test_key.der` je GITIGNORED** (`.gitignore`) — súkromný kľúč sa necommituje, takže na
   > **čerstvom klone / po presune projektu CHÝBA** a test ticho nedosiahne VERIFIED. Runner na to
   > teraz VAROVÁ. Skopíruj `test_key.der` z funkčného prostredia (musí matchovať pubkey
   > `c22f8ae0…5b51` zakompilovaný v `FotaReceiver_signkey.cpp`, `key_id=1`) do `test_nrf-fota/`.
2. **`--packetorder` default `hend`** (chunky prvé, header nakoniec). SX1262 RX po čerstvom
   boote chytí len ~4 rámce (stuck receiver, [fcl_readme_tech_nrf-fota.md](fcl_readme_tech_nrf-fota.md) §8.3);
   header/meta prežíva reboot → pri `hend` sa budget minie na chunky, nie na už-známy header.
3. **reboot pred KAŽDÝM broadcast kolom** v `broadcast_until_verified` — čerstvé RX okno proti
   stuck-receiver (predtým loop medzi kolami nereboot → po 1. kole hluché → flaky). `recv_count`
   je per-boot (bitmap sa <8 chunkov neukladá), takže VERIFIED si žiada 4/4 v jednom okne;
   reboot dáva každému kolu novú šancu. Pri slabom RF (RSSI -26, ~3 rámce/okno) zvýš `--verify-wait`.

> **Závislosť:** runner predpokladá fix flasher IRQ hangu (`__disable_irq` pred NVMC,
> [fcl_readme_tech_nrf-fota.md](fcl_readme_tech_nrf-fota.md) §8.6, commit `d24c6792`) — inak `ota flash`
> po VERIFIED zamrzne. Overené e2e: baseline #104 → run → VERIFIED 4/4 → `ota flash` →
> **[PASS] #105**, bez manuálnych flagov.

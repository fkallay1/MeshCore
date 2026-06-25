# OTA → FOTA rename — handoff pre Flutter appku a ostatné nástroje

**Branch:** `features/nrf-fota` (odvodený z `features/nrf-ota`).
**Dátum:** 2026-06-25.
**Rozsah:** premenovanie nášho LoRa-FOTA systému (delta-patch OTA pre nRF52840 repeater)
z `OTA`/`ota` na `FOTA`/`fota` — kód, súbory, adresáre, python, build, formát balíčkov.
NEtýka sa upstream MeshCore „OTA" (BLE DFU) — to ostáva.

---

## 1. Wire protokol — ZACHOVANÝ (žiadna zmena, plná spätná kompatibilita)

Tieto hodnoty sa **nezmenili** (premenovali sa len NÁZVY C symbolov, nie hodnoty),
takže komunikácia medzi zariadeniami aj `meshcore_py`/companion senderom funguje
ďalej bez úprav:

| Vec | Hodnota (nezmenená) | C symbol (premenovaný) |
|---|---|---|
| LoRa kanál | `#fkotanrf` (hash `0xA4`) | `OTA_CHANNEL_NAME` → `FOTA_CHANNEL_NAME` |
| GRP_DATA data_type (gating) | `0x07A0` | `OTA_MAGIC` → `FOTA_MAGIC` |
| Payload typy | `0x10`–`0x21` | `OTA_PKT_*` → `FOTA_PKT_*` |
| Protokol verzia | `0x00` | `OTA_PROT_INF_V0` → `FOTA_PROT_INF_V0` |
| Chunk max / NACK max | 144 / 60 | `OTA_CHUNK_DATA_MAX`, `OTA_NACK_MAX_IDX` → `FOTA_*` |
| Štruktúry META/SIG/CHUNK | rovnaký byte layout | `OtaHeaderPkt` → `FotaHeaderPkt` atď. |
| AES/HMAC/Ed25519 logika | nezmenená | — |

**Záver pre meshcore_py / companion / iné LoRa odosielače: NIČ netreba meniť.**

---

## 2. CLI príkaz — `ota` aj `fota` (keep-both)

Zariadenie (repeater) prijíma **oba** tvary admin príkazu:

```
ota status | ota flash | ota clear | ota id | ...      ← legacy, stále funguje
fota status | fota flash | fota clear | fota id | ...   ← nový, preferovaný
```

V FW kóde sú miesta aliasu označené komentárom **`FOTA-CLI-ALIAS`**
(`examples/simple_repeater/MyMesh.cpp`). Keď Flutter/skripty prejdú na `fota`,
stačí zmazať tie vetvy.

**Odporúčanie pre Flutter / nástroje:** prejsť na `fota …`, ale netreba to robiť
hneď — `ota …` ostane funkčné, kým alias nezmažeme.

---

## 3. Balíčkový formát `.fotapkg.json` — ZMENENÝ (Flutter MUSÍ updatnúť)

Generátor `gen_fotapkg.py` (pôvodne `gen_otapkg.py`) teraz produkuje:

| Vec | Pôvodné | Nové |
|---|---|---|
| Prípona súboru | `*.otapkg.json` | `*.fotapkg.json` |
| `format` magic | `mc-fotanrf-otapkg/1` | `mc-fotanrf-fotapkg/1` |
| Názvy súborov | `<old>-<new>.<dev>.otapkg.json` | `…fotapkg.json` |
| Výstupný adresár | `fotapkg_json/` | `fotapkg_json/` (bez zmeny) |

**JSON kľúče sa NEZMENILI** (neobsahovali „ota"): `format`, `created`,
`channel{name,idx}`, `radio{freq,bw,sf,cr}`, `scope`, `path`,
`fw{old_sha256,new_sha256,old_fw_size,patch_sha256,patch_len}`, `patch_b64`,
`signed{key_id,meta_b64,sig_b64}`.

### Čo musí Flutter appka (`mc_fotanrf_flutterapp`) upraviť
1. **Čítať príponu `.fotapkg.json`** (ideálne akceptovať aj starú `.otapkg.json`
   pre už vyexportované balíčky).
2. **Akceptovať `format` == `mc-fotanrf-fotapkg/1`** (ideálne aj `…-otapkg/1`).
3. **Parsovanie polí, odosielanie cez LoRa, kanál, podpisy — BEZ ZMENY** (wire je rovnaký).
4. (Voliteľné) admin príkazy prepnúť `ota …` → `fota …`.

---

## 4. Premenované súbory / adresáre / moduly (pre skripty a CI)

**Adresáre:**
- `examples/simple_repeater/nrfota/` → `examples/simple_repeater/nrffota/`
- `test_nrf-ota/` → `test_nrf-fota/`

**C/H súbory** (`nrffota/`): `OtaMesh`, `OtaReceiver`, `OtaReceiver_signkey`,
`OtaPatcher`, `OtaState`, `OtaFs`, `OtaProtocol` → `Fota*`.
Nezmenené: `FwId.{h,cpp}`, `flash_layout.h`, `flasher_code*.h`, `puff_stream.*`, `hpatchlite/*`.

**Python moduly** (`test_nrf-fota/`):
- `ota_sender.py` → `fota_sender.py`
- `ota_export_pkg.py` → `fota_export_pkg.py`
- `ota_sender_mcpy.py` → `fota_sender_mcpy.py`
- `ota_test_lora_repeater.py` → `fota_test_lora_repeater.py`
- `gen_otapkg.py` → `gen_fotapkg.py`
- `gen_otapkg_hook.py` → `gen_fotapkg_hook.py`
- `push_otapkg.py/.bat` → `push_fotapkg.py/.bat`
- `tests/test_ota_format.py` → `tests/test_fota_format.py`
- nezmenené: `gen_build_info.py`, `gen_fw_trailer.py`

**Build (`platformio.ini`):**
- `-D WITH_LORA_OTA` → `-D WITH_LORA_FOTA`
- `-D OTA_CHANNEL_NAME` → `-D FOTA_CHANNEL_NAME`
- `-D OTA_SOFTDEVICE_V7` → `-D FOTA_SOFTDEVICE_V7`
- include/`build_src_filter`/`extra_scripts` cesty → `nrffota` / `test_nrf-fota`

**C identifikátory:** `OTA_*` → `FOTA_*` (makrá/enum), `Ota*` → `Fota*` (typy),
`ota_*()` → `fota_*()` (funkcie), `OTA_FLASHER_BUILD` → `FOTA_FLASHER_BUILD`.

**Python identifikátory:** `OTA_CHANNEL_NAME` → `FOTA_CHANNEL_NAME`,
`OTA_CHUNK_DATA` → `FOTA_CHUNK_DATA`, `ota_crc16` → `fota_crc16`, atď.

---

## 5. Zámerne PONECHANÉ ako bare `ota`/`OTA` (nie identifikátory)

- LittleFS cesty na zariadení: `/ota`, `/ota/meta.bin`, … (device-local; zmena by
  osirela staré súbory pri upgrade — zbytočné). Premenované sú len makrá `FOTA_FS_*`.
- Magic perzistentnej meta: hodnota `0x4F544101` ("OTA\x01") — device-local.
- Kanál `#fkotanrf` (wire).
- Log prefixy v knižniciach hpatchlite/puff (cudzí kód) — netýka sa.

---

## 6. Stav

- FW build `SenseCap_Solar_repeater_fota`: **PASS** (#153).
- Python testy `test_nrf-fota/tests/`: **9/9 PASS**.
- Flasher bloby `flasher_code_v6.h` / `_v7.h`: regenerované, v6≠v7.
- `gen_fotapkg.py` end-to-end: produkuje `.fotapkg.json` + `.bin` + `.uf2`.

Pôvodný (predrenamový) dobrý stav je commitnutý na `features/nrf-ota`
(commit `6aa1588e`), tento rename je na `features/nrf-fota`.

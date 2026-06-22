# OTA cez companion (meshcore_py) — implementačný plán

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Posielať OTA delta-patch na `simple_repeater/nrfota` cez štandardný MeshCore companion (meshcore_py, serial COM3) aj cez doterajší FK_lora bridge, jedným zjednoteným on-air formátom; e2e zerohop test pre obe cesty.

**Architecture:** Jeden on-air GRP_DATA plaintext `[OTA_MAGIC 2B][len 1B][ts 4B][ota_payload]` pre obe cesty. HEADER rozdelený na META (102 B, podpisované) + SIG (99 B). Receiver odlúpne `[data_type][len]` v `onGroupDataRecv` → zvyšok je legacy tvar `[ts][ota_payload]`. Companion šifruje/smeruje sám (`CMD_SEND_CHANNEL_DATA`), bridge posiela hotový raw paket. Firmware sa na embedded nedá host-testovať → firmware tasky = edit + `pio run` (kompilácia) + on-device e2e (Fáza E). Python časť = host pytest (TDD).

**Tech Stack:** PlatformIO (nRF52840), C++ (MeshCore), Python 3 (pyserial, pycryptodome, meshcore_py), Ed25519, AES-128-ECB + HMAC-SHA256, hdiffi+zlib.

## Global Constraints

- `MAX_PACKET_PAYLOAD = 184`, `MAX_GROUP_DATA_LENGTH = 165` → `data = [ts 4B] + ota_payload ≤ 165` ⇒ `ota_payload ≤ 161 B`. KAŽDÝ typ paketu to musí splniť.
- OTA kanál: meno `#fkotanrf`, secret `2382c5b811d390667e6a7800c03338ca` = `SHA256("#fkotanrf")[0:16]`, gating `hash[0]=0xA4` = `SHA256(secret)[0]`.
- `OTA_MAGIC = 0x07A0` (16-bit, ≠ `DATA_TYPE_RESERVED 0x0000`). `OTA_PKT_HDR_SIG = 0x13`. `OTA_PROT_INF_V0 = 0x00`. `OTA_CHUNK_DATA = 144`.
- Podpis = Ed25519 nad presne 102 B META; pubkey v `OtaReceiver_signkey.cpp` (`s_authors[key_id]`) NEMENIŤ. `test_nrf-ota/test_key.der` (gitignored) musí existovať lokálne.
- Embedded disciplína: žiadna dynamická alokácia mimo `setup()/begin()`; nereformátovať existujúci kód; držať zmeny nízko-úrovňovo a stručne.
- HW: COM3 = Seeed XIAO nRF52840 (bridge ALEBO companion), COM5 = ProMicro nRF52840 (`ProMicro_repeater_ota`). Python cez penv: `D:\FkDev\.platformio\penv\Scripts\python.exe` (pyserial+pycryptodome). meshcore_py inštalovať doň.
- Companion encryptuje sám → `ota_sender_mcpy.py` NEROBÍ AES/HMAC. Bridge `ota_sender.py` šifruje (zostáva AES/HMAC).
- `OtaReceiver.cpp` referencie sú k aktuálnemu stavu (pred zmenami): `handle_header@500`, `handle_chunk@607`, `ota_process@784`, `verify_header_signature@17`.

---

## Fáza A — Zdieľaný Python builder + offline ekvivalencia (host, TDD)

### Task A1: Nové formátové konštanty a builder funkcie v `ota_sender.py`

**Files:**
- Modify: `test_nrf-ota/ota_sender.py` (konštanty ~51-74; `build_grpdata_payload`@155; HEADER builder @307/376; chunk @329)
- Test: `test_nrf-ota/tests/test_ota_format.py` (create)

**Interfaces:**
- Produces:
  - `OTA_MAGIC = 0x07A0`, `OTA_PKT_HEADER=0x10`, `OTA_PKT_HDR_SIG=0x13`, `OTA_PKT_CHUNK=0x11`, `OTA_PKT_APPLY=0x12`, `OTA_PROT_INF_V0=0x00`, `OTA_CHUNK_DATA=144`, `OTA_CHANNEL_NAME="#fkotanrf"`
  - `def build_meta_payload(total_chunks, patch_size, patch_sha256, new_sha256, old_sha256) -> bytes` (102 B; `total_chunks` arg ignorovaný v bajtoch — odvodené, ponechaný pre kompat. signatúry/logu)
  - `def build_sig_payload(meta: bytes, privkey, key_id: int) -> bytes` (99 B; podpis nad `meta`)
  - `def ota_channel_secret(name: str=OTA_CHANNEL_NAME) -> bytes` (16 B)
  - `def build_ota_chunk(idx, data, old_fw_size, old_sha256_prefix) -> bytes` (zmenené len cez `OTA_CHUNK_DATA`)

- [ ] **Step 1: Napíš padajúci test** `test_nrf-ota/tests/test_ota_format.py`

```python
import hashlib, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ota_sender as S

def test_constants():
    assert S.OTA_MAGIC == 0x07A0
    assert S.OTA_PKT_HDR_SIG == 0x13
    assert S.OTA_CHUNK_DATA == 144
    assert S.OTA_CHANNEL_NAME == "#fkotanrf"

def test_channel_secret_matches_known():
    assert S.ota_channel_secret().hex() == "2382c5b811d390667e6a7800c03338ca"

def test_meta_payload_layout():
    ps = hashlib.sha256(b"p").digest(); ns = hashlib.sha256(b"n").digest(); os_ = hashlib.sha256(b"o").digest()
    meta = S.build_meta_payload(7, 1234, ps, ns, os_)
    assert len(meta) == 102
    assert meta[0] == S.OTA_PKT_HEADER and meta[1] == S.OTA_PROT_INF_V0
    assert int.from_bytes(meta[2:6], "little") == 1234
    assert meta[6:38] == ps and meta[38:70] == ns and meta[70:102] == os_
    assert 4 + len(meta) <= 165   # data_len limit

def test_sig_payload_layout_and_verify():
    from Crypto.PublicKey import ECC
    from Crypto.Signature import eddsa
    key = ECC.generate(curve="ed25519")
    ps = hashlib.sha256(b"p").digest(); ns = hashlib.sha256(b"n").digest(); os_ = hashlib.sha256(b"o").digest()
    meta = S.build_meta_payload(7, 1234, ps, ns, os_)
    sig = S.build_sig_payload(meta, key, key_id=1)
    assert len(sig) == 99 and 4 + len(sig) <= 165
    assert sig[0] == S.OTA_PKT_HDR_SIG and sig[1] == S.OTA_PROT_INF_V0
    assert sig[2:34] == os_ and sig[34] == 1
    eddsa.new(key.public_key(), "rfc8032").verify(meta, sig[35:99])  # raises on bad
```

- [ ] **Step 2: Spusti — má zlyhať**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -m pytest test_nrf-ota/tests/test_ota_format.py -v`
Expected: FAIL (`AttributeError: module 'ota_sender' has no attribute 'OTA_MAGIC'`)

- [ ] **Step 3: Implementuj**

V `ota_sender.py` k existujúcim konštantám pridaj:
```python
OTA_MAGIC          = 0x07A0
OTA_PKT_HDR_SIG    = 0x13
OTA_PROT_INF_V0    = 0x00
OTA_CHUNK_DATA     = 144          # bolo 150 — limit GRP_DATA data_len ≤165
OTA_CHANNEL_NAME   = "#fkotanrf"
```
(Ak existuje `OTA_CHUNK_DATA = 150`, prepíš na 144. `OTA_CHUNK_DATA_MAX`/`OTA_CHUNK_DATA` referencie zladiť na 144.)

Pridaj funkcie:
```python
def ota_channel_secret(name: str = OTA_CHANNEL_NAME) -> bytes:
    return hashlib.sha256(name.encode("utf-8")).digest()[:16]

def build_meta_payload(total_chunks: int, patch_size: int,
                       patch_sha256: bytes, new_sha256: bytes, old_sha256: bytes) -> bytes:
    # META (102 B) = podpisovaná správa. total_chunks sa NEposiela (odvodí sa
    # z patch_size/OTA_CHUNK_DATA), old_sha256_prefix sa NEposiela (= old_sha256[:4]).
    msg = (bytes([OTA_PKT_HEADER, OTA_PROT_INF_V0])
           + struct.pack('<I', patch_size)
           + patch_sha256 + new_sha256 + old_sha256)
    assert len(msg) == 102, f"META musi byt 102B, je {len(msg)}"
    return msg

def build_sig_payload(meta: bytes, privkey, key_id: int) -> bytes:
    sig = sign_ota_header(meta, privkey) if privkey else bytes(64)
    old_sha256 = meta[70:102]
    out = bytes([OTA_PKT_HDR_SIG, OTA_PROT_INF_V0]) + old_sha256 + bytes([key_id]) + sig
    assert len(out) == 99, f"SIG musi byt 99B, je {len(out)}"
    return out
```
Uprav `build_ota_chunk` (riadok ~329): ponechaj logiku, len chunkovanie patchu používa `OTA_CHUNK_DATA` (riadok ~351 `range(0, len(patch), OTA_CHUNK_DATA)` namiesto `OTA_CHUNK_DATA`/`OTA_CHUNK_DATA_MAX` 150). `sign_ota_header` (existuje @101) podpisuje ľubovoľný `bytes` message — funguje aj pre 102 B META.

- [ ] **Step 4: Spusti — má prejsť**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -m pytest test_nrf-ota/tests/test_ota_format.py -v`
Expected: PASS (4 passed)

- [ ] **Step 5: Commit**

```bash
git add test_nrf-ota/ota_sender.py test_nrf-ota/tests/test_ota_format.py
git commit -m "feat(nrfota): zdielany OTA format v0 (META/SIG, magic, chunk144)"
```

---

### Task A2: Zjednotené GRP_DATA rámcovanie + ekvivalencia bridge↔companion

**Files:**
- Modify: `test_nrf-ota/ota_sender.py` (`build_grpdata_payload`@155)
- Test: `test_nrf-ota/tests/test_grpdata_framing.py` (create)

**Interfaces:**
- Consumes: `build_meta_payload`, `build_sig_payload`, `OTA_MAGIC`, `ota_channel_secret` (Task A1)
- Produces:
  - `def grpdata_plaintext(ota_payload: bytes, ts: int) -> bytes` — `[OTA_MAGIC 2B LE][len 1B = 4+len][ts 4B LE][ota_payload]`
  - `def companion_grpdata_plaintext(data_type: int, data: bytes) -> bytes` — replika firmware `sendGroupData` (`[data_type 2B][len 1B][data]`), pre testy
  - `build_grpdata_payload(psk, ota_payload, ts=None)` → `[ch_hash][MAC][AES(grpdata_plaintext)]`

- [ ] **Step 1: Napíš padajúci test** `test_nrf-ota/tests/test_grpdata_framing.py`

```python
import sys, struct, hashlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ota_sender as S

def test_bridge_plaintext_equals_companion_wrapping():
    # data čo companion CMD_SEND_CHANNEL_DATA posiela = [ts4][ota_payload]
    ota_payload = bytes([S.OTA_PKT_CHUNK]) + b"\x00"*12 + b"X"*144   # plný chunk
    ts = 0x11223344
    bridge_plain = S.grpdata_plaintext(ota_payload, ts)
    data = struct.pack('<I', ts) + ota_payload
    companion_plain = S.companion_grpdata_plaintext(S.OTA_MAGIC, data)
    assert bridge_plain == companion_plain          # bajt-identické
    assert len(data) <= 165                          # firmware limit

def test_all_types_fit_165():
    ps = ns = os_ = b"\x00"*32
    meta = S.build_meta_payload(1, 100, ps, ns, os_)
    sig  = S.build_sig_payload(meta, None, 1)
    chunk = bytes([S.OTA_PKT_CHUNK]) + b"\x00"*12 + b"X"*144
    for p in (meta, sig, chunk):
        assert 4 + len(p) <= 165
```

- [ ] **Step 2: Spusti — má zlyhať**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -m pytest test_nrf-ota/tests/test_grpdata_framing.py -v`
Expected: FAIL (`AttributeError: ... 'grpdata_plaintext'`)

- [ ] **Step 3: Implementuj**

V `ota_sender.py` pridaj a uprav `build_grpdata_payload`:
```python
def grpdata_plaintext(ota_payload: bytes, ts: int) -> bytes:
    data = struct.pack('<I', ts) + ota_payload
    return struct.pack('<HB', OTA_MAGIC, len(data)) + data

def companion_grpdata_plaintext(data_type: int, data: bytes) -> bytes:
    # replika BaseChatMesh::sendGroupData temp[] (len test ekvivalencie)
    return struct.pack('<HB', data_type, len(data)) + data

def build_grpdata_payload(psk: bytes, ota_payload: bytes, ts: int | None = None) -> bytes:
    ch_hash = hashlib.sha256(psk).digest()[0]   # POZN: pri #fkotanrf psk = ota_channel_secret()
    if ts is None:
        ts = int(time.time()) & 0xFFFFFFFF
    plain = grpdata_plaintext(ota_payload, ts)
    return bytes([ch_hash]) + meshcore_encrypt(psk, plain)
```
POZOR: gating hash sa teraz počíta `sha256(secret)[0]` kde `secret = ota_channel_secret()`. Volajúci musí dodať `psk = ota_channel_secret()` (nie reťazec mena). To zladí Task D1.

- [ ] **Step 4: Spusti — má prejsť**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -m pytest test_nrf-ota/tests/ -v`
Expected: PASS (všetky)

- [ ] **Step 5: Commit**

```bash
git add test_nrf-ota/ota_sender.py test_nrf-ota/tests/test_grpdata_framing.py
git commit -m "feat(nrfota): zjednotene GRP_DATA ramcovanie + bridge/companion ekvivalencia"
```

---

### Task A3: Bridge `send_ota` — META+SIG namiesto jedného HEADER

**Files:**
- Modify: `test_nrf-ota/ota_sender.py` (`send_ota`@343, najmä HEADER blok @376-411, `send_header`@392)

**Interfaces:**
- Consumes: `build_meta_payload`, `build_sig_payload`, `grpdata_plaintext` (A1/A2)
- Produces: `send_ota(...)` posiela 2 hlavičkové pakety (META, SIG) cez existujúcu `send_pkt`; `_hdr_sent` → `_meta_sent`/`_sig_sent`.

- [ ] **Step 1: Implementuj** (mechanická zmena, bez host testu — overí Fáza E)

V `send_ota`:
- Nahraď zostavenie `otbmsg` (107 B) volaním `meta = build_meta_payload(total, len(patch), patch_sha256, new_sha256, old_sha256)`.
- `sig_payload = build_sig_payload(meta, privkey, key_id)`.
- `send_header()` → `send_hdr()` posiela DVA pakety: `send_pkt(meta)`, potom `send_pkt(sig_payload)` (medzi nimi krátky `time.sleep(chunk_delay)`), nastav `_meta_sent=_sig_sent=True`.
- `header_every` redundancia: posiela META aj SIG.
- `send_pkt` ostáva (mode meshcore/serial-direct/direct) — len `ota_payload` je teraz META alebo SIG; pre `mode=='meshcore'` `meshcore_grp_data_packet` použije `build_grpdata_payload` (A2) s `ts` per paket.
- Odstráň starý `build_ota_header` (172 B) ak už nie je volaný (alebo nechaj nevyužitý — preferuj odstránenie kvôli DRY).

- [ ] **Step 2: Sanity beh** (vygeneruje pakety, neodosiela — over že nepadne import/štruktúra)

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -c "import sys; sys.path.insert(0,'test_nrf-ota'); import ota_sender"`
Expected: bez chyby

- [ ] **Step 3: Commit**

```bash
git add test_nrf-ota/ota_sender.py
git commit -m "feat(nrfota): bridge sender posiela META+SIG (rozdeleny HEADER)"
```

---

## Fáza B — `ota_sender_mcpy.py` (companion, meshcore_py)

### Task B1: Inštalácia meshcore_py do penv

**Files:** žiadne (prostredie)

- [ ] **Step 1:** Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -m pip install meshcore`
  - Ak PyPI verzia nesedí s klonom, alternatíva: `... -m pip install -e D:\FkDev\FkProj\VSC\meshcore_py`
- [ ] **Step 2:** Over: `D:\FkDev\.platformio\penv\Scripts\python.exe -c "from meshcore import MeshCore; from meshcore.events import EventType; print('ok')"`
  Expected: `ok`
- [ ] **Step 3:** Commit (žiadny — len prostredie). Pokračuj.

---

### Task B2: `ota_sender_mcpy.py` — async sender cez companion

**Files:**
- Create: `test_nrf-ota/ota_sender_mcpy.py`
- Test: `test_nrf-ota/tests/test_mcpy_frame.py` (create)

**Interfaces:**
- Consumes (import z `ota_sender`): `make_patch`, `build_meta_payload`, `build_sig_payload`, `build_ota_chunk`, `build_ota_apply`, `load_ed25519_privkey`, `OTA_*`, `OTA_CHANNEL_NAME`, `OTA_CHUNK_DATA`
- Produces:
  - `def companion_chan_data_frame(channel_idx, path_len, path, data_type, data) -> bytes` — `[62][channel_idx][path_len][path][data_type 2B LE][data]`
  - `def scope_to_path(scope, path_bytes=b"") -> tuple[int, bytes]` — `zerohop→(0,b"")`, `flood→(0xFF,b"")`, `direct→(len(path)//hsz, path)`
  - `async def run_mcpy(...)` hlavná session

- [ ] **Step 1: Napíš padajúci test** `test_nrf-ota/tests/test_mcpy_frame.py`

```python
import sys, struct
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ota_sender_mcpy as M

def test_chan_data_frame_zerohop():
    data = struct.pack('<I', 0x11223344) + b"\x10payload"
    f = M.companion_chan_data_frame(channel_idx=1, path_len=0, path=b"", data_type=0x07A0, data=data)
    assert f[0] == 62 and f[1] == 1 and f[2] == 0
    assert f[3:5] == struct.pack('<H', 0x07A0)
    assert f[5:] == data

def test_scope_to_path():
    assert M.scope_to_path("zerohop") == (0, b"")
    assert M.scope_to_path("flood") == (0xFF, b"")
    assert M.scope_to_path("direct", b"\x63\x68") == (2, b"\x63\x68")
```

- [ ] **Step 2: Spusti — má zlyhať**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -m pytest test_nrf-ota/tests/test_mcpy_frame.py -v`
Expected: FAIL (`ModuleNotFoundError: ota_sender_mcpy`)

- [ ] **Step 3: Implementuj** `test_nrf-ota/ota_sender_mcpy.py`

```python
#!/usr/bin/env python3
"""ota_sender_mcpy.py — OTA sender cez MeshCore companion (meshcore_py, serial).

Companion (Xiao_nrf52_companion_radio_usb na COM3) šifruje a smeruje sám cez
CMD_SEND_CHANNEL_DATA. Tento sender NEROBÍ AES/HMAC — len zostaví OTA payload
(META/SIG/chunk) a pošle ho ako GRP_DATA `data = [ts4][ota_payload]`.

  pip install meshcore pycryptodome
  python ota_sender_mcpy.py --old old.bin --new new.bin --port COM3 \
        --channel-name "#fkotanrf" --channel-idx 1 --scope zerohop \
        --privkey test_nrf-ota/test_key.der --keyid 1 --reboot
"""
import argparse, asyncio, struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ota_sender as S
from ota_sender import (make_patch, build_meta_payload, build_sig_payload,
                        build_ota_chunk, build_ota_apply, load_ed25519_privkey,
                        OTA_MAGIC, OTA_CHUNK_DATA, OTA_CHANNEL_NAME, OTA_PKT_APPLY)

for _s in (sys.stdout, sys.stderr):
    try: _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception: pass

CMD_SEND_CHANNEL_DATA = 62
OUT_PATH_FLOOD        = 0xFF

def companion_chan_data_frame(channel_idx, path_len, path, data_type, data):
    return (bytes([CMD_SEND_CHANNEL_DATA, channel_idx & 0xFF, path_len & 0xFF])
            + path + struct.pack('<H', data_type) + data)

def scope_to_path(scope, path_bytes=b""):
    if scope == "zerohop": return (0, b"")
    if scope == "flood":   return (OUT_PATH_FLOOD, b"")
    if scope == "direct":  return (len(path_bytes), path_bytes)  # 1B hashe default
    raise ValueError(f"scope {scope} nepodporovaný cez companion (zatiaľ)")

async def _send_data(mc, channel_idx, path_len, path, ota_payload, ts):
    data = struct.pack('<I', ts & 0xFFFFFFFF) + ota_payload
    if len(data) > 165:
        raise ValueError(f"data_len {len(data)} > 165")
    frame = companion_chan_data_frame(channel_idx, path_len, path, OTA_MAGIC, data)
    from meshcore.events import EventType
    res = await mc.commands.send(frame, [EventType.OK, EventType.ERROR])
    return res

async def run_mcpy(args):
    from meshcore import MeshCore
    patch, patch_sha256, new_sha256, old_sha256, old_fw_size = \
        make_patch(Path(args.old), Path(args.new), Path(args.patch))
    chunks = [patch[i:i+OTA_CHUNK_DATA] for i in range(0, len(patch), OTA_CHUNK_DATA)]
    total = len(chunks)
    old_prefix = old_sha256[:4]
    privkey = load_ed25519_privkey(Path(args.privkey)) if args.privkey else None
    meta = build_meta_payload(total, len(patch), patch_sha256, new_sha256, old_sha256)
    sig  = build_sig_payload(meta, privkey, args.keyid)
    path_len, path = scope_to_path(args.scope, bytes.fromhex(args.path) if args.path else b"")

    print(f"[mcpy] {len(patch)}B -> {total} chunkov; scope={args.scope} path_len={path_len}")
    mc = await MeshCore.create_serial(args.port, args.baud)
    if mc is None: sys.exit("[CHYBA] companion neodpovedá (je to serial companion?)")
    await mc.commands.set_radio(args.freq, args.bw, args.sf, args.cr)
    await mc.commands.set_channel(args.channel_idx, args.channel_name)
    print(f"[mcpy] radio={args.freq}/{args.bw}/SF{args.sf}/CR{args.cr}  kanal[{args.channel_idx}]={args.channel_name}")

    ts = int(time.time())
    async def snd(payload):
        nonlocal ts; ts += 1
        await _send_data(mc, args.channel_idx, path_len, path, payload, ts)
        await asyncio.sleep(args.delay)

    # META + SIG (poradie: chunky prvé alebo header prvý podľa --packetorder; default hend)
    async def send_hdr():
        await snd(meta); await snd(sig)
    if args.packetorder in ("normal", "hbegin"):
        await send_hdr()
    for idx in range(total):
        await snd(build_ota_chunk(idx, chunks[idx], old_fw_size, old_prefix))
    if args.packetorder == "hend":
        await send_hdr()
    if args.reboot:
        await snd(build_ota_apply(patch_sha256))
    await mc.disconnect()
    print("[mcpy] hotovo")

def main():
    ap = argparse.ArgumentParser(description="OTA sender cez MeshCore companion (meshcore_py)")
    ap.add_argument('--old', required=True); ap.add_argument('--new', required=True)
    ap.add_argument('--port', default="COM3"); ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--patch', default='ota_patch.bin')
    ap.add_argument('--channel-name', default=OTA_CHANNEL_NAME)
    ap.add_argument('--channel-idx', type=int, default=1)
    ap.add_argument('--scope', choices=['zerohop','flood','direct'], default='zerohop')
    ap.add_argument('--path', help='direct: hex hashe hopov (1B), napr. 6368')
    ap.add_argument('--delay', type=float, default=0.3)
    ap.add_argument('--packetorder', choices=['normal','hbegin','hend'], default='hend')
    ap.add_argument('--reboot', action='store_true')
    ap.add_argument('--privkey'); ap.add_argument('--keyid', type=int, default=1)
    ap.add_argument('--freq', type=float, default=869.618); ap.add_argument('--bw', type=float, default=62.5)
    ap.add_argument('--sf', type=int, default=8); ap.add_argument('--cr', type=int, default=5)
    args = ap.parse_args()
    asyncio.run(run_mcpy(args))

if __name__ == '__main__':
    main()
```

- [ ] **Step 4: Spusti — má prejsť**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -m pytest test_nrf-ota/tests/test_mcpy_frame.py -v`
Expected: PASS (2 passed)

- [ ] **Step 5: Commit**

```bash
git add test_nrf-ota/ota_sender_mcpy.py test_nrf-ota/tests/test_mcpy_frame.py
git commit -m "feat(nrfota): ota_sender_mcpy.py — OTA cez companion (meshcore_py)"
```

---

## Fáza C — Firmware receiver (`simple_repeater/nrfota` + `MyMesh.cpp`)

> Firmware sa host-netestuje. „Test" každého tasku = `pio run -e ProMicro_repeater_ota` (kompilácia bez chýb). Reálne správanie overí Fáza E.

### Task C1: `OtaProtocol.h` — nový formát v0

**Files:** Modify `examples/simple_repeater/nrfota/OtaProtocol.h`

**Interfaces:**
- Produces: `OTA_MAGIC`, `OTA_PKT_HDR_SIG`, `OTA_PROT_INF_V0`, `OTA_CHUNK_DATA_MAX=144`, `OtaHeaderPkt` (META 102B), `OtaHdrSigPkt` (99B).

- [ ] **Step 1: Uprav konštanty a štruktúry**

Pridaj k typom (po `OTA_PKT_APPLY`):
```c
#define OTA_PKT_HDR_SIG   0x13   // 2. časť HEADER — Ed25519 podpis
#define OTA_MAGIC         0x07A0 // GRP_DATA data_type pre OTA (gating)
#define OTA_PROT_INF_V0   0x00   // verzia OTA protokolu/štruktúr
```
Zmeň `#define OTA_CHUNK_DATA_MAX  150` → `144`.

Prepíš `OtaHeaderPkt` (META, 102 B):
```c
typedef struct __attribute__((packed)) {
    uint8_t  type;             // OTA_PKT_HEADER
    uint8_t  ota_prot_inf;     // OTA_PROT_INF_V0
    uint32_t patch_size;       // LE; total_chunks = ceil(patch_size/OTA_CHUNK_DATA_MAX)
    uint8_t  patch_sha256[32];
    uint8_t  new_sha256[32];
    uint8_t  old_sha256[32];
} OtaHeaderPkt;                // = 102 B (podpisovaná správa)
```
Pridaj `OtaHdrSigPkt` (99 B):
```c
typedef struct __attribute__((packed)) {
    uint8_t  type;             // OTA_PKT_HDR_SIG
    uint8_t  ota_prot_inf;     // OTA_PROT_INF_V0
    uint8_t  old_sha256[32];   // gating "patrí mne" (== META.old_sha256)
    uint8_t  key_id;
    uint8_t  signature[64];    // Ed25519 nad 102 B META
} OtaHdrSigPkt;
```
Aktualizuj komentár veľkostí (riadok ~24-27, ~36-38) na nový formát + limit 165.

- [ ] **Step 2: Kompiluj** (až po C2/C3 zbehne plný build; teraz syntax-check)

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -m platformio run -e ProMicro_repeater_ota 2>&1 | tail -20`
Expected: očakávaj chyby v `OtaReceiver.cpp` (ešte používa staré polia) — to vyrieši C3. Tu over len, že `OtaProtocol.h` sám nemá syntax chybu (chyby smerujú do .cpp, nie .h).

- [ ] **Step 3: Commit**

```bash
git add examples/simple_repeater/nrfota/OtaProtocol.h
git commit -m "feat(nrfota): OtaProtocol v0 — META/SIG split, magic, chunk144"
```

---

### Task C2: `OtaState.h` — sig/flag polia + perzistencia

**Files:** Modify `examples/simple_repeater/nrfota/OtaState.h`

**Interfaces:**
- Produces: `OtaState` rozšírený o `ota_prot_inf, meta_recv, sig_recv, hdr_key_id, hdr_sig[64]`; `OtaMetaPersist` o tie isté (perzistencia cez reboot).

- [ ] **Step 1: Rozšír `OtaState`** (po `err_code`, pred `bitmap`):
```c
    uint8_t  ota_prot_inf;             // verzia z META
    uint8_t  meta_recv;                // META prijaté
    uint8_t  sig_recv;                 // SIG prijaté
    uint8_t  hdr_key_id;               // key_id z SIG
    uint8_t  hdr_sig[64];              // Ed25519 podpis z SIG (over po prijatí META+SIG)
```

- [ ] **Step 2: Rozšír `OtaMetaPersist`** (pred `crc16`):
```c
    uint8_t  ota_prot_inf;
    uint8_t  meta_recv;
    uint8_t  sig_recv;
    uint8_t  hdr_key_id;
    uint8_t  hdr_sig[64];
```
(`old_fw_size` v perziste ostáva; META ho nenesie — dopĺňa sa z chunku ako dnes.)

- [ ] **Step 3: Commit**

```bash
git add examples/simple_repeater/nrfota/OtaState.h
git commit -m "feat(nrfota): OtaState/persist — META/SIG flagy + podpis"
```

---

### Task C3: `OtaReceiver.cpp` — handle_meta + handle_sig + verify + dispatch + chunk144

**Files:** Modify `examples/simple_repeater/nrfota/OtaReceiver.cpp` (`handle_header`@500, `handle_chunk`@607, `ota_process`@784, `ota_print_pkt`@720, `save_meta`@152/`load_meta`@175)

**Interfaces:**
- Consumes: `OtaHeaderPkt`/`OtaHdrSigPkt` (C1), `OtaState` polia (C2), `verify_header_signature` (@17, nemení sa).
- Produces: `handle_meta`, `handle_sig`, `try_verify_header()` (statická), `ota_process` rozšírené o `OTA_PKT_HDR_SIG`.

- [ ] **Step 1: Pridaj reconstruct+verify helper** (pred `handle_header`)

```c
// Zrekonštruuje 102 B META z uložených polí (musí byť bajt-identické s OtaHeaderPkt)
static void rebuild_meta(uint8_t out[102]) {
    out[0] = OTA_PKT_HEADER; out[1] = ota.ota_prot_inf;
    memcpy(out + 2, &ota.patch_size, 4);
    memcpy(out + 6,  ota.patch_sha256, 32);
    memcpy(out + 38, ota.new_sha256, 32);
    memcpy(out + 70, ota.old_sha256, 32);
}

// Keď máme META aj SIG → over podpis a "promuj" hlavičku (nastav total_chunks).
static void try_verify_header() {
    if (!(ota.meta_recv && ota.sig_recv)) return;
    if (ota.total_chunks > 0) return;   // už promované
    uint8_t meta[102]; rebuild_meta(meta);
    bool ok;
#ifdef OTA_ALLOW_UNSIGNED
    bool is_unsigned = (ota.hdr_sig[0]==0 && ota.hdr_sig[1]==0 && ota.hdr_sig[2]==0 && ota.hdr_sig[3]==0);
    if (is_unsigned) { Serial.println(F("[OTA] HEADER UNSIGNED (dev)")); ok = true; } else
#endif
    ok = verify_header_signature(ota.hdr_sig, meta, 102u, ota.hdr_key_id);
    if (!ok) { Serial.println(F("[OTA] HEADER: INVALID signature")); ota_set_error(OTA_ERR_SIGNATURE); return; }

    uint16_t tc = (uint16_t)((ota.patch_size + OTA_CHUNK_DATA_MAX - 1) / OTA_CHUNK_DATA_MAX);
    if (tc == 0 || tc > OTA_MAX_CHUNKS) { Serial.println(F("[OTA] HEADER: zlé total_chunks")); return; }
    ota.total_chunks = tc;
    ota.recv_count = bitmap_popcount();
    ota.status = OTA_ST_RECEIVING;
    save_bitmap(); save_meta();
    Serial.print(F("[OTA] HEADER OK (META+SIG overené) chunks=")); Serial.print(tc);
    Serial.print(F(" mám ")); Serial.print(ota.recv_count); Serial.println(F(" chunkov"));
    // re-check COMPLETE (chunky mohli doraziť pred hlavičkou)
    if (ota.recv_count >= ota.total_chunks) {
        ota.status |= OTA_ST_COMPLETE; save_meta();
        if (assemble_and_verify()) { ota.status |= OTA_ST_VERIFIED; save_meta(); Serial.println(F("[OTA] VERIFIED")); }
        else { ota_set_error(OTA_ERR_SHA256); save_meta(); }
    }
}
```

- [ ] **Step 2: Nahraď `handle_header` (@500-602) za `handle_meta`**

```c
static void handle_meta(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(OtaHeaderPkt)) { Serial.println(F("[OTA] META: krátky")); return; }
    const OtaHeaderPkt* pkt = (const OtaHeaderPkt*)plain;

    // Base FW gating (cache) — META.old_sha256 musí sedieť s bežiacim FW
    if (ota.base_fw_size > 0 && !ota_base_fw_check_full(ota.base_fw_size, pkt->old_sha256)) {
        Serial.println(F("[OTA] META: base FW nezhoda")); ota_set_error(OTA_ERR_BASEFW); return;
    }
    // Nová session ak ešte žiadna nebeží (partial z chunkov sa zachová cez merge nižšie)
    bool partial = (ota.status & OTA_ST_RECEIVING) && ota.total_chunks == 0;
    if (!(ota.status & OTA_ST_RECEIVING)) {
        OtaFS.remove(OTA_FS_LOG); OtaFS.remove(OTA_FS_PATCH); OtaFS.remove(OTA_FS_BITMAP); ota_clear();
        ota.status = OTA_ST_RECEIVING; ota.total_chunks = 0;
    } else if (!partial && memcmp(ota.patch_sha256, pkt->patch_sha256, 32) != 0) {
        // iný patch beží — prepíš
        OtaFS.remove(OTA_FS_LOG); OtaFS.remove(OTA_FS_PATCH); OtaFS.remove(OTA_FS_BITMAP); ota_clear();
        ota.status = OTA_ST_RECEIVING; ota.total_chunks = 0;
    }
    ota.ota_prot_inf = pkt->ota_prot_inf;
    ota.patch_size = pkt->patch_size;
    memcpy(ota.patch_sha256, pkt->patch_sha256, 32);
    memcpy(ota.new_sha256,   pkt->new_sha256, 32);
    memcpy(ota.old_sha256,   pkt->old_sha256, 32);
    ota.meta_recv = 1;
    save_meta();
    Serial.print(F("[OTA] META prijaté patch_size=")); Serial.println(pkt->patch_size);
    try_verify_header();
}

static void handle_sig(const uint8_t* plain, int plen) {
    if (plen < (int)sizeof(OtaHdrSigPkt)) { Serial.println(F("[OTA] SIG: krátky")); return; }
    const OtaHdrSigPkt* pkt = (const OtaHdrSigPkt*)plain;
    // gating: ak už máme META, old_sha256 musí sedieť; inak base FW cache
    if (ota.meta_recv) {
        if (memcmp(ota.old_sha256, pkt->old_sha256, 32) != 0) { Serial.println(F("[OTA] SIG: old_sha256 nezhoda")); return; }
    } else if (ota.base_fw_size > 0 && !ota_base_fw_check_full(ota.base_fw_size, pkt->old_sha256)) {
        Serial.println(F("[OTA] SIG: base FW nezhoda")); return;
    }
    if (!(ota.status & OTA_ST_RECEIVING)) { ota.status = OTA_ST_RECEIVING; ota.total_chunks = 0; }
    ota.hdr_key_id = pkt->key_id;
    memcpy(ota.hdr_sig, pkt->signature, 64);
    ota.sig_recv = 1;
    save_meta();
    Serial.print(F("[OTA] SIG prijaté key_id=0x")); Serial.println(pkt->key_id, HEX);
    try_verify_header();
}
```
POZN: pôvodný retransmit/promócia kód `handle_header` sa nahrádza vyššie uvedeným (META/SIG idempotentné — opätovné prijatie len prepíše rovnaké polia; `try_verify_header` má guard `total_chunks>0`).

- [ ] **Step 3: Uprav `handle_chunk` exp_len** — `OTA_CHUNK_DATA_MAX` je teraz 144 (@655-659 už používa makro, žiadna zmena okrem hodnoty z C1). Over že `data_len`/`exp_len` aritmetika používa `OTA_CHUNK_DATA_MAX`. Žiadna ďalšia zmena.

- [ ] **Step 4: Uprav `ota_process` (@784)**

```c
bool ota_process(const uint8_t* plain, int plen) {
    if (plen < 1) return false;
    switch (plain[0]) {
        case OTA_PKT_HEADER:  handle_meta(plain, plen); return true;
        case OTA_PKT_HDR_SIG: handle_sig(plain, plen);  return true;
        case OTA_PKT_CHUNK:   handle_chunk(plain, plen); return true;
        case OTA_PKT_APPLY:   handle_apply(plain, plen); return true;
        default:              return false;
    }
}
```

- [ ] **Step 5: Uprav `ota_print_pkt` (@720)** — pridaj `case OTA_PKT_HDR_SIG:` (vypíš key_id + prvé bajty podpisu) a `OTA_PKT_HEADER` zmeň na META výpis (patch_size, sha-prefixy). Mechanické; drž štýl existujúcich vetiev.

- [ ] **Step 6: Uprav `save_meta`/`load_meta` (@152/@175)** — kopíruj nové polia do/z `OtaMetaPersist` (`ota_prot_inf, meta_recv, sig_recv, hdr_key_id, hdr_sig`). Pri `load_meta` resume obnov tieto do `ota` a zavolaj `try_verify_header()` v `try_resume` (@402) ak `meta_recv && sig_recv && total_chunks==0`.

- [ ] **Step 7: Kompiluj celý OTA firmware**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -m platformio run -e ProMicro_repeater_ota 2>&1 | tail -25`
Expected: `SUCCESS` (žiadne chyby). Ak chyby — oprav podľa hlásení (typicky neaktualizované staré polia/`total_chunks` v META).

- [ ] **Step 8: Commit**

```bash
git add examples/simple_repeater/nrfota/OtaReceiver.cpp
git commit -m "feat(nrfota): receiver META+SIG stavovy automat + verify + dispatch"
```

---

### Task C4: `OtaMesh.cpp` — kanál `#fkotanrf`

**Files:** Modify `examples/simple_repeater/nrfota/OtaMesh.cpp` (`ota_build_channel`@12-30)

- [ ] **Step 1: Nahraď `ota_build_channel` telo**

```c
void ota_build_channel(mesh::GroupChannel& ch) {
    // secret = SHA256(OTA_CHANNEL_NAME)[0:16] (MeshCore #-konvencia, zhodné s
    // meshcore_py set_channel). Pozor: secret obsahuje 0x00 → NEhashovať ako string.
    const char* name = OTA_CHANNEL_NAME;
    uint8_t full[32];
    mesh::Utils::sha256(full, sizeof(full), (const uint8_t*)name, (int)strlen(name));
    memset(ch.secret, 0, PUB_KEY_SIZE);
    memcpy(ch.secret, full, 16);                      // AES kľúč=secret[:16], HMAC=secret[:32]

    uint8_t h[32];
    mesh::Utils::sha256(h, sizeof(h), ch.secret, 16); // hash = SHA256(secret)[0]
    memcpy(ch.hash, h, PATH_HASH_SIZE);

    Serial.print(F("[OTA] kanál ")); Serial.print(name);
    Serial.print(F(" hash=0x")); if (ch.hash[0] < 0x10) Serial.print('0');
    Serial.println(ch.hash[0], HEX);                  // očakávané 0xA4
}
```

- [ ] **Step 2: Kompiluj**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -m platformio run -e ProMicro_repeater_ota 2>&1 | tail -15`
Expected: SUCCESS

- [ ] **Step 3: Commit**

```bash
git add examples/simple_repeater/nrfota/OtaMesh.cpp
git commit -m "feat(nrfota): OTA kanal #fkotanrf (secret=SHA256(name), hash 0xA4)"
```

---

### Task C5: `MyMesh.cpp` — `onGroupDataRecv` demux `[data_type][len]`

**Files:** Modify `examples/simple_repeater/MyMesh.cpp` (`onGroupDataRecv`@878-902)

- [ ] **Step 1: Vlož demux pred `_ota_pending` kopírovanie** (po `if (channel.hash[0] != ...) return;`)

Nahraď podmienku `if (len < 5) return;` a nasleduj:
```c
  // Zjednotený OTA formát: štandardný GRP_DATA plaintext = [data_type 2B][len 1B][ts 4B][ota_payload].
  // Odlúpni [data_type][len]; ak data_type != OTA_MAGIC, nie je to OTA. Po odlúpnutí
  // má buffer tvar [ts 4B][ota_payload] — zvyšok pipeline (loop +4) ostáva nezmenený.
  if (len < 3 + 5) return;                                   // [dt2][len1] + [ts4][type1]
  uint16_t dtype = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
  if (dtype != OTA_MAGIC) return;                            // nie náš OTA data_type
  if (data[2] != (uint8_t)(len - 3)) return;                 // sanity: vnútorná dĺžka
  data += 3; len -= 3;                                       // → [ts4][ota_payload]
```
(Existujúci `#ifdef OTA_GDR_DIAG` a `memcpy(_ota_pending, data, n)` ostávajú — `data`/`len` sú už posunuté.)

- [ ] **Step 2: Kompiluj**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -m platformio run -e ProMicro_repeater_ota 2>&1 | tail -15`
Expected: SUCCESS (over že `OTA_MAGIC` je viditeľné — `OtaProtocol.h` je includnuté cez OTA hlavičky; ak nie, pridaj `#include "nrfota/OtaProtocol.h"`).

- [ ] **Step 3: Commit**

```bash
git add examples/simple_repeater/MyMesh.cpp
git commit -m "feat(nrfota): onGroupDataRecv demux OTA_MAGIC + strip [data_type][len]"
```

---

### Task C6: OTA env — `OTA_CHANNEL_NAME` build define

**Files:** Modify `variants/*/platformio.ini` (env `ProMicro_repeater_ota`) — nájdi `-D OTA_CHANNEL_PSK=...`

- [ ] **Step 1:** Nájdi define:

Run: `grep -rn "OTA_CHANNEL_PSK" variants/*/platformio.ini examples/`
Expected: riadok s `-D OTA_CHANNEL_PSK=\"meshcore-ota-key\"`

- [ ] **Step 2:** Nahraď `-D OTA_CHANNEL_PSK=\"meshcore-ota-key\"` → `-D OTA_CHANNEL_NAME=\"#fkotanrf\"`. Ak `OtaProtocol.h`/`OtaMesh.h` má fallback `#ifndef OTA_CHANNEL_PSK`, pridaj/uprav na `#ifndef OTA_CHANNEL_NAME #define OTA_CHANNEL_NAME "#fkotanrf" #endif`.

- [ ] **Step 3: Kompiluj + over hash v boote** (vyžaduje COM5 zariadenie — môže počkať na Fázu E)

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -m platformio run -e ProMicro_repeater_ota 2>&1 | tail -10`
Expected: SUCCESS

- [ ] **Step 4: Commit**

```bash
git add variants/ examples/
git commit -m "feat(nrfota): OTA env pouziva OTA_CHANNEL_NAME=#fkotanrf"
```

---

## Fáza D — Runner

### Task D1: `ota_test_lora_repeater.py` — `--sender {bridge,mcpy}`

**Files:** Modify `test_nrf-ota/ota_test_lora_repeater.py`

**Interfaces:**
- Consumes: `ota_sender.py` (bridge) / `ota_sender_mcpy.py` (companion), `ota_sender.ota_channel_secret`
- Produces: `--sender` voľba; `phase_baseline`/`phase_run` vetvy pre companion.

- [ ] **Step 1: Pridaj argumenty a konštanty**
```python
DEFAULT_COMPANION_ENV = "Xiao_nrf52_companion_radio_usb"
OTA_MCPY_SENDER = SCRIPT_DIR / "ota_sender_mcpy.py"
# v main(): 
ap.add_argument("--sender", choices=["bridge", "mcpy"], default="bridge")
```

- [ ] **Step 2: `phase_baseline` vetva pre `mcpy`** — namiesto FK_lora bridge buildni companion na COM3 (SK = root build_flags default), repeater OLD na COM5, potom `set radio 869.618,62.5,8,5` + reboot na COM5:
```python
if args.sender == "mcpy":
    run([PY, "-m", "platformio", "run", "-e", DEFAULT_COMPANION_ENV,
         "-t", "upload", "--upload-port", args.bridge_port],
        f"Build+Upload COMPANION ({DEFAULT_COMPANION_ENV}, SK) -> {args.bridge_port}")
    # repeater OLD (bez CZ flagu = SK default)
    run([PY, "-m", "platformio", "run", "-e", args.target_env,
         "-t", "upload", "--upload-port", args.target_port],
        f"Build+Upload REPEATER OLD ({args.target_env}, SK) -> {args.target_port}")
    sz = extract_app_image(args.target_env, OLD_BIN); old_build = read_build_number()
    wait_port_back(args.target_port, timeout=30)
    capture_serial(args.target_port, seconds=4, send_cmd="set radio 869.618,62.5,8,5\r")
    capture_serial(args.target_port, seconds=3, send_cmd="ota clear\r")
    capture_serial(args.target_port, seconds=14, send_cmd="reboot\r")
    return
# else: existujúca bridge (CZ) vetva
```

- [ ] **Step 3: `phase_run` — sender command podľa `--sender`**
```python
if args.sender == "mcpy":
    sender = [PY, str(OTA_MCPY_SENDER), "--old", str(OLD_BIN), "--new", str(NEW_BIN),
              "--port", args.bridge_port, "--channel-name", "#fkotanrf", "--channel-idx", "1",
              "--scope", (args.scope or "zerohop"), "--delay", str(args.delay),
              "--freq", "869.618", "--bw", "62.5", "--sf", "8", "--cr", "5"]
    if args.privkey: sender += ["--privkey", args.privkey, "--keyid", str(args.keyid)]
    if args.packetorder: sender += ["--packetorder", args.packetorder]
else:
    # existujúci bridge sender (--mode meshcore --psk ...), ale PSK je teraz #fkotanrf secret:
    import ota_sender as _S
    psk_hex = _S.ota_channel_secret().hex()
    sender = [PY, str(OTA_SENDER), "--old", str(OLD_BIN), "--new", str(NEW_BIN),
              "--port", args.bridge_port, "--mode", "meshcore", "--psk", psk_hex,
              "--delay", str(args.delay)]
    # ... existujúce --drop/--privkey/--packetorder/--scope passthrough
```
POZN: build NEW (`extract_app_image`) — pre `mcpy` BEZ `-DLORA_PRESET_CZ` (SK default); pre `bridge` s CZ ako dnes.

- [ ] **Step 4: Preset hlásenie** — `[preset] LoRa = SK (869.618/62.5/SF8)` keď `--sender mcpy`.

- [ ] **Step 5: Sanity import**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe -c "import sys; sys.path.insert(0,'test_nrf-ota'); import ota_test_lora_repeater"`
Expected: bez chyby

- [ ] **Step 6: Commit**

```bash
git add test_nrf-ota/ota_test_lora_repeater.py
git commit -m "feat(nrfota): runner --sender {bridge,mcpy} (companion SK / bridge CZ)"
```

---

## Fáza E — HW e2e (zerohop)

> Vyžaduje fyzický HW: COM3 = XIAO nRF52840, COM5 = ProMicro repeater. `test_key.der` musí byť v `test_nrf-ota/`. Riziko bricku (flash) — recovery: double-tap RESET → DFU → `baseline`.

### Task E1: e2e companion (hlavný cieľ) — zerohop SK

**Files:** žiadne (beh)

- [ ] **Step 1: Over `test_key.der`**

Run: `ls test_nrf-ota/test_key.der`
Expected: existuje. Ak nie — skopíruj z funkčného prostredia (pubkey == `s_authors[1]`).

- [ ] **Step 2: baseline (companion na COM3 + repeater OLD na COM5, SK)**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe test_nrf-ota/ota_test_lora_repeater.py baseline --sender mcpy --bridge-port COM3 --target-port COM5`
Expected: companion upload OK; repeater OLD upload OK; v boote COM5 `[OTA] kanál #fkotanrf hash=0xA4`; `ota clear` + reboot.

- [ ] **Step 3: run (build NEW, broadcast cez companion, VERIFIED, flash)**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe test_nrf-ota/ota_test_lora_repeater.py run --sender mcpy --bridge-port COM3 --target-port COM5 --delay 1.0`
Expected: COM5 logy `[OTA] META prijaté` + `[OTA] SIG prijaté` + `HEADER OK (META+SIG overené)` + rast chunkov → `VERIFIED`; potom `ota flash` → reboot → `[PASS] build #NEW`.

- [ ] **Step 4: Diagnostika pri zlyhaní**
  - `0xA4` hash nesedí → kanál/secret nezhoda (skontroluj C4/C6).
  - META/SIG „base FW nezhoda" → OLD obraz != bežiaci FW (zopakuj baseline).
  - žiadne META → companion neposiela (over `set_channel`/`set_radio` OK frames; `mc.commands.send` mapovanie OK).
  - dedup/žiadny rast → over že `ts` rastie per paket (B2 `snd` inkrementuje).

- [ ] **Step 5:** Bez commitu (test). Zaznamenaj výsledok do pamäte/fkclaude.

---

### Task E2: e2e bridge (regresia nového formátu) — zerohop CZ

**Files:** žiadne (beh). Vyžaduje FK_lora-sniffer projekt (sibling dir) na build bridge.

- [ ] **Step 1: baseline (FK_lora bridge na COM3 CZ + repeater OLD COM5 CZ)**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe test_nrf-ota/ota_test_lora_repeater.py baseline --sender bridge --bridge-port COM3 --target-port COM5`
Expected: bridge upload (CZ); repeater OLD (CZ); `ota clear` + reboot.

- [ ] **Step 2: run (bridge, nový META/SIG formát cez raw)**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe test_nrf-ota/ota_test_lora_repeater.py run --sender bridge --bridge-port COM3 --target-port COM5 --packetorder hend`
Expected: rovnaké OTA logy ako E1 (META/SIG/VERIFIED) → `ota flash` → `[PASS] build #NEW`. Potvrdzuje, že zjednotený formát funguje aj cez bridge.

- [ ] **Step 3:** Bez commitu (test). Zaznamenaj výsledok.

---

## Self-Review (autor plánu)

- **Pokrytie spec:** §3 formát→A2/C1/C5; §4 META/SIG/chunk→A1/C1; §4.5 podpis→A1/C3; §4.6 kanál→A1/C4/C6; §5 receiver→C3/C5; §6 senderi→A3/B2; §6.3 meshcore_py→B1/B2; §7 runner→D1; §8 e2e→E1/E2. ✓
- **Placeholdery:** žiadne „TBD/TODO"; kód uvedený pre netriviálne časti; mechanické edity (print, save/load) majú presný popis polí.
- **Typová konzistencia:** `OTA_MAGIC=0x07A0`, `OTA_PKT_HDR_SIG=0x13`, `OTA_CHUNK_DATA(_MAX)=144`, META 102 B, SIG 99 B, kanál hash `0xA4` — zhodné naprieč Python (A1) aj C (C1/C4) taskami. `build_meta_payload`/`build_sig_payload`/`grpdata_plaintext` názvy konzistentné A1→A2→A3→B2.
- **Známe medzery (vedomé):** firmware sa overuje len kompiláciou + Fázou E (embedded, bez host CI — v súlade s CLAUDE.md). `region` scope cez companion mimo rozsahu (zerohop/flood/direct stačí).

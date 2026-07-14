# FOTA ACL Sign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Overenie podpisu `.fotapkg` voči ACL adminom repeatera + podpisovanie companion identity hex kľúčom (spec: `fkclaude/docs/superpowers/specs/2026-07-15-fota-acl-sign-design.md`).

**Architecture:** Nový jednotný SIG formát `key_id=0` + 4 B pubkey prefix podpisovateľa (103 B); receiver hľadá prefix v `s_authors` a potom v ACL admin záznamoch (hook do `MyMesh::acl`). Legacy `key_id≥1` (99 B) ostáva pre staré FW. Podpis expandovaným (64 B) orlp kľúčom: pure-Python modul + Dart port; .der kľúče sa interne konvertujú na expandované → jediný signer všade.

**Tech Stack:** C (nRF52 FW, orlp ed25519), Python 3 (penv: `D:\FkDev\.platformio\penv\Scripts\python.exe`, pycryptodome len ako golden referencia), Dart/Flutter (`D:\FkDev\Tools\flutter\bin\flutter.bat`), pinenacl (seed cesta v appke).

## Global Constraints

- **Dva repozitáre:** MeshCore `D:\FkDev\FkProj\VSC\MeshCore` (vetva `features/nrf-fota`), Flutter appka `D:\FkDev\FkProj\VSC\meshcore-open` (vetva `feature/nrf-ota-sender`). Commity per-task, v správnom repe.
- **Bilingválne komentáre `//en:`/`//sk:`** vo FOTA C/C++ kóde — pri každom novom/zmenenom komentári udržuj OBA jazyky. Debug logy (`FOTA_DEBUG_PRINTLN`) po anglicky, inline.
- **Nereformátovať existujúci kód** (žiadne noise diffy). Žiadna dynamická alokácia mimo `setup()`/`begin()`.
- **Wire kompatibilita:** legacy SIG 99 B s `key_id≥1` musí ostať bajt-identická s dnešným výstupom (mapovanie `key_id N → s_authors[N-1]`).
- Python príkazy spúšťaj cez penv: `D:\FkDev\.platformio\penv\Scripts\python.exe` (má pycryptodome).
- Flutter príkazy: `D:\FkDev\Tools\flutter\bin\flutter.bat` (spúšťaj z `D:\FkDev\FkProj\VSC\meshcore-open`).
- Build verify FW: `pio run -e ProMicro_repeater_fota` (z koreňa MeshCore).
- Konštanty: `FOTA_KEY_ID_PREFIX = 0`, prefix = prvé 4 B Ed25519 pubkey, nový SIG = 103 B, META = 102 B (nemení sa).
- `test_key.der` je gitignored a MUSÍ existovať v `test_nrf-fota/` (pub = `s_authors` test záznam `C2 2F 8A E0 ...`).

---

### Task 1: Pure-Python expandovaný Ed25519 signer (`fota_ed25519_expanded.py`)

**Files:**
- Create: `test_nrf-fota/fota_ed25519_expanded.py`
- Test: `test_nrf-fota/test_fota_ed25519_expanded.py`

**Interfaces:**
- Produces: `expand_seed(seed32: bytes) -> bytes(64)`, `derive_pub(expanded64: bytes) -> bytes(32)`, `sign_expanded(msg: bytes, expanded64: bytes, pub32: bytes|None) -> bytes(64)`, `seed_from_pkcs8_der(der: bytes) -> bytes(32)`, `class ExpandedKey` (`.expanded`, `.pub`, `.prefix` = `pub[:4]`, `.sign(msg)->bytes(64)`), `key_from_der(path) -> ExpandedKey`, `key_from_hex(hexstr) -> ExpandedKey`.

- [ ] **Step 1: Napíš zlyhávajúci test**

```python
#!/usr/bin/env python3
"""Golden test: expandovaný signer == pycryptodome RFC8032 (rovnaký seed)."""
import sys, hashlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))

from fota_ed25519_expanded import (expand_seed, derive_pub, sign_expanded,
                                   seed_from_pkcs8_der, ExpandedKey, key_from_der)

def ref_sign(seed, msg):
    from Crypto.PublicKey import ECC
    from Crypto.Signature import eddsa
    k = ECC.construct(curve='ed25519', seed=seed)
    return eddsa.new(k, 'rfc8032').sign(msg), k.public_key().export_key(format='raw')

def main():
    fails = 0
    for i in range(4):
        seed = hashlib.sha256(b'fota-test-seed-%d' % i).digest()
        msg = hashlib.sha512(b'fota-msg-%d' % i).digest() + b'x' * 38  # 102 B ako META
        ref_sig, ref_pub = ref_sign(seed, msg)
        exp = expand_seed(seed)
        assert derive_pub(exp) == ref_pub, f"pub mismatch (case {i})"
        assert sign_expanded(msg, exp) == ref_sig, f"sig mismatch (case {i})"
        k = ExpandedKey(exp)
        assert k.pub == ref_pub and k.prefix == ref_pub[:4] and k.sign(msg) == ref_sig
    # test_key.der → pub musí sedieť so s_authors test záznamom
    tk = key_from_der(Path(__file__).parent / 'test_key.der')
    assert tk.pub[:4] == bytes([0xC2, 0x2F, 0x8A, 0xE0]), f"test_key pub prefix {tk.pub[:4].hex()}"
    print("OK: expanded signer == pycryptodome (4 vektory) + test_key.der pub sedi")

if __name__ == '__main__':
    main()
```

- [ ] **Step 2: Over, že zlyhá**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe test_nrf-fota\test_fota_ed25519_expanded.py`
Expected: `ModuleNotFoundError: No module named 'fota_ed25519_expanded'`

- [ ] **Step 3: Implementuj modul**

```python
#!/usr/bin/env python3
"""Ed25519 s expandovaným (64 B) privátnym kľúčom — orlp/ed25519 kompatibilné.

MeshCore companion zobrazuje privátny kľúč identity ako 64 B hex = SHA512(seed)
s clampingom (lib/ed25519/keypair.c). Zo seedu sa expandovaný kľúč VYROBIŤ dá
(expand_seed), naopak NIE (SHA512 je jednosmerná) — preto hex2der neexistuje.
Podpis je štandardný RFC8032 (overí ho fota_ed25519_verify aj pycryptodome).
"""
import hashlib
from pathlib import Path

_p = 2**255 - 19
_L = 2**252 + 27742317777372353535851937790883648493

def _inv(x): return pow(x, _p - 2, _p)

_d = (-121665 * _inv(121666)) % _p
_I = pow(2, (_p - 1) // 4, _p)

def _xrecover(y):
    xx = (y * y - 1) * _inv(_d * y * y + 1)
    x = pow(xx, (_p + 3) // 8, _p)
    if (x * x - xx) % _p != 0:
        x = (x * _I) % _p
    if x % 2 != 0:
        x = _p - x
    return x

_By = (4 * _inv(5)) % _p
_Bx = _xrecover(_By)
_B = (_Bx, _By, 1, (_Bx * _By) % _p)   # extended coordinates (x, y, z, t)
_ZERO = (0, 1, 1, 0)

def _edwards_add(P, Q):
    (x1, y1, z1, t1) = P; (x2, y2, z2, t2) = Q
    a = (y1 - x1) * (y2 - x2) % _p
    b = (y1 + x1) * (y2 + x2) % _p
    c = t1 * 2 * _d * t2 % _p
    dd = z1 * 2 * z2 % _p
    e = b - a; f = dd - c; g = dd + c; h = b + a
    return (e * f % _p, g * h % _p, f * g % _p, e * h % _p)

def _scalarmult_base(e):
    P = _B; Q = _ZERO
    while e:
        if e & 1:
            Q = _edwards_add(Q, P)
        P = _edwards_add(P, P)
        e >>= 1
    return Q

def _encodepoint(P):
    (x, y, z, t) = P
    zi = _inv(z)
    x = (x * zi) % _p; y = (y * zi) % _p
    return (y | ((x & 1) << 255)).to_bytes(32, 'little')

def expand_seed(seed32: bytes) -> bytes:
    """seed (32 B) -> expandovaný kľúč (64 B) — ako ed25519_create_keypair."""
    if len(seed32) != 32:
        raise ValueError("seed musí mať 32 B")
    h = bytearray(hashlib.sha512(seed32).digest())
    h[0] &= 248; h[31] &= 63; h[31] |= 64
    return bytes(h)

def derive_pub(expanded64: bytes) -> bytes:
    """expandovaný kľúč -> pubkey (32 B) — ako ed25519_derive_pub."""
    a = int.from_bytes(expanded64[:32], 'little')
    return _encodepoint(_scalarmult_base(a))

def sign_expanded(msg: bytes, expanded64: bytes, pub32: bytes | None = None) -> bytes:
    """Podpis expandovaným kľúčom — port lib/ed25519/sign.c (orlp)."""
    if len(expanded64) != 64:
        raise ValueError("expandovaný kľúč musí mať 64 B")
    if pub32 is None:
        pub32 = derive_pub(expanded64)
    a = int.from_bytes(expanded64[:32], 'little')
    prefix = expanded64[32:]
    r = int.from_bytes(hashlib.sha512(prefix + msg).digest(), 'little') % _L
    R = _encodepoint(_scalarmult_base(r))
    k = int.from_bytes(hashlib.sha512(R + pub32 + msg).digest(), 'little') % _L
    S = (r + k * a) % _L
    return R + S.to_bytes(32, 'little')

def seed_from_pkcs8_der(der: bytes) -> bytes:
    """PKCS#8 DER (RFC 8410, štandardne 48 B) -> 32 B seed (posledných 32 B za 0x0420)."""
    idx = der.rfind(b'\x04\x20')
    if idx < 0 or idx + 34 > len(der):
        raise ValueError("DER: nenašiel som OCTET STRING so seedom")
    return der[idx + 2: idx + 34]

class ExpandedKey:
    """Jednotný podpisový kľúč pre všetky sendre (der aj companion hex)."""
    def __init__(self, expanded64: bytes):
        self.expanded = expanded64
        self.pub = derive_pub(expanded64)
        self.prefix = self.pub[:4]
    def sign(self, msg: bytes) -> bytes:
        return sign_expanded(msg, self.expanded, self.pub)

def key_from_der(path) -> ExpandedKey:
    return ExpandedKey(expand_seed(seed_from_pkcs8_der(Path(path).read_bytes())))

def key_from_hex(hexstr: str) -> ExpandedKey:
    raw = bytes.fromhex(hexstr.strip())
    if len(raw) != 64:
        raise ValueError("companion hex musí mať 128 hex znakov (64 B)")
    return ExpandedKey(raw)
```

- [ ] **Step 4: Over, že test prejde**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe test_nrf-fota\test_fota_ed25519_expanded.py`
Expected: `OK: expanded signer == pycryptodome (4 vektory) + test_key.der pub sedi`

- [ ] **Step 5: Commit (MeshCore repo)**

```bash
git add test_nrf-fota/fota_ed25519_expanded.py test_nrf-fota/test_fota_ed25519_expanded.py
git commit -m "feat(fota): pure-python Ed25519 signer pre expandovany (companion hex) kluc + golden test"
```

---

### Task 2: `fota_keytool.py` + vygenerovanie `fota_signkey1..4.der`

**Files:**
- Create: `test_nrf-fota/fota_keytool.py`
- Create (vygenerované, gitignored): `test_nrf-fota/fota_signkey1.der` … `fota_signkey4.der`
- Modify: `.gitignore` (riadok za `test_nrf-fota/test_key.der`)

**Interfaces:**
- Consumes: `ExpandedKey`, `key_from_der`, `key_from_hex`, `expand_seed`, `seed_from_pkcs8_der` z Task 1.
- Produces: CLI `gen <out.der>` / `der2hex <key.der>` / `pub <key.der|128hex>`; výpis obsahuje pub hex, prefix, expandovaný hex a C snippet pre `s_authors`.

- [ ] **Step 1: Implementuj keytool**

```python
#!/usr/bin/env python3
"""fota_keytool.py — správa FOTA podpisových kľúčov.

  gen <out.der>      vygeneruje Ed25519 keypair, uloží PKCS#8 DER, vypíše info
  der2hex <key.der>  seed z DER -> expandovaný 64 B hex (companion formát) + pub
  pub <der|128hex>   vypíše pubkey/prefix/C snippet pre s_authors

hex2der zámerne NEEXISTUJE: companion hex je SHA512(seed) s clampingom —
jednosmerná funkcia, seed (a teda .der) sa z neho spätne vyrobiť nedá.
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fota_ed25519_expanded import ExpandedKey, key_from_der, key_from_hex

def print_key_info(name: str, k: ExpandedKey, show_priv: bool):
    print(f"== {name} ==")
    print(f"pub        : {k.pub.hex()}")
    print(f"prefix     : {k.prefix.hex().upper()}")
    if show_priv:
        print(f"priv (hex) : {k.expanded.hex()}   # companion/expanded formát")
    rows = [k.pub[i:i + 8] for i in range(0, 32, 8)]
    body = ",\n".join("           " + ", ".join(f"0x{b:02X}" for b in row) for row in rows)
    print("C snippet pre s_authors (FotaReceiver_signkey.cpp):")
    print(f"    //en: {name} — pub prefix {k.prefix.hex().upper()}")
    print("    { {" + body.lstrip() + " } },")

def load_any(arg: str) -> ExpandedKey:
    p = Path(arg)
    if p.exists():
        return key_from_der(p)
    return key_from_hex(arg)

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, arg = sys.argv[1], sys.argv[2]
    if cmd == 'gen':
        from Crypto.PublicKey import ECC
        out = Path(arg)
        if out.exists():
            sys.exit(f"[CHYBA] {out} už existuje — nechcem prepísať kľúč")
        key = ECC.generate(curve='ed25519')
        out.write_bytes(key.export_key(format='DER'))
        print(f"[gen] zapísané: {out}")
        print_key_info(out.name, key_from_der(out), show_priv=True)
    elif cmd == 'der2hex':
        print_key_info(Path(arg).name, key_from_der(arg), show_priv=True)
    elif cmd == 'pub':
        print_key_info(arg if not Path(arg).exists() else Path(arg).name,
                       load_any(arg), show_priv=False)
    else:
        sys.exit(f"[CHYBA] neznámy príkaz '{cmd}'\n{__doc__}")

if __name__ == '__main__':
    main()
```

- [ ] **Step 2: Over na test kľúči**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe test_nrf-fota\fota_keytool.py der2hex test_nrf-fota\test_key.der`
Expected: `prefix     : C22F8AE0` + C snippet zhodný s existujúcim `s_authors` záznamom.

- [ ] **Step 3: Vygeneruj 4 kľúče**

Run (4×, i=1..4): `D:\FkDev\.platformio\penv\Scripts\python.exe test_nrf-fota\fota_keytool.py gen test_nrf-fota\fota_signkey<i>.der`
Expected: 4 nové .der súbory; **ulož si vypísané pub kľúče/C snippety** pre Task 5.

- [ ] **Step 4: Gitignore**

V `.gitignore` za riadok `test_nrf-fota/test_key.der` pridaj:

```
test_nrf-fota/fota_signkey*.der
```

Over: `git status --short` nesmie ukazovať žiadny `.der`.

- [ ] **Step 5: Commit (MeshCore repo)**

```bash
git add test_nrf-fota/fota_keytool.py .gitignore
git commit -m "feat(fota): fota_keytool.py (gen/der2hex/pub) + gitignore pre fota_signkey*.der"
```

---

### Task 3: Python sendre — nový SIG formát + `--privkey-hex`

**Files:**
- Modify: `test_nrf-fota/fota_sender.py` (`load_ed25519_privkey` ~96, `sign_fota_header` ~106, `build_sig_payload` ~176, arg parser ~626, init výpis ~716)
- Modify: `test_nrf-fota/fota_export_pkg.py` (build_pkg ~13–37, args ~53)
- Modify: `test_nrf-fota/fota_sender_mcpy.py` (args ~132–133, warning ~79)
- Modify: `test_nrf-fota/gen_fotapkg.py` (privkey default blok ~139–145, `--keyid`/`--privkey` args ~183–187, výpis ~159)
- Test: rozšíri `test_nrf-fota/test_fota_ed25519_expanded.py`

**Interfaces:**
- Consumes: `ExpandedKey`, `key_from_der`, `key_from_hex` (Task 1).
- Produces: `FOTA_KEY_ID_PREFIX = 0` (fota_sender.py); `build_sig_payload(meta, privkey: ExpandedKey|None, key_id)` → 103 B pri `key_id==0` (`… + sig64 + prefix4`), 99 B legacy; `load_ed25519_privkey(path) -> ExpandedKey`; CLI `--privkey-hex` vo fota_sender/mcpy/export/gen; **default `--keyid 0`**; pkg `signed` blok: `{"key_id": 0, "signer_prefix": "<8hex>", "meta_b64", "sig_b64"}` (sig_b64 = celý 103 B payload).

- [ ] **Step 1: Rozšír test o formát SIG**

Do `test_fota_ed25519_expanded.py` pridaj (a zavolaj z `main()`):

```python
def test_sig_payload():
    import fota_sender as S
    seed = hashlib.sha256(b'fota-test-seed-0').digest()
    from fota_ed25519_expanded import expand_seed, ExpandedKey
    k = ExpandedKey(expand_seed(seed))
    meta = S.build_meta_payload(1, 100, b'\x11' * 32, b'\x22' * 32, b'\x33' * 32)
    # nový formát: key_id=0 → 103 B, prefix na konci
    sig0 = S.build_sig_payload(meta, k, 0)
    assert len(sig0) == 103 and sig0[35] == 0 and sig0[-4:] == k.prefix
    assert sig0[36:100] == k.sign(meta)
    # legacy: key_id=1 → 99 B, bajt-identické so starým formátom
    sig1 = S.build_sig_payload(meta, k, 1)
    assert len(sig1) == 99 and sig1[35] == 1 and sig1[36:100] == k.sign(meta)
    # unsigned legacy: privkey None → nulový podpis, 99 B
    sigu = S.build_sig_payload(meta, None, 1)
    assert len(sigu) == 99 and sigu[36:100] == bytes(64)
    print("OK: build_sig_payload novy/legacy/unsigned format")
```

- [ ] **Step 2: Over, že zlyhá**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe test_nrf-fota\test_fota_ed25519_expanded.py`
Expected: FAIL — `build_sig_payload` s ExpandedKey (starý kód volá `eddsa.new` s ECC objektom → TypeError), a nový formát ešte neexistuje.

- [ ] **Step 3: Uprav `fota_sender.py`**

Nahraď `load_ed25519_privkey` + `sign_fota_header` + `build_sig_payload` (pycryptodome už na podpis netreba — jediný signer je ExpandedKey; pycryptodome ostáva len v testoch/keytool `gen`):

```python
FOTA_KEY_ID_PREFIX = 0   # key_id=0 -> podpisovateľ identifikovaný 4B pubkey prefixom za podpisom

def load_ed25519_privkey(key_path: Path):
    """Načíta Ed25519 private key z DER súboru, vráti ExpandedKey (jednotný signer)."""
    from fota_ed25519_expanded import key_from_der
    return key_from_der(key_path)

def load_ed25519_privkey_hex(hexstr: str):
    """64 B expandovaný kľúč (companion 'dlhý hex') -> ExpandedKey."""
    from fota_ed25519_expanded import key_from_hex
    try:
        return key_from_hex(hexstr)
    except ValueError as e:
        sys.exit(f"[CHYBA] --privkey-hex: {e}")

def sign_fota_header(otbmsg: bytes, privkey) -> bytes:
    """Podpise message (102B META) a vráti 64B signature."""
    return privkey.sign(otbmsg)
```

`build_sig_payload`:

```python
def build_sig_payload(meta: bytes, privkey, key_id: int) -> bytes:
    """SIG = type+fota_prot_inf+old_sha256+key_id+signature[+signer_prefix].
    key_id=0 (nový formát): +4B prefix pubkey podpisovateľa -> 103 B; receiver
    hľadá prefix v s_authors a potom v ACL adminoch. key_id>=1 (legacy, 99 B):
    staré FW, s_authors[key_id-1]. Podpis vždy nad 102B META."""
    sig = sign_fota_header(meta, privkey) if privkey else bytes(64)
    old_sha256 = meta[70:102]
    out = bytes([FOTA_PKT_HDR_SIG, FOTA_PROT_INF_V0]) + old_sha256 + bytes([key_id]) + sig
    if key_id == FOTA_KEY_ID_PREFIX:
        if privkey is None:
            sys.exit("[CHYBA] key_id=0 (prefix formát) vyžaduje privkey — pre unsigned použi --keyid 1")
        out += privkey.pub[:4]
        assert len(out) == 103, f"SIG(v0-prefix) musi byt 103B, je {len(out)}"
    else:
        assert len(out) == 99, f"SIG musi byt 99B, je {len(out)}"
    return out
```

Arg parser (~626): zmeň default `--keyid` a pridaj `--privkey-hex`:

```python
    ap.add_argument('--privkey', help='Ed25519 private key (DER) na podpis HEADER')
    ap.add_argument('--privkey-hex', help='Ed25519 expandovaný kľúč (128 hex, companion formát)')
    ap.add_argument('--keyid',    type=int, default=0,
```

Za parse (kde sa dnes rieši `args.privkey`, ~716): jednotné načítanie + fallback pre unsigned:

```python
    if args.privkey_hex:
        privkey = load_ed25519_privkey_hex(args.privkey_hex)
        print(f'[init] Ed25519 privkey-hex (pub prefix {privkey.prefix.hex().upper()}, key_id=0x{args.keyid:02X})')
    elif args.privkey:
        privkey = load_ed25519_privkey(Path(args.privkey))
        print(f'[init] Ed25519 private key: {args.privkey} (pub prefix {privkey.prefix.hex().upper()}, key_id=0x{args.keyid:02X})')
    else:
        privkey = None
        if args.keyid == FOTA_KEY_ID_PREFIX:
            args.keyid = 1   # unsigned nejde s prefix formátom -> legacy zero-sig
            print('[init] bez privkey -> legacy key_id=1, nulový podpis')
```

(presné umiestnenie: existujúci blok s `[init] Ed25519 private key` — zachovaj okolitú logiku volania `privkey=privkey, key_id=args.keyid`.)

- [ ] **Step 4: Uprav `fota_export_pkg.py`**

`build_pkg`: default `keyid=0`; podpora hex kľúča; `signer_prefix` do `signed`:

```python
def build_pkg(old, new, patch_path, *, channel_name=FOTA_CHANNEL_NAME, channel_idx=1,
              freq=869.618, bw=62.5, sf=8, cr=5, scope='zerohop', path='',
              privkey=None, privkey_hex=None, keyid=0, created="1970-01-01T00:00:00Z"):
```

a v `if privkey:` bloku:

```python
    if privkey or privkey_hex:
        if privkey_hex:
            from fota_ed25519_expanded import key_from_hex
            pk = key_from_hex(privkey_hex)
        else:
            pk = load_ed25519_privkey(Path(privkey))
        total = (len(patch) + FOTA_CHUNK_DATA - 1) // FOTA_CHUNK_DATA
        meta = build_meta_payload(total, len(patch), patch_sha256, new_sha256, old_sha256)
        sig = build_sig_payload(meta, pk, keyid)
        pkg["signed"] = {"key_id": keyid,
                         "signer_prefix": pk.prefix.hex(),
                         "meta_b64": base64.b64encode(meta).decode(),
                         "sig_b64": base64.b64encode(sig).decode()}
```

Args: `ap.add_argument('--privkey'); ap.add_argument('--privkey-hex'); ap.add_argument('--keyid', type=int, default=0)` a do volania `build_pkg(..., privkey_hex=args.privkey_hex, ...)`.

- [ ] **Step 5: Uprav `fota_sender_mcpy.py` a `gen_fotapkg.py`**

`fota_sender_mcpy.py`: `--privkey-hex` arg + default `--keyid 0`; načítanie ako v Step 3 (hex > der > None s fallbackom na keyid=1); warning text nechaj.
`gen_fotapkg.py`: `--privkey-hex` + `--keyid` default 0; default test_key.der blok (~139–145) nechaj; do build_pkg pošli `privkey_hex=args.privkey_hex, keyid=args.keyid`; do súhrnného výpisu (~159) pridaj formát:

```python
    sgn = "signed" if (privkey or args.privkey_hex) else "UNSIGNED"
    fmt = "v0-prefix (nový; staré FW potrebujú --keyid 1)" if args.keyid == 0 else f"legacy key_id={args.keyid}"
```

a vypíš `fmt` pri súhrne.

- [ ] **Step 6: Over testy + smoke export**

Run: `D:\FkDev\.platformio\penv\Scripts\python.exe test_nrf-fota\test_fota_ed25519_expanded.py`
Expected: všetky OK riadky.
Run (smoke, použije 2 archívne buildy ak sú v `test_nrf-fota/builds/`, inak preskoč):
`D:\FkDev\.platformio\penv\Scripts\python.exe test_nrf-fota\gen_fotapkg.py --auto --device promicro`
Expected: výpis `signed` + `v0-prefix`; v JSON `signed.key_id == 0`, `signer_prefix == "c22f8ae0"`.

- [ ] **Step 7: Commit (MeshCore repo)**

```bash
git add test_nrf-fota/fota_sender.py test_nrf-fota/fota_export_pkg.py test_nrf-fota/fota_sender_mcpy.py test_nrf-fota/gen_fotapkg.py test_nrf-fota/test_fota_ed25519_expanded.py
git commit -m "feat(fota): SIG v0-prefix format (key_id=0 + 4B pubkey prefix) + --privkey-hex vo vsetkych sendroch"
```

---

### Task 4: FW — štruktúry (FotaProtocol.h, FotaState.h)

**Files:**
- Modify: `examples/simple_repeater/nrffota/FotaProtocol.h` (za `FOTA_PROT_INF_V0`, ~39; komentár FotaHdrSigPkt ~73–84)
- Modify: `examples/simple_repeater/nrffota/FotaState.h` (FOTA_META_MAGIC ~48, FotaMetaPersist ~50–68, FotaState ~74–102, FotaAuthorEntry ~130–133)

**Interfaces:**
- Produces: `FOTA_KEY_ID_PREFIX` (=0), `FOTA_SIG_PREFIX_LEN` (=4) v FotaProtocol.h; `FotaMetaPersist.hdr_signer_prefix[4]` + `FotaState.hdr_signer_prefix[4]`; `FOTA_META_MAGIC = 0x4F544102`; `FotaAuthorEntry` už len `{ uint8_t pub_key[32]; }`.

- [ ] **Step 1: FotaProtocol.h**

Za `#define FOTA_PROT_INF_V0   0x00` pridaj:

```c
//en: key_id=0 in SIG = "v0-prefix" format: signer is identified by the first
//en: 4 B of their Ed25519 pubkey APPENDED after signature[] (SIG = 99+4 = 103 B).
//en: The receiver looks the prefix up in s_authors, then in the ACL admins.
//en: key_id>=1 = legacy 99 B format (s_authors[key_id-1]) for old FW.
//sk: key_id=0 v SIG = "v0-prefix" formát: podpisovateľa identifikujú prvé
//sk: 4 B jeho Ed25519 pubkey PRIPOJENÉ za signature[] (SIG = 99+4 = 103 B).
//sk: Receiver hľadá prefix v s_authors, potom v ACL adminoch.
//sk: key_id>=1 = legacy 99 B formát (s_authors[key_id-1]) pre staré FW.
#define FOTA_KEY_ID_PREFIX   0x00
#define FOTA_SIG_PREFIX_LEN  4
```

V komentári nad `FotaHdrSigPkt` doplň za existujúci text (en aj sk vetu):

```c
//en: With key_id==FOTA_KEY_ID_PREFIX a 4 B signer_prefix follows the struct (103 B total).
//sk: Pri key_id==FOTA_KEY_ID_PREFIX za štruktúrou nasleduje 4 B signer_prefix (spolu 103 B).
```

- [ ] **Step 2: FotaState.h**

1. `#define FOTA_META_MAGIC  0x4F544102u   //en: "OTA\x02" — v2: + hdr_signer_prefix` (bump: starý meta.bin sa po update zahodí — bezpečné, session sa dá poslať znova).
2. Do `FotaMetaPersist` za `uint8_t hdr_sig[64];`:

```c
    uint8_t  hdr_signer_prefix[4]; //en: v0-prefix: first 4 B of signer pubkey (key_id==0)
                                   //sk: v0-prefix: prvé 4 B pubkey podpisovateľa (key_id==0)
```

3. Do `FotaState` za `uint8_t hdr_sig[64];` to isté pole (rovnaký komentár).
4. `FotaAuthorEntry`: odstráň `uint8_t id;` a uprav komentár:

```c
//en: Authorization table for Ed25519 verification (defined in FotaReceiver_signkey.cpp).
//en: Keys are addressed by pubkey prefix (v0-prefix format); legacy key_id N maps
//en: to s_authors[N-1] (test key = index 0 = legacy key_id 1).
//sk: Autorizačná tabuľka pre Ed25519 verify (definovaná vo FotaReceiver_signkey.cpp).
//sk: Kľúče sa adresujú prefixom pubkey (v0-prefix formát); legacy key_id N mapuje
//sk: na s_authors[N-1] (test kľúč = index 0 = legacy key_id 1).
typedef struct {
    uint8_t  pub_key[32];  //en: Ed25519 public key
} FotaAuthorEntry;
```

- [ ] **Step 3: Commit (MeshCore repo)**

Kompilácia sa overí až v Task 6 (signkey + receiver sa menia spolu); commitni štruktúry samostatne:

```bash
git add examples/simple_repeater/nrffota/FotaProtocol.h examples/simple_repeater/nrffota/FotaState.h
git commit -m "feat(fota): v0-prefix SIG konstanty + hdr_signer_prefix v state/meta + FotaAuthorEntry bez id"
```

---

### Task 5: FW — `FotaReceiver_signkey.cpp` (5 kľúčov bez id)

**Files:**
- Modify: `examples/simple_repeater/nrffota/FotaReceiver_signkey.cpp`

**Interfaces:**
- Consumes: `FotaAuthorEntry` bez `id` (Task 4); pub kľúče z Task 2 Step 3 výpisov.
- Produces: `s_authors[5]` — index 0 = test kľúč (legacy key_id 1), indexy 1–4 = `fota_signkey1..4`.

- [ ] **Step 1: Prepíš tabuľku**

Test záznam stráca `1,` a dostane tvar bez id; za neho vlož 4 snippety z keytoolu (Task 2). Vzor:

```c
extern const FotaAuthorEntry s_authors[] = {
    //en: index 0 — test keypair (privkey in test_nrf-fota/test_key.der; legacy key_id=1)
    //sk: index 0 — test keypair (privkey v test_nrf-fota/test_key.der; legacy key_id=1)
    { { 0xC2, 0x2F, 0x8A, 0xE0, 0x03, 0x51, 0xE7, 0x4A,
        0x81, 0x33, 0x8A, 0x93, 0xB8, 0x6C, 0x87, 0x45,
        0x8F, 0x05, 0xC4, 0xDD, 0x6A, 0xE9, 0xCE, 0xA5,
        0x49, 0xB0, 0x58, 0xE2, 0x3A, 0x0B, 0x5B, 0x51 } },
    //en: fota_signkey1.der — pub prefix <XXXXXXXX>
    { { /* 32 B z keytool výpisu */ } },
    // ... fota_signkey2..4 rovnako (každý s //en/+//sk komentárom s prefixom)
};
```

(Skutočné bajty vlož z `fota_keytool.py pub test_nrf-fota\fota_signkey<i>.der`.)

- [ ] **Step 2: Commit (MeshCore repo)**

```bash
git add examples/simple_repeater/nrffota/FotaReceiver_signkey.cpp
git commit -m "feat(fota): s_authors -> 5 klucov (test + fota_signkey1..4), adresovanie prefixom"
```

---

### Task 6: FW — receiver verify (prefix + ACL hook) a persistencia

**Files:**
- Modify: `examples/simple_repeater/nrffota/FotaReceiver.h` (deklarácia hooku, pri `fota_process` ~33)
- Modify: `examples/simple_repeater/nrffota/FotaReceiver.cpp` (`verify_header_signature` ~36–46, default hook, `save_meta` ~313–328, `load_meta`→resume ~581–599, `try_verify_header` ~903, `handle_sig` ~997–1026, `fota_clear` — nájdi a vynuluj nové pole)

**Interfaces:**
- Consumes: Task 4 konštanty/polia; `s_authors` bez id (Task 5).
- Produces: `int fota_acl_admin_pubkeys(const uint8_t prefix[4], const uint8_t* out_keys[], int max)` — deklarované vo FotaReceiver.h, default (0 kandidátov) vo FotaReceiver.cpp pri `#if !defined(FOTA_MESHCORE_BUILD)`; silnú implementáciu dodá Task 7. `FOTA_ACL_MAX_CANDIDATES` (=4).

- [ ] **Step 1: FotaReceiver.h — hook**

Za deklaráciu `bool fota_process(...)` pridaj:

```c
//en: v0-prefix ACL hook — the app layer (FotaMyMesh.cpp) returns pubkeys of ACL
//en: admins whose first 4 B match `prefix`. Default (non-MeshCore builds): 0.
//sk: v0-prefix ACL hook — aplikačná vrstva (FotaMyMesh.cpp) vráti pubkey ACL
//sk: adminov, ktorých prvé 4 B sedia s `prefix`. Default (ne-MeshCore buildy): 0.
#define FOTA_ACL_MAX_CANDIDATES 4
int fota_acl_admin_pubkeys(const uint8_t prefix[4], const uint8_t* out_keys[], int max);
```

- [ ] **Step 2: FotaReceiver.cpp — default hook + verify**

Pod `verify_header_signature` (alebo nad ňu) pridaj default:

```c
#if !defined(FOTA_MESHCORE_BUILD)
//en: Default — no ACL on this platform (ZephCore): only s_authors verify.
//sk: Default — platforma bez ACL (ZephCore): overuje sa len s_authors.
int fota_acl_admin_pubkeys(const uint8_t prefix[4], const uint8_t* out_keys[], int max) {
    (void)prefix; (void)out_keys; (void)max;
    return 0;
}
#endif
```

Prepíš `verify_header_signature`:

```c
static bool verify_header_signature(const uint8_t* sig,
                                    const uint8_t* msg, size_t msg_len,
                                    uint8_t key_id, const uint8_t* signer_prefix) {
    if (key_id == FOTA_KEY_ID_PREFIX) {
        //en: v0-prefix — signer by 4 B pubkey prefix: s_authors first, then ACL admins
        //sk: v0-prefix — podpisovateľ podľa 4 B prefixu pubkey: najprv s_authors, potom ACL admini
        for (int i = 0; i < s_author_count; i++) {
            if (memcmp(s_authors[i].pub_key, signer_prefix, FOTA_SIG_PREFIX_LEN) == 0 &&
                fota_ed25519_verify(sig, s_authors[i].pub_key, msg, msg_len)) {
                FOTA_DEBUG_PRINTLN("[FOTA] HEADER signer=builtin[%d]", i);
                return true;
            }
        }
        const uint8_t* acl_keys[FOTA_ACL_MAX_CANDIDATES];
        int n = fota_acl_admin_pubkeys(signer_prefix, acl_keys, FOTA_ACL_MAX_CANDIDATES);
        for (int i = 0; i < n; i++) {
            if (fota_ed25519_verify(sig, acl_keys[i], msg, msg_len)) {
                FOTA_DEBUG_PRINTLN("[FOTA] HEADER signer=ACL admin");
                return true;
            }
        }
        FOTA_DEBUG_PRINTLN("[FOTA] signer prefix %02X%02X%02X%02X not found/valid (authors+ACL)",
            (unsigned)signer_prefix[0], (unsigned)signer_prefix[1],
            (unsigned)signer_prefix[2], (unsigned)signer_prefix[3]);
        return false;
    }
    //en: legacy: key_id N -> s_authors[N-1] (test key = index 0)
    //sk: legacy: key_id N -> s_authors[N-1] (test kľúč = index 0)
    int idx = (int)key_id - 1;
    if (idx >= 0 && idx < s_author_count) {
        return fota_ed25519_verify(sig, s_authors[idx].pub_key, msg, msg_len);
    }
    FOTA_DEBUG_PRINTLN("[FOTA] UNKNOWN key_id=0x%X", (unsigned)key_id);
    return false;
}
```

V `try_verify_header()` uprav volanie (~903):

```c
    ok = verify_header_signature(fota.hdr_sig, meta, 102u, fota.hdr_key_id,
                                 fota.hdr_signer_prefix);
```

- [ ] **Step 3: `handle_sig` — parse prefixu**

Za kontrolu `plen < sizeof(FotaHdrSigPkt)` (997–999) a gating blok vlož parse; uprav dup check a uloženie (1015–1022):

```c
    //en: v0-prefix: with key_id==0 a 4 B signer prefix must follow the struct
    //sk: v0-prefix: pri key_id==0 musí za štruktúrou nasledovať 4 B prefix podpisovateľa
    const uint8_t* prefix = NULL;
    if (pkt->key_id == FOTA_KEY_ID_PREFIX) {
        if (plen < (int)sizeof(FotaHdrSigPkt) + FOTA_SIG_PREFIX_LEN) {
            FOTA_DEBUG_PRINTLN("[FOTA] SIG: key_id=0 without signer prefix — drop"); return;
        }
        prefix = plain + sizeof(FotaHdrSigPkt);
    }
```

(vlož PRED `if (!(fota.status & FOTA_ST_RECEIVING))`, aby chybný paket nezaložil session), a potom:

```c
    bool dup_sig = fota.sig_recv && fota.hdr_key_id == pkt->key_id
                && memcmp(fota.hdr_sig, pkt->signature, 64) == 0
                && (prefix == NULL || memcmp(fota.hdr_signer_prefix, prefix, FOTA_SIG_PREFIX_LEN) == 0);
    fota.hdr_key_id = pkt->key_id;
    memcpy(fota.hdr_sig, pkt->signature, 64);
    if (prefix) memcpy(fota.hdr_signer_prefix, prefix, FOTA_SIG_PREFIX_LEN);
    else        memset(fota.hdr_signer_prefix, 0, FOTA_SIG_PREFIX_LEN);
```

- [ ] **Step 4: Persistencia**

`save_meta` (za `memcpy(mp.hdr_sig, ...)`): `memcpy(mp.hdr_signer_prefix, fota.hdr_signer_prefix, 4);`
resume blok (za `memcpy(fota.hdr_sig, mp.hdr_sig, 64);`): `memcpy(fota.hdr_signer_prefix, mp.hdr_signer_prefix, 4);`
`fota_clear` (nájdi definíciu; ak zeruje celý struct memset-om, netreba nič — over): prefix musí byť po cleare nulový.

- [ ] **Step 5: Build verify**

Run: `pio run -e ProMicro_repeater_fota`
Expected: SUCCESS (bez warningov v nrffota súboroch). Pozn.: build bumpne lokálne `build_number.txt` (skip-worktree) — to je v poriadku.

- [ ] **Step 6: Commit (MeshCore repo)**

```bash
git add examples/simple_repeater/nrffota/FotaReceiver.h examples/simple_repeater/nrffota/FotaReceiver.cpp
git commit -m "feat(fota): receiver v0-prefix verify (s_authors+ACL hook), persist signer prefixu"
```

---

### Task 7: FW — ACL hook v `FotaMyMesh` + build verify všetkých FOTA env

**Files:**
- Modify: `examples/simple_repeater/nrffota/FotaMyMesh.h` (deklarácia metódy — súbor sa include-uje DOVNÚTRA `class MyMesh`)
- Modify: `examples/simple_repeater/nrffota/FotaMyMesh.cpp` (metóda + silná verzia free funkcie)

**Interfaces:**
- Consumes: `fota_acl_admin_pubkeys` deklarácia + `FOTA_ACL_MAX_CANDIDATES` (Task 6), `ClientACL` (`acl.getNumClients()`, `acl.getClientByIdx(i)`, `c->isAdmin()`, `c->id.pub_key`), globál `the_mesh` (main.cpp).
- Produces: `int MyMesh::fotaAclAdminPubkeys(const uint8_t prefix[4], const uint8_t* out_keys[], int max)`.

- [ ] **Step 1: Deklarácia vo FotaMyMesh.h**

Do sekcie metód (public časť, pri Serial-only debug CLI ~51) pridaj:

```c
  //en: v0-prefix FOTA: return pubkeys of ACL admins matching a 4 B prefix
  //sk: v0-prefix FOTA: vráť pubkey ACL adminov so zhodným 4 B prefixom
  int fotaAclAdminPubkeys(const uint8_t prefix[4], const uint8_t* out_keys[], int max);
```

- [ ] **Step 2: Implementácia vo FotaMyMesh.cpp**

```cpp
//en: v0-prefix FOTA signature — candidates from the ACL: admin role only
//en: (PERM_ACL_ADMIN; Read/Write and below do NOT qualify), matched by the
//en: first 4 B of their identity pubkey. Called from try_verify_header()
//en: in loop() context (deferred), so the Ed25519 verify cost stays off the RX path.
//sk: v0-prefix FOTA podpis — kandidáti z ACL: len admin rola (PERM_ACL_ADMIN;
//sk: Read/Write a nižšie sa NEkvalifikujú), zhoda prvých 4 B identity pubkey.
//sk: Volané z try_verify_header() v loop() kontexte (deferovane), takže cena
//sk: Ed25519 verify neblokuje RX cestu.
int MyMesh::fotaAclAdminPubkeys(const uint8_t prefix[4], const uint8_t* out_keys[], int max) {
  int n = 0;
  int cnt = acl.getNumClients();
  for (int i = 0; i < cnt && n < max; i++) {
    ClientInfo* c = acl.getClientByIdx(i);
    if (!c->isAdmin()) continue;
    if (memcmp(c->id.pub_key, prefix, FOTA_SIG_PREFIX_LEN) != 0) continue;
    out_keys[n++] = c->id.pub_key;
  }
  return n;
}

//en: strong override of the FotaReceiver default (MeshCore build)
//sk: silná verzia defaultu z FotaReceiver (MeshCore build)
extern MyMesh the_mesh;
int fota_acl_admin_pubkeys(const uint8_t prefix[4], const uint8_t* out_keys[], int max) {
  return the_mesh.fotaAclAdminPubkeys(prefix, out_keys, max);
}
```

(Umiestni k ostatným free funkciám vo FotaMyMesh.cpp; over, že súbor include-uje hlavičky, kde je `ClientACL`/`the_mesh` typ — `MyMesh.h` include je tam už dnes.)

- [ ] **Step 3: Build verify všetky 3 FOTA envy**

Run: `pio run -e ProMicro_repeater_fota -e SenseCap_Solar_repeater_fota -e Xiao_nrf52_repeater_fota`
Expected: 3× SUCCESS.

- [ ] **Step 4: Commit (MeshCore repo)**

```bash
git add examples/simple_repeater/nrffota/FotaMyMesh.h examples/simple_repeater/nrffota/FotaMyMesh.cpp
git commit -m "feat(fota): ACL admin lookup hook (MyMesh::fotaAclAdminPubkeys) pre v0-prefix verify"
```

---

### Task 8: Dart — expandovaný signer (`fota_ed25519_expanded.dart`)

**Repo:** `D:\FkDev\FkProj\VSC\meshcore-open`

**Files:**
- Create: `lib/fota/services/fota_ed25519_expanded.dart`
- Test: `test/fota_expanded_signer_test.dart`

**Interfaces:**
- Produces: `abstract class FotaSignKey { Uint8List get pub; Uint8List sign(Uint8List msg); }`; `class FotaSeedKey implements FotaSignKey` (pinenacl, ctor `FotaSeedKey(Uint8List seed32)`); `class FotaExpandedKey implements FotaSignKey` (ctor `FotaExpandedKey(Uint8List expanded64)`, BigInt port orlp `sign.c` + `derive_pub`).

- [ ] **Step 1: Vygeneruj golden vektory Pythonom**

Run (MeshCore repo):

```
D:\FkDev\.platformio\penv\Scripts\python.exe -c "import sys, hashlib; sys.path.insert(0, 'test_nrf-fota'); from fota_ed25519_expanded import expand_seed, derive_pub, sign_expanded; [print(f'case {i}:', hashlib.sha256(b'fota-test-seed-%d' % i).digest().hex(), expand_seed(hashlib.sha256(b'fota-test-seed-%d' % i).digest()).hex(), derive_pub(expand_seed(hashlib.sha256(b'fota-test-seed-%d' % i).digest())).hex(), sign_expanded((hashlib.sha512(b'fota-msg-%d' % i).digest() + b'x'*38), expand_seed(hashlib.sha256(b'fota-test-seed-%d' % i).digest())).hex(), sep='\n  ') for i in range(2)]"
```

Zapíš si výstup (seed / expanded / pub / sig pre case 0 a 1; msg = `sha512(b'fota-msg-<i>') + b'x'*38`).

- [ ] **Step 2: Napíš zlyhávajúci test**

`test/fota_expanded_signer_test.dart` (hex hodnoty doplň zo Step 1):

```dart
import 'dart:convert';
import 'dart:typed_data';
import 'package:crypto/crypto.dart' as c;
import 'package:flutter_test/flutter_test.dart';
import 'package:meshcore_open/fota/services/fota_ed25519_expanded.dart';

Uint8List _hex(String s) => Uint8List.fromList([
      for (var i = 0; i < s.length; i += 2) int.parse(s.substring(i, i + 2), radix: 16)
    ]);

Uint8List _msg(int i) => Uint8List.fromList(
    c.sha512.convert(utf8.encode('fota-msg-$i')).bytes + List.filled(38, 0x78));

void main() {
  // golden vektory z test_nrf-fota/fota_ed25519_expanded.py (viď plán Task 8 Step 1)
  const expanded0 = '<EXPANDED0_HEX>';
  const pub0 = '<PUB0_HEX>';
  const sig0 = '<SIG0_HEX>';
  const expanded1 = '<EXPANDED1_HEX>';
  const pub1 = '<PUB1_HEX>';
  const sig1 = '<SIG1_HEX>';

  test('FotaExpandedKey derive pub + sign == python golden', () {
    final k0 = FotaExpandedKey(_hex(expanded0));
    expect(k0.pub, _hex(pub0));
    expect(k0.sign(_msg(0)), _hex(sig0));
    final k1 = FotaExpandedKey(_hex(expanded1));
    expect(k1.pub, _hex(pub1));
    expect(k1.sign(_msg(1)), _hex(sig1));
  });

  test('FotaSeedKey == pinenacl (seed cesta nezmenená)', () {
    final seed = Uint8List.fromList(
        c.sha256.convert(utf8.encode('fota-test-seed-0')).bytes);
    final k = FotaSeedKey(seed);
    expect(k.pub.length, 32);
    expect(k.sign(_msg(0)).length, 64);
  });
}
```

(Import path `package:meshcore_open/...` uprav podľa `name:` v `pubspec.yaml` appky.)

- [ ] **Step 3: Over, že zlyhá**

Run (z meshcore-open): `D:\FkDev\Tools\flutter\bin\flutter.bat test test/fota_expanded_signer_test.dart`
Expected: FAIL — súbor `fota_ed25519_expanded.dart` neexistuje.

- [ ] **Step 4: Implementuj**

```dart
import 'dart:typed_data';
import 'package:crypto/crypto.dart' as c;
import 'package:pinenacl/ed25519.dart' as nacl;

/// Jednotný podpisový kľúč pre FOTA SIG (seed .der aj companion expanded hex).
abstract class FotaSignKey {
  Uint8List get pub;
  Uint8List sign(Uint8List msg);
}

/// 32 B seed (z PKCS#8 .der / 64-znakový hex) — podpis cez pinenacl ako doteraz.
class FotaSeedKey implements FotaSignKey {
  final Uint8List seed;
  final nacl.SigningKey _sk;
  FotaSeedKey(this.seed) : _sk = nacl.SigningKey(seed: seed);
  @override
  Uint8List get pub => Uint8List.fromList(_sk.publicKey.toUint8List());
  @override
  Uint8List sign(Uint8List msg) => Uint8List.fromList(_sk.sign(msg).signature);
}

/// 64 B expandovaný kľúč (companion identity, 128-znakový hex).
/// Port lib/ed25519 (orlp): SHA512(seed)+clamp už MÁ hotové — podpis preskočí
/// expanziu. Výsledok je štandardný RFC8032 podpis. BigInt aritmetika stačí —
/// podpisuje sa jedna 102 B META, výkon je irelevantný.
class FotaExpandedKey implements FotaSignKey {
  final Uint8List expanded;
  late final Uint8List _pub = _derivePub(expanded);
  FotaExpandedKey(this.expanded) {
    if (expanded.length != 64) {
      throw ArgumentError('expanded key must be 64 bytes');
    }
  }
  @override
  Uint8List get pub => _pub;

  @override
  Uint8List sign(Uint8List msg) {
    final a = _leToBig(expanded.sublist(0, 32));
    final prefix = expanded.sublist(32);
    final r = _leToBig(_sha512(Uint8List.fromList(prefix + msg))) % _L;
    final R = _encodePoint(_scalarMultBase(r));
    final k = _leToBig(_sha512(Uint8List.fromList(R + _pub + msg))) % _L;
    final S = (r + k * a) % _L;
    return Uint8List.fromList(R + _bigToLe(S, 32));
  }

  // ── ed25519 field/point math (ref10 ekvivalent, BigInt) ──
  static final BigInt _p = (BigInt.one << 255) - BigInt.from(19);
  static final BigInt _L =
      (BigInt.one << 252) + BigInt.parse('27742317777372353535851937790883648493');
  static final BigInt _d =
      (BigInt.from(-121665) * _inv(BigInt.from(121666))) % _p;
  static final BigInt _I = _p > BigInt.zero
      ? BigInt.two.modPow((_p - BigInt.one) >> 2, _p)
      : BigInt.zero;

  static BigInt _inv(BigInt x) => x.modPow(_p - BigInt.two, _p);

  static List<BigInt> get _B {
    final by = (BigInt.from(4) * _inv(BigInt.from(5))) % _p;
    final bx = _xRecover(by);
    return [bx, by, BigInt.one, (bx * by) % _p];
  }

  static BigInt _xRecover(BigInt y) {
    final xx = ((y * y - BigInt.one) % _p) * _inv((_d * y * y + BigInt.one) % _p) % _p;
    var x = xx.modPow((_p + BigInt.from(3)) >> 3, _p);
    if ((x * x - xx) % _p != BigInt.zero) x = (x * _I) % _p;
    if (x.isOdd) x = _p - x;
    return x;
  }

  static List<BigInt> _edAdd(List<BigInt> P, List<BigInt> Q) {
    final a = ((P[1] - P[0]) * (Q[1] - Q[0])) % _p;
    final b = ((P[1] + P[0]) * (Q[1] + Q[0])) % _p;
    final cc = (P[3] * BigInt.two * _d * Q[3]) % _p;
    final dd = (P[2] * BigInt.two * Q[2]) % _p;
    final e = b - a, f = dd - cc, g = dd + cc, h = b + a;
    return [(e * f) % _p, (g * h) % _p, (f * g) % _p, (e * h) % _p];
  }

  static List<BigInt> _scalarMultBase(BigInt e) {
    var P = _B;
    var Q = [BigInt.zero, BigInt.one, BigInt.one, BigInt.zero];
    while (e > BigInt.zero) {
      if (e.isOdd) Q = _edAdd(Q, P);
      P = _edAdd(P, P);
      e >>= 1;
    }
    return Q;
  }

  static Uint8List _encodePoint(List<BigInt> P) {
    final zi = _inv(P[2]);
    final x = (P[0] * zi) % _p;
    final y = (P[1] * zi) % _p;
    final v = y | ((x & BigInt.one) << 255);
    return _bigToLe(v, 32);
  }

  static Uint8List _derivePub(Uint8List expanded64) =>
      _encodePoint(_scalarMultBase(_leToBig(expanded64.sublist(0, 32))));

  static Uint8List _sha512(Uint8List d) =>
      Uint8List.fromList(c.sha512.convert(d).bytes);
  static BigInt _leToBig(List<int> b) {
    var v = BigInt.zero;
    for (var i = b.length - 1; i >= 0; i--) {
      v = (v << 8) | BigInt.from(b[i]);
    }
    return v;
  }
  static Uint8List _bigToLe(BigInt v, int len) {
    final out = Uint8List(len);
    var x = v;
    for (var i = 0; i < len; i++) {
      out[i] = (x & BigInt.from(0xff)).toInt();
      x >>= 8;
    }
    return out;
  }
}
```

POZOR na `%` v Darte: pre BigInt vracia nezáporný zvyšok pri kladnom module — `_d` s negatívnym čitateľom over testom (golden test to odhalí).

- [ ] **Step 5: Over test**

Run: `D:\FkDev\Tools\flutter\bin\flutter.bat test test/fota_expanded_signer_test.dart`
Expected: PASS (2 testy).

- [ ] **Step 6: Commit (meshcore-open repo)**

```bash
git add lib/fota/services/fota_ed25519_expanded.dart test/fota_expanded_signer_test.dart
git commit -m "feat(fota): FotaSignKey + FotaExpandedKey (companion identity hex signer) + golden test"
```

---

### Task 9: Dart — KeyStore, buildSig v0-prefix, sender

**Repo:** `D:\FkDev\FkProj\VSC\meshcore-open`

**Files:**
- Modify: `lib/fota/services/fota_key_store.dart`
- Modify: `lib/fota/services/fota_payload_builder.dart` (`signMeta`/`buildSig` ~23–46)
- Modify: `lib/fota/services/fota_sender.dart` (`seed32` ~41, ~63, ~133)
- Modify: `lib/fota/screens/fota_screen.dart` (~458–489, `toJob` volanie)
- Modify: `lib/fota/models/fotapkg.dart` (keyId default ~93)
- Test: `test/fota_expanded_signer_test.dart` (rozšírenie o buildSig)

**Interfaces:**
- Consumes: `FotaSignKey`, `FotaSeedKey`, `FotaExpandedKey` (Task 8).
- Produces: `FotaKeyStore.importHex(String hex)` (64 znakov→seed / 128→expanded, inak `ArgumentError`), `FotaKeyStore.loadSignKey() -> Future<FotaSignKey?>` (nový slot `fota_sign_key` = `"seed:<hex>"|"expanded:<hex>"`, fallback na legacy `fota_ed25519_seed`); `FotaPayloadBuilder.buildSig(Uint8List meta, FotaSignKey? key, int keyId)` → 103 B pri keyId==0 (+prefix), 99 B legacy; `FotaSendConfig.signKey` (nahrádza `seed32`); raw pkg: `keyId = (key == null || legacy) ? 1 : 0`.

- [ ] **Step 1: Rozšír test o buildSig**

Do `test/fota_expanded_signer_test.dart` pridaj:

```dart
  test('buildSig v0-prefix / legacy', () {
    final b = FotaPayloadBuilder();
    final meta = b.buildMeta(100, Uint8List(32), Uint8List(32), Uint8List(32));
    final k = FotaExpandedKey(_hex(expanded0));
    final sig0 = b.buildSig(meta, k, 0);
    expect(sig0.length, 103);
    expect(sig0[35], 0);
    expect(sig0.sublist(99), k.pub.sublist(0, 4));
    final sig1 = b.buildSig(meta, k, 1);
    expect(sig1.length, 99);
    final sigU = b.buildSig(meta, null, 1);
    expect(sigU.sublist(36, 100), Uint8List(64));
  });
```

(+ import `fota_payload_builder.dart`.) Run → FAIL (buildSig má starú signatúru).

- [ ] **Step 2: `fota_payload_builder.dart`**

Nahraď `signMeta`+`buildSig` (import `fota_ed25519_expanded.dart`, odstráň priamy pinenacl import ak už nie je potrebný):

```dart
  /// RFC8032 Ed25519 signature of [meta] (64B). Zeros if [key] is null.
  Uint8List signMeta(Uint8List meta, FotaSignKey? key) =>
      key == null ? Uint8List(64) : key.sign(meta);

  /// SIG payload. keyId==0 (v0-prefix): +4B signer pubkey prefix -> 103 B;
  /// keyId>=1 (legacy, old FW): 99 B, s_authors[keyId-1] on the receiver.
  Uint8List buildSig(Uint8List meta, FotaSignKey? key, int keyId) {
    if (keyId == 0 && key == null) {
      throw ArgumentError('key_id=0 (v0-prefix) requires a signing key');
    }
    final sig = signMeta(meta, key);
    final oldSha256 = meta.sublist(70, 102);
    final b = BytesBuilder();
    b.addByte(kFotaPktHdrSig);
    b.addByte(kFotaProtInfV0);
    b.add(oldSha256);
    b.addByte(keyId & 0xFF);
    b.add(sig);
    if (keyId == 0) b.add(key!.pub.sublist(0, 4));
    final out = b.toBytes();
    assert(out.length == (keyId == 0 ? 103 : 99),
        'SIG must be ${keyId == 0 ? 103 : 99}B, is ${out.length}');
    return out;
  }
```

- [ ] **Step 3: `fota_key_store.dart`**

Pridaj (ponechaj existujúce metódy kvôli kompatibilite):

```dart
  static const _signKey = 'fota_sign_key'; // "seed:<64hex>" | "expanded:<128hex>"

  /// Import z hexu: 64 znakov = seed (.der), 128 znakov = expandovaný companion kľúč.
  Future<void> importHex(String hex) async {
    final h = hex.trim().toLowerCase().replaceAll(RegExp(r'[\s:]'), '');
    if (!RegExp(r'^[0-9a-f]+$').hasMatch(h)) throw ArgumentError('not hex');
    if (h.length == 64) {
      await _s.write(key: _signKey, value: 'seed:$h');
    } else if (h.length == 128) {
      await _s.write(key: _signKey, value: 'expanded:$h');
    } else {
      throw ArgumentError('expected 64 (seed) or 128 (expanded) hex chars, got ${h.length}');
    }
  }

  Future<FotaSignKey?> loadSignKey() async {
    final v = await _s.read(key: _signKey);
    if (v != null) {
      final i = v.indexOf(':');
      final bytes = _unhex(v.substring(i + 1));
      return v.startsWith('seed:') ? FotaSeedKey(bytes) : FotaExpandedKey(bytes);
    }
    final legacy = await loadSeed(); // starý slot fota_ed25519_seed
    return legacy == null ? null : FotaSeedKey(legacy);
  }

  Future<void> clearSignKey() async {
    await _s.delete(key: _signKey);
    await clear();
  }

  static Uint8List _unhex(String v) => Uint8List.fromList(
      [for (var i = 0; i < v.length; i += 2) int.parse(v.substring(i, i + 2), radix: 16)]);
```

(+ import `fota_ed25519_expanded.dart`.)

- [ ] **Step 4: `fota_sender.dart` + `fotapkg.dart` + `fota_screen.dart`**

- `FotaSendConfig`: `final Uint8List? seed32;` → `final FotaSignKey? signKey;` (a ctor param `this.signKey`); riadok 133: `final sig = job.presignedSig ?? _b.buildSig(meta, cfg.signKey, job.keyId);`
- `fotapkg.dart` ~93: nechaj (presigned keyId z JSON; raw default 1 — prepíše sa v screene).
- `fota_screen.dart` ~458: 

```dart
      FotaSignKey? key;
      if (pkg.meta == null) key = await FotaKeyStore().loadSignKey(); // raw → need key
```

`toJob()`: ak má `keyId` parameter, pošli `keyId: (key == null || _legacySig) ? 1 : 0`; ak nemá, nastav v `FotaSendConfig`/job podľa skutočnej signatúry `toJob` (over v `fotapkg.dart` ~104 — keyId ide z pkg; pridaj do `toJob({int? keyIdOverride})` optional override). A `seed32: seed` → `signKey: key`.
- `_legacySig`: nový `bool _legacySig = false;` state + checkbox vo UI vedľa send ovládania:

```dart
        CheckboxListTile(
          dense: true,
          title: const Text('Legacy signature (old FW)'),
          value: _legacySig,
          onChanged: _busy ? null : (v) => setState(() => _legacySig = v ?? false),
        ),
```

- [ ] **Step 5: Key management dialóg**

Do `fota_screen.dart` AppBar/actions pridaj IconButton (`Icons.key`) → jednoduchý dialóg: TextField (hex, autodetekcia 64/128), tlačidlá Import / Clear, a riadok so stavom (`loadSignKey()` → null = "no key", inak typ + pub prefix hex — pub cez `key.pub.sublist(0,4)`). Implementuj ako `Future<void> _showKeyDialog()` + `showDialog` s `StatefulBuilder`; po Importe `FotaKeyStore().importHex(text)` v try/catch so SnackBar chybou.

- [ ] **Step 6: Testy + analýza**

Run: `D:\FkDev\Tools\flutter\bin\flutter.bat test`
Expected: všetky testy PASS (vrátane existujúceho golden `buildSig` testu — ten uprav, ak volá starú signatúru: seed → `FotaSeedKey(seed)`).
Run: `D:\FkDev\Tools\flutter\bin\flutter.bat analyze lib/fota test`
Expected: bez nových errorov.

- [ ] **Step 7: Commit (meshcore-open repo)**

```bash
git add lib/fota test/
git commit -m "feat(fota): v0-prefix SIG (key_id=0+prefix), import companion identity hex, legacy checkbox"
```

---

### Task 10: Docs + push (oba repá)

**Files:**
- Modify: `fkclaude/fcl_readme_nrf-fota.md` (sekcia Ovládanie/podpisovanie), `fkclaude/fcl_readme_tech_nrf-fota.md` (SIG formát, ACL verify, keytool), `fkclaude/fcl_HowTo_BuildApp&fotapkg.md` (podpis + keytool príkazy)
- Modify: `examples/simple_repeater/nrffota/README.md` (SIG 103 B / v0-prefix zmienka v protokole)

**Interfaces:** — (dokumentácia)

- [ ] **Step 1: Doplň docs**

Popíš: v0-prefix formát (key_id=0 + 4 B prefix, 103 B), poradie hľadania (s_authors → ACL admini, len PERM_ACL_ADMIN), legacy `--keyid 1` pre staré FW (prechod flotily!), `--privkey-hex` (companion identity hex), `fota_keytool.py` (gen/der2hex/pub; hex2der neexistuje — jednosmerná SHA512), `fota_signkey1..4.der` (gitignored, index 1–4 v s_authors), appka: import hexu (64/128), checkbox Legacy signature. Nezakladaj nové súbory, rozšír existujúce sekcie.

- [ ] **Step 2: Commit + push (MeshCore)**

```bash
git add fkclaude examples/simple_repeater/nrffota/README.md
git commit -m "docs(fkclaude): FOTA v0-prefix podpis, ACL verify, fota_keytool"
git push origin features/nrf-fota
```

- [ ] **Step 3: Push (meshcore-open)**

```bash
git push origin feature/nrf-ota-sender
```

---

### Task 11: Manuálny on-device e2e (ráno, s hardvérom — NEBLOKUJE plán)

Bez hardvéru sa nedá spustiť automaticky; priprav len príkazy do docs (Task 10) a over, že existujúci `fota_test_lora_repeater.py` (legacy `--keyid 1` default → po Task 3 default 0! — **over a uprav test na explicitné `--keyid 1`**, nech ostáva legacy regresiou):

- [ ] **Step 1:** V `fota_test_lora_repeater.py` skontroluj default keyid (import z fota_sender → ak preberá default 0, doplň explicitne `--keyid 1` v dokumentovanom príkaze/testovej konfigurácii tak, aby test ostal legacy regresiou). Ak test volá `build_sig_payload(meta, privkey, 1)` napriamo, nič netreba.
- [ ] **Step 2:** Do `fkclaude/fcl_readme_nrf-fota.md` (runbook sekcia) dopíš manuálny postup: (a) flash nového FW; (b) `fota_sender.py --privkey test_key.der` (v0-prefix, builtin); (c) admin login z appky → ACL; (d) `--privkey-hex <companion hex>` → ACL cesta; (e) negatívny test s cudzím kľúčom → `signer prefix ... not found`.

---

## Self-review (vykonaná)

- Spec coverage: wire formát (T3/T4/T6), s_authors bez id + 4 kľúče (T2/T5), ACL hook len admin (T6/T7), meta persist + magic bump (T4/T6), PC tooling + hex + keytool (T1–T3), appka import/signer/checkbox (T8/T9), docs (T10), e2e poznámky (T11).
- Bez placeholderov: `<XXXXXXXX>`/`<EXPANDED0_HEX>` sú vedomé — hodnoty vzniknú až generovaním kľúčov (T2) a golden vektorov (T8 Step 1); postup ich získania je v krokoch uvedený.
- Typová konzistencia: `ExpandedKey.prefix`=pub[:4] (T1) používaný v T3; `FotaSignKey`/`FotaSeedKey`/`FotaExpandedKey` konzistentné T8→T9; `fota_acl_admin_pubkeys` signatúra zhodná T6/T7; `FOTA_KEY_ID_PREFIX`/`FOTA_SIG_PREFIX_LEN` zhodné T4/T6.

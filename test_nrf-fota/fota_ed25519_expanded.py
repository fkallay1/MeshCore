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

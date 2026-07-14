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

def test_sig_payload():
    import fota_sender as S
    from fota_ed25519_expanded import expand_seed, ExpandedKey
    seed = hashlib.sha256(b'fota-test-seed-0').digest()
    k = ExpandedKey(expand_seed(seed))
    meta = S.build_meta_payload(1, 100, b'\x11' * 32, b'\x22' * 32, b'\x33' * 32)
    # layout: [0]=type [1]=prot_inf [2:34]=old_sha256 [34]=key_id [35:99]=sig [99:103]=prefix
    # nový formát: key_id=0 → 103 B, prefix na konci
    sig0 = S.build_sig_payload(meta, k, 0)
    assert len(sig0) == 103 and sig0[34] == 0 and sig0[-4:] == k.prefix
    assert sig0[35:99] == k.sign(meta)
    # legacy: key_id=1 → 99 B, bajt-identické so starým formátom
    sig1 = S.build_sig_payload(meta, k, 1)
    assert len(sig1) == 99 and sig1[34] == 1 and sig1[35:99] == k.sign(meta)
    # unsigned legacy: privkey None → nulový podpis, 99 B
    sigu = S.build_sig_payload(meta, None, 1)
    assert len(sigu) == 99 and sigu[35:99] == bytes(64)
    # load_ed25519_privkey vracia ExpandedKey (jednotný signer)
    tk = S.load_ed25519_privkey(Path(__file__).parent / 'test_key.der')
    assert isinstance(tk, ExpandedKey) and tk.prefix == bytes([0xC2, 0x2F, 0x8A, 0xE0])
    print("OK: build_sig_payload novy/legacy/unsigned format")

def main():
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
    test_sig_payload()

if __name__ == '__main__':
    main()

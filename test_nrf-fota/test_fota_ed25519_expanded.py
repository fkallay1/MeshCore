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

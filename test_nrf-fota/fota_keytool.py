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

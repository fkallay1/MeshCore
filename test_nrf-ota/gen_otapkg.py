#!/usr/bin/env python3
"""
gen_otapkg.py — vygeneruje fw.otapkg.json (upgrade) a fw_reverse.otapkg.json
(rollback) pre Flutter OTA appku (mc-fotanrf-otapkg/1).

Tri režimy:
  1) --from-hex <firmware.hex>   extrahuje app image z HEX, archivuje ho ako
                                 builds/fw_<build#>.bin, a (ak existuje predošlý
                                 build) vygeneruje obe json medzi posledným a
                                 predposledným buildom. (default HEX:
                                 .pio/build/ProMicro_repeater_ota/firmware.hex)
  2) --auto                      vyberie 2 najnovšie buildy z builds/ a vygeneruje.
  3) --old A.bin --new B.bin     explicitné bin súbory.

  fw.otapkg.json         = patch  old → new   (upgrade na novší build)
  fw_reverse.otapkg.json = patch  new → old   (rollback / opačný smer; iný delta)

Podpisovanie: ak existuje --privkey (default test_key.der, ak je prítomný),
oba balíky dostanú 'signed' blok (Ed25519). Radio/scope/kanál = rovnaké defaulty
ako ota_export_pkg.py; prepísateľné argumentmi.

Archív .bin je lokálny (gitignored) — slúži ako "pamäť posledných buildov".
"""
import argparse
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ota_export_pkg as X  # build_pkg(), reuse celej ota_sender pipeline
from ota_sender import OTA_CHANNEL_NAME

MAGIC = b"FKFWID01"
OFF_BUILD = 12  # uint32 LE za magicom (zhodné s gen_fw_trailer.py)

DEFAULT_HEX = HERE.parent / ".pio" / "build" / "ProMicro_repeater_ota" / "firmware.hex"
BUILDS_DIR = HERE / "builds"
FOTAPKG_DIR = HERE / "fotapkg_json"  # default cieľ pre vygenerované .otapkg.json


# ---- Intel HEX → flat app image (rovnaká logika ako gen_fw_trailer.read_ihex) ----
def read_ihex(path: Path) -> bytearray:
    ext = 0
    mem = {}
    mn = mx = None
    for line in path.read_text().splitlines():
        if not line.startswith(":"):
            continue
        b = bytes.fromhex(line[1:])
        ln, rectype = b[0], b[3]
        off = (b[1] << 8) | b[2]
        data = b[4:4 + ln]
        if rectype == 0x00:
            a = ext + off
            for i, by in enumerate(data):
                mem[a + i] = by
            lo, hi = a, a + ln - 1
            mn = lo if mn is None else min(mn, lo)
            mx = hi if mx is None else max(mx, hi)
        elif rectype == 0x04:
            ext = ((data[0] << 8) | data[1]) << 16
        elif rectype == 0x02:
            ext = ((data[0] << 8) | data[1]) << 4
        elif rectype == 0x01:
            break
    if mn is None:
        raise SystemExit(f"[otapkg] {path}: HEX bez dát")
    flat = bytearray(mx - mn + 1)
    for a, by in mem.items():
        flat[a - mn] = by
    return flat


def build_number(flat: bytes) -> int:
    idx = flat.find(MAGIC)
    if idx < 0:
        raise SystemExit("[otapkg] magic FKFWID01 nenájdený v image — nie OTA build?")
    return struct.unpack_from("<I", flat, idx + OFF_BUILD)[0]


def bin_build_num(path: Path) -> int:
    """Build# z FwIdTrailer v .bin súbore (app image)."""
    return build_number(Path(path).read_bytes())


def archive_from_hex(hex_path: Path, device: str) -> tuple[int, Path]:
    flat = read_ihex(hex_path)
    n = build_number(flat)
    BUILDS_DIR.mkdir(exist_ok=True)
    out = BUILDS_DIR / f"{device}.fw_{n}.bin"
    out.write_bytes(flat)
    print(f"[otapkg] archív: {out.name}  ({len(flat)} B, build #{n})")
    return n, out


def latest_two(device: str) -> tuple[Path, Path]:
    """(predposledný, posledný) z builds/ pre dané zariadenie podľa build čísla."""
    if not BUILDS_DIR.is_dir():
        raise SystemExit("[otapkg] builds/ neexistuje — najprv buildni s --from-hex")
    bins = []
    for p in BUILDS_DIR.glob(f"{device}.fw_*.bin"):
        try:
            bins.append((int(p.stem.rsplit("fw_", 1)[1]), p))
        except (IndexError, ValueError):
            pass
    bins.sort()
    if len(bins) < 2:
        raise SystemExit(f"[otapkg] v archíve je pre '{device}' len {len(bins)} build(ov) — treba aspoň 2")
    return bins[-2][1], bins[-1][1]


def gen_pair(old_bin: Path, new_bin: Path, out_dir: Path, args):
    privkey = args.privkey
    if privkey is None:
        cand = HERE / "test_key.der"
        privkey = str(cand) if cand.exists() else None
    common = dict(channel_name=args.channel_name, channel_idx=args.channel_idx,
                  freq=args.freq, bw=args.bw, sf=args.sf, cr=args.cr,
                  scope=args.scope, path=args.path, privkey=privkey, keyid=args.keyid)

    old_n, new_n = bin_build_num(old_bin), bin_build_num(new_bin)
    dev = args.device
    # Oba balíky nesú ROVNAKÝ číselný prefix <old>-<new> (nech sa v adresári radia
    # k sebe); reverse sa líši len vloženým ".rev".
    fwd_name = f"{old_n}-{new_n}.{dev}.otapkg.json"
    rev_name = f"{old_n}-{new_n}.rev.{dev}.otapkg.json"

    fwd = X.build_pkg(old_bin, new_bin, out_dir / "ota_patch_fwd.bin", **common)
    (out_dir / fwd_name).write_text(json.dumps(fwd, indent=2))
    rev = X.build_pkg(new_bin, old_bin, out_dir / "ota_patch_rev.bin", **common)
    (out_dir / rev_name).write_text(json.dumps(rev, indent=2))

    sgn = "signed" if privkey else "UNSIGNED"
    print(f"[otapkg] {fwd_name}  ({old_bin.name} → {new_bin.name})  "
          f"patch={fwd['fw']['patch_len']}B old={fwd['fw']['old_sha256'][:8]} "
          f"new={fwd['fw']['new_sha256'][:8]} [{sgn}]")
    print(f"[otapkg] {rev_name}  (rollback {new_n} → {old_n})  "
          f"patch={rev['fw']['patch_len']}B [{sgn}]")


def main():
    ap = argparse.ArgumentParser(description="Generuj fw.otapkg.json + fw_reverse.otapkg.json")
    ap.add_argument('--from-hex', nargs='?', const=str(DEFAULT_HEX),
                    help="extrahuj+archivuj app image z HEX (default .pio build), potom gen")
    ap.add_argument('--auto', action='store_true', help="2 najnovšie buildy z builds/")
    ap.add_argument('--old', help="explicitný old .bin")
    ap.add_argument('--new', help="explicitný new .bin")
    ap.add_argument('--out-dir', default=str(FOTAPKG_DIR),
                    help="kam zapísať json (default test_nrf-ota/fotapkg_json/)")
    ap.add_argument('--device', default='promicro', help="názov zariadenia v archíve+json (default promicro)")
    ap.add_argument('--channel-name', default=OTA_CHANNEL_NAME)
    ap.add_argument('--channel-idx', type=int, default=1)
    ap.add_argument('--scope', choices=['zerohop', 'flood', 'direct'], default='zerohop')
    ap.add_argument('--path', default='')
    ap.add_argument('--freq', type=float, default=869.618); ap.add_argument('--bw', type=float, default=62.5)
    ap.add_argument('--sf', type=int, default=8); ap.add_argument('--cr', type=int, default=5)
    ap.add_argument('--privkey', help="Ed25519 priv (default test_key.der ak je); '' = nepodpisuj")
    ap.add_argument('--keyid', type=int, default=1)
    args = ap.parse_args()
    if args.privkey == '':
        args.privkey = None
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.old and args.new:
        old_bin, new_bin = Path(args.old), Path(args.new)
    else:
        if args.from_hex is not None:
            archive_from_hex(Path(args.from_hex), args.device)
        old_bin, new_bin = latest_two(args.device)

    gen_pair(old_bin, new_bin, out_dir, args)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Export a .otapkg.json for mc_fotanrf_flutterapp (phase A).
Reuses ota_sender.make_patch / build_meta_payload / build_sig_payload."""
import argparse, base64, json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ota_sender as S
from ota_sender import (make_patch, build_meta_payload, build_sig_payload,
                        load_ed25519_privkey, OTA_CHANNEL_NAME, OTA_CHUNK_DATA)

def main():
    ap = argparse.ArgumentParser(description="Export .otapkg.json for the Flutter OTA app")
    ap.add_argument('--old', required=True); ap.add_argument('--new', required=True)
    ap.add_argument('--patch', default='ota_patch.bin')
    ap.add_argument('--out', required=True)
    ap.add_argument('--channel-name', default=OTA_CHANNEL_NAME)
    ap.add_argument('--channel-idx', type=int, default=1)
    ap.add_argument('--scope', choices=['zerohop','flood','direct'], default='zerohop')
    ap.add_argument('--path', default='')
    ap.add_argument('--freq', type=float, default=869.618); ap.add_argument('--bw', type=float, default=62.5)
    ap.add_argument('--sf', type=int, default=8); ap.add_argument('--cr', type=int, default=5)
    ap.add_argument('--privkey'); ap.add_argument('--keyid', type=int, default=1)
    args = ap.parse_args()

    patch, patch_sha256, new_sha256, old_sha256, old_fw_size = \
        make_patch(Path(args.old), Path(args.new), Path(args.patch))
    pkg = {
        "format": "mc-fotanrf-otapkg/1",
        "created": "1970-01-01T00:00:00Z",
        "channel": {"name": args.channel_name, "idx": args.channel_idx},
        "radio": {"freq": args.freq, "bw": args.bw, "sf": args.sf, "cr": args.cr},
        "scope": args.scope, "path": args.path,
        "fw": {"old_sha256": old_sha256.hex(), "new_sha256": new_sha256.hex(),
               "old_fw_size": old_fw_size, "patch_sha256": patch_sha256.hex(),
               "patch_len": len(patch)},
        "patch_b64": base64.b64encode(patch).decode(),
    }
    if args.privkey:
        privkey = load_ed25519_privkey(Path(args.privkey))
        total = (len(patch) + OTA_CHUNK_DATA - 1) // OTA_CHUNK_DATA
        meta = build_meta_payload(total, len(patch), patch_sha256, new_sha256, old_sha256)
        sig = build_sig_payload(meta, privkey, args.keyid)
        pkg["signed"] = {"key_id": args.keyid,
                         "meta_b64": base64.b64encode(meta).decode(),
                         "sig_b64": base64.b64encode(sig).decode()}
    Path(args.out).write_text(json.dumps(pkg, indent=2))
    print(f"[export] {args.out}: patch={len(patch)}B signed={'signed' in pkg}")

if __name__ == '__main__':
    main()

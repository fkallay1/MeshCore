#!/usr/bin/env python3
"""
ota_sender.py  — MeshCore OTA over LoRa / Serial sender

Závislosti:
    pip install pyserial pycryptodome

Použitie — cez LoRa gateway:
    python ota_sender.py --old old.bin --new new.bin --port COM5 --psk 9cd8... --mode meshcore
    python ota_sender.py --old old.bin --new new.bin --port COM5 --mode direct

Použitie — priamo cez serial na sniffer (bez LoRa!):
    python ota_sender.py --old old.bin --new new.bin --port COM5 --mode serial-direct

  serial-direct:  rámce [0xAB][0xCD][len][data] priamo na sniffer USB serial
                  sniffer ich detekuje v loop() cez serial_inject_try()
                  USB konzola stále funguje (text príkazy + výpisy súčasne)

Generovanie patchu:
  1. hdiffpatch (odporúčané, HPatchLite na zariadení):
       pip install hdiffpatch  (alebo stiahnuť hdiffz binary)
  2. bsdiff4 (záložné, pre budúcu kompatibilitu):
       pip install bsdiff4

Serial rámec od zariadenia (gateway reply): [0xCC][0xDD][len 2B LE][data]
"""

import argparse
import hashlib
import hmac as hmaclib
import random
import serial
import serial.threaded
import struct
import sys
import time
import threading
from pathlib import Path

# Windows konzola býva cp1250 — prepni na utf-8 (znaky →, š, č v print)
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

# ─────────────────────────────────────────────────────────────────────
# Protokol — synchronizované s ota_proto.h
# ─────────────────────────────────────────────────────────────────────
OTA_PKT_BEGIN   = 0x10
OTA_PKT_CHUNK   = 0x11
OTA_PKT_APPLY   = 0x12
OTA_PKT_STATUS  = 0x20
OTA_PKT_NACK    = 0x21

OTA_CHUNK_DATA  = 150
DIRECT_MAGIC    = b'\x4F\x54'   # 'OT'

OTA_ST_VERIFIED = 0x04
OTA_ST_ERROR    = 0x80

FRAME_SYNC_TX   = b'\xAB\xCD'  # PC → gateway / sniffer
FRAME_SYNC_RX   = b'\xCC\xDD'  # gateway → PC

# ─────────────────────────────────────────────────────────────────────
# CRC16/CCITT-FALSE
# ─────────────────────────────────────────────────────────────────────
def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc

# ─────────────────────────────────────────────────────────────────────
# Kryptografia (MeshCore GRP_DATA)
# ─────────────────────────────────────────────────────────────────────
def meshcore_encrypt(psk: bytes, plaintext: bytes) -> bytes:
    try:
        from Crypto.Cipher import AES
    except ImportError:
        sys.exit("[CHYBA] --mode meshcore potrebuje pycryptodome:\n"
                 "        <penv>/python.exe -m pip install pycryptodome")
    psk32 = psk.ljust(32, b'\x00')
    pad = (16 - len(plaintext) % 16) % 16
    padded = plaintext + b'\x00' * pad
    cipher = AES.new(psk32[:16], AES.MODE_ECB)
    ciphertext = cipher.encrypt(padded)
    mac = hmaclib.new(psk32, ciphertext, hashlib.sha256).digest()[:2]
    return mac + ciphertext

def meshcore_grp_data_packet(psk: bytes, ota_payload: bytes) -> bytes:
    ch_hash  = hashlib.sha256(psk).digest()[0]
    ts       = int(time.time()) & 0xFFFFFFFF
    plain    = struct.pack('<I', ts) + ota_payload
    enc      = meshcore_encrypt(psk, plain)
    header   = (6 << 2) | 1   # FLOOD + GRP_DATA = 0x19
    path_enc = 0x00
    return bytes([header, path_enc, ch_hash]) + enc

def direct_ota_packet(ota_payload: bytes) -> bytes:
    return DIRECT_MAGIC + ota_payload

# ─────────────────────────────────────────────────────────────────────
# Generovanie patchu — hdiffpatch alebo bsdiff4
# ─────────────────────────────────────────────────────────────────────
def make_patch(old_path: Path, new_path: Path, patch_path: Path):
    """Vracia (patch_bytes, patch_sha256, new_sha256, old_sha256, old_fw_size)."""
    old_data = old_path.read_bytes()
    new_data = new_path.read_bytes()

    new_sha256 = hashlib.sha256(new_data).digest()
    old_sha256 = hashlib.sha256(old_data).digest()
    print(f"[patch] OLD fw: {len(old_data)}B  sha256={old_sha256.hex()}")
    print(f"[patch] NEW fw: {len(new_data)}B  sha256={new_sha256.hex()}")

    # hdiffi -inplaceB: HPatchLite inplace formát — POVINNÉ pre in-place flasher na nRF52840.
    # Formát inplaceB zaručuje, že pri zápise na adresu X ešte nie sú potrebné staré dáta
    # z predchádzajúcich adries (extraSafeSize v hlavičke patch-u).
    # POZOR: hdiffi generuje iný formát ako hdiffz — nie sú vzájomne kompatibilné!
    #
    # Získaj hdiffi:
    #   git clone https://github.com/sisong/HPatchLite.git
    #   cd HPatchLite && g++ hdiffi.cpp HDiffPatch/libHDiffPatch/HDiff/*.cpp -O2 -o hdiffi
    # alebo stiahni binárku z: https://github.com/sisong/HPatchLite/releases
    import subprocess, os, tempfile, shutil, zlib, struct
    # Hľadaj hdiffi: vedľa skriptu (tools/), potom PATH
    _script_dir = Path(__file__).parent
    _hdiffi = None
    for candidate in [_script_dir / 'hdiffi.exe', _script_dir / 'hdiffi',
                      Path('hdiffi.exe'), Path('hdiffi')]:
        if candidate.exists():
            _hdiffi = str(candidate)
            break
    if _hdiffi is None:
        _hdiffi = shutil.which('hdiffi') or shutil.which('hdiffi.exe')
    if _hdiffi is None:
        print('[CHYBA] hdiffi.exe nenajdeny — skopiruj ho do tools/')
        sys.exit(1)
    print(f'[patch] hdiffi: {_hdiffi}')

    tmp = tempfile.mktemp(suffix='.hpatch')
    # Novšie hdiffi: -inplaceB; staršie (v1.x): -inplace-N (N=extraSafeSize)
    for flags in [['-inplaceB'], ['-inplace-4096'], ['-inplace']]:
        try:
            ret = subprocess.run(
                [_hdiffi, *flags, str(old_path), str(new_path), tmp],
                capture_output=True
            )
            if ret.returncode == 0:
                raw_patch = Path(tmp).read_bytes()
                if os.path.exists(tmp): os.unlink(tmp)
                print(f"[patch] hdiffi {' '.join(flags)}: raw={len(raw_patch)}B")

                # Komprimuj: raw DEFLATE, 512B okno (wbits=-9)
                # Flasher dekompresoruje pomocou puff.c (tiez wbits=9)
                comp_data = zlib.compress(raw_patch, level=9, wbits=-9)
                ratio = len(comp_data) * 100 // len(raw_patch) if raw_patch else 100
                print(f"[patch] puff kompresia: {len(raw_patch)}B -> {len(comp_data)}B ({ratio}%)")

                # Staged format: [magic 4B 'ZLIB'][uncomp_size 4B LE][new_fw_size 4B LE][deflate...]
                new_fw_size = len(new_data)
                staged = (b'ZLIB'
                          + struct.pack('<II', len(raw_patch), new_fw_size)
                          + comp_data)
                staged_sha = hashlib.sha256(staged).digest()
                print(f"[patch] staged (LittleFS): {len(staged)}B  (header=12B)")
                print(f"[patch] PATCH (staged) sha256={staged_sha.hex()}")

                patch_path.write_bytes(staged)
                return staged, staged_sha, new_sha256, old_sha256, len(old_data)
        except FileNotFoundError:
            print(f'[CHYBA] hdiffi sa nedal spustit: {_hdiffi}')
            sys.exit(1)

    print(f"[CHYBA] hdiffi zlyhalo: {ret.stderr.decode(errors='replace').strip()}")
    sys.exit(1)

# ─────────────────────────────────────────────────────────────────────
# Serial framing
# ─────────────────────────────────────────────────────────────────────
def send_frame(ser: serial.Serial, data: bytes):
    frame = FRAME_SYNC_TX + struct.pack('<H', len(data)) + data
    try:
        ser.write(frame)
    except serial.SerialException as e:
        raise serial.SerialException(
            f"[CHYBA] Serial port stratený — zariadenie sa rebootlo?\n  ({e})"
        ) from e

def read_response_frame(buf: bytearray, magic: bytes = FRAME_SYNC_RX):
    """Hľadaj rámec v buffri, vráti (payload, zvyšok_bufra) alebo (None, buf)."""
    idx = buf.find(magic)
    if idx < 0:
        return None, buf
    if len(buf) < idx + 4:
        return None, buf
    length = struct.unpack_from('<H', buf, idx + 2)[0]
    if len(buf) < idx + 4 + length:
        return None, buf
    payload = bytes(buf[idx + 4: idx + 4 + length])
    return payload, bytearray(buf[idx + 4 + length:])

# ─────────────────────────────────────────────────────────────────────
# Vlákno pre čítanie sériového výstupu zariadenia
# ─────────────────────────────────────────────────────────────────────
class SerialReader(threading.Thread):
    """Číta riadky zo sériového portu a vypisuje ich. Thread-safe queue pre odpovede."""
    def __init__(self, ser: serial.Serial, response_queue):
        super().__init__(daemon=True)
        self.ser = ser
        self.q = response_queue
        self._stop = threading.Event()
        self._buf = bytearray()

    def stop(self): self._stop.set()

    def run(self):
        while not self._stop.is_set():
            try:
                chunk = self.ser.read(256)
                if chunk:
                    self._buf.extend(chunk)
                    # Hľadaj binárne rámce (odpovede od gateway)
                    while True:
                        payload, self._buf = read_response_frame(self._buf)
                        if payload is None:
                            break
                        self.q.put(payload)
                    # Vypiš čitateľné znaky
                    while b'\n' in self._buf:
                        line, self._buf = self._buf.split(b'\n', 1)
                        try:
                            print('  [sniffer]', line.decode('utf-8', errors='replace').rstrip())
                        except Exception:
                            pass
            except serial.SerialException:
                break

# ─────────────────────────────────────────────────────────────────────
# OTA paket buiders
# ─────────────────────────────────────────────────────────────────────
def build_ota_begin(patch: bytes, patch_sha256: bytes, new_sha256: bytes,
                    old_sha256: bytes, old_fw_size: int) -> bytes:
    total = (len(patch) + OTA_CHUNK_DATA - 1) // OTA_CHUNK_DATA
    # OtaBeginPkt: [type 1B][total_chunks 2B][patch_size 4B][patch_sha256 32B]
    #              [new_sha256 32B][old_fw_size 4B][old_sha256 32B]
    return (bytes([OTA_PKT_BEGIN])
            + struct.pack('<HI', total, len(patch))
            + patch_sha256
            + new_sha256
            + struct.pack('<I', old_fw_size)
            + old_sha256)

def build_ota_chunk(idx: int, data: bytes) -> bytes:
    return bytes([OTA_PKT_CHUNK]) + struct.pack('<HH', idx, crc16(data)) + data

def build_ota_apply(patch_sha256: bytes) -> bytes:
    return bytes([OTA_PKT_APPLY]) + patch_sha256

# ─────────────────────────────────────────────────────────────────────
# Hlavná OTA session
# ─────────────────────────────────────────────────────────────────────
def send_ota(ser: serial.Serial,
             patch: bytes, patch_sha256: bytes, new_sha256: bytes,
             old_sha256: bytes, old_fw_size: int,
             psk: bytes | None, mode: str,
             chunk_delay: float, nack_retries: int, do_reboot: bool = False,
             drop_prob: float = 0.0):
    chunks = [patch[i:i+OTA_CHUNK_DATA] for i in range(0, len(patch), OTA_CHUNK_DATA)]
    total  = len(chunks)
    print(f"[OTA] {len(patch)}B → {total} chunkov")
    print(f"[OTA]   PATCH sha256 = {patch_sha256.hex()}")
    print(f"[OTA]   OLD   sha256 = {old_sha256.hex()}  ({old_fw_size}B)")
    print(f"[OTA]   NEW   sha256 = {new_sha256.hex()}")
    air_min = total * 1.2 / 60
    print(f"[OTA] ~{air_min:.1f} min @ SF8/BW62.5  chunk_delay={chunk_delay}s")

    import queue
    resp_queue: queue.Queue = queue.Queue()
    reader = SerialReader(ser, resp_queue)
    reader.start()

    def send_pkt(payload: bytes):
        if mode == 'meshcore':
            lora_pkt = meshcore_grp_data_packet(psk, payload)
        elif mode == 'direct':
            lora_pkt = direct_ota_packet(payload)
        else:  # serial-direct: inject priamo ako direct OTA cez serial rámec
            lora_pkt = direct_ota_packet(payload)
        send_frame(ser, lora_pkt)

    # --- BEGIN ---
    print("[OTA] Posielam BEGIN...")
    try:
        send_pkt(build_ota_begin(patch, patch_sha256, new_sha256, old_sha256, old_fw_size))
    except serial.SerialException as e:
        print(e)
        reader.stop()
        return False
    time.sleep(1.2)

    # --- CHUNKS ---
    to_send = list(range(total))
    for attempt in range(nack_retries + 1):
        if not to_send:
            break
        print(f"[OTA] Posielam {len(to_send)} chunkov (pokus {attempt+1}/{nack_retries+1})...")
        serial_lost = False
        dropped = 0
        for pos, idx in enumerate(to_send):
            # Simulácia straty paketu (test kumulácie naprieč cyklami)
            if drop_prob > 0.0 and random.random() < drop_prob:
                dropped += 1
                continue
            try:
                send_pkt(build_ota_chunk(idx, chunks[idx]))
            except serial.SerialException as e:
                print(f"\n{e}")
                serial_lost = True
                break
            if (pos + 1) % 10 == 0 or pos == len(to_send) - 1:
                print(f"  → {pos+1}/{len(to_send)} (idx={idx})", end='\r', flush=True)
            time.sleep(chunk_delay)
        print()
        if dropped:
            print(f"[OTA] (simulácia straty: vynechaných {dropped} chunkov)")
        if serial_lost:
            reader.stop()
            return False

        # Čakaj na odpoveď od zariadenia
        print("[OTA] Čakám na STATUS/NACK (5s)...")
        try:
            resp = resp_queue.get(timeout=5.0)
        except Exception:
            print("  (žiadna odpoveď — predpokladám OK)")
            to_send = []
            break

        if resp[0] == OTA_PKT_NACK and len(resp) >= 2:
            count = resp[1]
            missing = list(struct.unpack_from(f'<{count}H', resp, 2))
            print(f"[OTA] NACK: {count} chýba: {missing[:10]}...")
            to_send = missing
        elif resp[0] == OTA_PKT_STATUS and len(resp) >= 6:
            recv_c, tot_c, status = struct.unpack_from('<HHB', resp, 1)
            print(f"[OTA] STATUS: {recv_c}/{tot_c}  st=0x{status:02X}")
            if status & OTA_ST_VERIFIED:
                to_send = []
                break
            elif status & OTA_ST_ERROR:
                print("[OTA] CHYBA na zariadení!")
                reader.stop()
                return False
        else:
            to_send = []

    if to_send:
        print(f"[OTA] Stále chýba {len(to_send)} chunkov!")
        reader.stop()
        return False

    if not do_reboot:
        print("[OTA] Vsetky chunky odoslane. Ovladaj manualme cez konzolu:")
        print("  f  = LittleFS zoznam suborov")
        print("  o  = OTA stav session")
        print("  Q  = test patch (dry-run SHA256, bez zapisu)")
        print("  e  = execute: flash + reboot")
        reader.stop()
        return True

    # --- APPLY (iba ak --reboot) ---
    print("[OTA] Posielam APPLY (flash + reboot)...")
    try:
        send_pkt(build_ota_apply(patch_sha256))
    except serial.SerialException as e:
        print(e)
        reader.stop()
        return False
    time.sleep(2.0)
    print("[OTA] Hotovo — zariadenie sa rebootu je.")
    reader.stop()
    return True

# ─────────────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description='MeshCore OTA sender')
    ap.add_argument('--old',    required=True, help='Starý firmware .bin')
    ap.add_argument('--new',    required=True, help='Nový firmware .bin')
    ap.add_argument('--port',   required=True, help='Serial port (COM5 / /dev/ttyUSB0)')
    ap.add_argument('--baud',   type=int, default=115200)
    ap.add_argument('--psk',    help='Channel PSK hex (16B=32 znakov, pre mode=meshcore)')
    ap.add_argument('--mode',   choices=['meshcore', 'direct', 'serial-direct'],
                                default='serial-direct',
                                help=('meshcore=cez repeatre (šifrované) | '
                                      'direct=priamy LoRa gateway | '
                                      'serial-direct=USB serial bez LoRa (TEST)'))
    ap.add_argument('--patch',  default='ota_patch.bin', help='Medzisúbor patchu')
    ap.add_argument('--delay',  type=float, default=0.3,
                                help='Pauza medzi chunkmi [s] (serial-direct: môže byť 0.05)')
    ap.add_argument('--nack-retries', type=int, default=3)
    ap.add_argument('--reboot', action='store_true', default=False,
                                help='Po odoslaní chunkov pošli APPLY (flash+reboot). Default: len chunky, bez resetu.')
    ap.add_argument('--cycles', type=int, default=1,
                                help='Koľkokrát opakovať celý broadcast (BEGIN+chunky+APPLY). '
                                     'Fire-and-forget model: prijímač si kumuluje chunky naprieč cyklami.')
    ap.add_argument('--drop',   type=float, default=0.0,
                                help='Pravdepodobnosť [0..1] zahodenia chunku (simulácia LoRa straty, test kumulácie).')
    ap.add_argument('--cycle-delay', type=float, default=2.0,
                                help='Pauza medzi cyklami [s].')
    args = ap.parse_args()
    if not (0.0 <= args.drop < 1.0):
        print('[CHYBA] --drop musí byť v [0..1)'); sys.exit(1)

    # Validácia
    psk = None
    if args.mode == 'meshcore':
        if not args.psk:
            print('[CHYBA] --psk je povinný pre --mode meshcore')
            sys.exit(1)
        psk = bytes.fromhex(args.psk)
        if len(psk) not in (16, 32):
            print('[CHYBA] PSK musí byť 16 alebo 32 bajtov')
            sys.exit(1)
        print(f'[init] Mode=MeshCore  ch_hash=0x{hashlib.sha256(psk).digest()[0]:02X}')
    elif args.mode == 'direct':
        print('[init] Mode=Direct (LoRa bez šifrovania, cez gateway)')
    else:
        print('[init] Mode=SerialDirect (USB serial, inject do sniffera)')
        if args.delay > 0.1:
            print(f'[init] TIP: pre serial-direct môžeš skúsiť --delay 0.05')

    # Generuj patch
    patch, patch_sha256, new_sha256, old_sha256, old_fw_size = \
        make_patch(Path(args.old), Path(args.new), Path(args.patch))
    total = (len(patch) + OTA_CHUNK_DATA - 1) // OTA_CHUNK_DATA

    print(f'[init] {total} chunkov × {OTA_CHUNK_DATA}B = {len(patch)}B patch')

    # Otvor serial
    print(f'[serial] {args.port} @ {args.baud}')
    if args.cycles > 1:
        print(f'[init] Broadcast {args.cycles}× '
              f'(drop={args.drop:.0%}) — fire-and-forget, prijímač kumuluje chunky')
    ok = False
    with serial.Serial(args.port, args.baud, timeout=0.05) as ser:
        # DTR pri otvorení resetne Adafruit nRF52 bridge → čerstvý boot rádia.
        # Daj mu čas nabehnúť (radio.begin ~2s), inak sa stratí BEGIN.
        time.sleep(2.5)
        ser.reset_input_buffer()
        for cyc in range(args.cycles):
            if args.cycles > 1:
                print(f'\n========== CYKLUS {cyc+1}/{args.cycles} ==========')
            ok = send_ota(ser, patch, patch_sha256, new_sha256, old_sha256, old_fw_size,
                          psk, args.mode, args.delay, args.nack_retries, args.reboot,
                          drop_prob=args.drop)
            if not ok:
                print('[OTA] cyklus zlyhal (serial?) — končím')
                break
            if cyc < args.cycles - 1:
                time.sleep(args.cycle_delay)

    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()

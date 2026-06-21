#!/usr/bin/env python3
"""
ota_test_lora_repeater.py — end-to-end OTA test MeshCore repeatera cez LoRa.

Topológia (rovnaká ako FK_lora ota_test_lora.py):
    PC ──USB── BRIDGE (XIAO, FK_lora gateway_fw) ──LoRa── REPEATER (ProMicro, MeshCore) ──USB── PC
              (--bridge-port COM3)                        (--target-port COM5, monitoring + CLI)

OTA ide cez MeshCore GRP_DATA (šifrované, PSK). Sender = test_nrf-ota/ota_sender.py
v móde 'meshcore'. CZ preset (869.525/SF7) — mimo SK siete; bridge aj repeater
sa buildujú na CZ, aby sa počuli a nerušili produkčnú SK sieť.

Dvojfázový (patch potrebuje OLD != NEW; build# sa zvyšuje sám → diff vždy existuje):

  1) baseline
       - (voliteľne) build+upload BRIDGE (FK_lora Xiao_bridge, CZ) → COM3
       - build+upload REPEATER (OLD) → COM5
       - uloží OLD app obraz; na repeateri beží OLD
  2) run
       - build REPEATER → NEW, vyrobí patch OLD→NEW
       - broadcast cez BRIDGE (ota_sender --mode meshcore)
       - počká na VERIFIED (CLI 'ota status' cez COM5), spustí 'ota flash'
       - po reboote overí build# == NEW + [FLASHER-DBG] marker

Spúšťaj cez PlatformIO penv python (má pyserial + platformio):
   & "$HOME\\.platformio\\penv\\Scripts\\python.exe" \
       test_nrf-ota/ota_test_lora_repeater.py run --bridge-port COM3 --target-port COM5
"""
import argparse
import os
import re
import sys
import time
import zipfile
import subprocess
from pathlib import Path

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

SCRIPT_DIR   = Path(__file__).resolve().parent          # test_nrf-ota/
MESHCORE_DIR = SCRIPT_DIR.parent                         # MeshCore/
PY           = sys.executable
OTA_SENDER   = SCRIPT_DIR / "ota_sender.py"
BUILD_NUM    = SCRIPT_DIR / "build_number.txt"
OLD_BIN      = SCRIPT_DIR / "lora_old.bin"
NEW_BIN      = SCRIPT_DIR / "lora_new.bin"

DEFAULT_BRIDGE_PORT = "COM3"
DEFAULT_TARGET_PORT = "COM5"
DEFAULT_TARGET_ENV  = "ProMicro_repeater_ota"
DEFAULT_BRIDGE_ENV  = "Xiao_bridge"
DEFAULT_FK_LORA     = Path("x:/FkData/10_Dev/FkProj/VSC/FK_lora-sniffer")
OTA_PSK_STR         = "meshcore-ota-key"   # MUSÍ == OTA_CHANNEL_PSK v OTA env
# OTA env (ProMicro_repeater_ota) NEMÁ -D OTA_ALLOW_UNSIGNED → HEADER MUSÍ byť
# podpísaný Ed25519, inak repeater odmietne session (OTA_ERR_SIGNATURE 0x06).
# Default = test keypair (pubkey == s_authors[key_id=1] v OtaReceiver_signkey.cpp).
DEFAULT_PRIVKEY     = SCRIPT_DIR / "test_key.der"

# OTA status flags (z OtaState.h)
OTA_ST_VERIFIED = 0x04
OTA_ST_ERROR    = 0x80

# ── farby ──
def c(code, s): return f"\033[{code}m{s}\033[0m"
def green(s):  return c("32", s)
def red(s):    return c("31", s)
def yellow(s): return c("33", s)
def cyan(s):   return c("36", s)

def run(cmd, desc, cwd=MESHCORE_DIR, check=True, env_extra=None):
    print(cyan(f"\n>>> {desc}"))
    print(f"    {' '.join(str(x) for x in cmd)}")
    sub_env = dict(os.environ, PYTHONIOENCODING="utf-8")
    if env_extra:
        sub_env.update(env_extra)
    r = subprocess.run(cmd, cwd=str(cwd), env=sub_env)
    if check and r.returncode != 0:
        sys.exit(red(f"[FAIL] {desc} skončilo s kódom {r.returncode}"))
    return r.returncode

def read_build_number():
    try:
        return int(BUILD_NUM.read_text().strip())
    except Exception:
        return None

def fnv1a(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h

def zip_path(env):
    return MESHCORE_DIR / ".pio" / "build" / env / "firmware.zip"

def extract_app_image(env, dst: Path):
    zp = zip_path(env)
    if not zp.exists():
        sys.exit(red(f"[FAIL] {zp} neexistuje — build nezbehol?"))
    with zipfile.ZipFile(zp) as z:
        data = z.read("firmware.bin")
    dst.write_bytes(data)
    return len(data)

# ── serial ──
def port_present(port):
    from serial.tools import list_ports
    return any(p.device == port for p in list_ports.comports())

def wait_port_back(port, timeout=40):
    print(cyan(f">>> Čakám na návrat {port} (max {timeout}s)..."))
    deadline = time.time() + timeout
    while time.time() < deadline:
        if port_present(port):
            print(green(f"    {port} späť"))
            time.sleep(1.0)
            return True
        time.sleep(0.3)
    print(red(f"    {port} sa nevrátil do {timeout}s!"))
    return False

def _open_serial(port):
    import serial
    s = serial.Serial(port, 115200, timeout=0.2)
    s.dtr = True
    return s

def capture_serial(port, seconds=14, send_cmd=None, reboot_window=45):
    """Číta riadky z portu; prežije reboot (port zmizne → počká → znova otvorí)."""
    import serial
    try:
        ser = _open_serial(port)
    except Exception as e:
        print(red(f"    Nepodarilo sa otvoriť {port}: {e}"))
        return []
    if send_cmd:
        time.sleep(0.4)
        ser.reset_input_buffer()
        ser.write(send_cmd.encode())
        ser.flush()
        print(cyan(f">>> {port} <- {send_cmd!r}"))
    lines, buf = [], b""
    t_end = time.time() + seconds
    while time.time() < t_end:
        try:
            chunk = ser.read(256)
        except (serial.SerialException, OSError):
            print(yellow("    [port zmizol — flash/reboot]"))
            try: ser.close()
            except Exception: pass
            ser = None
            deadline = time.time() + reboot_window
            while time.time() < deadline:
                if port_present(port):
                    time.sleep(1.0)
                    try: ser = _open_serial(port); break
                    except Exception: time.sleep(0.3)
                else:
                    time.sleep(0.3)
            if ser is None:
                print(red(f"    Port {port} sa nevrátil do {reboot_window}s!"))
                break
            print(green("    [port späť — čítam nový boot]"))
            buf = b""
            t_end = time.time() + seconds
            continue
        if chunk:
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                s = raw.decode("utf-8", "replace").rstrip("\r")
                print("    [dev]", s)
                lines.append(s)
    if ser:
        try: ser.close()
        except Exception: pass
    return lines

def find_build(lines):
    found = None
    for l in lines:
        m = re.search(r"build #(\d+)", l)
        if m:
            found = int(m.group(1))
    return found

def find_ota_status(lines):
    """Vráti (recv, total, status) z posledného 'OTA n/m st=0x..' riadku, alebo None."""
    res = None
    for l in lines:
        m = re.search(r"OTA\s+(\d+)/(\d+)\s+st=0x([0-9A-Fa-f]+)", l)
        if m:
            res = (int(m.group(1)), int(m.group(2)), int(m.group(3), 16))
    return res

def broadcast_until_verified(args, sender):
    """Trpezlivý príjem: opakuj broadcast + poll 'ota status' kým nie je VERIFIED
    alebo nevyprší --verify-wait. Fire-and-forget príjem niekedy stráca pakety na
    začiatku (zaseknuté/nečinné RX okno), takže jeden-dva cykly nemusia stačiť —
    kumulujeme chunky naprieč kolami a sledujeme rast recv/total, nie len VERIFIED.
    Vracia True ak VERIFIED."""
    deadline = time.time() + args.verify_wait
    rnd, last, best_recv = 0, None, -1
    while time.time() < deadline:
        rnd += 1
        remaining = int(deadline - time.time())
        print(cyan(f"\n────── broadcast kolo {rnd} (drop={args.drop:.0%}, zostáva ~{remaining}s) ──────"))
        # Čerstvé rádio KAŽDÉ kolo: SX1262 RX po ~4 prijatých rámcoch ohluchne
        # ("stuck receiver", viď readme §8.3) a v rámci jedného bootu sa už nespamätá.
        # recv_count je per-boot (bitmap sa pri <8 chunkoch neukladá) → VERIFIED si
        # žiada všetky chunky v JEDNOM čerstvom okne. Reboot odsekne hluchotu a dá
        # nové čisté RX okno. HEADER/meta prežíva reboot, takže pri 'hend' (chunky
        # prvé) sa ~4-rámcový budget minie na chunky, nie na už-známy header.
        capture_serial(args.target_port, seconds=args.reboot_settle, send_cmd="reboot\r")
        run(sender, f"ota_sender broadcast #{rnd} cez {args.bridge_port}", check=False)
        # po každom broadcaste niekoľko trpezlivých 'ota status' pollov
        for _ in range(args.poll_tries):
            lines = capture_serial(args.target_port, seconds=args.poll_secs, send_cmd="ota status\r")
            st = find_ota_status(lines)
            if st:
                last = st
                print(cyan(f"   OTA stav: {st[0]}/{st[1]} st=0x{st[2]:02X}"))
                if st[0] > best_recv:           # vidíme rast → príjem žije, buď trpezlivý
                    best_recv = st[0]
                if st[2] & OTA_ST_ERROR:
                    print(yellow("   [OTA ERROR flag] — pokračujem (ďalší broadcast môže doplniť)"))
                if st[2] & OTA_ST_VERIFIED:
                    print(green("   VERIFIED — všetky chunky prijaté a SHA256 OK"))
                    return True
            if time.time() >= deadline:
                break
        time.sleep(args.cycle_delay)
    print(red(f"   VERIFIED nedosiahnuté za {args.verify_wait}s "
              f"(najlepší stav: {best_recv}/{last[1] if last else '?'} chunkov)"))
    return False

# ── fázy ──
def phase_baseline(args):
    print(green("\n══════ baseline — bridge (CZ) + OLD repeater (CZ) ══════"))
    cz_env = {"PLATFORMIO_BUILD_FLAGS": "-DLORA_PRESET_CZ"}

    if not args.skip_bridge:
        run([PY, "-m", "platformio", "run", "-d", str(args.fk_lora),
             "-e", args.bridge_env, "-t", "upload", "--upload-port", args.bridge_port],
            f"Build+Upload BRIDGE ({args.bridge_env}, CZ) → {args.bridge_port}",
            cwd=args.fk_lora, env_extra=cz_env)
    else:
        print(yellow("   (--skip-bridge — predpokladám že XIAO bridge na CZ už beží)"))

    # REPEATER OLD — upload (gen_build_info zvýši build#), potom extrahuj OLD z zip
    run([PY, "-m", "platformio", "run", "-e", args.target_env,
         "-t", "upload", "--upload-port", args.target_port],
        f"Build+Upload REPEATER OLD ({args.target_env}, CZ) → {args.target_port}")
    sz = extract_app_image(args.target_env, OLD_BIN)
    old_build = read_build_number()
    print(green(f"[OK] OLD app obraz ({sz}B)  build #{old_build}"))
    # Po DFU: počkaj na port, vyčisti starú OTA session (CustomLFS @0xD4000 prežije
    # reflash!) a spáľ čistý reboot — rádio RX po DFU/CLI býva v zaseknutom stave.
    wait_port_back(args.target_port, timeout=30)
    print(cyan(">>> ota clear + reboot (čistý štart rádia + bez stale session)"))
    capture_serial(args.target_port, seconds=3, send_cmd="ota clear\r")
    capture_serial(args.target_port, seconds=14, send_cmd="reboot\r")
    print(yellow("\n>>> Hotovo. Build# sa zvýši sám, potom:"))
    print(yellow(f"      {PY} test_nrf-ota/ota_test_lora_repeater.py run "
                 f"--bridge-port {args.bridge_port} --target-port {args.target_port}"
                 f"{' --skip-bridge' if args.skip_bridge else ''}\n"))

def phase_run(args):
    print(green("\n══════ run — patch OLD→NEW broadcast cez bridge (CZ) ══════"))
    if not OLD_BIN.exists():
        sys.exit(red(f"[FAIL] {OLD_BIN} chýba — najprv: baseline"))

    cz_env = {"PLATFORMIO_BUILD_FLAGS": "-DLORA_PRESET_CZ"}
    run([PY, "-m", "platformio", "run", "-e", args.target_env],
        f"Build REPEATER NEW ({args.target_env}, CZ)", env_extra=cz_env)
    sz = extract_app_image(args.target_env, NEW_BIN)
    new_build = read_build_number()
    print(green(f"[OK] NEW app obraz ({sz}B)  build #{new_build}"))
    if OLD_BIN.read_bytes() == NEW_BIN.read_bytes():
        sys.exit(red("[FAIL] OLD == NEW — patch by bol prázdny (build# sa nezmenil?)."))

    psk_hex = OTA_PSK_STR.encode().hex()
    sender = [PY, str(OTA_SENDER), "--old", str(OLD_BIN), "--new", str(NEW_BIN),
              "--port", args.bridge_port, "--mode", "meshcore", "--psk", psk_hex,
              "--delay", str(args.delay)]
    if args.drop > 0.0:
        sender += ["--drop", str(args.drop)]
    if args.privkey:
        sender += ["--privkey", args.privkey, "--keyid", str(args.keyid)]
    if args.packetorder and args.packetorder != "normal":
        sender += ["--packetorder", args.packetorder]

    # Pozn.: reboot repeatera robí broadcast_until_verified PRED KAŽDÝM kolom
    # (čerstvé RX okno proti "stuck receiver"), takže sa tu už nerebootuje.

    # 1) Trpezlivý príjem: broadcast + poll 'ota status' kým VERIFIED / --verify-wait.
    #    (Nahradilo fixný --cycles loop, ktorý pri strate paketov na začiatku zlyhal
    #    skôr, než sa session skumulovala — viď readme_verified_pooling.md.)
    if not broadcast_until_verified(args, sender):
        print(red("[FAIL] Repeater nedosiahol VERIFIED v --verify-wait okne. "
                  "Skús väčší --verify-wait / menší --drop / over LoRa spoj."))
        print(yellow(">>> 'ota nack' (chýbajúce chunky):"))
        capture_serial(args.target_port, seconds=5, send_cmd="ota nack\r")
        sys.exit(1)

    # 3) Dry-run (opt-in cez --verify-first). DEFAULT VYPNUTÝ: 'ota verify' robí
    #    malloc + streaming rekonštrukciu celého FW; po ňom nasledujúci 'ota flash'
    #    občas hardfaultne na malloc (heap stav po dry-rune) → flasher sa zastaví po
    #    "Komprimovany format" a repeater nabehne na OLD. Manuálny flash bez dry-runu
    #    je spoľahlivý; bezpečnosť drží base-FW SHA256 check vnútri 'ota flash'.
    #    (viď readme_verified_pooling.md)
    if args.verify_first:
        print(cyan("\n>>> Dry-run 'ota verify' (bez zápisu)..."))
        capture_serial(args.target_port, seconds=12, send_cmd="ota verify\r")

    # 4) Ostrý flash + reboot, čítaj cez reboot
    print(cyan("\n>>> 'ota flash' — OSTRÝ flash + reboot..."))
    lines = capture_serial(args.target_port, seconds=args.capture, send_cmd="ota flash\r")
    evaluate(lines, new_build, fnv1a(NEW_BIN.read_bytes()))

def evaluate(lines, new_build, expected_fnv):
    print(green("\n══════ VÝSLEDOK ══════"))
    dev_fnv = None
    for l in lines:
        m = re.search(r"FNV-1a výstupu.*?0x([0-9A-Fa-f]+)", l)
        if m: dev_fnv = int(m.group(1), 16)
    print(f"   FNV-1a očakávaný (NEW): 0x{expected_fnv:08X}")
    if dev_fnv is not None:
        print(f"   FNV-1a flasher výstup:  0x{dev_fnv:08X}  "
              + (green("✓ SEDÍ") if dev_fnv == expected_fnv else red("✗ NESEDÍ")))

    seen = find_build(lines)
    print(f"   Očakávaný NEW build #{new_build}   beží #{seen if seen is not None else '?'}")
    dbg = [l for l in lines if "FLASHER-DBG" in l or "FLASHER-TRACE" in l or "Step=" in l]
    for l in dbg:
        print("   ", l)

    joined = " ".join(lines)
    if seen == new_build:
        print(green(f"\n[PASS] Repeater nabehol na build #{new_build} — OTA cez LoRa funguje! 🎉"))
    elif "Step=0x06" in joined:
        print(yellow("\n[WARN] Flasher hlási Step=0x06 (dokončil) ale build# nesedí — over banner."))
    else:
        print(red(f"\n[FAIL] Repeater nebeží build #{new_build}. Pozri trace/Step vyššie."))

def main():
    ap = argparse.ArgumentParser(description="End-to-end OTA test MeshCore repeatera cez LoRa")
    ap.add_argument("phase", choices=["baseline", "run"])
    ap.add_argument("--bridge-port", default=DEFAULT_BRIDGE_PORT)
    ap.add_argument("--target-port", default=DEFAULT_TARGET_PORT)
    ap.add_argument("--target-env",  default=DEFAULT_TARGET_ENV)
    ap.add_argument("--bridge-env",  default=DEFAULT_BRIDGE_ENV)
    ap.add_argument("--fk-lora",     default=DEFAULT_FK_LORA, type=Path,
                    help="Cesta k FK_lora-sniffer projektu (pre build bridge)")
    ap.add_argument("--skip-bridge", action="store_true",
                    help="Nereflashuj bridge (XIAO už beží ako CZ bridge)")
    ap.add_argument("--cycles", type=int, default=4,
                    help="(legacy) pôvodný fixný počet kôl; príjem teraz riadi --verify-wait")
    ap.add_argument("--drop", type=float, default=0.0)
    ap.add_argument("--delay", type=float, default=0.3)
    ap.add_argument("--cycle-delay", type=float, default=2.0,
                    help="Pauza medzi broadcast kolami [s]")
    ap.add_argument("--capture", type=int, default=60,
                    help="Sekundy čítania serialu pri 'ota flash' (pokrýva flash+reboot)")
    # Spevnenie VERIFIED-pollingu (viď readme_verified_pooling.md)
    ap.add_argument("--verify-wait", type=int, default=60,
                    help="Max sekúnd opakovať broadcast+poll kým príjem dosiahne VERIFIED")
    ap.add_argument("--poll-tries", type=int, default=2,
                    help="Počet 'ota status' pollov po každom broadcast kole")
    ap.add_argument("--poll-secs", type=int, default=5,
                    help="Sekundy čítania serialu na jeden 'ota status' poll")
    ap.add_argument("--reboot-settle", type=int, default=4,
                    help="Sekundy po reboote pred prvým broadcastom (RX arm)")
    ap.add_argument("--verify-first", action="store_true",
                     help="Spustiť 'ota verify' (dry-run) pred ostrým flashom. DEFAULT vyp — "
                          "dry-run pred flashom občas spôsobí hardfault flashera (heap).")
    ap.add_argument("--privkey", default=str(DEFAULT_PRIVKEY) if DEFAULT_PRIVKEY.exists() else None,
                     help="Ed25519 private key (DER/PEM) na podpis OTA HEADER "
                          "(default: test_nrf-ota/test_key.der — OTA env vyžaduje podpísaný HEADER)")
    ap.add_argument("--keyid", type=int, default=1,
                     help="Key ID pre podpis OTA HEADER (default: 1)")
    ap.add_argument("--packetorder", choices=["normal", "hbegin", "hmiddle", "hend"],
                     default="hend",
                     help="Pozícia HEADER paketu pri broadcaste. DEFAULT 'hend' (chunky "
                          "prvé, header nakoniec): RX po čerstvom boote chytí len ~4 rámce, "
                          "header/meta prežíva reboot → budget sa minie na chunky, nie na "
                          "už-známy header (spoľahlivejšie VERIFIED pri slabom RF). "
                          "'normal'=header prvý (out-of-order test: hmiddle/hbegin).")
    args = ap.parse_args()

    # Default --privkey = test signing key, ak existuje. Firmware vyžaduje podpísaný
    # OTA HEADER (Ed25519, key_id=1); bez kľúča ide nepodpísaný header → zariadenie
    # ho ZAMIETNE (CHYBA=0x6) a test NIKDY nedosiahne VERIFIED. test_key.der je
    # spárovaný s pubkey zakompilovaným vo firmware (OtaReceiver_signkey.cpp).
    if not args.privkey:
        _tk = SCRIPT_DIR / "test_key.der"
        if _tk.exists():
            args.privkey = str(_tk)
            print(cyan(f"[init] --privkey auto = {_tk.name} (key_id={args.keyid}) "
                       f"— firmware vyžaduje podpísaný HEADER"))

    try:
        import serial  # noqa
        from serial.tools import list_ports  # noqa
    except ImportError:
        sys.exit(red("[FAIL] pyserial chýba — spúšťaj cez penv python "
                     "(~/.platformio/penv/Scripts/python.exe)"))

    print(cyan("[preset] LoRa = CZ (869.525/SF7)"))
    if args.phase == "baseline":
        phase_baseline(args)
    else:
        phase_run(args)

if __name__ == "__main__":
    main()

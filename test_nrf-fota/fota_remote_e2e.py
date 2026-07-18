#!/usr/bin/env python3
"""fota_remote_e2e.py — orchestrátor FOTA operácií na lokálnych (COM) aj vzdialených (RPi) repeateroch.

Zariadenia a role (target/sender/monitor) definuje fota_devices.json vedľa skriptu.
Transport 'com'  = repeater priamo na COM porte tohto PC (pyserial).
Transport 'rpi'  = repeater na serial porte RPi; ovládanie cez SSH + tmux + picocom log.

Subcommandy:
    setup      [-d DEV]        self-check + bootstrap (RPi: tmux/picocom/nrfvenv/ts-okno; COM: port test)
    status     [-d DEV]        fota id + fota status (build#, sha, stav session)
    cmd        [-d DEV] "..."  ľubovoľný CLI príkaz na zariadení
    log        [-d DEV]        živý výpis logu zariadenia (Ctrl+C koniec)
    flash-dfu  [-d DEV] [--latest|--build N|--rebuild]   serial DFU flash (bez LoRa)
    send       [-d DEV] [--new BIN] [--old BIN] [...]    LoRa FOTA doručenie + miss-loop
    e2e        [-d DEV] [...]                            send → verify → flash cez CLI → overenie build#

Príklady:
    python fota_remote_e2e.py status
    python fota_remote_e2e.py flash-dfu -d t1000e-200km --latest
    python fota_remote_e2e.py e2e -d t1000e-200km --rebuild --monitor promicro-local

Časové pečiatky: lokálne logy (test_nrf-fota/logs/<dev>.log) značí tento skript
hh:mm:ss.mmm časom PC; na RPi drží setup tmux okno 'ts', ktoré zrkadlí picocom log
do *.ts.log s časom RPi (NTP) — odoslanie vs. doručenie sa dá porovnávať naprieč stanovišťami.
"""
import argparse, hashlib, json, re, socket, subprocess, sys, threading, time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
LOGS = HERE / "logs"

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


def now_ts():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def log(msg):
    print(f"[{now_ts()}] {msg}", flush=True)


# ────────────────────────── config ──────────────────────────

def load_cfg(path=None):
    p = Path(path) if path else HERE / "fota_devices.json"
    cfg = json.loads(p.read_text(encoding="utf-8"))
    cfg["_path"] = str(p)
    return cfg


def get_device(cfg, name):
    d = cfg["devices"].get(name)
    if not d:
        sys.exit(f"[CHYBA] zariadenie '{name}' nie je vo {cfg['_path']} (mám: {', '.join(cfg['devices'])})")
    d = dict(d); d["name"] = name
    if d.get("transport") == "rpi" and not d.get("host"):
        sys.exit(f"[CHYBA] '{name}' nemá vyplnený host (viď _todo v configu)")
    return d


def kind_info(cfg, dev):
    k = cfg.get("kinds", {}).get(dev.get("kind"))
    if not k:
        sys.exit(f"[CHYBA] kind '{dev.get('kind')}' zariadenia '{dev['name']}' nie je v configu (sekcia kinds)")
    return k


# ────────────────────────── konzoly ──────────────────────────

class Console:
    """Spoločné API: cli(cmd) → text odpovede; wait(regex) → match; raw_send(text)."""
    def cli(self, cmd, expect=r"^\s*->", timeout=8.0):
        raise NotImplementedError
    def wait(self, pattern, timeout=60.0):
        raise NotImplementedError


class RpiConsole(Console):
    """Repeater za RPi: príkazy cez tmux send-keys, čítanie cez rast picocom logfile."""
    def __init__(self, dev, ssh_key=None):
        self.dev = dev
        self.addr = f"{dev['user']}@{dev['host']}"
        self.key = ssh_key
        self.logfile = dev["log"]
        self.tmux = dev["tmux"]

    def ssh(self, cmd, timeout=30, check=False):
        args = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                "-o", "StrictHostKeyChecking=accept-new"]
        if self.key:
            args += ["-i", self.key]
        args += [self.addr, cmd]
        r = subprocess.run(args, capture_output=True, text=True, timeout=timeout,
                           encoding="utf-8", errors="replace")
        if check and r.returncode != 0:
            raise RuntimeError(f"ssh zlyhalo ({r.returncode}): {cmd}\n{r.stderr.strip()}")
        return r

    def scp(self, local, remote, timeout=300):
        args = ["scp", "-o", "BatchMode=yes"]
        if self.key:
            args += ["-i", self.key]
        args += [str(local), f"{self.addr}:{remote}"]
        r = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        if r.returncode != 0:
            raise RuntimeError(f"scp zlyhalo: {r.stderr.strip()}")

    def _log_size(self):
        r = self.ssh(f"stat -c %s {self.logfile} 2>/dev/null || echo 0")
        try:
            return int(r.stdout.strip().splitlines()[-1])
        except (ValueError, IndexError):
            return 0

    def _read_from(self, offset):
        r = self.ssh(f"tail -c +{offset + 1} {self.logfile} 2>/dev/null")
        return r.stdout

    def send_keys(self, text, enter=True):
        # send-keys -l = literal (bez interpretácie medzier/kľúčových slov)
        quoted = text.replace("'", "'\\''")
        suffix = " Enter" if enter else ""
        self.ssh(f"tmux send-keys -t {self.tmux} -l '{quoted}'; tmux send-keys -t {self.tmux}{suffix}", check=True)

    def cli(self, cmd, expect=r"^\s*->", timeout=8.0):
        start = self._log_size()
        self.send_keys(cmd)
        rex = re.compile(expect, re.M)
        deadline = time.time() + timeout
        buf = ""
        while time.time() < deadline:
            time.sleep(1.2)
            buf = self._read_from(start)
            if rex.search(buf):
                return buf
        return buf  # timeout — vráť čo je (volajúci rozhodne)

    def wait(self, pattern, timeout=60.0, poll=5.0):
        start = self._log_size()
        rex = re.compile(pattern, re.M)
        deadline = time.time() + timeout
        while time.time() < deadline:
            buf = self._read_from(start)
            m = rex.search(buf)
            if m:
                return m
            time.sleep(poll)
        return None

    def alive(self):
        r = self.ssh("echo OK")
        return r.returncode == 0 and "OK" in r.stdout


class ComConsole(Console):
    """Repeater priamo na COM porte. Každá operácia port otvorí a zavrie
    (port ostáva voľný pre iné nástroje, napr. DFU). Všetko prečítané sa
    priebežne zapisuje s časovou pečiatkou do logs/<dev>.log."""
    def __init__(self, dev):
        import serial  # pyserial
        self._serial_mod = serial
        self.dev = dev
        self.port = dev["port"]
        LOGS.mkdir(exist_ok=True)
        self.logpath = LOGS / f"{dev['name']}.log"

    def _open(self):
        try:
            return self._serial_mod.Serial(self.port, 115200, timeout=0.4)
        except self._serial_mod.SerialException as e:
            if "denied" in str(e).lower() or "Access" in str(e):
                sys.exit(f"[CHYBA] {self.port} je obsadený (Serial Monitor/putty na ňom?) — zavri ho a skús znova")
            sys.exit(f"[CHYBA] {self.port} sa nedá otvoriť: {e}")

    def _stamp_write(self, text):
        if not text:
            return
        with self.logpath.open("a", encoding="utf-8", errors="replace") as f:
            for line in text.splitlines():
                f.write(f"{now_ts()} {line}\n")

    def _collect(self, ser, seconds, stop_rex=None):
        buf = ""
        deadline = time.time() + seconds
        while time.time() < deadline:
            chunk = ser.read(4096).decode("utf-8", errors="replace")
            if chunk:
                buf += chunk
                if stop_rex and stop_rex.search(buf):
                    break
        self._stamp_write(buf)
        return buf

    def cli(self, cmd, expect=r"^\s*->", timeout=8.0):
        with self._open() as ser:
            ser.reset_input_buffer()
            ser.write((cmd + "\r").encode())
            return self._collect(ser, timeout, re.compile(expect, re.M))

    def wait(self, pattern, timeout=60.0, poll=None):
        rex = re.compile(pattern, re.M)
        deadline = time.time() + timeout
        with self._open() as ser:
            buf = ""
            while time.time() < deadline:
                chunk = ser.read(4096).decode("utf-8", errors="replace")
                if chunk:
                    self._stamp_write(chunk)
                    buf += chunk
                    m = rex.search(buf)
                    if m:
                        return m
        return None

    def alive(self):
        try:
            with self._open():
                return True
        except Exception:
            return False


class HubConsole(Console):
    """COM zariadenie za fota_serial_hub.py — hub vlastní port, my sa pripájame TCP.
    Hub loguje sám (logs/<meno>.log), takže tu nič nestampujeme."""
    def __init__(self, dev):
        self.dev = dev
        self.addr = ("127.0.0.1", int(dev["hub_port"]))
        self.logpath = LOGS / f"{dev['name']}.log"

    def _connect(self, timeout=3.0):
        s = socket.create_connection(self.addr, timeout=timeout)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.settimeout(0.4)
        return s

    def alive(self):
        try:
            self._connect(1.0).close()
            return True
        except OSError:
            return False

    def _collect(self, s, seconds, stop_rex=None):
        buf = ""
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            buf += chunk.decode("utf-8", errors="replace")
            if stop_rex and stop_rex.search(buf):
                break
        return buf

    def cli(self, cmd, expect=r"^\s*->", timeout=8.0):
        with self._connect() as s:
            self._collect(s, 0.3)  #sk: zahoď rozbehnutý výstup (heartbeaty)
            s.sendall((cmd + "\r").encode())
            return self._collect(s, timeout, re.compile(expect, re.M))

    def wait(self, pattern, timeout=60.0, poll=None):
        rex = re.compile(pattern, re.M)
        with self._connect() as s:
            buf = self._collect(s, timeout, rex)
        return rex.search(buf)

    def control(self, word):
        with self._connect() as s:
            s.sendall(f"~~HUB:{word}~~\n".encode())
            time.sleep(0.3)


def console_for(cfg, dev):
    if dev["transport"] == "rpi":
        return RpiConsole(dev, ssh_key=cfg.get("defaults", {}).get("ssh_key"))
    if dev["transport"] == "com":
        #sk: ak beží serial hub, choď cez neho (port drží on); inak priamo na COM
        if dev.get("hub_port"):
            hc = HubConsole(dev)
            if hc.alive():
                return hc
            log(f"[{dev['name']}] hub na :{dev['hub_port']} nebeží — idem priamo na {dev['port']}")
        return ComConsole(dev)
    sys.exit(f"[CHYBA] neznámy transport '{dev['transport']}' zariadenia '{dev['name']}'")


# ────────────────────────── parsovanie CLI odpovedí ──────────────────────────

RE_ID = re.compile(r"id b#(\d+) sz=(\d+) sha=([0-9A-Fa-f]+)")
RE_MISS = re.compile(r"miss=(\d+)/(\d+):?\s*([0-9,\-]*)")
RE_ALIVE = re.compile(r"AALIVE build #(\d+)")


def fota_id(con):
    out = con.cli("fota id", expect=r"-> id b#", timeout=10)
    m = RE_ID.search(out)
    return (int(m.group(1)), int(m.group(2)), m.group(3).upper()) if m else None


def fota_missall(con):
    out = con.cli("fota missall", expect=r"-> FOTA (miss|0/0)", timeout=10)
    m = RE_MISS.search(out)
    if m:
        return int(m.group(1)), int(m.group(2)), m.group(3).strip()
    if "0/0" in out:
        return None  # žiadna session
    return None


# ────────────────────────── buildy / artefakty ──────────────────────────

def newest_build(prefix, ext):
    builds = HERE / "builds"
    best, best_n = None, -1
    for f in builds.glob(f"{prefix}.fw_*{ext}"):
        m = re.match(rf"{prefix}\.fw_(\d+)", f.name)
        if m and int(m.group(1)) > best_n:
            best_n, best = int(m.group(1)), f
    return best_n, best


def build_by_num(prefix, num, ext):
    f = HERE / "builds" / f"{prefix}.fw_{num}{ext}"
    return f if f.exists() else None


def pio_build(env):
    pio = Path(sys.executable).with_name("pio.exe")
    pio = str(pio) if pio.exists() else "pio"
    log(f"pio run -e {env} ...")
    r = subprocess.run([pio, "run", "-e", env], cwd=ROOT, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=900)
    tail = "\n".join((r.stdout or "").splitlines()[-6:])
    if r.returncode != 0:
        sys.exit(f"[CHYBA] build zlyhal:\n{tail}")
    log("build OK")
    return r.stdout


def sha256_prefix(path, nbytes=4):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()[: nbytes * 2].upper()


def resolve_zip(cfg, dev, args):
    """--latest / --build N / --rebuild → cesta k DFU zipu."""
    ki = kind_info(cfg, dev)
    if args.rebuild:
        pio_build(ki["env"])
    if ki.get("pio_zip"):
        # bez builds/ archívu (napr. companion) — flashuje sa priamo .pio výstup
        pz = ROOT / ".pio" / "build" / ki["env"] / "firmware.zip"
        if not pz.exists():
            sys.exit(f"[CHYBA] {pz} neexistuje — spusti s --rebuild")
        return None, pz
    if args.build:
        z = build_by_num(ki["prefix"], args.build, ".zip")
        if not z:
            sys.exit(f"[CHYBA] builds/{ki['prefix']}.fw_{args.build}.zip neexistuje")
        return args.build, z
    n, z = newest_build(ki["prefix"], ".zip")
    if not z:
        pz = ROOT / ".pio" / "build" / ki["env"] / "firmware.zip"
        if pz.exists():
            return None, pz
        sys.exit(f"[CHYBA] žiadny {ki['prefix']}.fw_*.zip v builds/ (skús --rebuild)")
    return n, z


def devices_from_arg(cfg, arg, need_env=True):
    """-d meno | a,b,c | all → zoznam zariadení. 'all' = všetky s vyplneným hostom/portom a známym kind."""
    if not arg:
        return [get_device(cfg, cfg["defaults"]["target"])]
    if arg == "all":
        out = []
        for name, d in cfg["devices"].items():
            if d.get("transport") == "rpi" and not d.get("host"):
                log(f"[{name}] preskakujem — chýba host")
                continue
            if need_env and not cfg.get("kinds", {}).get(d.get("kind")):
                log(f"[{name}] preskakujem — kind bez env mapy")
                continue
            out.append(get_device(cfg, name))
        return out
    return [get_device(cfg, n.strip()) for n in arg.split(",")]


# ────────────────────────── setup / bootstrap ──────────────────────────

FK_TS_PY = ("import sys, datetime\n"
            "for l in sys.stdin:\n"
            "    sys.stdout.write(datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3] + ' ' + l)\n"
            "    sys.stdout.flush()\n")
TS_WINDOW_CMD = "tail -F {log} | python3 -u ~/fk_ts.py >> {log}.ts.log"


def setup_rpi(cfg, dev):
    con = console_for(cfg, dev)
    log(f"[{dev['name']}] SSH check {con.addr} ...")
    if not con.alive():
        sys.exit(f"[CHYBA] SSH na {con.addr} nejde (kľúč? WireGuard? viď fcl_remote_e2e_rpi.md §4)")
    log("SSH OK")

    # tmux + picocom + serial zariadenie
    r = con.ssh("which tmux picocom; ls {ser} 2>/dev/null".format(ser=dev["serial"]))
    out = r.stdout
    if "tmux" not in out or "picocom" not in out:
        log("inštalujem tmux/picocom (apt)...")
        con.ssh("sudo apt-get update -qq && sudo apt-get install -y -qq tmux picocom", timeout=300, check=True)
    if dev["serial"] not in out:
        sys.exit(f"[CHYBA] {dev['serial']} na RPi neexistuje — je repeater pripojený?")

    # nrfvenv + adafruit-nrfutil (na DFU)
    r = con.ssh("~/nrfvenv/bin/adafruit-nrfutil version 2>/dev/null")
    if "version" not in r.stdout:
        log("inštalujem adafruit-nrfutil do ~/nrfvenv ...")
        con.ssh("python3 -m venv ~/nrfvenv && ~/nrfvenv/bin/pip -q install adafruit-nrfutil", timeout=600, check=True)

    ensure_session(con, dev)

    # ~/.tmux.conf: história + myš
    con.ssh("grep -q history-limit ~/.tmux.conf 2>/dev/null || printf 'set -g history-limit 100000\\nset -g mouse on\\n' >> ~/.tmux.conf")
    log(f"[{dev['name']}] setup hotový")


def ensure_session(con, dev):
    """tmux session s picocomom v reštart-slučke + 'ts' okno s časovými pečiatkami."""
    r = con.ssh(f"tmux has-session -t {dev['tmux']} 2>&1")
    if r.returncode != 0:
        log("zakladám tmux session s picocom slučkou...")
        pico = (f"while true; do picocom -b 115200 --imap lfcrlf --logfile {dev['log']} {dev['serial']}; "
                f"echo PICOCOM-EXIT, restart o 3 s; sleep 3; done")
        con.ssh(f"tmux new -s {dev['tmux']} -d '{pico}'", check=True)
    # ts okno vždy postav nanovo (idempotентne) — stamper skript + tail pipeline
    b64 = __import__("base64").b64encode(FK_TS_PY.encode()).decode()
    con.ssh(f"echo {b64} | base64 -d > ~/fk_ts.py")
    con.ssh(f"tmux kill-window -t {dev['tmux']}:ts 2>/dev/null; true")
    tscmd = TS_WINDOW_CMD.format(log=dev["log"])
    con.ssh(f"tmux new-window -d -t {dev['tmux']} -n ts \"{tscmd}\"", check=True)
    log("ts okno (časové pečiatky) beží")


def setup_com(cfg, dev):
    con = console_for(cfg, dev)
    if not con.alive():
        sys.exit(f"[CHYBA] {dev['port']} sa nedá otvoriť (obsadený monitorom? zlý port?)")
    log(f"[{dev['name']}] {dev['port']} OK")
    if dev.get("role") != "sender":
        ident = fota_id(con)
        if ident:
            log(f"  build #{ident[0]}, sha={ident[2]}")


def cmd_setup(cfg, args):
    devs = [args.device] if args.device else list(cfg["devices"])
    for name in devs:
        dev = cfg["devices"][name]
        if dev.get("transport") == "rpi" and not dev.get("host"):
            log(f"[{name}] preskakujem — chýba host (viď _todo)")
            continue
        dev = get_device(cfg, name)
        (setup_rpi if dev["transport"] == "rpi" else setup_com)(cfg, dev)


# ────────────────────────── operácie ──────────────────────────

def cmd_hub(cfg, args):
    """Spustí fota_serial_hub.py pre zariadenie v novom okne (beží ďalej sám)."""
    dev = get_device(cfg, args.device or cfg["defaults"]["target"])
    if dev.get("transport") != "com" or not dev.get("hub_port"):
        sys.exit(f"[CHYBA] '{dev['name']}' nie je com zariadenie s hub_port v configu")
    if HubConsole(dev).alive():
        log(f"hub pre {dev['name']} už beží na localhost:{dev['hub_port']}")
        return
    flags = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
    subprocess.Popen([sys.executable, str(HERE / "fota_serial_hub.py"), "--device", dev["name"]],
                     cwd=ROOT, creationflags=flags)
    time.sleep(2)
    ok = HubConsole(dev).alive()
    log(f"hub {'beží' if ok else 'sa nerozbehol?!'} — pripojenie: PuTTY Raw localhost:{dev['hub_port']}"
        f" | log: test_nrf-fota/logs/{dev['name']}.log")


def cmd_status(cfg, args):
    dev = get_device(cfg, args.device or cfg["defaults"]["target"])
    con = console_for(cfg, dev)
    ident = fota_id(con)
    if not ident:
        sys.exit(f"[CHYBA] {dev['name']} neodpovedá na 'fota id' (CLI mŕtve? port obsadený?)")
    log(f"{dev['name']}: build #{ident[0]}  image={ident[1]}B  sha={ident[2]}")
    out = con.cli("fota status", expect=r"-> FOTA", timeout=10)
    m = re.search(r"-> (FOTA .+)$", out, re.M)
    if m:
        log(f"{dev['name']}: {m.group(1)}")


def cmd_cmd(cfg, args):
    dev = get_device(cfg, args.device or cfg["defaults"]["target"])
    con = console_for(cfg, dev)
    out = con.cli(args.command, timeout=args.timeout)
    print(out)


def cmd_log(cfg, args):
    dev = get_device(cfg, args.device or cfg["defaults"]["target"])
    con = console_for(cfg, dev)
    log(f"živý log {dev['name']} — Ctrl+C koniec")
    try:
        if dev["transport"] == "rpi":
            key = ["-i", cfg["defaults"].get("ssh_key")] if cfg["defaults"].get("ssh_key") else []
            subprocess.run(["ssh", "-o", "BatchMode=yes", *key, con.addr, f"tail -n 20 -F {dev['log']}"])
        else:
            while True:
                m = con.wait(r".+", timeout=3600)
                if m:
                    print(f"[{now_ts()}] {m.group(0)}")
    except KeyboardInterrupt:
        pass


def cmd_build(cfg, args):
    """build vybraných/všetkých zariadení (len preklad, bez flashu); envy sa deduplikujú."""
    devs = devices_from_arg(cfg, args.device)
    envs = []
    for dev in devs:
        env = kind_info(cfg, dev)["env"]
        if env not in envs:
            envs.append(env)
    for env in envs:
        pio_build(env)
    log(f"buildy hotové: {', '.join(envs)}")


def cmd_flash_dfu(cfg, args):
    for dev in devices_from_arg(cfg, args.device):
        flash_one(cfg, dev, args)


def flash_one(cfg, dev, args):
    ki = kind_info(cfg, dev)
    if ki.get("method") == "pio-upload":
        # ESP32 (napr. Heltec) — pio upload cez esptool, len lokálny COM
        if dev["transport"] != "com":
            sys.exit(f"[CHYBA] {dev['name']}: pio-upload ide len na lokálnom COM porte")
        if args.rebuild:
            pio_build(ki["env"])
        pio = Path(sys.executable).with_name("pio.exe")
        pio = str(pio) if pio.exists() else "pio"
        log(f"pio upload {ki['env']} → {dev['port']} ...")
        r = subprocess.run([pio, "run", "-e", ki["env"], "-t", "upload",
                            "--upload-port", dev["port"]], cwd=ROOT, capture_output=True,
                           text=True, encoding="utf-8", errors="replace", timeout=900)
        if r.returncode != 0:
            sys.exit(f"[CHYBA] upload zlyhal:\n" + "\n".join((r.stdout or "").splitlines()[-8:]))
        log(f"{dev['name']}: upload OK")
        return
    num, zpath = resolve_zip(cfg, dev, args)
    log(f"DFU flash {dev['name']} ← {zpath.name}" + (f" (build #{num})" if num else ""))
    if dev["transport"] == "rpi":
        con = console_for(cfg, dev)
        con.scp(zpath, "/tmp/fota_dfu.zip")
        log("balík prenesený; zastavujem picocom session...")
        con.ssh(f"tmux kill-session -t {dev['tmux']} 2>/dev/null; sleep 1")
        r = con.ssh("~/nrfvenv/bin/adafruit-nrfutil dfu serial --package /tmp/fota_dfu.zip "
                    f"-p {dev['serial']} -b 115200 --singlebank --touch 1200", timeout=420)
        ok = "Device programmed" in r.stdout
        log("DFU: " + ("Device programmed" if ok else f"NEISTÉ — výstup: {r.stdout[-300:]} {r.stderr[-200:]}"))
        time.sleep(6)
        ensure_session(con, dev)
        if not ok:
            sys.exit("[CHYBA] DFU pravdepodobne zlyhalo — zariadenie môže byť v bootloaderi, DFU sa dá zopakovať")
    else:
        nrfutil = Path(sys.executable).with_name("adafruit-nrfutil.exe")
        nrfutil = str(nrfutil) if nrfutil.exists() else "adafruit-nrfutil"
        #sk: ak port drží serial hub, vypýtaj si ho (PAUSE) a po DFU vráť (RESUME)
        hub = HubConsole(dev) if dev.get("hub_port") else None
        hub_paused = False
        if hub and hub.alive():
            hub.control("PAUSE")
            hub_paused = True
            time.sleep(1.5)
            log("hub PAUSED — port uvoľnený na DFU")
        try:
            r = subprocess.run([nrfutil, "dfu", "serial", "--package", str(zpath),
                                "-p", dev["port"], "-b", "115200", "--singlebank", "--touch", "1200"],
                               capture_output=True, text=True, timeout=420)
        finally:
            if hub_paused:
                time.sleep(3)
                hub.control("RESUME")
                log("hub RESUME")
        if "Device programmed" not in (r.stdout or ""):
            sys.exit(f"[CHYBA] DFU zlyhalo: {(r.stdout or '')[-300:]} {(r.stderr or '')[-200:]}")
        log("DFU: Device programmed")
        time.sleep(6)
    #sk: companion/pio_zip FW nemá FOTA CLI → 'fota id' overenie len pre repeatre
    if dev.get("role") == "sender" or kind_info(cfg, dev).get("pio_zip"):
        log(f"{dev['name']}: naflashované (bez fota id overenia — nie je repeater FW)")
    else:
        verify_running_build(cfg, dev, num)


def verify_running_build(cfg, dev, expect_num, tries=12, delay=10):
    con = console_for(cfg, dev)
    for _ in range(tries):
        ident = fota_id(con)
        if ident:
            log(f"{dev['name']} beží: build #{ident[0]} sha={ident[2]}")
            if expect_num and ident[0] != expect_num:
                sys.exit(f"[CHYBA] čakal som build #{expect_num}, beží #{ident[0]}")
            return ident
        time.sleep(delay)
    sys.exit(f"[CHYBA] {dev['name']} po flashi neodpovedá na 'fota id'")


def run_sender(cfg, dev, old_bin, new_bin, chunks=None, scope=None, delay=None, with_header=False):
    sender = get_device(cfg, cfg["defaults"]["sender"])
    d = cfg["defaults"]
    scope = scope or d.get("scope", "direct")
    args = [sys.executable, str(HERE / "fota_sender_mcpy.py"),
            "--old", str(old_bin), "--new", str(new_bin),
            "--port", sender["port"], "--delay", str(delay or d.get("delay", 5)),
            "--scope", scope,
            "--privkey", str(ROOT / d["privkey"]),
            "--patch", str(LOGS / "patch_last.bin")]
    if scope == "direct":
        if not dev.get("path_to"):
            sys.exit(f"[CHYBA] scope=direct, ale '{dev['name']}' nemá path_to v configu")
        args += ["--path", dev["path_to"]]
    if chunks:
        args += ["--chunks", chunks]
    if with_header:
        args += ["--with-header"]
    if dev.get("keyid") is not None:
        args += ["--keyid", str(dev["keyid"])]  #sk: staré FW (pred v0-prefix) = 1
    log(f"sender: scope={scope}" + (f" chunks={chunks}" if chunks else " (celý patch)"))
    r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=3600)
    if r.returncode != 0:
        sys.exit(f"[CHYBA] sender zlyhal:\n{(r.stdout or '')[-500:]}\n{(r.stderr or '')[-300:]}")
    return r.stdout


class MonitorCapture:
    """Monitor zariadenie počas send/e2e: COM → vlákno číta port do logs/<dev>.log;
    RPi → len značka v logu, počty sa zrátajú na konci grepom."""
    def __init__(self, cfg, name):
        self.dev = get_device(cfg, name)
        self.cfg = cfg
        self.stop_evt = threading.Event()
        self.thread = None
        self.rpi_start = None

    def __enter__(self):
        if self.dev["transport"] == "com" and self.dev.get("hub_port") and HubConsole(self.dev).alive():
            #sk: hub loguje sám do logs/<meno>.log — stačí si zapamätať offset
            self.mark = LOGS / f"{self.dev['name']}.log"
            self.start_size = self.mark.stat().st_size if self.mark.exists() else 0
            self.thread = None
            log(f"monitor: {self.dev['name']} (cez hub log)")
            return self
        if self.dev["transport"] == "com":
            con = ComConsole(self.dev)
            def pump():
                try:
                    with con._open() as ser:
                        while not self.stop_evt.is_set():
                            chunk = ser.read(4096).decode("utf-8", errors="replace")
                            con._stamp_write(chunk)
                except Exception as e:
                    log(f"[monitor {self.dev['name']}] výpadok: {e}")
            self.mark = ComConsole(self.dev).logpath
            self.start_size = self.mark.stat().st_size if self.mark.exists() else 0
            self.thread = threading.Thread(target=pump, daemon=True)
            self.thread.start()
        else:
            con = RpiConsole(self.dev, self.cfg["defaults"].get("ssh_key"))
            self.rpi_start = con._log_size()
        log(f"monitor: {self.dev['name']} zapnutý")
        return self

    def __exit__(self, *exc):
        counts = {}
        if self.dev["transport"] == "com":
            if self.thread:
                self.stop_evt.set()
                self.thread.join(timeout=3)
            text = ""
            if self.mark.exists():
                with self.mark.open(encoding="utf-8", errors="replace") as f:
                    f.seek(self.start_size)
                    text = f.read()
        else:
            con = RpiConsole(self.dev, self.cfg["defaults"].get("ssh_key"))
            text = con._read_from(self.rpi_start)
        counts["GRP_DATA/FOTA RAW"] = len(re.findall(r"GRP_DATA/FOTA", text))
        counts["CHUNK spracované"] = len(re.findall(r"\[FOTA\] CHUNK", text))
        counts["[FK] drop"] = len(re.findall(r"\[FK\] GRP_DATA", text))
        log(f"monitor {self.dev['name']}: " + ", ".join(f"{k}={v}" for k, v in counts.items()))
        return False


def do_send(cfg, args):
    """Doručenie patchu + miss-loop. Vráti (dev, con, new_num)."""
    dev = get_device(cfg, args.device or cfg["defaults"]["target"])
    ki = kind_info(cfg, dev)
    con = console_for(cfg, dev)

    if args.rebuild:
        pio_build(ki["env"])

    # NEW bin
    if args.new:
        new_bin, new_num = Path(args.new), None
        m = re.search(r"fw_(\d+)", new_bin.name)
        new_num = int(m.group(1)) if m else None
    else:
        new_num, new_bin = newest_build(ki["prefix"], ".bin")
        if not new_bin:
            sys.exit(f"[CHYBA] žiadny {ki['prefix']}.fw_*.bin v builds/ (--rebuild alebo --new)")

    # OLD bin = bežiaci FW (fota id → build# + sha krížová kontrola)
    ident = fota_id(con)
    if not ident:
        sys.exit(f"[CHYBA] {dev['name']} neodpovedá na 'fota id'")
    run_num, _, run_sha = ident
    if args.old:
        old_bin = Path(args.old)
    else:
        old_bin = build_by_num(ki["prefix"], run_num, ".bin")
        if not old_bin:
            sys.exit(f"[CHYBA] beží #{run_num}, ale builds/{ki['prefix']}.fw_{run_num}.bin nemám — zadaj --old")
    if sha256_prefix(old_bin) != run_sha:
        sys.exit(f"[CHYBA] sha bežiaceho FW ({run_sha}) nesedí s {old_bin.name} "
                 f"({sha256_prefix(old_bin)}) — zlý --old / builds archív")
    if new_num and new_num == run_num:
        sys.exit(f"[CHYBA] cieľový build #{new_num} už beží")
    log(f"{dev['name']}: #{run_num} → #{new_num or '?'}  (old={old_bin.name}, new={new_bin.name})")

    con.cli("fota clear", expect=r"-> FOTA cleared", timeout=10)

    run_sender(cfg, dev, old_bin, new_bin, scope=args.scope, delay=args.delay)

    max_rounds = args.max_rounds or cfg["defaults"].get("max_rounds", 12)
    header_retried = False
    for rnd in range(1, max_rounds + 1):
        miss = fota_missall(con)
        if miss is None:
            if header_retried:
                sys.exit(f"[CHYBA] {dev['name']} nemá FOTA session ani po re-sende META/SIG — skús --scope flood")
            log("session bez headera — doposielam META/SIG")
            #sk: --chunks "H" = prázdny zoznam chunkov (H/S sa ignorujú) + --with-header = len META/SIG
            run_sender(cfg, dev, old_bin, new_bin, chunks="H", scope=args.scope,
                       delay=args.delay, with_header=True)
            header_retried = True
            continue
        n, total, lst = miss
        if n == 0:
            log(f"KOMPLET {total}/{total} po {rnd - 1} doposielacích kolách")
            return dev, con, new_num
        log(f"kolo {rnd}: chýba {n}/{total}: {lst}")
        run_sender(cfg, dev, old_bin, new_bin, chunks=lst, scope=args.scope, delay=args.delay)
    sys.exit(f"[CHYBA] po {max_rounds} kolách stále chýbajú chunky — RF podmienky? skús neskôr / iný scope")


#sk: staré FW (#263) nemajú /FOTA tag a rssi/snr je pred type — tolerantný vzor
RE_RAW_FOTA = re.compile(r"type=6\(GRP_DATA(?:/FOTA)?\).*?route=1.*?path\[(\d+)\]=([0-9A-Fa-f]+)")


def cmd_probe_path(cfg, args):
    """Flood sonda (len META/SIG) → z RAW logu targetu vyčítať, kadiaľ flood došiel,
    a navrhnúť path_to pre scope=direct. Nič neflashuje; session po sebe uprace."""
    dev = get_device(cfg, args.device or cfg["defaults"]["target"])
    ki = kind_info(cfg, dev)
    con = console_for(cfg, dev)

    ident = fota_id(con)
    if not ident:
        sys.exit(f"[CHYBA] {dev['name']} neodpovedá na 'fota id'")
    old_bin = build_by_num(ki["prefix"], ident[0], ".bin")
    new_num, new_bin = newest_build(ki["prefix"], ".bin")
    if not old_bin or not new_bin:
        sys.exit(f"[CHYBA] na sondu treba builds/{ki['prefix']}.fw_{ident[0]}.bin aj nejaký novší .bin")

    start = con._log_size() if isinstance(con, RpiConsole) else None
    n_probes = args.probes
    for i in range(n_probes):
        log(f"sonda {i + 1}/{n_probes} (META+SIG, flood)...")
        run_sender(cfg, dev, old_bin, new_bin, chunks="H", scope="flood",
                   delay=args.delay, with_header=True)
        time.sleep(3)

    text = con._read_from(start) if isinstance(con, RpiConsole) else ""
    if not isinstance(con, RpiConsole):
        sys.exit("[CHYBA] probe-path zatiaľ len pre RPi target (lokálny netreba — je na USB)")
    paths = [(int(m.group(1)), m.group(2).upper()) for m in RE_RAW_FOTA.finditer(text)]
    con.cli("fota clear", expect=r"-> FOTA cleared", timeout=10)
    if not paths:
        sys.exit(f"[CHYBA] sonda na {dev['name']} nedošla (v RAW logu nič) — zopakuj / skús neskôr")
    paths.sort()
    uniq = []
    for hops, p in paths:
        if p not in [u[1] for u in uniq]:
            uniq.append((hops, p))
    log(f"počuté flood cesty ({len(paths)} paketov): " +
        "; ".join(f"{p} ({h} hopov)" for h, p in uniq[:5]))
    best = uniq[0]
    log(f"NÁVRH path_to = {best[1]}  ({best[0]} hopy) — zapíš do fota_devices.json "
        f"('{dev['name']}'.path_to) ak vyzerá rozumne")
    return best[1]


def cmd_send(cfg, args):
    mon = MonitorCapture(cfg, args.monitor) if args.monitor else None
    if mon:
        with mon:
            do_send(cfg, args)
    else:
        do_send(cfg, args)


def cmd_e2e(cfg, args):
    t0 = time.time()
    mon = MonitorCapture(cfg, args.monitor) if args.monitor else None
    ctx = mon if mon else _null_ctx()
    with ctx:
        dev, con, new_num = do_send(cfg, args)
        out = con.cli("fota verify", expect=r"-> FOTA dry-run", timeout=90)
        if "dry-run OK" not in out:
            sys.exit(f"[CHYBA] verify nezbehol:\n{out[-400:]}")
        log("verify OK — spúšťam fota flash (repeater ~1 min nebude repeatovať)")
        con.send_keys("fota flash") if isinstance(con, RpiConsole) else con.cli("fota flash", expect=r"FLASHER", timeout=15)
        time.sleep(45)
        if isinstance(con, RpiConsole):
            ensure_session(con, dev)  # picocom slučka sa zdvihne sama, session pre istotu
        ident = verify_running_build(cfg, dev, new_num)
        log(f"=== E2E PASS: {dev['name']} beží build #{ident[0]} (celkovo {int(time.time() - t0)} s) ===")


class _null_ctx:
    def __enter__(self):
        return self
    def __exit__(self, *a):
        return False


# ────────────────────────── main ──────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", help="cesta k fota_devices.json (default vedľa skriptu)")
    sub = ap.add_subparsers(dest="op", required=True)

    def dev_arg(p):
        p.add_argument("-d", "--device", help="meno zariadenia z configu (default: defaults.target)")

    p = sub.add_parser("setup", help="self-check + bootstrap zariadení"); dev_arg(p)

    p = sub.add_parser("status", help="fota id + fota status"); dev_arg(p)

    p = sub.add_parser("cmd", help="ľubovoľný CLI príkaz"); dev_arg(p)
    p.add_argument("command")
    p.add_argument("--timeout", type=float, default=8.0)

    p = sub.add_parser("log", help="živý log zariadenia"); dev_arg(p)

    p = sub.add_parser("hub", help="spusti serial hub pre COM zariadenie (nové okno)"); dev_arg(p)

    p = sub.add_parser("build", help="len preklad (pio run) vybraných zariadení"); dev_arg(p)

    p = sub.add_parser("flash-dfu", help="priamy flash (serial DFU / pio upload), bez LoRa"); dev_arg(p)
    g = p.add_mutually_exclusive_group()
    g.add_argument("--latest", action="store_true", help="najnovší zip v builds/ (default)")
    g.add_argument("--build", type=int, help="konkrétny build# z builds/")
    p.add_argument("--rebuild", action="store_true", help="najprv pio run -e <env>")

    p = sub.add_parser("probe-path", help="flood sonda → návrh path_to pre direct"); dev_arg(p)
    p.add_argument("--probes", type=int, default=2, help="počet sond (default 2)")
    p.add_argument("--delay", type=float)

    for name, hlp in (("send", "LoRa FOTA doručenie + miss-loop"),
                      ("e2e", "send → verify → flash cez CLI → overenie build#")):
        p = sub.add_parser(name, help=hlp); dev_arg(p)
        p.add_argument("--new", help="nový FW .bin (default najnovší v builds/)")
        p.add_argument("--old", help="bežiaci FW .bin (default z builds/ podľa fota id)")
        p.add_argument("--rebuild", action="store_true", help="najprv pio run -e <env>")
        p.add_argument("--scope", choices=["zerohop", "flood", "direct"])
        p.add_argument("--delay", type=float, help="s medzi paketmi (default z configu; živá sieť = 5)")
        p.add_argument("--max-rounds", type=int)
        p.add_argument("--monitor", help="meno monitor zariadenia (počty RAW/CHUNK počas behu)")

    args = ap.parse_args()
    cfg = load_cfg(args.config)
    {"setup": cmd_setup, "status": cmd_status, "cmd": cmd_cmd, "log": cmd_log,
     "hub": cmd_hub, "build": cmd_build, "flash-dfu": cmd_flash_dfu,
     "probe-path": cmd_probe_path, "send": cmd_send, "e2e": cmd_e2e}[args.op](cfg, args)


if __name__ == "__main__":
    main()

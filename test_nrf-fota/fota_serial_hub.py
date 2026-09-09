#!/usr/bin/env python3
"""fota_serial_hub.py — zdieľanie COM portu (Windows ekvivalent tmux+picocom na RPi).

Jeden proces vlastní COM port; klienti (PuTTY typ Raw, telnet, orchestrátor) sa
pripájajú na localhost:<hub_port> a zdieľajú výstup aj vstup zariadenia. Všetko
prečítané sa loguje s časovými pečiatkami do test_nrf-fota/logs/<meno>.log.
Prežíva odpojenie zariadenia (DFU/FOTA reboot, USB re-enumerácia) — retry slučka.

Spustenie:
    python fota_serial_hub.py --device promicro-local     # COM + hub_port z fota_devices.json
    python fota_serial_hub.py --port COM5 --listen 7455   # ručne, bez configu

Pripojenie používateľa: PuTTY → Connection type: Raw, Host: localhost, Port: <hub_port>
(scrollback rieši PuTTY; celá história je v logs/<meno>.log).

Kontrolné riadky od klienta (neposielajú sa do serialu):
    ~~HUB:PAUSE~~    uvoľní COM port (napr. na DFU flash)
    ~~HUB:RESUME~~   port znova otvorí
    ~~HUB:STATUS~~   vypíše stav hubu
"""
import argparse, json, socket, sys, threading, time
from datetime import datetime
from pathlib import Path

import serial

HERE = Path(__file__).resolve().parent
LOGS = HERE / "logs"

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


def now_ts():
    # den v pecatke je zamerne: pri behoch cez polnoc sa inak ten isty cas
    # v logu opakuje a hladanie podla casu trafi iny den (stalo sa)
    return datetime.now().strftime("%d-%H:%M:%S.%f")[:-3]


class Hub:
    def __init__(self, com, listen_port, logpath, baud=115200):
        self.com, self.listen_port, self.baud = com, listen_port, baud
        self.logpath = logpath
        self.clients = []            # [socket]
        self.lock = threading.Lock()
        self.ser = None
        self.paused = False
        self.running = True
        self._pending = ""           # rozpísaný riadok pre log

    # ── log s pečiatkami (riadkovo bufferovaný) ──
    def log_text(self, text):
        self._pending += text
        if "\n" not in self._pending:
            return
        *lines, self._pending = self._pending.split("\n")
        with self.logpath.open("a", encoding="utf-8", errors="replace") as f:
            for ln in lines:
                f.write(f"{now_ts()} {ln.rstrip()}\n")

    def status_line(self, msg):
        line = f"\r\n[HUB {now_ts()}] {msg}\r\n"
        print(line.strip(), flush=True)
        self.broadcast(line.encode())
        self.log_text(line)

    # ── klienti ──
    def broadcast(self, data, exclude=None):
        with self.lock:
            dead = []
            for c in self.clients:
                if c is exclude:
                    continue
                try:
                    c.sendall(data)
                except OSError:
                    dead.append(c)
            for c in dead:
                self.clients.remove(c)

    def handle_client(self, sock, addr):
        self.status_line(f"klient {addr[0]}:{addr[1]} pripojený ({len(self.clients)} spolu)")
        buf = b""
        try:
            while self.running:
                data = sock.recv(1024)
                if not data:
                    break
                buf += data
                #sk: kontrolné príkazy chytáme po riadkoch; ostatné bajty idú rovno do serialu
                while b"~~HUB:" in buf and (b"\n" in buf or b"\r" in buf):
                    line, _, rest = buf.replace(b"\r", b"\n").partition(b"\n")
                    if line.strip().startswith(b"~~HUB:"):
                        self.control(line.strip().decode(errors="replace"), sock)
                        buf = rest
                    else:
                        break
                if buf and not buf.startswith(b"~~HUB:"):
                    self.to_serial(buf)
                    buf = b""
        except OSError:
            pass
        finally:
            with self.lock:
                if sock in self.clients:
                    self.clients.remove(sock)
            sock.close()
            self.status_line(f"klient {addr[0]}:{addr[1]} odpojený")

    def control(self, cmd, sock):
        if "QUIT" in cmd:
            self.status_line("QUIT — hub končí")
            self.running = False
            self.close_serial()
            import os
            os._exit(0)
        elif "PAUSE" in cmd:
            self.paused = True
            self.close_serial()
            self.status_line(f"PAUSED — {self.com} uvoľnený (DFU môže bežať)")
        elif "RESUME" in cmd:
            self.paused = False
            self.status_line("RESUME — pripájam port")
        elif "STATUS" in cmd:
            st = "paused" if self.paused else ("open" if self.ser else "reconnecting")
            try:
                sock.sendall(f"\r\n[HUB] {self.com} {st}, {len(self.clients)} klientov\r\n".encode())
            except OSError:
                pass

    # ── serial ──
    def to_serial(self, data):
        with self.lock:
            if self.ser:
                try:
                    self.ser.write(data)
                except serial.SerialException:
                    pass

    def close_serial(self):
        with self.lock:
            if self.ser:
                try:
                    self.ser.close()
                except Exception:
                    pass
                self.ser = None

    def serial_loop(self):
        #en: this thread is the ONLY writer of live data — if it dies (any
        #en: uncaught exception, e.g. a transient Windows file-sharing conflict
        #en: on the log file), the hub looks alive (TCP still accepts clients)
        #en: but silently stops reading/logging forever. Wrap the whole body so
        #en: a single bad iteration can't kill the loop; log the exception and
        #en: keep going instead of dying invisibly.
        #sk: toto vlákno je JEDINÝ zapisovateľ živých dát — ak zomrie (čokoľvek
        #sk: nezachytené, napr. prechodný Windows file-sharing konflikt na
        #sk: logovacom súbore), hub vyzerá živo (TCP stále prijíma klientov),
        #sk: ale ticho navždy prestane čítať/logovať. Obaľ celé telo, nech jedna
        #sk: zlá iterácia slučku nezabije; výnimku zaloguj a pokračuj ďalej.
        announced = False
        while self.running:
            try:
                if self.paused:
                    time.sleep(0.5)
                    continue
                if self.ser is None:
                    try:
                        self.ser = serial.Serial(self.com, self.baud, timeout=0.3)
                        self.status_line(f"{self.com} otvorený")
                        announced = False
                    except serial.SerialException as e:
                        #en: port absent (reboot/re-enumeration) → fast retry to catch boot
                        #en: messages from the device CDC TX buffer; busy → slow retry.
                        #sk: port neexistuje (reboot/re-enumerácia) → rýchly retry, nech
                        #sk: chytíme boot hlášky z CDC TX buffera zariadenia; obsadený → pomalý.
                        busy = "denied" in str(e).lower() or "access" in str(e).lower()
                        if not announced:
                            self.status_line(f"{self.com} {'obsadený iným procesom' if busy else 'zmizol — čakám na návrat'}"
                                             f" (retry {'1 s' if busy else '0.15 s'})")
                            announced = True
                        time.sleep(1.0 if busy else 0.15)
                        continue
                try:
                    data = self.ser.read(4096)
                except serial.SerialException:
                    self.close_serial()
                    self.status_line(f"{self.com} odpojený (reboot?) — čakám na návrat")
                    time.sleep(2)
                    continue
                if data:
                    self.broadcast(data)
                    try:
                        self.log_text(data.decode("utf-8", errors="replace"))
                    except OSError as e:
                        #en: log write failed (e.g. file locked by a concurrent reader) —
                        #en: drop this chunk from the log, but keep the loop (and broadcast) alive.
                        #sk: zápis do logu zlyhal (napr. súbor uzamknutý iným čítačom) —
                        #sk: tento kúsok logu zahoď, ale slučku (aj broadcast) drž nažive.
                        print(f"[HUB {now_ts()}] WARN log zápis zlyhal: {e}", flush=True)
            except Exception as e:
                #en: last-resort catch-all — never let this thread die silently.
                #sk: posledná poistka — toto vlákno nesmie ticho zomrieť.
                print(f"[HUB {now_ts()}] CHYBA v serial_loop: {e!r} — pokračujem", flush=True)
                time.sleep(0.5)

    def _serial_loop_supervisor(self):
        #en: belt-and-suspenders — serial_loop() already catches everything, but
        #en: if it somehow still returns/dies (e.g. a fatal interpreter error),
        #en: restart it rather than leaving the hub silently deaf forever.
        #sk: pre istotu — serial_loop() už chytá všetko, ale keby aj tak niekedy
        #sk: skončil (napr. fatálna chyba interpretera), reštartuj ho namiesto
        #sk: toho, aby hub ostal navždy ticho hluchý.
        while self.running:
            self.serial_loop()
            if self.running:
                print(f"[HUB {now_ts()}] serial_loop skončil neočakávane — reštart o 1 s", flush=True)
                time.sleep(1)

    def serve(self):
        LOGS.mkdir(exist_ok=True)
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", self.listen_port))
        srv.listen(8)
        threading.Thread(target=self._serial_loop_supervisor, daemon=True).start()
        self.status_line(f"hub beží: {self.com} ↔ localhost:{self.listen_port}, log={self.logpath.name}")
        try:
            while self.running:
                sock, addr = srv.accept()
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                with self.lock:
                    self.clients.append(sock)
                threading.Thread(target=self.handle_client, args=(sock, addr), daemon=True).start()
        except KeyboardInterrupt:
            self.running = False
            self.status_line("hub končí (Ctrl+C)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", help="meno z fota_devices.json (vezme port + hub_port)")
    ap.add_argument("--port", help="COM port (ručne)")
    ap.add_argument("--listen", type=int, help="TCP port na localhost (ručne)")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    name = args.device or (args.port or "hub")
    com, listen = args.port, args.listen
    if args.device:
        cfg = json.loads((HERE / "fota_devices.json").read_text(encoding="utf-8"))
        dev = cfg["devices"].get(args.device)
        if not dev or dev.get("transport") != "com":
            sys.exit(f"[CHYBA] '{args.device}' nie je com zariadenie v configu")
        com = dev["port"]
        listen = dev.get("hub_port")
        if not listen:
            sys.exit(f"[CHYBA] '{args.device}' nemá hub_port v configu")
    if not com or not listen:
        sys.exit("[CHYBA] zadaj --device, alebo --port aj --listen")

    Hub(com, listen, LOGS / f"{name}.log", args.baud).serve()


if __name__ == "__main__":
    main()

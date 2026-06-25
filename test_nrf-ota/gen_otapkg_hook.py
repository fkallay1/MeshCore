#!/usr/bin/env python3
"""
gen_otapkg_hook.py — PlatformIO POST-build hook.

Po tom, čo gen_fw_trailer.py vyplní FwIdTrailer vo firmware.hex, tento hook:
  - archivuje app image aktuálneho buildu (builds/fw_<N>.bin),
  - vygeneruje fw.otapkg.json (upgrade) + fw_reverse.otapkg.json (rollback)
    medzi POSLEDNÝM a PREDPOSLEDNÝM buildom (cez gen_otapkg.py --from-hex).

NEFATÁLNE: akékoľvek zlyhanie (chýbajúci predošlý build, hdiffi, privkey…) len
vypíše varovanie a NIKDY nezhodí build.

Zapojené v OTA env PO gen_fw_trailer (poradie v extra_scripts určuje poradie
post-akcií na firmware.hex):
    post:test_nrf-ota/gen_fw_trailer.py
    post:test_nrf-ota/gen_otapkg_hook.py

Vypnutie: zakomentuj riadok v variants/promicro/platformio.ini, alebo nastav
env premennú OTAPKG_SKIP=1.
"""
import os
import subprocess
import sys
from pathlib import Path

Import("env")  # type: ignore  # PlatformIO SCons kontext


def _post(source, target, env):  # noqa: ANN001
    if os.environ.get("OTAPKG_SKIP"):
        print("[otapkg] hook: OTAPKG_SKIP nastavené — preskakujem")
        return
    hexf = env.subst("$BUILD_DIR/${PROGNAME}.hex")
    script = Path(env.subst("$PROJECT_DIR")) / "test_nrf-ota" / "gen_otapkg.py"
    # názov zariadenia z env: "ProMicro_repeater_ota" → "promicro"
    device = env.subst("$PIOENV").split("_")[0].lower() or "device"
    try:
        r = subprocess.run([sys.executable, str(script), "--from-hex", hexf, "--device", device],
                           capture_output=True, text=True)
        for ln in ((r.stdout or "") + (r.stderr or "")).splitlines():
            if ln.startswith(("[otapkg]", "[export]")):
                print(ln)
        if r.returncode != 0:
            print("[otapkg] hook: json nevygenerovaný (možno len 1 build v archíve) — OK")
    except Exception as e:  # noqa: BLE001
        print(f"[otapkg] hook chyba (nefatálne): {e}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", _post)  # type: ignore

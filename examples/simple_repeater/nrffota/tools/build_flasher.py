#!/usr/bin/env python3
"""
build_flasher.py — kompiluje nrffota/flasher/flasher.c + HPatchLite → nrffota/flasher_code.h

Použitie:
    python examples/simple_repeater/nrffota/tools/build_flasher.py

Výstup:
    nrffota/flasher_code.h — JEDEN board-agnostický blob (ARM Thumb2). App base
    (v6=0x26000 / v7=0x27000) sa odovzdáva RUNTIME (4. arg flasher_entry), nie
    compile-time — preto netreba per-board variant.

Závislosť:
    arm-none-eabi-gcc (z PlatformIO balíkov alebo systémový PATH)
    HPatchLite — vendorovaná lokálne v nrffota/hpatchlite/

Port z FK_lora-sniffer/tools/build_flasher.py — prispôsobené štruktúre nrffota
a MeshCore flash mape (flasher beží z 0xEB000, viď nrffota/flash_layout.h).
"""

import os
import sys
import shutil
import subprocess
import tempfile
from pathlib import Path

# ── Konfigurácia ──────────────────────────────────────────────────────
FLASHER_ADDR  = 0xEB000    # adresa kde beží flasher (info; reálne z flasher.ld)
SCRIPT_DIR    = Path(__file__).parent
NRFOTA_DIR    = SCRIPT_DIR.parent
FLASHER_SRC   = NRFOTA_DIR / "flasher" / "flasher.c"
# Streaming DEFLATE — puff_stream (z nrffota/, kompiluje sa aj do hlavného FW pre dry-run).
PUFF_SRC      = NRFOTA_DIR / "puff_stream.c"
PUFF_HDR      = NRFOTA_DIR / "puff_stream.h"
# Per-board flash adresy (freestanding-safe) — single source pre flasher aj FW.
FLASH_LAYOUT  = NRFOTA_DIR / "flash_layout.h"
FLASHER_LD    = NRFOTA_DIR / "flasher" / "flasher.ld"
# JEDEN board-agnostický flasher: app base (v6=0x26000 / v7=0x27000) sa neviaže
# compile-time, FW ho odovzdá flasheru RUNTIME ako 4. arg flasher_entry (viď
# s_app_base vo flasher.c). APP_FLASH_END (0xD4000) je board-nezávislé.
OUT_HEADER    = NRFOTA_DIR / "flasher_code.h"
HPATCH_DIR    = NRFOTA_DIR / "hpatchlite"
HPATCH_SRCS   = ["hpatch_lite.c"]
HPATCH_HDRS   = ["hpatch_lite.h", "hpatch_lite_types.h", "hpatch_lite_input_cache.h"]

# ── Hľadanie arm-none-eabi toolchain ─────────────────────────────────
def find_toolchain():
    username = os.environ.get("USERNAME", os.environ.get("USER", ""))
    # PLATFORMIO_CORE_DIR má prednosť (setup s core dir mimo HOME, napr. D:\FkDev\.platformio)
    core = os.environ.get("PLATFORMIO_CORE_DIR")
    pio_roots = [
        Path(core) / "packages" if core else None,
        Path.home() / ".platformio" / "packages",
        Path(f"C:/Users/{username}/.platformio/packages") if username else None,
    ]
    for root in pio_roots:
        if root is None or not root.exists():
            continue
        for tc in sorted(root.glob("toolchain-gccarmnoneeabi*/bin"), reverse=True):
            cc = tc / "arm-none-eabi-gcc.exe"
            if not cc.exists():
                cc = tc / "arm-none-eabi-gcc"
            if cc.exists():
                return str(tc / "arm-none-eabi")
    for cmd in ["arm-none-eabi-gcc", "arm-none-eabi-gcc.exe"]:
        try:
            r = subprocess.run([cmd, "--version"], capture_output=True)
            if r.returncode == 0:
                print(f"[toolchain] PATH: {cmd}")
                return "arm-none-eabi"
        except FileNotFoundError:
            pass
    return None

def run(cmd, desc):
    print(f"[run] {desc}")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"CHYBA ({' '.join(str(c) for c in cmd)}):")
        print(r.stderr or r.stdout)
        sys.exit(1)
    return r

def main():
    for f in (FLASHER_SRC, PUFF_SRC, FLASH_LAYOUT, FLASHER_LD):
        if not f.exists():
            print(f"CHYBA: {f} neexistuje"); sys.exit(1)
    if not (HPATCH_DIR / "hpatch_lite.h").exists():
        print(f"CHYBA: HPatchLite nenájdený v {HPATCH_DIR}"); sys.exit(1)
    print("[flasher] board-agnostický (app base runtime z FW)")

    tc = find_toolchain()
    if not tc:
        print("CHYBA: arm-none-eabi toolchain nenájdený.")
        print("Inštaluj PlatformIO (pio run raz) alebo arm-none-eabi-gcc do PATH.")
        sys.exit(1)
    print(f"[toolchain] {tc}-gcc")

    GCC  = tc + "-gcc"
    LD   = tc + "-gcc"   # gcc ako linker (zahrnie libgcc, libc automaticky)
    OCP  = tc + "-objcopy"
    DUMP = tc + "-objdump"

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)

        # Kopíruj HPatchLite, puff_stream, flash_layout, flasher do tmp (flat include)
        for fn in HPATCH_SRCS + HPATCH_HDRS:
            shutil.copy(HPATCH_DIR / fn, tmpdir / fn)
        shutil.copy(FLASHER_SRC, tmpdir / "flasher.c")
        shutil.copy(PUFF_SRC,    tmpdir / "puff_stream.c")
        shutil.copy(PUFF_HDR,    tmpdir / "puff_stream.h")
        shutil.copy(FLASH_LAYOUT, tmpdir / "flash_layout.h")

        cflags = [
            "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft",
            "-Os", "-fno-stack-protector",   # -Os: minimalizuj veľkosť (4kB limit)
            "-ffunction-sections", "-fdata-sections",
            "-ffreestanding", "-nostdlib",
            f"-I{tmpdir}",
            "-DFOTA_FLASHER_BUILD",   # zapne telá flasher.c/puff_stream.c/hpatch_lite.c
            "-DHPATCH_LITE_INCLUDE_DECOMPRESS=0",
            "-DNDEBUG",              # zakáže assert() v hpatch_lite.c → žiadny newlib I/O
            "-DFLASHER_DEBUG=0",     # produkcia: trace OFF (miesto), verify+DFU ON (bezpečnosť)
            # POZN.: žiadny -DBOARD_* — flasher nepoužíva APP_FLASH_START makro,
            # app base dostáva runtime (s_app_base). APP_FLASH_END je board-nezávislé.
        ]

        obj_hpatch  = tmpdir / "hpatch_lite.o"
        obj_puff    = tmpdir / "puff_stream.o"
        obj_flasher = tmpdir / "flasher.o"
        run([GCC] + cflags + ["-c", str(tmpdir / "hpatch_lite.c"), "-o", str(obj_hpatch)], "CC hpatch_lite.c")
        run([GCC] + cflags + ["-c", str(tmpdir / "puff_stream.c"), "-o", str(obj_puff)],    "CC puff_stream.c")
        run([GCC] + cflags + ["-c", str(tmpdir / "flasher.c"),     "-o", str(obj_flasher)], "CC flasher.c")

        elf_f = tmpdir / "flasher.elf"
        run([LD, "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft",
             "-nostdlib", "--specs=nosys.specs",
             "-T", str(FLASHER_LD),
             str(obj_hpatch), str(obj_puff), str(obj_flasher),
             "-lgcc", "-lc",
             "-Wl,--gc-sections",
             "-o", str(elf_f)],
            "LD flasher.elf")

        bin_f = tmpdir / "flasher.bin"
        run([OCP, "-O", "binary", "--only-section=.text", str(elf_f), str(bin_f)],
            "objcopy -> .bin")
        data = bin_f.read_bytes()

        try:
            r = subprocess.run([DUMP, "-d", str(elf_f)], capture_output=True, text=True)
            disasm = r.stdout if r.returncode == 0 else ""
        except FileNotFoundError:
            disasm = ""

    if not data:
        print("CHYBA: prázdny binárny výstup"); sys.exit(1)

    while len(data) % 4:
        data += b'\xff'

    # POZOR: 0xEB000-0xEC000 = flasher kód (4kB), 0xEC000-0xED000 = flash trace log.
    # Flasher kód NESMIE prekročiť 4096B, inak prepíše trace stránku.
    if len(data) > 4096:
        print(f"CHYBA: flasher.bin = {len(data)}B > 4096B — prepísal by trace stránku 0xEC000!")
        sys.exit(1)

    print(f"[OK] flasher.bin: {len(data)} bajtov ({len(data)*100//4096}% zo 4kB kód oblasti)")

    lines = [
        f"/* {OUT_HEADER.name} — AUTO-GENERATED by nrffota/tools/build_flasher.py",
        " * Flasher: ARM Thumb2, standalone, runs from 0xEB000 (MeshCore OTA mapa)",
        " * Includes HPatchLite in-place streaming patcher + NVMC writes.",
        " * Board-agnostic: app base (v6=0x26000 / v7=0x27000) passed at RUNTIME",
        " * as flasher_entry's 4th arg (FW reads it from the linker symbol).",
        " * Regenerate after modifying nrffota/flasher/flasher.c:",
        " *   python examples/simple_repeater/nrffota/tools/build_flasher.py",
        " */",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define FLASHER_CODE_SIZE {len(data)}u",
        "",
        "/* Array je const → uložený vo flash, nie v RAM */",
        "static const uint8_t flasher_code[FLASHER_CODE_SIZE] = {",
    ]
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines[-1] = lines[-1].rstrip(",")
    lines += ["};", ""]

    OUT_HEADER.write_text("\n".join(lines), encoding="utf-8")
    print(f"[OK] Zapísaný: {OUT_HEADER}")

    if disasm:
        print("\n--- Disassembly (prvých 60 riadkov) ---")
        print('\n'.join(disasm.split('\n')[:60]))

if __name__ == "__main__":
    main()

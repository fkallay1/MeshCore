#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Overi odkazy vo fkclaude/ dokumentacii (subor + kotva) a najde duplicitne nadpisy.

en: Verifies every relative markdown link inside fkclaude/ resolves to an existing
    file and, when it carries a #fragment, to an existing heading anchor.
sk: Overi, ze kazdy relativny markdown odkaz vo fkclaude/ ukazuje na existujuci
    subor a pri #kotve aj na existujuci nadpis.

Pouzitie:
    python fkclaude/tools/check_doc_links.py              # cely fkclaude/
    python fkclaude/tools/check_doc_links.py fcl_sess_summary.md

Kotva sa tvori rovnako ako v GitHube aj VS Code: text nadpisu na male pismena,
zahodi sa vsetko okrem pismen, cislic, znaciek, medzier, '-' a '_', medzery na '-'.
Diakritika ostava. Ukazky v spatnych apostrofoch a v ```blokoch``` sa ignoruju,
inak by hlavicka fcl_sess_summary.md hlasila falosnu chybu.
"""
import io
import os
import re
import sys
import unicodedata

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # .../fkclaude
KEEP = set("-_")


def anchor(text):
    """en: heading text -> anchor  sk: text nadpisu -> kotva"""
    text = re.sub(r"`([^`]*)`", r"\1", text)           # spatne apostrofy zmiznu
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)  # odkaz -> jeho text
    out = []
    for ch in text.strip().lower():
        cat = unicodedata.category(ch)
        if cat[0] in ("L", "N", "M") or ch in KEEP:
            out.append(ch)
        elif ch.isspace():
            out.append("-")
        # ostatne (zatvorky, ':', '—', '/', '.') sa zahodia
    return "".join(out)


def read(path):
    with io.open(path, encoding="utf-8") as fh:
        return fh.read()


def strip_code(md):
    """en: blank out fenced code blocks  sk: vyprazdni ```bloky```"""
    out, fenced = [], False
    for line in md.split("\n"):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            out.append("")
            continue
        out.append("" if fenced else line)
    return "\n".join(out)


def headings(path):
    """en: {anchor: [heading texts]}  sk: kotva -> zoznam nadpisov (na duplicity)"""
    found = {}
    for line in strip_code(read(path)).split("\n"):
        m = re.match(r"^(#{1,6})\s+(.*?)\s*$", line)
        if m:
            found.setdefault(anchor(m.group(2)), []).append(m.group(2))
    return found


LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")

def check(md_files):
    problems = []
    cache = {}
    for src in md_files:
        body = strip_code(read(src))
        body = re.sub(r"`[^`\n]*`", "", body)  # inline ukazky formatu von
        for target in LINK.findall(body):
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            path_part, _, frag = target.partition("#")
            if path_part:
                dest = os.path.normpath(os.path.join(os.path.dirname(src), path_part))
            else:
                dest = src  # odkaz v ramci toho isteho suboru
            rel = os.path.relpath(src, ROOT)
            if not os.path.exists(dest):
                problems.append("%s -> %s : SUBOR NEEXISTUJE" % (rel, target))
                continue
            if not frag or os.path.isdir(dest):
                continue
            if not dest.lower().endswith(".md"):
                continue  # #Lriadok v zdrojaku sa neoveruje
            if dest not in cache:
                cache[dest] = headings(dest)
            if frag not in cache[dest]:
                near = [a for a in cache[dest] if frag[:18] and frag[:18] in a]
                hint = ("  (blizke: %s)" % ", ".join(near[:3])) if near else ""
                problems.append("%s -> %s : KOTVA NEEXISTUJE%s" % (rel, target, hint))
    return problems


def duplicates(md_files):
    problems = []
    for src in md_files:
        for anc, texts in headings(src).items():
            if len(texts) > 1:
                problems.append("%s : DUPLICITNY NADPIS #%s -> %s"
                                % (os.path.relpath(src, ROOT), anc, " | ".join(texts)))
    return problems


def collect(args):
    if args:
        return [a if os.path.isabs(a) else os.path.join(ROOT, a) for a in args]
    files = []
    for dirpath, _dirs, names in os.walk(ROOT):
        for n in names:
            if n.lower().endswith(".md"):
                files.append(os.path.join(dirpath, n))
    return sorted(files)


def main():
    files = collect(sys.argv[1:])
    problems = check(files) + duplicates(files)
    for p in problems:
        print(p)
    print("--- %d suborov, %d problemov" % (len(files), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

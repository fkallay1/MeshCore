#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""strip_lang_comments.py — odstráni jednu jazykovú vrstvu z dvojjazyčných komentárov.

Konvencia v kóde (viď fkclaude/fcl_readme_tech_nrf-fota.md):
    //en: English comment (canonical, goes upstream)
    //sk: Slovenský komentár (pracovná vetva)
Značka je hneď za komentárovým leaderom, bez medzery: `//en:`, `//sk:`
(python `#en:`, ini `;en:`). Značkujú sa LEN riadkové komentáre (nie /* */).

Použitie:
    python strip_lang_comments.py --keep en [--dry-run] cesty...
    python strip_lang_comments.py --keep sk examples/simple_repeater

  --keep en  → zmaže //sk: riadky, z //en: spraví obyčajný // komentár (pre PR)
  --keep sk  → zmaže //en: riadky, z //sk: spraví obyčajný // komentár
  Neoznačené komentáre (štrukturálne značky, #endif // X, third-party) nemení.
  Trailing varianta (`kód  //en: text`): keep → `kód  // text`, drop → značka
  aj text sa odrežú (riadok ostáva); ak by ostal prázdny riadok, zmaže sa celý.

POZOR: prepisuje súbory na mieste — púšťaj na čistej PR vetve. Značky vnútri
string literálov nerozlišuje (v našom kóde sa nevyskytujú).
"""
import argparse
import re
import sys
from pathlib import Path

# prípona → komentárový leader
LEADERS = {
    '.c': '//', '.cpp': '//', '.h': '//', '.hpp': '//', '.ld': '//',
    '.py': '#',
    '.ini': ';',
}
LANGS = ('en', 'sk')


def process_text(text, leader, keep):
    drop = 'sk' if keep == 'en' else 'en'
    esc = re.escape(leader)
    # celý riadok: len whitespace pred značkou
    full_keep = re.compile(r'^(\s*)' + esc + keep + r':( ?)')
    full_drop = re.compile(r'^\s*' + esc + drop + r':')
    # trailing: značka kdekoľvek za kódom
    trail_keep = re.compile(esc + keep + r':( ?)')
    trail_drop = re.compile(r'\s*' + esc + drop + r':.*$')

    out = []
    changed = 0
    for line in text.splitlines(keepends=True):
        eol = line[len(line.rstrip('\r\n')):]
        body = line.rstrip('\r\n')
        if full_drop.match(body):
            changed += 1
            continue                                    # zmaž celý riadok
        m = full_keep.match(body)
        if m:
            body = full_keep.sub(r'\1' + leader + ' ', body, count=1)
            changed += 1
        else:
            if trail_drop.search(body):
                body = trail_drop.sub('', body)
                changed += 1
                if body.strip() == '':
                    continue                            # ostal prázdny → zmaž
            if trail_keep.search(body):
                body = trail_keep.sub(leader + ' ', body, count=1)
                changed += 1
        out.append(body + eol)
    return ''.join(out), changed


def iter_files(paths):
    for p in paths:
        p = Path(p)
        if p.is_dir():
            for f in sorted(p.rglob('*')):
                if f.suffix in LEADERS and f.is_file():
                    # third-party — nedotýkať sa
                    if 'hpatchlite' in f.parts:
                        continue
                    yield f
        elif p.is_file():
            yield p
        else:
            print(f'[skip] neexistuje: {p}', file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--keep', choices=LANGS, required=True,
                    help='ktorý jazyk PONECHAŤ (druhý sa zmaže)')
    ap.add_argument('--dry-run', action='store_true', help='len vypíš štatistiku')
    ap.add_argument('paths', nargs='+', help='súbory alebo adresáre (rekurzívne)')
    args = ap.parse_args()

    total = 0
    for f in iter_files(args.paths):
        leader = LEADERS.get(f.suffix)
        if not leader:
            continue
        text = f.read_text(encoding='utf-8')
        new, changed = process_text(text, leader, args.keep)
        if changed:
            total += changed
            print(f'{f}: {changed} zmien')
            if not args.dry_run:
                f.write_text(new, encoding='utf-8', newline='')
    print(f'{"[dry-run] " if args.dry_run else ""}spolu {total} zmien (keep={args.keep})')


if __name__ == '__main__':
    main()

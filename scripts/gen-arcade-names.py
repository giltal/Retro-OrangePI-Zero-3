#!/usr/bin/env python3
"""
Generate arcade short-name -> full-title tables for the launcher.

Arcade ROMs are named after the emulator's driver ("mslug.zip"); the real
title lives only in that emulator's driver list. So the tables are extracted
from the SOURCE of the exact core versions we build, and can never disagree
with what the core itself knows.

    gen-arcade-names.py <fbalpha2012-build-dir> <mame2003plus-build-dir> <outdir>

Writes <outdir>/fbalpha2012.txt and <outdir>/mame2003plus.txt, one entry per
line, sorted by short name:

    mslug=Metal Slug - Super Vehicle-001
    neogeo=*Neo Geo                        <- '*' marks a BIOS / non-game set

The launcher (rom_display_name / rom_is_hidden) uses these when a ROM folder's
own gamenames.txt has no entry, and hides the '*' sets from game lists.
Either build dir may be "-" to skip that core.
"""
import os
import re
import sys

STR = r'"((?:[^"\\]|\\.)*)"'          # a C string literal, escapes kept
OPT = r'(?:NULL|' + STR + r')'         # NULL or a C string literal

# struct BurnDriver[D] BurnDrvxxx = { "short", parent, board, sample, "year",
#                                     "Full Name\0", ...
FBA_RE = re.compile(
    r'struct\s+BurnDriver\w*\s+\w+\s*=\s*\{\s*' + STR + r'\s*,\s*' + OPT +
    r'\s*,\s*' + OPT + r'\s*,\s*' + OPT + r'\s*,\s*' + OPT + r'\s*,\s*' + STR)

# GAME( 1996, mslug, neogeo, ..., "Nazca", "Metal Slug - ...", flags ... )
# (GAME, GAMEX, GAMEB, GAMEBX; always single-line in mame2003-plus)
MAME_RE = re.compile(r'^\s*GAME[BX]*\(\s*[^,]+,\s*(\w+)\s*,')


def c_unescape(s):
    s = s.split('\\0')[0]              # FBA terminates names with "\0"
    return s.replace('\\"', '"').replace('\\\\', '\\').strip()


# Neo Geo cartridge catalogue numbers, "(NGM-2410) (NGH-2410)", "(ALM-001)":
# the cabinet (MVS) and home (AES) part numbers. Noise on a game list, unlike
# the variant tags ("(set 1)", "(bootleg)", "(World)"), which are kept.
CATNO_RE = re.compile(r'\s*\((?:[A-Z]{3}-\d+[A-Z]?)(?:\s*/\s*[A-Z]{3}-\d+[A-Z]?)*\)')


def fba(build_dir):
    root = os.path.join(build_dir, 'svn-current', 'trunk', 'src', 'burn', 'drv')
    out = {}
    for dirpath, _, files in os.walk(root):
        for fn in files:
            if not fn.endswith('.cpp'):
                continue
            text = open(os.path.join(dirpath, fn), encoding='latin-1').read()
            for m in FBA_RE.finditer(text):
                short = m.group(1)
                name = CATNO_RE.sub('', c_unescape(m.group(6))).strip()
                end = text.find('};', m.end())
                bios = 'BDF_BOARDROM' in text[m.end():end]
                if not (short and name):
                    continue
                # A short name can be defined more than once (FBA has several
                # "neogeo" entries: the BIOS-only board and system variants).
                # BIOS if ANY definition is: the first version of this script
                # let a later non-BIOS entry overwrite the BIOS mark.
                prev = out.get(short, '')
                if bios or prev.startswith('*'):
                    out[short] = '*' + (prev.lstrip('*') if prev else name)
                elif not prev:
                    out[short] = name
    return out


def mame(build_dir):
    root = os.path.join(build_dir, 'src', 'drivers')
    out = {}
    for fn in sorted(os.listdir(root)):
        if not fn.endswith('.c'):
            continue
        for line in open(os.path.join(root, fn), encoding='latin-1'):
            m = MAME_RE.match(line)
            if not m:
                continue
            strs = re.findall(STR, line)
            if len(strs) < 2:
                continue
            name = c_unescape(strs[1])     # [0] = company, [1] = full name
            bios = 'NOT_A_DRIVER' in line
            out[m.group(1)] = ('*' if bios else '') + name
    return out


def write(path, table):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        for k in sorted(table):
            f.write(f'{k}={table[k]}\n')
    print(f'gen-arcade-names: {len(table)} entries '
          f'({sum(v.startswith("*") for v in table.values())} BIOS) -> {path}')


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    fba_dir, mame_dir, outdir = sys.argv[1:]
    os.makedirs(outdir, exist_ok=True)
    if fba_dir != '-':
        write(os.path.join(outdir, 'fbalpha2012.txt'), fba(fba_dir))
    if mame_dir != '-':
        write(os.path.join(outdir, 'mame2003plus.txt'), mame(mame_dir))


if __name__ == '__main__':
    main()

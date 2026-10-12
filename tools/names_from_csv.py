#!/usr/bin/env python3
"""Function names from a CSV, as work/names.json for run_lift.py.

    py -3 tools/names_from_csv.py symbols.csv [-o work/names.json]

The CSV's first column is the address in hex and its second the name; a
header row and any further columns are ignored. Names that are not C
identifiers, or that a disassembler made up (FUN_, sub_), are skipped, and a
name used twice gets its address appended.
"""
import argparse
import json
import os
import re

IDENT = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')
MADE_UP = re.compile(r'^(FUN_|sub_|thunk_FUN_|LAB_|caseD_)', re.I)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('csv')
    ap.add_argument('-o', '--out', default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), 'work', 'names.json'))
    a = ap.parse_args()
    names, seen, skipped = {}, {}, 0
    for line in open(a.csv, encoding='utf-8', errors='replace'):
        cols = line.rstrip('\r\n').split(',', 2)
        if len(cols) < 2:
            continue
        try:
            va = int(cols[0], 16)
        except ValueError:
            continue                              # the header row
        name = cols[1].strip()
        if not IDENT.match(name) or MADE_UP.match(name):
            skipped += 1
            continue
        if name in seen:
            name = '%s_%08X' % (name, va)
        seen[name] = va
        names['0x%08X' % va] = name
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump(names, open(a.out, 'w'), indent=0, sort_keys=True)
    print('%d names -> %s (%d rows skipped)' % (len(names), a.out, skipped))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""What a whole-archive mod actually changes, as loose files.

    py -3 tools/archive_diff.py <clean game folder> <modded folder> <mod folder to write>

A mod for this game has usually been a replacement ED6_DTnn.dat/.dir pair,
though it changes only a few of the archive's entries. This compares each
archive in the modded folder with the clean game's, entry by entry, and
writes every entry that differs (or is new) into

    <mod folder>/packed/ED6_DTnn/<name>

exactly as the archive holds it, which is how the port's mod loader takes a
packed entry (src/mods/vfs.c). A mod made this way no longer replaces an
archive, so mods that touch the same archive can be installed together.

The modded files may still carry their download names (ED6_DT04EVO.dat,
ED6_DT00FCEVO.dir): the archive number is read from the name's start.
"""
import argparse
import os
import re
import struct
import sys

ENTRY = 36
NAME = re.compile(r'^ED6_DT([0-9A-Fa-f]{2})\w*\.dir$', re.I)


def read_dir(path):
    """[(name, size, offset)] from a .dir file; names as stored (12 bytes)."""
    data = open(path, 'rb').read()
    if data[:7] != b'LB DIR\x1a':
        raise ValueError('%s is not a .dir file' % path)
    count = struct.unpack_from('<I', data, 8)[0]
    out = []
    for i in range(count):
        e = data[16 + ENTRY * i:16 + ENTRY * (i + 1)]
        if len(e) < ENTRY:
            break
        size, = struct.unpack_from('<I', e, 0x10)
        offset, = struct.unpack_from('<I', e, 0x20)
        out.append((e[:12], size, offset))
    return out


def loose_name(stored):
    """b'H_KAO000._CH' -> 'h_kao000._ch'; b'T0310   ._SN' -> 't0310._sn'."""
    base, ext = stored[:8].decode('ascii').rstrip(' '), stored[8:].decode('ascii').rstrip(' ')
    return (base + ext).lower()


def archives(folder):
    """{archive number: (.dir path, .dat path)} for the pairs in a folder."""
    out = {}
    for fn in os.listdir(folder):
        m = NAME.match(fn)
        if not m:
            continue
        dat = os.path.join(folder, fn[:-4] + '.dat')
        if not os.path.exists(dat):
            dat = os.path.join(folder, fn[:-4] + '.DAT')
        if os.path.exists(dat):
            out[int(m.group(1), 16)] = (os.path.join(folder, fn), dat)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('clean')
    ap.add_argument('modded')
    ap.add_argument('out')
    ap.add_argument('--only', default='', help='comma-separated archive numbers in hex (default: all found)')
    a = ap.parse_args()
    clean, modded = archives(a.clean), archives(a.modded)
    only = {int(x, 16) for x in a.only.split(',') if x.strip()}
    total = 0
    for n in sorted(modded):
        if only and n not in only:
            continue
        mdir, mdat = modded[n]
        if n not in clean:
            print('ED6_DT%02X: not in the clean game (a new archive): copy the pair as it is' % n)
            continue
        cdir, cdat = clean[n]
        if os.path.getsize(mdat) == os.path.getsize(cdat) and open(mdir, 'rb').read() == open(cdir, 'rb').read():
            same = True
            with open(mdat, 'rb') as f, open(cdat, 'rb') as g:
                while same:
                    x, y = f.read(1 << 20), g.read(1 << 20)
                    same = x == y
                    if not x:
                        break
            if same:
                continue
        centries = {name: (size, off) for name, size, off in read_dir(cdir)}
        changed = new = 0
        with open(mdat, 'rb') as f, open(cdat, 'rb') as g:
            for name, size, off in read_dir(mdir):
                if not size or name.startswith(b'/'):
                    continue
                f.seek(off)
                payload = f.read(size)
                if name in centries:
                    csize, coff = centries[name]
                    if csize == size:
                        g.seek(coff)
                        if g.read(csize) == payload:
                            continue
                    changed += 1
                else:
                    new += 1
                folder = os.path.join(a.out, 'packed', 'ED6_DT%02X' % n)
                os.makedirs(folder, exist_ok=True)
                with open(os.path.join(folder, loose_name(name)), 'wb') as o:
                    o.write(payload)
        print('ED6_DT%02X: %d entries changed, %d new (of %d)' % (n, changed, new, len(centries)))
        total += changed + new
    print('%d files written under %s' % (total, a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())

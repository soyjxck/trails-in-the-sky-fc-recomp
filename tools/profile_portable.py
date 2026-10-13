#!/usr/bin/env python3
"""Names for a profile of the portable host (its --profile FILE).

    tools/profile_portable.py <program> <profile> [top=40] [function ...]

<program> is what was run, with its symbols: build-linux/trails-portable, or
for a profile from the Switch the trails.elf the NRO was made from (the NRO
itself has no names). Run it where the program was built: it asks nm for the
symbols (NM names another one; devkitA64's is tried for an .elf).

A sample is the address the game's thread was found at, followed by the
addresses it was called from, as far as they could be followed. On Linux the
samples are of processor time; on the Switch of the clock, so a thread that
waits is found where it waits. Three tables come out:

  where the samples landed   the program's functions, a library as one line
  library time, by who asked for samples outside the program (Linux: Mesa,
                             the C library), the nearest caller inside it
  under each function        samples in it or in anything it called, within
                             the callers recorded

and for each function named after the count, the chains of callers it was
found under.

Lifted functions are sub_<address>; everything else is the host's own, or on
the Switch a library's, which is linked in there.
"""
import bisect
import os
import subprocess
import sys


def symbols(prog):
    tools = [os.environ['NM']] if os.environ.get('NM') else ['nm']
    if prog.endswith('.elf'):
        tools.insert(0, os.path.join(os.environ.get('DEVKITPRO', '/opt/devkitpro'), 'devkitA64/bin/aarch64-none-elf-nm'))
    for nm in tools:
        try:
            out = subprocess.run([nm, '-n', '--defined-only', prog], capture_output=True, text=True).stdout
        except OSError:
            continue
        syms = []
        for line in out.splitlines():
            a = line.split()
            if len(a) == 3 and a[1] in 'tTwW':
                syms.append((int(a[0], 16), a[2]))
        if syms:
            return syms
    sys.exit('no symbols could be read from %s' % prog)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    prog, path = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    callers_of = sys.argv[4:]
    syms = symbols(prog)
    addrs = [a for a, _ in syms]
    me = os.path.basename(prog)
    base, maps, samples = None, [], []
    for line in open(path):
        if line.startswith('map '):
            f = line.split()
            lo, hi = (int(x, 16) for x in f[1].split('-'))
            name = os.path.basename(f[6]) if len(f) > 6 else '(nameless)'
            maps.append((lo, hi, name))
            if name == me:
                base = lo if base is None else min(base, lo)
        elif line.strip():
            samples.append([int(x, 16) for x in line.split()])
    if base is None:
        sys.exit('%s is not among the profile\'s maps: is it the program that was run?' % me)
    if not samples:
        sys.exit('the profile has no samples')

    def name_of(a):
        """(the function's name, or None outside the program; where it is)"""
        where = next((m for m in maps if m[0] <= a < m[1]), None)
        if where is None or where[2] != me:
            return None, where[2] if where else '(unmapped)'
        i = bisect.bisect_right(addrs, a - base) - 1
        return (syms[i][1] if i >= 0 else '?'), me

    flat, libs, under, asked = {}, {}, {}, {}
    for s in samples:
        names = [name_of(a) for a in s]
        fn, lib = names[0]
        if fn is None:
            libs[lib] = libs.get(lib, 0) + 1
            who = next((n for n, _ in names[1:] if n is not None), '(not known)')
            asked[(who, lib)] = asked.get((who, lib), 0) + 1
        else:
            flat[fn] = flat.get(fn, 0) + 1
        for n in set(n for n, _ in names if n is not None):
            under[n] = under.get(n, 0) + 1
    n = len(samples)
    lifted = sum(c for name, c in flat.items() if name.startswith('sub_') or name.startswith('fc_orig_'))
    rest = sum(flat.values()) - lifted
    print('%d samples: %.1f%% in lifted functions, %.1f%% in the rest of the program, %.1f%% outside it'
          % (n, 100.0 * lifted / n, 100.0 * rest / n, 100.0 * sum(libs.values()) / n))
    if libs:
        print('outside: ' + ', '.join('%s %.1f%%' % (k, 100.0 * v / n) for k, v in sorted(libs.items(), key=lambda x: -x[1])[:8]))
    print('\nwhere the samples landed')
    run = 0
    for name, c in sorted(flat.items(), key=lambda x: -x[1])[:top]:
        run += c
        print('%6.2f%%  %6.2f%%  %s' % (100.0 * c / n, 100.0 * run / n, name))
    if asked:
        print('\nlibrary time, by who asked')
        for (who, lib), c in sorted(asked.items(), key=lambda x: -x[1])[:top]:
            print('%6.2f%%  %-40s %s' % (100.0 * c / n, who, lib))
    print('\nunder each function')
    for name, c in sorted(under.items(), key=lambda x: -x[1])[:top]:
        print('%6.2f%%  %s' % (100.0 * c / n, name))
    for want in callers_of:
        chains = {}
        for s in samples:
            names = [name_of(a)[0] or '(outside)' for a in s]
            if names[0] == want:
                key = ' < '.join(names[1:4])
                chains[key] = chains.get(key, 0) + 1
        print('\n%s was found, called from' % want)
        for key, c in sorted(chains.items(), key=lambda x: -x[1])[:12]:
            print('%6.2f%%  %s' % (100.0 * c / n, key))


if __name__ == '__main__':
    main()

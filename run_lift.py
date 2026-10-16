#!/usr/bin/env python3
"""Trails in the Sky FC lift driver: work/functions.json -> src/recomp/gen/.

The driver sp00nznet's native32 games share (tiberiumwars-recomp's and
ut99-recomp's run_lift.py), on pcrecomp's `tools/lift/generate.py` + `lift32`:

* **Extents by walking the branches** (`generate.true_extent`), capped by
  reach rather than the catalog's clamp.
* **Closure-limited lifting** (`generate.closure`) for bisecting a bad lift:
  everything outside the closure gets a stub that aborts naming itself.
* **C++ exceptions** (`eh32.resume_points`): a function that catches gets a
  landing pad and a label where each of its catch blocks continues. The game
  leaves its loops by throwing (PumpMessages throws an int on WM_QUIT).

Every vtable slot RTTI names is an entry (work/rtti_seeds.json): a method that
is only ever called virtually is named by no call anywhere in the image.

    py -3 run_lift.py --all
    py -3 run_lift.py --max 2000
    py -3 run_lift.py --roots 0x004B7280
"""
import argparse
import json
import os
import re
import shutil
import struct
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
# PCRECOMP picks another toolkit checkout, the same knob CMakeLists.txt has:
# the lifter and the runtime header must come from the same tree.
_TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, 'pcrecomp')), 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32          # noqa: E402
from eh32 import resume_points                             # noqa: E402
from generate import (EXTENT_REACH, closure, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                  # noqa: E402
from pe_analyze import analyze_pe, build_iat_map           # noqa: E402

GAME = os.path.join(_HERE, 'Trails in the Sky FC')
EXE = os.path.join(GAME, 'ed6_win_DX9.exe')
EXE_STAMP = 0x632CA5C5   # ed6_win_DX9.exe's PE timestamp: Steam's 2022-09-22 build (docs/RECON.md)
CATALOG = os.path.join(_HERE, 'work', 'functions.json')
SEEDS = os.path.join(_HERE, 'work', 'rtti_seeds.json')
NAMES = os.path.join(_HERE, 'work', 'names.json')
OUT = os.path.join(_HERE, 'src', 'recomp', 'gen')
STATS = os.path.join(_HERE, 'work', 'lift_stats.json')

# Entries a run finds that the catalog does not have (an unresolved ICALL
# names its target), so a clean checkout lifts them too. None so far: the
# catalog is seeded with config/function_starts.json (docs/bringup.md, 8).
RUN_SEEDS = []

# Functions whose body is the host's instead of the lift: the host defines
# fc_hook_XXXXXXXX(void) and it runs like an import shim (src/runtime/host.c).
#   0x004FD597  _CxxThrowException. The C runtime is linked into the exe, so
#               a throw is not an import native32 can answer by name: it is
#               this function, which would raise on the host's SEH chain.
#   0x00504ABF, 0x00504A01, 0x00522510, 0x004FE110
#               _isnan, _finite, the float to 64-bit integer helper and
#               memset: C runtime functions that are at the top of a profile
#               as lifted SSE2 code, and a line each as what they mean
#               (src/common/crt.c).
HOOKS = {0x004FD597, 0x00504ABF, 0x00504A01, 0x00522510, 0x004FE110}

# Functions the host wraps: the lift is emitted as fc_orig_XXXXXXXX and the
# function itself calls the host's fc_wrap_XXXXXXXX(void), which decides
# whether and when to run the original (src/runtime/host.c).
#   0x004802C0  Input_Poll: the buttons held, in edx:eax. A play script
#               (--script) answers instead of the keyboard.
#   0x004C11A0  Clock_Update: the frame limiter is in it, as a loop that
#               spins on Sleep(0). The host waits properly first.
#   0x00477580  ReadDirFiles and
#   0x00477180  ArchiveReadRaw: mods' loose files stand in for archive
#               entries (src/mods/vfs.c).
#   0x004886D0  MoviePlay (the file's name in ecx): DirectShow on Windows;
#               a host without it plays the file itself (src/portable/movie.c)
#               and leaves the game's movie state at "over".
#   0x004459B0  collision tests, called by the thousand a frame and nearly
#   0x00442820  always answering no, which the host answers itself; the
#   0x00446D60  originals run for the rest (src/common/hot.c): a segment and
#               a plane, a segment and a shape of twelve faces, a raycast
#               down onto a triangle.
#   0x00467740  a triangle the game sorts by depth and draws itself: done by
#               the host whole where D3DX is the host's own.
WRAPS = {0x004802C0, 0x004C11A0, 0x00477580, 0x00477180, 0x004886D0, 0x004459B0, 0x00442820, 0x00446D60,
         0x00467740}

# Additions to the game itself: C run after the instruction at the address.
# The lift stays faithful; a patch is for what the original cannot have.
#
# Voices (SoraVoice's, which hooks the Windows game at the same places; the
# hosts define the three functions, src/portable/voice.c):
#   0x004E191C  the window's text reader has found a '#', the start of a
#               command, at esi: a voice tag is '#', digits, 'v'.
#   0x004E2EC4  the typing beep is about to be played: not over a voice.
#   0x004E2DDD  the click as the player turns the page: the voice stops.
PATCHES = {
    0x004E191C: '{ void fc_voice_tag(uint32_t); fc_voice_tag(esi); }',
    0x004E2EC4: '{ int fc_voice_playing(void); if (fc_voice_playing()) goto L_004E2EE2; }',
    0x004E2DDD: '{ void fc_voice_page(void); fc_voice_page(); }',
}


# Calls done in place. A call into lifted code hands every register over and
# takes it back, which for a function of three instructions that is called
# from a loop is most of the cost (docs/bringup.md, 23). A call to one of
# these is replaced, where it is made, by C that leaves in eax and edx what the
# function leaves there; the functions themselves stay (HOOKS above) for
# whatever reaches them some other way.
#   0x00504ABF  _isnan(double): the argument is at the top of the stack
#   0x00504A01  _finite(double)
#   0x00522510  (__int64)float: xmm0 in, edx:eax out
INLINES = {
    0x00504ABF: '{ uint32_t _hi = MEM32(esp + 4); eax = (_hi & 0x7FF00000u) == 0x7FF00000u && ((_hi & 0x000FFFFFu) | MEM32(esp)) != 0; }',
    0x00504A01: 'eax = (MEM32(esp + 4) & 0x7FF00000u) != 0x7FF00000u;',
    0x00522510: '{ float _f = _xmm[0].f32[0]; int64_t _v = INT64_MIN; '
                'if (_f >= -9223372036854775808.0f && _f < 9223372036854775808.0f) _v = (int64_t)_f; '
                'eax = (uint32_t)(uint64_t)_v; edx = (uint32_t)((uint64_t)_v >> 32); }',
}
_CALL = re.compile(r'RECOMP_CALL\(\w+\); (/\* 0x[0-9A-F]{8}: call 0x([0-9a-f]+) \*/)\n[ \t]*RECOMP_FLAGS_IN\(\);')


def inline_calls(body):
    """Replace each call to a function in INLINES, and the flags it would hand back, by its C."""
    def one(m):
        c = INLINES.get(int(m.group(2), 16))
        return '%s %s' % (c, m.group(1)) if c else m.group(0)
    return _CALL.sub(one, body)


def apply_patches(body, patches):
    """Add each patch's C after the lines that lift its instruction.

    Some instructions lift to a block over several lines (idiv's `{ ... }`):
    the patch goes after the block closes, not inside it."""
    for va, c in patches.items():
        tag = '/* 0x%08X:' % va
        k = body.find(tag)
        if k != -1:
            s = body.rfind('\n', 0, k) + 1
            e = body.index('\n', k)
            depth = body[s:e].count('{') - body[s:e].count('}')
            while depth > 0:
                s, e = e + 1, body.index('\n', e + 1)
                depth += body[s:e].count('{') - body[s:e].count('}')
            body = body[:e + 1] + '    ' + c + '\n' + body[e + 1:]
    return body


def exe_stamp(path):
    """The PE header's TimeDateStamp: which build of the game an exe is."""
    with open(path, 'rb') as f:
        head = f.read(4096)
    nt = int.from_bytes(head[0x3C:0x40], 'little')
    return int.from_bytes(head[nt + 8:nt + 12], 'little')


def sync_dir(new, old):
    """Make `old` equal to `new`, touching only the files that differ; removes `new`."""
    os.makedirs(old, exist_ok=True)
    changed = 0
    for fn in os.listdir(old):
        if not os.path.exists(os.path.join(new, fn)):
            os.remove(os.path.join(old, fn))
    for fn in os.listdir(new):
        a, b = os.path.join(new, fn), os.path.join(old, fn)
        if not os.path.exists(b) or open(a, 'rb').read() != open(b, 'rb').read():
            os.replace(a, b)
            changed += 1
    shutil.rmtree(new, ignore_errors=True)
    return changed


def mapped_reader(path):
    """read(va, n) over the image as the loader lays it out, or None."""
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    nsec, optsize = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    base = struct.unpack_from('<I', data, pe + 24 + 28)[0]
    secs = []
    for i in range(nsec):
        vsize, rva, rsize, roff = struct.unpack_from('<IIII', data, pe + 24 + optsize + 40 * i + 8)
        secs.append((base + rva, min(vsize, rsize) if vsize else rsize, roff))

    def read(va, n):
        for lo, size, roff in secs:
            if lo <= va < lo + size:
                return data[roff + va - lo:roff + min(va - lo + n, size)]
        return None
    return read


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=EXE)
    ap.add_argument('--catalog', default=CATALOG)
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--roots', default='', help='comma-separated extra root VAs')
    ap.add_argument('--max', type=int, default=3000, help='closure size cap')
    ap.add_argument('--all', action='store_true', help='lift every function')
    ap.add_argument('--virtual', action='store_true',
                    help='root the closure at every RTTI vtable method too')
    ap.add_argument('--split', type=int, default=250, help='functions per .c file')
    # More entries a run found, on top of RUN_SEEDS (seed_from_log.py JSON).
    ap.add_argument('--seeds', default=os.path.join(_HERE, 'work', 'run_seeds.json'),
                    help='seed_from_log.py JSON of unresolved targets from runs')
    ap.add_argument('--no-names', action='store_true', help='keep the sub_XXXXXXXX names')
    args = ap.parse_args()
    if not os.path.exists(args.catalog):
        sys.exit('no catalog at %s -- run disasm32.py first (README, Step by step)' % args.catalog)

    # The hooks and patches are at this build's addresses.
    stamp = exe_stamp(args.exe)
    if stamp != EXE_STAMP:
        sys.exit('%s is not the build this project supports: its PE timestamp is 0x%08X, '
                 'the Steam release\'s ed6_win_DX9.exe is 0x%08X (docs/RECON.md).'
                 % (args.exe, stamp, EXE_STAMP))
    info = analyze_pe(args.exe)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    print('[*] base=0x%08X code=0x%08X-0x%08X IAT=%d' % (info.image_base, cs, ce, len(iat)))

    cat = json.load(open(args.catalog))
    byaddr = {f['address']: f for f in cat['functions'] if cs <= f['address'] < ce}
    print('[*] catalog: %d functions inside .text' % len(byaddr))

    # Vtable slots. Bound each by the next known entry: a slot landing mid-code
    # handed `ce` makes the extent walk descend the whole of .text.
    want = {a for a in RUN_SEEDS if cs <= a < ce and a not in byaddr}
    for path in [SEEDS] + ([args.seeds] if args.seeds else []):
        if os.path.exists(path):
            want |= {e['address'] for e in json.load(open(path))
                     if cs <= e['address'] < ce and e['address'] not in byaddr}
    known = sorted(set(byaddr) | want)
    nxt = {a: (known[i + 1] if i + 1 < len(known) else ce) for i, a in enumerate(known)}
    for a in want:
        byaddr[a] = {'address': a, 'end': min(nxt[a], a + 0x100), 'calls_to': [],
                     'entry_kind': 'start'}
    print('[*] vtable slots and run seeds not in the catalog: %d injected' % len(want))

    entry = info.image_base + info.entry_point_rva
    roots = [entry] + [int(x, 0) for x in args.roots.split(',') if x.strip()]
    if args.virtual and os.path.exists(SEEDS):
        roots += sorted(e['address'] for e in json.load(open(SEEDS)))
    if args.all:
        chosen = set(byaddr)
    else:
        chosen = set(closure(byaddr, roots, args.max))
    print('[*] lifting %d of %d functions (%s)'
          % (len(chosen), len(byaddr), 'all' if args.all else 'closure, cap %d' % args.max))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    text = [s for s in info.sections if s.name == '.text'][0]
    code = open(args.exe, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]
    read = mapped_reader(args.exe)      # the EH tables are in .rdata

    # Known targets become RECOMP_CALL; anything else a decoded `call` names
    # becomes RECOMP_ICALL, which reports at run time instead of failing the build.
    # precise carry: adc/sbb take their carry from the flag state that set it
    # (64-bit adds and subtracts; off by default in lift32 only for Fury3).
    patches = dict(PATCHES)
    lifter = Lifter(iat_map=iat, lifted=set(byaddr), precise_carry=True, precise_sbb=True)
    # Lifted into a scratch folder; at the end only the files that changed are
    # replaced (and stale chunks removed), so a build after a small change to
    # the lift compiles the chunks it altered, not all of them.
    final_out, args.out = args.out, args.out.rstrip('\\/') + '.new'
    shutil.rmtree(args.out, ignore_errors=True)
    os.makedirs(args.out)
    entries, chunk, idx, errors, dirty, ehs, unimpl = [], [], 0, 0, 0, 0, {}
    ordered = sorted(byaddr)
    starts = {a for a in ordered if byaddr[a].get('entry_kind') != 'alias'}
    t_split = time.time()
    splits = find_splits(md, code, cs, ce, starts)
    starts -= splits          # still dispatchable; just not a wall inside its parent
    print('[*] split entries (the middle of the function before them): %d, %.0fs'
          % (len(splits), time.time() - t_split))

    # Where a function resumes after its own catch blocks. Nothing branches
    # there (the catch funclet returns the address), so the catalog often
    # starts a function at it and the parent's body stops short: the resume
    # point is not a wall, and it is walked as a root of its parent's body.
    eh_at = {}
    for a in byaddr:
        r = resume_points(md, read, list(md.disasm(read(a, 64) or b'', a))[:12])
        if r:
            eh_at[a] = r
    starts -= set().union(*eh_at.values()) if eh_at else set()
    print('[*] functions with catch blocks: %d' % len(eh_at))
    t0 = time.time()

    def flush(force=False):
        nonlocal chunk, idx
        if chunk and (force or len(chunk) >= args.split):
            write_chunk(args.out, idx, chunk)
            idx += 1
            chunk = []

    def lift_one(addr, name, end, reached):
        nonlocal errors, ehs

        if addr in HOOKS:
            chunk.append(('extern void fc_hook_%08X(void);\nvoid %s(void) { fc_hook_%08X(); }\n'
                          % (addr, name, addr), addr, name))
            entries.append((addr, name))
            return
        try:
            lo = min(reached) if reached else addr   # a chunk can sit below the entry
            insns, leaders = (linear_disassemble_function(md, code, cs, lo, end, reached=reached)
                              if end > addr else ([], None))
            if leaders is not None:
                leaders.add(addr)
            eh = eh_at.get(addr)
            ehs += bool(eh)
            inner = 'fc_orig_%08X' % addr if addr in WRAPS else name
            body = (lift_function_linear(lifter, inner, insns, leaders, addr, eh_resume=eh) if insns
                    else 'void %s(void) { }\n' % inner)
            body = apply_patches(body, {va: c for va, c in patches.items() if reached and va in reached})
            body = inline_calls(body)
            if addr in WRAPS:
                body += ('\nextern void fc_wrap_%08X(void);\nvoid %s(void) { fc_wrap_%08X(); }\n'
                         % (addr, name, addr))
        except Exception as e:                      # noqa: BLE001 -- counted, not hidden
            body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (name, e, name)
            errors += 1
        for m in re.findall(r'UNIMPLEMENTED: (\w+)', body):   # lifted as a no-op: says so
            unimpl[m] = unimpl.get(m, 0) + 1
        chunk.append((body, addr, name))
        entries.append((addr, name))
        if len(chunk) >= args.split:
            flush()
            print('[*]   %d/%d (%d err)' % (len(entries), len(chosen), errors), flush=True)

    # A direct branch that leaves a body backward to an address nothing
    # catalogued is a tail call the catalog missed, or a jump into shared code
    # in a neighbour; a direct call can name one too. Either way the target
    # has to be dispatchable, or the RECOMP_ITAIL cannot resolve. So each
    # round's outside targets become entries and are lifted in the next round,
    # until none are new.
    todo, added = sorted(chosen), 0
    while todo:
        outside = set()
        for addr in todo:
            name = 'sub_%08X' % addr
            reached, behind, called = set(), set(), set()
            end, clean = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                     reached=reached, behind=behind, called=called)
            for r in eh_at.get(addr, ()):
                if cs <= r < ce:
                    e2, _ = true_extent(md, code, cs, r, min(r + EXTENT_REACH, ce), starts,
                                        reached=reached, behind=behind, called=called)
                    end = max(end, e2)
            outside |= {t for t in behind if t not in reached} | called
            dirty += not clean
            lift_one(addr, name, end, reached)
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr)
        for t in todo:
            byaddr[t] = {'address': t, 'end': ce, 'calls_to': [], 'entry_kind': 'start'}
            chosen.add(t)
        added += len(todo)
    print('[*] branch and call targets outside the catalog, added as entries: %d' % added)

    stubs = [a for a in ordered if a not in chosen]
    for a in stubs:
        name = 'sub_%08X' % a
        chunk.append(('void %s(void) { RECOMP_NOT_LIFTED(0x%08Xu); }\n' % (name, a), a, name))
        entries.append((a, name))
        flush()
    flush(force=True)

    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* ed6_win_DX9.exe - AUTO-GENERATED by run_lift.py */\n#pragma once\n'
                '#include <stdint.h>\n\n'
                'void recomp_not_lifted(uint32_t va);\n'
                '#define RECOMP_NOT_LIFTED(va) recomp_not_lifted(va)\n\n')
        for a, n in entries:
            f.write('void %s(void);\n' % n)
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* ed6_win_DX9.exe - AUTO-GENERATED by run_lift.py */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n'
                'const uint32_t fc_entry_va = 0x%08Xu;\n' % (len(entries), entry))

    # Readable names, and a header on every function saying what it is, from
    # what the binary itself says: RTTI's vtables, debug messages, vtable
    # stores (pcrecomp's tools/lift/name_lift.py). work/names.json adds names
    # from outside the binary, as {"0x004B3B80": "CreateMainWindow", ...}.
    if not args.no_names:
        import name_lift
        rtti_path = os.path.join(os.path.dirname(SEEDS), 'rtti.json')
        rtti = json.load(open(rtti_path)) if os.path.exists(rtti_path) else {}
        given = ({int(k, 16): v for k, v in json.load(open(NAMES)).items()}
                 if os.path.exists(NAMES) else {})
        named = name_lift.name_lift(args.out, name_lift.Image(args.exe), rtti, given)
        print('[*] names: %d of %d functions named (%d given in work/names.json)'
              % (sum(1 for f in named.values() if f.name), len(named), len(given)))

    lines = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8', errors='replace'))
                for fn in os.listdir(args.out))
    changed = sync_dir(args.out, final_out)
    print('[*] %d of %d files changed in %s' % (changed, len(os.listdir(final_out)), final_out))
    stats = {'lifted': len(chosen), 'stubs': len(stubs), 'errors': errors,
             'no_terminator': dirty, 'eh_functions': ehs, 'files': idx, 'lines': lines,
             'unimplemented': dict(sorted(unimpl.items(), key=lambda kv: -kv[1]))}
    json.dump(stats, open(STATS, 'w'), indent=1)
    print('=' * 60)
    print('  lifted %d   not-lifted stubs %d   errors %d   no terminator %d   with catch blocks %d'
          % (len(chosen), len(stubs), errors, dirty, ehs))
    print('  %s lines of C in %d files, %.1fs' % (format(lines, ','), idx, time.time() - t0))
    if unimpl:
        print('  UNIMPLEMENTED instructions (lifted as no-ops): %d' % sum(unimpl.values()))
        print('    ' + '  '.join('%s:%d' % kv for kv in stats['unimplemented'].items()))
    print('=' * 60)


if __name__ == '__main__':
    main()

# Recon

The build: the Steam release of *The Legend of Heroes: Trails in the Sky*
(app 251150), `ed6_win_DX9.exe`.

## The exe

| | `ed6_win_DX9.exe` |
|---|---|
| sha256 | `99f08accd8e7d3a6a8732f8b965add4d51168431ad65bc0ec0a33adfae4daca2` |
| PE timestamp | `0x632CA5C5`, 2022-09-22 18:13 UTC (the "configfix" build) |
| Image | 0x00400000, size 0x01555000, **no relocations** |
| Entry | 0x004FBDAA |
| `.text` | 0x0012A34B bytes (1.22 MB) |
| `.data` | 0x242400 on disk, **0x013BE57C in memory** (20 MB, mostly zero-filled) |
| Protection | none: no SteamStub, ordinary section entropy |
| Compiler | Visual C++ 2017 15.9 (linker 14.16), **link-time code generation**, C runtime linked in |

Sections: `.text`, `.rdata`, `.data`, `_RDATA`, `.rsrc`. No TLS directory.
A Direct3D 8 twin, `ed6_win.exe`, sits beside it (same build); this project
lifts the Direct3D 9 one.

Imports, 227 from 15 DLLs: KERNEL32 (124), USER32 (40), d3dx9_43 (33), WINMM
(10), ole32 (5), VERSION, SHELL32, OLEAUT32, ADVAPI32, d3d9
(`Direct3DCreate9`), DINPUT8 (`DirectInput8Create`), DSOUND (ordinal 1),
d3dxof, XINPUT9_1_0, dbghelp. Delay-loaded: `steam_api.dll` (8) and
`Galaxy.dll` (3). The Ogg Vorbis DLLs are loaded by name at run time.

```
$ py -3 pcrecomp\tools\cpp\rtti.py "Trails in the Sky FC\ed6_win_DX9.exe" -o work\rtti.json --seeds work\rtti_seeds.json
[*] type descriptors : 142
[*] vtables          : 133
[*] classes          : 133
[*] virtual methods  : 408  (385 attributable to one class)
```

## What the code is made of

A linear sweep of `.text`: 346,091 instructions, of which

| | instructions |
|---|---|
| use an xmm register | 40,587 (12%) |
| x87 | 1,039 |
| `movss` alone | 12,334 |

So it is an SSE2 program (`/arch:SSE2`, the VC 2017 default), with x87 left
only in the C runtime's math library. The vectoriser has been at it too:
`pshufd`, `punpckldq`, `pshuflw`, `pmuludq`, and SSE4.1 forms (`pmulld`,
`pmuldq`, `pmovsxwd`) behind `__isa_available` tests. Six instructions use
`ymm`, all in the runtime's AVX2 string routines.

Link-time code generation shows in the calling conventions: internal
functions take arguments in whatever registers the linker chose, floats in
`xmm1`-`xmm3` included, and some are caller-cleaned where the prototype says
`__fastcall`. None of that matters to a lift, which keeps every register and
the stack as the original had them.

## The function catalog

`disasm32.py` on its own, scored against a Ghidra analysis of the same exe
(4,109 functions: 2,144 game, 1,184 C runtime, 747 exception funclets):

```
reference     4,109 functions
candidate     3,698 function starts
  true positives       3,355
  false negatives        754     (752 of them found, but as aliases: the EH funclets)
  precision   90.72%   recall   81.65%
```

Seeded with that list and the 408 RTTI vtable slots, recall is 100% and the
catalog has 7,220 entries (4,439 starts, 2,781 aliases: labels inside another
body that something branches to, and the exception funclets).

## The lift

```
$ py -3 tools/run_lift.py --all
  lifted 7220   not-lifted stubs 0   errors 0   no terminator 87   with catch blocks 15
  2,030,021 lines of C in 29 files, 52.5s
  UNIMPLEMENTED instructions (lifted as no-ops): 43
    pcmpistri:13  int:10  bnd:6  vxorps:2  vpmovmskb:2  vzeroupper:2  fnstenv:2  fldenv:2  pause:1  xgetbv:1  vpcmpeqb:1  vpcmpeqw:1
```

What is left unimplemented is the runtime's SSE4.2 and AVX2 string routines,
which never run because the host's CPUID answers "SSE2" (docs/bringup.md),
`int 0x29` on the runtime's fail-fast paths, and jump-table bytes in `.text`
decoded as code (`bnd`).

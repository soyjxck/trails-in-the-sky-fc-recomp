# Bring-up

How far the lifted game gets, and each wall on the way, newest last. Every
entry is something the next reader would otherwise spend a day on.

## 1. The toolkit: which lifter, which host

pcrecomp has two 32-bit lifters. `lift32_cpu` (an explicit CPU struct) is for
hybrid builds, where lifted code runs inside the original program. `lift32`
(global registers) is the one `generate.py`, the C++ exception support
(`eh32`) and the `native32` host are built around, and native32 is the host
this game wants: a 32-bit process that maps the exe at its own base, so a
pointer the game holds is a pointer Windows can take, and Direct3D 9,
DirectSound and DirectInput need no shims at all.

Upstream's `lift32` had no SSE when this started, and this exe is an SSE2
program (docs/RECON.md). The `pcrecomp/` submodule is a fork whose `main` is
upstream's plus the fix branches that were open there:

| upstream PR | what |
|---|---|
| #79, #94, #96 | lift32: SSE and SSE2, packed integer SSE2, the x87 compares and EFLAGS |
| #80, #93 | lift32: `xadd`/`cmpxchg`; `loope`/`loopne` test ZF |
| #82, #83, #95 | native32: a fully shimmed DLL is not loaded; MSVC's SetThreadName exception returns; hold a guest image's range from process start |
| #81, #65 | disasm32: incremental-link thunk tables; a seed is never dropped |

and two fixes of its own, found here:

- **`pe_analyze` named no import.** Without `pefile` installed its fallback
  parser reported the import *lookup* table's addresses as the IAT slots. The
  two tables are parallel arrays in different places, so no `call [slot]` in
  the code matched an import.
- **The vectoriser's instructions.** `punpckldq`, `punpckhdq`, `pshuflw`,
  `pshufhw`, `pmuludq`, byte and word `padd`/`psub`, `cvttpd2dq`, and the
  SSSE3/SSE4.1 forms VC 2017 emits behind an `__isa_available` test
  (`palignr`, `pmulld`, `pmuldq`, `pmovsx*`, `pinsrb`). 215 sites lifted as
  no-ops before; each is now checked against Unicorn by `difftest.py`.

## 2. An SSE2 CPU, as far as the C runtime knows

The C runtime linked into the exe picks its `memcpy`, `strlen`, `strchr` and
math routines by CPUID at start-up, and on a modern CPU it picks AVX2 and
SSE4.2 ones (`vpcmpeqb`, `pcmpistri`), which `lift32` does not lift. The host
clears CPUID leaf 1's `ecx` for lifted code (`g_cpuid_ecx1 = 0`): that is
every feature after SSE2, and `OSXSAVE`, so the runtime stays on its SSE2
paths. Native DLLs (D3DX above all) ask the real CPU and are not affected.

## 3. The image needs 21 MB at 0x00400000

The exe has no relocations and its `.data` is 20 MB in memory, so it needs
0x00400000-0x01955000, which is where a 32-bit process puts its first
thread's stack and its first heaps. The host is linked at 0x60000000 with a
256 KB stack, and starts itself a second time with that range reserved
before the loader runs (`native32_hold_image`); the lifted code then runs on
a thread of its own with a 64 MB stack (`guest_main`).

## 4. A throw is not an import

native32 answers `_CxxThrowException` by name, as an import from `msvcrt` or
`MSVCRxx`. Here the C runtime is linked in, so a throw is the function at
0x004FD597, which builds an exception record and calls `RaiseException`:
on the host's SEH chain, which has never heard of the guest's frames. The
lift replaces that function's body with a hook (`HOOKS` in `run_lift.py`,
`fc_hook_004FD597` in `host.c`) that hands the throw to native32's `eh32`,
which dispatches it over the guest's own `fs:[0]` chain.

It matters at once: the game leaves its loops by throwing. `PumpMessages`
(0x004B39E0) throws the `int` 0x12345678 on `WM_QUIT`.

## 5. The guest is not the process

Windows knows the process as `trails.exe`. The game's `hInstance`, its own
path, its command line and its resources are the guest's:

- `GetModuleHandleW(NULL)` answers 0x00400000 (the C runtime reads its own PE
  headers there), `GetModuleFileNameA/W` and `GetCommandLineA/W` the game exe;
- `LoadIconW` reads the window icon from the exe loaded as a resource module;
- `DirectInput8Create` is given the host's instance, since it wants a module
  Windows knows;
- `SetUnhandledExceptionFilter` does nothing: a fault is the host's to report.

## 6. The first run: a throw from a native DLL

The C runtime's start-up, the static constructors and the game's entry chain
ran at once. It ended, ten seconds in and with no report, with exit code
0xC0000409, inside the GOG Galaxy SDK:

```
[native] KERNEL32.dll!LoadLibraryExA      (0052CE60 ...) a0="steam_api.dll" -> 70E70000
[native] ?                                (...) from sub_004F9F26 @70E79680 -> 00000000      SteamAPI_Init: no
[native] KERNEL32.dll!LoadLibraryExA      (0052CE70 ...) a0="Galaxy.dll" -> 70050000
[native] ?                                (005421A8 005421C0 00000001 ...) from sub_004B6F00 @70053413
```

The exe is one build for Steam and GOG. With Steam not running it asks
Galaxy instead, as

```
0x004B7142  mov  dword ptr [ebp - 4], 0          ; try {
0x004B7149  call dword ptr [0x79824c]            ;     galaxy::api::GetInstance()
0x004B715F  call dword ptr [edx + 8]             ;         ->Init(clientID, clientSecret, 1);
0x004B71CC  ...                                  ; } catch (const galaxy::api::IError& e) { log(e.GetMsg()); }
```

and without the Galaxy client `Init` always throws. That throw is raised by
native code on the host's SEH chain, where no lifted function has a frame:
nothing caught it, and the host's own runtime ended the process as an
unhandled C++ exception (`terminate`, hence the fail-fast code and no fault
report). native32's bridge now wraps the native call in `__try`: a C++ throw
the callee did not catch itself is copied out while the thrower's frames
still exist, the native frames are unwound, and the throw is dispatched over
the guest's own frames like one of its own (pcrecomp fork,
`foreign_throw` in `eh32.c`).

## 7. The title screen

With that the game creates its window and its Direct3D 9 device and draws:

```
[window] CreateWindowExW(The Legend of Heroes: Trails in the Sky (DX9)) style 0x90000000 1280x720 -> 2B4E0574
[d3d9] CreateDevice 1280x720 windowed=1 flags 0x54 -> 0x00000000
[d3d9] first Present
[watchdog] 90 s: in sub_004B39E0, last native call KERNEL32.dll!Sleep, 206978994 indirect calls, 9614 frames presented
```

The title screen, its menu and its changing backgrounds, at about 107 frames
a second in an unattended run, with no fault and no unresolved call.

Two of the first four runs ended early instead, the same way each time:

```
[d3d9] Reset 1280x720 windowed=1 -> 0x88760868
[exit] ExitProcess(1) from sub_0051095E
```

That is the original's own behaviour, not the lift's. Something else on the
machine had changed the desktop, every Direct3D 9 device on it was lost, and
the game answers a lost device by resetting it once and leaving when the
reset fails (`Device_CheckLost`, 0x00460740, throws the same `int` as a
closed window). A lost device cannot be reset until Windows says it can, so
the reset failed and the game left: through its own `throw`, its own `catch`
and the C runtime's `exit`, which is also the first full trip through the
guest's exception handling. The host's `Reset` now waits for the device to
become resettable first, a fix the original does not have.

## 8. The catalog decides whether the lift is right

The first catalog was seeded with a symbol-matching disassembler's function
list for this exe (4,109 starts). To check the steps a clean checkout would
take, the game was lifted again from a catalog seeded with the RTTI vtable
slots alone. It built, and ran at six frames a second: 158 frames in 25
seconds on the title screen, against 2,500.

The two catalogs differ in 54 entries. Without the seeds, `disasm32`'s scan of
data for code pointers takes nine addresses inside large functions for
function starts, round ones among them (0x00436000, 0x00437000, 0x00438000),
and a start is a wall: the body before it ends there, and what was one
function is lifted as two. Which of them breaks the timing was not chased;
the catalog is what is wrong, and a correct one is cheap.

So the starts are in the repository, as `config/function_starts.json`: the
4,109 addresses and nothing else. `disasm32 --seed-functions` with that file
reproduces the catalog the lift is tested with, entry for entry (7,220).

## 9. In game

Input for hands-off runs is a script of button presses (`--script`,
`tests/*.txt`): `Input_Poll` (0x004802C0) returns the buttons held in
`edx:eax`, the lift emits it under another name and calls the host's wrapper
(`WRAPS` in `run_lift.py`), and while a script plays the wrapper answers with
the script's buttons. With that, unattended, each ending with the game
closed through its own window procedure, `throw`, `catch` and `exit`:

| script | what it plays | frames |
|---|---|---|
| `tests/title.txt` | the title menu, the difficulty dialog and back | 1,288 |
| `tests/new_game.txt` | a new game: the opening scenes and their dialogue | 15,683 |
| load a save | the save list, a save from the final chapter, walking about | 4,577 |
| a battle | a save from chapter 2, into three monsters, the fight won | 13,588 |
| a shop | the camp menu, talking, buying, a map change, an NPC | 7,965 |

No fault, no unresolved call and no instruction lifted as a no-op was
reached in any of them. About 100 frames a second throughout.

Sound runs too, unheard: `--silent` mutes the process's audio session, and
with music and effects switched on the multimedia timer's thread calls the
lifted music stream 83 times in 15 seconds (0x004A93C0), with the Ogg
decoder's read and seek callbacks between: the first lifted code on a second
thread, through native32's machine lock.

Not tested yet: the game in a visible window with a real keyboard, mouse or
pad; what the sound sounds like; and the logo and opening movies, which
did not start in an unattended run even when switched on in `config.ini`.

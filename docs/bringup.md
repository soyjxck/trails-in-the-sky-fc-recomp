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

## 10. Where a frame's time went

`--profile FILE` samples the main thread a thousand times a second and says
where it was, by this build's own symbols (`src/runtime/profile.c`). The
first profile, loading a save and walking about:

```
Where the main thread was:
    9.05%  asleep (Sleep, waits)
   20.84%  lifted game code
   35.11%  the runtime in this exe (bridge, lock, dispatch, host)
   35.00%  native code the game called (Windows, Direct3D, the driver)

   11.91%  native32_name      11.32%  native_bridge      4.81%  dispatch_find
```

The game's own code was a fifth of the thread. The rest was getting in and
out of it, about 28,000 times a frame (`--count-natives`):

| what | calls a frame | |
|---|---|---|
| `Sleep`, `PeekMessageW`, `QueryPerformanceCounter` | 13,500 | the game's frame limiter, spinning |
| D3DX vector maths (`D3DXMatrixMultiply` 4,650) | 10,500 | |
| Direct3D 9 device calls | 3,000 | |

What was done about it, most of it in the pcrecomp fork's bridge:

- **The register file stays put.** The bridge released the machine lock
  around every native call and saved, then reloaded, every register (x87,
  MMX and SSE included) each time. The registers now stay in the globals and
  move only when a different thread claims the machine.
- **One instruction was a tenth of the thread.** After each native call the
  bridge ran `fxam` to tell a returned float from an `fninit`. An empty
  `st(0)` still holds whatever bits were left in it, after MMX code a NaN
  pattern, and classifying one takes a microcode assist. It now runs only
  when the x87 stack did move.
- **Names and dispatch.** The import's name, kept for fault reports, was a
  walk of every name at every call (a cache now); an indirect call was a
  binary search of 7,220 functions (a flat table now).
- **Calls that keep the machine.** Direct3D, the vector maths, the heap and
  the clock neither sleep nor wait for another thread of the game, so the
  bridge does not hand the machine over for them at all
  (`native32_fast_*`, `fast_imports` in `host.c`). The seven hottest D3DX
  functions are called directly: the real functions still, bit for bit.
- **The frame limiter sleeps.** `Clock_Update` (0x004C11A0) held a frame to 8
  ms by looping on `PumpMessages(); Sleep(0)`: thousands of turns a frame, and
  one core at full load to do nothing. The host waits instead (a
  high-resolution timer, then a spin as short as the timer has earned) on a
  schedule, so the rate is exact, and `--fps N` sets it.

After, same script, same machine:

| | main thread, each frame | of one core |
|---|---|---|
| the game's limiter, as shipped (`--original-limiter`) | 7.86 ms | 92% |
| 125 a second (the default) | 4.26 ms | 50% |
| 60 a second | 4.75 ms | 27% |

```
--fps 125   1287 frames, 125.0 a second (8.00 ms each); 0 took half as long again or more, the longest 8.6 ms
--fps 60     566 frames,  60.0 a second (16.67 ms each); 0 took half as long again or more, the longest 17.1 ms
--fps 144   1552 frames, 144.0 a second (6.94 ms each); 0 took half as long again or more, the longest 8.3 ms
--fps 240   2514 frames, 240.0 a second (4.17 ms each); 0 took half as long again or more, the longest 5.1 ms
```

The bridge and dispatch are now about 4% of the thread, lifted code 22%.
Above 125 frames a second the game runs on shorter time steps than it was
made for; nothing has gone wrong at 144 and 240 on the title screen, and
that is all that has been looked at.

Start-up has its own cost, not yet dealt with (`--slow-calls 200`): the GOG
Galaxy SDK's `Init` takes a second to fail, DirectInput's set-up 0.8 s, and
the game's "is this pad an XInput one" check, a WMI query, 0.7 s.

## 11. Vulkan and Direct3D 12

The game draws through Direct3D 9 and nothing else, so the renderer is a
question of what answers `Direct3DCreate9`, and that import is the host's
(`--renderer`, `wrap_d3d9` in `host.c`):

| | |
|---|---|
| `d3d9` | Windows' own Direct3D 9 (the default) |
| `vulkan` | [DXVK](https://github.com/doitsujin/dxvk), Direct3D 9 on Vulkan: its 32-bit `d3d9.dll` in a `dxvk` folder beside the exe |
| `d3d12` | Direct3D 9 on Direct3D 12, part of Windows 10 and later (`Direct3DCreate9On12`) |

Players have used DXVK with this game for years by putting its `d3d9.dll`
beside the game's exe. Here it is loaded by path from the port's own folder:
nothing goes into the game's folder, DXVK's log and shader cache land beside
the port's exe, and a missing or failing renderer falls back to Direct3D 9
with a line in the log. D3DX stays the system's in every case, since it only
talks to the device it is handed.

```
[renderer] Vulkan (DXVK)                  DXVK v2.6.2, x86
[d3d9] CreateDevice 1280x720 windowed=1 flags 0x54 -> 0x00000000
[frames] 17490 frames, 122.8 a second (8.15 ms each)

[renderer] Direct3D 12 (D3D9On12)
[frames] 17448 frames, 121.8 a second (8.21 ms each)
```

Both played the title menu and the three-minute save, field and battle
script to the end, with the picture as under Direct3D 9.

## 12. Mods as loose files

A mod for this game has always replaced a whole `ED6_DTnn.dat` and `.dir`
pair, so two mods for one archive could not both be installed, or it came
with a loader injected into the game (LB-ARK as `d3dxof.dll`, SoraVoice as
`dinput8.dll`), which patch the game's machine code in memory. Here that code
is not what runs, so neither loader can work, and neither is needed: the
loader is part of the port (`src/mods/vfs.c`).

`ReadDirFiles` (0x00477580) and `ArchiveReadRaw` (0x00477180) are wrapped.
After the tables are loaded, each entry a mod replaces is given its
replacement's size and, for an offset, 0x80000000 plus a number; every read
of an entry goes through `ArchiveReadRaw` with that entry's own offset and
size, so a replacement is known by its offset and filled from the mod's file.
A mod is a folder under `mods`:

| | |
|---|---|
| `mods\<mod>\ED6_DT05\h_kao000._ch` | a plain file |
| `mods\<mod>\data\ED6_DT05\...` | the same, laid out as LB-ARK mods are |
| `mods\<mod>\packed\ED6_DT05\...` | an entry exactly as an archive holds it |
| `mods\<mod>\voice\scena\T0310   ._SN` | a script, laid out as SoraVoice's are |

The game unpacks most entries after reading them. A `packed` file is still
packed; a plain one is handed over as a stream of stored runs, which the
game's own unpacker copies through, so no compressor is needed.

`tools/archive_diff.py` turns a whole-archive mod into such a folder: it
compares the mod's archive with the clean game's entry by entry and writes
out only what differs. The four Evolution art archives come to 620 entries:

```
ED6_DT00: 8 entries changed, 0 new (of 102)       status images
ED6_DT04: 21 entries changed, 0 new (of 177)      event visuals
ED6_DT05: 583 entries changed, 0 new (of 2001)    portraits
ED6_DT0F: 8 entries changed, 0 new (of 38)        battle faces
```

With those and SoraVoice's 491 scripts as a second mod, a new game plays
its opening with the Evolution portraits in the message windows:

```
[mods] 10-evolution-art: 620 files
[mods] 20-voice-scripts: 491 files
[mods] 1111 files from 2 mods stand in for archive entries
```

Not done yet: files a mod adds that the archive has no entry for, the voices
themselves (the scripts only carry the tags), and music and movies, which the
game opens by path and not through its archives.

## 13. Off Windows: Linux, and the Switch

Everything up to here runs the game's code as lifted C but borrows the real
Windows for the rest: Direct3D, D3DX, DirectSound, DirectInput, windows,
files. That host (`src/runtime`, on pcrecomp's `native32`) can only ever be a
32-bit Windows program. The second host, `src/portable`, is the same lifted C
on pcrecomp's `win32hle`, where every import is answered by the host's own
code. On a 64-bit machine the guest's memory is one block and every guest
address an offset into it, which is what arm64 needs.

`tools/build_portable.sh linux` (in WSL) and `tools/build_portable.sh switch`
(devkitA64 and libnx) build it. What it took:

- **The lift is portable as it stands.** All 2.03 million lines compile with
  gcc for x86-64 and for aarch64 with no change to the lifter or the
  generated C.
- **C++ exceptions** (`eh32.c`) were written inside the Windows host. They
  build on their own now, every guest address through `ADDR()`.
- **CPUID.** With no x86 CPU to ask, the runtime left the registers as they
  were, and the game's C runtime read its feature bits out of whatever was in
  them. It now answers as a plain SSE2 CPU.
- **A newer C runtime.** win32hle grew up on Windows 95-era games and the
  narrow API; this exe is Visual C++ 2017 and calls the wide one.
  `src/portable/win32_wide.c` answers 70 imports: most are the wide call
  forwarded to the narrow one win32hle has, with the strings narrowed into
  guest memory; the rest are what the runtime asks on its way to `WinMain`.
- **Steam and Galaxy.** `steam_api.dll` is delay-loaded by a helper in the
  exe; it is answered with shims that say Steam is not running, and the
  game's Windows-version test for Galaxy with "too old".
- **The Switch has no `mmap`,** and a homebrew program cannot reserve address
  space without committing it, so there the guest lives in one allocated
  block of 512 MB (`HLE_GUEST_ARENA`), with everything placed low. It also
  names its storage device inside paths (`sdmc:/switch/...`), which put a
  colon in the middle of every Windows path made from one and crashed the
  game's first `CreateDirectory`; the device name is taken off.

Where it gets, on Linux and, as `trails.nro` (38 MB), on the Switch under
Ryujinx: through the C runtime's start-up, the static constructors,
`WinMain`, the settings and the save folder, to the engine asking for
Direct3D, where the game itself says so and leaves:

```
[pe] /switch/trails/game/ed6_win_DX9.exe mapped at 0x00400000, span 0x1555000
Trails in the Sky FC, portable host: image at 0x00400000, entry 0x004FBDAA, 7220 lifted functions
[pe] bound 227 imports, 0 unresolved
38 imports have no answer yet
entering 0x004FBDAA

[portable] Direct3D 9 is not implemented in this host yet
[messagebox] Trails in the Sky - Error: This program requires DirectX9.0c or later.
Please install the latest DirectX.
[win32hle] ExitProcess(0)
```

What stands between that and a picture:

| | |
|---|---|
| Direct3D 9 | the device itself: about fifty of its methods, fixed-function drawing and the game's assembled shaders, on OpenGL ES (the Switch's driver) |
| D3DX, 33 imports | 22 are vector maths; textures from memory (DDS); meshes from X files, with the X file reader (`d3dxof`); the shader assembler |
| sound | DirectSound is in win32hle; `mmio` (5 imports) for the WAV files, and the Ogg Vorbis DLLs the game loads by name |
| input | XInput and DirectInput, from SDL's controllers |
| movies | DirectShow |

Under Ryujinx the first minute of a run is the emulator translating the
lifted functions as they are first reached, some of them tens of thousands
of lines each; that cost is the emulator's, not the console's. What the
console's CPU makes of the game is not measured yet.

## 14. A picture off Windows: Direct3D 9 on OpenGL ES

The title screen now draws on Linux and on the Switch (under Ryujinx, at the
60 frames a second the emulator shows a docked console). What draws it is in
the fork, in `runtime/win32hle`: `d3d9.c` (Direct3D as the game sees it),
`d3d9_gl.c` (that state drawn with OpenGL ES 3), `d3dx9.c` (the D3DX the game
uses) and `d3d9_vs.c` (vertex shader assembly).

**What the game asks for** was measured on Windows before any of it was
written, two ways:

- apitrace's Direct3D 9 wrapper, put where the host looks for DXVK's
  (`build\dxvk\d3d9.dll`, `--renderer vulkan`), records every call of a run.
  The title and a battle use about 45 of the device's 119 methods: one
  texture stage, no pixel shaders, pre-transformed and fixed-function
  vertices, and eleven assembly vertex shaders with nothing in them but
  `mov`, `add`, `mul`, `mad`, `dp3`, `dp4`, `min`, `max`, `rsq`, `rcp` and the
  matrix macros. Its listing leaves out calls that only ask a question
  (`GetDeviceCaps`, `EnumAdapterModes`, `CheckDeviceFormat`), so the census is
  short of those; and a battle's recording was 9 GB, because the game locks
  the whole of a 786 KB vertex buffer some 25 times a frame.
- `--d3dx-dump DIR` (new, in the Windows host) writes what D3DX is handed
  and hands back. Of the 33 imports a battle uses the 22 of maths, textures
  from DDS files in memory (never compressed ones), copies between surfaces
  of the same format, and the shader assembler. The game assembles each
  shader, has D3DX list the result, and assembles the listing: so the
  disassembler has to exist too. `D3DXLoadMeshFromXA` is called once, for a
  file that is not there.

**How it is drawn.** Direct3D's picture has its first row at the top and
OpenGL's at the bottom. Everything is drawn upside down as OpenGL sees it, so
row 0 of every framebuffer is the top row and a texture that was drawn into
samples exactly like one the game filled from memory; only the copy to the
window turns the picture over. That flip, the half pixel between the two
APIs' pixel centres and the depth range are the last line of every vertex
program. Fixed-function processing is a GLSL program written from the state
a draw is made with, and kept by that state; a vertex shader is its bytecode
translated to GLSL between the same first and last lines. Vertices the game
keeps rewriting are sent a draw's worth at a time. Nothing GL owns is made
until a draw needs it, because the game creates and fills things from more
than one thread.

**Walls**, in the order they stood:

- Two methods of `IDirect3D9` were in each other's slots, so a call with six
  arguments was answered as one with seven and the game's next pointer was
  gone. `HLE_SHIMTRACE=prefix` (new) lists every call out by name.
- The game asks each new surface for "its texture" (`GetContainer`) to learn
  whether it has one. Answered with the device, it called the device as a
  texture.
- The game draws from two threads, one at a time (a second thread draws
  while the first loads). A GL context is one thread's at a time: the thread
  that has it lets it go whenever it stops to wait, when it ends, and when
  another has asked, and whoever draws next takes it.
- **The Switch: threads.** The C library there starts every thread at the
  one priority the console shares a core's time among; the main thread starts
  above it. Wherever the game polls for another thread's work, that thread
  never ran. The main thread now takes the same priority.
- **The Switch: fifty years.** The game takes its first reading of the
  performance counter as time gone by and counts it off a millisecond at a
  time. Windows counts from when it was started; the Switch's monotonic clock
  is the date. win32hle's clocks now count from the process's start.
- **Ryujinx: reading a picture back.** `glReadPixels` there fills the left
  quarter of each row and leaves the rest black, for a 4096-wide target and
  for the 1280-wide picture alike (a row's width in bytes taken for one in
  pixels, by the look of it). What is on screen is whole. The game draws
  into each render target it makes and reads it back to see that it worked,
  so under Ryujinx it concludes that it did not and goes without them; a
  host's own screenshots are cut the same way there. Whether a console does
  this is not known.

Test runs: `tools/run_portable.sh` (Linux, no window, PNGs of the picture),
and the same options on the Switch from `/switch/trails/args.txt`:
`--frames N`, `--shots DIR --shot-every N`, `--watch S` (where each thread
is, every S seconds: how the two Switch walls were found), `--trace PREFIX`,
`--d3d-log`.

What is still missing off Windows:

| | |
|---|---|
| input | a controller works, read through SDL and added to what the game's own poll returns (`Input_Poll` is wrapped, as it is for play scripts on Windows); the keyboard, the mouse and DirectInput itself are not there |
| sound | done since: see 15 |
| movies | DirectShow |
| Direct3D | the stencil buffer, lights beyond the simple cases (nothing has drawn with one yet), meshes from X files, anything past the title screen that has not been reached |
| the frame limiter, play scripts, the mod loader | the Windows host's: they move over |

And what the console's own CPU and graphics chip make of it is still not
measured: Ryujinx on a PC says nothing about that.

## 15. Sound off Windows

Music plays on Linux and on the Switch.

The game streams its music itself: a looping DirectSound buffer that a
multimedia timer tops up ten times a second, from an Ogg Vorbis file it
decodes through `vorbisfile.dll`. It loads that DLL by name and asks it for
seven functions with `GetProcAddress`. win32hle maps such a DLL without
running its code, and so far answered nothing for it: the game took that as
"no vorbisfile" and stayed silent, without complaint.

- The seven are now answered by the host (`runtime/win32hle/vorbisfile.c`),
  and `GetProcAddress` looks a mapped DLL's exports up among the host's shims
  by name. They are cdecl, unlike everything else the host answers. What the
  game calls an `OggVorbis_File` is only a name for the stream here: the file
  is read whole through the game's own read function when it is opened, and
  decoded from memory.
- **Floor 0.** The first decoder tried was stb_vorbis, which needs no
  library. It refused 55 of the game's 58 tracks: they were made by an encoder
  old enough to use Vorbis's first way of describing a spectrum, which
  stb_vorbis leaves out. The decoder is now the real libvorbis
  (`HLE_WITH_LIBVORBIS`; devkitPro's on the Switch, the system's or
  `VORBIS_CFLAGS`/`VORBIS_LIBS` on Linux), with stb_vorbis left as what a
  build without the library falls back to, saying what it cannot play.
- A failed open is not survivable: the game does not look at the answer of
  `ov_open_callbacks` and reads the channel count from whatever `ov_info`
  returns.
- `mmio` (the five imports for a music track kept as a wave file) is answered
  too, for files on disk; nothing has asked for it yet.

How it was checked without listening: `--audio-out FILE` writes everything
the mixer puts out to a WAV file (test runs play into SDL's dummy driver).
Against the title track decoded by ffmpeg, the recording correlates at 0.994,
at 0.56 of the level, which is the game's own music volume setting.
`SOUND=1 tools/run_portable.sh` switches music and effects on for a run.

Sound effects are wave files the game reads itself and plays from plain
DirectSound buffers, which win32hle already had; they are switched on with
the music.

A second copy of the game (a modded one, say) is a second folder on the card
with the program in it: the program works from the folder it was started
from. An emulator given the file does not say where that is; there the first
line of `/switch/trails/home.txt` names the folder to use.

## 16. Voices: SoraVoice without its DLL

SoraVoice gives the game spoken dialogue. On Windows it is a DLL the game is
made to load (as `dinput8.dll`), which patches the running game in a handful
of places. A port that runs none of the game's DLLs cannot load it, and does
not need to: the port can do at those places what the DLL does. With
SoraVoice installed in the game's folder as its own instructions say
(`voice\scena`, `voice\ogg`), the portable host now speaks.

Three things make it work:

- **Its scripts.** The voiced lines are in SoraVoice's own copies of the
  game's scripts, `voice\scena\<name>`, which stand in for the entries of
  `ED6_DT01`. That is what the mod loader does already (12): `src/mods/vfs.c`
  now builds for both hosts and takes that folder from the game's own
  folder before any mod's. 491 scripts.
- **Where a line is voiced.** In those scripts a voiced line carries a tag in
  its text: `#`, one to ten digits, a lowercase `v`. The game draws nothing
  for it. The lift now calls out (`PATCHES` in `run_lift.py`) where the
  window's text reader meets a `#` (0x004E191C), where it is about to play
  the typing beep (0x004E2EC4: not over a voice), and where the player's
  page-turn clicks (0x004E2DDD: the voice stops). Each tag is met once.
- **Which file.** Ten digits are the file's name, `ch<digits>.ogg`; six are
  `ed<the last four>.ogg`. Five or fewer are a number into the list of the
  `ch` files in name order, which the host makes by reading the folder (31,368
  files; on a thread of its own, at start-up). The file is decoded whole on
  another thread and handed to the mixer as a clip (`hle_audio_clip_play`,
  new in win32hle's DirectSound); one voice at a time.

Checked without listening, on Linux: the new-game script on a modded copy of
the game, with `--voice-log` and `--audio-out`. The opening scene's lines ask
for `ch0010000001`, `...02`, `...03` and on in order, with the second
speaker's `ch016...` between them, each once; and the clips decoded by ffmpeg
are found in the recording where the log says they were played (correlation
0.77 and 0.75 against a background of 0.009, music and all).

The portable host also has the Windows host's play scripts now (`--script`),
which is what made that run possible.

The Windows host got the same voices later (18). Not done: SoraVoice's own
settings file (volume, which sounds a voice mutes) is not read.

## 17. Movies

The logo movie plays on Linux and in the Switch build under Ryujinx (the
Evolution mod's 1080p file there: 1,539 frames shown, 30 dropped).

The game plays its movies through DirectShow's multimedia streaming
(`CLSID_AMMultiMediaStream`): a filter graph decodes the file, the game copies
each frame into a texture and draws it. With the movies switched on and no
DirectShow, the game faded out and waited on a black screen for a movie that
never started, until a button was pressed.

Answering DirectShow would have meant being a filter graph for the game's own
renderer. Instead the host plays the file itself, at the one door every movie
goes through:

- **`MoviePlay` (0x004886D0, the file's name in `ecx`) is wrapped.** On the
  portable host the wrap plays the whole file before it returns
  (`src/portable/movie.c`, on ffmpeg): each frame is scaled to at most 1280
  by 720, handed to the Direct3D layer as a picture (`hle_d3d9_show_picture`:
  drawn into the back buffer and presented like a frame of the game's), and
  the sound goes to the mixer as it is read. A button ends it.
- **The clock is the wall's.** The first version took the time from what the
  mixer had played, and stopped dead in the logo: that file's picture starts
  0.7 s after its sound, and the sound that would have moved the clock was
  only read once the wait for the picture was over.
- **The game is told the movie is over, in its own way.** A byte (0x0079AA3E)
  is the game's movie state: 0 none, 1 playing, 2 over. Everything that waits
  for a movie waits for the 2: the loop of the function that plays the logo
  (0x00488050: until the movie is over or a button is pressed), and a scene's
  script, which starts the movie with one command, asks "is it over" with
  another and stops it with a third. `MoviePlay` itself leaves a 2 for a movie
  that is switched off in the settings, so that is what the wrap leaves when
  the host has played one.

  The first version left the state alone and wrapped the logo's function as
  well, answering its loop with the confirm button. That did for the logo.
  The opening is not played by that function: a script plays it, after the
  first scenes of a new game. Before the fix a new game played to that point
  showed the whole opening and then a black screen until a button was
  pressed; with the state set, the next scene starts by itself (the same play
  script on both builds, the buttons left alone for 15 s after the movie).
- **The picture is drawn, not copied.** The first version filled a texture
  with the frame and copied it into the back buffer with `glBlitFramebuffer`.
  Linux showed it; under Ryujinx the screen stayed black for the whole movie
  while the log counted every frame as shown. The frame is now a texture on a
  rectangle, the way every texture of the game's reaches the screen.

The files: the plain game's logo is MPEG-1 with MP2 sound and its opening
XviD at 1920 by 960 with PCM; the Evolution mod's are H.264 at 1080p. ffmpeg
decodes all of them. On the Switch it is devkitPro's `switch-ffmpeg`;
`tools/build_portable.sh` takes `FFMPEG_CFLAGS` and `FFMPEG_LIBS`, and a build
without them skips the movies and says so.

Not known yet: whether the console itself decodes 1080p fast enough (three
decoder threads, frames that are late by more than an eighth of a second
dropped; an emulator on a desktop processor says nothing about that). The
stock game's opening (XviD, 200 s) has been played through on Linux, 11,995
frames and none dropped; movies later in the game than that have not been
reached.

## 18. The stock game and a modded one, on every host

Every host has to play the game as Steam installs it and a copy with mods
in it. Pointed at a modded folder (`--game`), the Windows host showed what
that takes: three things were wrong, none of them visible in the stock game.

**It was running on a mod's DLLs without knowing.** The folder held DXVK as
`d3d9.dll` and SoraVoice's loader as `dinput8.dll`. The host puts the game's
folder in the DLL search path, because the game loads `steam_api.dll`,
`Galaxy.dll` and `xinput1_3.dll` from there by name; so `d3d9.dll` by name
was the mod's too, and the log's "Direct3D 9" was Vulkan. The loader came
along the same way and did nothing (its `ed_voice.dll` is looked for beside
the exe that is running), which is the kind of nothing that turns into a
fault the day a file moves. Both are made for the original exe. Now, where
the game's folder has a file with the name of a DLL the game imports from,
Windows' own is loaded first, by its full path, and the log says which were
passed over. The exe's own imports are told to come from the system folder
too (`/DEPENDENTLOADFLAG:0x800`), for the day it is run from inside such a
folder.

**It had no voices.** The voices (16) were written against the portable
host's mixer, and the Windows host's three functions were empty. The player
is now one file for every host, `src/mods/voice.c`, and each host gives it
two things in a small file of its own, an Ogg Vorbis file as samples and
samples played:

| | reads with | plays through |
|---|---|---|
| Windows (`src/runtime/voice_out.c`) | the game's own `dll\vorbisfile.dll`, called directly | a DirectSound buffer of the host's own |
| Linux, the Switch (`src/portable/voice_out.c`) | win32hle's reader (libvorbis) | a clip in win32hle's mixer |

The game's libvorbisfile is an old one: it calls the `close` callback
without looking whether there is one, and the first voiced line was a jump
to address 0 on the decoding thread until it was given one that does
nothing.

**It did not read the mod's scripts.** `mods_scan_game` was only called by
the portable host. It is called by both now.

What the hosts take from a modded folder is then the same everywhere:
replaced archives, `BGM` and its packs (`BgmPack` in the settings; the game
falls back to `BGM` for a pack that is not there), the movie files, and
SoraVoice's `voice\scena` and `voice\ogg`.

The check is the same play scripts on each host and each folder, unattended
and muted (`tools\run_test.ps1 -Game`, `GAME=` for `tools/run_portable.sh`),
the modded folder being the Evolution set (art, music, movies, SoraVoice,
Japanese battle voices):

| | stock | modded |
|---|---|---|
| Windows: title, new game | pass | pass; 84 voiced lines, each read and started |
| Linux: title, new game | pass; logo 269 frames, none dropped | pass; logo (H.264, 1080p) 1,569 frames, none dropped; 82 voiced lines |
| Switch, under Ryujinx: to the title | logo shown, title at 60 frames a second | logo shown (1,549 frames, 20 dropped), title |

Not heard yet: the voices on Windows (the run is muted; the log shows each
line's file, length and that DirectSound took it).

## 19. Settings without the launcher

The game's OPTION screen has the sound, the camera and the mini map. The
resolution, window or full screen, anti-aliasing and the rest of the video
settings were only ever in its launcher, `Config_DX9.exe`, and the port's
own two (frame rate, renderer) only on the command line. A port should not
send its player to another program, or to a command line, to change the
resolution. Now there is one file to edit and a menu in the game.

**What the game does with its settings.** `config.ini` is read once at
start-up (0x004B1270), each key into a variable of its own, and written
back whole as the game leaves (0x004B06A0), from those variables. So a file
edited while the game runs is overwritten, and a setting changed from
outside has to be changed in the variable as well. The writer is a table
for the taking: for every key it formats one variable and writes it under
one section, 60 of them (`[Video]` from 0x007A8100, `[Sound]` from
0x007A85B0, `[Control]` from 0x007A85C0), and the file's path is at
0x007A8600.

What the video keys mean was read from where the game uses them:
`FullAntiAlias` is a sample count tested against the device's multisample
mask, `TextureFilter` is 1 nearest, 2 linear, 3 anisotropic (each falling
back by the device's caps), `ShadowQuality` 0 allocates nothing at start-up
and 2 runs a pass that 1 does not, `WaitVSync` picks the presentation
interval. In full screen the game asks for `WidthDX9` by `HeightDX9` as the
screen mode whatever `ModeDX9` holds, as long as that is not -1; with -1 no
device is made at all. That was checked without switching the display: an
unattended run keeps Direct3D windowed and logs what was asked for, and
1920x1080, 2560x1440 and 3840x2160 were each asked for as set.

**The port's own settings** went into the same file, in a section the game
does not know and so leaves alone: `[Recomp]`, `Renderer` and `Fps`. The
host reads it before it makes Direct3D (it works the path out itself: the
Saved Games folder, or `--savedir`); the command line wins for one run.

**The menu** (`src/ui/settings_menu.cpp`) is Dear ImGui drawn on the game's
own Direct3D 9 device, so it is the same under all three renderers. F2, or
both sticks of a pad pressed in, opens it.

- The game is held in the `Present` of the frame it has just drawn
  (`menu_hold`, host.c). That frame is copied to a texture and stands behind
  the menu, dimmed; the menu's frames are presented in its place. The
  window's messages are pumped there as the game's own loop would pump
  them. This thread is outside the machine during `Present` (11), so the
  game's window procedure can run from that pump, and the music's thread
  keeps playing.
- The window's procedure is the menu's first: with the menu open it takes
  the keys and the mouse and the game is given neither; closed, everything
  passes through.
- Nothing of the menu's is on the device while it is closed: the backend is
  started when the menu opens and shut down when it closes, and `Reset`
  closes it first. The game's own handling of a lost device never meets a
  buffer it did not make.
- When it closes, the game's clock (0x007C8BB8) is put forward by the time
  it was held, so the frame after is an ordinary one, and no button counts
  for the game until all have been let go: the key that closed the menu is
  not a key for the game.
- A row sets the game's variable and writes the key, both at once. The
  frame rate is the host's and changes there and then; everything else the
  game looks at when it makes its device, at the next start.

Checked unattended, by a hook for tests (`TRAILS_MENU_GRAB=<frame>` opens the
menu by itself at that frame, saves a picture of each page and closes it;
`<frame>,set` also changes two settings the way a row does): the three pages
draw over the title screen with the game's values in them; vertical sync
and 60 frames a second, set from the menu, are in the file after the game
has left, and the rest of that run is at 60; a file with `Renderer=vulkan`
and `Fps=60` starts on Vulkan at 59.4 frames a second, and `--renderer d3d12
--fps 144` over the same file gives Direct3D 12 at 142.

Not checked yet: the menu worked by hand (F2, the keyboard, a pad, the
mouse), which needs the window to be the active one.

## 20. The pointer off Windows, and a finger for a mouse

The game has two ways of being driven. By buttons; and by a pointer, which
it draws itself, whose place picks the menu item, and whose click chooses it.
`Input_Poll` goes over to the pointer when the pointer has moved since the
last poll (0x00480FB0 compares `GetCursorPos` with the place it remembers)
and back to the buttons at a direction button. Two bytes hold which
(0x00557E5E, 0x006B0018), and everything that draws or follows the pointer
looks at the second.

The game also glides the pointer to the selected item itself, with
`SetCursorPos`. On Windows that goes unnoticed, because the place it
remembers is the place it set. On the portable host nothing but that gliding
ever moved the pointer, and the game took it for a hand on a mouse: a
pointer on the title screen of a console that has no mouse, and menus that
followed it.

So the host decides. The fork counts the reports of a real pointer
(`hle_input_pointer_events`; `SetCursorPos` is not one), and the wrap of
`Input_Poll` keeps the game with the buttons, both bytes cleared after every
poll, except from such a report until the next button.

A real pointer is a mouse over the window, which the Direct3D layer's window
now passes on (it passed on nothing but its close button), at the point of
the back buffer the present has put under it. And it is a touch screen,
which SDL reports as a mouse. One thing is different about a finger: it does
not hover. It lands and presses in one event, and the menus take what the
pointer is over a frame before they take the click. A touch therefore moves
the pointer at once, presses two frames later and lets go no sooner than two
after that (`hle_d3d9_touch`).

Play scripts have a verb for it on this host, `touch X Y [MS]`, and the
check is one script on Linux without a window (the pictures are the back
buffer): the title screen with no pointer; a touch on OPTION, and the option
screen is open with the pointer where the finger was; ESC, the screen
closed and the pointer gone; a touch on LOAD, and the list of saves, the one
under the pointer lit.

Not tried yet: a real finger on a real console, and a real mouse.

## 21. A borderless window, the mouse switched off, and three faults a player found

The first hands-on session with the settings menu (4K, full screen,
Direct3D 12, vertical sync, all chosen in the menu and all taken at the next
start) asked for two things and turned up three faults.

**A borderless window** (`[Recomp] Borderless=1` with `WindowMode=1`; the
menu's Display row has it between Window and Full screen). The game stays in
its window mode and the window is the host's business: the shims of
`CreateWindowExW`, `SetWindowPos` and `SetWindowLongW` give it a frameless
style and the rectangle of its screen whatever the game asks for. The
picture, at the resolution the settings say, is stretched to it by
`Present`, to the largest rectangle of its own shape when the screen's is
another. The process now tells Windows it works in real pixels
(`SetProcessDpiAwarenessContext`): without that a 3840 by 2160 screen at
125% is 3072 by 1728 to the program, its window is stretched and blurred,
and a borderless window cannot be the size of the screen.

The first build of it was a black screen. The host had given the window's
class a black brush for the bars beside a picture of another shape, and the
game never validates its window, so Windows went on erasing it with that
brush, over the picture. The game was running all the while (its back
buffer, saved by a play script, held the title screen). The bars are now
painted by the host every 120 frames, and only when there are bars.

**The mouse switched off** (`[Recomp] Mouse=0`, and a row in the menu, taken
at once). The poll reads the mouse's buttons from its DirectInput device
when it has one (0x0079B1A4) and otherwise from what the window procedure
noted of the button messages; the pointer's place is always `GetCursorPos`.
So for the length of the poll the device is not there, the window is not
given mouse messages (the menu has the window's procedure first),
`GetCursorPos` and `SetCursorPos` are the private pointer the unattended
runs use, and after the poll the game is with the buttons (20). The device
is let go once, so that Windows has its pointer back.

**A lost device was waited for with the window dead.** The game resets a
lost device once and leaves if that fails; the host's `Reset` waits first
until the device can be reset. It waited in a loop of `Sleep` for at most
ten minutes, without pumping the window's messages. A game in full screen
that the player switches away from gets its device back only when its
window is active again, and a window whose thread does not answer cannot be
made that: ten minutes after Alt+Tab the game left. The wait now pumps
messages, has no limit, and puts the game's clock forward by its length.

**Vertical sync in a window was 32 frames a second.** Direct3D 9, and
Direct3D 9 on 12, asked to wait for the screen while drawing into a window,
held every `Present` for 31 ms on a 120 Hz screen, whatever the resolution
(`--slow-calls 20` names the call); the same setting in full screen gave
120, and DXVK gave 120 in a window too. For the other two the host now
makes a windowed device without the wait and follows `Present` with
`DwmFlush`, the desktop's own beat: 120.0 frames a second at 3840 by 2160
on both.

Checked: the borderless window's place, size and style from outside
(0,0 to 3840,2160, no caption, no frame); its picture on Direct3D 9 by
`PrintWindow` (black with the brush, the title screen without); the frame
rates above in unattended runs; the menu's new rows in its pictures. A
borderless window drawn by Direct3D 9 on 12 cannot be photographed this way
(the desktop's capture calls see what is under it), so that one is the
player's to look at.

The player looked: music, and no picture. **Direct3D 9 on 12 draws nothing
into a window here.** Not into the borderless one (which then shows whatever
was behind it) and not into an ordinary one (a black client area, seen in a
grab of the screen); the game runs and its back buffer is right, and in full
screen the same renderer is fine. It had only ever been run unattended,
where the pictures are the back buffer's and nobody sees the window (11).
The cause is not known. Until it is, a window is Direct3D 9's: asked for
Direct3D 12 with `WindowMode=1`, the host says so in the log and uses
Direct3D 9, and the menu says so under its Display page. On Direct3D 9 the
player confirmed the borderless window at 3840 by 2160.

Not tried yet: Alt+Tab out of full screen and back, and the mouse row with a
real mouse.

## 22. Frame times

`--frame-times FILE` writes every frame: when it was presented, how long
after the one before, and how much of that the main thread ran (by its cycle
count). A play script's `log` lines mark the file, so a stretch of frames can
be told from the loading before it.

The run measured is a walk: a chapter 3 save loaded in Zeiss, out of doors,
and 72 seconds of walking and turning the camera, unattended and muted, on
the machine all of this was developed on (a 120 Hz screen, a GeForce RTX
4090). The window of an unattended run is off screen, so these are the
game's and the renderer's times, not the desktop's. Frames in which the test
saved a picture are left out (a 3840 by 2160 PNG takes two to three
seconds). Frame time in ms; the last column is the main thread's share of
one core.

| | frames a second | median | 99% | 99.9% | main thread |
|---|---|---|---|---|---|
| Direct3D 9, 125 (the default) | 124.5 | 7.99 | 10.21 | 11.59 | 46% |
| Vulkan (DXVK), 125 | 124.6 | 8.00 | 9.82 | 10.83 | 42% |
| Direct3D 12, 125 | 124.5 | 7.99 | 10.13 | 11.31 | 48% |
| Direct3D 9, `--fps 144` | 143.5 | 6.94 | 8.91 | 9.91 | 51% |
| Direct3D 9, `--fps 60` | 59.8 | 16.66 | 18.64 | 19.76 | 21% |
| Direct3D 9, `--fps 30` | 29.7 | 33.31 | 36.71 | 101.49 | 15% |
| Direct3D 9, the game's own limiter | 123.1 | 7.99 | 10.57 | 20.08 | 94% |
| Direct3D 9, no limit | 682.6 | 0.63 | 4.55 | 5.76 | 91% |
| Vulkan, no limit | 913.2 | 0.38 | 4.03 | 5.26 | 98% |
| Direct3D 12, no limit | 539.2 | 1.11 | 4.89 | 6.26 | 89% |
| the modded copy, Direct3D 9, 125 | 124.5 | 7.99 | 10.16 | 11.41 | 45% |
| 3840 by 2160, Direct3D 9, 125 | 119.1 | 7.99 | 12.95 | 21.48 | 42% |
| 3840 by 2160, Vulkan, 125 | 124.4 | 8.00 | 12.01 | 14.21 | 51% |
| 3840 by 2160, Direct3D 12, 125 | 124.1 | 7.97 | 12.49 | 16.07 | 61% |
| 3840 by 2160, Direct3D 9, no limit | 352.3 | 1.16 | 25.10 | 53.80 | 63% |
| 3840 by 2160, Vulkan, no limit | 438.7 | 1.59 | 6.64 | 26.20 | 89% |

What it says:

- **The pace holds.** At every limit the median frame is the limit's own
  length to a hundredth of a millisecond, and ninety-nine frames in a hundred
  are within about two milliseconds of it, under all three renderers.
- **One hitch, the game's own.** Every run has one frame of about 0.3 s at
  the same place in the walk, with the main thread busy for nearly all of
  it: the game loading something as the party walks on. It is in the lifted
  code, not in a renderer; what it loads has not been looked at.
- **The host's limiter is half the cost of the game's.** The game holds a
  frame back by spinning (94% of a core); the host sleeps to the same pace
  (46%).
- **There is a great deal of room.** Without a limit the walk runs at 540 to
  910 frames a second, and at 3840 by 2160 still at 350 to 440. Vulkan is
  the lightest on the main thread and the most even without a limit;
  Direct3D 9 at 3840 by 2160 without a limit has frames of 25 ms among its
  one-millisecond ones.
- **The modded copy costs nothing** that this walk can show.

Two cautions from making it. The first `--fps 60` run stalled for most of a
minute, in frames of up to 15 seconds with the main thread idle; two more on
a quiet machine, one of them with `--slow-calls`, were clean, so that was
the machine and not the limit, and the lesson is to keep the machine still
while timing. And the 3840 by 2160 line for Direct3D 9 has its frames a
second pulled down by one frame of three seconds, nineteen frames before one
of the test's pictures, in which the main thread ran for 0.4 s and waited
for the rest. It is not a picture (those are left out, and it is not at one's
place in the file), the Vulkan and Direct3D 12 runs at that resolution do not
have it, and it is not explained.

Vertical sync in a window is section 21's: 32 frames a second before, 120
after.

## 23. Slow out of doors on the Switch: where a frame goes

On a console the port "is perfect but stuttering", and worse out of doors
than in. A console has no debugger and no profiler, so the first work was
instruments, and they are in the portable host for good:

- `--perf S` (ten seconds on the Switch unless told otherwise) writes to the
  log how many frames a second, the longest frame, how long the game's
  thread was at work in a frame, and the frame's split into the game,
  drawing, clearing, presenting and waiting. `--perf-detail` splits the
  drawing into finding the program, state, uniforms, vertices and the call.
- `--profile FILE` samples the game's thread. On Linux a timer's signal does
  it, with the callers from the unwinder. On the Switch a thread of the
  host's pauses the game's thread five hundred times a second, reads its
  registers, follows the frame records up its stack and lets it go; the
  samples are added to the file every ten seconds. `tools/profile_portable.py`
  names them from `trails.elf`, and since the graphics driver is linked into
  the program, Mesa's functions are named like the game's.
- `HLE_D3D9_CENSUS=N` says what the draws of every Nth frame were;
  `HLE_D3D9_FILLS=1` what the first four hundred texture fills were of.
- `--fps 0` takes the wait between frames out, so a frame is as long as the
  work in it.

All of it runs under Ryujinx, where the times are the emulator's but the
program is the console's, byte for byte, driver included: which functions the
time is in can be read there. The scene measured is a dungeon floor with the
party walking about: 870 draws a frame.

What was found, in the order it was found:

**Every draw said everything.** The Direct3D layer's GL half set the whole
of the pixel state, every uniform and every vertex array for each draw, and
made a buffer for each draw's vertices. The census says why that hurts here:
of 870 draws only about 75 have the program, texture and state of the draw
before them, so nothing can be drawn together; the map is a pile of small
pieces, each with its own texture. A draw now sends only what differs
(pcrecomp's `d3d9_gl.c`: the state GL was last told, the uniforms a program
was last sent, a serial for the vertex shader's constants, one stream for
vertices that come with their draw, the layout of a vertex worked out once
for each pairing of a declaration with a shader). On Linux the drawing went
from between 3.5 and 5 milliseconds a frame to 2.6, of which 2.4 is the
driver's own draw call and 0.3 the layer.

**An import was three lookups.** An indirect call searched the dispatch table
(thirteen probes), found nothing for an import, and then asked for a shim.
The table is flat now and the first question answers both.

**The music held the machine.** `ov_read` decoded with the machine lock
held: the game's own thread stood still for every burst of decoding. It is
given up around the decoder now. (And threads other than the game's keep off
its core on the Switch.)

**The wait for the display is not in Present.** The profile under Ryujinx
has a quarter of the game thread's samples in `svcWaitSynchronization`, under
`nwindowDequeueBuffer`, under Mesa's `st_framebuffer_validate`, under
`eglMakeCurrent`'s side of the first thing a frame draws: the driver takes
its next buffer, and waits for the display, when the frame after the swap
begins. So "presenting" is short and the wait turns up in the game's clear.
A frame's work is therefore taken as the processor time its thread had
(`svcGetInfo`'s thread tick count; `CLOCK_THREAD_CPUTIME_ID` on Linux), which
no wait is in.

**Forty-odd frames a second is a stutter.** Where Present waits for the
display, a scene that cannot quite be drawn at sixty comes out as frames of
one refresh and of two, mixed. `src/portable/pace.c` (`--fps auto`, the
Switch's default) presents at every refresh while frames are ready in time,
at every second one when eighteen of the last hundred and twenty came late,
and goes back up when the thread's work has stayed under thirteen
milliseconds a frame for six seconds; going up and falling again at once
doubles that wait. `--fps 60` and `--fps 30` pin it.

**Sprite sheets.** While the party walks, ten to twenty textures a second are
filled: 256 by 256, four bits a channel, the people's sprite sheets, which
the game keeps a few dozen of and refills as they turn. A fill was two passes
over the pixels a byte at a time; it is one now, sixteen pixels at a time on
arm64.

**The C runtime, lifted.** The top lifted function in the profile was
`_isnan`, with `_finite` and the compiler's float to 64-bit integer helper
not far behind: the game's culling asks whether each plane distance is a
number, twice over, and casts through `__int64` in 249 places. These are
written for SSE2, and an SSE register is memory to lifted code; a call into
lifted code also hands all eight registers over and back. They are the
host's now (`src/common/crt.c`, HOOKS in `run_lift.py`), `memset` with them,
and the three small ones are done in place where they are called
(INLINES): 458 call sites.

Smaller: a guest pointer is turned into the host's in line; a call out of
lifted code keeps its notes for `--watch` only when someone watches (a
thread-local variable is a function call on the Switch); matrix products go
a row at a time on arm64.

What was tried and left: `-O2` for the lifted code hangs the game after a
save is loaded; with `-fno-strict-aliasing -fwrapv
-fno-delete-null-pointer-checks` it runs, and is no faster on x86-64.

Where that leaves a frame, under Ryujinx, on the walk: the game's thread at
work for about twelve milliseconds of 16.7, sixty frames a second held. Of
the work, a third is lifted code, a fifth Mesa's side of the draws (state
validation, mostly for the texture each draw changes), and the layer's own
part of a draw a twentieth. Linux, with no wait between frames: 150 frames a
second on the software renderer, the game's thread at work 5.3 ms a frame.
Windows is unchanged but for the C runtime functions, which are the host's
there too.

Not known yet: the same numbers from a console. Its processor is several
times slower than the emulator's host, so sixty out of doors is not to be
expected from this alone; an even thirty is what the pacing is for. The log
of a run on one (`trails.log`, the `[perf]` lines) is the next thing to
read, and a profile with it (`--profile sdmc:/switch/trails/profile.txt` in
`args.txt`, then `tools/profile_portable.py trails.elf profile.txt`).

What would move it further, in the order of what the profile promises:

- **SSE registers as locals.** The game was built for SSE2, and every scalar
  float operation of the lifted code reads and writes the global register
  file. The integer registers are locals of each function already
  (`RECOMP_LOCAL_REGS`); doing the same for `_xmm` needs the lifter to know
  which of them a call takes and returns.
- **The hot functions natively**: 0x00453890, 0x00453AD0 and 0x00453D20
  (2 to 3% each), 0x00446A60 and 0x00446D60, 0x00465400.
- **Fewer draws.** Nothing in the layer can merge draws that each have a
  texture of their own, and none of the game's settings takes any away:
  the count is the same with shadows off and with `LowEffect=1`.

One setting of the game's does help, on the processor's side:
`ShadowQuality=0` in config.ini (no shadows under the people) takes a sixth
off the game's own part of a frame on Linux, 1.9 ms against 2.3.
`ShadowQuality=1` and `LowEffect=1` change nothing that can be measured.

## 24. A menu goes black on a console, and nowhere else

With section 23's build on a console: "the menu goes black, but sound keeps
going". It has not been made to happen anywhere else. Linux and Ryujinx draw
the logo movie, the title screen left alone for a minute, the load screen, a
loaded save and the camp menu with that build, at the display's rate and at
half of it (pictures of each; the emulator's by photographing its window).

What Ryujinx runs is the console's program, driver included, so what can
differ on a console is what an emulator is lenient about or has no like of:
the graphics chip's own memory and its timing, the display's queue, the
cores. Four things in section 23 lean on those, and each now has a switch,
which on the Switch is off until a run on a console has shown it right:

| in `args.txt` | what it switches on |
|---|---|
| `--stream` | vertices that come with a draw go into one 4 MB buffer, mapped without waiting and thrown away for a fresh one when full; without it, a buffer of its own for every draw, as before section 23 |
| `--fps auto`, `--fps 0`, `--fps 30` | the host paces the frames (by presenting at every refresh or every second one) and the game's own wait loop is skipped; without it, the game's own limiter, as before |
| `--aside` | threads other than the game's first are kept off its core |
| `--gl-plain` | not one of the four but the test for "none of them": every draw says everything to GL again |

So a console is back to how it drew before, with what section 23 took out
of the processor's work left in (the state that is not sent twice, the C
runtime functions, the sprite sheets, the music decoder that lets go of the
machine).

And the log says more, since nobody but the player sees a console's screen:

- a `[host]` line at the start with how the switches stand;
- GL's errors, a line each for the first two dozen (nothing here asks GL
  for what it may refuse);
- every 120th frame (`--watch-picture N`; 0 for never) three small pieces of
  the frame are read back, from the game's own buffer before it is shown and
  from the window's after it has been copied there, and a line is written
  when either turns black and when it is a picture again. Which of the two
  went black says whether the drawing failed or the showing;
- the `[perf]` lines count what was not drawn because another thread kept
  the graphics context.

One thing seen on the way: the camp menu's background is the frame read back
from GL and sent up again as a texture. Under Ryujinx only the left quarter
of each row comes back (section 14), so there it is a strip of the scene and
black beside it; a console reads the whole of it.

Open: which of the four it is, or what else. The next log from a console
says.

# Trails in the Sky FC - static recompilation

*The Legend of Heroes: Trails in the Sky* (Steam, `ed6_win_DX9.exe`) lifted
from its x86 machine code to C and rebuilt as a new executable. Every one of
the game's functions runs as recompiled C; Direct3D 9, DirectSound,
DirectInput and the rest of Windows are the real ones.

Built on [pcrecomp](https://github.com/sp00nznet/pcrecomp) (the `pcrecomp/`
submodule is a fork with the fixes this game needed), using its `lift32`
lifter and `native32` host, and laid out like sp00nznet's other native32
games.

No game files are in this repository, and neither is the lifted C: both are
made on your machine from your own copy of the game.

## Status

**In game.** All 7,220 functions of the exe lift (2.0 million lines of C, no
lift errors) and the lifted game plays: the title screen, a new game's
opening scenes, loading saves, walking about, the camp menu, shops and
battles, at about 100 frames a second, with music and sound effects running.
Each of those is a scripted, unattended test run that ends with the game
closing normally (docs/bringup.md, 9).

Played by hand in a window with keyboard and sound, and it plays. Not tested
yet: the logo and opening movies.

**Off Windows: the title screen.** The same lifted C also builds for 64-bit
Linux and for the Nintendo Switch (`tools/build_portable.sh`, `src/portable`),
on a host that answers the Windows calls itself instead of passing them to
Windows, Direct3D 9 included (on OpenGL ES 3). There the game starts and
draws its title screen, on Linux and on the Switch under Ryujinx, and a
controller's buttons reach it. Sound, movies, the keyboard and the mouse are
not there yet (docs/bringup.md, 13 and 14).

See [docs/bringup.md](docs/bringup.md) for every wall on the way.

## Getting started

You need the Steam release of the game (the 2022-09-22 build of
`ed6_win_DX9.exe`), Visual Studio with the C++ x86 tools, CMake, Ninja and
Python 3.10+ with `capstone`.

```
git clone --recursive <this repository>
```

Copy the game's folder into the checkout as `Trails in the Sky FC\`, then:

```
py -3 pcrecomp\tools\pe\pe_analyze.py "Trails in the Sky FC\ed6_win_DX9.exe" --json work\pe_analysis.json
py -3 pcrecomp\tools\cpp\rtti.py "Trails in the Sky FC\ed6_win_DX9.exe" -o work\rtti.json --seeds work\rtti_seeds.json
py -3 pcrecomp\tools\disasm\disasm32.py "Trails in the Sky FC\ed6_win_DX9.exe" --output work\functions.json --seed-functions config\function_starts.json
py -3 run_lift.py --all
build.cmd
play.cmd
```

The catalog takes about two minutes, the lift one, the build five. The seed
file matters: without it the catalog splits some functions in two and the
game runs wrong (docs/bringup.md, 8).

`build\trails.exe` on its own maps and binds the game and stops (a dry run).
`--run` plays. The other options:

| Option | |
|---|---|
| `--game DIR` | the game's folder (default: `Trails in the Sky FC` beside the build) |
| `--savedir DIR` | use DIR as the Saved Games folder, leaving your own saves and settings alone |
| `--unattended` | for tests: window off screen and never activated, a private mouse pointer, Direct3D kept windowed |
| `--active` | keep the game's clock running while its window is not the active one |
| `--silent` | mute the game (its sound code still runs) |
| `--shots DIR`, `--shot-every N` | save the back buffer as a PNG every N frames |
| `--script FILE` | play a script of button presses (`tests/*.txt`; `src/runtime/host.c` has the format) |
| `--renderer d3d9`, `vulkan`, `d3d12` | what draws: Windows' Direct3D 9, Vulkan (DXVK's 32-bit `d3d9.dll` in a `dxvk` folder beside the exe), or Direct3D 12 |
| `--fps N` | frames a second: 30, 60, 144... (default 125, the game's own ceiling; 0 = no limit) |
| `--watchdog S` | report where the game is and stop after S seconds |
| `--profile FILE` | sample the main thread and write where its time went |
| `--count-natives [N]`, `--slow-calls MS` | what the game calls outside its own code (the N most called), and which calls are slow |
| `--d3dx-dump DIR` | what the game asks of D3DX: shader sources and what was assembled from them, written to DIR; pictures, surface copies and meshes, listed in the log |
| `--calltrace FILE`, `--watch VA`, ... | pcrecomp's bring-up instruments (`trails.exe --help`) |

`play.cmd` is `build\trails.exe --run` with the log kept in `run\play.log`.
`tools\run_test.ps1 -Play tests\new_game.txt` is an unattended, silent test
run with all of the above set up; put a `config.ini` and saves for it in
`run\saves_base\FALCOM\ED6`.

## Layout

| | |
|---|---|
| `run_lift.py` | the lift driver: `work/functions.json` -> `src/recomp/gen/` |
| `src/runtime/host.c` | the Windows host: the guest's identity, exceptions, test modes, the fault report |
| `src/portable/`, `tools/build_portable.sh`, `tools/run_portable.sh` | the host for everything else (Linux, the Switch): the Windows calls answered in C; its unattended run |
| `src/mods/vfs.c`, `tools/archive_diff.py` | mods as loose files, and making one from a whole-archive mod |
| `config/function_starts.json` | the function starts the catalog is seeded with |
| `tests/`, `tools/run_test.ps1` | play scripts and the unattended run that plays them |
| `pcrecomp/` | the toolkit (submodule) |
| `docs/RECON.md` | what the exe is |
| `docs/bringup.md` | the walls, newest last |
| `work/`, `src/recomp/gen/` | generated, never committed |

## The game's files

The game is Nihon Falcom's. Lifting it makes a derivative work that carries
the game's own licence, which is why neither the game nor the lifted C is
here: bring your own copy.

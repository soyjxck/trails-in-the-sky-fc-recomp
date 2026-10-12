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

**Boots to the title screen.** All 7,220 functions of the exe lift (2.0
million lines of C, no lift errors), the build links, and the lifted game runs
its C runtime, creates its window and Direct3D 9 device and draws the title
screen and its menu at full speed, with no fault and no unresolved call.
Beyond the title screen is not tested yet.

See [docs/bringup.md](docs/bringup.md) for how far it gets and every wall on
the way.

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
py -3 pcrecomp\tools\disasm\disasm32.py "Trails in the Sky FC\ed6_win_DX9.exe" --output work\functions.json --seed-functions work\rtti_seeds.json
py -3 run_lift.py --all
build.cmd
build\trails.exe --run
```

The catalog takes about two minutes, the lift one, the build five.

`build\trails.exe` on its own maps and binds the game and stops (a dry run).
`--run` plays. The other options:

| Option | |
|---|---|
| `--game DIR` | the game's folder (default: `Trails in the Sky FC` beside the build) |
| `--savedir DIR` | use DIR as the Saved Games folder, leaving your own saves and settings alone |
| `--unattended` | for tests: window off screen and never activated, a private mouse pointer, Direct3D kept windowed |
| `--active` | keep the game's clock running while its window is not the active one |
| `--shots DIR`, `--shot-every N` | save the back buffer as a PNG every N frames |
| `--watchdog S` | report where the game is and stop after S seconds |
| `--calltrace FILE`, `--watch VA`, ... | pcrecomp's bring-up instruments (`trails.exe --help`) |

`tools\run_test.ps1 -Seconds 60` is an unattended run with all of that set up.

## Layout

| | |
|---|---|
| `run_lift.py` | the lift driver: `work/functions.json` -> `src/recomp/gen/` |
| `src/runtime/host.c` | the host: the guest's identity, exceptions, test modes, the fault report |
| `pcrecomp/` | the toolkit (submodule) |
| `docs/RECON.md` | what the exe is |
| `docs/bringup.md` | the walls, newest last |
| `work/`, `src/recomp/gen/` | generated, never committed |

## The game's files

The game is Nihon Falcom's. Lifting it makes a derivative work that carries
the game's own licence, which is why neither the game nor the lifted C is
here: bring your own copy.

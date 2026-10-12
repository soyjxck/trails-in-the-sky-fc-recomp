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

Not tested yet: playing in a visible window with a real keyboard, mouse or
pad, whether the sound sounds right, and the logo and opening movies.

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
| `--watchdog S` | report where the game is and stop after S seconds |
| `--calltrace FILE`, `--watch VA`, ... | pcrecomp's bring-up instruments (`trails.exe --help`) |

`play.cmd` is `build\trails.exe --run` with the log kept in `run\play.log`.
`tools\run_test.ps1 -Play tests\new_game.txt` is an unattended, silent test
run with all of the above set up; put a `config.ini` and saves for it in
`run\saves_base\FALCOM\ED6`.

## Layout

| | |
|---|---|
| `run_lift.py` | the lift driver: `work/functions.json` -> `src/recomp/gen/` |
| `src/runtime/host.c` | the host: the guest's identity, exceptions, test modes, the fault report |
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

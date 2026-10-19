# Building from source

The port is made in two steps: the game's exe is analysed and lifted to C
on Windows (`tools/run_lift.py`, into `src/recomp/gen/`, which is never
committed), and that C is built into a program for Windows, Linux or the
Switch. [bringup.md](bringup.md) is the history: every wall on the way and
how it was got past.

## What you need

- The Steam release of *The Legend of Heroes: Trails in the Sky* (the
  2022-09-22 build of `ed6_win_DX9.exe`; the lift and the program check its
  PE timestamp).
- Windows with Visual Studio 2022 (any edition, or the Build Tools) and its
  C++ x86 tools, CMake, Ninja, and Python 3.10+ with `capstone`.
- For Linux and the Switch, a Linux shell as well (WSL will do; see below).

## The lift, and the Windows program

```
git clone --recursive https://github.com/soyjxck/trails-in-the-sky-fc-recomp
```

Copy the game's folder into the checkout as `Trails in the Sky FC\`, then:

```
py -3 pcrecomp\tools\pe\pe_analyze.py "Trails in the Sky FC\ed6_win_DX9.exe" --json work\pe_analysis.json
py -3 pcrecomp\tools\cpp\rtti.py "Trails in the Sky FC\ed6_win_DX9.exe" -o work\rtti.json --seeds work\rtti_seeds.json
py -3 pcrecomp\tools\disasm\disasm32.py "Trails in the Sky FC\ed6_win_DX9.exe" --output work\functions.json --seed-functions config\function_starts.json
py -3 tools\run_lift.py --all
tools\build.cmd
build\trails.exe
```

The catalog takes about two minutes, the lift one, the build five. The seed
file matters: without it the catalog splits some functions in two and the
game runs wrong (bringup.md, 8). `tools\build.cmd` builds into `build\`
(`BUILD_DIR` names another folder).

`trails.exe` finds the game where `--game DIR` says, in `Trails in the Sky
FC` in the working directory, in its own folder (put beside
`ed6_win_DX9.exe`, as a player installs it), or in `Trails in the Sky FC`
beside the build folder. Started from Explorer it writes its log to
`trails.log` beside itself and lets its console window go; from a command
prompt it writes there. Its options:

| Option | |
|---|---|
| `--dry-run` | map and bind the game, and stop |
| `--game DIR` | the game's folder; a modded copy is one too |
| `--mods DIR` | mods as loose files (default: a `mods` folder beside the exe; `src/mods/vfs.c` has the layout) |
| `--voice-log` | say each voiced line and its file as it is spoken |
| `--savedir DIR` | use DIR as the Saved Games folder, leaving your own saves and settings alone |
| `--renderer d3d9`, `vulkan`, `d3d12` | what draws: Windows' Direct3D 9, Vulkan (DXVK's 32-bit `d3d9.dll` in a `dxvk` folder beside the exe), or Direct3D 12 |
| `--fps N` | frames a second: 30, 60, 120, 144... (default 120; 0 = no limit; above 125, the game's own ceiling, it runs on shorter time steps than it was made for) |
| `--unattended` | for tests: window off screen and never activated, a private mouse pointer, Direct3D kept windowed |
| `--active` | keep the game's clock running while its window is not the active one |
| `--silent` | mute the game (its sound code still runs) |
| `--shots DIR`, `--shot-every N` | save the back buffer as a PNG every N frames |
| `--script FILE` | play a script of button presses (`tests/*.txt`; `src/runtime/host.c` has the format) |
| `--watchdog S` | report where the game is and stop after S seconds |
| `--profile FILE` | sample the main thread and write where its time went |
| `--frame-times FILE` | write every frame's length, and how much of it the main thread ran |
| `--count-natives [N]`, `--slow-calls MS` | what the game calls outside its own code (the N most called), and which calls are slow |
| `--calltrace FILE`, `--watch VA`, ... | pcrecomp's bring-up instruments (`trails.exe --help`) |

### Test runs

`tools\run_test.ps1 -Play tests\new_game.txt` is an unattended, silent run
of a play script: a fresh copy of `run\saves_base` as the Saved Games
folder, the window off screen, the log in `run\trails.log`. `-Game DIR` runs
another copy of the game, a modded one say; the port has to play both.
Saves are not in the repository: put a `config.ini` and a few saves of your
own in `run\saves_base\FALCOM\ED6` (the scripts expect the title menu's
cursor to start on LOAD, which it does once a save exists).

## Linux and the Switch

The same lifted C builds for both, from a Linux shell, once the lift above
has been made:

```
tools/build_portable.sh linux      # build-linux/trails-portable
tools/build_portable.sh switch     # build-switch/trails.nro
```

Linux wants gcc, SDL2 and SDL2_ttf, OpenGL ES 3 (Mesa), libvorbisfile, and
FFmpeg for the movies. The Switch wants devkitPro's devkitA64 and libnx with
`switch-sdl2`, `switch-sdl2_ttf`, `switch-mesa`, `switch-libvorbis` and
`switch-ffmpeg`. Without FFmpeg the movies are skipped; without the real
libvorbis most of the music cannot be played (the top of
`tools/build_portable.sh` has the details).

`tools/run_portable.sh` is the unattended run there. The portable program's
options are listed when it is started without the exe's name; on the Switch
they go in `args.txt` beside the program, on one line. Besides the Windows
host's (`--fps`, `--voice-log`, `--script`, `--profile`...):

| Option | |
|---|---|
| `--saves DIR` | the Saved Games folder (default: `save` in the working directory; on the Switch beside the program) |
| `--headless` | no window (tests) |
| `--perf S`, `--perf-detail` | every S seconds, where a frame's time went |
| `--hot-check`, `--no-hot` | the game functions done natively (`src/common/hot.c`) checked against the originals, or not used |
| `--original-limiter` | the game's own frame waits, and its own Frame Rate row |

`--profile FILE` samples the game's thread; `tools/profile_portable.py`
names the samples from the program (on the Switch from the `trails.elf` the
NRO was made from).

## Layout

| | |
|---|---|
| `tools/run_lift.py` | the lift driver: `work/functions.json` -> `src/recomp/gen/`, and the host's wraps, hooks and patches |
| `src/runtime/host.c` | the Windows host: the guest's identity, exceptions, the frame limiter, test modes, the fault report |
| `src/ui/settings_menu.cpp` | the settings menu (Dear ImGui, in `third_party/imgui`, on the game's own Direct3D device) |
| `src/portable/` | the host for everything else (Linux, the Switch): the Windows calls answered in C, the movies, the Switch's start |
| `src/common/crt.c` | pieces of the game's C runtime done by the host |
| `src/common/hot.c` | the game's busiest functions (collision tests and walks, sorted triangles, shader constants) done natively |
| `src/common/frame_option.c` | the game's Frame Rate option, as the host's frame rate |
| `src/mods/vfs.c`, `tools/archive_diff.py` | mods as loose files, and making one from a whole-archive mod |
| `src/mods/voice.c` | SoraVoice's voices, for every host; `src/runtime/voice_out.c` and `src/portable/voice_out.c` play them |
| `tools/build.cmd` | the Windows build (the 32-bit compiler, CMake and Ninja) |
| `tools/build_portable.sh`, `tools/run_portable.sh` | the Linux and Switch builds, and the unattended run |
| `tools/profile_portable.py` | names for a profile of the portable host |
| `config/function_starts.json` | the function starts the catalog is seeded with |
| `tests/`, `tools/run_test.ps1` | play scripts and the unattended run that plays them |
| `pcrecomp/` | the toolkit (submodule) |
| `docs/RECON.md` | what the exe is |
| `docs/bringup.md` | the walls, newest last |
| `work/`, `src/recomp/gen/` | generated, never committed |

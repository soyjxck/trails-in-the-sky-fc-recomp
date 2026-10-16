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
battles, with music and sound effects running.
Each of those is a scripted, unattended test run that ends with the game
closing normally (docs/bringup.md, 9).

Played by hand in a window with keyboard and sound, and it plays, the logo
movie included. It plays the stock game and a modded copy alike (below).

**Off Windows: in game.** The same lifted C also builds for 64-bit
Linux and for the Nintendo Switch (`tools/build_portable.sh`, `src/portable`),
on a host that answers the Windows calls itself instead of passing them to
Windows, Direct3D 9 included (on OpenGL ES 3). There the game starts and
plays, on Linux and on the Switch (under Ryujinx and on a console), with a
controller, with music, and with SoraVoice's voices if that mod is installed.
The logo and opening movies play too. The game's pointer shows only while a
pointer is used: a mouse, or the Switch's touch screen (touch a menu item to
choose it). The keyboard is not there yet (docs/bringup.md, 13 to 21).

On a console most of the game runs at 60 frames a second; the busiest towns
(a festival crowd, say) still fall below it, and the Frame Rate row of the
game's OPTION screen holds an even 30 for those who would rather have that.
`trails.log` beside the program says every ten seconds where a frame's time
went, and `--profile FILE` samples the game's thread, there and on Linux
(`tools/profile_portable.py` names the result; docs/bringup.md, 23 to 27).

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
| `--game DIR` | the game's folder (default: `Trails in the Sky FC` beside the build); a modded copy is one too |
| `--mods DIR` | mods as loose files (default: a `mods` folder beside the exe; `src/mods/vfs.c` has the layout) |
| `--voice-log` | say each voiced line and its file as it is spoken |
| `--savedir DIR` | use DIR as the Saved Games folder, leaving your own saves and settings alone |
| `--unattended` | for tests: window off screen and never activated, a private mouse pointer, Direct3D kept windowed |
| `--active` | keep the game's clock running while its window is not the active one |
| `--silent` | mute the game (its sound code still runs) |
| `--shots DIR`, `--shot-every N` | save the back buffer as a PNG every N frames |
| `--script FILE` | play a script of button presses (`tests/*.txt`; `src/runtime/host.c` has the format) |
| `--renderer d3d9`, `vulkan`, `d3d12` | what draws: Windows' Direct3D 9, Vulkan (DXVK's 32-bit `d3d9.dll` in a `dxvk` folder beside the exe), or Direct3D 12 |
| `--fps N` | frames a second: 30, 60, 120, 144... (default 120; 0 = no limit; above 125, the game's own ceiling, it runs on shorter time steps than it was made for) |
| `--watchdog S` | report where the game is and stop after S seconds |
| `--profile FILE` | sample the main thread and write where its time went |
| `--frame-times FILE` | write every frame's length, and how much of it the main thread ran |
| `--count-natives [N]`, `--slow-calls MS` | what the game calls outside its own code (the N most called), and which calls are slow |
| `--d3dx-dump DIR` | what the game asks of D3DX: shader sources and what was assembled from them, written to DIR; pictures, surface copies and meshes, listed in the log |
| `--calltrace FILE`, `--watch VA`, ... | pcrecomp's bring-up instruments (`trails.exe --help`) |

`play.cmd` is `build\trails.exe --run` with the log kept in `run\play.log`,
and passes its own arguments on: `play.cmd --fps 60 --renderer vulkan`.

### Settings

**F2** in the game, or both sticks of a pad pressed in, opens the port's
settings menu over it: resolution; a window, a borderless window or full
screen; vertical sync, anti-aliasing, the renderer, the frame rate, texture
filtering, shadows, the mouse on or off, the movies. A change is saved as it
is made. The frame rate and the mouse change at once; the rest take effect
the next time the game starts.

All of it is in one file, `%USERPROFILE%\Saved Games\FALCOM\ED6\config.ini`
(with `--savedir DIR`, `DIR\FALCOM\ED6\config.ini`; the log's `[settings]`
line says where it is). It can be edited by hand while the game is closed:
the game writes its own keys back as it leaves. The game's launcher
(`Config_DX9.exe`) is not needed for any of it.

| key | |
|---|---|
| `[Video] WidthDX9`, `HeightDX9` | the picture's size: the window's, or the screen mode in full screen |
| `[Video] WindowMode` | 1 a window, 0 full screen |
| `[Video] WaitVSync` | 1 waits for the screen before each frame |
| `[Video] FullAntiAlias` | samples: 0, 2, 4 or 8 |
| `[Video] TextureFilter` | 1 nearest, 2 linear, 3 anisotropic |
| `[Video] UseMipmap` | 1 uses mipmaps |
| `[Video] ShadowQuality` | 0 none, 1 simple, 2 full |
| `[Video] LogoPlayOff`, `OpeningPlayOff` | 1 skips that movie |
| `[Recomp] Renderer` | `d3d9`, `vulkan` or `d3d12`: the port's own key |
| `[Recomp] Fps` | frames a second, 0 = no limit (default 120): the port's own key, also set by Frame Rate in the game's OPTION screen |
| `[Recomp] Borderless` | 1, with `WindowMode=1`: a window without a frame that covers the screen, the picture stretched to it |
| `[Recomp] Mouse` | 0: no pointer, and the mouse does nothing in the game (for a pad or the keyboard alone) |

`--renderer` and `--fps` on the command line win over the file for that run.
Music, sound effects, voices, the camera and the mini map are in the game's
own OPTION screen. Its Frame Rate row is the same setting as the menu's frame
rate: Uncapped, 120, 60 or 30 FPS Limit (on the Switch 60 or 30), in force
at once.

### Linux and the Switch

The same lifted C builds for both, from a Linux shell (WSL will do), once
the lift above has been made on Windows:

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

On Linux, run `trails-portable` from the game's folder:
`cd "Trails in the Sky FC" && ../build-linux/trails-portable ed6_win_DX9.exe`
(run without the exe's name, it lists its options). The settings and saves
go in a `save` folder there, or in the one `--saves DIR` names.

On the Switch (a console with homebrew, or Ryujinx), make a folder on the SD
card and put the game in it:

```
/switch/trails/trails.nro
/switch/trails/game/          everything in the game's folder (ed6_win_DX9.exe and the rest)
```

The saves and settings go in `save/` beside it, the log in `trails.log`.
Start it from the Homebrew Menu opened through a game (hold R while you
start any game), not from the Album, which gives a program too little
memory. Any folder works the same way, so a modded copy can sit in, say,
`/switch/trails_modded/` with its own `trails.nro`. Options a command line
would carry go in `args.txt` beside the program, on one line (`--fps 30`,
`--voice-log`, `--profile sdmc:/switch/trails/profile.txt`).
`trails-check.nro`, built beside it, says what the game needs of the
console and the card, and whether it is there.

### A modded copy of the game

`--game` takes any copy of the game's folder, and so do the Linux and Switch
builds. What a mod puts in that folder is used as it is found there:
replaced archives (`ED6_DT*.dat`), music (`BGM`), movies (`ed6_logo.avi`,
`ed6_op.avi`), and SoraVoice (`voice\scena`, `voice\ogg`), whose lines the
port speaks itself. A mod's DLLs are not loaded: `dinput8.dll`, `d3d9.dll`
and the like are made for the original exe, and the port uses Windows' own
(the log says so for each one it finds).
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
| `src/ui/settings_menu.cpp` | the settings menu (Dear ImGui, in `third_party/imgui`, on the game's own Direct3D device) |
| `src/mods/voice.c` | SoraVoice's voices, for every host; `src/runtime/voice_out.c` and `src/portable/voice_out.c` read and play the files |
| `src/common/crt.c` | pieces of the game's C runtime done by the host, on every host |
| `src/common/hot.c` | the game's busiest functions (collision tests and walks, sorted triangles, shader constants) done natively, each checkable against the original (`--hot-check`, `--no-hot`) |
| `src/common/frame_option.c` | the game's Frame Rate option, as the host's frame rate |
| `tools/switch_check.c` | `trails-check.nro`: what the game needs of a Switch and its card |
| `tools/profile_portable.py` | names for a profile of the portable host |
| `config/function_starts.json` | the function starts the catalog is seeded with |
| `tests/`, `tools/run_test.ps1` | play scripts and the unattended run that plays them |
| `pcrecomp/` | the toolkit (submodule) |
| `docs/RECON.md` | what the exe is |
| `docs/bringup.md` | the walls, newest last |
| `work/`, `src/recomp/gen/` | generated, never committed |

## Legal

This project is not affiliated with or endorsed by Nihon Falcom or XSEED
Games. *The Legend of Heroes: Trails in the Sky* and all of its assets belong
to their owners. None are included, and the port only runs from a copy of
the game you own. Lifting the game makes a derivative work that carries the
game's own licence, which is why neither the game nor the lifted C is here:
both are made on your machine from your own copy.

The code in this repository is under the [MIT License](LICENSE). It builds on
pcrecomp (MIT) and includes Dear ImGui (MIT); see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), which also lists what the
Linux and Switch builds link.

## Credits

- [pcrecomp](https://github.com/sp00nznet/pcrecomp) by sp00nznet: the
  `lift32` lifter, the `native32` host and the `win32hle` layer that the
  Linux and Switch builds stand on. This port's changes to it are in the
  [soyjxck/pcrecomp](https://github.com/soyjxck/pcrecomp) fork, the
  `pcrecomp/` submodule, which also carries Steve Albrechtsen's guest
  address space for 64-bit hosts.
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut: the settings
  menu.
- [SoraVoice](https://github.com/ZhenjianYang/SoraVoice) by ZhenjianYang:
  the voice mod whose files the port reads and plays itself.
- [DXVK](https://github.com/doitsujin/dxvk): the Vulkan renderer, if you put
  its `d3d9.dll` beside the exe.
- [devkitPro](https://devkitpro.org) and libnx: the Switch toolchain, and
  Ryujinx, where the Switch build was brought up before it met a console.

# Third-party notices

## In this repository

- **pcrecomp** (the `pcrecomp/` submodule, the
  [soyjxck/pcrecomp](https://github.com/soyjxck/pcrecomp) fork of
  [sp00nznet/pcrecomp](https://github.com/sp00nznet/pcrecomp)): MIT License,
  Copyright (c) 2026 sp00nz. The fork's own changes are under the same
  licence; its `LICENSE` has the text.
- **Dear ImGui** (`third_party/imgui`): MIT License, Copyright (c) 2014-2026
  Omar Cornut. `third_party/imgui/LICENSE.txt` has the text.

## Linked into a build, not in this repository

The Linux and Switch builds link these libraries, from the system or from
devkitPro's portlibs. A program built with them carries their licences:

- SDL2 and SDL2_ttf (zlib License), with FreeType (FreeType License);
- Mesa, as libEGL, libGLESv2 and libglapi (MIT License), on the Switch;
- libvorbis and libogg (BSD 3-Clause License);
- FFmpeg's libavformat, libavcodec, libswscale, libswresample and libavutil
  (GNU LGPL 2.1 or later, or the GPL where a build of it enables GPL parts;
  check the one you link), with dav1d (BSD 2-Clause), bzip2 and zlib, for
  the movies;
- libnx (ISC License), on the Switch.

## Used if you put it there, not in this repository

- **DXVK**'s 32-bit `d3d9.dll`, in a `dxvk` folder beside the Windows exe,
  for `--renderer vulkan` (zlib License).
- **SoraVoice** ([ZhenjianYang/SoraVoice](https://github.com/ZhenjianYang/SoraVoice)),
  installed in the game's folder: its voice scripts and files are read as
  they are. Its DLL is not loaded.

## The game

*The Legend of Heroes: Trails in the Sky* is Nihon Falcom's. None of its
files, and none of the C lifted from its executable, are in this repository.

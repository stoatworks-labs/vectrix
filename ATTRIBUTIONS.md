# Attributions

Vectrix is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

### OpenFX image effect plug-in API

<https://github.com/AcademySoftwareFoundation/openfx>  
Licence: BSD-3-Clause  
Copyright: OpenFX and contributors to the OpenFX project

Vendored at external/openfx — a git submodule in resolume-ofx-bridge, a copy of the headers and Support library elsewhere.

The plugin ABI for the DaVinci Resolve and Nuke side of the same effects, so one core renders through both hosts.

### stb_vorbis

<https://github.com/nothings/stb>  
Licence: MIT or Public Domain (dual, at your option)  
Copyright: Sean Barrett

Single-file decoder vendored under external/stb/ and compiled into one translation unit.

Decodes Ogg Vorbis, with no build dependency at all -- the alternative is libvorbis and two more libraries.

### dr_libs

<https://github.com/mackron/dr_libs>  
Licence: MIT-0 or Public Domain (dual, at your option)  
Copyright: David Reid

Single-header decoders vendored under external/dr_libs/ and compiled into one translation unit.

Decodes WAV, MP3 and FLAC. A plugin that plays oscilloscope music has to read the files people actually have, and these cost nothing to build.

### Opus

<https://gitlab.xiph.org/xiph/opus>  
Licence: BSD-3-Clause  
Copyright: Xiph.Org Foundation, Skype Limited, Octasic, Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding, Erik de Castro Lopo

Git submodule under external/opus/, built from source by CMake.

Decodes Opus audio. Same reason as libogg for building it from source.

### libogg

<https://gitlab.xiph.org/xiph/ogg>  
Licence: BSD-3-Clause  
Copyright: Xiph.Org Foundation

Git submodule under external/ogg/, built from source by CMake.

The container Opus streams arrive in. Built from source rather than taken from a package manager because a per-architecture binary cannot produce a universal macOS build.

### opusfile

<https://gitlab.xiph.org/xiph/opusfile>  
Licence: BSD-3-Clause  
Copyright: Xiph.Org Foundation

Git submodule under external/opusfile/. Its own CMakeLists is not used -- four of its C files are compiled directly, with HTTP support disabled, because upstream's build calls find_package for Ogg and Opus and cannot see in-tree targets.

Turns libogg and libopus into something that can open a .opus file.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.

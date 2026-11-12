# Xpeccy+

A ZX Spectrum emulator with a built-in debugger.

Xpeccy+ is a fork of [Xpeccy](https://github.com/samstyle/Xpeccy) by SAM style. All the
emulation code comes from that project. This fork does not claim authorship of it.

Current version: **2026.9**. Forked from upstream build `20260807`.

Windows (x64, ARM64 for Windows 11 on ARM, and legacy x64 and x86 builds for Windows 7 and
8.1), Linux and macOS are built by CI on every nightly and off every release tag. The macOS
build is arm64 only, runs on macOS 13 and later and carries no developer signature, so a
downloaded DMG has to be allowed through Gatekeeper by hand.

## Why a fork

Upstream Xpeccy is a multi-platform emulator: ZX Spectrum and clones, plus MSX, Game Boy,
NES, Commodore 64, BK0010, IBM PC and more. Its focus is to support more machines.

This fork narrows the focus to the ZX Spectrum, and spends the effort on the parts a user
touches every day. That is a different plan for the same code, so it is easier to follow it
in a separate project than to steer one project in two directions.

Xpeccy+ is also open to other people's work. The aim is a good ZX emulator built together,
so patches, ideas and bug reports are welcome.

## Goals

- **ZX Spectrum only.** Since 2026.4 the other machines are not built, and since 2026.5
  their code is gone from the tree.
- **Works out of the box.** Releases come as a bundle with configuration and ROM images, so
  the emulator is ready to use right after unpacking.
- **Comfortable to play with.** Better gamepad support, and a smoother way to work with
  tapes and disks.
- **Fewer loading problems.** Fixes for RZX and for non-standard TZX loaders.
- **A debugger worth using.** Interface work and more tools for people who develop for the
  machine, not only for those who poke at it.
- **A modern build.** Qt 6 and a current SDL, dropping the old paths behind.
- **Documentation** that actually explains things.

Accuracy is not traded away for any of this. Most of the work goes into everything around
the emulation core - but where the core itself was wrong, it was fixed.

## What is already different

On top of upstream build `20260807`:

- **Timing.** Memory contention on the 48K, 128K, +2 and +2A/+3 matches a real machine, the
  floating bus and ULA snow are there (a game that snows says so), timing tests come out as
  photographed on real machines, and frames are handed over at the machine's real rate rather
  than on a 20 ms timer.
- **The ZX Spectrum Next** from its FPGA core: NextZXOS on a card of its own, Wi-Fi
  included. And divMMC with esxDOS on the 48K to the +3 and the Pentagons.
- **Input lag** measured with a photosensor and kept low, and run ahead for less still.
- **Ready to use.** Every machine is built in, ROMs and all, so it runs straight out of the
  archive, and what you change is kept for that machine. macOS builds included. A newer
  version is announced, and on Windows and with the AppImage it puts itself in place.
- **A window like any desktop program's.** A menu bar with every command and its key, a
  toolbar of your choosing and a status bar. Hotkeys come in Modern and Classic layouts that
  leave the machine its own keys, with your changes kept on top, and symbol keys type what is
  printed on them.
- **Media that starts itself.** A tape or disk opens and runs, or asks first, from the menu, a
  drop or the command line, on a machine that can run it, straight from a zip, gz or 7z;
  the other sides of a set are found by their names. A Disk manager lists and copies
  what is on a TR-DOS or +3DOS disk, and a host folder can stand in for an SD card or a hard
  disk, keeping what the machine saves there.
- **Tapes.** Fast loading that works whatever the loader, every TZX block, a WAV read back
  as the tape it records and a tape written out as one, and a player laid out like a deck.
- **Speed.** Up to three times faster with nothing emulated differently - fast forward at
  x18 to x30 on most machines. The whole machine steps from 1/8 to x16, and the CPU alone
  can be overclocked up to x8.
- **Rewind.** Hold a key to go back through the last half minute, picture and sound, and
  keys for slow motion and fast forward.
- **Gamepads.** A pad plays at once as any joystick interface, its buttons bind to keys,
  Kempston or emulator actions, and the keyboard can be a player too.
- **Video recording** to MP4 or MKV through FFmpeg, which the emulator offers to get, RZX
  recording with rewind and bookmarks, and screenshots at the machine's own size, named after
  the game or put into the clipboard.
- **A debugger worth using.** Movable panels, conditional breakpoints that can log, a listing
  that reads like one, with the ROMs and system variables named and labels offered as you
  type, a memory heat map, register layouts to choose from, ZX Evo and Next panels, a screen
  and a sound chip panel that detach into windows of their own, and debugging from VS Code
  with DeZog.
- **Sound.** Made on a CPU core of its own; the FM half of TurboSound runs on ymfm, the
  General Sound comes as the original card or the NeoGS, latency looks after itself, and the
  mix can be filtered.
- **Snapshots.** SZX by default, on every machine, the Next included, coming back exactly
  where it was saved, with the tape and disks inside if you like; `.z80` and `.sna` as well.
  Quick saves are kept for each game, twenty deep, and Resume Last Session picks up where
  you left off.
- **Looks.** Eight interface styles, six border sizes, a window that maximizes and resizes
  with the picture fitted and the sides filled from its edges, VSync for even motion, and an
  optional antiflicker that leaves moving pictures alone.
- **Machines built from their own sources.** ZX Evo, BaseConf and TSConf alike, follows its
  FPGA sources; Scorpion ZS 256 and Profi, two boards each, follow their schematics.
- **Fixes.** Tape loading, disks with loaders of their own, sound that does not click.

See [CHANGELOG.md](CHANGELOG.md) for the full list.

## Build

Requirements: CMake, a C/C++ compiler, Qt (5 or 6), SDL (1.2 or 2), and zlib.

```
mkdir build && cd build
cmake [options] ..
make
```

Options:

| Option | Values | Meaning |
| --- | --- | --- |
| `-DQTVERSION=` | `5` (default) or `6` | Qt version |
| `-DSDL1BUILD=` | `0` (default) or `1` | use SDL 1.2 instead of SDL2 |
| `-DUSEOPENGL=` | `1` (default) or `0` | draw through a QtOpenGL widget |
| `-DUSEQTNETWORK=` | `0` (default) or `1` | QtNetwork support, experimental |
| `-DXRELEASE=` | `0` (default) or `1` | release version string, without `-dev` and build date |
| `-DTRIMDEPLOY=` | `ON` (default) or `OFF` | Windows: drop the Qt plugins the emulator never loads |
| `-DXLOGHOT=` | `OFF` (default) or `ON` | build the log calls on the hot paths - per opcode or dot |

The result is the `xpeccy-plus` executable. On Linux you can also build a package with
`make package`, or install with `make install`.

### Linux

On Debian and Ubuntu `packaging/linux-setup.sh` installs the toolchain (build tools, Qt 5,
SDL2, zlib) and downloads the AppImage tools into `~/.cache/xpeccy-plus-tools`. After that:

```
packaging/linux-setup.sh
packaging/make-appimage.sh
```

`make-appimage.sh` configures, builds, installs into an `AppDir` and runs `linuxdeploy`,
leaving `xpeccy-plus-<version>-linux-x86_64.AppImage` in the build directory. `CLEAN=1`,
`RELEASE=1`, `SRC_DIR`, `BUILD_DIR` and `JOBS` change what it does.

The icon it deploys is `images/xpeccy-plus.png`, which has to keep one of the sizes the
icon theme spec allows - `linuxdeploy` refuses anything else, and 128x128 is what it is.

The build directory defaults to `~/build/xpeccy-plus` rather than to `build/` inside the
sources, because `linuxdeploy` makes symlinks and sets permissions in the `AppDir`, which
a Windows filesystem mounted into WSL cannot do. The sources themselves may live anywhere.

An AppImage carries Qt and SDL but not the C library, so it needs a distribution at least
as new as the one it was built on - built on Ubuntu 22.04 it wants glibc 2.35 or newer.

### Windows

Windows is built and tested here daily. What has to be installed:

| | |
| --- | --- |
| Qt | 5.15.2 with MinGW 8.1 (32 or 64 bit), or 6.6.2 with MinGW 11.2 (64 bit) |
| MinGW, CMake | come with the Qt installer, under `C:\Qt\Tools` |
| zlib | comes with MinGW and is linked statically |
| SDL2 | fetched by the script below |

Qt 6 has no 32-bit Windows build at all, so a 32-bit binary means Qt 5.

```
packaging\fetch-deps.ps1
packaging\make-dist.ps1 -Preset qt6-x64 -Zip
```

The first downloads SDL2 into `build\deps`. Its version is pinned in
`packaging\deps.json` and the archive is checked against a stored SHA-256, so a build of a
given tag stays reproducible; `-Update` moves the pin to the newest upstream release and
rewrites the manifest, which is meant to be committed on its own.

The second configures, builds, and stages a ready-to-run folder under `build\dist` -
binary, Qt and SDL runtime, `config\`, and the docs - optionally zipped. Presets are
`qt6-x64` (default, the `win-x64` package), `qt5-x64` and `qt5-x86` (`win-x64-legacy` and
`win-x86-legacy`, for Windows 7 and 8.1); useful flags are `-Clean`, `-Release`,
`-Zip` and `-Full`. Paths to the toolchains live in a table at the top of the script.

`qt6-arm64` (`win-arm64`) builds on an ARM64 machine with MSYS2's CLANGARM64
environment, which gives clang, Qt, SDL2 and zlib - qt.io has no MinGW Qt for ARM64.
The packages are listed in `.github/workflows/windows.yml`; the script needs only
`fetch-deps.ps1` for curl.

Assembling that folder is CMake's job, not the script's: `cmake --install <build-dir>
--prefix <dir>` produces the same result. `windeployqt` runs as an install step, after
which the Qt plugins this emulator never loads are removed - see
`cmake/windeploy.cmake.in`, which lists what goes and why. Build with `-DTRIMDEPLOY=OFF`
to keep everything `windeployqt` copied. The same step then adds the DLLs the program, Qt
and its plugins import that `windeployqt` leaves out - none with the Qt installer's MinGW;
with MSYS2's Qt, its libc++, ICU, harfbuzz and the rest.

`version.h` is generated by CMake from `cmake/version.h.in` and is not stored in the
sources. A development build carries the build date as SemVer build metadata and shows
the version in brackets in the window title - `Xpeccy+ (2026.10-dev+20261102)`, against
`Xpeccy+ (2026.10)` for a release build.

## Bundled ROMs

Releases ship with `config/`, so the emulator works right after unpacking. Machines, screen
layouts, palettes, shaders, styles and keymaps are built into the binary; `config/` carries
the ROM images (`config/roms/`), and a file of the same name dropped in there replaces a
built-in one, and the files below.

The images are firmware of the emulated machines and are **not** covered by the MIT license
of this project - they stay under the terms of their own copyright holders. Details:
[`config/roms/LICENSE`](config/roms/LICENSE), the canonical Amstrad notice in
[`config/roms/AMSTRAD.copyright`](config/roms/AMSTRAD.copyright), and a per-file table of
where each image came from in [`config/roms/PROVENANCE.md`](config/roms/PROVENANCE.md).
No image was modified; the copyright messages inside them are intact.

`config/esxdos/` holds esxDOS 0.8.5 to 0.8.9, the divMMC's firmware and the files its card
needs, one folder a release, unmodified. esxDOS is (c) Papaya Dezign and is bundled with their permission - many thanks to
Miguel Guerreiro / Papaya Dezign. It keeps its own terms: see
[`config/esxdos/LICENSE`](config/esxdos/LICENSE).

`config/boot.$B` is bundled for the same reason: TR-DOS images that carry no boot file of
their own get it appended when they are opened. It is Dimon boot 2024 by Dmitry Yurinov and
keeps its own terms, like the ROM images.

`config/labels/` holds the lists that name the routines of known ROMs in the Debugger: data
files the emulator reads when it starts, not built into it, each under the terms of its
source. `48.labels`, the 48K BASIC's, comes from [The Complete Spectrum ROM
Disassembly](https://github.com/skoolkid/rom) by Richard Dymond, under CC BY-SA 3.0. The
names and data ranges of the 128K and +2A/+3 ROMs come from the ROM annotations of
[Klive IDE](https://github.com/Dotneteer/kliveide) by Istvan Novak, under the MIT license.
TR-DOS's come from [TR-DOS Disassembled](https://github.com/programandala-net/tr-dos) by
Marcos Cruz, used with his permission. Sources and terms of every list:
[`config/labels/README.md`](config/labels/README.md).

The debugger's default font, DejaVu Sans Mono 2.37, is built into the binary so it looks the
same on every platform. It keeps its own terms too - see [`LICENSE_DEJAVU`](LICENSE_DEJAVU).

The FM half of the YM2203, the chip a TurboSound FM board carries, is emulated by
[ymfm](https://github.com/aaronsgiles/ymfm) by Aaron Giles, under the BSD 3-Clause license -
see [`LICENSE_YMFM`](LICENSE_YMFM). Its sources are in `src/libxpeccy/sound/ymfm/`.

The virtual keyboard's lettering is drawn with FreeType 2.14.3, under the FreeType License -
see [`LICENSE_FREETYPE`](LICENSE_FREETYPE). Its sources are in `thirdparty/freetype/`.
Portions of this software are copyright (C) 2026 The FreeType Project
(https://freetype.org). All rights reserved.

Images packed in a .7z archive are opened with the 7z reader of the LZMA SDK 26.05 by Igor
Pavlov, which is in the public domain - see [`LICENSE_LZMA`](LICENSE_LZMA). Its sources are in
`thirdparty/lzma/`.

The lettering itself is "Xpeccy Keys Sans", cut down from Liberation Sans Bold 2.1.5 and
built into the binary. It is under the SIL Open Font License 1.1 - see
[`LICENSE_LIBERATION`](LICENSE_LIBERATION).

Downloads (the ZX Spectrum Next's card, FFmpeg on Windows) go through
[libcurl](https://curl.se/libcurl/), under the curl license - see
[`LICENSE_CURL`](LICENSE_CURL). The Windows builds carry it, built in; macOS and Linux use
the system's own.

FFmpeg is not shipped with Xpeccy+. Video recording runs it as a separate program, and the
copy the emulator fetches on Windows is a GPL build from
[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds), kept under its own terms.

Some icons come from Oxygen (LGPL v3), FatCow Farm-Fresh and Fugue (CC BY 3.0), Twemoji
(CC BY 4.0) and the FFmpeg logo (public domain); which file is whose is in
[`LICENSE_ICONS`](LICENSE_ICONS).

## Credits and license

Xpeccy was written by **SAM style** (<https://github.com/samstyle/Xpeccy>) and is
distributed under the MIT license. The original copyright is kept intact - see
`LICENSE_eng` (`LICENSE_rus` for the Russian text). Xpeccy+ is released under the same
license, and is maintained by Oleksandr ".koval" Kovalchuk.

The Z80 timing test under `tools/z80test/` derives from Fuse by Philip Kendall and
carries Fuse's GNU GPL v2-or-later. It is a development tool: none of it is built into
the emulator, which stays MIT.

The original build instructions are kept as `README.upstream`, together with the links to
the original documentation. They describe upstream's process, which already differs from
this fork's.

## Contact

Bug reports, ideas and patches: [GitHub issues](https://github.com/dotkoval/xpeccy-plus/issues)
and pull requests. The [wiki](https://github.com/dotkoval/xpeccy-plus/wiki) has the roadmap.

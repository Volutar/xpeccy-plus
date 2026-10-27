# Regression tests

`regress.py` runs a staged build headless over a suite of images and compares what the
machine did with a reference: each case prints `--bench-hash` hashes of every frame, every
sound sample, the RAM and the CPU state, so a case either ran exactly as before or it did not.

```
python tests/regress.py run                 # every suite against the reference
python tests/regress.py run timing -k 'float*'
python tests/regress.py run --twice         # also flag a case whose two runs differ
python tests/regress.py bless timing        # take the current results as the reference
python tests/regress.py list
```

- **The build** is the newest dev folder in `build/dist`, or `--app <folder>`.
- **The images and the references** are not in this repository - most of the images are not
  ours to publish. `XPECCY_CORPUS` (or `--corpus`) is the folder the suite paths are relative
  to, `XPECCY_GOLDEN` (or `--golden`) holds one `<suite>.json` per suite.
- **Suites** are `suites/<name>.txt`, one case a line:
  `name machine auto|open skip frames file [options]`. `auto` starts the image with
  `--autostart`, `open` just opens it (a snapshot). `name machine boot skip frames [options]`
  takes no file and runs the firmware from power-on. The machine runs `skip` frames
  unhashed, then `frames` hashed.
  `list` ends with the number of cases per machine, marking any under 3.
- **Options** set the case up; anything else is passed to the emulator (`--bench-full`):
  - `cfg:SECTION.key=value` - a line in the case's `config.conf` (`cfg:TAPE.fast=yes`)
  - `mac:section.key=value` - a line in `machines/<machine>.conf`, the patch over the
    definition (`mac:sound.gs=yes`)
  - `nvram:FILE` - becomes `nvram/<machine>.cmos`
  - `sd:PATH`, `hdd:PATH` - an image or a folder as the SD card or the IDE master. It is
    copied first, and a folder's files get one fixed mtime: the FAT served for a folder
    carries the host's dates, which a fresh checkout would change.
  - `put:PATH=FILE` - lays a file over the card copied just before (`put:bin/autoexec.bat=...`)
  - `keys:F:KEYS,...` - `--bench-keys`: keys pressed at frame F counted from the start, held
    4 frames (`/N` for N). Keys are matrix names as autostart types them (`E` Enter, `C`
    Caps, `S` Symbol, `_` Space, `C6` down); `joy:fire+up` and `joyb:...` are the two
    joysticks, with the row names of a `.pad` (`up`... `fire`, `fire2`..`fire4`), `nmi` the
    NMI button.
- Every case gets a fresh copy of the build's `config/`, so nothing carries over between
  cases; they run in parallel (`-j`, half the CPU threads by default), the longest first.

A difference is not a failure by itself: a change meant to alter timing moves the hashes of
the machines it touches, and those are blessed once the change is checked against real
hardware. A change that should alter nothing - a refactor, a speed-up - must leave every
hash where it was.

## System/Next archive reader

The Next's card is unpacked inside the program, so this target tests that reader
(`src/xcore/sysnext_unpack.cpp`) on its own: Qt Core and zlib, no emulator, and no
external archiver. It is a project of its own rather than a bench of the quick tier,
which builds libxpeccy with no Qt at all.

```
cmake -S tests/sysnext -B build/out/sysnext -DQTVERSION=6 -DCMAKE_PREFIX_PATH=<Qt prefix>
cmake --build build/out/sysnext
ctest --test-dir build/out/sysnext --output-on-failure
```

`-DQTVERSION=5` builds it against Qt 5. The fixtures cover USTAR, GNU and pax names and
sizes, stored and deflated entries, byte progress, cancellation, corrupt and truncated
gzip, tar and zip data, a zip entry that unpacks past its stated size, paths that would
land outside the folder, links and special files, versions of the formats that are refused
(ZIP64, encrypted zips, sparse tar), and a destination that is not empty.

## Quick tier

Label imports and expressions also have an optional Qt test, built against the program's
own readers and evaluator. It covers the Next's 8K pages, MMU changes, physical watches,
save/reload and the existing 16K format:

```
cmake -S tests -B build/out/tests-qt -DXPECCY_QT_TESTS=ON -DQTVERSION=6 -DCMAKE_PREFIX_PATH=<Qt prefix> -DSDL2_INCLUDE_DIR=<SDL2 include>
cmake --build build/out/tests-qt --target labels-check
ctest --test-dir build/out/tests-qt --output-on-failure
```

Use the same compiler and zlib options as the core tests. Qt's DLLs must be on PATH when
running the test on Windows. The ordinary quick tier does not need Qt.

The update check's logic (`src/xcore/update.cpp`) has a Qt Core test of its own, over
GitHub's answers saved in `tests/update/data`: each package's file on both channels,
which build is newer, and the changelog entries a build lacks:

```
cmake -S tests/update -B build/out/tests-update -DQTVERSION=6 -DCMAKE_PREFIX_PATH=<Qt prefix>
cmake --build build/out/tests-update
ctest --test-dir build/out/tests-update --output-on-failure
```

```
python tests/regress.py quick                 # build, run every bench, PASS/FAIL per case
python tests/regress.py quick tsconf -k 't1*'
python tests/regress.py quick tape --bless    # take the current output as expected
python tests/core/mkbins.py                   # reassemble the Z80 programs (needs sjasmplus)
```

Seconds, no Qt and no app: `tests/CMakeLists.txt` is a project of its own that builds
libxpeccy as a static library and the benches in `core/` against it, into
`build/out/tests` (on Windows with the qt6-x64 MinGW of `packaging/make-dist.ps1`, or
`--mingw`). Each bench's output is compared, case by case, with `expected.txt` beside it.

- **z80** - `tools/z80test`, Fuse's per-instruction tests.
- **zxnext** - eight independent 8K MMU slots, CPU and physical breakpoints, instruction
  previews across divMMC traps and Multiface NMI entry, and heat-map operand accesses.
  Also switches from a Next to a 48K and checks that the Next's CPU hooks are gone.
- **tsconf**, **baseconf** - small Z80 programs on the TSConf and BaseConf cores, each
  checking one thing the hardware's RTL says; `cases.txt` lists them. The programs are
  committed assembled in `bin/` beside their sources in `asm/`, so a run needs no assembler.
- **tape** - TAP and TZX images written by `core/tape/mktape.py`: the blocks as read, the
  bytes decoded back, the pulse lengths a loader measures, and a TAP -> WAV -> TAP round trip.
- **pok** - the `.pok` reader over the hand-made files in `core/pok/files` (line ends,
  padding, every way a trainer goes bad), then pokes going in and out of a 16K, a 48K and
  a 128K: the original check, a page poked while another is paged in, undo, a rollback.
  `pok-bench -sweep <files>` prints one line a file, for a run over a collection.

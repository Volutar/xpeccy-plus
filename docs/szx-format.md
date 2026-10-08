# SZX snapshots in Xpeccy+

How Xpeccy+ reads and writes zx-state (`.szx`) files: what it takes from each standard block,
what it adds, and where. The format is Spectaculator's, version 1.5, maintained by Jonathan
Needle ([the specification](http://www.spectaculator.com/redirect.php?id=szx)); this page only covers what the spec leaves open and what Xpeccy+ keeps beyond it. The code is
`src/libxpeccy/filetypes/szx.c` (the standard blocks) and `szx_ext.c` (our own data); where
this page and the code disagree, the code is right.

It is written so that another emulator can read our files, write files we read, and - for the
two machines the spec has no blocks for, TSConf and TurboSound FM - agree on a shared block.
The last section is that proposal.

## Two kinds of file

- **A machine the spec names** - 16K, 48K, 128K, +2, +2A, +3, Pentagon 128/512/1024,
  Scorpion - is written with its own machine id and the standard blocks, and any reader that
  knows SZX loads it. What the standard blocks have no room for (a second and third sound chip,
  the FM half, the exact raster phase, the disk controller in mid-command) goes into the
  creator block, where other readers skip it.
- **A machine it cannot name** - ZX Evolution (BaseConf, TSConf), ATM Turbo 2+, Profi,
  Phoenix, ALF - is written with machine id `0x80`. The standard blocks are still there (CPU,
  memory, AY, disks, tape), but only Xpeccy+ knows which machine to build for it, so other
  readers refuse the file. Ids 0..16 are the spec's; `0x80` was picked well clear of them.

Both are loaded on the model they name. A Pentagon file opened while a 128K is running
switches the machine to a Pentagon first, since the frame position in the file means nothing on
a machine with another frame. Only a machine of the user's own (one saved under its own id)
on the same core is left as it is. Settings the file records and the machine does not match -
ULA timings, board issue, ULA+, an AY, a Beta 128, a Covox, a device not emulated - are not
changed: the user is told and the load goes on. A joystick, a mouse and a General Sound are only
logged, as writers record whatever they were set to, used or not.

## Standard blocks

Read means what Xpeccy+ takes from a block; written means what it puts in one.

| Block | Read | Written |
|---|---|---|
| `CRTR` | name and version; `chData` is searched for our data (below) | `Xpeccy+`, the version, our data |
| `Z80R` | all of it, see below | all of it |
| `SPCR` | border, `7FFD`, `1FFD` (`EFF7` on a Pentagon 1024), the `FE` latch | the same |
| `RAMP` | every page, compressed or not | every page, compressed |
| `AY` | chip 0's sixteen registers and the selected register | the same |
| `PLTT` | ULA+ state, when the machine has ULA+ | when it has ULA+ |
| `B128` | the WD1793 registers and the system register, see below | the same |
| `BDSK`, `DSK` | the image, linked or embedded | a link |
| `+3` | nothing: the drives are the machine's | the drive count |
| `TAPE` | the image, linked or embedded, and the block it stood at | a link and the block |
| `COVX` | the level | the level |
| `ROM` | a custom ROM, put in place of the machine's | the custom ROM, when one was put in |
| `GS`, `GSRP` | the General Sound's CPU, ports and RAM | the same |
| `JOY`, `KEYB`, `AMXM` | compared with the machine, never taken: they are settings | what the machine has |
| `IF1`, `MDRV`, `IF2R`, `DRUM`, `MFCE` | skipped: not emulated; the user is told | - |
| any other | skipped | - |

What the spec leaves open, and how it is read here:

- **`Z80R.dwCyclesStart`** is where the machine stands in its frame, in T from the start of the
  frame interrupt; the raster, the contention phase and the floating bus are put there. A
  `.z80` or `.sna` cannot say this, which is why SZX is the default format. The T is the
  CPU's at the speed it runs when the snapshot is taken, turbo included. Our own `RAY `
  record (below) carries the exact dot as well.
- **`Z80R.chHoldIntReqCycles`** is what is left of the INT pulse, in T. A pulse not yet taken
  can still be taken after the load.
- **`Z80R.chFlags`**: `SUPPRESS_INTS` is "the last instruction was EI", `HALTED` the HALT
  state, `FSET` "the last instruction changed the flags" (for SCF/CCF's undocumented bits).
  `SUPPRESS_INTS` and `HALTED` exclude each other.
- **IFF1 and IFF2**: inside an NMI handler IFF1 is 0 and IFF2 1, and the RETN at its end
  enables interrupts with no EI. A reader that gates INT on IFF1 alone handles this anyway;
  one that keeps a separate "EI was last" flag has to set it from IFF1 *or* IFF2.
- **`Z80R.wMemPtr`** is the Z80's internal WZ register; it decides bits 3 and 5 of the flags
  after `BIT n,(HL)`.
- **`AY.chCurrentRegister`** is the byte last written to `FFFD`, all eight bits. An AY or YM
  only answers when the upper four bits are 0000: any other number deselects it until the next
  one, so writes go nowhere and a read sees the floating bus. A reader should keep the byte, not
  mask it to four bits.
- **`B128`**: the system register (`FF`) is set, not written. Written, its MR bit going high
  would start the RESTORE a real WD1793 runs coming out of reset, which the machine saved had
  finished long ago. The `CONNECTED` flag is not looked at on reading, as some writers leave it
  clear on a Pentagon.
- **`RAMP` on a Pentagon 512/1024** numbers pages 0..31 and 0..63 the way `7FFD` and `EFF7`
  page them.
- **Links** in `TAPE`, `BDSK` and `DSK` are written as full paths in the host's own form. On
  reading, a link that does not resolve is looked for beside the snapshot by its file name, and
  an image already in the drive is left there, so a disk written to since is not lost.

## Our data: the creator block's `chData`

Some readers report every block they do not know to the user as an error, even though the
load succeeds. So Xpeccy+ adds no blocks of its own: its data goes in `CRTR.chData`, which the
spec gives to "the program that created the file" and every reader skips.

```
CRTR block
  char  szCreator[32]     "Xpeccy+"
  word  chMajorVersion
  word  chMinorVersion
  chData:
    char  tag[8]          "Xpeccy+\0"
    record...             to the end of the block
```

A record is shaped like a block:

```
  dword id                four characters, like a block id
  dword len
  byte  data[len]
```

Most records are a list of named **fields**:

```
  byte  keylen
  char  key[keylen]       not terminated
  word  len
  byte  data[len]
```

A reader takes a field only when it has one of that name **and** that size, and leaves the
rest as the standard blocks set it. That is what lets a build read a file of an older or newer
one: a field that changed size is skipped, not misread. An unknown record is skipped whole.

**What a field holds is Xpeccy+'s own memory layout** - the C structure the emulator runs on,
not a description of the hardware. That is fine for a snapshot that has to put the machine
back exactly, and useless to anyone else. Only `MACH`, `MAP`, `RAY`, `TS`, `FDCP` and `FM n`
have a layout of their own, written down here (`TS  ` is fields with one byte after them); the
rest are opaque.

### Which machine: `MACH` and id `0x80`

`MACH` is always the first record:

```
  char  core[]            the core's name, zero terminated ("ZX48", "TSLab", "PentEvo"...)
  char  machine[]         the machine's id, zero terminated ("zx48", "evo-tsconf"...)
```

A file with machine id `0x80` is built as the machine `MACH` names. The other records are only
taken when the file's core is the running core; on another core only the sound chips' records
(`TS`, `CHPn`, `FM n`) are, since the ports, flags and video mean something different there.

### The records

| Record | What it holds |
|---|---|
| `MACH` | core and machine id, above |
| `CPU ` | the Z80 beyond `Z80R`: the interrupt request and enable masks, the vector |
| `COMP` | the machine: turbo, the ear input's RC state, the floating bus's last byte, frame and line counters, the core's ports and registers, the CMOS clock, TSConf's own state |
| `MAP ` | the pager's map, 256 entries of 256 bytes each: `byte type` (RAM/ROM/none), `dword page` |
| `VID ` | the video: mode, border, flash, INT pulse, TSConf's video state and line buffer |
| `PAL ` | the palette, on a machine whose palette is its own (ATM, Evo, TSConf, Profi) |
| `RAY ` | `dword`: dots past the frame interrupt, more exact than `dwCyclesStart` |
| `TS  ` | TurboSound: mutes, status-read mode, then `byte`: the selected chip, 0..3 |
| `CHP0`..`CHP3` | each sound chip's counters, envelope, selected register and all 256 register bytes |
| `FM 0`..`FM 3` | a YM2203's FM engine, `dword len` then the bytes; see TurboSound FM |
| `BEEP` | the beeper's level and filter |
| `SAA `, `SDRV` | SAA1099, Soundrive/Covox |
| `DIF `, `FDC ` | the disk interface and its controller |
| `FDCP` | the controller's command in progress: `dword plan` (-1 none), `byte` idle/seek-end bits, `byte` selected drive |
| `FLP0`..`FLP3` | each drive: motor, head, track, position on the track |
| `IDE `, `ATA0`, `ATA1` | the IDE interface and its two devices |
| `SMNV` | SMUC's NVRAM |
| `SDC ` | the SD card |
| `GSX `, `GSCP`, `GSRX` | General Sound beyond `GS`/`GSRP`: its ports, its CPU's hidden state, RAM past the spec's pages |
| `TAPE` | the tape's position inside the block, the level, the pause |

The order of reading matters in one place: the drives and the tape are mounted after the
snapshot is in, so `FLPn`, `DIF ` and `TAPE` wait until their images are back.

## TSConf

TSConf is a ZX Evolution configuration, so its files carry machine id `0x80` and `MACH` names
`TSLab` / `evo-tsconf`. Its state is in three records, all opaque:

- `COMP` - the `#xxAF` ports as the core keeps them (most in its register array, the DMA
  addresses in its 32-bit registers), the cache configuration and the cache's tag RAM, a DMA
  transfer in progress (source, destination, length, burst count, the word in flight and the
  DRAM cycles banked for it), the power-up flag, and the Evo's 256-byte clock RAM.
- `VID ` - the video side: graphics and tile offsets, tile and sprite pages, palette select,
  the INT line and position and the lines it moves on each frame, the values written but not
  yet taken at the start of the blanking, CRAM (512 bytes), SFILE (512 bytes), the tile map
  rows read ahead, and the line buffer half drawn when the snapshot was taken.
- `MAP ` - windows 1-3 exist only in the pager's map (`#11AF`..`#13AF` write it directly), so
  the map is carried as it is rather than rebuilt from ports.

`RAY ` puts the raster back to the dot, which matters here: TSConf moves its INT anywhere in
the frame, and a T is a different number of dots at each of its three speeds.

## TurboSound FM

A TurboSound FM is two YM2203s behind one pair of ports. A write to `FFFD` of `%11111xxx` is
the board's own command, not a register number, and neither chip sees it: bit 0 picks the
chip, bit 1 clear means a read of `FFFD` returns the status, and bit 2 set turns the FM halves
off. Any other byte goes to the selected chip.

How it is kept:

- The standard `AY` block holds chip 0's SSG half, so any SZX reader gets the first chip's
  music, without FM.
- `TS  ` holds the command's state: which chip is selected, the status-read mode, and the
  mutes.
- `CHPn` holds each chip's SSG counters and its 256 register bytes: `0x00`..`0x0F` the SSG,
  `0x10`..`0xFE` the FM registers as last written, `0xFF` the status. The register number in
  `curReg` is all eight bits.
- `FM n` is the FM engine's own state - operator phases, envelope stages and levels, the LFO,
  both timers, the busy flag, the prescaler - in the order of the FM core Xpeccy+ uses. It is
  size-checked like a field, so a build whose engine changed skips it and keeps the registers.

The FM registers alone do not put a note back: they say what was last written, not how far each
envelope has gone. A reader that has only them can restart the notes from the registers, which
is what Xpeccy+ does when `FM n` does not fit.

## A shared block for TSConf and TurboSound FM

What follows is a proposal, not something Xpeccy+ reads yet. Both blocks describe the hardware
at register level, so any emulator can write and read them whatever its own structures.

### `TSFM`: TurboSound and TurboSound FM

```
  byte  chFlags           b0: FM halves fitted (YM2203s, not AYs)
                          b1: a read of FFFD returns the status
                          b2: the FM halves are switched off
  byte  chSelected        0 or 1
  for each of the two chips:
    byte  chCurrentRegister   all eight bits, as written
    byte  chAyRegs[16]        the SSG registers
    byte  chFmRegs[256]       the FM registers as last written; 0x00..0x0F unused
    byte  chPrescaler         the last of 0x2D/0x2E/0x2F written: it is set by the address alone
```

The first chip duplicates the `AY` block, which stays in the file for readers that do not know
this one. Chip-engine state (envelope phases, timers) stays in each emulator's own data, as
above.

### `TSCF`: TSConf

```
  byte  chPorts[256]      the last value written to each #nnAF port, nn = 00..FF
  byte  chLatched[13]     written since the last blank and not yet taken, in the order
                          00 01 02 03 04 05 07 17 18 40 41 44 45 (#nnAF)
  word  wLatchedMask      which of the thirteen are waiting, b0 = the first
  byte  chCram[512]
  byte  chSfile[512]
  word  wCacheTags[256]   b15 valid, page and A13..A9
  DMA in progress:
    byte  chActive
    dword dwSource, dwDestination
    word  wBurstsLeft, wWordsLeft
    word  wWordInFlight
  word  wIntPulseLeft     dots of the INT pulse still to run
  dword dwRayDots         dots past the frame interrupt
  byte  chNvram[256]      the Evo's clock RAM
```

`chPorts` is the core of it: TSConf's registers are write-only, so the last value written is
the state, and an emulator rebuilds its own decoding (pages, offsets, video mode) from them as
it would from the writes. Paging windows 1-3 are in `chPorts` at `#11AF`..`#13AF`. What the
ports do not hold - the palette, the sprite file, the cache, a DMA mid-transfer - follows them.
The frame position is given in dots, as a T is a different number of dots at each of TSConf's
three speeds.

The specification invites other authors to contribute to it, through
[its enquiry address](http://www.spectaculator.com/redirect.php?id=szxenquiry); a block both
sides agree on would go there before any emulator writes it.

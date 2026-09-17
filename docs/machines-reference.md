# Machine reference table

Where each machine's numbers came from, and which of them are still resting on nothing but
convention. **The machines themselves are `res/machines/*.conf`** - those files are what the
emulator runs and what to change; this table does not decide anything. It was written when
they were generated and is kept for the one thing they cannot carry: provenance, and the
open questions.

Sources, in order of weight: the values that shipped after the raster-geometry work
(`config/config.conf` layouts, validated against Spectaculator and Fuse), the shipped profiles,
the cores themselves, and published hardware documentation. Anything that rests on nothing
better than convention is marked **open** and left alone.

---

## 1. Timing and video

`layout` names an entry of the `[VIDEO]` layout table in `config.conf`. Its fields, in file
order, are `full.x:full.y:bord.x:bord.y:blank.x:blank.y:intSize:intpos.y:intpos.x:scr.x:scr.y`,
all in dots; two dots make one CPU tick, so T per line is `full.x / 2` and the INT pulse is
`intSize / 2` ticks.

| machine | CPU, Hz | layout | T/line | lines | T/frame | INT, T | contPattern | contio | contmem | 4t-border | earlyTiming |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ZX Spectrum 48K | 3 500 000 | ZX 48K | 224 | 312 | 69 888 | 32 | 1 (Ferranti) | yes | yes | yes | yes |
| ZX Spectrum 128K / +2 | 3 546 900 | ZX 128K | 228 | 311 | 70 908 | 36 | 1 (Ferranti) | yes | yes | yes | yes |
| ZX Spectrum +2A | 3 546 900 | ZX +2A/+3 | 228 | 311 | 70 908 | 32 **open** | 2 (Amstrad) | no | yes | yes | yes |
| ZX Spectrum +3 | 3 546 900 | ZX +2A/+3 | 228 | 311 | 70 908 | 32 **open** | 2 (Amstrad) | no | yes | yes | yes |
| Pentagon | 3 500 000 | Pentagon | 224 | 320 | 71 680 | 36 | 0 (none) | no | no | no | no |
| Pentagon 1024 SL | 3 500 000 | Pentagon | 224 | 320 | 71 680 | 36 | 0 (none) | no | no | no | no |
| ZS Scorpion 256 | 3 500 000 | Scorpion | 224 | 312 | 69 888 | 36 | 0 (none) | no | no | yes | no |
| Profi | 3 500 000 | Profi | 224 | 312 | 69 888 | 32 **open** | 0 (none) | no | no | no | no |
| ATM Turbo 2+ | 3 500 000 | ATM Turbo 2+ | 224 | 312 | 69 888 | 32 **open** | 0 (none) | no | no | no | no |
| ZXM-Phoenix | 3 500 000 | Pentagon | 224 | 320 | 71 680 | 36 | 0 (none) | no | no | no | no |
| ZX Evolution (BaseConf) | 3 500 000 | Pentagon | 224 | 320 | 71 680 | 36 | 0 (none) | no | no | no | no |
| ZX Evolution (TSConf) | 3 500 000 | TSConf | 224 | 320 | 71 680 | 32 **open** | 0 (none) | no | no | no | no |

Contention patterns are `vid_wait_dots()` in `video/video.c`: 1 is the Ferranti ULA's
`12,11,...,1,0,0,0,0` over banks 1/3/5/7, 2 the Amstrad ASIC's `2,1,0,0,14,...,3` over banks
4-7 and mreq cycles only, 0 is no contention at all.

`floatbus` is what a port nothing answers reads back. The 48K, 128K and +2 get `ula`, the
+2A and +3 `asic`; both were checked against Woodmass's Float48K and Float128K, where the
first of the four bytes lands one tick earlier than fuse prints it because these machines
run early timings. The clones get `attr`, the documented port `#FF` that hands back the
attribute of the cell being shown - not a floating bus, but what this emulator has always
done for them and what a demo written for a Pentagon expects. The +2A/+3 rules (ports
1, 5, ... 4093, paging on, bit 0 forced, the last contended-memory byte between fetches)
are Ast A. Moore and Hikaru's, sky.relative-path.com/zx/floating_bus.html.

The `issue` key is what bit 6 of `#FE` reads back with no tape playing, and only two values
are settled. The 48K/128K/+2 get issue 3 - bit 4 of the last `OUT #FE` - from the World of
Spectrum 48K reference, which also gives issue 2 as bit 4 *or* bit 3; issue 2 is a board
variant of the same machine, so it is a patch the user makes rather than a machine of its own.
The +2A/+3 get `none`, from the 128K reference: "Bit 6 of Port 0xfe of the +2A/+3 does not
show the same dependence on what was written to Port 0xfe as it does on the other machines,
and always returns 0 if there is no signal." Every clone is left at issue 3 and that is
**open** - nobody has measured one.

That same sentence of the 128K reference says the +2A/+3 return 255 from an unattached port
instead of the screen byte the 48K/128K/+2 give. `plus3.c` still returns the attribute, and
the floating bus is a raw `vid->atrbyte` on every machine that has one - Pentagon included,
which has none at all. That is **open** and wants its own round.

The **open** INT lengths are the ones nobody has confirmed a figure for. Three of them
(Profi, ATM, TSConf) were deliberately left at 32 T in the raster work. The fourth is a real
inconsistency: the +2A/+3 was named in that round's decision - 36 T for the 311-line machines
plus Pentagon and Scorpion - but its layout line still says 32 T while the 128K, Pentagon and
Scorpion lines carry 36. Nothing on screen changes either way; only a program sampling the INT
line late in a long instruction can tell.

Also still open from that work: Profi's `intpos` puts the first paper dot 32 T after the
interrupt where UnrealSpeccy's preset says 12 580, and ATM Turbo 2+ is 11 T out. Neither has an
official figure behind it.

**None of these is being changed by this rework** (decided 2026-09-09): the timings stay as
they ship. They are listed so a definition generated from this table carries today's value on
purpose rather than by accident.

## 2. Memory, storage and sound

RAM is what the core's `mask` field allows; the bold size is what the shipped profile picks.

| machine | RAM | disk | HDD | sound | mouse |
|---|---|---|---|---|---|
| ZX Spectrum 48K | 16K, **64K** | none | none | beeper | no |
| ZX Spectrum 128K / +2 | **128K** | none | none | 1 AY 1.773, mono | no |
| ZX Spectrum +2A | **128K** | none | none | 1 AY 1.773, mono | no |
| ZX Spectrum +3 | **128K** | uPD765 | none | 1 AY 1.773, mono | no |
| Pentagon | **128K**, 512K | Beta Disk | none | 1 YM 1.75, ABC + Covox | yes |
| Pentagon 1024 SL | **1M** | Beta Disk | none | 1 YM 1.75, ABC + Covox | yes |
| ZS Scorpion 256 | **256K**, 1M | Beta Disk | SMUC | 1 YM 1.75, BAC + Covox | yes |
| Profi | 512K, **1M** | Beta Disk | Profi | 1 YM 1.75, ACB + Covox | yes |
| ATM Turbo 2+ | 128K, 256K, 512K, **1M** | Beta Disk | ATM | 1 YM 1.75, ABC + Covox | yes |
| ZXM-Phoenix | **2M** | Beta Disk | none | 1 YM 1.75, ABC + Covox | yes |
| ZX Evolution (BaseConf) | **4M** | Beta Disk | NemoIDE | 2 YM 1.75, ABC (TurboSound) + Covox | yes |
| ZX Evolution (TSConf) | **4M** | Beta Disk | NemoIDE | 2 YM 1.75, ABC (TurboSound) + Covox | yes |

The sound column gives the chip, its clock in MHz and the channel order. Both belong to the
machine: decision 6 of the plan made them a global preference, which meant the clock no
longer followed the machine, and that was reversed on 2026-09-12.

This table is what the definitions in `res/machines/` are generated from, so a row that is
wrong silently becomes a machine that is wrong: the HDD column said "none" everywhere and
the mouse column "no" for every clone until 2026-09-12, and the definitions shipped that way.

**There is no separate `+ TR-DOS` machine.** The plan asked for one (5.1 and 7.3 there) and
that was reversed: a Beta Disk on a 48K or a 128K is a setting, and once a machine of your
own can be built on top of a shipped one, a second list entry says nothing. An old
`+ TR-DOS` profile migrates to the bare machine, so the interface is switched on by hand.
Section 4 still records which banks such a ROM set needs.

The shipped profile is the authority on what a machine carries (decided 2026-09-09), with
one correction: the 128K had a three-chip TurboSound, and is back to the one AY it comes
with.

## 3. Port decode

Only the fields that separate one machine from another; the rest is common ZX.

| machine | 0x7FFD mask | bank field | extra paging |
|---|---|---|---|
| ZX Spectrum 128K / +2 | 0xC002 | bits 0-2 | - |
| ZX Spectrum +2A / +3 | 0xC002 | bits 0-2 | 0x1FFD (mask 0xF002): special modes, ROM bit 2 |
| Pentagon | 0x8002 | bits 0-2 + 6-7 | - |

The 0x8002 mask is what makes a Pentagon a Pentagon here: a write to 0x3FFD pages as well,
where a Sinclair machine ignores it. Bits 6-7 are the 512K extension the 128K does not have.
Both differences are why the 128K needed its own core (phase 1) instead of borrowing
Pentagon's.

## 4. ROM banks

The bank index is `roffset / 16K` in today's romset table and becomes the `rom<N>` key in the
machine definition (plan, 6.2). ROM paging is `(flgDOS ? 2 : 0) | flgROM` on every 128K-style machine,
so bank 0 is the 128 editor, 1 the 48 BASIC, and 2/3 the interface ROM.

| machine | 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| ZX Spectrum 48K | 48.rom | - | - | - |
| ZX Spectrum 48K + TR-DOS | 48.rom | trdos.rom | - | - |
| ZX Spectrum 128K | 128-0.rom | 128-1.rom | - | - |
| ZX Spectrum 128K + TR-DOS | 128-0.rom | 128-1.rom | - | trdos.rom |
| ZX Spectrum +2 (ROM set of the 128K) | plus2-0.rom | plus2-1.rom | - | - |
| ZX Spectrum +2A / +3 (v4.0) | plus3-0.rom | plus3-1.rom | plus3-2.rom | plus3-3.rom |
| ZX Spectrum +3 (v4.1) | plus3-41.rom, 64K combined | | | |
| Pentagon 128 | 128p-0.rom | 128p-1.rom | gluck.rom | trdos504t.rom |
| Pentagon 128 (TR-DOS 5.03) | 128p-0.rom | 128p-1.rom | gluck.rom | trdos.rom |
| ZS Scorpion 256 | 256s-0.rom | 256s-1.rom | 256s-2.rom | 256s-3.rom |
| ZS Scorpion 256 (ProfROM 3.9f) | prof39f.rom, 128K combined | | | |
| Profi | profi.rom, 64K combined | | | |
| ATM Turbo 2+ | atm2.rom, 64K combined | | | |
| ZXM-Phoenix | phoenix.rom, 64K combined | | | |
| ZX Evolution (BaseConf) | zxevo-fe.rom, 512K combined | | | |
| ZX Evolution (TSConf) | tsconf.rom, 64K combined | | | |

Every ZX machine except the Sinclair ones also names `gs = gs105b.rom` (General Sound), and
ATM, Profi and both Evo sets name `font = sgen.rom`.

## 5. Names

The display names and machine ids are section 7.3 of the plan; nothing in this file competes
with it. The core names phase 1 settles on are `ZX48`, `ZX128`, `Plus2A`, `Plus3`, `Pentagon`,
`Pentagon1024SL`, `Scorpion`, `Profi`, `ATM2`, `Phoenix`, `Baseconf`, `TSConf`.

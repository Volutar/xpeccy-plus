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
| Scorpion ZS 256 | 3 500 000 | Scorpion | 224 | 312 | 69 888 | 32 | 0 (none) | no | no | yes | no |
| Scorpion ZS 256 Turbo+ | 3 500 000 | Scorpion Turbo+ | 224 | 316 | 70 784 | 32 | 0 (none) | no | no | yes | no |
| Profi v3 | 3 500 000 | Profi | 224 | 320 | 71 680 | 32 **open** | 0 (none) | no | no | no | no |
| Profi v5 | 3 500 000 | Profi v5 | 224 **open** | 312 | 69 888 | 32 **open** | 0 (none) | no | no | no | no |
| ATM Turbo 2+ | 3 500 000 | ATM Turbo 2+ | 224 | 312 | 69 888 | 32 **open** | 0 (none) | no | no | no | no |
| ZXM-Phoenix | 3 500 000 | Pentagon | 224 | 320 | 71 680 | 36 | 0 (none) | no | no | no | no |
| ZX Evolution (BaseConf) | 3 500 000 | Evo | 224 | 320 | 71 680 | 32 | 0 (none) | no | no | no | no |
| ZX Evolution (TSConf) | 3 500 000 | TSConf | 224 | 320 | 71 680 | 32 **open** | 0 (none) | no | no | no | no |

Contention patterns are `vid_wait_dots()` in `video/video.c`: 1 is the Ferranti ULA's
`12,11,...,1,0,0,0,0` over banks 1/3/5/7, 2 the Amstrad ASIC's `2,1,0,0,14,...,3` over banks
4-7 and mreq cycles only, 0 is no contention at all.

`cpu.turbo` lists the turbo steps a board has, as multipliers of its base clock. Five of them
switch it from a port and the core already does that: Scorpion ZS 256 Turbo+, ATM Turbo 2+ and Pentagon
1024SL at x2, ZX Evo and TSConf at x2 and x4. Profi and ZXM-Phoenix have 7 MHz too, but no
port for it, so they are turbo by switch - Alt+T is the switch, and whether the real
ZXM-Phoenix has a port for it (its `#EFF7` is decoded for nothing today) is **open**.

`floatbus` is what a port nothing answers reads back. The 48K, 128K and +2 get `ula`, the
+2A and +3 `asic`; both were checked against Woodmass's Float48K and Float128K, where the
first of the four bytes lands one tick earlier than fuse prints it because these machines
run early timings. The clones get `attr`, the documented port `#FF` that hands back the
attribute of the cell being shown - not a floating bus, but what this emulator has always
done for them and what a demo written for a Pentagon expects. ZX Evo's BaseConf is `none`:
its fpga drives FF on any read nothing claims (`zbus.v`). The +2A/+3 rules (ports
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
plus Pentagon and Scorpion - but its layout line still says 32 T while the 128K and Pentagon
lines carry 36. Nothing on screen changes either way; only a program sampling the INT line late
in a long instruction can tell.

The two Scorpion ZS 256 boards are read off their schematics, not off an emulator's preset:
github.com/romychs/YScorp for the yellow board and github.com/romychs/Scorpion256TPlus for the
Turbo+. The yellow board has 312 lines and the Turbo+ 316: the Turbo+ board as built reloads its
line counter with #44 where the paper schematic says #48 (one pin of DD5, checked on a real V16
board), and the maker's page says the Turbo+ frame is slightly longer. INT is clocked by the end
of the frame sync, 64 lines above the paper, and the first paper dot comes about 14 340 T after
it, the pixel pipeline adding ~4 T. The Turbo+ makes a 32 T pulse with a flip-flop (the maker
gives 9 us); the yellow board's is an RC pulse of roughly 45 T that varies with the chips, left
at 32 T as every emulator has it. The yellow board holds an opcode fetch from RAM, and only from
RAM, to an even T (`scrp.wait`); the Turbo+ has no such wait with its PLD 15.3, only with the
older 15.1. In turbo the Turbo+ runs off the 7 MHz dot clock and its PLD makes a RAM access
wait for the phase the video leaves free (one dot in four on the screen, one in two in the
border), adds a wait to every i/o cycle, and drops to 3.5 MHz while INT is up.

Horizontally the two layouts are the same on purpose. The Turbo+ clocks its blanking off H5,
48 T against the yellow board's 40 T, which leaves 20 T of border on the left instead of 28;
the layout keeps 40 T so the frame is 352 wide like every other machine, and the 8 dots the
board blanks are drawn in the border color. Blank plus left border is 136 dots either way, so
no timing moves.

Also still open from that work: ATM Turbo 2+ is 11 T out, with no official figure behind it.

The Profi's frame comes from a 2K sync PROM, and more than one was in circulation. Decoded
(dumps in `build/refs/profi/rom/`, all v3): `VR3-0A1DFAFD` puts the first paper dot 12 580 T
after INT - UnrealSpeccy's preset, credited to DDp - and `VR3-15E9B638`, the standard SAMX6,
12 588 T with 32-line borders, which is ZXMAK2's. A third family (the file named
`VR3-5A0AB56B`, really CRC FB0579B6) has INT about a line before the paper, on a 320-line frame.
A real v3.2 Kramis board, photographed in its BIOS menu with and without turbo, puts INT 47 T
before the first paper dot: the menu HALTs, counts a fixed delay and then draws border bands,
whose line says where INT is to two lines, and whose colour changes - each one a few lines
apart, so some fall in the paper and show as a step between the left and the right border -
say where in the line to about 10 T. A timing test run on that board (`build/profitest/`) gives
a 320-line frame, 71 680 T: every figure it printed came out 1.0256 times short of the model,
which is that frame over the 69 888 the test assumed - and with this layout the emulator prints
the same sixteen figures to the hundredth. Both agree with the `FB0579B6` PROM family above.
Tact Meter 1.0 (Strunov, 2006) on the same board reads 71 680 T per interrupt at 3.5 MHz and,
in turbo, 143 206 for code in ROM and 88 208 for code in RAM; the emulator reads the same.

The v5 PROM (`VR5-D2D4A7C8`, from a Kondor 5.04) decodes to a 216 T line, a 67 392 T frame and 13 860 T to the paper. The line length has
nothing else behind it and stays 224 T until a board confirms it, but v5 takes the 13 860 T: its
BIOS rewrites the palette right after a HALT with 17 writes to `#7E`, each of which is a border
write too, and with INT at the paper they flicker beside the title of the palette test.

The Profi turbo is the v3.2 schematic's: the cpu and the video share a 3.5 MHz DRAM slot and a
RAM access waits for the cpu's turn; ROM, i/o and refresh run without waits, and the VG93's HLD
drops the turbo altogether. A cycle that starts on a slot edge waits two (a NOP in RAM is 6 T).
One that starts between edges has 71 ns to get its request to U28, and waits three, not one.
The timing test on a real v3.2 matches it in all sixteen cases it measures, the mixed pairs that
start on the second phase included; an owner's timing table from the 90s does in all
ten entries checked (`ADD A,N` 6, `ADD HL,BC` 7, `ADC A,(IX+d)` 13, `BIT b,(IX+d)` 15, in
3.5 MHz T), with its data reads from ROM - `ADD A,(HL)` is 5 there, not the 6 of `ADD A,N` -
where the one-wait reading is out in seven. And the photographs' band steps fit only a colour
period of 1220-1231 turbo T: this gives 1230, the one-wait reading 1106. The v3.2 manual's fix for
an unstable turbo - 200-400 pF on that very path - is what makes it miss. The v4.01 drawings
add the cpu at 3.5 MHz while IORQ is low, which would fit the band steps as well (1226); the
test's OUT and IN, 8 T like the model, rule it out on the v3.2.

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
| Scorpion ZS 256 | **256K** | Beta Disk | none | 1 AY 1.75, BAC + Covox | yes |
| Scorpion ZS 256 Turbo+ | **256K**, 1M | Beta Disk | SMUC | 1 AY 1.75, BAC + Covox | yes |
| Profi v3 | 256K, **512K**, 768K, 1M | Beta Disk | none | 1 AY 1.75, ACB + Covox | yes |
| Profi v5 | 512K, **1M** | Beta Disk | Profi | 1 YM 1.75, ACB + Covox | yes |
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
| Scorpion ZS 256 | scorpion295.rom (ROM 2.95), 64K combined | | | |
| Scorpion ZS 256 Turbo+ | prof401.rom (ProfROM 4.01), 256K combined | | | |
| Profi v3 | profi-kramis02.rom (JV "KRAMIS" V.02, TR-DOS 5.03), 64K combined | | | |
| Profi v5 | profi-bios20.rom (Micco ROM Bios 2.0), or profi-bios10.rom (1.0), 64K combined | | | |
| ATM Turbo 2+ | atm2.rom, 64K combined | | | |
| ZXM-Phoenix | phoenix.rom, 64K combined | | | |
| ZX Evolution (BaseConf) | zxevo-fe.rom, 512K combined | | | |
| ZX Evolution (TSConf) | tsconf.rom, 64K combined | | | |

Every ZX machine except the Sinclair ones also names `gs = gs105b.rom` (General Sound), and
ATM, Profi and both Evo sets name `font = sgen.rom`.

## 5. Names

The display names and machine ids are section 7.3 of the plan; nothing in this file competes
with it. The core names phase 1 settles on are `ZX48`, `ZX128`, `Plus2A`, `Plus3`, `Pentagon`,
`Pentagon1024SL`, `Scorpion`, `ScorpionTP`, `Profi3`, `Profi`, `ATM2`, `Phoenix`, `Baseconf`, `TSConf`.

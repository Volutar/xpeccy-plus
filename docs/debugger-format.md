# The Debugger's text format

One text format holds everything the Xpeccy+ Debugger knows about a machine's memory: names
(labels), which bytes are data and how they split into statements, comments, breakpoints, and
which ROM a list of names belongs to. The same format is used for:

- **label files** - Load labels, Save labels, the `.labels` file beside an image, `--labels`;
- **sessions** - Save session and Load session (`.xdbg`), which also name the machine and the
  images in it;
- **breakpoint lists** - the breakpoint panel's Save and Load, `--brk`;
- **ROM lists** - the names of known ROMs that come with the program (`config/rom-labels`).

This document describes the files as the program reads them (`src/xcore/dbgtext.cpp`,
`labels.cpp`, `breakpoints.cpp`); where it disagrees with the code, the code is right. It is
written so that another program - an assembler, a disassembler, another emulator - can write
or read the format from it alone.

## Principles

1. **Every line stands alone.** There are no sections and nothing carries over from one line
   to the next: files can be joined, split, sorted and searched line by line, and two files
   read one after the other say the same as their concatenation.
2. **A line's first word tells what it is.** A first word with a colon in it is a place, and
   the line is a label. A first word without one is a keyword. So a label named `db` or
   `CAFE` is never taken for anything else.
3. **A label line is a SjASMPlus `LABELSLIST` line.** Names are full names, the parts joined
   by dots as SjASMPlus writes them: `module.main.local`, `BASIC48.LD_BYTES`.
4. **Data records are named after the assembler directives** that would make those bytes:
   `db`, `dw`, `dm`, `dc`.
5. **Unknown keywords are skipped**, counted in the log, never an error for the file: a newer
   writer's records do not stop an older reader.

## Lines

- Text in UTF-8; LF or CRLF line ends.
- A line whose first non-blank character is `;` is a comment line. Blank lines are skipped.
- Words are separated by blanks (spaces or tabs). Keywords may be in any case; writers write
  them in lower case.
- A record ends with its last field. Anything after the fields of a label or a data record is
  ignored by this version and is reserved for later ones; `comment`, `open` and `machine` take
  the rest of their line, `rom` and `brk` all of their words.

## Places

A place says where a record is. All numbers in a place are hex, without a prefix (`0x` is
accepted).

| place | meaning |
|---|---|
| `05:1A00` | RAM page 5, offset `1A00`. The page is **decimal** and counted in the machine's own pages: 16K on every machine but the ZX Spectrum Next, whose pages are 8K. The offset is taken within the page (modulo its size), as SjASMPlus does, so `05:5C3A` is `05:1C3A`. |
| `:8000` | a CPU address. A label there is placed as SjASMPlus's `LABELSLIST` reader always did: in the 128K bank the address has (`0000`-`3FFF` bank 0, `4000` bank 5, `8000` bank 2, `C000` bank 0); on the Next it stays a CPU address and names whatever is paged in there. Data, a comment or a breakpoint at a CPU address is put on the cell the CPU sees there when the file is read. |
| `ROM0:0038`, `ALTROM1:0010`, `DIV3:0010`, `DIVROM:0000`, `MFROM:0066`, `MFRAM:0000`, `SLT2:0000` | a page of another memory of the machine: ROM, the alternative ROM, the divMMC's RAM pages and EEPROM, the Next's Multiface ROM and RAM, a cartridge. The page number is decimal. |
| `BASIC48:0556` | an offset in a ROM named by a `rom` record (below): every 16K ROM bank of the machine with one of that name's MD5s. |
| `START` | (not on a label line) where the label `START` is. |
| `io:7FFD`, `io:00FE/00FF` | (`brk` only) a port, and the bits of it that count. |

A range is `PLACE-END`, END an offset in the same memory, counted from the first place's
page (it may run on into the next pages): `:5B00-5BFF`, `05:1A00-1AFF`, `00:3FF0-400F`.

`FF:8000` and a page with hex digits in it (`0A:0010`) are read as Xpeccy+ before 2026.8.2
wrote them: a CPU address, and a hex page.

## Records

### Labels

```
WHERE NAME [SIZE]
```

`05:1A00 SPRITES`, `:8000 START`, `BASIC48:0556 BASIC48.LD_BYTES`, `05:5C3A BASIC48.ERR_NR 1`.

A name has no blanks and no `;`. SIZE (decimal) is a variable's size in bytes; the Debugger
uses it for system variables (`ERR_NR + 1` reaches as far as the size). The same name twice:
the later line wins.

### Data

```
db   WHERE LEN | N*K       bytes             (DB, DEFB)
dw   WHERE LEN | N*K       words             (DW, DEFW)
da   WHERE LEN | N*K       words that are addresses, shown as labels
dm   WHERE LEN | N*K       text              (DM, DEFM)
dc   WHERE LEN             text whose strings each end with a character with bit 7 set
                           (SjASMPlus's DC)
code WHERE LEN             code known to run (the Debugger's Opcode mark)
```

LEN counts the record's elements: bytes for `db`, `dm`, `dc` and `code`, words for `dw` and
`da`. `LEN` alone is **one statement** of LEN elements; `N*K` is **K statements of N elements**
each, one after another. Lengths are decimal; `#`, `$` or `0x` in front makes one hex.

| record | bytes marked | statements |
|---|---|---|
| `db :9000 28` | 9000-901B | one `DB` of 28 bytes |
| `db :9000 7*4` | 9000-901B | four `DB` of 7 bytes |
| `dw :9100 4` | 9100-9107 | one `DW` of 4 words |
| `dc BASIC48:0095 368` | 0095-0204 | one per string: a string ends after a byte with bit 7 |
| `code :8000 256` | 8000-80FF | the instructions, as the CPU decodes them |

Statements of different lengths are several records: `db :03F9 1*6` then `db :03FF 4` then
`db :0403 1`. A statement longer than the Debugger's row (DB in row, DW in row, Max text length
in Options) goes on over as many rows as it needs.

A data record sets the view of its bytes and marks where each statement starts. A record at a
CPU address goes cell by cell as the CPU sees them; any other place runs on in its memory.

### Comments

```
comment WHERE TEXT
```

`comment :8003 jump table\nentries of 3 bytes`. The comment is the rest of the line. A line
break in it is written `\n`, a backslash `\\`, so a record stays one line. The Debugger shows a
comment above its cell, a row for each of its lines.

### ROMs

```
rom NAME MD5 [skip X...]
```

`rom BASIC48 4c42a2f075212361c3117015b107ff68`

A ROM is known by its bytes: MD5 is the MD5 (32 hex digits) of a 16K bank. NAME (a letter or
`_`, then letters, digits and `_`; not something that reads as a page, an area or `io`) makes
`NAME:OFFSET` a place in every bank of the machine's ROM with that MD5. Several `rom` records of
one NAME are the variants of one ROM. A `rom` record is a definition, not a header: it may stand
anywhere in the file.

`skip` lists what a variant has not: a label name, or `NAME:OFFSET` - data that starts at that
offset - or `NAME:START-END` - data that starts anywhere in that range. A patched copy of a ROM
in a clone keeps the names of the code that did not move.

A ROM list (in `config/rom-labels`) uses `rom` records, labels and data at `NAME:` places, and
system variables at RAM places. A label's **namespace** is the part of its name before the first
dot (`BASIC48` of `BASIC48.LD_BYTES`); a system variable of a list is there only while a ROM
with names of its namespace is in the machine.

### Breakpoints

```
brk WHERE[-END] WORDS...
brk irq WORDS...
brk if "EXPR" WORDS...
```

| word | meaning |
|---|---|
| `exec`, `read`, `write` | what access fires it; none given: `exec` (a port: `read` and `write`) |
| `off` | there, switched off |
| `if "EXPR"` | it fires only when the expression is not 0 (the Debugger's expressions) |
| `log`, `log "EXPR,..."` | each hit writes a line to the breakpoint log: the registers, or these values |
| `span=open`, `span=close` | it opens or closes a measured span of T-states |
| `screen`, `count` | what a hit does: take a screenshot, or only count; none: stop in the Debugger |
| `onchange` | a global condition (`brk if ...`) fires when it becomes true, not while it is |

A quoted word may hold blanks; `\"` and `\\` in it are a quote and a backslash. WHERE may be a
label's name. A word this version does not know is logged and passed over; the breakpoint is
still set.

Examples: `brk START exec`, `brk :5B00-5BFF write count`, `brk 05:1A00 read log "A,DE"`,
`brk :8000 exec if "A==#FF" span=open`, `brk io:00FE/00FF read`, `brk irq`,
`brk if "FRAME == 300" onchange`.

### Sessions

```
machine ID
open [DRIVE:] PATH
```

A session (`.xdbg`) may name the machine (`machine zx128` - the id Options and `-m` use) and the
images in it: `open PATH` for the tape or a cartridge, `open A: PATH` to `open D: PATH` for a
disk drive. A relative path is relative to the session file. Load session switches to the
machine and puts the images in (nothing is started), then reads the rest; a label file with
these lines ignores them.

## Statements in the Debugger

Bit 7 of a cell's flag byte marks where a statement starts (`DBG_STMT`); bits 4-6 are its view.
A DB or DW row stops where the next statement starts, at a label, or at the row's size; a text
row where the next statement starts, at a character it cannot show, after a character with
bit 7, or at Max text length. Typing `db`, `dw` or `db "text"` in the listing starts a
statement there; View on a selection makes the selection one statement.

## Other files Load labels reads

Load labels knows these by what they hold:

- **SjASMPlus `LABELSLIST`** (`LABELSLIST "file"`): label lines, as above.
- **SjASMPlus `CSPECTMAP`**: `LOGICAL LINEAR TYPE NAME`; labels and ROM labels, EQU left out.
- **SjASMPlus SLD** (`--sld`, starts with `|SLD.data.version|`): labels (`L`, EQU and module
  markers left out) on their pages - the page size from the `Z` line, a device of no more than
  64K as CPU addresses; every instruction start (`T`) as code; the comments `SLDOPT COMMENT`
  keeps (`K`) as comments, and in them DeZog's keywords as breakpoints:
  `WPMEM [addr [, len [, r|w|rw]]]` a read/write watch (the line's own cell, or a CPU address),
  `ASSERTION expr` a stop where `expr` is false, `LOGPOINT [group] text ${expr}` a logged count
  of those values.
- **SkoolKit control files** (`.ctl`): `@ ADDR label=NAME`, the blocks `b c g i s t u w` and
  sub-blocks `B C S T W` with their lengths (`N`, `a:b` parts of one statement, `*COUNT`, the
  last length repeating), loops (`L`); on the CPU's addresses.
- **ZX Spin symbol files** (`.sym`): `$AAAA[,B|W|TT80[,COUNT]]<tab>NAME [;comment]`.
- **Breakpoint lists of older versions** (`CPU:8000::F:DBG::`) and Unreal Speccy's `bpx.ini`
  lines (`x0=0x8000`), in the breakpoint panel's Load and `--brk`.
- **Memory maps of older versions** (`.xmap`), in Load session.

## For assemblers

An assembler that wants the Debugger to show its program as written can write:

1. its `LABELSLIST` lines as they are;
2. for each data directive it assembled, one record: `db`, `dw`, `dm` (or `dc` for a string
   ending with bit 7) at the directive's place, its length in elements - a run of directives of
   one length may be one `N*K` record;
3. `code PLACE LEN` for runs of instructions;
4. `comment PLACE TEXT` for the end-of-line comments it wants kept;
5. `brk PLACE exec [if "COND"]` for its breakpoints.

Nothing else is needed: no header, no ROM records, no order.

## Reserved

`struct` (a structure's layout, and data records of it) is reserved for a later version.

## A sample

```
; Xpeccy+ Debugger session (docs/debugger-format.md)
machine zx128
open game.tap
open A: data.trd
:8000 START
05:1A00 SPRITES
ROM0:0038 MY_IM1
code 02:0000 256
db 02:1000 7*4
da 02:1110 6
dm 02:1200 40
comment 02:0003 jump table\nentries of 3 bytes
brk START exec
brk :5B00-5BFF write count
brk io:00FE/00FF read
```

# The machine definition format

What a machine is, as the loader reads it today (`src/xcore/machines.cpp`). This describes the
files, not a plan for them: where it disagrees with the code, the code is right.

## Where the files are

One file per machine, named by its id - `pent.conf` is the machine `pent`. There is no `id`
key; the file name is it. Ids are lowercase ASCII, digits and `-`, because an id is a file
name, a section name and a command-line argument.

- **What ships**: `res/machines/*.conf`, compiled into the binary as Qt resources.
- **What the user has**: `machines/*.conf` in the config directory (`--confdir`, otherwise
  `config/` beside the executable).

The two are read apart and combined per id:

- a user file whose id **is** a built-in one is a **patch over it** - the built-in definition
  is read first, the user's lines on top. A shipped fix still reaches the machine, and
  deleting the user file puts it back as it comes.
- a user file with an id of **its own** is a machine of its own. It names `inherit`.

The user's files are written by the emulator itself (Options -> Machine), holding only what
differs from the machine as it ships. They are ordinary text and can be edited by hand.

## Inheritance

`inherit = <id>` in `[machine]` builds that machine first and applies this file's lines over
it, ROM set and all. It is how the definitions are written - a `+2` is a `128K` with two lines
changed - and it goes at most eight deep.

**What is inherited is the parent as it *ships*, never the parent with the user's patch on it.**
Otherwise changing the 128K would move the +2 with it, which nobody asked for. For the same
reason a machine of the user's own is a snapshot: it is written out against the stock parent
and carries what that parent was patched with as its own.

## Sections and keys

Six sections. A key in the wrong section is not found, and a section that is not one of these
is skipped without a word.

### `[machine]`

| key | value | |
|---|---|---|
| `name` | text | what the machine is called in the list |
| `family` | text | groups the list; a change of family draws a separator |
| `inherit` | machine id | see above |
| `hw` | `HardWare.name` | the emulation core, looked up by `findHardware()` |
| `cpu` | `cpuCore.name` | `Z80`, or `name@library` for a core from a library |
| `cpu.frq` | Hz | the board's crystal; the dot clock comes off it too, so it sets the frame rate |
| `cpu.turbo` | list, `1,2,4` | the turbo steps this board has, x1 first. A board that switches turbo from a port sets it itself; on one whose turbo is a switch on the case, Alt+T is the switch |
| `memory` | KB | fitted to what the core can page |
| `ram.cold` | hex groups | what RAM holds at power-on, `ff*8 00*8` style; absent leaves it alone |
| `ram.noise` | 0..1000 | bytes per thousand that come up wrong in that pattern |
| `reset` | `basic128` `basic48` `dos` `shadow` | which ROM a reset lands in |
| `issue` | `3` `2` `none` | what bit 6 of `#FE` reads with no tape playing: bit 4 of the last `OUT #FE` (issue 3), bit 4 or bit 3 (issue 2), or nothing at all (`none`, the +2A/+3) |
| `contio` | yes/no | contended i/o |
| `contmem` | yes/no | contended memory |
| `scrp.wait` | yes/no | ZS Scorpion: an opcode fetch starts on an even T-state |
| `builtin` | `disk` `ide` | controllers on the board itself: the options lock them. Read from the definition, never written back |

### `[video]`

| key | value | |
|---|---|---|
| `geometry` | layout name | from `layouts.conf`; `ULA.48` if the name is not there |
| `contPattern` | number | which contention table the ULA uses |
| `earlyTiming` | yes/no | |
| `4t-border` | yes/no | |
| `snow` | yes/no | the ULA snow effect |
| `snow.crash` | yes/no | ...and RAM that cannot take it: hangs or resets |
| `floatbus` | `none` `ula` `asic` `attr` | what a port nothing answers reads back: `#FF`, the byte the 48K/128K ULA is fetching, the same on a +2A/+3 gate array (its own ports, paging on, bit 0 forced), or the clones' port `#FF`, which gives the attribute of the cell being shown |
| `ULAplus` | yes/no | |
| `DDpal` | yes/no | Profi's own palette registers |

### `[sound]`

| key | value | |
|---|---|---|
| `psg.count` | 0..3 | AY/YM chips (TurboSound) |
| `psg.type` | `none` `ay` `ym` `ym2203` | |
| `psg.frq` | MHz | absent or 0 is Auto: half the cpu's `cpu.frq`, and 3.5 MHz for `ym2203` |
| `psg.stereo` | `mono` `abc` `acb` `bac` `bca` `cab` `cba` | |
| `soundrive` | `none` `covox` `soundrive1` `soundrive2` | |
| `gs` | yes/no | General Sound |
| `saa` | yes/no | SAA1099 |

### `[storage]`

| key | value | |
|---|---|---|
| `disk` | `none` `trdos` `plus3` | disk interface |
| `ide` | `none` `nemo` `nemo-a8` `nemo-evo` `smuc` `atm` `profi` | |

### `[input]`

| key | value | |
|---|---|---|
| `mouse` | yes/no | Kempston mouse |
| `mouse.wheel` | yes/no | |
| `joy` | `none` `kempston` `kempston8` | what answers on `#1F`: nothing (the port reads the floating bus), a Kempston joystick, or one with the extra buttons on d5..d7. The older `joy.buttons = yes` still reads as `kempston8` |
| `kbd.scantab` | `none` `xt` `at` `ps2` | which PC keys the machine's keys sit on; `none` leaves the keyboard core's own type |

### `[rom]`

| key | value | |
|---|---|---|
| `rom0`..`rom3` | `file[:offset[:size]]` | a 16K bank. Offset and size are in KB; size 0 reads as far as the file goes. An empty value empties the bank, which is how a child says "there is nothing here" over a set that has something |
| `gs` | file | the General Sound ROM |
| `font` | file | |
| `banks` | 1..4 | how many 16K ROM banks the core can page |

A file name with no path is looked for in the ROM directory. A child's `[rom]` block names only
the banks it changes, on top of the set it inherits.

## Two things that bite

**A key that is absent is not an error - it is the default**, and the defaults are
`mac_defaults()`, not "whatever the last machine had". A definition written without
`psg.type` gets an AY, silently. This is how a whole round of machines once shipped with
their sound cards and hard disks missing.

**Only `[machine]` and `[rom]` warn about a key they do not know.** In the other four
sections an unknown or misspelled key is dropped without a word. Check a new key by starting
the machine in a throwaway `--confdir`, quitting, and looking at what the emulator wrote back.

## Proving a definition is applied

Start the machine in a throwaway `--confdir`, close the window properly (a killed process
never runs `saveConfig()`), and look at `machines/<id>.conf` in that directory. Only what
differs from the definition is written, so **no file at all means every key took**. A key that
was ignored shows up there with the default value.

The one catch: an empty result only proves something when the definition's value differs from
`mac_defaults()`. For a key where the definition happens to repeat the default, put a wrong
value in the file by hand and watch it come back.

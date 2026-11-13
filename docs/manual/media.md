# Media menu

```{index} Media menu, disk
```

A machine can have floppy drives, hard disks, an SD card or a cartridge slot, depending on
the hardware it copies. A divMMC can be added to the Sinclair models and the Pentagons. The
**Media** menu lists only what the running machine has:

| Items | Described in |
|---|---|
| {icon}`tape` **Tape Player**, **Tape Play/Stop**, **Tape Record/Stop** | [Tapes](tapes.md) |
| {icon}`fdd_disk` **Disk Manager**, {icon}`fdd` **Floppy Drives**, **Fast Disk Access** | [Floppy disks](disks.md) |
| **Next Side**, **Previous Side** | [The other sides beside an image](files.md#the-other-sides-beside-an-image) |
| {icon}`cartrige` **Cartridge** | [Cartridge](#cartridge), below |
| {icon}`sdcard` **divMMC** | [divMMC](divmmc.md) |
| {icon}`sdcard` **SD Card**, {icon}`hdd` **Drives** | [SD cards and hard disks](sd-hdd.md) |

## Which machine has what

```{index} Beta Disk, TR-DOS, +3DOS, disk interface, NemoIDE, SMUC
```

| Machine | Floppy | Hard disk | Other |
|---|---|---|---|
| ZX Spectrum 48K | - | - | cartridge, divMMC |
| ZX Spectrum 128K, +2, +2A | - | - | divMMC |
| ZX Spectrum +3 | +3 drives | - | divMMC |
| ZX Spectrum Next | - | - | SD card, divMMC built in |
| Pentagon, Pentagon 1024 SL | Beta Disk | - | divMMC or Z-Controller |
| Scorpion ZS 256 | Beta Disk | - | Z-Controller |
| Scorpion ZS 256 Turbo+ | Beta Disk | SMUC | Z-Controller |
| ATM Turbo 2+ | Beta Disk | ATM Turbo | |
| Profi v3 | Beta Disk | - | |
| Profi v5 | Beta Disk | Profi | |
| ZX Evolution, both | Beta Disk | NemoIDE (ZX Evo) | SD card |
| ZXM-Phoenix | Beta Disk | - | |
| ALF TV Game, ALF TV Game 128K | - | - | cartridge |

**Beta Disk** is the TR-DOS disk interface with up to four drives, A to D. The **+3**
interface has two drives, A and B, and runs +3DOS.

A machine with no disk interface of its own can be given one. Open the
[Machine page](options-machine.md) of **Options** and pick it in the **Floppy disk** row: **None**, **Beta Disk (WD1793)** or
**+3 (uPD765)**. The **Hard disk** row does the same for an IDE interface: **NemoIDE**,
**NemoIDE A8**, **NemoIDE (ZX Evo)**, **SMUC**, **ATM Turbo** or **Profi**. An interface that
is part of the machine's board cannot be changed: the list is grayed out, and its tooltip
says **Built into this board**. A Beta Disk cannot be added to the +2A and +3.

## Cartridge

```{index} cartridge, Interface 2
```

The ZX Spectrum 48K has a slot for an Interface 2 ROM cartridge, and the ALF TV Game consoles
have a slot for their own game cartridges. **Media > Cartridge** shows the cartridge in the
slot, or **(empty)**, and has:

- **Open...** - a cartridge image, `.rom` or `.bin`, up to 4 MB. The machine is reset with
  the cartridge in.
- **Eject** - take the cartridge out; the machine is reset if it was running from it.

```{toctree}
:maxdepth: 2

tapes
disks
divmmc
sd-hdd
```

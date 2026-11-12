# SD cards and hard disks

**Media > SD Card** holds the card of a machine with an SD card slot, and **Media > Drives**
the hard disks of a machine with an IDE interface. Either can be an image file or a folder
of your computer. The divMMC's cards are in [divMMC](divmmc.md).

## SD card

```{index} SD card, --sdcard
```

The two ZX Evolution machines and the ZX Spectrum Next have an SD card slot. The Next
starts from a card with System/Next on it, and its menu has **System/Next Card...** too:
see [The SD card](zx-next.md#the-sd-card). **Media > SD Card** shows the card in
the slot, or **(no card)**, and has:

- **Open Image...** - insert a card from an image file.
- **Open Folder...** - use a folder on your computer as the card. See
  [A folder as a disk](#a-folder-as-a-disk).
- **Eject** - take the card out.
- **Refresh** - a folder card only: files added, changed or deleted in the folder reach the
  machine while it runs. See [Refresh](#refresh).
- **Write Protect** - the card's lock switch: the machine cannot write to the card. On a
  folder card it makes that one folder read only.

What the machine writes to a card image goes into the image file at once. To start with a
card, use `--sdcard <file or folder>` on the [command line](command-line.md).

## Hard disks

```{index} hard disk, IDE, HDI, LBA
```

```{index} see: HDD; hard disk
```

A machine with an IDE interface (see [Which machine has what](media.md#which-machine-has-what)) has
two places for a drive, **Master** and **Slave**. Connect them first: on the **Machine** page
of **Options**, click the {icon}`settings` gear in the **Hard disk** row and set **Master**
(and **Slave**, if you want two) to **HDD (ATA)**.

**Media > Drives** then has a submenu for each connected device, named **Master:** or
**Slave:** followed by the name of the image in it:

- **Open Image...** - open a hard disk image: `.hdi`, or a plain image of the disk with any
  other extension.
- **Open Folder...** - use a folder on your computer as the disk. See
  [A folder as a disk](#a-folder-as-a-disk).
- **Eject** - take the image out.
- **Refresh** - a folder disk only, as on an SD card: see [Refresh](#refresh).
- **Properties...** - the window **Hard Disk: Master** (or **Slave**): the image, its
  cylinders, heads and sectors (**C / H / S**), its **Capacity**, and the **LBA** box. LBA
  lets the system address the disk by sector number; clear it for an old system that only
  knows cylinders, heads and sectors.

What the machine writes to a hard disk image goes into the file at once. The images stay
mounted when you switch machines, and are opened again at the next start.

## A folder as a disk

```{index} folder as a disk, Open Folder, FAT32, NedoOS
```

**Open Folder...** in **Media > SD Card**, in a divMMC's card menus and in **Media > Drives**
lets the machine use a folder of your computer as if it were a FAT32 card or disk. There is
no image to make: copy programs into the folder and open it. The window that picks the
folder is **Folder to serve as the card** or **Folder to serve as the disk**. How the disk is made and
written back is in [A folder as a disk](folder-as-disk.md) of Under the Hood.

- The folder is read when it is opened, and again at **Machine > Reset > Hard Reset** and
  when you switch machines; **Apply** in **Options** does not read it again. To bring in
  files added, changed or deleted since without a reset, use **Refresh** (see
  [Refresh](#refresh)).
- The disk is made big enough for the folder, from 512 MB up to 32 GB. A folder that takes
  writes gets as much free room again as its files take, and 256 MB more. A larger folder
  is not opened, and the message **folder too big for a disk** says so.
- These are left out: files of 4 GB and over, folders more than 16 levels deep, entries
  past 65536 in one folder, and anything past 200000 entries in all. The message
  **folder: not all of it fits** says when something was left out.
- A link to a folder (a junction on Windows) is followed once: one that leads back into
  the folder, or to a place already shown, is left out. A Windows shortcut (`.lnk`) is a
  file like any other; files your system hides are left out.
- A folder is not a whole disk: attributes, dates other than the last write and sectors
  outside files last only until the folder is read again (see
  [What a folder cannot do](folder-as-disk.md#what-a-folder-cannot-do)). For full
  compatibility, use an image.
- On a hard disk, a folder that holds NedoOS with its `ZXLDR` file in the top level can
  boot NedoOS.

### Writing to a folder

```{index} folder as a disk; writing, Writing to a folder, Folders, Deleted files
```

A folder takes what the machine writes: a program it saves, a file it changes or deletes
there changes the files in the folder itself. The folder follows about half a second after
the machine stops writing. A file the machine saves with no date keeps the time it was
written on your computer.

The first time the machine writes to a folder, it stops and **Writing to a folder** asks:

- **Allow writing** - the folder takes this write and every one after it.
- **Read-only** - the folder stays as it is. What the machine wrote stays in memory, and
  the machine sees it, until the folder is read again. Folders opened after that are
  read only.

It asks once: the answer becomes the **Folders** setting. Both settings are in the
**Folder as a disk** group on the [Emulation page](options-emulation.md) page of **Options**:

- **Folders** - **R/W**, read/write (the default), or **R/O**, read-only. The machine cannot
  write to a folder opened read-only: esxDOS reports `I/O ERROR`, a hard disk an error.
  **R/O** takes effect at **Apply**: a folder in use stops changing, and what the machine
  writes to it from then on stays in memory, as with the **Read-only** answer above.
  **R/W** takes effect when the folder is next read: at a **Hard Reset** or a change of
  machine.
- **Deleted files** - **Move to trash** (the default) puts a file the machine deletes in
  your system's trash; **Delete permanently** removes it. A file the trash will not take
  stays in the folder.

**Write Protect** in a card's menu makes that one folder read only, whatever **Folders**
says.

What the machine wrote is never thrown away on the way. When something cannot be carried
over - a file another program has open, a name your system refuses - a message says so
once, naming the files, and a file's content that could not be put in its place is kept in
the folder's top level with **(recovered)** in its name. A file you change on your computer
while the machine has the folder keeps your change: the machine's version is saved beside it
as **name (2)**, and the machine deleting it leaves it where it is.

Keep a copy of anything you cannot afford to lose. Formatting the card or disk, or
deleting most of the folder at once, does not reach your files: the folder is left alone
from then on, what the machine writes stays in memory, and a message says so.

### Refresh

```{index} folder as a disk; Refresh, Refresh; a folder card
```

**Refresh** in a folder card's or folder disk's menu brings in what you changed in the folder
since it was read: new files and folders, changed files, deleted ones. The machine keeps
running and needs no remount, as a real card put back would. What the machine wrote there
first goes to the folder. The message **refreshed: 2 new, 1 gone, 0 changed** says what
changed.

The machine sees the change the next time it reads the folder: esxDOS at its next listing,
the NextZXOS Browser when it shows the folder again (leave it and come back). Files that
stayed keep their place on the disk, so a file the machine has open, a disk image it has
mounted and the folder it is in read on as before.

**Refresh** is refused, with a message, when what the machine wrote has not all reached the
folder, when the folder no longer follows the machine, or when the changes do not fit on
the disk; **Eject** and **Open Folder...** then read the folder anew. An image file has no
**Refresh**. A changed `ZXLDR` boots only after a **Hard Reset**.

Two [hotkeys](hotkeys.md), with no key until you give them one, work on SD cards from the
keyboard: **SD Card: Refresh** refreshes every folder card the machine uses, and **SD Card:
Eject** takes out the machine's card - the slot's on the ZX Evolution and the Next (where
the Next's own card goes back in), the divMMC's first card that holds one on the others.

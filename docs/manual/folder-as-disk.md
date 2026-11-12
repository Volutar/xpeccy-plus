# A folder as a disk

```{index} folder as a disk; how it works, FAT32; folder, Writing to a folder; how it works
```

A folder on your computer can be used as an SD card, a divMMC card or an IDE hard disk:
the machine sees a FAT32 disk with your files on it, and its own DOS reads and writes them.
Nothing is copied into an image. This page says how that works and where its limits are. How to
use it is in [A folder as a disk](sd-hdd.md#a-folder-as-a-disk); its settings are in the
**Folder as a disk** group on the [Emulation page](options-emulation.md).

## A disk made up as it is read

When a folder is opened, its tree is read once - names, sizes, dates - and a FAT32 disk is laid
out in memory around it: a partition table, a boot sector, two file tables, and every file given
one unbroken run of 4 KB clusters. The file tables then follow from simple arithmetic, and only
the directories are made up as the machine reads them. A file's own bytes are read from the file
on your computer at the moment the machine reads that part of the disk.

Names come as long names, as any FAT32 has them, and as the short 8.3 names an older DOS reads.
In a short name, Russian letters are put into the cp866 code page, the one ZX systems expect;
other letters become `_`. The tree is read breadth first, so when a limit is reached, it is the
deepest folders that are left out, never the program at the top. A link to a folder is followed
once; one leading back up the tree is left out, so the machine never sees one file under two
names.

The disk is 512 MB, or twice that and so on, up to 32 GB - whatever holds the files. A folder
that takes writes gets as much free room again as its files take, and 256 MB more.

The folder is read anew with the power off: at a **Hard Reset** or a change of machine - as a
real card is taken out, has files copied onto it and goes back in. **Apply** in Options does
not read it again: the program running keeps the disk it knows.

## Reload

```{index} Reload; how it works
```

**Reload** brings the folder's changes in while the machine runs, and does not lay the disk
out anew. What the machine wrote is carried to the folder first; then the folder is read and
compared with the disk, and only the difference is written into the disk, as a DOS would
write it: a new file gets free clusters near the end of the disk and an entry in a free slot
of its directory, which grows by a cluster when it is full; a deleted one has its entries
marked deleted and its clusters freed; a changed one gets its new size and as many clusters
as that needs. Everything else stays where it was.

That matters because a DOS keeps some of the disk in memory between calls. esxDOS, run here
through a reload, reads directories and the file table again each time, but keeps the
cluster of the directory it is in, the clusters of a file it has open - a mounted disk image
or tape too - and the free cluster count it read when it started. Laid out anew, the disk
would put other files under all of these. Kept in place, they read on as before. The free
count a DOS writes back from memory can be wrong after a reload; it is only a hint, and
nothing is lost by it.

A folder of many thousands of files takes seconds to read, and the window waits meanwhile:
when a read goes on for over a second, the status bar says **Reading the folder ...**. The
SD card slot of the ZX Evo and the Next is read only on those machines: on another one the
card stays named, and its folder is read when a machine with the slot is chosen again.

## Writing back

```{index} Writing to a folder; sync, (recovered)
```

A DOS writing a file does not do it all at once: it writes the data, then the file table, then
the directory, and in between the disk is half done. So the machine's writes are kept in memory
first, and it always reads back what it wrote. Once it has written nothing for about half a
second, the program reads the file tables and directories back, as the machine left them,
compares them with the tree it knows, and makes the same changes in the folder: new files,
changed files, renames, moves and deletions.

It is careful with your files:

- a file is deleted only when all of its old clusters are free, and replaced only when its new
  content is complete;
- what cannot be placed is kept in the top folder as `name (recovered)`;
- a file changed on the computer in the meantime wins: the machine's version goes beside it as
  `name (2)`;
- a name Windows refuses gets `_` in place of what it refuses;
- a deleted file goes to the system's trash, unless **Deleted files** says otherwise.

If the machine formats the disk, rewrites its partition table, or deletes nine tenths of a large
tree at once, the folder stops following: the disk lives on in memory until it is ejected, and a
message says so once. A folder set to read only refuses every write, and the machine's DOS
reports an error, as it would with a protected card.

## What a folder cannot do

```{index} folder as a disk; limits, image; or folder
```

A folder is files on your computer, and your computer's file system keeps less than a FAT disk
does. Whatever the machine puts on the disk that is not a file's name, size, last write time or
content lives in memory only, and is gone once the folder is read again:

- **attributes** - hidden, system, read only: the machine sees them while the disk is in; the
  files on your computer do not take them;
- **other dates** - a creation or access date the machine sets; after a reread both are the
  last write time;
- **sectors outside files** - what a DOS writes to sectors of its own: a boot sector, the
  reserved sectors, free space. A copy protection or a map of the disk kept there does not last;
- **where files lie** - after a reread every file is laid out anew, in one piece. A program that
  remembers where a file is on the disk, by sector or by cluster, finds something else there;
- **the volume label** - it goes back to the disk's own.

The folder also shows what your computer has, as it has it: a Windows shortcut (`.lnk`) is a
file like any other and is not followed, and files the system hides are not on the disk.

**For full compatibility, use an image.** An image file is a disk sector for sector, and
nothing of the above applies to it. A folder is for convenience: files shared between the
machine and the computer.

## Booting from a folder

The ZX Evolution boots from a hard disk without reading its file system: it runs an IS-DOS boot
sector at a fixed place. A folder used as an IDE disk gets such a sector, taken from a `ZXLDR`
file in its top level, so a system like NedoOS can start from a folder.

# Getting started

## Download

```{index} download, installation, AppImage
```

Every release is on the [releases page](https://github.com/dotkoval/xpeccy-plus/releases).
Pick the file for your system:

| System | File | |
|---|---|---|
| Windows 10 or 11 | `xpeccy-plus-<version>-win-x64-qt6.zip` | |
| Older Windows | `xpeccy-plus-<version>-win-x64.zip` | 64-bit |
| | `xpeccy-plus-<version>-win-x86.zip` | 32-bit |
| Linux | `xpeccy-plus-<version>-linux-x86_64.AppImage` | |
| macOS | `xpeccy-plus-<version>-macos-arm64-qt6.dmg` | Apple silicon |

Nothing else has to be installed. Machines, ROM images and settings come with the
program, so it runs right away.

**Windows.** Unpack the archive anywhere you can write to, and run `xpeccy-plus.exe`.
Folder and file names may have any letters (Cyrillic, Greek, Chinese...) on Windows 10
(version 1903 or later) and 11; on older Windows, the letters of the system's language.

**Linux.** Allow the file to run (`chmod +x`, or the file's properties), then run it. It
needs a distribution no older than Ubuntu 22.04 (glibc 2.35).

**macOS.** Open the disk image and drag **xpeccy-plus** into Applications. The program is not
signed by a registered developer, so macOS refuses to start it the first time: open
**System Settings > Privacy & Security**, find the message about Xpeccy+ and click
**Open Anyway**.

## The first start

```{index} first start, Which machine to start?
```

The first start asks which machine to run:

- **Which machine to start?** - any of the built-in machines.
- **Next time** - **Start the last one chosen**, **Ask again**, or **Start this one**.

You can change the answer later on the **Application** page of **Options**.

## Run a program

```{index} Open, Run, Mount, Remember my choice, drag and drop, media; run or mount
```

Open a file in any of these ways:

- **File > Open...**;
- drop the file onto the window;
- give its name on the command line: `xpeccy-plus game.tzx`.

A snapshot (`.szx`, `.z80`, `.sna`) continues straight away from where it was saved.

A tape or a disk can be **run** or just **mounted**. Run resets the machine and starts
the program on it; Mount only puts the tape or disk in, as if into a real recorder or
drive. The first time, Xpeccy+ asks which one you want. Tick **Remember my choice** to stop
the question; to change the choice later, use the **File types** page of **Options**. To be
asked about one file only, hold Shift as you open or drop it. With a [divMMC](divmmc.md)
on, Run starts a tape from a Hard Reset and cannot start a disk in a drive.

```{index} machine; switched by a file
```

If the current machine cannot run the file (a +3 disk on a Pentagon, for instance),
Xpeccy+ switches to a machine that can - to the machine named in the file, if the file
names one. When you next open a file that your own machine can run, Xpeccy+ switches back
to your machine.

```{index} Choose a Machine
```

The **File types** page of **Options** sets this for each kind of file:

- **Auto** - switch only when needed, then back to your machine;
- **Ask** - when this machine cannot run the file, the **Choose a Machine** window lists
  the machines that can: **Switch** to the one picked, **Keep** the current one, or
  **Cancel** and not open the file;
- **Keep current** - never switch the machine;
- or one machine that always opens this kind of file.

## Where the settings are

```{index} settings folder, config.conf, roms folder, --confdir
```

```{index} see: configuration folder; settings folder
```

| System | Folder |
|---|---|
| Windows | `config` beside `xpeccy-plus.exe` |
| Linux, macOS | `~/.config/xpeccy-plus` (or `$XDG_CONFIG_HOME/xpeccy-plus`) |

Everything you change is kept there: `config.conf` holds the settings, `roms` holds the
ROM images. A file put in `roms` under the name of a built-in image replaces that image.

On Windows the settings are in the program's folder, so the program can be moved or
copied together with its settings. To run with a
different settings folder, start the program with `--confdir <folder>`.

## Updates

```{index} updates, Check for Updates, Update channel, nightly build, What's New, Restart Now
```

Once a day Xpeccy+ asks GitHub whether a newer version is out. When one is, a window says
so and shows what is new since your version, with **Download** to get it (or **Update**,
see below) and **Later** to close the window until the next check. The window has two settings, saved at once:

- **Check for updates** - **Every day**, **Every week** or **Never**.
- **Update channel** - **Releases**: new versions only. **Nightly**: the nightly build too,
  made each night from the newest code; a release newer than the last nightly build is
  offered as well. A release starts on **Releases**, a nightly build on **Nightly**.

Both are also on the [Application page](options-application.md).
**Help > Check for Updates...** asks at once.

On Windows, and with the Linux AppImage, the window has **Update** instead of **Download**:
the program downloads the new version, checks it and gets it ready. **Restart Now** closes
the program the usual way (the machine is saved, a disk with unsaved changes is asked
about) and starts the new version; with **Later**, the new version takes the place of the
old one when you quit.

- On Windows only the program's own files change: the folder keeps its name and place.
  In `config`, your settings, machines, NVRAM, cards and the files you added are never
  touched. A file that came with the program and that you changed, such as a ROM image
  you replaced, is kept; if the new version has a new one, it goes to
  `config\update-<version>` and the program tells you so.
- An update cut short, by a power cut say, is undone at the next start.
- When the program's folder cannot be written to (`Program Files`), **Download** is offered
  instead.
- The AppImage is replaced by the new one.

The first start does not check, and `--no-update-check` skips the check for one run. The
check sends a request to `api.github.com` and, for the list of changes, one to
`raw.githubusercontent.com`; the program's version is in them, nothing else is.

A program you built yourself, or one from a distribution's package, checks only when you
turn the check on, and then offers the release page instead of a file.

## This manual

```{index} Manual
```

**Help > Manual** opens this manual from the program's own files, so it needs no network.
It opens in the language of your system when the manual has that language, and in English
when it does not. Switch the language at the top of the left column.

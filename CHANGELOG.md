# Changelog

Notable changes in Xpeccy+, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), versions use CalVer
(`YEAR.NUMBER[.PATCH]`).

Xpeccy+ starts from [Xpeccy](https://github.com/samstyle/Xpeccy) build `20260807`. Anything
before that point is upstream's history and is not repeated here.

## Unreleased

### Added

- **Snapshots can be saved as `.z80`**, not only `.sna`. Give the name a `.z80`
  ending in the save dialog. The file holds the whole 128K (256K on a Scorpion),
  the AY registers and where the beam was, and comes out three to five times
  smaller than a `.sna` because it is packed. ZX 48K, 128K, +2, +2A, +3, Pentagon
  128 and Scorpion 256 can be written this way; on a machine the format has no
  name for it says so rather than write something wrong.

- **An anti-alias filter for the sound**, on the Sound page. The mix is worked out
  32 times more often than the sound card plays it, and was averaged down, which
  folds everything too high to be played back into what you hear. The filter cuts
  that away instead.
  Off by default: on the material this emulator makes there is nothing to hear, so
  it is there to be compared. It costs about 1.5% of a processor core.

- **A DC filter for the sound**, on the Sound page and off by default. The chips do
  not swing about zero - an AY sits between silence and full - and that offset is not
  sound, it only eats the headroom they all share. Worst is a device that walks its
  own level up while it plays, a General Sound being the one that does: everything
  else ends up crowded against the clip. With the filter on, each device has its
  offset taken off before the mixer sees it, so one of them drifting no longer costs
  the others anything. The waveform in the debugger sits around the middle of its box
  as well.
  Thanks to Volutar for asking for it, and for saying where it had to go.

- ALF TV Game, the Belarusian ZX Spectrum console, is back in the machine list - in the
  stock 64K shape and with the 128K memory expansion. Its ROM is bundled, so it boots
  into the games menu straight away, and a cartridge opens as a cartridge image. The
  console has no keyboard, so the keys drive its two joysticks instead: arrows with
  Space or Enter for player one, WASD with left Shift or left Ctrl for player two.
  A keyboard layout of your own still wins, and picking this machine does not change
  the layout you have.
  Thanks to Prusak, whose [zxbyte.ru](https://zxbyte.ru/alf.htm) has the schematic,
  the port map and the ROM dumps this was built from.
- Pentagon starts with the memory pattern a real one comes up with, specks and all.
- The ULA snow effect: the rubbish a 48K or 128K puts on screen when the interrupt
  vector points at a screen bank. On for those two machines, off for the rest, with a
  switch per machine in the advanced settings, and under it a second one, off by
  default, for the machines whose RAM gives up under snow and hangs or resets.
- `--wav-out FILE` records the sound to a wav file from startup, the same recording
  the hotkey makes.
- **The debugger's screen panel can move into a window of its own**, with the button
  in the panel or with Alt+S. There it follows the machine while it runs and stays
  open after the debugger is closed, and it comes back at the size and in the place
  you left it. Closing the window puts the panel back in the debugger.
- **The debugger's sound panel, rebuilt.** One tab per sound chip across the top -
  PSG1, FM1, PSG2, FM2 - built from the machine in front of you, so a second chip or
  an FM half only shows up when there is one. Like the screen panel it moves into a
  window of its own, with the button in the panel, the menu or Alt+A, and there it
  keeps up with the machine while it plays.
  The PSG page has all sixteen registers at the top and a row per channel: the
  period, the volume with a bar beside it for how loud the channel is, the mixer,
  and - while the machine is stopped - what it is handing the chip's output and the
  state of its square wave. Those last two swing far too fast to be caught while it
  runs, so they are blank there. The envelope is drawn as its shape.
  The FM page has the timers and the channel 3 mode, a tab per channel, and a row
  per operator with every parameter the chip has, which operators are keyed and
  where they are in their envelope, and a star on the ones that reach the output
  for the algorithm in use.
  How loud a channel or an operator is is a meter, green through amber to red the
  way any level meter is, so it can be watched rather than read. Every number on
  both pages can be typed over, and goes into the chip the way the machine would
  write it. A PSG channel can be silenced on its own, to hear what the other two
  are doing.
  Under both pages is an oscillogram of the sound. While the machine plays it
  holds a couple of frames of it; while the debugger has the machine it holds a
  couple of milliseconds, fine enough to count the edges of a beeper or a
  digital routine, and it carries on filling as you step.
  Thanks to Volutar for the design and for talking it through.

### Removed

- The machines that are not ZX Spectrum - MSX, Game Boy, NES, Commodore 64, BK0010,
  IBM PC, PC-9801 and Specialist. They have not been built since 2026.4; now their
  code is gone as well. The settings lose the CPU type, cartridge mapper and mouse
  type rows with nothing left to pick in them.
- Octal in the debugger, which only the BK ever used. Addresses read as four hex
  digits everywhere, and X in a number field switches between hex and decimal
  instead of cycling through octal on the way.

### Changed

- **The right-click menu is shorter.** Watcher, Screen and Sound chips sit under
  Debugger now, and clicking Debugger, Favorites or Reset itself does the obvious
  thing - opens the debugger, opens the favorites list, resets the machine - so the
  `default` and `Manage...` items they used to carry are gone.

- **The debugger's screen panel picks a screen with one click.** Main, shadow, or both
  at once, with Auto following whatever the machine is showing - the memory bank and
  start offset it used to ask for are behind a Custom button now, for the rare case
  that wants them. The picture is drawn in the machine's own colours, so a palette you
  loaded, greyscale and ULA+ all look like the real screen, and the flash attribute
  blinks. Every picture is headed with the page it came from, and that heading lights
  up when it is the screen the machine is showing. Zoom moved in with the other
  switches and can Fit the picture to the panel.
  Moving over a dot now tells you where it is and what holds it - the place on
  screen in pixels and in character cells, the two addresses, and the bit of the
  byte - with dashes where there is no dot. A click holds the readout on one dot
  and marks it on the picture; typing an address into either of the two fields
  marks where that one lands, and the right button copies one. On a shadow screen
  the address reads from #C000, where the page actually is, in the emulator
  window too.

- **TurboSound FM sounds like the chip it emulates.** The FM half of the YM2203 runs
  on the ymfm core now; the one it replaces fed two of every channel's four operators
  each other's registers, so no instrument came out the way it was written. The FM is
  as loud against the AY as it is on a real board, and a track that speaks through the
  chip's CSM mode says its words instead of holding one note.

### Fixed

- **`IN #FE` bit 6 read as 1 whenever no tape was playing.** A real machine hears its
  own last `OUT #FE` back on that bit, which is how a program tells an issue 2 board
  from an issue 3 one; here it heard a stuck 1 and could tell nothing. Which board a
  machine is is now a setting of its own, on the ULA block of the Machine page - and
  the +2A/+3 are set to what they really do, which is feed nothing back at all.

- **Two wrong flags in the Z80.** `BIT n,r` and `CPI` left the wrong value in the two
  undocumented flag bits. Only something written to tell one processor from another
  would notice, but that is what those programs are for.

- **`RETI` puts the interrupt state back**, the way the processor does and `RETN`
  already did here. It changes nothing unless a program returns from an interrupt
  with it and counts on that.

- **A saved `.wav` carried a wrong length in its header**, which some players refuse
  to open.

- **RZX recordings play again.** On Windows opening one always failed with "can't
  open file" - the scratch file it unpacks into was asked for in the root of the
  system drive. And a recording only replays on the machine it was made on; that
  machine is read from the file now, instead of the recording being fed to whatever
  was running and running out of input part way through.

- **A saved `.sna` was losing three registers.** BC', DE' and HL' all went into the
  same place in the header, so two of them were dropped and the third held the wrong
  pair - a snapshot saved from here and loaded back resumed with the wrong values.
  Two smaller things in the same save: taking a 48K snapshot left the running machine
  with two bytes of its own stack overwritten and its stack pointer two lower, and
  the R register went out with the wrong top bit.

- **Two things about loading a `.z80`.** A snapshot taken in the middle of a frame
  now starts with the beam where it was saved instead of at the top of the picture -
  the file says where it was, and that was being skipped. And a snapshot that
  carries a ROM page beside its RAM no longer loses every page after it.

- **The right-click menu no longer picks an item by itself.** If the mouse moved a
  little while the button was going down, letting it go counted as a click on
  whatever was under the cursor by then - usually Open, and the file dialog came up
  uninvited.

- **A program that drives the SD card itself got nowhere.** The card answered as
  if it were past its idle state, so such a program took it for an old card and
  read 512 times past the address it meant; and a single-block read that followed
  a multi-block one never stopped. A card image and a folder served as one are
  both affected.
  Thanks to Alexander Nihirash, who brought the card contents and the test tool.

- **ZX Evolution and TSConf lost their NVRAM settings on every start.** What the
  firmware keeps there - the boot target, the TR-DOS drive, autoboot - was written
  over with an empty block before the machine came up, so every launch came up on
  the defaults, TSConf in TR-DOS. Whatever a machine had saved before is gone: set
  it once more and it stays.

- **What you change on a machine now stays with it.** Switch to another machine and
  back and your settings are still there, instead of quietly going back to stock.
  They live in a file of the machine's own under `machines/`, so an update can still
  fix the machine itself without touching them, and "Restore machine" is what throws
  them away. A machine carrying settings of yours is marked with a `*` in the machine
  list, in the menu and in the settings alike.

- **Saving a machine under a name that is already taken** made a second machine with
  the same name in the list. Saving now always makes a machine of your own and asks
  for a name nothing else wears.

- **ALF TV Game ran a shade too fast and its second joystick was scrambled.** The frame
  was 68964 T instead of the 69888 a 48K has, because the machine kept the screen clock
  of whichever one ran before it; and player two's joystick reads in an order of its
  own, where it was being given player one's.

- **A snapshot opened on ZX Evolution (BaseConf) or ATM Turbo 2+ landed in the
  machine's boot menu** instead of running - `.sna` and `.z80` alike.

- **ZX Evolution had no sound from the AY ports** unless a program used `#FFFD` and
  `#BFFD` exactly, which a TurboSound FM player does not. It decodes them the way its
  firmware does now.

- **Every AY and YM played slightly sharp** - the clock period was kept in whole
  nanoseconds. They are exact now, so the pitch drops a fraction of a semitone onto
  where it belongs.

- A window opened from the settings, such as the advanced machine settings, no longer
  lets the settings behind it close first.

## 2026.4.1 - 2026-09-12

### Changed

- **ZX Evo is one machine again.** The two entries split by firmware generation are gone, and
  the one that remains runs either. It ships with EVO Reset Service 0.61 FE and NEO-DOS 0.60.

- Both ZX Evo machines start with the Kempston mouse, its wheel and the five-button joystick
  switched on, and the machine now reports itself as Xpeccy+ rather than a 2012 FPGA build.

### Added

- **The Magic button works on ZX Evo.** F10 opens the EVO Magic Service over the running
  program and goes back to it.

- **TR-DOS emulation.** A drive the firmware marks virtual is served the way a real BaseConf
  serves it. A drive with a disk in it is left alone, so an image you opened still boots.

- ZX Evo starts with a pattern in its memory, the way real hardware does.

- Holding a key on ZX Evo repeats it, as its own keyboard does.

### Fixed

- **The SD card and the hard disks were lost whenever the machine was switched**, so booting
  from them failed while the settings still named the image.

- **The machines had lost settings the old profiles carried.** Every clone is back to its YM
  sound chip, its Covox, its mouse and Kempston buttons, and the hard disk interface it comes
  with - SMUC on a Scorpion, Nemo on a ZX Evo. ZX Evo has its two-chip TurboSound again.

- **The AY clock and the stereo channel order follow the machine again.** They had become one
  setting shared by every machine, so whichever was up last decided the clock for all of
  them - and a 128K runs its AY faster than a clone does.

- **ZX Evo's text mode was drawn in the wrong place** and showed nothing at all. The 320x200
  modes of the ATM family were off center on every machine that has them.

- Applying settings wiped the text mode font.

- ZX Evo: a number of hardware details the service ROM and NedoOS depend on - reading the
  palette, the font and the virtual drive mask back, entering and leaving TR-DOS, and page
  write protection.

- An NMI no longer leaves the Z80 stuck in HALT.

## 2026.4 - 2026-09-11

### Added

- **Opening a file can switch to the machine that runs it** - a `.spg` to the TSConf, a 128K
  snapshot off the 48K, a TR-DOS disk to a Pentagon, a `.dsk` to the +3. Options -> Media ->
  File types sets it per format: switch when needed, ask, keep the machine, or always use
  one. A tape or a disk that is only mounted leaves the machine alone, and any change of
  machine says so at the bottom of the window.

- **The window title names the tape, disk or snapshot in use** - a tape once it plays, a disk
  once the machine reads it. That image goes straight into Favorites from the menu or a
  hotkey, under its own name or one you type.

- **Run ahead, experimental**: the emulator works a frame or two ahead of the screen, so a key
  press lands about 20 ms sooner. Options -> Xpeccy+ -> Emulation, off by default. It doubles
  the emulation work, puts the picture slightly ahead of the sound, and stands aside while a
  tape, a disk or an RZX recording is running.

- **A log, for when a screenshot is not enough.** `--log`, or Options -> Xpeccy+ -> General, and
  every event lands in `logs/<date>/` beside the emulator, carrying the frame and T-state it
  happened on and the part of the emulator it came from. `--log-groups video:debug,sound:off`
  tunes those parts one by one.

- **Sound follows the sound card's own sample rate** instead of asking for a fixed one, which
  on Windows could be heard as a faint whistle in quiet moments. Rate is Auto by default;
  44100 and 48000 can still be picked by hand, and the old 11025 and 22050 are gone.

- **Sound latency looks after itself**: 30 ms to start with, up when the buffer runs thin,
  back down after a long clean spell. The slider still sets it by hand, anywhere from 10 to
  150 ms - bluetooth headphones want a good deal more than wired ones. Untick Auto adjust to
  keep it where you put it.

- The virtual keyboard, on the menu and Alt+K, can be resized, and can sit under the emulator
  window at its width. Right-click an empty spot on it for those choices.

- Reload snapshot and labels works outside the debugger too, with a hotkey of its own - pick
  one in Options -> Xpeccy+ -> Keys.

### Changed

- **Profiles are gone: a machine is a machine again.** The emulator knows what a 48K, a 128K,
  a Pentagon or a Scorpion is, and what you change on one is kept for that one - so an update
  can fix a machine without touching your settings. Your old profiles come across on first
  start.
  - Options -> Machine holds what a machine *is*: its CPU, memory, ROMs and the ROM it resets
    into. ROM sets went with the profiles - a machine carries its ROMs, one row per slot, and
    another TR-DOS is another file rather than another set.
  - Machines of your own: set one up, Save machine, and it stands in the list beside the ones
    that ship.
  - The whole configuration is one text file you can export, import, or reset to what the
    emulator ships with.
  - Machines, screen layouts, palettes, shaders, styles and keymaps live inside the binary; a
    file of the same name in the config directory replaces one, so a shipped fix reaches
    everyone who has not touched it.
  - The names are the hardware's. The +2 is a machine of its own; the two Spectrums with a
    disk interface bolted on are not shipped any more - fit one yourself and keep it.
  - The 128K runs on a core of its own instead of the Pentagon's, and the screen layouts are
    named after the ULA they belong to: `ULA.48`, `ULA.128`, `ULA.Pentagon`.

- **Xpeccy+ is a ZX Spectrum emulator only now.** MSX, Game Boy, NES, Commodore 64, BK0010,
  IBM PC, PC-9801, Specialist and ALF are gone, and so is everything only they used - in the
  machine, CPU, disk and hard disk lists, and in the debugger.

- **The border is a choice of fixed sizes** - none (256x192), tiny (272x208), small (288x224),
  medium (320x240), full (352x288) and overscan - instead of a percentage. It starts on full,
  the whole PAL frame a TV of the day showed. Every machine shows the same size picture with
  the screen exactly in the middle, so switching machines no longer moves it around; overscan
  is the one that varies, showing every dot the machine puts out. Changing it now takes effect
  on Apply, window and all.

- **In a window the picture is always drawn at a whole number of pixels per dot** - some
  border sizes used to come out a pixel wider in a few columns at x3 and x5. Fullscreen fits
  the picture to the screen and fills the sides of a wide one with the machine's own border
  instead of black bars. The shader is skipped at size x1, where there is nothing for it to
  work on.

- **The tape player's block list says what is on the tape**: the name in bold, the size in
  bytes, and what a header announces - `PROGRAM LINE 10`, `CODE 32768,2786`. TZX images label
  their own blocks, so a game's levels are named too. The same list is the tape map in
  Options -> Media -> Tape, where the stop mark of a block takes a click again.

- **Gamepad buttons can be bound by name** - A, B, d-pad up, left stick - instead of by
  number, so one map fits pads of different makes. Maps written the old way still work, and a
  `gamecontrollerdb.txt` dropped in the config folder is picked up.

- **Options was rebuilt around the machine.** The Video page keeps what the picture is made of
  apart from how the window shows it; the interface style moved to Xpeccy+ -> General, the old
  Tools page is that General page now, Low latency and the indicators have a page of their own
  (Emulation), Storage is called Media, and most settings say what they do when the mouse
  rests on them.

- **Bookmarks are now Favorites**, and the list has a window of its own - Manage... at the
  bottom of the Favorites menu - instead of taking up room in Options.

- Options -> Media -> Disk: the interleave list shows the order the sectors go in on a track,
  TR-DOS's own (1, 9, 2, 10...) by default. Without fast disk access the drive head steps and
  settles as slowly as a real one.

- Open... in the right-click menu goes straight to the file dialog, for any kind of image, the
  way F3 does, instead of asking first whether it is a snapshot, a tape or a disk. The file
  dialogs no longer cut long file names short.

### Fixed

- **Disks with loaders of their own work again.** CHORDOUT no longer hangs when it goes back
  to the disk, Battle Command finishes loading without stuttering on the way, and the Profi's
  service ROM gets past its disk check.

- **The Profi's keyboard works again** - the cursor keys and the other PC keys no longer
  stick. With Grab keyboard on it takes the layout of the Profi's own PC keyboard, which is
  what the keyboard test in its service menu expects.

- **The sound no longer clicks.** On the machines it happened to, part of the sound was never
  made at all, so no latency setting could help - whether a machine was hit came down to how
  two threads happened to be scheduled, which is why it plagued some people and never showed
  up for others. The buffer also keeps a reserve now and holds it against the drift between
  the sound card's clock and the computer's.

- **A machine that turns its own turbo on** - Scorpion, ATM, ZX Evolution, or a `.spg` that
  asks for one - no longer leaves it on for the machine you switch to next. A `.spg` that asks
  for 14 MHz now gets 14 MHz, not 10.5.

- `.z80` snapshots from a +2, +2A or +3 load now, and a 48K `.z80` no longer drops to the 128
  menu on a Pentagon or any other 128K machine.

- A tape stops at its end when "Rewind at end" is off. It rewound whatever the setting said,
  so a tape the loader could make nothing of started itself over and over.

- A gamepad keeps its setting while it is unplugged or asleep, two pads of the same model no
  longer swap places, and a press reaches the machine right away instead of sitting for up to
  40 ms.

- Picking a palette preset changes the colors right away again on the 48K and 128K, where they
  only changed after a reset. The palette editor had the same problem.

- Recording to WAV writes the file at the rate and depth the emulator is really playing at -
  it always claimed 44100 Hz, 8 bit - and the grainy beat that used to ride on the recording
  is gone.

- The emulator no longer crashes on some Intel graphics a moment after the options dialog is
  closed.

- Opening a tape or a disk starts it whatever keyboard layout is picked.

- Cancel in Options really cancels a ROM picked on the Machine page, and Apply leaves a whole
  picture on screen instead of half of one frame over half of another.

- Holding a key down on the virtual keyboard with the right mouse button works again.

- **Smaller interface fixes**: sliders keep their tick marks under every interface style, the
  Video and Emulation pages line up in columns, the ROM/RAM boxes in the debugger's memory map
  are no longer stretched wide when it opens, and the smaller windows Options opens close with
  the same red cross as the dialog itself.

## 2026.3.2 - 2026-09-03

### Fixed

- TSConf: a `.spg` opened on a machine that has not booted yet is no longer a black
  screen. The palette now starts out as the standard 16 colors, the way a loader leaves
  it.
- TSConf: an effect that sets the border or a tile offset once per line no longer leaves a
  20-pixel strip of the previous line's color down the left edge. The line interrupt now
  arrives with the blanking, the way the hardware sends it.
- TSConf: the border starts out black instead of white, which is where the machine's own
  register leaves it. What color it ends up is the ROM's business - TR-DOS sets black,
  128 BASIC white - and a snapshot loaded without either now looks the same as on hardware.
- TSConf: switching the line or DMA interrupt off now also drops one that is already
  waiting. The demo *cpir* fell apart and started over about twenty seconds in because
  of it.
- The window now comes up before the machine starts running, with a picture in it from the
  first moment, and takes about a third less time to get there. Most visible on macOS,
  where a file opened from Finder could be heard playing before anything was seen.
- macOS: Finder knows which files Xpeccy+ opens, so they are offered in "Open With".

## 2026.3.1 - 2026-09-02

### Fixed

- TSConf: programs that set the TS registers by writing into the FMAPS memory window
  instead of using `out` now run. Reported by Sergei Smirnov.

## 2026.3 - 2026-09-02

### Added

- **A tape or a disk opened in the emulator now starts by itself** - from the Open menu, a
  bookmark or a file dropped on the window, the same as one named on the command line. Turn it
  off with "Auto-run opened media" in Setup - Storage, or hold Shift while dropping a file to
  pick Run or Mount just for that one.

- **The register panel picks its shape from a menu on its header** (right click), and has a
  third one: a wide layout with four columns of registers and the flags standing beside them,
  which sits well above the disassembler. The panel fits itself to the chosen shape and hands
  the freed room to the panels under it, so nothing has to be dragged.

- **The memory heat map has a panel of its own** in the debugger. It draws the address space as
  its four 16K slots side by side, each headed with the range it covers, one square per four
  bytes; a single RAM/ROM page is drawn byte by byte instead. A cell takes the colour of
  whatever the cpu did there most - read, written or executed - and grey when nothing did. All
  three counters can be shown at once or one at a time, and the legend under the picture doubles
  as the readout for the cell under the cursor. A double click jumps the disassembler there.
  Collecting, resetting and exporting moved here from the disassembler's options menu.

- **A folder on the host can stand in for an SD card or a hard disk image.** Pick a folder
  instead of an image file in Setup - Storage (or pass one to `--sdcard`) and it shows up in
  the emulator as a read-only FAT32 card or disk: drop files in from the desktop and they are
  there, no image to build or resize. Long and Russian names are kept.

- `--sdcard FILE` selects an SD-card image at startup. (nihirash)

- A Nix flake, for building and running on NixOS/Nix-based systems. (nihirash)

- On Windows 11, window titlebars now follow the chosen interface style instead of always
  looking like the plain system one.

- The disassembler's jump-target arrow now also shows up for unconditional `jp`/`call` and for
  `ret`, not just conditional jumps and calls.

### Changed

- **The Storage options and the tape player say what they do.** No more "Turbo" and "Autoplay":
  the checkboxes have real names, every icon button has hover text, and the speed slider is on
  both pages, reading out the per cent it is set to.

- **A tape played to its end starts over on Play** instead of doing nothing. "Rewind at end" on
  the Tape page turns that off.

- **The memory map panel is gone - its controls are in the MEMMAP block** of the side panel.
  Each of the four 16K banks is a ROM/RAM box and a page number to type in, and a bank set by
  hand stays highlighted until the machine pages over it. Restore is on the right-click menu
  of the block. Machines that do not page in 16K blocks keep the read-only list they had.

- **A register that changed lights only the byte that changed**, not the whole field - so `IR`
  shows that `I` moved even though `R` moves on nearly every instruction. `PC` and `SP` stay
  whole, nothing touches half of an address. "Split pairs" in the menu on the CPU panel's
  header turns it back into the whole-field highlight.

- **The stack panel fills its whole height** instead of always showing nine entries, and the
  offset it starts from is set in Setup - Xpeccy+ - Debugger. The entries are centered now,
  and the row at SP itself is highlighted and shows its own address instead of an offset.

- **The Leds page of Setup is now Indicators**, a row per indicator: the icon it draws on
  screen, its name and a line saying what it shows.

- **The bundled profiles were gone over machine by machine** and now match the real hardware
  more closely. Profiles you already have are left alone; update them by hand to pick this up.

- The debugger headers that react to a mouse click - CPU, MEMMAP, PORTS and FRAME - are
  marked with a dot in the corner.

### Fixed

- **Files dropped on the window open again when their path has spaces in it**, and a drop is
  never taken as a move, which could have the file manager delete the original. Dropping several
  files at once opens the first instead of loading them over each other, and a dropped `.rzx`
  now starts playing.

- The checkboxes in the breakpoint list follow the chosen interface style, sit centred under
  their headings, and no longer leave a stray outline on the empty half of a cell. Flags a
  breakpoint has no use for - the F/R/W of an IRQ or of a global condition - are left blank
  instead of quietly taking clicks.

- **Memory contention is accurate on the 48K, 128K, +2 and +2A/+3.** The ULA now also
  holds the CPU during an instruction's idle ticks and during `IN`, and the screen is
  read ahead of the beam the way the real one does. `LDIR` across the screen used to
  run about a fifth too fast; timing tests, beam-racing demos and multicolour now match
  a real machine.

- The +2A/+3 raster and interrupt position match the documented figures, and those
  machines no longer contend port access, which their ASIC does not do.

- Pentagon, 128K, +2A/+3 and Scorpion now hold the frame interrupt for the 36 T-states
  the real machines do, instead of the 48K's 32.

- Scorpion ZS 256 draws its borders and retrace at the documented widths, and takes the
  frame interrupt where the real machine does.

- The disassembler no longer loses its selected row's highlight and syntax colours when the
  debugger loses focus.

- Copying or saving the disassembly now keeps the blank line between branches, matching the
  on-screen listing.

- Half-register opcodes now disassemble as `IXH`/`IXL`/`IYH`/`IYL` instead of `HX`/`LX`/`HY`/`LY`
  (the old names still work when typing code in).

- Jumping to an address (F4) or returning (F5) in the disassembler now puts the cursor in the
  right place instead of leaving it off by a row or back at the top. The blank lines between
  blocks count as rows too, so the jump and Page Up no longer step over lines.

- Screenshots no longer have transparent pixels, and ZX screenshots with the border kept are
  now centered correctly.

- On macOS, the hotkey editor now shows the correct modifier for the key you press (Ctrl no
  longer gets swapped with Cmd), and Options opens with the native Cmd+, by default.

- On macOS, Cmd+Q now quits the app (it used to do nothing). Alt+F4 already quit on Windows and
  Linux.

- The debugger window no longer takes noticeably longer to open the first time.

- The icons on the debugger's panel tabs are centered again when an interface style is in
  use, instead of sitting left of the middle.

- Emulation timing is more precise: it used to run every machine up to ~1% faster than real
  hardware, affecting audio pitch and frame rate.

- The FPS indicator now shows a more accurate frame rate, including right after unpausing.

- **Smaller interface fixes**: the toolbar icons read on dark styles, the debugger's context
  menus use the interface font, the breakpoint list keeps its column widths and no longer
  wastes room on the address column, the stack panel comes back its own size after a restart,
  and a constant in the listing goes bold instead of changing colour under the cursor.

### From upstream

Taken from [Xpeccy](https://github.com/samstyle/Xpeccy) build `20260824`, by SAM style.

- A new hardware profile, `PentEvo21` ("Evo Baseconf, after 2021"), alongside the existing
  PentEvo. Marked unstable by upstream.
- Jumping to an address in the disassembler (F4) or returning (F5) no longer leaves a duplicate
  entry in the jump history.

## 2026.2 - 2026-08-21

### Added

- **The side panel of the debugger shows the frame number**, under SIGNALS: frames since
  reset, the same number breakpoint conditions read as `FRAME`. A right click on it puts the
  counter back to 0.

- **The ports in that panel are a list of your own now.** The ones a machine keeps by itself
  are in it from the start, any port on the bus can join them (up to 16 per profile), and each
  one can be switched off without leaving the list. The cell shows the last value that went
  through the port, either way. A port of four digits is the address as it is (`7FFD`, `BFFE`
  for one keyboard half-row); two digits are a byte port and only the low byte counts, which
  is what catches `in a,(31)` on the Kempston joystick or `out (#FE),a` on the border - the
  Z80 puts A in the high byte there. Edited in Options - Debugger, or by a right click on the
  PORTS block.

- **Blocks of that panel can be turned off** - ports, signals, frame, beam position - in
  Options - Debugger. That page is laid out anew: the style picker and the font moved into
  View, the panel switches and the port list sit under it, and the palette takes the whole
  height beside them.

- **Conditional breakpoints** (the way Unreal's debugger does it): a breakpoint can carry a
  C-like condition and only stops when it is true - `bc == 0x1234`, `(out & 0xff) == 0xfd`. A
  condition with no address of its own is a breakpoint in itself: it is checked after every
  instruction and stops while it is true, or only when it becomes true if "On change" is
  ticked. Expressions take CPU registers, labels, memory (`M(x)`, `[x]`), the last memory/IO
  access (`RD`, `WR`, `MDT`, `IN`, `OUT`, `VAL`), the machine state (`DOS`, `SLOT0`..`SLOT3`,
  `FRAME`, the beam position `RAYX`/`RAYY`, `RAY(x, y)` for the instruction the beam passed a
  given dot in) and the breakpoint's own hit counter (`HITS`), so an address breakpoint with
  `HITS > 30` lets the first 30 hits pass and stops on the 31st, and `FRAME == 300` stops on
  the first instruction of frame 300. Numbers follow the same rules as the assembler in the
  disassembler window - decimal, `0x`/`#` for hex, a leading zero for octal - and while a
  condition is typed, the line under the field shows how it was understood, with the
  priorities as brackets. The `?` button in the breakpoint editor lists them all with
  examples. Conditions are saved and loaded with the breakpoint list, and the list can be
  loaded at startup with `--brk FILE`, which also reads Unreal's `bpx.ini` format
  (`x0=0x80A6`, `r0=0x8000-0x8FFF`) - the one sjasmplus writes.

- **The disassembly listing reads more like a listing** (ideas borrowed from Spectaculator):
  the address and the opcode columns can be dimmed, constants are coloured and labels go
  bold, and an empty line follows every `RET`/`JP`/`JR`. Four switches in the debugger
  Options menu, kept between runs; the constant colour is `Const` in the palette editor and
  every bundled style brings its own.

- **Address jumps in the debugger from the keyboard** (the way Unreal's debugger does it): `G`
  puts the cursor on the address of the first row - in the disassembler and in the memory dump -
  ready for a new address. In the dump `Ctrl+P`, `Ctrl+S`, `Ctrl+B`, `Ctrl+D`, `Ctrl+H`, `Ctrl+X`
  and `Ctrl+Y` jump to what `PC`, `SP`, `BC`, `DE`, `HL`, `IX` and `IY` point at, the same as a
  right click on the register name. All of them are rebindable; `Ctrl+S` in the dump no longer
  opens "Save dump".

- **Autoload for tapes and disks given on the command line** (the way a snapshot always
  did). The loading method is the standard one for the machine and is picked automatically.
  To only mount the file, use `--no-autostart`.

### Changed

- Watcher expressions understand the same syntax as breakpoint conditions - comparisons,
  logic, shifts, `M(x)` and the last memory/IO access. Single Z80 registers (`b`, `c`, `h'`,
  `ixl` and the rest) can be used as well. **Two things changed meaning**: a number without a
  prefix is decimal now, where it used to be read in the machine's base, so hex needs `0x` or
  `#` (`hl == 4000` is four thousand, `hl == 0x4000` is the screen) and a name is never a
  number, which is what makes `bc` unambiguously the register pair; and operator priorities
  are C's now, so mixed bitwise and arithmetic can shift - `hl&0xff+1` used to mean
  `(hl&0xff)+1` and now means `hl&(0xff+1)`. Labels, `.name`, `[x]` and `0x` work as before,
  and nothing on disk holds expressions, so saved xmap/label files are not affected.

- **One set of AY/TurboSound settings instead of three.** Sound now asks how many chips there
  are - none, one, TurboSound (NedoPC) or TurboSound (ZX Next) - and the type, clock and stereo
  mode apply to all of them. No machine ever mixed different chips, clocks or stereo layouts in
  one TurboSound, so there is nothing left to set per chip. In the profile they are five
  `psg.*` keys instead of ten; a profile written by an older build is read as before and
  converted the first time it is saved.
- **The stereo separation is adjustable** (the mixing itself is Volutar's): a `Separation`
  slider runs from mono to full panorama, and profiles ship at 75%.
- **Profiles ship closer to the real machines**: the 48K has no sound chip at all, every other
  one has a single chip instead of TurboSound - an AY at 1.773447 MHz on 128/+2/+3, at 1.75 MHz
  on the clones. SounDrive is off, plain Covox stays on the clones. The Sinclair machines play
  the AY in mono, the way a television of the day took it; the clones keep the layout they were
  built with - ABC on Pentagon, ATM and Evo, ACB on Profi, BAC on Scorpion.
- **`I` and `R` are one `IR` field in the debugger**, and share a line with `IM` when the
  register panel is wide enough for two columns.

- **The debugger and the watcher go by their names now** - deBUGa and WUTcha are gone from the
  window titles, the menu, the hotkey list and the Options page. A handful of interface typos
  went with them (`Maping`, `Palete`, `Lenght`, `Ouput`, `MSX Maper`, `Debuger`).

### Fixed

- A breakpoint loaded from a file could come up with random flags - most visibly a stray
  "temporary" one - because the loader left part of the record uninitialised.

- **The context menu of a changed register is readable again.** It took the pink
  highlight of the field as its own background.
- **The register panel comes back the way it was left.** It was rebuilt in one column on
  every start, whatever the saved layout said, and pushed the panels under it down.
- **Two more visual artefacts in ZX Evo (TSConf) demos**: a seam running along the horizon,
  and single dots of a wrong colour drifting about the picture. Both show in the greetings
  part of *Synchronization*.

- **Smaller interface fixes**: the `Sublime` style takes its chrome colours from Monokai
  Classic and keeps constants readable on the PC row, the hotkey list no longer cuts long
  names short, and a picked AY frequency is readable again in the sound settings.

### From upstream

Taken from [Xpeccy](https://github.com/samstyle/Xpeccy) builds `20260814` to `20260816`, by
SAM style.

- A palette panel in the debugger: all 256 colours at once, with the index and the RGB of the
  one you click.

## 2026.1 - 2026-08-14

Entries marked **(Volutar)** are the work of [Volutar](https://github.com/Volutar), taken from
[his fork](https://github.com/Volutar/Xpeccy) with his authorship kept on every commit.

### Added

- **Eight interface styles**, in `config/styles` - pick one in Setup - Xpeccy+ - Debugger -
  Style Sheet.
  `Light`, `Dark`, `ZX Spectrum`, and five from the editors people already use: `Sublime`
  (Monokai), `Gruvbox` (Pavel Pertsev), `Solarized Dark` and `Solarized Light` (Ethan
  Schoonover), `Dracula` (Zeno Rocha and contributors) - their palettes, under the same MIT
  license. Each style also brings the debugger colours a style sheet cannot reach, in a `.pal`
  file next to it; they stay editable in Setup - Xpeccy+ - Debugger - Palette.
- **DejaVu Sans Mono ships with the emulator** (Volutar), so the debugger lines up the same way
  everywhere - macOS has no copy of it at all. It keeps its own license, see `LICENSE_DEJAVU`.
- **Kempston joystick (port `#1F`) on ATM Turbo 2+, ZXM-Phoenix, ZX Spectrum +2 and +3.**
  (Volutar)
- **Disk images with more than 80 tracks**, in both `.trd` and `.scl`. (Volutar)
- **macOS builds again, and stays that way** - every change is built, started and packed into a
  dmg on an Apple silicon runner.
- **The debugger's panels can be arranged freely** (experimental). Registers, disassembler,
  memory map and stack are dock panels now, so any of them can be dragged, split, tabbed or
  put side by side, and the arrangement is remembered. Registers switch to two columns when
  the panel has room for them. `Reset panel layout` in the disassembler options puts
  everything back.
- **Bytes per row in the dumps** - `Auto`, `8`, `12` or `16`, next to the code page. `Auto`
  fills the width a group at a time. The track dump has it too, where a line was always
  eight bytes long.

### Changed

- **ZX Evo (TSConf) has a screen geometry of its own** instead of borrowing Pentagon's. Its
  border and blanking are where the real machine has them, which is what raster effects and
  the line interrupt are timed against.
- **The debugger opens at 960x720**, and its font and screen zoom start smaller, so the window
  fits a 1280x800 display.
- **Labels stand out in the disassembler.** They share the disk ID colours, so a style sets
  them and Setup - Xpeccy+ - Debugger - Palette retunes them.
- **The dumps read in groups of four.** Cells no longer stretch with the panel: they keep a
  fixed width, line up from the left and leave a gap every four bytes, the same in the
  memory, track and register dumps. A register dump row now says which pointer it follows,
  `HL (1234):`.
- **Small changes to the defaults**, mostly to get around conflicts found on macOS:
    - profiles no longer pick a keyboard layout. The one they carried was laid out for 48K
      machines only, so a 128K one was added next to it. Details in `config/keymaps/README.md`
    - border size is now 100% at scale 2 - not every configuration was shown correctly
- **More fits on the debugger's screen** (Volutar): tighter margins, spacing and row heights
  across 23 panels, nine stack entries where there were six. No panel moved.
- **The on-screen keyboard is drawn the classic skewed way**, with a key map to match.
  (Volutar)
- **Less separated AY stereo** (Volutar) - three sixteenths of each side channel bleed into the
  other. Volume unchanged, mono sounds exactly as before.
- **Windows: `Shift+Alt+Space` sets a read breakpoint** on the cell under the cursor (Volutar).
  Plain `Alt+Space` belongs to the system menu there, so that breakpoint had no key at all.
  Other platforms are unchanged.

### Fixed

- **An update brought no new files to an existing installation** on macOS and Linux. The
  shipped configuration was copied only into an empty config directory, so styles, roms or
  palettes added by a later version showed up only after deleting the old one. Whatever is
  missing is filled in on every start now; files already there, edited or not, are left alone.
  Windows runs from that directory and was never affected.
- **ZX Evo (TSConf): a number of problems with keeping up with the ray**, in every screen mode.
  Programs that drive the picture from the raster - most demos - lost or shifted lines, showed
  strips of rubbish next to moving objects, and rippled over screens that should have been
  still.
- **macOS drew a black screen** - it has no OpenGL compatibility profile above 2.1, so every
  shader failed to compile while everything else carried on working. It asks for a 3.3 core
  profile there now.
- **A style sheet took the debugger's font away**, dropping the disassembler and the dumps to
  the interface font, proportional and a size smaller.
- **Labels in the disassembler were drawn in the interface font** as well, for the same reason.
- **The track dump's field markers ignored the palette** - three pastels compiled into the
  panel, so a dark style hid the bytes. They are palette entries now, background and text.
- **The emulator crashed on its way out when the window had never been shown**, which is
  exactly what `--help` does.
- **The first frame bound a texture that had never been generated.**
- **ZXM-Phoenix paged the wrong memory** (Volutar): bits 4, 6 and 7 of port `#1FFD` were masked
  and shifted as one.
- **Profi did not initialise port `#DFFD` on reset.** (Volutar)
- **The debugger's disk dump stopped at 83 tracks**, a hardcoded number derived from nothing.
  It reads the drive's own geometry now.
- **Long profile names were cut off** in Setup - Xpeccy+ - Profiles, and the mark on the
  current profile pushed its name sideways.

### From upstream

Taken from [Xpeccy](https://github.com/samstyle/Xpeccy) builds `20260809` to `20260811b`, by
SAM style. The base was build `20260807`.

- ZX Evo (TSConf) interrupts: its own handler for them, the line and DMA sources work, and a
  heavy screen no longer freezes the machine. *Synchronization* used to stop three and a half
  minutes in. It supersedes the fix we made in 2026.0.
- ZX Evo (TSConf) reads the PC keyboard as well as the ZX matrix, the way PentEvo already did.
- PentEvo: virtual DOS and NMI memory banks, and write protection per memory page.
- YM2203: an envelope rate of 0 now holds forever instead of creeping, and the fastest attack
  rates reach full volume at once. The rates are worked out when a register is written rather
  than on every tick.
- The build works again with `-DUSEOPENGL=0`.

## 2026.0 - 2026-08-08

### Added

- **Much lower input lag** - about 20 ms where it used to be about 60 ms, which is the tier
  the other ZX emulators are in. Emulation used to run in bursts the size of the audio
  buffer and the display used to show the oldest of three queued frames; it is paced by a
  wall-clock timer now and shows the newest frame. The new "Low latency" switch in
  Options - Video is on by default; turn it off if the motion looks uneven on a plain 60 Hz
  display.
- **Memory heat map**: per-cell read, write and exec counters for RAM and ROM, shown in the
  debugger and exported as CSV (also written next to a saved `.sna`). `tools/heatmap_png.py`
  renders the CSV as a PNG.
- **AF and AF' are back in the debugger.** The accumulator is shown as a register pair, the
  way ZX debuggers have always shown it, instead of `A` alone with the flags only as
  checkboxes. `A` and `A'` still resolve by name in watcher expressions and breakpoint
  conditions. Widening the register panel puts each register next to its alternative in a
  second column.
- **A release runs right after unpacking.** `config/` carries 29 rom images covering every
  ZX-compatible machine the emulator supports, with a romset and a ready profile for each -
  video geometry, contention, sound chips and the disk interface the machine shipped with -
  plus 20 CRT shaders, nine palettes and a default keyboard and gamepad mapping. Everything
  is named after the machine it belongs to: profile `ZX Spectrum +3`, romset
  `ZX Spectrum +2A/+3 (v4.0)`, geometry `ZX +2A/+3`. The defaults are a Spectrum's rather
  than the code's: an AY-3-8912 at 1.7734 MHz in ACB stereo on the Sinclair line, a YM2149
  at 1.75 MHz in ABC with SounDrive on the clones, the real 3.5469 MHz on the 128K family,
  a 3x picture with a 65% border and a CRT filter, Kempston mouse on everywhere. The rom
  images and the shaders keep their own licenses, separate from the MIT license of the
  project - see `config/roms/LICENSE`, `config/roms/PROVENANCE.md` and
  `config/shaders/README.md`.
- **Ready-to-run builds for both platforms.** On Windows `packaging\make-dist.ps1` stages
  the binary with the Qt and SDL runtime, `config\` and the docs, and can zip it; presets
  cover Qt 5 on x86 and x64 and Qt 6 on x64. On Linux `packaging/make-appimage.sh` packs an
  AppImage, so one file covers Ubuntu, Debian, Fedora, Arch and SteamOS. Both use CMake's
  own install layout, which a `.deb` or `.rpm` built with CPack carries too, and `config/`
  is part of it - a Linux package now starts with the same 16 machine profiles a Windows
  release has instead of the single built-in 48K rom. Each build also carries the metadata
  its platform expects and neither had before: product name, version, description and
  copyright on the Windows binary, description, license and documentation in a Linux
  package.
- **A boot loader for TR-DOS images.** "Add boot" is on by default and now has something to
  add: a `.trd` or `.scl` with no `boot` file of its own gets `config/boot.$B` appended when
  it is opened, so the disk can be started without typing a command. Dimon boot 2024 by
  Dmitry Yurinov - see `config/roms/PROVENANCE.md`.
- **Version scheme**: the tracked `VERSION` file is the only source, and the window title
  carries the whole thing - `Xpeccy+ (2026.0-dev+20260807)` for a development build,
  `Xpeccy+ (2026.0)` for a release.

### Changed

- Renamed to **Xpeccy+**. The binary, the desktop entry and the package are `xpeccy-plus`;
  window titles and the exported file headers carry the new name. The configuration files
  themselves are deliberately untouched, so existing settings keep working.
- **Own configuration directory on Linux and macOS**: `~/.config/xpeccy-plus` instead of
  upstream's `~/.config/samstyle/xpeccy`, which the fork still used - installed side by side,
  the two shared one `config.conf`, one set of profiles and one rom directory, and overwrote
  each other's settings. `$XDG_CONFIG_HOME` is honoured now where it used to be ignored.
  Nothing is migrated: the first run starts from the defaults, and the old directory is left
  alone - point `--confdir` at it to keep using the old settings. Windows is unaffected, its
  configuration has always lived next to the binary.
- **"Preset" in the romset editor** now fills in the names of the bundled images instead of
  names from the author's own machine.
- **The sources mirror the runtime layout**: `conf/` became `config/`, the same shape the
  emulator expects next to the binary. The built-in emergency defaults, used when no
  configuration is found at all, moved to `res/fallback/`.

### Fixed

- **Tape did not start for loaders that bypass the ROM routine**, so they had to be started
  by hand.
- **Register fields in the debugger sized themselves wrong**: one fixed width for every
  machine, too wide for a byte register and too narrow for a 32-bit one, and a 32-bit field
  (`PSW` on BK0010) took only a single digit of input. They follow the value and the number
  base now.
- **ZX Evo (TSConf) drew nothing but black.** The window into the FPGA at the address in
  `#15AF` was still tested as `flag & 0x10` after that flag became a `bool`, so it never
  opened: nothing could reach the palette or the sprite file. Loading screens, sprites and
  backgrounds were all missing. An upstream regression from build `20260418b`.
- **TSConf stalled wherever software drove the line interrupt.** The CPU only accepted an
  interrupt while a frame interrupt was being held, so the line and DMA sources were taken
  roughly once a frame instead of once a scanline, and demos slowed to a stop.
- **TSConf did not get through its boot rom.** `#21AF` bit 0 is the same rom select as
  `#7FFD` bit 4, but only the latter was recorded, so the wrong rom was paged in and the
  `#3Dxx` entry into TR-DOS never fired - the boot rom looped forever with the stack
  pointing into rom.
- **TurboSound had its two chips the wrong way round**: `#FFFD` `#FF` selects the first AY
  and `#FE` the second.
- **The AY could not be detected through port `#FFFD` any more.** Upstream's TSFM work made
  "read the status register" the state every machine powers up in, so a program that probes
  the chip by writing a register and reading it back got a status byte instead: a plain 128K
  reported no sound chip at all. Reading registers is the power-up state again, and the
  status register is only readable where the hardware has one.
- **Crash when switching profiles.** A profile's machine is created on first use, but the
  emulation thread could pick up the profile before that machine existed, and use it while
  it was still being built. Present upstream as well.
- **`--confdir` set up the configuration twice**, which leaked the gamepad controller, added
  a second `default` layout and profile, and left the windows holding values from the first
  configuration. The option is read before anything is initialised now.
- **Windows with no icon of their own** - options, tape, rzx, watcher - had none at all on
  X11, where there is no exe resource to fall back to. The application now carries an icon,
  which Qt hands to every window that does not set one. The Linux desktop entry also points
  at a path the icon theme spec knows, `share/icons/hicolor/128x128/apps`, instead of a flat
  `share/icons`.
- **The x86 trace log wrote one garbage character in place of the `CS` register**, since the
  value was appended as a character rather than as a hex word. It also broke the Qt 6 build.
- **Link error in MinSizeRel and Debug builds** (`lr_swaph` declared C99 `inline` with no
  external definition).

# Changelog

Notable changes in Xpeccy+, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), versions use CalVer
(`YEAR.NUMBER[.PATCH]`).

Xpeccy+ starts from [Xpeccy](https://github.com/samstyle/Xpeccy) build `20260807`. Anything
before that point is upstream's history and is not repeated here.

## Unreleased

### Added

- **A menu bar, a toolbar and a status bar**:
  - The menus carry every command with its hotkey. File keeps the last ten images opened;
    Machine, Media and Debug hold what the toolbar's buttons do. A command shows its icon and
    a setting its tick. *(thanks to Volutar)*
  - The toolbar holds the buttons you choose: right-click it to add or remove one, drag one to
    move it. It starts with Mute and the virtual keyboard.
  - The status bar shows the machine, the clock, the tape and each drive; a click opens its
    window, a right-click its menu. The frame rate leaves the picture while the bar shows it.
  - In fullscreen they come up when the pointer rests at the top of the screen. With Low
    latency on, the screen flashes as a menu opens there. *(thanks to Volutar)*

- **Mute**: a toolbar button silences the speakers, while recordings keep the sound. The volume
  is beside it, and Alt+wheel over the picture sets it - the bare wheel no longer does, it was
  too easily turned. *(thanks to Volutar)*

- **Quick save and quick load** on F5 and F9, and Shift+F9 takes a quick load back: the save
  outlives the session on every machine. *(thanks to Volutar)*

- **SZX snapshots**, the default format now: Save writes one, and so does quick save.
  - A 48K, 128K, +2, +2A, +3, Pentagon or Scorpion snapshot opens in the other emulators that
    read the format, and theirs open here.
  - One saved here puts the machine back exactly: the sound chips, the beam to the dot, a disk
    command or a tape block halfway through.
  - ZX Evolution, TSConf, ATM, Profi, Phoenix and ALF save as SZX as well, which only Xpeccy+
    opens.
  - A snapshot opens on the machine it was taken on. What it was taken with and this machine
    lacks - other ULA timings, ULA+, a Beta 128 - is named in a warning, and the machine's
    settings are left alone.
  - The tape and the disks are linked by name, not carried in the file.
  - What Xpeccy+ keeps in the format, TSConf and TurboSound FM included, is written up in
    `docs/szx-format.md` for other emulators.

- **The RZX player shows what is in a recording**: what made it, the machine, its length and
  its snapshots, with a list of its blocks. A click on the bar goes to that point of the
  recording, a double click on a block to where it starts.

- **Hotkey layouts**: Modern, the new default, and Classic, the keys as they were. Keys you
  change are kept as Custom, whichever layout is picked.
  - Modern leaves the machine every key it has: Esc is BREAK, F10 opens the debugger,
    Shift+F10 is NMI, Shift+F4 takes the tape to its start, and the Alt+arrows rewind, run
    fast, slow down and run flat out.
  - Fast forward, slow motion and Fast mode switch on a tap and hold for as long as the key
    is held; Options, Emulation, has the old toggle and hold too.
  - Ctrl+Alt (Ctrl+Cmd on a Mac) lets the mouse and the keyboard go, and a click on the
    picture takes the mouse only while a program reads it.
  - The Hotkeys page searches by action or by key, says when a key is already taken, and
    takes a second key for each action. *(thanks to Volutar for the search)*
  - An older config keeps the keys you changed and takes Modern for the rest.
  - Hotkeys work in any keyboard layout, and a hotkey's Shift or Ctrl no longer holds Caps or
    Symbol Shift down on the machine.
  - Hotkeys work from the tape, RZX, screen, sound and disk windows too, all but the keys
    those windows use themselves. *(thanks to Volutar)*
  - The debugger's own keys are on the Hotkeys page as well: the kinds of breakpoint, the
    bookmarks and Esc to close it. Ctrl+Break opens it.

- **Labels as you type in the debugger**: label names are offered in the listing, the dump, the
  watcher and breakpoint conditions, and the listing assembles labels in operands, label+2
  included. *(thanks to Volutar for the idea)*

- **Rewind shows how far back it has gone**, in seconds under its sign; signs and messages on
  the picture fade out when they are over (Options, Appearance). *(thanks to Volutar)*

- **Shift held in the open dialog** asks whether to run the file or only put it in, as it does
  on a drop.

- **A Gamepads window**, from the toolbar, the new Input menu or Options, Input: for each player
  a device and the joystick it stands for - Kempston, either Sinclair port, Cursor/Protek/AGF,
  QAOP or keys of your own. A pad plays at once, nothing to bind. *(thanks to Volutar for the
  design)*
  - Its table shows what the Spectrum gets and what presses it. A row can press the player's
    joystick, whichever is picked, any Spectrum key - True Video, Edit and the like by name -,
    a Kempston button, the Kempston mouse, which speeds up the longer it moves, or an emulator
    action, with turbo. Its button is pressed on the pad or picked from a list, and a button given a row of
    its own stops pressing the one it had by default.
  - The PC keyboard can be a player too, on the arrows and Ctrl or on WASD and Space, so a game
    that wants QAOP plays on the arrows.
  - Bindings save to a .pad file and load back; an older .pad or config is brought across.
    A .pad named after an image comes with it for player 1, over the player's own bindings
    and until the next image; Save as offers that name, and a change asks where it goes.
  - The Input menu and the toolbar button's list switch the first player's joystick mid-game.
  - Picking Kempston switches it on for the machine, and a pad never plugged in before takes a
    free player and opens the window. A pad coming or going - plugged in or pulled out, woken up
    or gone to sleep - says so on the screen.

### Changed

- **Options** is one list of pages down the left side, with Appearance and File types of their
  own and the hotkeys in sections, as in the menus. *(thanks to Volutar for the idea)*

- **Menus no longer pause the machine.** It can pause instead while another application has the
  focus (Options, Application).

- **The numpad no longer plays the Kempston** in the stock keyboard layouts: the keyboard as a
  player in the Gamepads window does it, on whichever keys you like.

- **Smaller things**: two floppy drives unless a machine says otherwise, Fast disk access in
  the disk manager too, Run ahead no longer marked experimental, and an About that names the maintainer and the license and no longer
  stops the machine. *(thanks to Volutar)*

### Fixed

- **RZX playback** keeps in step with the recording to its end, plays recordings that carry an
  SZX snapshot or keep it beside them, and two copies of the emulator no longer mix theirs up.

- **The AY** answers to registers 0-15 only and reads back its port pins, as the chip does: a
  TurboSound FM tune on a plain AY no longer plays its FM part, and a 128K looks for its keypad.

- **.sna and .z80 snapshots** keep interrupts as saved, in an NMI handler too, and a +2A or +3
  one taken in 48 BASIC loads with its ROM.

- **Tool windows** - the tape and RZX players, the disk manager - open over the middle of the
  main window, not a little to its left. A taller tape player gives the room to its block list.

- **Rewind at end** works for a program that loads its own parts, such as levels: a tape that
  has run out goes back to the start when the program asks for a block that is on it.

- **Debugger**: a click on the palette shows the color clicked, an instruction's bytes are
  edited without blanks padding the field, a label from the list goes into a watcher once, and
  the CMOS and disk panels show only on machines that have them. *(thanks to Volutar for the
  report)*

- **Antiflicker** no longer flickers while a window is being dragged, and on Windows a click on
  a title bar no longer holds the picture for half a second. *(thanks to Volutar for the
  report)* The adaptive modes leave alone a game that scrolls slower than the frame rate, such
  as Robocop, where the picture blinked as it moved.

- **The docked virtual keyboard** shows over the picture in fullscreen with Low latency on; it
  took clicks without being seen. *(thanks to Volutar for the report)*

- **A folder as an SD card or a hard disk** always has the files in its root, however much its
  subfolders hold. It takes ten times as many files, mounts thousands of similar long names in
  seconds and says what does not fit. *(thanks to nodeus for the report)*

- **Large disk images**: SD cards over 1 GB work - one of 1 to 2 GB hung the emulator, a bigger
  one read as empty - and so do hard disks over 2 GB on Windows. *(thanks to nodeus for the
  report)*

- **Interface styles**: links take a color of the style's own, the selected tab stands out, and
  a checked menu item with an icon is framed. *(thanks to Volutar for the report)*

- **MIC is heard with the beeper** on the 48K, 128K and +2, a tenth as loud as EAR: speech in
  Cobra's Arc and the second voice in Dizzy are back.

- **128K and +2 paging**: port #3FFD pages memory like #7FFD, as on the real machines, so
  software that tells a 128K from a +3 that way no longer takes it for a +3.

## 2026.6.2 - 2026-10-03

### Fixed

- +2A/+3: once 48 BASIC has locked the memory paging, port 1FFD is locked as well and can no
  longer switch the ROM or turn on the all-RAM mode.
- Z80: SCF and CCF after a DD or FD prefix set flag bits 3 and 5 as a real CPU does - found by
  Tony Brewer.
- Kempston joystick: port 31 reads 0 with the stick at rest, as the interface's manual says, and
  on the 48K it answers whatever the upper byte of the port address is.
- +2, and any machine set to late timings: ULA snow is back. The +2 now snows as a real one does
  in Mark Woodmass's IR Contention 128.

## 2026.6.1 - 2026-10-03

### Fixed

- **The 48K, 128K, +2, +2A and +3 behave as the real machines do.** Each point is checked against
  a test as run on the real machine, with results from the
  [ZX Spectrum tests wiki](https://github.com/redcode/ZXSpectrum/wiki): photos by Guesser, Ricardo
  Martínez Cantero, Víctor Iborra, Marta Sevillano Mancilla, Brendon Alford and Richard Chandler.
  - +2A/+3: memory waits come two ticks earlier and last a tick longer on each line - Patrik Rak's
    Timing Test on a real +3 and +2A. An unused port between screen reads returns the last screen
    byte - Hikaru's +2A/+3 Floating Bus Test on a real +2A.
  - 128K and +2: reading port 7FFD switches memory pages, as on the real board - Mark Woodmass's
    FloatFFD on a real 128K.
  - 128K and +2: a port in the #C000-#FFFF range waits like the memory there when a slow page is
    paged in. Patrik Rak measured it on a real 128K in 2007: "The IO behaves the same way as on
    48k, with the addition of contending also ports in #C000-#FFFF range in case a contended page
    is paged in" ([World of Spectrum](https://worldofspectrum.org/forums/discussion/comment/215386/#Comment_215386)).
  - The +2 runs with late ULA timings, as every +2 tested does, and the 128K keeps early ones -
    Richard and Tim Butler's 128K timing tests on three 128Ks and three +2s, and Rak's test and
    Woodmass's OUTI Time on a +2.
  - 48K: the EAR bit still reads 1 for a short while after a program turns it off - Weiv's
    FE-Delay on a real issue 2 and issue 6A 48K.
  - A machine switched on starts at a random point of its frame, and a reset leaves it where it
    was - Peter Helcmanovsky's Time to First INT on a real 48K.

## 2026.6 - 2026-10-02

### Added

- **Video recording**: Ctrl+F7 writes the picture and the sound to an MP4 or MKV file through
  FFmpeg, in a form any player opens. The settings are on Options -> Video.
  - The bare picture scaled by whole pixels, or the window as shown, shader included.
  - The machine's own frame rate or 50 fps, a folder and a file name template.
  - H.264, H.265 and AV1, on the graphics card too (NVIDIA, AMD, Intel), or FFmpeg's own
    options typed in.
  - It can start and stop by itself - at the program's first code, at a reset, or when the PC
    reaches an address - and the command line has switches for all of it.
  - Without FFmpeg, the record key offers to get it: downloaded on Windows, installed by the
    package manager on Linux and by Homebrew on macOS.

- **Rewind**: hold Delete to go back through the last 30 seconds of play, picture and sound,
  and carry on from any point. The step and the length are on Xpeccy+ -> Emulation.

- **Slow motion and fast forward keys**: End and Home run the machine at 1/4 and x4 of its
  speed, sound included, as a toggle or while held. The speeds and the key behavior are on
  Xpeccy+ -> Emulation; loading a tape, disk or snapshot goes back to normal speed.

- **A VCR-style sign in the corner of the screen** while the machine is paused, or while
  rewind, fast mode, fast forward or slow motion is on.

- **Gamepad buttons press hotkeys**: a key bound to the pad acts as that key on the host
  keyboard, so it can rewind, or pause the machine and start it again.

- **Scorpion ZS 256 Turbo+** as a machine of its own, beside the yellow-board Scorpion ZS 256:
  a longer frame of 316 lines, the 7 MHz turbo with the waits the board puts on memory and
  ports, ProfROM 4.01 and SMUC. Both machines are built from the boards' schematics
  *(redrawn by romychs)*.

- **Profi v3 and Profi v5** instead of one Profi that was a mix of both. Each starts in its
  own factory BIOS: JV "Kramis" V.02 on v3, Micco ROM Bios 2.0 on v5 (1.0 is bundled too).
  - v3: 256K to 1M, including the 768K a board with three rows of chips had; 512x240 in the
    border's two colors, as the board draws it.
  - v5: the palette, the clock and the hard disk, on the ports the v5 controller gives them.
  - The 7 MHz turbo takes the waits the board puts on RAM, and goes off while the disk
    drive's head is loaded *(thanks to Volutar for the measurements and the schematic)*.
  - Fixed on both: paper brightness in 512x240, the Covox (on the parallel port, in stereo),
    writes to `#DFFD` reaching the sound chip, and the disk ports under CP/M.

### Changed

- **Adaptive antiflicker leaves moving pictures alone and follows scrolling GigaScreen.**
  Antiflicker is still off out of the box (Intensity 0); once switched on, it now starts in
  the adaptive 2-/3-frame mode, with Look ahead on: it catches a flicker sooner by running
  the next frames in advance.

- **Clearer gamepad binding window**: the pad's buttons have plain names ("Right trigger",
  "D-pad up"), and the choice follows the field you click into.

- **Tidier Options**: controllers and chips are named by their part numbers - Beta Disk
  (WD1793), NemoIDE, YM2149 - the settings groups have icons, and the check boxes of the
  Video page stand in one column. The Drives menu shows a folder icon for a folder.

### Fixed

- **Snapshots no longer crash soon after loading.** A `.z80` comes back exactly where it was
  saved; a `.sna` keeps no place in the frame, so F2 saves one at the frame's interrupt. The
  save dialog suggests `.z80`.

- **Bundled styles**: the Options tabs sit on a base line, radio buttons keep their place when
  checked, and separator lines are no longer near-black in the dark styles.

- **A held hotkey no longer repeats**, so fast mode, pause and the like stay switched instead of
  flickering; only the speed steps repeat.

- **Switching machines ends slow motion** set on the speed slider; it used to carry on
  unseen.

- **No crash when switching to TurboSound FM** in the options while a machine runs.

- **Disk access and waiting for a key no longer start the tape**, with its sound and the fast
  loading sign.

- **Clock chip**: the ZX Evo start menu shows its clock, every cell of the chip's memory keeps
  to itself, the day of the week counts from 1 and the alarm registers read back - on ZX Evo,
  Pentagon 1024, Profi and SMUC. ZX Evo settings saved earlier are carried over.

- **TSConf, ZX Evo and ATM Turbo show their own screen modes whole** whatever the border size;
  on TSConf the border setting is locked.

- **TSConf follows its FPGA sources more closely:**
  - interrupts, DMA, 1024K paging and SPG files work as on the board, and RAM mapped at #0000
    keeps its write protection;
  - sprites, tiles, the border and the palette are drawn as the board draws them, changes in
    mid-frame and text mode included;
  - at 14 MHz the processor waits for memory its cache does not hold, and DMA takes time;
  - disks run from the SD card as virtual drives, and TR-DOS can page memory and play sound.

- **ZX Evo (BaseConf) follows its FPGA sources more closely:**
  - Pentagon 16-color mode, hardware multicolor and the ATM screen modes show as they should;
  - keys read through the PS/2 port, the Magic Service, a reset and the frame interrupt
    behave as on the board, and a reset no longer says "SD card lost";
  - the ports, the palette, memory write protection, the clock and the disk ports in TR-DOS
    answer as the board decodes them, and Covox plays in TR-DOS too;
  - 14 MHz is no longer a flat 14: with the waits the board puts on memory, the sound chip and
    the disk controller, it is about 11 MHz on typical code.

- **Scorpion ZS 256 follows its board:**
  - the maker's own ROM 2.95, and the ports, the frame interrupt and the instruction timing
    of the yellow board;
  - the Magic button opens the service monitor from 128 BASIC and TR-DOS from 48 BASIC;
  - the Kempston port reads 0 with no joystick instead of #FF.

- **SMUC works with ProfROM**: its clock keeps time, its NVRAM keeps all 2K of settings, and
  the Kempston mouse works in the ProfROM monitor.

- **Hard disk images and SD cards**: the whole of a large image is reachable and the disk
  reports its real size, so NedoOS installs on it and boots from it; a new disk supports LBA,
  the NemoIDE alternate status port works, and a sector written to an SD card is no longer
  shifted by a byte. A folder with NedoOS in it, served as a hard disk, boots on ZX Evo too.

## 2026.5.1 - 2026-09-26

### Added

- **Arrow buttons on the debugger's stack panel** step it a word at a time.

### Changed

- **Faster tape loading.** Fast loading takes about half the time and shows the loading
  screen as it comes in, and Flash loading also works for games that carry their own copy
  of the ROM loader.

- **Better TZX support**: every block type is played, and tapes that lost blocks or reset
  after loading now start.

- **Reliable tape automatics**: auto play and stop follow the loader, known or not, and the
  tape stops once the game has taken over.

- **Faster emulation** in fast mode, about 15% on a Pentagon.

- **No Kempston joystick on the stock 48K, 128K and +2**, as they came. Switch it on in the
  machine's settings for a game that wants one.

### Fixed

- **More accurate 48K and 128K timings**: Richard Butler's timing tests pass in full, the
  128K with late timings.

- **The PSG clock follows the chip type** when switching between AY, TurboSound and
  TurboSound FM.

- **SAA1099 is back** among the machine's Sound devices, lost from Setup in 2026.5.

- **A `.z80` snapshot loads onto a reset machine**, as a `.sna` does.

## 2026.5 - 2026-09-23

### Added

- **Fast loading runs a tape through in seconds, whatever its loader.** One switch in the
  tape player: while a loader reads the tape the machine runs flat out and silent, the
  loading screen is shown as it comes in, and the game starts at normal speed from its first
  frame - R-Type takes about four seconds. Under it in Options, both on: **Flash loading**
  hands ROM blocks straight to the machine (what Fast loading used to mean), and **Edge
  detection** skips the wait for each pulse in the common loaders - switch that one off if a
  game does not load.

- **Disk manager** in the right-click menu, with Open, New, Save, Eject and Write protect for
  each drive. Clicked, it opens a window that lists a TR-DOS disk under its title, keeps up
  with what is written to it, and copies files to the tape or out as Hobeta or raw. A changed
  disk or tape is marked with a star. *(thanks to Volutar)*

- **The debugger's sound panel, rebuilt.** One tab per chip - PSG1, FM1, PSG2, FM2 - built from
  the machine in front of you, and it moves into a window of its own with the button, the menu
  or Alt+A.
  - The PSG page has all sixteen registers and a row per channel: the period, the mixer, the
    envelope drawn as its shape, and how loud the channel is as a meter.
  - The FM page has the timers and the channel 3 mode, a tab per channel, a row per operator
    with every parameter the chip has, and a star on the operators that reach the output.
  - Every number can be typed over, and a PSG channel can be silenced on its own.
  - Under both pages is an oscillogram of the sound, fine enough to count the edges of a beeper
    routine while you step. *(thanks to Volutar for the design)*

- **ALF TV Game**, the Belarusian ZX Spectrum console, in the stock 64K shape and with the
  128K expansion. Its ROM is bundled, so it boots into the games menu, and a cartridge opens
  as a cartridge image. It has no keyboard, so the keys drive its two joysticks: arrows with
  Space or Enter for player one, WASD with left Shift or left Ctrl for player two.
  *(thanks to Prusak, whose [zxbyte.ru](https://zxbyte.ru/alf.htm) has the schematic and the
  ROM dumps)*

- **Closer to the real machines.**
  - The floating bus, the Sinclair way on the 48K, 128K and +2 and the Amstrad way on the +2A
    and +3, which is what games that follow the beam - Cobra, Sidewize - need.
  - The ULA snow effect on the 48K and 128K: the rubbish on screen when the interrupt vector
    points at a screen bank, with a second switch for the machines whose RAM hangs under it.
  - Pentagon starts with the memory pattern a real one comes up with, specks and all.

- **Snapshots can be saved as `.z80`**, not only `.sna` - give the name a `.z80` ending in the
  save dialog. It is packed, so it comes out three to five times smaller; a machine the format
  has no name for says so rather than write something wrong.

- **The tape can be saved as a WAV recording**, to play back into a real machine - the
  button beside Save in the tape player. The dialog sets the rate, the sample size, the level
  and the silence at either end. Recordings written by earlier versions were mistimed, too
  quiet and cut off at the end; re-export anything that would not load.

- **A breakpoint can write down what it caught.** Tick Log in the breakpoint editor and every
  hit leaves a line in the event log - where it fired, the access and its byte, the registers
  and the pages in - alongside whatever else the breakpoint does. The log itself has to be on
  (Xpeccy+ -> General).

- **Alt+T switches the turbo the machine really has** - 7 MHz on a ZS Scorpion, ATM Turbo 2+,
  Pentagon 1024SL, Profi and ZXM-Phoenix, 7 and 14 MHz on ZX Evolution and TSConf, and Turbo
  mode in the right-click menu picks a step directly. The clock it lands on is shown bottom
  right for as long as it is not the machine's own, and the machine switching its turbo by
  itself says so too.

- **Reset to 48K, 128K, DOS and Service** as hotkeys, unbound until you give them keys -
  Reset to DOS included, which loses Alt+F12. The Reset menu names the starts the machine
  really has instead of ROM page numbers.

- **Two filters on the Sound page**, both on by default. High cut (anti-alias) takes off what
  is too high to be played back instead of folding it into what you hear, for about 1.5% of a
  processor core; Low cut (DC) takes each device's offset off before the mixer, so one of them
  drifting - a General Sound does - no longer eats the headroom they share. *(thanks to
  Volutar)*

- **`--wav-out FILE`** records the sound to a WAV file from startup, the same recording the
  hotkey makes.

### Changed

- **The machine's devices are on the Machine page of Options.** Storage, Input, Sound and
  Board, one row per device with its choice and a window for the rest. The ROM set is one
  button there, and the ROM a reset starts from is picked on its slot. The images moved to the
  right-click menu, so the Media tab keeps only what happens to the files you open.
  - A Beta Disk can have fewer than four drives, and a drive left out is not there at all.
  - The Kempston joystick can be left out; the +2A and +3 come without it, as they shipped.
  - TurboSound FM is a choice of its own, and the chip clock defaults to Auto: half the
    processor clock for an AY, 3.5 MHz for TurboSound FM.
  - A controller built into a board cannot be swapped out.

- **One slider for the machine's speed**, on the Machine page, with Alt+Plus and Alt+Minus on
  the keypad. Left of x1 picture and sound slow down together; right of it the CPU runs up to
  eight times faster at the same frame rate, the way an overclocked machine does. It always
  starts at x1, and the base clock has moved to Advanced settings. Past 14 MHz, software that
  times itself against the picture may not keep up.

- **The emulation is up to three times faster**, with nothing it emulates done differently -
  every frame, sample and byte of memory comes out as before. Fast forward reaches about x18 on
  the 48K and 128K, x29 on a Pentagon, x30 on ZX Evolution and x6 on TSConf, up from about x10
  and x4, and playing at normal speed takes a third less of the processor.

- **The tape player is the whole tape now.**
  - The transport buttons are laid out like a tape deck's - record, play, rewind, stop,
    eject - and the image it holds is named across the top with Open and Save beside it.
  - Auto play / stop, Fast loading and Rewind at end are on the player. Rewind at end starts
    off, and winds the tape back for the next load as well as for the Play button.
  - A block can be moved, dropped or copied to a disk straight from the list.
  - A reset stops the tape, and switching the machine also winds it back to the start.

- **The debugger's screen panel picks a screen with one click.** Main, shadow, both at once, or
  Auto following the machine; the bank and offset are behind a Custom button.
  - The picture is drawn in the machine's own colors, so a loaded palette, grayscale and ULA+
    all look like the real screen, and the flash attribute blinks.
  - Each picture is headed with the page it came from, and the heading lights up when that is
    the screen the machine is showing.
  - Moving over a dot says where it is and what holds it; a click holds the readout on one dot,
    an address typed in marks where it lands, and the right button copies one.
  - It moves into a window of its own with the button or Alt+S, which stays open after the
    debugger is closed and comes back at the size and in the place you left it.

- **TurboSound FM sounds like the chip it emulates.** The FM half of the YM2203 runs on the ymfm
  core, at the level it has against the AY on a real board, and a track that speaks through the
  chip's CSM mode says its words.

- **The right-click menu is shorter.** Watcher, Screen and Sound chips sit under Debugger,
  Machine, Turbo mode and Reset stand together, and clicking Debugger, Favorites or Reset
  itself does the obvious thing.

- **Advanced settings for a machine is tidier**: the pickers together at the top, the switches
  in one run under them, and Even M1 and the DD palette in a group of their own. The contention
  patterns are named after the chips that have them, Ferranti and Amstrad.

- **The colors a machine starts with are the Xpeccy+ palette** - the one that shipped as a
  preset file, built in now and named `Xpeccy+` in the palette list where it said `default`.

### Removed

- **The machines that are not ZX Spectrum** - MSX, Game Boy, NES, Commodore 64, BK0010, IBM PC,
  PC-9801 and Specialist. The settings lose the CPU type, cartridge mapper and mouse type rows.

- **Octal in the debugger**, which only the BK ever used. Addresses read as four hex digits
  everywhere, and X in a number field switches between hex and decimal.

### Fixed

- **More tapes load.** Loaders that time the tape themselves - ATF, Deflektor, Technician Ted -
  load now, and so do high-speed DeciLoad tapes, on a Profi as well. A tape that starts itself
  no longer eats the pilot tone of the next block, and Auto play starts the tape for a loader
  that does not count its edges in B, such as Styx's. *(thanks to Volutar for the last two)*

- **The tape player does what it shows.** Stop stops the tape until you press play, rewind it
  or put another one in, and Auto play / stop also stops it after a game's own loader. A
  double-clicked block is the one that loads, a tape opened while another plays starts from
  its beginning, the block mark and the progress bar keep up, and the record button records.

- **A WAV is read as the tape it is a recording of**, in under a second. Standard blocks come
  back as their bytes, named in the tape map and ready to save as `.tap`; a turbo loader is
  kept as the signal. Any sample size, float and stereo included, and opening one replaces the
  tape rather than adding to it.

- **A tape block named in BASIC tokens or graphics** showed an empty name in the block list.
  The tokens are spelled out and the graphics drawn now.

- **Sound.** Every AY and YM played a fraction of a semitone sharp. The beeper gets half the
  room the sound chips have instead of a quarter, and is let go on a reset or a snapshot. A
  pause between tape blocks is silent instead of holding a level, and so is a machine with no
  tape. *(thanks to Volutar for the last two)*

- **Z80 details.** `SCF` and `CCF` set the two undocumented flag bits the way a Zilog Z80 does,
  `BIT n,r` and `CPI` no longer leave a wrong value in them, and `RETI` puts the interrupt
  state back the way `RETN` does.

- **`IN #FE` bit 6** reads back the machine's own last `OUT #FE`, which is how a program tells
  an issue 2 board from an issue 3 one. Which board a machine is is a setting on the Machine
  page, and the +2A/+3 feed nothing back at all, as they really do.

- **Snapshots and recordings come back whole.** A saved `.sna` no longer loses BC', DE', HL' or
  the top bit of R, nor overwrites two bytes of a 48K's stack. A `.z80` saved mid-frame starts
  with the beam where it was, and one carrying a ROM page keeps every page after it. On ZX
  Evolution and ATM Turbo 2+ a snapshot runs instead of landing in the boot menu. RZX
  recordings play again, on Windows too, and a saved `.wav` has the right length in its header.

- **ZX Evolution** hears a TurboSound FM player on every AY port its firmware decodes, and no
  longer switches its own clock between 3.5 and 7 MHz. It and TSConf keep their NVRAM settings
  across starts now - whatever was saved before is gone, so set it once more.

- **ATM Turbo 2+ starts disks and tapes itself.** Opening a disk on it no longer switches the
  machine to a Pentagon.

- **A program that drives the SD card itself got nowhere.** The card answered as if it were past
  its idle state, and a single-block read following a multi-block one never stopped. A card
  image and a folder served as one are both affected. *(thanks to Alexander Nihirash for the
  card contents and the test tool)*

- **What you change on a machine stays with it** across a switch to another machine and back,
  and an update can still fix the machine itself. "Restore machine" throws it away, and a
  machine carrying your settings is marked with a `*`. Saving a machine always makes one of
  your own, under a name nothing else wears.

- **Resets and drives.** F12 and Alt+F12 sometimes did not take until a second press. Reset to
  48K on a +2A or +3 landed in the 128K menu. Copying files off a disk with deleted files on it
  picked the wrong ones, and ejecting a changed disk ignored Cancel. A disk in drive B at start
  read as "No disk" the first time TR-DOS turned to it, and the drive light flickered green
  while writing. *(thanks to Volutar for the last two)*

- **A breakpoint on the interrupt no longer stops the machine dead.** Unless it was set to
  open the debugger, the machine stood on the same interrupt and never went on.

- **The mouse moves with the hand on every host.** It no longer shoots off in one direction in
  a virtual machine or over a remote desktop, nor jumps when it is grabbed, and a sensitivity
  setting means the same at every zoom.

- **Smaller interface fixes.** Applying the settings is quick again, and on Windows 10 the
  title bars take a new color at once. The right-click menu no longer picks an item by itself
  when the mouse moves while the button goes down. A window opened from the settings no longer
  lets the settings behind it close first.

## 2026.4.1 - 2026-09-12

### Added

- **The Magic button works on ZX Evo.** F10 opens the EVO Magic Service over the running program
  and goes back to it.

- **TR-DOS emulation.** A drive the firmware marks virtual is served the way a real BaseConf
  serves it. A drive with a disk in it is left alone, so an image you opened still boots.

- **ZX Evo starts with a pattern in its memory**, the way real hardware does.

- **Holding a key on ZX Evo repeats it**, as its own keyboard does.

### Changed

- **ZX Evo is one machine again.** The two entries split by firmware generation are gone, and
  the one that remains runs either. It ships with EVO Reset Service 0.61 FE and NEO-DOS 0.60.

- **Both ZX Evo machines start with the Kempston mouse**, its wheel and the five-button joystick
  switched on, and the machine now reports itself as Xpeccy+ rather than a 2012 FPGA build.

### Fixed

- **The SD card and the hard disks were lost whenever the machine was switched**, so booting
  from them failed while the settings still named the image.

- **The machines had lost settings the old profiles carried.** Every clone is back to its YM
  sound chip, its Covox, its mouse and Kempston buttons, and the hard disk interface it comes
  with - SMUC on a Scorpion, Nemo on a ZX Evo. ZX Evo has its two-chip TurboSound again.

- **The AY clock and the stereo channel order follow the machine again.** They had become one
  setting shared by every machine, so whichever was up last decided the clock for all of them -
  and a 128K runs its AY faster than a clone does.

- **ZX Evo's text mode was drawn in the wrong place** and showed nothing at all. The 320x200
  modes of the ATM family were off center on every machine that has them.

- **Applying settings wiped the text mode font.**

- **ZX Evo:** a number of hardware details the service ROM and NedoOS depend on - reading the
  palette, the font and the virtual drive mask back, entering and leaving TR-DOS, and page write
  protection.

- **An NMI** no longer leaves the Z80 stuck in HALT.

## 2026.4 - 2026-09-11

### Added

- **Opening a file can switch to the machine that runs it** - a `.spg` to the TSConf, a 128K
  snapshot off the 48K, a TR-DOS disk to a Pentagon, a `.dsk` to the +3. Options -> Media ->
  File types sets it per format: switch when needed, ask, keep the machine, or always use one. A
  tape or a disk that is only mounted leaves the machine alone, and any change of machine says
  so at the bottom of the window.

- **The window title names the tape, disk or snapshot in use** - a tape once it plays, a disk
  once the machine reads it. That image goes straight into Favorites from the menu or a hotkey,
  under its own name or one you type.

- **Run ahead, experimental**: the emulator works a frame or two ahead of the screen, so a key
  press lands about 20 ms sooner. Options -> Xpeccy+ -> Emulation, off by default. It doubles
  the emulation work, puts the picture slightly ahead of the sound, and stands aside while a
  tape, a disk or an RZX recording is running.

- **A log, for when a screenshot is not enough.** `--log`, or Options -> Xpeccy+ -> General, and
  every event lands in `logs/<date>/` beside the emulator, carrying the frame and T-state it
  happened on and the part of the emulator it came from. `--log-groups video:debug,sound:off`
  tunes those parts one by one.

- **Sound follows the sound card's own sample rate** instead of asking for a fixed one, which on
  Windows could be heard as a faint whistle in quiet moments. Rate is Auto by default; 44100 and
  48000 can still be picked by hand, and the old 11025 and 22050 are gone.

- **Sound latency looks after itself**: 30 ms to start with, up when the buffer runs thin, back
  down after a long clean spell. The slider still sets it by hand, anywhere from 10 to 150 ms -
  bluetooth headphones want a good deal more than wired ones. Untick Auto adjust to keep it
  where you put it.

- **The virtual keyboard**, on the menu and Alt+K, can be resized, and can sit under the
  emulator window at its width. Right-click an empty spot on it for those choices.

- **Reload snapshot and labels** works outside the debugger too, with a hotkey of its own - pick
  one in Options -> Xpeccy+ -> Keys.

### Changed

- **Profiles are gone: a machine is a machine again.** The emulator knows what a 48K, a 128K, a
  Pentagon or a Scorpion is, and what you change on one is kept for that one - so an update can
  fix a machine without touching your settings. Your old profiles come across on first start.
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
  - The names are the hardware's. The +2 is a machine of its own; the two Spectrums with a disk
    interface bolted on are not shipped any more - fit one yourself and keep it.
  - The 128K runs on a core of its own instead of the Pentagon's, and the screen layouts are
    named after the ULA they belong to: `ULA.48`, `ULA.128`, `ULA.Pentagon`.

- **Xpeccy+ is a ZX Spectrum emulator only now.** MSX, Game Boy, NES, Commodore 64, BK0010, IBM
  PC, PC-9801, Specialist and ALF are gone, and so is everything only they used - in the
  machine, CPU, disk and hard disk lists, and in the debugger.

- **The border is a choice of fixed sizes** - none (256x192), tiny (272x208), small (288x224),
  medium (320x240), full (352x288) and overscan - instead of a percentage. It starts on full,
  the whole PAL frame a TV of the day showed. Every machine shows the same size picture with the
  screen exactly in the middle, so switching machines no longer moves it around; overscan is the
  one that varies, showing every dot the machine puts out. Changing it now takes effect on
  Apply, window and all.

- **In a window the picture is always drawn at a whole number of pixels per dot** - some border
  sizes used to come out a pixel wider in a few columns at x3 and x5. Fullscreen fits the
  picture to the screen and fills the sides of a wide one with the machine's own border instead
  of black bars. The shader is skipped at size x1, where there is nothing for it to work on.

- **The tape player's block list says what is on the tape**: the name in bold, the size in
  bytes, and what a header announces - `PROGRAM LINE 10`, `CODE 32768,2786`. TZX images label
  their own blocks, so a game's levels are named too. The same list is the tape map in Options
  -> Media -> Tape, where the stop mark of a block takes a click again.

- **Gamepad buttons can be bound by name** - A, B, d-pad up, left stick - instead of by number,
  so one map fits pads of different makes. Maps written the old way still work, and a
  `gamecontrollerdb.txt` dropped in the config folder is picked up.

- **Options was rebuilt around the machine.** The Video page keeps what the picture is made of
  apart from how the window shows it; the interface style moved to Xpeccy+ -> General, the old
  Tools page is that General page now, Low latency and the indicators have a page of their own
  (Emulation), Storage is called Media, and most settings say what they do when the mouse rests
  on them.

- **Bookmarks are now Favorites**, and the list has a window of its own - Manage... at the
  bottom of the Favorites menu - instead of taking up room in Options.

- **Options -> Media -> Disk:** the interleave list shows the order the sectors go in on a
  track, TR-DOS's own (1, 9, 2, 10...) by default. Without fast disk access the drive head steps
  and settles as slowly as a real one.

- **Open... in the right-click menu** goes straight to the file dialog, for any kind of image,
  the way F3 does, instead of asking first whether it is a snapshot, a tape or a disk. The file
  dialogs no longer cut long file names short.

### Fixed

- **Disks with loaders of their own work again.** CHORDOUT no longer hangs when it goes back to
  the disk, Battle Command finishes loading without stuttering on the way, and the Profi's
  service ROM gets past its disk check.

- **The Profi's keyboard works again** - the cursor keys and the other PC keys no longer stick.
  With Grab keyboard on it takes the layout of the Profi's own PC keyboard, which is what the
  keyboard test in its service menu expects.

- **The sound no longer clicks.** On the machines it happened to, part of the sound was never
  made at all, so no latency setting could help - whether a machine was hit came down to how two
  threads happened to be scheduled, which is why it plagued some people and never showed up for
  others. The buffer also keeps a reserve now and holds it against the drift between the sound
  card's clock and the computer's.

- **A machine that turns its own turbo on** - Scorpion, ATM, ZX Evolution, or a `.spg` that asks
  for one - no longer leaves it on for the machine you switch to next. A `.spg` that asks for 14
  MHz now gets 14 MHz, not 10.5.

- **`.z80` snapshots from a +2, +2A or +3** load now, and a 48K `.z80` no longer drops to the
  128 menu on a Pentagon or any other 128K machine.

- **A tape stops at its end** when "Rewind at end" is off. It rewound whatever the setting said,
  so a tape the loader could make nothing of started itself over and over.

- **A gamepad keeps its setting** while it is unplugged or asleep, two pads of the same model no
  longer swap places, and a press reaches the machine right away instead of sitting for up to 40
  ms.

- **Picking a palette preset** changes the colors right away again on the 48K and 128K, where
  they only changed after a reset. The palette editor had the same problem.

- **Recording to WAV** writes the file at the rate and depth the emulator is really playing at -
  it always claimed 44100 Hz, 8 bit - and the grainy beat that used to ride on the recording is
  gone.

- **The emulator no longer crashes on some Intel graphics** a moment after the options dialog is
  closed.

- **Opening a tape or a disk** starts it whatever keyboard layout is picked.

- **Cancel in Options** really cancels a ROM picked on the Machine page, and Apply leaves a
  whole picture on screen instead of half of one frame over half of another.

- **Holding a key down on the virtual keyboard** with the right mouse button works again.

- **Smaller interface fixes**: sliders keep their tick marks under every interface style, the
  Video and Emulation pages line up in columns, the ROM/RAM boxes in the debugger's memory map
  are no longer stretched wide when it opens, and the smaller windows Options opens close with
  the same red cross as the dialog itself.

## 2026.3.2 - 2026-09-03

### Fixed

- **TSConf:** a `.spg` opened on a machine that has not booted yet is no longer a black screen.
  The palette now starts out as the standard 16 colors, the way a loader leaves it.

- **TSConf:** an effect that sets the border or a tile offset once per line no longer leaves a
  20-pixel strip of the previous line's color down the left edge. The line interrupt now arrives
  with the blanking, the way the hardware sends it.

- **TSConf:** the border starts out black instead of white, which is where the machine's own
  register leaves it. What color it ends up is the ROM's business - TR-DOS sets black, 128 BASIC
  white - and a snapshot loaded without either now looks the same as on hardware.

- **TSConf:** switching the line or DMA interrupt off now also drops one that is already
  waiting. The demo *cpir* fell apart and started over about twenty seconds in because of it.

- **The window now comes up before the machine starts running**, with a picture in it from the
  first moment, and takes about a third less time to get there. Most visible on macOS, where a
  file opened from Finder could be heard playing before anything was seen.

- **macOS:** Finder knows which files Xpeccy+ opens, so they are offered in "Open With".

## 2026.3.1 - 2026-09-02

### Fixed

- **TSConf:** programs that set the TS registers by writing into the FMAPS memory window instead
  of using `out` now run. Reported by Sergei Smirnov.

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
  bytes; a single RAM/ROM page is drawn byte by byte instead. A cell takes the color of
  whatever the CPU did there most - read, written or executed - and gray when nothing did. All
  three counters can be shown at once or one at a time, and the legend under the picture doubles
  as the readout for the cell under the cursor. A double click jumps the disassembler there.
  Collecting, resetting and exporting moved here from the disassembler's options menu.

- **A folder on the host can stand in for an SD card or a hard disk image.** Pick a folder
  instead of an image file in Setup - Storage (or pass one to `--sdcard`) and it shows up in the
  emulator as a read-only FAT32 card or disk: drop files in from the desktop and they are there,
  no image to build or resize. Long and Russian names are kept.

- **`--sdcard FILE`** selects an SD-card image at startup. (nihirash)

- **A Nix flake**, for building and running on NixOS/Nix-based systems. (nihirash)

- **On Windows 11, window titlebars** now follow the chosen interface style instead of always
  looking like the plain system one.

- **The disassembler's jump-target arrow** now also shows up for unconditional `jp`/`call` and
  for `ret`, not just conditional jumps and calls.

### Changed

- **The Storage options and the tape player say what they do.** No more "Turbo" and "Autoplay":
  the checkboxes have real names, every icon button has hover text, and the speed slider is on
  both pages, reading out the per cent it is set to.

- **A tape played to its end starts over on Play** instead of doing nothing. "Rewind at end" on
  the Tape page turns that off.

- **The memory map panel is gone - its controls are in the MEMMAP block** of the side panel.
  Each of the four 16K banks is a ROM/RAM box and a page number to type in, and a bank set by
  hand stays highlighted until the machine pages over it. Restore is on the right-click menu of
  the block. Machines that do not page in 16K blocks keep the read-only list they had.

- **A register that changed lights only the byte that changed**, not the whole field - so `IR`
  shows that `I` moved even though `R` moves on nearly every instruction. `PC` and `SP` stay
  whole, nothing touches half of an address. "Split pairs" in the menu on the CPU panel's header
  turns it back into the whole-field highlight.

- **The stack panel fills its whole height** instead of always showing nine entries, and the
  offset it starts from is set in Setup - Xpeccy+ - Debugger. The entries are centered now, and
  the row at SP itself is highlighted and shows its own address instead of an offset.

- **The Leds page of Setup is now Indicators**, a row per indicator: the icon it draws on
  screen, its name and a line saying what it shows.

- **The bundled profiles were gone over machine by machine** and now match the real hardware
  more closely. Profiles you already have are left alone; update them by hand to pick this up.

- **The debugger headers that react to a mouse click** - CPU, MEMMAP, PORTS and FRAME - are
  marked with a dot in the corner.

### Fixed

- **Files dropped on the window open again when their path has spaces in it**, and a drop is
  never taken as a move, which could have the file manager delete the original. Dropping several
  files at once opens the first instead of loading them over each other, and a dropped `.rzx`
  now starts playing.

- **The checkboxes in the breakpoint list** follow the chosen interface style, sit centered
  under their headings, and no longer leave a stray outline on the empty half of a cell. Flags a
  breakpoint has no use for - the F/R/W of an IRQ or of a global condition - are left blank
  instead of quietly taking clicks.

- **Memory contention is accurate on the 48K, 128K, +2 and +2A/+3.** The ULA now also holds the
  CPU during an instruction's idle ticks and during `IN`, and the screen is read ahead of the
  beam the way the real one does. `LDIR` across the screen used to run about a fifth too fast;
  timing tests, beam-racing demos and multicolor now match a real machine.

- **The +2A/+3 raster and interrupt position** match the documented figures, and those machines
  no longer contend port access, which their ASIC does not do.

- **Pentagon, 128K, +2A/+3 and Scorpion** now hold the frame interrupt for the 36 T-states the
  real machines do, instead of the 48K's 32.

- **Scorpion ZS 256** draws its borders and retrace at the documented widths, and takes the
  frame interrupt where the real machine does.

- **The disassembler no longer loses its selected row's highlight** and syntax colors when the
  debugger loses focus.

- **Copying or saving the disassembly** now keeps the blank line between branches, matching the
  on-screen listing.

- **Half-register opcodes** now disassemble as `IXH`/`IXL`/`IYH`/`IYL` instead of
  `HX`/`LX`/`HY`/`LY` (the old names still work when typing code in).

- **Jumping to an address (F4) or returning (F5)** in the disassembler now puts the cursor in
  the right place instead of leaving it off by a row or back at the top. The blank lines between
  blocks count as rows too, so the jump and Page Up no longer step over lines.

- **Screenshots** no longer have transparent pixels, and ZX screenshots with the border kept are
  now centered correctly.

- **On macOS, the hotkey editor** now shows the correct modifier for the key you press (Ctrl no
  longer gets swapped with Cmd), and Options opens with the native Cmd+, by default.

- **On macOS, Cmd+Q** now quits the app (it used to do nothing). Alt+F4 already quit on Windows
  and Linux.

- **The debugger window** no longer takes noticeably longer to open the first time.

- **The icons on the debugger's panel tabs** are centered again when an interface style is in
  use, instead of sitting left of the middle.

- **Emulation timing** is more precise: it used to run every machine up to ~1% faster than real
  hardware, affecting audio pitch and frame rate.

- **The FPS indicator** now shows a more accurate frame rate, including right after unpausing.

- **Smaller interface fixes**: the toolbar icons read on dark styles, the debugger's context
  menus use the interface font, the breakpoint list keeps its column widths and no longer wastes
  room on the address column, the stack panel comes back its own size after a restart, and a
  constant in the listing goes bold instead of changing color under the cursor.

### From upstream

Taken from [Xpeccy](https://github.com/samstyle/Xpeccy) build `20260824`, by SAM style.

- **A new hardware profile, `PentEvo21`** ("Evo Baseconf, after 2021"), alongside the existing
  PentEvo. Marked unstable by upstream.

- **Jumping to an address in the disassembler (F4) or returning (F5)** no longer leaves a
  duplicate entry in the jump history.

## 2026.2 - 2026-08-21

### Added

- **The side panel of the debugger shows the frame number**, under SIGNALS: frames since reset,
  the same number breakpoint conditions read as `FRAME`. A right click on it puts the counter
  back to 0.

- **The ports in that panel are a list of your own now.** The ones a machine keeps by itself are
  in it from the start, any port on the bus can join them (up to 16 per profile), and each one
  can be switched off without leaving the list. The cell shows the last value that went through
  the port, either way. A port of four digits is the address as it is (`7FFD`, `BFFE` for one
  keyboard half-row); two digits are a byte port and only the low byte counts, which is what
  catches `in a,(31)` on the Kempston joystick or `out (#FE),a` on the border - the Z80 puts A
  in the high byte there. Edited in Options - Debugger, or by a right click on the PORTS block.

- **Blocks of that panel can be turned off** - ports, signals, frame, beam position - in
  Options - Debugger. That page is laid out anew: the style picker and the font moved into
  View, the panel switches and the port list sit under it, and the palette takes the whole
  height beside them.

- **Conditional breakpoints** (the way Unreal's debugger does it): a breakpoint can carry a
  C-like condition and only stops when it is true - `bc == 0x1234`, `(out & 0xff) == 0xfd`. A
  condition with no address of its own is a breakpoint in itself: it is checked after every
  instruction and stops while it is true, or only when it becomes true if "On change" is ticked.
  Expressions take CPU registers, labels, memory (`M(x)`, `[x]`), the last memory/IO access
  (`RD`, `WR`, `MDT`, `IN`, `OUT`, `VAL`), the machine state (`DOS`, `SLOT0`..`SLOT3`, `FRAME`,
  the beam position `RAYX`/`RAYY`, `RAY(x, y)` for the instruction the beam passed a given dot
  in) and the breakpoint's own hit counter (`HITS`), so an address breakpoint with `HITS > 30`
  lets the first 30 hits pass and stops on the 31st, and `FRAME == 300` stops on the first
  instruction of frame 300. Numbers follow the same rules as the assembler in the disassembler
  window - decimal, `0x`/`#` for hex, a leading zero for octal - and while a condition is typed,
  the line under the field shows how it was understood, with the priorities as brackets. The `?`
  button in the breakpoint editor lists them all with examples. Conditions are saved and loaded
  with the breakpoint list, and the list can be loaded at startup with `--brk FILE`, which also
  reads Unreal's `bpx.ini` format (`x0=0x80A6`, `r0=0x8000-0x8FFF`) - the one sjasmplus writes.

- **The disassembly listing reads more like a listing** (ideas borrowed from Spectaculator): the
  address and the opcode columns can be dimmed, constants are colored and labels go bold, and
  an empty line follows every `RET`/`JP`/`JR`. Four switches in the debugger Options menu, kept
  between runs; the constant color is `Const` in the palette editor and every bundled style
  brings its own.

- **Address jumps in the debugger from the keyboard** (the way Unreal's debugger does it): `G`
  puts the cursor on the address of the first row - in the disassembler and in the memory dump -
  ready for a new address. In the dump `Ctrl+P`, `Ctrl+S`, `Ctrl+B`, `Ctrl+D`, `Ctrl+H`,
  `Ctrl+X` and `Ctrl+Y` jump to what `PC`, `SP`, `BC`, `DE`, `HL`, `IX` and `IY` point at, the
  same as a right click on the register name. All of them are rebindable; `Ctrl+S` in the dump
  no longer opens "Save dump".

- **Autoload for tapes and disks given on the command line** (the way a snapshot always did).
  The loading method is the standard one for the machine and is picked automatically. To only
  mount the file, use `--no-autostart`.

### Changed

- **Watcher expressions** understand the same syntax as breakpoint conditions - comparisons,
  logic, shifts, `M(x)` and the last memory/IO access. Single Z80 registers (`b`, `c`, `h'`,
  `ixl` and the rest) can be used as well. **Two things changed meaning**: a number without a
  prefix is decimal now, where it used to be read in the machine's base, so hex needs `0x` or
  `#` (`hl == 4000` is four thousand, `hl == 0x4000` is the screen) and a name is never a
  number, which is what makes `bc` unambiguously the register pair; and operator priorities are
  C's now, so mixed bitwise and arithmetic can shift - `hl&0xff+1` used to mean `(hl&0xff)+1`
  and now means `hl&(0xff+1)`. Labels, `.name`, `[x]` and `0x` work as before, and nothing on
  disk holds expressions, so saved xmap/label files are not affected.

- **One set of AY/TurboSound settings instead of three.** Sound now asks how many chips there
  are - none, one, TurboSound (NedoPC) or TurboSound (ZX Next) - and the type, clock and stereo
  mode apply to all of them. No machine ever mixed different chips, clocks or stereo layouts in
  one TurboSound, so there is nothing left to set per chip. In the profile they are five `psg.*`
  keys instead of ten; a profile written by an older build is read as before and converted the
  first time it is saved.

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

- **A breakpoint loaded from a file** could come up with random flags - most visibly a stray
  "temporary" one - because the loader left part of the record uninitialized.

- **The context menu of a changed register is readable again.** It took the pink highlight of
  the field as its own background.

- **The register panel comes back the way it was left.** It was rebuilt in one column on every
  start, whatever the saved layout said, and pushed the panels under it down.

- **Two more visual artefacts in ZX Evo (TSConf) demos**: a seam running along the horizon, and
  single dots of a wrong color drifting about the picture. Both show in the greetings part of
  *Synchronization*.

- **Smaller interface fixes**: the `Sublime` style takes its chrome colors from Monokai Classic
  and keeps constants readable on the PC row, the hotkey list no longer cuts long names short,
  and a picked AY frequency is readable again in the sound settings.

### From upstream

Taken from [Xpeccy](https://github.com/samstyle/Xpeccy) builds `20260814` to `20260816`, by
SAM style.

- **A palette panel in the debugger**: all 256 colors at once, with the index and the RGB of
  the one you click.

## 2026.1 - 2026-08-14

Entries marked **(Volutar)** are the work of [Volutar](https://github.com/Volutar), taken from
[his fork](https://github.com/Volutar/Xpeccy) with his authorship kept on every commit.

### Added

- **Eight interface styles**, in `config/styles` - pick one in Setup - Xpeccy+ - Debugger -
  Style Sheet. `Light`, `Dark`, `ZX Spectrum`, and five from the editors people already use:
  `Sublime` (Monokai), `Gruvbox` (Pavel Pertsev), `Solarized Dark` and `Solarized Light` (Ethan
  Schoonover), `Dracula` (Zeno Rocha and contributors) - their palettes, under the same MIT
  license. Each style also brings the debugger colors a style sheet cannot reach, in a `.pal`
  file next to it; they stay editable in Setup - Xpeccy+ - Debugger - Palette.

- **DejaVu Sans Mono ships with the emulator** (Volutar), so the debugger lines up the same way
  everywhere - macOS has no copy of it at all. It keeps its own license, see `LICENSE_DEJAVU`.

- **Kempston joystick (port `#1F`) on ATM Turbo 2+, ZXM-Phoenix, ZX Spectrum +2 and +3.**
  (Volutar)

- **Disk images with more than 80 tracks**, in both `.trd` and `.scl`. (Volutar)

- **macOS builds again, and stays that way** - every change is built, started and packed into a
  DMG on an Apple silicon runner.

- **The debugger's panels can be arranged freely** (experimental). Registers, disassembler,
  memory map and stack are dock panels now, so any of them can be dragged, split, tabbed or put
  side by side, and the arrangement is remembered. Registers switch to two columns when the
  panel has room for them. `Reset panel layout` in the disassembler options puts everything
  back.

- **Bytes per row in the dumps** - `Auto`, `8`, `12` or `16`, next to the code page. `Auto`
  fills the width a group at a time. The track dump has it too, where a line was always eight
  bytes long.

### Changed

- **ZX Evo (TSConf) has a screen geometry of its own** instead of borrowing Pentagon's. Its
  border and blanking are where the real machine has them, which is what raster effects and the
  line interrupt are timed against.

- **The debugger opens at 960x720**, and its font and screen zoom start smaller, so the window
  fits a 1280x800 display.

- **Labels stand out in the disassembler.** They share the disk ID colors, so a style sets them
  and Setup - Xpeccy+ - Debugger - Palette retunes them.

- **The dumps read in groups of four.** Cells no longer stretch with the panel: they keep a
  fixed width, line up from the left and leave a gap every four bytes, the same in the memory,
  track and register dumps. A register dump row now says which pointer it follows, `HL (1234):`.

- **Small changes to the defaults**, mostly to get around conflicts found on macOS:
  - profiles no longer pick a keyboard layout. The one they carried was laid out for 48K
    machines only, so a 128K one was added next to it. Details in `config/keymaps/README.md`
  - border size is now 100% at scale 2 - not every configuration was shown correctly

- **More fits on the debugger's screen** (Volutar): tighter margins, spacing and row heights
  across 23 panels, nine stack entries where there were six. No panel moved.

- **The on-screen keyboard is drawn the classic skewed way**, with a key map to match. (Volutar)

- **Less separated AY stereo** (Volutar) - three sixteenths of each side channel bleed into the
  other. Volume unchanged, mono sounds exactly as before.

- **Windows: `Shift+Alt+Space` sets a read breakpoint** on the cell under the cursor (Volutar).
  Plain `Alt+Space` belongs to the system menu there, so that breakpoint had no key at all.
  Other platforms are unchanged.

### Fixed

- **An update brought no new files to an existing installation** on macOS and Linux. The shipped
  configuration was copied only into an empty config directory, so styles, ROMs or palettes
  added by a later version showed up only after deleting the old one. Whatever is missing is
  filled in on every start now; files already there, edited or not, are left alone. Windows runs
  from that directory and was never affected.

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

- **The emulator crashed on its way out when the window had never been shown**, which is exactly
  what `--help` does.

- **The first frame bound a texture that had never been generated.**

- **ZXM-Phoenix paged the wrong memory** (Volutar): bits 4, 6 and 7 of port `#1FFD` were masked
  and shifted as one.

- **Profi did not initialize port `#DFFD` on reset.** (Volutar)

- **The debugger's disk dump stopped at 83 tracks**, a hardcoded number derived from nothing. It
  reads the drive's own geometry now.

- **Long profile names were cut off** in Setup - Xpeccy+ - Profiles, and the mark on the current
  profile pushed its name sideways.

### From upstream

Taken from [Xpeccy](https://github.com/samstyle/Xpeccy) builds `20260809` to `20260811b`, by
SAM style. The base was build `20260807`.

- **ZX Evo (TSConf) interrupts:** its own handler for them, the line and DMA sources work, and a
  heavy screen no longer freezes the machine. *Synchronization* used to stop three and a half
  minutes in. It supersedes the fix we made in 2026.0.

- **ZX Evo (TSConf) reads the PC keyboard** as well as the ZX matrix, the way PentEvo already
  did.

- **PentEvo:** virtual DOS and NMI memory banks, and write protection per memory page.

- **YM2203:** an envelope rate of 0 now holds forever instead of creeping, and the fastest
  attack rates reach full volume at once. The rates are worked out when a register is written
  rather than on every tick.

- **The build works again with `-DUSEOPENGL=0`.**

## 2026.0 - 2026-08-08

### Added

- **Much lower input lag** - about 20 ms where it used to be about 60 ms, which is the tier the
  other ZX emulators are in. Emulation used to run in bursts the size of the audio buffer and
  the display used to show the oldest of three queued frames; it is paced by a wall-clock timer
  now and shows the newest frame. The new "Low latency" switch in Options - Video is on by
  default; turn it off if the motion looks uneven on a plain 60 Hz display.

- **Memory heat map**: per-cell read, write and exec counters for RAM and ROM, shown in the
  debugger and exported as CSV (also written next to a saved `.sna`). `tools/heatmap_png.py`
  renders the CSV as a PNG.

- **AF and AF' are back in the debugger.** The accumulator is shown as a register pair, the way
  ZX debuggers have always shown it, instead of `A` alone with the flags only as checkboxes. `A`
  and `A'` still resolve by name in watcher expressions and breakpoint conditions. Widening the
  register panel puts each register next to its alternative in a second column.

- **A release runs right after unpacking.** `config/` carries 29 ROM images covering every
  ZX-compatible machine the emulator supports, with a romset and a ready profile for each -
  video geometry, contention, sound chips and the disk interface the machine shipped with - plus
  20 CRT shaders, nine palettes and a default keyboard and gamepad mapping. Everything is named
  after the machine it belongs to: profile `ZX Spectrum +3`, romset `ZX Spectrum +2A/+3 (v4.0)`,
  geometry `ZX +2A/+3`. The defaults are a Spectrum's rather than the code's: an AY-3-8912 at
  1.7734 MHz in ACB stereo on the Sinclair line, a YM2149 at 1.75 MHz in ABC with SounDrive on
  the clones, the real 3.5469 MHz on the 128K family, a 3x picture with a 65% border and a CRT
  filter, Kempston mouse on everywhere. The ROM images and the shaders keep their own licenses,
  separate from the MIT license of the project - see `config/roms/LICENSE`,
  `config/roms/PROVENANCE.md` and `config/shaders/README.md`.

- **Ready-to-run builds for both platforms.** On Windows `packaging\make-dist.ps1` stages the
  binary with the Qt and SDL runtime, `config\` and the docs, and can zip it; presets cover Qt 5
  on x86 and x64 and Qt 6 on x64. On Linux `packaging/make-appimage.sh` packs an AppImage, so
  one file covers Ubuntu, Debian, Fedora, Arch and SteamOS. Both use CMake's own install layout,
  which a `.deb` or `.rpm` built with CPack carries too, and `config/` is part of it - a Linux
  package now starts with the same 16 machine profiles a Windows release has instead of the
  single built-in 48K ROM. Each build also carries the metadata its platform expects and neither
  had before: product name, version, description and copyright on the Windows binary,
  description, license and documentation in a Linux package.

- **A boot loader for TR-DOS images.** "Add boot" is on by default and now has something to add:
  a `.trd` or `.scl` with no `boot` file of its own gets `config/boot.$B` appended when it is
  opened, so the disk can be started without typing a command. Dimon boot 2024 by Dmitry
  Yurinov - see `config/roms/PROVENANCE.md`.

- **Version scheme**: the tracked `VERSION` file is the only source, and the window title
  carries the whole thing - `Xpeccy+ (2026.0-dev+20260807)` for a development build, `Xpeccy+
  (2026.0)` for a release.

### Changed

- **Renamed to Xpeccy+.** The binary, the desktop entry and the package are `xpeccy-plus`;
  window titles and the exported file headers carry the new name. The configuration files
  themselves are deliberately untouched, so existing settings keep working.

- **Own configuration directory on Linux and macOS**: `~/.config/xpeccy-plus` instead of
  upstream's `~/.config/samstyle/xpeccy`, which the fork still used - installed side by side,
  the two shared one `config.conf`, one set of profiles and one ROM directory, and overwrote
  each other's settings. `$XDG_CONFIG_HOME` is honoured now where it used to be ignored. Nothing
  is migrated: the first run starts from the defaults, and the old directory is left alone -
  point `--confdir` at it to keep using the old settings. Windows is unaffected, its
  configuration has always lived next to the binary.

- **"Preset" in the romset editor** now fills in the names of the bundled images instead of
  names from the author's own machine.

- **The sources mirror the runtime layout**: `conf/` became `config/`, the same shape the
  emulator expects next to the binary. The built-in emergency defaults, used when no
  configuration is found at all, moved to `res/fallback/`.

### Fixed

- **Tape did not start for loaders that bypass the ROM routine**, so they had to be started by
  hand.

- **Register fields in the debugger sized themselves wrong**: one fixed width for every machine,
  too wide for a byte register and too narrow for a 32-bit one, and a 32-bit field (`PSW` on
  BK0010) took only a single digit of input. They follow the value and the number base now.

- **ZX Evo (TSConf) drew nothing but black.** The window into the FPGA at the address in `#15AF`
  was still tested as `flag & 0x10` after that flag became a `bool`, so it never opened: nothing
  could reach the palette or the sprite file. Loading screens, sprites and backgrounds were all
  missing. An upstream regression from build `20260418b`.

- **TSConf stalled wherever software drove the line interrupt.** The CPU only accepted an
  interrupt while a frame interrupt was being held, so the line and DMA sources were taken
  roughly once a frame instead of once a scanline, and demos slowed to a stop.

- **TSConf did not get through its boot ROM.** `#21AF` bit 0 is the same ROM select as `#7FFD`
  bit 4, but only the latter was recorded, so the wrong ROM was paged in and the `#3Dxx` entry
  into TR-DOS never fired - the boot ROM looped forever with the stack pointing into ROM.

- **TurboSound had its two chips the wrong way round**: `#FFFD` `#FF` selects the first AY and
  `#FE` the second.

- **The AY could not be detected through port `#FFFD` any more.** Upstream's TSFM work made
  "read the status register" the state every machine powers up in, so a program that probes the
  chip by writing a register and reading it back got a status byte instead: a plain 128K
  reported no sound chip at all. Reading registers is the power-up state again, and the status
  register is only readable where the hardware has one.

- **Crash when switching profiles.** A profile's machine is created on first use, but the
  emulation thread could pick up the profile before that machine existed, and use it while it
  was still being built. Present upstream as well.

- **`--confdir` set up the configuration twice**, which leaked the gamepad controller, added a
  second `default` layout and profile, and left the windows holding values from the first
  configuration. The option is read before anything is initialized now.

- **Windows with no icon of their own** - options, tape, RZX, watcher - had none at all on X11,
  where there is no exe resource to fall back to. The application now carries an icon, which Qt
  hands to every window that does not set one. The Linux desktop entry also points at a path the
  icon theme spec knows, `share/icons/hicolor/128x128/apps`, instead of a flat `share/icons`.

- **The x86 trace log wrote one garbage character in place of the `CS` register**, since the
  value was appended as a character rather than as a hex word. It also broke the Qt 6 build.

- **Link error in MinSizeRel and Debug builds** (`lr_swaph` declared C99 `inline` with no
  external definition).

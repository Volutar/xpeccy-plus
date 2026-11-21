# Tape loading by itself

```{index} Auto play / stop; how it works, Fast loading; how it works, Flash loading; how it works, Edge detection; how it works, loader
```

A real tape deck knows nothing about the program: someone presses Play, waits, and presses
Stop. Xpeccy+ can do that by itself, and load in seconds instead of minutes. For that it has to
tell, from what the machine is doing, when a loader is reading the tape. This page says how it
tells, and what each switch does with it. The switches are in the
[Tape Player](tapes.md#playing) and on the [Machine: Tape](options-machine.md#machine-tape)
sheet; all of them are on by default.

## How a loader is recognized

A program reads the tape through port `#FE`: one bit of it is the level of the signal from the
tape. The same port also gives the keyboard, so a read alone says little. What follows the read
says more, so Xpeccy+ looks at the code right after it:

- code that tests the tape's bit is a **loader**;
- code that tests only the key bits is a **keyboard scan**;
- a read by the ROM's own loading routine belongs to [flash loading](#flash-loading).

A loader waiting for the next pulse also reads in a tight loop: the same instruction again and
again, with exactly one register moving - its counter, which counts by one when it is B. Code
that plays a sound or draws from the port stores what it reads, and more registers move; a loop
where nothing moves, or only the register the read went into, waits for a key.

## Auto play / stop

```{index} Auto play / stop; start, Auto play / stop; stop
```

**Starting.** A stopped tape starts after ten loader reads in a row, each soon after the one
before. A keyboard scan never starts it, nor any read from the ROM: a disk system that checks
the BREAK key through the ROM must not start the tape. A tape started afresh gives the loader
half a second of silence first, as a deck does.

**Stopping.** The tape stops at its end and at a stop mark in a TZX image. It also stops when,
for about a frame, every read is unlike a loader's: the program has finished loading and is
scanning the keys. One odd read is not enough - a key scan in the middle of a load, from an
interrupt, would otherwise cut the tape mid-pulse. A loader busy between blocks - unpacking,
decrypting - reads nothing at all, and that does not stop the tape either.

A key scan during a pause between blocks does not stop the tape while there is more to play:
a deck plays the whole pause, and a loader may be timing it.

**Pause, not stop.** When auto play stops the tape and starts it again with nothing in between,
it goes on from the same point of the same pulse, like a deck's pause button. Started fresh, it
would cut the signal into pieces each time.

**Stop by hand wins.** After **Stop** is pressed, nothing moves the tape by itself until
**Play**, a rewind, a recording or another tape.

## Flash loading

```{index} Flash loading; ROM routine
```

Most tapes load at least their first blocks through the ROM's loading routine. With **Flash
loading**, a block that this routine asks for goes straight into memory, and the processor
leaves the routine exactly as the ROM would have: the same registers and flags, a wrong flag
byte skipped, a short block failing as on a real tape. Programs often copy the routine into RAM
and call the copy; such a copy is recognized by its bytes, and loads the same way.

Only that routine is caught. A loader of a program's own is never decoded, it is played - so
anything that loads at all still loads, if not in a flash. Two cases are played on purpose:

- a block longer than the routine was asked for: some loaders have the ROM read only the
  start of a block and read the rest themselves;
- the pause after a block, if it is under 0.2 seconds: a loader that goes on reading needs the
  tone that follows.

After a longer pause, the next block that is not a standard one waits until the program's own
loader first reads the tape, then plays. A loader that shows a picture or a prompt first would
otherwise find its pilot tone half gone.

A SAVE through the ROM writes its block straight onto the tape the same way.

## Fast loading

```{index} Fast loading; going back
```

**Fast loading** does not change how anything loads: while a loader reads the tape, the machine
simply runs as fast as the computer can, without sound. The loader runs instruction by
instruction, as at normal speed, so turbo loaders and protected tapes load this way too.

When the tape stops, the machine is put back to where the loader finished. The moment the
loader stopped reading, the whole machine is saved; if it reads again, the save is dropped.
When fast loading ends, that save is put back. Without it, the game would already have run for a
second or two at full speed - its title music half played, its demo started - by the time it is
shown at normal speed.

Part of fast loading is always on: for the edge loops of the ROM and of the well-known turbo
loaders, the turns of the loop up to the next pulse are counted instead of run, and the whole
machine - picture and all - is moved on by the same time. The result is the same machine,
only sooner.

## Edge detection

```{index} Edge detection; not exact
```

**Edge detection** goes further: a loader waiting for the next pulse gets it at once. Only the
tape is moved on; the rest of the machine - the picture, the interrupts - stays where it was.
The loader's own delay loops between pulses are skipped the same way. Loading gets several
times faster, but it is no longer what the machine would have done: a loader that also plays
music, counts interrupts or watches the picture while loading can see the difference. If a tape
loads with normal speed and fails with **Edge detection**, turn it off.

## Rewind at end

```{index} Rewind at end; how it works, multi-load
```

A game that loads its levels as it goes reads the tape again later. **Rewind at end** acts when
the tape is wanted again, never at the end itself:

- at **Play**: back to the start;
- at a load through the ROM: to the first block with the flag and length that load asks for -
  not simply the start, as a game may react to a block it did not ask for;
- when a program's own loader reads a tape that has run out: back to the start, and it plays.
  Such a loader cannot say which block it wants, so the start is the only safe guess; a game
  that reloads a level after each game over finds it there every time.

A load through the ROM on a stopped tape also winds forward, past blocks that do not fit, to a
block that fits exactly - as winding a deck to side B. A longer block is not skipped: a loader
may read only its start.

When the tape is one of a set (`Game (Side A).tzx`, `Game (Side B).tzx`... - see
[The other sides beside an image](files.md#the-other-sides-beside-an-image)), a loader reading
past its end, or a ROM load asking for a block it does not have, gets the next tape of the set
instead of a rewind, and the picture says which side is in. **Play** still rewinds the tape that
is in. **Next tape of a set** off in Options leaves the set out: the tape rewinds as a lone
one does.

With **Rewind at end** off, the tape stays at its end, as a deck would leave it: a game that
says "turn the tape over" expects that.

# Contention, snow and the floating bus

```{index} contention; how it works, ULA snow; how it works, floating bus; how it works, ULA
```

In a Sinclair Spectrum the processor and the chip that draws the picture - the ULA, or the
gate array on a +2A and +3 - share part of the memory. Three effects come out of that sharing:
the processor slowed down in that memory, rubbish on the screen called snow, and ports that read
back what the ULA is drawing. Programs depend on the first and the third; the second is a
defect that some programs show. This page says what each is and how Xpeccy+ emulates it. Each
can be switched per machine in **Machine: Advanced Settings**, group **ULA** (see
[Advanced settings](options-machine.md#advanced-settings)); every machine starts with the
settings of the real one.

## One memory, two users

The ULA reads the screen from RAM while the beam draws it, and the beam cannot wait. When the
processor wants the same memory at the same time, the ULA wins and the processor's clock is held
until the ULA's read is over. This is **contention**. For every 16 pixels the ULA reads four
bytes in a row - pixels, attribute, pixels, attribute - and then leaves the memory alone for a
while, so how long the processor waits depends on the exact moment it asks: from 6 ticks to
none. Over the border and while the beam returns, nothing waits.

- On a 48K, the 16K at `#4000`-`#7FFF` is shared.
- On a 128K and +2, pages 1, 3, 5 and 7, wherever they are paged in. A port whose address looks
  like shared memory, or whose lowest bit is 0 (the ULA's own port), is held the same way.
- On a +2A and +3, the gate array shares pages 4, 5, 6 and 7, with a pattern of its own, and
  holds no ports.
- The Pentagon, the Scorpion and the other clones share nothing: their processor never waits.

Two batches of the same ULA start all of this one tick apart: **early** and **late** timings. The
128K machines that were tested are early, every +2 tested is late.

This matters to any program that counts ticks: a border effect, a multicolor picture, a sound
played by the processor. Xpeccy+ looks up the wait at the tick each memory or port access
starts, including the accesses the Z80 makes without reading anything, and waits it out.

## Snow

```{index} ULA snow, ...and it crashes
```

Every time the Z80 fetches an instruction, it also puts a refresh address on the bus for the
memory chips: the `I` register and the `R` counter. When `I` points into shared memory - on a
48K at `#40`-`#7F`, on a 128K also at `#C0`-`#FF` with an odd page there - that refresh reaches
the memory the ULA is reading, and spoils its read. Depending on where in the ULA's cycle it
falls:

- the ULA reads a pixel byte and its attribute from the wrong address, the low bits taken from
  `R`: a cell of rubbish - the snow;
- or it misses its second pair of bytes and shows the first pair twice.

It happens mostly when a program puts its interrupt table in shared memory. A +2A, a +3 and the
clones do not snow. Xpeccy+ follows the study and the tests of weiv, filmed on real machines by
PheeL and balford: the snow falls on the same columns, in the same shape, and on a 128K comes
from the same page as on the real machine. A +2 running Mark Woodmass's IR Contention 128 snows
as a real +2 does in its photo.

Some 128K machines hang or reset under snow. How is not known, so **...and it crashes** is a
model of the right shape, not of a known mechanism, and is off on every machine.

### When a game snows

```{index} ULA snow; notice, Turn Off for This Game, Don't show this again
```

Snow is on for the 48K, the 128K and the +2, as on the real machines, so that a program that
snows shows it. When a game snows in ten frames of any two seconds, even lightly, as Dizzy 3
on a 128K does in one frame of four, Xpeccy+ says so: the picture is dimmed, snow falls over
it, a snowflake blinks in the top left corner, and a message tells what it is. The game runs on meanwhile. The message comes once for each image -
a tape, a side of it, the disk in drive A, a snapshot - and again when another one is
opened.

- **Turn Off for This Game** - no snow until another image is opened. Nothing is saved:
  the machine keeps its setting.
- **Continue** (or Esc) - the snow stays. The real machine shows it too: the game's author
  may want to know.

Tick **Don't show this again** before either button to have no more of these messages, for
any game: the snow is shown as before, without a word. **Snow notice** in
**Options > Appearance**, **Indicators**, brings the message back (see
[Indicators on the picture](options-appearance.md#indicators-on-the-picture)).

To be rid of the snow for good, choose a machine that does not snow, or switch
**ULA snow...** off in **Machine: Advanced Settings** (see
[Advanced settings](options-machine.md#advanced-settings)). Fast loading, fast forward,
rewind, an RZX recording or playback and the Debugger do not raise the message.

## The floating bus

```{index} Floating bus; how it works, port #FF
```

When a program reads a port that nothing answers, it reads whatever is on the bus at that
moment. On a Spectrum that is the byte the ULA is fetching for the screen, or `#FF` while it
fetches nothing. A program can use it to know where the beam is - wait for a known attribute,
then draw - and so move sprites without flicker. Sidewize, Short Circuit and Terra Cresta do,
and Arkanoid and Cobra in their first releases.

The **Floating bus** setting has four answers:

- **Ferranti ULA** - 48K, 128K, +2: the screen byte being fetched, else `#FF`. On a 128K and +2,
  even a read of port `#7FFD` is taken by the paging latch, and switches pages.
- **Amstrad gate array** - +2A, +3: the bus was thought absent on these for decades, until
  Ast A. Moore and Hikaru worked out its rules on real machines: only some ports, only with
  paging on, bit 0 always set, and between fetches the last byte the processor moved in shared
  memory.
- **Port #FF (clones)** - the Pentagon, Scorpion and others: no shared bus, but a port `#FF` that
  gives the attribute of the cell being shown, `#FF` over the border. On the Scorpion this is
  taken from its schematic.
- **None** - always `#FF`.

The joystick port with no joystick, the mouse ports with the mouse off and the AY's port with no
AY read the floating bus too, as they do on the machine.

## One model for all three

Contention, snow and the floating bus are the same thing seen three ways: the ULA's reads, tick
by tick. Xpeccy+ counts all three from one position of the ULA in its 16-pixel cycle, so they
cannot drift apart. They were checked against tests whose results on real machines are
published, photographed or filmed: Richard and Tim Butler's timing tests (37 of 37 on the 48K,
and on an early 128K the same five failures a real one prints), Patrik Rak's timing test, Mark
Woodmass's floating bus, paging and contention tests, Hikaru's +2A/+3 floating bus test, and
weiv's snow tests. The ZX Spectrum Next is the exception: its contention and floating bus come
from its own FPGA design, and it does not snow.

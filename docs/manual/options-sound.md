# Sound page

```{index} Sound page
```

![The Sound page](images/options-sound.png)

```{index} sound
```

What you hear comes from the machine's own sound devices - the beeper, the tape, and
whatever sound chips and cards the machine has. Which devices a machine has is set on
the **Machine** page of **Options** (see [Sound devices](options-machine.md#sound-devices)); how the sound is played and how loud each device is
are on the **Sound** page.

```{index} Emulate sound, sound; off
```

**Emulate sound**, over the groups, switches the sound off and on. With it off, the program
plays silence. The machine still runs at its normal speed, and its sound is not worked out
at all, which leaves more of the computer to the emulation. The Debugger's scope and the
sound of a rewind are silent for that time too. The rest of the page is greyed while it is
off. This is not the same as the **NULL** driver below, which works the sound out and only
does not play it.

```{index} Driver, Latency, Auto adjust, sound; latency
```

Under it are four groups.

**Output** - how the sound reaches your speakers:

- **Driver** - **SDL** plays the sound; **NULL** plays nothing.
- **Rate** - the sample rate: **Auto** uses the rate of your computer's sound device
  and shows that rate; **48000** and **44100** set a fixed rate.
- **Latency** - how much sound is kept ready ahead of playback, from 10 to 150 ms. A
  lower value makes the sound follow the picture and the keys more closely; a value too
  low makes the sound click. With **Auto adjust** ticked (the default) the program finds
  the value by itself: it raises the latency when the sound runs short and lowers it
  again after a long time with no gaps. The slider then shows the value the program has
  settled on.

```{index} Stereo, Filters, Board filter, PSG stereo, Separation, PSG; stereo, By machine
```

**Stereo** and **Filters**, side by side, are how every machine is heard. This is a matter
of taste, as a shader is, so it is set once here for all of them and stays when the machine
changes. A machine can have its own **PSG stereo**, **Separation** and **Board filter** over
these (see [A machine's own](#a-machines-own) below).

**Stereo**:

- **PSG stereo** - the order of the sound chip's three voices from left to right: **Mono**,
  **ABC**, **ACB**, **BAC**, **BCA**, **CAB**, **CBA**. **ABC** puts the second voice in the
  middle; **ACB** puts the third voice there. **By machine**, the default, keeps each
  board's own - mono on the 128K, **ABC** on the Pentagon, **BAC** on the Scorpion - shown in
  the machine's **Machine: PSG**. The ZX Next keeps the order its own software sets.
- **Separation** - how far apart the left and right sides are, for the sound chips (the ZX
  Next's too) and the General Sound: 100% keeps them fully apart, 0% is mono. 75% at first.

```{index} Low cut (DC), High cut (anti-alias), anti-alias filter
```

**Filters**:

- **Board filter** - what every machine sounds through (see [Board filter](#board-filter)
  below). **No filter**, the default, plays every machine clean. **By machine** gives each
  machine its own board's filter, the one its **Board filter** list on the **Machine** page
  names: on the beeper, and on some boards on the sound chip. Any other choice puts all of
  the sound of every machine through that filter.
- **Low cut (DC)** - takes out the lowest frequencies. The chips' output sits above zero
  rather than around it, and that offset only takes loudness away from the rest of the
  sound. On by default.
- **High cut (anti-alias)** - takes out the frequencies too high for the output rate.
  Without it they come back as false notes, which is heard most in beeper music as a
  harsh crunch. On by default; it costs a little CPU time.

```{index} Volumes, Master
```

**Volumes** - one slider for each device, from 0 to 100:

| Volume | What it sets |
|---|---|
| **Master** | everything together; the same as the toolbar's volume |
| **Beeper** | the machine's own speaker |
| **Tape** | the tape, while it plays or records |
| **PSG** | the AY or YM sound chips |
| **DAC** | Covox and Soundrive |
| **General Sound** | the General Sound card |
| **SAA1099** | the SAA1099 chip |

The rest of the **Sound** page is the same for every machine.

## A machine's own

```{index} A machine's own, Apply to all machines, Overridden, Global setting, Machine spec
```

A machine can be heard otherwise than this page says: its **PSG stereo** and **Separation**
in **Machine: PSG** (see [PSG](options-machine.md#psg)), its **Board filter** on the
**Machine** page (see [Board filter](options-machine.md#board-filter)). The first two
choices of those lists say in gray what they stand for:

- **Global setting**, the default - this page's choice: **Global setting (ABC, 75%)**, or
  with **By machine** here, **Global setting (Machine spec)**: each machine as its board was
  made. What it gives this machine is noted under **Stereo** in **Machine: PSG** - **Result:
  Mono (machine spec)** - and in the **Board filter**'s tooltip;
- **Machine spec** - how its board was made, whatever this page says: **Machine spec (TV
  (RF))**;
- then, under a line, the values.

Any choice but the first is the machine's own. It is heard whatever this page says, and a
change here does not take it away. The first choice puts the machine back on this page.

While some machine has a choice of its own, the foot of **Stereo** or **Filters** says
how many, marked {icon}`exclamation` - **Overridden: 2** - and its tooltip names them, one a
line, with what each one has. **Apply to all machines** beside it, grayed out while there
are none, sets that group's choice for every machine at once: **Stereo**'s order and
**Separation**, taking their own order and separation away, or **Filters**' board filter,
taking theirs. It asks first, with the same list, and keeps whatever else you changed on
them. **Cancel** in **Options** does not undo it.

The ZX Next has no choices of its own here: it is heard as this page says.

## Board filter

```{index} Board filter, beeper; speaker, beeper; filter, piezo
```

The beeper is a single bit of a port, but what you heard from it depended on the board: a
48K's own small speaker, a TV, an amplifier behind a coupling capacitor, a piezo sounder.
On some machines the sound chip went the same way. **Board filter** in **Filters**
picks what every machine sounds through. Its choices are also under the arrow of the
toolbar's {icon}`speaker` **Mute** button and in **Sound** of the right-click menu, and with
them, under **Output filters**, **Low cut (DC)** and **High cut (anti-alias)**. They are
this page's: when the running machine is heard through a filter of its own, the menu says so
over them.

The filter runs in the machine's time, so it slows down and speeds up with it: in **Slow
Motion** a high tone it cuts stays cut as it drops in pitch.

| Choice | Filters | What goes through them | What it is on the board | Machines it comes with |
|---|---|---|---|---|
| **No filter** | none | - | boards with only resistors between the beeper and the output | Pentagon, Pentagon 1024 SL, Profi, Phoenix, ALF, ZX Next |
| **48K speaker** | low-pass, 4.4 kHz on a rising edge and 2.3 kHz on a falling one; high-pass, 500 Hz, 12 dB an octave | beeper, tape | the ULA's pin drives the speaker with slow, uneven edges; the speaker is small and gives little bass. The 500 Hz is a guess: there is no measurement of the speaker | 48K |
| **TV (RF)** | low-pass, 3.2 kHz | beeper, tape, PSG | a TV's FM sound on its aerial input (50 us de-emphasis); the board mixes the PSG in before the modulator | 128K, +2 |
| **+2A/+3** | low-pass, 2.5 kHz; high-pass, 340 Hz on the beeper and the tape, 150 Hz on the PSG | beeper, tape, PSG | the transistor that mixes them has a 100 nF capacitor on its output, on every socket; the high-passes are the coupling capacitors. Worked out from the schematic, not measured; the 150 Hz is a guess | +2A, +3 |
| **ZX Evolution** | low-pass, 8.6 kHz | beeper, tape | a 2.2K resistor and a 10 nF capacitor on the beeper's input | ZX Evolution |
| **Scorpion** | high-pass, 530 Hz | beeper, tape | the coupling capacitor on the beeper's way out | Scorpion ZS 256 and Turbo+ |
| **ATM Turbo** | high-pass, 80 Hz | beeper, tape | the coupling capacitor into the amplifier | ATM Turbo 2+ |
| **Piezo** | a ring at 4.1 kHz; under it the bass falls off 6 dB an octave | beeper, tape | a piezo sounder of the ZP-3 kind, as some clones had. The ring is the element's own; how sharp it is and how the bass falls were set by ear, as there is no measurement of the element | - |

The low-pass and high-pass filters here fall off 6 dB an octave unless the table says
otherwise.

With **By machine**, a filter takes what the table's third column says, as the board did; the
other sound devices - General Sound, the SAA1099, Covox and Soundrive - have outputs of their
own on every board and are not filtered. A filter picked from the list takes all of the sound,
the sound chips and cards too: it is how the music would sound through that board, whether
or not the machine could have had it. With **TV (RF)** and **+2A/+3** they go the way the
table gives the PSG.

```{index} Board filter; PWM music, Low-pass 5 kHz
```

**Low-pass 5 kHz, 24 dB/oct**, last in the list, is no board's. It cuts the highs from
5 kHz, 24 dB an octave, and keeps the bass. It is made for beeper music played by pulse
width (PWM), whose pulses carry a whistle far above the tune: the whistle is gone, and the
music is a little dulled.

## Beeper

```{index} beeper, MIC in the beeper
```

The beeper is the machine's own speaker, and every machine has it. Its volume is
**Beeper** on the **Sound** page. **MIC in the beeper**, in the **ULA** group of
**Machine: Advanced Settings**, also mixes the signal the machine sends to the tape into
the beeper sound, a tenth as loud, as a machine with a ULA does (see
[Advanced settings](options-machine.md#advanced-settings)).

## Recording the sound

```{index} Record Sound to WAV, sound; recording
```

**Capture > Record Sound to WAV...** writes what you hear to a file; see
[Capture menu](capture.md).

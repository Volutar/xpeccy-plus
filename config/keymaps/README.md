# Keyboard layouts

Every `*.map` file here is one keyboard layout. The emulator lists them by file
name (Options / Input, and the Keymap menu); `ZX Spectrum 128K` means "no file",
i.e. the built-in table in `src/xcore/keymap.cpp`. A `ZX Spectrum 128K.map` as
earlier versions shipped it is not listed (that layout is built in now); an edited
one is listed under its full file name.

A layout only overrides the host keys it mentions; everything else keeps its
built-in mapping. An override replaces a key's whole sequence, it does not
extend it.

## Format

Tab-separated, one host key per line:

```
<host key>	<zx key>	<zx key> ...
```

All fields after the first are concatenated into one sequence, so `` ` `` +
`C` + `1` means "pressing the host backquote presses Caps Shift and 1 together"
(that is EDIT).

Host key names are the first column of `keyMapInit[]` in
`src/xcore/keymap.cpp`: `1`..`0`, `Q`..`P`, `A`..`L`, `Z`..`M`, `ENT`, `SPC`,
`LS`, `RS`, `LC`, `RC`, `LA`, `RA`, `TAB`, `CAPS`, `BSP`, `ESC`, `F1`..`F11`,
`UP`, `DOWN`, `LEFT`, `RIGHT`, `HOME`, `END`, `INS`, `DEL`, `PGUP`, `PGDN`,
`` ` ``, `\`, `[`, `]`, `;`, `"`, `-`, `+`, `,`, `.`, `/`, and the numpad keys
`N0`..`N9`, `NDOT`, `NENT`, `NPLUS`, `NMINUS`, `NMUL`, `NSLASH`, `NLOCK`.

Spectrum keys are single characters: `1`..`0`, lowercase `a`..`z`, `E` for
Enter, `C` for Caps Shift, `S` for Symbol Shift, and a literal space for Space.

A joystick binding is `J` plus a direction instead of a Spectrum key: `JU`,
`JD`, `JL`, `JR` for the four directions, `JF` for fire, `J2`..`J4` for the
extra buttons. Append `*` (`JF*`) to bind the second joystick.

## Note on modifiers

`LS`/`RS` and `LC`/`RC` are only told apart on Windows and Linux. On macOS both
Shifts collapse to `LS` and both Ctrl and Cmd collapse to `LC`, so a layout that
puts Symbol Shift only on the right-hand keys leaves Symbol Shift unreachable
there. This is why the built-in `ZX Spectrum 128K` is what the emulator starts
with: it keeps Symbol Shift on `LC`.

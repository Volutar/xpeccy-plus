# Bundled interface styles

Qt style sheets for the emulator's own windows - menus, dialogs and the debugger. Pick one in
Setup - Xpeccy+ - General - Style, or set `style = <file>` in `config/config.conf`. `System` there,
an empty `style =`, means no style sheet at all: every window looks the way the desktop tells
it to, which on Windows is light and on most Linux desktops follows the system theme.

| File | Look |
| --- | --- |
| `Light.qss` | the grays Windows itself draws - same as `System` there |
| `Dark.qss` | neutral dark gray, the usual editor palette |
| `Sublime.qss` | Monokai, the color scheme Sublime Text ships with |
| `Gruvbox.qss` | warm brown-olive dark, mustard accent - Pavel Pertsev |
| `Solarized Dark.qss` | low contrast, blue-green dark - Ethan Schoonover |
| `Solarized Light.qss` | the same palette on warm paper instead of white |
| `Dracula.qss` | violet-tinted dark with purple and pink - Zeno Rocha and contributors |
| `ZX Spectrum.qss` | near-black chrome, the machine's own magenta on top |

These are built into the emulator (their sources are in `res/styles/`). A `.qss` of your own
dropped in this folder joins the list, and one named like a built-in replaces it.

Gruvbox, Solarized and Dracula are the work of their authors and carry the MIT license, same as
the rest of Xpeccy+; the palettes are reproduced here, nothing else is taken from them.

Every file has the same rules and differs only in colors, so a palette can be moved from one
to another by hand. Each file lists its palette in the header comment.

## Four rules that look odd out of context

**No font anywhere.** The debugger takes its font from Setup and measures its columns with it;
a `font-family` here would win over that setting and the columns would follow the style sheet
instead of the user.

**Arrows and check marks come from `url(:/images/styles/...)`.** Once any style sheet is set,
Qt stops asking the desktop theme to draw those marks and falls back to its own: a filled
triangle in the text color, framed by a bevel that looks its age on a light background. A
style sheet cannot draw a chevron either - the CSS border triangle every web page uses ends up
as a filled rectangle in Qt - and there is no property that recolors a mark on its own, the
one available color is the widget's text color. So the marks are drawn ahead of time, in two
tints, and live in the application resources: a resource path is absolute, while a relative
`url()` would be resolved against the working directory rather than against this file. See
`packaging/make-style-icons.py` to change a shape or a tint.

**`QGroupBox[title=""]`.** A style sheet frame brings its own box model, and the top margin
that makes room for the title pushes every row inside the box down - a few pixels of drift for
nothing on a box that has no title. The second rule takes the margin back off those.

**`min-width` / `min-height` on `QPushButton`.** Without them a button is only as wide as its
label, and a dialog's OK / Apply / Cancel row visibly reflows the moment a style is picked. The
values match what the platform style gives those buttons.

## Debugger highlight colors

The disassembler's PC row, the selected row, the constants in the listing, the panel headers, the
changed-value fields and the id / data / crc markers in the track dump are not part of a style sheet - Qt has nowhere to put
them. They live in the `[PALETTE]` section of `config/config.conf`, and each style brings a set
of its own in the `.pal` file next to it: `Dracula.qss` and `Dracula.pal`, same
`name = #rrggbb` lines, `;` starts a comment.

Picking a style in Setup does two things, once, at the moment the style changes: it puts the
built-in colors back and then reads the `.pal` over them. So every style starts from the same
place - `System` ends up with the defaults, and a `.pal` naming only a few colors leaves no
leftovers from the style before.

From there the colors belong to the configuration like any other: change any of them in Setup -
Xpeccy+ - Debugger - Palette (right click on a swatch puts the built-in default back) and what you set
stays, both when the emulator is restarted and when the same style is picked again later. Only
switching to a *different* style starts the two steps over.

On Windows 11, `dbg.header.bg`/`dbg.header.txt` also color the native titlebar of every window
(via DWM - see `src/titlebar.cpp`), so picking a style tints the titlebar the same as the
debugger's panel headers instead of leaving it the plain system one. `System` leaves it alone.

# The virtual keyboard

- `zx-black.json`, `zx-white.json` - the skins, drawn for Xpeccy+ by Volutar in the format of
  his `layout_svg.py` (canvas, gradient, contour, colors, icons, rows of keys with their
  labels). The program draws them itself at the size the window has (`src/xgui/vkbdskin.cpp`);
  the keys' places in the rows say which Spectrum key each one is.
- `zx-128.json`, `zx-128-white.json` - the Spectrum 128 (the Spectrum+ keyboard), classic and
  in white plastic with colored lettering, drawn by Volutar on the places and sizes measured
  off photos of the real one, with a strip on the right to right-click for the menu, the case's
  steps between the rows running out to its edge. Each key says what it presses.
- `zx-next.json`, `zx-next-white.json`, `zx-next-magenta.json` - the ZX Spectrum Next: the
  128K skins' keys on the Next's places (its membrane schematic and the user manual's
  drawing), with its legends and its red, yellow, green and blue stripes on the right; black,
  white, and Magenta - the black keys in that edition's translucent case.
- `zx-kopir.json`, `zx-kopir-worn.json` - the 58-key keyboard of the Kopir plant
  (Kozmodemyansk) and of Karon (Ryazan), early 1990s, on many of the clones (the Delta-S,
  Profi, Scorpion, Spektr-BK001...), drawn by Volutar off photos of one in a Spektr-BK001
  case: cream, grey and a red BREAK, its Enter two rows tall with a flat step, its L with a
  strip glued on (`"parts"`); its caps a little wider than the real ones, for the lettering.
  The worn one is it with some caps crooked and the cream ones yellowed (`"base"`, `"wobble"`).
- In the menu: 48K (Classic), 48K (White), 128K (Classic), 128K (White), Next (Black), Next
  (White), Next (Magenta), Klava (the worn Kopir), Klava (Washed) - black, white, 128,
  128-white, next, next-white, next-magenta, kopir-worn, kopir in the config.
- `keys-font.ttf` - the lettering, made by `tools/vkbd_font.py` from Liberation Sans Bold
  2.1.5; see LICENSE_ICONS and LICENSE_LIBERATION.

A file of the same name in `vkbd/` of the settings folder is drawn instead of the built-in
one, and again each time it is saved. A file of any other name there, `zx-<name>.json`, is
in the Skin menu under that name.

## Fields of the program's own

All of them may be left out; a layout without them is drawn as before.

Comments: `//` to the end of the line and `/* ... */` anywhere outside a string are left out
when the layout is read.

In `canvas`:
- `"ground"` - what shows through the contours around the keys (the picture's `bg` if not
  given).
- `"lit"` - the color of a pressed key's light (`#00c8ff`). The key under the pointer gets
  the same light, dimmer.
- `"light"` - where the light is: `"contour"` (the default, and for any other value) shows it
  through the contour around the key; `"pad"` puts it on the key itself, the rim of its dome,
  or a ring inside its cap's edge on a key with no dome, `"light_w"` units wide (1.8) - for
  keys set too close for a contour to carry a light (the 128K).

In a label: `"eb"` - that many canvas pixels thicker, a number or `[x, y]`.

In a key:
- `"zx"` - what it presses: `"1"`..`"0"`, `"A"`..`"Z"`, `"CS"`, `"SS"`, `"ENTER"`, `"SPACE"`,
  or several at once, `["CS", "1"]`; `[]` presses nothing (lettering). Without it the key's
  place in the 48K's four rows of ten says what it is.
- `"y"`, `"h"` - its own, over the row's.
- `"outline"` - `[[x, y], ...]`, a polygon of right angles in canvas units drawn instead of
  the box (an Enter over two rows); its labels are placed from the middle of its bounds.
- `"corner"` - its own corner radius, a number or `[tl, tr, br, bl]`. On an outline the
  four go to the corners that jut out by the side they face; the corners that cut in take
  the smallest.
- `"gradient"` - its own, as the layout's below; `"dome"` - its own, or `false` for none;
  `"side"` - its own, or `false` for none. Each is read over what the key had, field by
  field: `"dome": {"gradient": ...}` gives the layout's dome other colors and keeps its shape.
- `"style"` - the name of one of the layout's `"styles"`; the key's own fields go over it.
- `"tilt"` - turned that many degrees clockwise about its middle, its labels, contour, light
  and the place a click takes with it; `"shift"` - `[dx, dy]`, moved that far. Over the
  wobble's; `"tilt": 0` keeps a key straight.
- `"parts"` - more caps of the same key, `[{...}, ...]`: each a box, `"x"`, `"w"`, `"y"`,
  `"h"` in canvas units (the key's where not given), or an `"outline"`, with the cap fields
  a key has - `"style"`, `"gradient"`, `"dome"`, `"side"`, `"corner"` - over the key's own.
  Each is drawn under the key's cap, or over it with `"over": true`; a click, the light and
  a tilt take them with the key, its labels stay where the key's own box puts them; one with
  `"light": false` takes the click but is drawn the same when the key is lit. The
  Kopir's Enter is a tall cap and a flat step on its lower left over its slope; its L, a cap
  and a flat strip glued on its right.

`"styles"` - named sets of a key's cap fields, `{"grey": {...}, "red": {...}}`: `"gradient"`,
`"dome"`, `"side"`, `"corner"`, as a key has them. A key with `"style"` takes the layout's, then
the style's over them, then its own. A key naming a style that is not there stops the layout
from loading.

`"side"` - the cap's sloped sides seen from above (the layout's for every key, a style's, a
key's own): a band `"w"` wide inside the cap's edge, `[left, top, right, bottom]` in canvas
units, cut into four by the lines from its outer corners to its inner ones, each side
`"shade"`d - from 0 to 1 that much darker, from 0 to -1 that much lighter - or a `"fill"` color
of its own. `"w"`, `"shade"` and `"fill"` each take one value for all four. The sides are drawn
over the cap's gradient and under its dome.

`"wobble"` - some caps sitting a little crooked, as on a worn keyboard: `{"tilt", "shift",
"share", "seed"}` - that share of the keys (0..1, all if not given) turned up to `"tilt"`
degrees either way and moved up to `"shift"` canvas units each way. Which keys and how far
come from `"seed"`, the same every time the layout is drawn.

`"base"` - the name of another skin (`"128-white"` for `zx-128-white.json`) this one is built
on: that one's fields, this one's own in place of them, a whole field at a time. A worn skin
is `{"base": "<its straight one>", "wobble": {...}}`. The base is looked for in `vkbd/` of the
settings folder first, then among the built-in ones.

`"gradient"` (the caps', and a key's own): the stops, or `{"stops", "h", "from"}` - `"h"` its
length in canvas units (the whole body if not given) from the body's top, or its bottom with
`"from": "bottom"`; past it the end colors go on. A key two rows tall keeps a one-row shading.
`"angle"` turns it that many degrees clockwise from straight down (45: to the bottom right),
across the whole box.

`"dome"` - a shape laid on every cap (the 128's rounded pad): `"inset"` `[left, top, right,
bottom]` from the key's box (a number for all four; negative goes past it), `"corner"` a
number or `[tl, tr, br, bl]`, `"cut"` - cut flat that far below the key's top, and
`"gradient"` (as above) or `"fill"` a color, and `"edge"` a color drawn `"ew"` wide inside
its rim and under its cut - or a gradient (as above) across the dome, an edge lit on one side
and dark on the other: `{"angle": 15, "stops": [[0, "#808080"], [1, "#d0d0d0"]]}`. It never leaves the cap. Instead of the box it may have an
`"outline"` of its own, `[[x, y], ...]` from the key's top left, with `"corner"` one radius
for all its corners or one each; `"more"` lists more such shapes (inset or outline, corner,
cut), joined to it - the 128K's Enter: a wide key's round end and a channel up its narrow
part.

`"bars"` - strips on the ground that are not keys (the case's steps, the Next's colors):
`[{"x", "y", "w", "h", "corner", "gradient" or "fill"}, ...]` in canvas units.

In `rainbow`: `"bands"` - `[[color, x], ...]`, each band's x at `"y0"`; the bands run from
`"y0"` to `"y1"` (by default the four of the 48K, over the whole height); `"bands": []` for
none.

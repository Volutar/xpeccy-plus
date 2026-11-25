# Bundled shaders

GLSL post-processing filters, drawn through OpenGL: none with `--no-gl` or without an
OpenGL 3.3 driver (OpenGL ES 3.0). Pick a look in the Display list of Options - Video or in
View - Display; in `config/config.conf` it is `shader = <file>` and, for a file with presets,
`shader.preset = <name>`. A fresh config starts with the RGB monitor look of `crt.txt`; an empty or
missing `shader` line turns filtering off.

A shader with parameters has a sheet of knobs: the gear beside the list, or View - Display -
Settings. A knob turned is kept with its look, in `values.conf` of this folder. Save as
preset writes the look as it stands into `<name>.preset` here - the shader's file, a name
and the values that are not the shader's defaults:

```
shader = crt.txt
name = My TV
SCAN = 0.7
CURVE = 0.02
```

With Side fill set to Stretched Edges and its Blur to Off, the shader draws the whole view:
the texture coordinates run on past 0 and 1 to the view's edges, the frame is clamped at its
edge dots there, and `uniform float xpWide` is 1 (0 otherwise). A shader of your own can read
it to leave off what belongs to the tube's edge - rounded corners, a vignette - as the bundled
ones do; a curve is best kept to the picture, `warp(clamp(uv, 0.0, 1.0)) + (uv - clamp(uv, 0.0, 1.0))`.

The bundled files are Xpeccy+'s own, under its MIT license; what each builds on is named
with it below.

## Scanlines - Xpeccy+

`scanlines.txt`, MIT as the rest of Xpeccy+: plain scanlines with a hard or a soft dot.
Three knobs: Depth, Gap width and Softness. A scanline is a line of the picture at any
scale (Line fit, under All settings, counts whole screen rows instead: even lines, but no
longer one to a line of the picture). The light the gaps take is given back: the picture is as bright as with
no shader, but for colors at full light, whose lines are flat on top and a little darker
over all.

## CRT - Xpeccy+

`crt.txt`, MIT as the rest of Xpeccy+: a tube in stages, each a frame of knobs and each off
at 0, when its pass is not drawn at all.

| Stage | What it does |
| --- | --- |
| Beam | scanlines (how dark the gap, how wide), the dot's focus along the line, bright lines wider |
| Divergence | the red and blue beams land beside the green one, more towards the screen's edges |
| Mask | aperture grille, slot mask or shadow mask, as fine as the tube: triads to a dot of the picture |
| Glow | light around bright things |
| Afterglow | a lit dot fades over the next frames, blue first |
| Signal | the cable - RGB; composite or RF into a PAL set, by a 128K's color carrier or a 48K's; or a SECAM set; a ghost, interference, noise |
| Screen | curvature, round corners, vignette, brightness, contrast |

The mask is drawn in whole screen pixels and thins out by itself as the picture gets
smaller; a small picture has no curve. What the scanlines and the mask take is given back.

With Input at Composite or RF each line is coded as a PAL encoder does it - luma with
the color on a carrier - and decoded as a set does: the color low-passed and combined
with what its delay line holds of the line before, the luma less a band around the
carrier. So fine black and white
patterns take on color and color edges crawl. Color carrier picks the encoder, for any
machine alike: a 128K's is locked to its dots and its patterns stand (Phase picks one
of the 8 ways it can lock at power-on), a 48K's runs free and they move. Programs that
draw with the carrier are written for the 128K's. With Input at SECAM a line carries one
color difference as the frequency of a carrier and the set reads that frequency alone
(ITU-R BT.470's numbers): a pattern of a dot on, a dot off - 3.5 MHz, beside the
carrier's band - pulls the reading its way and takes on one flat warm color, the same
for a checkerboard and for stripes, with no crawl. Such a look takes the frames
before Anti-flicker mixes them and mixes them itself, after decoding. Every preset comes
on RGB: Input is turned by hand.

| Preset | Look |
| --- | --- |
| Home TV | a 14" set: a coarse slot mask, a soft beam, glow, a bulging screen |
| RGB monitor | the Commodore 1084S: a finer slot mask, a sharp beam, a slight curve |
| PVM | a broadcast monitor: aperture grille, the sharpest beam, flat |
| Xpixie | soft and glowing, with afterglow, a ghost and noise |

The two-pass beam follows Hyllian's CRT (MIT); the mask layouts Timothy Lottes's and
hunterk's (public domain); the beams' parting towards the edges is Volutar's; the ghost,
the interference and the noise are after Mattias Gustavsson's newpixie CRT (MIT / public
domain), which Xpixie is named after.

## Names of older versions

The aperture grille and shadow mask files (`grille-nocurve-crisp.txt`,
`tridot-curve-light.txt`...), `tvline-nocurve-light.txt` and `tvline-curve-light.txt`,
`monitor-1084.txt`, `monitor-1084-flat.txt`, `pvm-sharp.txt`, `composite-crt.txt`,
`composite-crt-flat.txt` and `zx-rf-artifact.txt` are not bundled any more:
the tube does what each did. A config that names one opens the tube with its knobs turned
to stand for it. A file of that name put into this folder is used as it is, as any shader
of your own.

## Writing a shader

A shader is one text file in this folder. The reload key (Options - Hotkeys, "Reload Shader")
loads it again; if it does not compile, the picture is drawn plain, the button "Error: check
the log" stands beside the Display list of Options - Video (the first error in its tip, all
of them on a click), and the event log has every error as `file:line: text`. The file stays
chosen: fix it and press the key.

### One pass

Two sections, `vertex:` and `fragment:`; what stands before the first one is free text. The
older bundled files are written this way, in GLSL 1.10 with the names below (they are
rewritten for the context at load), and a file in GLSL 3.30 / ES 3.00 with its own `#version`
line is taken as it is.

| Name | What |
| --- | --- |
| `uniform sampler2D rubyTexture` | the machine's frame, two texels a dot across, unfiltered |
| `uniform vec2 rubyInputSize`, `rubyTextureSize` | its size in texels |
| `uniform vec2 rubyOutputSize` | the picture's size in the window, pixels |
| `uniform float xpWide` | 1 when the shader draws the whole view (see the top of this file) |
| `gl_FragCoord` | window pixels: a mask counted from it keeps its pitch at any scale |

### Several passes

**This part of the format is new and may still change** while the display models that are to
use it are being written.

A file with a `pass` line is a chain. Each pass draws into a texture of its own and the next
one reads it; the last pass draws into the window. The names a pass sees are the ones
RetroArch's slang shaders use, so a shader from there ports by hand: drop its `#version`,
its uniform blocks and its `layout(...) uniform sampler2D` lines, and split it into sections.

```
#pragma parameter GLOW "Glow" 0.30 0.0 1.0 0.05
texture SLOT = slot.png linear repeat mipmap

common:
	// GLSL put in front of every stage of every pass

pass glow:
	scale viewport 0.25
	filter linear
	format rgba16f
	when GLOW
fragment:
	in vec2 vTexCoord;
	layout(location = 0) out vec4 FragColor;
	void main() { FragColor = texture(Source, vTexCoord) * GLOW; }

pass:
fragment:
	in vec2 vTexCoord;
	layout(location = 0) out vec4 FragColor;
	void main() { FragColor = texture(Original, vTexCoord) + texture(glow, vTexCoord); }
```

- `#pragma parameter NAME "text" default min max step` - a number the shader reads as
  `uniform float NAME`. The engine declares it, as it declares every name below: a pass has
  no `#version` line and declares only its own `in` and `out`. Each parameter is a knob
  of the sheet, and the lines below, before the first pass, say how it is shown:
  - `name "text"` - what the lists call the file; without it, its file name.
  - `frames raw [when NAME]` - the frames come as the machine made them, before
    Anti-flicker mixes them (while NAME is not 0): a model of a color signal needs each
    frame as it was, and mixes after decoding, by `xpBlend`. Not while the machine's own
    picture is being recorded: the recording takes the same frames, mixed.
  - `group "Title"` - a frame of the sheet: the parameters after it stand in it.
    `group "Title" when NAME [value]` gives the frame a switch: off sets NAME to 0 (a pass
    `when NAME` is then not drawn), on puts back what it had, or `value` (with none, the
    parameter's default).
  - `tip NAME = text` - a line of what the knob does.
  - `list NAME = a, b, c` - the knob is a list; the names stand for min, min + step...
  - `more NAME ...` - shown only under All settings.
  - `preset "Name": NAME value, NAME value` - a named set of values, a line of the
    Display list. A file with presets is listed by them.
- `texture NAME = file.png [linear|nearest] [repeat|clamp|border|mirror] [mipmap]` - a
  picture beside the preset, read as `NAME`, its size as `NAMESize`.
- `pass [name]:` starts a pass; lines of its own may follow, then `vertex:` (optional: the
  default one hands `vTexCoord` on) and `fragment:`.
  - `scale source K` (the default, K = 1), `scale viewport K`, `scale absolute W H`:
    the target's size, of the pass's input, of the picture in the window, or in texels;
    `scale_x` and `scale_y` set one axis.
  - `filter nearest` (the default) or `linear`, `wrap border` (the default: transparent
    black past the edge), `clamp`, `repeat`, `mirror`: how the pass reads its inputs.
  - `format rgba8` (the default), `srgb8`, `rgba16f`. Where the GPU cannot draw into a float
    texture (OpenGL ES without the extension) `rgba16f` is `rgba8`, and the log says so.
  - `mipmap`: the pass's input gets mipmaps first.
  - `when NAME`: the pass is not run while parameter NAME is 0 - it costs nothing then,
    and the passes after it read what it would have read.
  - On the last pass `scale` and `format` are not used.

| Name | What |
| --- | --- |
| `Source` | the pass before this one; the frame for the first pass |
| `Original` | the machine's frame, two texels a dot across |
| `OriginalHistory1` .. `8` | the frame that many frames back; transparent black until there is one |
| `PassOutputN`, a pass's name | the output of pass N (counted from 0), for the passes after it |
| `PassFeedbackN`, name + `Feedback` | the output of pass N at the last paint |
| a texture's name | the picture of a `texture` line |
| any of these + `Size` | `vec4(width, height, 1 / width, 1 / height)` |
| `OutputSize`, `FinalViewportSize` | this pass's target and the picture in the window, the same way |
| `FrameCount` (`uint`) | paints counted up |
| `FrameDirection` (`int`) | 1, and -1 while rewinding |
| `MVP`, `Position`, `TexCoord` | the vertex stage's matrix and inputs |

A frame further back or a feedback is kept only when a pass names it. In the passes before
the last, row 0 of a texture is the picture's top and `gl_FragCoord.y` counts from the top;
in the last pass `gl_FragCoord` is the window's, from the bottom left. The old names of the
one-pass form work in the last pass as well.

The machine's own numbers, for a shader that models the signal:

| Name | What |
| --- | --- |
| `xpPicture` (`vec4`) | the picture in the window: x, y, width, height in pixels from the bottom left |
| `xpWindow` (`vec2`) | the window's size in pixels |
| `xpRaster` (`vec4`) | dots a line, lines a frame, the first shown dot, the first shown line |
| `xpCarrier` (`vec4`) | PAL color carrier: cycles a dot; 1 if it is locked to the dots; its phase at the frame's first dot, in cycles; the V switch's sense on the frame's first line |
| `xpFrame` (`int`) | the machine's number of the frame shown |
| `xpBlend` (`vec2`) | Anti-flicker left to the shader (`frames raw`): the frame before's part in the mix, 0 .. 0.5, else 0; 1 if only what flickers is to be mixed (the adaptive modes) |

`tests/shaders/chain.txt` in the source tree uses all of it.

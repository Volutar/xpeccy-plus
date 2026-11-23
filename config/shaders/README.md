# Bundled shaders

GLSL post-processing filters, drawn through OpenGL: none with `--no-gl` or without an
OpenGL 3.3 driver (OpenGL ES 3.0). Pick one in Options - Video, or set
`shader = <file>` in `config/config.conf`. A fresh config starts with `tvline-nocurve-light.txt`;
an empty or missing `shader` line turns filtering off.

With Side fill set to Stretched Edges and its Blur to Off, the shader draws the whole view:
the texture coordinates run on past 0 and 1 to the view's edges, the frame is clamped at its
edge dots there, and `uniform float xpWide` is 1 (0 otherwise). A shader of your own can read
it to leave off what belongs to the tube's edge - rounded corners, a vignette - as the bundled
ones do; a curve is best kept to the picture, `warp(clamp(uv, 0.0, 1.0)) + (uv - clamp(uv, 0.0, 1.0))`.

Like the ROM images, **these files are not covered by the MIT license of Xpeccy+** - each one
keeps the terms of its own author, listed below.

## Aperture grille and shadow mask - Timothy Lottes, tuned by Volutar

Public domain ("Please take and use, change, or whatever" - see the header of each file).
`crisp` keeps the scanlines hard, `light` softens them; `curve` bends the screen, `nocurve`
leaves it flat.

| File | Look |
| --- | --- |
| `grille-nocurve-crisp.txt`, `grille-nocurve-light.txt` | aperture grille, Trinitron-like |
| `grille2-nocurve-crisp.txt`, `grille2-nocurve-light.txt` | the same with a finer grille |
| `tridot-curve-crisp.txt`, `tridot-curve-light.txt` | shadow-mask triads, curved screen |
| `tridot-nocurve-crisp.txt`, `tridot-nocurve-light.txt` | shadow-mask triads, flat screen |
| `widetridot-curve-crisp.txt`, `widetridot-curve-light.txt` | wider triads, curved screen |
| `widetridot-nocurve-crisp.txt`, `widetridot-nocurve-light.txt` | wider triads, flat screen |

## TV scanlines - cgwg, Themaister and DOLLS

Copyright (C) 2010-2012 cgwg, Themaister and DOLLS, **GNU GPL v2 or later** - the notice is
kept intact at the top of each file. Distributed here as a separate data file, not linked
into the emulator.

| File | Look |
| --- | --- |
| `tvline-nocurve-light.txt` | soft TV scanlines, flat screen - **the default** |
| `tvline-curve-light.txt` | the same with screen curvature |

## Display models - Xpeccy+

Written for this project. Same MIT license as the rest of Xpeccy+.

| File | Look |
| --- | --- |
| `composite-crt.txt`, `composite-crt-flat.txt` | composite / RF home TV, with and without curvature |
| `monitor-1084.txt`, `monitor-1084-flat.txt` | Commodore 1084 shadow-mask RGB monitor |
| `pvm-sharp.txt` | Sony PVM broadcast monitor, sharp |
| `zx-rf-artifact.txt` | ZX Spectrum RF color artifacts |

## Writing a shader

A shader is one text file in this folder. The reload key (Options - Hotkeys, "Reload Shader")
loads it again; if it does not compile, the picture is drawn plain, the button "Error: check
the log" stands beside the Shader list of Options - Video (the first error in its tip, all
of them on a click), and the event log has every error as `file:line: text`. The file stays
chosen: fix it and press the key.

### One pass

Two sections, `vertex:` and `fragment:`; what stands before the first one is free text. The
bundled files are written this way, in GLSL 1.10 with the names below (they are rewritten
for the context at load), and a file in GLSL 3.30 / ES 3.00 with its own `#version` line is
taken as it is.

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
  no `#version` line and declares only its own `in` and `out`.
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

`tests/shaders/chain.txt` in the source tree uses all of it.

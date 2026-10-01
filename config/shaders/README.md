# Bundled shaders

GLSL post-processing filters for the OpenGL build. Pick one in Options - Video, or set
`shader = <file>` in `config/config.conf`. The bundled default is `tvline-nocurve-light.txt`.
An empty `shader =` turns filtering off.

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

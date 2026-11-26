# Shader engine test chain

`chain.txt` is a preset that uses every kind of thing a pass can ask the engine for, each
in a form a window shot can prove. It is not a look. `grid.png` is its texture.

To run it, copy both files into `shaders/` of a config directory, start the emulator with
that `--confdir` and pick **chain** in View > Display. Over the control port (`port` in
`config.conf`):

- `shader-info` - the chain as built: every pass, its target's size and format, whether its
  program came from the cache or was compiled and in how many milliseconds, the parameters,
  the last load's errors.
- `shader-param GRIDON 0` - turns the `grid` pass off: it is not drawn, and has no target.
- `gpu-time on`, then `gpu-time` - GPU time of each pass, the picture (the last pass) and the
  side fill, a mean of about 60 paints.
- `winshot-main FILE` - the window as painted.

What to see:

| Pass | Asks for | In the shot |
| --- | --- | --- |
| `bright`, `back` | a float target (`format rgba16f`) | the picture as it went in; dark and flat if the target is not float |
| `ghost` | `OriginalHistory3` | a moving thing is there twice |
| `trail` | its own feedback, a parameter | a moving thing leaves a tail |
| `grid` | `when`, a viewport-sized target, a texture | a grid of 8 pixels over the picture, or none |
| the last | the machine's numbers | rows of 16 squares at the bottom left, a bit each, high bit left |

The rows, from the bottom: dots a line, lines a frame, the first shown dot, the first shown
line, the color carrier in cycles a dot x 65536, locked x 2 + the V switch's sense, the
carrier's phase x 65535, the machine's frame number, the paints counted, the frame texture's
width, the picture's width in the window, antiflicker left to the chain in per cent.

`shader-param RAW 1` asks for frames unmixed (`frames raw when RAW`): with Anti-flicker on,
the top row then reads its strength (50 at full, and 256 more in an adaptive mode) and a picture that flickers between two frames is no
longer mixed; at `RAW 0` the row is 0 and the frames come mixed as ever.

## OpenGL ES

On Linux `XPECCY_GL_ES=1` in the environment asks for an OpenGL ES context instead of a
desktop one (a test switch: nothing else sets it). With Mesa,
`MESA_EXTENSION_OVERRIDE="-GL_EXT_color_buffer_float -GL_EXT_color_buffer_half_float"` takes
the float targets away, and the chain must still draw: `bright` falls back to `rgba8` and the
log says so.

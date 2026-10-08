#pragma once

#include "../libxpeccy/spectrum.h"

// A recording run on to a frame: from the snapshot rzx_seek() stood it at,
// flat out with the picture held, the way fast loading runs a tape.
// start() is called under emu_lock and returns the frame playback stands at,
// frame() at the end of every emulated frame.
int rzx_seek_start(Computer*, int frame);
void rzx_seek_frame(Computer*);
int rzx_seeking();
// let the machine go now - the rewind is about to take it
void rzx_seek_cancel(Computer*);

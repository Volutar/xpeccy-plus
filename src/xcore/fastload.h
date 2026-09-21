#pragma once

#include "../libxpeccy/spectrum.h"

// Fast loading: the machine runs flat out while a loader reads the tape.
// frame() is called at the end of every emulated frame and decides it.
void fastload_frame(Computer*);
// let the machine go now - the debugger is about to take it
void fastload_stop(Computer*);
// the machine is being run through a load: the picture is held
int fastload_busy();
// keep fast loading from taking the machine (the bench runs its own mode)
void fastload_hold(int);
// after every opcode while fastload_on: skips an edge loop, returns the ns it moved on
extern int fastload_on;
int fastload_step(Computer*);
// --bench-loops: skip edge loops in any fast mode - 1 exactly, 2 as edge
// detection, 3 exactly and checked against running them
void fastload_bench(int mode);

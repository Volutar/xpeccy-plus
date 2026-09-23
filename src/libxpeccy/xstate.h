// A snapshot of the running machine, taken and put back in place.
//
// Not a save file: nothing here is portable, versioned or written to disk. It
// is a list of memory ranges inside the live machine, copied into one buffer
// and copied back. That is all run-ahead needs, and it is what rewind would
// need too.
//
// What it does NOT cover, on purpose:
//  - the keyboard, the joysticks and the mouse. Input is meant to survive a
//    rollback: that is the whole point of running ahead.
//  - the tape, and the data behind the disk / hdd / sd media. The controllers'
//    own state is kept, the images are not, so a write that happened inside a
//    rolled-back frame stays written. xstate_safe() below says when that
//    matters.
//  - the ROM. memSetBank leaves a ROM page with no write callback, so it can
//    never change.
//  - the breakpoint maps - 4.6 MB of debugger bookkeeping the machine never
//    reads back.
//
// Checking that the list is complete: take a snapshot, run N frames, hash every
// range, load the snapshot back, run the same N frames and hash again. State
// that matters and is not in the list sends the two runs apart, and the hashes
// differ. Keep N even, or the two runs end on different image buffers and the
// ray pointers differ for no reason. Run in 2026-09 over all 12 ZX profiles: it
// found the ZX48 ramMask case straight away. Worth building in properly before
// rewind, which is far less forgiving of a gap than run-ahead - there a
// rollback is 20 ms, here it would be seconds.

#pragma once

#include <stddef.h>

#include "spectrum.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xState xState;

xState* xstate_create(void);
void xstate_destroy(xState*);

// 1 on success. A save fails only when the machine has more parts than the
// chunk list holds; a load fails when the machine is not the one that was
// saved (a profile or hardware switch in between), and then it changes nothing.
int xstate_save(xState*, Computer*);
int xstate_load(xState*, Computer*);

// 1 when a frame may be run and then thrown away: everything it can touch is
// either inside the snapshot or unmoved by an extra frame of emulation. This is
// the other half of the coverage list above, so the two live in one file.
int xstate_safe(Computer*);
// The same, for a caller that carries the tape's position across itself.
int xstate_safe_tape_aside(Computer*);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stddef.h>

#include "../libxpeccy/spectrum.h"

// Rewind: a snapshot of the machine every few frames (conf.emu.rewind), kept
// for a while and put back on request. Everything here runs on the emulation
// thread, but rewind_clear().

// at the end of every emulated frame. phase is where the next sound sample
// falls (xThread::sndNsFixed): the chips are flushed on the samples, so it is
// machine state too, if one kept outside the machine.
// A key held there starts playing the history back.
void rewind_frame(Computer*, long long* phase);
// While the history plays back it stands in for the machine: one output sample
// a call, and now and then the picture of the next snapshot back. 0 when it is
// not playing (the key was let go: the machine is back on the snapshot shown),
// 1 for a sample, 2 for a sample and a new picture.
int rewind_play(Computer*, long long* phase);
// every sound sub-sample the machine makes and the emulated time it covers
// (16.16 ns), for playing back
void rewind_sound(sndPair, long long nsFixed);
// the key, down or up (any thread)
void rewind_want(int);
// 1 while the history is being played back (any thread)
int rewind_active();
// how far back the picture shown is from where the rewind began, in tenths of a
// second of emulated time; the last one stays after the key is let go (any thread)
int rewind_back_tenths();
// the machine is not the one the history was taken from: drop it at the next
// frame. Safe from any thread.
void rewind_clear();

// snapshots held, the newest being 0
int rewind_count();
// memory the history holds, bytes
size_t rewind_bytes();
// put back the snapshot back places behind the newest and drop the ones after
// it; 1 on success
int rewind_restore(Computer*, int back, long long* phase);

// the same without dropping anything, and the frame a snapshot was taken on,
// counted by rewind_clock(): frames emulated, which a reset does not start again
int rewind_load(Computer*, int back, long long* phase);
int rewind_frame_of(int back);
int rewind_clock();

#ifdef XBENCH
// For the check (--bench-rewind): whether the machine as it stands now is that
// snapshot, byte for byte. A difference is reported to the log.
int rewind_matches(Computer*, int back);
// no snapshots are taken while this is on
void rewind_hold(int);
#endif

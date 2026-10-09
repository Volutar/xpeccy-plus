#pragma once

#include <QString>

#include "../libxpeccy/spectrum.h"

// Making an RZX recording, the app's half: when it starts and stops, where the
// file goes, what the creator block says, and the file written now and then
// while the machine runs on. The log itself is libxpeccy's (rzxrec.c).
// Start and stop are called with the machine held (emu_lock); tick() takes it.

bool rzxr_on();
// into that file, from the machine as it stands or from a recording being
// played taken over at the frame it has got to; an ERR_* code
int rzxr_start(Computer*, const QString& path);
// where a recording of what the machine runs would go: beside the image in
// use, named after it
QString rzxr_suggest();
// ends it and writes the file, without its bookmarks when finalized: its
// path, empty when nothing was written
QString rzxr_stop(Computer*, bool finalize = false);
// a bookmark at the next frame's end, and back to the last one (or the start)
void rzxr_bookmark();
bool rzxr_rollback(Computer*);
// a file on disk without its bookmarks; an ERR_* code
int rzxr_finalize_file(const QString& path);
// every emulated frame: how long slow motion and rollbacks were in use
void rzxr_frame(Computer*);
// the gui's timer: the file brought up to date every so often
void rzxr_tick(Computer*);

QString rzxr_path();			// the file being recorded into
int rzxr_rollbacks();
double rzxr_slow_secs();
QString rzxr_message();			// what the window should say, once

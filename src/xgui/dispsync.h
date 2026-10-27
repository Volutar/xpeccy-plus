#pragma once

// The display the main window is on: its refresh rate, as exactly as the system
// tells it, and a tick on each of its vertical blanks. VSync locks the emulation
// to that tick (see pacing.cpp).
//
// The rate Qt gives (QScreen::refreshRate) is a rounded integer on Windows - 60
// for 59.94 - and a lock needs better than 0.1 %. So it is asked of the system
// itself, and checked against the vblank period measured once the tick runs.

class QWindow;

// called on the vblank thread at each vblank, ns on paceClockNs
typedef void (*dsyncTick)(long long ns);

// The window to follow: the rate is asked again for its screen. Call it again
// when the window moves to another screen or the mode changes.
void dsync_probe(QWindow* win);
// the rate the system gives for that screen, Hz; 0: not known
double dsync_rate();
// the rate to lock to: the measured one when it is settled and the system's is
// off from it, else the system's; 0: not known
double dsync_lock_rate();
// the measured vblank period's rate, Hz; 0 until enough vblanks are counted
double dsync_measured();
// the screen adapts its rate (VRR); only macOS says so
bool dsync_adaptive();

// The vblank tick, on its own thread. On Linux there is no thread to wait on a
// vblank with: the GL view's swaps stand in for it (dsync_swapped). Returns
// false when the platform has no tick of its own.
bool dsync_start(dsyncTick cb);
void dsync_stop();
bool dsync_running();
// a buffer swap with swap interval 1 has returned: Linux's vblank tick
void dsync_swapped();

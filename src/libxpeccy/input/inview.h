#pragma once

// What the program read from the input ports. An rzx replay has no key presses
// at all, only the bytes its IN instructions got, so this is the one source of
// what a player pressed that works the same live and in a replay.

#ifdef __cplusplus
extern "C" {
#endif

// frames a half-row keeps what was last read from it; a program that stopped
// scanning the keyboard has stopped seeing keys too
#define IV_STALE	5

// mouse buttons
#define IVM_LEFT	1
#define IVM_RIGHT	2
#define IVM_MIDDLE	4

typedef struct {
	unsigned char keys[8];	// half-rows in kbd->map order (A15 first), a set bit is a key down
	unsigned joyLive:1;	// the program reads the kempston (or the machine has one)
	unsigned mouseLive:1;	// ...and the mouse
	unsigned ext:1;		// the kempston has eight buttons
	unsigned char joy;	// kempston bits as read: R L D U F, buttons 2-4 above
	unsigned char mbtn;	// IVM_*
	unsigned char mwheel;	// the interface's 4-bit counter
	unsigned char mx;	// the position as the ports give it
	unsigned char my;
} InState;

typedef struct {
	unsigned on:1;
	InState s;		// as last read; the live flags are worked out from the frames below
	int keyFrm[8];		// frame each half-row was last known on
	int joyFrm;
	int mouseFrm;
} InView;

typedef struct Computer Computer;

extern InView inview;

void iview_reset(InView*);
void iview_in(InView*, Computer*, int port, int val);
void iview_state(InView*, int frm, InState*);

#ifdef __cplusplus
}
#endif

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// pc keys
#include "../keycode_linux.h"
#include "../keycode_windows.h"
#include "../keycode_others.h"

#include "../defines.h"

// Bumped by whatever reaches the machine from outside: a key, the joystick, the mouse,
// a reset or a load, a cheat, the debugger, Options. Frames run ahead are the machine's
// own while it stands still.
extern unsigned int x_input_gen;
static inline void x_input_bump(void) {
	__atomic_add_fetch(&x_input_gen, 1, __ATOMIC_RELAXED);
}

// joystick type
enum {
	XJ_NONE = 0,
	XJ_KEMPSTON,
	XJ_SINCLAIR_R,
	XJ_SINCLAIR_L
};

enum {
	XM_NONE = 0,
	XM_UP,
	XM_DOWN,
	XM_LEFT,
	XM_RIGHT,
	XM_LMB,
	XM_MMB,
	XM_RMB,
	XM_WHEELUP,
	XM_WHEELDN
};

// keyboard type
enum {
	KBD_NONE = 0,
	KBD_SPECTRUM,
	KBD_PROFI,
	KBD_ATM2_CODE,
	KBD_ATM2_CPM,
	KBD_ATM2_DIRECT
};

// atm2 mode submodes
enum {
	kbdZX = 0,
	kbdCODE,
	kbdCPM,
	kbdDIRECT
};

// what a ps/2 keyboard beside the matrix sends (zx evo)
enum {
	KBD_XT = 1,
	KBD_AT,
	KBD_PS2
};

#define KFL_SHIFT	(1)
#define KFL_CTRL	(1<<1)
#define KFL_ALT		(1<<2)
#define KFL_CAPS	(1<<4)
#define KFL_RUS		(1<<7)
#define KFL_RSHIFT	KFL_SHIFT

// joystick contacts
#define XJ_NONE		0
#define	XJ_RIGHT	1
#define	XJ_LEFT		(1<<1)
#define	XJ_DOWN		(1<<2)
#define	XJ_UP		(1<<3)
#define	XJ_FIRE		(1<<4)
#define XJ_BUT2		(1<<5)
#define XJ_BUT3		(1<<6)
#define XJ_BUT4		(1<<7)
#define XJ_JOYB		(1<<20)
typedef struct {
	unsigned used:1;
	unsigned enable:1;
	unsigned hasWheel:1;
	unsigned swapButtons:1;
	double sensitivity;

	unsigned lmb:1;
	unsigned rmb:1;
	unsigned mmb:1;
	unsigned char wheel;

	int xpos;
	int ypos;
	int xdelta;		// already in emulated pixels, but not scaled by
	int ydelta;		// sensitivity - that is done where the port is read
	int autox;
	int autoy;
	// callbacks
	cbirq xirq;
	void* xptr;
} Mouse;

typedef struct {
	unsigned char cpmCode;
	unsigned char rowScan;
} atmKey;

#define KEYSEQ_MAXLEN 16

typedef struct {
	const char* name;
	signed int key;		// XKEY_*
	unsigned char zxKey[KEYSEQ_MAXLEN];
	unsigned char extKey[KEYSEQ_MAXLEN];
	atmKey atmCode;
	int psCode;		// set 3
	int atCode;		// set 2	0xXXYYZZ = ZZ,YY,XX in buffer
	int xtCode;		// set 1
	int joyMask;
	unsigned char extShKey[KEYSEQ_MAXLEN];	// profi xt keyboard with Shift held, if it differs
	unsigned char chKey[KEYSEQ_MAXLEN];	// the symbol the host key typed, if it types one (kbd_ty_press)
	int chHeld;		// typed with Shift: a Shift reported after it is the same press
} keyEntry;

#define KBD_TY_MODS	4	// the host's Shift and Ctrl keys
#define KBD_TY_KEYS	8	// typed keys down at once

typedef struct Keyboard Keyboard;

typedef struct {
	int id;
	int(*read)(Keyboard*, int);		// address
	void(*press)(Keyboard*, keyEntry*);
	void(*release)(Keyboard*, keyEntry*);
	void(*sync)(Keyboard*, int);		// ns
} xKbdCore;

struct Keyboard {
	unsigned char port;		// high byte of xxFE port
	unsigned char scanmask;	// zx: half-rows the rom has read one by one. 0xff = a full KEY-SCAN

	xKbdCore* core;
	// callbacks
	cbirq xirq;
	void* xptr;
	// i8031 block (TODO: move it into ATM2)
	unsigned wcom:1;		// i8031 waiting for command
	unsigned warg:1;		// i8031 waiting for argument
	int com;
	int arg;
	unsigned char keycode;		// current pressed key code
	unsigned char lastkey;		// last pressed key code
	unsigned char flag1;		// [7] rus.scrlock.numlock.caps.0.alt.ctrl.shift [0]
	unsigned char flag2;		// [7] 0.0.0.0.0.0.0.rshift [0]
	// key matrix
	int matrix[16][16];
	int map[8];			// ZX keyboard half-row bits (0-5)
	int extkey;			// profi: keys holding EXT (D5) down
	unsigned grab:1;		// host keyboard grabbed: profi takes its xt layout
	int prfshift;			// profi xt: Shift keys held
	int prfsh[8];			// profi xt: keys that went down with their Shift pair
	// the Next's ps/2 keyboard (input/keyboard/ps2_keyb.vhd): what the host holds down
	unsigned char nxKeys[8];	// a matrix row's 7 columns, 1 pressed
	unsigned char nxCaps, nxSym;	// CAPS / SYM SHIFT presses counted, 3 bits each
	unsigned short nxLast;		// the last code taken, b9 release, b8 E0: the typematic filter
	unsigned char nxDown[64];	// the codes down now, b8 E0: a host's make twice, or a break of a key up, is no key
	unsigned short nxW[4];		// the extended keys at the membrane scan's last 4 commits, newest first: row r's columns 6, 5 at bits 2r+1, 2r
	int nxScan;			// ns to the scan's next commit while they settle, 0 when settled
	int nxNmiDown;			// F9, the NMI button, is down...
	long long nxNmiAt;		// ...since this CLK_28 (nx_now)
	int nxNmiCut;			// a digit was pressed with it: a function key, no NMI
	// a host key's Spectrum combination, its shift and its key apart (kbd_seq_press)
	int cbLead;			// ns from the shift going down to its key
	int cbLag;			// ns from the key going up to its shift, 0: both at once
	int cbWait;			// ns left before cbKeys go, 0: nothing waits
	int cbRel;			// cbKeys go up (the shift), else down (the key)
	unsigned char cbKeys[KEYSEQ_MAXLEN];
	unsigned char cbThen[KEYSEQ_MAXLEN];	// pressed when cbKeys go up: the host shifts a typed key hid
	// typing by character: the host's Shift/Ctrl held, and the typed keys down (kbd_ty_*)
	struct {int key; int hid; unsigned char zx[KEYSEQ_MAXLEN];} tyMod[KBD_TY_MODS];
	struct {int key; int held; unsigned char zx[KEYSEQ_MAXLEN];} tyKey[KBD_TY_KEYS];
	// pc keyboard
	unsigned lock:1;	// ps/2 keyboard disabled
	int pcmode;		// xt/at/ps2 (self)
	int pcmodeovr;		// override pcmode (0:pcmode, xt/at - convert scancodes in ps/2 controller)
	unsigned char log[16];	// scancodes not read yet, the avr's ring (XT_LOG_*)
	unsigned char logStart;
	unsigned char logEnd;
	keyEntry kent;
	int per;
	int kdel;		// pc:delay after 1st press
	int kper;		// pc:autorepeat period
};

typedef struct {
	unsigned used:1;
	unsigned extbuttons:1;
	int type;
	int state;
} Joystick;

typedef struct {
	char key;
	int row;
	int mask;
} keyScan;

Keyboard* kbd_create(cbirq, void*);
void kbd_destroy(Keyboard*);
void kbd_set_type(Keyboard*, int);
void kbd_set_core(Keyboard*, xKbdCore*);
void kbd_press(Keyboard*, keyEntry*);
void kbd_release(Keyboard*, keyEntry*);
void kbdTrigger(Keyboard*, keyEntry*);
void kbdReleaseAll(Keyboard*);
void kbd_reset(Keyboard*);
void kbd_sync(Keyboard*, int);
void kbd_combo_sync(Keyboard*, int);
void kbd_set_combo(Keyboard*, int, int);
// The Next's membrane scan: 4608 clocks of 28 MHz (membrane.vhd, zxnext_top 1179)
#define KBD_SCAN_NS	164571
// The Evo's avr holds a key SHIFT_PAUSE (8) ticks of its 2.951 ms timer after the
// CS/SS of its pair, 20.7 to 23.6 ms by the timer's phase; both go up at once (zx.c)
#define KBD_EVO_LEAD	22135417
void kbd_set_repeat(Keyboard*, int);
int kbd_rd(Keyboard*, int);
int xt_read(Keyboard*);
void xt_log_clear(Keyboard*);

Mouse* mouseCreate(cbirq, void*);
void mouseDestroy(Mouse*);
void mousePress(Mouse*, int, int);
void mouseRelease(Mouse*, int);
void mouseReleaseAll(Mouse*);
int mouseGetX(Mouse*);
int mouseGetY(Mouse*);
int mouse_buttons(Mouse*);

Joystick* joyCreate();
void joyDestroy(Joystick*);
void joyPress(Joystick*, int);
void joyRelease(Joystick*, int);
unsigned char joyInput(Joystick*);

#ifdef __cplusplus
}
#endif

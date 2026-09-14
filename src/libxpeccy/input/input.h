#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// pc keys
#include "../keycode_linux.h"
#include "../keycode_windows.h"
#include "../keycode_others.h"

#include "../defines.h"

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
#define KFL_NUMLOCK	(1<<5)
#define KFL_SCRLOCK	(1<<6)
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
	int xdelta;
	int ydelta;
	int autox;
	int autoy;
	// callbacks
	cbirq xirq;
	void* xptr;
} Mouse;

typedef struct {
	int key;
	const char seq[8];
} xKeySeq;

typedef struct {
	char key;
	unsigned char row;
	unsigned char mask;
} xKeyMtrx;

typedef struct {
	int pos;
	unsigned char data[16];
} xKeyBuf;

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
} keyEntry;

typedef struct Keyboard Keyboard;

#define KF_AUTORPT	1

typedef struct {
	int id;
	int flag;
	void(*reset)(Keyboard*);
	int(*read)(Keyboard*, int);		// address
	void(*write)(Keyboard*, int, int);	// address, data
	void(*press)(Keyboard*, keyEntry*);
	void(*release)(Keyboard*, keyEntry*);
	void(*sync)(Keyboard*, int);		// ns
} xKbdCore;

struct Keyboard {
//	unsigned reset:1;		// RES signal to CPU
	unsigned used:1;
	unsigned caps:1;
	unsigned shift:1;
	unsigned ar2:1;
	unsigned lang:1;

	unsigned inten:1;		// interrupt enabled
	unsigned kpress:1;		// key pressed
	unsigned drq:1;			// keyboard buffer have data

	unsigned char port;		// high byte of xxFE port
	unsigned char scanmask;	// zx: half-rows the rom has read one by one. 0xff = a full KEY-SCAN
	int mode;
	int flag;

	xKbdCore* core;
	// callbacks
	cbirq xirq;
	void* xptr;
	// i8031 block (TODO: move it into ATM2)
	unsigned wcom:1;		// i8031 waiting for command
	unsigned warg:1;		// i8031 waiting for argument
//	int submode;			// i8031 mode
	int com;
	int arg;
	unsigned char keycode;		// current pressed key code
	unsigned char lastkey;		// last pressed key code
	unsigned char flag1;		// [7] rus.scrlock.numlock.caps.0.alt.ctrl.shift [0]
	unsigned char flag2;		// [7] 0.0.0.0.0.0.0.rshift [0]
	// key matrix
	int row;
	int mask;
	int matrix[16][16];
	int map[8];			// ZX keyboard half-row bits (0-5)
	int extkey;			// profi: keys holding EXT (D5) down
	unsigned grab:1;		// host keyboard grabbed: profi takes its xt layout
	int prfshift;			// profi xt: Shift keys held
	int prfsh[8];			// profi xt: keys that went down with their Shift pair
	// pc keyboard
	unsigned lock:1;	// ps/2 keyboard disabled
	int pcmode;		// xt/at/ps2 (self)
	int pcmodeovr;		// override pcmode (0:pcmode, xt/at - convert scancodes in ps/2 controller)
	unsigned long outbuf;	// 0 = empty, else key scancode
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
	int shiftreg;
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
// void kbdSetMode(Keyboard*, int);
void kbd_press(Keyboard*, keyEntry*);
void kbd_release(Keyboard*, keyEntry*);
void kbdTrigger(Keyboard*, keyEntry*);
void kbdReleaseAll(Keyboard*);
void kbd_reset(Keyboard*);
void kbd_sync(Keyboard*, int);
void kbd_set_repeat(Keyboard*, int);
int kbd_rd(Keyboard*, int);
void kbd_wr(Keyboard*, int, int);
// TODO: terminate this:
//void key_press_seq(Keyboard* kbd, keyScan* tab, int* mtrx, unsigned char* xk);
//void key_release_seq(Keyboard* kbd, keyScan* tab, int* mtrx, unsigned char* xk);
//void xt_press(Keyboard*, keyEntry*);
//void xt_release(Keyboard*, keyEntry*);
int xt_read(Keyboard*);
// void kbd_nec_write(Keyboard*, int, int);
//void xt_sync(Keyboard*, int);
// end of TODO

Mouse* mouseCreate(cbirq, void*);
void mouseDestroy(Mouse*);
void mousePress(Mouse*, int, int);
void mouseRelease(Mouse*, int);
void mouseReleaseAll(Mouse*);

Joystick* joyCreate();
void joyDestroy(Joystick*);
void joyPress(Joystick*, int);
void joyRelease(Joystick*, int);
unsigned char joyInput(Joystick*);

#ifdef __cplusplus
}
#endif

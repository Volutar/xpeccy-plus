// Autostart for media given on the command line.
//
// A snapshot starts by itself: it carries the whole machine state. Tapes and
// disks are only mounted, so something has to do what the user would do by
// hand: pick an item in the boot menu, or type the line that starts the load.
//
// The keys come from the same idea as Fuse's phantom typist: they are not
// played out on a timer counted from the reset, they wait until the rom itself
// starts scanning the whole keyboard matrix, which only happens once its
// interrupt handler runs. A rom is not ready for input at that point - the 128
// still has a good two seconds of setting up to do - so a settle time follows,
// but it is measured from the rom's own progress instead of from the reset, and
// so it holds for a rom that boots slower.
//
// None of it is meant to be watched: while it runs the machine goes at full
// speed and the video puts out no pixels, so the frame the gui keeps showing is
// the blank one it started with. The first picture drawn is the loading one. It
// all ends on the last key, which is where the loading itself begins.
//
// Disks do not go in through a reset into the tr-dos rom: running "boot" off
// that reset is a TR-DOS 5.04T feature and not every romset has it. Instead
// every machine that resets to a rom page takes the same route through 48
// basic, which sits in every romset, and asks the beta disk to run boot. Evo
// and TSConf cannot be reset to a rom page at all, so there it is their own
// menu. ATM Turbo 2+ cannot either, but it can be stood in 48 basic the way a
// snapshot stands it (comp_snap_map), and then takes the common route.
//
// What a machine boots into is romset business, so the table below matches the
// romsets shipped in config/. A machine that is not in it is left alone.

#include <string.h>

#include "xcore.h"
#include "autostart.h"

#define AS_SETTLE	150	// frames between the rom waking up and the first key
#define AS_KEY_HOLD	4	// frames a key is held down
#define AS_KEY_GAP	8	// frames between two keys
#define AS_MENU_GAP	100	// after a menu pick: the rom has an editor to set up
#define AS_GIVEUP	1500	// frames to wait for a rom that never scans the keyboard

// The keys are named the way the zx keyboard itself sees them, not as host
// keys: a host key is turned into these by the user's keymap, and a layout that
// moves a key would take the typing apart - "ZX Spectrum 48K.map" puts symbol
// shift on the right-hand keys, which is enough to break the tr-dos line below.
// The strings are the ones a keymap file is written in (keyTab in
// libxpeccy/input/keyboard.c): a letter is that key, "C" is caps shift, "S" is
// symbol shift, "E" is enter, and two of them together are pressed together.
#define ZK_ENTER	"E"
#define ZK_UP		"C7"
#define ZK_DOWN		"C6"
#define ZK_QUOTE	"Sp"	// "
#define ZK_COLON	"Sz"	// :
#define ZK_EXT		"CS"	// extended mode

typedef struct {
	const char* keys;	// keys pressed together, NULL ends the list
	int gap;		// frames to wait after they are released
} asKey;

// 48 basic: LOAD is a keyword on J
static const asKey as_keyword[] = {
	{"j", AS_KEY_GAP}, {ZK_QUOTE, AS_KEY_GAP},
	{ZK_QUOTE, AS_KEY_GAP}, {ZK_ENTER, 0}, {NULL, 0}
};
// boot menu: the item wanted is the first one
static const asKey as_menu[] = {
	{ZK_ENTER, 0}, {NULL, 0}
};
// ... the second one
static const asKey as_menu2[] = {
	{ZK_DOWN, AS_KEY_GAP}, {ZK_ENTER, 0}, {NULL, 0}
};
// ... the last one: one step up beats walking down the whole list. Picking
// tr-dos this way only reaches its command line, so RUN has to follow - with no
// name it is the shortcut for the boot file
static const asKey as_menu_last_run[] = {
	{ZK_UP, AS_KEY_GAP}, {ZK_ENTER, AS_MENU_GAP},
	{"r", AS_KEY_GAP}, {ZK_ENTER, 0}, {NULL, 0}
};
// scorpion has no tape entry at all: go to 128 basic and type it out letter by
// letter - the 128 editor has no keywords
static const asKey as_scorpion[] = {
	{ZK_DOWN, AS_KEY_GAP}, {ZK_ENTER, AS_MENU_GAP},
	{"l", AS_KEY_GAP}, {"o", AS_KEY_GAP},
	{"a", AS_KEY_GAP}, {"d", AS_KEY_GAP},
	{ZK_QUOTE, AS_KEY_GAP}, {ZK_QUOTE, AS_KEY_GAP},
	{ZK_ENTER, 0}, {NULL, 0}
};
// RANDOMIZE USR 15619: REM: RUN - the beta disk entry that takes its command
// from the REM, so it lands in tr-dos and runs "boot" in one line. Typed in 48
// basic: RANDOMIZE is on T, USR needs extended mode.
static const asKey as_trdos_basic[] = {
	{"t", AS_KEY_GAP},				// RANDOMIZE
	{ZK_EXT, AS_KEY_GAP},
	{"l", AS_KEY_GAP},				// USR
	{"1", AS_KEY_GAP}, {"5", AS_KEY_GAP},
	{"6", AS_KEY_GAP}, {"1", AS_KEY_GAP},
	{"9", AS_KEY_GAP},
	{ZK_COLON, AS_KEY_GAP},
	{"e", AS_KEY_GAP},				// REM
	{ZK_COLON, AS_KEY_GAP},
	{"r", AS_KEY_GAP},				// RUN: a colon puts the editor back
							// into keyword mode, so this is the token
	{ZK_ENTER, 0}, {NULL, 0}
};
// evo reset service: the items are picked by letter
static const asKey as_evo_tape[] = {
	{"t", 0}, {NULL, 0}
};
static const asKey as_evo_disk[] = {
	{"s", 0}, {NULL, 0}
};

#define AS_NOPE	(-1)		// this machine takes no such media

typedef struct {
	int res;		// reset bank to boot into, AS_NOPE if unsupported
	const asKey* seq;
	int snap;		// the reset boots a firmware: page as a snapshot would
} asAct;

typedef struct {
	int hwid;
	asAct tape;
	asAct disk;		// tr-dos image
	asAct disk3;		// +3 disk
} asMachine;

static const asMachine as_mtab[] = {
	{HW_ZX48,	{RES_48, as_keyword},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_ZX128,	{RES_128, as_menu},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_PENT,	{RES_128, as_menu},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_P1024,	{RES_128, as_menu},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_PROFI,	{RES_128, as_menu},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_PHOENIX,	{RES_128, as_menu2},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_SCORP,	{RES_128, as_scorpion},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_PLUS2A,	{RES_128, as_menu},	{AS_NOPE, NULL},		{AS_NOPE, NULL}},
	{HW_PLUS3,	{RES_128, as_menu},	{AS_NOPE, NULL},		{RES_128, as_menu}},
	{HW_TSLAB,	{RES_128, as_menu},	{RES_128, as_menu_last_run},	{AS_NOPE, NULL}},
	{HW_ATM2,	{RES_48, as_keyword, 1},	{RES_48, as_trdos_basic, 1},	{AS_NOPE, NULL}},
	{HW_PENTEVO,	{RES_128, as_evo_tape},	{RES_128, as_evo_disk},		{AS_NOPE, NULL}},
	{HW_NULL,	{AS_NOPE, NULL},	{AS_NOPE, NULL},		{AS_NOPE, NULL}}
};

static int as_kind = AS_NONE;	// what is being started: a tape has to be played
static Computer* as_comp = NULL;
static const asKey* as_seq = NULL;	// what is left to press: NULL when idle
static int as_step = 0;
static int as_wait = 0;		// frames left before the next press or release
static int as_down = 0;		// the current key is pressed
static int as_life = 0;		// frames left before giving up
static int as_held = 0;		// fast mode and the blank screen are ours to undo

static void as_key(Computer* comp, const asKey* k, int press) {
	cbHwKey cb = press ? comp->hw->keyp : comp->hw->keyr;
	if (!cb) return;
	keyEntry ent = {};
	strncpy((char*)ent.zxKey, k->keys, KEYSEQ_MAXLEN - 1);
	cb(comp, &ent);
}

int autostart_busy() {
	return !!as_seq;
}

// The last key is in and the rom is about to ask for the media. A tape also
// has to be playing, so press that too - opening a tape to be started is a
// person putting it in and pressing Play, and "Auto play / stop" is about
// following the rom unattended, not about this. Nothing to press when fast
// loading is on: the trap reads the block without the tape moving at all, and
// one left rolling behind it would only run itself to the end.
static void as_start_media(Computer* comp) {
	if ((as_kind != AS_TAPE) || tape_flash()) return;
	tapUserPlay(comp->tape);
}

static void autostart_stop() {
	as_seq = NULL;
	as_kind = AS_NONE;
	as_step = 0;
	as_wait = 0;
	as_down = 0;
	as_life = 0;
	if (as_held) {
		conf.emu.fast = 0;
		if (as_comp) as_comp->vid->nodraw = 0;
		as_held = 0;
	}
	as_comp = NULL;
}

// What a machine would do with that media, NULL if it can do nothing: it has
// no key sequence for it, or no interface to read the image through.
static const asAct* as_act(int hwid, int dif, int kind) {
	const asMachine* mac = as_mtab;
	while (mac->hwid && (mac->hwid != hwid))
		mac++;
	const asAct* act;
	switch (kind) {
		case AS_TAPE: act = &mac->tape; break;
		case AS_DISK: act = &mac->disk; break;
		case AS_DISK3: act = &mac->disk3; break;
		default: return NULL;
	}
	if (!act->seq) return NULL;	// an AS_NOPE row: no such media on this machine
	if ((kind == AS_DISK) && (dif != DIF_BDI)) return NULL;
	if ((kind == AS_DISK3) && (dif != DIF_P3DOS)) return NULL;
	return act;
}

static const asAct* as_find(Computer* comp, int kind) {
	if (!comp) return NULL;
	return as_act(comp->hw->id, comp->dif->type, kind);
}

int autostart_can(int hwid, int dif, int kind) {
	return as_act(hwid, dif, kind) != NULL;
}

int autostart_arm(Computer* comp, int kind) {
	autostart_stop();
	const asAct* act = as_find(comp, kind);
	if (!act) return 0;
	compReset(comp, act->res);
	if (act->snap) comp_snap_map(comp);
	comp->keyb->scanmask = 0;
	as_comp = comp;
	as_kind = kind;
	as_seq = act->seq;
	as_wait = -1;					// still waiting for the rom
	as_life = AS_GIVEUP;
	return 1;
}

void autostart_frame(Computer* comp) {
	if (!as_seq) return;
	if (!as_held) {			// not in arm(): the window is not up yet there
		conf.emu.fast = 1;
		comp->vid->nodraw = 1;
		as_held = 1;
	}
	if (--as_life < 0) {		// a rom that never got to its keyboard
		autostart_stop();
		return;
	}
	if (as_wait < 0) {		// the rom is scanning the matrix: it lives
		if (comp->keyb->scanmask != 0xff) return;
		as_wait = AS_SETTLE;
	}
	if (as_wait > 0) {
		as_wait--;
		return;
	}
	if (as_down) {
		as_key(comp, &as_seq[as_step], 0);
		as_down = 0;
		as_wait = as_seq[as_step].gap;
		as_step++;
		if (!as_seq[as_step].keys) {
			as_start_media(comp);
			autostart_stop();	// the loading starts right about now
		}
	} else {
		as_key(comp, &as_seq[as_step], 1);
		as_down = 1;
		as_wait = AS_KEY_HOLD;
	}
}

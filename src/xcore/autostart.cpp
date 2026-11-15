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
// all ends on the last key, which is where the loading itself begins - but a
// tape on the +3 holds on to the first frame the rom uses it, a guard against
// the rom looking at a drive first.
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
#define AS_LOAD_WAIT	500	// after the last key: frames to wait for the rom to use the tape

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
// ... LOAD ""CODE, for a tape with no basic loader: CODE is I in extended mode
static const asKey as_keyword_code[] = {
	{"j", AS_KEY_GAP}, {ZK_QUOTE, AS_KEY_GAP},
	{ZK_QUOTE, AS_KEY_GAP}, {ZK_EXT, AS_KEY_GAP},
	{"i", AS_KEY_GAP}, {ZK_ENTER, 0}, {NULL, 0}
};
// boot menu: the item wanted is the first one
static const asKey as_menu[] = {
	{ZK_ENTER, 0}, {NULL, 0}
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
// phoenix: the menu of the ROM for revision 07 has no tape entry, and 128 BASIC is
// the third item there as in BIOS 5.03
static const asKey as_phoenix[] = {
	{ZK_DOWN, AS_KEY_GAP}, {ZK_DOWN, AS_KEY_GAP}, {ZK_ENTER, AS_MENU_GAP},
	{"l", AS_KEY_GAP}, {"o", AS_KEY_GAP},
	{"a", AS_KEY_GAP}, {"d", AS_KEY_GAP},
	{ZK_QUOTE, AS_KEY_GAP}, {ZK_QUOTE, AS_KEY_GAP},
	{ZK_ENTER, 0}, {NULL, 0}
};
// +3: not the Loader, which boots a disk in drive A before it ever asks for the
// tape. +3 BASIC and LOAD "t:":LOAD "" - "t:" makes the tape the default for
// LOAD, so the rom goes straight to it and the disks stay where they are. The
// +3 editor has no keywords either
static const asKey as_plus3_tape[] = {
	{ZK_DOWN, AS_KEY_GAP}, {ZK_ENTER, AS_MENU_GAP},
	{"l", AS_KEY_GAP}, {"o", AS_KEY_GAP},
	{"a", AS_KEY_GAP}, {"d", AS_KEY_GAP},
	{ZK_QUOTE, AS_KEY_GAP}, {"t", AS_KEY_GAP},
	{ZK_COLON, AS_KEY_GAP}, {ZK_QUOTE, AS_KEY_GAP},
	{ZK_COLON, AS_KEY_GAP},
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
	asAct code;		// a tape with no basic loader: LOAD ""CODE in 48 basic, as
				// 128 basic loads through #5B00, which such tapes overwrite
	asAct disk;		// tr-dos image
	asAct disk3;		// +3 disk
} asMachine;

static const asMachine as_mtab[] = {
	{HW_ZX48,	{RES_48, as_keyword},	{RES_48, as_keyword_code},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_ZX128,	{RES_128, as_menu},	{RES_48, as_keyword_code},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_PENT,	{RES_128, as_menu},	{RES_48, as_keyword_code},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_P1024,	{RES_128, as_menu},	{RES_48, as_keyword_code},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_PROFI,	{RES_128, as_menu},	{RES_48, as_keyword_code},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_PROFI3,	{RES_128, as_menu},	{RES_48, as_keyword_code},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_PHOENIX,	{RES_128, as_phoenix},	{RES_48, as_keyword_code},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_SCORP,	{RES_128, as_scorpion},	{RES_48, as_keyword_code},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_SCORPTP,	{RES_128, as_scorpion},	{RES_48, as_keyword_code},	{RES_48, as_trdos_basic},	{AS_NOPE, NULL}},
	{HW_PLUS2A,	{RES_128, as_menu},	{RES_48, as_keyword_code},	{AS_NOPE, NULL},		{AS_NOPE, NULL}},
	{HW_PLUS3,	{RES_128, as_plus3_tape},	{RES_48, as_keyword_code},	{AS_NOPE, NULL},		{RES_128, as_menu}},
	{HW_TSLAB,	{RES_128, as_menu},	{AS_NOPE, NULL},	{RES_128, as_menu_last_run},	{AS_NOPE, NULL}},
	{HW_ATM2,	{RES_48, as_keyword, 1},	{RES_48, as_keyword_code, 1},	{RES_48, as_trdos_basic, 1},	{AS_NOPE, NULL}},
	{HW_PENTEVO,	{RES_128, as_evo_tape},	{AS_NOPE, NULL},	{RES_128, as_evo_disk},		{AS_NOPE, NULL}},
	{HW_NULL,	{AS_NOPE, NULL},	{AS_NOPE, NULL},	{AS_NOPE, NULL},		{AS_NOPE, NULL}}
};

// NextZXOS: NextBASIC from its menu, .cd dir, then cls:.nexload name or, for a .snx,
// cls:spectrum "name" - built per file, the lines NextZXOS's own browser types
// (browser.cfg). Not the Command Line: it leaves the global transparency at 00, and a
// file that keeps the Next's registers starts with it. A dot command takes its argument
// as typed: no quotes, so a name with a space goes in 8.3
#define AS_NEX_KEYS	160
static asKey as_nex[AS_NEX_KEYS];
static char as_nex_str[AS_NEX_KEYS][3];

// one character of a file name as the keys that type it, NULL if none does
static const char* as_nex_char(char c) {
	static const char* sym[][2] = {
		{".", "Sm"}, {"_", "S0"}, {"-", "Sj"}, {" ", " "}, {"(", "S8"}, {")", "S9"},
		{"!", "S1"}, {"&", "S6"}, {"'", "S7"}, {"+", "Sk"}, {"=", "Sl"}, {",", "Sn"},
		{"#", "S3"}, {"$", "S4"}, {"%", "S5"}, {"@", "S2"}, {";", "So"}, {"~", "Sa"},
		{"{", "Sf"}, {"}", "Sg"}, {"\"", "Sp"}, {":", "Sz"}, {"/", "Sv"}, {NULL, NULL}
	};
	static char one[2];
	if ((c >= 'A') && (c <= 'Z')) c += 'a' - 'A';	// names are matched without case
	if (((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9'))) {
		one[0] = c;
		one[1] = 0;
		return one;
	}
	for (int i = 0; sym[i][0]; i++) {
		if (c == sym[i][0][0]) return sym[i][1];
	}
	return NULL;
}

static int as_kind = AS_NONE;	// what is being started: a tape has to be played
static Computer* as_comp = NULL;
static int as_board = 0;	// a divMMC maps: its firmware says where the machine ends up
static int as_codeTape = 0;	// the tape's first header is bytes: LOAD ""CODE
static const asKey* as_seq = NULL;	// what is left to press: NULL when idle
static int as_step = 0;
static int as_wait = 0;		// frames left before the next press or release
static int as_down = 0;		// the current key is pressed
static int as_life = 0;		// frames left before giving up
static int as_held = 0;		// fast mode and the blank screen are ours to undo
static int as_block = -1;	// the keys are in: the tape's block then, until the rom uses it

void autostart_key(Computer* comp, const char* keys, int press) {
	keyEntry ent = {};
	strncpy((char*)ent.zxKey, keys, KEYSEQ_MAXLEN - 1);
	x_key(comp, &ent, press);
}

static void as_key(Computer* comp, const asKey* k, int press) {
	autostart_key(comp, k->keys, press);
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
	as_block = -1;
	if (as_held) {
		conf.emu.fast = 0;
		if (as_comp) vid_set_nodraw(as_comp->vid, 0);
		as_held = 0;
	}
	as_comp = NULL;
}

void autostart_cancel() {
	if (as_seq) xlog(XLG_FILE, XLL_INFO, "autostart cancelled");
	autostart_stop();
}

// What a machine would do with that media, NULL if it can do nothing: it has
// no key sequence for it, or no interface to read the image through.
static const asMachine* as_row(int hwid) {
	const asMachine* mac = as_mtab;
	while (mac->hwid && (mac->hwid != hwid))
		mac++;
	return mac;
}

static const asAct* as_act(int hwid, int dif, int kind) {
	const asMachine* mac = as_row(hwid);
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

// The first header the rom will meet is bytes: LOAD "" would skip it, and a
// tape with no basic loader on it then never loads at all.
static int as_tape_code(Tape* tap) {
	for (int i = 0; i < tap->blkCount; i++) {
		TapeBlockInfo inf = tapGetBlockInfo(tap, (tap->block + i) % tap->blkCount);
		if (inf.type == TAPE_HEAD)
			return inf.htype == TAPE_HT_CODE;
	}
	return 0;
}

// A divMMC that maps (a firmware in its EEPROM with the jumper closed, or
// MAPRAM set) boots before the rom and decides where the machine ends up -
// esxDOS leaves every machine in 48 BASIC - and the TR-DOS entry at 3Dxx and
// the +3 Loader are its from then on: no disk in a drive can be started.
static int as_board_maps(Computer* comp) {
	DivMMC* div = comp->div;
	return div->model && (!div->eeWrite || div->mapram);
}

int autostart_board_blocks(Computer* comp, int kind) {
	return comp && (kind != AS_TAPE) && as_board_maps(comp);
}

// The 48 BASIC rom is the one paged in at 0000: bit 4 of 7FFD, and on a +2A/+3
// bit 2 of 1FFD too (rom 3). Asked once the rom scans the keyboard, it says
// which editor the keys go into, whatever firmware put the machine there.
static int as_in_48_rom(Computer* comp) {
	if (!(comp->p7FFD & 0x10)) return 0;
	if ((comp->hw->id == HW_PLUS2A) || (comp->hw->id == HW_PLUS3))
		return (comp->p1FFD & 0x04) ? 1 : 0;
	return 1;
}

int autostart_arm(Computer* comp, int kind) {
	autostart_stop();
	const asAct* act = as_find(comp, kind);
	if (!act) return 0;
	if (autostart_board_blocks(comp, kind)) return 0;
	const asAct* code = &as_row(comp->hw->id)->code;
	as_codeTape = (kind == AS_TAPE) && code->seq && as_tape_code(comp->tape);
	if (as_codeTape)
		act = code;
	as_board = as_board_maps(comp);
	// With the board on, the firmware is started from a power cycle: what it
	// kept across a reset - esxDOS holds on to a tape attached in its browser,
	// and LOAD "" would take that one - goes, and it boots as on a cold start.
	if (as_board)
		xm_power_cycle();
	else
		x_user_reset(comp, act->res);
	if (act->snap) comp_snap_map(comp);
	comp->keyb->scanmask = 0;
	as_comp = comp;
	as_kind = kind;
	as_seq = act->seq;
	as_wait = -1;					// still waiting for the rom
	as_life = AS_GIVEUP;
	return 1;
}

// one typed line: a command, its argument and what closes it, then Enter; a space in
// the argument only where it is quoted
static int as_nex_line(int n, const char* cmd, const char* arg, const char* tail, int gap) {
	for (int part = 0; part < 3; part++) {
		for (const char* s = (part == 0) ? cmd : (part == 1) ? arg : tail; *s; s++) {
			const char* k = ((part == 1) && (*s == ' ') && !*tail) ? NULL : as_nex_char(*s);
			if (!k || (n >= AS_NEX_KEYS - 2)) return -1;
			snprintf(as_nex_str[n], 3, "%s", k);
			as_nex[n].keys = as_nex_str[n];
			as_nex[n++].gap = AS_KEY_GAP;
		}
	}
	as_nex[n++] = {ZK_ENTER, gap};
	return n;
}

int autostart_arm_nex(Computer* comp, const char* dir, const char* name, int welcome, int snx, int powered) {
	autostart_stop();
	if (!comp || (comp->hw->id != HW_ZXNEXT)) return 0;
	int n = 0;
	if (welcome) as_nex[n++] = {" ", AS_MENU_GAP};	// SPACE=Start NextZXOS
	as_nex[n++] = {"n", AS_MENU_GAP};		// NextBASIC
	n = as_nex_line(n, ".cd ", dir, "", AS_MENU_GAP);
	if ((n >= 0) && snx) n = as_nex_line(n, "cls:spectrum \"", name, "\"", 0);
	else if (n >= 0) n = as_nex_line(n, "cls:.nexload ", name, "", 0);
	if (n < 0) return 0;
	as_nex[n] = {NULL, 0};
	if (!powered) x_user_reset(comp, RES_DEFAULT);	// else the caller has just power-cycled it
	comp->keyb->scanmask = 0;
	as_comp = comp;
	as_kind = AS_NONE;
	as_seq = as_nex;
	as_wait = -1;
	as_life = AS_GIVEUP;
	return 1;
}

void autostart_frame(Computer* comp) {
	if (!as_seq) return;
	// not in arm(): the window is not up yet there. Again every frame: fast
	// loading from the tape before this one lets go on the first, and takes
	// fast mode with it
	conf.emu.fast = 1;
	vid_set_nodraw(comp->vid, 1);
	as_held = 1;
	if (--as_life < 0) {		// a rom that never got to its keyboard, or the tape
		autostart_stop();
		return;
	}
	if (as_block >= 0) {		// the tape plays, or the trap has read a block
		Tape* tap = comp->tape;
		if (tap->on || (tap->block != as_block))
			autostart_stop();
		return;
	}
	if (as_wait < 0) {		// the rom is scanning the matrix: it lives
		if (comp->keyb->scanmask != 0xff) return;
		// the board's firmware had its say: a machine left in 48 BASIC
		// takes the 48 keys, not its boot menu's
		if (as_board && (as_kind == AS_TAPE) && as_in_48_rom(comp))
			as_seq = as_codeTape ? as_keyword_code : as_keyword;
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
			if ((as_kind != AS_TAPE) || (comp->dif->type != DIF_P3DOS) || comp->tape->on) {
				autostart_stop();	// the loading starts right about now
			} else {
				as_block = comp->tape->block;
				as_life = AS_LOAD_WAIT;
			}
		}
	} else {
		as_key(comp, &as_seq[as_step], 1);
		as_down = 1;
		as_wait = AS_KEY_HOLD;
	}
}

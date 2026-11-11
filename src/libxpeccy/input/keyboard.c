#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "input.h"

// common matrix

keyScan findKey(keyScan* tab, char key) {
	int idx = 0;
	while (tab[idx].key && (tab[idx].key != key)) {
		idx++;
	}
	return tab[idx];
}

void key_press(Keyboard* kbd, keyScan* tab, int* mtrx, unsigned char ch) {
	if (!ch) return;
//	printf("kbd_press_key %c\n", ch);
	keyScan key = findKey(tab, ch & 0x7f);
	key.row &= 0x0f;
	mtrx[key.row] &= ~key.mask;
	// if (key.mask) printf("row %i : %X\n",key.row, mtrx[key.row]);
	if (ch & 0x80)			// profi EXT, see kbdScanProfi
		kbd->extkey++;
	// update matrix
	for (int i = 0; i < 16; i++) {
		if (key.mask & (1 << i))
			kbd->matrix[key.row][i]++;
	}
}

void key_press_seq(Keyboard* kbd, keyScan* tab, int* mtrx, unsigned char* xk) {
	while (*xk != 0x00) {
		key_press(kbd, tab, mtrx, *xk);
		xk++;
	}
}

void key_release(Keyboard* kbd, keyScan* tab, int* mtrx, unsigned char ch) {
//	if (ch) printf("kbd_release_key %c\n", ch);
	keyScan key = findKey(tab, ch & 0x7f);
	key.row &= 0x0f;
	if ((ch & 0x80) && (kbd->extkey > 0))
		kbd->extkey--;
	for (int i = 0; i < 16; i++) {
		if (key.mask & (1 << i)) {
			if (kbd->matrix[key.row][i] > 0)
				kbd->matrix[key.row][i]--;
			if (kbd->matrix[key.row][i] == 0) {
				mtrx[key.row] |= key.mask;
			}
		}
	}
}

void key_release_seq(Keyboard* kbd, keyScan* tab, int* mtrx, unsigned char* xk) {
	while (*xk != 0x00) {
		key_release(kbd, tab, mtrx, *xk);
		xk++;
	}
}

void key_trigger(Keyboard* kbd, keyScan* tab, int* mtrx, unsigned char ch) {
	keyScan key = findKey(tab, ch & 0x7f);
	mtrx[key.row] ^= key.mask;
	for (int i = 0; i < 16; i++) {
		if (key.mask & (1 << i)) {
			if (!(mtrx[key.row] & (1 << i))) {	// 0 = pressed now
				kbd->matrix[key.row][i]++;
			} else if (kbd->matrix[key.row][i] > 0) {
				kbd->matrix[key.row][i]--;
			}
		}
	}
}

void key_trigger_seq(Keyboard* kbd, keyScan* tab, int* mtrx, unsigned char* xk) {
	while (*xk != 0x00) {
		key_trigger(kbd, tab, mtrx, *xk & 0x7f);
		xk++;
	}
}

// zx spectrum std keyboard

keyScan keyTab[] = {
	{'1',4,1},{'2',4,2},{'3',4,4},{'4',4,8},{'5',4,16},{'6',3,16},{'7',3,8},{'8',3,4},{'9',3,2},{'0',3,1},
	{'q',5,1},{'w',5,2},{'e',5,4},{'r',5,8},{'t',5,16},{'y',2,16},{'u',2,8},{'i',2,4},{'o',2,2},{'p',2,1},
	{'a',6,1},{'s',6,2},{'d',6,4},{'f',6,8},{'g',6,16},{'h',1,16},{'j',1,8},{'k',1,4},{'l',1,2},{'E',1,1},
	{'C',7,1},{'z',7,2},{'x',7,4},{'c',7,8},{'v',7,16},{'b',0,16},{'n',0,8},{'m',0,4},{'S',0,2},{' ',0,1},
	{0,0,0}
};

// A host key that is a Spectrum combination (. is SS+M, Delete CS+0) goes down shift
// first and up shift last, cbLead / cbLag apart in emulated time: a program that reads
// the key and its shift by separate INs, or takes the shift from its previous scan,
// never sees the key alone. The Next's membrane scan does the same for its extended
// keys (164.6 us before, two scans after), the Evo's avr holds the key ~22 ms after
// its shift. Keys of our own (autostart, the virtual keyboard, a gamepad's) have no
// ps/2 code and go whole.

static int kbd_is_shift(unsigned char ch) {
	return (ch == 'C') || (ch == 'S');
}

// xk's shifts and its other keys; 0 when it is not both
static int kbd_combo_split(const unsigned char* xk, unsigned char* sh, unsigned char* rest) {
	int ns = 0, nr = 0;
	for (; *xk && (ns + nr < KEYSEQ_MAXLEN - 1); xk++) {
		if (kbd_is_shift(*xk)) sh[ns++] = *xk;
		else rest[nr++] = *xk;
	}
	sh[ns] = rest[nr] = 0;
	return ns && nr;
}

static void kbd_combo_go(Keyboard* kbd) {
	kbd->cbWait = 0;
	if (kbd->cbRel) {
		key_release_seq(kbd, keyTab, kbd->map, kbd->cbKeys);
		key_press_seq(kbd, keyTab, kbd->map, kbd->cbThen);
	} else {
		key_press_seq(kbd, keyTab, kbd->map, kbd->cbKeys);
	}
	kbd->cbThen[0] = 0;
}

// what still waits goes before any other key, so the order holds
static void kbd_combo_flush(Keyboard* kbd) {
	if (kbd->cbWait) kbd_combo_go(kbd);
}

void kbd_combo_sync(Keyboard* kbd, int ns) {
	kbd->cbWait -= ns;
	if (kbd->cbWait <= 0) kbd_combo_go(kbd);
}

// lead, lag in ns; a lag of 0 lets the key and its shift up at once
void kbd_set_combo(Keyboard* kbd, int lead, int lag) {
	kbd_combo_flush(kbd);
	kbd->cbLead = lead;
	kbd->cbLag = lag;
}

static void kbd_seq_press(Keyboard* kbd, keyEntry* ent, unsigned char* xk) {
	unsigned char sh[KEYSEQ_MAXLEN], rest[KEYSEQ_MAXLEN];
	if (ent->atCode && (kbd->cbLead > 0) && kbd_combo_split(xk, sh, rest)) {
		key_press_seq(kbd, keyTab, kbd->map, sh);
		memcpy(kbd->cbKeys, rest, sizeof(kbd->cbKeys));
		kbd->cbThen[0] = 0;
		kbd->cbRel = 0;
		kbd->cbWait = kbd->cbLead;
	} else {
		key_press_seq(kbd, keyTab, kbd->map, xk);
	}
}

static void kbd_seq_release(Keyboard* kbd, keyEntry* ent, unsigned char* xk) {
	unsigned char sh[KEYSEQ_MAXLEN], rest[KEYSEQ_MAXLEN];
	if (ent->atCode && (kbd->cbLag > 0) && kbd_combo_split(xk, sh, rest)) {
		key_release_seq(kbd, keyTab, kbd->map, rest);
		memcpy(kbd->cbKeys, sh, sizeof(kbd->cbKeys));
		kbd->cbThen[0] = 0;
		kbd->cbRel = 1;
		kbd->cbWait = kbd->cbLag;
	} else {
		key_release_seq(kbd, keyTab, kbd->map, xk);
	}
}

// Typing by character: a host key that types a symbol the Spectrum has (Shift+; is :)
// comes with that symbol's keys in chKey. While one is down, what the host's Shift and
// Ctrl press is hidden: the Shift that typed : is no CAPS SHIFT, and CAPS and SYMBOL
// SHIFT down together are EXTEND to the ROM. They go up before the symbol's shift goes
// down and come back after it is up (kbd_seq_press/release keep the key inside its
// shift). A Shift the host reports after a key typed with it (the Windows poll) is
// hidden as it comes; one pressed after a key typed without it is a Shift of its own.
// A typed key goes up by its host key, as what it pressed, whatever is held.
// The ZX matrix only, and the Profi's while it is one: elsewhere chKey is not read.

static int kbd_ty_on(Keyboard* kbd) {
	return (kbd->core->id == KBD_SPECTRUM) || ((kbd->core->id == KBD_PROFI) && !kbd->grab);
}

static int kbd_is_mod(int key) {
	return (key == XKEY_LSHIFT) || (key == XKEY_RSHIFT) || (key == XKEY_LCTRL) || (key == XKEY_RCTRL);
}

static void kbd_ty_hide(Keyboard* kbd) {
	for (int i = 0; i < 4; i++) {
		if (!kbd->tyMod[i].key || kbd->tyMod[i].hid) continue;
		key_release_seq(kbd, keyTab, kbd->map, kbd->tyMod[i].zx);
		kbd->tyMod[i].hid = 1;
	}
}

// the hidden host shifts still held, into seq, to be pressed again
static void kbd_ty_shown(Keyboard* kbd, unsigned char* seq) {
	int n = (int)strlen((char*)seq);
	for (int i = 0; i < 4; i++) {
		if (!kbd->tyMod[i].key || !kbd->tyMod[i].hid) continue;
		for (unsigned char* z = kbd->tyMod[i].zx; *z && (n < KEYSEQ_MAXLEN - 1); z++)
			seq[n++] = *z;
		kbd->tyMod[i].hid = 0;
	}
	seq[n] = 0;
}

// a host Shift/Ctrl going down: 1 when it is hidden, and so not pressed
static int kbd_ty_mod_press(Keyboard* kbd, keyEntry* ent) {
	if (!kbd_is_mod(ent->key)) return 0;
	int i, slot = -1;
	for (i = 0; i < 4; i++) {
		if (kbd->tyMod[i].key == ent->key) return 0;		// twice: as it always went
		if (!kbd->tyMod[i].key && (slot < 0)) slot = i;
	}
	if (slot < 0) return 0;
	kbd->tyMod[slot].key = ent->key;
	kbd->tyMod[slot].hid = (kbd->tyHeld > 0);
	memcpy(kbd->tyMod[slot].zx, ent->zxKey, KEYSEQ_MAXLEN);
	return kbd->tyMod[slot].hid;
}

// ...and up: 1 when it was hidden, and so is not in the matrix
static int kbd_ty_mod_release(Keyboard* kbd, keyEntry* ent) {
	if (!kbd_is_mod(ent->key)) return 0;
	for (int i = 0; i < 4; i++) {
		if (kbd->tyMod[i].key != ent->key) continue;
		kbd->tyMod[i].key = 0;
		return kbd->tyMod[i].hid;
	}
	return 0;
}

static void kbd_ty_press(Keyboard* kbd, keyEntry* ent) {
	int i, slot = -1;
	for (i = 0; i < 8; i++) {
		if (kbd->tyKey[i].key == ent->key) return;	// down already
		if (!kbd->tyKey[i].key && (slot < 0)) slot = i;
	}
	if (slot < 0) return;
	kbd->tyKey[slot].key = ent->key;
	kbd->tyKey[slot].held = ent->chHeld;
	memcpy(kbd->tyKey[slot].zx, ent->chKey, KEYSEQ_MAXLEN);
	kbd->tyDown++;
	if (ent->chHeld) kbd->tyHeld++;
	kbd_ty_hide(kbd);		// a Shift pressed since another typed key went down too
	kbd_seq_press(kbd, ent, ent->chKey);
}

// 1 when the key went down typed
static int kbd_ty_release(Keyboard* kbd, keyEntry* ent) {
	int i;
	if (!ent->key) return 0;		// ENDKEY: one of ours, never typed
	for (i = 0; (i < 8) && (kbd->tyKey[i].key != ent->key); i++);
	if (i == 8) return 0;
	kbd->tyKey[i].key = 0;
	kbd_seq_release(kbd, ent, kbd->tyKey[i].zx);
	if (kbd->tyKey[i].held && (kbd->tyHeld > 0)) kbd->tyHeld--;
	if (kbd->tyDown > 0) kbd->tyDown--;
	if (kbd->tyDown) return 1;
	if (kbd->cbWait && kbd->cbRel) {		// after the symbol's shift is up
		kbd_ty_shown(kbd, kbd->cbThen);
	} else {
		unsigned char back[KEYSEQ_MAXLEN] = {0};
		kbd_ty_shown(kbd, back);
		key_press_seq(kbd, keyTab, kbd->map, back);
	}
	return 1;
}

void kbd_zx_press(Keyboard* kbd, keyEntry* ent) {
	kbd_seq_press(kbd, ent, ent->zxKey);
}

void kbd_zx_release(Keyboard* kbd, keyEntry* ent) {
	kbd_seq_release(kbd, ent, ent->zxKey);
}

// Note which half-row the rom asked for. Only a single-row read counts: the rom
// scans them one by one, everything else is a game polling one row or the whole
// matrix at once. All 8 seen means the interrupt-driven KEY-SCAN is running, which
// is how the autostart knows the machine is ready for keys.
static void kbd_note_scan(Keyboard* kbd, int port) {
	unsigned char row = ~(port >> 8);
	if (row && !(row & (row - 1)))		// one bit = one half-row
		kbd->scanmask |= row;
}

int kbdScanZX(Keyboard* kbd, int port) {
	int res = 0x3f;
	kbd_note_scan(kbd, port);
	for (int i = 0; i < 8; i++) {
		if (!(port & 0x8000))
			res &= kbd->map[i];
		port <<= 1;
	}
	return res;
}

// profi: a zx keyboard, and with the host keyboard grabbed the layout of its own
// xt keyboard (the ext column), wherever a key has one. The xt keys the matrix
// lacks are a letter plus EXT, a key wired to D5 of every half-row. Shift is SS
// there, and the controller turns a shifted symbol into its own pair: = is SS+L,
// Shift+= is SS+K. A key keeps the pair it went down with until it is released.

static int prf_is_shift(keyEntry* ent) {
	return (ent->key == XKEY_LSHIFT) || (ent->key == XKEY_RSHIFT);
}

static unsigned char* prf_keys(Keyboard* kbd, keyEntry* ent) {
	return (kbd->grab && ent->extKey[0]) ? ent->extKey : ent->zxKey;
}

void kbd_prf_press(Keyboard* kbd, keyEntry* ent) {
	int i;
	if (kbd->prfshift && ent->extShKey[0]) {		// counted in grab mode only
		for (i = 0; (i < 8) && kbd->prfsh[i]; i++);
		if (i < 8) {
			kbd->prfsh[i] = ent->key;
			kbd_seq_press(kbd, ent, ent->extShKey);
			return;
		}
	}
	if (kbd->grab && prf_is_shift(ent))
		kbd->prfshift++;
	kbd_seq_press(kbd, ent, prf_keys(kbd, ent));
}

void kbd_prf_release(Keyboard* kbd, keyEntry* ent) {
	for (int i = 0; i < 8; i++) {
		if (kbd->prfsh[i] && (kbd->prfsh[i] == ent->key)) {
			kbd->prfsh[i] = 0;
			kbd_seq_release(kbd, ent, ent->extShKey);
			return;
		}
	}
	if (prf_is_shift(ent) && (kbd->prfshift > 0))
		kbd->prfshift--;
	kbd_seq_release(kbd, ent, prf_keys(kbd, ent));
}

int kbdScanProfi(Keyboard* kbd, int port) {
	int res = kbdScanZX(kbd, port);
	if (kbd->extkey && (~port & 0xff00))
		res &= ~0x20;
	return res;
}

// atm2-zx (same as KBD_SPECTRUM)
// atm2-code
// atm2-cpm
// atm2-direct (TODO: read docs one more time)

int kbd_atm2code_rd(Keyboard* kbd, int adr) {
	int res = kbd->keycode;
	kbd->keycode = 0;
	return res;
}

int kbd_atm2cpm_rd(Keyboard* kbd, int adr) {
	int res = -1;
	switch((adr >> 8) & 0xff) {
		case 0x00: res = kbd->keycode;
			kbd->keycode = 0;
			break;
		case 0x40: res = kbd->flag2;
			break;
		case 0x80: res = kbd->flag1;
			break;
	}
	return res;
}

void kbd_atm2code_press(Keyboard* kbd, keyEntry* ent) {
	kbd->keycode = ent->atmCode.cpmCode;
	kbd->lastkey = kbd->keycode;
}

void kbd_atm2code_release(Keyboard* kbd, keyEntry* ent) {
	kbd->keycode = 0;
}

// common codes

// The scancode log as ZX Evo's avr keeps it (ps2.c, ps2keyboard_*_log): 16
// bytes, and filling them all is an overflow. A read gives 0 when there is
// nothing, and FF once on an overflow, which also starts the log over.
#define XT_LOG_CLEARED	0xff		// logStart: cleared, nothing since
#define XT_LOG_OVER	0xff		// logEnd: overflowed

void xt_log_clear(Keyboard* kbd) {
	kbd->logStart = XT_LOG_CLEARED;
}

static int xt_log_empty(Keyboard* kbd) {
	if (kbd->logStart == XT_LOG_CLEARED) return 1;
	return (kbd->logEnd == kbd->logStart);
}

static void xt_log_byte(Keyboard* kbd, int bt) {
	if (kbd->logEnd == XT_LOG_OVER) return;
	kbd->log[kbd->logEnd] = bt & 0xff;
	kbd->logEnd = (kbd->logEnd + 1) & 15;
	if (kbd->logEnd == kbd->logStart)
		kbd->logEnd = XT_LOG_OVER;
}

// a key's whole code, first byte in the low bits; so a log cleared mid-code
// starts again on a key's first byte, as the avr's does
static void xt_log_put(Keyboard* kbd, unsigned long code) {
	if (!code) return;
	if (kbd->logStart == XT_LOG_CLEARED) {
		kbd->logStart = 0;
		kbd->logEnd = 0;
	}
	while (code) {
		xt_log_byte(kbd, code & 0xff);
		code >>= 8;
	}
}

int xt_read(Keyboard* kbd) {
	int res = 0;
	if (kbd->logStart == XT_LOG_CLEARED) return 0;
	if (kbd->logEnd == XT_LOG_OVER) {
		res = 0xff;
		xt_log_clear(kbd);
	} else if (!xt_log_empty(kbd)) {
		res = kbd->log[kbd->logStart];
		kbd->logStart = (kbd->logStart + 1) & 15;
		kbd->lastkey = res;
	}
	return res;
}

// peka common

unsigned long add_msb(unsigned long code, unsigned long bt) {
	unsigned long msk = 0xff;
	while (code & msk) {
		bt <<= 8;
		msk <<= 8;
	}
	code |= bt;
	return code;
}

// keyboard

// A host that sends 0xF3 picks its own delay and rate: b6..5 of the parameter
// are the delay in 250 ms steps and b4..0 the rate, 0 being the fastest at 30
// a second. ZX Evolution's avr sends 0xF3 with a parameter of 0 at start-up
// (PS2KEYBOARD_CMD_AUTOREPEAT in the avr's ps2.c), so that machine's keyboard
// runs at the fastest setting there is.
void kbd_set_repeat(Keyboard* kbd, int par) {
	kbd->kdel = (((par >> 5) & 3) + 1) * 250e6;	// 1st delay: 250, 500, 750, 1000 ms
	kbd->kper = (33 + 7 * (par & 0x1f)) * 1e6;	// repeat period: 33 to 250 ms
}

void kbd_reset(Keyboard* kbd) {
	kbd->com = -1;
	kbd_set_repeat(kbd, 0x2b);	// ps/2 power-on default until a host says otherwise
}

Keyboard* kbd_create(cbirq cb, void* p) {
	Keyboard* keyb = (Keyboard*)malloc(sizeof(Keyboard));
	memset(keyb, 0x00, sizeof(Keyboard));
	keyb->xirq = cb;
	keyb->xptr = p;
	kbd_reset(keyb);
	return keyb;
}

void kbd_destroy(Keyboard* keyb) {
	free(keyb);
}

unsigned int x_input_gen = 0;

// key press/release/trigger

void kbd_press(Keyboard* kbd, keyEntry* ent) {
	x_input_bump();
	kbd_combo_flush(kbd);
	if (!kbd->core) return;
	if (kbd_ty_on(kbd)) {
		if (ent->chKey[0]) {
			kbd_ty_press(kbd, ent);
			return;
		}
		if (kbd_ty_mod_press(kbd, ent)) return;
	}
	if (kbd->core->press)
		kbd->core->press(kbd, ent);
}

void kbd_release(Keyboard* kbd, keyEntry* ent) {
	x_input_bump();
	kbd_combo_flush(kbd);
	if (!kbd->core) return;
	if (kbd_ty_release(kbd, ent)) return;
	if (kbd_ty_mod_release(kbd, ent)) return;
	if (kbd->core->release)
		kbd->core->release(kbd, ent);
}

void kbdReleaseAll(Keyboard* kbd) {
	int i;
	x_input_bump();
	for (i = 0; i < 8; i++) {
		kbd->map[i] = -1;
	}
	for (i = 0; i < 16 * 8; i++) {
		kbd->matrix[(i >> 3) & 15][i & 7] = 0;
	}
	kbd->extkey = 0;
	kbd->prfshift = 0;
	memset(kbd->prfsh, 0, sizeof(kbd->prfsh));
	memset(kbd->nxKeys, 0, sizeof(kbd->nxKeys));
	kbd->nxCaps = kbd->nxSym = 0;
	kbd->nxLast = 0x3ff;
	memset(kbd->nxDown, 0, sizeof(kbd->nxDown));
	memset(kbd->nxW, 0, sizeof(kbd->nxW));
	kbd->nxScan = 0;
	kbd->nxNmiDown = 0;
	kbd->cbWait = 0;
	kbd->cbThen[0] = 0;
	memset(kbd->tyMod, 0, sizeof(kbd->tyMod));
	memset(kbd->tyKey, 0, sizeof(kbd->tyKey));
	kbd->tyDown = 0;
	kbd->tyHeld = 0;
	kbd->keycode = 0;
	kbd->lastkey = 0;
	if (kbd->per > 0) {
		kbd_release(kbd, &kbd->kent);
	}
	kbd->per = 0;
}

// trigger is using by kbd-window only

void kbdTrigger(Keyboard* kbd, keyEntry* ent) {
	x_input_bump();
	kbd_combo_flush(kbd);
	if (!kbd->core) return;
	switch(kbd->core->id) {
		case KBD_SPECTRUM:
		case KBD_PROFI:
			key_trigger_seq(kbd, keyTab, kbd->map, ent->zxKey);
			break;
	}
	// at/xt ???
}

// at/xt keyboard buffer
// example (at code):
// 0xE0, 0x72 (code 0x72e0) = cursor down pressed
// 0xE0, 0xF0, 0x72 (code 72f0e0) = cursor down released

unsigned long xt_get_code(Keyboard* kbd, keyEntry* kent, int rel) {
	unsigned long res = 0;
	int code;
	int mode = kbd->pcmodeovr ? kbd->pcmodeovr : kbd->pcmode;
	if (rel) {
		switch(mode) {
			case KBD_PS2:			// F0,code
				res = 0xf000 | (kent->psCode & 0xff);
				break;
			case KBD_AT:			// insert F0 before each byte with bit7=0
				code = kent->atCode;
				while(code) {
					if (!(code & 0x80))
						res = add_msb(res, 0xf0);
					res = add_msb(res, code & 0xff);
					code >>= 8;
				}
				break;
			case KBD_XT:			// set every b7
				code = kent->xtCode;
				while(code) {
					res = add_msb(res, (code & 0xff) | 0x80);
					code >>= 8;
				}
				break;
		}
	} else {		// press
		switch(mode) {
			case KBD_PS2: res = kent->psCode; break;
			case KBD_AT: res = kent->atCode; break;
			case KBD_XT: res = kent->xtCode; break;
		}
	}
	return res;
}

// the held key is due to repeat, and the clock starts again for the next one
static int xt_rpt_due(Keyboard* kbd, int ns) {
	if (kbd->per == 0) return 0;
	kbd->per -= ns;
	if (kbd->per > 0) return 0;
	kbd->per = kbd->kper;
	return 1;
}

// A machine with a ps/2 keyboard beside its own matrix - ZX Evo - gets the
// make code again while a key is held, because that is what the keyboard does
// by itself. The matrix needs nothing: the key is simply still down. Nothing
// is queued while the machine has not read what is already there, so a program
// that stops reading does not come back to a burst.
void xt_rpt_sync(Keyboard* kbd, int ns) {
	if (!xt_rpt_due(kbd, ns)) return;
	if (kbd->lock || !xt_log_empty(kbd)) return;
	xt_log_put(kbd, xt_get_code(kbd, &kbd->kent, 0));
}

void xt_press(Keyboard* kbd, keyEntry* kent) {
	if (kbd->lock) return;
	xt_log_put(kbd, xt_get_code(kbd, kent, 0));
	kbd->kent = *kent;
	kbd->per = kbd->kdel;
	kbd->xirq(IRQ_KBD_DATA, kbd->xptr);
}

void xt_release(Keyboard* kbd, keyEntry* kent) {
	if (kbd->lock) return;
	xt_log_put(kbd, xt_get_code(kbd, kent, 1));
	kbd->per = 0;		// 0 for stopping autorepeat
	kbd->xirq(IRQ_KBD_DATA, kbd->xptr);
}

int kbd_rd(Keyboard* kbd, int port) {
	int res = -1;
	if (kbd->core) {
		if (kbd->core->read) {
			res = kbd->core->read(kbd, port);
		}
	}
	return res;
}

void kbd_sync(Keyboard* kbd, int ns) {
	if (kbd->core) {
		if (kbd->core->sync) {
			kbd->core->sync(kbd, ns);
		}
	}
}

// id,cbRead,cbPress,cbRelease,cbSync
xKbdCore kbdTypeTab[] = {
	{KBD_SPECTRUM, kbdScanZX, kbd_zx_press, kbd_zx_release, xt_rpt_sync},
	{KBD_PROFI, kbdScanProfi, kbd_prf_press, kbd_prf_release, NULL},
	{KBD_ATM2_CODE, kbd_atm2code_rd, kbd_atm2code_press, kbd_atm2code_release, NULL},
	{KBD_ATM2_CPM, kbd_atm2cpm_rd, kbd_atm2code_press, kbd_atm2code_release, NULL},
	{KBD_ATM2_DIRECT, NULL, NULL, NULL, NULL},			// TODO
	{-1, NULL, NULL, NULL, NULL}
};

void kbd_set_core(Keyboard* kbd, xKbdCore* core) {
	kbd->core = core;
//	printf("kbd set mode %i\n", core->id);
}

void kbd_set_type(Keyboard* kbd, int t) {
	xKbdCore* itm = kbdTypeTab;
	while((itm->id >= 0) && (itm->id != t))
		itm++;
	if (itm->id >= 0)
		kbd_set_core(kbd, itm);
}

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
	kbd->row = key.row;
	kbd->mask = key.mask;
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

void kbd_zx_press(Keyboard* kbd, keyEntry* ent) {
	key_press_seq(kbd, keyTab, kbd->map, ent->zxKey);
}

void kbd_zx_release(Keyboard* kbd, keyEntry* ent) {
	key_release_seq(kbd, keyTab, kbd->map, ent->zxKey);
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
			key_press_seq(kbd, keyTab, kbd->map, ent->extShKey);
			return;
		}
	}
	if (kbd->grab && prf_is_shift(ent))
		kbd->prfshift++;
	key_press_seq(kbd, keyTab, kbd->map, prf_keys(kbd, ent));
}

void kbd_prf_release(Keyboard* kbd, keyEntry* ent) {
	for (int i = 0; i < 8; i++) {
		if (kbd->prfsh[i] && (kbd->prfsh[i] == ent->key)) {
			kbd->prfsh[i] = 0;
			key_release_seq(kbd, keyTab, kbd->map, ent->extShKey);
			return;
		}
	}
	if (prf_is_shift(ent) && (kbd->prfshift > 0))
		kbd->prfshift--;
	key_release_seq(kbd, keyTab, kbd->map, prf_keys(kbd, ent));
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

int xt_read(Keyboard* kbd) {
	int res;
	if (kbd->outbuf & 0xff) {
		res = kbd->outbuf & 0xff;
		kbd->lastkey = res;
		kbd->outbuf >>= 8;
	} else {
		res = -1;
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
	if (kbd->core) {
		if (kbd->core->reset) {
			kbd->core->reset(kbd);
		}
	}
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

// TODO: make this obsolete
void kbdSetMode(Keyboard* kbd, int mode) {
	kbd->mode = mode;
}

// key press/release/trigger

void kbd_press(Keyboard* kbd, keyEntry* ent) {
	if (kbd->core) {
		if (kbd->core->press) {
			kbd->core->press(kbd, ent);
		}
	}
}

void kbd_release(Keyboard* kbd, keyEntry* ent) {
	if (kbd->core) {
		if (kbd->core->release) {
			kbd->core->release(kbd, ent);
		}
	}
}

void kbdReleaseAll(Keyboard* kbd) {
	int i;
	for (i = 0; i < 8; i++) {
		kbd->map[i] = -1;
	}
	for (i = 0; i < 16 * 8; i++) {
		kbd->matrix[(i >> 3) & 15][i & 7] = 0;
	}
	kbd->extkey = 0;
	kbd->prfshift = 0;
	memset(kbd->prfsh, 0, sizeof(kbd->prfsh));
	kbd->keycode = 0;
	kbd->lastkey = 0;
//	kbd->outbuf = 0;	//kbd->kbuf.pos = 0;
//	kbd->flag = 0;
	if (kbd->per > 0) {
		kbd_release(kbd, &kbd->kent);
//		xt_release(kbd, kbd->kent);
	}
	kbd->per = 0;
}

// trigger is using by kbd-window only

void kbdTrigger(Keyboard* kbd, keyEntry* ent) {
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
	if (kbd->lock || kbd->outbuf) return;
	kbd->outbuf = add_msb(kbd->outbuf, xt_get_code(kbd, &kbd->kent, 0));
}

void xt_press(Keyboard* kbd, keyEntry* kent) {
	if (kbd->lock) return;
	kbd->outbuf = add_msb(kbd->outbuf, xt_get_code(kbd, kent, 0));
	kbd->kent = *kent;
	kbd->per = kbd->kdel;
	kbd->xirq(IRQ_KBD_DATA, kbd->xptr);
	// printf("xt press, buf = %X\n", kbd->outbuf);
}

void xt_release(Keyboard* kbd, keyEntry* kent) {
	if (kbd->lock) return;
	kbd->outbuf = add_msb(kbd->outbuf, xt_get_code(kbd, kent, 1));	// kbd->outbuf = xt_get_code(kbd, kent, 1);
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

void kbd_wr(Keyboard* kbd, int adr, int val) {
	if (kbd->core) {
		if (kbd->core->write) {
			kbd->core->write(kbd, adr, val);
		}
	}
}

void kbd_sync(Keyboard* kbd, int ns) {
	if (kbd->core) {
		if (kbd->core->sync) {
			kbd->core->sync(kbd, ns);
		}
	}
}

// id,flag,cbReset,cbRead,cbWrite,cbPress,cbRelease,cbSync
xKbdCore kbdTypeTab[] = {
	{KBD_SPECTRUM, 0, NULL, kbdScanZX, NULL, kbd_zx_press, kbd_zx_release, xt_rpt_sync},
	{KBD_PROFI, 0, NULL, kbdScanProfi, NULL, kbd_prf_press, kbd_prf_release, NULL},
	{KBD_ATM2_CODE, 0, NULL, kbd_atm2code_rd, NULL, kbd_atm2code_press, kbd_atm2code_release, NULL},
	{KBD_ATM2_CPM, 0, NULL, kbd_atm2cpm_rd, NULL, kbd_atm2code_press, kbd_atm2code_release, NULL},
	{KBD_ATM2_DIRECT, 0, NULL, NULL, NULL, NULL, NULL, NULL},			// TODO
	{-1, 0, NULL, NULL, NULL, NULL, NULL}
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

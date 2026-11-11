// Headless keyboard bench: bin/poll.bin (asm/poll.asm) on a machine, fed . and Delete
// - with their ps/2 codes as the host's keys come (KEYS=host), or as autostart and the
// virtual keyboard press them (KEYS=ours, no code) - and what the poll counted.
//
//	kbd-bench <poll.bin> <presses>	MACHINE=ZX48|ZXNext|... KEYS=host|ours
//
// Keys change at 20 ms ticks, as the host's come at frame ends: down for 3, up for 3,
// <presses> times . and then as many Delete.
//
//	kbd-bench <poll.bin> <presses>	MACHINE=... KEYS=typed ORDER=first|late|upfirst|held|plain [CHAR=0]
//
// Typing by character: the host's Left Shift and its ; key typing : (SS+Z), <presses>
// times in the ORDER given, and the matrix looked at after every step of the CPU: CAPS
// and SYMBOL SHIFT never down together (EXTEND to the ROM), Z never without SYMBOL SHIFT
// or with CAPS SHIFT, O never down, nothing left down at the end. CHAR=0 sends the ; key
// by position, as with typing off: Shift is CAPS SHIFT there, and the ROM gets EXTEND.
// ORDER=plain types the key with no Shift and then presses one: that Shift is the
// player's own and reaches the machine (CAPS SHIFT with the symbol's SYMBOL SHIFT).
//
//	kbd-bench <poll.bin> 0	MACHINE=... KEYS=mouse [MOUSE=0]
//
// The Kempston mouse at X #12, Y #34 with the left button down, and what the machine's
// ports answer: #FADF, #FBDF, #FFDF, #7B5F (X too: the interface decodes A5, A8, A9 and
// A10 only) and #F8DF (A9 low, not the mouse). MOUSE=0 has the mouse off.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spectrum.h"
#include "cpu/Z80/z80.h"		// the register names

static const char* cntName[6] = {"sep_m", "sep_0", "edge_m", "bare_m", "edge_0", "bare_0"};

// the order of the host's Shift (S) and ; (K) going down (+) and up (-), a tick each
// ('.' waits one), from the moment the poll is running
static const struct {const char* name; const char* steps;} tyOrders[] = {
	{"first", "S+.K+..K-.S-...."},	// Shift, then the key: what a typist does
	{"late", "K+S+..K-.S-....."},	// the key before its Shift: the Windows poll is late
	{"upfirst", "S+.K+.S-.K-....."},	// Shift let go first
	{"held", "S+.K+.K-.K+.K-.S-...."},	// :: with Shift held
	{"plain", "K+.S+..S-.K-...."},	// a key typed with no Shift, then a Shift of its own
	{NULL, NULL}
};

// row 7 (#FEFE): CAPS SHIFT b0, Z b1; row 0 (#7FFE): SYMBOL SHIFT b1; row 2 (#DFFE): O b1
#define TY_CS(m)	(!((m)[7] & 1))
#define TY_Z(m)		(!((m)[7] & 2))
#define TY_SS(m)	(!((m)[0] & 2))
#define TY_O(m)		(!((m)[2] & 2))

static int typed(Computer* comp, int presses) {
	const char* ord = getenv("ORDER") ? getenv("ORDER") : "first";
	const char* steps = NULL;
	for (int i = 0; tyOrders[i].name; i++)
		if (!strcmp(ord, tyOrders[i].name)) steps = tyOrders[i].steps;
	if (!steps) {
		fprintf(stderr, "no ORDER %s\n", ord);
		return 1;
	}
	keyEntry sh, semi;
	memset(&sh, 0, sizeof(sh));
	memset(&semi, 0, sizeof(semi));
	sh.key = XKEY_LSHIFT;		// as keymap.cpp has them
	strcpy((char*)sh.zxKey, "C");
	sh.atCode = 0x12;
	semi.key = XKEY_DOTCOM;
	strcpy((char*)semi.zxKey, "So");
	if (!getenv("CHAR") || strcmp(getenv("CHAR"), "0"))
		strcpy((char*)semi.chKey, "Sz");	// typed: Shift+; is :, else by position as before
	semi.chHeld = strcmp(ord, "plain");	// typed with the Shift: a late one is that Shift
	semi.atCode = 0x4c;
	int ext = 0, bare_z = 0, cap_z = 0, z_down = 0, cs_down = 0, o_down = 0;
	int pz = 0, pcs = 0, po = 0;
	const long long tick = 20000000;
	long long t = 0;
	int k = 0;
	int* m = comp->keyb->map;
	for (int n = -10; n < presses * (int)strlen(steps); k++) {
		if (n >= 0) {
			const char* st = steps + (n % strlen(steps));
			if ((st[0] == 'S') || (st[0] == 'K')) {
				keyEntry* e = (st[0] == 'S') ? &sh : &semi;
				if (st[1] == '+') comp->hw->keyp(comp, e);
				else comp->hw->keyr(comp, e);
				n += 2;
			} else {
				n++;
			}
		} else {
			n++;
		}
		while (t < (k + 1) * tick) {
			t += compExec(comp);
			int cs = TY_CS(m), ss = TY_SS(m), z = TY_Z(m), o = TY_O(m);
			if (cs && ss) ext++;
			if (z && !ss) bare_z++;
			if (z && cs) cap_z++;
			if (z && !pz) z_down++;
			if (cs && !pcs) cs_down++;
			if (o && !po) o_down++;
			pz = z;
			pcs = cs;
			po = o;
		}
	}
	int left = 0;
	for (int r = 0; r < 8; r++)
		if ((m[r] & 0x1f) != 0x1f) left |= 1 << r;
	printf("ext %d\nbare_z %d\ncap_z %d\nz_down %d\ncs_down %d\no_down %d\nleft %d\n",
		ext, bare_z, cap_z, z_down, cs_down, o_down, left);
	return 0;
}

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: kbd-bench poll.bin presses\n");
		return 2;
	}
	int presses = atoi(argv[2]);
	const char* mach = getenv("MACHINE") ? getenv("MACHINE") : "ZX48";
	int host = !getenv("KEYS") || strcmp(getenv("KEYS"), "ours");
	xhost_time_fixed = 1759665600;
	Computer* comp = compCreate();
	if (!compSetHardware(comp, mach)) {
		fprintf(stderr, "no %s core\n", mach);
		return 1;
	}
	compSetBaseFrq(comp, 3.5);
	comp_power_on(comp);		// the Next's ps/2 keymap comes in with the power
	compReset(comp, RES_DEFAULT);
	comp_kbd_release(comp);
	FILE* f = fopen(argv[1], "rb");
	if (!f) {
		fprintf(stderr, "can't open %s\n", argv[1]);
		return 1;
	}
	int c, adr = 0x8000;
	while ((c = fgetc(f)) != EOF)
		comp->hw->mwr(comp, adr++, c);
	fclose(f);
	comp->cpu->regPC = 0x8000;
	comp->cpu->flgIFF1 = comp->cpu->flgIFF2 = 0;

	// KEYS=ps2 ATCODE=n: a PC key of that set 2 code held, the half-rows #BFFE and #7FFE
	// as a program reads them meanwhile, the five keys' bits (0 pressed)
	if (getenv("KEYS") && !strcmp(getenv("KEYS"), "ps2")) {
		static const unsigned char rd[] = {
			0x01, 0xfe, 0xbf, 0xed, 0x78, 0x32, 0x00, 0x90,	// ld bc,#bffe: in a,(c): ld (#9000),a
			0x06, 0x7f, 0xed, 0x78, 0x32, 0x01, 0x90,		// ld b,#7f: in a,(c): ld (#9001),a
			0x18, 0xef						// jr #8000
		};
		for (int i = 0; i < (int)sizeof(rd); i++)
			comp->hw->mwr(comp, 0x8000 + i, rd[i]);
		comp->cpu->regPC = 0x8000;
		keyEntry key;
		memset(&key, 0, sizeof(key));
		key.atCode = (int)strtol(getenv("ATCODE") ? getenv("ATCODE") : "0", NULL, 0);
		comp->hw->keyp(comp, &key);
		long long t = 0;
		while (t < 100000000LL)		// 100 ms held
			t += compExec(comp);
		printf("bffe %02X\n7ffe %02X\n", comp->hw->mrd(comp, 0x9000, 0) & 0x1f, comp->hw->mrd(comp, 0x9001, 0) & 0x1f);
		comp->hw->keyr(comp, &key);
		compDestroy(comp);
		return 0;
	}

	if (getenv("KEYS") && !strcmp(getenv("KEYS"), "mouse")) {
		static const int port[] = {0xfadf, 0xfbdf, 0xffdf, 0x7b5f, 0xf8df};
		comp->mouse->enable = !getenv("MOUSE") || atoi(getenv("MOUSE"));
		comp->mouse->xpos = 0x12;
		comp->mouse->ypos = 0x34;
		comp->mouse->lmb = 1;
		for (int i = 0; i < 5; i++)
			printf("%04x %02X\n", port[i], comp->hw->in(comp, port[i]) & 0xff);
		compDestroy(comp);
		return 0;
	}

	if (getenv("KEYS") && !strcmp(getenv("KEYS"), "typed")) {
		int rc = typed(comp, presses);
		compDestroy(comp);
		return rc;
	}

	keyEntry dot, del;
	memset(&dot, 0, sizeof(dot));
	memset(&del, 0, sizeof(del));
	strcpy((char*)dot.zxKey, "Sm");
	strcpy((char*)del.zxKey, "C0");
	if (host) {
		dot.atCode = 0x49;		// set 2, as keymap.cpp has them
		del.atCode = 0x66;
	}
	const long long tick = 20000000;
	int ticks = 10 + presses * 2 * 6 + 10;
	long long t = 0;
	for (int k = 0; k < ticks; k++) {
		int n = k - 10;
		if ((n >= 0) && (n / 6 < 2 * presses)) {
			keyEntry* e = (n / 6 < presses) ? &dot : &del;
			if (n % 6 == 0) comp->hw->keyp(comp, e);
			else if (n % 6 == 3) comp->hw->keyr(comp, e);
		}
		while (t < (k + 1) * tick)
			t += compExec(comp);
	}
	for (int i = 0; i < 6; i++)
		printf("%s %d\n", cntName[i], comp->hw->mrd(comp, 0x9000 + i, 0));
	compDestroy(comp);
	return 0;
}

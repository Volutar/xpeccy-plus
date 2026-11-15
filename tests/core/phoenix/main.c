// Headless ZXM-Phoenix bench: the core against what the board's sources say - the
// 05.2 / 07.1 schematics (paging, ROM select, INT, turbo), the DD71 source (the ROM set
// on #37, the SD port) and the multicard's firmware 2.8.1 (#EFF7, the clock, the version).
// The expected pages are worked out here from the board's equations, not read back
// from the core.
//
//	phoenix-bench

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spectrum.h"
#include "cpu/Z80/z80.h"		// the register names

static int failures;

static void check(const char* name, int ok) {
	printf("%s: %s\n", name, ok ? "ok" : "FAIL");
	if (!ok) failures++;
}

// the i/o wrapper's part: the Beta Disk ports follow TR-DOS
static void pout(Computer* comp, int port, int val) {
	comp->flgBDI = comp->flgDOS;
	comp->hw->out(comp, port, val);
}

static int pin(Computer* comp, int port) {
	comp->flgBDI = comp->flgDOS;
	return comp->hw->in(comp, port) & 0xff;
}

// every 16K page holds its own number: ROM 00..1F, RAM 80..FF
static int page_at(Computer* comp, int adr) {
	return comp->hw->mrd(comp, adr, 0) & 0xff;
}

static void set_dos(Computer* comp, int dos) {
	comp->flgDOS = dos;
	comp->hw->mapMem(comp);
}

static Computer* machine(int ram) {
	Computer* comp = compCreate();
	if (!compSetHardware(comp, "Phoenix")) {
		fprintf(stderr, "no Phoenix core\n");
		exit(1);
	}
	// set up by hand after res/machines/phoenix.conf: a change there does not reach it
	vLayout lay = {{448, 312}, {72, 56}, {64, 16}, {256, 192}, {128, 16}, 64};	// ULA.Phoenix
	comp_set_layout(comp, &lay);
	bytesPerLine = comp->vid->full.x * 8;
	memSetSize(comp->mem, ram, MEM_512K);
	for (int p = 0; p < 32; p++) memset(comp->mem->romData + (p << 14), p, 0x4000);
	for (int p = 0; p < 128; p++) memset(comp->mem->ramData + (p << 14), 0x80 | p, 0x4000);
	difSetHW(comp->dif, DIF_BDI);
	ide_set_type(comp->ide, IDE_NEMO);
	comp->zc->on = 1;
	comp->flgGLUK = 1;
	comp->fbus = FBUS_NONE;
	comp->joy->type = XJ_KEMPSTON;
	compSetBaseFrq(comp, 3.5);
	compReset(comp, RES_128);
	comp_kbd_release(comp);
	return comp;
}

// DD21/DD23, DD31/DD32: 7FFD.0-2, 1FFD.4, 7FFD.7, 1FFD.7, 1FFD.6; XP21 and a missing SIMM cut the top
static int ram_expect(int p7, int p1, int ram) {
	int page = (p7 & 7) | ((p1 & 0x10) >> 1) | ((p7 & 0x80) >> 3) | ((p1 & 0x80) >> 2) | (p1 & 0x40);
	if (ram == MEM_128K) page &= 7;
	else if (ram == MEM_1M) page &= 0x3f;
	return 0x80 | page;
}

static void ram_pages(int ram, const char* name) {
	Computer* comp = machine(ram);
	int wrong = 0, n = 0;
	for (int a = 0; a < 16; a++) {
		for (int b = 0; b < 8; b++) {
			int p7 = (a & 7) | ((a & 8) << 4);
			int p1 = ((b & 1) << 4) | ((b & 2) << 5) | ((b & 4) << 5);
			pout(comp, 0x7ffd, p7);
			pout(comp, 0x1ffd, p1);
			n++;
			if (page_at(comp, 0xc000) != ram_expect(p7, p1, ram)) wrong++;
			if ((page_at(comp, 0x4000) != 0x85) || (page_at(comp, 0x8000) != 0x82)) wrong++;
		}
	}
	printf("%s: %d combinations of #7FFD b0-b2,b7 and #1FFD b4,b6,b7, %d wrong\n", name, n, wrong);
	if (wrong) failures++;
	pout(comp, 0x7ffd, 0x87);
	pout(comp, 0x1ffd, 0xd0);
	printf("%s: all seven bits set, page %02X\n", name, page_at(comp, 0xc000) & 0x7f);
	compDestroy(comp);
}

// RA14 = 7FFD.4 & !1FFD.1, RA15 = !1FFD.1 & (DOS/ ^ 1FFD.3); DD71: the set, 1 in TR-DOS
static int rom_expect(int p7, int p1, int dos, int set) {
	int ra14 = ((p7 >> 4) & 1) & !((p1 >> 1) & 1);
	int ra15 = !((p1 >> 1) & 1) & ((dos ? 0 : 1) ^ ((p1 >> 3) & 1));
	return ((dos ? 1 : set) << 2) | (ra15 << 1) | ra14;
}

static void rom_pages(void) {
	Computer* comp = machine(MEM_2M);
	static const char* names[4] = {"service", "TR-DOS", "Basic 128", "Basic 48"};
	int wrong = 0, n = 0;
	for (int set = 0; set < 8; set++) {
		pout(comp, 0x0037, set);
		for (int c = 0; c < 16; c++) {
			int p7 = (c & 1) << 4;
			int p1 = (c & 2) | ((c & 4) << 1);
			int dos = (c >> 3) & 1;
			pout(comp, 0x7ffd, p7);
			pout(comp, 0x1ffd, p1);
			set_dos(comp, dos);
			int got = page_at(comp, 0x0000);
			n++;
			if (got != rom_expect(p7, p1, dos, set)) wrong++;
			if (set == 0)
				printf("set 0, 7FFD.4=%d 1FFD.1=%d 1FFD.3=%d dos=%d: set %d, %s\n", (c & 1), (c >> 1) & 1, (c >> 2) & 1, dos, got >> 2, names[got & 3]);
		}
	}
	set_dos(comp, 0);
	printf("%d combinations over the 8 sets, %d wrong\n", n, wrong);
	if (wrong) failures++;
	// the DD71 decode: A6-A4 = 011 on #x7; #F7 is the multicard's
	pout(comp, 0x1ffd, 0);
	pout(comp, 0x7ffd, 0);
	pout(comp, 0x0037, 5);
	check("#37 sets the ROM set", page_at(comp, 0) == ((5 << 2) | 2));
	pout(comp, 0xffb7, 3);
	check("#B7 is #37 (A7 not decoded)", page_at(comp, 0) == ((3 << 2) | 2));
	pout(comp, 0x0027, 6);
	pout(comp, 0x00f7, 6);
	check("#27 and #F7 are not", page_at(comp, 0) == ((3 << 2) | 2));
	pout(comp, 0x0037, 0x81);
	check("b7 of #37 leaves the ROM read-only", (comp->hw->mwr(comp, 0x0000, 0x55), page_at(comp, 0) == ((1 << 2) | 2)));
	// RAM page 0 under the ROM, 1FFD.0
	pout(comp, 0x1ffd, 1);
	check("1FFD.0: RAM page 0 in #0000", page_at(comp, 0) == 0x80);
	comp->hw->mwr(comp, 0x0000, 0x55);
	check("...and writable", page_at(comp, 0) == 0x55);
	compDestroy(comp);
}

static void lock_reset(void) {
	Computer* comp = machine(MEM_2M);
	pout(comp, 0x7ffd, 0x23);
	pout(comp, 0x7ffd, 0x05);
	check("7FFD.5 locks #7FFD", page_at(comp, 0xc000) == 0x83);
	pout(comp, 0x1ffd, 0x10);
	check("...not #1FFD", page_at(comp, 0xc000) == 0x8b);
	pout(comp, 0x0037, 2);
	check("...nor #37", page_at(comp, 0) == ((2 << 2) | 2));
	pout(comp, 0xeff7, 0x80);
	compReset(comp, RES_128);
	check("reset clears #7FFD, #1FFD, #37 and #EFF7",
		!comp->reg[0] && !comp->reg[1] && !comp->reg[2] && !comp->reg[3]);
	check("...into the system ROM of set 0", page_at(comp, 0) == 2);
	comp_snap_reset(comp, RES_128);
	pout(comp, 0x7ffd, 0x10);
	check("a snapshot's reset stands in set 1, where the BASICs are", page_at(comp, 0) == ((1 << 2) | 3));
	compReset(comp, RES_128);
	check("#7FFD reads nothing", pin(comp, 0x7ffd) == 0xff);
	check("#1FFD reads nothing", pin(comp, 0x1ffd) == 0xff);
	compDestroy(comp);
}

static void multicard(void) {
	Computer* comp = machine(MEM_2M);
	xhost_time_fixed = 1759665600;		// Sunday 2025-10-05 12:00 UTC: runs repeat
	printf("version: #00F7 %02X, #00DF %02X, #00EF %02X, #01F7 %02X\n",
		pin(comp, 0x00f7), pin(comp, 0x00df), pin(comp, 0x00ef), pin(comp, 0x01f7));
	check("#EFF7 reads 00 after reset", pin(comp, 0xeff7) == 0x00);
	pout(comp, 0xdff7, 0x0b);
	check("the clock is not there without #EFF7 b7", pin(comp, 0xbff7) == 0xff);
	check("Kempston on #1F", pin(comp, 0x001f) == 0x00);
	check("...and on #6F (A6-A4 not decoded)", pin(comp, 0x006f) == 0x00);
	check("...not on #9F", pin(comp, 0x009f) == 0xff);
	pout(comp, 0xeff7, 0x80);
	check("#EFF7 reads back", pin(comp, 0xeff7) == 0x80);
	check("IODOS takes the Kempston off #6F", pin(comp, 0x006f) == 0xff);
	check("...and puts the Beta Disk's status on #1F", pin(comp, 0x001f) != 0x00);
	printf("clock: A %02X, B %02X, D %02X\n",
		(pout(comp, 0xdff7, 0x0a), pin(comp, 0xbff7)), (pout(comp, 0xdff7, 0x0b), pin(comp, 0xbff7)),
		(pout(comp, 0xdff7, 0x0d), pin(comp, 0xbff7)));
	// the hour is the host's zone's: left out
	printf("clock: minutes %02X, seconds %02X, year %02X\n",
		(pout(comp, 0xdff7, 2), pin(comp, 0xbff7)), (pout(comp, 0xdff7, 0), pin(comp, 0xbff7)),
		(pout(comp, 0xdff7, 9), pin(comp, 0xbff7)));
	pout(comp, 0xdff7, 0xc0);
	pout(comp, 0xbff7, 0x5a);
	pout(comp, 0xdff7, 0x40);
	pout(comp, 0xbff7, 0xa5);
	pout(comp, 0xdff7, 0xc0);
	check("EEPROM cell #C0 is not cell #40", pin(comp, 0xbff7) == 0x5a);
	pout(comp, 0xeff7, 0x00);
	check("the cells go with #EFF7 b7", pin(comp, 0xbff7) == 0xff);
	check("a port nothing decodes reads FF", (pin(comp, 0x00fb) == 0xff) && (pin(comp, 0x00ff) == 0xff));
	// the SD port: #57 data, #77 /CS write only; no card in
	check("#77 reads nothing", pin(comp, 0x0077) == 0xff);
	pout(comp, 0x0077, 0x00);
	check("#57 with no card reads FF", pin(comp, 0x0057) == 0xff);
	check("#5F is the Kempston, not the SD port", pin(comp, 0x005f) == 0x00);
	compDestroy(comp);
}

// a frame of JR $ at #8000, the INT line watched: clock ticks and instructions a frame, ticks with INT low
static void frame(Computer* comp, const char* name) {
	comp->mem->ramData[(2 << 14)] = 0x18;
	comp->mem->ramData[(2 << 14) + 1] = 0xfe;
	comp->cpu->regPC = 0x8000;
	comp->cpu->flgIFF1 = comp->cpu->flgIFF2 = 0;
	int f = comp->vid->fcnt;
	while (comp->vid->fcnt == f) compExec(comp);		// to a frame edge
	long long t0 = comp->tickTotal;
	long long low = 0;
	int steps = 0;
	f = comp->vid->fcnt;
	while (comp->vid->fcnt == f) {
		long long t = comp->tickTotal;
		int was = comp->vid->intFRAME ? 1 : 0;
		compExec(comp);
		steps++;
		if (was) low += comp->tickTotal - t;
	}
	printf("%s: %lld ticks and %d JRs a frame, INT low for %lld ticks\n", name, comp->tickTotal - t0, steps, low);
}

static void timing(void) {
	Computer* comp = machine(MEM_2M);
	frame(comp, "3.5 MHz");
	compSetHwTurbo(comp, 2);
	frame(comp, "turbo");
	pout(comp, 0xeff7, 0x80);
	frame(comp, "turbo, IODOS");
	pout(comp, 0xeff7, 0x00);
	set_dos(comp, 1);
	comp->cpu->regPC = 0x8000;
	// the fetch from RAM pages TR-DOS out again: only the first instruction is slow
	frame(comp, "turbo, out of TR-DOS");
	compDestroy(comp);
}

int main(void) {
	printf("== ram\n");
	ram_pages(MEM_2M, "2048K");
	ram_pages(MEM_1M, "1024K");
	ram_pages(MEM_128K, "128K");
	printf("== rom\n");
	rom_pages();
	printf("== lock-reset\n");
	lock_reset();
	printf("== multicard\n");
	multicard();
	printf("== timing\n");
	timing();
	return failures ? 1 : 0;
}

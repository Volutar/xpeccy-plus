// Headless SMUC bench: the two boards' decode against Black_Cat's SMUC 2.1/2.2
// schematics and port table (lab specs/smuc-open-ports). The original 1.x answers
// only with TR-DOS paged; the home-made 2.x in any ROM. Both hold IORQGE, so a port
// they take never reaches the machine's own #FE, which A0 = 0 hits on the Pentagon
// and A0/A1/A5 on the Scorpion.
//
//	smuc-bench

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spectrum.h"

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

static void set_dos(Computer* comp, int dos) {
	comp->flgDOS = dos;
	comp->hw->mapMem(comp);
}

static Computer* machine(const char* hw, int ram, int ide) {
	Computer* comp = compCreate();
	if (!compSetHardware(comp, hw)) {
		fprintf(stderr, "no %s core\n", hw);
		exit(1);
	}
	vLayout lay = {{448, 312}, {72, 56}, {64, 16}, {256, 192}, {128, 16}, 64};
	comp_set_layout(comp, &lay);
	bytesPerLine = comp->vid->full.x * 8;
	memSetSize(comp->mem, ram, ram);
	difSetHW(comp->dif, DIF_BDI);
	ide_set_type(comp->ide, ide);
	comp->fbus = FBUS_NONE;
	compSetBaseFrq(comp, 3.5);
	compReset(comp, RES_128);
	comp_kbd_release(comp);
	return comp;
}

// the version and revision registers read #3F/#57 (1.2), and IDE-Hi keeps a byte
static int answers(Computer* comp) {
	pout(comp, 0xd8be, 0xa5);
	int hi = pin(comp, 0xd8be);
	pout(comp, 0xd8be, 0x5a);
	return (pin(comp, 0x5fba) == 0x3f) && (pin(comp, 0x5fbe) == 0x57)
		&& (hi == 0xa5) && (pin(comp, 0xd8be) == 0x5a);
}

// a write to SYS (#FFBA) and to IDE-Hi leaves the border as an OUT #FE set it,
// outside TR-DOS: the Scorpion's #FE is off in it
static int border_kept(Computer* comp) {
	int dos = comp->flgDOS;
	set_dos(comp, 0);
	pout(comp, 0x00fe, 0x05);
	set_dos(comp, dos);
	pout(comp, 0xffba, 0xf2);
	pout(comp, 0xd8be, 0x03);
	return comp->vid->nextbrd == 5;
}

static void board(const char* hw, int ram, int ide, const char* name) {
	Computer* comp = machine(hw, ram, ide);
	int open = (ide == IDE_SMUC2);
	char buf[128];
	set_dos(comp, 1);
	snprintf(buf, sizeof(buf), "%s, TR-DOS: answers", name);
	check(buf, answers(comp));
	snprintf(buf, sizeof(buf), "%s, TR-DOS: border kept", name);
	check(buf, border_kept(comp));
	set_dos(comp, 0);
	snprintf(buf, sizeof(buf), "%s, BASIC: %s", name, open ? "answers" : "silent");
	check(buf, answers(comp) == open);
	if (open) {
		snprintf(buf, sizeof(buf), "%s, BASIC: border kept", name);
		check(buf, border_kept(comp));
	}
	// the keyboard (#xxFE, A6 = 1) and the 128K's paging stay the machine's
	pout(comp, 0x7ffd, 0x13);
	snprintf(buf, sizeof(buf), "%s, BASIC: #7FFD written", name);
	check(buf, comp->p7FFD == 0x13);
	compDestroy(comp);
}

int main(void) {
	printf("== decode\n");
	board("ScorpionTP", MEM_256K, IDE_SMUC, "Scorpion Turbo+, SMUC");
	board("ScorpionTP", MEM_256K, IDE_SMUC2, "Scorpion Turbo+, SMUC 2.x");
	board("Pentagon", MEM_128K, IDE_SMUC, "Pentagon, SMUC");
	board("Pentagon", MEM_128K, IDE_SMUC2, "Pentagon, SMUC 2.x");
	return failures ? 1 : 0;
}

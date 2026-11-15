// Headless SMUC bench: the two boards' decode against Black_Cat's SMUC 2.1/2.2
// schematics and port table (lab specs/smuc-open-ports). The original 1.x answers
// only with TR-DOS paged; the home-made 2.x in any ROM, and reads 2.0. Both hold
// IORQGE, so a port they take never reaches the machine's own #FE, which A0 = 0 hits
// on the Pentagon and A0/A1/A5 on the Scorpion - but on the +2A/+3, whose edge has
// no such line. SYS b0 is the drives' /RESET.
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

// the version and revision registers read #3F/#57 (1.2) or, on a 2.x, #57/#17
// (2.0); and IDE-Hi keeps a byte
static int answers(Computer* comp, int v2) {
	pout(comp, 0xd8be, 0xa5);
	int hi = pin(comp, 0xd8be);
	pout(comp, 0xd8be, 0x5a);
	return (pin(comp, 0x5fba) == (v2 ? 0x57 : 0x3f)) && (pin(comp, 0x5fbe) == (v2 ? 0x17 : 0x57))
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

// SYS b0 is the drives' /RESET (SMUC 2.1 schematic, DD8 Q0 to the IDE pin 1): a 1
// keeps the sector count written, a 0 resets it to the signature's 1 (the
// register itself, read back from the device)
static void sys_reset(Computer* comp, const char* name) {
	char buf[128];
	int was = comp->flgDOS;
	set_dos(comp, 1);
	comp->ide->master->type = IDE_ATA;
	comp->ide->master->image = strdup("none");	// a drive is there; no file is read
	pout(comp, 0xffba, 0x77);		// b7 = 0: the CS0 block
	pout(comp, 0xfebe, 0xe0);		// master, LBA
	pout(comp, 0xfabe, 0x55);
	pout(comp, 0xffba, 0x77);
	snprintf(buf, sizeof(buf), "%s: SYS b0 = 1 runs", name);
	check(buf, comp->ide->master->reg.count == 0x55);
	pout(comp, 0xffba, 0x76);
	pout(comp, 0xffba, 0x77);
	snprintf(buf, sizeof(buf), "%s: SYS b0 = 0 resets", name);
	check(buf, comp->ide->master->reg.count == 0x01);
	// EXECUTE DEVICE DIAGNOSTIC given to an absent slave: both run it, device 0 is
	// selected and shows its signature
	pout(comp, 0xfabe, 0x55);
	pout(comp, 0xfebe, 0xf0);		// the slave
	pout(comp, 0xffbe, 0x90);
	snprintf(buf, sizeof(buf), "%s: diagnostic ends on the master", name);
	check(buf, (comp->ide->curDev == comp->ide->master) && (comp->ide->master->reg.count == 0x01));
	free(comp->ide->master->image);
	comp->ide->master->image = NULL;
	set_dos(comp, was);
}

static void board(const char* hw, int ram, int ide, const char* name) {
	Computer* comp = machine(hw, ram, ide);
	int open = (ide == IDE_SMUC2);
	char buf[128];
	set_dos(comp, 1);
	snprintf(buf, sizeof(buf), "%s, TR-DOS: answers", name);
	check(buf, answers(comp, open));
	snprintf(buf, sizeof(buf), "%s, TR-DOS: border kept", name);
	check(buf, border_kept(comp));
	set_dos(comp, 0);
	snprintf(buf, sizeof(buf), "%s, BASIC: %s", name, open ? "answers" : "silent");
	check(buf, answers(comp, open) == open);
	if (open) {
		snprintf(buf, sizeof(buf), "%s, BASIC: border kept", name);
		check(buf, border_kept(comp));
	} else {
		// nothing holds IORQGE here: the write is the machine's #FE's
		pout(comp, 0x00fe, 0x05);
		pout(comp, 0xffba, 0xf2);
		snprintf(buf, sizeof(buf), "%s, BASIC: #FFBA sets the border", name);
		check(buf, comp->vid->nextbrd == 2);
	}
	// the keyboard (#xxFE, A6 = 1) and the 128K's paging stay the machine's
	keyEntry kent;
	memset(&kent, 0, sizeof(kent));
	kent.zxKey[0] = 'a';
	kbd_press(comp->keyb, &kent);
	snprintf(buf, sizeof(buf), "%s, BASIC: key A on #FDFE", name);
	check(buf, (pin(comp, 0xfdfe) & 0x1f) == 0x1e);
	kbd_release(comp->keyb, &kent);
	pout(comp, 0x7ffd, 0x13);
	snprintf(buf, sizeof(buf), "%s, BASIC: #7FFD written", name);
	check(buf, comp->p7FFD == 0x13);
	sys_reset(comp, name);
	compDestroy(comp);
}

// a card's write against the board's #FE: kept out where the bus has IORQGE (a
// NemoIDE on the Pentagon), not on the +2A/+3, whose edge has no such line
static void bus_line(const char* hw, int ram, int ide, int port, int kept, const char* name) {
	Computer* comp = machine(hw, ram, ide);
	char buf[128];
	set_dos(comp, 0);
	pout(comp, 0x00fe, 0x05);
	pout(comp, port, 0x02);
	snprintf(buf, sizeof(buf), "%s: %s", name, kept ? "border kept" : "border set");
	check(buf, comp->vid->nextbrd == (kept ? 5 : 2));
	compDestroy(comp);
}

// the 128K: IORQGE stops the ULA, not the paging latch - a NemoIDE's #0010 is a
// #7FFD as well
static void zx128_nemo(void) {
	Computer* comp = machine("ZX128", MEM_128K, IDE_NEMO);
	pout(comp, 0x00fe, 0x05);
	pout(comp, 0x0010, 0x02);
	check("128K, NemoIDE #0010: border kept, #7FFD written", (comp->vid->nextbrd == 5) && (comp->p7FFD == 0x02));
	compDestroy(comp);
}

// the 48K the same: a NemoIDE's #FFF0 is the AY's #FFFD as well
static void zx48_nemo(void) {
	Computer* comp = machine("ZX48", MEM_48K, IDE_NEMO);
	pout(comp, 0x00fe, 0x05);
	pout(comp, 0xfff0, 0x07);		// AY register 7 selected by the card's port
	pout(comp, 0xbffd, 0x3c);
	check("48K, NemoIDE #FFF0: border kept, the AY's register picked", (comp->vid->nextbrd == 5) && (pin(comp, 0xfffd) == 0x3c));
	compDestroy(comp);
}

int main(void) {
	printf("== decode\n");
	board("ScorpionTP", MEM_256K, IDE_SMUC, "Scorpion Turbo+, SMUC");
	board("ScorpionTP", MEM_256K, IDE_SMUC2, "Scorpion Turbo+, SMUC 2.x");
	board("Pentagon", MEM_128K, IDE_SMUC, "Pentagon, SMUC");
	board("Pentagon", MEM_128K, IDE_SMUC2, "Pentagon, SMUC 2.x");
	printf("== bus\n");
	bus_line("Pentagon", MEM_128K, IDE_NEMO, 0x00f0, 1, "Pentagon, NemoIDE #F0");
	bus_line("Plus3", MEM_128K, IDE_SMUC2, 0xffba, 0, "+3, SMUC 2.x #FFBA");
	zx128_nemo();
	zx48_nemo();
	return failures ? 1 : 0;
}

// Headless NMI button bench: the Beta disk's "magic" button against the Scorpion
// ZS 256 schematic (D9.1 / D59.2, lab specs/scorpion-nmi). The button is a level:
// an opcode fetch from RAM takes it into the NMI line, a fetch from ROM lets the
// line go, the Z80 takes the line's edge. A tap is held NMI_MIN_FRAMES.
//
//	nmi-bench ROMDIR

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spectrum.h"
#include "cpu/Z80/z80.h"

static int failures;
static const char* romdir;

static void check(const char* name, int ok) {
	printf("%s: %s\n", name, ok ? "ok" : "FAIL");
	if (!ok) failures++;
}

// the ROM files one after another
static int load_roms(Computer* comp, const char** names) {
	int n = 0;
	for (; *names; names++) {
		char path[4096];
		snprintf(path, sizeof(path), "%s/%s", romdir, *names);
		FILE* f = fopen(path, "rb");
		if (!f) {
			fprintf(stderr, "no rom '%s'\n", path);
			exit(1);
		}
		n += (int)fread(comp->mem->romData + n, 1, MEM_512K - n, f);
		fclose(f);
	}
	return n;
}

static const char* sco_roms[] = {"scorpion295.rom", NULL};
static const char* pen_roms[] = {"128p-0.rom", "128p-1.rom", "gluck.rom", "trdos.rom", NULL};

static Computer* machine(const char* hw, int res) {
	Computer* comp = compCreate();
	if (!compSetHardware(comp, hw)) {
		fprintf(stderr, "no %s core\n", hw);
		exit(1);
	}
	memSetSize(comp->mem, MEM_256K, MEM_256K);
	int n = load_roms(comp, strcmp(hw, "Pentagon") ? sco_roms : pen_roms);
	memSetSize(comp->mem, MEM_256K, n);
	difSetHW(comp->dif, DIF_BDI);
	compSetBaseFrq(comp, 3.5);
	xhost_time_fixed = 1759665600;
	comp_power_on(comp);
	compReset(comp, res);
	comp_kbd_release(comp);
	return comp;
}

static void run_frames(Computer* comp, int n) {
	int end = comp->vid->fcnt + n;
	while (comp->vid->fcnt < end)
		compExec(comp);
}

// NMIs taken over n frames (or until the first, stop), and how DOS stood at #0066
static int run_count(Computer* comp, int n, int stop, int* dos) {
	int end = comp->vid->fcnt + n;
	int taken = 0;
	while (comp->vid->fcnt < end) {
		int req = comp->cpu->intrq & Z80_NMI;
		compExec(comp);
		if (req && !(comp->cpu->intrq & Z80_NMI) && (comp->cpu->regPC == 0x66)) {
			taken++;
			if (dos) *dos = comp->flgDOS;
			if (stop) break;
		}
	}
	return taken;
}

// The 128 menu, which is in RAM once a frame for a few dozen T: a tap at each of
// 10 points of a frame lands, DOS on (on the Scorpion from 128: the service ROM)
static void tap_in_128(const char* hw, const char* name) {
	int landed = 0, dosok = 1, rompg = 1;
	for (int i = 0; i < 10; i++) {
		Computer* comp = machine(hw, RES_128);
		run_frames(comp, 300);
		long long until = comp->tickTotal + comp->fCount * i / 10;
		while (comp->tickTotal < until)
			compExec(comp);
		comp_irq(IRQ_NMI, comp);
		comp_irq(IRQ_NMI_UP, comp);
		int dos = 0;
		if (run_count(comp, 5, 1, &dos)) {
			landed++;
			dosok &= dos;
			if (comp->nmiRamGate)
				rompg &= (comp->mem->map[0].type == MEM_ROM) && (comp->mem->map[0].num == (2 << 6));
		}
		compDestroy(comp);
	}
	char buf[128];
	snprintf(buf, sizeof(buf), "%s, 128 menu: a tap at 10 points of a frame lands 10", name);
	check(buf, landed == 10);
	snprintf(buf, sizeof(buf), "%s, 128 menu: DOS on at #0066%s", name, strcmp(hw, "Pentagon") ? ", the service ROM" : "");
	check(buf, dosok && rompg);
}

// 48 BASIC never runs from RAM: a second's hold gets nothing, as on the board, and
// leaves no request behind
static void hold_in_48(const char* hw, const char* name) {
	Computer* comp = machine(hw, RES_48);
	run_frames(comp, 300);
	comp_irq(IRQ_NMI, comp);
	int n = run_count(comp, 50, 0, NULL);
	comp_irq(IRQ_NMI_UP, comp);
	n += run_count(comp, 10, 0, NULL);
	char buf[128];
	snprintf(buf, sizeof(buf), "%s, 48 BASIC: a second's hold, no NMI", name);
	check(buf, n == 0);
	snprintf(buf, sizeof(buf), "%s, 48 BASIC: let go, no request left", name);
	check(buf, !comp->flgNMIRQ && !comp->nmiLine);
	compDestroy(comp);
}

static void put(Computer* comp, int adr, const char* bytes, int len) {
	for (int i = 0; i < len; i++)
		comp->hw->mwr(comp, adr + i, (unsigned char)bytes[i]);
}

// A loop in RAM at #8000, the handler in ROM: held, the NMI comes again each time
// the program is back in RAM; let go, a tap is one NMI and no more
static void back_in_ram(const char* hw, const char* name) {
	Computer* comp = machine(hw, RES_128);
	run_frames(comp, 300);
	comp_irq(IRQ_NMI, comp);
	int n = 0;
	for (int i = 0; i < 3; i++) {
		// the program, whatever the handler did before
		comp->cpu->flgIFF1 = 0;
		comp->cpu->flgIFF2 = 0;
		comp->cpu->regSP = 0xbf00;
		comp->flgDOS = 0;
		comp->hw->mapMem(comp);
		put(comp, 0x8000, "\x18\xfe", 2);	// JR $
		comp->cpu->regPC = 0x8000;
		n += run_count(comp, 2, 1, NULL);
		comp->cpu->regPC = 0x0000;		// in ROM for a while: the line lets go
		compExec(comp);
	}
	comp->cpu->flgIFF1 = 0;
	comp->flgDOS = 0;
	comp->hw->mapMem(comp);
	put(comp, 0x8000, "\x18\xfe", 2);
	comp->cpu->regPC = 0x8000;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s, held: back in RAM 3 times, 3 NMIs", name);
	check(buf, n == 3);
	comp->nmiUntil = 0;
	comp_irq(IRQ_NMI_UP, comp);
	n = run_count(comp, 5, 0, NULL);
	snprintf(buf, sizeof(buf), "%s, let go: back in RAM, none", name);
	check(buf, n == 0);
	compDestroy(comp);
}

// The Scorpion's RAM at #0000 (1FFD.0): a fetch there takes the button too. With
// the handler in RAM as well the line never lets go: one NMI for a second's hold
static void ram_at_0(void) {
	Computer* comp = machine("Scorpion", RES_128);
	run_frames(comp, 300);
	comp->cpu->flgIFF1 = 0;
	comp->cpu->flgIFF2 = 0;
	comp->hw->out(comp, 0x1ffd, 0x01);
	put(comp, 0x0100, "\x18\xfe", 2);		// JR $
	put(comp, 0x0066, "\x18\xfe", 2);		// the handler: JR $ too
	comp->cpu->regPC = 0x0100;
	comp_irq(IRQ_NMI, comp);
	int n = run_count(comp, 50, 0, NULL);
	check("Scorpion, RAM at #0000: a second's hold, one NMI", n == 1);
	compDestroy(comp);
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "nmi-bench ROMDIR\n");
		return 2;
	}
	romdir = argv[1];
	printf("== hold\n");
	tap_in_128("Scorpion", "Scorpion");
	tap_in_128("Pentagon", "Pentagon");
	hold_in_48("Scorpion", "Scorpion");
	hold_in_48("Pentagon", "Pentagon");
	printf("== edge\n");
	back_in_ram("Scorpion", "Scorpion");
	back_in_ram("Pentagon", "Pentagon");
	ram_at_0();
	return failures ? 1 : 0;
}

// Headless power-on bench: what a Hard Reset leaves (lab specs/hard-reset-ram).
// Every memory the supply holds is filled with a marker, the power comes on
// (mem_power_on, div_power_on, comp_power_on, as xm_power_cycle has them), and
// no byte of the marker may be left: the RAM, the sound card's RAM, the divMMC's,
// the machine's registers, the palettes held in the chip, TSConf's sprites. The
// flags come up as at power-on, the NVRAM chip keeps what it holds.
//
//	power-bench

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spectrum.h"
#include "cpu/Z80/z80.h"

#define MARK	0x5a

static int failures;

static void check(const char* name, const char* what, int ok) {
	printf("%s: %s: %s\n", name, what, ok ? "ok" : "FAIL");
	if (!ok) failures++;
}

// bytes still holding the marker
static int marked(const unsigned char* p, size_t len) {
	int n = 0;
	for (size_t i = 0; i < len; i++)
		if (p[i] == MARK) n++;
	return n;
}

static Computer* machine(const char* hw) {
	Computer* comp = compCreate();
	if (!compSetHardware(comp, hw)) {
		fprintf(stderr, "no %s core\n", hw);
		exit(1);
	}
	compSetBaseFrq(comp, 3.5);
	xhost_time_fixed = 1759665600;
	return comp;
}

// what ran before: the marker everywhere a program could have put it
static void dirty(Computer* comp) {
	memset(comp->mem->ramData, MARK, sizeof(comp->mem->ramData));
	memset(comp->gs->mem->ramData, MARK, sizeof(comp->gs->mem->ramData));
	memset(comp->div->data + DIV_EE_SIZE, MARK, DIV_RAM_MAX);
	memset(comp->reg, MARK, sizeof(comp->reg));
	memset(comp->xreg, MARK, sizeof(comp->xreg));
	memset(comp->vid->tsconf.sfile, MARK, sizeof(comp->vid->tsconf.sfile));
	memset(comp->vid->ula->pal, MARK, sizeof(comp->vid->ula->pal));
	memset(comp->sdrv->chan, MARK, sizeof(comp->sdrv->chan));
	comp->ide->smuc.sys = MARK;
	comp->ide->smuc.nv->state = 3;
	comp->ide->smuc.nv->mem[0x10] = MARK;	// the chip's own: kept
	comp->tsconf.pwr_up = 0;
	comp->flgNMIRQ = 1;
	comp->nmiUp = 1;
	comp->nmiLine = 1;
	comp->gs->cpu->regPC = 0x1234;
}

static void power(Computer* comp) {
	mem_power_on(comp->mem, NULL, 0, 0);
	div_power_on(comp->div);
	comp_power_on(comp);
}

static void one(const char* hw) {
	Computer* comp = machine(hw);
	dirty(comp);
	power(comp);
	check(hw, "RAM", !marked(comp->mem->ramData, sizeof(comp->mem->ramData)));
	check(hw, "the sound card's RAM, its CPU from #0000",
		!marked(comp->gs->mem->ramData, sizeof(comp->gs->mem->ramData)) && (comp->gs->cpu->regPC == 0));
	check(hw, "the divMMC's RAM", !marked(comp->div->data + DIV_EE_SIZE, DIV_RAM_MAX));
	check(hw, "registers", !marked(comp->reg, sizeof(comp->reg)) && !marked((unsigned char*)comp->xreg, sizeof(comp->xreg)));
	check(hw, "palettes, sprites, Soundrive",
		!marked(comp->vid->ula->pal, sizeof(comp->vid->ula->pal))
		&& !marked(comp->vid->tsconf.sfile, sizeof(comp->vid->tsconf.sfile))
		&& !marked(comp->sdrv->chan, sizeof(comp->sdrv->chan)));
	check(hw, "PWR_UP set, no NMI held", comp->tsconf.pwr_up && !comp->flgNMIRQ && !comp->nmiUp && !comp->nmiLine);
	check(hw, "SMUC idle, its NVRAM kept", (comp->ide->smuc.sys == 0) && (comp->ide->smuc.fdd == 0xc0)
		&& (comp->ide->smuc.nv->state == 0) && (comp->ide->smuc.nv->mem[0x10] == MARK));
	compReset(comp, RES_DEFAULT);		// and a reset after it runs
	for (int i = 0; i < 1000; i++) compExec(comp);
	compDestroy(comp);
}

// a measured pattern over what the machine reaches, zero past it
static void pattern(void) {
	static const unsigned char pat[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0};
	Computer* comp = machine("Pentagon");
	memSetSize(comp->mem, MEM_128K, -1);
	dirty(comp);
	mem_power_on(comp->mem, pat, 16, 0);
	size_t ext = mem_ram_extent(comp->mem);
	int ok = 1;
	for (size_t i = 0; i < ext; i++)
		if (comp->mem->ramData[i] != pat[i & 15]) ok = 0;
	check("Pentagon", "the pattern over 128K", ok);
	ok = 1;
	for (size_t i = ext; i < sizeof(comp->mem->ramData); i++)
		if (comp->mem->ramData[i]) ok = 0;
	check("Pentagon", "zero past it", ok);
	compDestroy(comp);
}

static const char* cores[] = {"ZX48", "ZX128", "Plus2A", "Plus3", "Pentagon", "Pentagon1024SL", "Scorpion",
	"ScorpionTP", "Profi3", "Profi", "ATM2", "Phoenix", "ALF", "Baseconf", "TSConf", "ZXNext", NULL};

int main(void) {
	printf("== machines\n");
	for (const char** c = cores; *c; c++)
		one(*c);
	printf("== pattern\n");
	pattern();
	return failures ? 1 : 0;
}

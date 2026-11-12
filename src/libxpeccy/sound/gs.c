#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "gs.h"
#include "../cpu/Z80/z80.h"

// NeoGS: CKSEL1:0 of GSCFG0 -> the CPU clock (ports.inc C_24MHZ #00 .. C_10MHZ #30)
static const int ngsMHz[4] = {24, 12, 20, 10};

#define GS_UNITS_TICK(mhz)	(1000 * GS_UNITS_NS / (mhz))

// memory map

// the original card: ROM at #0000, RAM page 0 at #4000, and #8000-#FFFF from
// the page register - the ROM for 0, a 32K RAM page from 1 on
static void gs_map_original(GSound* gs) {
	int val = gs->rp0 & 0x1f;
	if (val == 0) {
		memSetBank(gs->mem, 0x80, MEM_ROM, 0, MEM_16K, NULL, NULL, NULL);
		memSetBank(gs->mem, 0xc0, MEM_ROM, 1, MEM_16K, NULL, NULL, NULL);
	} else {
		val--;
		memSetBank(gs->mem, 0x80, MEM_RAM, val << 1, MEM_16K, NULL, NULL, NULL);
		memSetBank(gs->mem, 0xc0, MEM_RAM, (val << 1) + 1, MEM_16K, NULL, NULL, NULL);
	}
}

static void gs_ro_wr(int adr, int val, void* data) {}

// NeoGS, as memmap.v: four 16K windows - page 0 at #0000, page 3 at #4000,
// #8000 and #C000 from MPAG and MPAGEX. With NOROM clear the flash is
// everywhere but #4000-#7FFF (page bits 4:0); else RAM (all 8 bits, 4M), and
// RAMRO keeps the pages with bits 6:1 clear from writes. Not modelled: the
// 2013 ports PG0-PG3 (#20-#23) and the timer with its INT ports (#0C-#0E),
// which no firmware of the card uses.
static void gs_map_neogs(GSound* gs) {
	const unsigned char pg[4] = {0, 3, gs->pg2, gs->pg3};
	int norom = gs->cfg0 & NGS_NOROM;
	int ro = (gs->cfg0 & NGS_RAMRO) && norom;
	for (int w = 0; w < 4; w++) {
		int h = pg[w];
		if (!norom && (w != 1)) {
			memSetBank(gs->mem, w << 6, MEM_ROM, h & 0x1f, MEM_16K, NULL, NULL, NULL);
		} else if (ro && !(h & 0x7e)) {
			memSetBank(gs->mem, w << 6, MEM_RAM, h, MEM_16K, memStdRd, gs_ro_wr, NULL);
		} else {
			memSetBank(gs->mem, w << 6, MEM_RAM, h, MEM_16K, NULL, NULL, NULL);
		}
	}
}

void gs_remap(GSound* gs) {
	if (gs->model == GS_NEOGS) {
		gs->tickUnits = GS_UNITS_TICK(ngsMHz[(gs->cfg0 >> 4) & 3]);
		gs_map_neogs(gs);
	} else {
		gs->tickUnits = GS_UNITS_TICK(12);
		memSetBank(gs->mem, 0x00, MEM_ROM, 0, MEM_16K, NULL, NULL, NULL);
		memSetBank(gs->mem, 0x40, MEM_RAM, 0, MEM_16K, NULL, NULL, NULL);
		gs_map_original(gs);
	}
}

static int* gs_ch(GSound* gs, int n) {
	switch (n & 7) {
		case 0: return &gs->ch1;
		case 1: return &gs->ch2;
		case 2: return &gs->ch3;
		case 3: return &gs->ch4;
		case 4: return &gs->ch5;
		case 5: return &gs->ch6;
		case 6: return &gs->ch7;
	}
	return &gs->ch8;
}

// internal MEMRQ
int gsmemrd(int adr, int m1, void* ptr) {
	GSound* gs = (GSound*)ptr;
	adr &= 0xffff;
	unsigned char res = mem_rd_fast(gs->mem, adr) & 0xff;
	// a read at #6000-#7FFF latches the byte into a channel's DAC: A9:A8,
	// A10:A8 on a NeoGS in 8 channels (ports.v snd_addr)
	if ((adr & 0xe000) == 0x6000) {
		int eight = (gs->model == GS_NEOGS) && (gs->cfg0 & NGS_8CHANS);
		*gs_ch(gs, (adr >> 8) & (eight ? 7 : 3)) = res;
		gs->lpDirty = 1;
	}
	return res;
}

void gsmemwr(int adr, int val, void* ptr) {
	GSound* gs = (GSound*)ptr;
	adr &= 0xffff;
	mem_wr_fast(gs->mem, adr, val & 0xff);
	gs->lpDirty = 1;
}

// internal IORQ

// DAMNPORT1 (#0A) and 2 (#0B), on a read and on a write: the data bit from
// bit 0 of the page at #8000, the command bit from bit 5 of a volume
static void gs_damn(GSound* gs, int port) {
	int bit;
	if (port == 0x0a) {
		bit = ((gs->model == GS_NEOGS) ? gs->pg2 : gs->rp0) & 1;
		if (bit) gs->pstate &= 0x7f; else gs->pstate |= 0x80;
	} else {
		bit = (gs->model == GS_NEOGS) ? gs->vol4b5 : ((gs->vol1 >> 5) & 1);
		if (bit) gs->pstate |= 1; else gs->pstate &= 0xfe;
	}
}

// the ports both cards have, #01-#0B
static int gs_port_rd(GSound* gs, int port) {
	switch (port) {
		case 0x01: return gs->pbb_zx;					// ZXCMD
		case 0x02: gs->pstate &= 0x7f; return gs->pb3_zx;		// ZXDATRD
		case 0x03: if (gs->model != GS_NEOGS) gs->pstate |= 0x80; break;
		case 0x04: return gs->pstate;					// ZXSTAT
		case 0x05: gs->pstate &= 0xfe; break;				// CLRCBIT
		case 0x0a:
		case 0x0b: gs_damn(gs, port); break;
	}
	return 0xff;
}

static void gs_port_wr(GSound* gs, int port, int val) {
	switch (port) {
		case 0x01: if (gs->model == GS_NEOGS) gs->led = (val & 1) ? 0 : 1; break;	// LEDCTR
		case 0x02: if (gs->model != GS_NEOGS) gs->pstate &= 0x7f; break;
		case 0x03: gs->pstate |= 0x80; gs->pb3_gs = val; break;		// ZXDATWR
		case 0x05: gs->pstate &= 0xfe; break;				// CLRCBIT
		case 0x06: gs->vol1 = val & 0x3f; break;
		case 0x07: gs->vol2 = val & 0x3f; break;
		case 0x08: gs->vol3 = val & 0x3f; break;
		case 0x09: gs->vol4 = val & 0x3f; gs->vol4b5 = (val >> 5) & 1; break;
		case 0x0a:
		case 0x0b: gs_damn(gs, port); break;
	}
}

// The ports a loop may read and still change nothing (gs_loop_at): the host's
// command and the state, and the NeoGS's settings. Any other one counts as a
// side effect, the ones that have none here as well - the SD and MP3 ports would
// have one on the card.
static int gs_port_clean(GSound* gs, int port) {
	if (gs->model != GS_NEOGS) port &= 0x0f;
	else if (port & 0xc0) return 0;
	switch (port & 0x3f) {
		case 0x01: case 0x04: return 1;
		case 0x0f: case 0x11: case 0x12: case 0x1b: return gs->model == GS_NEOGS;
	}
	return 0;
}

// The original card decodes A3:A0; the NeoGS #00-#3F (ports.v)
int gsiord(int port, void* ptr) {
	GSound* gs = (GSound*)ptr;
	if (!gs_port_clean(gs, port)) gs->lpDirty = 1;
	if (gs->model != GS_NEOGS) return gs_port_rd(gs, port & 0x0f);
	if (port & 0xc0) return 0xff;
	port &= 0x3f;
	switch (port) {
		case 0x0f: return gs->cfg0;					// GSCFG0
		case 0x11: return gs->sctrl;					// SCTRL
		// SSTAT: no card (detect and protect pulled up), the MP3 control SPI
		// idle, no data request from an MP3 chip
		case 0x12: return 0x0e;
		case 0x1b: return 0x00;						// DMA_MOD: none
	}
	// SD and MP3 SPI read #FF: nothing drives MISO
	return (port <= 0x0b) ? gs_port_rd(gs, port) : 0xff;
}

void gsiowr(int port, int val, void* ptr) {
	GSound* gs = (GSound*)ptr;
	gs->lpDirty = 1;
	val &= 0xff;
	if (gs->model != GS_NEOGS) {
		port &= 0x0f;
		if (port == 0) {
			gs->rp0 = val;
			gs_map_original(gs);
		} else {
			gs_port_wr(gs, port, val);
		}
		return;
	}
	if (port & 0xc0) return;
	port &= 0x3f;
	switch (port) {
		case 0x00:							// MPAG
			gs->pg2 = (gs->cfg0 & NGS_EXPAG) ? ((val << 1) | (val >> 7)) : (val << 1);
			if (!(gs->cfg0 & NGS_EXPAG)) gs->pg3 = (val << 1) | 1;
			gs_map_neogs(gs);
			break;
		case 0x10:							// MPAGEX
			if (!(gs->cfg0 & NGS_EXPAG)) break;
			gs->pg3 = (val << 1) | (val >> 7);
			gs_map_neogs(gs);
			break;
		case 0x16: gs->vol5 = val & 0x3f; break;
		case 0x17: gs->vol6 = val & 0x3f; break;
		case 0x18: gs->vol7 = val & 0x3f; break;
		case 0x19: gs->vol8 = val & 0x3f; break;
		case 0x0f:							// GSCFG0
			gs->cfg0 = val;
			gs_remap(gs);
			break;
		case 0x11:							// SCTRL: bit 7 sets or clears the rest
			if (val & 0x80) gs->sctrl |= val & 0x3f;
			else gs->sctrl &= ~val & 0x3f;
			break;
		default:
			if (port <= 0x0b) gs_port_wr(gs, port, val);
			break;
	}
}

int gsintrq(void* ptr) {
	return 0xff;
}

// EXTERNAL

// What a reset line sets (ports.v rst_n): the CPU, and on the NeoGS GSCFG0
// (#30: the flash mapped, 10 MHz) and SCTRL (#0B: both chip selects up, the
// MP3 control at its first speed) - the pages at #8000 keep what they had.
static void gs_reset_hw(GSound* gs) {
	cpu_reset(gs->cpu);
	gs_loop_forget(gs);
	if (gs->model == GS_NEOGS) {
		gs->cfg0 = NGS_CFG0_RESET;
		gs->sctrl = NGS_SCTRL_RESET;
		gs->led = 1;
	}
	gs_remap(gs);
}

void gs_rom_refresh(GSound* gs) {
	if (gs->model == GS_NEOGS) {
		memcpy(gs->mem->romData, gs->romNGS, NGS_ROM_SIZE);
		memSetSize(gs->mem, NGS_RAM_SIZE, NGS_ROM_SIZE);
	} else {
		memcpy(gs->mem->romData, gs->romGS, GS_ROM_SIZE);
		memSetSize(gs->mem, GS_RAM_SIZE, GS_ROM_SIZE);
	}
	gs_remap(gs);
	gs_loop_forget(gs);
}

void gs_set_model(GSound* gs, int model) {
	gs->model = (model == GS_NEOGS) ? GS_NEOGS : GS_ORIGINAL;
	gs->pg2 = 0;
	gs->pg3 = 1;
	gs->rp0 = 0;
	gs_rom_refresh(gs);
	gs_reset_hw(gs);
}

GSound* gsCreate() {
	GSound* res = (GSound*)malloc(sizeof(GSound));
	memset(res,0x00,sizeof(GSound));
	res->cpu = cpuCreate(CPU_Z80, gsmemrd, gsmemwr, gsiord, gsiowr, gsintrq, NULL, res);
	res->cpu->inten = 0;
	res->cpu->flgACK = 1;
	res->mem = memCreate();
	res->romGS = (unsigned char*)malloc(GS_ROM_SIZE);
	res->romNGS = (unsigned char*)malloc(NGS_ROM_SIZE);
	memset(res->romGS, 0xff, GS_ROM_SIZE);
	memset(res->romNGS, 0xff, NGS_ROM_SIZE);
	memSetSize(res->mem, GS_RAM_SIZE, GS_ROM_SIZE);
	res->model = GS_ORIGINAL;
	res->tickUnits = GS_UNITS_TICK(12);
	res->intDue = GS_INT_UNITS;
	res->lpHead = -1;
	memSetBank(res->mem, 0x00, MEM_ROM, 0, MEM_16K, NULL, NULL, NULL);
	memSetBank(res->mem, 0x40, MEM_RAM, 0, MEM_16K, NULL, NULL, NULL);
	memSetBank(res->mem, 0x80, MEM_RAM, 0, MEM_16K, NULL, NULL, NULL);
	memSetBank(res->mem, 0xc0, MEM_RAM, 1, MEM_16K, NULL, NULL, NULL);
	res->pstate = 0x7e;
	res->stereo = GS_12_34;
	res->sep = 75;		// the PSG's default
	res->sctrl = NGS_SCTRL_RESET;
	res->pg3 = 1;
	res->ch1 = 0xff;
	res->ch2 = 0xff;
	res->ch3 = 0xff;
	res->ch4 = 0xff;
	res->ch5 = 0xff;
	res->ch6 = 0xff;
	res->ch7 = 0xff;
	res->ch8 = 0xff;
	return res;
}

void gsDestroy(GSound* gs) {
	cpuDestroy(gs->cpu);
	memDestroy(gs->mem);
	free(gs->romGS);
	free(gs->romNGS);
	free(gs);
}

// the time before the reset is the old program's: a parked card, or one run only
// once a frame in fast mode, may owe a lot of it
void gsReset(GSound* gs) {
	if (!gs->reset) return;
	gsFlush(gs);
	gs_reset_hw(gs);
}

void gsSync(GSound* gs, int ns) {
	if (!gs->enable) return;
	gs->due += (long long)ns * GS_UNITS_NS;
}

// The card's firmware waits in HALT. Halted with nothing it would take, the
// Z80 repeats one M1 cycle of 4 T - a read at PC+1, R counting, no other
// work - so all but the last of the cycles up to the next INT, or to the end
// of the time given, are counted at once; the last runs as ever, and leaves
// everything as the cycles one by one would.
static void gs_halt_skip(GSound* gs) {
	CPU* cpu = gs->cpu;
	if (!cpu->flgHALT || cpu_int_ready(cpu)) return;
	long long h = 4LL * gs->tickUnits;
	long long n = (gs->due + h - 1) / h;		// the cycles the time given holds
	long long ni = (gs->intDue + h - 1) / h;	// the cycles to the next INT
	if (ni < n) n = ni;
	if (n < 2) return;
	n--;
	gs->lpDirty = 1;
	cpu->regR += (unsigned char)n;
	gs->due -= n * h;
	gs->intDue -= n * h;
}

void gs_loop_forget(GSound* gs) {
	gs->lpHead = -1;
	gs->lpPark = 0;
	gs->lpWait = 0;
	gs->lpBack = 0;
}

// An INT the loop's turns could take: IFF1 set at an instruction's start
// anywhere in a turn - an EI ... DI inside it as well (inten follows IFF1 here,
// so it says nothing past the head)
static int gs_int_on(GSound* gs) {
	return gs->lpEi;
}

// Turns of the loop the CPU stands at the head of, counted at once: R by the
// turns' step and the time they take - with DI, the INTs their time holds
// are requested as they would have been, with EI they stop short of the next
// one. At least part of a turn is left to run as ever, so the CPU ends where
// the turns one by one would leave it.
static void gs_loop_skip(GSound* gs) {
	CPU* cpu = gs->cpu;
	long long turn = (long long)gs->lpLen * gs->tickUnits;
	long long lim = gs->due;
	if (gs_int_on(gs) && (gs->intDue < lim)) lim = gs->intDue;
	if ((turn <= 0) || (lim <= turn)) return;
	long long k = (lim - 1) / turn;
	cpu->regR += (unsigned char)(k * gs->lpStep);
	gs->lpR = cpu->regR;			// the head's R, for the next turn's step
	gs->due -= k * turn;
	gs->intDue -= k * turn;
	if (gs->intDue <= 0) {
		gs->intDue += (-gs->intDue / GS_INT_UNITS + 1) * GS_INT_UNITS;
		cpu->intrq |= Z80_INT;
	}
}

// every register and flag as at the head, R aside
static int gs_loop_same(GSound* gs) {
	CPU* cpu = gs->cpu;
	unsigned char r = cpu->regR;
	cpu->regR = ((unsigned char*)gs->lpRegs)[(unsigned char*)&cpu->regR - (unsigned char*)cpu->regs];
	int same = !memcmp(cpu->regs, gs->lpRegs, sizeof(gs->lpRegs))
		&& !memcmp(cpu->flags, gs->lpFlags, sizeof(gs->lpFlags));
	cpu->regR = r;
	return same;
}

// A jump back, or to itself: the CPU is at a loop's head. Reached the turn before
// as well, with no side effect between (lpDirty: a write, an OUT, a port read
// with an effect, a DAC read, LD A,R, an interrupt) and every register but R as
// it was there, the turn changed nothing, and every next one does the same as
// long as nothing outside changes: the host's accesses run the CPU out first and
// forget the loop. A head that fails is looked at less often after, up to every
// 32nd. park: stop at the head (1 back) rather than count the turns, unless an
// INT the turns would take falls in the time given.
static int gs_loop_at(GSound* gs, int park) {
	CPU* cpu = gs->cpu;
	int pc = cpu->regPC;
	int found = 0;
	// turns with EI somewhere in them take an INT already asked for, DI at the head
	// or not
	int ei = gs->lpIff || cpu->flgIFF1;
	int asked = (cpu->intrq & Z80_INT) && ei;
	if (gs->lpWait > 0) {
		gs->lpWait--;
		gs->lpHead = -1;
	} else if ((pc == gs->lpHead) && !gs->lpDirty && !asked && !cpu_int_ready(cpu) && gs_loop_same(gs)) {
		gs->lpLen = gs->lpT;
		gs->lpStep = cpu->regR - gs->lpR;
		gs->lpEi = ei;
		gs->lpBack = 0;
		found = 1;
	} else {
		if (gs->lpHead >= 0) {
			gs->lpWait = gs->lpBack;
			gs->lpBack = gs->lpBack ? ((gs->lpBack < 16) ? gs->lpBack * 2 : 32) : 1;
		}
		if (gs->lpWait > 0) {
			gs->lpHead = -1;
		} else {
			gs->lpHead = pc;
			memcpy(gs->lpRegs, cpu->regs, sizeof(gs->lpRegs));
			memcpy(gs->lpFlags, cpu->flags, sizeof(gs->lpFlags));
		}
	}
	gs->lpT = 0;
	gs->lpR = cpu->regR;
	gs->lpDirty = 0;
	gs->lpIff = 0;
	if (!found) return 0;
	if (park && !(gs_int_on(gs) && (gs->due >= gs->intDue))) {
		gs->lpPark = 1;
		return 1;
	}
	gs_loop_skip(gs);
	return 0;
}

// INT comes from the card's own divider at 37.5 kHz, whatever the CPU runs at.
// park: the mix's flush, which only needs the DAC levels - a loop that changes
// nothing leaves them as they are, so the CPU waits at its head
static void gs_run(GSound* gs, int park) {
	CPU* cpu = gs->cpu;
	if (gs->lpPark) {
		if (park && !(gs_int_on(gs) && (gs->due >= gs->intDue))) return;
		gs->lpPark = 0;
		gs_loop_skip(gs);
	}
	const opCode* ldar = &cpu->tabs->ed[0x5f];	// LD A,R: a turn that reads R
	while (gs->due > 0) {
		gs_halt_skip(gs);
		if (cpu_int_ready(cpu)) gs->lpDirty = 1;
		if (cpu->flgIFF1) gs->lpIff = 1;
		int pc = cpu->regPC;
		int t = cpu_exec(cpu);
		long long u = (long long)t * gs->tickUnits;
		gs->due -= u;
		gs->intDue -= u;
		gs->lpT += t;
		if (cpu->op == ldar) gs->lpDirty = 1;
		while (gs->intDue <= 0) {
			gs->intDue += GS_INT_UNITS;
			cpu->intrq |= Z80_INT;
		}
		if ((cpu->regPC <= pc) && gs_loop_at(gs, park)) return;
	}
}

void gsFlush(GSound* gs) {
	if (!gs->enable) return;
	gs_run(gs, 0);
}

void gsFlushMix(GSound* gs) {
	if (!gs->enable) return;
	gs_run(gs, 1);
}

// A snapshot keeps less than an INT period of the time owed (szx_ext.c): a parked
// CPU's pile is run out first. Run out, it is where it would have been anyway.
void gsSettle(GSound* gs) {
	if (!gs->enable) return;
	if (gs->lpPark || (gs->due >= GS_INT_UNITS)) gs_run(gs, 0);
}

// the host's side: #B3 data, #BB command and state; the NeoGS adds #33,
// GSCTR (write): #80 resets the card, #40 is its NMI, #20 toggles its LED

int gsCheck(GSound* gs, int adr) {
	if (!gs->enable) return 0;
	if ((adr & 0xf7) != 0xb3) return 0;	// bit3 is for register selection
	return 1;
}

static int gs_ctr(GSound* gs, int adr, int data) {
	if (!gs->enable || (gs->model != GS_NEOGS) || ((adr & 0xff) != 0x33)) return 0;
	gsFlush(gs);
	if (data & 0x80) {
		gs_reset_hw(gs);
		gs->intDue = GS_INT_UNITS;
	}
	if (data & 0x40) gs->cpu->intrq |= Z80_NMI;
	if (data & 0x20) gs->led ^= 1;
	gs_loop_forget(gs);
	return 1;
}

int gsWrite(GSound* gs, int adr, int data) {
	if (gs_ctr(gs, adr, data)) return 1;
	if (!gsCheck(gs, adr)) return 0;
	gsFlush(gs);
	if (adr & 8) {
		gs->pbb_zx = data & 0xff;
		gs->pstate |= 1;
	} else {
		gs->pb3_zx = data & 0xff;
		gs->pstate |= 0x80;		// set b7,state
	}
	gs_loop_forget(gs);		// what the CPU reads has changed
	return 1;
}

int gsRead(GSound* gs, int adr, int* dptr) {
	if (!gsCheck(gs, adr)) return 0;
	gsFlush(gs);
	if (adr & 8) {
		*dptr = gs->pstate;
	} else {
		*dptr = gs->pb3_gs;
		gs->pstate &= 0x7f;
		gs_loop_forget(gs);
	}
	return 1;
}

// mix

// One channel of the original card: the sample DAC's reference is the
// volume DAC's output, both against GND, so the level is code * vol and a
// volume change moves the mid level too.
static int gs_chan_original(int ch, int vol) {
	return ch * vol;
}

// NeoGS, sound_mulacc.v: the sample signed around #80 (or as it is, with
// INV7B), times the volume
static int gs_chan_neogs(GSound* gs, int ch, int vol) {
	int s = (gs->cfg0 & NGS_INV7B) ? (signed char)ch : (ch - 0x80);
	return s * vol;
}

// The original card: 1+2 left, 3+4 right, and each side's output reaches the
// other mixer's + input (22K against 6K8 to VIRTGND, that input's gain 2):
// solved for both sides, ~0.47 of the other side against 1 of its own. The
// sum is divided by 1.47, so all four at full are no louder than they were
// without the bleed.
static sndPair gs_mix_original(GSound* gs) {
	sndPair res;
	int l = (gs_chan_original(gs->ch1, gs->vol1) + gs_chan_original(gs->ch2, gs->vol2)) / 2;
	int r = (gs_chan_original(gs->ch3, gs->vol3) + gs_chan_original(gs->ch4, gs->vol4)) / 2;
	res.left = (l * 100 + r * 47) / 147;
	res.right = (r * 100 + l * 47) / 147;
	return res;
}

// NeoGS, sound_main.v: 8 channels 1,2,5,6 left and 3,4,7,8 right; panning 4
// channels, samples 1-4 with volumes 1,2,5,6 left and 3,4,7,8 right (the
// volume read at {chanptr[1], lrptr, chanptr[0]}); plain 4 channels, 1,2
// left and 3,4 right, each summed twice. A quarter of the 16-bit sum, so 4
// channels peak where the original card's do.
static sndPair gs_mix_neogs(GSound* gs) {
	sndPair res;
	int l, r;
	if (gs->cfg0 & NGS_8CHANS) {
		l = gs_chan_neogs(gs, gs->ch1, gs->vol1) + gs_chan_neogs(gs, gs->ch2, gs->vol2)
			+ gs_chan_neogs(gs, gs->ch5, gs->vol5) + gs_chan_neogs(gs, gs->ch6, gs->vol6);
		r = gs_chan_neogs(gs, gs->ch3, gs->vol3) + gs_chan_neogs(gs, gs->ch4, gs->vol4)
			+ gs_chan_neogs(gs, gs->ch7, gs->vol7) + gs_chan_neogs(gs, gs->ch8, gs->vol8);
	} else if (gs->cfg0 & NGS_PAN4CH) {
		l = gs_chan_neogs(gs, gs->ch1, gs->vol1) + gs_chan_neogs(gs, gs->ch2, gs->vol2)
			+ gs_chan_neogs(gs, gs->ch3, gs->vol5) + gs_chan_neogs(gs, gs->ch4, gs->vol6);
		r = gs_chan_neogs(gs, gs->ch1, gs->vol3) + gs_chan_neogs(gs, gs->ch2, gs->vol4)
			+ gs_chan_neogs(gs, gs->ch3, gs->vol7) + gs_chan_neogs(gs, gs->ch4, gs->vol8);
	} else {
		l = 2 * (gs_chan_neogs(gs, gs->ch1, gs->vol1) + gs_chan_neogs(gs, gs->ch2, gs->vol2));
		r = 2 * (gs_chan_neogs(gs, gs->ch3, gs->vol3) + gs_chan_neogs(gs, gs->ch4, gs->vol4));
	}
	res.left = l / 4;
	res.right = r / 4;
	return res;
}

// What reaches the ears is a listener's choice as the PSG's is: each side
// takes some of the other, the sum kept. 100 leaves the card's own output,
// mono is 0.
sndPair gsVolume(GSound* gs) {
	sndPair res = {0, 0};
	if (!gs->enable) return res;
	res = (gs->model == GS_NEOGS) ? gs_mix_neogs(gs) : gs_mix_original(gs);
	int sep = (gs->stereo == GS_MONO) ? 0 : (gs->sep < 0) ? 0 : (gs->sep > 100) ? 100 : gs->sep;
	if (sep < 100) {
		int l = res.left;
		res.left = (l * (100 + sep) + res.right * (100 - sep)) / 200;
		res.right = (res.right * (100 + sep) + l * (100 - sep)) / 200;
	}
	return res;
}

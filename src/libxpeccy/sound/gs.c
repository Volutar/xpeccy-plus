#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "gs.h"
#include "../cpu/Z80/z80.h"

// NeoGS: CKSEL1:0 of GSCFG0 -> the CPU clock (ports.inc C_24MHZ #00 .. C_10MHZ #30)
static const int ngsMHz[4] = {24, 12, 20, 10};

#define GS_UNITS_TICK(mhz)	(1000 * GS_UNITS_NS / (mhz))

// memory map

// the classic card: ROM at #0000, RAM page 0 at #4000, and #8000-#FFFF from
// the page register - the ROM for 0, a 32K RAM page from 1 on
static void gs_map_classic(GSound* gs) {
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

// NeoGS, as memmap.v: four 16K windows, each an 8-bit page. With NOROM clear
// the flash is everywhere but #4000-#7FFF (page bits 4:0); else RAM (bits
// 6:0, 2M), and RAMRO keeps RAM pages 0 and 1 from writes.
static void gs_map_neogs(GSound* gs) {
	const unsigned char pg[4] = {0, 3, gs->pg2, gs->pg3};
	int norom = gs->cfg0 & NGS_NOROM;
	int ro = (gs->cfg0 & NGS_RAMRO) && norom;
	for (int w = 0; w < 4; w++) {
		int h = pg[w];
		if (!norom && (w != 1)) {
			memSetBank(gs->mem, w << 6, MEM_ROM, h & 0x1f, MEM_16K, NULL, NULL, NULL);
		} else if (ro && !(h & 0x7e)) {
			memSetBank(gs->mem, w << 6, MEM_RAM, h & 0x7f, MEM_16K, memStdRd, gs_ro_wr, NULL);
		} else {
			memSetBank(gs->mem, w << 6, MEM_RAM, h & 0x7f, MEM_16K, NULL, NULL, NULL);
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
		gs_map_classic(gs);
	}
}

// internal MEMRQ
int gsmemrd(int adr, int m1, void* ptr) {
	GSound* gs = (GSound*)ptr;
	adr &= 0xffff;
	unsigned char res = mem_rd_fast(gs->mem, adr) & 0xff;
	if ((adr & 0xe000) != 0x6000) return res;
	// a read at #6000-#7FFF latches the byte into a channel's DAC
	if (gs->model == GS_NEOGS) {
		// 4 channels: A9:A8, and 5-8 take a copy; 8 channels: A10:A8
		int* ch[8] = {&gs->ch1, &gs->ch2, &gs->ch3, &gs->ch4, &gs->ch5, &gs->ch6, &gs->ch7, &gs->ch8};
		if (gs->cfg0 & NGS_8CHANS) {
			*ch[(adr >> 8) & 7] = res;
		} else {
			*ch[(adr >> 8) & 3] = res;
			*ch[((adr >> 8) & 3) + 4] = res;
		}
		return res;
	}
	switch (adr & 0x0300) {
		case 0x0000: gs->ch1 = res; break;
		case 0x0100: gs->ch2 = res; break;
		case 0x0200: gs->ch3 = res; break;
		case 0x0300: gs->ch4 = res; break;
	}
	return res;
}

void gsmemwr(int adr, int val, void* ptr) {
	GSound* gs = (GSound*)ptr;
	adr &= 0xffff;
	mem_wr_fast(gs->mem, adr, val & 0xff);
}

// internal IORQ, NeoGS: ports.v, $00-$3F decoded

static int ngs_iord(GSound* gs, int port) {
	if (port & 0xc0) return 0xff;
	switch (port & 0x3f) {
		case 0x01: return gs->pbb_zx;				// ZXCMD
		case 0x02: gs->pstate &= 0x7f; return gs->pb3_zx;	// ZXDATRD
		case 0x04: return gs->pstate;				// ZXSTAT
		case 0x05: gs->pstate &= 0xfe; break;			// CLRCBIT
		case 0x0a: if (gs->pg2 & 1) gs->pstate &= 0x7f; else gs->pstate |= 0x80; break;
		case 0x0b: if (gs->vol4b5) gs->pstate |= 1; else gs->pstate &= 0xfe; break;
		case 0x0f: return gs->cfg0;				// GSCFG0
		case 0x11: return gs->sctrl;				// SCTRL
		// SSTAT: no card (detect and protect pulled up), the MP3 control SPI
		// idle, no data request from an MP3 chip
		case 0x12: return 0x0e;
		case 0x1b: return 0x00;					// DMA_MOD: none
	}
	return 0xff;	// SD and MP3 SPI read #FF: nothing drives MISO
}

static void ngs_iowr(GSound* gs, int port, int val) {
	if (port & 0xc0) return;
	val &= 0xff;
	switch (port & 0x3f) {
		case 0x00:						// MPAG
			gs->pg2 = (gs->cfg0 & NGS_EXPAG) ? ((val << 1) | (val >> 7)) : (val << 1);
			if (!(gs->cfg0 & NGS_EXPAG)) gs->pg3 = (val << 1) | 1;
			gs_map_neogs(gs);
			break;
		case 0x10:						// MPAGEX
			if (!(gs->cfg0 & NGS_EXPAG)) break;
			gs->pg3 = (val << 1) | (val >> 7);
			gs_map_neogs(gs);
			break;
		case 0x01: gs->led = (val & 1) ? 0 : 1; break;		// LEDCTR
		case 0x03: gs->pstate |= 0x80; gs->pb3_gs = val; break;	// ZXDATWR
		case 0x05: gs->pstate &= 0xfe; break;			// CLRCBIT
		case 0x06: gs->vol1 = val & 0x3f; break;
		case 0x07: gs->vol2 = val & 0x3f; break;
		case 0x08: gs->vol3 = val & 0x3f; break;
		case 0x09: gs->vol4 = val & 0x3f; gs->vol4b5 = (val >> 5) & 1; break;
		case 0x16: gs->vol5 = val & 0x3f; break;
		case 0x17: gs->vol6 = val & 0x3f; break;
		case 0x18: gs->vol7 = val & 0x3f; break;
		case 0x19: gs->vol8 = val & 0x3f; break;
		case 0x0a: if (gs->pg2 & 1) gs->pstate &= 0x7f; else gs->pstate |= 0x80; break;
		case 0x0b: if (gs->vol4b5) gs->pstate |= 1; else gs->pstate &= 0xfe; break;
		case 0x0f:						// GSCFG0
			gs->cfg0 = val;
			gs_remap(gs);
			break;
		case 0x11:						// SCTRL: bit 7 sets or clears the rest
			if (val & 0x80) gs->sctrl |= val & 0x3f;
			else gs->sctrl &= ~val & 0x3f;
			break;
	}
}

// internal IORQ
int gsiord(int port,void* ptr) {
	GSound* gs = (GSound*)ptr;
	if (gs->model == GS_NEOGS) return ngs_iord(gs, port & 0xff);
	int res = 0xff;
	port &= 0x0f;
	switch (port) {
		case 0: break;
		case 1: res = gs->pbb_zx; break;
		case 2: gs->pstate &= 0x7f; res = gs->pb3_zx; break;
		case 3: gs->pstate |= 0x80; break;
		case 4: res = gs->pstate; break;
		case 5: gs->pstate &= 0xfe; break;
		case 6: break;
		case 7: break;
		case 8: break;
		case 9: break;
		case 10: if (gs->rp0 & 0x01) gs->pstate &= 0x7f; else gs->pstate |= 0x80; break;
		case 11: if (gs->vol1 & 0x20) gs->pstate |= 1; else gs->pstate &= 0xfe; break;
	}
	return res;
}

void gsiowr(int port, int val, void* ptr) {
	GSound* gs = (GSound*)ptr;
	if (gs->model == GS_NEOGS) {
		ngs_iowr(gs, port & 0xff, val);
		return;
	}
	port &= 0x0f;
	switch (port) {
		case 0: gs->rp0 = val & 0xff;
			gs_map_classic(gs);
			break;
		case 1: break;
		case 2: gs->pstate &= 0x7f;
			break;
		case 3: gs->pstate |= 0x80;
			gs->pb3_gs = val & 0xff;
			break;
		case 4: break;
		case 5: gs->pstate &= 0xfe;
			break;
		case 6: gs->vol1 = val & 0x3f;
			break;
		case 7: gs->vol2 = val & 0x3f;
			break;
		case 8: gs->vol3 = val & 0x3f;
			break;
		case 9: gs->vol4 = val & 0x3f;
			break;
		case 10: if (gs->rp0 & 0x01)
				gs->pstate &= 0x7f;
			else
				gs->pstate |= 0x80;
			break;
		case 11: if (gs->vol1 & 0x20)
				gs->pstate |= 1;
			else
				gs->pstate &= 0xfe;
			break;
	}
}

int gsintrq(void* ptr) {
	return 0xff;
}

// EXTERNAL

// What a reset line sets: the CPU, and on the NeoGS GSCFG0 (#30: the flash
// mapped, 10 MHz) - the page registers at #8000 keep what they had.
static void gs_reset_hw(GSound* gs) {
	cpu_reset(gs->cpu);
	if (gs->model == GS_NEOGS) {
		gs->cfg0 = NGS_CFG0_RESET;
		gs->led = 1;
	}
	gs_remap(gs);
}

void gs_rom_refresh(GSound* gs) {
	if (gs->model == GS_NEOGS) {
		memcpy(gs->mem->romData, gs->romNGS, NGS_ROM_SIZE);
		memSetSize(gs->mem, -1, NGS_ROM_SIZE);
	} else {
		memcpy(gs->mem->romData, gs->romGS, GS_ROM_SIZE);
		memSetSize(gs->mem, -1, GS_ROM_SIZE);
	}
	gs_remap(gs);
}

void gs_set_model(GSound* gs, int model) {
	gs->model = (model == GS_NEOGS) ? GS_NEOGS : GS_CLASSIC;
	gs->pg2 = 0;
	gs->pg3 = 1;
	gs->rp0 = 0;
	gs->sctrl = 0;
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
	memSetSize(res->mem, MEM_2M, GS_ROM_SIZE);
	res->model = GS_CLASSIC;
	res->tickUnits = GS_UNITS_TICK(12);
	res->intDue = GS_INT_UNITS;
	memSetBank(res->mem, 0x00, MEM_ROM, 0, MEM_16K, NULL, NULL, NULL);
	memSetBank(res->mem, 0x40, MEM_RAM, 0, MEM_16K, NULL, NULL, NULL);
	memSetBank(res->mem, 0x80, MEM_RAM, 0, MEM_16K, NULL, NULL, NULL);
	memSetBank(res->mem, 0xc0, MEM_RAM, 1, MEM_16K, NULL, NULL, NULL);
	res->pstate = 0x7e;
	res->stereo = GS_12_34;
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

void gsReset(GSound* gs) {
	if (!gs->reset) return;
	gs_reset_hw(gs);
}

void gsSync(GSound* gs, int ns) {
	if (!gs->enable) return;
	gs->due += (long long)ns * GS_UNITS_NS;
}

// INT comes from the card's own divider at 37.5 kHz, whatever the CPU runs at
void gsFlush(GSound* gs) {
	if (!gs->enable) return;
	while (gs->due > 0) {
		long long u = (long long)cpu_exec(gs->cpu) * gs->tickUnits;
		gs->due -= u;
		gs->intDue -= u;
		while (gs->intDue <= 0) {
			gs->intDue += GS_INT_UNITS;
			gs->cpu->intrq |= Z80_INT;
		}
	}
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
	}
	return 1;
}

// mix

// One channel of the classic card: the sample DAC's reference is the
// volume DAC's output, both against GND, so the level is code * vol and a
// volume change moves the mid level too.
static int gs_chan_classic(int ch, int vol) {
	return ch * vol;
}

// NeoGS, sound_mulacc.v: the sample signed around #80 (or as it is, with
// INV7B), times the volume
static int gs_chan_neogs(GSound* gs, int ch, int vol) {
	int s = (gs->cfg0 & NGS_INV7B) ? (signed char)ch : (ch - 0x80);
	return s * vol;
}

// The classic card: 1+2 left, 3+4 right, and each side's output reaches the
// other mixer's + input (22K against 6K8 to VIRTGND, that input's gain 2):
// solved for both sides, ~0.47 of the other side against 1 of its own.
// Scaled back so all four at full peak where they did without it.
static sndPair gs_mix_classic(GSound* gs) {
	sndPair res;
	int c1 = gs_chan_classic(gs->ch1, gs->vol1);
	int c2 = gs_chan_classic(gs->ch2, gs->vol2);
	int c3 = gs_chan_classic(gs->ch3, gs->vol3);
	int c4 = gs_chan_classic(gs->ch4, gs->vol4);
	if (gs->stereo == GS_MONO) {
		res.left = (c1 + c2 + c3 + c4) / 4;
		res.right = res.left;
		return res;
	}
	int l = (c1 + c2) / 2;
	int r = (c3 + c4) / 2;
	res.left = (l * 100 + r * 47) / 147;
	res.right = (r * 100 + l * 47) / 147;
	return res;
}

// NeoGS, sound_main.v: 8 channels 1,2,5,6 left and 3,4,7,8 right; panning
// 4 channels, data 1-4 with volumes 1,2,5,6 left and 3,4,7,8 right; plain 4
// channels, 1,2 left and 3,4 right, each summed twice. A quarter of the
// 16-bit sum, so 4 channels peak where the classic card's do.
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
	if (gs->stereo == GS_MONO) {
		l = (l + r) / 2;
		r = l;
	}
	res.left = l / 4;
	res.right = r / 4;
	return res;
}

sndPair gsVolume(GSound* gs) {
	if (!gs->enable) {
		sndPair res = {0, 0};
		return res;
	}
	return (gs->model == GS_NEOGS) ? gs_mix_neogs(gs) : gs_mix_classic(gs);
}

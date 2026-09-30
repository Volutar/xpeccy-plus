#include "hardware.h"
#include "../xlog.h"
#include "../cpu/Z80/z80.h"

#include <stdio.h>
#include <string.h>

#define flgMEN	flag[0]		// 15AF[4] = FM_EN
#define flgVDOS	flag[1]
#define flgLINT	flag[2]		// 1 on vid->intLINE, 0 on line int ack
#define flgDINT	flag[3]		// (same for dma int)

#define p01AF	reg[7]		// move to tsconf.c (what to do with ports tab?)
#define p02AF	reg[8]
#define p03AF	reg[9]
#define p04AF	reg[10]
#define p05AF	reg[11]
//#define p21AF	reg[12]
#define regMADR	reg[16]		// 15AF[0..3] = MapAddr
#define dmaLen	reg[17]
#define dmaCnt	reg[18]
#define fmLow	reg[20]		// FMAPS: even byte waiting for its odd one
#define fddOpen	reg[21]		// FDDVirt b7: the disk ports answer outside DOS too

#define dmaSrc	xreg[0]
#define dmaDst	xreg[1]
#define frmHold	xreg[2].i	// dots of frame INT pulse held while VDOS runs

// Ram in window 0 without W0_WE (#21AF bit 1) reads but drops writes.
static void tslNoWr(int adr, int val, void* data) {}

void tslMapMem(Computer* comp) {
// bank0 maping taken from Unreal(TSConf)
	if (comp->flgVDOS) {
		memSetBank(comp->mem,0x00,MEM_RAM,0xff, MEM_16K,NULL,NULL,NULL);		// vdos on : ramFF in bank0
	} else {
		int pg = (comp->tsconf.p21af & 4) ? comp->tsconf.Page0 : (comp->tsconf.Page0 & 0xfc) | ((comp->flgROM) ? 1 : 0) | (comp->flgDOS ? 0 : 2);
		if (comp->tsconf.p21af & 8)
			memSetBank(comp->mem,0x00,MEM_RAM,pg, MEM_16K,NULL,(comp->tsconf.p21af & 2) ? NULL : tslNoWr,NULL);
		else
			memSetBank(comp->mem,0x00,MEM_ROM,pg, MEM_16K,NULL,NULL,NULL);
	}
}

// A .spg carries no palette - on hardware the loader's is still in CRAM - so seed the
// bank PALSEL points at after reset, as unreal's load_spec_colors() does. 0RRrrrGG gggBBbbb.
static const unsigned short tslSpecCols[16] = {
	0x0000, 0x0010, 0x4000, 0x4010, 0x0200, 0x0210, 0x4200, 0x4210,
	0x0000, 0x0018, 0x6000, 0x6018, 0x0300, 0x0318, 0x6300, 0x6318
};

void tslUpdatePal(Computer* comp);

void tslReset(Computer* comp) {
	unsigned char* cp = comp->vid->tsconf.cram + (0xf0 << 1);
	comp->vid->tsconf.scrPal = 0xf0;
	memset(comp->vid->tsconf.cram,0x00,0x200);
	for (int i = 0; i < 16; i++) {
		*cp++ = tslSpecCols[i] & 0xff;
		*cp++ = tslSpecCols[i] >> 8;
	}
	tslUpdatePal(comp);
	comp->flgROM = 0;
	comp->flgDOS = 0;
	comp->cmos.mode = 2;
	kbd_set_repeat(comp->keyb, 0);		// what the avr asks the ps/2 keyboard for
	comp->tsconf.p21af = 0x04;
	comp->tsconf.Page0 = 0;
	// #0FAF resets to 0 like every other TS register; white is a ROM's doing, not the machine's
	comp->vid->nextbrd = 0x00;

	comp->vid->tsconf.p00af = 0;
	comp->p01AF = 0x05;
	comp->vid->tsconf.xOffset = 0;
	comp->vid->tsconf.yOffset = 0;
	comp->vid->tsconf.p07af = 0x0f;

	comp->vid->vidPage = 5;
	comp->vid->tsconf.T0XOffset = 0;
	comp->vid->tsconf.T0YOffset = 0;
	comp->vid->tsconf.T1XOffset = 0;
	comp->vid->tsconf.T1YOffset = 0;
	comp->vid->tsconf.tconfig = 0;
	comp->vid->tsconf.intLine = 0;
	comp->vid->tsconf.intInc = 0;
	comp->vid->tsconf.hsint = 2;		// HSINT resets to 1
	comp->vid->tsconf.lat.mask = 0;
	comp->vid->intp.x = comp->vid->tsconf.hsint;
	comp->vid->intp.y = 0;
	comp->vid->inten = 1;
	comp->flgLINT = 0;		// the ack latches are not cleared anywhere else
	comp->flgDINT = 0;
	comp->flgVDOS = 0;
	comp->frmHold = 0;
	comp->flgMEN = 0;			// FMAPS off
	comp->sdc->on = 1;
	for (int i = 0; i < 4; i++)		// FDDVirt = 0
		comp->dif->fdc->flop[i]->virt = 0;
	comp->fddOpen = 0;
	memSetBank(comp->mem, 0x40, MEM_RAM, 5, MEM_16K, NULL, NULL, NULL);
	memSetBank(comp->mem, 0x80, MEM_RAM, 2, MEM_16K, NULL, NULL, NULL);
	memSetBank(comp->mem, 0xc0, MEM_RAM, 0, MEM_16K, NULL, NULL, NULL);
	tslUpdatePorts(comp->vid);
	tslMapMem(comp);
}

static const unsigned char tslCoLevs[32] = {
	0,11,21,32,42,53,64,74,
	85,95,106,117,127,138,148,159,
	170,180,191,201,212,223,233,244,
	255,255,255,255,255,255,255,255
};

static const unsigned char tsl5bLevs[32] = {
	0,8,16,24,32,41,49,57,
	65,74,82,90,98,106,115,123,
	131,139,148,156,164,172,180,189,
	197,205,213,222,230,238,246,255
};

void tslUpdatePalX(void* ptr);		// called from the video side at line start

void tslUpdatePal(Computer* comp) {
	int col;
	xColor xcol;
	for (int i = 0; i < 256; i++) {
		col = (comp->vid->tsconf.cram[(i << 1) + 1] << 8) | (comp->vid->tsconf.cram[i << 1]);
		const unsigned char* tab = (col & 0x8000) ? tsl5bLevs : tslCoLevs;
		xcol.r = tab[(col >> 10) & 0x1f];
		xcol.g = tab[(col >> 5) & 0x1f];
		xcol.b = tab[col  & 0x1f];
		vid_set_col(comp->vid, i, xcol);
	}
}

void tslUpdatePalX(void* ptr) {
	tslUpdatePal((Computer*)ptr);
}

int tslMRd(Computer* comp, int adr, int m1) {
	if (m1 && (comp->dif->type == DIF_BDI)) {
		if (comp->flgDOS && (adr >= 0x4000) && (!comp->flgVDOS)) {
			comp->flgDOS = 0;
			comp->hw->mapMem(comp);
		}
		if (!comp->flgDOS && ((adr & 0xff00) == 0x3d00) && (comp->flgROM) && ((comp->tsconf.p21af & 0x04) == 0x00)) {
			comp->flgDOS = 1;
			comp->hw->mapMem(comp);
		}
	}
	return memRd(comp->mem,adr);
}

static void tslRegWr(Computer* comp, int reg, int val);

void tslMWr(Computer* comp, int adr, int val) {
	if (comp->flgMEN && (((adr & 0xf000) >> 12) == comp->regMADR)) {
		if ((adr & 0xf00) == 0x400) {				// ts registers
			tslRegWr(comp, adr & 0xff, val);
		} else if ((adr & 0xc00) == 0x000) {			// cram, sfile: a word on its odd byte (zmaps.v)
			unsigned char* ptr = (adr & 0x200) ? comp->vid->tsconf.sfile : comp->vid->tsconf.cram;
			if (adr & 1) {
				ptr[adr & 0x1fe] = comp->fmLow;
				ptr[adr & 0x1ff] = val & 0xff;
				if (~adr & 0x200) comp->vid->tsconf.palUpd = 1;
			} else {
				comp->fmLow = val & 0xff;
			}
		}
	}
	memWr(comp->mem,adr,val);
}

// VDOS takes the INT line away (zint.v): nothing is acknowledged, what is pending waits,
// and the frame pulse stops counting until VDOS is left.
static void ts_hold_frame(Computer* comp) {
	comp->frmHold = comp->vid->intFRAME;
	if (comp->frmHold) vid_set_int_frame(comp->vid, 0);
}

static void ts_set_vdos(Computer* comp, int on) {
	if (comp->flgVDOS == !!on) return;
	comp->flgVDOS = !!on;
	if (on) {
		ts_hold_frame(comp);
	} else if (comp->frmHold) {
		vid_set_int_frame(comp->vid, comp->frmHold);
		comp->frmHold = 0;
		comp->cpu->intrq |= Z80_INT;
	}
	tslMapMem(comp);
}

// in

// The disk ports answer in DOS, or anywhere with FDDVirt b7. A virtual drive only turns
// VDOS on from DOS; outside it the drive is just not there (zports.v).
static int ts_disk_open(Computer* comp) {
	return comp->flgBDI || comp->fddOpen;
}

int tsInFF(Computer* comp, int port) {
	int res = -1;
	if (!ts_disk_open(comp)) {
		res = zx_in_float(comp, port);
	} else if (comp->dif->fdc->flp->virt) {
		if (comp->flgBDI) ts_set_vdos(comp, 1);
	} else {
		difIn(comp->dif, port, &res, 1);
	}
	return res;
}

int tsInBDI(Computer* comp, int port) {
	int res = -1;
	if (comp->flgVDOS) {
		ts_set_vdos(comp, 0);
	} else {
		res = tsInFF(comp, port);
	}
	return res;
}

int tsIn57(Computer* comp, int port) {
	int res = sdcRead(comp->sdc);
//	printf("in #57(%.4X) = %.2X\n",port,res);
	return res;
}

// the card is taken as always in and writable (zports.v, as on BaseConf)
int tsIn77(Computer* comp, int port) {
	return 0x00;
}

int tsIn1F(Computer* comp, int port) {
	return comp->fddOpen ? tsInBDI(comp, port) : zx_in_joy(comp, port);
}

// the clock at #BFF7/#DFF7: #xxF7 is shut in DOS but open again in VDOS, where the
// clock needs no #EFF7 (zports.v)
static int ts_cmos_on(Computer* comp) {
	return (!comp->flgBDI || comp->flgVDOS) && ((comp->pEFF7 & 0x80) || comp->flgBDI);
}

int tsInBFF7(Computer* comp, int port) {
	int res = 0xff;
	if (ts_cmos_on(comp)) {
		res = evo_cmos_rd(comp);
	}
	return res;
}

// out

void tsOutBDI(Computer* comp, int port, int val) {
	if (!ts_disk_open(comp)) return;
	if (comp->flgVDOS) {
		ts_set_vdos(comp, 0);
	} else {
		if (comp->dif->fdc->flp->virt) {
			if (comp->flgBDI) ts_set_vdos(comp, 1);
		} else {
			difOut(comp->dif, port, val, 1);
		}
	}
}

void tsOutFF(Computer* comp, int port, int val) {
	if (!ts_disk_open(comp)) return;
	comp->dif->fdc->flp = comp->dif->fdc->flop[val & 3];
	if (comp->dif->fdc->flp->virt) {
		if (comp->flgBDI) ts_set_vdos(comp, 1);
	} else if (comp->flgVDOS) {
		// comp->dif->fdc->fptr = comp->dif->fdc->flop[val & 3];	// out VGSys[1:0]
	} else {
		difOut(comp->dif, port, val, 1);
	}
}

void tsOutFE(Computer* comp, int port, int val) {
	comp->vid->brdcol = ((comp->vid->tsconf.p07af & 0x0f) << 4) | (val & 7);	// PalSel's own bank (video_ports.v)
	comp->vid->nextbrd = comp->vid->brdcol;
	comp->beep->lev = (val & 0x10) ? 1 : 0;
	comp->tape->levRec = (val & 0x08) ? 1 : 0;
}

/*
void tsOutFB(Computer* comp, unsigned short port, unsigned char val) {
	sdrvOut(comp->sdrv, 0xfb, val);
}
*/

void tsOut57(Computer* comp, int port, int val) {
//	printf("out #57(%.4X),%.2X\n",port,val);
	sdcWrite(comp->sdc,val);
}

void tsOut77(Computer* comp, int port, int val) {
	comp->sdc->cs = (val & 2) ? 1 : 0;	// b1: 0 selects the card; b0 is the SPI mode, not power
}

// MemConfig b7..6 (LCK128) says what #7FFD pages: 00 512K, 01 128K, 11 1024K with D5 as a
// page bit instead of LOCK, and 10 picks 128K or 512K by the opcode that wrote it (zports.v)
void tsOut7FFD(Computer* comp, int port, int val) {
	if (comp->p7FFD & 0x20) return;
	int lck = (comp->tsconf.p21af >> 6) & 3;
	int num = val & 7;
	switch (lck) {
		case 0: num |= (val & 0xc0) >> 3; break;
		case 2:				// OUT (n),A is D3: 128K; OUT (C),r is ED 41..79: 512K
			if ((comp->cpu->com ^ (comp->cpu->com >> 1)) & 0x40)
				num |= (val & 0xc0) >> 3;
			break;
		case 3: num |= ((val & 0xc0) >> 3) | (val & 0x20); break;
	}
	// 1024K leaves the lock as it was
	comp->p7FFD = (lck == 3) ? ((val & ~0x20) | (comp->p7FFD & 0x20)) : (val & 0xff);
	comp->flgROM = (val & 0x10) ? 1 : 0;
	memSetBank(comp->mem,0xc0,MEM_RAM,num, MEM_16K,NULL,NULL,NULL);
	comp->vid->vidPage = (val & 8) ? 7 : 5;	// at once, over whatever waits
	comp->vid->tsconf.lat.mask &= ~TSL_LAT_VPAGE;
	tslMapMem(comp);
}

void tsOutBFF7(Computer* comp, int port, int val) {
	if (ts_cmos_on(comp))
		evo_cmos_wr(comp, val);
}

void tsOutDFF7(Computer* comp, int port, int val) {
	if (ts_cmos_on(comp))
		evo_cmos_adr(comp, val);
}

void tsOutEFF7(Computer* comp, int port, int val) {
	if (!comp->flgBDI)
		comp->pEFF7 = val & 0xff;
}

// xxaf

int tsIn00AF(Computer* comp, int port) {
	int res = comp->tsconf.pwr_up ? 0x40 : 0x00;	// b6: PWR_UP (1st run)
	comp->tsconf.pwr_up = 0;
	res |= 3;						// 11 : 5bit VDAC
	return res;
}

// the registers video_ports.v latches at the line start wait in tsconf.lat until then
#define TS_LAT(fld, bit, v) {comp->vid->tsconf.lat.fld = (v); comp->vid->tsconf.lat.mask |= (bit);}

void tsOut00AF(Computer* comp, int port, int val) TS_LAT(vconf, TSL_LAT_VCONF, val & 0xff)
void tsOut01AF(Computer* comp, int port, int val) TS_LAT(vpage, TSL_LAT_VPAGE, val & 0xff)
void tsOut02AF(Computer* comp, int port, int val) TS_LAT(gxl, TSL_LAT_GXL, val & 0xff)
void tsOut03AF(Computer* comp, int port, int val) TS_LAT(gxh, TSL_LAT_GXH, val & 1)

void tsOut04AF(Computer* comp, int port, int val) TS_LAT(gyl, TSL_LAT_GYL, val & 0xff)
void tsOut05AF(Computer* comp, int port, int val) TS_LAT(gyh, TSL_LAT_GYH, val & 1)

void tsOut06AF(Computer* comp, int port, int val) {comp->vid->tsconf.tconfig = val & 0xff;}
void tsOut07AF(Computer* comp, int port, int val) TS_LAT(palsel, TSL_LAT_PAL, val & 0xff)
void tsOut0FAF(Computer* comp, int port, int val) {comp->vid->nextbrd = val & 0xff;}

void tsOut10AF(Computer* comp, int port, int val) {
	comp->tsconf.Page0 = val & 0xff;
	tslMapMem(comp);
}

void tsOut11AF(Computer* comp, int port, int val) {memSetBank(comp->mem,0x40,MEM_RAM,val, MEM_16K,NULL,NULL,NULL);}
void tsOut12AF(Computer* comp, int port, int val) {memSetBank(comp->mem,0x80,MEM_RAM,val, MEM_16K,NULL,NULL,NULL);}
void tsOut13AF(Computer* comp, int port, int val) {memSetBank(comp->mem,0xc0,MEM_RAM,val, MEM_16K,NULL,NULL,NULL);}

int tsIn12AF(Computer* comp, int port) {return comp->mem->map[0x80].num >> 6;}
int tsIn13AF(Computer* comp, int port) {return comp->mem->map[0xc0].num >> 6;}

void tsOut15AF(Computer* comp, int port, int val) {
	comp->flgMEN = !!(val & 0x10);		// FM_EN
	comp->regMADR = val & 0x0f;
}

void tsOut16AF(Computer* comp, int port, int val) {comp->vid->tsconf.TMPage = val & 0xff;}
void tsOut17AF(Computer* comp, int port, int val) TS_LAT(t0g, TSL_LAT_T0G, val & 0xf8)
void tsOut18AF(Computer* comp, int port, int val) TS_LAT(t1g, TSL_LAT_T1G, val & 0xf8)
void tsOut19AF(Computer* comp, int port, int val) {comp->vid->tsconf.SGPage = val & 0xf8;}

void tsOut1AAF(Computer* comp, int port, int val) {comp->dmaSrc.l = val & 0xff;}
void tsOut1BAF(Computer* comp, int port, int val) {comp->dmaSrc.h = val & 0xff;}
void tsOut1CAF(Computer* comp, int port, int val) {comp->dmaSrc.ih = val & 0xff;}
void tsOut1DAF(Computer* comp, int port, int val) {comp->dmaDst.l = val & 0xff;}
void tsOut1EAF(Computer* comp, int port, int val) {comp->dmaDst.h = val & 0xff;}
void tsOut1FAF(Computer* comp, int port, int val) {comp->dmaDst.ih = val & 0xff;}

void tsOut20AF(Computer* comp, int port, int val) {
	switch (val & 3) {
		case 0: compSetHwTurbo(comp,1); break;
		case 1: compSetHwTurbo(comp,2); break;
		case 2: compSetHwTurbo(comp,4); break;	// normal
		case 3: compSetHwTurbo(comp,4); break;	// overclock
	}
}

void tsOut21AF(Computer* comp, int port, int val) {
	comp->tsconf.p21af = val & 0xff;
	comp->p7FFD &= ~0x10;
	if (val & 1) comp->p7FFD |= 0x10;
	comp->flgROM = val & 1;
	tslMapMem(comp);
}

void tsOut22AF(Computer* comp, int port, int val) {
	// counted in dots from the start of the blanking, as ray.xb is (video_sync.v)
	comp->vid->tsconf.hsint = (val << 1);
	comp->vid->intp.x = comp->vid->tsconf.hsint;
}

void tsOut23AF(Computer* comp, int port, int val) {
	comp->vid->tsconf.ilinl = val & 0xff;
	comp->vid->intp.y = comp->vid->tsconf.intLine;
}

void tsOut24AF(Computer* comp, int port, int val) {
	comp->vid->tsconf.ilinh = val & 1;		// VSINT is 9 bits
	comp->vid->tsconf.intInc = (val >> 4) & 0x0f;
	comp->vid->intp.y = comp->vid->tsconf.intLine;
}

void tsOut26AF(Computer* comp, int port, int val) {
	comp->dmaLen = val & 0xff;
}

int tsIn27AF(Computer* comp, int port) {return 0x00;}

// DMA as dma.v runs it. Addresses count 16-bit words over 21 bits, so 4 MB wraps round.
// With S_ALGN/D_ALGN an address wraps inside its 256/512 byte block during a burst, and
// each burst starts one block further on. The transfer is done at once.

typedef struct {
	int base;	// block part, or the whole address without align
	int low;	// offset in the block the bursts start from
	int pos;	// words into the current burst
	int mask;	// block size - 1, 0 without align
} tsDmaAdr;

static void ts_dma_adr_init(tsDmaAdr* a, xreg32* r, int algn, int asz) {
	int w = ((r->ih << 13) | ((r->w & 0x3ffe) >> 1)) & 0x1fffff;
	a->mask = algn ? (asz ? 0xff : 0x7f) : 0;
	a->base = w & ~a->mask;
	a->low = w & a->mask;
	a->pos = 0;
}

static int ts_dma_adr(tsDmaAdr* a) {
	return a->mask ? ((a->base | ((a->low + a->pos) & a->mask)) & 0x1fffff) : ((a->base + a->pos) & 0x1fffff);
}

static void ts_dma_burst_end(tsDmaAdr* a) {
	if (a->mask) {
		a->base += a->mask + 1;
		a->pos = 0;
	}
}

static void ts_dma_adr_store(tsDmaAdr* a, xreg32* r) {
	int w = ts_dma_adr(a);
	r->ih = (w >> 13) & 0xff;
	r->w = (w << 1) & 0x3ffe;
}

static int ts_dma_rd(Computer* comp, int w) {
	unsigned char* p = comp->mem->ramData + (w << 1);
	return p[0] | (p[1] << 8);
}

static void ts_dma_wr(Computer* comp, int w, int val) {
	unsigned char* p = comp->mem->ramData + (w << 1);
	p[0] = val & 0xff;
	p[1] = (val >> 8) & 0xff;
}

// blitter: a source pixel that is not 0 goes over the destination (BLT1), or the two
// are added, saturating when D6 asks (BLT2); pixels are bytes with A_SZ, nibbles without
static int ts_blit(int src, int dst, int add, int sat, int asz) {
	int res = 0;
	int bits = asz ? 8 : 4;
	int m = (1 << bits) - 1;
	for (int sh = 0; sh < 16; sh += bits) {
		int s = (src >> sh) & m;
		int d = (dst >> sh) & m;
		int v;
		if (add) {
			v = s + d;
			if (v > m) v = sat ? m : (v & m);
		} else {
			v = s ? s : d;
		}
		res |= v << sh;
	}
	return res;
}

void tsOut27AF(Computer* comp, int port, int val) {
	tsDmaAdr s, d;
	int asz = (val & 0x08) ? 1 : 0;
	int dev = ((val & 0x80) >> 4) | (val & 0x07);
	int len = comp->dmaLen + 1;
	int num = comp->dmaCnt + 1;
	int data = 0;
	int w;
	ts_dma_adr_init(&s, &comp->dmaSrc, val & 0x20, asz);
	ts_dma_adr_init(&d, &comp->dmaDst, val & 0x10, asz);
	// W/R:DDEV that exist: ram, spi, ide, fill, add-blit, blit, and back, cram, sfile
	if (!((0x3e5e >> dev) & 1)) {
		xlog(XLG_HW, XLL_DEBUG, "0x27AF: unsupported src-dst: %.2X", val & 0x87);
		return;
	}
	if (dev == 0x4) {			// fill: one word read, then written all over
		data = ts_dma_rd(comp, ts_dma_adr(&s));
		s.pos++;
	}
	for (int b = 0; b < num; b++) {
		for (int i = 0; i < len; i++) {
			switch (dev) {
				case 0x1:
					ts_dma_wr(comp, ts_dma_adr(&d), ts_dma_rd(comp, ts_dma_adr(&s)));
					break;
				case 0x9:
				case 0x6:
					w = ts_dma_adr(&d);
					ts_dma_wr(comp, w, ts_blit(ts_dma_rd(comp, ts_dma_adr(&s)), ts_dma_rd(comp, w), dev == 0x6, val & 0x40, asz));
					break;
				case 0x2:
					data = sdcRead(comp->sdc) & 0xff;
					data |= (sdcRead(comp->sdc) & 0xff) << 8;
					ts_dma_wr(comp, ts_dma_adr(&d), data);
					break;
				case 0xa:
					data = ts_dma_rd(comp, ts_dma_adr(&s));
					sdcWrite(comp->sdc, data & 0xff);
					sdcWrite(comp->sdc, (data >> 8) & 0xff);
					break;
				case 0x3:
					ts_dma_wr(comp, ts_dma_adr(&d), ataRd(comp->ide->curDev, HDD_DATA));
					break;
				case 0xb:
					ataWr(comp->ide->curDev, HDD_DATA, ts_dma_rd(comp, ts_dma_adr(&s)));
					break;
				case 0x4:
					ts_dma_wr(comp, ts_dma_adr(&d), data);
					break;
				case 0xc:
				case 0xd: {
					unsigned char* p = (dev == 0xc) ? comp->vid->tsconf.cram : comp->vid->tsconf.sfile;
					data = ts_dma_rd(comp, ts_dma_adr(&s));
					w = (ts_dma_adr(&d) & 0xff) << 1;
					p[w] = data & 0xff;
					p[w + 1] = (data >> 8) & 0xff;
					break;
				}
			}
			// both addresses step on every word, whatever the device; a fill read its one
			if (dev != 0x4) s.pos++;
			d.pos++;
		}
		if (dev != 0x4) ts_dma_burst_end(&s);
		ts_dma_burst_end(&d);
	}
	if (dev == 0xc) comp->vid->tsconf.palUpd = 1;
	ts_dma_adr_store(&s, &comp->dmaSrc);
	ts_dma_adr_store(&d, &comp->dmaDst);
	if (comp->vid->inten & 4) {
		comp->vid->intDMA = 1;
		comp->hw->irq(comp, IRQ_DMA);
	}
}

void tsOut28AF(Computer* comp, int port, int val) {
	comp->dmaCnt = val & 0xff;
}

void tsOut29AF(Computer* comp, int port, int val) {
	// comp->tsconf.FDDVirt = val;
	comp->dif->fdc->flop[0]->virt = (val & 0x01) ? 1 : 0;
	comp->dif->fdc->flop[1]->virt = (val & 0x02) ? 1 : 0;
	comp->dif->fdc->flop[2]->virt = (val & 0x04) ? 1 : 0;
	comp->dif->fdc->flop[3]->virt = (val & 0x08) ? 1 : 0;
	comp->fddOpen = (val & 0x80) ? 1 : 0;
}

void tsOut2AAF(Computer* comp, int port, int val) {
	comp->vid->inten = val & 0xff;
	// masking a source drops what it already has pending, the ack latch included: a stale
	// flgLINT/flgDINT makes the next ack hand over the vector of an interrupt just masked
	if (~val & 1) vid_set_int_frame(comp->vid, 0);
	if (~val & 2) {
		comp->vid->intLINE = 0;
		comp->flgLINT = 0;
	}
	if (~val & 4) {
		comp->vid->intDMA = 0;
		comp->flgDINT = 0;
	}
}

void tsOut40AF(Computer* comp, int port, int val) TS_LAT(t0xl, TSL_LAT_T0XL, val & 0xff)
void tsOut41AF(Computer* comp, int port, int val) TS_LAT(t0xh, TSL_LAT_T0XH, val & 1)
void tsOut42AF(Computer* comp, int port, int val) {comp->vid->tsconf.t0yl = val & 0xff;}
void tsOut43AF(Computer* comp, int port, int val) {comp->vid->tsconf.t0yh = val & 1;}
void tsOut44AF(Computer* comp, int port, int val) TS_LAT(t1xl, TSL_LAT_T1XL, val & 0xff)
void tsOut45AF(Computer* comp, int port, int val) TS_LAT(t1xh, TSL_LAT_T1XH, val & 1)
void tsOut46AF(Computer* comp, int port, int val) {comp->vid->tsconf.t1yl = val & 0xff;}
void tsOut47AF(Computer* comp, int port, int val) {comp->vid->tsconf.t1yh = val & 1;}

// catch

static xPort tsPortMap[] = {
	// xxaf
	{0xffff,0x00af,2,2,2,tsIn00AF,	tsOut00AF},
	{0xffff,0x01af,2,2,2,NULL,	tsOut01AF},
	{0xffff,0x02af,2,2,2,NULL,	tsOut02AF},
	{0xffff,0x03af,2,2,2,NULL,	tsOut03AF},
	{0xffff,0x04af,2,2,2,NULL,	tsOut04AF},
	{0xffff,0x05af,2,2,2,NULL,	tsOut05AF},
	{0xffff,0x06af,2,2,2,NULL,	tsOut06AF},
	{0xffff,0x07af,2,2,2,NULL,	tsOut07AF},
	{0xffff,0x0faf,2,2,2,NULL,	tsOut0FAF},

	{0xffff,0x10af,2,2,2,NULL,	tsOut10AF},
	{0xffff,0x11af,2,2,2,NULL,	tsOut11AF},
	{0xffff,0x12af,2,2,2,tsIn12AF,	tsOut12AF},
	{0xffff,0x13af,2,2,2,tsIn13AF,	tsOut13AF},

	{0xffff,0x15af,2,2,2,NULL,	tsOut15AF},
	{0xffff,0x16af,2,2,2,NULL,	tsOut16AF},
	{0xffff,0x17af,2,2,2,NULL,	tsOut17AF},
	{0xffff,0x18af,2,2,2,NULL,	tsOut18AF},
	{0xffff,0x19af,2,2,2,NULL,	tsOut19AF},

	{0xffff,0x1aaf,2,2,2,NULL,	tsOut1AAF},
	{0xffff,0x1baf,2,2,2,NULL,	tsOut1BAF},
	{0xffff,0x1caf,2,2,2,NULL,	tsOut1CAF},
	{0xffff,0x1daf,2,2,2,NULL,	tsOut1DAF},
	{0xffff,0x1eaf,2,2,2,NULL,	tsOut1EAF},
	{0xffff,0x1faf,2,2,2,NULL,	tsOut1FAF},

	{0xffff,0x20af,2,2,2,NULL,	tsOut20AF},
	{0xffff,0x21af,2,2,2,NULL,	tsOut21AF},
	{0xffff,0x22af,2,2,2,NULL,	tsOut22AF},
	{0xffff,0x23af,2,2,2,NULL,	tsOut23AF},
	{0xffff,0x24af,2,2,2,NULL,	tsOut24AF},
	// {0xffff,0x25af,2,2,2,NULL,	NULL},		// INTVECT (obsolete)

	{0xffff,0x26af,2,2,2,NULL,	tsOut26AF},
	{0xffff,0x27af,2,2,2,tsIn27AF,	tsOut27AF},

	{0xffff,0x28af,2,2,2,NULL,	tsOut28AF},
	{0xffff,0x29af,2,2,2,NULL,	tsOut29AF},
	{0xffff,0x2aaf,2,2,2,NULL,	tsOut2AAF},
	{0xffff,0x2baf,2,2,2,NULL,	NULL},		// cache config

	{0xffff,0x40af,2,2,2,NULL,	tsOut40AF},
	{0xffff,0x41af,2,2,2,NULL,	tsOut41AF},
	{0xffff,0x42af,2,2,2,NULL,	tsOut42AF},
	{0xffff,0x43af,2,2,2,NULL,	tsOut43AF},
	{0xffff,0x44af,2,2,2,NULL,	tsOut44AF},
	{0xffff,0x45af,2,2,2,NULL,	tsOut45AF},
	{0xffff,0x46af,2,2,2,NULL,	tsOut46AF},
	{0xffff,0x47af,2,2,2,NULL,	tsOut47AF},
	// dos or not: only the joystick gives way to the disk; #xxF7 sorts itself out
	{0x00f7,0x00fe,2,2,2,xInFE,	tsOutFE},	// fe
	{0x00ff,0x0057,2,2,2,tsIn57,	tsOut57},	// 57
	{0x00ff,0x0077,2,2,2,tsIn77,	tsOut77},	// 77
	{0x00ff,0x001f,0,2,2,tsIn1F,	NULL},
//	{0x00ff,0x00fb,0,2,2,NULL,	tsOutFB},	// fb
	{0x10ff,0xeff7,2,2,2,NULL,	tsOutEFF7},	// eff7
	{0x20ff,0xdff7,2,2,2,NULL,	tsOutDFF7},	// dff7
	{0x40ff,0xbff7,2,2,2,tsInBFF7,	tsOutBFF7},	// bff7
	{0x80ff,0x7ffd,2,2,2,NULL,	tsOut7FFD},	// 7ffd
	{0xc0ff,0xbffd,2,2,2,NULL,	xOutBFFD},	// bffd
	{0xc0ff,0xfffd,2,2,2,xInFFFD,	xOutFFFD},	// fffd
	{0xffff,0xfadf,2,2,2,xInFADF,	NULL},		// fadf
	{0xffff,0xfbdf,2,2,2,xInFBDF,	NULL},		// fbdf
	{0xffff,0xffdf,2,2,2,xInFFDF,	NULL},		// ffdf
	// the disk: in DOS, or with FDDVirt b7
	{0x009f,0x001f,2,2,2,tsInBDI,	tsOutBDI},	// 1f,3f,5f,7f
	{0x00ff,0x00ff,2,2,2,tsInFF,	tsOutFF},	// ff

	{0x0000,0x0000,2,2,2,zx_in_float,	NULL},
};

// FMAPS register window: writing a byte there is the same as out to the matching #xxAF port
static void tslRegWr(Computer* comp, int reg, int val) {
	hwOut(tsPortMap, comp, (reg << 8) | 0xaf, val, 1);
}

void tslOut(Computer* comp, int port, int val) {
	zx_dev_wr(comp, port, val);
	hwOut(tsPortMap, comp, port, val, 1);
}

int tslIn(Computer* comp, int port) {
	int res = -1;
	if (zx_dev_rd(comp, port, &res)) return res;
	res = hwIn(tsPortMap, comp, port);
	return  res;
}

// irq

void ts_irq(Computer* comp, int t) {
	switch(t) {
		case IRQ_VID_INT:
			zx_irq(comp, t);
			// the pulse is 32 cpu clocks at whatever speed the cpu runs (zint.v)
			if (comp->vid->intFRAME)
				vid_set_int_frame(comp->vid, (int)((32 * comp->nsPerTickFixed + comp->vid->nsPerDotFixed - 1) / comp->vid->nsPerDotFixed));
			if (comp->vid->tsconf.intInc) {		// video_ports.v: past line 319 it wraps by 320
				int v = comp->vid->tsconf.intLine + comp->vid->tsconf.intInc;
				if ((v >> 6) == 5) v &= 0x3f;
				comp->vid->tsconf.intLine = v & 0x1ff;
				comp->vid->intp.y = comp->vid->tsconf.intLine;
			}
			if (comp->flgVDOS)
				ts_hold_frame(comp);
			break;
		case IRQ_VID_IEND:
			if (!comp->flgLINT && !comp->flgDINT) {
				comp->cpu->intrq &= ~Z80_INT;	// reset cpu int line if there is no other ints
			}
			break;
		case IRQ_VID_LINE:			// line int (tsconf) FD
			comp->flgLINT = 1;
			comp->cpu->intrq |= Z80_INT;
			comp->vid->intLINE = 0;
			break;
		case IRQ_DMA:				// dma int (tsconf) FB
			comp->flgDINT = 1;
			comp->cpu->intrq |= Z80_INT;
			comp->vid->intDMA = 0;		// reset on vector request
			break;
		case IRQ_NMI:				// the NMI line is tied off on TSConf (top.v)
			break;
		case IRQ_CPU_ACK:
			zx_irq(comp, t);
			// line and dma hold until acked; the cpu drops INT on every ack it takes
			if (comp->flgLINT || comp->flgDINT) {
				comp->cpu->flgACK = 1;
				comp->cpu->intrq |= Z80_INT;
			}
			if (comp->flgVDOS)
				comp->cpu->flgACK = 0;
			break;
		default:
			zx_irq(comp, t);
			break;
	}
}

int ts_ack(Computer* comp) {
	if (comp->vid->intFRAME) {		// frame int have highest priority
		comp->intVector = 0xff;
		// taken is gone: the rest of the pulse must not hand it over again
		vid_set_int_frame(comp->vid, 0);
	} else if (comp->flgLINT) {		// line int
		comp->flgLINT = 0;
		comp->intVector = 0xfd;
	} else if (comp->flgDINT) {		// dma int
		comp->flgDINT = 0;
		comp->intVector = 0xfb;
	}
	return comp->intVector & 0xff;
}

// keys

void xt_press(Keyboard*, keyEntry*);
void xt_release(Keyboard*, keyEntry*);

void ts_keyp(Computer* comp, keyEntry* ent) {
	zx_keyp(comp, ent);
	xt_press(comp->keyb, ent);
}

void ts_keyr(Computer* comp, keyEntry* ent) {
	zx_keyr(comp, ent);
	xt_release(comp->keyb, ent);
}

// tsconf
xPortDsc zx_port_tab_ts[] = {
	{0x7ffd, REG_BYTE, offsetof(Computer, p7FFD)},
	{0xeff7, REG_BYTE, offsetof(Computer, pEFF7)},
	{0x01af, REG_BYTE, offsetof(Computer, p01AF)},
//	{0x02af, REG_BYTE, offsetof(Computer, p02AF)},
//	{0x03af, REG_BYTE, offsetof(Computer, p03AF)},
//	{0x04af, REG_BYTE, offsetof(Computer, p04AF)},
//	{0x05af, REG_BYTE, offsetof(Computer, p05AF)},
	{0x21af, REG_BYTE, offsetof(Computer, tsconf.p21af)},
	{-1, 0, 0}
};

HardWare tsl_hw_core = {HW_TSLAB,"TSConf","ZX Evolution (TSConf)",MEM_4M,1.0,NULL,zx_port_tab_ts,
			zx_init,tslMapMem,tslOut,tslIn,tslMRd,tslMWr,ts_irq,ts_ack,tslReset,zx_sync,ts_keyp,ts_keyr,zx_vol};

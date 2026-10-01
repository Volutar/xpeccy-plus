#include "../spectrum.h"

// Profi: the old boards (v3.x, Kramis / TOO "Profi") and the new ones (v5.0x,
// Kondor). Ports 7FFD and DFFD mean the same on both; v5 adds the palette and
// the extended port map (CP/M with ROM14): clock, IDE, a second home for the
// FDC and the 8255. Ground truth is the boards' own albums and manuals, plus
// the v3.2 schematic for the turbo; UnrealSpeccy and ZXMAK2 for the rest.

#define	pFE	reg[16]		// last OUT #FE: the palette index comes from it
#define pDFFD	reg[17]

// CP/M with ROM14: the v5 controller's extended port map
static int prf_ext(Computer* comp) {
	return comp->flgCPM && comp->flgROM;
}

// A 16K bank of RAM, or nothing where a row of chips is not fitted: a board
// with 768K has three rows of four, pages #00-#2F
static void prf_ram(Computer* comp, int adr, int bank) {
	int fitted = ((bank << 14) & comp->mem->ramMask) < comp->mem->ramSize;
	memSetBank(comp->mem, adr, fitted ? MEM_RAM : MEM_EXT, bank, MEM_16K, NULL, NULL, NULL);	// MEM_EXT reads #FF
}

// Profi ROM: SYS,DOS,128,48
void prfMapMem(Computer* comp) {
	if (comp->pDFFD & 0x10) {
		prf_ram(comp, 0x00, 0);
	} else {
		memSetBank(comp->mem, 0x00, MEM_ROM, (comp->flgDOS ? 0 : 2) | (comp->flgROM ? 1 : 0), MEM_16K, NULL, NULL, NULL);
	}
	int bank = ((comp->pDFFD & 7) << 3) | (comp->p7FFD & 7);
	prf_ram(comp, 0x40, (comp->pDFFD & 0x08) ? bank : 5);
	prf_ram(comp, 0x80, ((comp->pDFFD & 0x40) && (comp->p7FFD & 8)) ? 6 : 2);
	prf_ram(comp, 0xc0, (comp->pDFFD & 0x08) ? 7 : bank);
}

// hw->init runs again on every turbo change: a debt from the old speed goes
void prf_init(Computer* comp) {
	zx_init(comp);
	kbd_set_type(comp->keyb, KBD_PROFI);
	comp->waitDebt = comp->waitPaid = 0;
}

// Turbo (v3.2 schematic). The cpu and the video take turns at a 3.5 MHz DRAM
// slot; a RAM access waits for the cpu's slot, ROM, i/o and refresh do not.
// T3 comes at the second slot edge after the request: two waits for a cycle
// that starts on a slot edge. One that starts between edges has 71 ns to get
// its request to U28 and on a real board misses it - the manual's fix for an
// unstable turbo, 200-400 pF on that path, makes sure it does - so it waits
// three. A NOP in RAM is 6 T. The video never holds the cpu.
// HLD of the VG93 drops the turbo, and the board runs 3.5 MHz with no waits.
static int prf_hld(Computer* comp) {
	Floppy* flp = comp->dif->fdc->flp;
	return flp && flp->motor;
}

// The phase is the beam's, which is where the board counts its slots from: one
// dot is one turbo tick, a slot two dots, and the paper starts on a slot edge.
// The cycle's T1 is at the dot after the ray. comp_cont_hw() has paid waitDebt.
static void prf_cont(Computer* comp, int mreq) {
	if (!mreq || (comp->hwMul < 2) || prf_hld(comp)) return;
	if (mem_get_page(comp->mem, comp->cpu->adr)->type != MEM_RAM) return;
	Video* vid = comp->vid;
	vid_unlazy(vid);
	int odd = (vid->ray.x + 1 - vid->blank.x - vid->bord.x) & 1;
	comp->cpu->t += odd ? 3 : 2;
}

// HLD up drops the turbo
void prfSync(Computer* comp, int ns) {
	comp_owe(comp, ns, (comp->hwMul > 1) && prf_hld(comp));
	zx_sync(comp, ns);
}

// out

// v5 palette: an OUT with A0 and A7 low while 80DS is on writes ~A15..A8 as
// GGGRRRBB into the entry the previous OUT #FE named, inverted
static const unsigned char prfCol3[8] = {0,36,73,109,146,182,219,255};
static const unsigned char prfCol2[4] = {0,85,170,255};

static void prfOutPal(Computer* comp, int port, int val) {
	if (!comp->flgDDP || !(comp->pDFFD & 0x80)) return;
	xColor col;
	int c = ~port >> 8;
	col.g = prfCol3[(c >> 5) & 7];
	col.r = prfCol3[(c >> 2) & 7];
	col.b = prfCol2[c & 3];
	vid_set_col(comp->vid, ~comp->pFE & 15, col);
}

// in 512x240 the border is drawn inverted
static void prf_border(Computer* comp) {
	comp->vid->nextbrd = comp->pFE & 7;
	if (comp->pDFFD & 0x80)
		comp->vid->nextbrd ^= 7;
}

// FE is decoded on A0 alone, so #7E is a border write too
void prfOutFE(Computer* comp, int port, int val) {
	xOutFE(comp, port, val);
	comp->pFE = val & 0xff;
	prf_border(comp);
}

// clock: A5 up is the address, down the data
void prfOutCMOS(Computer* comp, int port, int val) {
	cmos_wr(&comp->cmos, (port & 0x20) ? CMOS_ADR : CMOS_DATA, val);
}

void prfOutBDI(Computer* comp, int port, int val) {
	difOut(comp->dif, (port & 0x60) | 0x1f, val, 1);
}

void prfOutBDIFF(Computer* comp, int port, int val) {
	difOut(comp->dif, 0xff, val, 1);
}

// Covox on the 8255: port B is the right channel, port C the left
static void prf_covox(Computer* comp, int ch, int val) {
	if (comp->sdrv->type != SDRV_COVOX) return;
	comp->sdrv->chan[ch] = val & 0xff;
	comp->sdrv->chan[ch + 1] = val & 0xff;
}

void prfOut8255(Computer* comp, int port, int val) {
	switch (port & 0x60) {
		case 0x20: prf_covox(comp, 2, val); break;
		case 0x40: prf_covox(comp, 0, val); break;
	}
}

void prfOut7FFD(Computer* comp, int port, int val) {
	if ((~comp->pDFFD & 0x10) && (comp->p7FFD & 0x20)) return;	// 7FFD is blocked
	comp->p7FFD = val & 0xff;
	comp->flgROM = (val & 0x10) ? 1 : 0;
	comp->vid->vidPage = (val & 0x08) ? 7 : 5;
	prfMapMem(comp);
}

void prfOutDFFD(Computer* comp, int port, int val) {
	comp->pDFFD = val;
	comp->flgCPM = (val & 0x20) ? 1 : 0;
	vid_set_mode(comp->vid, (val & 0x80) ? ((comp->hw->id == HW_PROFI) ? VID_PRF_MC : VID_PRF_MONO) : VID_NORMAL);
	prf_border(comp);
	prfMapMem(comp);
}

// in

// d7 is not wired here, so it reads 1 the way it does on a spectrum - a loader
// that tests the parity of the byte rather than masking bit 6 reads it upside
// down otherwise. d5 is the EXT key and comes from the keyboard scan.
int prfInFE(Computer* comp, int port) {
	unsigned char res = kbd_rd(comp->keyb, port) | 0x80;
	res |= zx_ear(comp) ? 0x40 : 0x00;
	zx_tape_detect(comp);
	return res;
}

int prfInBDI(Computer* comp, int port) {
	int res = -1;
	difIn(comp->dif, (port & 0x60) | 0x1f, &res, 1);
	return res;
}

int prfInBDIFF(Computer* comp, int port) {
	int res = -1;
	difIn(comp->dif, 0xff, &res, 1);
	return res;
}

int prfInCMOS(Computer* comp, int port) {
	return cmos_rd(&comp->cmos, CMOS_DATA);
}

// the joystick sits on port A of the 8255
int prfIn8255(Computer* comp, int port) {
	return (port & 0x60) ? 0xff : zx_in_joy(comp, port);
}

#define PRF_COMMON \
	{0x0081,0x0000,2,2,2,NULL,	prfOutPal},	/* before FE: it takes the index FE had */ \
	{0x0001,0x00fe,2,2,2,prfInFE,	prfOutFE}, \
	{0x8002,0x7ffd,2,2,2,NULL,	prfOut7FFD}, \
	{0x2002,0xdffd,2,2,2,NULL,	prfOutDFFD}, \
	{0xe002,0xbffd,2,2,2,NULL,	xOutBFFD}, \
	{0xe002,0xfffd,2,2,2,xInFFFD,	xOutFFFD},

// dos,rom,cpm: 0/1, 2 = either
#define PRF_BASIC \
	{0x009f,0x001f,1,2,0,prfInBDI,	prfOutBDI},	/* TR-DOS: 1f,3f,5f,7f fdc */ \
	{0x00ff,0x00ff,1,2,0,prfInBDIFF,prfOutBDIFF},	/* ff fdc system */ \
	{0x0083,0x0003,0,2,0,prfIn8255,	prfOut8255},	/* BASIC: 1f,3f,5f,7f 8255 */ \
	{0xffff,0xfadf,0,2,0,xInFADF,	NULL},		/* mouse */ \
	{0xffff,0xfbdf,0,2,0,xInFBDF,	NULL}, \
	{0xffff,0xffdf,0,2,0,xInFFDF,	NULL}, \
	{0x0000,0x0000,2,2,2,zx_in_float,NULL}

// The CP/M bit takes the FDC from TR-DOS and gives it to code in RAM, so the
// CP/M rows hold with DOS in or out.

// v3: one CP/M map, whatever ROM14 says
static xPort prf3PortMap[] = {
	PRF_COMMON
	{0x009f,0x001f,2,2,1,prfInBDI,	prfOutBDI},	// 1f,3f,5f,7f fdc
	{0x00ff,0x00bf,2,2,1,prfInBDIFF,prfOutBDIFF},	// bf fdc system
	PRF_BASIC
};

// v5: CP/M with ROM14 is the extended map (IDE is in prf5In/prf5Out)
static xPort prf5PortMap[] = {
	PRF_COMMON
	{0x009f,0x0083,2,1,1,prfInBDI,	prfOutBDI},	// 83,a3,c3,e3 fdc
	{0x00ff,0x003f,2,1,1,prfInBDIFF,prfOutBDIFF},	// 3f fdc system
	{0x009f,0x009f,2,1,1,prfInCMOS,	prfOutCMOS},	// bf,ff address; 9f,df data
	{0x009f,0x0087,2,1,1,NULL,	prfOut8255},	// 87,a7,c7,e7 8255
	{0x009f,0x001f,2,0,1,prfInBDI,	prfOutBDI},	// 1f,3f,5f,7f fdc
	{0x00ff,0x00bf,2,0,1,prfInBDIFF,prfOutBDIFF},	// bf fdc system
	PRF_BASIC
};

// The devices every ZX core shares, but the IDE answers only on the v5's
// extended map and the Covox is on the 8255, not on #FB
static int prf_dev_wr(Computer* comp, int port, int val, int ide) {
	if (gsWrite(comp->gs, port, val)) return 1;
	if (!comp->flgBDI && saaWrite(comp->saa, port, val)) return 1;
	if (!comp->flgBDI && (comp->sdrv->type != SDRV_COVOX) && sdrvWrite(comp->sdrv, port, val)) return 1;
	if (ide && ideOut(comp->ide, port, val, 0)) return 1;
	return ula_wr(comp->vid->ula, port, val);
}

static int prf_dev_rd(Computer* comp, int port, int* res, int ide) {
	if (gsRead(comp->gs, port, res)) return 1;
	if (ide && ideIn(comp->ide, port, res, 0)) return 1;
	return ula_rd(comp->vid->ula, port, res);
}

void prf3Out(Computer* comp, int port, int val) {
	if (!prf_dev_wr(comp, port, val, 0))
		hwOut(prf3PortMap, comp, port, val, 1);
}

int prf3In(Computer* comp, int port) {
	int res = -1;
	return prf_dev_rd(comp, port, &res, 0) ? res : hwIn(prf3PortMap, comp, port);
}

void prf5Out(Computer* comp, int port, int val) {
	if (!prf_dev_wr(comp, port, val, prf_ext(comp)))
		hwOut(prf5PortMap, comp, port, val, 1);
}

int prf5In(Computer* comp, int port) {
	int res = -1;
	return prf_dev_rd(comp, port, &res, prf_ext(comp)) ? res : hwIn(prf5PortMap, comp, port);
}

void prfReset(Computer* comp) {
	kbd_set_type(comp->keyb, KBD_PROFI);
	comp->pFE = 0;
	prfOutDFFD(comp, 0, 0);
}

void prf_keyp(Computer* comp, keyEntry* ent) {
	kbd_press(comp->keyb, ent);
}

void prf_keyr(Computer* comp, keyEntry* ent) {
	kbd_release(comp->keyb, ent);
}

// profi
xPortDsc zx_port_tab_p[] = {
	{0x7ffd, REG_BYTE, offsetof(Computer, p7FFD)},
	{0xdffd, REG_BYTE, offsetof(Computer, pDFFD)},
	{-1, 0, 0}
};

HardWare prf3_hw_core = {HW_PROFI3,"Profi3","Profi v3",MEM_256K | MEM_512K | MEM_768K | MEM_1M,1.0,NULL,zx_port_tab_p,
			prf_init,prfMapMem,prf3Out,prf3In,stdMRd,stdMWr,zx_irq,zx_ack,prfReset,prfSync,prf_keyp,prf_keyr,zx_vol,
			NULL,prf_cont};

HardWare prf_hw_core = {HW_PROFI,"Profi","Profi v5",MEM_512K | MEM_1M,1.0,NULL,zx_port_tab_p,
			prf_init,prfMapMem,prf5Out,prf5In,stdMRd,stdMWr,zx_irq,zx_ack,prfReset,prfSync,prf_keyp,prf_keyr,zx_vol,
			NULL,prf_cont};

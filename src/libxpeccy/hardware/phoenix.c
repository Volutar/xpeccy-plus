#include "hardware.h"

// ZXM-Phoenix, revision 08.1 (07.1 plus TurboSound): the schematics, the DD22 and DD71
// sources and Caro's ZXMC2 firmware 2.8.1 in the board's ATmega

// #37 (DD71): b0-b2 which 64K of the flash, b7 its write enable - kept, the ROM stays read-only
#define p37	prt2

// DOSEN/: TR-DOS paged in, or IODOS - bit 7 of #EFF7
static int phx_dosen(Computer* comp) {
	return comp->flgDOS || (comp->pEFF7 & 0x80);
}

void phxMapMem(Computer* comp) {
	memSetBank(comp->mem, 0x40, MEM_RAM, 5, MEM_16K, NULL, NULL, NULL);
	memSetBank(comp->mem, 0x80, MEM_RAM, 2, MEM_16K, NULL, NULL, NULL);
	// DD21/DD23, DD31/DD32: 7FFD.0-2, then 1FFD.4, 7FFD.7, 1FFD.7 and 1FFD.6, the second SIMM
	int bank = (comp->p7FFD & 7) | ((comp->p1FFD & 0x10) >> 1) | ((comp->p7FFD & 0x80) >> 3) | ((comp->p1FFD & 0x80) >> 2) | (comp->p1FFD & 0x40);
	memSetBank(comp->mem, 0xc0, MEM_RAM, bank, MEM_16K, NULL, NULL, NULL);
	// RA14 = 7FFD.4 & !1FFD.1, RA15 = !1FFD.1 & (DOS/ ^ 1FFD.3): service, TR-DOS, Basic 128, Basic 48
	int page = 0;
	if (!(comp->p1FFD & 2)) {
		page = comp->flgROM ? 1 : 0;
		if (!comp->flgDOS ^ ((comp->p1FFD >> 3) & 1)) page |= 2;
	}
	// DD71: the set is the register's, but 1 in TR-DOS; a 64K image is every set
	int set = comp->flgDOS ? 1 : (comp->p37 & 7);
	// not the 48 basic for the tape traps: another page, or a set that is not TR-DOS's own
	comp->flgEXT = ((page != 3) || (((set ^ 1) << 16) & comp->mem->romMask)) ? 1 : 0;
	if (comp->p1FFD & 1)
		memSetBank(comp->mem, 0x00, MEM_RAM, 0, MEM_16K, NULL, NULL, NULL);
	else
		memSetBank(comp->mem, 0x00, MEM_ROM, (set << 2) | page, MEM_16K, NULL, NULL, NULL);
}

// hw->init runs again on every turbo change: a debt from the old speed goes
static void phx_init(Computer* comp) {
	zx_init(comp);
	comp->waitDebt = comp->waitPaid = 0;
}

// the board's reset starts the system ROM of set 0; Basic 48 and TR-DOS are short cuts into set 1, the service page is no target
void phxReset(Computer* comp) {
	if (!comp->flgROM) comp->flgDOS = 0;
	comp->p37 = comp->flgROM ? 1 : 0;
	phxMapMem(comp);
}

// a snapshot knows #7FFD alone: it gets the set that has the BASICs, not the system ROM's
static void phx_snapmap(Computer* comp) {
	comp->p37 = 1;
	phxMapMem(comp);
}

// turbo (DD50.2): the XP20 switch, hwMul, counts while DOSEN/ is high; nothing waits, cont is there for comp_owe's debt
static void phx_cont(Computer* comp, int mreq) {
}

void phxSync(Computer* comp, int ns) {
	comp_owe(comp, ns, (comp->hwMul > 1) && phx_dosen(comp));
	zx_sync(comp, ns);
}

// DD6.2: INT rises with H6M, which is H5 in turbo: 32 cpu ticks at either speed
void phx_irq(Computer* comp, int t) {
	zx_irq(comp, t);
	if ((t != IRQ_VID_INT) && (t != IRQ_RZX_INT)) return;
	Video* vid = comp->vid;
	if ((comp->hwMul > 1) && !phx_dosen(comp) && vid->intFRAME)	// zx_irq raised it
		vid_set_int_frame(vid, vid->intsize / 2);
}

// out

void phxOut1FFD(Computer* comp, int port, int val) {
	comp->p1FFD = val & 0xff;
	phxMapMem(comp);
}

void phxOut7FFD(Computer* comp, int port, int val) {
	if (comp->p7FFD & 0x20) return;
	comp->p7FFD = val & 0xff;
	comp->flgROM = (val & 0x10) ? 1 : 0;
	comp->vid->vidPage = (val & 0x08) ? 7 : 5;
	phxMapMem(comp);
}

// The multicard (ZXMC2 2.8.1, its INT1 handler at 0xEE): #xxDF, #xxEF and #xxF7

void phxOutEFF7(Computer* comp, int port, int val) {
	comp->pEFF7 = val & 0xff;
}

int phxInEFF7(Computer* comp, int port) {
	return comp->pEFF7;
}

// any of its ports with A15-A8 = 0 reads flash byte #0FFF: the firmware's version, 28
int phxInVer(Computer* comp, int port) {
	return 0x1c;
}

// the clock on Gluk's ports (0x3A6): data behind #EFF7 b7; A, B, D are the AVR's own answers, cells from #0E its EEPROM
static int phx_clock_on(Computer* comp) {
	return comp->flgGLUK && (comp->pEFF7 & 0x80);
}

void phxOutDFF7(Computer* comp, int port, int val) {
	if (comp->flgGLUK) comp->cmos.adr = val & 0xff;
}

// a write to A or B changes nothing the AVR answers: kept out of the cells cmos_rd reads the time by
void phxOutBFF7(Computer* comp, int port, int val) {
	if (phx_clock_on(comp) && (comp->cmos.adr != 0x0a) && (comp->cmos.adr != 0x0b))
		cmos_wr(&comp->cmos, CMOS_DATA, val);
}

int phxInBFF7(Computer* comp, int port) {
	if (!phx_clock_on(comp)) return zx_in_float(comp, port);
	switch (comp->cmos.adr) {
		case 0x0a: return 0x20;
		case 0x0b: return 0x02;
		case 0x0d: return 0x00;
	}
	return cmos_rd(&comp->cmos, CMOS_DATA);
}

static xPort phxPortMap[] = {
	{0x0007,0x00fe,2,2,2,xInFE,	xOutFE},
	{0xc007,0x1ffd,2,2,2,NULL,	phxOut1FFD},	// DD19: write strobes only
	{0xc007,0x7ffd,2,2,2,NULL,	phxOut7FFD},
	{0xc007,0xbffd,2,2,2,NULL,	xOutBFFD},
	{0xc007,0xfffd,2,2,2,xInFFFD,	xOutFFFD},
	{0x008f,0x000f,0,2,2,xIn1F,	NULL},		// Kempston (DD22): A7 = 0, A3-A0 = 1111, DOSEN/ high
	{0xffff,0x00df,2,2,2,phxInVer,	NULL},
	{0xffff,0x00ef,2,2,2,phxInVer,	NULL},
	{0xffff,0x00f7,2,2,2,phxInVer,	NULL},
	// the mouse (0x10A): A15-A11 and A9 high; A8 low the buttons, else A10 picks Y or X
	{0xfbff,0xfadf,2,2,2,xInFADF,	NULL},
	{0xffff,0xfbdf,2,2,2,xInFBDF,	NULL},
	{0xffff,0xffdf,2,2,2,xInFFDF,	NULL},
	{0xffff,0xeff7,2,2,2,phxInEFF7,	phxOutEFF7},
	{0xffff,0xdff7,2,2,2,NULL,	phxOutDFF7},
	{0xffff,0xbff7,2,2,2,phxInBFF7,	phxOutBFF7},
	{0x0000,0x0000,2,2,2,zx_in_float,	NULL}
};

// DD22 strobes #x7 but #F7 for DD71, which takes A6-A4: 3 the ROM set, 5 SD data, 7 SD /CS
static int phx_x7(int port) {
	if (((port & 0x0f) != 0x07) || ((port & 0xff) == 0xf7)) return 0;
	return (port >> 4) & 7;
}

void phxOut(Computer* comp, int port, int val) {
	if (comp->pEFF7 & 0x80) comp->flgBDI = 1;	// IODOS: the Beta Disk ports without its ROM
	switch (phx_x7(port)) {
		case 3:
			comp->p37 = val & 0x87;
			phxMapMem(comp);
			return;
		case 5:
			zc_out(comp->zc, 0x57, val, 0);
			return;
		case 7:
			zc_out(comp->zc, 0x77, val | ZC_POWER, 0);	// b1 the card's /CS; its supply has no switch here
			return;
	}
	if (difOut(comp->dif, port, val, comp->flgBDI)) return;
	if (ideOut(comp->ide, port, val, 0)) return;		// Nemo ahead of zx_dev_wr's, which TR-DOS shuts out
	if (zx_dev_wr(comp, port, val) == IDE_HELD) return;
	hwOut(phxPortMap, comp, port, val, 1);
}

int phxIn(Computer* comp, int port) {
	int res = -1;
	if (comp->pEFF7 & 0x80) comp->flgBDI = 1;
	switch (phx_x7(port)) {
		case 5:
			return zc_in(comp->zc, 0x57, &res, 0) ? res : 0xff;
		case 3: case 7:
			return 0xff;		// DD71 drives the bus for #57 alone
	}
	if (difIn(comp->dif, port, &res, comp->flgBDI)) return res;
	if (ideIn(comp->ide, port, &res, 0)) return res;	// ...as on the way out
	if (zx_dev_rd_bus(comp, port, &res)) return res;
	return hwIn(phxPortMap, comp, port);
}

// phoenix
xPortDsc zx_port_tab_px[] = {
	{0x7ffd, REG_BYTE, offsetof(Computer, p7FFD)},
	{0x1ffd, REG_BYTE, offsetof(Computer, p1FFD)},
	{0xeff7, REG_BYTE, offsetof(Computer, pEFF7)},
	{0x0037, REG_BYTE, offsetof(Computer, p37)},
	{-1, 0, 0}
};

// the board turbo owes per step (comp_owe): a run only without it
static const hwQuiet phx_quiet = {HWQ_ACK_ZX, hwq_no_turbo, NULL, zx_devs_sync, zx_fetch_quiet, 0};

HardWare phx_hw_core = {HW_PHOENIX,"Phoenix","ZXM Phoenix",MEM_128K | MEM_1M | MEM_2M,1.0,NULL,zx_port_tab_px,
			phx_init,phxMapMem,phxOut,phxIn,stdMRd,stdMWr,phx_irq,zx_ack,phxReset,phxSync,zx_keyp,zx_keyr,zx_vol,
			phx_snapmap,phx_cont,&phx_quiet, .contLazy = 1, .sndtake = zx_snd_take, .sndmix = zx_mix};

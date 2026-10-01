#include "../spectrum.h"

// Scorpion ZS 256: the yellow board (SC12) and the Turbo+ (green, V16, turbo
// PLD 15.3). Ground truth is the boards' schematics: github.com/romychs/YScorp
// and github.com/romychs/Scorpion256TPlus.

// Turbo+ - AY port A reads the paging back: x.x.rom1.b3.scr.b2.b1.b0
// rom1=b4,7ffd  scr=b3,7ffd  b0..2=b0..2,7ffd  b3=b4,1ffd; IOA6-7 are not wired

int scrp_ayx_rd(int adr, void* p) {
	Computer* comp = (Computer*)p;
	int res = 0xff;
	if (!(adr & 1)) {
		res = 0xc0;
		res |= comp->p7FFD & 0x0f;		// b0,1,2,scr
		res |= comp->p1FFD & 0x10;		// b3
		res |= comp->flgROM << 5;		// rom
	}
	return res;
}

// the yellow board leaves the AY ports unwired
void scrp_init(Computer* comp) {
	zx_init(comp);
	chip_set_xdev(comp->ts->chipA, NULL, NULL, comp);
}

// hw->init runs again on every turbo change: a debt from the old speed goes
void scrptp_init(Computer* comp) {
	zx_init(comp);
	chip_set_xdev(comp->ts->chipA, scrp_ayx_rd, NULL, comp);
	comp->waitDebt = 0;
}

// Turbo+ in turbo (PLD 15.3): the cpu runs off the 7 MHz dot clock and a RAM
// access waits for the phase the video leaves it - RAS_ low and, while the
// video fetches, H1 low as well: one dot in four on the screen, one in two in
// the border. The ROM is not shared and does not wait. WAIT is sampled in T2.
static void scotp_cont(Computer* comp, int mreq) {
	if (!mreq || (comp->hwMul < 2)) return;
	Video* vid = comp->vid;
	if (mem_get_page(comp->mem, comp->cpu->adr)->type != MEM_RAM) return;
	vid_unlazy(vid);			// the ray itself, not just the time
	int d = vid->ray.x - vid->bord.x + 1;
	int mask = (vid->hbrd || vid->vbrd) ? 1 : 3;
	comp->cpu->t += (-d) & mask;		// one tick of the turbo is one dot
}

// ROM page 0..3 in the 64K ROM: A15 = dos | 1FFD.1, A14 = 7FFD.4 & !1FFD.1
static int sco_rom_page(Computer* comp) {
	if (comp->p1FFD & 0x02) return 2;
	return (comp->flgDOS ? 2 : 0) | (comp->flgROM ? 1 : 0);
}

void scoMapMem(Computer* comp) {
	int rp;
	if (comp->p1FFD & 0x01) {
		memSetBank(comp->mem,0x00,MEM_RAM,0, MEM_16K, NULL, NULL, NULL);
	} else {
		rp = sco_rom_page(comp) | ((comp->prt2 & 3) << 2);	// ProfROM plane on A16-A17
		memSetBank(comp->mem,0x00,MEM_ROM,rp, MEM_16K, NULL, NULL, NULL);
	}
	rp = (comp->p7FFD & 7) | ((comp->p1FFD & 0x10) >> 1) | ((comp->p1FFD & 0xc0) >> 2);
	memSetBank(comp->mem,0x40,MEM_RAM, 5, MEM_16K, NULL, NULL, NULL);
	memSetBank(comp->mem,0x80,MEM_RAM, 2, MEM_16K, NULL, NULL, NULL);
	memSetBank(comp->mem,0xc0,MEM_RAM, rp, MEM_16K, NULL, NULL, NULL);
}

// ProfROM plane switch (its GAL, profrom.jed): a read of 0100-010F in ROM
// page 2 moves the plane by A3-A2 and the current plane, after the read.
// Only a board with a ProfROM has the GAL, and a ProfROM is bigger than 64K.
// The GAL has no reset term, but compReset() clears prt2, which other cores
// share - a machine switch must not start in a plane left over.
static const int ZSLays[4][4] = {
	{0,1,2,3},
	{3,3,3,2},
	{2,2,0,1},
	{1,0,1,0}
};

// Even M1 (scrp.wait): an opcode fetch from RAM, not ROM, gets a wait state
// when its T1 falls on an odd tick. Yellow boards have it wired, the Turbo+
// only with the older PLD 15.1. Nothing on the INT acknowledge.
int scoMRd(Computer* comp, int adr, int m1) {
	if (m1 && comp->flgEM1 && (comp->hwMul == 1) && ((comp->tickCount + comp->cpu->t) & 1)	// cpu->t is past T1,T2
			&& (mem_get_page(comp->mem, adr)->type == MEM_RAM))
		comp->cpu->t++;
	int res = stdMRd(comp, adr, m1);
	if (((adr & 0xfff0) == 0x0100) && (comp->mem->romMask > 0xffff) && !(comp->p1FFD & 0x01)
			&& (sco_rom_page(comp) == 2)) {
		comp->prt2 = ZSLays[(adr & 0x000c) >> 2][comp->prt2 & 3];
		comp->hw->mapMem(comp);
	}
	return res;
}

// NMI (the magic button) raises DOS and nothing else: ROM A14 stays 7FFD.4,
// so from 128 it lands in the service ROM, from 48 in TR-DOS.
void scoSync(Computer* comp, int ns) {
	int nmi = comp->flgNMIRQ && (cpu_get_pc(comp->cpu) > 0x3fff);
	int rom = comp->flgROM;
	zx_sync(comp, ns);
	if (nmi) {
		comp->flgROM = rom;
		scoMapMem(comp);
	}
}

// Turbo+: while INT is up the PLD drops the turbo, so an opcode run then took
// twice its turbo ticks - owed to the next bus cycle (comp_cont_hw)
void scoTpSync(Computer* comp, int ns) {
	if ((comp->hwMul > 1) && comp->vid->intFRAME)
		comp->waitDebt += (int)((((long long)ns << NS_FIXED_BITS) + comp->nsPerTickFixed / 2) / comp->nsPerTickFixed);
	scoSync(comp, ns);
}

// in

// The Kempston port is on the board: with no joystick its lines are pulled to
// 0, not left floating; d5 is grounded and d6-d7 are the VG93's DRQ and INTRQ
int scrpIn1F(Computer* comp, int port) {
	int res = (comp->joy->type == XJ_KEMPSTON) ? (joyInput(comp->joy) & 0x1f) : 0;
	if (comp->dif->type == DIF_BDI)
		res |= (comp->dif->fdc->irq ? 0x80 : 0x00) | (comp->dif->fdc->drq ? 0x40 : 0x00);
	return res;
}

// The attribute port: D46 latches the video data bus at every attribute fetch
// and gives it to IN #FF while the counters are in the screen area, FF in the
// border. The counters run 4 T (8 dots) ahead of the picture: the pixel
// pipeline.
#define SCO_FF_LEAD	8

int scrpInFF(Computer* comp, int port) {
	if (comp->fbus != FBUS_ATTR) return zx_in_float(comp, 0xff);
	Video* vid = comp->vid;
	vid_unlazy(vid);
	int x = vid->ray.x + SCO_FF_LEAD;
	int y = vid->ray.y;
	if (x >= vid->full.x) {
		x -= vid->full.x;
		y++;
	}
	x -= vid->bord.x;
	y -= vid->bord.y;
	if ((x < 0) || (x >= vid->scrn.x) || (y < 0) || (y >= vid->scrn.y)) return 0xff;
	int pix, atr;
	vid_scr_adr(0, x & ~7, y, &pix, &atr);
	return vid->mrd(MADR(vid->vidPage, atr), vid->xptr) & 0xff;
}

// Turbo+: a read of 1FFD turns the turbo off, a read of 7FFD on

int scrpIn1FFD(Computer* comp, int port) {
	compSetHwTurbo(comp, 1);
	return 0xff;
}

int scrpIn7FFD(Computer* comp, int port) {
	compSetHwTurbo(comp, 2);
	return 0xff;
}

// out

void scrpOutDD(Computer* comp, int port, int val) {
	sdrvWrite(comp->sdrv, 0xfb, val);
}

void scrpOut7FFD(Computer* comp, int port, int val) {
	if (comp->p7FFD & 0x20) return;
	comp->p7FFD = val;
	comp->flgROM = (val & 0x10) ? 1 : 0;
	comp->vid->vidPage = (val & 0x08) ? 7 : 5;
	scoMapMem(comp);
}

void scrpOut1FFD(Computer* comp, int port, int val) {
	comp->p1FFD = val;
	comp->flgEXT = (val & 2) ? 1 : 0;
	scoMapMem(comp);
}

// Yellow board: A0,A1,A5 on the decoder, A2 = 1 to enable it, A12 on the FD
// group. In TR-DOS the ports with A1 = 1 are off (SMUC lives there).
static xPort scrpPortMap[] = {
	{0x0027,0x00fe,0,2,2,xInFE,	xOutFE},
	{0xd027,0x1ffd,2,2,2,NULL,	scrpOut1FFD},	// mem
	{0xd027,0x7ffd,2,2,2,NULL,	scrpOut7FFD},
	{0xd027,0xbffd,2,2,2,NULL,	xOutBFFD},	// ay
	{0xd027,0xfffd,2,2,2,xInFFFD,	xOutFFFD},
	{0x0027,0x00dd,2,2,2,NULL,	scrpOutDD},	// printer, a covox on it
	{0x0027,0x001f,0,2,2,scrpIn1F,	NULL},		// kjoy
	{0x0027,0x00ff,0,2,2,scrpInFF,	NULL},		// attribute port
	{0x0000,0x0000,2,2,2,zx_in_float,NULL}
};

// Turbo+: neither A2 nor A12 in the decode any more
static xPort scrptpPortMap[] = {
	{0x0023,0x00fe,0,2,2,xInFE,	xOutFE},
	{0xc023,0x1ffd,2,2,2,scrpIn1FFD,scrpOut1FFD},	// mem
	{0xc023,0x7ffd,2,2,2,scrpIn7FFD,scrpOut7FFD},
	{0xc023,0xbffd,2,2,2,NULL,	xOutBFFD},	// ay
	{0xc023,0xfffd,2,2,2,xInFFFD,	xOutFFFD},
	{0x0023,0x00dd,2,2,2,NULL,	scrpOutDD},	// printer, a covox on it
	{0x0023,0x001f,0,2,2,scrpIn1F,	NULL},		// kjoy
	{0x0023,0x00ff,0,2,2,scrpInFF,	NULL},		// attribute port
	{0x0000,0x0000,2,2,2,zx_in_float,NULL}
};

static void sco_out(Computer* comp, int port, int val, xPort* map) {
	difOut(comp->dif, port, val, comp->flgBDI);
	zx_dev_wr(comp, port, val);
	hwOut(map, comp, port, val, 1);
}

// The Kempston mouse is a card on the bus and holds IORQGE, which shuts the
// board's own decoders out - TR-DOS's #FF included, which #xxDF would hit
static int sco_mouse(Computer* comp, int port, int* res) {
	if (!comp->mouse->enable) return 0;
	switch (port & 0x05ff) {		// A8, A10 and the whole low byte
		case 0x00df: *res = xInFADF(comp, port); return 1;
		case 0x01df: *res = xInFBDF(comp, port); return 1;
		case 0x05df: *res = xInFFDF(comp, port); return 1;
	}
	return 0;
}

static int sco_in(Computer* comp, int port, xPort* map) {
	int res = -1;
	if (sco_mouse(comp, port, &res)) return res;
	if (difIn(comp->dif, port, &res, comp->flgBDI)) return res;
	if (zx_dev_rd(comp, port, &res)) return res;
	return hwIn(map, comp, port);
}

void scoOut(Computer* comp, int port, int val) {sco_out(comp, port, val, scrpPortMap);}
int scoIn(Computer* comp, int port) {return sco_in(comp, port, scrpPortMap);}

// Turbo+ in turbo: the PLD's Pin13 toggles while IORQ is low and holds WAIT
// on every other dot, one wait more per i/o cycle
void scoTpOut(Computer* comp, int port, int val) {
	if (comp->hwMul > 1) comp->cpu->t++;
	sco_out(comp, port, val, scrptpPortMap);
}

int scoTpIn(Computer* comp, int port) {
	if (comp->hwMul > 1) comp->cpu->t++;
	return sco_in(comp, port, scrptpPortMap);
}

// scorp
xPortDsc sco_port_tab[] = {
	{0x7ffd, REG_BYTE, offsetof(Computer, p7FFD)},
	{0x1ffd, REG_BYTE, offsetof(Computer, p1FFD)},
	{-1, 0, 0}
};

HardWare sco_hw_core = {HW_SCORP,"Scorpion","Scorpion ZS 256",MEM_256K,1.0,NULL,sco_port_tab,
				scrp_init,scoMapMem,scoOut,scoIn,scoMRd,stdMWr,zx_irq,zx_ack,zx_reset,scoSync,zx_keyp,zx_keyr,zx_vol};

HardWare scotp_hw_core = {HW_SCORPTP,"ScorpionTP","Scorpion ZS 256 Turbo+",MEM_256K | MEM_1M,1.0,NULL,sco_port_tab,
				scrptp_init,scoMapMem,scoTpOut,scoTpIn,scoMRd,stdMWr,zx_irq,zx_ack,zx_reset,scoTpSync,zx_keyp,zx_keyr,zx_vol,
				NULL,scotp_cont};

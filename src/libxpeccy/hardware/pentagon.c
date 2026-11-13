#include "../spectrum.h"

void penMapMem(Computer* comp) {
	zx128_map_mem(comp, 0xc0);	// bits 6,7 of 0x7FFD extend the bank field
}

// the Gluk clock: on the bus while bit 7 of its #EFF7 latch is up

static int pen_gluk_on(Computer* comp) {
	return comp->flgGLUK && (comp->pEFF7 & 0x80);
}

// in

int penInBFF7(Computer* comp, int port) {
	return pen_gluk_on(comp) ? cmos_rd(&comp->cmos, CMOS_DATA) : zx_in_float(comp, port);
}

// out

void penOut7FFD(Computer* comp, int port, int val) {
	if (comp->p7FFD & 0x20) return;
	comp->flgROM = (val & 0x10) ? 1 : 0;
	comp->p7FFD = val & 0xff;
	comp->vid->vidPage = (val & 0x08) ? 7 : 5;
	penMapMem(comp);
}

void penOutEFF7(Computer* comp, int port, int val) {
	if (comp->flgGLUK)
		comp->pEFF7 = val & 0x82;
}

void penOutDFF7(Computer* comp, int port, int val) {
	if (pen_gluk_on(comp))
		cmos_wr(&comp->cmos, CMOS_ADR, val);
}

void penOutBFF7(Computer* comp, int port, int val) {
	if (pen_gluk_on(comp))
		cmos_wr(&comp->cmos, CMOS_DATA, val);
}

static xPort penPortMap[] = {
	{0x0001,0x00fe,2,2,2,xInFE,	xOutFE},
	{0x8002,0x7ffd,2,2,2,NULL,	penOut7FFD},
	{0xc002,0xbffd,2,2,2,NULL,	xOutBFFD},
	{0xc002,0xfffd,2,2,2,xInFFFD,	xOutFFFD},
	{0x00ff,0x001f,0,2,2,xIn1F,	NULL},		// joystick
	{0x05a3,0xfadf,0,2,2,xInFADF,	NULL},		// mouse
	{0x05a3,0xfbdf,0,2,2,xInFBDF,	NULL},
	{0x05a3,0xffdf,0,2,2,xInFFDF,	NULL},
	// the Gluk clock, decoded on A15..A12 and A3: after the mouse, which
	// shares some of those ports and was there first
	{0xf008,0xeff7,2,2,2,NULL,	penOutEFF7},
	{0xf008,0xdff7,2,2,2,NULL,	penOutDFF7},
	{0xf008,0xbff7,2,2,2,penInBFF7,	penOutBFF7},
	{0x0000,0x0000,2,2,2,zx_in_float,	NULL}
};

void penOut(Computer* comp, int port, int val) {
	difOut(comp->dif, port, val, comp->flgBDI);
	zx_dev_wr(comp, port, val);
	hwOut(penPortMap, comp, port, val, 1);
}

int penIn(Computer* comp, int port) {
	int res = -1;
	if (difIn(comp->dif, port, &res, comp->flgBDI)) return res;
	if (zx_dev_rd(comp, port, &res)) return res;
	res = hwIn(penPortMap, comp, port);
	return res;
}

// pent
xPortDsc zx_port_tab_a[] = {
	{0x7ffd, REG_BYTE, offsetof(Computer, p7FFD)},
	{0xeff7, REG_BYTE, offsetof(Computer, pEFF7)},	// the Gluk clock's latch
	{-1, 0, 0}
};

HardWare pnt_hw_core = {HW_PENT,"Pentagon","Pentagon",MEM_128K | MEM_512K,1.0,NULL,zx_port_tab_a,
			zx_init,penMapMem,penOut,penIn,stdMRd,stdMWr,zx_irq,zx_ack,zx_reset,zx_sync,zx_keyp,zx_keyr,zx_vol,
			NULL,NULL,&hwq_zx, .sndtake = zx_snd_take, .sndmix = zx_mix};

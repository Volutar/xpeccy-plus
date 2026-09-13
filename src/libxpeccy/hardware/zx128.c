#include "../spectrum.h"

// The Sinclair 128K, and the grey +2 with it - the +2 is the same machine in a
// different case, so it is a ROM set here, not a core. What separates it from
// the Pentagon that used to stand in for it: 0x7FFD is decoded with all of
// A15,A1 clear (mask 0xC002), so a write to 0x3FFD does not page, and the bank
// field is bits 0-2 alone - there are no bits 6,7 to extend it with.

void z128MapMem(Computer* comp) {
	zx128_map_mem(comp, 0);
}

// in

int z128InFF(Computer* comp, int port) {
	return (comp->vid->vbrd || comp->vid->hbrd) ? 0xff : comp->vid->atrbyte & 0xff;
}

// out

void z128Out7FFD(Computer* comp, int port, int val) {
	if (comp->p7FFD & 0x20) return;			// paging locked until reset
	comp->flgROM = (val & 0x10) ? 1 : 0;
	comp->p7FFD = val & 0xff;
	comp->vid->vidPage = (val & 0x08) ? 7 : 5;
	z128MapMem(comp);
}

static xPort z128PortMap[] = {
	{0x0001,0x00fe,2,2,2,xInFE,	xOutFE},
	{0xc002,0x7ffd,2,2,2,NULL,	z128Out7FFD},
	{0xc002,0xbffd,2,2,2,NULL,	xOutBFFD},
	{0xc002,0xfffd,2,2,2,xInFFFD,	xOutFFFD},
	{0x00ff,0x001f,0,2,2,xIn1F,	NULL},		// joystick
	{0x05a3,0xfadf,0,2,2,xInFADF,	NULL},		// mouse
	{0x05a3,0xfbdf,0,2,2,xInFBDF,	NULL},
	{0x05a3,0xffdf,0,2,2,xInFFDF,	NULL},
	{0x0000,0x0000,2,2,2,z128InFF,	NULL}
};

void z128Out(Computer* comp, int port, int val) {
	difOut(comp->dif, port, val, comp->flgBDI);
	zx_dev_wr(comp, port, val);
	hwOut(z128PortMap, comp, port, val, 1);
}

int z128In(Computer* comp, int port) {
	int res = -1;
	if (difIn(comp->dif, port, &res, comp->flgBDI)) return res;
	if (zx_dev_rd(comp, port, &res)) return res;
	res = hwIn(z128PortMap, comp, port);
	return res;
}

xPortDsc z128_port_tab[] = {
	{0x7ffd, REG_BYTE, offsetof(Computer, p7FFD)},
	{-1, 0, 0}
};

HardWare z128_hw_core = {HW_ZX128,"ZX128","ZX 128K",MEM_128K,1.0,NULL,z128_port_tab,
			zx_init,z128MapMem,z128Out,z128In,stdMRd,stdMWr,zx_irq,zx_ack,zx_reset,zx_sync,zx_keyp,zx_keyr,zx_vol};

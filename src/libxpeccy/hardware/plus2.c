#include "../spectrum.h"

// layout for +2a extend memory mode

unsigned char plus2Lays[4][4] = {
	{0,1,2,3},
	{4,5,6,7},
	{4,5,6,3},
	{4,7,6,3}
};

void pl2MapMem(Computer* comp) {
	if (comp->p1FFD & 1) {
		// extend mem mode
		int rp = ((comp->p1FFD & 0x06) >> 1);	// b1,2 of 1ffd
		memSetBank(comp->mem,0x00,MEM_RAM,plus2Lays[rp][0], MEM_16K, NULL, NULL, NULL);
		memSetBank(comp->mem,0x40,MEM_RAM,plus2Lays[rp][1], MEM_16K, NULL, NULL, NULL);
		memSetBank(comp->mem,0x80,MEM_RAM,plus2Lays[rp][2], MEM_16K, NULL, NULL, NULL);
		memSetBank(comp->mem,0xc0,MEM_RAM,plus2Lays[rp][3], MEM_16K, NULL, NULL, NULL);
	} else {
		// normal mem mode
		memSetBank(comp->mem,0x00,MEM_ROM,(comp->flgROM ? 1 : 0) | ((comp->p1FFD & 0x04) >> 1), MEM_16K, NULL, NULL, NULL);
		memSetBank(comp->mem,0x40,MEM_RAM,5, MEM_16K, NULL, NULL, NULL);
		memSetBank(comp->mem,0x80,MEM_RAM,2, MEM_16K, NULL, NULL, NULL);
		memSetBank(comp->mem,0xc0,MEM_RAM,comp->p7FFD & 7, MEM_16K, NULL, NULL, NULL);
	}
}

// A reset to 48K is what the menu's 48 BASIC does: ROM 3, paging locked.
// compReset() says which one it was with bit 4 of 7FFD.
void plusRes(Computer* comp) {
	int b48 = (comp->p7FFD & 0x10) ? 1 : 0;
	comp->p1FFD = b48 ? 0x04 : 0;
	comp->p7FFD = b48 ? 0x30 : 0;
	comp->flgROM = b48;
	comp->flgDOS = 0;
	comp->flgEXT = 0;
	zx_set_vmode(comp);
}

// in

// no upd765 in +2
int p2_dos_rd(Computer* comp, int port) {
	return -1;
//	int res = -1;
//	difIn(comp->dif, port, &res, 0);
//	return res;
}

// out

void p2Out1FFD(Computer* comp, int port, int val) {
	comp->p1FFD = val & 0xff;
	pl2MapMem(comp);
}

void p2_dos_wr(Computer* comp, int port, int val) {
//	difOut(comp->dif, port, val, 0);
}

void p2Out7FFD(Computer* comp, int port, int val) {
	if (comp->p7FFD & 0x20) return;
	comp->flgROM = (val & 0x10) ? 1 : 0;
	comp->p7FFD = val & 0xff;
	comp->vid->vidPage = (val & 0x08) ? 7 : 5;
	pl2MapMem(comp);
}

static xPort p2PortMap[] = {
	{0x00ff,0x001f,0,2,2,xIn1F,	NULL},		// joystick
	{0x0003,0x00fe,2,2,2,xInFE,	xOutFE},
	{0xc002,0x7ffd,2,2,2,NULL,	p2Out7FFD},
	{0xf002,0x1ffd,2,2,2,NULL,	p2Out1FFD},
	{0xe002,0x2ffd,2,2,2,p2_dos_rd,	p2_dos_wr},	// 2ffd/3ffd
	{0xc002,0xbffd,2,2,2,NULL,	xOutBFFD},
	{0xc002,0xfffd,2,2,2,xInFFFD,	xOutFFFD},
	{0x0000,0x0000,2,2,2,zx_in_float,	NULL}
};

void pl2Out(Computer* comp, int port, int val) {
	zx_dev_wr(comp, port, val);
	hwOut(p2PortMap, comp, port, val, 1);
}

int pl2In(Computer* comp, int port) {
	int res = -1;
	if (zx_dev_rd(comp, port, &res)) return res;
	res = hwIn(p2PortMap, comp, port);
	return  res;
}

xPortDsc pl2_port_tab[] = {
	{0x7ffd, REG_BYTE, offsetof(Computer, p7FFD)},
	{0x1ffd, REG_BYTE, offsetof(Computer, p1FFD)},
	{-1, 0, 0}
};

HardWare pl2_hw_core = {HW_PLUS2A,"Plus2A","Spectrum +2A",MEM_128K,1.0,NULL,pl2_port_tab,
			zx_init,pl2MapMem,pl2Out,pl2In,asicMRd,asicMWr,zx_irq,zx_ack,plusRes,zx_sync,zx_keyp,zx_keyr,zx_vol};

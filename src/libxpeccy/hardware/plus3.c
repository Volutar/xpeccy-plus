#include "../spectrum.h"

// in

int p3_dos_rd(Computer* comp, int port) {
	int res = -1;
	difIn(comp->dif, port, &res, 0);
	return res;
}

// out

void pl2MapMem(Computer*);
void p3Out1FFD(Computer* comp, int port, int val) {
	// the 7FFD lock holds this port too
	if (comp->p7FFD & 0x20) return;
	comp->p1FFD = val & 0xff;
	// one motor line for every drive on the cable: the uPD765 has no motor
	// outputs, and the drive it selects later is already turning
	for (int i = 0; i < 4; i++)
		comp->dif->flp[i]->motor = (val & 8) ? 1 : 0;
	pl2MapMem(comp);
}

void p3Out7FFD(Computer* comp, int port, int val) {
	if (comp->p7FFD & 0x20) return;
	comp->flgROM = (val & 0x10) ? 1 : 0;
	comp->p7FFD = val & 0xff;
	comp->vid->vidPage = (val & 0x08) ? 7 : 5;
	pl2MapMem(comp);
}

// A command from +3DOS may go fast; one from a program's own loader in RAM keeps the
// drive's time, which copy-protection loaders count on
void p3_dos_wr(Computer* comp, int port, int val) {
	difOut(comp->dif, port, val, zx_rom_code(comp, cpu_get_pc(comp->cpu)) ? 0 : DIF_FROM_RAM);
}

static xPort p3PortMap[] = {
	{0x00ff,0x001f,0,2,2,xIn1F,	NULL},		// joystick
	{0xc002,0x7ffd,2,2,2,NULL,	p3Out7FFD},
	{0xc002,0xbffd,2,2,2,NULL,	xOutBFFD},
	{0xc002,0xfffd,2,2,2,xInFFFD,	xOutFFFD},
	{0xe002,0x2ffd,2,2,2,p3_dos_rd,	p3_dos_wr},	// 2ffd/3ffd
	{0xf002,0x1ffd,2,2,2,NULL,	p3Out1FFD},
	{0x0003,0x00fe,2,2,2,xInFE,	xOutFE},
	{0x0320,0xfadf,2,2,2,xInFADF,	NULL},		// Kempston mouse: A5, A8, A9, A10, as on the 48K
	{0x0720,0xfbdf,2,2,2,xInFBDF,	NULL},
	{0x0720,0xffdf,2,2,2,xInFFDF,	NULL},
	{0x0000,0x0000,2,2,2,zx_in_float,	NULL}
};

void pl3Out(Computer* comp, int port, int val) {
	zx_dev_wr(comp, port, val);
	hwOut(p3PortMap, comp, port, val, 1);
}

int pl3In(Computer* comp, int port) {
	int res = -1;
	if (zx_dev_rd(comp, port, &res)) return res;
	res = hwIn(p3PortMap, comp, port);
	return res;
}

xPortDsc pl3_port_tab[] = {
	{0x7ffd, REG_BYTE, offsetof(Computer, p7FFD)},
	{0x1ffd, REG_BYTE, offsetof(Computer, p1FFD)},
	{-1, 0, 0}
};

void plusRes(Computer*);

// The uPD765's US1 is not wired on the +3: units 2 and 3 are drives A and B
static void pl3_init(Computer* comp) {
	zx_init(comp);
	dif_align_flps(comp->dif, comp->dif->fdc, 0, 1, 0, 1);
}

HardWare pl3_hw_core = {HW_PLUS3,"Plus3","Spectrum +3",MEM_128K,1.0,NULL,pl3_port_tab,
			pl3_init,pl2MapMem,pl3Out,pl3In,asicMRd,asicMWr,zx_irq,zx_ack,plusRes,zx_sync,zx_keyp,zx_keyr,zx_vol,
			NULL,NULL,&hwq_zx, .sndtake = zx_snd_take, .sndmix = zx_mix};

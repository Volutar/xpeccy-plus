#include "../spectrum.h"

#include <string.h>

// A Sinclair ULA reads a character pair two columns before it shows them;
// the clones read at display time. Only the first leaves a multicolour
// engine room to rewrite an attribute right after the ULA has taken it, so
// the machines that have such a ULA - the ones with a contention pattern -
// get the early-fetch drawer.
void zx_set_vmode(Computer* comp) {
	vid_set_mode(comp->vid, (comp->vid->ula->conttype == CONT_NONE) ? VID_NORMAL : VID_ULA_SCR);
}

void zx48_reset(Computer* comp) {
	comp->mem->ramMask = MEM_128K - 1;	// to acces pages 2,5
	comp->flgROM = 1;				// to switch to trdos
	comp->vid->vidPage = 5;
	zx_reset(comp);
}

void zx_reset(Computer* comp) {
	zx_set_pal(comp);
	zx_set_vmode(comp);
}

int zx_slt_rd(int adr, void* ptr) {
	Computer* comp = (Computer*)ptr;
	int res = 0xff;
	if (comp->slot->data) {
		res = comp->slot->data[adr & comp->slot->memMask];
	}
	return res;
}

void zx_slt_wr(int adr, int val, void* ptr) {}

int zx_rd_ff(int adr, void* ptr) {
	return 0xff;
}

void zx_wr_null(int adr, int val, void* ptr) {}

void speMapMem(Computer* comp) {
	comp->mem->ramMask = MEM_128K - 1;
	if (comp->slot->data) {
		memSetBank(comp->mem,0x00,MEM_SLOT, 0, MEM_16K, zx_slt_rd, zx_slt_wr, comp);
	} else {
		memSetBank(comp->mem,0x00,MEM_ROM,(comp->flgDOS) ? 1 : 0, MEM_16K,NULL,NULL,NULL);
	}
	memSetBank(comp->mem,0x40,MEM_RAM,5,MEM_16K,NULL,NULL,NULL);		// 101 / x01
	if (comp->mem->ramSize > MEM_16K) {
		memSetBank(comp->mem,0x80,MEM_RAM,2,MEM_16K,NULL,NULL,NULL);		// 010 / x10
		memSetBank(comp->mem,0xc0,MEM_RAM,0,MEM_16K,NULL,NULL,NULL);		// 000 / x00
	} else {
		memSetBank(comp->mem,0x80,MEM_EXT,1,MEM_32K,zx_rd_ff,zx_wr_null,comp);
	}
}

// in

int spIn1F(Computer* comp, int port) {
	return zx_in_joy(comp, port);
}

static xPort spePortMap[] = {
	{0x0001,0x00fe,2,2,2,xInFE,	xOutFE},
	{0xc002,0xfffd,2,2,2,xInFFFD,	xOutFFFD},
	{0xc002,0xbffd,2,2,2,NULL,	xOutBFFD},
	{0x00e1,0x001f,0,2,2,spIn1F,	NULL},		// Kempston decodes A5..A7, so #FF1F is the joystick and #FFDF is not
	{0x0320,0xfadf,2,2,2,xInFADF,	NULL},		// Kempston mouse: A5, A8-A10
	{0x0720,0xfbdf,2,2,2,xInFBDF,	NULL},
	{0x0720,0xffdf,2,2,2,xInFFDF,	NULL},
	{0x0000,0x0000,2,2,2,zx_in_float,NULL}		// a port nothing answers: the floating bus
};

void speOut(Computer* comp, int port, int val) {
	difOut(comp->dif, port, val, comp->flgBDI);
	zx_dev_wr(comp, port, val);
	hwOut(spePortMap, comp, port, val, 1);
}

int speIn(Computer* comp, int port) {
	int res;
	if (difIn(comp->dif, port, &res, comp->flgBDI)) return res;
	if (zx_dev_rd(comp, port, &res)) return res;
	return hwIn(spePortMap, comp, port);
}

HardWare z48_hw_core = {HW_ZX48,"ZX48","ZX 48K",MEM_64K | MEM_48K | MEM_16K,1.0,NULL,NULL,
			zx_init,speMapMem,speOut,speIn,stdMRd,stdMWr,zx_irq,zx_ack,zx48_reset,zx_sync,zx_keyp,zx_keyr,zx_vol,
			NULL,NULL,&hwq_zx, .sndtake = zx_snd_take, .sndmix = zx_mix};

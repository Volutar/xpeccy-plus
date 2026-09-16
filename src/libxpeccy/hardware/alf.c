#include "hardware.h"

// ALF TV Game ("Elf"), a ZX48 clone console on the T34VG1 gate array, built in
// Brest 1991-1995. It has no keyboard: two joysticks, a cartridge slot and the
// beeper. Window 0 holds a 16K page picked by #5F - bit 7 says whether it comes
// from the cartridge or from the machine's own rom, the rest is the page number
// - and the internal rom is the games menu in page 0 with Sinclair BASIC 48 in
// page 1, which is what cartridge games return to. 128K is an aftermarket
// memory expansion: plain #7FFD paging, and no #7FFD at all on a stock 64K one.
//
// Every port is decoded on two or three address lines, which is what the masks
// below say: #5F on a7=0, a1=1, a0=1, and #FE on a7=1, a0=0. The bank number is
// seven bits wide on the connector but the firmware only ever walks six of
// them, so six is what is masked here. Ground truth for all of it, schematic
// and rom dumps included, is zxbyte.ru/alf.htm.

#define regRomN	reg[0x5f]

int alf_sltrd(int adr, void* ptr) {
	Computer* comp = (Computer*)ptr;
	int res = -1;
	if (comp->slot) {
		adr = (adr & 0x3fff) | ((comp->regRomN & 0x3f) << 14);		// full address
		if (comp->slot->data && (adr <= comp->slot->memMask)) {				// not loaded = 0xff
			res = comp->slot->data[adr];
		}
	}
	return res;
}

void alf_mapmem(Computer* comp) {
	if (comp->flgROM) {		// switch to rom1 after snapshot loading
		comp->flgROM = 0;
		comp->regRomN = 1;
	}
	if (comp->regRomN & 0x80) {
		memSetBank(comp->mem, 0x00, MEM_SLOT, comp->regRomN & 0x3f, MEM_16K, alf_sltrd, NULL, comp);	// cartrige data
	} else {
		memSetBank(comp->mem, 0x00, MEM_ROM, comp->regRomN & 0x3f, MEM_16K, NULL, NULL, NULL);		// std rom
	}
	memSetBank(comp->mem, 0x40, MEM_RAM, 5, MEM_16K, NULL, NULL, NULL);
	memSetBank(comp->mem, 0x80, MEM_RAM, 2, MEM_16K, NULL, NULL, NULL);
	memSetBank(comp->mem, 0xc0, MEM_RAM, comp->p7FFD & 7, MEM_16K, NULL, NULL, NULL);
}

void alf_reset(Computer* comp) {
	comp->intVector = 0xff;
	comp->vid->vidPage = 5;
	vid_set_mode(comp->vid, VID_NORMAL);
	comp->regRomN = 0x00;
	comp->p7FFD = 0x00;
	comp->flgROM = 0;
	alf_mapmem(comp);
}

// 1F rd: joystick 1, kempston order and active high. d5..d7 are not wired to
// anything and read 1,0,1, so an idle stick reads #a0 - no extra buttons here,
// whatever the setting says: the pad has two and both go to the same fire line.
int alf_in1F(Computer* comp, int adr) {
	return (comp->joy->state & 0x1f) ^ 0xa0;
}

// 5F wr:rom page
void alf_out5F(Computer* comp, int adr, int data) {
	comp->regRomN = data & 0xff;
	alf_mapmem(comp);
}

// FE wr: border/sound
void alf_outFE(Computer* comp, int adr, int data) {
	comp->vid->nextbrd = (data & 0x07);
	comp->beep->lev = !!(data & 0x10);
}

// FE rd: joystick 2, active low and in an order of its own - fire, down, right,
// up, left - not the one joystick 1 has. d5..d7 are not wired: d6 always reads
// 0, d5 and d7 float, and a program is told to ignore all three.
int alf_inFE(Computer* comp, int adr) {
	int st = comp->joyb->state;
	int res = 0;
	if (st & XJ_FIRE) res |= 0x01;
	if (st & XJ_DOWN) res |= 0x02;
	if (st & XJ_RIGHT) res |= 0x04;
	if (st & XJ_UP) res |= 0x08;
	if (st & XJ_LEFT) res |= 0x10;
	return res ^ 0x1f;
}

void alf_out7FFD(Computer* comp, int adr, int data) {
	if (comp->mem->ramSize == MEM_64K) return;		// 48K
	comp->p7FFD = data & 7;
	comp->vid->vidPage = (data & 0x08) ? 7 : 5;
	alf_mapmem(comp);
}

static xPort alf_port_map[] = {
	{0x0081,0x00fe,2,2,2,alf_inFE,	alf_outFE},
	{0x0083,0x001f,2,2,2,alf_in1F,	alf_out5F},
	{0xc002,0x7ffd,2,2,2,NULL,	alf_out7FFD},
	{0xc002,0xbffd,2,2,2,NULL,	xOutBFFD},
	{0xc002,0xfffd,2,2,2,xInFFFD,	xOutFFFD},
	{0x0000,0x0000,2,2,2,NULL,	NULL}
};

int alf_ird(Computer* comp, int adr) {
	return hwIn(alf_port_map, comp, adr);
}

void alf_iwr(Computer* comp, int adr, int data) {
	hwOut(alf_port_map, comp, adr, data, 1);
}

int alf_mrd(Computer* comp, int adr, int m1) {
	return memRd(comp->mem, adr);
}

void alf_mwr(Computer* comp, int adr, int data) {
	memWr(comp->mem, adr, data);
}

void alf_sync(Computer* comp, int ns) {
	bcSync(comp->beep, ns);
	tsSync(comp->ts, ns);
}

// A beeper and the chips, and a dc blocker for each - see zx_vol().
sndPair alf_vol(Computer* comp, sndVolume* sv) {
	static sndDC dcBeep, dcAy;
	sndPair p;
	sndPair v;
	v.left = comp->beep->val * sv->beep / 6;
	v.right = v.left;
	p = snd_dc(&dcBeep, v, sv->dc);
	v = snd_dc(&dcAy, tsGetVolume(comp->ts), sv->dc);
	p.left += v.left * sv->ay / 100;
	p.right += v.right * sv->ay / 100;
	return p;
}

// zx_init, like every other ZX core: it is what sets the dot period from the cpu
// clock, and without it the machine keeps the dot clock of whatever ran before.
HardWare alf_hw_core = {HW_ALF,"ALF","ALF TV Game",MEM_64K | MEM_128K,1.0,NULL,NULL,
			zx_init,alf_mapmem,alf_iwr,alf_ird,alf_mrd,alf_mwr,zx_irq,zx_ack,alf_reset,alf_sync,NULL,NULL,alf_vol};

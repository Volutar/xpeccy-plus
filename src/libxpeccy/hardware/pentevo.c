#include "../spectrum.h"
#include "../cpu/Z80/z80.h"

#include <stdio.h>

// One core for every firmware. The 2021 baseconf moved the configuration block
// from #xxBE to #xxBD, but it still answers reads on both - zports.v keeps #xxBE
// with a "TODO: remove read capability" beside it - and writing #BE has meant
// "leave NMI" since 2014. So reads are taken from either port, and the hardware
// trap address is taken at its old place (#00BD/#01BD) as well as its new one
// (#10BD/#11BD), which the other firmware leaves undecoded. Nothing collides.

#define regBF	reg[16]
#define reg2F	reg[17]
#define reg4F	reg[18]
#define reg6F	reg[19]
#define reg8F	reg[20]
#define regM1CNT reg[21]

#define flgVDOS	flag[100]	// trd emulation: ram page FE @ 0x0000
#define flgVNMI flag[101]	// in NMI: ram page FF @ 0x0000
#define flgVDWP	flag[102]	// no writes to that page until the next M1
#define flgNMIR	flag[103]	// NMI asked for, taken with the next frame int
#define flgNMIS	flag[104]	// map the NMI page in on the next M1

#define regPal(_n) reg[0xe0 + (_n)]	// what was last written to palette cell n
#define memFlag(_n) reg[0xf0 + (_n)]
#define memPage(_n) reg[0xf8 + (_n)]

#define xregBRKA xreg[0]

void evoSetVideoMode(Computer* comp) {
	int mode = (comp->pEFF7 & 0x20) | ((comp->pEFF7 & 0x01) << 1) | (comp->prt2 & 0x07);	// z5.z0.0.b2.b1.b0	b:FF77, z:eff7
	switch (mode) {
		case 0x03: vid_set_mode(comp->vid,VID_NORMAL); break;		// common
		case 0x13: vid_set_mode(comp->vid,VID_ALCO); break;		// alco 16c
		case 0x23: vid_set_mode(comp->vid,VID_HWMC); break;		// zx hardware multicolor
		case 0x02: vid_set_mode(comp->vid,VID_ATM_HWM); break;	// atm hardware multicolor
		case 0x00: vid_set_mode(comp->vid,VID_ATM_EGA); break;	// atm ega
		case 0x06: vid_set_mode(comp->vid,VID_ATM_TEXT); break;	// atm text
		case 0x07: vid_set_mode(comp->vid,VID_EVO_TEXT); break;	// pentevo text
		default: vid_set_mode(comp->vid,VID_UNKNOWN); break;
	}
}

void evoSetBank(Computer* comp, int bank, int idx) { // memEntry me) {
	idx &= 7;
	unsigned char page = comp->memPage(idx) ^ 0xff;
	if (comp->memFlag(idx) & 0x80) {
		if (comp->memFlag(idx) & 0x40) {
			if (comp->pEFF7 & 4) {
				page = (page & 0xf8) | (comp->p7FFD & 7);				// mix with b0..2 (7FFD) - 128K mode
			} else {
				page = (page & 0xc0) | (comp->p7FFD & 7) | ((comp->p7FFD & 0xe0) >> 2);	// mix with b0..2,5..7 (7FFD) - P1024 mode
			}
		} else {
			page = (page & 0x3e) | (comp->flgDOS ? 1 : 0);				// mix with dosen
		}
	}
	memSetBank(comp->mem, bank, (comp->memFlag(idx) & 0x40) ? MEM_RAM : MEM_ROM, page, MEM_16K, NULL, NULL, NULL);
}

// The three things that can stand in window 0, in the pager's own order. Called
// from the pager-on branch only: with A8 of #xx77 low there is no pager and every
// window holds the service rom.
static void evo_map_win0(Computer* comp) {
	if (comp->flgVNMI) {			// while NMI
		memSetBank(comp->mem,0x00,MEM_RAM,0xff,MEM_16K,NULL,NULL,NULL);
	} else if (comp->flgVDOS) {		// trd emulation
		memSetBank(comp->mem,0x00,MEM_RAM,0xfe,MEM_16K,NULL,NULL,NULL);
	} else if (comp->pEFF7 & 8) {		// b3.EFF7: ram0 @ 0x0000
		memSetBank(comp->mem,0x00,MEM_RAM,0x00, MEM_16K, NULL, NULL, NULL);
	}
}

void evoMapMem(Computer* comp) {
	if (comp->prt2 & 0x20) {		// A8.xx77
		int adr = (comp->flgROM) ? 4 : 0;
		evoSetBank(comp, 0x00, adr); //comp->memMap[adr]);
		evoSetBank(comp, 0x40, adr+1); //comp->memMap[adr+1]);
		evoSetBank(comp, 0x80, adr+2); //comp->memMap[adr+2]);
		evoSetBank(comp, 0xc0, adr+3); //comp->memMap[adr+3]);
		evo_map_win0(comp);
	} else {
		comp->flgDOS = 1;
		memSetBank(comp->mem,0x00,MEM_ROM,0xff, MEM_16K, NULL, NULL, NULL);
		memSetBank(comp->mem,0x40,MEM_ROM,0xff, MEM_16K, NULL, NULL, NULL);
		memSetBank(comp->mem,0x80,MEM_ROM,0xff, MEM_16K, NULL, NULL, NULL);
		memSetBank(comp->mem,0xc0,MEM_ROM,0xff, MEM_16K, NULL, NULL, NULL);
	}
}

// The pages that stand in window 0 out of turn, and the NMI that brings one in
static void evo_drop_virt(Computer* comp) {
	comp->flgVDOS = 0;
	comp->flgVNMI = 0;
	comp->flgVDWP = 0;
	comp->flgNMIR = 0;
	comp->flgNMIS = 0;
}

void evoReset(Computer* comp) {
	comp->flgDOS = 1;
	comp->regBF = 0;
	comp->prt2 = 0x83;		// A14 = 1: palette closed; video mode 011
	comp->sdc->on = 1;
	sdcReset(comp->sdc);
	kbd_set_repeat(comp->keyb, 0);		// what the avr asks the ps/2 keyboard for
	evo_drop_virt(comp);
	comp->regM1CNT = 0;
	for (int i = 0; i < 8; i++) {		// power-on pager: rom page 0 everywhere
		comp->memFlag(i) = 0;
		comp->memPage(i) = 0xff;
	}
	for (int i = 0; i < 4; i++) {
		comp->dif->flp[i]->virt = 0;
	}
}

// Raise the NMI and arrange for the handler's page to come with it. Both go
// together or the handler runs out of whatever was mapped at 0x0000.
static void evo_nmi(Computer* comp) {
	comp->cpu->intrq |= Z80_NMI;
	comp->flgNMIS = 1;
}

int evoMRd(Computer* comp, int adr, int m1) {
	if (!m1) return memRd(comp->mem, adr);
	int nmient = 0;
	if (comp->dif->type == DIF_BDI) {
		int win = (adr >> 14) & 3;
		// leave TR-DOS: M1 from a window the current map holds RAM in.
		// A9 of #xx77 low holds the DOS signal on, so it blocks this.
		if (comp->flgDOS && (comp->prt2 & 0x40) &&
				(comp->memFlag((comp->flgROM << 2) | win) & 0x40)) {
			comp->flgDOS = 0;
			if (comp->flgROM) comp->hw->mapMem(comp);
		}
		// enter TR-DOS: M1 from offset #3Dxx of a window whose map 1
		// entry is rom with the dos7ffd bit set
		if (!comp->flgDOS && ((adr & 0x3f00) == 0x3d00) && comp->flgROM &&
				((comp->memFlag(4 | win) & 0xc0) == 0x80)) {
			comp->flgDOS = 1;
			comp->hw->mapMem(comp);
		}
	}
	// The NMI handler's page comes in on the fetch from 0x0066, and that fetch
	// reads as NOP whatever is under it - the fpga drives 00 on the bus for it.
	// So the handler's second opcode, at 0x0067, is the first byte that really
	// comes from page FF.
	if (comp->flgNMIS && (adr == 0x0066)) {
		comp->flgNMIS = 0;
		comp->flgVNMI = 1;
		evoMapMem(comp);
		nmient = 1;
	}
	// hardware trap: the NMI comes on the spot, not with the next frame int
	if ((comp->regBF & 0x10) && (adr == comp->xregBRKA.w)) {
		evo_nmi(comp);
	}
	comp->flgVDWP = 0;
	int res = nmient ? 0x00 : memRd(comp->mem, adr);
	// The second M1 after out (#BE) puts the map back - after this fetch, so
	// that RETN's own opcode still comes from the handler's page. The count
	// runs whether or not an NMI is up, the way the fpga does: left armed it
	// would eat the next NMI two opcodes in.
	if (comp->regM1CNT) {
		comp->regM1CNT--;
		if ((comp->regM1CNT == 0) && comp->flgVNMI) {
			comp->flgVNMI = 0;
			evoMapMem(comp);
		}
	}
	return res;
}

// TODO: write protect, see xBF7)
// adr = (flgRom << 2) | ((adr >> 14) & 3)
// memFlag(adr) & 0x20 = write protect
void evoMWr(Computer* comp, int adr, int val) {
	if (comp->regBF & 4) {
		vid_fnt_wr(comp->vid, adr & 0x7ff, val & 0xff);		// PentEvo: write font byte
	}
	if (comp->flgVDWP && (adr < 0x4000)) return;		// trd emulation page just came in
	if (comp->memFlag((comp->flgROM << 2) | ((adr >> 14) & 3)) & 0x20) return;		// write protect
	memWr(comp->mem,adr,val);
}

// in

int evoIn1F(Computer* comp, int port) {	// !dos
	return joyInput(comp->joy);
}

int evoIn57(Computer* comp, int port) {	// !dos
	int res = sdcRead(comp->sdc);
	// sdcWrite(comp->sdc, 0xff);
	return res;
}

int evoIn77(Computer* comp, int port) {	// !dos
	return 0x00;
	//int res = 0x02;		// rd only
	//if (comp->sdc->image != NULL) res |= 0x01;
	//return res;
}

int memflag_collect(Computer* comp, int mask) {
	int res = 0;
	for(int i = 0; i < 8; i++) {
		res = (res >> 1);
		if (comp->memFlag(i) & mask) res |= 0x80;
	}
	return res;
}

int evoInCfg(Computer* comp, int port) {
	int res = -1;
	if ((port & 0xf800) == 0x0000) {
		res = comp->memPage((port >> 8) & 7); // comp->memMap[(port & 0x0700) >> 8].page;
	} else {
		switch (port & 0xff00) {
			case 0x0800: res = memflag_collect(comp, 0x40); break;
			case 0x0900: res = memflag_collect(comp, 0x80); break;
			case 0x0a00: res = comp->p7FFD; break;
			case 0x0b00: res = comp->pEFF7; break;
			case 0x0c00: res = comp->prt2 | (comp->flgDOS ? 0x10 : 0x00); break;
			// the palette cell the border colour points at, in the format
			// it was written in through #FF - bits 2,3 read back as 1
			case 0x0d00: res = (comp->regPal(comp->vid->brdcol & 0x0f) & 0xf3) | 0x0c; break;
			case 0x0e00: res = comp->vid->fntbyte; break;	// font byte the text mode is showing
			case 0x0f00: res = comp->vid->nextbrd & 0x0f; break;	// last one written
			case 0x1000: res = comp->xregBRKA.l; break;
			case 0x1100: res = comp->xregBRKA.h; break;
			case 0x1200: res = memflag_collect(comp, 0x20); break;	// write protect
			// new one:
			case 0x1300: res = comp->dif->flp[0]->virt | (comp->dif->flp[1]->virt << 1)
					| (comp->dif->flp[2]->virt << 2) | (comp->dif->flp[3]->virt << 3);
				break;
			case 0x1400:
				break;
			case 0x1500:
				break;
			case 0x1600:
				break;
			default:
//				printf("PentEvo\tin %.4X.%i\n",port,bdiz);
//				assert(0);
				break;
		}
	}
	return res;
}

void evoOutCfg(Computer* comp, int port, int val) {
	int i = 0;
	switch(port & 0xff00) {
		// the trap address, at both the addresses it has ever had
		case 0x0000: case 0x1000: comp->xregBRKA.l = val; break;
		case 0x0100: case 0x1100: comp->xregBRKA.h = val; break;
		case 0x1200:
			for (i = 0; i < 8; i++) {
				if (val & 1) {
					comp->memFlag(i) |= 0x20;
				} else {
					comp->memFlag(i) &= ~0x20;
				}
				val >>= 1;
			}
			break;
		case 0x1300:
			comp->dif->flp[0]->virt = !!(val & 1);
			comp->dif->flp[1]->virt = !!(val & 2);
			comp->dif->flp[2]->virt = !!(val & 4);
			comp->dif->flp[3]->virt = !!(val & 8);
			break;
		case 0x1400:
			break;
		case 0x1500:
			break;
		case 0x1600:
			break;
	}
}

int evoInBF(Computer* comp, int port) {
	return comp->regBF;
}

// TR-DOS emulation. Touching a disk controller register with the selected
// drive marked virtual (#13BD) swaps ram page FE in over the TR-DOS rom, and
// it stays there until the rom writes #BE. The chip itself still sees the
// access - on the board it sits on the bus either way. The page cannot be
// written to for the rest of the instruction, which is what stops an INI from
// landing in it.
//
// Writes to #FF are left out on purpose. The port is the controller's system
// register, but it is also where the palette is written, and the ERS sets the
// palette one colour at a time with drive A selected by the colour value - so
// taking those would swap the page out from under the Magic Service while it
// paints. Reads of #FF (the status register) still count, and so does every
// register at #1F/#3F/#5F/#7F, which is what a loader actually drives.
//
// One thing here is ours rather than the hardware's: a drive with a disk in it
// is left to the real controller even when the rom marks it virtual. The ERS
// ships with drive A virtual, and on a real machine you would move that to a
// free letter before putting a floppy in A; here the disk the user opened has
// to be the one that boots.
void evo_trdemu(Computer* comp) {
	if (comp->flgVDOS || comp->flgVNMI) return;
	if (!comp->flgDOS) return;
	if (!comp->dif->fdc->flp->virt || comp->dif->fdc->flp->insert) return;
	if (mem_get_page(comp->mem, 0x0000)->type != MEM_ROM) return;
	comp->flgVDOS = 1;
	comp->flgVDWP = 1;
	evoMapMem(comp);
}

int evoInBDI(Computer* comp, int port) {
	int res = -1;
	difIn(comp->dif, port, &res, 1);
	evo_trdemu(comp);
	return res;
}

int evoIn2F(Computer* comp, int port) {
	return comp->reg2F;
}

int evoIn4F(Computer* comp, int port) {
	return comp->reg4F;
}

int evoIn6F(Computer* comp, int port) {
	return comp->reg6F;
}

int evoIn8F(Computer* comp, int port) {
	return comp->reg8F;
}

int evoInBEF7(Computer* comp, int port) {	// dos
	int res = cmsRd(comp);
	switch (comp->cmos.adr & 0xff) {
		case 0x0a: res = 0x00; break;
		case 0x0b: res = 0x02; break;
		case 0x0c:
			res = 0x00;
			// b2: 0 if sdc write only
			res |= 4;
			// b3: 1 if sdc is in slot (image present)
			if (comp->sdc->image) res |= 8;
			// b4: rtc cells changed
			break;
		case 0x0d:	// pc keys flags
			res = 0x80;
			// b0:left ctrl
			// b1:right ctrl
			// b2:left alt
			// b3:right alt
			// b4:left shift
			// b5:right shift
			// b6:f12 (allways controlled by emulator)
			// b7:=1
			if (comp->keyb->flag1 & 2) res |= 1;
			if (comp->keyb->flag2 & 2) res |= 2;
			if (comp->keyb->flag1 & 4) res |= 4;
			if (comp->keyb->flag2 & 4) res |= 8;
			if (comp->keyb->flag1 & 1) res |= 16;
			if (comp->keyb->flag2 & 1) res |= 32;
			break;
	}
//	printf("cmos rd: %.2X\n", res);
	return res;
}

int evoInBFF7(Computer* comp, int port) {	// !dos
	return (comp->pEFF7 & 0x80) ? evoInBEF7(comp, port) : 0xff;
}

int evoInFF(Computer* comp, int port) {
	return (comp->vid->vbrd || comp->vid->hbrd) ? 0xff : comp->vid->atrbyte & 0xff;
}

// out

// out (#BE),a - leave the NMI handler and the trd emulation both
void evoStopVrt(Computer* comp, int port, int val) {
	if (comp->flgVDOS) {
		comp->flgVDOS = 0;
		evoMapMem(comp);
	}
	comp->regM1CNT = 2;		// 2 M1 after the out, so RETN pops from the real page
}

void evoOutBF(Computer* comp, int port, int val) {
	if ((comp->regBF & ~val) & 8)	// b3: 1->0 - NMI comes with the next frame int
		comp->flgNMIR = 1;
	comp->regBF = val & 0xff;
}

void evoOut2F(Computer* comp, int port, int val) {
	comp->reg2F = val & 0xff;
}

void evoOut4F(Computer* comp, int port, int val) {
	comp->reg4F = val & 0xff;
}

void evoOut6F(Computer* comp, int port, int val) {
	comp->reg6F = val & 0xff;
}

void evoOut8F(Computer* comp, int port, int val) {
	comp->reg8F = val & 0xff;
}

void evoOut57(Computer* comp, int port, int val) {	// !dos
	sdcWrite(comp->sdc, val);
}

void evoOut77(Computer* comp, int port, int val) {
	// comp->sdc->on = (val & 1) ? 0 : 1;	// b0: must be 0
	comp->sdc->cs = (val & 2) ? 1 : 0;	// b1: 0 if sdc is selected
}

void evoOut77d(Computer* comp, int port, int val) {
	comp->prt2 = ((port & 0x4000) >> 7) | ((port & 0x0300) >> 3) | (val & 0x0f);	// a14.a9.a8.0.b3.b2.b1.b0
	if (!(comp->prt2 & 0x40)) comp->flgDOS = 1;	// A9 low: hold TR-DOS on
	compSetHwTurbo(comp,(val & 0x08) ? 4 : ((comp->pEFF7 & 0x10) ? 1 : 2));
	evoSetVideoMode(comp);
	evoMapMem(comp);
}

void evoOutF7(Computer* comp, int port, int val) {
	int adr = ((comp->flgROM) ? 4 : 0) | ((port & 0xc000) >> 14);
	if (port & 0x0800) {			// #xFF7: page, ram/rom and the dos7ffd bit
		comp->memFlag(adr) = (comp->memFlag(adr) & 0x20) | (val & 0xc0);
		comp->memPage(adr) = (val & 0x3f) | 0xc0;
	} else {				// #x7F7: full 8 bit ram page, dos7ffd untouched
		comp->memFlag(adr) |= 0x40;		// ram
		comp->memPage(adr) = val & 0xff;
	}
	evoMapMem(comp);
}

void evoOutBF7(Computer* comp, int port, int val) {
	int adr = ((comp->flgROM) ? 4 : 0) | ((port & 0xc000) >> 14);
	comp->memFlag(adr) &= ~0x20;
	if (val & 1)
		comp->memFlag(adr) |= 0x20;
}

void evoOutBDI(Computer* comp, int port, int val) {		// dos
	difOut(comp->dif, port, val, 1);
	evo_trdemu(comp);
}

static const unsigned char atm3clev[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};

void evoOutFF(Computer* comp, int port, int val) {		// dos
	difOut(comp->dif, 0xff, val, 1);
	xColor xcol;
	if (!(comp->prt2 & 0x80)) {	// A14 of #xx77 low: palette writes allowed
		int adr = comp->vid->brdcol & 0x0f;
		comp->regPal(adr) = val & 0xff;			// for the #0DBx readback
		val ^= 0xff;					// inverse colors
		port ^= 0xff00;
		if (!comp->flgDDP) port = (port & 0xff) | ((val << 8) & 0xff00);
		xcol.b = atm3clev[((val & 0x01) << 3) | ((val & 0x20) >> 3) | ((port & 0x0100) >> 7) | ((port & 0x2000) >> 13)];
		xcol.r = atm3clev[((val & 0x02) << 2) | ((val & 0x40) >> 4) | ((port & 0x0200) >> 8) | ((port & 0x4000) >> 14)];
		xcol.g = atm3clev[((val & 0x10) >> 1) | ((val & 0x80) >> 5) | ((port & 0x1000) >> 11)| ((port & 0x8000) >> 15)];
		vid_set_col(comp->vid, adr, xcol);
	}
}

void evoOut7FFD(Computer* comp, int port, int val) {
	if ((comp->pEFF7 & 4) && (comp->p7FFD & 0x20)) return;
	comp->flgROM = (val & 0x10) ? 1 : 0;
	comp->p7FFD = val & 0xff;
	comp->vid->vidPage = (val & 0x08) ? 7 : 5;
	evoMapMem(comp);
}

void evoOutBEF7(Computer* comp, int port, int val) {	// dos
	cmsWr(comp,val);
	switch(comp->cmos.adr) {
		case 0x0a:
			// set eeprom adr
			break;
		case 0x0c:
			if (val & 1) comp->keyb->outbuf = 0;
			// b7: eeprom access enabled
			break;
	}
}

void evoOutDEF7(Computer* comp, int port, int val) {	// dos
	cmos_wr(&comp->cmos, CMOS_ADR, val);
}

void evoOutBFF7(Computer* comp, int port, int val) {	// !dos
	if (comp->pEFF7 & 0x80)
		evoOutBEF7(comp,port,val);
}

void evoOutDFF7(Computer* comp, int port, int val) {	// !dos
	if (comp->pEFF7 & 0x80) {
		cmos_wr(&comp->cmos, CMOS_ADR, val);
	}
}

void evoOutEFF7(Computer* comp, int port, int val) {	// !dos
	comp->pEFF7 = val & 0xff;
	compSetHwTurbo(comp,(comp->prt2 & 0x08) ? 4 : (val & 0x08) ? 2 : 1);
	evoSetVideoMode(comp);
	evoMapMem(comp);
}

// Where a snapshot expects to find the machine. Until the firmware sets the
// pager up there is no pager at all here and the service rom stands in every
// window, so a snapshot loaded into a machine fresh from reset runs the
// service menu instead of the snapshot. This is what the firmware's own
// "128k basic" entry leaves behind, read off ERS 0.61: the rom pair at the top
// of the rom space (the 128 one with 7FFD.b4 = 0, the 48 one with it set,
// TR-DOS in the odd page of each), ram 5 and 2, and the 7FFD page in window 3,
// written through the two ports the firmware writes.
static const unsigned char evo_snap_flag[8] = {0x80, 0x40, 0x40, 0xc0, 0x80, 0x40, 0x40, 0xc0};
static const unsigned char evo_snap_page[8] = {0xc1, 0xfa, 0xfd, 0xff, 0xc3, 0xfa, 0xfd, 0xff};

static void evo_snap_map(Computer* comp) {
	comp->flgDOS = 0;
	comp->flgROM = (comp->p7FFD & 0x10) ? 1 : 0;
	evo_drop_virt(comp);
	for (int i = 0; i < 8; i++) {
		comp->memFlag(i) = evo_snap_flag[i];
		comp->memPage(i) = evo_snap_page[i];
	}
	evoOutEFF7(comp, 0xeff7, 0x14);		// page ram like a 128K, not like a Pentagon 1024
	evoOut77d(comp, 0x4377, 0x03);		// A14, A9, A8 high: pager on, TR-DOS signal free, palette closed; common video, 3.5 MHz
}

void evoOutFE(Computer* comp, int port, int val) {
	xOutFE(comp, port, val);
	comp->vid->nextbrd |= ((port ^ 8) & 8);
}

// common for all firmware versions
static xPort evoPortMap[] = {
	{0x00f7,0x00fe,2,2,2,xInFE,	evoOutFE},	// A3 = border bright
//	{0x00ff,0x00fb,2,2,2,NULL,	evoOutFB},	// covox
	{0x00ff,0x00bf,2,2,2,evoInBF,	evoOutBF},
	{0xc0fe,0x7ffd,2,2,2,NULL,	evoOut7FFD},
	{0xffff,0xfadf,2,2,2,xInFADF,	NULL},		// k-mouse (fadf,fbdf,ffdf)
	{0xffff,0xfbdf,2,2,2,xInFBDF,	NULL},
	{0xffff,0xffdf,2,2,2,xInFFDF,	NULL},
	// ay/ym. The fpga takes the low byte as #fd and then a15, a14 alone -
	// "loa==PORTFD" and "a[15:14]" in zports.v - so a9..a13 are no part of
	// it. A mask that wants them all set drops every write a player makes
	// through any other high byte, which is most of them.
	{0xc0ff,0xbffd,2,2,2,NULL,	xOutBFFD},
	{0xc0ff,0xfffd,2,2,2,xInFFFD,	xOutFFFD},
	// dos only
	{0x009f,0x001f,1,2,2,evoInBDI,	evoOutBDI},	// bdi 1f,3f,5f,7f
	{0x00ff,0x00ff,1,2,2,evoInBDI,	evoOutFF},	// bdi ff + set palette
	{0x00ff,0x002f,1,2,2,evoIn2F,	evoOut2F},	// extend bdi ports
	{0x00ff,0x004f,1,2,2,evoIn4F,	evoOut4F},
	{0x00ff,0x006f,1,2,2,evoIn6F,	evoOut6F},
	{0x00ff,0x008f,1,2,2,evoIn8F,	evoOut8F},
	{0x80ff,0x0057,1,2,2,evoIn57,	evoOut57},	// dos 57, a15=0: spi wr
	{0x80ff,0x8057,1,2,2,evoIn57,	evoOut77},	// dos 57, a15=1: control spi
	{0x00ff,0x0077,1,2,2,NULL,	evoOut77d},
	{0xffff,0xbef7,1,2,2,evoInBEF7,	evoOutBEF7},	// nvram
	{0xffff,0xdef7,1,2,2,NULL,	evoOutDEF7},
	{0x07ff,0x07f7,1,2,2,NULL,	evoOutF7},	// x7f7
	{0x0fff,0x0bf7,1,2,2,NULL,	evoOutBF7},
	// !dos only
	{0x00ff,0x001f,0,2,2,evoIn1F,	NULL},		// k-joy
	{0x00ff,0x0057,0,2,2,evoIn57,	evoOut57},	// 57,77 : spi
	{0x00ff,0x0077,0,2,2,evoIn77,	evoOut77},
	{0xffff,0xbff7,0,2,2,evoInBFF7,	evoOutBFF7},	// nvram
	{0xffff,0xdff7,0,2,2,NULL,	evoOutDFF7},
	{0xffff,0xeff7,0,2,2,NULL,	evoOutEFF7},

	{0x0000,0x0000,2,2,2,evoInFF,	NULL}
};

void evoOutCmn(Computer* comp, int port, int val) {
	hwOut(evoPortMap, comp, port, val, 1);
}

int evoInCmn(Computer* comp, int port) {
	return hwIn(evoPortMap, comp, port);
}

// the configuration block, serving both the port it used to be on and the one
// it is on now (see the note at the top of this file)
static xPort evoCfgPortMap[] = {
	{0x00ff,0x00be,2,2,2,evoInCfg,	evoStopVrt},
	{0x00ff,0x00bd,2,2,2,evoInCfg,	evoOutCfg},
	{0x0000,0x0000,2,2,2,evoInCmn,	evoOutCmn}	// go to check common ports
};

// A9 of #xx77 low (cp/m mode) holds the DOS signal, and with it the shadow
// ports, on. b0 of #BF opens them whatever else says.
static void evo_shadow(Computer* comp) {
	if ((comp->regBF & 0x01) || !(comp->prt2 & 0x40)) comp->flgBDI = 1;
}

void evoOut(Computer* comp, int port, int val) {
	evo_shadow(comp);
	zx_dev_wr(comp, port, val);
	hwOut(evoCfgPortMap, comp, port, val, 1);
}

int evoIn(Computer* comp, int port) {
	int res = -1;
	evo_shadow(comp);
	if (zx_dev_rd(comp, port, &res)) return res;
	return hwIn(evoCfgPortMap, comp, port);
}

void xt_press(Keyboard*, keyEntry*);
void xt_release(Keyboard*, keyEntry*);

void evo_keyp(Computer* comp, keyEntry* ent) {
	switch(ent->key) {
		case XKEY_LSHIFT: comp->keyb->flag1 |= 1; break;
		case XKEY_LCTRL: comp->keyb->flag1 |= 2; break;
		case XKEY_LALT: comp->keyb->flag1 |= 4; break;
		case XKEY_RSHIFT: comp->keyb->flag2 |= 1; break;
		case XKEY_RCTRL: comp->keyb->flag2 |= 2; break;
		case XKEY_RALT: comp->keyb->flag2 |= 4; break;
	}
	zx_keyp(comp, ent);
	xt_press(comp->keyb, ent);
}

void evo_keyr(Computer* comp, keyEntry* ent) {
	switch(ent->key) {
		case XKEY_LSHIFT: comp->keyb->flag1 &= ~1; break;
		case XKEY_LCTRL: comp->keyb->flag1 &= ~2; break;
		case XKEY_LALT: comp->keyb->flag1 &= ~4; break;
		case XKEY_RSHIFT: comp->keyb->flag2 &= ~1; break;
		case XKEY_RCTRL: comp->keyb->flag2 &= ~2; break;
		case XKEY_RALT: comp->keyb->flag2 &= ~4; break;
	}
	zx_keyr(comp, ent);
	xt_release(comp->keyb, ent);
}

// The NMI key and b3 of #BF going 1->0 only arm the NMI: it arrives with the
// next frame interrupt, instead of it. The hardware trap does not wait - it
// calls evo_nmi() from the fetch it caught.
void evo_irq(Computer* comp, int t) {
	switch (t) {
		case IRQ_NMI:				// armed, waiting for the frame int
			comp->flgNMIR = 1;
			break;
		case IRQ_VID_INT:
			if (!comp->flgNMIR) {
				zx_irq(comp, t);
				break;
			}
			comp->flgNMIR = 0;		// the NMI comes instead of the frame int
#if HAVEZLIB
			if (!comp->rzx.play)
#endif
				evo_nmi(comp);
			break;
		default:
			zx_irq(comp, t);
			break;
	}
}

xPortDsc evo_port_tab[] = {
	{0x7ffd, REG_BYTE, offsetof(Computer, p7FFD)},
	{0xeff7, REG_BYTE, offsetof(Computer, pEFF7)},
	{-1, 0, 0}
};

HardWare evo_hw_core = {HW_PENTEVO,HWG_ZX,"Baseconf","ZX Evolution (BaseConf)",16,MEM_4M,1.0,NULL,16,evo_port_tab,
			zx_init,evoMapMem,evoOut,evoIn,evoMRd,evoMWr,evo_irq,zx_ack,evoReset,zx_sync,evo_keyp,evo_keyr,zx_vol,
			evo_snap_map};

#include "filetypes.h"
#include "../xlog.h"
#include "../cpu/Z80/z80.h"

#pragma pack (push, 1)

typedef struct {
	unsigned char a,f,c,b,l,h;
	unsigned char pcl,pch;
	unsigned char spl,sph;
	unsigned char i,r7,flag12;
	unsigned char e,d;
	unsigned char _c,_b,_e,_d,_l,_h,_a,_f;
	unsigned char iyl,iyh;
	unsigned char ixl,ixh;
	unsigned char iff1,iff2,flag29;
} z80v1Header;

#pragma pack (pop)

/*
unsigned short fgetwLE(FILE* file) {		// get WORD little endian (low-hi)
	unsigned short res = fgetc(file);
	res |= (fgetc(file) << 8);
	return res;
}
*/

void fputwLE(FILE* file, unsigned short wrd) {
	fputc(wrd & 0xff, file);
	fputc((wrd & 0xff00) >> 8, file);
}

void z80uncompress(FILE* file, char* buf, int maxlen) {
	char *ptr = buf;
	unsigned char tmp,tmp2;
	unsigned char lst = 0xed;
	int btm = 1;
	do {
		tmp = fgetc(file);
		if (tmp == 0xed) {
			tmp = fgetc(file);
			if (tmp == 0xed) {
				tmp2 = fgetc(file);
				if ((lst == 0x00) && (tmp2 == 0x00)) {
					btm = 0;	// stop @ 00 ed ed 00
				} else {
					tmp = fgetc(file);
					do {
						*(ptr++) = tmp;
						tmp2--;
					} while (tmp2 && (ptr - buf < maxlen));
				}
			} else {
				*(ptr++) = 0xed;
				if (ptr - buf < maxlen) *(ptr++) = tmp;
				lst = tmp;
			}
		} else {
			*(ptr++) = tmp;
			lst = tmp;
		}
	} while (btm && !feof(file) && (ptr - buf < maxlen));
}

unsigned char z80readblock(FILE* file, char* buf) {
	unsigned char tmp;
	int adr;
	adr = fgetw(file);		// compressed size
	tmp = fgetc(file);		// page num
	if (adr == 0xffff) {
		fread(buf, 0x4000, 1, file);
	} else {
		z80uncompress(file, buf, 0x4000);
	}
	return tmp;
}

static const char* v2hardware[16] = {
	"48k","48k + If.1","SamRam","128k","128k + If.1","unknown","unknown",
	"Spectrum +3","unknown","Pentagon 128K","Scorpion 256K","Didaktik",
	"Spectrum +2","Spectrum +2A","TC1048","TC2068"
};

static const char* v3hardware[16] = {
	"48k","48k + If.1","SamRam","48k + M.G.T","128k","128k + If.1","128k + M.G.T.",
	"Spectrum +3","unknown","Pentagon 128K","Scorpion 256K","Didaktik",
	"Spectrum +2","Spectrum +2A","TC1048","TC2068"
};

// What the hardware byte (34) of a v2/v3 header names, with the "modify
// hardware" bit (37.7) folded in: it turns a 128K into a +2 and a +3 into a
// +2A (and a 48K into a 16K, which loads as a 48K)

static int z80_hardware(int v3, int hw, int mod) {
	switch (hw) {
		case 0:
		case 1:
		case 2: return SNAP_HW_48K;			// 2: SamRam, loads as a 48K
		case 3: if (v3) return SNAP_HW_48K;		// v3: 48K + M.G.T.
			return mod ? SNAP_HW_PLUS2 : SNAP_HW_128K;
		case 4: return mod ? SNAP_HW_PLUS2 : SNAP_HW_128K;
		case 5:
		case 6: if (!v3) return SNAP_HW_UNKNOWN;
			return mod ? SNAP_HW_PLUS2 : SNAP_HW_128K;
		case 7:
		case 8: return mod ? SNAP_HW_PLUS2A : SNAP_HW_PLUS3;
		case 9: return SNAP_HW_PENTAGON;
		case 10: return SNAP_HW_SCORPION;
		case 12: return SNAP_HW_PLUS2;
		case 13: return SNAP_HW_PLUS2A;
	}
	return SNAP_HW_UNKNOWN;		// Didaktik, Timex
}

int z80GetHardware(const char* name) {
	FILE* file = fopen(name, "rb");
	if (!file) return SNAP_HW_UNKNOWN;
	unsigned char buf[38];
	int res = SNAP_HW_UNKNOWN;
	if (fread(buf, 1, sizeof(buf), file) == sizeof(buf)) {
		if (buf[6] | buf[7]) {				// PC set: version 1, always a 48K
			res = SNAP_HW_48K;
		} else {
			res = z80_hardware((buf[30] | (buf[31] << 8)) > 23, buf[34], buf[37] & 0x80);
		}
	}
	fclose(file);
	return res;
}

int loadZ80(Computer* comp, const char* name, int drv) {
	FILE* file = fopen(name, "rb");
	if (!file) return ERR_CANT_OPEN;
	int res = loadZ80_f(comp, file);
	fclose(file);
	if (res == ERR_OK)
		mem_set_path(comp->mem, name);
	return res;
}

int loadZ80_f(Computer* comp, FILE* file) {
	int btm;
	int err = ERR_OK;
	unsigned char tmp,tmp2,reg,lst,pg;
	unsigned short adr, twrd;
	int hw;
	int p1ffd = -1;
//	CPU* cpu = comp->cpu;
	char pageBuf[0xc000];
	z80v1Header hd;
	comp->p7FFD = 0x10;
	comp->p1FFD = 0x00;
	comp->pEFF7 = 0x00;
	memSetBank(comp->mem,0x00,MEM_ROM,1,MEM_16K,NULL,NULL,NULL);
	memSetBank(comp->mem,0xc0,MEM_RAM,0,MEM_16K,NULL,NULL,NULL);
	comp_snap_map(comp);
	comp->vid->vidPage = 5;
	comp_heat_reset(comp);		// snapshot load teleports state; pre-load hit counts are no longer valid

	fread((char*)&hd, sizeof(z80v1Header), 1, file);
	if (hd.flag12 == 0xff) hd.flag12 = 0x01;	// Because of compatibility, if byte 12 is 255, it has to be regarded as being 1.

	comp->cpu->regA = hd.a;
	cpu_set_flag(comp->cpu, hd.f);
	comp->cpu->regBC = (hd.b << 8) | hd.c;
	comp->cpu->regDE = (hd.d << 8) | hd.e;
	comp->cpu->regHL = (hd.h << 8) | hd.l;
	comp->cpu->regAa = hd._a;
	comp->cpu->regFa = hd._f;
	comp->cpu->regBCa = (hd._b << 8) | hd._c;
	comp->cpu->regDEa = (hd._d << 8) | hd._e;
	comp->cpu->regHLa = (hd._h << 8) | hd._l;
	comp->cpu->regPC = (hd.pch << 8) | hd.pcl;
	comp->cpu->regSP = (hd.sph << 8) | hd.spl;
	comp->cpu->regIX = (hd.ixh << 8) | hd.ixl;
	comp->cpu->regIY = (hd.iyh << 8) | hd.iyl;
	comp->cpu->regI = hd.i;
	comp->cpu->regR7 = (hd.flag12 & 1) ? 0x80 : 0;
	comp->cpu->regR = (hd.r7 & 0x7f) | comp->cpu->regR7;
	comp->cpu->regIM = hd.flag29 & 3;
	comp->cpu->flgIFF1 = hd.iff1;
	comp->cpu->flgIFF2 = hd.iff2;
	comp->cpu->inten = Z80_NMI | (hd.iff1 ? Z80_INT : 0);
	comp->vid->brdcol = (hd.flag12 >> 1) & 7;
	comp->vid->nextbrd = comp->vid->brdcol;
// unsupported things list
	if (hd.flag12 & 0x10) xlog(XLG_FILE, XLL_DEBUG, "...flag 12.bit 4.Basic SamRom switched in");
	if (hd.flag29 & 0x04) xlog(XLG_FILE, XLL_DEBUG, "...flag 29.bit 2.Issue 2 emulation");
	if (hd.flag29 & 0x08) xlog(XLG_FILE, XLL_DEBUG, "...flag 29.bit 3.Double interrupt frequency");
// continued
	if (comp->cpu->regPC == 0) {
		adr = fgetw(file);
		twrd = fgetw(file);
		comp->cpu->regPC = twrd;
		lst = fgetc(file);			// 34: HW mode
		pg = fgetc(file);			// 35: 7FFD last out
		tmp = fgetc(file);			// 36: skip (IF1)
		tmp = fgetc(file);			// 37: flags
		hw = z80_hardware(adr > 23, lst, tmp & 0x80);
		reg = fgetc(file);			// 38: last out to fffd
		for (tmp2 = 0; tmp2 < 16; tmp2++) {	// AY regs
			tmp = fgetc(file);
			tsOut(comp->ts, 0xfffd, tmp2);
			tsOut(comp->ts, 0xbffd, tmp);
		}
		comp->flgBDI = 0;
		comp->hw->out(comp, 0xfffd, reg);

		if (adr > 23) {
xlog(XLG_FILE, XLL_DEBUG, ".z80 version 3");
			if (lst < 16) xlog(XLG_FILE, XLL_DEBUG, "Hardware: %s",v3hardware[lst]);
			long base = ftell(file) - 55;		// where the snapshot starts, in an rzx too
			if (adr > 54) {				// 86: last out to 1ffd
				fseek(file, base + 86, SEEK_SET);
				p1ffd = fgetc(file);
			}
			fseek(file, base + 32 + adr, SEEK_SET);	// skip all other bytes
		} else {
xlog(XLG_FILE, XLL_DEBUG, ".z80 version 2");
			if (lst < 16) xlog(XLG_FILE, XLL_DEBUG, "Hardware: %s",v2hardware[lst]);
		}
		// a 48K has no paging, and byte 35 means nothing there: taking it
		// as 7ffd would put the 128 rom in on a machine that has one
		comp->hw->out(comp, 0x7ffd, (hw == SNAP_HW_48K) ? 0x10 : pg);
		// 1ffd is a different port on each machine that has it
		if ((p1ffd >= 0) && ((hw == SNAP_HW_PLUS2A) || (hw == SNAP_HW_PLUS3) || (hw == SNAP_HW_SCORPION))
			&& snapHwRuns(hw, comp->hw->id))
			comp->hw->out(comp, 0x1ffd, p1ffd);
		switch (hw) {
			case SNAP_HW_48K:
				btm = 1;
				do {
					tmp = z80readblock(file,pageBuf);
					switch (tmp) {
						case 4: memPutData(comp->mem,MEM_RAM,2,MEM_16K,pageBuf); break;
						case 5: memPutData(comp->mem,MEM_RAM,0,MEM_16K,pageBuf); break;
						case 8: memPutData(comp->mem,MEM_RAM,5,MEM_16K,pageBuf); break;
						default: btm = 0; break;
					}
				} while (btm && !feof(file));
				break;
			case SNAP_HW_128K:
			case SNAP_HW_PLUS2:
			case SNAP_HW_PLUS2A:
			case SNAP_HW_PLUS3:
			case SNAP_HW_PENTAGON:
				btm = 1;
				do {
					tmp = z80readblock(file,pageBuf);
					if ((tmp > 2) && (tmp < 11)) {
						memPutData(comp->mem,MEM_RAM,tmp-3,MEM_16K,pageBuf);
					} else {
						btm = 0;
					}
				} while (btm && !feof(file));
				break;
			case SNAP_HW_SCORPION:
				btm = 1;
				do {
					tmp = z80readblock(file,pageBuf);
					if ((tmp > 2) && (tmp < 19)) {
						memPutData(comp->mem,MEM_RAM,tmp-3,MEM_16K,pageBuf);
					} else {
						btm = 0;
					}
				} while (btm && !feof(file));
				break;
			default:
				xlog(XLG_FILE, XLL_WARN, "Hardware mode not supported. reset");
				compReset(comp, RES_DEFAULT);
				err = ERR_Z80_HW;
				break;
		}
	} else {			// version 1
xlog(XLG_FILE, XLL_DEBUG, ".z80 version 1");
		if (hd.flag12 & 0x20) {
			xlog(XLG_FILE, XLL_DEBUG, "data is compressed");
			z80uncompress(file,pageBuf,0xc000);
			memPutData(comp->mem,MEM_RAM,5,MEM_16K,pageBuf);
			memPutData(comp->mem,MEM_RAM,2,MEM_16K,pageBuf + MEM_16K);
			memPutData(comp->mem,MEM_RAM,0,MEM_16K,pageBuf + MEM_32K);
		} else {
			xlog(XLG_FILE, XLL_DEBUG, "data is not compressed");
			fread(pageBuf, 0x4000, 1, file);
			memPutData(comp->mem,MEM_RAM,5,MEM_16K,pageBuf);
			fread(pageBuf, 0x4000, 1, file);
			memPutData(comp->mem,MEM_RAM,2,MEM_16K,pageBuf);
			fread(pageBuf, 0x4000, 1, file);
			memPutData(comp->mem,MEM_RAM,0,MEM_16K,pageBuf);
		}
	}
	tsReset(comp->ts);
	vid_reset_ray(comp->vid);
	return err;
}

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

// One block: compressed size, page number, data. Returns the page number, or
// -1 at the end of the file. The block is left exactly at its end whatever the
// data says, so a page this machine has no use for can be stepped over.
static int z80readblock(FILE* file, char* buf) {
	int len = fgetc(file);		// compressed size
	int hib = fgetc(file);
	int pg = fgetc(file);		// page num
	if ((len < 0) || (hib < 0) || (pg < 0)) return -1;
	len |= (hib << 8);
	if (len == 0xffff) {
		if (fread(buf, 0x4000, 1, file) != 1) return -1;
	} else {
		long pos = ftell(file);
		z80uncompress(file, buf, 0x4000);
		fseek(file, pos + len, SEEK_SET);
	}
	return pg;
}

// One row per hardware the format can name: the byte 34 value version 3 gives
// it, how many 16K ram banks it holds, and whether it has a 1FFD - which only a
// 55-byte extended header has room for. The bytes are the table z80_hardware()
// reads, the other way round.
typedef struct {
	int snap;
	int byte;
	int banks;
	int p1ffd;
} z80Hardware;

static const z80Hardware z80hwtab[] = {
	{SNAP_HW_48K, 0, 3, 0},
	{SNAP_HW_128K, 4, 8, 0},
	{SNAP_HW_PLUS3, 7, 8, 1},
	{SNAP_HW_PENTAGON, 9, 8, 0},
	{SNAP_HW_SCORPION, 10, 16, 1},
	{SNAP_HW_PLUS2, 12, 8, 0},
	{SNAP_HW_PLUS2A, 13, 8, 1},
	{SNAP_HW_UNKNOWN, 0, 0, 0}
};

static const z80Hardware* z80_hw(int snap) {
	int i = 0;
	while ((z80hwtab[i].snap != SNAP_HW_UNKNOWN) && (z80hwtab[i].snap != snap))
		i++;
	return &z80hwtab[i];
}

// The ram bank a block's page number stands for, -1 for anything else. The 48K
// layout is the odd one: 4 is 8000, 5 is c000 and 8 is 4000.
static int z80_block_bank(int snap, int page) {
	if (snap == SNAP_HW_48K) {
		switch (page) {
			case 4: return 2;
			case 5: return 0;
			case 8: return 5;
		}
		return -1;
	}
	if ((page > 2) && (page - 3 < z80_hw(snap)->banks))
		return page - 3;
	return -1;
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

// the same from a header already in memory - a snapshot inside an rzx never
// reaches a file of its own
int z80_hardware_of(const unsigned char* buf, int len) {
	if (len < 8) return SNAP_HW_UNKNOWN;
	if (buf[6] | buf[7]) return SNAP_HW_48K;	// PC set: version 1, always a 48K
	if (len < 38) return SNAP_HW_UNKNOWN;
	return z80_hardware((buf[30] | (buf[31] << 8)) > 23, buf[34], buf[37] & 0x80);
}

int z80GetHardware(const char* name) {
	FILE* file = fopen(name, "rb");
	if (!file) return SNAP_HW_UNKNOWN;
	unsigned char buf[38];
	int res = z80_hardware_of(buf, (int)fread(buf, 1, sizeof(buf), file));
	fclose(file);
	return res;
}

// Where in the frame the snapshot was taken, as a tick from the interrupt.
// Byte 57 counts the quarter of the frame down from there, bytes 55-56 the T
// left in that quarter; the quarter itself is not in the file - it is the frame
// of whatever machine reads it - so a snapshot from another machine's timings
// lands a few T out. Versions 1 and 2 say nothing (low < 0), and -1 is the
// start of the frame.
static int z80_frame_tick(Computer* comp, int low, int hi) {
	int quart = comp_frame_ticks(comp) / 4;
	if ((low < 0) || (quart < 1)) return -1;
	return (((hi + 1) & 3) + 1) * quart - (low + 1);
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
	int tlow = -1;			// beam position, version 3 only
	int thi = 0;
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
			tlow = fgetw(file);			// 55: T left in this quarter of the frame
			thi = fgetc(file);			// 57: which quarter
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
		if ((p1ffd >= 0) && z80_hw(hw)->p1ffd && snapHwRuns(hw, comp->hw->id))
			comp->hw->out(comp, 0x1ffd, p1ffd);
		switch (hw) {
			case SNAP_HW_48K:
			case SNAP_HW_128K:
			case SNAP_HW_PLUS2:
			case SNAP_HW_PLUS2A:
			case SNAP_HW_PLUS3:
			case SNAP_HW_PENTAGON:
			case SNAP_HW_SCORPION:
				// stop on the machine's last bank rather than at the end of the
				// file: a .z80 inside an rzx has the next record after it
				btm = z80_hw(hw)->banks;
				while (btm > 0) {
					int blk = z80readblock(file, pageBuf);
					if (blk < 0) break;			// end of file
					int bnk = z80_block_bank(hw, blk);
					if (bnk >= 0) {
						memPutData(comp->mem, MEM_RAM, bnk, MEM_16K, pageBuf);
						btm--;
					} else if ((blk > 3) && (blk != 11)) {
						break;				// not a block of this snapshot
					}
					// 0..3 and 11 are roms: a snapshot may carry one, and the
					// blocks are in no set order, so it is skipped not obeyed
				}
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
	comp_set_frame_tick(comp, z80_frame_tick(comp, (err == ERR_OK) ? tlow : -1, thi));
	return err;
}

// --- saving ---

// A snapshot is written as version 3. What the format has no place for is
// simply not in the file: TR-DOS paged in, ram past the machine's first 128K
// (256K on a Scorpion), and any machine the hardware byte cannot name.

// What a machine is written as. The twin of snapHwRuns(), which asks the same
// question the other way round - a machine added here belongs there too.
static int z80_snap_hw(int hwid) {
	switch (hwid) {
		case HW_ZX48: return SNAP_HW_48K;
		case HW_ZX128: return SNAP_HW_128K;
		case HW_PLUS2A: return SNAP_HW_PLUS2A;
		case HW_PLUS3: return SNAP_HW_PLUS3;
		case HW_PENT: return SNAP_HW_PENTAGON;
		case HW_SCORP: return SNAP_HW_SCORPION;
	}
	return SNAP_HW_UNKNOWN;
}

// RLE for a block: five or more equal bytes become ED ED count value, and a
// pair of EDs is coded too, so the decoder cannot read it as the start of a
// run. A lone ED takes the byte after it out uncoded for the same reason.
static int z80_compress(const unsigned char* src, int len, unsigned char* dst) {
	int i = 0;
	int n = 0;
	int cnt;
	unsigned char val;
	while (i < len) {
		val = src[i];
		if ((val == 0xed) && (i + 1 < len) && (src[i + 1] != 0xed)) {
			dst[n++] = 0xed;
			dst[n++] = src[i + 1];
			i += 2;
			continue;
		}
		cnt = 1;
		while ((i + cnt < len) && (src[i + cnt] == val) && (cnt < 255))
			cnt++;
		if ((cnt > 4) || ((val == 0xed) && (cnt > 1))) {
			dst[n++] = 0xed;
			dst[n++] = 0xed;
			dst[n++] = cnt;
			dst[n++] = val;
		} else {
			memset(dst + n, val, cnt);
			n += cnt;
		}
		i += cnt;
	}
	return n;
}

static void z80_write_page(FILE* file, unsigned char* src, int num) {
	unsigned char buf[MEM_16K * 2];		// the coding can double a block at worst
	int len = z80_compress(src, MEM_16K, buf);
	int raw = (len >= MEM_16K);		// nothing gained: the block goes in as it is
	fputw(raw ? 0xffff : len, file);
	fputc(num, file);
	fwrite(raw ? src : buf, raw ? MEM_16K : len, 1, file);
}

int saveZ80(Computer* comp, const char* name, int drv) {
	int snap = z80_snap_hw(comp->hw->id);
	if (snap == SNAP_HW_UNKNOWN) return ERR_Z80_HW;
	const z80Hardware* hwi = z80_hw(snap);
	FILE* file = fopen(name, "wb");
	if (!file) return ERR_CANT_OPEN;
	int i;
	int psg = (comp->ts->chipA->type != SND_NONE);
	int hlen = hwi->p1ffd ? 55 : 54;	// only a 55-byte header has room for it
	z80v1Header hd;
	xreg16 rp;
	memset(&hd, 0, sizeof(z80v1Header));
	hd.a = comp->cpu->regA;
	hd.f = cpu_get_flag(comp->cpu);
	hd.b = comp->cpu->regB; hd.c = comp->cpu->regC;
	hd.d = comp->cpu->regD; hd.e = comp->cpu->regE;
	hd.h = comp->cpu->regH; hd.l = comp->cpu->regL;
	hd._a = comp->cpu->regAa; hd._f = comp->cpu->regFa;
	rp.w = comp->cpu->regBCa; hd._b = rp.h; hd._c = rp.l;
	rp.w = comp->cpu->regDEa; hd._d = rp.h; hd._e = rp.l;
	rp.w = comp->cpu->regHLa; hd._h = rp.h; hd._l = rp.l;
	hd.ixh = comp->cpu->regIXh; hd.ixl = comp->cpu->regIXl;
	hd.iyh = comp->cpu->regIYh; hd.iyl = comp->cpu->regIYl;
	hd.sph = comp->cpu->regSPh; hd.spl = comp->cpu->regSPl;
	hd.pcl = 0; hd.pch = 0;			// 0: the pc is in the extended header
	hd.i = comp->cpu->regI;
	hd.r7 = z80_get_r(comp->cpu);
	hd.flag12 = ((hd.r7 & 0x80) ? 1 : 0) | ((comp->vid->brdcol & 7) << 1);
	hd.iff1 = comp->cpu->flgIFF1 ? 1 : 0;
	hd.iff2 = comp->cpu->flgIFF2 ? 1 : 0;
	hd.flag29 = comp->cpu->regIM & 3;
	fwrite((char*)&hd, sizeof(z80v1Header), 1, file);

	fputw(hlen, file);			// 30: extended header size
	fputw(comp->cpu->regPC, file);		// 32: pc
	fputc(hwi->byte, file);			// 34: hardware
	fputc(comp->p7FFD, file);		// 35: last out to 7ffd
	fputc(0x00, file);			// 36: no interface 1
	fputc(psg ? 0x04 : 0x00, file);		// 37: flags (b2: ay in use)
	fputc(psg ? comp->ts->chipA->curReg : 0, file);		// 38: last out to fffd
	for (i = 0; i < 16; i++)		// 39: ay registers
		fputc(psg ? comp->ts->chipA->reg[i] : 0, file);
	// 55: the frame split in quarters, counted down from the interrupt
	int flen = comp_frame_ticks(comp);
	int quart = (flen > 3) ? (flen / 4) : 1;
	int tick = comp->frmtCount;				// T since the interrupt
	if (tick < 0) tick = 0;
	if (tick >= quart * 4) tick = quart * 4 - 1;
	fputw(quart - (tick % quart) - 1, file);
	fputc(((tick / quart) + 3) & 3, file);	// 57
	for (i = 58; i < 86; i++)		// 58..85: nothing of ours
		fputc(0x00, file);
	if (hwi->p1ffd)
		fputc(comp->p1FFD, file);	// 86: last out to 1ffd

	// page numbers, not the machine's own bank numbers: 19 is one past the
	// highest any of them uses
	for (i = 0; i < 19; i++) {
		int bank = z80_block_bank(snap, i);
		if (bank >= 0)
			z80_write_page(file, comp->mem->ramData + ((bank << 14) & comp->mem->ramMask), i);
	}
	fclose(file);
	mem_set_path(comp->mem, name);
	return ERR_OK;
}

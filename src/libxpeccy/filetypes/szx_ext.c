#include <stddef.h>

#include "filetypes.h"
#include "szx.h"
#include "../xlog.h"
#include "../cpu/Z80/z80.h"
#include "../sound/saa1099.h"
#include "../sound/gs.h"
#include "../sound/soundrive.h"
#include "../hdd.h"
#include "../sdcard.h"
#include "../nvram.h"

void gsiowr(int, int, void*);		// sound/gs.c: the GS cpu's own ports

// What a zx-state file has no block for, kept in the creator block's own data,
// which the spec leaves to the program that wrote it: a tag, then records shaped
// like the file's blocks - a 4-byte id, a 32-bit length and the bytes. A record
// holds named fields: a length-prefixed key, a 16-bit length and the bytes as
// they are in memory. A field is taken only when this build has one of that
// name and size, so a file of an older or newer build loads what still fits and
// leaves the rest as the standard blocks set it.

static const char szx_ext_tag[8] = {'X','p','e','c','c','y','+',0};

typedef struct {
	const char* key;
	size_t off;
	size_t size;
} szxField;

#define FLD(type, f)		{#f, offsetof(type, f), sizeof(((type*)0)->f)}
#define FLDN(key, type, f)	{key, offsetof(type, f), sizeof(((type*)0)->f)}
#define FLDRAW(key, off, sz)	{key, off, sz}
#define FLDEND			{NULL, 0, 0}

static void fld_put(szxBuf* d, const void* base, const szxField* tab) {
	for (; tab->key; tab++) {
		size_t k = strlen(tab->key);
		sb_byte(d, (int)k);
		sb_put(d, tab->key, k);
		sb_word(d, (int)tab->size);
		sb_put(d, (const unsigned char*)base + tab->off, tab->size);
	}
}

// what the record has of tab, into base; 1 if anything at all was taken
static int fld_get(void* base, const szxField* tab, const unsigned char* p, size_t n) {
	size_t pos = 0;
	int got = 0;
	while (pos < n) {
		size_t k = p[pos++];
		if (pos + k + 2 > n) break;
		const char* key = (const char*)(p + pos);
		pos += k;
		size_t len = rd_word(p + pos);
		pos += 2;
		if (pos + len > n) break;
		const szxField* f = tab;
		while (f->key && ((strlen(f->key) != k) || memcmp(f->key, key, k))) f++;
		if (f->key && (f->size == len)) {
			memcpy((unsigned char*)base + f->off, p + pos, len);
			got = 1;
		} else if (f->key) {
			xlog(XLG_FILE, XLL_DEBUG, "szx: field %.*s is %u bytes here, %u in the file: skipped",
				(int)k, key, (unsigned)f->size, (unsigned)len);
		}
		pos += len;
	}
	return got;
}

static void rec_begin(szxBuf* b, unsigned id, size_t* mark) {
	sb_dword(b, id);
	*mark = b->len;
	sb_dword(b, 0);
}

static void rec_end(szxBuf* b, size_t mark) {
	if (b->fail) return;
	unsigned n = (unsigned)(b->len - mark - 4);
	b->data[mark] = n & 0xff;
	b->data[mark + 1] = (n >> 8) & 0xff;
	b->data[mark + 2] = (n >> 16) & 0xff;
	b->data[mark + 3] = (n >> 24) & 0xff;
}

static void rec_fields(szxBuf* b, unsigned id, const void* base, const szxField* tab) {
	size_t mark;
	if (!base) return;
	rec_begin(b, id, &mark);
	fld_put(b, base, tab);
	rec_end(b, mark);
}

// --- the tables ---

// the cpu: its whole register file, and the flags that are state rather than
// what the machine's settings make of it (contention, snow, the debugger's)
static const szxField fld_cpu[] = {
	FLD(CPU, regs),
	FLD(CPU, flags),
	FLD(CPU, intrq),
	FLD(CPU, inten),
	FLD(CPU, intvec),
	FLDEND
};
static const int cpu_state_flags[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 58, 59, 60, 62, -1};

static const szxField fld_comp[] = {
	FLD(Computer, hwMul),
	FLD(Computer, intVector),
	FLD(Computer, earV),
	FLD(Computer, earTick),
	FLD(Computer, earDead),
	FLD(Computer, fbusLast),
	FLD(Computer, fbusTick),
	FLD(Computer, snowBad),
	FLD(Computer, frmCount),
	FLD(Computer, tickCount),
	FLD(Computer, frmtCount),
	FLD(Computer, hCount),
	FLD(Computer, waitDebt),
	FLD(Computer, waitPaid),
	FLD(Computer, fCount),
	FLD(Computer, flag),
	FLD(Computer, sysflag),
	FLD(Computer, reg),
	FLD(Computer, xreg),
	FLD(Computer, cmos),
	FLD(Computer, tsconf),
	FLDEND
};
// the machine's own flags, not the debugger's or the settings'
static const int comp_state_flags[] = {4, 11, 12, 13, 14, 15, 20, -1};

static const szxField fld_vid[] = {
	FLDRAW("bits1", 12, 4),		// intLINE..vbrd
	FLD(Video, intFRAME),
	FLD(Video, intlen),
	FLD(Video, nsDrawFixed),
	FLD(Video, nsOwedFixed),
	FLD(Video, nsCalmFixed),
	FLD(Video, time),
	FLD(Video, busy),
	FLD(Video, flash),
	FLD(Video, vidPage),
	FLD(Video, brdcol),
	FLD(Video, nextbrd),
	FLD(Video, inten),
	FLD(Video, intrq),
	FLD(Video, intbf),
	FLD(Video, paln),
	FLD(Video, vmode),
	FLD(Video, fcnt),
	FLD(Video, lcnt),
	FLD(Video, atrbyte),
	FLD(Video, fntbyte),
	FLD(Video, snowLow),
	FLD(Video, snowBank),
	FLD(Video, intp),
	FLD(Video, intsize),
	FLD(Video, sc),
	FLD(Video, scrsize),
	FLD(Video, inth),
	FLD(Video, intf),
	FLD(Video, idx),
	FLD(Video, tsconf),
	FLD(Video, line),		// the line TSConf's layers are drawn into, half done mid-line
	FLD(Video, linb),
	FLDEND
};
// a palette is the machine's only where its own ports write it: elsewhere it is
// the one the user picked for the program
static const szxField fld_vid_pal[] = {
	FLD(Video, pal),
	FLD(Video, gpal),
	FLD(Video, bpal),
	FLDEND
};

static const szxField fld_chip[] = {
	FLDRAW("bits", 0, 4),		// coarse, blk_fm
	FLD(aymChip, wait),
	FLD(aymChip, chanA),
	FLD(aymChip, chanB),
	FLD(aymChip, chanC),
	FLD(aymChip, chanN),
	FLD(aymChip, chanE),
	FLD(aymChip, eForm),
	FLD(aymChip, tickAcc),
	FLD(aymChip, pendNs),
	FLD(aymChip, curReg),
	FLD(aymChip, reg),
	FLDEND
};

static const szxField fld_ts[] = {
	FLDRAW("bits", 0, 4),		// mute_l, mute_r, r_stat
	FLDEND
};

static const szxField fld_beep[] = {
	FLDRAW("bits", 0, 4),		// lev, mic
	FLD(bitChan, val),
	FLD(bitChan, lo),
	FLD(bitChan, hi),
	FLD(bitChan, step),
	FLD(bitChan, perH),
	FLD(bitChan, perL),
	FLD(bitChan, pcount),
	FLDEND
};

static const szxField fld_saa[] = {
	FLDRAW("bits", 0, 4),		// enabled (the setting), off
	FLD(saaChip, time),
	FLD(saaChip, curReg),
	FLD(saaChip, chan),
	FLD(saaChip, noiz),
	FLD(saaChip, env),
	FLDEND
};

static const szxField fld_sdrv[] = {
	FLD(SDrive, chan),
	FLDEND
};

static const szxField fld_fdc[] = {
	FLDRAW("bits", 4, 4),		// irq, drq, dir, mr, block, side, step, mfm, idle, crchi
	FLD(FDC, trk),
	FLD(FDC, sec),
	FLD(FDC, data),
	FLD(FDC, com),
	FLD(FDC, state),
	FLD(FDC, tmp),
	FLD(FDC, wdata),
	FLD(FDC, tdata),
	FLD(FDC, bytedelay),
	FLD(FDC, crc),
	FLD(FDC, fcrc),
	FLD(FDC, buf),
	FLD(FDC, fmode),
	FLD(FDC, cnt),
	FLD(FDC, wait),
	FLD(FDC, tns),
	FLD(FDC, hold),
	FLD(FDC, drdy),
	FLD(FDC, pos),
	FLDRAW("bits2", offsetof(FDC, hlt) - 4, 4),	// dma, intr
	FLD(FDC, hlt),
	FLD(FDC, hut),
	FLD(FDC, srt),
	FLD(FDC, comBuf),
	FLD(FDC, comCnt),
	FLD(FDC, comPos),
	FLD(FDC, resBuf),
	FLD(FDC, resCnt),
	FLD(FDC, resPos),
	FLD(FDC, sr0),
	FLD(FDC, sr1),
	FLD(FDC, sr2),
	FLD(FDC, sr3),
	FLDEND
};

static const szxField fld_dif[] = {
	FLDRAW("bits", 0, 4),		// inten, doors
	FLD(DiskIF, sys),
	FLDEND
};

// a drive's head, motor and where the disk has turned to; what is in it is the
// medium's, not the snapshot's
static const szxField fld_flp[] = {
	FLDRAW("bits", 0, 4),		// motor, virt, door, changed, index
	FLD(Floppy, dwait),
	FLDRAW("bits2", 8, 4),		// rd, wr
	FLD(Floppy, trk),
	FLD(Floppy, field),
	FLD(Floppy, pos),
	FLDEND
};

static const szxField fld_ide[] = {
	FLD(IDE, bus),
	FLD(IDE, hiTrig),
	FLD(IDE, smuc.sys),
	FLD(IDE, smuc.fdd),
	FLD(IDE, flag),
	FLD(IDE, reg),
	FLDEND
};

static const szxField fld_ata[] = {
	FLDRAW("bits", 0, 4),		// idle, standby, sleep, dma, inten, intrq
	FLD(ATADev, lba),
	FLD(ATADev, buf),
	FLD(ATADev, reg),
	FLDEND
};

static const szxField fld_sdc[] = {
	FLDRAW("bits", 0, 4),		// on, cs, acmd, checkCrc, cont, lock, busy, idle
	FLD(SDCard, state),
	FLD(SDCard, argCnt),
	FLD(SDCard, arg),
	FLD(SDCard, respCnt),
	FLD(SDCard, respPos),
	FLD(SDCard, resp),
	FLD(SDCard, blkSize),
	FLD(SDCard, addr),
	FLD(SDCard, buf),
	FLDEND
};

static const szxField fld_nv[] = {
	FLD(nvRam, state),
	FLD(nvRam, adr),
	FLD(nvRam, datain),
	FLD(nvRam, dataout),
	FLD(nvRam, bitsin),
	FLD(nvRam, bitsout),
	FLDRAW("bits", offsetof(nvRam, bitsout) + 4, 4),	// sda, scl, out, outz
	FLD(nvRam, mem),
	FLDEND
};

static const szxField fld_gs[] = {
	FLDRAW("bits", 0, 4),		// enable (the setting), reset
	FLD(GSound, pb3_gs),
	FLD(GSound, pb3_zx),
	FLD(GSound, pbb_zx),
	FLD(GSound, rp0),
	FLD(GSound, pstate),
	FLD(GSound, vol1),
	FLD(GSound, vol2),
	FLD(GSound, vol3),
	FLD(GSound, vol4),
	FLD(GSound, ch1),
	FLD(GSound, ch2),
	FLD(GSound, ch3),
	FLD(GSound, ch4),
	FLD(GSound, cnt),
	FLD(GSound, time),
	FLD(GSound, counter),
	FLDEND
};

// the tape as it plays: where it is, how far into the pulse, and what its
// automatics have seen; the image itself is the medium's
static const szxField fld_tape[] = {
	FLDRAW("bits", 0, 4),		// on, rec, isData, wait, blkChange, newBlock
	FLD(Tape, levRec),
	FLDRAW("bits2", offsetof(Tape, speed) + 4, 4),	// armed .. flash
	FLD(Tape, detectLastTick),
	FLD(Tape, detectLastPc),
	FLD(Tape, detectRegs),
	FLD(Tape, detectReads),
	FLD(Tape, detectAlien),
	FLDRAW("bits3", offsetof(Tape, detectAlien) + 4, 4),	// alien
	FLD(Tape, paused),
	FLD(Tape, portReads),
	FLD(Tape, loaderReads),
	FLD(Tape, ldBase),
	FLD(Tape, ldDir),
	FLD(Tape, ldBlock),
	FLD(Tape, inPc),
	FLD(Tape, inFrame),
	FLD(Tape, inUse),
	FLD(Tape, tickAcc),
	FLD(Tape, nsLazy),
	FLD(Tape, nsCalm),
	FLD(Tape, volPlay),
	FLD(Tape, block),
	FLD(Tape, pos),
	FLD(Tape, sigLen),
	FLDEND
};

// --- what goes in, and the tape and drives held back for the program ---

// ids of the records
#define XR_MACH	BID('M','A','C','H')
#define XR_CPU	BID('C','P','U',' ')
#define XR_COMP	BID('C','O','M','P')
#define XR_VID	BID('V','I','D',' ')
#define XR_MAP	BID('M','A','P',' ')	// what each 256-byte page of the cpu's space holds
#define XR_PAL	BID('P','A','L',' ')
#define XR_RAY	BID('R','A','Y',' ')
#define XR_TS	BID('T','S',' ',' ')
#define XR_CHIP	BID('C','H','P','0')	// ..3, the last byte is the chip
#define XR_FM	BID('F','M',' ','0')	// ..3
#define XR_BEEP	BID('B','E','E','P')
#define XR_SAA	BID('S','A','A',' ')
#define XR_SDRV	BID('S','D','R','V')
#define XR_DIF	BID('D','I','F',' ')
#define XR_FDC	BID('F','D','C',' ')
#define XR_FDCP	BID('F','D','C','P')	// the command in progress: its plan, and where in it
#define XR_FLP	BID('F','L','P','0')	// ..3
#define XR_IDE	BID('I','D','E',' ')
#define XR_ATA	BID('A','T','A','0')	// master, 1 slave
#define XR_SMNV	BID('S','M','N','V')	// SMUC's nvram
#define XR_SDC	BID('S','D','C',' ')
#define XR_GS	BID('G','S','X',' ')
#define XR_TAPE	BID('T','A','P','E')
#define XR_GSCPU	BID('G','S','C','P')
#define XR_GSRAM	BID('G','S','R','X')	// a 32K page past what the GS block holds

static void ext_gs_save(szxBuf*, Computer*);
static void ext_gs_ram(GSound*, const unsigned char*, size_t);

static int ext_chip_index(TSound* ts, aymChip* chip) {
	int i;
	for (i = 0; i < 4; i++)
		if (ts_chip(ts, i) == chip) return i;
	return 0;
}

// n of a numbered record (CHP0..3 and the like), -1 when id is not one of base's
static int xr_slot(unsigned id, unsigned base, int count) {
	int n = (int)(id >> 24) - '0';
	return (((id ^ base) & 0x00ffffff) || (n < 0) || (n >= count)) ? -1 : n;
}

// a machine the spec has no id for: its palette is its own
static int ext_own_palette(Computer* comp) {
	switch (comp->hw->id) {
		case HW_ATM1:
		case HW_ATM2:
		case HW_PENTEVO:
		case HW_TSLAB:
		case HW_PROFI:
		case HW_PHOENIX:
			return 1;
	}
	return 0;
}

int szx_ext_can_save(Computer* comp) {
	return comp && comp->hw && (comp->hw->id != HW_NULL) && (comp->hw->id != HW_DUMMY);
}

static void ext_put_string(szxBuf* d, const char* s) {
	sb_put(d, s, strlen(s) + 1);
}

static const char* szx_mac_id = "";

void szx_set_machine(const char* id) {
	szx_mac_id = id ? id : "";
}

void szx_ext_save(szxBuf* b, Computer* comp) {
	size_t mark;
	int i;
	sb_put(b, szx_ext_tag, sizeof(szx_ext_tag));

	rec_begin(b, XR_MACH, &mark);
	ext_put_string(b, comp->hw->name);
	ext_put_string(b, szx_mac_id);
	rec_end(b, mark);

	rec_fields(b, XR_CPU, comp->cpu, fld_cpu);
	rec_fields(b, XR_COMP, comp, fld_comp);
	// The pager's own map: a core rebuilds it from its ports, but not every
	// core keeps every window in a port (TSConf's #11AF..#13AF write it alone)
	rec_begin(b, XR_MAP, &mark);
	for (i = 0; i < 256; i++) {
		sb_byte(b, comp->mem->map[i].type);
		sb_dword(b, (unsigned)comp->mem->map[i].num);
	}
	rec_end(b, mark);
	rec_fields(b, XR_VID, comp->vid, fld_vid);
	if (ext_own_palette(comp))
		rec_fields(b, XR_PAL, comp->vid, fld_vid_pal);
	// where the ray is, as dots past the INT: the counters and pointers are
	// worked out from it again, the way a load at a T does it
	rec_begin(b, XR_RAY, &mark);
	sb_dword(b, (unsigned)vid_ray_dots(comp->vid));
	rec_end(b, mark);

	rec_begin(b, XR_TS, &mark);
	fld_put(b, comp->ts, fld_ts);
	sb_byte(b, ext_chip_index(comp->ts, comp->ts->curChip));
	rec_end(b, mark);
	ts_state_capture(comp->ts);
	for (i = 0; i < 4; i++) {
		aymChip* chip = ts_chip(comp->ts, i);
		void* fm = NULL;
		int fmsize;
		if (!chip || (chip->type == SND_NONE)) continue;
		// the meter's peak and the mute are the debugger's, not the chip's
		aymChip tmp = *chip;
		aymChan* ch[5] = {&tmp.chanA, &tmp.chanB, &tmp.chanC, &tmp.chanN, &tmp.chanE};
		int k;
		for (k = 0; k < 5; k++) {
			ch[k]->levpk = 0;
			ch[k]->mute = 0;
		}
		rec_fields(b, XR_CHIP + ((unsigned)i << 24), &tmp, fld_chip);
		fmsize = ts_state_range(comp->ts, i, &fm);
		if (fm && (fmsize > 0)) {
			rec_begin(b, XR_FM + ((unsigned)i << 24), &mark);
			sb_dword(b, (unsigned)fmsize);
			sb_put(b, fm, fmsize);
			rec_end(b, mark);
		}
	}
	rec_fields(b, XR_BEEP, comp->beep, fld_beep);
	if (comp->saa && comp->saa->enabled)
		rec_fields(b, XR_SAA, comp->saa, fld_saa);
	if (comp->sdrv && (comp->sdrv->type != SDRV_NONE))
		rec_fields(b, XR_SDRV, comp->sdrv, fld_sdrv);
	if (comp->dif && (comp->dif->type != DIF_NONE)) {
		rec_fields(b, XR_DIF, comp->dif, fld_dif);
		rec_fields(b, XR_FDC, comp->dif->fdc, fld_fdc);
		rec_begin(b, XR_FDCP, &mark);
		sb_dword(b, (unsigned)dif_plan_id(comp->dif));
		sb_byte(b, (comp->dif->fdc->idle ? 1 : 0) | (comp->dif->fdc->seekend ? 2 : 0));
		int sel = 0;
		while ((sel < 3) && (comp->dif->fdc->flp != comp->dif->fdc->flop[sel])) sel++;
		sb_byte(b, sel);			// the drive selected, by the controller's numbering
		rec_end(b, mark);
		for (i = 0; i < 4; i++)
			if (comp->dif->flp[i]->fitted)
				rec_fields(b, XR_FLP + ((unsigned)i << 24), comp->dif->flp[i], fld_flp);
	}
	if (comp->ide && (comp->ide->type != IDE_NONE)) {
		rec_fields(b, XR_IDE, comp->ide, fld_ide);
		rec_fields(b, XR_ATA, comp->ide->master, fld_ata);
		rec_fields(b, XR_ATA + (1u << 24), comp->ide->slave, fld_ata);
		if (comp->ide->smuc.nv)
			rec_fields(b, XR_SMNV, comp->ide->smuc.nv, fld_nv);
	}
	if (comp->sdc)
		rec_fields(b, XR_SDC, comp->sdc, fld_sdc);
	if (comp->gs && comp->gs->enable)
		ext_gs_save(b, comp);
	if (comp->tape && comp->tape->blkCount) {
		Tape tape = *comp->tape;
		tape.blkChange = 0;		// the window's to clear, not the tape's
		rec_fields(b, XR_TAPE, &tape, fld_tape);
	}
}

// --- reading ---

// the records of the creator data, one at a time; 0 at the end
static int ext_next(const unsigned char* ext, size_t len, size_t* pos, unsigned* id, const unsigned char** p, size_t* n) {
	if (*pos + 8 > len) return 0;
	*id = rd_dword(ext + *pos);
	*n = rd_dword(ext + *pos + 4);
	*p = ext + *pos + 8;
	if (*n > len - *pos - 8) return 0;
	*pos += 8 + *n;
	return 1;
}

// the creator data of a file in memory, NULL when it is no file of ours. Only
// the head of a file may be there: MACH comes first, and that is what is read.
static const unsigned char* ext_of_file(const unsigned char* file, size_t len, size_t* extlen) {
	if ((len < 16 + 36) || memcmp(file, "ZXST", 4)) return NULL;
	if (rd_dword(file + 8) != BID('C','R','T','R')) return NULL;
	size_t n = rd_dword(file + 12);
	if (n > len - 16) n = len - 16;
	if (n < 36) return NULL;
	return szx_ext_find(file + 16 + 36, n - 36, extlen);
}

// the core's name and the machine's id, out of a MACH record
static void ext_mach(const unsigned char* ext, size_t len, char* core, size_t csize, char* mac, size_t msize) {
	size_t pos = 0;
	unsigned id;
	const unsigned char* p;
	size_t n;
	if (csize) core[0] = 0;
	if (msize) mac[0] = 0;
	while (ext_next(ext, len, &pos, &id, &p, &n)) {
		if (id != XR_MACH) continue;
		size_t a = strnlen((const char*)p, n);
		if (a < n) {
			snprintf(core, csize, "%s", (const char*)p);
			snprintf(mac, msize, "%.*s", (int)strnlen((const char*)p + a + 1, n - a - 1), (const char*)p + a + 1);
		}
		return;
	}
}

int szx_ext_hardware(const unsigned char* file, size_t len) {
	size_t extlen;
	char core[64];
	const unsigned char* ext = ext_of_file(file, len, &extlen);
	if (!ext) return SNAP_HW_UNKNOWN;
	ext_mach(ext, extlen, core, sizeof(core), NULL, 0);
	HardWare* hw = core[0] ? findHardware(core) : NULL;
	return hw ? SNAP_HW_CORE + hw->id : SNAP_HW_UNKNOWN;
}

void szx_machine_of(const char* name, char* id, size_t idsize) {
	unsigned char buf[0x1000];	// the creator block comes first, MACH first in it
	char core[64];
	size_t extlen;
	FILE* file = fopen(name, "rb");
	if (idsize) id[0] = 0;
	if (!file) return;
	size_t n = fread(buf, 1, sizeof(buf), file);
	fclose(file);
	const unsigned char* ext = ext_of_file(buf, n, &extlen);
	if (ext) ext_mach(ext, extlen, core, sizeof(core), id, idsize);
}

const unsigned char* szx_ext_find(const unsigned char* data, size_t len, size_t* extlen) {
	if ((len < sizeof(szx_ext_tag)) || memcmp(data, szx_ext_tag, sizeof(szx_ext_tag))) return NULL;
	if (extlen) *extlen = len - sizeof(szx_ext_tag);
	return data + sizeof(szx_ext_tag);
}

// The tape and the drives are put in by the program after the load, so what
// they were doing is kept here until it has: szx_ext_media() applies it.
static unsigned char* ext_held = NULL;
static size_t ext_held_len = 0;

// the same as EXT_TAKE below for a struct with megabytes of data after its
// head: the copy is on the heap and only the head is copied, which is all the
// table reaches
static void* ext_take_head(const void* obj, size_t head, const szxField* tab, const unsigned char* p, size_t n) {
	void* tmp = malloc(head);
	if (!tmp) return NULL;
	memcpy(tmp, obj, head);
	fld_get(tmp, tab, p, n);
	return tmp;
}

// a field table taken into a copy of the object, so a caller can pick the
// members it wants out of bit fields that hold settings too
#define EXT_TAKE(type, obj, tab, p, n) \
	type tmp_; memcpy(&tmp_, (obj), sizeof(type)); fld_get(&tmp_, (tab), (p), (n));

static void ext_cpu_into(CPU* cpu, const unsigned char* p, size_t n) {
	EXT_TAKE(CPU, cpu, fld_cpu, p, n)
	int i;
	memcpy(cpu->regs, tmp_.regs, sizeof(cpu->regs));
	for (i = 0; cpu_state_flags[i] >= 0; i++)
		cpu->flags[cpu_state_flags[i]] = tmp_.flags[cpu_state_flags[i]];
	cpu->intrq = tmp_.intrq;
	cpu->inten = tmp_.inten;
	cpu->intvec = tmp_.intvec;
}

static void ext_comp(Computer* comp, const unsigned char* p, size_t n) {
	bool sys[32];
	int i;
	double mul = comp->hwMul;
	memcpy(sys, comp->sysflag, sizeof(sys));
	fld_get(comp, fld_comp, p, n);
	// of the shared flags only the machine's own; the rest are the debugger's
	// and the settings'
	for (i = 0; i < 32; i++) {
		int k = 0;
		while ((comp_state_flags[k] >= 0) && (comp_state_flags[k] != i)) k++;
		if (comp_state_flags[k] < 0) comp->sysflag[i] = sys[i];
	}
	// the board's own turbo, set from its port, through what works out the timings
	double was = comp->hwMul;
	comp->hwMul = mul;
	if (was > 0) compSetHwTurbo(comp, was);
	comp->hw->mapMem(comp);
}

// After the core has paged from its ports: a page it did not put back is set
// plainly, ram or rom, which is all a window paged from a port of its own is
static void ext_map(Computer* comp, const unsigned char* p, size_t n) {
	int i;
	if (n < 256 * 5) return;
	for (i = 0; i < 256; i++) {
		int type = p[i * 5];
		int num = (int)rd_dword(p + i * 5 + 1);
		MemPage* pg = &comp->mem->map[i];
		if ((pg->type == type) && (pg->num == num)) continue;
		if ((type == MEM_RAM) || (type == MEM_ROM))
			memSetBank(comp->mem, i, type, num, MEM_256, NULL, NULL, NULL);
	}
}

static void ext_vid(Computer* comp, const unsigned char* p, size_t n) {
	Video* vid = comp->vid;
	EXT_TAKE(Video, vid, fld_vid, p, n)
	vid->intLINE = tmp_.intLINE;
	vid->intDMA = tmp_.intDMA;
	vid->intFRAME = tmp_.intFRAME;
	vid->intlen = tmp_.intlen;
	vid->nsDrawFixed = tmp_.nsDrawFixed;
	vid->nsOwedFixed = tmp_.nsOwedFixed;
	vid->nsCalmFixed = tmp_.nsCalmFixed;
	vid->time = tmp_.time;
	vid->busy = tmp_.busy;
	vid->flash = tmp_.flash;
	vid->vidPage = tmp_.vidPage;
	vid->brdcol = tmp_.brdcol;
	vid->nextbrd = tmp_.nextbrd;
	vid->inten = tmp_.inten;
	vid->intrq = tmp_.intrq;
	vid->intbf = tmp_.intbf;
	vid->paln = tmp_.paln;
	vid->fcnt = tmp_.fcnt;
	vid->lcnt = tmp_.lcnt;
	vid->atrbyte = tmp_.atrbyte;
	vid->fntbyte = tmp_.fntbyte;
	vid->snowLow = tmp_.snowLow;
	vid->snowBank = tmp_.snowBank;
	vid->intp = tmp_.intp;
	vid->intsize = tmp_.intsize;
	vid->sc = tmp_.sc;
	vid->scrsize = tmp_.scrsize;
	vid->inth = tmp_.inth;
	vid->intf = tmp_.intf;
	vid->idx = tmp_.idx;
	memcpy(&vid->tsconf, &tmp_.tsconf, sizeof(vid->tsconf));
	memcpy(vid->line, tmp_.line, sizeof(vid->line));
	memcpy(vid->linb, tmp_.linb, sizeof(vid->linb));
	if (tmp_.vmode != vid->vmode)
		vid_set_mode(vid, tmp_.vmode);
}

static void ext_chip_load(Computer* comp, int n, const unsigned char* p, size_t len) {
	aymChip* chip = ts_chip(comp->ts, n);
	int i;
	if (!chip || (chip->type == SND_NONE)) {
		xlog(XLG_FILE, XLL_INFO, "szx: the snapshot has a sound chip %i, this machine has none", n + 1);
		return;
	}
	EXT_TAKE(aymChip, chip, fld_chip, p, len)
	// the registers the way a port writes them, so whatever follows from them
	// is set; then the generators where they were
	for (i = 0; i < 16; i++)
		ay_poke_reg(chip, i, tmp_.reg[i]);
	memcpy(chip->reg, tmp_.reg, sizeof(chip->reg));
	aymChan* dst[5] = {&chip->chanA, &chip->chanB, &chip->chanC, &chip->chanN, &chip->chanE};
	aymChan* src[5] = {&tmp_.chanA, &tmp_.chanB, &tmp_.chanC, &tmp_.chanN, &tmp_.chanE};
	for (i = 0; i < 5; i++) {
		unsigned mute = dst[i]->mute;
		int peak = dst[i]->levpk;
		*dst[i] = *src[i];
		dst[i]->mute = mute;
		dst[i]->levpk = peak;
	}
	chip->eForm = tmp_.eForm;
	chip->tickAcc = tmp_.tickAcc;
	chip->pendNs = tmp_.pendNs;
	chip->wait = tmp_.wait;
	chip->blk_fm = tmp_.blk_fm;
	chip->curReg = tmp_.curReg;
}

static void ext_fm_load(Computer* comp, int n, const unsigned char* p, size_t len) {
	void* fm = NULL;
	int size;
	if (len < 4) return;
	size = ts_state_range(comp->ts, n, &fm);
	if (!fm || (size <= 0)) return;
	if (((size_t)size != rd_dword(p)) || ((size_t)size > len - 4)) {
		xlog(XLG_FILE, XLL_INFO, "szx: fm state of another size, the fm half starts over");
		return;
	}
	ts_state_capture(comp->ts);		// the other chips' as they are
	memcpy(fm, p + 4, size);
	ts_state_restore(comp->ts);
}

static void ext_fdc(Computer* comp, const unsigned char* p, size_t n) {
	FDC* fdc = comp->dif->fdc;
	FDC* t = (FDC*)ext_take_head(fdc, offsetof(FDC, slst), fld_fdc, p, n);
	if (!t) return;
	fdc->irq = t->irq;
	fdc->drq = t->drq;
	fdc->dir = t->dir;
	fdc->mr = t->mr;
	fdc->block = t->block;
	fdc->side = t->side;
	fdc->step = t->step;
	fdc->mfm = t->mfm;
	fdc->crchi = t->crchi;
	fdc->intr = t->intr;
	fdc->trk = t->trk;
	fdc->sec = t->sec;
	fdc->data = t->data;
	fdc->com = t->com;
	fdc->state = t->state;
	fdc->tmp = t->tmp;
	fdc->wdata = t->wdata;
	fdc->tdata = t->tdata;
	fdc->bytedelay = t->bytedelay;
	fdc->crc = t->crc;
	fdc->fcrc = t->fcrc;
	memcpy(fdc->buf, t->buf, sizeof(fdc->buf));
	fdc->fmode = t->fmode;
	fdc->cnt = t->cnt;
	fdc->wait = t->wait;
	fdc->tns = t->tns;
	fdc->hold = t->hold;
	fdc->drdy = t->drdy;
	fdc->pos = t->pos;
	fdc->hlt = t->hlt;
	fdc->hut = t->hut;
	fdc->srt = t->srt;
	memcpy(fdc->comBuf, t->comBuf, sizeof(fdc->comBuf));
	fdc->comCnt = t->comCnt;
	fdc->comPos = t->comPos;
	memcpy(fdc->resBuf, t->resBuf, sizeof(fdc->resBuf));
	fdc->resCnt = t->resCnt;
	fdc->resPos = t->resPos;
	fdc->sr0 = t->sr0;
	fdc->sr1 = t->sr1;
	fdc->sr2 = t->sr2;
	fdc->sr3 = t->sr3;
	free(t);
	// the selected drive, out of the system register
	if (comp->dif->type == DIF_BDI)
		fdc->flp = fdc->flop[comp->dif->sys & 3];
}

static void ext_flp(Floppy* flp, const unsigned char* p, size_t n) {
	Floppy* t = (Floppy*)ext_take_head(flp, offsetof(Floppy, path), fld_flp, p, n);
	if (!t) return;
	flp->motor = t->motor;
	flp->virt = t->virt;
	flp->door = t->door;
	flp->index = t->index;
	flp->dwait = t->dwait;
	flp->trk = t->trk;
	flp->field = t->field;
	flp->pos = t->pos;
	free(t);
}

static void ext_ata(ATADev* ata, const unsigned char* p, size_t n) {
	if (!ata) return;
	EXT_TAKE(ATADev, ata, fld_ata, p, n)
	ata->idle = tmp_.idle;
	ata->standby = tmp_.standby;
	ata->sleep = tmp_.sleep;
	ata->dma = tmp_.dma;
	ata->inten = tmp_.inten;
	ata->intrq = tmp_.intrq;
	ata->lba = tmp_.lba;
	memcpy(&ata->buf, &tmp_.buf, sizeof(ata->buf));
	memcpy(&ata->reg, &tmp_.reg, sizeof(ata->reg));
}

static void ext_saa(saaChip* saa, const unsigned char* p, size_t n) {
	EXT_TAKE(saaChip, saa, fld_saa, p, n)
	saa->off = tmp_.off;
	saa->time = tmp_.time;
	saa->curReg = tmp_.curReg;
	memcpy(saa->chan, tmp_.chan, sizeof(saa->chan));
	memcpy(saa->noiz, tmp_.noiz, sizeof(saa->noiz));
	memcpy(saa->env, tmp_.env, sizeof(saa->env));
}

static void ext_gs(GSound* gs, const unsigned char* p, size_t n) {
	EXT_TAKE(GSound, gs, fld_gs, p, n)
	gs->reset = tmp_.reset;
	gs->pb3_gs = tmp_.pb3_gs;
	gs->pb3_zx = tmp_.pb3_zx;
	gs->pbb_zx = tmp_.pbb_zx;
	gs->pstate = tmp_.pstate;
	gs->vol1 = tmp_.vol1;
	gs->vol2 = tmp_.vol2;
	gs->vol3 = tmp_.vol3;
	gs->vol4 = tmp_.vol4;
	gs->ch1 = tmp_.ch1;
	gs->ch2 = tmp_.ch2;
	gs->ch3 = tmp_.ch3;
	gs->ch4 = tmp_.ch4;
	gs->cnt = tmp_.cnt;
	gs->time = tmp_.time;
	gs->counter = tmp_.counter;
	gsiowr(0, tmp_.rp0, gs);		// the page register, which maps #8000 as it says
}

int szx_ext_load(Computer* comp, const unsigned char* ext, size_t len) {
	size_t pos = 0;
	unsigned id;
	const unsigned char* p;
	size_t n;
	int ray = -1;
	char core[64];
	char mac[256];
	free(ext_held);
	ext_held = NULL;
	ext_held_len = 0;
	// The ports, the flags and the video mean what they mean on one core only:
	// a 128K snapshot of ours run on a Pentagon takes the sound chips and no more.
	ext_mach(ext, len, core, sizeof(core), mac, sizeof(mac));
	int same = !strcmp(core, comp->hw->name);
	if (!same)
		xlog(XLG_FILE, XLL_INFO, "szx: taken on %s, run on %s: only the sound chips' own state is carried over",
			core[0] ? core : "?", comp->hw->name);
	while (ext_next(ext, len, &pos, &id, &p, &n)) {
		int idx;
		if (!same && (xr_slot(id, XR_CHIP, 4) < 0) && (xr_slot(id, XR_FM, 4) < 0) && (id != XR_TS))
			continue;
		switch (id) {
			case XR_CPU: ext_cpu_into(comp->cpu, p, n); break;
			case XR_COMP: ext_comp(comp, p, n); break;
			case XR_VID: ext_vid(comp, p, n); break;
			case XR_MAP: ext_map(comp, p, n); break;
			case XR_PAL:
				if (ext_own_palette(comp)) fld_get(comp->vid, fld_vid_pal, p, n);
				break;
			case XR_RAY: if (n >= 4) ray = (int)rd_dword(p); break;
			case XR_TS:
				if (n > 0) {
					EXT_TAKE(TSound, comp->ts, fld_ts, p, n - 1)
					aymChip* cur = ts_chip(comp->ts, p[n - 1]);
					comp->ts->mute_l = tmp_.mute_l;
					comp->ts->mute_r = tmp_.mute_r;
					comp->ts->r_stat = tmp_.r_stat;
					if (cur) comp->ts->curChip = cur;
				}
				break;
			case XR_BEEP: fld_get(comp->beep, fld_beep, p, n); break;
			case XR_SAA:
				if (comp->saa && comp->saa->enabled) ext_saa(comp->saa, p, n);
				break;
			case XR_SDRV:
				if (comp->sdrv && (comp->sdrv->type != SDRV_NONE)) fld_get(comp->sdrv, fld_sdrv, p, n);
				break;
			case XR_DIF:
				if (comp->dif && (comp->dif->type != DIF_NONE)) {
					EXT_TAKE(DiskIF, comp->dif, fld_dif, p, n)
					comp->dif->inten = tmp_.inten;
					comp->dif->sys = tmp_.sys;
				}
				break;
			case XR_FDC:
				if (comp->dif && (comp->dif->type != DIF_NONE)) ext_fdc(comp, p, n);
				break;
			case XR_FDCP:
				if (comp->dif && (comp->dif->type != DIF_NONE) && (n >= 5) && ((int)rd_dword(p) >= 0)) {
					dif_plan_set(comp->dif, (int)rd_dword(p));
					comp->dif->fdc->idle = p[4] & 1;
					comp->dif->fdc->seekend = (p[4] >> 1) & 1;
					if (n >= 6) comp->dif->fdc->flp = comp->dif->fdc->flop[p[5] & 3];
				}
				break;
			case XR_IDE:
				if (comp->ide && (comp->ide->type != IDE_NONE)) fld_get(comp->ide, fld_ide, p, n);
				break;
			case XR_SMNV:
				if (comp->ide && comp->ide->smuc.nv) fld_get(comp->ide->smuc.nv, fld_nv, p, n);
				break;
			case XR_SDC:
				if (comp->sdc) fld_get(comp->sdc, fld_sdc, p, n);
				break;
			case XR_GS:
				if (comp->gs && comp->gs->enable) ext_gs(comp->gs, p, n);
				break;
			case XR_GSCPU:
				if (comp->gs && comp->gs->enable) ext_cpu_into(comp->gs->cpu, p, n);
				break;
			case XR_GSRAM:
				if (comp->gs && comp->gs->enable) ext_gs_ram(comp->gs, p, n);
				break;
			case XR_FLP + (0u << 24): case XR_FLP + (1u << 24):
			case XR_FLP + (2u << 24): case XR_FLP + (3u << 24):
			case XR_TAPE:
				break;		// held until the media are in, below
			default:
				if ((idx = xr_slot(id, XR_CHIP, 4)) >= 0) {
					ext_chip_load(comp, idx, p, n);
				} else if ((idx = xr_slot(id, XR_FM, 4)) >= 0) {
					ext_fm_load(comp, idx, p, n);
				} else if ((idx = xr_slot(id, XR_ATA, 2)) >= 0) {
					if (comp->ide && (comp->ide->type != IDE_NONE))
						ext_ata(idx ? comp->ide->slave : comp->ide->master, p, n);
				} else if (id != XR_MACH) {
					xlog(XLG_FILE, XLL_DEBUG, "szx: record %.4s of a newer build skipped", (const char*)&id);
				}
				break;
		}
	}
	ext_held = same ? (unsigned char*)malloc(len ? len : 1) : NULL;
	if (ext_held) {
		memcpy(ext_held, ext, len);
		ext_held_len = len;
	}
	// the ray last: the mode and the INT position above decide what a dot is
	if (same && (ray >= 0)) {
		int frame = comp->vid->intFRAME;
		int length = comp->vid->intlen;
		vid_set_ray(comp->vid, ray);
		comp->vid->intFRAME = frame;
		comp->vid->intlen = length;
		return 1;
	}
	return 0;
}

// What the tape and the drives were doing, once the program has put the media
// back. A medium that did not come back keeps what loading it set.
void szx_ext_media(Computer* comp) {
	size_t pos = 0;
	unsigned id;
	const unsigned char* p;
	size_t n;
	szx_media_clear();		// the images carried in the file are in by now
	if (!ext_held) return;
	while (ext_next(ext_held, ext_held_len, &pos, &id, &p, &n)) {
		int idx;
		if ((id == XR_TAPE) && comp->tape->blkCount) {
			Tape* tape = comp->tape;
			EXT_TAKE(Tape, tape, fld_tape, p, n)
			if (tmp_.block >= tape->blkCount) continue;
			tape->on = tmp_.on;
			tape->rec = tmp_.rec;
			tape->isData = tmp_.isData;
			tape->wait = tmp_.wait;
			tape->armed = tmp_.armed;
			tape->tail = tmp_.tail;
			tape->userStop = tmp_.userStop;
			tape->autoPlay = tmp_.autoPlay;
			tape->alien = tmp_.alien;
			tape->detectLastTick = tmp_.detectLastTick;
			tape->detectLastPc = tmp_.detectLastPc;
			memcpy(tape->detectRegs, tmp_.detectRegs, sizeof(tape->detectRegs));
			tape->detectReads = tmp_.detectReads;
			tape->detectAlien = tmp_.detectAlien;
			tape->paused = tmp_.paused;
			tape->portReads = tmp_.portReads;
			tape->loaderReads = tmp_.loaderReads;
			tape->ldBase = tmp_.ldBase;
			tape->ldDir = tmp_.ldDir;
			tape->ldBlock = tmp_.ldBlock;
			tape->inPc = tmp_.inPc;
			tape->inFrame = tmp_.inFrame;
			tape->inUse = tmp_.inUse;
			tape->tickAcc = tmp_.tickAcc;
			tape->nsLazy = tmp_.nsLazy;
			tape->nsCalm = tmp_.nsCalm;
			tape->volPlay = tmp_.volPlay;
			tape->block = tmp_.block;
			tape->pos = tmp_.pos;
			tape->sigLen = tmp_.sigLen;
		} else if ((id == XR_DIF) && comp->dif && (comp->dif->type != DIF_NONE)) {
			// putting a disk in starts its door closing; the snapshot's had closed
			EXT_TAKE(DiskIF, comp->dif, fld_dif, p, n)
			comp->dif->doors = tmp_.doors;
		} else if ((idx = xr_slot(id, XR_FLP, 4)) >= 0) {
			if (comp->dif && (comp->dif->type != DIF_NONE))
				ext_flp(comp->dif->flp[idx], p, n);
		}
	}
	free(ext_held);
	ext_held = NULL;
	ext_held_len = 0;
}

// --- General Sound ---

// The GS block is the GS cpu and its ports, GSRP its ram in 32K pages: page n
// is what the page register maps at #8000 as n + 1 (0 is the rom), and the
// first 16K of page 0 is also at #4000. The spec stops at 512K; a GS of ours
// has 2M, and the pages past 14 go in our own data.

#define SZG_EILAST	1
#define SZG_HALTED	2
#define SZG_CUSTOMROM	64
#define SZG_PAGES	15		// a GS512's

void szx_rd_gs(Computer* comp, const unsigned char* p, size_t n) {
	GSound* gs = comp->gs;
	if (n < 46) return;
	if (!gs || !gs->enable) {
		xlog(XLG_FILE, XLL_INFO, "szx: the snapshot has a General Sound, this machine has it off");
		return;
	}
	CPU* cpu = gs->cpu;
	if (p[10] & SZG_CUSTOMROM)
		xlog(XLG_FILE, XLL_INFO, "szx: custom GS rom not taken, the machine's own is used");
	gs->vol1 = p[2] & 0x3f;
	gs->vol2 = p[3] & 0x3f;
	gs->vol3 = p[4] & 0x3f;
	gs->vol4 = p[5] & 0x3f;
	gs->ch1 = p[6];
	gs->ch2 = p[7];
	gs->ch3 = p[8];
	gs->ch4 = p[9];
	cpu->flgNOINT = (p[10] & SZG_EILAST) ? 1 : 0;
	cpu->flgHALT = (p[10] & SZG_HALTED) ? 1 : 0;
	szx_rd_regs(cpu, p + 11, 0);
	// 40: the GS's own T into its frame, 44: what is left of its INT, 45: the
	// BIT n,(HL) register - our GS counts its time its own way
	gsiowr(0, p[1], gs);
}

void szx_rd_gsrp(Computer* comp, const unsigned char* p, size_t n) {
	GSound* gs = comp->gs;
	unsigned char buf[MEM_32K];
	if (!gs || !gs->enable || (n < 3)) return;
	int page = p[2];
	int ok = (rd_word(p) & SZR_COMPRESSED) ? szx_inflate(p + 3, n - 3, buf, MEM_32K)
			: ((n - 3 >= MEM_32K) && memcpy(buf, p + 3, MEM_32K));
	if (!ok || ((page + 1) * MEM_32K > gs->mem->ramSize)) {
		xlog(XLG_FILE, XLL_WARN, "szx: GS ram page %i is broken or past its memory", page);
		return;
	}
	memcpy(gs->mem->ramData + page * MEM_32K, buf, MEM_32K);
}

void szx_wr_gs(szxBuf* b, szxBuf* d, Computer* comp) {
	GSound* gs = comp->gs;
	CPU* cpu = gs->cpu;
	int i;
	sb_byte(d, 1);				// a GS512, the biggest the spec has
	sb_byte(d, gs->rp0 & 0x1f);
	sb_byte(d, gs->vol1 & 0x3f);
	sb_byte(d, gs->vol2 & 0x3f);
	sb_byte(d, gs->vol3 & 0x3f);
	sb_byte(d, gs->vol4 & 0x3f);
	sb_byte(d, gs->ch1);
	sb_byte(d, gs->ch2);
	sb_byte(d, gs->ch3);
	sb_byte(d, gs->ch4);
	sb_byte(d, cpu->flgNOINT ? SZG_EILAST : (cpu->flgHALT ? SZG_HALTED : 0));
	szx_wr_regs(d, cpu);
	sb_dword(d, 0);
	sb_byte(d, 0);
	sb_byte(d, cpu->regWZh);
	sb_block(b, BID('G','S',0,0), d);
	for (i = 0; (i < SZG_PAGES) && ((i + 1) * MEM_32K <= gs->mem->ramSize); i++)
		szx_wr_page(b, d, BID('G','S','R','P'), i, gs->mem->ramData + i * MEM_32K, MEM_32K);
}

// the GS's own state past the GS block: its cpu exactly, and its ram past 512K
static void ext_gs_save(szxBuf* b, Computer* comp) {
	GSound* gs = comp->gs;
	size_t mark;
	int i;
	rec_fields(b, XR_GS, gs, fld_gs);
	rec_fields(b, XR_GSCPU, gs->cpu, fld_cpu);
	for (i = SZG_PAGES; (i + 1) * MEM_32K <= gs->mem->ramSize; i++) {
		rec_begin(b, XR_GSRAM, &mark);
		sb_byte(b, i);
		sb_deflate(b, gs->mem->ramData + i * MEM_32K, MEM_32K);
		rec_end(b, mark);
	}
}

static void ext_gs_ram(GSound* gs, const unsigned char* p, size_t n) {
	if (n < 2) return;
	int page = p[0];
	if ((page + 1) * MEM_32K > gs->mem->ramSize) return;
	unsigned char* dst = gs->mem->ramData + page * MEM_32K;
	if (n - 1 == MEM_32K) {
		memcpy(dst, p + 1, MEM_32K);
	} else if (!szx_inflate(p + 1, n - 1, dst, MEM_32K)) {
		xlog(XLG_FILE, XLL_WARN, "szx: GS ram page %i is broken", page);
	}
}

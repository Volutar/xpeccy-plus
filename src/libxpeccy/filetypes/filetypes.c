#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "filetypes.h"

#ifdef WORDS_BIG_ENDIAN

// swap 16/32 bit variables (LE <-> BE)
unsigned short swap16(unsigned short wr) {
	return ((wr & 0xff) << 8) | ((wr >> 8) & 0xff);
}

unsigned int swap32(unsigned int wr) {
	unsigned int res = ((wr & 0xff) << 24);
	res |= ((wr & 0xff00) << 8);
	res |= ((wr >> 8) & 0xff00);
	res |= ((wr >> 24) & 0xff);
	return res;
}

#else

unsigned short swap16(unsigned short wr) {return wr;}
unsigned int swap32(unsigned int wr) {return wr;}

#endif

// get filesize & rewind it
size_t fgetSize(FILE* file) {
	fseek(file, 0, SEEK_END);
	size_t res = ftell(file);
	rewind(file);
	return res;
}

// A scratch file to unpack into. Not tmpfile(): on Windows that asks for its
// file in the root of the current drive, which an ordinary user cannot write
// to, so it always failed and took the caller with it. This makes one in the
// directory the environment names instead.
FILE* fopen_tmp(void) {
	static int cnt = 0;
	const char* dir;
	char path[1024];
	FILE* file = NULL;
#if defined(_WIN32)
	dir = getenv("TEMP");
	if (!dir) dir = getenv("TMP");
	if (!dir) dir = ".";
#else
	dir = getenv("TMPDIR");
	if (!dir) dir = "/tmp";
#endif
	// the pid too: two instances opening a file in the same second shared one
	snprintf(path, sizeof(path), "%s%sxpeccy-%u-%u-%i.tmp", dir, SLASH, (unsigned)getpid(), (unsigned)time(NULL), cnt++);
#if defined(_WIN32)
	// D: the crt drops the file when the last handle on it closes. Not every
	// crt takes the flag, and then the file is left for TEMP's own housekeeping
	file = fopen(path, "w+bD");
#endif
	if (!file) {
		file = fopen(path, "w+b");
#if !defined(_WIN32)
		if (file) remove(path);		// gone the moment it is closed
#endif
	}
	return file;
}

enum {ED_NONE = 0, ED_SINCLAIR, ED_PLUS2};

// The 128 editor a machine pages in with 7FFD bit 4 clear. The clones all carry
// Sinclair's (a few bytes patched, the same addresses); the +2's was assembled
// again, its addresses moved from #9B on; the +2A/+3 and the Next have four ROMs
// of their own, and the ALF its games menu.
static int snap_editor(int hwid, int model) {
	switch (hwid) {
		case HW_ZX48:
		case HW_ROBIK:
		case HW_PLUS2A:
		case HW_PLUS3:
		case HW_ZXNEXT:
		case HW_ALF: return ED_NONE;
		case HW_ZX128: return (model == SNAP_HW_PLUS2) ? ED_PLUS2 : ED_SINCLAIR;
	}
	return ED_SINCLAIR;
}

// pc is where the machine was: for a snapshot taken as the interrupt came (PC
// #0038) the caller passes the address on top of the stack. Running the editor
// (paged in, the PC under #4000) it goes on only on its model: the clones patch
// the very menu. With the editor paged in, or the PC in 48 BASIC - which 128
// BASIC calls, its own addresses on the stack - it needs that editor. One in
// RAM with 48 BASIC paged is a program running.
int snap_rom_flags(int pc, int p7ffd) {
	int editor = !(p7ffd & 0x10);
	int rom = (pc < 0x4000);
	return ((editor || rom) ? SNAP_EDITOR : 0) | ((editor && rom) ? SNAP_IN_ROM : 0);
}

// One stopped in its ROM goes on only on the model it names: the clones patch
// the very menu (the Pentagon's TR-DOS item), and the +2's editor is another
// build.
static int snap_on_model(int snap, int hwid, int model) {
	int ed = snap_editor(hwid, model);
	switch (snap) {
		case SNAP_HW_128K: return (hwid == HW_ZX128) && (ed == ED_SINCLAIR);
		case SNAP_HW_PLUS2: return ed == ED_PLUS2;
		case SNAP_HW_PENTAGON: return (hwid == HW_PENT) || (hwid == HW_P1024);
	}
	return 0;
}

// can a machine on that core run a snapshot taken on that hardware: the paging
// has to be there, the editor paged in, and for one running the ROM, that ROM
int snapHwRuns(int snap, int hwid, int model) {
	int rom = snap & SNAP_IN_ROM;
	int editor = snap & SNAP_EDITOR;
	snap = SNAP_HW_OF(snap);
	if (snap >= SNAP_HW_CORE) return hwid == snap - SNAP_HW_CORE;
	switch (snap) {
		case SNAP_HW_128K:
		case SNAP_HW_PENTAGON:
		case SNAP_HW_PLUS2:
			if (rom) return snap_on_model(snap, hwid, model);
			if (editor) return snap_editor(hwid, model) == ((snap == SNAP_HW_PLUS2) ? ED_PLUS2 : ED_SINCLAIR);
			return (hwid != HW_ZX48) && (hwid != HW_ROBIK);
		// one ROM, but a +3 in its editor has +3DOS set up for a drive the +2A lacks
		case SNAP_HW_PLUS2A: return (hwid == HW_PLUS2A) || (!rom && (hwid == HW_PLUS3));
		case SNAP_HW_PLUS3: return (hwid == HW_PLUS3) || (!rom && (hwid == HW_PLUS2A));
		case SNAP_HW_SCORPION: return (hwid == HW_SCORP) || (hwid == HW_SCORPTP);
		// pages past 128K through bits 6 and 7 of 7FFD
		case SNAP_HW_PENT512: return (hwid == HW_PENT) || (hwid == HW_P1024);
		case SNAP_HW_PENT1024: return hwid == HW_P1024;
	}
	return 1;			// a 48K, or one nobody knows
}

// An rzx replays an exact instruction stream, so the machine has to be the one
// it was recorded on, not merely one that can run the snapshot: the frame is a
// different length on each, and playback then runs out of input part way in.
int snapHwIs(int snap, int hwid) {
	snap = SNAP_HW_OF(snap);
	if (snap >= SNAP_HW_CORE) return hwid == snap - SNAP_HW_CORE;
	switch (snap) {
		case SNAP_HW_48K: return hwid == HW_ZX48;
		case SNAP_HW_128K:
		case SNAP_HW_PLUS2: return hwid == HW_ZX128;
		case SNAP_HW_PLUS2A: return hwid == HW_PLUS2A;
		case SNAP_HW_PLUS3: return hwid == HW_PLUS3;
		case SNAP_HW_PENTAGON: return hwid == HW_PENT;
		case SNAP_HW_SCORPION: return (hwid == HW_SCORP) || (hwid == HW_SCORPTP);
		case SNAP_HW_PENT512: return hwid == HW_PENT;
		case SNAP_HW_PENT1024: return hwid == HW_P1024;
	}
	return 1;			// one nobody knows: leave the machine alone
}

// A snapshot that says nothing of 1FFD, loaded on a +2A/+3: the +3 ROM pages
// 48 BASIC (ROM 3) in by 1FFD bit 2 together with 7FFD bit 4 (its routine at
// #3F2A), so a program it runs in 48 BASIC has 1FFD #04; with bit 4 clear it is
// the editor, ROM 0, and 1FFD 0 (a .sna saved on a +2A in its menu). Before
// 7FFD is written: the +3 takes 1FFD only while 7FFD is unlocked.
void snap_plus3_basic48(Computer* comp, int snap, int p7ffd) {
	snap = SNAP_HW_OF(snap);
	if ((comp->hw->id != HW_PLUS2A) && (comp->hw->id != HW_PLUS3)) return;
	if (snap >= SNAP_HW_CORE) return;	// a file of ours brings its machine's own ports
	comp->hw->out(comp, 0x1ffd, (p7ffd & 0x10) ? 0x04 : 0x00);
}

int fgetw(FILE* file) {
	int res = fgetc(file) & 0xff;
	res |= ((fgetc(file) & 0xff) << 8);
	return res & 0xffff;
}

int fgett(FILE* file) {
	int res = fgetw(file);
	res |= ((fgetc(file) & 0xff) << 16);
	return res & 0xffffff;
}

int fgeti(FILE* file) {
	unsigned int res = fgetw(file);
	res |= ((unsigned int)fgetw(file) << 16);	// unsigned: a top bit set is no overflow
	return (int)res;
}

void fputw(unsigned short val, FILE* file) {
	fputc(val & 0xff, file);
	fputc((val >> 8) & 0xff, file);
}

void fputi(int val, FILE* file) {
	fputw(val & 0xffff, file);
	fputw((val >> 16) & 0xffff, file);
}

// load boot PATH to floppy FLP, if there is TRDOS disk
void boot_read(Floppy* flp, const char* path) {
	if (diskGetType(flp) != DISK_TYPE_TRD) return;
	TRFile cat[128];
	int catSize = diskGetTRCatalog(flp, cat);
	int gotBoot = 0;
	for (int i = 0; i < catSize; i++) {
		if (strncmp((char*)cat[i].name, "boot    B", 9) == 0)
			gotBoot = 1;
	}
	if (gotBoot) return;
	hobeta_read(flp, path);
	flp->changed = 0;
}

void loadBoot(Computer* comp, const char* path, int drv) {
	boot_read(comp->dif->flp[drv & 3], path);
}

// every track a disk read in can have: what the drive held before is gone
void diskClear(Floppy* flp) {
	for (int i = 0; i < 256; i++) {
		memset(flp->data[i].byte, 0, flp->trklen);
		flpFillFields(flp, i, 0);
	}
	flp->cyls = 0;
}

// trdos

static unsigned char trd_8e0[] = {
	0x00,0x00,0x01,0x16,0x00,0xf0,0x09,0x10,0x00,0x00,0x20,0x20,0x20,0x20,0x20,0x20,
	0x20,0x20,0x20,0x00,0x00,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00
};

// format whole disk as 168x16x256 and init as TRDOS
void trd_format(Floppy* flp) {
	int i;
	unsigned char buf[0x1000];
	memset(buf, 0, 0x1000);
	for (i = 1;i < 168; i++) diskFormTRDTrack(flp,i,buf);
	memcpy(buf + 0x8e0, trd_8e0, 0x20);
	diskFormTRDTrack(flp,0,buf);
}

// build a single track 16x256 (TRDOS), sector data @bpos (4K)
void diskFormTRDTrack(Floppy* flp, int tr, unsigned char* bpos) {
	Sector lst[16];
	Sector sct;
	int sc;
	unsigned char* ppos = bpos;
	sct.type = 0xfb;
	sct.flags = 0;
	sct.crc = -1;
	sct.trk = ((tr & 0xfe) >> 1);
	sct.head = (tr & 0x01) ? 1 : 0;
	sct.sz = 1;
	for (sc = 0; sc < 16; sc++) {
		sct.sec = sc + 1;
		memcpy(sct.data, ppos, 256);
		lst[sc] = sct;
		ppos += 256;
	}
	diskFormTrack(flp,tr,lst,16);
}

// a track's gaps: 4E before an ID, 00 before each mark, 4E between an ID and its data
typedef struct {
	int gap1;
	int sync;
	int gap2;
} formGaps;

static const formGaps form_std = {10, 12, 22};
static const formGaps form_tight = {2, 3, 4};	// an image's track the standard one does not hold

// the bytes a sector's data counts for in the gap
static long long sec_counted(Sector* s) {
	// A size code past 23 overflowed the shift, which on x86 came out as 0
	// (221 -> 128 << 29 -> 0). Protected disks carry such codes in an id
	// field, and Kobrahsoft's SP4/SP5/DB1 boot only with the layout that
	// gave: so they count as nothing here, on purpose, not 128 << (sz & 3).
	// Not checked against a real drive.
	return (s->sz < 24) ? (128LL << s->sz) : 0;
}

// the bytes a sector takes before the gap after it, with len bytes of data
static long long sec_room(Sector* s, long long len, const formGaps* g) {
	long long n = 0;
	if (!(s->flags & SEC_NO_ID))
		n += g->gap1 + g->sync + 10;		// 3 A1, FE, CHRN, CRC
	if (!(s->flags & SEC_NO_DATA))
		n += ((s->flags & SEC_NO_ID) ? g->gap1 : g->gap2) + g->sync + 4 + len + 2;
	return n;
}

// 4E past gap3 after an ID with no data, so that the WD1793 finds no data mark
// in its window and the ID reads as Record Not Found
static long long sec_pad(Sector* sdata, int i, int scount, const formGaps* g, long long gap3) {
	if (!(sdata[i].flags & SEC_NO_DATA)) return 0;
	Sector* nx = &sdata[(i + 1) % scount];
	if (nx->flags & SEC_NO_DATA) return 0;
	long long lead = gap3 + ((nx->flags & SEC_NO_ID) ? g->gap1 : g->gap1 + g->sync + 10 + g->gap2) + g->sync + 4;
	if (i + 1 >= scount) lead += 16;		// past the index: the track's start
	return (lead > WD_DAM_WINDOW) ? 0 : WD_DAM_WINDOW + 1 - lead;
}

static long long disk_form_sum(Sector* sdata, int scount, const formGaps* g, long long gap3) {
	long long sum = 0;		// up to a gigabyte a sector by the size code: no int
	for (int i = 0; i < scount; i++)
		sum += sec_room(&sdata[i], sec_counted(&sdata[i]), g) + sec_pad(sdata, i, scount, g, gap3);
	return sum;
}

// gap3; start: bytes counted before the first sector (the standard layout's 16 never were)
static long long disk_form_gap(Floppy* flp, Sector* sdata, int scount, const formGaps* g, int start) {
	long long gap = (flp->trklen - start - disk_form_sum(sdata, scount, g, 0)) / scount;
	if (gap > 0) {				// a gap takes the place of padding: count it again
		long long more = (flp->trklen - start - disk_form_sum(sdata, scount, g, gap)) / scount;
		if (more > gap) gap = more;
	}
	return gap;
}

int diskFormFits(Floppy* flp, Sector* sdata, int scount) {
	return (scount > 0) && (disk_form_gap(flp, sdata, scount, &form_std, 0) >= 10);
}

// cut: no sector starts past the track's end, the one running over it is cut there
static void disk_form_lay(Floppy* flp, int tr, Sector* sdata, int scount, const formGaps* g, int gap3, int cut) {
	unsigned char lay[TRKLEN_HD + 0x2000];	// cut: room for the sector run past an HD track's end
	memcpy(lay, flp->data[tr].byte, TRKLEN_HD);
	long limit = cut ? (long)sizeof(lay) : TRKLEN_HD;
	unsigned char* base = lay;
	unsigned char* ppos = base;
	int bad[256];			// where the CRCs to spoil are
	int nbad = 0;
	int nid_pos[256];		// the data marks with no ID before them, and their size codes
	unsigned char nid_sz[256];
	int nnid = 0;
	memset(ppos, 0x4e, flp->trklen);
	memset(ppos, 0x00, 12); ppos += 12;		// 12	space
	*(ppos++) = 0xc2;				// 	track mark
	*(ppos++) = 0xc2;
	*(ppos++) = 0xc2;
	*(ppos++) = 0xfc;
	for (int sc = 0; (sc < scount) && (sc < 256); sc++) {
		Sector* s = &sdata[sc];
		int ln = (128 << (s->sz & 3));
		if (cut && (ppos - base >= flp->trklen)) break;
		// a size code past 23 counts as nothing in the gap, but its bytes are
		// written: what would run past the track's buffer stays off it
		if ((ppos - base) + sec_room(s, ln, g) > limit) break;
		ppos += g->gap1;				//	GAP1
		if (!(s->flags & SEC_NO_ID)) {
			memset(ppos, 0x00, g->sync); ppos += g->sync;	//	space
			*(ppos++) = 0xa1;			//	address mark
			*(ppos++) = 0xa1;
			*(ppos++) = 0xa1;
			*(ppos++) = 0xfe;
			*(ppos++) = s->trk;			// 	addr field
			*(ppos++) = s->head;
			*(ppos++) = s->sec;
			*(ppos++) = s->sz;
			*(ppos++) = 0xf7; *(ppos++) = 0xf7;
			if (!(s->flags & SEC_NO_DATA)) ppos += g->gap2;	//	GAP2
		}
		if (!(s->flags & SEC_NO_DATA)) {
			memset(ppos, 0x00, g->sync); ppos += g->sync;	//	space
			*(ppos++) = 0xa1;			//	data mark
			*(ppos++) = 0xa1;
			*(ppos++) = 0xa1;
			if (s->flags & SEC_NO_ID) {
				nid_pos[nnid] = (int)(ppos - base);
				nid_sz[nnid++] = s->sz;
			}
			*(ppos++) = s->type;
			memcpy(ppos, s->data, ln); ppos += ln;	//	data
			if (s->flags & SEC_BAD_CRC) bad[nbad++] = (int)(ppos - base);
			*(ppos++) = 0xf7; *(ppos++) = 0xf7;
		}
		ppos += gap3 + sec_pad(sdata, sc, scount, g, gap3);	//	GAP3
	}
	memcpy(flp->data[tr].byte, lay, TRKLEN_HD);
	flpFillFieldsEx(flp, tr, 1, nid_pos, nid_sz, nnid);
	for (int i = 0; i < nbad; i++) {
		if (bad[i] + 1 >= flp->trklen) continue;	// cut off with the track's end
		flp->data[tr].byte[bad[i]] ^= 0xff;
		flp->data[tr].byte[bad[i] + 1] ^= 0xff;
	}
}

// 0 if the standard layout does not hold them; no sectors: left as the loader cleared it
static int disk_form_std(Floppy* flp, int tr, Sector* sdata, int scount) {
	if ((tr < 0) || (tr > 255) || (scount <= 0)) return 1;
	long long gap = disk_form_gap(flp, sdata, scount, &form_std, 0);
	if (gap < 10) return 0;
	disk_form_lay(flp, tr, sdata, scount, &form_std, (int)gap, 0);
	return 1;
}

void diskFormTrack(Floppy* flp, int tr, Sector* sdata, int scount) {
	disk_form_std(flp, tr, sdata, scount);
}

// an image's track the standard layout does not hold: tight gaps, else cut at the end
void diskFormImageTrack(Floppy* flp, int tr, Sector* sdata, int scount) {
	if (disk_form_std(flp, tr, sdata, scount)) return;
	long long gap = disk_form_gap(flp, sdata, scount, &form_tight, 16);
	if (gap >= 2)
		disk_form_lay(flp, tr, sdata, scount, &form_tight, (int)gap, 0);
	else
		disk_form_lay(flp, tr, sdata, scount, &form_tight, 2, 1);
}

static unsigned char fbuf[0x100];

int diskGetType(Floppy* flp) {
	int res = -1;
	// trdos
	if (diskGetSectorData(flp,0,15,fbuf,0x100)) {			// at least 16 sectors
		if (diskGetSectorData(flp,0,9,fbuf,0x100)) {
			if (fbuf[0xe7] == 0x10) res = DISK_TYPE_TRD;
		}
	}
	return res;
}

int diskCreateDescriptor(Floppy* flp,TRFile* dsc) {
	unsigned char files;
	unsigned short freesec;
	if (!diskGetSectorData(flp,0,9,fbuf,256)) return ERR_SHIT;
	dsc->sec = fbuf[0xe1];
	dsc->trk = fbuf[0xe2];
	files = fbuf[0xe4];
	if (files > 127) return ERR_MANYFILES;
	files++;
	fbuf[0xe4] = files;
	freesec = fbuf[0xe5] + (fbuf[0xe6] << 8);
	if (freesec < dsc->slen) return ERR_NOSPACE;
	freesec -= dsc->slen;
	fbuf[0xe5] = freesec & 0xff;
	fbuf[0xe6] = ((freesec & 0xff00) >> 8);
	fbuf[0xe1] += (dsc->slen & 0x0f);
	fbuf[0xe2] += ((dsc->slen & 0xf0) >> 4);
	if (fbuf[0xe1] > 0x0f) {
		fbuf[0xe1] -= 0x10;
		fbuf[0xe2]++;
	}
	diskPutSectorData(flp,0,9,fbuf,256);
	files--;
	freesec = ((files & 0xf0) >> 4)+1;
	if (!diskGetSectorData(flp,0,freesec,fbuf,256)) return ERR_SHIT;
	memcpy(fbuf + ((files & 0x0f) << 4), (char*)dsc, 16);
	diskPutSectorData(flp,0,freesec,fbuf,256);
	flp->changed = 1;
	return ERR_OK;
}

int diskCreateFile(Floppy* flp, TRFile dsc, unsigned char* data, int len) {
	if (len > 0xff00) return ERR_SIZE;
	dsc.slen = ((len >> 8) & 0xff) + ((len & 0xff) ? 1 : 0);
	int err = diskCreateDescriptor(flp, &dsc);
	if (err != ERR_OK) return err;
	for (int i = 0; i < dsc.slen; i++) {
		diskPutSectorData(flp, dsc.trk, dsc.sec + 1, &data[i << 8], 256);
		dsc.sec++;
		if (dsc.sec > 15) {
			dsc.trk++;
			dsc.sec -= 16;
		}
	}
	return ERR_OK;
}

TRFile diskGetCatalogEntry(Floppy* flp, int num) {
	TRFile res;
	memset(&res, 0x00, sizeof(TRFile));
	TRFile cat[128];
	int cnt = diskGetTRCatalog(flp, cat);
	int i = 0;
	while (i < cnt) {
		switch(cat[i].name[0]) {
			case 0x00: i = cnt; break;	// end
			case 0x01: break;		// skip
			default: if (num == 0) {
					res = cat[i];
					i = cnt;
				}
				num--;
				break;
		}
		i++;
	}
	return res;
}

TRFile diskMakeDescriptor(const char* name, char ext, int start, int len) {
	TRFile dsc;
	memset(dsc.name,' ',8);
	memcpy(dsc.name,name,(strlen(name) > 8) ? 8 : strlen(name));
	dsc.ext = ext;
	dsc.hst = (start >> 8) & 0xff;
	dsc.lst = start & 0xff;
	dsc.hlen = (len >> 8) & 0xff;
	dsc.llen = len & 0xff;
	dsc.slen = dsc.hlen + (dsc.llen ? 1 : 0);
	return dsc;
}

// The bytes a TR-DOS file holds: a program's start is its length with the
// variables, which 80 AA and the autostart line may follow; anything else has
// its length. -1 when the sectors do not match: files joined into one, or cut.
// line gets the autostart, 0x8000 for none.
int trd_file_len(TRFile* f, unsigned char* data, int* line) {
	int start = f->lst | (f->hst << 8);
	int prog = (f->ext == 'B');
	int tail = prog && (start < f->slen * 256 - 3) && (data[start] == 0x80) && (data[start + 1] == 0xaa);
	int size = prog ? start : (f->llen | (f->hlen << 8));
	if (line) *line = tail ? (data[start + 2] | (data[start + 3] << 8)) : 0x8000;
	return (f->slen == (size + (tail ? 4 : 0) + 255) / 256) ? size : -1;
}

int diskGetTRCatalog(Floppy *flp, TRFile *dst) {
	int cnt = 0;
	if (diskGetType(flp) == DISK_TYPE_TRD) {
		int sc;
		int fc;
		unsigned char* ptr;
		unsigned char* dpt = (unsigned char*)dst;
		for (sc = 1; sc < 9; sc++) {
			if (diskGetSectorData(flp,0,sc,fbuf,256)) {
				ptr = fbuf;
				for (fc = 0; fc < 16; fc++) {
					if (*ptr == 0) break;
					memcpy(dpt,ptr,16);
					dpt += 16;
					ptr += 16;
					cnt++;
				}
				if (fc < 16) break;
			} else {
				break;
			}
		}
	}
	return cnt;
}

unsigned char* diskGetSectorDataPtr(Floppy* flp, unsigned char tr, unsigned char sc) {
	int pos = flp->data[tr].map[sc];	// input nr 1+, tab nr 0+
	if (pos == 0) return NULL;
	return flp->data[tr].byte + pos;
}

int diskPutSectorData(Floppy* flp, unsigned char tr,unsigned char sc,unsigned char* buf,int len) {
	unsigned short crc;
	unsigned char* ptr = diskGetSectorDataPtr(flp,tr,sc);
	if (ptr == NULL) return 0;
	flp_track_written(flp, tr);
	memcpy(ptr,buf,len);
	crc = getCrc(ptr - 4,len + 4);
	*(ptr + len) = ((crc & 0xff00) >> 8);
	*(ptr + len + 1) = (crc & 0x00ff);
	return 1;
}

int diskGetSectorData(Floppy* flp, unsigned char tr,unsigned char sc,unsigned char* buf,int len) {
	unsigned char* ptr = diskGetSectorDataPtr(flp,tr,sc);
	if (ptr == NULL) return 0;
	memcpy(buf, ptr, len);
	return 1;
}

int diskGetSectorsData(Floppy* flp, unsigned char tr, unsigned char sc, unsigned char* ptr, int sl) {
	while (sl > 0) {
		if (!diskGetSectorData(flp,tr,sc,ptr,256)) return 0;
		ptr += 256;
		sc++;
		if (sc > 16) {
			sc = 1;
			tr++;
		}
		sl--;
	}
	return 1;
}

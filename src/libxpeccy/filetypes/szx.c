#include <zlib.h>

#include "filetypes.h"
#include "szx.h"
#include "../xlog.h"
#include "../cpu/Z80/z80.h"

// zx-state, Spectaculator's format and Fuse's. The spec is version 1.5,
// spectaculator.com/docs/zx-state; what Fuse makes of it is libspectrum's szx.c.
// All numbers are little endian, a block is a 4-byte id and a 32-bit length.
// What the format has no block for goes in the creator block's own data (see
// szx_ext_*): an unknown block id is skipped, but Fuse reports every one of them
// in an error box, the creator's data it passes over in silence.

#define SZX_MAJOR	1
#define SZX_MINOR	5

// machine ids, ZXSTMID_*
enum {
	SZM_16K = 0,
	SZM_48K,
	SZM_128K,
	SZM_PLUS2,
	SZM_PLUS2A,
	SZM_PLUS3,
	SZM_PLUS3E,
	SZM_PENT128,
	SZM_TC2048,
	SZM_TC2068,
	SZM_SCORPION,
	SZM_SE,
	SZM_TS2068,
	SZM_PENT512,
	SZM_PENT1024,
	SZM_NTSC48K,
	SZM_128KE,
	// ours: a machine the spec cannot name, the creator data says which
	SZM_XPECCY = 0x80
};

#define SZF_LATE	1		// ZXSTMF_ALTERNATETIMINGS

#define SZZ_NOINT	1		// ZXSTZF_SUPPRESS_INTS (EILAST before 1.5)
#define SZZ_HALTED	2
#define SZZ_FSET	4


#define SZB_CONNECTED	1		// B128
#define SZB_CUSTOMROM	2
#define SZB_PAGED	4
#define SZB_AUTOBOOT	8
#define SZB_SEEKLOWER	16
#define SZB_COMPRESSED	32

#define SZD_EMBEDDED	1		// BDSK
#define SZD_COMPRESSED	2
#define SZD_PROTECT	4

#define SZ3_COMPRESSED	1		// DSK
#define SZ3_EMBEDDED	2

#define SZT_EMBEDDED	1		// TAPE
#define SZT_COMPRESSED	2

#define SZK_ISSUE2	1		// KEYB
#define SZJ_KEMPSTON	0		// JOY, KEYB
#define SZJ_NONE	8
#define SZM_MOUSE_KEMPSTON	2	// AMXM
#define SZY_128AY	2		// AY
#define SZI_ENABLED	1		// IF1
#define SZF_MF_DISABLED	0x10		// MFCE

// --- machine ---

// The snapshot hardware an id stands for. The ones with no match here (the
// Timex machines, the SE) are refused rather than loaded as something near.
static int szx_snap_of(int mid) {
	switch (mid) {
		case SZM_16K:
		case SZM_48K:
		case SZM_NTSC48K: return SNAP_HW_48K;
		case SZM_128K:
		case SZM_128KE: return SNAP_HW_128K;
		case SZM_PLUS2: return SNAP_HW_PLUS2;
		case SZM_PLUS2A: return SNAP_HW_PLUS2A;
		case SZM_PLUS3:
		case SZM_PLUS3E: return SNAP_HW_PLUS3;
		case SZM_PENT128: return SNAP_HW_PENTAGON;
		case SZM_PENT512: return SNAP_HW_PENT512;
		case SZM_PENT1024: return SNAP_HW_PENT1024;
		case SZM_SCORPION: return SNAP_HW_SCORPION;
	}
	return SNAP_HW_UNKNOWN;
}

// what the running machine is written as, -1 for one the spec cannot name
static int szx_mid_of(Computer* comp) {
	switch (comp->hw->id) {
		case HW_ZX48: return (comp->mem->ramSize <= MEM_16K) ? SZM_16K : SZM_48K;
		case HW_ZX128: return (comp->snapModel == SNAP_HW_PLUS2) ? SZM_PLUS2 : SZM_128K;
		case HW_PLUS2A: return SZM_PLUS2A;
		case HW_PLUS3: return SZM_PLUS3;
		case HW_PENT: return (comp->mem->ramSize > MEM_128K) ? SZM_PENT512 : SZM_PENT128;
		case HW_P1024: return SZM_PENT1024;
		case HW_SCORP:
		case HW_SCORPTP: return SZM_SCORPION;
	}
	return -1;
}

// ram pages the model has, in the file's numbering
static int szx_pages_of(int mid) {
	switch (mid) {
		case SZM_16K: return 1;
		case SZM_48K:
		case SZM_NTSC48K: return 3;
		case SZM_PENT512: return 32;
		case SZM_PENT1024: return 64;
		case SZM_SCORPION: return 16;
	}
	return 8;
}

static int szx_is48(int mid) {
	return (mid == SZM_16K) || (mid == SZM_48K) || (mid == SZM_NTSC48K);
}

// --- growing buffer, for writing ---

void sb_put(szxBuf* b, const void* src, size_t n) {
	if (b->fail || !n) return;
	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap : 0x10000;
		while (cap < b->len + n) cap <<= 1;
		unsigned char* p = (unsigned char*)realloc(b->data, cap);
		if (!p) {
			b->fail = 1;
			return;
		}
		b->data = p;
		b->cap = cap;
	}
	memcpy(b->data + b->len, src, n);
	b->len += n;
}

void sb_byte(szxBuf* b, int v) {
	unsigned char c = v & 0xff;
	sb_put(b, &c, 1);
}

void sb_word(szxBuf* b, int v) {
	sb_byte(b, v);
	sb_byte(b, v >> 8);
}

void sb_dword(szxBuf* b, unsigned v) {
	sb_word(b, v & 0xffff);
	sb_word(b, v >> 16);
}

void sb_free(szxBuf* b) {
	free(b->data);
	memset(b, 0, sizeof(szxBuf));
}

// a block from what was put in body, which is emptied for the next one
void sb_block(szxBuf* b, unsigned id, szxBuf* body) {
	sb_dword(b, id);
	sb_dword(b, (unsigned)body->len);
	sb_put(b, body->data, body->len);
	body->len = 0;
}

// zlib, when it gains anything; 1 if it did
int sb_deflate(szxBuf* b, const unsigned char* src, size_t n) {
	uLongf len = compressBound(n);
	unsigned char* tmp = (unsigned char*)malloc(len);
	int res = 0;
	if (tmp && (compress2(tmp, &len, src, n, Z_BEST_COMPRESSION) == Z_OK) && (len < n)) {
		sb_put(b, tmp, len);
		res = 1;
	} else {
		sb_put(b, src, n);
	}
	free(tmp);
	return res;
}

// --- reading ---

// exactly len bytes out of a zlib stream, 0 if it holds anything else
int szx_inflate(const unsigned char* src, size_t n, unsigned char* dst, size_t len) {
	uLongf out = len;
	if (uncompress(dst, &out, src, n) != Z_OK) return 0;
	return out == len;
}

// the same, for a stream whose size is only known from the header beside it
static unsigned char* szx_inflate_new(const unsigned char* src, size_t n, size_t len) {
	unsigned char* dst = (unsigned char*)malloc(len ? len : 1);
	if (dst && !szx_inflate(src, n, dst, len)) {
		free(dst);
		dst = NULL;
	}
	return dst;
}

// --- media the file links to or carries ---

static szxMedia szx_med[SZX_MEDIA_MAX];
static int szx_med_count = 0;

static void szx_media_clear(void) {
	int i;
	for (i = 0; i < szx_med_count; i++)
		free(szx_med[i].data);
	memset(szx_med, 0, sizeof(szx_med));
	szx_med_count = 0;
}

static szxMedia* szx_media_add(int kind, int drive) {
	if (szx_med_count >= SZX_MEDIA_MAX) return NULL;
	szxMedia* m = &szx_med[szx_med_count++];
	m->kind = kind;
	m->drive = drive;
	return m;
}

int szx_media(const szxMedia** list) {
	if (list) *list = szx_med;
	return szx_med_count;
}

// a linked file's name, cut to what fits
static void szx_media_name(szxMedia* m, const unsigned char* src, size_t n) {
	if (n >= sizeof(m->path)) n = sizeof(m->path) - 1;
	memcpy(m->path, src, n);
	m->path[n] = 0;
}

static const char* szx_disk_ext[4] = {"trd", "scl", "fdi", "udi"};

// --- the blocks, read ---

static int szx_noted = 0;

int szx_notes(void) {
	return szx_noted;
}

void szx_note(int n) {
	szx_noted |= n;
}

typedef struct {
	int mid;		// machine id from the header
	int version;		// major << 8 | minor
	int swapaf;		// libspectrum before 0.5.0 wrote A and F the wrong way round
	int tick;		// T into the frame, -1: not said
	int p7ffd, p1ffd;	// -1: not in the file
	int fe;
	int paged;		// TR-DOS paged in (B128)
	int gotram;
	int gotcpu;
} szxLoad;

static void szx_rd_z80r(Computer* comp, szxLoad* ld, const unsigned char* p, size_t n) {
	CPU* cpu = comp->cpu;
	if (n < 37) return;
	if (ld->swapaf) {
		cpu->regA = p[0];
		cpu_set_flag(cpu, p[1]);
		cpu->regAa = p[8];
		cpu->regFa = p[9];
	} else {
		cpu_set_flag(cpu, p[0]);
		cpu->regA = p[1];
		cpu->regFa = p[8];
		cpu->regAa = p[9];
	}
	cpu->regBC = rd_word(p + 2);
	cpu->regDE = rd_word(p + 4);
	cpu->regHL = rd_word(p + 6);
	cpu->regBCa = rd_word(p + 10);
	cpu->regDEa = rd_word(p + 12);
	cpu->regHLa = rd_word(p + 14);
	cpu->regIX = rd_word(p + 16);
	cpu->regIY = rd_word(p + 18);
	cpu->regSP = rd_word(p + 20);
	cpu->regPC = rd_word(p + 22);
	cpu->regI = p[24];
	cpu->regR = p[25];
	cpu->regR7 = p[25] & 0x80;
	cpu->flgIFF1 = p[26] ? 1 : 0;
	cpu->flgIFF2 = p[27] ? 1 : 0;
	cpu->regIM = p[28] & 3;
	// INT stays enabled from an EI until a DI, while IFF1 drops in a handler:
	// IFF2 set means an EI came after the last DI, so RETN has interrupts back
	cpu->inten = Z80_NMI | ((cpu->flgIFF1 || cpu->flgIFF2) ? Z80_INT : 0);
	ld->tick = (int)rd_dword(p + 29);
	// 33: what is left of the INT pulse, which the machine works out itself
	if (ld->version >= 0x0101) {
		cpu->flgNOINT = (p[34] & SZZ_NOINT) ? 1 : 0;
		// a halted cpu keeps its pc on the HALT, as ours does
		cpu->flgHALT = (p[34] & SZZ_HALTED) ? 1 : 0;
		cpu->flgFW = (p[34] & SZZ_FSET) ? 1 : 0;
	}
	if (ld->version >= 0x0104)
		cpu->regWZ = rd_word(p + 35);
	ld->gotcpu = 1;
}

static void szx_rd_spcr(Computer* comp, szxLoad* ld, const unsigned char* p, size_t n) {
	if (n < 8) return;
	ld->fe = (p[0] & 7) | ((ld->version >= 0x0101) ? (p[3] & 0xf8) : 0);
	ld->p7ffd = p[1];
	ld->p1ffd = p[2];
}

static void szx_rd_ramp(Computer* comp, szxLoad* ld, const unsigned char* p, size_t n) {
	unsigned char buf[MEM_16K];
	if (n < 3) return;
	int page = p[2];
	int ok = (rd_word(p) & SZR_COMPRESSED) ? szx_inflate(p + 3, n - 3, buf, MEM_16K)
			: ((n - 3 >= MEM_16K) && memcpy(buf, p + 3, MEM_16K));
	if (!ok) {
		xlog(XLG_FILE, XLL_WARN, "szx: ram page %i is broken", page);
		return;
	}
	if ((page << 14) >= mem_ram_extent(comp->mem)) {
		xlog(XLG_FILE, XLL_WARN, "szx: ram page %i is past this machine's memory", page);
		return;
	}
	memPutData(comp->mem, MEM_RAM, page, MEM_16K, (char*)buf);
	ld->gotram = 1;
}

static void szx_rd_ay(Computer* comp, szxLoad* ld, const unsigned char* p, size_t n) {
	aymChip* ay = comp->ts->chipA;
	int i;
	if (n < 18) return;
	if (ay->type == SND_NONE) {
		xlog(XLG_FILE, XLL_INFO, "szx: the snapshot has an AY, this machine has none");
		// a 48K writes the block for a Fuller box too, which is no AY of ours
		if (!szx_is48(ld->mid) || (p[0] & SZY_128AY))
			szx_note(SZN_AY);
		return;
	}
	comp->ts->curChip = ay;
	for (i = 0; i < 16; i++)
		ay_poke_reg(ay, i, p[2 + i]);
	ay->curReg = p[1];
}

static void szx_rd_pltt(Computer* comp, szxLoad* ld, const unsigned char* p, size_t n) {
	ulaPlus* ula = comp->vid->ula;
	if (n < 66) return;
	if (!ula->enabled) {
		if (p[0] & 1) {
			xlog(XLG_FILE, XLL_WARN, "szx: the snapshot uses ULA+, this machine has it off");
			szx_note(SZN_ULAPLUS);
		}
		return;
	}
	ula->active = p[0] & 1;
	ula->reg = p[1];
	memcpy(ula->pal, p + 2, 64);
	if (n > 66) ula->data = p[66];
	ula->palchan = 1;
}

static void szx_rd_b128(Computer* comp, szxLoad* ld, const unsigned char* p, size_t n) {
	if (n < 10) return;
	// CONNECTED is not looked at: ZXMAK2 leaves it clear on a Pentagon, and
	// libspectrum takes the block whatever it says
	unsigned flags = rd_dword(p);
	if (!comp->dif || (comp->dif->type != DIF_BDI)) {
		if (flags & SZB_CONNECTED) {
			xlog(XLG_FILE, XLL_WARN, "szx: the snapshot has a Beta 128, this machine has none");
			szx_note(SZN_BETA);
		}
		return;
	}
	if (flags & SZB_CUSTOMROM)
		xlog(XLG_FILE, XLL_INFO, "szx: custom TR-DOS rom not taken, the machine's own is used");
	// The system register is set, not written: a write that takes MR high
	// would start the RESTORE a real WD1793 runs coming out of reset, where the
	// machine saved had finished it long ago. Whatever the reset before the
	// load started is dropped for the same reason.
	FDC* fdc = comp->dif->fdc;
	int sys = p[5];
	comp->dif->sys = sys;
	fdc->flp = fdc->flop[sys & 3];
	fdc->mr = (sys & 0x04) ? 1 : 0;
	fdc->block = (sys & 0x08) ? 1 : 0;
	fdc->side = (sys & 0x10) ? 0 : 1;
	fdc->mfm = (sys & 0x40) ? 1 : 0;
	fdc->plan = NULL;
	fdc->idle = 1;
	fdc->trk = p[6];
	fdc->sec = p[7];
	fdc->data = p[8];
	fdc->state = p[9];
	fdc->step = (flags & SZB_SEEKLOWER) ? 0 : 1;
	ld->paged = (flags & SZB_PAGED) ? 1 : 0;
}

static void szx_rd_bdsk(Computer* comp, szxLoad* ld, const unsigned char* p, size_t n) {
	if (n < 7) return;
	unsigned flags = rd_dword(p);
	szxMedia* m = szx_media_add(SZX_MED_BETA, p[4] & 3);
	if (!m) return;
	m->cylinder = p[5];
	m->protect = (flags & SZD_PROTECT) ? 1 : 0;
	if (flags & SZD_EMBEDDED) {
		strcpy(m->ext, szx_disk_ext[p[6] & 3]);
		if (flags & SZD_COMPRESSED) {
			// the block does not say how big the image is: let zlib find out
			z_stream zs;
			szxBuf out;
			unsigned char tmp[0x4000];
			int err;
			memset(&zs, 0, sizeof(zs));
			memset(&out, 0, sizeof(out));
			if (inflateInit(&zs) != Z_OK) return;
			zs.next_in = (unsigned char*)(p + 7);
			zs.avail_in = (uInt)(n - 7);
			do {
				zs.next_out = tmp;
				zs.avail_out = sizeof(tmp);
				err = inflate(&zs, Z_NO_FLUSH);
				sb_put(&out, tmp, sizeof(tmp) - zs.avail_out);
			} while (err == Z_OK);
			inflateEnd(&zs);
			if ((err == Z_STREAM_END) && !out.fail) {
				m->data = out.data;
				m->size = out.len;
			} else {
				sb_free(&out);
			}
		} else {
			m->data = (unsigned char*)malloc(n - 7);
			if (m->data) {
				memcpy(m->data, p + 7, n - 7);
				m->size = n - 7;
			}
		}
	} else {
		szx_media_name(m, p + 7, n - 7);
	}
}

static void szx_rd_dsk(Computer* comp, szxLoad* ld, const unsigned char* p, size_t n) {
	if (n < 7) return;
	int flags = rd_word(p);
	if (flags & (SZ3_EMBEDDED | SZ3_COMPRESSED)) {
		xlog(XLG_FILE, XLL_WARN, "szx: embedded +3 disk images are not in the spec, skipped");
		return;
	}
	szxMedia* m = szx_media_add(SZX_MED_PLUS3, p[2] & 1);
	if (m) szx_media_name(m, p + 7, n - 7);
}

static void szx_rd_tape(Computer* comp, szxLoad* ld, const unsigned char* p, size_t n) {
	if (n < 28) return;
	int flags = rd_word(p + 2);
	unsigned usize = rd_dword(p + 4);
	unsigned csize = rd_dword(p + 8);
	if (csize > n - 28) csize = (unsigned)(n - 28);
	szxMedia* m = szx_media_add(SZX_MED_TAPE, 0);
	if (!m) return;
	m->block = rd_word(p);
	if (flags & SZT_EMBEDDED) {
		memcpy(m->ext, p + 12, 15);
		m->ext[15] = 0;
		if (!strcmp(m->ext, "tapw")) strcpy(m->ext, "tap");	// Warajevo's
		m->data = (flags & SZT_COMPRESSED) ? szx_inflate_new(p + 28, csize, usize) : (unsigned char*)malloc(csize ? csize : 1);
		if (m->data && !(flags & SZT_COMPRESSED)) memcpy(m->data, p + 28, csize);
		m->size = m->data ? ((flags & SZT_COMPRESSED) ? usize : csize) : 0;
	} else {
		szx_media_name(m, p + 28, csize);
	}
}

static void szx_rd_covx(Computer* comp, szxLoad* ld, const unsigned char* p, size_t n) {
	if (n < 1) return;
	if (!comp->sdrv || (comp->sdrv->type == SDRV_NONE)) {
		xlog(XLG_FILE, XLL_INFO, "szx: the snapshot has a Covox, this machine has none");
		szx_note(SZN_COVOX);
		return;
	}
	memset(comp->sdrv->chan, p[0], sizeof(comp->sdrv->chan));
}

static void szx_rd_rom(Computer* comp, szxLoad* ld, const unsigned char* p, size_t n) {
	if (n < 6) return;
	size_t size = rd_dword(p + 2);
	if ((size == 0) || (size > MEM_64K) || (size > (size_t)comp->mem->romSize)) {
		xlog(XLG_FILE, XLL_WARN, "szx: custom rom of %u bytes does not fit, skipped", (unsigned)size);
		return;
	}
	unsigned char* rom = (rd_word(p) & SZR_COMPRESSED) ? szx_inflate_new(p + 6, n - 6, size) : NULL;
	if (!rom && !(rd_word(p) & SZR_COMPRESSED) && (n - 6 >= size)) {
		rom = (unsigned char*)malloc(size);
		if (rom) memcpy(rom, p + 6, size);
	}
	if (!rom) {
		xlog(XLG_FILE, XLL_WARN, "szx: custom rom is broken, skipped");
		return;
	}
	// it stays in until the machine's roms are loaded again
	memcpy(comp->mem->romData, rom, size);
	comp->romCustom = (int)size;
	free(rom);
	xlog(XLG_FILE, XLL_INFO, "szx: custom rom of %u bytes put in", (unsigned)size);
}

// a device we do not have; on means it was switched on, as some writers put
// the block in for one that is off
static void szx_rd_ignore(const char* what, int on) {
	xlog(XLG_FILE, XLL_INFO, "szx: %s not emulated, skipped", what);
	if (on) szx_note(SZN_DEVICE);
}

// The input blocks are the user's settings, not the machine's state: they are
// compared, not taken. A joystick and a mouse are only logged, as most files
// carry whatever the emulator that wrote them was set to.
static void szx_rd_keyb(Computer* comp, szxLoad* ld, const unsigned char* p, size_t n) {
	if ((n < 4) || !szx_is48(ld->mid)) return;
	if (!!(rd_dword(p) & SZK_ISSUE2) != (comp->earback == EAR_ISSUE2)) {
		xlog(XLG_FILE, XLL_INFO, "szx: taken on an issue %i board", (rd_dword(p) & SZK_ISSUE2) ? 2 : 3);
		szx_note(SZN_ISSUE);
	}
}

static void szx_rd_joy(Computer* comp, const unsigned char* p, size_t n) {
	if (n < 6) return;
	if (((p[4] == SZJ_KEMPSTON) || (p[5] == SZJ_KEMPSTON)) && (comp->joy->type != XJ_KEMPSTON))
		xlog(XLG_FILE, XLL_INFO, "szx: the snapshot has a Kempston joystick, this machine has none");
}

static void szx_rd_amxm(Computer* comp, const unsigned char* p, size_t n) {
	if (n < 1) return;
	if (p[0] && ((p[0] != SZM_MOUSE_KEMPSTON) || !comp->mouse->enable))
		xlog(XLG_FILE, XLL_INFO, "szx: the snapshot has a mouse, this machine has none on");
}

// --- loading ---

// what the header says, -1 when it is no zx-state file
static int szx_header(const unsigned char* buf, size_t len, int* version) {
	if ((len < 8) || memcmp(buf, "ZXST", 4)) return -1;
	if (version) *version = (buf[4] << 8) | buf[5];
	return buf[6];
}

int szx_hardware_of(const unsigned char* buf, size_t len) {
	int mid = szx_header(buf, len, NULL);
	if (mid == SZM_XPECCY) return szx_ext_hardware(buf, len);
	return (mid < 0) ? SNAP_HW_UNKNOWN : szx_snap_of(mid);
}

int szxGetHardware(const char* name) {
	unsigned char buf[0x400];		// the header and the creator block before anything else
	FILE* file = fopen(name, "rb");
	if (!file) return SNAP_HW_UNKNOWN;
	size_t n = fread(buf, 1, sizeof(buf), file);
	fclose(file);
	return szx_hardware_of(buf, n);
}

int loadSZX_buf(Computer* comp, const unsigned char* buf, size_t len) {
	szxLoad ld;
	int version;
	int mid = szx_header(buf, len, &version);
	szx_media_clear();
	szx_noted = 0;
	if (mid < 0) return ERR_SZX_SIGN;
	int snap = (mid == SZM_XPECCY) ? szx_ext_hardware(buf, len) : szx_snap_of(mid);
	if (snap == SNAP_HW_UNKNOWN) {
		xlog(XLG_FILE, XLL_WARN, "szx: machine id %i is not one this emulator has", mid);
		return ERR_SZX_HW;
	}
	if ((version >> 8) != SZX_MAJOR)
		xlog(XLG_FILE, XLL_WARN, "szx: version %i.%i, reading it as %i.x", version >> 8, version & 0xff, SZX_MAJOR);
	memset(&ld, 0, sizeof(ld));
	ld.mid = mid;
	ld.version = version;
	ld.tick = -1;
	ld.p7ffd = -1;
	ld.p1ffd = -1;
	if ((mid == SZM_NTSC48K) || (mid == SZM_128KE) || (mid == SZM_PLUS3E)) {
		xlog(XLG_FILE, XLL_WARN, "szx: machine id %i is loaded on its nearest relative", mid);
		szx_note(SZN_RELATIVE);
	}
	if (((mid == SZM_16K) || (mid == SZM_48K) || (mid == SZM_128K)) && ((buf[7] & SZF_LATE) == comp->vid->ula->early)) {
		xlog(XLG_FILE, XLL_INFO, "szx: taken with %s timings, this machine has the other ones",
			(buf[7] & SZF_LATE) ? "late" : "early");
		szx_note(SZN_TIMINGS);
	}

	// a 128K reset, not a 48K one: that locks paging on a +2A, and the
	// specregs block is still to come
	comp_snap_reset(comp, RES_128);
	comp->flgBDI = 0;

	// The creator block comes first and can say how its own files were
	// written; our own data is applied last, over what the standard blocks set.
	const unsigned char* ext = NULL;
	size_t extlen = 0;
	size_t pos = 8;
	while (pos + 8 <= len) {
		unsigned id = rd_dword(buf + pos);
		size_t n = rd_dword(buf + pos + 4);
		const unsigned char* p = buf + pos + 8;
		if (n > len - pos - 8) {
			xlog(XLG_FILE, XLL_WARN, "szx: block %.4s runs past the end of the file", (const char*)(buf + pos));
			break;
		}
		switch (id) {
			case BID('C','R','T','R'):
				if (n >= 36) {
					xlog(XLG_FILE, XLL_DEBUG, "szx: made by %.32s %i.%i", (const char*)p, rd_word(p + 32), rd_word(p + 34));
					ld.swapaf = szx_libspectrum_swap(p + 36, n - 36);
					ext = szx_ext_find(p + 36, n - 36, &extlen);
				}
				break;
			case BID('Z','8','0','R'): szx_rd_z80r(comp, &ld, p, n); break;
			case BID('S','P','C','R'): szx_rd_spcr(comp, &ld, p, n); break;
			case BID('R','A','M','P'): szx_rd_ramp(comp, &ld, p, n); break;
			case BID('A','Y',0,0): szx_rd_ay(comp, &ld, p, n); break;
			case BID('P','L','T','T'): szx_rd_pltt(comp, &ld, p, n); break;
			case BID('B','1','2','8'): szx_rd_b128(comp, &ld, p, n); break;
			case BID('B','D','S','K'): szx_rd_bdsk(comp, &ld, p, n); break;
			case BID('D','S','K',0): szx_rd_dsk(comp, &ld, p, n); break;
			case BID('T','A','P','E'): szx_rd_tape(comp, &ld, p, n); break;
			case BID('C','O','V','X'): szx_rd_covx(comp, &ld, p, n); break;
			case BID('R','O','M',0): szx_rd_rom(comp, &ld, p, n); break;
			case BID('G','S',0,0): szx_rd_gs(comp, p, n); break;
			case BID('G','S','R','P'): szx_rd_gsrp(comp, p, n); break;
			case BID('+','3',0,0):		// the drives are the machine's: nothing to set
				break;
			case BID('K','E','Y','B'): szx_rd_keyb(comp, &ld, p, n); break;
			case BID('J','O','Y',0): szx_rd_joy(comp, p, n); break;
			case BID('A','M','X','M'): szx_rd_amxm(comp, p, n); break;
			case BID('I','F','1',0): szx_rd_ignore("Interface 1", (n >= 2) && (rd_word(p) & SZI_ENABLED)); break;
			case BID('M','D','R','V'): szx_rd_ignore("Microdrive", 0); break;	// IF1 says
			case BID('I','F','2','R'): szx_rd_ignore("Interface 2 cartridge", 1); break;
			case BID('Z','X','P','R'): break;		// the printer: no state worth a word
			case BID('D','R','U','M'): szx_rd_ignore("SpecDrum", 1); break;
			case BID('M','F','C','E'): szx_rd_ignore("Multiface", (n >= 2) && !(p[1] & SZF_MF_DISABLED)); break;
			default:
				xlog(XLG_FILE, XLL_INFO, "szx: block %.4s skipped", (const char*)(buf + pos));
				break;
		}
		pos += 8 + n;
	}
	if (!ld.gotcpu || !ld.gotram) {
		xlog(XLG_FILE, XLL_WARN, "szx: no %s in the file", ld.gotcpu ? "memory" : "cpu state");
		compReset(comp, RES_DEFAULT);
		return ERR_SZX_DATA;
	}

	// paging, after the memory is in: a +3 takes 1FFD only while 7FFD is
	// unlocked, so 1FFD goes first
	if (comp->hw->id == HW_P1024) {
		// a Pentagon 512 snapshot pages through 7FFD alone, which is our 1024's mode with EFF7.2 clear
		comp->hw->out(comp, 0xeff7, (ld.mid == SZM_PENT1024) && (ld.p1ffd >= 0) ? ld.p1ffd : 0x00);
	} else if ((ld.p1ffd >= 0) && (snap == SNAP_HW_PLUS2A || snap == SNAP_HW_PLUS3 || snap == SNAP_HW_SCORPION)
			&& snapHwRuns(snap, comp->hw->id)) {
		comp->hw->out(comp, 0x1ffd, ld.p1ffd);
	}
	// a 48K has no paging: its 7ffd byte is 0, and taken as one it would put
	// the 128 rom in on a machine that has one
	comp->hw->out(comp, 0x7ffd, (szx_is48(ld.mid) || (ld.p7ffd < 0)) ? 0x10 : ld.p7ffd);
	if (ld.paged) {
		comp->flgDOS = 1;
		comp->hw->mapMem(comp);
	}
	xOutFE(comp, 0xfe, ld.fe);
	comp->vid->brdcol = ld.fe & 7;
	comp->vid->nextbrd = ld.fe & 7;

	if (!ext || !szx_ext_load(comp, ext, extlen))
		comp_set_frame_tick(comp, ld.tick);
	return ERR_OK;
}

int loadSZX(Computer* comp, const char* name, int drv) {
	FILE* file = fopen(name, "rb");
	if (!file) return ERR_CANT_OPEN;
	size_t len = fgetSize(file);
	unsigned char* buf = (unsigned char*)malloc(len ? len : 1);
	int res = ERR_CANT_OPEN;
	if (buf && (fread(buf, 1, len, file) == len))
		res = loadSZX_buf(comp, buf, len);
	free(buf);
	fclose(file);
	if (res == ERR_OK)
		mem_set_path(comp->mem, name);
	return res;
}

// --- saving ---

static const char* szx_creator = "Xpeccy+";
static int szx_cre_major = 0;
static int szx_cre_minor = 0;

void szx_set_creator(const char* name, int major, int minor) {
	szx_creator = name;
	szx_cre_major = major;
	szx_cre_minor = minor;
}

static const char* szx_tape_path = NULL;
static const char* szx_disk_path[4] = {NULL, NULL, NULL, NULL};

void szx_set_links(const char* tape, const char* const* disks) {
	int i;
	szx_tape_path = tape;
	for (i = 0; i < 4; i++)
		szx_disk_path[i] = disks ? disks[i] : NULL;
}

static void szx_wr_z80r(szxBuf* b, szxBuf* d, Computer* comp, int tick) {
	CPU* cpu = comp->cpu;
	sb_byte(d, cpu_get_flag(cpu));
	sb_byte(d, cpu->regA);
	sb_word(d, cpu->regBC);
	sb_word(d, cpu->regDE);
	sb_word(d, cpu->regHL);
	sb_byte(d, cpu->regFa);
	sb_byte(d, cpu->regAa);
	sb_word(d, cpu->regBCa);
	sb_word(d, cpu->regDEa);
	sb_word(d, cpu->regHLa);
	sb_word(d, cpu->regIX);
	sb_word(d, cpu->regIY);
	sb_word(d, cpu->regSP);
	sb_word(d, cpu->regPC);
	sb_byte(d, cpu->regI);
	sb_byte(d, z80_get_r(cpu));
	sb_byte(d, cpu->flgIFF1 ? 1 : 0);
	sb_byte(d, cpu->flgIFF2 ? 1 : 0);
	sb_byte(d, cpu->regIM & 3);
	sb_dword(d, (unsigned)tick);
	// what is left of the INT pulse, in T: a pulse not yet taken can still be
	Video* vid = comp->vid;
	int flen = comp_frame_ticks(comp);
	int hold = 0;
	if ((vid->intFRAME > 0) && (flen > 0)) {
		int dpt = vid->dotPerFrame / flen;
		hold = (dpt > 0) ? (vid->intFRAME + dpt - 1) / dpt : 0;
		if (hold > 255) hold = 255;
	}
	sb_byte(d, hold);
	int flags = 0;
	if (cpu->flgNOINT) flags |= SZZ_NOINT;
	else if (cpu->flgHALT) flags |= SZZ_HALTED;	// the two exclude each other
	if (cpu->flgFW) flags |= SZZ_FSET;
	sb_byte(d, flags);
	sb_word(d, cpu->regWZ);
	sb_block(b, BID('Z','8','0','R'), d);
}

static void szx_wr_spcr(szxBuf* b, szxBuf* d, Computer* comp, int mid) {
	int brd = comp->vid->nextbrd & 7;
	sb_byte(d, brd);
	sb_byte(d, szx_is48(mid) ? 0 : comp->p7FFD);
	switch (mid) {
		case SZM_PLUS2A:
		case SZM_PLUS3:
		case SZM_SCORPION: sb_byte(d, comp->p1FFD); break;
		case SZM_PENT1024: sb_byte(d, comp->pEFF7); break;
		default: sb_byte(d, 0); break;
	}
	sb_byte(d, brd | (comp->beep->lev ? 0x10 : 0) | (comp->tape->levRec ? 0x08 : 0));
	sb_dword(d, 0);
	sb_block(b, BID('S','P','C','R'), d);
}

static void szx_wr_ramp(szxBuf* b, szxBuf* d, Computer* comp, int page) {
	const unsigned char* src = comp->mem->ramData + ((page << 14) & comp->mem->ramMask);
	sb_word(d, 0);
	sb_byte(d, page);
	if (sb_deflate(d, src, MEM_16K))
		d->data[0] = SZR_COMPRESSED;
	sb_block(b, BID('R','A','M','P'), d);
}

static void szx_wr_ay(szxBuf* b, szxBuf* d, Computer* comp, int mid) {
	aymChip* ay = comp->ts->chipA;
	int i;
	sb_byte(d, szx_is48(mid) ? 2 : 0);	// on a 48K it is a Melodik: the 128's ports
	sb_byte(d, ay->curReg);			// past 15 the chip is not selected
	for (i = 0; i < 16; i++)
		sb_byte(d, ay->reg[i]);
	sb_block(b, BID('A','Y',0,0), d);
}

static void szx_wr_pltt(szxBuf* b, szxBuf* d, Computer* comp) {
	ulaPlus* ula = comp->vid->ula;
	sb_byte(d, ula->active ? 1 : 0);
	sb_byte(d, ula->reg);
	sb_put(d, ula->pal, 64);
	sb_byte(d, ula->active ? 1 : 0);	// the mode register: palette on
	sb_block(b, BID('P','L','T','T'), d);
}

static int szx_drives(DiskIF* dif) {
	int res = 0;
	int i;
	for (i = 0; i < 4; i++)
		if (dif->flp[i]->fitted) res = i + 1;
	return res;
}

static void szx_wr_b128(szxBuf* b, szxBuf* d, Computer* comp) {
	FDC* fdc = comp->dif->fdc;
	int i;
	unsigned flags = SZB_CONNECTED;
	if (comp->flgDOS) flags |= SZB_PAGED;
	if (!fdc->step) flags |= SZB_SEEKLOWER;
	sb_dword(d, flags);
	sb_byte(d, szx_drives(comp->dif));
	sb_byte(d, comp->dif->sys);
	sb_byte(d, fdc->trk);
	sb_byte(d, fdc->sec);
	sb_byte(d, fdc->data);
	sb_byte(d, fdc->state);
	sb_block(b, BID('B','1','2','8'), d);
	for (i = 0; i < 4; i++) {
		Floppy* flp = comp->dif->flp[i];
		const char* path = szx_disk_path[i];
		if (!flp->insert || !path || !*path) continue;
		const char* ext = strrchr(path, '.');
		int type = 0;
		while (ext && (type < 3) && strcasecmp(ext + 1, szx_disk_ext[type])) type++;
		if (!ext || (type > 3)) type = 0;
		sb_dword(d, flp->protect ? SZD_PROTECT : 0);
		sb_byte(d, i);
		sb_byte(d, flp->trk);
		sb_byte(d, type);
		sb_put(d, path, strlen(path) + 1);
		sb_block(b, BID('B','D','S','K'), d);
	}
}

static void szx_wr_plus3(szxBuf* b, szxBuf* d, Computer* comp) {
	int i;
	int motor = 0;
	for (i = 0; i < 2; i++)
		if (comp->dif->flp[i]->motor) motor = 1;
	sb_byte(d, szx_drives(comp->dif) > 1 ? 2 : 1);
	sb_byte(d, motor);
	sb_block(b, BID('+','3',0,0), d);
	for (i = 0; i < 2; i++) {
		const char* path = szx_disk_path[i];
		if (!comp->dif->flp[i]->insert || !path || !*path) continue;
		sb_word(d, 0);
		sb_byte(d, i);
		sb_dword(d, (unsigned)strlen(path) + 1);
		sb_put(d, path, strlen(path) + 1);
		sb_block(b, BID('D','S','K',0), d);
	}
}

static void szx_wr_tape(szxBuf* b, szxBuf* d, Computer* comp) {
	const char* path = szx_tape_path;
	char ext[16];
	memset(ext, 0, sizeof(ext));
	sb_word(d, comp->tape->block);
	sb_word(d, 0);				// a link, not the image
	sb_dword(d, 0);
	sb_dword(d, (unsigned)strlen(path) + 1);
	sb_put(d, ext, 16);
	sb_put(d, path, strlen(path) + 1);
	sb_block(b, BID('T','A','P','E'), d);
}

static void szx_wr_crtr(szxBuf* b, szxBuf* d, Computer* comp) {
	char name[32];
	memset(name, 0, sizeof(name));
	strncpy(name, szx_creator, sizeof(name) - 1);
	sb_put(d, name, 32);
	sb_word(d, szx_cre_major);
	sb_word(d, szx_cre_minor);
	szx_ext_save(d, comp);
	sb_block(b, BID('C','R','T','R'), d);
}

int szxCanSave(Computer* comp) {
	return (szx_mid_of(comp) >= 0) || szx_ext_can_save(comp);
}

int saveSZX(Computer* comp, const char* name, int drv) {
	int mid = szx_mid_of(comp);
	if ((mid < 0) && !szx_ext_can_save(comp)) return ERR_SZX_HW;
	szxBuf b;
	szxBuf d;
	int i;
	memset(&b, 0, sizeof(b));
	memset(&d, 0, sizeof(d));
	sb_put(&b, "ZXST", 4);
	sb_byte(&b, SZX_MAJOR);
	sb_byte(&b, SZX_MINOR);
	sb_byte(&b, (mid < 0) ? SZM_XPECCY : mid);
	sb_byte(&b, ((mid == SZM_16K || mid == SZM_48K || mid == SZM_128K) && !comp->vid->ula->early) ? SZF_LATE : 0);
	szx_wr_crtr(&b, &d, comp);
	szx_wr_z80r(&b, &d, comp, comp_get_frame_tick(comp));
	szx_wr_spcr(&b, &d, comp, mid);
	// the joystick on the first player and the keyboard as it is wired
	sb_dword(&d, 0);
	sb_byte(&d, (comp->joy->type == XJ_KEMPSTON) ? SZJ_KEMPSTON : SZJ_NONE);
	sb_byte(&d, SZJ_NONE);
	sb_block(&b, BID('J','O','Y',0), &d);
	sb_dword(&d, (szx_is48(mid) && (comp->earback == EAR_ISSUE2)) ? SZK_ISSUE2 : 0);
	sb_byte(&d, SZJ_NONE);
	sb_block(&b, BID('K','E','Y','B'), &d);
	if (comp->mouse->enable) {
		sb_byte(&d, SZM_MOUSE_KEMPSTON);
		sb_put(&d, "\0\0\0\0\0\0", 6);
		sb_block(&b, BID('A','M','X','M'), &d);
	}
	// a machine of our own keeps all its ram: the creator data says how much
	int pages = (mid < 0) ? (int)(mem_ram_extent(comp->mem) >> 14) : szx_pages_of(mid);
	for (i = 0; i < pages; i++) {
		int page = (mid == SZM_16K) ? 5 : szx_is48(mid) ? ((i == 0) ? 5 : (i == 1) ? 2 : 0) : i;
		szx_wr_ramp(&b, &d, comp, page);
	}
	if (comp->romCustom > 0) {
		sb_word(&d, 0);
		sb_dword(&d, (unsigned)comp->romCustom);
		if (sb_deflate(&d, comp->mem->romData, comp->romCustom))
			d.data[0] = SZR_COMPRESSED;
		sb_block(&b, BID('R','O','M',0), &d);
	}
	if (comp->ts->chipA->type != SND_NONE)
		szx_wr_ay(&b, &d, comp, mid);
	if (comp->vid->ula->enabled)
		szx_wr_pltt(&b, &d, comp);
	if (comp->dif && (comp->dif->type == DIF_BDI))
		szx_wr_b128(&b, &d, comp);
	if (comp->dif && (comp->dif->type == DIF_P3DOS))
		szx_wr_plus3(&b, &d, comp);
	if (szx_tape_path && *szx_tape_path)
		szx_wr_tape(&b, &d, comp);
	if (comp->sdrv && (comp->sdrv->type == SDRV_COVOX)) {
		sb_byte(&d, comp->sdrv->chan[0]);
		sb_put(&d, "\0\0\0", 3);
		sb_block(&b, BID('C','O','V','X'), &d);
	}
	if (comp->gs && comp->gs->enable)
		szx_wr_gs(&b, &d, comp);
	sb_free(&d);
	if (b.fail) {
		sb_free(&b);
		return ERR_CANT_OPEN;
	}
	FILE* file = fopen(name, "wb");
	int res = ERR_CANT_OPEN;
	if (file) {
		if (fwrite(b.data, b.len, 1, file) == 1) res = ERR_OK;
		fclose(file);
	}
	sb_free(&b);
	if (res == ERR_OK)
		mem_set_path(comp->mem, name);
	return res;
}

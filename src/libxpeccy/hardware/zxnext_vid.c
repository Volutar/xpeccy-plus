// The ZX Spectrum Next's picture, as its FPGA core (3.02.03) draws it: the ULA
// (video/zxula.vhd), LoRes (video/lores.vhd), layer 2 (video/layer2.vhd) and the
// mixer (zxnext.vhd, video pipeline stages 0-2), and the tilemap (video/tilemap.vhd).
// Sprites are in zxnext_spr.c.
//
// Time. The ULA's fetch and the mixer run over a line once the ray has left it
// (nxv_line, as the next one starts), and read what they would have read with the
// ray on each dot: memory as it stood when the core read it (a write made since is
// kept with its old byte, nxvWrite, and undone for the read), the values the fetch
// samples with the time each changed, and every register change queued for the
// first half-pixel the core shows it on (nxv_at), so a mid-line write lands where
// it does on the board. The mixer keeps a few dots behind the fetch. A pixel is
// addressed by A = 2 D + half, D the absolute dot whose phc is the pixel's.
//
// Still frames. A frame in which nothing the picture reads moves draws what the one
// before drew: it runs as an undrawn frame (vid->still) with both buffers holding
// that picture, and the first change takes the drawing up at the ray, the state
// there worked out as after any undrawn stretch (nxv_resync).

#include <stddef.h>
#include <string.h>
#include <limits.h>

#include "zxnext.h"

#define V (NX.v)
#define NXV_SHADOW	0x200000	// a memory key past the 2 MB: NX.shadow[]
#define NXV_LAG		4		// dots the mixer keeps behind the fetch
#define NXV_RAM		0x040000	// SRAM of RAM page 0

static int nxv_flushing = 0;		// in nxv_flush: no drawing from here
static int nxv_sweeping = 0;		// in nxv_sweep: no copper, no sprite pass
static void nxv_flush(Computer* comp, long long end);

// the dot under the ray (the video walked to the cpu first)
static inline long long nxv_ray_dot(Computer* comp) {
	int h, v;
	nx_raw(comp, &h, &v);
	return NX.frameBase + h;
}

// the picture is being drawn: in step with the machine, the frame drawn and not still
static inline int nxv_drawing(Computer* comp) {
	return V.live && !comp->vid->nodraw && !comp->vid->still;
}

// an SZX keeps nxVideo as one field, its length in 16 bits
_Static_assert(sizeof(nxVideo) < 65536, "nxVideo does not fit an SZX field");

// what the mixer reads, as the registers hold it
enum {
	NXF_TR = 0, NXF_FB, NXF_ULAEN, NXF_BLEND, NXF_STENCIL,
	NXF_ULAP, NXF_ULANEXT, NXF_FMT,
	NXF_SEL43, NXF_TMEN, NXF_TMPAL, NXF_TMTOP,
	NXF_LOEN, NXF_SPREN, NXF_ORDER,
	NXF_L2EN, NXF_L2BANK, NXF_L2SX, NXF_L2SY, NXF_L2CX1, NXF_L2CX2, NXF_L2CY1, NXF_L2CY2, NXF_L2RES,
	NXF_L2OFS, NXF_LOSX, NXF_LOSY, NXF_LOMODE, NXF_LOXOR, NXF_LOOFS, NXF_DFILE,
	NXF_UCX1, NXF_UCX2, NXF_UCY1, NXF_UCY2,
	NXF_SOVER, NXF_SCLIP, NXF_SCX1, NXF_SCX2, NXF_SCY1, NXF_SCY2,
	NXF_LORAD,		// Radastan takes ULA+ colours: ULA+ on and ULANext off
	NXF_TMTR, NXF_TMENB,	// the tilemap's live inputs: nr_4C, nr_6B b7 as the below bit takes it
	NXF_TCX1, NXF_TCX2, NXF_TCY1, NXF_TCY2,
	NXF_CMP,		// the ones above follow the registers (nxv_regs)
	NXF_FLASH = NXF_CMP,	// flash_cnt as the pixel sees it
	NXF_PAL = 0xff		// a palette entry
};

// When a register write at CLK_28 edge w shows, as the first half-pixel A, by
// the path it takes (research: video-mix.diff.md 3, layer2.diff.md 3, lores.diff.md 3)
enum {
	NXC_PAL,	// a palette entry, looked up by ULA/LoRes or layer 2: H-13 half / H-12
	NXC_PARAM,	// the CLK_14 chain _0, _1a, _1, _2 to the mixer: H-12 half / H-11 / H-11 half
	NXC_ULANEXT,	// the CLK_14 copy the ULA itself reads (ULANext, ULA+): one half-pixel earlier
	NXC_BELOW,	// nr_6B b0 into tm_pixel_below_1 straight: two half-pixels earlier than PARAM
	NXC_STAGE0,	// the CLK_7 stage 0 copies (layer order, clip, LoRes, layer 2 palette offset): H-11 / H-10
	NXC_L2F,	// layer 2's own CLK_7 copies, one column ahead of its pixel: H-10 / H-9
	NXC_FF,		// port FF bit 0 for Radastan: the falling-cpu-edge copy, then stage 0 (nxv_regs)
	NXC_LOPIX,	// the ULA's CLK_14 copies as LoRes takes them: into its CLK_7 pixel latch, whole pixels
	NXC_TMTR	// nr_4C at the tilemap's output register (its clip copies, reloaded at
			// hcount(1:0) = 11, are NXC_STAGE0)
};

static const unsigned char nxv_class[NXF_CMP] = {
	NXC_PARAM, NXC_PARAM, NXC_PARAM, NXC_PARAM, NXC_PARAM,
	NXC_ULANEXT, NXC_ULANEXT, NXC_ULANEXT,
	NXC_PARAM, NXC_PARAM, NXC_PARAM, NXC_BELOW,
	NXC_PARAM, NXC_PARAM, NXC_STAGE0,
	NXC_L2F, NXC_L2F, NXC_L2F, NXC_L2F, NXC_L2F, NXC_L2F, NXC_L2F, NXC_L2F, NXC_L2F,
	NXC_STAGE0, NXC_STAGE0, NXC_STAGE0, NXC_STAGE0, NXC_STAGE0, NXC_STAGE0, NXC_FF,
	NXC_STAGE0, NXC_STAGE0, NXC_STAGE0, NXC_STAGE0,
	NXC_STAGE0, NXC_STAGE0, NXC_STAGE0, NXC_STAGE0, NXC_STAGE0, NXC_STAGE0,
	NXC_LOPIX,
	NXC_TMTR, NXC_BELOW,
	NXC_STAGE0, NXC_STAGE0, NXC_STAGE0, NXC_STAGE0
};

static long long nxv_at(int cls, long long w) {
	switch (cls) {
		case NXC_TMTR: return w >> 1;
		case NXC_PAL: return ((w + 1) >> 1) - 2;
		case NXC_PARAM: return (w >> 1) + 1;
		case NXC_ULANEXT: return w >> 1;
		case NXC_BELOW: return (w >> 1) - 1;
		case NXC_STAGE0: return ((w >> 2) + 1) << 1;
		case NXC_LOPIX: return (((w & ~1LL) + 2) >> 2) << 1;	// the CLK_14 edge after w, then the pixel latch
	}
	return ((w >> 2) + 2) << 1;
}

// RGB333 to the image buffer's colour (R, G, B bytes, then alpha)
static unsigned int nxv_col[512];
// a byte with each bit doubled: the shift register of a non-hires mode takes a pixel twice
static unsigned short nxv_dbl[256];

static void nxv_col_init(void) {
	if (nxv_col[511]) return;
	for (int b = 0; b < 256; b++) {
		int w = 0;
		for (int i = 0; i < 8; i++)
			if (b & (1 << i)) w |= 3 << (2 * i);
		nxv_dbl[b] = w;
	}
	for (int i = 0; i < 512; i++) {
		unsigned int r = ((i >> 6) * 255 + 3) / 7;
		unsigned int g = (((i >> 3) & 7) * 255 + 3) / 7;
		unsigned int b = ((i & 7) * 255 + 3) / 7;
		nxv_col[i] = r | (g << 8) | (b << 16) | 0xff000000u;
	}
}

// == the values the ULA fetch samples: kept with the time a flop sees them ==

static void nxv_hset(nxvHist* h, int v) {
	h->n = 0;
	h->head = 0;
	h->v0 = v;
}

static int nxv_hnew(nxvHist* h) {
	return h->n ? h->v[(h->head + h->n - 1) & (NXV_HIST - 1)] : h->v0;
}

static int nxv_hpush(nxvHist* h, long long t, int v) {
	if (nxv_hnew(h) == v) return 0;
	if (h->n == NXV_HIST) {
		h->v0 = h->v[h->head];		// the oldest goes
		h->head = (h->head + 1) & (NXV_HIST - 1);
		h->n--;
	}
	int i = (h->head + h->n) & (NXV_HIST - 1);
	h->t[i] = t;
	h->v[i] = v;
	h->n++;
	return 1;
}

// the value a flop clocked at t takes: the last change before t (most often the
// newest, asked first)
static int nxv_hat(nxvHist* h, long long t) {
	for (int k = h->n - 1; k >= 0; k--) {
		int i = (h->head + k) & (NXV_HIST - 1);
		if (h->t[i] < t) return h->v[i];
	}
	return h->v0;
}

// == memory as the core read it ==

// A byte the picture may read, with the layers as the registers hold them: the ULA's
// screens (bank 5, the shadow one in bank 7), all of banks 5 and 7 under LoRes or the
// tilemap, layer 2's banks (pages 0A, 0B, 0E through their SRAM copies)
static int nxv_seen(Computer* comp, int key) {
	const unsigned short* q = V.q;
	if (key >= NXV_SHADOW) return q[NXF_L2EN];
	if (key < NXV_RAM) return 0;
	int pg = (key - NXV_RAM) >> 13;
	if ((pg == 0x0a) || (pg == 0x0b) || (pg == 0x0e)) {
		if (q[NXF_TMEN] || q[NXF_TMENB] || q[NXF_LOEN]) return 1;
		int a = ((pg == 0x0b) ? 0x2000 : 0) | (key & 0x1fff);
		return (a < 0x1b00) || ((pg == 0x0b) && (a < 0x3b00));
	}
	if (!q[NXF_L2EN]) return 0;
	int b = (key - NXV_RAM) >> 14;
	return (b >= q[NXF_L2BANK]) && (b < q[NXF_L2BANK] + 5);
}

void nxv_memw(Computer* comp, int key, int old, long long w) {
	if ((comp->vid->still || !NX.still.dirty) && nxv_seen(comp, key)) nxv_touch(comp);
	nxvWrite* e = &V.wr[V.wrHead];
	e->w = w;
	e->key = key;
	e->old = old;
	e->prev = V.wrH[key & (NXV_WRT - 1)];
	V.wrH[key & (NXV_WRT - 1)] = V.wrHead;
	V.wrHead = (V.wrHead + 1) & (NXV_WRQ - 1);
	V.wrT[key & (NXV_WRT - 1)] = (unsigned int)w;
	// the next write takes the oldest one's place: if a read may still have to undo it
	// (the mixer draws a line behind the ray, and reads up to 32 dots back), the picture
	// is drawn up to this write first
	if ((V.wr[V.wrHead].w > (V.cur - 32) * 4) && nxv_drawing(comp) && !nxv_flushing) {
		long long end = w >> 2;
		long long ray = nxv_ray_dot(comp);
		nxv_flush(comp, (end < ray) ? end : ray);
	}
}

// the byte at key as memory holds it now
static inline int nxv_peek(Computer* comp, int key) {
	return (key >= NXV_SHADOW) ? NX.shadow[key - NXV_SHADOW] : comp->mem->ramData[key];
}

// the byte at key as a read at CLK_28 r saw it: writes after r undone. A slot's time and
// r are never 2^31 clk28 apart the wrong way: an old slot only asks for the walk, of its
// own slot's writes, the newest first. A place written over since holds a newer write
// (the age goes down) or another slot's: the walk ends there, what r needs is never
// written over. Nothing here may write: the drawing is compiled knowing it only reads
static __attribute__((noinline)) int nxv_mem_undo(Computer* comp, int key, long long r, int v) {
	if ((int)(V.wrT[key & (NXV_WRT - 1)] - (unsigned int)r) <= 0) return v;
	int age = -1;
	// (the places are a snapshot's as it came: masked)
	for (int i = V.wrH[key & (NXV_WRT - 1)] & (NXV_WRQ - 1); ; i = V.wr[i].prev & (NXV_WRQ - 1)) {
		const nxvWrite* e = &V.wr[i];
		int a = (V.wrHead - 1 - i) & (NXV_WRQ - 1);
		if ((a <= age) || (e->w <= r) || ((e->key ^ key) & (NXV_WRT - 1))) break;
		age = a;
		if (e->key == key) v = e->old;
	}
	return v;
}

// the writes come in time order and most often none is after r: answered in place
static inline int nxv_mem(Computer* comp, int key, long long r) {
	int v = nxv_peek(comp, key);
	if (V.wr[(V.wrHead - 1) & (NXV_WRQ - 1)].w <= r) return v;
	return nxv_mem_undo(comp, key, r, v);
}

// the key of an SRAM byte: pages 0A, 0B, 0E are the copies behind the CPU's BRAM
static inline int nxv_key_sram(int adr) {
	int pg = (adr - NXV_RAM) >> 13;
	if ((adr >= NXV_RAM) && ((pg == 0x0a) || (pg == 0x0b) || (pg == 0x0e)))
		return NXV_SHADOW + (((pg == 0x0e) ? 0x4000 : ((pg - 0x0a) << 13)) | (adr & 0x1fff));
	return adr & 0x1fffff;
}

// the key of a bank 5 (16K) or bank 7 (8K) BRAM byte
static inline int nxv_key_bram(int bank7, int adr) {
	return bank7 ? NXV_RAM + (0x0e << 13) + (adr & 0x1fff) : NXV_RAM + (0x0a << 13) + (adr & 0x3fff);
}

static int nxv_sram(Computer* comp, int adr, long long r) {
	return nxv_mem(comp, nxv_key_sram(adr), r);
}

static int nxv_bram(Computer* comp, int bank7, int adr, long long r) {
	return nxv_mem(comp, nxv_key_bram(bank7, adr), r);
}

// == the events ==

static void nxv_apply(Computer* comp, long long a) {
	while ((V.evHead < V.evN) && (V.ev[V.evHead].t <= a)) {
		nxvEvent* e = &V.ev[V.evHead++];
		if (e->f == NXF_PAL) {
			V.pal[e->x >> 8][e->x & 0xff] = e->v;
		} else {
			V.f[e->f] = e->v;
			V.dirty = 1;
		}
	}
	if (V.evHead == V.evN)
		V.evHead = V.evN = 0;
}

// nxv_apply without the call when nothing is due (and nothing to tidy up)
static inline void nxv_apply_to(Computer* comp, long long a) {
	if (V.evN && ((V.evHead == V.evN) || (V.ev[V.evHead].t <= a)))
		nxv_apply(comp, a);
}

static void nxv_push(Computer* comp, long long t, int f, int x, int v) {
	if (!nxv_drawing(comp)) {
		// nothing is drawn: the change is in effect at once
		nxv_apply(comp, LLONG_MAX);
		if (f == NXF_PAL) {
			V.pal[x >> 8][x & 0xff] = v;
		} else {
			V.f[f] = v;
			V.dirty = 1;
		}
		return;
	}
	if (t < V.cur * 2) V.late++;
	if (V.evN == NXV_EVQ) {
		if (V.evHead) {
			memmove(V.ev, V.ev + V.evHead, (V.evN - V.evHead) * sizeof(nxvEvent));
			V.evN -= V.evHead;
			V.evHead = 0;
		} else {
			nxv_apply(comp, V.ev[0].t);	// full: the oldest goes in early
		}
	}
	int i = V.evN++;
	while ((i > V.evHead) && (V.ev[i - 1].t > t)) {
		V.ev[i] = V.ev[i - 1];
		i--;
	}
	V.ev[i].t = t;
	V.ev[i].f = f;
	V.ev[i].x = x;
	V.ev[i].v = v;
}

static void nxv_fields(Computer* comp, unsigned short* q) {
	unsigned char* nr = NX.nr;
	q[NXF_TR] = nr[0x14];
	q[NXF_FB] = nr[0x4a];
	q[NXF_ULAEN] = !(nr[0x68] & 0x80);
	q[NXF_BLEND] = (nr[0x68] >> 5) & 3;
	q[NXF_STENCIL] = nr[0x68] & 1;
	q[NXF_ULAP] = (nr[0x68] >> 3) & 1;
	q[NXF_ULANEXT] = nr[0x43] & 1;
	q[NXF_FMT] = nr[0x42];
	q[NXF_SEL43] = (nr[0x43] >> 1) & 7;
	q[NXF_TMEN] = nr[0x6b] >> 7;
	q[NXF_TMPAL] = (nr[0x6b] >> 4) & 1;
	q[NXF_TMTOP] = nr[0x6b] & 1;
	q[NXF_LOEN] = nr[0x15] >> 7;
	q[NXF_SPREN] = nr[0x15] & 1;
	q[NXF_ORDER] = (nr[0x15] >> 2) & 7;
	q[NXF_L2EN] = (NX.p123b >> 1) & 1;
	q[NXF_L2BANK] = nr[0x12] & 0x7f;
	q[NXF_L2SX] = ((nr[0x71] & 1) << 8) | nr[0x16];
	q[NXF_L2SY] = nr[0x17];
	q[NXF_L2CX1] = NX.clip[0][0];
	q[NXF_L2CX2] = NX.clip[0][1];
	q[NXF_L2CY1] = NX.clip[0][2];
	q[NXF_L2CY2] = NX.clip[0][3];
	q[NXF_L2RES] = (nr[0x70] >> 4) & 3;
	q[NXF_L2OFS] = nr[0x70] & 15;
	q[NXF_LOSX] = nr[0x32];
	q[NXF_LOSY] = nr[0x33];
	q[NXF_LOMODE] = (nr[0x6a] >> 5) & 1;
	q[NXF_LOXOR] = (nr[0x6a] >> 4) & 1;
	q[NXF_LOOFS] = nr[0x6a] & 15;
	q[NXF_DFILE] = NX.pff & 1;
	q[NXF_UCX1] = NX.clip[2][0];
	q[NXF_UCX2] = NX.clip[2][1];
	q[NXF_UCY1] = NX.clip[2][2];
	q[NXF_UCY2] = NX.clip[2][3];
	q[NXF_SOVER] = (nr[0x15] >> 1) & 1;
	q[NXF_SCLIP] = (nr[0x15] >> 5) & 1;
	q[NXF_SCX1] = NX.clip[1][0];
	q[NXF_SCX2] = NX.clip[1][1];
	q[NXF_SCY1] = NX.clip[1][2];
	q[NXF_SCY2] = NX.clip[1][3];
	q[NXF_LORAD] = ((nr[0x68] >> 3) & 1) && !(nr[0x43] & 1);
	q[NXF_TMTR] = nr[0x4c] & 15;
	q[NXF_TMENB] = nr[0x6b] >> 7;
	q[NXF_TCX1] = NX.clip[3][0];
	q[NXF_TCX2] = NX.clip[3][1];
	q[NXF_TCY1] = NX.clip[3][2];
	q[NXF_TCY2] = NX.clip[3][3];
}

static int nxv_tm_sx(Computer* comp) {return ((NX.nr[0x2f] & 3) << 8) | NX.nr[0x30];}

// the first falling edge of the cpu clock after w (the ULA takes port FE, port FF
// and 7FFD b3 from copies made there, zxnext.vhd 3601-3681); at 28 MHz half a
// CLK_28 after w, which a flop clocked on an edge sees from w + 1 on as well
static long long nxv_fall(Computer* comp, long long w) {
	int ph, per;
	switch (NX.speed) {
		case 0: ph = 4; per = 8; break;		// a 3.5 MHz T-state starts on an even dot
		case 1: ph = 2; per = 4; break;
		case 2: ph = 1; per = 2; break;
		default: return w;
	}
	long long t = w + 1;
	long long r = ((ph - t) % per + per) % per;
	return t + r;
}

// == room for a line of changes ==
// The mixer draws a line behind the ray, so the changes of a whole line wait in the
// queue and the histories. Before one more could overflow them, the picture is drawn up
// to the write, as when it was drawn a few dots behind the ray. From inside the drawing
// (the copper run there, nxv_fetch_to) the copper is stopped at the write instead
// (nx_cop_hold), and the drawing goes on to there before it runs on.


// the changes no read can reach any more go: those older than the next one before
// dot from (whatever reads a value reads it at a dot past from)
static void nxv_hprune(nxvHist* h, long long from) {
	long long t = from * 4;
	while ((h->n > 1) && (h->t[(h->head + 1) & (NXV_HIST - 1)] < t)) {
		h->v0 = h->v[h->head];
		h->head = (h->head + 1) & (NXV_HIST - 1);
		h->n--;
	}
	if ((h->n == 1) && (h->t[h->head] < t)) {
		h->v0 = h->v[h->head];
		h->n = 0;
		h->head = 0;
	}
}

#define NXV_HFULL	(NXV_HIST - 2)

// every history of nxVideo, for what walks them all (one added goes here too)
static const size_t nxv_hists[] = {
	offsetof(nxVideo, hBrd), offsetof(nxVideo, hPff), offsetof(nxVideo, hShadow),
	offsetof(nxVideo, hSx), offsetof(nxVideo, hSy), offsetof(nxVideo, hFine),
	offsetof(nxVideo, hTmSx), offsetof(nxVideo, hTmSy), offsetof(nxVideo, hTmMap),
	offsetof(nxVideo, hTmTile), offsetof(nxVideo, hTmCtl), offsetof(nxVideo, hTmAttr)
};
#define NXV_NHISTS	(sizeof(nxv_hists) / sizeof(nxv_hists[0]))

static inline nxvHist* nxv_hist(Computer* comp, unsigned i) {
	return (nxvHist*)((char*)&V + nxv_hists[i]);
}

static int nxv_hroom(Computer* comp) {
	for (unsigned i = 0; i < NXV_NHISTS; i++)
		if (nxv_hist(comp, i)->n >= NXV_HFULL) return 0;
	return 1;
}

static void nxv_hprune_all(Computer* comp, long long from) {
	for (unsigned i = 0; i < NXV_NHISTS; i++) nxv_hprune(nxv_hist(comp, i), from);
}

// the queue has room for the changes of one more write (NXF_CMP at most)
static inline int nxv_qroom(Computer* comp) {
	return V.evN - V.evHead < NXV_EVQ - NXF_CMP - 2;
}

// A write at CLK_28 w has left the queue or a history short of room for one more: all
// before w has landed (the copper's writes too: it stands at w when its own lands,
// nx_cop_run), so the picture is drawn up to w and what no read reaches goes
static void nxv_room(Computer* comp, long long w) {
	long long ray = nxv_ray_dot(comp);
	// the dot the reads start from: the mixer's when it draws, else the one the drawing
	// is taken up at (the ray, or the dot of a reset)
	long long from = ((V.upAt >= 0) && (V.upAt < ray)) ? V.upAt : ray;
	if (nxv_drawing(comp)) {
		long long end = w >> 2;
		if (end > ray) end = ray;
		if (!nxv_flushing)
			nxv_flush(comp, end);
		else if (end > V.lastDot + 1)
			nx_cop_hold = 1;	// a dot ended before the write: the drawing can go on to it
		from = V.cur;
	}
	// what reads back past it: the tilemap's latch 15 dots, a take-up and the
	// floating bus 24 before the ray (which is at or past the mixer)
	nxv_hprune_all(comp, from - 24);
}

// a register write landed at CLK_28 w: queue whatever the picture reads that moved
void nxv_regs(Computer* comp, long long w) {
	unsigned short q[NXF_CMP];
	nxv_fields(comp, q);
	// a still frame ends before the first change goes in
	int moved = memcmp(q, V.q, sizeof(q));
	if (moved || nx_cop_on(comp)) nxv_touch(comp);
	for (int f = 0; moved && (f < NXF_CMP); f++) {
		if (q[f] != V.q[f]) {
			V.q[f] = q[f];
			long long a = (nxv_class[f] == NXC_FF) ? (((nxv_fall(comp, w) >> 2) + 1) << 1) : nxv_at(nxv_class[f], w);
			nxv_push(comp, a, f, 0, q[f]);
		}
	}
	// the sampled ones keep their time, which the ray is short of when a still frame ends
	long long e = nxv_fall(comp, w);
	int ch = nxv_hpush(&V.hBrd, e, NX.pfe & 7);
	ch |= nxv_hpush(&V.hPff, e, NX.pff & 0x3f);
	ch |= nxv_hpush(&V.hShadow, e, (NX.p7ffd >> 3) & 1);
	ch |= nxv_hpush(&V.hSx, w, NX.nr[0x26]);
	ch |= nxv_hpush(&V.hSy, w, NX.nr[0x27]);
	ch |= nxv_hpush(&V.hFine, w, (NX.nr[0x68] >> 2) & 1);
	ch |= nxv_hpush(&V.hTmSx, w, nxv_tm_sx(comp));
	ch |= nxv_hpush(&V.hTmSy, w, NX.nr[0x31]);
	ch |= nxv_hpush(&V.hTmMap, w, NX.nr[0x6e]);
	ch |= nxv_hpush(&V.hTmTile, w, NX.nr[0x6f]);
	ch |= nxv_hpush(&V.hTmCtl, w, NX.nr[0x6b] & 0x7f);
	ch |= nxv_hpush(&V.hTmAttr, w, NX.nr[0x6c]);
	if (ch) nxv_touch(comp);
	if (!nxv_qroom(comp) || (ch && !nxv_hroom(comp))) nxv_room(comp, w);
}

void nxv_pal(Computer* comp, int p, int idx, long long w) {
	// sprites and the tilemap look up on the other CLK_28 (sc(0) = 1, zxnext.vhd 6981, 7033)
	long long a = ((p & 3) >= 2) ? ((w >> 1) - 2) : nxv_at(NXC_PAL, w);
	if (!comp->vid->still || (V.pal[p][idx & 0xff] != NX.pal[p][idx & 0xff])) nxv_touch(comp);
	nxv_push(comp, a, NXF_PAL, (p << 8) | (idx & 0xff), NX.pal[p][idx & 0xff]);
	if (!nxv_qroom(comp)) nxv_room(comp, w);
}

// == the ULA's fetch (zxula.vhd 194-306) ==

static int nxv_vbord(int vc) {
	return vc >= 192;		// vc(8) or vc(7) and vc(6)
}

// the sample at the falling CLK_7 of n = 3 and B
static void nxv_sample(Computer* comp, nxvFetch* fe, long long d, int hc, int vc) {
	long long t = d * 4 + 2;
	int sx = nxv_hat(&V.hSx, t);
	int sy = nxv_hat(&V.hSy, t);
	int fine = nxv_hat(&V.hFine, t);
	int pff = nxv_hat(&V.hPff, t);
	int sh = nxv_hat(&V.hShadow, t);
	fe->px = (fine << 8) | ((((hc >> 3) + (sx >> 3)) & 31) << 3) | (sx & 7);
	int pys = (vc + sy) & 0x1ff;
	if ((pys & 0x180) == 0x180)
		fe->py = pys & 0x7f;
	else if ((pys & 0x100) || ((pys & 0xc0) == 0xc0))
		fe->py = (pys + 0x40) & 0xff;
	else
		fe->py = pys & 0xff;
	fe->smode = sh ? 0 : (pff & 7);		// bank 7 has 8K: no Timex modes on the shadow screen
	fe->sshadow = sh;
}

// a shift register load (t = 2 of n = C or 4): sixteen half-pixels, for the dots d..d+7
static void nxv_load(Computer* comp, nxvFetch* fe, long long d, int hc, int vc, int s0) {
	long long t = d * 4 + 2;
	int pb = s0 ? (fe->pb00 << 8) | fe->pb01 : (fe->pb10 << 8) | fe->pb11;
	int ab = s0 ? (fe->ab00 << 8) | fe->ab01 : (fe->ab10 << 8) | fe->ab11;
	int mode = s0 ? fe->mode0 : fe->mode1;
	int scroll = s0 ? fe->scroll0 : fe->scroll1;
	unsigned int sr32 = 0;
	if (mode & 4) {
		sr32 = ((unsigned)(pb >> 8) << 24) | ((unsigned)(ab >> 8) << 16) | ((pb & 0xff) << 8) | (ab & 0xff);
	} else {
		sr32 = ((unsigned)nxv_dbl[(pb >> 8) & 0xff] << 16) | nxv_dbl[pb & 0xff];
	}
	unsigned int sr = (unsigned int)((((unsigned long long)sr32) << scroll) >> 16) & 0xffff;
	int attr;
	if (mode & 4) {
		int c = (nxv_hat(&V.hPff, t) >> 3) & 7;
		int a = 0x40 | ((~c & 7) << 3) | c;
		attr = (a << 8) | a;
	} else if ((((hc - 12) & 0x1ff) & 0x100) || nxv_vbord(vc)) {
		int b = nxv_hat(&V.hBrd, t);
		int a = (b << 3) | b;
		attr = (a << 8) | a;
	} else {
		attr = ab;
	}
	// both halves of each of the eight dots at once
	int m2 = ((mode >> 2) & 1) * 0x0c;
	int ahi = attr >> 8;
	int alo = attr & 0xff;
	for (int j = 0; j < 8; j++) {
		int k = j << 1;
		int i = (int)((d + j) & (NXV_ULA - 1));
		V.ula[i].attr[0] = (scroll + k < 16) ? ahi : alo;
		V.ula[i].attr[1] = (scroll + k + 1 < 16) ? ahi : alo;
		V.ula[i].bits = (V.ula[i].bits & 0xf0) | ((sr >> (15 - k)) & 1) | (((sr >> (14 - k)) & 1) << 1) | m2;
	}
}

// one dot of the fetch, d its absolute number. dry: the floating bus only, no loads
static void nxv_fetch(Computer* comp, nxvFetch* fe, long long d, int hc, int vc, int dry) {
	int n = hc & 15;
	if (n & 1) {
		// the read of this period (t = 0.5) at the address set at the end of the last odd one
		int b = nxv_bram(comp, fe->vramSh, fe->vramA, d * 4);
		switch (n) {
			case 0x1: fe->pb11 = b; break;
			case 0x3: fe->ab11 = b; break;
			case 0x5: fe->pb01 = b; break;
			case 0x7: fe->ab01 = b; break;
			case 0x9: fe->pb00 = b; break;
			case 0xb: fe->ab00 = b; break;
			case 0xd: fe->pb10 = b; break;
			case 0xf: fe->ab10 = b; break;
		}
		if ((hc & 0x100) || nxv_vbord(vc) || (n == 1)) {
			fe->fbR = 0xff;
			fe->fbEn = 0;
		} else if (n == 9) {
			fe->fbR = b;
			fe->fbEn = 1;
		} else if (n >= 0xb) {
			fe->fbR = b;
		}
		if ((n == 3) || (n == 0xb))
			nxv_sample(comp, fe, d, hc, vc);
		if (n == 7) {
			fe->mode0 = fe->smode;
			fe->scroll0 = ((fe->px & 7) << 1) | (fe->px >> 8);
		} else if (n == 0xb) {
			fe->mode1 = fe->smode;
			fe->scroll1 = ((fe->px & 7) << 1) | (fe->px >> 8);
		}
		// the address for the next read, set at the end of this period
		int py = fe->py;
		int ap = ((py & 0xc0) | ((py & 7) << 3) | ((py >> 3) & 7)) << 5;
		int aa = (0xc0 | (py >> 3)) << 5;
		int col = (fe->px >> 3) & 31;
		int col1 = (col + 1) & 31;
		int m0 = (fe->smode & 1) << 13;
		switch (n) {
			case 0xf: case 0x3: fe->vramA = m0 | ap | col1; break;
			case 0x1: case 0x5: fe->vramA = (fe->smode & 2) ? (0x2000 | ap | col1) : (m0 | aa | col1); break;
			case 0x7: case 0xb: fe->vramA = m0 | ap | col; break;
			default: fe->vramA = (fe->smode & 2) ? (0x2000 | ap | col) : (m0 | aa | col); break;
		}
		fe->vramSh = fe->sshadow;
	} else if (!dry && ((n == 4) || (n == 0xc))) {
		nxv_load(comp, fe, d, hc, vc, n == 0xc);
	}
}

// Where no byte the fetch reads is seen: a border line, and a line's border but for
// its last 16 dots (the next line's display takes nothing read before them). The mixer
// drops the pixel bits there (border_active) and the floating bus is off
static inline int nxv_fetch_unseen(int hc, int vc, int L) {
	return nxv_vbord(vc) || ((hc >= 0x110) && (hc < L - 16));
}

// a dot of it there: no read; the samples, the modes and the loads' border go on
static void nxv_fetch_idle(Computer* comp, nxvFetch* fe, long long d, int hc, int vc) {
	int n = hc & 15;
	if (n & 1) {
		fe->fbR = 0xff;
		fe->fbEn = 0;
		if ((n == 3) || (n == 0xb))
			nxv_sample(comp, fe, d, hc, vc);
		if (n == 7) {
			fe->mode0 = fe->smode;
			fe->scroll0 = ((fe->px & 7) << 1) | (fe->px >> 8);
		} else if (n == 0xb) {
			fe->mode1 = fe->smode;
			fe->scroll1 = ((fe->px & 7) << 1) | (fe->px >> 8);
		}
		fe->vramSh = fe->sshadow;
	} else {
		nxv_load(comp, fe, d, hc, vc, n == 0xc);
	}
}

// no change is seen at t or later
static inline int nxv_hquiet(const nxvHist* h, long long t) {
	return !h->n || (h->t[(h->head + h->n - 1) & (NXV_HIST - 1)] < t);
}

// nothing the fetch samples changes from t on, and its modes have settled
static int nxv_fetch_quiet(Computer* comp, long long t) {
	return nxv_hquiet(&V.hBrd, t) && nxv_hquiet(&V.hPff, t) && nxv_hquiet(&V.hShadow, t)
		&& nxv_hquiet(&V.hSx, t) && nxv_hquiet(&V.hSy, t) && nxv_hquiet(&V.hFine, t)
		&& (V.fe.mode0 == V.fe.smode) && (V.fe.mode1 == V.fe.smode);
}

// the loads of k unseen dots from d on (hc its counter) with the fetch quiet: every one
// puts the same border in its eight dots
static void nxv_fetch_fill(Computer* comp, long long d, int hc, int k) {
	int off = (4 - hc) & 7;
	if (off >= k) return;
	int n = ((k - off + 7) >> 3) << 3;
	int mode = V.fe.mode0;
	int a;
	if (mode & 4) {
		int c = (nxv_hnew(&V.hPff) >> 3) & 7;
		a = 0x40 | ((~c & 7) << 3) | c;
	} else {
		int b = nxv_hnew(&V.hBrd);
		a = (b << 3) | b;
	}
	int m2 = ((mode >> 2) & 1) * 0x0c;
	for (long long x = d + off; n > 0; n--, x++) {
		int i = (int)(x & (NXV_ULA - 1));
		V.ula[i].attr[0] = a;
		V.ula[i].attr[1] = a;
		V.ula[i].bits = (V.ula[i].bits & 0xf0) | m2;
	}
}

// the raw counters k dots after (h, v) (k may be negative)
static void nxv_step(Computer* comp, int* h, int* v, int k) {
	int L = nx_tim(comp)->L;
	int F = nx_lines(comp);
	int hh = *h + k;
	int vv = *v;
	while (hh < 0) {
		hh += L;
		vv = (vv + F - 1) % F;
	}
	while (hh >= L) {
		hh -= L;
		vv = (vv + 1) % F;
	}
	*h = hh;
	*v = vv;
}

// The value the CPU latches from the ULA's floating bus at CLK_28 lt (zxula.vhd
// 308-345, 573): the update at the falling CLK_7 before it, gated by the period it
// falls in. -1 when the ULA has no byte there.
int nxv_float(Computer* comp, long long lt) {
	int h, v;
	nx_raw(comp, &h, &v);
	long long dr = NX.frameBase + h;	// the dot the ray is on
	long long p = (lt - 3) >> 2;		// the last update before the latch
	long long g = (lt - 1) >> 2;		// the period the latch falls in
	// the fetch, a line behind the ray, drawn up to the latch first: its registers are
	// the ones read, with no dots run again here
	if (nxv_drawing(comp) && !nxv_flushing && (V.lastDot < p))
		nxv_flush(comp, (p + 1 < dr) ? p + 1 : dr);
	nxvFetch fe;
	long long from;
	if (V.live && (V.lastDot >= p - 32) && (V.lastDot <= p)) {
		fe = V.fe;
		from = V.lastDot + 1;
	} else {
		memset(&fe, 0, sizeof(fe));
		from = p - 24;
	}
	for (long long d = from; d <= p; d++) {
		int hh = h, vv = v, hc, vc;
		nxv_step(comp, &hh, &vv, (int)(d - dr));
		nx_ula(comp, hh, vv, &hc, &vc, NULL);
		nxv_fetch(comp, &fe, d, hc, vc, 1);
	}
	int hh = h, vv = v, hc, vc;
	nxv_step(comp, &hh, &vv, (int)(g - dr));
	nx_ula(comp, hh, vv, &hc, &vc, NULL);
	if ((hc & 0x100) || nxv_vbord(vc) || !fe.fbEn) return -1;
	return fe.fbR;
}

// == the tilemap (tilemap.vhd) ==

// The tilemap's counters are the mixer's whc/wvc of the same dot: its output register
// (hcount(0) = 1) holds the pixel of clk28 2A+1 for half-pixel A, which is what puts
// nr_4C at (w >> 1), as a simulation of the core shows.
#define NXT_EDGE	1	// the S_IDLE latch: the edge ending sc = 0 of its whc pixel
#define NXT_ATTR	2	// clk28s from the latch to TILE_1, where nr_6C is read when stripped

// a tilemap byte: 6-bit adder in 256-byte units, bank 7 (8K) or bank 5 (16K)
static int nxv_tm_byte(Computer* comp, int base, int sub, long long t) {
	int a = ((((sub >> 8) + base) & 63) << 8) | (sub & 0xff);
	return nxv_bram(comp, (base >> 6) & 1, a, t);
}

// The 8 pixels of group K (tilemap pixels 8K..8K+7 of line wvc, w80: 80 columns), as
// the fetch latched at clk28 t made them: scroll, bases and control at t, memory as
// read then
static void nxv_tm_group(Computer* comp, int K, int w80, int wvc, long long t) {
	int sx = nxv_hat(&V.hTmSx, t);
	int sy = nxv_hat(&V.hTmSy, t);
	int mapb = nxv_hat(&V.hTmMap, t);
	int tileb = nxv_hat(&V.hTmTile, t);
	int ctl = nxv_hat(&V.hTmCtl, t);
	int defa = nxv_hat(&V.hTmAttr, t + NXT_ATTR);
	mapb = ((mapb >> 1) & 0x40) | (mapb & 0x3f);	// bank 7 in b6 here, as tm_map_base
	tileb = ((tileb >> 1) & 0x40) | (tileb & 0x3f);
	int strip = (ctl >> 5) & 1;
	int text = (ctl >> 3) & 1;
	int m512 = (ctl >> 1) & 1;
	int ontop = ctl & 1;
	int W = w80 ? 640 : 320;
	int ay = (sy + wvc) & 0xff;
	int yi = ay & 7;
	int row = (ay >> 3) * (W >> 3);
	int ax = ((sx + K * 8) % W + W) % W;
	// all the reads are at t: a cell or a pattern byte read again gives the same
	int cell0 = -1, tile = 0, attr = 0;
	int pa0 = -1, b = 0;
	for (int i = 0; i < 8; i++, ax = (ax + 1 == W) ? 0 : ax + 1) {
		int xi = ax & 7;
		int cell = row + (ax >> 3);
		if (cell != cell0) {
			cell0 = cell;
			if (strip) {
				tile = nxv_tm_byte(comp, mapb, cell, t);
				attr = defa;
			} else {
				tile = nxv_tm_byte(comp, mapb, cell * 2, t);
				attr = nxv_tm_byte(comp, mapb, cell * 2 + 1, t);
			}
		}
		int t9 = ((m512 & attr) << 8) | tile;
		int pix;
		if (text) {
			int pa = (t9 << 3) | yi;
			if (pa != pa0) b = nxv_tm_byte(comp, tileb, pa0 = pa, t);
			pix = (attr & 0xfe) | (((b << xi) >> 7) & 1);
		} else {
			int mx = ((attr >> 3) ^ (attr >> 1)) & 1;
			int ex = mx ? 7 - xi : xi;
			int ey = (attr & 4) ? 7 - yi : yi;
			int u = (attr & 2) ? ey : ex;
			int v = (attr & 2) ? ex : ey;
			int pa = (t9 << 5) | (v << 2) | (u >> 1);
			if (pa != pa0) b = nxv_tm_byte(comp, tileb, pa0 = pa, t);
			pix = (attr & 0xf0) | ((u & 1) ? (b & 15) : (b >> 4));
		}
		int below = ((attr & 1) | m512) & !ontop;
		V.tmPix[i] = pix | (below << 8) | (text << 9);
	}
}

// The tilemap's pixel at (whc, wvc), half h, drawn at dot D: b7:0 pixel, b8 below,
// b9 text mode, b10 shown (in its window, not the transparent nibble); 0 if none.
// The half of the line buffer on show holds what the fetch latched at the start of
// the period before; 40/80 columns (tm_mode) is taken at every 8th whc pixel, so the
// first period after a switch shows the other mode's 8 pixels in this one's addressing.
// nxv_tm_pixel in two: the group of dot D latched (both halves share it), its w80 given
static int nxv_tm_load(Computer* comp, long long D, int whc, int wvc) {
	int w80 = (nxv_hat(&V.hTmCtl, (D - (whc & 7)) * 4) >> 6) & 1;
	int start = w80 ? (whc & ~3) : (whc & ~7);		// this period's first whc pixel
	int m = w80;
	if (!(start & 7))
		m = (nxv_hat(&V.hTmCtl, (D - (whc & 7) - 8) * 4) >> 6) & 1;	// the mode of the period before
	int prev = (start - (m ? 4 : 8)) & 0x1ff;
	long long t = (D - ((whc - prev) & 0x1ff)) * 4 + NXT_EDGE;
	if (V.tmKey != t) {
		// hcount_eff at the latch: one group ahead; 40 columns count whc 384..511 as negative
		int K = m ? (((prev << 1) + 8) >> 3) & 0x7f : ((((prev >= 384) ? prev - 512 : prev) + 8) >> 3);
		nxv_tm_group(comp, K, m, wvc, t);
		V.tmKey = t;
	}
	return w80;
}

// and the pixel of half h, by the fields as they are
static inline int nxv_tm_pickw(Computer* comp, int w80, int whc, int h, int inwin, int tmtr) {
	int p = V.tmPix[(w80 ? (whc << 1) | h : whc) & 7];
	int en = inwin && ((p & 0x200) || ((p & 15) != tmtr));
	return p | (en << 10);
}

static inline int nxv_tm_pick(Computer* comp, int w80, int whc, int wvc, int h) {
	const unsigned short* f = V.f;
	int inwin = (whc >= (f[NXF_TCX1] << 1)) && (whc <= ((f[NXF_TCX2] << 1) | 1)) && (wvc >= f[NXF_TCY1]) && (wvc <= f[NXF_TCY2]);
	return nxv_tm_pickw(comp, w80, whc, h, inwin, f[NXF_TMTR]);
}

static int nxv_tm_pixel(Computer* comp, long long D, int whc, int wvc, int h) {
	if ((whc >= 320) || (wvc >= 256)) return 0;
	return nxv_tm_pick(comp, nxv_tm_load(comp, D, whc, wvc), whc, wvc, h);
}

// == the mixer ==

// the mixer (zxnext.vhd 7100-7356): the layers in their order, or blended
static inline __attribute__((always_inline)) int nxv_mixo(int order, int blend, int l2, int l2_t, int l2p, int s, int s_t,
		int fin, int fin_t, int umix, int umix_t, int u, int u_t, int tm, int tm_t, int below, int bad, int fb9) {
	int mix, mix_t, top, top_t, bot, bot_t;
	switch (blend) {
		case 0:
			mix = umix; mix_t = umix_t;
			top = tm; top_t = tm_t || below;
			bot = tm; bot_t = tm_t || !below;
			break;
		case 2:
			mix = fin; mix_t = fin_t;
			top = tm; top_t = 1;
			bot = tm; bot_t = 1;
			break;
		case 3:
			mix = tm; mix_t = tm_t;
			top = u; top_t = u_t || !below;
			bot = u; bot_t = u_t || below;
			break;
		default:
			mix = 0; mix_t = 1;
			if (below) {
				top = u; top_t = u_t;
				bot = tm; bot_t = tm_t;
			} else {
				top = tm; top_t = tm_t;
				bot = u; bot_t = u_t;
			}
			break;
	}
	int out = fb9;
	int bx = bad && tm_t && !s_t;
	switch (order) {
		case 0:		// SLU
			if (l2p) out = l2;
			else if (!s_t) out = s;
			else if (!l2_t) out = l2;
			else if (!fin_t) out = fin;
			break;
		case 1:		// LSU
			if (!l2_t) out = l2;
			else if (!s_t) out = s;
			else if (!fin_t) out = fin;
			break;
		case 2:		// SUL
			if (l2p) out = l2;
			else if (!s_t) out = s;
			else if (!fin_t) out = fin;
			else if (!l2_t) out = l2;
			break;
		case 3:		// LUS
			if (!l2_t) out = l2;
			else if (!fin_t && !bx) out = fin;
			else if (!s_t) out = s;
			break;
		case 4:		// USL
			if (l2p) out = l2;
			else if (!fin_t && !bx) out = fin;
			else if (!s_t) out = s;
			else if (!l2_t) out = l2;
			break;
		case 5:		// ULS
			if (l2p) out = l2;
			else if (!fin_t && !bx) out = fin;
			else if (!l2_t) out = l2;
			else if (!s_t) out = s;
			break;
		default: {	// blend: layer 2 + the blend source, 110 clamped, 111 less 5
			int ch[3];
			for (int k = 0; k < 3; k++) {
				int sum = ((l2 >> (6 - 3 * k)) & 7) + ((mix >> (6 - 3 * k)) & 7);
				if (order == 6) {
					if (sum > 7) sum = 7;
				} else if (!mix_t) {
					if (sum <= 4) sum = 0;
					else if (sum >= 12) sum = 7;
					else sum -= 5;
				}
				ch[k] = sum & 7;
			}
			int bl = (ch[0] << 6) | (ch[1] << 3) | ch[2];
			if (l2p) out = bl;
			else if (!top_t) out = top;
			else if (!s_t) out = s;
			else if (!bot_t) out = bot;
			else if (!l2_t) out = bl;
			break;
		}
	}
	return out;
}

// nxv_mixo in orders 0-5 with the tilemap off: fin = u, fin_t = u_t, bx = bad && !s_t
static inline int nxv_mix6(int order, int l2, int l2_t, int l2p, int s, int s_t, int u, int u_t, int bx, int fb9) {
	switch (order) {
		case 0:		// SLU
			if (l2p) return l2;
			if (!s_t) return s;
			if (!l2_t) return l2;
			if (!u_t) return u;
			break;
		case 1:		// LSU
			if (!l2_t) return l2;
			if (!s_t) return s;
			if (!u_t) return u;
			break;
		case 2:		// SUL
			if (l2p) return l2;
			if (!s_t) return s;
			if (!u_t) return u;
			if (!l2_t) return l2;
			break;
		case 3:		// LUS
			if (!l2_t) return l2;
			if (!u_t && !bx) return u;
			if (!s_t) return s;
			break;
		case 4:		// USL
			if (l2p) return l2;
			if (!u_t && !bx) return u;
			if (!s_t) return s;
			if (!l2_t) return l2;
			break;
		default:	// ULS
			if (l2p) return l2;
			if (!u_t && !bx) return u;
			if (!l2_t) return l2;
			if (!s_t) return s;
			break;
	}
	return fb9;
}

static void nxv_derive(Computer* comp) {
	const unsigned short* f = V.f;
	nxvDerived* g = &V.dv;
	g->plain = !f[NXF_TMEN] && !f[NXF_TMENB] && !f[NXF_LOEN] && !f[NXF_ULAP] && (f[NXF_ORDER] < 6);
	g->ulaOnly = g->plain && !f[NXF_L2EN] && !f[NXF_ULANEXT];
	g->flashOn = ((f[NXF_FLASH] >> 4) & 1) & !f[NXF_ULANEXT] & !f[NXF_ULAP];
	g->upal = (f[NXF_SEL43] & 1) << 2;
	g->spal = (f[NXF_SEL43] & 4) ? 6 : 2;
	g->tmpal = f[NXF_TMPAL] ? 7 : 3;
	g->l2pal = (f[NXF_SEL43] & 2) ? 5 : 1;
	g->fb9 = (f[NXF_FB] << 1) | (((f[NXF_FB] >> 1) | f[NXF_FB]) & 1);
	g->ucx1 = f[NXF_UCX1];
	g->ucx2 = f[NXF_UCX2];
	g->ucy1 = f[NXF_UCY1];
	g->ucy2 = ((f[NXF_UCY2] & 0xc0) == 0xc0) ? 0xbf : f[NXF_UCY2];
	if (f[NXF_SOVER]) {
		if (f[NXF_SCLIP]) {
			g->sxs = f[NXF_SCX1] << 1;
			g->sxe = (f[NXF_SCX2] << 1) | 1;
			g->sys = f[NXF_SCY1];
			g->sye = f[NXF_SCY2];
		} else {
			g->sxs = 0; g->sxe = 319; g->sys = 0; g->sye = 255;
		}
	} else {
		g->sxs = f[NXF_SCX1] + 32;
		g->sxe = f[NXF_SCX2] + 32;
		g->sys = f[NXF_SCY1] + 32;
		g->sye = f[NXF_SCY2] + 32;
	}
	int wide = (f[NXF_L2RES] != 0);
	g->l2cx1 = wide ? (f[NXF_L2CX1] << 1) : f[NXF_L2CX1];
	g->l2cx2 = wide ? ((f[NXF_L2CX2] << 1) | 1) : f[NXF_L2CX2];
	V.dirty = 0;
}

// What a dot reads that holds while no field moves: taken once for a run of dots
// (nxv_span_draw) or for one dot (nxv_draw_dot). The plain path (ULA, layer 2, sprites)
// reads the first part, the general one all of it.
typedef struct {
	const unsigned short* upal;
	const unsigned short* lpal;
	const unsigned short* spal;
	int tr, fb9, fmt, flashOn, ulaOff;
	int ucx1, ucx2, ucy1, ucy2;
	int sprOn, sxs, sxe, sys, sye, sover;
	int l2On, wide, res, l2cx1, l2cx2, l2cy1, l2cy2, l2sx, l2sy, l2ofs, l2bank;
	int direct;		// no memory write after the first layer 2 read
	// the rest of the general path: LoRes, ULA+, the tilemap, stencil, blend
	const unsigned short* tpal;
	int ulap, ulaen, blend, stencil;
	int loen, losx, losy, lomode, lobase, loofs, lohi;
	int tmen, tmenb, tmtop, tmtr, tcx1, tcx2, tcy1, tcy2;
} nxvSpan;

static void nxv_span_set(Computer* comp, nxvSpan* sp, long long D) {
	if (V.dirty) nxv_derive(comp);
	const nxvDerived* g = &V.dv;
	const unsigned short* f = V.f;
	sp->upal = V.pal[g->upal];
	sp->lpal = V.pal[g->l2pal];
	sp->spal = V.pal[g->spal];
	sp->tr = f[NXF_TR];
	sp->fb9 = g->fb9;
	sp->fmt = f[NXF_ULANEXT] ? f[NXF_FMT] : -1;
	sp->flashOn = g->flashOn;
	sp->ulaOff = !f[NXF_ULAEN];
	sp->ucx1 = g->ucx1;
	sp->ucx2 = g->ucx2;
	sp->ucy1 = g->ucy1;
	sp->ucy2 = g->ucy2;
	sp->sprOn = f[NXF_SPREN];
	sp->sxs = g->sxs;
	sp->sxe = g->sxe;
	sp->sys = g->sys;
	sp->sye = g->sye;
	sp->sover = f[NXF_SOVER];
	sp->l2On = f[NXF_L2EN];
	sp->res = f[NXF_L2RES];
	sp->wide = (sp->res != 0);
	sp->l2cx1 = g->l2cx1;
	sp->l2cx2 = g->l2cx2;
	sp->l2cy1 = f[NXF_L2CY1];
	sp->l2cy2 = f[NXF_L2CY2];
	sp->l2sx = f[NXF_L2SX];
	sp->l2sy = f[NXF_L2SY];
	sp->l2ofs = f[NXF_L2OFS];
	sp->l2bank = f[NXF_L2BANK];
	// nxv_mem's first test: the newest write is before the read, so is every later read's
	sp->direct = (V.wr[(V.wrHead - 1) & (NXV_WRQ - 1)].w <= (D - 1) * 4 + 2);
	sp->tpal = V.pal[g->tmpal];
	sp->ulap = f[NXF_ULAP];
	sp->ulaen = f[NXF_ULAEN];
	sp->blend = f[NXF_BLEND];
	sp->stencil = f[NXF_STENCIL] && f[NXF_ULAEN] && f[NXF_TMEN];
	sp->loen = f[NXF_LOEN];
	sp->losx = f[NXF_LOSX];
	sp->losy = f[NXF_LOSY];
	sp->lomode = f[NXF_LOMODE];
	sp->lobase = (f[NXF_DFILE] ^ f[NXF_LOXOR]) << 13;
	sp->loofs = f[NXF_LOOFS];
	sp->lohi = f[NXF_LORAD] ? (0x0c | (sp->loofs & 3)) : sp->loofs;
	sp->tmen = f[NXF_TMEN];
	sp->tmenb = f[NXF_TMENB];
	sp->tmtop = f[NXF_TMTOP];
	sp->tmtr = f[NXF_TMTR];
	sp->tcx1 = f[NXF_TCX1] << 1;
	sp->tcx2 = (f[NXF_TCX2] << 1) | 1;
	sp->tcy1 = f[NXF_TCY1];
	sp->tcy2 = f[NXF_TCY2];
}

// layer 2's byte for the dot, -1 outside its area (one read; 4-bit pixels take a nibble a half)
// The SRAM address layer 2 fetches for the pixel period (phc, vc, wvc), -1 for none
// (layer2.vhd 147-185): clip x in its own units (doubled when wide), banks from 112 on
// make no SRAM cycle
static inline int nxv_l2_adr(int wide, int cx1, int cx2, int cy1, int cy2, int sx, int sy, int bank0,
		int phc, int vc, int wvc) {
	int c = wide ? ((phc + 32) & 0x1ff) : phc;
	int yv = wide ? wvc : vc;
	int hv = wide ? (c < 320) : (c < 256);
	int vv = wide ? (yv < 256) : (yv < 192);
	if (!(hv && vv && (c >= cx1) && (c <= cx2) && (yv >= cy1) && (yv <= cy2))) return -1;
	int xpre = c + sx;
	int xx = wide ? ((xpre >= 320) ? (((xpre & 0x1ff) + 192) & 0x1ff) : xpre) : (xpre & 0xff);
	int ypre = (yv + sy) & 0x1ff;
	int yy = (!wide && ((ypre & 0x100) || ((ypre & 0xc0) == 0xc0))) ? ((ypre + 0x40) & 0xff) : (ypre & 0xff);
	int adr = wide ? ((xx << 8) | yy) : ((yy << 8) | xx);
	int bank = bank0 + (adr >> 14);
	if (bank >= 112) return -1;
	return NXV_RAM + bank * 0x4000 + (adr & 0x3fff);
}

static inline int nxv_span_l2(Computer* comp, const nxvSpan* sp, int phc, int vc, int wvc, long long D) {
	int sram = nxv_l2_adr(sp->wide, sp->l2cx1, sp->l2cx2, sp->l2cy1, sp->l2cy2, sp->l2sx, sp->l2sy, sp->l2bank, phc, vc, wvc);
	if (sram < 0) return -1;
	int key = nxv_key_sram(sram);
	// no write after the run's first read: memory as it stands
	return sp->direct ? nxv_peek(comp, key) : nxv_mem(comp, key, (D - 1) * 4 + 2);
}

// == pieces of a dot, shared by the paths below (inlined into each) ==

#define NXV_INL static inline __attribute__((always_inline))

// the dot's two halves alike in the ULA's pixel and attribute
NXV_INL int nxv_alike(Computer* comp, int ui, int bits) {
	return (V.ula[ui].attr[0] == V.ula[ui].attr[1]) && !((bits ^ (bits >> 1)) & 1);
}

// ULANext's paper: the attribute's bits above the ink mask, past 0x80; -1 for a mask not
// of that shape (the fallback colour)
NXV_INL int nxv_unext_paper(int fmt, int attr) {
	switch (fmt) {
		case 0x01: return 0x80 | (attr >> 1);
		case 0x03: return 0x80 | (attr >> 2);
		case 0x07: return 0x80 | (attr >> 3);
		case 0x0f: return 0x80 | (attr >> 4);
		case 0x1f: return 0x80 | (attr >> 5);
		case 0x3f: return 0x80 | (attr >> 6);
		case 0x7f: return 0x80 | (attr >> 7);
		default: return -1;
	}
}

// the sprite pixel of the dot, in the span's window: 0 and its colour in *s, else 1
NXV_INL int nxv_span_spr(Computer* comp, const nxvSpan* sp, long long D, int whc, int wvc, int* s) {
	*s = 0;
	if (sp->sprOn && (whc < 320) && (wvc < 256)) {
		int sb = nxs_line(comp, D)[whc];
		if ((sb & 0x100) && (whc >= sp->sxs) && (whc <= sp->sxe) && (wvc >= sp->sys) && (wvc <= sp->sye) && (sp->sover || (wvc < 224))) {
			*s = sp->spal[sb & 0xff] & 0x1ff;
			return 0;
		}
	}
	return 1;
}

// layer 2's colour of half h from the dot's byte (-1: none): 0 and the colour and its
// priority bit, else 1
NXV_INL int nxv_span_l2pix(const nxvSpan* sp, int l2byte, int h, int* l2, int* l2p) {
	*l2 = 0;
	*l2p = 0;
	if (l2byte < 0) return 1;
	int px = (sp->res & 2) ? (h ? (l2byte & 15) : (l2byte >> 4)) : l2byte;
	int li = ((((px >> 4) + sp->l2ofs) & 15) << 4) | (px & 15);
	int w = sp->lpal[li];
	int rgb = w & 0x1ff;
	if ((rgb >> 1) == sp->tr) return 1;
	*l2 = rgb;
	*l2p = (w >> 10) & 1;
	return 0;
}

// one dot on the plain path: the ULA, layer 2 and the sprite pixel, mixed in the order
NXV_INL void nxv_plain_dot(Computer* comp, const nxvSpan* sp, const int order,
		long long D, int phc, int vc, int wvc, int bad, int whc, unsigned int* ptr) {
	int s;
	int s_t = nxv_span_spr(comp, sp, D, whc, wvc, &s);
	int l2byte = sp->l2On ? nxv_span_l2(comp, sp, phc, vc, wvc, D) : -1;
	int ui = (int)(D & (NXV_ULA - 1));
	int bits = V.ula[ui].bits;
	int inwin = (phc >= sp->ucx1) && (phc <= sp->ucx2) && (vc >= sp->ucy1) && (vc <= sp->ucy2);
	int clip = !(inwin || bad) || sp->ulaOff;
	int bx = bad && !s_t;
	// the two halves alike (and layer 2's byte not split in nibbles): the first one's
	// colour is the second's
	int one = nxv_alike(comp, ui, bits) && !((l2byte >= 0) && (sp->res & 2));
	for (int h = 0; h < 2 - one; h++) {
		int attr = V.ula[ui].attr[h];
		int pen = (((bits >> h) & 1) ^ ((attr >> 7) & sp->flashOn)) & !bad;
		int urgb;
		if (sp->fmt < 0) {
			int ink = attr & 7;
			int paper = (attr >> 3) & 7;
			urgb = sp->upal[((pen ^ 1) << 4) | (((attr >> 6) & 1) << 3) | (paper ^ ((ink ^ paper) & -pen))] & 0x1ff;
		} else if (bad) {
			urgb = (sp->fmt == 0xff) ? sp->fb9 : sp->upal[0x80 | ((attr >> 3) & 7)] & 0x1ff;
		} else if (pen) {
			urgb = sp->upal[attr & sp->fmt] & 0x1ff;
		} else {
			int idx = nxv_unext_paper(sp->fmt, attr);
			urgb = (idx < 0) ? sp->fb9 : sp->upal[idx] & 0x1ff;
		}
		int u_t = ((urgb >> 1) == sp->tr) || clip;
		int u = u_t ? 0 : urgb;
		int l2, l2p;
		int l2_t = nxv_span_l2pix(sp, l2byte, h, &l2, &l2p);
		ptr[h] = nxv_col[nxv_mix6(order, l2, l2_t, l2p, s, s_t, u, u_t, bx, sp->fb9) & 0x1ff];
	}
	if (one) ptr[1] = ptr[0];
}

// nxv_plain_dot for one dot, kept out of nxv_draw_dot (inlined there it slows the other paths)
static __attribute__((noinline)) void nxv_plain_one(Computer* comp, long long D, int phc, int vc, int wvc, int bad, int whc, unsigned int* ptr) {
	nxvSpan sp;
	nxv_span_set(comp, &sp, D);
	nxv_plain_dot(comp, &sp, V.f[NXF_ORDER], D, phc, vc, wvc, bad, whc, ptr);
}

// nxv_tm_load's answer kept over the dots of a run (nxv_gen_dot); blk LLONG_MIN: none
typedef struct {
	long long blk;
	int w80;
} nxvTmKept;

// one dot on the general path (LoRes, ULANext/ULA+, the tilemap, blend): nxv_draw's mix
// with the fields taken from the span. Memory reads as nxv_mem makes them: straight
// from memory when the span saw no write after its first read.
static inline __attribute__((always_inline)) void nxv_gen_dot(Computer* comp, const nxvSpan* sp, const int order,
		long long D, int phc, int vc, int wvc, int bad, int whc, unsigned int* ptr, nxvTmKept* tk) {
	int s;
	int s_t = nxv_span_spr(comp, sp, D, whc, wvc, &s);
	int l2byte = sp->l2On ? nxv_span_l2(comp, sp, phc, vc, wvc, D) : -1;
	int ui = (int)(D & (NXV_ULA - 1));
	int bits = V.ula[ui].bits;
	int inwin = (phc >= sp->ucx1) && (phc <= sp->ucx2) && (vc >= sp->ucy1) && (vc <= sp->ucy2);
	// LoRes, in the ULA's clip window: one read for the dot
	int lpen = inwin && sp->loen;
	int lidx = 0;
	if (lpen) {
		int x = (phc + sp->losx) & 0xff;
		int ypre = (vc + sp->losy) & 0x1ff;
		int y = (ypre >= 192) ? ((ypre + 0x40) & 0xff) : ypre;
		int a;
		if (sp->lomode) {
			a = sp->lobase | ((y >> 1) << 6) | (x >> 2);
		} else {
			a = ((y >> 1) << 7) | (x >> 1);
			if (y >= 96) a = (a + 0x800) & 0x3fff;
		}
		int key = nxv_key_bram(0, a);
		int b = sp->direct ? nxv_peek(comp, key) : nxv_mem(comp, key, D * 4 + 2);
		if (sp->lomode) {
			int nib = (x & 2) ? (b & 15) : (b >> 4);
			lidx = (sp->lohi << 4) | nib;
		} else {
			lidx = ((((b >> 4) + sp->loofs) & 15) << 4) | (b & 15);
		}
	}
	int uclip = !(inwin || bad) && !lpen;
	// the tilemap's group, for both halves
	int tmw80 = -1;
	if ((sp->tmen || sp->tmenb) && (whc < 320) && (wvc < 256)) {
		// the load's answer and the group it latches are one for the four dots of a block
		// (whc & ~3): asked once a block over a run
		long long blk = D - (whc & 3);
		if (tk->blk != blk) {
			tk->w80 = nxv_tm_load(comp, D, whc, wvc);
			tk->blk = blk;
		}
		tmw80 = tk->w80;
	}
	int tmwin = (whc >= sp->tcx1) && (whc <= sp->tcx2) && (wvc >= sp->tcy1) && (wvc <= sp->tcy2);
	// the two halves alike: as nxv_plain_dot, and the tilemap not in 80 columns (the ULA+
	// bits b2, b3 are set together)
	int one = nxv_alike(comp, ui, bits) && !((l2byte >= 0) && (sp->res & 2)) && (tmw80 != 1);
	for (int h = 0; h < 2 - one; h++) {
		int attr = V.ula[ui].attr[h];
		int pix = (bits >> h) & 1;
		int m2 = (bits >> (2 + h)) & 1;
		int pen = (pix ^ ((attr >> 7) & sp->flashOn)) & !bad;
		int idx = 0;
		int bg = 0;
		if (sp->fmt >= 0) {
			if (bad) {
				if (sp->fmt == 0xff) bg = 1;
				idx = 0x80 | ((attr >> 3) & 7);
			} else if (pen) {
				idx = attr & sp->fmt;
			} else {
				idx = nxv_unext_paper(sp->fmt, attr);
				bg = (idx < 0);
				if (bg) idx = 0;
			}
		} else {
			int ink = attr & 7;
			int paper = (attr >> 3) & 7;
			int col = paper ^ ((ink ^ paper) & -pen);
			if (sp->ulap)
				idx = 0xc0 | ((attr >> 6) << 4) | ((m2 | (pen ^ 1)) << 3) | col;
			else
				idx = ((pen ^ 1) << 4) | (((attr >> 6) & 1) << 3) | col;
		}
		int urgb = (lpen ? sp->upal[lidx] : bg ? sp->fb9 : sp->upal[idx]) & 0x1ff;
		int umix_t = ((urgb >> 1) == sp->tr) || uclip;
		int umix = umix_t ? 0 : urgb;
		int u_t = umix_t || !sp->ulaen;
		int u = u_t ? 0 : urgb;
		// the tilemap, in the span's window
		int tp = (tmw80 >= 0) ? nxv_tm_pickw(comp, tmw80, whc, h, tmwin, sp->tmtr) : 0;
		int tm = sp->tpal[tp & 0xff] & 0x1ff;
		int tm_t = !(tp & 0x400) || ((tp & 0x200) && ((tm >> 1) == sp->tr)) || !sp->tmen;
		if (tm_t) tm = 0;
		int below = sp->tmenb ? (tp >> 8) & 1 : !sp->tmtop;
		int fin, fin_t;
		if (sp->stencil) {
			fin_t = u_t || tm_t;
			fin = fin_t ? 0 : (u & tm);
		} else {
			fin_t = u_t && tm_t;
			fin = (!tm_t && (!below || u_t)) ? tm : u;
		}
		int l2, l2p;
		int l2_t = nxv_span_l2pix(sp, l2byte, h, &l2, &l2p);
		ptr[h] = nxv_col[nxv_mixo(order, sp->blend, l2, l2_t, l2p, s, s_t, fin, fin_t, umix, umix_t, u, u_t, tm, tm_t, below, bad, sp->fb9) & 0x1ff];
	}
	if (one) ptr[1] = ptr[0];
}

// nxv_gen_dot for one dot, kept out of nxv_draw_dot
static __attribute__((noinline)) void nxv_gen_one(Computer* comp, long long D, int phc, int vc, int wvc, int bad, int whc, unsigned int* ptr) {
	nxvSpan sp;
	nxvTmKept tk = {LLONG_MIN, 0};
	nxv_span_set(comp, &sp, D);
	nxv_gen_dot(comp, &sp, V.f[NXF_ORDER], D, phc, vc, wvc, bad, whc, ptr, &tk);
}

// One dot drawn on its own where no field moves between its two half-pixels (the dot of
// an event that lands past it): the plain path or the general one, as a run of such
// dots would take (nxv_span_draw)
static void nxv_draw_dot(Computer* comp, unsigned int* ptr, long long D, int phc, int vc, int wvc, int whc, int bad) {
	if (V.dirty) nxv_derive(comp);
	if (V.dv.plain)
		nxv_plain_one(comp, D, phc, vc, wvc, bad, whc, ptr);
	else
		nxv_gen_one(comp, D, phc, vc, wvc, bad, whc, ptr);
}

// what a raw line gives every dot of it: vc_ula and wvc before and after they step
typedef struct {
	int ry;
	int vcA, vcB;		// lv = ry, ry - 1
	int wvcA, wvcB;		// wl = ry, ry - 1
	// the raster drawn (vid->dlt, drb), in raw counters: the dot of raw rx goes
	// to column rx - 1, so rx from cx0 to cx1 (exclusive), lines cy0 to cy1
	int cx0, cx1, cy0, cy1;
} nxvLine;

static void nxv_line_set(nxvLine* ln, int ry, int F, int vact) {
	int prev = (ry > 0) ? ry - 1 : ry - 1 + F;
	ln->ry = ry;
	ln->vcA = (ry >= vact) ? ry - vact : 0x1ff;
	ln->vcB = (prev >= vact) ? prev - vact : 0x1ff;
	ln->wvcA = (ry >= vact - 34) ? ry - vact + 32 : 0x1ff;
	ln->wvcB = (prev >= vact - 34) ? prev - vact + 32 : 0x1ff;
}

// the counters of the pixel period of the dot at raw rx on the line: phc, vc_ula, wvc
// (zxula_timing.vhd 423-520)
NXV_INL void nxv_rx_ctr(const nxTiming* t, int L, const nxvLine* ln, int rx, int* phc, int* vc, int* wvc) {
	*phc = ((rx >= t->hact - 47) ? rx - t->hact - 1 : rx + L - t->hact - 1) & 0x1ff;
	*vc = (rx >= t->hact - 11) ? ln->vcA : ln->vcB;
	*wvc = (rx >= t->hact - 47) ? ln->wvcA : ln->wvcB;
}

// == the SRAM bus a chip that is not fitted leaves (zxnext.c nx_absent_rd) ==

// line yv is one of layer 2's rows
static inline int nxv_l2_row(int wide, int cy1, int cy2, int yv) {
	return (wide ? (yv < 256) : (yv < 192)) && (yv >= cy1) && (yv <= cy2);
}

// The last layer 2 fetch of fitted SRAM on bus half `lane` (A0) at a CLK_28 edge in
// (since, now]: that edge, and the byte it read in *byte; -1 for none, a frame back at
// most. The fetch for the pixel of dot D is at edge (D - 1) * 4 + 2 (layer2.vhd 175-186,
// as the mixer reads it). In the 256 mode the halves take turns pixel by pixel, in the
// wide ones a line is all on one (the address is x << 8 | y). The registers are taken as
// they stand now; a fetch from a chip not fitted drives nothing and is passed over.
// Lines with none of its rows are jumped; on the rest the walk stops after four lines'
// dots (a clip one column wide, a bank across the end of the chips), taken as none.
long long nxv_l2_lane(Computer* comp, int lane, long long since, long long now, int* byte) {
	unsigned short q[NXF_CMP];
	nxv_fields(comp, q);
	if (!q[NXF_L2EN]) return -1;
	int size = comp->mem->ramSize;
	if (NXV_RAM + q[NXF_L2BANK] * 0x4000 >= size) return -1;	// all of it past the chips
	int wide = (q[NXF_L2RES] != 0);
	int cx1 = wide ? (q[NXF_L2CX1] << 1) : q[NXF_L2CX1];
	int cx2 = wide ? ((q[NXF_L2CX2] << 1) | 1) : q[NXF_L2CX2];
	int cy1 = q[NXF_L2CY1];
	int cy2 = q[NXF_L2CY2];
	const nxTiming* t = nx_tim(comp);
	int L = t->L;
	int F = nx_lines(comp);
	int vact = nx_vact(comp);
	long long D = nx_fdiv(now - 2, 4) + 1;		// the dot of the last fetch edge up to now
	long long lo = nx_fdiv(since - 2, 4) + 2;	// the first dot whose fetch is after since
	if (lo < D - (long long)L * F) lo = D - (long long)L * F;
	int rx, ry;
	nx_raw_at(comp, D * 4, &rx, &ry);
	nxvLine ln;
	memset(&ln, 0, sizeof(ln));
	ln.ry = -1;		// set at the first dot
	int left = 4 * L;	// dots to look at on lines with layer 2 rows
	while ((D >= lo) && (left-- > 0)) {
		if (ry != ln.ry) {
			nxv_line_set(&ln, ry, F, vact);
			// none of layer 2's rows on this line: on to the end of the one before
			if (!nxv_l2_row(wide, cy1, cy2, wide ? ln.wvcA : ln.vcA) && !nxv_l2_row(wide, cy1, cy2, wide ? ln.wvcB : ln.vcB)) {
				D -= rx + 1;
				rx = L - 1;
				ry = (ry + F - 1) % F;
				left++;		// a jump is not a dot looked at
				continue;
			}
		}
		int phc, vc, wvc;
		nxv_rx_ctr(t, L, &ln, rx, &phc, &vc, &wvc);
		int sram = nxv_l2_adr(wide, cx1, cx2, cy1, cy2, q[NXF_L2SX], q[NXF_L2SY], q[NXF_L2BANK], phc, vc, wvc);
		if ((sram >= 0) && ((sram & 1) == lane) && (sram < size)) {
			long long e = (D - 1) * 4 + 2;
			*byte = nxv_mem(comp, nxv_key_sram(sram), e);
			return e;
		}
		D--;
		if (--rx < 0) {
			rx = L - 1;
			ry = (ry + F - 1) % F;
		}
	}
	return -1;
}

static void nxv_draw(Computer* comp, const nxTiming* t, int L, int F, int vact, const nxvLine* ln) {
	long long D = V.cur;
	long long A = D * 2;
	// the image: the dot before this one's raw position (the pixel of phc P is drawn
	// at the column of raw P + hact, so that paper pixel 0 lands on the screen origin)
	int ix = V.rx - 1;
	int iy = V.ry;
	if (ix < 0) {
		ix = L - 1;
		iy = (iy + F - 1) % F;
	}
	if ((ix < ln->cx0 - 1) || (ix >= ln->cx1 - 1) || (iy < ln->cy0) || (iy >= ln->cy1) || comp->vid->nodraw) {
		nxv_apply_to(comp, A + 1);
		return;
	}
	unsigned int* ptr = (unsigned int*)(scrimg + (iy - t->vblank) * bytesPerLine + (ix - t->hblank) * 8);
	// the counters of this pixel's period: phc, vc_ula, wvc (zxula_timing.vhd 423-520)
	int phc, vc, wvc;
	nxv_rx_ctr(t, L, ln, V.rx, &phc, &vc, &wvc);
	int bad = (phc & 0x100) || nxv_vbord(vc);	// border_active
	int whc = (phc + 32) & 0x1ff;
	int sb = 0;					// the sprite line buffer's entry for whc
	if ((whc < 320) && (wvc < 256)) {
		sb = nxs_line(comp, D)[whc];
		if (!(sb & 0x100)) sb = 0;
	}
	int ui = (int)(D & (NXV_ULA - 1));
	nxv_apply_to(comp, A);
	if (!((V.evHead < V.evN) && (V.ev[V.evHead].t <= A + 1)) && !((NX.timing == NXT_PENT) && bad)) {
		nxv_draw_dot(comp, ptr, D, phc, vc, wvc, whc, bad);
		return;
	}
	for (int h = 0; h < 2; h++) {
		nxv_apply(comp, A + h);
		unsigned short* f = V.f;
		// -- the ULA's palette index (zxula.vhd 470-558) --
		int attr = V.ula[ui].attr[h];
		int pix = (V.ula[ui].bits >> h) & 1;
		int m2 = (V.ula[ui].bits >> (2 + h)) & 1;
		if ((NX.timing == NXT_PENT) && bad) {
			// the Pentagon's border colour is taken again on every CLK_14 edge
			long long te = D * 4 + 2 + h * 2;
			if (m2) {
				int c = (nxv_hat(&V.hPff, te) >> 3) & 7;
				attr = 0x40 | ((~c & 7) << 3) | c;
			} else {
				int b = nxv_hat(&V.hBrd, te);
				attr = (b << 3) | b;
			}
		}
		int flash = (f[NXF_FLASH] >> 4) & 1;
		int pen = (pix ^ ((attr >> 7) & flash & !f[NXF_ULANEXT] & !f[NXF_ULAP])) & !bad;
		int idx = 0;
		int bg = 0;
		if (f[NXF_ULANEXT]) {
			int fmt = f[NXF_FMT];
			if (bad) {
				if (fmt == 0xff) bg = 1;
				idx = 0x80 | ((attr >> 3) & 7);
			} else if (pen) {
				idx = attr & fmt;
			} else {
				idx = nxv_unext_paper(fmt, attr);
				bg = (idx < 0);
				if (bg) idx = 0;
			}
		} else {
			// ink or paper by the pixel bit, without a branch: the bit is random, a
			// branch on it mispredicts on every other pixel of text
			int ink = attr & 7;
			int paper = (attr >> 3) & 7;
			int col = paper ^ ((ink ^ paper) & -pen);
			if (f[NXF_ULAP])
				idx = 0xc0 | ((attr >> 6) << 4) | ((m2 | (pen ^ 1)) << 3) | col;
			else
				idx = ((pen ^ 1) << 4) | (((attr >> 6) & 1) << 3) | col;
		}
		// -- LoRes (lores.vhd), in the ULA's clip window --
		int y2 = ((f[NXF_UCY2] & 0xc0) == 0xc0) ? 0xbf : f[NXF_UCY2];
		int inwin = (phc >= f[NXF_UCX1]) && (phc <= f[NXF_UCX2]) && (vc >= f[NXF_UCY1]) && (vc <= y2);
		int lpen = inwin && f[NXF_LOEN];
		int lidx = 0;
		if (lpen) {
			int x = (phc + f[NXF_LOSX]) & 0xff;
			int ypre = (vc + f[NXF_LOSY]) & 0x1ff;
			int y = (ypre >= 192) ? ((ypre + 0x40) & 0xff) : ypre;
			int ofs = f[NXF_LOOFS];
			int a;
			if (f[NXF_LOMODE]) {
				a = ((f[NXF_DFILE] ^ f[NXF_LOXOR]) << 13) | ((y >> 1) << 6) | (x >> 2);
			} else {
				a = ((y >> 1) << 7) | (x >> 1);
				if (y >= 96) a = (a + 0x800) & 0x3fff;
			}
			int b = nxv_bram(comp, 0, a, D * 4 + 2);
			if (f[NXF_LOMODE]) {
				int nib = (x & 2) ? (b & 15) : (b >> 4);
				int hi = f[NXF_LORAD] ? (0x0c | (ofs & 3)) : ofs;
				lidx = (hi << 4) | nib;
			} else {
				lidx = ((((b >> 4) + ofs) & 15) << 4) | (b & 15);
			}
		}
		int fb9 = (f[NXF_FB] << 1) | (((f[NXF_FB] >> 1) | f[NXF_FB]) & 1);
		// (a pointer: a palette number kept in 16 bits was spilt and read back in 32, a
		// store forwarding stall on every pixel)
		const unsigned short* upal = V.pal[(f[NXF_SEL43] & 1) << 2];
		int urgb = (lpen ? upal[lidx] : bg ? fb9 : upal[idx]) & 0x1ff;
		int tr = f[NXF_TR];
		int uclip = !(inwin || bad) && !lpen;
		int umix_t = ((urgb >> 1) == tr) || uclip;
		int umix = umix_t ? 0 : urgb;
		int u_t = umix_t || !f[NXF_ULAEN];
		int u = u_t ? 0 : urgb;
		// -- the tilemap --
		int tp = (f[NXF_TMEN] || f[NXF_TMENB]) ? nxv_tm_pixel(comp, D, whc, wvc, h) : 0;
		int tm = V.pal[f[NXF_TMPAL] ? 7 : 3][tp & 0xff] & 0x1ff;
		int tm_t = !(tp & 0x400) || ((tp & 0x200) && ((tm >> 1) == f[NXF_TR])) || !f[NXF_TMEN];
		if (tm_t) tm = 0;
		int below = f[NXF_TMENB] ? (tp >> 8) & 1 : !f[NXF_TMTOP];
		// -- sprites: the line buffer the pass drew, in its window (sprites.vhd 1000-1070) --
		int s_t = 1;
		int s = 0;
		if (sb && f[NXF_SPREN]) {
			int xs, xe, ys, ye;
			if (f[NXF_SOVER]) {
				if (f[NXF_SCLIP]) {
					xs = f[NXF_SCX1] << 1;
					xe = (f[NXF_SCX2] << 1) | 1;
					ys = f[NXF_SCY1];
					ye = f[NXF_SCY2];
				} else {
					xs = 0; xe = 319; ys = 0; ye = 255;
				}
			} else {
				xs = f[NXF_SCX1] + 32;
				xe = f[NXF_SCX2] + 32;
				ys = f[NXF_SCY1] + 32;
				ye = f[NXF_SCY2] + 32;
			}
			if ((whc >= xs) && (whc <= xe) && (wvc >= ys) && (wvc <= ye) && (f[NXF_SOVER] || (wvc < 224))) {
				s = V.pal[(f[NXF_SEL43] & 4) ? 6 : 2][sb & 0xff] & 0x1ff;
				s_t = 0;
			}
		}
		int fin, fin_t;
		if (f[NXF_STENCIL] && f[NXF_ULAEN] && f[NXF_TMEN]) {
			fin_t = u_t || tm_t;
			fin = fin_t ? 0 : (u & tm);
		} else {
			fin_t = u_t && tm_t;
			fin = (!tm_t && (!below || u_t)) ? tm : u;
		}
		// -- layer 2 (layer2.vhd) --
		int res = f[NXF_L2RES];
		int wide = (res != 0);
		int c = wide ? ((phc + 32) & 0x1ff) : phc;
		int yv = wide ? wvc : vc;
		int l2 = 0, l2_t = 1, l2p = 0;
		if (f[NXF_L2EN]) {
			int hv = wide ? (c < 320) : (c < 256);
			int vv = wide ? (yv < 256) : (yv < 192);
			int cx1 = wide ? (f[NXF_L2CX1] << 1) : f[NXF_L2CX1];
			int cx2 = wide ? ((f[NXF_L2CX2] << 1) | 1) : f[NXF_L2CX2];
			if (hv && vv && (c >= cx1) && (c <= cx2) && (yv >= f[NXF_L2CY1]) && (yv <= f[NXF_L2CY2])) {
				int xpre = c + f[NXF_L2SX];
				int xx = wide ? ((xpre >= 320) ? (((xpre & 0x1ff) + 192) & 0x1ff) : xpre) : (xpre & 0xff);
				int ypre = (yv + f[NXF_L2SY]) & 0x1ff;
				int yy = (!wide && ((ypre & 0x100) || ((ypre & 0xc0) == 0xc0))) ? ((ypre + 0x40) & 0xff) : (ypre & 0xff);
				int adr = wide ? ((xx << 8) | yy) : ((yy << 8) | xx);
				int bank = f[NXF_L2BANK] + (adr >> 14);
				if (bank < 112) {
					int byte = nxv_sram(comp, NXV_RAM + bank * 0x4000 + (adr & 0x3fff), (D - 1) * 4 + 2);
					int px = (res & 2) ? (h ? (byte & 15) : (byte >> 4)) : byte;
					int li = ((((px >> 4) + f[NXF_L2OFS]) & 15) << 4) | (px & 15);
					int w = V.pal[(f[NXF_SEL43] & 2) ? 5 : 1][li];
					int rgb = w & 0x1ff;
					if ((rgb >> 1) != tr) {
						l2 = rgb;
						l2_t = 0;
						l2p = (w >> 10) & 1;
					}
				}
			}
		}
		int out = nxv_mixo(f[NXF_ORDER], f[NXF_BLEND], l2, l2_t, l2p, s, s_t, fin, fin_t, umix, umix_t, u, u_t, tm, tm_t, below, bad, fb9);
		ptr[h] = nxv_col[out & 0x1ff];
	}
}

// A run of dots on one line with no event in it, on the plain (gen 0) or the general
// path: what holds over the run is taken once, and the path and the layer order are
// constants of each copy of the loop
static inline __attribute__((always_inline)) void nxv_span_run(Computer* comp, const nxvSpan* sp, const int gen, const int order,
		const nxTiming* t, int L, const nxvLine* ln, int rx, int rxe, long long D, unsigned int* ptr) {
	nxvTmKept tk = {LLONG_MIN, 0};
	for (; rx < rxe; rx++, D++, ptr += 2) {
		int phc, vc, wvc;
		nxv_rx_ctr(t, L, ln, rx, &phc, &vc, &wvc);
		int bad = (phc & 0x100) || nxv_vbord(vc);
		if (gen)
			nxv_gen_dot(comp, sp, order, D, phc, vc, wvc, bad, (phc + 32) & 0x1ff, ptr, &tk);
		else
			nxv_plain_dot(comp, sp, order, D, phc, vc, wvc, bad, (phc + 32) & 0x1ff, ptr);
	}
}

// the dot of raw rx is border (border_active), as nxv_draw has it
static inline int nxv_bad_at(const nxTiming* t, int L, const nxvLine* ln, int rx) {
	int phc, vc, wvc;
	nxv_rx_ctr(t, L, ln, rx, &phc, &vc, &wvc);
	return (phc & 0x100) || nxv_vbord(vc);
}

static void nxv_span_draw(Computer* comp, const nxTiming* t, int L, const nxvLine* ln, int rx, int rxe, long long D, unsigned int* ptr) {
	nxvSpan sp;
	nxv_span_set(comp, &sp, D);
	if (V.dv.plain) {
		switch (V.f[NXF_ORDER]) {
			case 0: nxv_span_run(comp, &sp, 0, 0, t, L, ln, rx, rxe, D, ptr); break;
			case 1: nxv_span_run(comp, &sp, 0, 1, t, L, ln, rx, rxe, D, ptr); break;
			case 2: nxv_span_run(comp, &sp, 0, 2, t, L, ln, rx, rxe, D, ptr); break;
			case 3: nxv_span_run(comp, &sp, 0, 3, t, L, ln, rx, rxe, D, ptr); break;
			case 4: nxv_span_run(comp, &sp, 0, 4, t, L, ln, rx, rxe, D, ptr); break;
			default: nxv_span_run(comp, &sp, 0, 5, t, L, ln, rx, rxe, D, ptr); break;
		}
		return;
	}
	switch (V.f[NXF_ORDER]) {
		case 0: nxv_span_run(comp, &sp, 1, 0, t, L, ln, rx, rxe, D, ptr); break;
		case 1: nxv_span_run(comp, &sp, 1, 1, t, L, ln, rx, rxe, D, ptr); break;
		case 2: nxv_span_run(comp, &sp, 1, 2, t, L, ln, rx, rxe, D, ptr); break;
		case 3: nxv_span_run(comp, &sp, 1, 3, t, L, ln, rx, rxe, D, ptr); break;
		case 4: nxv_span_run(comp, &sp, 1, 4, t, L, ln, rx, rxe, D, ptr); break;
		case 5: nxv_span_run(comp, &sp, 1, 5, t, L, ln, rx, rxe, D, ptr); break;
		case 6: nxv_span_run(comp, &sp, 1, 6, t, L, ln, rx, rxe, D, ptr); break;
		default: nxv_span_run(comp, &sp, 1, 7, t, L, ln, rx, rxe, D, ptr); break;
	}
}

// Nothing pushes an event while the mixer draws, so the dots of a line before the next
// one is due need no apply: an undrawn stretch is passed over whole, a drawn one goes to
// nxv_span_draw (the ULA alone: the loop here) with its counters worked out here. The
// dot of an event and a line's first dot take nxv_draw.
static void nxv_render(Computer* comp, long long end) {
	const nxTiming* t = nx_tim(comp);
	int L = t->L;
	int F = nx_lines(comp);
	int vact = nx_vact(comp);
	int nodraw = comp->vid->nodraw || comp->vid->still;
	int pent = (NX.timing == NXT_PENT);
	nxvLine ln;
	ln.ry = -1;
	Video* vid = comp->vid;
	if (vid->drawAll) {
		ln.cx0 = t->hblank + 1;
		ln.cx1 = L + 1;
		ln.cy0 = t->vblank;
		ln.cy1 = F;
	} else {
		ln.cx0 = t->hblank + 1 + vid->dlt.x;
		ln.cx1 = t->hblank + 1 + vid->drb.x;
		ln.cy0 = t->vblank + vid->dlt.y;
		ln.cy1 = t->vblank + vid->drb.y;
	}
	while (V.cur < end) {
		if (V.ry != ln.ry) nxv_line_set(&ln, V.ry, F, vact);
		// the dots with 2D + 1 short of the next event, on this line
		long long stop = (V.evHead < V.evN) ? (V.ev[V.evHead].t >> 1) : LLONG_MAX;
		if (stop > end) stop = end;
		if (stop > V.cur + L - V.rx) stop = V.cur + L - V.rx;
		if ((stop <= V.cur) || !V.rx) {
			nxv_draw(comp, t, L, F, vact, &ln);
			V.cur++;
			V.rx++;
		} else if (nodraw || (V.ry < ln.cy0) || (V.ry >= ln.cy1)) {
			V.rx += (int)(stop - V.cur);
			V.cur = stop;
		} else {
			int rx = V.rx;
			int rxa = rx + (int)(stop - V.cur);	// where the run ends
			int rxe = (rxa < ln.cx1) ? rxa : ln.cx1;	// and its drawn part
			long long D = V.cur;
			if (rx < ln.cx0) {
				int k = ((rxe < ln.cx0) ? rxe : ln.cx0) - rx;
				if (k < 0) k = 0;
				rx += k;
				D += k;
			}
			unsigned int* ptr = (unsigned int*)(scrimg + (V.ry - t->vblank) * bytesPerLine + (rx - 1 - t->hblank) * 8);
			if (V.dirty) nxv_derive(comp);
			if (!pent && V.dv.ulaOnly && !V.f[NXF_SPREN]) {
				// The ULA alone and no sprites (an editor, a menu): what holds over the
				// run taken once - no field moves in it
				const nxvDerived* g = &V.dv;
				const unsigned short* upal = V.pal[g->upal];
				int tr = V.f[NXF_TR];
				int fb9 = g->fb9;
				int off = !V.f[NXF_ULAEN];
				int fl = g->flashOn;
				for (; rx < rxe; rx++, D++, ptr += 2) {
					int phc, vc, wvc;
					nxv_rx_ctr(t, L, &ln, rx, &phc, &vc, &wvc);
					int bad = (phc & 0x100) || nxv_vbord(vc);
					int inwin = (phc >= g->ucx1) && (phc <= g->ucx2) && (vc >= g->ucy1) && (vc <= g->ucy2);
					int clip = !(inwin || bad) || off;
					int ui = (int)(D & (NXV_ULA - 1));
					int bits = V.ula[ui].bits;
					int one = nxv_alike(comp, ui, bits);
					for (int h = 0; h < 2 - one; h++) {
						int attr = V.ula[ui].attr[h];
						int pen = (((bits >> h) & 1) ^ ((attr >> 7) & fl)) & !bad;
						int ink = attr & 7;
						int paper = (attr >> 3) & 7;
						int sidx = ((pen ^ 1) << 4) | (((attr >> 6) & 1) << 3) | (paper ^ ((ink ^ paper) & -pen));
						int urgb = upal[sidx] & 0x1ff;
						ptr[h] = nxv_col[(((urgb >> 1) == tr) || clip) ? fb9 : urgb];
					}
					if (one) ptr[1] = ptr[0];
				}
			}
			if (!pent && (rx < rxe)) {
				nxv_span_draw(comp, t, L, &ln, rx, rxe, D, ptr);
				D += rxe - rx;
				rx = rxe;
			}
			// the Pentagon takes its border colour again on every CLK_14 edge (nxv_draw):
			// the border dot by dot, the paper between as a run
			while (rx < rxe) {
				if (nxv_bad_at(t, L, &ln, rx)) {
					V.cur = D;
					V.rx = rx;
					nxv_draw(comp, t, L, F, vact, &ln);
					rx++;
					D++;
					ptr += 2;
					continue;
				}
				int r2 = rx + 1;
				while ((r2 < rxe) && !nxv_bad_at(t, L, &ln, r2)) r2++;
				nxv_span_draw(comp, t, L, &ln, rx, r2, D, ptr);
				D += r2 - rx;
				ptr += 2 * (r2 - rx);
				rx = r2;
			}
			// the run's dots past the drawn part are passed over
			if (rx < rxa) {
				D += rxa - rx;
				rx = rxa;
			}
			V.cur = D;
			V.rx = rx;
		}
		if (V.rx >= L) {
			V.rx = 0;
			if (++V.ry >= F) V.ry = 0;
		}
	}
}

// the state taken up afresh at dot d (raw h, v): queued changes in effect, the
// fetch run over the dots before so its registers hold what they would
static void nxv_resync(Computer* comp, long long d, int h, int v) {
	nxv_apply(comp, LLONG_MAX);
	memset(&V.fe, 0, sizeof(V.fe));
	for (int k = 24; k > 0; k--) {
		int hh = h, vv = v, hc, vc;
		nxv_step(comp, &hh, &vv, -k);
		nx_ula_seen(comp, hh, vv, &hc, &vc);	// d may be behind the ray (a reset's dot)
		nxv_fetch(comp, &V.fe, d - k, hc, vc, 0);
	}
	V.cur = d - NXV_LAG + 1;
	V.rx = h;
	V.ry = v;
	nxv_step(comp, &V.rx, &V.ry, -NXV_LAG + 1);
	V.live = 1;
	V.upAt = -1;
}

// the copper stopped: nx_cop_run's own shortcut, its edges counted up to the end of dot d
static inline void nxv_cop_idle(Computer* comp, long long d) {
	if (NX.copT < d * 4 + 4) {
		NX.copT = d * 4 + 4;
		NX.copDout = 0;
	}
}

// == still frames ==

// flash_cnt counted at the (0, 0) dots the ray has passed undrawn (they count at the
// end of the dot; the one under the ray has not ended)
// ...before dot d: a still frame's, or an undrawn stretch's (no fetch counted them)
static void nxv_flash_to(Computer* comp, long long d) {
	if ((NX.still.flashDot < 0) || (NX.still.frameDots <= 0)) return;
	while (NX.still.flashDot + NX.still.frameDots < d) {
		NX.still.flashDot += NX.still.frameDots;
		V.flash = (V.flash + 1) & 31;
	}
	V.f[NXF_FLASH] = V.flash;		// undrawn: in effect at once
	V.dirty = 1;
}

void nxv_settle(Computer* comp) {
	if (!comp->vid->still) return;
	nxv_flash_to(comp, nxv_ray_dot(comp));
}

void nxv_touch(Computer* comp) {
	NX.still.dirty = 1;
	if (comp->vid->still && !NX.still.waking) vid_still_end(comp->vid);
}

// The still frame ends (vid_still_end): the dots counted pass undrawn, and the
// drawing is taken up at the dot under the ray as after any undrawn stretch. The
// buffers both hold the picture to here; what moved goes in after this, as queued
// changes and logged writes, all at or past the ray.
void nxv_wake(Video* vid) {
	Computer* comp = (Computer*)vid->xptr;
	if (!vid->still) return;
	if (!comp->hw || (comp->hw->id != HW_ZXNEXT)) {	// a machine switch: nothing to take up
		vid->still = 0;
		return;
	}
	NX.still.waking = 1;
	vid_unlazy(vid);
	NX.still.waking = 0;
	nxv_settle(comp);
	vid->still = 0;
	vid->nsCalmFixed = vid->nsPerDotFixed;	// the calm worked out for an undrawn frame
	if (vid->nodraw) return;		// taken up when the drawing is
	int h, v;
	nx_raw(comp, &h, &v);
	long long d = NX.frameBase + h;
	// flash_cnt counted at a dot the mixer draws again (from d - NXV_LAG + 1 on, as
	// nxv_resync sets it): its change goes in the queue
	long long fd = NX.still.flashDot;
	int late = (fd >= d - NXV_LAG + 1);
	if (late) V.f[NXF_FLASH] = (V.flash - 1) & 31;
	nxv_resync(comp, d, h, v);
	V.lastDot = d - 1;
	if (late) nxv_push(comp, fd * 2 + 1, NXF_FLASH, 0, V.flash);
}

// The frame starting goes still: the two before it were drawn and nothing the
// picture reads moved in them, nothing is queued, the copper is stopped, and the
// FLASH this frame counts does not turn over where it shows
static int nxv_still_ok(Computer* comp) {
	Video* vid = comp->vid;
	if (!vid_still_on || !vid->stillOk || vid->nodraw || vid->debug) return 0;
	if ((NX.still.clean < 2) || (NX.still.flashDot < 0) || NX.powerFrame) return 0;
	if ((!V.live && !vid->still) || (V.evHead != V.evN) || nx_cop_on(comp)) return 0;
	if (!((V.flash + 1) & 15) && !V.q[NXF_ULANEXT] && !V.q[NXF_ULAP]) return 0;
	return 1;
}

static void nxv_take_up(Computer* comp);

// The frame start (VID_NEXT's frm): the frame that ended counted clean or not, and
// the one starting made still or drawn
void nxv_frm(Video* vid) {
	Computer* comp = (Computer*)vid->xptr;
	if (vid->still) nxv_settle(comp);
	int moved = NX.still.dirty || vid->stillBreak || vid->nodraw || vid->debug;
	if (moved)
		NX.still.clean = 0;
	else if (NX.still.clean < 2)
		NX.still.clean++;
	NX.still.dirty = 0;
	vid->stillBreak = 0;
	if (NX.still.waking) return;		// ending it already
	int ok = nxv_still_ok(comp);
	if (ok && !vid->still) {
		// both buffers hold the picture that stands: what the mixer draws of it
		// (raw lines from vblank, dots from hblank), the rest of each left as it was
		const nxTiming* t = nx_tim(comp);
		int rows = nx_lines(comp) - t->vblank;
		int len = (t->L - t->hblank) * 8;
		for (int y = 0; y < rows; y++)
			memcpy(scrimg + y * bytesPerLine, bufimg + y * bytesPerLine, len);
		vid->still = 1;
		// the drawing stops at lastDot: whatever ends the still (nxv_wake, or a state or
		// snapshot loaded with still off) takes it up at the ray, not from there
		V.live = 0;
	} else if (!ok && vid->still) {
		vid_still_end(vid);
	}
	// a drawn frame after undrawn ones: the drawing taken up at its first dot
	if (!vid->nodraw && !vid->still && !V.live) nxv_take_up(comp);
}

// The fetch over the dots from lastDot + 1 to end - 1, the ray past them: their
// counters as it had them. What the fetch and the mixer read is kept with its time
// (the queued changes, the samples, the write log), so it reads what it read with the
// ray on the dot. The copper and the sprite pass are run up to there first.
static void nxv_fetch_to(Computer* comp, long long end) {
	long long d = V.lastDot + 1;
	if (d >= end) return;
	// a copper running may stop itself on the way (nx_cop_run then counts its edges as
	// the stopped one does); a stopped one is not started by anything here
	if (nxv_sweeping) {
		// nxv_sweep: the machine's copper and sprite pass are not run for it
	} else if (nx_cop_on(comp)) {
		nx_cop_run(comp, (end - 1) * 4 + 4);
		if (!V.live) return;		// it wrote a reset (NR 02): nothing more here
		// stopped for room at edge copT (nx_cop_hold): the dots it has ended go first
		long long e = NX.copT >> 2;
		if ((e < end) && (e > d)) end = e;
	} else {
		nxv_cop_idle(comp, end - 1);
	}
	if (!nxv_sweeping) nxs_run_to(comp, (end - 1) * 8 + 8);
	const nxTiming* t = nx_tim(comp);
	int L = t->L;
	int F = nx_lines(comp);
	int r0 = t->hact - 11;		// hc_ula 0, where vc_ula steps: nx_ula again there
	int h, v, hc, vc;
	nx_raw(comp, &h, &v);
	nxv_step(comp, &h, &v, (int)(d - (NX.frameBase + h)));
	nx_ula_seen(comp, h, v, &hc, &vc);
	for (; d < end; d++) {
		int idle = nxv_fetch_unseen(hc, vc, L);
		if (idle) {
			// a quiet stretch of it (up to where the counters are asked again) in one go,
			// but for its last 16 dots: any 16 hold a sample, so they leave the fetch's
			// registers as dot by dot does. Quiet for 32 dots before: both modes latched
			long long z = ((h < r0) ? r0 : L) - h;
			if (!nxv_vbord(vc) && (z > L - 16 - hc)) z = L - 16 - hc;
			if (z > end - d) z = end - d;
			if ((z > 32) && nxv_fetch_quiet(comp, (d - 32) * 4)) {
				int k = (int)z - 16;
				nxv_fetch_fill(comp, d, hc, k);
				d += k;
				h += k;
				hc += k;
			}
		}
		if ((hc & 1) || ((hc & 7) == 4)) {	// an even dot other than n = 4, C does nothing
			if (idle)
				nxv_fetch_idle(comp, &V.fe, d, hc, vc);
			else
				nxv_fetch(comp, &V.fe, d, hc, vc, 0);
		}
		if (!hc && !vc) {
			// flash_cnt counts at the end of (0, 0); pixel_en uses it from the next half-pixel on
			V.flash = (V.flash + 1) & 31;
			nxv_push(comp, d * 2 + 1, NXF_FLASH, 0, V.flash);
			NX.still.flashDot = d;
			NX.still.frameDots = (long long)L * F;
			if (!(V.flash & 15) && !V.q[NXF_ULANEXT] && !V.q[NXF_ULAP])
				NX.still.dirty = 1;		// FLASH turned over
		}
		if (++h == L) {
			h = 0;
			if (++v == F) v = 0;
			nx_ula_seen(comp, h, v, &hc, &vc);
		} else if (h == r0) {
			nx_ula_seen(comp, h, v, &hc, &vc);
		} else {
			hc++;
		}
	}
	V.lastDot = end - 1;
}

// the drawing taken up at the dot a reset left it at, or else at the dot under the
// ray, as after any undrawn stretch
static void nxv_take_up(Computer* comp) {
	int h, v;
	nx_raw(comp, &h, &v);
	long long d = NX.frameBase + h;
	if ((V.upAt >= 0) && (V.upAt < d)) {
		nxv_step(comp, &h, &v, (int)(V.upAt - d));
		d = V.upAt;
	}
	// flash_cnt counted at the (0, 0) dots the undrawn stretch passed; one the mixer
	// draws again (from d - NXV_LAG + 1 on) goes in the queue, as in nxv_wake
	nxv_flash_to(comp, d);
	long long fd = NX.still.flashDot;
	int late = (fd >= d - NXV_LAG + 1);
	if (late) V.f[NXF_FLASH] = (V.flash - 1) & 31;
	nxv_resync(comp, d, h, v);
	V.lastDot = d - 1;
	if (late) nxv_push(comp, fd * 2 + 1, NXF_FLASH, 0, V.flash);
}

// The picture up to dot end (the first one the ray has not ended): the fetch up to
// there, the mixer NXV_LAG dots behind it, as a change queued at the ray may still
// show a dot or so back
static void nxv_flush(Computer* comp, long long end) {
	Video* vid = comp->vid;
	if (vid->still) return;
	if (vid->nodraw) {
		V.live = 0;		// taken up afresh when the drawing is (nxv_frm)
		V.upAt = -1;
		return;
	}
	if (!V.live) nxv_take_up(comp);
	// from inside one (a reset the copper writes, nxv_before_reset): this one draws only
	// up to where the copper stands, the outer one goes on
	int outer = nxv_flushing;
	nxv_flushing = 1;
	for (;;) {
		// the fetch stops short where the copper was stopped for room (nxv_room): drawn
		// to there, then on
		do {
			nxv_fetch_to(comp, end);
			if (!V.live) break;		// a reset the copper wrote (NR 02)
			nxv_render(comp, V.lastDot - NXV_LAG + 1);
		} while (V.lastDot + 1 < end);
		if (V.live || outer || (V.upAt < 0)) break;
		nxv_take_up(comp);			// ...on from the reset's dot
	}
	nxv_flushing = outer;
	if (!outer) nx_cop_hold = 0;
}

void nxv_line(Computer* comp) {
	nxv_flush(comp, NX.frameBase);
}

// Fast mode's "memory dump" picture (vid_sweep): a whole frame from the memory and the
// registers as they stand, drawn at once. It is drawn as the frame two ahead, past
// every write, so each read sees the memory as it is now; no copper or sprite pass is
// run for it, and the sprites are left out. All the drawing's own state is put back.
void nxv_sweep(Video* vid) {
	Computer* comp = (Computer*)vid->xptr;
	static nxVideo keep;
	keep = V;
	__typeof__(NX.still) still = NX.still;
	unsigned nodraw = vid->nodraw;
	unsigned stillOn = vid->still;
	vid->nodraw = 0;
	vid->still = 0;
	nxv_sweeping = 1;
	int h, v;
	nx_raw(comp, &h, &v);
	long long frame = (long long)nx_tim(comp)->L * nx_lines(comp);
	long long d0 = NX.frameBase - (long long)v * nx_tim(comp)->L + 2 * frame;	// raw (0, 0) of it
	nxv_resync(comp, d0, 0, 0);
	V.lastDot = d0 - 1;
	V.f[NXF_SPREN] = 0;
	V.dirty = 1;
	// a line at a time, as nxv_line draws: the fetch's ring holds a line or so
	int L = nx_tim(comp)->L;
	for (long long e = d0 + L; e <= d0 + frame; e += L) {
		nxv_fetch_to(comp, (e < d0 + frame) ? e : e + NXV_LAG);
		nxv_render(comp, (e < d0 + frame) ? V.lastDot - NXV_LAG + 1 : e);
	}
	nxv_sweeping = 0;
	vid->nodraw = nodraw;
	vid->still = stillOn;
	NX.still = still;
	V = keep;
}

// A soft reset written at CLK_28 w (-1: the button, at the ray), before the registers
// go: the picture up to it as they stood, and the dot to take the drawing up at after it
// (-1: at the ray when it is drawn again). A copper writing it stands at w, inside the
// drawing (nxv_fetch_to) or not, so drawing up to w runs it no further.
long long nxv_before_reset(Computer* comp, long long w) {
	// a still frame ends first (the write's own nxv_touch comes after the reset): the
	// drawing taken up at the ray, and on as for any reset
	vid_still_end(comp->vid);
	if (comp->vid->nodraw || comp->vid->still) return -1;
	if (!V.live) return V.upAt;		// one not taken up yet (in this line): from its dot
	long long d = nxv_ray_dot(comp);
	if ((w >= 0) && ((w >> 2) < d)) d = w >> 2;
	nxv_flush(comp, d);
	// the mixer one dot on, to where the take-up at d starts it (nxv_resync)
	nxv_render(comp, d - NXV_LAG + 1);
	return d;
}

// ...and after it (nxv_reset done): taken up at that dot at once, so what lands next
// goes in the queue at its own time. From inside the drawing the outer nxv_flush does it.
void nxv_after_reset(Computer* comp, long long up) {
	V.upAt = up;
	if ((up >= 0) && !nxv_flushing && !comp->vid->nodraw && !comp->vid->still) nxv_take_up(comp);
}

// the raw counters of the dot under the ray
static inline void nxv_ray_hv(Computer* comp, Video* vid, int* h, int* v) {
	const nxTiming* t = nx_tim(comp);
	int x = vid->ray.x;
	int y;
	if (x >= vid->vend.x) {
		*h = x - vid->vend.x;
		y = vid->ray.y + t->vblank + 1;
	} else {
		*h = x + t->hblank;
		y = vid->ray.y + t->vblank;
	}
	while (y >= vid->full.y) y -= vid->full.y;
	*v = y;
}

// The dot callback (VID_NEXT): the end of the dot under the ray. The lines draw
// themselves as the ray leaves them (nxv_line); the debugger, stepping, wants the
// picture up to the ray at every step
void nxv_dot(Video* vid) {
	if (!vid->debug) return;
	Computer* comp = (Computer*)vid->xptr;
	int h, v;
	nxv_ray_hv(comp, vid, &h, &v);
	nxv_flush(comp, NX.frameBase + h + 1);
}

// a run of dots: nothing to do but count them, but under the debugger (a dot at a time)
int nxv_run(Video* vid, int k) {
	return vid->debug ? 0 : k;
}

// Where the picture stops in the buffer: the mixer's next dot, placed as nxv_render
// places it (raw line V.ry, dot V.rx at column V.rx - 1 - hblank), drawn up to the ray
unsigned char* nxv_drawn(Video* vid) {
	Computer* comp = (Computer*)vid->xptr;
	nxv_flush(comp, nxv_ray_dot(comp));
	if (!V.live) return vid->ray.ptr;
	const nxTiming* t = nx_tim(comp);
	int row = V.ry - t->vblank;
	int col = V.rx - 1 - t->hblank;
	if (row < 0) return scrimg;		// still in the blanking: the whole picture to come
	if (col < 0) col = 0;
	return scrimg + row * bytesPerLine + col * 8;
}

// The raster changes at raw (0, 0), the absolute dot d0: first draw the old one up
// to there, then take the drawing up at raw (0, 0) of the new one
void nxv_retime(Computer* comp, long long d0, int after) {
	nxv_touch(comp);
	if (!V.live) return;
	if (!after) {
		long long end = (V.lastDot + 1 < d0) ? V.lastDot + 1 : d0;
		nxv_render(comp, end);
		return;
	}
	if (V.cur < d0) V.cur = d0;
	V.rx = (int)(V.cur - d0);
	V.ry = 0;
	nxv_step(comp, &V.rx, &V.ry, 0);
}

// at a reset: what the picture reads, as the registers hold it now
void nxv_reset(Computer* comp) {
	nxv_col_init();
	nxv_fields(comp, V.q);
	memcpy(V.f, V.q, sizeof(V.f));
	memcpy(V.pal, NX.pal, sizeof(V.pal));
	V.evHead = V.evN = 0;
	memset(V.wr, 0, sizeof(V.wr));
	V.wrHead = 0;
	memset(V.wrT, 0, sizeof(V.wrT));
	memset(V.wrH, 0, sizeof(V.wrH));
	memset(V.sprKeep, 0, sizeof(V.sprKeep));
	V.sprKeepTo[0] = V.sprKeepTo[1] = -1;
	V.sprKeepNew = 0;
	V.upAt = -1;
	nxv_hset(&V.hBrd, NX.pfe & 7);
	nxv_hset(&V.hPff, NX.pff & 0x3f);
	nxv_hset(&V.hShadow, (NX.p7ffd >> 3) & 1);
	nxv_hset(&V.hSx, NX.nr[0x26]);
	nxv_hset(&V.hSy, NX.nr[0x27]);
	nxv_hset(&V.hFine, (NX.nr[0x68] >> 2) & 1);
	nxv_hset(&V.hTmSx, nxv_tm_sx(comp));
	nxv_hset(&V.hTmSy, NX.nr[0x31]);
	nxv_hset(&V.hTmMap, NX.nr[0x6e]);
	nxv_hset(&V.hTmTile, NX.nr[0x6f]);
	nxv_hset(&V.hTmCtl, NX.nr[0x6b] & 0x7f);
	nxv_hset(&V.hTmAttr, NX.nr[0x6c]);
	V.tmKey = -1;
	V.f[NXF_FLASH] = V.flash;
	V.dirty = 1;
	V.live = 0;
	comp->vid->still = 0;
	NX.still.flashDot = -1;
	NX.still.frameDots = 0;
	NX.still.clean = 0;
	NX.still.dirty = 1;
	NX.still.waking = 0;
}

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

#include "vidcommon.h"
#include "../defines.h"

typedef struct Video Video;

#include <stdlib.h>
#include <stdint.h>

#include "ulaplus.h"

#define vid_irq(_v, _n) _v->xirq(_n, _v->xptr)
// FLASH as a picture drawn shows it. Fast loading and Fast mode show a frame now and then in
// host time, and the machine's own phase would land anywhere each time: a strobe. They give
// the picture one of their own (flashShow); the machine's FLASH is not touched.
#define vid_flash_shown(_v) (((_v)->flashShow < 0) ? (_v)->flash : (_v)->flashShow)

// How much of the border is shown. The frame is the same size on every ZX
// machine, with the screen in the middle of it; see vid_upd_crop.
enum {
	VID_BRD_NONE = 0,	// 256x192
	VID_BRD_TINY,		// 272x208
	VID_BRD_SMALL,		// 288x224
	VID_BRD_MEDIUM,		// 320x240
	VID_BRD_FULL,		// 352x288
	VID_BRD_OVERSCAN	// as much as the machine's raster holds
};

// screen mode
enum {
	VID_UNKNOWN = -1,
// spectrum
	VID_NORMAL = 0,
	VID_ULA_SCR,
	VID_ALCO,
	VID_ATM_EGA,
	VID_ATM_TEXT,
	VID_ATM_HWM,
	VID_HWMC,
	VID_EVO_TEXT,
	VID_TSL_16,	// TSConf 4bpp
	VID_TSL_256,	// TSConf 8bpp
	VID_TSL_NORMAL,	// TSConf common screen
	VID_TSL_TEXT,
	VID_PRF_MC,	// Profi 512x240, v5: in color
	VID_PRF_MONO,	// Profi 512x240, v3: in the border colors
	VID_NEXT	// ZX Spectrum Next: the core's own ULA, layers and mixer (hardware/zxnext_vid.c)
};

extern int bufSize;
extern int bufSwaps;		// pictures finished into bufimg so far
extern int bytesPerLine;
extern int greyScale;
//extern int scanlines;
extern int noflic;
extern int noflicMode;
extern float noflicGamma;
extern int noflicAhead;		// the adaptive antiflicker looks at frames run ahead

extern unsigned char* scrimg;
extern unsigned char* bufimg;
// which buffer is which, for a caller that has to undo the swaps of a run (xreplay)
typedef struct {
	unsigned char* scr;
	unsigned char* buf;
	int cur;
	int swaps;
} vBufState;
void vid_buf_get(vBufState*);
void vid_buf_set(const vBufState*);
extern unsigned char pscr[];

void vid_dot_full(Video*, unsigned char);
void vid_dot_half(Video*, unsigned char);

//typedef int(*vcbmrd)(int, void*);
//typedef void(*vcbmwr)(int, int, void*);
typedef void(*cbvid)(Video*);

typedef struct {
	int id;
	cbvid init;
	cbvid dot;		// each dot
	cbvid hbl;		// hblank start
	cbvid line;		// visible line start
	cbvid vbl;		// @vblank (right after last line)
	cbvid frm;		// @1st visible line (called before cbLine)
	// A stretch of dots at once, where the mode can do better than one call
	// each (a border line is one colour). Takes the dots it wants and returns
	// how many it drew; 0 leaves the whole run to the dot callback.
	int (*run)(Video*, int);
	// The dots leave nothing the machine can see but the border latch and what
	// latch says, so an undrawn frame skips them (vid_skips()) - in every mode.
	int latch;		// VL_*: a byte of the drawing the machine reads back
	// where the picture really stops, for a mode that draws through a pointer of its
	// own (VID_NEXT, behind the ray); NULL: at ray.ptr
	unsigned char* (*drawn)(Video*);
} xVideoMode;

// What a mode's dots leave behind that the machine can read: worked out from
// the memory where the dots are skipped (vid_atrbyte, vid_fntbyte)
enum {
	VL_NONE = 0,
	VL_ATR_ZX,		// atrbyte: the ZX attribute of the cell, 0xff on the border
	VL_FNT_ATM,		// fntbyte: the font byte of ATM text
	VL_FNT_EVO		// fntbyte: ...of ZX Evo text
};

struct Video {
	unsigned nogfx:1;	// tsl : nogfx flag, pc98xx:disable display
	unsigned newFrame:1;	// set @ start of VBlank
	int intFRAME;		// aka INT
	int intlen;		// dots the pulse in intFRAME started with; keeps its
				// value while intFRAME is 0 - a snapshot saves it
	unsigned intLINE:1;	// for TSConf
	unsigned intDMA:1;	// for TSConf
//	unsigned noScreen:1;
	unsigned nodraw:1;	// emulate the raster but put no pixels out; set at a frame start,
				// since a blind mode's fetch counter stands still meanwhile
	unsigned debug:1;
	unsigned upd:1;
	unsigned tail:1;
	unsigned linedbl:1;	// lines doubler

	unsigned hblank:1;	// HBlank signal
	unsigned vblank:1;	// VBlank signal

	unsigned hbrd:1;	// border.H
	unsigned vbrd:1;	// border.V

	int nodrawNext;		// nodraw for the next frame, taken as it starts; -1: keep
	int latchFrm;		// where atrbyte/fntbyte were last taken in an undrawn frame:
	int latchPos;		// fcnt and the dot (vid_latch)

	int nsPerFrame;
	long long nsPerDotFixed;	// the dot period, 16.16, and what the ray steps by
	double nsPerDotExact;	// same value as a double, for vid_upd_layout() to
				// re-derive nsPerFrame from
	long long nsDrawFixed;	// time handed in but not yet drawn, 16.16
	long long nsOwedFixed;	// fraction of a ns owed to vid->time, carried not dropped
	long long nsCalmFixed;	// nsDrawFixed may grow up to this with no dot drawn (vid_sync_lazy)
	int lazyDraw;		// dots a drawn frame may count unwalked too: a mode whose drawer keeps
				// what it reads with its time (VID_NEXT); 0 none
	long long clkFixed;	// time handed in short of a whole dot (vid_clock)
	long long stepFixed;	// time handed in since the step began, made into ns at its end
	int tickWhole;		// every time handed in is a whole number of dots (comp_update_timings)
	int time;		// whole ns of the dots the last step completed
	int dotPerFrame;

	int flash;
	int flashShow;		// -1: a picture drawn shows the FLASH above; 0/1: the one whoever shows the frames gives it
	int vidPage;

	int brdstep;
	int brdmode;		// VID_BRD_* : how much border to show
	int brdmin;		// VID_BRD_* : the least the machine's own picture needs
	unsigned char brdcol;
	unsigned char nextbrd;

	unsigned char inten;	// interrupts enable (8 bits = 8 signals)
	uint32_t pal[256];	// ABGR inside int, R = LSB, A = FF
	uint32_t gpal[256];	// greyscale copy of pal
	uint32_t bpal[256];	// base palette (loaded preset)

	int vmode;
	xVideoMode* cb;

	cbxrd mrd;		// external memory reading
	cbirq xirq;		// interrupt
	void* xptr;

	int fcnt;
	unsigned char atrbyte;
	unsigned char fntbyte;			// font byte the text mode is showing: ZX Evo reads it back
	int snowLow;				// ULA snow: address bits 6-0 the refresh cycle forced on this burst (-1 = none)
	int snowBank;				// ULA snow: the bank that burst reads instead of the screen one
	unsigned snowDup:1;			// ULA snow: this burst is lost, the ULA shows the previous one again
	vRay ray;
	vCoord full;
	vCoord blank;
	vCoord bord;
	vCoord scrn;
	vCoord send;
	vCoord vend;
	// The whole raster is drawn into the buffer; these three are the part of
	// it that is shown, not a limit on what is drawn.
	vCoord lcut;		// top left corner of the shown frame
	vCoord rcut;		// bottom right corner (exclusive)
	vCoord vsze;		// shown frame size
	vCoord intp;		// intp.y = the line TSConf raises its INT on
	int intsize;
	vCoord res;		// current resolution (-1 = from layout)

	int idx;

	vCoord scrsize;		// << tsconf.xSize, tsconf.ySize
	// what tslUpdatePorts() last derived from; -1 forces the next call to
	// recompute. vid_upd_layout() and whatever changes vmode/nogfx behind
	// tslUpdatePorts' back (reset, snapshot loaders) reset it. xstate copies
	// the whole Video, so a rollback keeps the key and the ports in step.
	int tslKey;		// p00af | p07af<<8 | tconfig<<16

	struct {
		int xPos;			// position of screen @ monitor [32|12] x [44|24|0]
		int yPos;
		int tsXPos;			// the tiles/sprites window, the same unless TSConfig b0 widens it
		int tsYPos;
		vCoord tsSize;
		unsigned char tconfig;		// port 06AF
		unsigned char TMPage;		// tiles map page
		unsigned char T0GPage;		// lay 0 graphics
		unsigned char T1GPage;		// lay 1 graphics
		unsigned char SGPage;		// sprites graphics
		unsigned char T0Pal76;		// b7.6 of tiles palete (07AF)
		unsigned char T1Pal76;
		unsigned char scrPal;		// b7..4: bitmap palete
		int hsint;			// tsconf INT x pos = p22AF << 1
		unsigned char p00af;
		unsigned char p07af;
		ePair(xOffset,soxh,soxl);	// offsets of screen corner
		ePair(yOffset,soyh,soyl);
		ePair(T0XOffset,t0xh,t0xl);	// tile 0 offsets
		ePair(T0YOffset,t0yh,t0yl);
		ePair(T1XOffset,t1xh,t1xl);	// tile 1 offsets
		ePair(T1YOffset,t1yh,t1yl);
		ePair(scrLine, loffh, loffl);
		ePair(intLine, ilinh, ilinl);	// INT line
		unsigned char tsNext[0x200];	// tiles and sprites of the next line, drawn during this one
		unsigned short tmb[2][4][64];	// tile map rows read ahead, per layer (video_ts.v tmbuf)
		int dramLoad;			// DRAM cycles of this line the video and the tiles/sprites take, of 448
		unsigned char intInc;		// VSINTH b7..4: lines the INT line moves on after each frame INT
		struct {			// written by the cpu, taken at the start of the blanking (video_ports.v)
			unsigned char vconf, vpage, gxl, gxh, palsel, t0xl, t0xh, t1xl, t1xh, t0g, t1g, gyl, gyh;
			unsigned short mask;	// TSL_LAT_* of the fields waiting
		} lat;
		unsigned char hide;		// TSL_HIDE_*: layers the debugger keeps out of the picture
		// tsNext and line layer by layer, S0 T0 S1 T1 S2: the two are put together
		// from these, so a layer hidden after they were drawn can still be left out.
		// Two sets that change places every line; lay[layNext] is the next line's.
		// Only while a layer is hidden or a redraw may come (tslLayersApart): else
		// the TSU draws straight into tsNext and the set is marked TSL_LAY_WHOLE
		unsigned char lay[2][5][0x200];
		unsigned char layUsed[2];	// TSL_HIDE_* bits of the layers drawn into each set, TSL_LAY_WHOLE
		unsigned char layNext;
		unsigned char cram[0x200];	// pal = colram?
		unsigned char sfile[0x200];	// sprites = ram?
//		int dmabytes;
	} tsconf;
	unsigned char line[0x500];		// buffer for render sprites & tiles
	unsigned char linb[0x500];		// buffer for rendered bitplane
	// TODO: allocate font only if it loaded
	struct {
		unsigned char* data;		// ATM text mode font
		int size;			// TODO:use it for uploadable font
	} font;

	ulaPlus* ula;
};

Video* vidCreate(cbxrd, cbirq, void*);
void vidDestroy(Video*);

void vid_reset(Video*);
// The ns the devices are given come from the dots the time handed in completes,
// not from the dots walked: a frame not drawn walks them much later
// (vid_sync_lazy). The time a step hands in only adds up (stepFixed) and is made
// into ns once, at the step's end: the video stands at the step's end then and
// has never been past it, so the dots it completes are the same whichever way
// it was handed in.
// The whole ns back.
int vid_clock_slow(Video*, long long);
static inline int vid_clock(Video* vid, long long nsFixed) {
	long long p = vid->nsPerDotFixed;
	long long c = vid->clkFixed;
	if ((nsFixed < 0) || (c < 0) || (c >= p))
		return vid_clock_slow(vid, nsFixed);
	if (vid->tickWhole) {
		vid->nsOwedFixed += nsFixed;	// all of it whole dots, the fraction left as it was
	} else if (nsFixed < (p << 3)) {	// a few dots: counted off (a tick of half a dot, 14 MHz)
		long long f = c + nsFixed;
		while (f >= p) {
			f -= p;
			vid->nsOwedFixed += p;
		}
		vid->clkFixed = f;
	} else {
		return vid_clock_slow(vid, nsFixed);
	}
	long long whole = FIXED_TO_NS(vid->nsOwedFixed);
	vid->nsOwedFixed -= NS_TO_FIXED(whole);
	return (int)whole;
}
// Steps of nsFixed each, the step's ns of each worked out as vid_step_ns() would
// (vid->stepFixed with the first): the first one whose ns take the sum past room,
// max if none does, -1 when the clock is not in a state to say. Nothing moves.
static inline int vid_clock_count(const Video* vid, long long nsFixed, long long room, int max) {
	long long p = vid->nsPerDotFixed;
	long long c = vid->clkFixed;
	long long owed = vid->nsOwedFixed;
	long long add = vid->stepFixed + nsFixed;
	long long sum = 0;
	if ((nsFixed < 0) || (add < 0) || (c < 0) || (c >= p)) return -1;
	for (int k = 1; k <= max; k++) {
		if (vid->tickWhole) {
			owed += add;
		} else {
			long long f = c + add;
			long long d = f / p;
			owed += d * p;
			c = f - d * p;
		}
		long long whole = FIXED_TO_NS(owed);
		owed -= NS_TO_FIXED(whole);
		sum += NS_TO_FIXED(whole);
		if (sum > room) return k;
		add = nsFixed;
	}
	return max;
}
// the step's ns, and the next step starts from nothing; vid->time keeps them for
// the snapshot, which has always had the field
static inline int vid_step_ns(Video* vid) {
	int ns = vid_clock(vid, vid->stepFixed);
	vid->stepFixed = 0;
	vid->time = ns;
	return ns;
}
void vid_sync_fixed(Video*,long long);
void vid_sync_lazy_slow(Video*,long long);
void vid_unlazy_slow(Video*);
void vid_settle_slow(Video*);
// The dots counted by vid_sync_lazy() are drawn, and it counts no more until the
// next vid_sync_lazy_slow() has looked at the video again - for whatever runs
// next may change it.
static inline void vid_unlazy(Video* vid) {
	if (vid->nsCalmFixed > vid->nsPerDotFixed)
		vid_unlazy_slow(vid);
}
// The ray as vid_sync_fixed() would move it, but the dots of an undrawn blind
// frame that pass no event are only counted, and walked when something looks:
// vid_settle() or the next vid_sync_fixed(). A reader of the ray inside an
// instruction settles AFTER walking the video up to the cpu's t - on a
// contended machine the time handed in may lag it (see res4 in spectrum.h).
// Only for the cpu's own bus cycles - the code that runs between two of them
// must not change the video.
static inline void vid_sync_lazy(Video* vid, long long nsFixed) {
	long long d = vid->nsDrawFixed + nsFixed;
	if ((nsFixed >= 0) && (d < vid->nsCalmFixed)) {
		vid->nsDrawFixed = d;
		vid->stepFixed += nsFixed;
		return;
	}
	vid_sync_lazy_slow(vid, nsFixed);
}
// The whole dots vid_sync_lazy() has counted and not walked. They never reach
// past the next event, and so not past the line end either: the ray stands at
// ray.x plus these on its line, which is all a reader of the bus timing needs to
// know - without the walk. The count stays under a line, so it fits 32 bits.
static inline int vid_lazy_dots(Video* vid) {
	long long f = vid->nsDrawFixed;
	if (f < vid->nsPerDotFixed) return 0;
	if (f < 0x7fffffffLL) return (int)((unsigned)f / (unsigned)vid->nsPerDotFixed);
	return (int)(f / vid->nsPerDotFixed);
}
// only dots vid_sync_lazy() counted: after a change of the dot period the
// time left over can be a dot or more, and that waits for the next sync as ever
static inline void vid_settle(Video* vid) {
	if ((vid->nsDrawFixed >= vid->nsPerDotFixed) && (vid->nsCalmFixed > vid->nsPerDotFixed))
		vid_settle_slow(vid);
}
// Whole dots in f at period p (both 16.16 ns), the remainder left in *rem.
// A span is nearly always a few dots, so they are counted off; the 64-bit
// division only runs for a long stretch, where it is paid once per many dots.
// Exact only for f >= 0.
static inline int vid_count_dots(long long f, long long p, long long* rem) {
	int n = 0;
	if (f < (p << 4)) {
		while (f >= p) {
			f -= p;
			n++;
		}
	} else {
		n = (int)(f / p);
		f -= (long long)n * p;
	}
	*rem = f;
	return n;
}
// sets the dot period alone. vid_upd_timings() is the complete one - it also
// re-derives nsPerFrame, which the frame timer in emulwin.cpp reads
void vid_set_dot_ns(Video*,double);
// void vid_irq(Video*, int);
void vid_set_mode(Video*,int);
void vid_set_nodraw(Video*,int);
void vid_set_nodraw_next(Video*,int);		// the same from the next frame start on
void vid_sweep(Video*);				// the memory now as a whole frame, into bufimg
void vid_set_int_frame(Video*, int);		// dots left of the frame INT pulse, 0: none
void vid_set_ray(Video*, int);
int vid_ray_dots(Video*);

int vid_wait_dots(Video*, int, int);		// contention wait in dots, not ns
// Dots from the ray to the first one the machine would see something on: the
// frame end, the INT pulse starting or ending, and with lineInt the next hblank
// (TSConf's line INT). Walking fewer dots than this changes nothing but the
// picture. For the ray where it stands: settled first.
long long vid_quiet_dots(Video*, int lineInt);
// An internal cycle only parks an address on the bus, no MREQ; the Ferranti
// ULA contends it all the same, the +2A/+3 asic leaves it alone. The one
// place that rule lives.
static inline int vid_cont_internal(Video* vid, int mreq) {
	return mreq || (vid->ula->conttype == CONT_PATA);
}
// Can this physical address be contended at all - the pattern/address test of
// vid_wait_dots() alone, with the ray left untouched. A miss here means
// vid_wait_dots() would return 0 without looking at the ray, so the caller may
// skip the video sync the ray read would have needed.
// The address bit of a contended ram page for the pattern, 0 for none - what memory's
// cont map is made of (comp_cont_map)
static inline int vid_cont_bit(Video* vid) {
	switch (vid->ula->conttype) {
		case CONT_PATA: return 0x4000;		// pages 1,3,5,7
		case CONT_PATB: return 0x10000;		// pages 4,5,6,7
	}
	return 0;
}
static inline int vid_cont_hit(Video* vid, int adr, int mreq) {
	return vid_cont_internal(vid, mreq) && (adr & vid_cont_bit(vid));
}
int vid_snow(Video*, int, int);			// cpu refresh cycle: disturb the ULA if it is fetching now
int vid_float_bus(Video*, int*);		// the byte the ULA has on the bus now, -1 if none
int vid_atrbyte(Video*);			// the attribute the ULA last fetched, 0xff on the border
int vid_fntbyte(Video*);			// the font byte text mode last fetched
// In an undrawn frame the bytes the dots would have fetched are worked out when
// read, from the memory as it is then. Anything about to change that memory, the
// page or the mode first has them taken here, as the dots would have had them.
void vid_latch(Video*);
// ...and whether a write to this ram address (bank << 14 | offset) is one of those
int vid_latch_hit(Video*, int);
void vid_dark_tail(Video*);

void vid_clear_image(void);
void vid_set_layout(Video*, vLayout*);
void vid_set_resolution(Video*, int, int);
void vid_set_border(Video*, int);
void vid_upd_crop(Video*);
void vid_widen_crop(Video*, int, int);
vCoord vid_crop_size(Video*, int);
void vid_upd_layout(Video*);
void vid_upd_timings(Video*, double);

// A screen the debugger's view wants, copied at the frame boundary. It decodes
// in the gui thread, which reaches the memory whenever it is scheduled - by
// then the machine has run on and is usually halfway through rewriting the
// screen, which is what made moving sprites tear.

#define VSCR_SLOTS	2		// the Both view is the most that is ever shown
#define VSCR_BYTES	0x1b00		// pixels and attributes of one screen
#define VSCR_ONAIR	-2		// whichever page the machine is showing

void vid_scr_want(int slot, int page, int shift);
void vid_scr_snap(Video*);
const unsigned char* vid_scr_snap_get(int slot, int page, int shift);
int vid_scr_snap_page(int slot);

// what vid_get_screen() leaves out

#define VSCR_MONO	1	// white on black, no attributes and no palette
#define VSCR_NOPIX	2	// a dot in place of every pixel byte
#define VSCR_GRID	4	// dim every other 8x8 cell
#define VSCR_NOFLASH	8	// hold the flash attribute still

void vid_get_screen(Video*, unsigned char*, int, int, int, const unsigned char*);
xColor vid_brd_col(Video*);
int vid_scr_base(int);
void vid_scr_adr(int, int, int, int*, int*);
// pixel and attribute address of the character box vid->idx counts (y * 32 + x)
static inline int zx_box_pix_adr(int idx) {
	return (idx & 0x181f) | ((idx & 0x700) >> 3) | ((idx & 0xe0) << 3);
}
static inline int zx_box_atr_adr(int idx) {
	return 0x1800 | ((idx & 0x1f00) >> 3) | (idx & 0x1f);
}
// the same pixel address from screen coordinates
static inline int zx_pix_adr_xy(int x, int y) {
	return ((y & 0xc0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2) | ((x & 0xf8) >> 3);
}
// ...and the attribute address of the cell those coordinates land in
static inline int zx_atr_adr_xy(int x, int y) {
	return 0x1800 | ((y & 0xf8) << 2) | ((x & 0xf8) >> 3);
}
int vid_scr_dot(int, int*, int*);
int vid_scr_bit(int);

void vid_set_grey(int);
xColor vid_get_col(Video*, int);
void vid_set_col(Video*, int, xColor);
int vid_zx_palette(Video*);
void vid_set_bcol(Video*, int, xColor);
void vid_reset_col(Video*, int);

void vid_fnt_wr(Video*, int, int);
int vid_fnt_rd(Video*, int);
void vid_fnt_load(Video*, const char*);
void vid_fnt_del(Video*);

void tslUpdatePorts(Video*);
// put this line's and the next one's tiles and sprites together again, after hide changed
void tsl_recompose(Video*);
// the TSU draws its layers apart even with none hidden, for a redraw (xreplay_arm)
extern int tslLayersApart;

// TSConf sprites and tiles for the debugger, decoded the way the TSU does it
#define TSL_SPRITES	85		// SFILE entries
typedef struct {
	int x, y, w, h;			// position in the tile/sprite window, size in dots
	int tile, pal;
	int act, leap, xf, yf;
} tsSprite;
void tsl_get_sprite(Video*, int idx, tsSprite*);
// for each entry the sprite layer it is drawn in, 0..2 for S0..S2, -1 after the third LEAP
void tsl_sprite_layers(Video*, signed char* layer);
// w*h dots as the TSU puts them out: pal << 4 | dot, 0 where transparent
void tsl_sprite_dots(Video*, int idx, unsigned char* out);
// 8x8 dots of a tile of a graphics page, palhi the color's high nibble
void tsl_tile_dots(Video*, int page, int tile, int palhi, unsigned char* out);
// where line `line` of tile `tile` of a graphics page starts
int tsl_gfx_adr(int page, int tile, int line);
// the tile map entry of layer 0/1 at row, col (0..63), from TMPage
int tsl_map_entry(Video*, int lay, int row, int col);
typedef struct {
	int adr;			// the entry's byte address
	int entry;
	int pal;			// the color's high nibble the cell's dots get
	int hole;			// tile 0 with the layer's Z_EN off: not drawn
} tsMapCell;
void tsl_map_cell(Video*, int lay, int row, int col, tsMapCell*);
// a map cell's 8x8 dots as the layer draws it: page, palette, flips, tile 0 as a hole
void tsl_map_dots(Video*, int lay, int entry, unsigned char* out);

// TSConf registers the controller takes at the start of a line, not at once
enum {
	TSL_LAT_VCONF = 1 << 0,
	TSL_LAT_VPAGE = 1 << 1,
	TSL_LAT_GXL = 1 << 2,
	TSL_LAT_GXH = 1 << 3,
	TSL_LAT_PAL = 1 << 4,
	TSL_LAT_T0XL = 1 << 5,
	TSL_LAT_T0XH = 1 << 6,
	TSL_LAT_T1XL = 1 << 7,
	TSL_LAT_T1XH = 1 << 8,
	TSL_LAT_T0G = 1 << 9,
	TSL_LAT_T1G = 1 << 10,
	TSL_LAT_GYL = 1 << 11,		// GYOffs itself is at once, but the row counter reloads
	TSL_LAT_GYH = 1 << 12		// from it only at the next line start
};

// Layers kept out of the picture (debugger). Only the pixels go: a hidden layer is
// still fetched, so the DRAM load and everything timed by it stay as they were
enum {
	TSL_HIDE_S0 = 1 << 0,
	TSL_HIDE_T0 = 1 << 1,
	TSL_HIDE_S1 = 1 << 2,
	TSL_HIDE_T1 = 1 << 3,
	TSL_HIDE_S2 = 1 << 4,
	TSL_HIDE_GFX = 1 << 5
};
// in layUsed: the set was drawn as one buffer, straight into tsNext - nothing to put together again
#define TSL_LAY_WHOLE	0x80

#ifdef __cplusplus
}
#endif

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
	VID_PRF_MC	// Profi multicolor
};

extern int bufSize;
extern int bytesPerLine;
extern int greyScale;
//extern int scanlines;
extern int noflic;
extern int noflicMode;
extern float noflicGamma;

extern unsigned char* scrimg;
extern unsigned char* bufimg;
extern unsigned char pscr[];

void vid_dot_full(Video*, unsigned char);
void vid_dot_half(Video*, unsigned char);

//typedef int(*vcbmrd)(int, void*);
//typedef void(*vcbmwr)(int, int, void*);
typedef void(*cbvid)(Video*);
typedef void(*vcbptr)(void*);

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
} xVideoMode;

struct Video {
	unsigned nogfx:1;	// tsl : nogfx flag, pc98xx:disable display
	unsigned newFrame:1;	// set @ start of VBlank
	int intFRAME;		// aka INT
	unsigned intLINE:1;	// for TSConf
	unsigned intDMA:1;	// for TSConf
//	unsigned noScreen:1;
	unsigned nodraw:1;	// emulate the raster but put no pixels out
	unsigned debug:1;
	unsigned upd:1;
	unsigned tail:1;
	unsigned linedbl:1;	// lines doubler

	unsigned hblank:1;	// HBlank signal
	unsigned vblank:1;	// VBlank signal

	unsigned hbrd:1;	// border.H
	unsigned vbrd:1;	// border.V

	int nsPerFrame;
	int nsPerLine;
	long long nsPerDotFixed;	// the dot period, 16.16, and what the ray steps by
	double nsPerDotExact;	// same value as a double, for vid_upd_layout() to
				// re-derive nsPerLine/nsPerFrame from
	int nsPerDot;		// whole-ns form. Nothing here reads it any more; it is
				// kept because upstream's Video struct has it
	long long nsDrawFixed;	// time handed in but not yet drawn, 16.16
	long long nsOwedFixed;	// fraction of a ns owed to vid->time, carried not dropped
	int time;		// whole ns drawn since the step began
	int busy;		// (cycles) to emulate busy period
	int dotPerFrame;

	int flash;
	int vidPage;

	int brdstep;
	int brdmode;		// VID_BRD_* : how much border to show
	unsigned char brdcol;
	unsigned char nextbrd;

	unsigned char inten;	// interrupts enable (8 bits = 8 signals)
	unsigned char intrq;	// interrupt output signals (8 bits)
	unsigned char intbf;	// buffered int (last int signals)

	unsigned char paln;	// high bits = palete number
	uint32_t pal[256];	// ABGR inside int, R = LSB, A = FF
	uint32_t gpal[256];	// greyscale copy of pal
	uint32_t bpal[256];	// base palette (loaded preset)

	int vmode;
	xVideoMode* cb;
	cbvid cbCount;		// call when busy count down to 0

	cbxrd mrd;		// external memory reading
	cbxwr mwr;		// external memory writing
	cbirq xirq;		// interrupt
	void* xptr;

	int fcnt;
	int lcnt;
	unsigned char atrbyte;
	unsigned char fntbyte;			// font byte the text mode is showing: ZX Evo reads it back
	int snowLow;				// ULA snow: address bits 6-0 the refresh cycle forced on this burst (-1 = none)
	int snowBank;				// ULA snow: the bank that burst reads instead of the screen one
	unsigned snowDup:1;			// ULA snow: this burst is lost, the ULA shows the previous one again
	size_t frmsz;
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
	vCoord sc;		// screen scroll registers
	int inth;		// interrupts
	int intf;

	struct {
		int xPos;			// position of screen @ monitor [32|12] x [44|24|0]
		int yPos;
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
		unsigned palUpd:1;		// cram was written: apply it at the next line start
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
void vid_sync(Video*,int);		// whole-ns entry point, for callers outside the ZX paths
void vid_sync_fixed(Video*,long long);
// sets the dot period alone. vid_upd_timings() is the complete one - it also
// re-derives nsPerLine/nsPerFrame, which the frame timer in emulwin.cpp reads
void vid_set_dot_ns(Video*,double);
// void vid_irq(Video*, int);
void vid_set_mode(Video*,int);
void vid_reset_ray(Video*);
void vid_set_ray(Video*, int);

// How far an i/o cycle's ULA window sits from a memory cycle's at the same ray,
// in dots. Fuse anchors both at the same tstate, but ula128.tap - which matches a
// real 128K and Spectaculator pixel for pixel - only comes out right this way, so
// the test wins over the model until we know why they differ. The wait states and
// the floating bus are both read through it.
#define IO_CONT_DOTS (-4)

int vid_wait_dots(Video*, int, int, int);	// contention wait in dots, not ns
int vid_snow(Video*, int, int);			// cpu refresh cycle: disturb the ULA if it is fetching now
int vid_float_bus(Video*);			// the byte the ULA has on the bus now, -1 if none
void vid_dark_tail(Video*);

void vid_clear_image(void);
void vid_flat_border(Video*, int idx);
void vid_set_layout(Video*, vLayout*);
void vid_set_resolution(Video*, int, int);
void vid_set_border(Video*, int);
void vid_upd_crop(Video*);
void vid_widen_crop(Video*, int);
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
int vid_scr_dot(int, int*, int*);
int vid_scr_bit(int);

void vid_set_grey(int);
xColor vid_get_col(Video*, int);
void vid_set_col(Video*, int, xColor);
void vid_set_red(Video*, int, int);
void vid_set_green(Video*, int, int);
void vid_set_blue(Video*, int, int);
int vid_zx_palette(Video*);
void vid_set_bcol(Video*, int, xColor);
void vid_reset_col(Video*, int);

void vid_fnt_wr(Video*, int, int);
int vid_fnt_rd(Video*, int);
void vid_fnt_load(Video*, const char*);
void vid_fnt_del(Video*);

void tslUpdatePorts(Video*);

#ifdef __cplusplus
}
#endif

// The ZX Spectrum Next, as its FPGA core (3.02.03) is. Every rule here comes from
// the core's VHDL, checked against a simulation of the core itself; the register
// and signal names are the core's (zxnext.vhd, video/zxula_timing.vhd).
//
// Time: the core's raster counters are the master. Xpeccy's ray stands on the
// core's raw frame counters shifted by the blanking (ray.x 0 is the first border
// dot), so its blanking-edge counters xb/yb ARE the raw counters H/V. The cpu runs
// at 3.5/7/14/28 MHz off the pixel clocks; a 3.5 MHz T-state starts on an odd
// hc_ula, always.

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "zxnext.h"
#include "zx_step.h"
#include "../fatfile.h"
#include "../filetypes/filetypes.h"
#include "../xlog.h"
#include "../cpu/Z80/z80.h"

// a test bench's view of the bus: every i/o write and every nextreg write, the ray
// at the moment the core would log them
void (*next_log_out)(Computer*, int, int) = NULL;
void (*next_log_nr)(Computer*, int, int, int) = NULL;
int next_reset_open = 0;
void (*next_board_reset)(Computer*) = NULL;
int next_ear_tied = 0;
long long next_log_at = -1;
long long next_log_t = -1;
long long next_log_cyc = 0;

const nxTiming nxTim[4] = {
	{448, 312, 264, 128, 64, 40, 96, 8, 116, 0, 0, 32},	// 48K
	{456, 311, 264, 136, 64, 40, 96, 8, 128, 1, 0, 36},	// 128K
	{456, 311, 264, 136, 64, 40, 96, 8, 126, 1, 0, 32},	// +3
	{448, 320, 320, 128, 80, 80, 64, 16, 439, 319, 319, 36}	// Pentagon (no 60 Hz)
};

static void nx_layout(Computer* comp) {
	const nxTiming* t = nx_tim(comp);
	NX.spr.idleTo = 0;		// the swaps move with the raster
	vLayout lay;
	int hz60 = NX.hz60 && (NX.timing != NXT_PENT);
	lay.full.x = t->L;
	lay.full.y = nx_lines(comp);
	lay.blank.x = t->hblank;
	lay.blank.y = t->vblank;
	lay.bord.x = t->hact - t->hblank;
	lay.bord.y = nx_vact(comp) - t->vblank;
	lay.scr.x = 256;
	lay.scr.y = 192;
	lay.intpos.x = t->inth + 1;	// int_ula is a flop: high on the pixel after the compare
	lay.intpos.y = hz60 ? t->intv60 : t->intv50;
	lay.intSize = 0;		// the pulse is the machine's own (next_irq_ack)
	vid_set_layout(comp->vid, &lay);
	comp->vid->upd = 1;
}

// == the raster ==

// The raw counters where the ray stands now (the video walked to cpu->t first)
void nx_raw(Computer* comp, int* h, int* v) {
	Video* vid = comp->vid;
	const nxTiming* t = nx_tim(comp);
	int x = vid->ray.x + vid_lazy_dots(vid);
	int y = vid->ray.y + t->vblank;
	if (x >= vid->vend.x) {
		*h = x - vid->vend.x;
		y++;
	} else {
		*h = x + t->hblank;
	}
	while (y >= vid->full.y) y -= vid->full.y;
	*v = y;
}

// hc_ula / vc_ula / cvc at raw (h, v). They are flops fed from the raw counters
// one pixel back (zxula_timing.vhd 344): hc_ula is 0 on the pixel after raw H =
// c_min_hactive - 12, and vc_ula steps there. The first frame after power-on still
// counts from the configuration values (hc_ula 256, vc_ula 0, cvc 0).
// rv: the raw line the ray is on (-1: where it is now)
static void nx_ula_rv(Computer* comp, int h, int v, int* hc, int* vc, int* cvc, int rv) {
	const nxTiming* t = nx_tim(comp);
	int L = t->L;
	int F = nx_lines(comp);
	int vact = nx_vact(comp);
	int r0 = t->hact - 11;			// raw H of hc_ula 0
	if (NX.powerFrame && (v == 0) && (h < r0)) {
		*hc = 256 + h;
		*vc = 0;
		if (cvc) *cvc = 0;
		return;
	}
	*hc = (h >= r0) ? h - r0 : h - r0 + L;
	int lv = (h >= r0) ? v : v - 1;		// the line vc_ula has stepped to
	if (lv < 0) {
		// the last line of the frame before: vcLine0 once the ray is in the new frame,
		// worked out while it is still on that line (it is not taken yet)
		if (rv < 0) {
			int rh;
			nx_raw(comp, &rh, &rv);
		}
		if (rv == 0) {
			*vc = NX.vcLine0;
			if (cvc) *cvc = NX.cvcLine0;
		} else {
			*vc = F - 1 - vact;
			if (cvc) *cvc = (NX.cuOffset + *vc) % F;
		}
		return;
	}
	if (lv >= vact) {
		*vc = lv - vact;
		if (cvc) *cvc = (NX.cuOffset + *vc) % F;
		return;
	}
	// above the paper they count on from the frame before, whatever its length was
	int n = lv + 1;
	*vc = NX.vcLine0 + n;
	if (cvc) {
		int c = NX.cvcLine0 + n;		// cvc wraps from c_max_vc to 0, a 9-bit counter past it
		if (NX.cvcLine0 < F) c %= F;
		else if (c > 511) c = (c - 512) % F;
		*cvc = c;
	}
}

void nx_ula(Computer* comp, int h, int v, int* hc, int* vc, int* cvc) {
	nx_ula_rv(comp, h, v, hc, vc, cvc, -1);
}

// hc_ula / vc_ula as the ray on (h, v) had them, the ray long gone (a line drawn after it)
void nx_ula_seen(Computer* comp, int h, int v, int* hc, int* vc) {
	nx_ula_rv(comp, h, v, hc, vc, NULL, v);
}

void next_counters(Computer* comp, int* hc, int* vc, int* cvc) {
	int h, v;
	nx_raw(comp, &h, &v);
	nx_ula(comp, h, v, hc, vc, cvc);
}

static void nx_raw_ahead(Computer* comp, int clk28, int* h, int* v) {
	Video* vid = comp->vid;
	nx_raw(comp, h, v);
	long long sub = (vid->nsDrawFixed % vid->nsPerDotFixed) * 4 / vid->nsPerDotFixed;
	*h += (int)((sub + clk28) / 4);
	while (*h >= nx_tim(comp)->L) {
		*h -= nx_tim(comp)->L;
		*v = (*v + 1) % nx_lines(comp);
	}
}

void next_counters_ahead(Computer* comp, int clk28, int* hc, int* vc, int* cvc) {
	int h, v;
	nx_raw_ahead(comp, clk28, &h, &v);
	nx_ula(comp, h, v, hc, vc, cvc);
}

int next_vsync_frame_at(Computer*, long long);

// The raw counters of the dot holding CLK_28 time t, from the line the ray is on
void nx_raw_at(Computer* comp, long long t, int* h, int* v) {
	nx_raw(comp, h, v);
	long long k = (t >> 2) - (NX.frameBase + *h);
	int L = nx_tim(comp)->L;
	int F = nx_lines(comp);
	long long hh = *h + k;
	long long dv = nx_fdiv(hh, L);
	long long vv = (*v + dv) % F;
	*h = (int)(hh - dv * L);
	*v = (int)((vv < 0) ? vv + F : vv);
}

void next_counters_at(Computer* comp, long long t, int* hc, int* vc, int* cvc) {
	int h, v;
	nx_raw_at(comp, t, &h, &v);
	nx_ula(comp, h, v, hc, vc, cvc);
}

int next_vsync_frame_at(Computer* comp, long long t) {
	int h, v, rh, rv;
	nx_raw(comp, &rh, &rv);			// vsyncCount counts the line 1 starts the ray has passed
	nx_raw_at(comp, t, &h, &v);
	int before = (v == 1) && ((h < 5) || ((h == 5) && ((t & 3) < 3)));
	return NX.vsyncCount - (((rv == 1) && before) ? 1 : 0);
}

// The frame count as the simulation's testbench takes it at a CLK_28 edge: vsync_n
// goes low on raw (1, 1), the VS_n pin at raw (5, 1) after the video pipeline
// (zxula_timing.vhd 360-378, zxnext.vhd 6882, 7083, 7367-7399), the testbench sees it
// on the CLK_14 edge after and its frame number from the CLK_28 edge after that
int next_vsync_frame(Computer* comp, int clk28) {
	return next_vsync_frame_at(comp, nx_now(comp) + clk28);
}

int next_esp_fitted = 0;
int next_esp_pins = 0;

// the ESP's lines: held in reset by NR 02 b7 (o_RESET_PERIPHERAL); GPIO0 driven by the Next
// while NR A8 b0 is set (level NR A9 b0) and by the ESP when it makes it an output; GPIO2 by
// the ESP alone. Undriven, both are pulled up on the ESP-01. Both driving: 0 wins (not measured)
int nx_esp_reset(Computer* comp) {
	return (NX.nr[0x02] & 0x80) ? 1 : 0;
}

int nx_esp_gpio0(Computer* comp) {
	int lv = 1;
	if (NX.nr[0xa8] & 1) lv &= NX.nr[0xa9] & 1;
	if (next_esp_pins & 1) lv &= (next_esp_pins >> 1) & 1;
	return lv;
}

int nx_esp_gpio2(Computer* comp) {
	return (next_esp_pins & 4) ? (next_esp_pins >> 3) & 1 : 1;
}

// The time now in CLK_28 periods since power-on: the dot the ray is on, and the
// part of it gone
long long nx_now(Computer* comp) {
	Video* vid = comp->vid;
	long long p = vid->nsPerDotFixed;
	long long f = vid->nsDrawFixed;
	if (f < 0) {
		int h, v;
		nx_raw(comp, &h, &v);
		return (NX.frameBase + h) * 4 + ((f % p) * 4) / p;
	}
	// nx_raw's h with one division at most: asked on every DMA edge and i/o cycle
	int lazy = 0;
	if (f >= p) {
		lazy = (f < 0x7fffffffLL) ? (int)((unsigned)f / (unsigned)p) : (int)(f / p);
		f -= lazy * p;
	}
	int x = vid->ray.x + lazy;
	int h = (x >= vid->vend.x) ? x - vid->vend.x : x + nx_tim(comp)->hblank;
	long long f4 = f * 4;			// the quarters of the dot gone: f < p
	return (NX.frameBase + h) * 4 + (f4 >= p) + (f4 >= 2 * p) + (f4 >= 3 * p);
}

// The cpu clock edges since the start: what 48K/128K contention holds is time without
// an edge. cpu->t stands at the start of a clock.
long long nx_edge(Computer* comp) {
	return comp->tickTotal + comp->cpu->t - NX.holds;
}

// == memory ==

// SRAM map (zxnext.vhd 2920-2931): ROM0-3 at 0, divMMC ROM 0x010000, Multiface ROM
// 0x014000 and RAM 0x016000, alt ROM 0x018000/0x01C000, divMMC RAM 0x020000, RAM page p
// at 0x040000 + p * 0x2000. The CPU's copies of pages 0A, 0B and 0E are BRAM: they are
// kept in ramData at their SRAM place, and the SRAM bytes behind them in NX.shadow.
// where a NEXTREG's write lands, CLK_28 after its request
#define NX_NR_W		2

#define NX_RAM		0x040000
#define NX_DIVROM	0x010000
#define NX_MFROM	0x014000
#define NX_MFRAM	0x016000
#define NX_ALTROM	0x018000
#define NX_DIVRAM	0x020000

static int nx_bram_page(int pg) {
	return (pg == 0x0a) || (pg == 0x0b) || (pg == 0x0e);
}

static int nx_shadow_ofs(int pg, int adr) {
	return ((pg == 0x0e) ? 0x4000 : ((pg - 0x0a) << 13)) | (adr & 0x1fff);
}

// the machine type's ROM (zxnext.vhd 2981-3008): rom number 0-3 and alt_128_n
static int nx_mtype(Computer* comp) {
	int t = NX.nr[0x03] & 7;
	if ((t == 0) || (t == 1)) return NXT_48;
	if ((t == 2) || (t == 4)) return NXT_128;
	return NXT_P3;
}

static int nx_rom(Computer* comp, int* alt128n) {
	int lock = (NX.nr[0x8c] >> 4) & 3;
	int r7 = (NX.p7ffd >> 4) & 1;
	switch (nx_mtype(comp)) {
		case NXT_48:
			*alt128n = !(!(lock & 2) && (lock & 1));
			return 0;
		case NXT_128:
			*alt128n = lock ? (lock >> 1) : r7;
			return lock ? ((lock >> 1) & 1) : r7;
	}
	*alt128n = lock ? (lock >> 1) : r7;
	return lock ? lock : (((NX.p1ffd >> 2) & 1) << 1) | r7;
}

static int nx_p1024(Computer* comp) {
	return ((NX.nr[0x8f] & 3) == 3) && !(NX.peff7 & 4);
}

static int nx_7ffd_locked(Computer* comp) {
	return nx_p1024(comp) ? 0 : (NX.p7ffd & 0x20);
}

static void nx_divmmc(Computer* comp, int adr, int wr, int* sram, int* ro) {
	// divmmc.vhd 85-101: conmem (automap later), mapram
	int mapram = NX.pe3 & 0x40;
	if (adr < 0x2000) {
		*sram = mapram ? (NX_DIVRAM + 3 * 0x2000) : NX_DIVROM;
		*ro = 1;
	} else {
		int bank = NX.pe3 & 0x0f;
		*sram = NX_DIVRAM + bank * 0x2000;
		*ro = mapram && (bank == 3);
	}
	*sram |= adr & 0x1fff;
}

static int nx_div_on(Computer* comp) {
	return ((NX.pe3 & 0x80) || NX.dmMap) && (NX.nr[0x83] & 1);
}

// the layer 2 CPU mapping over adr, or -1 (zxnext.vhd 2966-2971, 3065)
static int nx_l2map(Computer* comp, int adr, int wr) {
	if (!(NX.p123b & (wr ? 1 : 4))) return -1;	// (NR 83 b5 gates the port only, not the mapping)
	int seg = (NX.p123b >> 6) & 3;
	int blk;
	if (seg == 3) {
		if (adr >= 0xc000) return -1;
		blk = adr >> 14;
	} else {
		if (adr >= 0x4000) return -1;
		blk = seg;
	}
	int bank = ((NX.p123b & 8) ? NX.nr[0x13] : NX.nr[0x12]) + blk + NX.l2ofs;
	return bank & 0x7f;		// bit 7 dropped: wraps modulo 128
}

typedef struct {
	int kind;		// NXM_*
	int adr;		// SRAM address (or ramData place of a BRAM page)
	int pg;			// MMU page for RAM
} nxAccess;

// NXM_ABSENT: an SRAM cycle to a chip the board does not have (past 1 MB on a 1 MB board)
enum {NXM_NONE = 0, NXM_SRAM, NXM_BRAM, NXM_ROM, NXM_SHADOW, NXM_BUS, NXM_RO, NXM_ABSENT};

// The decode for one CPU access (zxnext.vhd 2933-3133)
static void nx_back(Computer* comp, int* h, int* v, int n);

static nxAccess nx_decode_core(Computer* comp, int adr, int wr) {
	nxAccess a = {NXM_NONE, 0, 0};
	int slot = (adr >> 13) & 7;
	int pg = NX.mmu[slot];
	if (adr < 0x4000) {
		if (nx_mf_mem(comp)) {
			// its ROM then its RAM, over divMMC, layer 2 and ROMCS (zxnext.vhd 3030-3036)
			a.kind = (wr && !(adr & 0x2000)) ? NXM_RO : NXM_SRAM;
			a.adr = NX_MFROM + (adr & 0x3fff);
			return a;
		}
		if (nx_div_on(comp)) {
			int ro;
			nx_divmmc(comp, adr, wr, &a.adr, &ro);
			a.kind = (wr && ro) ? NXM_RO : NXM_SRAM;
			return a;
		}
	}
	int l2 = nx_l2map(comp, adr, wr);
	if (l2 >= 0) {
		int p = (l2 << 1) | ((adr >> 13) & 1);
		if (p >= 0xe0) return a;		// banks 112-127: no SRAM cycle
		a.pg = p;
		a.kind = nx_bram_page(p) ? NXM_SHADOW : NXM_SRAM;
		a.adr = nx_bram_page(p) ? nx_shadow_ofs(p, adr) : (NX_RAM + (p << 13) + (adr & 0x1fff));
		return a;
	}
	if (pg < 0xe0) {
		a.pg = pg;
		a.kind = nx_bram_page(pg) ? NXM_BRAM : NXM_SRAM;
		a.adr = NX_RAM + (pg << 13) + (adr & 0x1fff);
		return a;
	}
	if (slot > 1) return a;			// nothing: the last SRAM byte reads back
	if (NX.configMode) {
		// config mode: the 16K SRAM bank NR 04 names, writable (zxnext.vhd 3043-3049)
		a.kind = NXM_SRAM;
		a.adr = (NX.nr[0x04] << 14) | (adr & 0x3fff);
		return a;
	}
	// the ROM branch: ROMCS, alt ROM, ROM
	if ((NX.nr[0x80] & 0x80) && !(NX.nr[0x80] & 0x10) && NX.busRomcs) {
		if (NX.nr[0x80] & 0x40) {
			a.kind = wr ? NXM_RO : NXM_SRAM;
			a.adr = NX_DIVRAM + (0x0e << 13) + (adr & 0x3fff);
		} else {
			a.kind = wr ? NXM_RO : NXM_BUS;
		}
		return a;
	}
	int alt128n;
	int rom = nx_rom(comp, &alt128n);
	if (NX.nr[0x8c] & 0x80) {
		if ((NX.nr[0x8c] & 0x40) ? wr : !wr) {
			a.kind = NXM_SRAM;
			a.adr = NX_ALTROM + (alt128n << 14) + (adr & 0x3fff);
			return a;
		}
	}
	a.kind = wr ? NXM_RO : NXM_ROM;
	a.adr = (rom << 14) + (adr & 0x3fff);
	return a;
}

// The core has no RAM size: on a 1 MB board (issue 2 without its expansion) the cycle
// goes out to chip selects CE2/CE3 with no chip behind them (zxnext_top_issue2.vhd
// 1296-1312). The SRAM image is laid out as the board's SRAM, so that is all of it
// past mem->ramSize.
static inline nxAccess nx_decode(Computer* comp, int adr, int wr) {
	nxAccess a = nx_decode_core(comp, adr, wr);
	if ((a.kind == NXM_SRAM) && (a.adr >= comp->mem->ramSize)) a.kind = NXM_ABSENT;
	return a;
}

// What a read of an absent chip gives. Nothing drives the data bus then and it has no
// pull-ups (zxnext_pins_issue2.ucf): the latch takes what the lines of the byte's half
// still hold (A0: even D7..0, odd D15..8) - the byte the last read of that half
// carried, or the last write (the FPGA drives both halves), cpu, DMA or a layer 2 fetch,
// whichever came last. Not measured on a board, where the charge may leak away.
static int nx_absent_rd(Computer* comp, int adr) {
	int l = adr & 1;
	long long now = comp->tickTotal + comp->cpu->t;
	long long c = nx_now(comp);
	// the cpu's last cycle on that half, in CLK_28 edges back from now (8 a T at 3.5 MHz)
	long long since = c - (now - NX.laneT[l]) * (8 >> NX.speed);
	int b;
	if (nxv_l2_lane(comp, l, since, c, &b) >= 0) return b;
	return NX.lane[l];
}

// an access that makes an SRAM cycle (a read: ROM and the BRAM pages' SRAM copy too, a
// chip or not behind it): it waits a clock at 28 MHz, and it moves the data bus
static inline int nx_sram_cycle(int kind) {
	return (kind == NXM_SRAM) || (kind == NXM_ROM) || (kind == NXM_SHADOW) || (kind == NXM_ABSENT);
}

// a cpu or DMA cycle that puts v on the bus at T t: a read on its half, a write on both
static inline void nx_lane_rd(Computer* comp, int adr, int v, long long t) {
	NX.lane[adr & 1] = v;
	NX.laneT[adr & 1] = t;
}

static inline void nx_lane_wr(Computer* comp, int v) {
	NX.lane[0] = NX.lane[1] = v;
	NX.laneT[0] = NX.laneT[1] = comp->tickTotal + comp->cpu->t;
}

// the cpu clock goes to speed sp at T ts: the times on the lanes kept as the CLK_28
// edges they lie back from it, which nx_absent_rd counts at the new speed
static void nx_lane_rebase(Computer* comp, long long ts, int sp) {
	for (int l = 0; l < 2; l++) {
		long long e = (ts - NX.laneT[l]) * (8 >> NX.speed);
		NX.laneT[l] = ts - e / (8 >> sp);
	}
}

// p3_floating_bus_dat: the byte of the last cpu memory cycle whose slot's MMU page is
// in 16K banks 4-7, in +3 timing, whatever answered it - layer 2, divMMC (zxnext.vhd
// 4488-4509, mem_contend from the MMU page)
static void nx_p3_byte(Computer* comp, int adr, int v) {
	int pg = NX.mmu[(adr >> 13) & 7];
	if ((NX.timing == NXT_P3) && (pg >= 8) && (pg < 16))
		NX.p3fb = v;
}

// == divMMC automap (device/divmmc.vhd, zxnext.vhd 2848-2908, 3137-3138, 4105-4190) ==

// automap_reset: no automap, nothing held (nr_0A b4, port enable bit 8)
static void nx_dm_check(Computer* comp) {
	if (!(NX.nr[0x0a] & 0x10) || !(NX.nr[0x83] & 1))
		NX.dmHeld = NX.dmNext = NX.dmMap = NX.dmButton = 0;
}

// sram_divmmc_automap_rom3_en for a fetch at adr: the ROM branch in that slot with
// ROM 3 (or the 48K alt ROM) seen, no layer 2 read mapping, no ROMCS
static int nx_dm_rom3(Computer* comp, int adr) {
	if (NX.mmu[(adr >> 13) & 7] < 0xe0) return 0;
	if (nx_l2map(comp, adr, 0) >= 0) return 0;
	if ((NX.nr[0x80] & 0x80) && !(NX.nr[0x80] & 0x10) && NX.busRomcs) return 0;
	int alt;
	int rom = nx_rom(comp, &alt);
	if ((NX.nr[0x8c] & 0x80) && !(NX.nr[0x8c] & 0x40)) return alt;	// the alt ROM in read mode
	switch (nx_mtype(comp)) {
		case NXT_48: return 1;
		case NXT_P3: return rom == 3;
	}
	return rom == 1;
}

// An opcode fetch at adr: whether divMMC answers it, and what is held after it
// (dmNext). Instant entries map the fetch itself, delayed ones and the off trap
// act from the next access on; the decode is latched while M1 is low.
static int nx_dm_fetch(Computer* comp, int adr) {
	unsigned char* nr = NX.nr;
	nx_dm_check(comp);
	if (!(nr[0x0a] & 0x10) || !(nr[0x83] & 1)) return 0;
	if (adr >= 0x4000) {			// no entry or exit point up there: what is held stays
		NX.dmNext = NX.dmHeld;
		return NX.dmHeld;
	}
	int ai = 0, ad = 0, ri = 0, rd = 0, off = 0, ni = 0, nd = 0;
	if (!(adr & 0xffc7)) {			// RST n: bit n of B8 (on), B9 (any ROM), BA (instant)
		int b = 1 << ((adr >> 3) & 7);
		if (nr[0xb8] & b) {
			int inst = nr[0xba] & b;
			if (nr[0xb9] & b) {
				ai = inst; ad = !inst;
			} else {
				ri = inst; rd = !inst;
			}
		}
	}
	int bb = nr[0xbb];
	if (((adr & 0xff00) == 0x3d00) && (bb & 0x80)) ri = 1;
	if (((adr & 0xfff8) == 0x1ff8) && (bb & 0x40)) off = 1;
	if (((adr == 0x056a) && (bb & 0x20)) || ((adr == 0x04d7) && (bb & 0x10))
			|| ((adr == 0x0562) && (bb & 0x08)) || ((adr == 0x04c6) && (bb & 0x04)))
		rd = 1;
	if (adr == 0x0066) {
		ni = bb & 2;
		nd = bb & 1;
	}
	int r3 = (ri || rd) && nx_dm_rom3(comp, adr);
	int act = !nx_mf_mem(comp);		// automap_active: not under the Multiface's memory
	int bt = NX.dmButton;
	NX.dmNext = (act && (ai || ad || (bt && (ni || nd)))) || (r3 && (ri || rd)) || (NX.dmHeld && !(act && off));
	return NX.dmHeld || (act && (ai || (bt && ni))) || (r3 && ri);
}

static int nx_mrd_in(Computer* comp, int adr);
static int nx_dmaBus = 0;		// the access is the DMA's: its waits are its own

static int nx_mrd(Computer* comp, int adr, int m1) {
	if (!m1) {
		int p = nx_nmi_pop(comp);		// a stackless NMI's RETN reads NR C2/C3, no memory
		return (p >= 0) ? p : nx_mrd_in(comp, adr);
	}
	nx_nmi_fetch(comp, adr);
	nx_int_m1(comp, comp->tickTotal + comp->cpu->t - 2, (NX.speed == 3) ? 3 : 2);
	NX.dmMap = nx_dm_fetch(comp, adr);
	int v = nx_mrd_in(comp, adr);
	NX.dmHeld = NX.dmMap = NX.dmNext;
	if (NX.dmHeld) NX.dmButton = 0;
	nx_int_opcode(comp, v);
	return v;
}

static int nx_mrd_in(Computer* comp, int adr) {
	if ((NX.nr[0x62] & 0xc0) || NX.copN) {
		// a copper write landing by the T1 edge of this read is in its decode (the address
		// is frozen once MREQ is low, zxnext.vhd 3001-3065); the ray is walked to T3 -
		// unless the clock, counted from the T, is still short of the copper's next move
		long long ts = comp->tickTotal + comp->cpu->t;
		if (NX.copN || (NX.refT < 0) || (NX.refC + (ts - NX.refT) * (8 >> NX.speed) + 4 >= NX.copWake)) {
			comp_vid_to_t(comp);
			long long now = nx_now(comp);
			NX.refT = ts;
			NX.refC = now;
			nx_cop_run(comp, now - 2 * (8 >> NX.speed));
		}
	}
	nxAccess a = nx_decode(comp, adr, 0);
	unsigned char* ram = comp->mem->ramData;
	int v;
	switch (a.kind) {
		case NXM_SRAM:
		case NXM_ROM:
			v = ram[a.adr];
			NX.lastSram = v;
			nx_lane_rd(comp, adr, v, comp->tickTotal + comp->cpu->t);
			break;
		case NXM_SHADOW:
			v = NX.shadow[a.adr];
			NX.lastSram = v;
			nx_lane_rd(comp, adr, v, comp->tickTotal + comp->cpu->t);
			break;
		case NXM_ABSENT:		// the lines keep what they hold: no lane moves
			v = nx_absent_rd(comp, adr);
			NX.lastSram = v;
			break;
		case NXM_BRAM:
			v = ram[a.adr];
			nx_p3_byte(comp, adr, v);
			if (NX.speed == 3 && (a.pg != 0x0e) && !nx_dmaBus) comp->cpu->t++;	// bank 5 waits as SRAM does, bank 7 not
			return v;
		case NXM_BUS:
			nx_p3_byte(comp, adr, 0xff);
			return 0xff;
		default:
			nx_p3_byte(comp, adr, NX.lastSram);
			return NX.lastSram;
	}
	nx_p3_byte(comp, adr, v);
	if ((NX.speed == 3) && !nx_dmaBus) comp->cpu->t++;		// one wait state on an SRAM read at 28 MHz
	return v;
}

// The CLK_28 edge a memory write lands on: the edge after the falling cpu clock
// in T2 (t80na.vhd 226-240). The ray stands at the end of T2 here (z80_mwr).
static long long nx_wr_time(Computer* comp) {
	int tau = 8 >> NX.speed;
	return nx_now(comp) - (tau >> 1) + ((tau > 1) ? 1 : 0);
}

// a memory write landing on CLK_28 edge w
static void nx_mem_wr(Computer* comp, int adr, int val, long long w) {
	nxAccess a = nx_decode(comp, adr, 1);
	// an SRAM write cycle, a chip behind it or not: the FPGA drives the whole bus (a
	// write decodes to no ROM: that is NXM_RO, no cycle)
	if (nx_sram_cycle(a.kind))
		nx_lane_wr(comp, val);
	switch (a.kind) {
		// a byte written as it was changes nothing a read could see: not logged
		case NXM_SRAM:
		case NXM_BRAM:
			if (comp->mem->ramData[a.adr] == (unsigned char)val) break;
			nxv_memw(comp, a.adr, comp->mem->ramData[a.adr], w);
			comp->mem->ramData[a.adr] = val;
			break;
		case NXM_SHADOW:
			if (NX.shadow[a.adr] == (unsigned char)val) break;
			nxv_memw(comp, 0x200000 + a.adr, NX.shadow[a.adr], w);
			NX.shadow[a.adr] = val;
			break;
	}
	nx_p3_byte(comp, adr, val);
}

static void nx_mwr(Computer* comp, int adr, int val) {
	if (nx_nmi_push(comp, val)) return;		// a stackless NMI pushes nothing
	nx_mem_wr(comp, adr, val, nx_wr_time(comp));
}

// The debugger looks through the same overlays as a cpu access, without moving
// the automapper, the floating byte, the raster or any other machine state.
typedef struct {
	int held, next, map, button, last;
	nxIrq irq;
} nxMemViewState;

static nxMemViewState nx_view_save(Computer* comp) {
	nxMemViewState state = {NX.dmHeld, NX.dmNext, NX.dmMap, NX.dmButton, NX.lastSram, NX.irq};
	return state;
}

static void nx_view_restore(Computer* comp, const nxMemViewState* state) {
	NX.dmHeld = state->held;
	NX.dmNext = state->next;
	NX.dmMap = state->map;
	NX.dmButton = state->button;
	NX.lastSram = state->last;
	NX.irq = state->irq;
}

// Whether the machine can write the cell a read of adr shows: its RAM can be
// written whatever lies over it, anything else only if a CPU write lands there
// too - not divMMC's ROM or its bank 3 under MAPRAM, the Multiface's ROM, a ROM
// or the alt ROM being read
static int nx_edit_ok(Computer* comp, int adr, const nxAccess* a) {
	switch (a->kind) {
		case NXM_BRAM:
		case NXM_SHADOW:
			return 1;
		case NXM_SRAM: {
			if (a->adr >= NX_RAM) return 1;
			nxAccess w = nx_decode(comp, adr, 1);
			return (w.kind == NXM_SRAM) && (w.adr == a->adr);
		}
	}
	return 0;
}

// Only an instruction preview advances these latches; nx_disasm restores them.
enum {NX_VIEW_STEP = 8};

static void nx_mem_view(Computer* comp, int adr, int mode, int* value, xAdr* xadr) {
	nxMemViewState state;
	if (mode & MEM_VIEW_FETCH) {
		state = nx_view_save(comp);
		nx_nmi_fetch(comp, adr);
		NX.dmMap = nx_dm_fetch(comp, adr);
	}
	// a value to write is a debugger edit: it goes to the cell a read shows
	int edit = (mode & MEM_VIEW_WRITE) && value;
	nxAccess a = nx_decode(comp, adr, (mode & MEM_VIEW_WRITE) && !edit);
	if (mode & MEM_VIEW_FETCH) {
		if (mode & NX_VIEW_STEP) {
			NX.dmHeld = NX.dmMap = NX.dmNext;
			if (NX.dmHeld) NX.dmButton = 0;
		} else {
			nx_view_restore(comp, &state);
		}
	}

	if (xadr) {
		xadr->type = MEM_EXT;
		xadr->bank = 0;
		xadr->adr = adr & 0xffff;
		xadr->abs = -1;
		switch (a.kind) {
			case NXM_SRAM:
			case NXM_BRAM:
			case NXM_ROM:
				xadr->type = MEM_RAM;
				xadr->bank = a.adr >> comp->mem->pgshift;
				xadr->abs = a.adr;
				break;
			case NXM_SHADOW:
				xadr->bank = (0x200000 + a.adr) >> comp->mem->pgshift;
				xadr->abs = 0x200000 + a.adr;
				break;
		}
	}
	if (!value) return;
	if (edit) {
		if (!(mode & MEM_VIEW_ROM_WRITE) && !nx_edit_ok(comp, adr, &a)) return;
		vid_still_break(comp->vid);		// no write log: the picture is drawn again
		switch (a.kind) {
			case NXM_SRAM:
			case NXM_BRAM:
			case NXM_ROM:
				comp->mem->ramData[a.adr] = *value & 0xff;
				break;
			case NXM_SHADOW:
				NX.shadow[a.adr] = *value & 0xff;
				break;
		}
		return;
	}
	switch (a.kind) {
		case NXM_SRAM:
		case NXM_BRAM:
		case NXM_ROM: *value = comp->mem->ramData[a.adr]; break;
		case NXM_SHADOW: *value = NX.shadow[a.adr]; break;
		case NXM_BUS: *value = 0xff; break;
		case NXM_ABSENT: *value = nx_absent_rd(comp, adr); break;
		default: *value = NX.lastSram; break;
	}
	if ((mode & NX_VIEW_STEP) && nx_sram_cycle(a.kind))
		NX.lastSram = *value;
}

typedef struct {
	Computer* comp;
	int start;
	unsigned char local[16];
	unsigned char* bytes;
	int count, capacity;
} nxDasmView;

static int nx_dasm_read(int adr, int access, void* ptr) {
	nxDasmView* view = (nxDasmView*)ptr;
	int off = (adr - view->start) & 0xffff;
	int code = access & CPU_DASM_CODE;
	// A delayed trap can change the page after the opcode has been fetched.
	if (code && (off < view->count)) return view->bytes[off];
	int mode = (access == CPU_DASM_FETCH) ? MEM_VIEW_FETCH : MEM_VIEW_READ;
	int value;
	nx_mem_view(view->comp, adr & 0xffff, mode | (code ? NX_VIEW_STEP : 0), &value, NULL);
	if (code && (off == view->count)) {
		if (view->count == view->capacity) {
			int size = view->capacity * 2;
			unsigned char* bytes = (unsigned char*)malloc(size);
			if (!bytes) return value;
			memcpy(bytes, view->bytes, view->count);
			if (view->bytes != view->local) free(view->bytes);
			view->bytes = bytes;
			view->capacity = size;
		}
		view->bytes[view->count++] = value;
	}
	return value;
}

static xMnem nx_disasm(Computer* comp, int adr, char* buf, unsigned char* bytes, int size, int fmt) {
	nxMemViewState state = nx_view_save(comp);
	nxDasmView view;
	view.comp = comp;
	view.start = adr & 0xffff;
	view.bytes = view.local;
	view.count = 0;
	view.capacity = sizeof(view.local);
	xMnem mn = cpuDisasmViewFmt(comp->cpu, adr, buf, nx_dasm_read, &view, fmt);
	if (bytes) {
		for (int i = 0; (i < size) && (i < mn.len); i++) bytes[i] = nx_dasm_read(adr + i, CPU_DASM_CODE, &view);
	}
	if (view.bytes != view.local) free(view.bytes);
	nx_view_restore(comp, &state);
	return mn;
}

// The generic map and snapshots read the MMU view, overlays left out
static int nx_rd_last(int adr, void* ptr) {
	Computer* comp = (Computer*)ptr;
	return NX.lastSram;
}

static void nx_wr_none(int adr, int val, void* ptr) {}

// a page with no chip behind it: what a cpu read there would take
static int nx_rd_absent(int adr, void* ptr) {
	return nx_absent_rd((Computer*)ptr, adr);
}

// one 8K slot of the map the debugger and the snapshots read (the cpu's own accesses decode)
static void nx_map_slot(Computer* comp, int s, int rom) {
	int pg = NX.mmu[s];
	if ((pg < 0xe0) && (NX_RAM + (pg << 13) >= comp->mem->ramSize)) {
		memSetBank(comp->mem, s << 5, MEM_EXT, 0, MEM_8K, nx_rd_absent, nx_wr_none, comp);
	} else if (pg < 0xe0) {
		memSetBank(comp->mem, s << 5, MEM_RAM, (NX_RAM >> 13) + pg, MEM_8K, NULL, NULL, NULL);
	} else if (s < 2) {
		memSetBank(comp->mem, s << 5, MEM_RAM, (rom << 1) | s, MEM_8K, NULL, nx_wr_none, NULL);
	} else {
		memSetBank(comp->mem, s << 5, MEM_EXT, 0, MEM_8K, nx_rd_last, nx_wr_none, comp);
	}
}

static void nx_map_mem(Computer* comp) {
	int alt;
	int rom = nx_rom(comp, &alt);
	for (int s = 0; s < 8; s++)
		nx_map_slot(comp, s, rom);
	comp->vid->vidPage = (NX_RAM + (((NX.p7ffd & 8) ? 0x0e : 0x0a) << 13)) >> 14;
}

// A cell of the SRAM image by its area (CELL_*), the page in that area and the
// offset in the page: 16K ROMs, 8K divMMC banks and RAM pages (SRAM map above)
int next_cell(int abs, int* page, int* ofs) {
	static const struct {int base, end, shift, area;} tab[] = {
		{0, NX_DIVROM, 14, CELL_ROM},
		{NX_DIVROM, NX_DIVROM + 0x2000, 13, CELL_DIVROM},
		{NX_MFROM, NX_MFRAM, 13, CELL_MFROM},
		{NX_MFRAM, NX_ALTROM, 13, CELL_MFRAM},
		{NX_ALTROM, NX_DIVRAM, 14, CELL_ALTROM},
		{NX_DIVRAM, NX_RAM, 13, CELL_DIV},
		{NX_RAM, NX_RAM + (0xe0 << 13), 13, CELL_RAM}
	};
	for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
		if ((abs < tab[i].base) || (abs >= tab[i].end)) continue;
		*page = (abs - tab[i].base) >> tab[i].shift;
		*ofs = (abs - tab[i].base) & ((1 << tab[i].shift) - 1);
		return tab[i].area;
	}
	return CELL_NONE;
}

// ...and back: -1 when the area has no such page or offset
int next_cell_abs(int area, int page, int ofs) {
	int base, shift, pages;
	switch (area) {
		case CELL_ROM: base = 0; shift = 14; pages = 4; break;
		case CELL_DIVROM: base = NX_DIVROM; shift = 13; pages = 1; break;
		case CELL_MFROM: base = NX_MFROM; shift = 13; pages = 1; break;
		case CELL_MFRAM: base = NX_MFRAM; shift = 13; pages = 1; break;
		case CELL_ALTROM: base = NX_ALTROM; shift = 14; pages = 2; break;
		case CELL_DIV: base = NX_DIVRAM; shift = 13; pages = 16; break;
		case CELL_RAM: base = NX_RAM; shift = 13; pages = 0xe0; break;
		default: return -1;
	}
	if ((page < 0) || (page >= pages) || (ofs < 0) || (ofs >> shift)) return -1;
	return base + (page << shift) + ofs;
}

// The SRAM behind the BRAM pages 0A, 0B and 0E, as the debugger addresses it
// (0x200000 + its offset in NX.shadow), at its place in the SRAM image
int next_shadow_sram(int abs) {
	int ofs = abs - 0x200000;
	if ((ofs < 0) || (ofs >= (int)sizeof(((xNextState*)0)->shadow))) return -1;
	int pg = (ofs < 0x4000) ? (0x0a + (ofs >> 13)) : 0x0e;
	return NX_RAM + (pg << 13) + (ofs & 0x1fff);
}

int next_mmu_get(Computer* comp, int slot) {
	return ((slot >= 0) && (slot < 8)) ? NX.mmu[slot] : 0xff;
}

void next_mmu_set(Computer* comp, int slot, int page) {
	if ((slot < 0) || (slot >= 8)) return;
	NX.mmu[slot] = page & 0xff;
	int alt;
	nx_map_slot(comp, slot, nx_rom(comp, &alt));
}

// The legacy paging remap (zxnext.vhd 3805-3818, 4619-4684): ramChange is
// port_memory_ram_change_dly, leaving is port_1ffd_special_old
static void nx_remap(Computer* comp, int ramChange, int leaving) {
	int pent = ((NX.nr[0x8f] & 3) == 2) || nx_p1024(comp);
	int bank = NX.p7ffd & 7;
	bank |= (pent ? ((NX.p7ffd >> 6) & 3) : (NX.pdffd & 3)) << 3;
	bank |= (pent ? (nx_p1024(comp) && (NX.p7ffd & 0x20)) : ((NX.pdffd >> 2) & 1)) << 5;
	bank |= (pent ? 0 : ((NX.pdffd >> 3) & 1)) << 6;
	if (NX.p1ffd & 1) {
		static const unsigned char tab[4][8] = {
			{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07},
			{0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f},
			{0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x06, 0x07},
			{0x08, 0x09, 0x0e, 0x0f, 0x0c, 0x0d, 0x06, 0x07}
		};
		memcpy(NX.mmu, tab[(NX.p1ffd >> 1) & 3], 8);
	} else {
		NX.mmu[0] = (NX.peff7 & 8) ? 0x00 : 0xff;
		NX.mmu[1] = (NX.peff7 & 8) ? 0x01 : 0xff;
		if (leaving) {
			NX.mmu[2] = 0x0a;
			NX.mmu[3] = 0x0b;
			NX.mmu[4] = 0x04;
			NX.mmu[5] = 0x05;
		}
		if (ramChange || leaving) {
			NX.mmu[6] = (bank << 1) & 0xff;
			NX.mmu[7] = ((bank << 1) | 1) & 0xff;
		}
	}
	nx_map_mem(comp);
}

// == interrupts ==

static int nx_tick28(Computer* comp) {
	return 8 >> NX.speed;
}

// The line interrupt: int_line is high for the pixel after hc_ula 255 of the line
// whose cvc is N - 1, from the live NR 22/23/C4 and cvc (zxula_timing.vhd 563-583).
// Compared here for the pixels of line v (its raw H 0 at base) and of the line
// before it that lie in [lineChk, now) - the registers have not changed in between,
// every write to them comes here first.
static void nx_line_scan(Computer* comp, long long now, long long base, int v) {
	const nxTiming* t = nx_tim(comp);
	int F = nx_lines(comp);
	int hl = t->hact + 245;				// hc_ula 256
	for (int k = 1; k >= 0; k--) {
		long long p = (base - k * t->L + hl) * 4;
		if ((p < NX.lineChk) || (p >= now)) continue;	// [lineChk, now): a pixel on the last now is not lost
		if (NX.nr[0xc4] & 2) {
			int n = ((NX.nr[0x22] & 1) << 8) | NX.nr[0x23];
			int want = n ? n - 1 : F - 1;
			int hc, vc, cvc;
			nx_ula(comp, hl, (v - k + F) % F, &hc, &vc, &cvc);
			if ((n <= F) && (cvc == want)) {
				nx_ctc_run(comp, p + 1);	// the sources in the order they fire
				nx_int_event(comp, 0, p, 1, 0);
			}
		}
	}
	if (now > NX.lineChk) NX.lineChk = now;
}

// the line compare up to where the ray stands now (the video walked to it)
static void nx_line_now(Computer* comp) {
	int h, v;
	nx_raw(comp, &h, &v);
	nx_line_scan(comp, nx_now(comp), NX.frameBase, v);
}

// the sources that fire by themselves, up to now: the DMA asks while the cpu is held
// inside an instruction, where nothing else looks at them
void nx_int_catch_up(Computer* comp) {
	nx_line_now(comp);
	nx_ctc_run(comp, nx_now(comp) + 1);
}

// After nx_int_catch_up(): the first CLK_28 edge it could find a source firing at - the
// next line interrupt pixel it has not compared, a CTC channel's ZC/TO - bounded by the
// frame's end, where the raster may change
long long nx_int_quiet_until(Computer* comp) {
	const nxTiming* t = nx_tim(comp);
	int L = t->L;
	int h, v;
	nx_raw(comp, &h, &v);
	long long u = (NX.frameBase + (long long)(nx_lines(comp) - v) * L) * 4;
	if (NX.nr[0xc4] & 2) {
		// a pixel is compared once now has passed it ([lineChk, now))
		long long p = (NX.frameBase - L + t->hact + 245) * 4;
		while (p < NX.lineChk) p += (long long)L * 4;
		if (p + 1 < u) u = p + 1;
	}
	long long c = nx_ctc_quiet_until(comp);	// run up to now + 1: an edge at c is in once now reaches it
	if (c < u) u = c;
	c = nx_uart_quiet_until(comp);
	if (c + 1 < u) u = c + 1;
	return u;
}

// CPU.xend through comp_ack_in: INT is sampled one cpu clock before the end of an
// instruction - cpu->t stands there now
// The first CLK_28 after s where INT could come on by itself, with nothing pending
// now: the ULA interrupt pixel, the next line's interrupt pixel. s itself when INT has
// to be looked at on every instruction - a pulse or a device under way, the copper
// or the CTC about to move. A running DMA reaches INT only through what it writes,
// and a nextreg or i/o write looks again (irqDead).
static long long nx_irq_quiet_until(Computer* comp, long long s) {
	if (NX.pulseE >= 0) return s;
	long long chain = nx_int_req_until(comp, s);	// a device in service holds INT off
	if (chain <= s) return s;
	int cop = nx_cop_on(comp);
	if (cop && (NX.copN || (NX.copWake <= s))) return s;
	const nxTiming* t = nx_tim(comp);
	int L = t->L;
	int F = nx_lines(comp);
	int h, v;
	nx_raw(comp, &h, &v);
	// no further than the next frame's start: the raster, and the INT pixel with it, may
	// change there (video_frame_sync)
	long long until = (NX.frameBase + (long long)(F - v) * L) * 4;
	if (until <= s) until = s + 1;
	if (!(NX.pff & 0x40)) {
		long long p = (NX.frameBase + (long long)(comp->vid->intp.y - v) * L + comp->vid->intp.x) * 4;
		if (p < s) p += (long long)F * L * 4;
		if (p < until) until = p;
	}
	if (NX.nr[0xc4] & 2) {
		// a pixel at s itself is the next scan's ([lineChk, now)): no quiet stretch over it
		long long p = (NX.frameBase + t->hact + 245) * 4;
		while (p < s) p += (long long)L * 4;
		if (p < until) until = p;
	}
	if (cop && (NX.copWake < until)) until = NX.copWake;		// a copper write may touch anything
	long long c = nx_ctc_quiet_until(comp);
	if (c < until) until = c;
	c = nx_uart_quiet_until(comp);
	if (c < until) until = c;
	if (chain < until) until = chain;
	return until;
}

void next_irq_ack(Computer* comp) {
	// the edge ending the instruction ends an M-cycle: a DMA takes the bus there before
	// INT is looked at, and the cpu takes an interrupt as it gets the bus back
	if (NX.dma.seq || NX.dma.busrq || NX.dma.busak) {
		comp->cpu->t++;
		nx_dma_cycle(comp);
		comp->cpu->t--;
	}
	long long ts = comp->tickTotal + comp->cpu->t;
	if (ts < NX.irqDead) {		// nothing can have come on since INT was last looked at
		comp->cpu->flgACK = 0;
		return;
	}
	comp_vid_to_t(comp);
	long long s = nx_now(comp);
	NX.refT = ts;
	NX.refC = s;
	nx_cop_run(comp, s);
	nx_line_now(comp);
	nx_ctc_run(comp, s + 1);
	int pulse = nx_pulse_at(comp, s);
	int act = pulse || nx_int_level(comp, s, comp->tickTotal + comp->cpu->t);
	// with the expansion bus on, INT comes from the bus pin alone (zxnext.vhd 1837-1840),
	// and nothing on a bus here pulls it
	if (NX.expbusEff && !(NX.nr[0x80] & 0x20) && (NX.nr[0xc4] & 0x80))
		act = 0;
	if (!pulse && (NX.pulseE >= 0) && (s > NX.pulseE))
		NX.pulseE = -1;
	NX.irqDead = 0;
	if (!act) {
		long long u = nx_irq_quiet_until(comp, s);
		if (u > s) NX.irqDead = ts + (u - s) / nx_tick28(comp);
	}
	comp->cpu->flgACK = act;		// a recording played too: the Next takes its INTs itself (rzx_play_step)
	if (act)
		comp->cpu->intrq |= Z80_INT;
	else
		comp->cpu->intrq &= ~Z80_INT;
}

static int nx_ack(Computer* comp) {
	return nx_int_ack(comp);
}

// the NMI acknowledge: an M1 cycle reading memory as a fetch does (the 28 MHz wait
// of SRAM and bank 5 included, owed to the push after it), then the push NR C2/C3 take
void next_nmi_m1(Computer* comp, int adr) {
	nx_int_m1(comp, comp->tickTotal, (NX.speed == 3) ? 3 : 2);
	nx_nmi_ack(comp);
	if (NX.speed == 3) {
		nxAccess a = nx_decode(comp, adr, 0);
		if (nx_sram_cycle(a.kind) || ((a.kind == NXM_BRAM) && (a.pg != 0x0e)))
			comp->waitDebt++;
	}
}

// the start of a raw line (hblank): the frame latch at raw (0, 0), the vsync count,
// the line interrupt pixel of this line
static void nx_hblank(Computer* comp) {
	Video* vid = comp->vid;
	const nxTiming* t = nx_tim(comp);
	int h, v;
	nx_raw(comp, &h, &v);			// the line starting
	// the line interrupt of the line that ended, before its base moves on
	nx_line_scan(comp, (NX.frameBase + t->L) * 4, NX.frameBase, (v + nx_lines(comp) - 1) % nx_lines(comp));
	NX.frameBase += t->L;			// the line that ended
	nxv_line(comp);				// ...is drawn, the ray past it
	if (v == 0) {
		// the counters' last line of the frame that ended, before the raster changes
		{
			int vc, cvc, hc;
			nx_ula(comp, t->hact - 11, nx_lines(comp) - 1, &hc, &vc, &cvc);
			NX.vcLine0 = vc;
			NX.cvcLine0 = cvc;
			NX.wvcLast = (nx_lines(comp) - 1 - nx_vact(comp) + 32) & 0x1ff;
		}
		// video_frame_sync: the timing and 50/60 Hz in effect from here
		int tim = NX.nr[0x03] >> 4 & 7;
		int ntim = (tim == 2) ? NXT_128 : (tim == 3) ? NXT_P3 : (tim == 4) ? NXT_PENT : (tim >= 5) ? NXT_P3 : NXT_48;
		int nhz = (NX.nr[0x05] >> 2) & 1;
		NX.eff05 = NX.nr[0x05] & 1;
		NX.eff09 = NX.nr[0x09] & 3;
		if ((ntim != NX.timing) || (nhz != NX.hz60)) {
			nxv_retime(comp, NX.frameBase, 0);	// the picture up to the old raster's end
			NX.timing = ntim;
			NX.hz60 = nhz;
			vid_unlazy(vid);
			nx_raw(comp, &h, &v);		// a few dots into line 0 by now (comp_irq walks what was counted)
			nx_layout(comp);
			// the ray at the same raw (h, 0) of the new raster
			vid->ray.x = vid->vend.x + h;
			vid->ray.y = vid->vend.y - 1;
			vid->ray.xb = h;
			vid->ray.yb = 0;
			t = nx_tim(comp);
			nxv_retime(comp, NX.frameBase, 1);	// ...and on from raw (0, 0) of the new one
		}
	}
	if (v == 1)
		NX.vsyncCount++;
	int vact = nx_vact(comp);
	if (NX.powerFrame && (v >= 1))
		NX.powerFrame = 0;
	if (v == vact)
		NX.cuOffset = NX.nr[0x64];	// a write before hc_ula 0 of this line still counts (nx_nr_write)
}

static void nx_irq(Computer* comp, int t) {
	switch (t) {
		case IRQ_VID_INT:
			// the ULA interrupt pixel (raw counters), unless port FF b6 holds it off;
			// a line interrupt pixel before it comes first
			nx_line_now(comp);
			if (!(NX.pff & 0x40)) {
				long long e = (NX.frameBase + comp->vid->intp.x) * 4;	// the pixel, wherever the ray got to
				nx_ctc_run(comp, e + 1);
				nx_int_event(comp, 11, e, 1, 0);
			}
			break;
		case IRQ_NMI:
			nx_nmi_raise(comp, 1, 0);	// the NMI key: the M1 button (F9), the Multiface's
			break;
		case IRQ_RZX_INT:		// a recording's frame ends (a seek): the Next raises its INTs itself
			rzx_frame_end(comp);
			break;
		case IRQ_VID_HBLANK:
			nx_hblank(comp);
			break;
	}
}

// == timing: contention, speed ==

// 48K/128K: the T-state starting at hc_ula s is held this many T (zxula.vhd 579-595)
static const unsigned char nx_hold[16] = {0, 0, 0, 6, 0, 5, 0, 4, 0, 3, 0, 2, 0, 1, 0, 0};

static int nx_page_contended(Computer* comp, int pg) {
	if (pg >= 0x10) return 0;
	switch (NX.timing) {
		case NXT_48: return (pg == 0x0a) || (pg == 0x0b);
		case NXT_128: return (pg & 2) != 0;
		case NXT_P3: return pg >= 8;
	}
	return 0;
}

static int nx_cont_on(Computer* comp) {
	return (NX.speed == 0) && !NX.contDis && (NX.timing != NXT_PENT);
}

// +3: the waits a memory cycle whose T1 starts at hc_ula s gets: WAIT_n is
// sampled at e = s + 3 and on every 2 dots while wait_s holds
static int nx_p3_waits(Computer* comp, int h, int v) {
	const nxTiming* t = nx_tim(comp);
	int hh = h + 3;
	int vv = v;
	if (hh >= t->L) {
		hh -= t->L;
		vv = (vv + 1) % nx_lines(comp);
	}
	int hc, vc;
	nx_ula(comp, hh, vv, &hc, &vc, NULL);
	int w = 0;
	while ((hc < 256) && (vc < 192)) {
		int n = hc & 15;
		if ((n > 0) && (n < 3)) break;		// wait_s is low at nibbles 1, 2
		w++;
		hc += 2;
	}
	return w;
}

// After power-on hc_ula counts from 256 and its first restart flips its parity
// against the raw counter: the 3.5 MHz T-state that spans it is a dot longer, so
// T-states still start on odd hc_ula. Asked at the start of a bus cycle, back dots
// before where the ray stands now.
static void nx_relock(Computer* comp, int back) {
	if (!NX.powerFrame || NX.speed) return;
	int h, v, hc, vc;
	nx_raw(comp, &h, &v);
	nx_back(comp, &h, &v, back);
	nx_ula(comp, h, v, &hc, &vc, NULL);
	if (!(hc & 1)) {
		vid_sync_fixed(comp->vid, comp->vid->nsPerDotFixed);
		NX.refT = -1;
	}
}

// a nr_07 write takes effect at the next cpu edge with the bus idle (zxnext.vhd 5809)
static void nx_speed_apply(Computer* comp) {
	NX.irqDead = 0;
	NX.speedPend = 0;
	// expbus_eff_en and cpu_speed are taken on the same edge; with the bus on, the
	// speed is the bus's, which is always 3.5 MHz (zxnext.vhd 5800-5820, 5496)
	NX.expbusEff = (NX.nr[0x80] >> 7) & 1;
	int sp = NX.expbusEff ? 0 : (NX.nr[0x07] & 3);
	if (sp == NX.speed) return;
	comp_vid_to_t(comp);
	long long s = nx_now(comp);
	long long ts = comp->tickTotal + comp->cpu->t;
	nx_line_now(comp);			// the sources up to here fired in the old clock
	nx_ctc_run(comp, s + 1);
	NX.refT = ts;
	NX.refC = s;
	nx_pulse_rebase(comp, s);
	nx_lane_rebase(comp, ts, sp);
	NX.speed = sp;
	compSetHwTurbo(comp, (double)(1 << sp));
	// The T-state that starts here runs to the next edge of the new clock: short
	// when the edge is off its grid. 3.5 MHz edges are the dot starts of odd
	// hc_ula, the faster ones every 4/2/1 CLK_28.
	long long d;
	if (sp == 0) {
		int h, v, hc, vc;
		nx_raw(comp, &h, &v);
		nx_ula(comp, h, v, &hc, &vc, NULL);
		d = 4 - (s & 3);			// the next dot start, 1..4 ahead
		if (!((hc + 1) & 1)) d += 4;		// ...and the one after if that dot is even
	} else {
		long long g = nx_tick28(comp);
		d = g - (s % g);
	}
	if (d != nx_tick28(comp)) {
		vid_sync_fixed(comp->vid, d * comp->vid->nsPerDotFixed / 4);
		res4 = comp->cpu->t + 1;		// that T-state is d CLK_28 long, the video stands at its end
	}
	NX.refT = ts + 1;			// the first edge of the new clock
	NX.refC = s + d;
}

// the start of a memory cycle (mreq) or of an internal T-state, the ray at it
static void nx_cont(Computer* comp, int mreq) {
	// the ray at the cycle start, for what below reads it: a pending speed change, the
	// power-on relock, the DMA, a contention switch, contention itself. At 7-28 MHz
	// with none of them nothing does, and what draws catches up at its own next look.
	if (NX.speedPend || NX.powerFrame || NX.dma.seq || NX.dma.busrq || NX.dma.busak
			|| nx_cont_on(comp) || (NX.contDis != ((NX.nr[0x08] >> 6) & 1)))
		comp_vid_to_t(comp);
	if (NX.speedPend && (nx_now(comp) >= NX.speedT + 2)) nx_speed_apply(comp);
	int h, v, hc, vc;
	nx_relock(comp, 0);
	int mstart = mreq || comp->cpu->flgMSTART;	// a memory cycle, or an internal one of its own
	comp->cpu->flgMSTART = 0;
	if (mstart && (NX.dma.seq || NX.dma.busrq || NX.dma.busak)) nx_dma_cycle(comp);
	if (mreq && nx_nmi_stack(comp)) {
		// a stackless push or pop: no memory cycle, so no WAIT, and in 48K/128K timing
		// held on all three T-states as an internal cycle is (zxula.vhd 595)
		if (!nx_cont_on(comp) || (NX.timing == NXT_P3)) return;
		if (!nx_page_contended(comp, NX.mmu[(comp->cpu->adr >> 13) & 7])) return;
		const nxTiming* t = nx_tim(comp);
		int F = nx_lines(comp);
		nx_raw(comp, &h, &v);
		long long c = nx_now(comp);
		int held = 0;
		for (int k = 0; k < 3; k++) {
			nx_ula(comp, h, v, &hc, &vc, NULL);
			int d = ((hc < 256) && (vc < 192)) ? nx_hold[hc & 15] : 0;
			held += d;
			nx_hold_note(comp, c, d);
			c += 8 * (d + 1);
			h += 2 * (d + 1);
			while (h >= t->L) {
				h -= t->L;
				v = (v + 1) % F;
			}
		}
		comp->cpu->t += held;
		NX.holds += held;
		return;
	}
	// eff_nr_08_contention_disable is taken on an idle edge in the second half of the line
	if (NX.contDis != ((NX.nr[0x08] >> 6) & 1)) {
		nx_raw(comp, &h, &v);
		nx_ula(comp, h, v, &hc, &vc, NULL);
		if (hc >= 256) NX.contDis ^= 1;
	}
	if (!nx_cont_on(comp)) return;
	int pg = NX.mmu[(comp->cpu->adr >> 13) & 7];
	if (!nx_page_contended(comp, pg)) return;
	nx_raw(comp, &h, &v);
	if (NX.timing == NXT_P3) {
		if (mreq) comp->cpu->t += nx_p3_waits(comp, h, v);	// WAIT: the clock runs on
		return;
	}
	nx_ula(comp, h, v, &hc, &vc, NULL);
	if ((hc < 256) && (vc < 192)) {
		int d = nx_hold[hc & 15];
		nx_hold_note(comp, nx_now(comp), d);
		comp->cpu->t += d;
		NX.holds += d;
	}
}

// 48K/128K i/o contention (zxnext.vhd 4496): the port term and the page of the
// high byte. Which T-states of T1 T2 TW T3 are held, by those two.
static int nx_port_contended(Computer* comp, int port) {
	if (!(port & 1)) return 1;
	// port_7ffd_active (zxnext.vhd 2593-2599): the 7FFD decode with its enable, not a
	// 1FFD that is enabled, in 128K or +3 timing
	int is1ffd = ((port & 0xf003) == 0x1001) && (NX.nr[0x82] & 8);
	if (((port & 0x8003) == 0x0001) && !is1ffd && (NX.nr[0x82] & 2) && (NX.timing != NXT_48)) {
		if ((NX.timing != NXT_P3) || (port & 0x4000)) return 1;
	}
	if (((port & 0xff) == 0x3b) && (NX.nr[0x85] & 1) && (((port >> 8) == 0xbf) || ((port >> 8) == 0xff))) return 1;
	return 0;
}

// The holds of an i/o cycle whose T1 starts at raw (h, v), CLK_28 c; hold[k] for T-state k
static void nx_io_holds(Computer* comp, int port, int h, int v, long long c, int* hold) {
	hold[0] = hold[1] = hold[2] = hold[3] = 0;
	if (!nx_cont_on(comp) || (NX.timing == NXT_P3)) return;
	int hi = nx_page_contended(comp, NX.mmu[(port >> 13) & 7]);
	int pc = nx_port_contended(comp, port);
	int cand = hi ? (pc ? 3 : 15) : (pc ? 2 : 0);		// bit k: T-state k a candidate
	if (!cand) return;
	const nxTiming* t = nx_tim(comp);
	int F = nx_lines(comp);
	for (int k = 0; k < 4; k++) {
		if (cand & (1 << k)) {
			int hc, vc;
			nx_ula(comp, h, v, &hc, &vc, NULL);
			if ((hc < 256) && (vc < 192)) hold[k] = nx_hold[hc & 15];
		}
		nx_hold_note(comp, c, hold[k]);
		c += 8 * (hold[k] + 1);
		h += 2 * (hold[k] + 1);
		while (h >= t->L) {
			h -= t->L;
			v = (v + 1) % F;
		}
	}
}

// The interrupt acknowledge: 7 T, T1a T1b T1c (two automatic waits) T2 T3 T4 T5, the
// address PC to T2 and I:R from T3. In 48K/128K timing T1a-c and T5 are stretch
// candidates always, T2 and T3 only when PC is not a contended port (IORQ is low then).
// The cpu runs it as fixed T, so its holds are owed to the next bus cycle.
static void nx_inta(void* ptr) {
	Computer* comp = (Computer*)ptr;
	nx_int_m1(comp, comp->tickTotal, 4);		// M1 is low through T1 T1 T1 T2
	if (!nx_cont_on(comp) || (NX.timing == NXT_P3)) return;
	CPU* cpu = comp->cpu;
	int pc = cpu->regPC;
	int ir = cpu->regI << 8;
	int pcport = nx_port_contended(comp, pc);
	const nxTiming* t = nx_tim(comp);
	int F = nx_lines(comp);
	int h, v;
	nx_raw(comp, &h, &v);		// the step before left the ray where this one starts
	long long c = nx_now(comp);
	int held = 0;
	for (int k = 0; k < 7; k++) {
		int adr = -1;
		if (k < 3) adr = pc;
		else if ((k == 3) && !pcport) adr = pc;
		else if ((k == 4) && !pcport) adr = ir;
		else if (k == 6) adr = ir;
		int d = 0;
		if ((adr >= 0) && nx_page_contended(comp, NX.mmu[(adr >> 13) & 7])) {
			int hc, vc;
			nx_ula(comp, h, v, &hc, &vc, NULL);
			if ((hc < 256) && (vc < 192)) d = nx_hold[hc & 15];
		}
		held += d;
		nx_hold_note(comp, c, d);
		c += 8 * (d + 1);
		h += 2 * (d + 1);
		while (h >= t->L) {
			h -= t->L;
			v = (v + 1) % F;
		}
	}
	comp->waitDebt += held;
	NX.holds += held;
}

// raw (h, v) of a time n dots back
static void nx_back(Computer* comp, int* h, int* v, int n) {
	const nxTiming* t = nx_tim(comp);
	*h -= n;
	while (*h < 0) {
		*h += t->L;
		*v = (*v + nx_lines(comp) - 1) % nx_lines(comp);
	}
}

// == nextreg ==

static void nx_soft_reset(Computer* comp, long long w);
static void nx_hard_reset(Computer* comp);

// The CLK_28 time a port 253B read samples the counters at: port_253b_dat follows the
// register one CLK_28 behind, port_253b_dat_0 takes it on each falling cpu clock edge,
// and the cpu reads that at the end of T3 - so as it was at the edge in TW (zxnext.vhd
// 5871-5882). The ray stands at the start of T3 when the read is made.
static long long nx_rd_t;

static int nx_nr_read(Computer* comp, int r) {
	unsigned char* nr = NX.nr;
	switch (r) {
		case 0x00: return 0x0a;			// machine id
		case 0x01: return 0x32;			// core version 3.02
		case 0x02: return (nr[0x02] & 0x80) | (nx_nmi_cause(comp) ? 0x10 : 0) | (nr[0x02] & 0x0c) | (NX.resetType & 3);
		case 0x03: return (NX.palPhase << 7) | (nr[0x03] & 0x70) | (nr[0x03] & 0x08) | (nr[0x03] & 7);
		case 0x05: return (nr[0x05] & 0xfa) | (NX.hz60 << 2) | (NX.eff05 & 1);
		case 0x07: return (NX.speed << 4) | (nr[0x07] & 3);
		case 0x08: return (nx_7ffd_locked(comp) ? 0 : 0x80) | (NX.contDis << 6) | (nr[0x08] & 0x3f);
		case 0x09: return (nr[0x09] & 0xf0) | (nr[0x09] & 0x04) | (NX.eff09 & 3);
		case 0x0a: return nr[0x0a] & 0xfb;
		case 0x0b: return nr[0x0b] & 0xb1;
		case 0x0e: return 0x03;			// sub version
		case 0x0f: return 0x00;			// board issue 2
		case 0x10: return nr[0x10] & 0x7c;
		case 0x11: return nr[0x11] & 7;
		case 0x12: case 0x13: return nr[r] & 0x7f;
		case 0x18: case 0x19: case 0x1a: case 0x1b:
			return NX.clip[r - 0x18][NX.clipIdx[r - 0x18]];
		case 0x1c: return (NX.clipIdx[3] << 6) | (NX.clipIdx[2] << 4) | (NX.clipIdx[1] << 2) | NX.clipIdx[0];
		case 0x1e: case 0x1f: {
			int h, v, hc, vc, cvc;
			nx_raw_at(comp, nx_rd_t, &h, &v);
			nx_ula(comp, h, v, &hc, &vc, &cvc);
			return (r == 0x1e) ? (cvc >> 8) & 1 : cvc & 0xff;
		}
		case 0x20: {
			nx_line_now(comp);
			nx_ctc_run(comp, nx_rd_t + 1);
			int s = nx_int_status(comp);
			return ((s & 1) << 7) | (((s >> 11) & 1) << 6) | ((s >> 3) & 0x0f);
		}
		case 0x22: {
			if (!NX.inVid) {
				comp_vid_to_t(comp);
				nx_line_now(comp);
				nx_ctc_run(comp, nx_rd_t + 1);
			}
			int p = NX.inVid ? nx_pulse_at(comp, nx_now(comp)) : nx_pulse_read(comp, nx_rd_t);
			return (p ? 0x80 : 0) | ((NX.pff & 0x40) ? 4 : 0) | (NX.nr[0xc4] & 2) | (nr[0x22] & 1);
		}
		case 0x28: return NX.pal44;		// NR 44's first byte as stored
		case 0x2c: case 0x2e: return 0x80;	// the I2S sample, I2S off
		case 0x2f: return nr[r] & 3;
		case 0x34: return nxs_mirror(comp);
		case 0x41: return (NX.pal[(nr[0x43] >> 4) & 7][nr[0x40]] >> 1) & 0xff;
		case 0x44: {
			unsigned short c = NX.pal[(nr[0x43] >> 4) & 7][nr[0x40]];
			return ((c >> 9) & 3) << 6 | (c & 1);
		}
		case 0x4c: return nr[r] & 0x0f;
		case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
			return NX.mmu[r - 0x50];
		case 0x61: return NX.coppAdr & 0xff;
		case 0x62: return (nr[0x62] & 0xc0) | ((NX.coppAdr >> 8) & 7);
		case 0x69: return ((NX.p123b & 2) << 6) | ((NX.p7ffd & 8) << 3) | (NX.pff & 0x3f);
		case 0x6a: return nr[r] & 0x3f;
		case 0x6e: case 0x6f: return nr[r] & 0xbf;
		case 0x70: return nr[r] & 0x3f;
		case 0x71: return nr[r] & 1;
		case 0x81: return (NX.busRomcs ? 0x80 : 0) | (nr[0x81] & 0x78);
		case 0x85: case 0x89: return nr[r] & 0x8f;
		case 0x8a: return nr[r] & 0x3f;
		case 0x8e: return ((NX.pdffd & 1) << 7) | ((NX.p7ffd & 7) << 4) | 8 | ((NX.p1ffd & 1) << 2) | ((NX.p1ffd >> 1) & 2)
				| ((((NX.p7ffd >> 4) & 1) && !(NX.p1ffd & 1)) || (((NX.p1ffd >> 1) & 1) && (NX.p1ffd & 1)));
		case 0x8f: return nr[r] & 3;
		case 0x90: return nr[r] & 0xfc;
		case 0x93: return nr[r] & 0x0f;
		case 0xa0: return nr[r] & 0x39;
		case 0xa2: return (nr[r] & 0xdd) | 0x02;
		case 0xa8: return nr[r] & 1;
		case 0xa9: return !next_esp_fitted ? 0 : (nx_esp_gpio2(comp) << 2) | nx_esp_gpio0(comp);	// the pins GPIO2, GPIO0
		case 0xc0: return (nr[0xc0] & 0xe9) | ((comp->cpu->regIMN & 3) << 1);
		case 0xc4: return (nr[0xc4] & 0x82) | ((NX.pff & 0x40) ? 0 : 1);
		case 0xc5: return nx_ctc_c5_read(comp);
		case 0xc6: case 0xce: return nr[r] & 0x77;
		case 0xc8: {
			nx_line_now(comp);
			int s = nx_int_status(comp);
			return ((s & 1) << 1) | ((s >> 11) & 1);
		}
		case 0xc9:
			nx_ctc_run(comp, nx_rd_t + 1);
			return (nx_int_status(comp) >> 3) & 0xff;
		case 0xca: {
			int s = nx_int_status(comp);
			int b = ((s >> 13) & 1) << 6 | ((s >> 2) & 1) * 0x30 | ((s >> 12) & 1) << 2 | ((s >> 1) & 1) * 0x03;
			return b;
		}
		case 0xb0: case 0xb1: case 0xb2: return nx_inp_nr_read(comp, r);
		case 0xcc: return nr[r] & 0x83;
		case 0xd8: return nr[r] & 1;
		case 0xda: return nx_nmi_cause(comp);
		// stored as written
		case 0x06: case 0x14: case 0x15: case 0x16: case 0x17: case 0x23: case 0x26: case 0x27:
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x40: case 0x42: case 0x43: case 0x4a: case 0x4b:
		case 0x64: case 0x68: case 0x6b: case 0x6c: case 0x7f: case 0x80: case 0x82: case 0x83: case 0x84:
		case 0x86: case 0x87: case 0x88: case 0x8c: case 0x91: case 0x92: case 0xb8: case 0xb9: case 0xba:
		case 0xbb: case 0xc2: case 0xc3: case 0xcd: case 0xd9:
			return nr[r];
	}
	return 0x00;		// unassigned, write-only, or a block not yet here
}

// == for a viewer: nothing below moves the machine ==

// A nextreg as a read gives it, with nothing run up to now first: the line and CTC
// status may stand a few dots behind what the cpu would read.
int next_nr_peek(Computer* comp, int r) {
	int s = nx_int_status(comp);
	switch (r) {
		case 0x1e: case 0x1f: {
			int hc, vc, cvc;
			next_counters(comp, &hc, &vc, &cvc);
			return (r == 0x1e) ? (cvc >> 8) & 1 : cvc & 0xff;
		}
		case 0x20: return ((s & 1) << 7) | (((s >> 11) & 1) << 6) | ((s >> 3) & 0x0f);
		case 0x22: return (nx_pulse_at(comp, nx_now(comp)) ? 0x80 : 0) | ((NX.pff & 0x40) ? 4 : 0)
				| (NX.nr[0xc4] & 2) | (NX.nr[0x22] & 1);
		case 0xc8: return ((s & 1) << 1) | ((s >> 11) & 1);
		case 0xc9: return (s >> 3) & 0xff;
	}
	return nx_nr_read(comp, r);
}

// "... Garry Lancaster v2.09" in NextZXOS's ROM, its last character with bit 7 set
void next_os_version(Computer* comp, char* buf, int len) {
	static const char key[] = "Lancaster v";
	const int kl = sizeof(key) - 1;
	unsigned char* sram = comp->mem->ramData;
	if (len < 2) return;
	buf[0] = 0;
	for (int i = 0; i + kl < 0x10000; i++) {
		if (memcmp(sram + i, key, kl)) continue;
		int n = 0;
		buf[n++] = 'v';
		for (int j = i + kl; (j < 0x10000) && (n < len - 1); j++) {
			int c = sram[j] & 0x7f;
			if ((c < 0x20) || (c > 0x7e)) break;
			buf[n++] = c;
			if (sram[j] & 0x80) break;
		}
		buf[n] = 0;
		return;
	}
}

// a byte of 8K RAM page pg as the cpu reads it
int next_ram(Computer* comp, int pg, int ofs) {
	if ((pg < 0) || (pg >= 0xe0)) return 0xff;
	return comp->mem->ramData[NX_RAM + (pg << 13) + (ofs & 0x1fff)];
}

// ...and as layer 2 reads it: the SRAM behind the cpu's BRAM copies of 0A, 0B, 0E
int next_l2ram(Computer* comp, int pg, int ofs) {
	if (nx_bram_page(pg)) return NX.shadow[nx_shadow_ofs(pg, ofs)];
	return next_ram(comp, pg, ofs);
}

// the ROM slots 0 and 1 show with MMU FF
int next_rom(Computer* comp) {
	int alt;
	return nx_rom(comp, &alt);
}

unsigned int next_rgb(int c) {
	return (((c >> 6) & 7) * 255 + 3) / 7 | (((((c >> 3) & 7) * 255 + 3) / 7) << 8) | ((((c & 7) * 255 + 3) / 7) << 16);
}

// the CLK_28 edge the nextreg write being made lands on
static long long nx_w;

// the video up to the cpu, unless the write comes from inside the video tick
static void nx_vsync(Computer* comp) {
	if (!NX.inVid) comp_vid_to_t(comp);
}

static void nx_reset(Computer* comp);
static void nx_set_vmode(Computer* comp, int m);
static int nx_vmode_of(int v);
static int nxInFirmware = 0;		// the writes below are the firmware's own (nx_firmware)

static void nx_nr_write(Computer* comp, int r, int v) {
	unsigned char* nr = NX.nr;
	NX.irqDead = 0;
	NX.copWake = 0;
	switch (r) {		// what nx_dma_int() reads: the raster, the line compare, the chain
		case 0x02: case 0x03: case 0x05: case 0x22: case 0x23: case 0x64:
		case 0xc0: case 0xc4: case 0xc5: case 0xc6: case 0xc8: case 0xc9: case 0xca:
		case 0xcc: case 0xcd: case 0xce:
			NX.dmaIntDead = 0;
			break;
	}
	switch (r) {
		case 0x00: case 0x01: case 0x0e: case 0x0f: case 0x1e: case 0x1f:
			return;		// read only
		case 0x02:
			// b7 stored; b3/b2 request an NMI (the flags set while none is being taken,
			// which is always here until the NMI comes), a 0 clears them; b4 (io trap) a 0 clears
			// (the flags are set while an NMI could be taken, NR 06 or not)
			nr[0x02] = (nr[0x02] & 0x0c & v) | (v & 0x80) | (nx_nmi_accepts(comp) ? (v & 0x0c) : 0);
			if (!(v & 0x10)) nx_nmi_cause_clear(comp);
			if (v & 0x0c) nx_nmi_raise(comp, v & 8, v & 4);
			if ((v & 2) && !next_reset_open) {
				nx_hard_reset(comp);
				return;
			}
			if (v & 1) nx_soft_reset(comp, NX.nrLastW);
			return;
		case 0x04:
			nr[0x04] = v & 0x7f;		// issue 2 board: 7 bits (zxnext.vhd 5708-5719)
			return;
		case 0x03:
			if ((v & 0x80) && !(nr[0x03] & 0x08) && !(v & 0x08)) {
				int tm = (v >> 4) & 7;
				if (tm == 0) tm = 1;
				if (tm > 4) tm = 3;
				nr[0x03] = (nr[0x03] & 0x0f) | (tm << 4);
				if (tm == 4) nr[0x05] &= ~4;	// no 60 Hz on the Pentagon
			}
			if (v & 0x08) nr[0x03] ^= 0x08;
			if (NX.configMode && ((v & 7) >= 1) && ((v & 7) <= 4)) nr[0x03] = (nr[0x03] & 0xf8) | (v & 7);
			if ((v & 7) == 7) {
				NX.configMode = 1;
				nx_nmi_config(comp);
			}
			else if (v & 7) NX.configMode = 0;
			return;
		case 0x05:
			nr[0x05] = v;
			if (((nr[0x03] >> 4) & 7) == 4) nr[0x05] &= ~4;
			return;
		case 0x07:
			nr[0x07] = v & 3;
			NX.speedPend = 1;
			nx_vsync(comp);
			NX.speedT = nx_now(comp);
			return;
		case 0x80:
			nr[0x80] = v;
			NX.speedPend = 1;		// the bus's enable is taken with the speed
			nx_vsync(comp);
			NX.speedT = nx_now(comp);
			return;
		case 0x20:
			// software interrupts: b7 line, b6 ULA, b3:0 CTC - requests whatever the
			// enables, for the one CLK_28 of the write (zxnext.vhd 1946-1947)
			if (v & 0xcf) {
				nx_vsync(comp);
				nx_line_now(comp);
				nx_ctc_run(comp, nx_w + 1);
				if (v & 0x80) nx_int_event(comp, 0, nx_w, 0, 1);
				for (int k = 0; k < 4; k++)
					if (v & (1 << k)) nx_int_event(comp, 3 + k, nx_w, 0, 1);
				if (v & 0x40) nx_int_event(comp, 11, nx_w, 0, 1);
			}
			return;
		case 0x23: case 0x64:
			nx_vsync(comp);
			nx_line_now(comp);		// the compare so far, with the value it had
			nr[r] = v;
			if (r == 0x64) {
				int h, vv;
				nx_raw(comp, &h, &vv);
				if ((vv == nx_vact(comp)) && (h < nx_tim(comp)->hact - 11))
					NX.cuOffset = v;	// before hc_ula 0 of the first paper line: taken this frame
			}
			return;
		case 0x08:
			if (v & 0x80) NX.p7ffd &= ~0x20;
			nr[0x08] = v & 0x7f;
			nx_snd_nr(comp, 0x08, nx_w);
			return;
		case 0x06:
			nr[0x06] = (v & 0xfb) | (NX.configMode ? (v & 0x04) : (nr[0x06] & 0x04));
			nx_snd_nr(comp, 0x06, nx_w);
			return;
		case 0x2c: case 0x2d: case 0x2e:
			nx_snd_dac_nr(comp, r, v, nx_w);
			break;
		case 0x0a:
			nr[0x0a] = (v & 0x1b) | (NX.configMode ? (v & 0xe0) : (nr[0x0a] & 0xe0));
			nx_dm_check(comp);
			return;
		case 0x83:
			nr[0x83] = v;
			nx_dm_check(comp);
			nx_mf_enable(comp);
			return;
		case 0x81:
			nr[0x81] = v;
			if (!(v & 8)) NX.p1ffdMtr = 0;		// the bus's FDC off holds the motor off
			return;
		case 0x10:
			if (NX.configMode) nr[0x10] = (nr[0x10] & 0x60) | (v & 0x1f);
			return;
		case 0x11:
			// a change of timing is a power-on reset (zxnext_top_issue2.vhd 771-781), after
			// which the firmware writes it again from its config: from the firmware the
			// same write comes back and settles; from a program it ends in a power cycle
			if (NX.configMode) {
				int m = nx_vmode_of(v);
				nr[0x11] = m;
				if (m == NX.vmode) return;
				nx_set_vmode(comp, m);
				if (!nxInFirmware && !next_reset_open) {
					NX.powered = 0;
					NX.firmware = 1;
					nx_reset(comp);
					cpu_reset(comp->cpu);
				}
			}
			return;
		case 0x18: case 0x19: case 0x1a: case 0x1b: {
			int c = r - 0x18;
			NX.clip[c][NX.clipIdx[c]] = v;
			NX.clipIdx[c] = (NX.clipIdx[c] + 1) & 3;
			return;
		}
		case 0x1c:
			for (int i = 0; i < 4; i++)
				if (v & (1 << i)) NX.clipIdx[i] = 0;
			return;
		case 0x22:
			nx_vsync(comp);
			nx_line_now(comp);
			nr[0x22] = v & 1;
			nr[0xc4] = (nr[0xc4] & ~2) | (v & 2);
			NX.pff = (NX.pff & ~0x40) | ((v & 4) << 4);
			return;
		case 0x40:
			nr[0x40] = v;
			NX.palPhase = 0;
			return;
		case 0x41: {
			int p = (nr[0x43] >> 4) & 7;
			NX.pal[p][nr[0x40]] = (v << 1) | (((v >> 1) | v) & 1);
			nxv_pal(comp, p, nr[0x40], nx_w);
			if (!(nr[0x43] & 0x80)) nr[0x40]++;
			NX.palPhase = 0;
			return;
		}
		case 0x43:
			nr[0x43] = v;
			NX.palPhase = 0;
			return;
		case 0x44: {
			int p = (nr[0x43] >> 4) & 7;
			if (!NX.palPhase) {
				NX.pal44 = v;
				NX.palPhase = 1;
			} else {
				NX.pal[p][nr[0x40]] = (NX.pal44 << 1) | (v & 1) | (((v >> 6) & 3) << 9);
				nxv_pal(comp, p, nr[0x40], nx_w);
				if (!(nr[0x43] & 0x80)) nr[0x40]++;
				NX.palPhase = 0;
			}
			return;
		}
		case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
			// the paging ports' remap wins over an MMU write on the same edge (zxnext.vhd 4607-4700)
			if ((nx_w >= NX.memChg0) && (nx_w <= NX.memChg1)) return;
			NX.mmu[r - 0x50] = v;
			{
				int alt;
				nx_map_slot(comp, r - 0x50, nx_rom(comp, &alt));
			}
			return;
		case 0x60:
			// each byte lands at once; an even one also loads the latch 0x63 shares (zxnext.vhd 5418-5424)
			NX.coppMem[NX.coppAdr] = v;
			if (!(NX.coppAdr & 1)) NX.coppStored = v;
			NX.coppAdr = (NX.coppAdr + 1) & 0x7ff;
			return;
		case 0x61:
			NX.coppAdr = (NX.coppAdr & 0x700) | v;
			return;
		case 0x62:
			NX.coppAdr = (NX.coppAdr & 0xff) | ((v & 7) << 8);
			nr[0x62] = v & 0xc0;
			return;
		case 0x63:
			if (!(NX.coppAdr & 1)) {
				NX.coppStored = v;
			} else {
				NX.coppMem[NX.coppAdr - 1] = NX.coppStored;
				NX.coppMem[NX.coppAdr] = v;
			}
			NX.coppAdr = (NX.coppAdr + 1) & 0x7ff;
			return;
		case 0x69:
			NX.p123b = (NX.p123b & ~2) | ((v >> 6) & 2);
			NX.p7ffd = (NX.p7ffd & ~8) | ((v >> 3) & 8);
			NX.pff = (NX.pff & 0xc0) | (v & 0x3f);
			nx_map_mem(comp);
			return;
		case 0x68:
			nr[0x68] = v & 0xfd;
			return;
		case 0x6e: case 0x6f:
			nr[r] = v & 0xbf;
			return;
		case 0x28: case 0x29: case 0x2b:
			nx_inp_nr(comp, r, v);		// the keymaps; a read of 28 shows NR 44's byte
			return;
		case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
		case 0x75: case 0x76: case 0x77: case 0x78: case 0x79:
			nxs_nr(comp, r, v, nx_w);
			return;
		case 0x09: case 0x15: case 0x4b:
			// the sprite pass reads these live: it runs up to the write first
			nxs_run(comp, 2 * nx_w - 1);
			if (r == 0x09) {
				if (v & 8) NX.pe3 &= ~0x40;
				nr[0x09] = v & 0xf7;
			} else {
				nr[r] = v;
			}
			return;
		case 0x85: case 0x89:
			nr[r] = v & 0x8f;
			return;
		case 0x8e:
			if (v & 8) {
				NX.p7ffd = (NX.p7ffd & ~7) | ((v >> 4) & 7);
				NX.pdffd = (NX.pdffd & ~0x0f) | ((v >> 7) & 1);
			}
			if (!(v & 4)) NX.p7ffd = (NX.p7ffd & ~0x10) | ((v & 1) << 4);
			{
				int leaving = (NX.p1ffd & 1) && !(v & 4);
				NX.p1ffd = (NX.p1ffd & ~7) | ((v >> 1) & 1) << 2 | ((v & 1) << 1) | ((v >> 2) & 1);
				nx_remap(comp, (v & 8) ? 1 : 0, leaving);
			}
			NX.memChg0 = NX.memChg1 = nx_w + 1;	// port_memory_change_dly from nr_8e_we (zxnext.vhd 3813)
			return;
		case 0x8f:
			nr[0x8f] = v & 3;
			nx_remap(comp, 1, 0);
			NX.memChg0 = NX.memChg1 = nx_w + 2;	// ...and from nr_8f_we_dly, an edge later
			return;
		case 0xc0:
			nr[0xc0] = v & 0xe9;
			nx_int_mode(comp);
			if (!(v & 8)) NX.irq.retnC2 = 0;
			return;
		case 0xc5:
			nx_ctc_c5(comp, v, nx_w);
			return;
		case 0xc9:
			nx_ctc_run(comp, nx_w + 1);
			nx_int_clear(comp, (v & 0xff) << 3);
			return;
		case 0xca:
			nx_int_clear(comp, ((v & 0x40) ? 1 << 13 : 0) | ((v & 0x04) ? 1 << 12 : 0)
					| ((v & 0x30) ? 1 << 2 : 0) | ((v & 0x03) ? 1 << 1 : 0));
			return;
		case 0xc4:
			nx_vsync(comp);
			nx_line_now(comp);
			nr[0xc4] = v & 0x82;
			NX.pff = (NX.pff & ~0x40) | ((v & 1) ? 0 : 0x40);
			return;
		case 0xc8:
			nx_int_clear(comp, ((v & 2) ? 1 : 0) | ((v & 1) ? 1 << 11 : 0));
			return;
		case 0xff:
			// the ULA+ palette: ULA palette (first/second by NR 43 b6) entry C0 + the ULA+ index
			NX.pal[(nr[0x43] & 0x40) ? 4 : 0][0xc0 | (NX.ulapSel & 0x3f)] = (v << 1) | (((v >> 1) | v) & 1);
			nxv_pal(comp, (nr[0x43] & 0x40) ? 4 : 0, 0xc0 | (NX.ulapSel & 0x3f), nx_w);
			return;
	}
	nr[r] = v;
}

// A nextreg written for a debugger, at once: no cpu cycle around it, so it lands
// where the machine stands, as a NEXTREG the stopped cpu made would
void next_nr_poke(Computer* comp, int r, int v) {
	nx_nr_write(comp, r & 0xff, v & 0xff);
}

// == the copper (device/copper.vhd, zxnext.vhd 3942-4000, 4706-4777) ==

// hc_ula and cvc as a copper edge t sees them: those of the dot that ends at or after it
static void nx_cop_ctr(Computer* comp, long long t, int* hc, int* cvc) {
	int h, v, vc;
	nx_raw_at(comp, t - 1, &h, &v);
	nx_ula(comp, h, v, hc, &vc, cvc);
}

// a nextreg write latched on edge w: the log (one line per run of consecutive edges,
// as the simulation's), the register, the picture
static void nx_nr_land(Computer* comp, int reg, int val, int late, long long w, int cop) {
	if (next_log_nr && (w != NX.nrLastW + 1)) {
		next_log_at = cop ? w : -1;
		next_log_nr(comp, reg, val, late);
		next_log_at = -1;
	}
	NX.nrLastW = w;
	nx_w = w;
	NX.inVid += cop;
	if (cop) {
		int i = NX.copLogHead;
		NX.copLog[i].w = w;
		NX.copLog[i].reg = reg;
		NX.copLog[i].old = nx_nr_read(comp, reg);
		NX.copLogHead = (i + 1) & 7;
	}
	nx_nr_write(comp, reg, val);
	nxv_regs(comp, w);
	NX.inVid -= cop;
}

// The copper's edges up to t. A MOVE goes out on its edge and is latched two edges
// later; on every edge the copper sees the nextreg writes latched before it.
int nx_cop_hold = 0;

void nx_cop_run(Computer* comp, long long t) {
	if (NX.copT >= t) return;
	int mode = NX.nr[0x62] >> 6;
	if (!mode && (NX.copLast == mode) && !NX.copN) {
		NX.copT = t;
		NX.copDout = 0;
		return;
	}
	if ((mode == NX.copLast) && !NX.copN && (t < NX.copWake)) {
		NX.copT = t;			// a WAIT that cannot be met before copWake: every edge alike
		return;
	}
	// hc_ula/cvc stand for the four edges of a dot: worked out once a dot, and only where
	// asked (mode 11's restart, a WAIT); a nextreg write landing may move cvc (nr_64)
	long long cDot = -1;
	int chc = 0, ccvc = 0;
	for (long long e = NX.copT + 1; e <= t; e++) {
		NX.copWake = 0;
		mode = NX.nr[0x62] >> 6;
		if (mode != NX.copLast) {
			// a new mode: 01 and 11 start from the top, nothing runs on this edge
			NX.copLast = mode;
			if (mode & 1) NX.copPc = 0;
			NX.copDout = 0;
		} else if (mode) {
			int hc = 0, cvc = 0;
			if ((mode == 3) || !NX.copDout) {
				if (((e - 1) >> 2) != cDot) {
					nx_cop_ctr(comp, e, &chc, &ccvc);
					cDot = (e - 1) >> 2;
				}
				hc = chc;
				cvc = ccvc;
			}
			if ((mode == 3) && !hc && !cvc) {
				NX.copPc = 0;
				NX.copDout = 0;
			} else if (NX.copDout) {
				NX.copDout = 0;
			} else {
				int ins = (NX.coppMem[NX.copPc << 1] << 8) | NX.coppMem[(NX.copPc << 1) | 1];
				if (ins & 0x8000) {
					// WAIT: the line equal, hc_ula at or past 8h + 12 (9 bits)
					int hw = (((ins >> 9) & 0x3f) * 8 + 12) & 0x1ff;
					if ((cvc == (ins & 0x1ff)) && (hc >= hw)) {
						NX.copPc = (NX.copPc + 1) & 0x3ff;
					} else if (!NX.copN && !NX.powerFrame) {
						// Nothing moves before the counters reach the column waited for on
						// its line, or the next line (hc_ula 0, also mode 11's restart):
						// the counters stand for the 4 edges of a dot, and step by one a dot.
						// Not across the frame start, where the raster may change.
						int L = nx_tim(comp)->L;
						long long s = ((cvc == (ins & 0x1ff)) && (hw < L)) ? (hw - hc) : (L - hc);
						long long k = (e - 1) >> 2;
						long long ne = (k + s) * 4;	// the edge before the first of that dot
						int rh, rv;
						nx_raw_at(comp, e - 1, &rh, &rv);
						if (rv == nx_lines(comp) - 1) {
							long long fs = (k + (L - rh)) * 4;
							if (fs < ne) ne = fs;
						}
						NX.copWake = ne + 1;
						if (ne > t) ne = t;
						if (ne > e) e = ne;
					}
				} else {
					int r = (ins >> 8) & 0x7f;
					if (r && (NX.copN < 4)) {
						NX.copDout = 1;
						NX.copQ[NX.copN].w = e + 2;
						NX.copQ[NX.copN].reg = r;
						NX.copQ[NX.copN].val = ins & 0xff;
						NX.copN++;
					}
					NX.copPc = (NX.copPc + 1) & 0x3ff;
				}
			}
		} else {
			NX.copDout = 0;
		}
		if (NX.copN && (NX.copQ[0].w <= e)) {
			// the copper stands at e: a write landing may have the picture drawn up to
			// it (nxv_room), which runs the copper to there - nothing to run again
			NX.copT = e;
			do {
				int reg = NX.copQ[0].reg;
				int val = NX.copQ[0].val;
				NX.copN--;
				memmove(NX.copQ, NX.copQ + 1, NX.copN * sizeof(NX.copQ[0]));
				nx_nr_land(comp, reg, val, 0, e, 1);
				cDot = -1;
			} while (NX.copN && (NX.copQ[0].w <= e));
			if (nx_cop_hold) {
				// the picture's queues are full: it is drawn up to here before the
				// copper runs on (nxv_fetch_to)
				nx_cop_hold = 0;
				return;
			}
		}
	}
	NX.copT = t;
	nx_cop_hold = 0;
}

// the nextreg write path for the cpu: NEXTREG, port 253B
// late: the request is that many cpu ticks past cpu->t (an OUT's is in T2, cpu->t at T1)
// w: the CLK_28 edge the write lands on, unless the copper's write has it (then one later)
static void nx_nr_req(Computer* comp, int reg, int val, int late, long long w) {
	nx_cop_run(comp, w);
	if (NX.nrLastW == w) w++;
	nx_nr_land(comp, reg, val, late, w, 0);
}

static void nx_xnextreg(void* ptr, int reg, int val) {
	Computer* comp = (Computer*)ptr;
	comp_vid_to_t(comp);
	nx_relock(comp, 0);			// the request goes out at the start of a bus cycle
	vid_settle(comp->vid);
	nx_nr_req(comp, reg & 0xff, val & 0xff, 0, nx_now(comp) + NX_NR_W);
}

// == ports ==

static void nx_out_7ffd(Computer* comp, int val) {
	if (nx_7ffd_locked(comp)) return;
	NX.p7ffd = val;
	nx_remap(comp, 1, 0);
}

// One SPI byte both ways (serial/spi_master.vhd): SD0 is the machine's SD card;
// nothing answers SD1, the Pi or the FPGA flash, so MISO stays high
static int nx_spi_byte(Computer* comp, int val) {
	SDCard* sd = comp->sdc;
	if (NX.pe7 & 1) return 0xff;
	sd->on = 1;
	sd->cs = 0;
	int r = sdc_xfer(sd, val);
	sd->cs = 1;
	return r;
}

// A transfer is 16 CPU clocks (serial/spi_master.vhd). The port EB strobe is up on
// two rising CPU edges, TW and T3 (iord/iowr, zxnext.vhd 2705-2706): an access is
// taken on the first of them that finds the transfer under way in its last clock,
// else it is lost. miso_dat takes a transfer's byte a clock after its last, and the
// CPU latches a read on the falling edge in T3. e: the TW edge, in CLK_28.
static int nx_spi_miso(Computer* comp, long long e) {
	int tau = 8 >> NX.speed;
	if (NX.spiT < 0) return NX.spiMiso;
	return (2 * e + 3 * tau >= 2 * (NX.spiT + 16 * tau)) ? NX.spiMiso : NX.spiOld;
}

static void nx_spi_xfer(Computer* comp, int val, long long e) {
	int tau = 8 >> NX.speed;
	if ((NX.spiT >= 0) && (e < NX.spiT + 14 * tau)) return;
	NX.spiOld = NX.spiMiso;
	NX.spiMiso = nx_spi_byte(comp, val);
	NX.spiT = ((NX.spiT >= 0) && (e < NX.spiT + 15 * tau)) ? (NX.spiT + 15 * tau) : e;
}

static int nx_in_dev(Computer* comp, int port, int cpu);

// FE bit 6: i_AUDIO_EAR or port_fe_reg(4), whatever the speaker does (zxnext.vhd 3459, 3598).
// The board's EAR input is the tape; left steady for 1.15 ms it relaxes to the issue 2 MIC
// read-back, NR 08 b0 with FE b3 (zxnext_top_issue2.vhd 643-677), which is all it is with
// no tape playing.
static int nx_fe_ear(Computer* comp) {
	int ear = 0;
	if (!next_ear_tied) {
		if (comp->tape->on && !comp->tape->rec)
			ear = (comp->tape->volPlay & 0x80) ? 1 : 0;
		else
			ear = (NX.nr[0x08] & 1) && (NX.pfe & 8);
	}
	return ear || (NX.pfe & 0x10);
}

static int nx_in(Computer* comp, int port) {
	// an M-cycle boundary: the DMA may take the bus first
	if ((NX.dma.seq || NX.dma.busrq || NX.dma.busak) && nx_dma_cycle(comp))
		comp_vid_to_lazy(comp, comp->cpu->t + 3);
	// the holds of 48K/128K i/o contention: iord walked the ray to T1 + 3, the
	// data is taken in T3. Contention is 3.5 MHz only, where 3 T are 6 dots.
	int h, v;
	nx_relock(comp, 6);
	nx_raw(comp, &h, &v);
	nx_back(comp, &h, &v, 6);
	int hold[4];
	nx_io_holds(comp, port, h, v, nx_now(comp) - 24, hold);
	int held = hold[0] + hold[1] + hold[2] + hold[3];
	if (held) {
		comp->cpu->t += held;
		NX.holds += held;
		comp_vid_to_lazy(comp, comp->cpu->t + 3);
	}
	return nx_in_dev(comp, port, 1);
}

// What answers an i/o read, the ray at the start of T3 (the DMA's: of its last
// READ_4); cpu: the cpu's cycle, not the DMA's
static int nx_in_dev(Computer* comp, int port, int cpu) {
	vid_settle(comp->vid);
	int lo = port & 0xff;
	int hi = (port >> 8) & 0xff;
	int res = 0;
	int any = 0;
	unsigned char* nr = NX.nr;
	// the zxnDMA's ports; not to the DMA itself (port_dma_rd needs the bus the cpu's)
	if (((lo == 0x6b) && (nr[0x82] & 0x20)) || ((lo == 0x0b) && (nr[0x85] & 2))) {
		if (cpu) {
			res |= nx_dma_port_rd(comp, port);
			any = 1;
		}
	}
	if (!(port & 1)) {
		// the membrane, then the rest of the matrix; bit 6 the Next's own
		res |= (xInFE(comp, port) & (0xa0 | nx_inp_fe(comp, port))) | (nx_fe_ear(comp) ? 0x40 : 0);
		any = 1;
	}
	{
		int hit = 0;
		int j = nx_inp_in(comp, port, &hit);		// joysticks and mouse
		if (hit) {res |= j; any = 1;}
	}
	{
		int s = nx_snd_in(comp, port);
		if (s >= 0) {res |= s; any = 1;}
	}
	if ((lo == 0x3b) && (hi == 0x24)) {res |= NX.nrsel; any = 1;}
	if ((lo == 0x3b) && (hi == 0x30) && (nr[0x83] & 0x40)) {res |= nxs_status(comp, nx_now(comp) - 2 * (8 >> NX.speed)); any = 1;}	// taken on the falling edge as T2 starts
	if ((lo == 0x3b) && (hi == 0x25)) {
		nx_rd_t = nx_now(comp) - (((8 >> NX.speed) + 1) >> 1) - 2;
		long long cut = nx_now(comp) - 2 * (8 >> NX.speed);
		nx_cop_run(comp, cut);		// a copper write by the T2 edge is in the read, a later one not
		int v = nx_nr_read(comp, NX.nrsel);
		for (int k = 0; k < 8; k++) {
			int i = (NX.copLogHead + 7 - k) & 7;
			if (NX.copLog[i].w <= cut) break;
			if (NX.copLog[i].reg == NX.nrsel) v = NX.copLog[i].old;
		}
		res |= v;
		any = 1;
	}
	if ((lo == 0x3b) && (hi == 0x12) && (nr[0x83] & 0x80)) {res |= NX.p123b & 0xcf; any = 1;}
	// the I2C lines as they stand, port_i2c_io_en = NR 83 b2 (zxnext.vhd 2630, 3256-3267): SCL is
	// the master's alone, nothing on the bus stretches it
	if ((lo == 0x3b) && (hi == 0x10) && (nr[0x83] & 4)) {res |= 0xfe | NX.i2cScl; any = 1;}
	// the UARTs: 133B-163B, port_uart_io_en = NR 83 b4 (zxnext.vhd 2420, 2639)
	if ((lo == 0x3b) && (hi >= 0x13) && (hi <= 0x16) && (nr[0x83] & 0x10)) {
		res |= nx_uart_in(comp, hi & 3, nx_now(comp) - (8 >> NX.speed));
		any = 1;
	}
	if ((lo == 0x3b) && (hi == 0x11) && (nr[0x83] & 4)) {res |= 0xfe | (NX.i2cSda & ds_sda(comp->rtc)); any = 1;}
	if ((lo == 0xeb) && (nr[0x83] & 8)) {
		// the byte of the last transfer; the read starts the next one, sending FF (for
		// the DMA its transfers are begun by nx_dma_waits, the strobe being a level)
		long long e = nx_now(comp) - (8 >> NX.speed);	// the ray is at T1 + 3 T here
		res |= cpu ? nx_spi_miso(comp, e) : NX.spiMiso;
		if (cpu) nx_spi_xfer(comp, 0xff, e);
		any = 1;
	}
	if ((lo == 0xe3) && (nr[0x83] & 1)) {res |= NX.pe3 & 0xcf; any = 1;}
	nx_nmi_trap(comp, port, 0, 0);
	// the CTC: 183B-1F3B, A10-A8 the channel (zxnext.vhd 2690, 4076)
	if (((port & 0xf8ff) == 0x183b) && (nr[0x85] & 8)) {
		res |= nx_ctc_in(comp, (port >> 8) & 7, nx_now(comp) - (8 >> NX.speed));
		any = 1;
	}
	{
		int mf = nx_mf_in(comp, port);
		if (mf >= 0) {res |= mf; any = 1;}
	}
	// the floating bus: the cpu takes the byte on the falling clock edge in T3, which
	// starts where the ray stands (at 28 MHz half a CLK_28 in: a whole one for the
	// edges nxv_float counts)
	if (lo == 0xff) {
		if ((nr[0x08] & 4) && (nr[0x82] & 1)) {
			res |= NX.pff;
		} else if ((NX.timing == NXT_48) || (NX.timing == NXT_128)) {
			int fb = nxv_float(comp, nx_now(comp) + (((8 >> NX.speed) + 1) >> 1));
			res |= (fb < 0) ? 0xff : fb;
		} else {
			res |= 0xff;
		}
		any = 1;
	}
	// the +3 floating bus port: A15-A12 0, A1-A0 01 (zxnext.vhd 2589, 4513-4517)
	if (((port & 0xf003) == 0x0001) && (NX.timing == NXT_P3) && (nr[0x82] & 0x10)) {
		if (nx_7ffd_locked(comp)) {
			res |= 0xff;
		} else {
			int fb = nxv_float(comp, nx_now(comp) + (((8 >> NX.speed) + 1) >> 1));
			res |= (fb < 0) ? NX.p3fb : (fb | 1);
		}
		any = 1;
	}
	if ((lo == 0x3b) && (hi == 0xff) && (nr[0x85] & 1)) {
		if (!NX.ulapMode) {
			unsigned short c = NX.pal[(nr[0x43] & 0x40) ? 4 : 0][0xc0 | (NX.ulapSel & 0x3f)];
			res |= (((c >> 3) & 7) << 5) | (((c >> 6) & 7) << 2) | ((c >> 1) & 3);	// GGGRRRBB
		} else {
			res |= (nr[0x68] >> 3) & 1;
		}
		any = 1;
	}
	return any ? res : 0xff;
}

static void nx_out_dev(Computer* comp, int port, int val, long long w, int cpu);

static void nx_out(Computer* comp, int port, int val) {
	int h, v;
	// an M-cycle boundary: the DMA may take the bus first
	if ((NX.dma.seq || NX.dma.busrq || NX.dma.busak) && nx_dma_cycle(comp))
		comp_vid_to_lazy(comp, comp->cpu->t + 1);
	// iowr walked the ray to T1 + 1: the T2 start, where the write strobe goes out
	nx_relock(comp, 2);
	nx_raw(comp, &h, &v);
	nx_back(comp, &h, &v, 2);
	int hold[4];
	nx_io_holds(comp, port, h, v, nx_now(comp) - 8, hold);
	if (hold[0]) {
		comp->cpu->t += hold[0];
		comp_vid_to_lazy(comp, comp->cpu->t + 1);
	}
	NX.holds += hold[0];
	vid_settle(comp->vid);
	long long w = nx_now(comp) + 1;		// iowr is taken on the CLK_28 edge after T2 starts, a nextreg write one later
	if (next_log_out) next_log_out(comp, port, val);
	nx_out_dev(comp, port, val, w, 1);
	comp->cpu->t += hold[1] + hold[2] + hold[3];
	NX.holds += hold[1] + hold[2] + hold[3];
}

// What an i/o write reaches, iowr taken on CLK_28 edge w; cpu: the cpu's cycle, not
// the DMA's
static void nx_out_dev(Computer* comp, int port, int val, long long w, int cpu) {
	NX.irqDead = 0;
	unsigned char* nr = NX.nr;
	int lo = port & 0xff;
	int hi = (port >> 8) & 0xff;
	int pfd = ((port & 3) == 1);
	int conflict = ((lo == 0xf1) || (lo == 0xf9)) && (nr[0x84] & 4);
	if (!(port & 1)) {
		NX.pfe = val;
		xOutFE(comp, port, val);
		nx_snd_fe(comp, val);
	}
	nx_snd_out(comp, port, val, w);
	if (lo == 0xff && (nr[0x82] & 1)) {
		NX.pff = val;
		nx_map_mem(comp);
	}
	// 7FFD, DFFD, 1FFD, EFF7 (zxnext.vhd 2593-2604). A write to one repeats the remap
	// on every edge iowr is up, T2 to the falling edge in T3 (zxnext.vhd 3813, t80na.vhd
	// 140-152): port_memory_change_dly follows one edge behind.
	{
		int tau = 8 >> NX.speed;
		NX.memChg0 = w + 1;
		NX.memChg1 = w + 2 * tau + (tau >> 1);
	}
	int memChg = 0;
	int p3 = (NX.timing == NXT_P3);
	int is1ffd = pfd && ((port & 0xf000) == 0x1000) && (nr[0x82] & 8);
	if (pfd && !(port & 0x8000) && ((port & 0x4000) || !p3) && !is1ffd && (nr[0x82] & 2) && !conflict) {
		memChg |= !nx_7ffd_locked(comp);
		nx_out_7ffd(comp, val);
		if (memChg && nx_7ffd_locked(comp)) NX.memChg1 = NX.memChg0;	// locked from the next edge on (zxnext.vhd 3769, 3813)
	}
	if (pfd && ((port & 0xf000) == 0xd000) && (nr[0x82] & 4) && !conflict) {
		memChg |= !nx_7ffd_locked(comp);	// (nr_8f_mapping_mode_profi is tied 0 in 3.02.03)
		if (!nx_7ffd_locked(comp)) {
			NX.pdffd = val & 0x5f;
			nx_remap(comp, 1, 0);
		}
	}
	if (is1ffd && !conflict) {
		if (nr[0x81] & 8) NX.p1ffdMtr = (val >> 3) & 1;	// with the bus's FDC on, locked or not
		memChg |= !nx_7ffd_locked(comp);
		if (!nx_7ffd_locked(comp)) {
			int leaving = (NX.p1ffd & 1) && !(val & 1);
			NX.p1ffd = val & 7;
			nx_remap(comp, 1, leaving);
		}
	}
	if (((port & 0xf0ff) == 0xe0f7) && (nr[0x85] & 4)) {
		memChg = 1;
		NX.peff7 = val & 0x0c;
		nx_remap(comp, 1, 0);
	}
	if (!memChg) NX.memChg1 = NX.memChg0 - 1;	// no window
	if (lo == 0x3b) {
		if (hi == 0x24) NX.nrsel = val;
		if (hi == 0x25) nx_nr_req(comp, NX.nrsel, val, 1, w + 1);
		if ((hi >= 0x13) && (hi <= 0x16) && (nr[0x83] & 0x10))
			nx_uart_out(comp, hi & 3, val, w);
		if (((hi == 0x10) || (hi == 0x11)) && (nr[0x83] & 4)) {
			if (hi == 0x10) NX.i2cScl = val & 1;
			else NX.i2cSda = val & 1;
			ds_bus(comp->rtc, NX.i2cScl, NX.i2cSda);
		}
		if ((hi == 0x12) && (nr[0x83] & 0x80)) {	// port_layer2_io_en = NR 83 b7 (zxnext.vhd 2424)
			if (val & 0x10)
				NX.l2ofs = val & 7;
			else
				NX.p123b = val & 0xcf;
		}
	}
	if ((lo == 0xe3) && (nr[0x83] & 1))
		NX.pe3 = (val & 0x8f) | (NX.pe3 & 0x40) | (val & 0x40);
	if ((lo == 0xe7) && (nr[0x83] & 8)) {
		// one slave at a time; esxDOS may write anything above b1 (zxnext.vhd 3306-3326)
		int sw = (nr[0x0a] >> 5) & 1;
		if ((val & 3) == 2) NX.pe7 = 0xfc | (sw ? 1 : 2);
		else if ((val & 3) == 1) NX.pe7 = 0xfc | (sw ? 2 : 1);
		else if ((val == 0xfb) || (val == 0xf7)) NX.pe7 = val;
		else if ((val == 0x7f) && (NX.configMode || (NX.resetType & 4))) NX.pe7 = val;
		else NX.pe7 = 0xff;
	}
	if ((lo == 0xeb) && (nr[0x83] & 8)) {
		if (cpu)
			nx_spi_xfer(comp, val, w - 1 + (8 >> NX.speed));	// w is a CLK_28 past T2's start
		else
			NX.spiDma = val;		// nx_dma_waits begins its transfers
	}
	if (((port & 0xf8ff) == 0x183b) && (nr[0x85] & 8))
		nx_ctc_out(comp, (port >> 8) & 7, val, w);
	nx_mf_out(comp, port);
	if (!conflict) nx_nmi_trap(comp, port, 1, val);
	// sprites: 0x57 and 0x5B by the low byte, 0x303B whole (zxnext.vhd 2679-2681)
	if (((lo == 0x57) || (lo == 0x5b) || ((lo == 0x3b) && (hi == 0x30))) && (nr[0x83] & 0x40)) {
		nx_cop_run(comp, w + 1);	// a copper mirror write on the same RAM edge goes first
		nxs_out(comp, port, val, w);
	}
	// ULA+: BF3B picks the mode group and the index, FF3B writes in group 00 are an NR FF
	// write with the bits turned from GGGRRRBB to RRRGGGBB, in group 01 the enable
	if ((lo == 0x3b) && (hi == 0xbf) && (nr[0x85] & 1)) {
		NX.ulapMode = (val >> 6) & 3;
		if (!NX.ulapMode) NX.ulapSel = val & 0x3f;
	}
	if ((lo == 0x3b) && (hi == 0xff) && (nr[0x85] & 1)) {
		if (NX.ulapMode == 0)
			nx_nr_req(comp, 0xff, (((val >> 2) & 7) << 5) | (((val >> 5) & 7) << 2) | (val & 3), 1, w + 1);
		else if (NX.ulapMode == 1)
			nr[0x68] = (nr[0x68] & ~8) | ((val & 1) << 3);
	}
	// the zxnDMA's ports; the DMA's own writes to them are not taken (port_dma_wr)
	if (cpu && (((lo == 0x6b) && (nr[0x82] & 0x20)) || ((lo == 0x0b) && (nr[0x85] & 2))))
		nx_dma_port_wr(comp, port, val);
	nxv_regs(comp, w);
}

// == the DMA's side of the bus: its cycles are the cpu's with M1 high (zxnext.vhd
// 1825-1850), so they reach what the cpu's reach, divMMC automap and the like apart ==

// the read at the end of the last READ_4, cpu->t just past it
int nx_dma_bus_rd(Computer* comp, int adr, int io) {
	if (comp->cpu->t - 1 > res4) comp_vid_to_lazy(comp, comp->cpu->t - 1);
	if (io) {
		int lo = adr & 0xff;
		if ((lo == 0x6b) || (lo == 0x0b)) return 0xff;	// its own ports: nothing answers
		return nx_in_dev(comp, adr, 0) & 0xff;
	}
	nx_dmaBus = 1;
	int v = nx_mrd_in(comp, adr);
	nx_dmaBus = 0;
	return v & 0xff;
}

// The write: its strobe falls in WRITE_1, which starts at cpu->t (edge e); a DMA i/o
// write is logged as the bench logs the cpu's, at the falling edge and edge count e - 1
void nx_dma_bus_wr(Computer* comp, int adr, int io, int val, long long e) {
	if (comp->cpu->t > res4) comp_vid_to_t(comp);
	vid_settle(comp->vid);
	int tau = 8 >> NX.speed;
	if (io) {
		long long w = nx_now(comp) + (tau >> 1) + 1;
		next_log_t = w - 1;
		next_log_cyc = e - 1;
		if (next_log_out) next_log_out(comp, adr, val);
		nx_out_dev(comp, adr, val, w, 0);
		next_log_t = -1;
	} else {
		nx_mem_wr(comp, adr, val, nx_now(comp) + (tau >> 1) + ((tau > 1) ? 1 : 0));
	}
}

// The clocks READ_4 / WRITE_4 repeat, it starting at cpu->t: WAIT is sampled on the
// falling edge inside it. At 28 MHz a memory read waits one clock in the second clock
// of its phase, which only a 2-clock read samples; +3 contention is WAIT too.
// The SPI master under the DMA, edge by edge (spi_master.vhd 82-97, 177): an edge with
// the EB strobe up begins a transfer when the master is idle or in its last clock, and
// spi_wait_n (idle or state_last_d) holds every READ_4/WRITE_4 of the DMA, on any port
// (zxnext.vhd 1844). The strobe is a level up from X1 to the clock WAIT lets go, so a
// write held for its transfer begins it again in that transfer's last clock. g: the edge
// READ_4/WRITE_4 starts on. The repeats of READ_4/WRITE_4 it costs.
static int nx_dma_spi(Computer* comp, int adr, int io, int rd, int len, long long g) {
	int eb = io && ((adr & 0xff) == 0xeb) && (NX.nr[0x83] & 8);
	if (!eb && (NX.spiT < 0)) return 0;
	if (comp->cpu->t > res4) comp_vid_to_t(comp);
	int tau = 8 >> NX.speed;
	long long now = nx_now(comp);			// edge g
	long long b = (NX.spiT < 0) ? g - 64 : g - (now - NX.spiT + tau / 2) / tau;
	if (!eb && (g - b > 16)) return 0;
	for (long long e = g - (len - 2); ; e++) {
		long long bp = b;
		if (eb && (e - b >= 16)) {
			b = e;
			NX.spiOld = NX.spiMiso;
			NX.spiMiso = nx_spi_byte(comp, rd ? 0xff : NX.spiDma);
			NX.spiT = now + (e - g) * tau;
		}
		if (e < g) continue;
		if ((e - b >= 16) || (e - 1 - bp == 15)) return (int)(e - g);
		if (e - g > 64) return (int)(e - g);
	}
}

// the memory side: the 28 MHz read wait and the +3 WAIT
static int nx_dma_waits_mem(Computer* comp, int adr, int rd, int len) {
	nxAccess a = nx_decode(comp, adr, !rd);
	if (NX.speed == 3) {
		if (!rd || (len != 2)) return 0;
		if (nx_sram_cycle(a.kind)) return 1;
		return (a.kind == NXM_BRAM) && (a.pg != 0x0e);
	}
	if (!nx_cont_on(comp) || (NX.timing != NXT_P3)) return 0;
	if (!nx_page_contended(comp, NX.mmu[(adr >> 13) & 7])) return 0;
	// wait_s from i_hc (zxula.vhd 583), taken on the falling edge, which is where hc
	// steps: the hc of the clock's first half, and again a clock later while waiting
	int h, v, hc, vc;
	if (comp->cpu->t > res4) comp_vid_to_t(comp);
	nx_raw(comp, &h, &v);
	nx_ula(comp, h, v, &hc, &vc, NULL);
	int w = 0;
	while ((hc < 256) && (vc < 192) && ((hc & 15) != 1) && ((hc & 15) != 2)) {
		w++;
		hc += 2;
	}
	return w;
}

// WAIT for READ_4/WRITE_4 is the AND of the SPI master's and the memory's (zxnext.vhd 1844)
int nx_dma_waits(Computer* comp, int adr, int io, int rd, int len, long long g) {
	int sw = nx_dma_spi(comp, adr, io, rd, len, g);
	int mw = io ? 0 : nx_dma_waits_mem(comp, adr, rd, len);
	return (sw > mw) ? sw : mw;
}

// 48K/128K: a clock that starts with MREQ high and a contended address on the bus is
// held like a cpu T1 (zxula.vhd 587-595); cpu->t at its start
void nx_dma_stretch(Computer* comp, int adr) {
	if (!nx_cont_on(comp) || (NX.timing == NXT_P3)) return;
	if (!nx_page_contended(comp, NX.mmu[(adr >> 13) & 7])) return;
	int h, v, hc, vc;
	if (comp->cpu->t > res4) comp_vid_to_t(comp);
	nx_raw(comp, &h, &v);
	nx_ula(comp, h, v, &hc, &vc, NULL);
	if ((hc < 256) && (vc < 192)) {
		int d = nx_hold[hc & 15];
		nx_hold_note(comp, nx_now(comp), d);
		comp->cpu->t += d;
		NX.holds += d;
	}
}

// == reset ==

static void nx_regs_reset(Computer* comp) {
	unsigned char* nr = NX.nr;
	// zxnext.vhd reset branches (nextreg.diff.md 1.5)
	nr[0x02] &= 0x80;
	nr[0x03] &= 0x7f;
	nr[0x06] = (nr[0x06] & 0x5f) | 0xa0;
	nr[0x07] = 0;
	nr[0x08] &= 0x3f;
	nr[0x09] &= 0xef;
	nr[0x0b] = 0x01;
	nr[0x12] = 0x08;
	nr[0x13] = 0x0b;
	nr[0x14] = 0xe3;
	nr[0x15] = 0; nr[0x16] = 0; nr[0x17] = 0;
	memset(NX.clipIdx, 0, sizeof(NX.clipIdx));
	for (int i = 0; i < 3; i++) {
		NX.clip[i][0] = 0x00; NX.clip[i][1] = 0xff; NX.clip[i][2] = 0x00; NX.clip[i][3] = 0xbf;
	}
	NX.clip[3][0] = 0x00; NX.clip[3][1] = 0x9f; NX.clip[3][2] = 0x00; NX.clip[3][3] = 0xff;
	nr[0x22] = 0; nr[0x23] = 0;
	nr[0x26] = 0; nr[0x27] = 0;
	NX.pal44 = 0;
	nr[0x2f] = 0;
	nr[0x30] = 0; nr[0x31] = 0; nr[0x32] = 0; nr[0x33] = 0; nr[0x34] = 0;
	nr[0x40] = 0;
	nr[0x42] = 0x07;
	nr[0x43] = 0;
	nr[0x4a] = 0xe3; nr[0x4b] = 0xe3; nr[0x4c] = 0x0f;
	static const unsigned char mmu0[8] = {0xff, 0xff, 0x0a, 0x0b, 0x04, 0x05, 0x00, 0x01};
	memcpy(NX.mmu, mmu0, 8);
	NX.coppAdr = 0;
	nr[0x62] = 0;
	NX.copPc = 0;
	NX.copDout = 0;
	NX.copN = 0;
	nxs_reset(comp);
	nr[0x64] = 0;
	nr[0x68] = 0;
	nr[0x6a] = 0; nr[0x6b] = 0; nr[0x6c] = 0;
	nr[0x6e] = 0x2c; nr[0x6f] = 0x0c;
	nr[0x70] = 0; nr[0x71] = 0;
	nr[0x80] = (nr[0x80] & 0x0f) | ((nr[0x80] & 0x0f) << 4);
	if (nr[0x85] & 0x80) {
		nr[0x82] = nr[0x83] = nr[0x84] = 0xff;
		nr[0x85] |= 0x0f;
	}
	if (!(nr[0x89] & 0x80)) {
		nr[0x86] = nr[0x87] = nr[0x88] = 0xff;
		nr[0x89] |= 0x0f;
	}
	nr[0x8c] = (nr[0x8c] & 0x0f) | ((nr[0x8c] & 0x0f) << 4);
	nr[0x90] = 0; nr[0x91] = 0; nr[0x92] = 0; nr[0x93] = 0;
	nr[0xa0] = 0; nr[0xa2] = 0; nr[0xa8] = 0; nr[0xa9] = 1;
	nr[0xb8] = 0x83; nr[0xb9] = 0x01; nr[0xba] = 0x00; nr[0xbb] = 0xcd;
	nr[0xc0] = 0;
	nr[0xc2] = 0; nr[0xc3] = 0;
	nr[0xc4] = 0x81;
	nr[0xc5] = 0; nr[0xc6] = 0; nr[0xcc] = 0; nr[0xcd] = 0; nr[0xce] = 0;
	nr[0xd8] = 0; nr[0xd9] = 0;
	NX.nrsel = 0x24;
	NX.p7ffd = 0; NX.pdffd = 0; NX.p1ffd = 0; NX.peff7 = 0;
	NX.p1ffdMtr = 0;
	NX.p123b = 0; NX.l2ofs = 0;
	NX.pe3 = 0;			// mapram too (zxnext.vhd 4176-4177)
	NX.dmHeld = NX.dmNext = NX.dmMap = NX.dmButton = 0;
	NX.pff = 0;
	NX.pe7 = 0xff;			// (the SPI master itself has no reset, zxnext.vhd 3285)
	NX.ulapSel = 0; NX.ulapMode = 0;
	NX.palPhase = 0;
	NX.speed = 0;
	NX.speedPend = 0;
	NX.contDis = 0;
	nx_int_reset(comp);
	NX.pulseE = -1;
	NX.lineChk = 0;
	NX.expbusEff = 0;
	nx_dma_reset(comp);
}

// the configuration of the FPGA: declaration values (zxnext.vhd), counters at
// raw (0, 0) with hc_ula 256. Everything in it starts over, its CLK_28 clock with it, as
// at the first start: a time kept from before - the sprite pass's, the copper's, the last
// write's - would lie far ahead of the new clock and stall what waits for it. The SRAM
// behind the block RAM pages keeps what it holds, as the rest of the SRAM does.
static void nx_power(Computer* comp) {
	unsigned char* nr = NX.nr;
	static unsigned char sram[sizeof(NX.shadow)];
	int vm = NX.vmode;
	memcpy(sram, NX.shadow, sizeof(sram));
	memset(&NX, 0, sizeof(NX));
	memcpy(NX.shadow, sram, sizeof(sram));
	if (vm) comp_update_timings(comp);	// nr_11's declaration value: VGA 0, until the firmware says
	nx_w = 0;
	nx_rd_t = 0;
	nx_uart_power(comp);
	nr[0x03] = 0x33;		// +3 timing, machine type +3
	nr[0x05] = 0x41;
	nr[0x08] = 0x10;
	nr[0x86] = nr[0x87] = nr[0x88] = 0xff;
	nr[0x0a] = 0x01;
	nr[0x10] = 0x04;
	nr[0x7f] = 0xff;
	nr[0x85] = 0x8f;
	nr[0x89] = 0x8f;
	NX.resetType = 4;
	NX.configMode = 0;		// the firmware leaves config mode off
	NX.pe3 = 0;
	NX.timing = NXT_P3;
	NX.hz60 = 0;
	NX.eff05 = 0;
	NX.eff09 = 0;
	NX.holds = 0;
	NX.powerFrame = 1;
	NX.vsyncCount = 0;
	NX.frameBase = 0;
	NX.cuOffset = 0;
	NX.vcLine0 = 0;
	NX.cvcLine0 = 0;
	NX.lastSram = 0;
	NX.lane[0] = NX.lane[1] = 0;
	NX.laneT[0] = NX.laneT[1] = comp->tickTotal;
	NX.spiMiso = NX.spiOld = 0;	// miso_dat's declaration value
	NX.spiT = -1;
	nx_inp_power(comp);
	memset(NX.pal, 0, sizeof(NX.pal));
	memset(NX.coppMem, 0, sizeof(NX.coppMem));
	NX.powered = 1;
	nx_regs_reset(comp);
	nx_layout(comp);
	// the ray at raw (0, 0); the reset holds the cpu for 16 dots, the T80 leaves it
	// on its first clock edge and fetches from the second (hc_ula 275)
	comp->vid->ray.x = comp->vid->vend.x;
	comp->vid->ray.y = comp->vid->vend.y - 1;
	comp->vid->ray.xb = 0;
	comp->vid->ray.yb = 0;
	vid_sync_fixed(comp->vid, 19 * comp->vid->nsPerDotFixed);
}

// NR 02 b1 or the F1 key: the FPGA reloads the core - power-on and the firmware
// again; the memory is kept
static void nx_hard_reset(Computer* comp) {
	NX.powered = 0;
	NX.firmware = 1;
	nx_reset(comp);
	compSetHwTurbo(comp, 1.0);	// 3.5 MHz, on power-on's grid (nx_relock)
	cpu_reset(comp->cpu);
}

// nextreg 02's reset type: a soft reset moves its hard bit down
static void nx_reset_type_soft(Computer* comp) {
	NX.resetType = (NX.resetType >> 2 & 1) << 1 | ((NX.resetType & 3) ? 1 : 0);
}

// the parts of the board a reset reaches besides the registers: the I2C lines let go
// (zxnext.vhd 3231-3252), which the clock chip sees, and the UARTs
static void nx_reset_lines(Computer* comp) {
	NX.i2cScl = NX.i2cSda = 1;
	nx_uart_reset(comp);
	ds_bus(comp->rtc, 1, 1);
}

// The soft reset inside a step (nr_02_soft_reset, zxnext.vhd 6370): NR 02 b0 on its
// edge w (nx_nr_land), F4 where the ray stands (-1). The raster stays.
static void nx_soft_reset(Computer* comp, long long w) {
	nx_reset_type_soft(comp);
	if (next_reset_open) return;
	long long e = (w >= 0) ? w : nx_now(comp);
	long long up = nxv_before_reset(comp, w);
	int sp = NX.speed;
	nx_regs_reset(comp);
	if (sp) {
		// 3.5 MHz again (5785-5790) through an NR 07 write's phase step (nx_speed_apply)
		NX.speed = sp;
		NX.speedPend = 1;
		NX.speedT = e - 2;
	}
	nx_reset_lines(comp);
	nx_map_mem(comp);
	nxv_reset(comp);
	nxv_after_reset(comp, up);	// drawn on from there, as the dot after the reset did
	nx_snd_reset(comp, e);
	cpu_reset(comp->cpu);
}

// A board key made, F1..F10 as 0..9, as the core's hotkeys take it (zxnext.vhd
// 2090-2091, 5790-5845, 6335-6350); F2, F5-F7 (video output, expansion bus) do
// nothing here. A change joins an RZX recording; a played one ignores the keys.
void nx_board_key(Computer* comp, int n) {
	unsigned char* nr = NX.nr;
	if (comp->rzx.play) return;
	switch (n) {
		case 0:			// hard reset
			if (next_reset_open) return;
			nx_hard_reset(comp);
			if (next_board_reset) next_board_reset(comp);	// the user's, as Machine > Reset
			break;
		case 2: {		// 50/60 Hz (none on the Pentagon timing)
			if (!(nr[0x06] & 0x20)) return;
			int old = nr[0x05];
			nx_nr_write(comp, 0x05, nr[0x05] ^ 4);
			if (nr[0x05] == old) return;
			break;
		}
		case 3:			// soft reset
			if (NX.configMode || next_reset_open) return;
			nx_soft_reset(comp, -1);
			if (next_board_reset) next_board_reset(comp);
			break;
		case 7:			// the next CPU speed
			if (!(nr[0x06] & 0x80)) return;
			nx_nr_write(comp, 0x07, (nr[0x07] + 1) & 3);
			break;
		case 8:			// the NMI button: the Multiface's NMI
			nx_nmi_raise(comp, 1, 0);
			return;
		case 9:			// DRIVE: the divMMC's NMI, with its ports on
			if (nr[0x83] & 1) nx_nmi_raise(comp, 0, 1);
			return;
		default:
			return;
	}
	rzx_rec_touch();
}

// A ROM as load_roms reads it: from /machines/next/ on the SD card. A card that
// has not got it leaves the machine's own ROM banks, which hold the same layout.
static int nx_card_sector(void* ctx, unsigned int lba, unsigned char* buf) {
	return sdc_read_lba((SDCard*)ctx, lba, buf);
}

// A field of a "menu=" line (config.c parsestring): up to the next comma
static const char* nx_menu_field(const char* s, char* out, int max) {
	int n = 0;
	while (*s && (*s != ',') && (*s != '\r') && (*s != '\n')) {
		if (n < max - 1) out[n++] = *s;
		s++;
	}
	out[n] = 0;
	return (*s == ',') ? s + 1 : s;
}

// a "name=value" line of config.ini (config.c load_config: the name from the line's
// start); -1 if there is none
static int nx_config_value(const char* buf, const char* name) {
	size_t len = strlen(name);
	for (const char* l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
		if (!strncmp(l, name, len) && (l[len] == '=')) return atoi(l + len + 1);
	}
	return -1;
}

// The menu entry the firmware boots (config.c): "default=" of config.ini, in menu.ini
// or else menu.def. Its ROM names replace the standard ones when it is a Next entry
// (mode 2) - the hand-off below is that mode's; another mode keeps them, said in the log.
// *vt: the video timing (update_video_settings): config.ini's "timing=", 8 (none, 0)
// if absent, unless the entry's own is under 8
static void nx_menu_roms(Computer* comp, char* zx, char* div, char* mf, int* vt) {
	static unsigned char buf[8192];
	int def = 0;
	*vt = 8;
	int n = fat_read_file(nx_card_sector, comp->sdc, "machines/next/config.ini", buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[(n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1] = 0;
		const char* d = strstr((char*)buf, "default=");
		if (d) def = atoi(d + 8);
		int t = nx_config_value((char*)buf, "timing");
		if (t >= 0) *vt = (t > 8) ? 8 : t;		// CLAMP(.., MAX_TIMING)
	}
	n = fat_read_file(nx_card_sector, comp->sdc, "machines/next/menu.ini", buf, sizeof(buf) - 1);
	if (n <= 0) n = fat_read_file(nx_card_sector, comp->sdc, "machines/next/menu.def", buf, sizeof(buf) - 1);
	if (n <= 0) return;
	buf[(n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1] = 0;
	char title[64], mode[8], tim[8], f1[32], f2[32], f3[32], pick[3][32];
	int count = 0, got = 0, pmode = -1, ptim = 8;
	for (const char* l = (char*)buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
		if (strncmp(l, "menu=", 5)) continue;
		const char* s = nx_menu_field(l + 5, title, sizeof(title));
		s = nx_menu_field(s, mode, sizeof(mode));
		s = nx_menu_field(s, tim, sizeof(tim));
		s = nx_menu_field(s, f1, sizeof(f1));
		s = nx_menu_field(s, f2, sizeof(f2));
		nx_menu_field(s, f3, sizeof(f3));
		if (!f1[0]) continue;
		if ((count <= def) || !got) {			// the default, else the last one (clamped)
			snprintf(pick[0], 32, "%s", f1);
			snprintf(pick[1], 32, "%s", f2);
			snprintf(pick[2], 32, "%s", f3);
			pmode = atoi(mode);
			ptim = atoi(tim);
			got = (count == def);
		}
		count++;
	}
	if (pmode < 0) return;
	if (ptim < 8) *vt = ptim;		// the entry's own timing overrides config.ini's
	if (pmode != 2) {
		xlog(XLG_HW, XLL_WARN, "zxnext: the card's menu boots mode %d, the Next's is used", pmode);
		return;
	}
	snprintf(zx, 32, "%s", pick[0]);
	if (pick[1][0]) snprintf(div, 32, "%s", pick[1]);
	if (pick[2][0]) snprintf(mf, 32, "%s", pick[2]);
}

static void nx_load_rom(Computer* comp, const char* name, unsigned char* dst, int len, int bank) {
	char path[64];
	snprintf(path, sizeof(path), "machines/next/%s", name);
	if (comp->sdc->image && (fat_read_file(nx_card_sector, comp->sdc, path, dst, len) >= len)) return;
	xlog(XLG_HW, XLL_WARN, "zxnext: no %s on the SD card, the ROM set's is used", name);
	comp->msg = (char*)" no System/Next on the SD card ";
	memcpy(dst, comp->mem->romData + bank, len);
}

// What TBBLUE.FW 1.44 does before it hands over (boot.c main, load_roms and
// init_registers, config.c reset_settings and update_video_settings), with the
// CONFIG.INI it ships and its first menu line: ZX Spectrum Next, mode 2, video
// timing not overridden, enNextZX.rom, enNxtmmc.rom and enNextMF.rom - the last
// two turn divMMC and the Multiface on. The card's own config.ini and menu say which
// entry and video timing (nx_menu_roms).
static void nx_firmware(Computer* comp) {
	static const unsigned char wr[][2] = {
		{0x07, 0x03}, {0x06, 0xa0},	// main: 28 MHz, PS/2 keyboard first
		{0x03, 0x00},			// disable_bootrom
		{0x02, 0x80},			// reset_settings: the ESP held in reset...
		{0x05, 0xfb},			// ...and the joysticks on keyjoys (nr_05 | FA)
		{0x11, 0x80}, {0x05, 0xfb},	// set_video_mode(0, 50 Hz, scandoubler)
		{0x09, 0x00},			// no scanlines
		{0x04, 0x03},			// load_roms: the last ROM bank it wrote through NR 04
		{0x05, 0x71},			// init_registers: Kempston 1, Sinclair 1, scandoubler
		{0x06, 0x9d},			// F8 key, divMMC and Multiface NMI, PS/2 mouse first, AY
		{0x08, 0x1e},			// internal speaker, DACs, Timex modes, TurboSound
		{0x09, 0x00},			// HDMI sound on
		{0x0a, 0x11},			// divMMC automap, Multiface +3, mouse dpi 1
		{0x82, 0xda}, {0x83, 0x3f}, {0x84, 0xff}, {0x85, 0x01},	// port enables, +3
		{0x03, 0xb3},			// machine type +3, +3 timing: config mode off
		{0x02, 0x01}			// soft reset
	};
	// keyjoy_default: the user-defined key joysticks, R L D U fire... = P O A Q SPACE M ENTER S Y Z X
	static const unsigned char keyjoy[11] = {0x28, 0x29, 0x08, 0x10, 0x38, 0x3a, 0x30, 0x09, 0x2c, 0x01, 0x02};
	unsigned char* sram = comp->mem->ramData;
	NX.configMode = 1;		// the boot ROM runs it in config mode
	NX.nr[0x03] &= 0xf8;
	// load_keymap: keymap.bin, a byte pair an entry of which NR 2A takes the first (the
	// core has no NR 2A); load_keyjoys: both joysticks' first eleven user entries
	unsigned char km[1024];
	if (comp->sdc->image && (fat_read_file(nx_card_sector, comp->sdc, "machines/next/keymap.bin", km, 1024) >= 1024)) {
		nx_nr_write(comp, 0x28, 0x00);
		nx_nr_write(comp, 0x29, 0x00);
		for (int i = 0; i < 512; i++)
			nx_nr_write(comp, 0x2b, km[2 * i + 1]);
	}
	for (int j = 0; j < 2; j++) {
		nx_nr_write(comp, 0x28, 0x80);
		nx_nr_write(comp, 0x29, j << 4);
		for (int k = 0; k < 11; k++)
			nx_nr_write(comp, 0x2b, keyjoy[k]);
	}
	// load_config: the menu entry and the video timing
	char zx[32] = "enNextZX.rom", div[32] = "enNxtmmc.rom", mf[32] = "enNextMF.rom";
	int vt = 8;
	if (comp->sdc->image) nx_menu_roms(comp, zx, div, mf, &vt);
	nxInFirmware = 1;
	for (int i = 0; i < (int)(sizeof(wr) / sizeof(wr[0])); i++) {
		if ((wr[i][0] == 0x02) && (wr[i][1] & 1)) {
			// load_roms, which ran before init_registers: nothing between reads them
			nx_load_rom(comp, div, sram + NX_DIVROM, 0x2000, 0x10000);
			nx_load_rom(comp, mf, sram + NX_MFROM, 0x2000, 0x14000);
			nx_load_rom(comp, zx, sram, 0x10000, 0);
		}
		if (wr[i][0] == 0x11)		// set_video_mode: (timing & 7) | 0x80
			nx_nr_write(comp, 0x11, (vt & 7) | 0x80);
		else
			nx_nr_write(comp, wr[i][0], wr[i][1]);
	}
	nxInFirmware = 0;
}

// The power comes on: the reset that follows configures the FPGA and then hands
// over as the firmware does
static void nx_power_on(Computer* comp) {
	NX.powered = 0;
	NX.firmware = 1;
	memset(NX.shadow, 0, sizeof(NX.shadow));	// the SRAM behind the BRAM pages, cleared with the rest
	next_ram_size(comp, comp->mem->ramSize);	// the half with no chips, as layer 2 shows it
}

// The SRAM fitted: 1 MB (an issue 2 board without its expansion) or 2 MB. The image
// past it is never written; layer 2 over it shows it as #FF - a steady picture, not the
// floating bus (that would depend on when the picture is drawn). The map is the
// caller's: a reset or a snapshot's paging builds it after this
void next_ram_size(Computer* comp, int size) {
	memSetSize(comp->mem, size, -1);
	size = comp->mem->ramSize;
	if (size < MEM_2M) memset(comp->mem->ramData + size, 0xff, MEM_2M - size);
}

// 8K RAM pages the cpu can reach: 0x60 on 1 MB, 0xE0 on 2 MB
int next_ram_pages(Computer* comp) {
	int n = (comp->mem->ramSize - NX_RAM) >> 13;
	return (n < 0) ? 0 : (n > 0xe0) ? 0xe0 : n;
}

static void nx_reset(Computer* comp) {
	if (comp->cpu->type != CPU_Z80N)
		cpu_set_type(comp->cpu, "Z80N");
	comp->cpu->xnextreg = nx_xnextreg;
	comp->cpu->xinta = nx_inta;
	int fw = !NX.powered && NX.firmware;
	NX.firmware = 0;
	long long up = -1;
	if (!NX.powered) {
		nx_power(comp);
	} else {
		// the reset button: the soft reset of nx_soft_reset, between steps (compReset
		// sets the clock), with the machine set up again around it
		nx_reset_type_soft(comp);
		up = nxv_before_reset(comp, -1);	// the raster stays: it changes at a frame start only
		nx_regs_reset(comp);
		nx_layout(comp);
	}
	nx_reset_lines(comp);
	comp->vid->inten |= 1;		// the ULA interrupt pixel is reported (nx_irq decides)
	zx_set_pal(comp);
	vid_set_mode(comp->vid, VID_NEXT);
	comp_set_cont(comp, 0);		// the machine contends by itself (nx_cont)
	nx_map_mem(comp);
	nxv_reset(comp);
	nxv_after_reset(comp, up);	// the button: drawn on from where the reset found the ray
	nx_snd_reset(comp, -1);
	if (fw) comp->flgBOOT = 1;	// nx_firmware, once the card is in
}

// CLK_28 of each video timing (nextreg.txt 0x11): the CPU, the raster and everything
// else the core clocks off it run that much faster than at 28 MHz
static const double nxClk28[7] = {28000000.0, 28571429.0, 29464286.0, 30000000.0, 31000000.0, 32000000.0, 33000000.0};

double nx_clk_scale(Computer* comp) {
	int m = NX.vmode;
	return (m > 0) && (m < 7) ? nxClk28[0] / nxClk28[m] : 1.0;
}

// NR 11 as the issue 2 core takes it (zxnext.vhd 5208-5217, g_video_inc "10": no VGA
// module, HDMI only): 7 is 0, and of the rest only bit 0 - VGA 0 or VGA 1
static int nx_vmode_of(int v) {
	return ((v & 7) == 7) ? 0 : (v & 1);
}

// the board's clock moved: every period that comes off it again
static void nx_set_vmode(Computer* comp, int m) {
	if (NX.vmode == m) return;
	NX.vmode = m;
	comp_update_timings(comp);
	xlog(XLG_HW, XLL_INFO, "zxnext: video timing %d, CLK_28 %.0f Hz", m, nxClk28[m]);
}

static void nx_init(Computer* comp) {
	zx_init(comp);
	// a 28 MHz tick is a quarter of a dot: a dot period that is a multiple of 8
	// in 16.16 lets every speed's tick divide it exactly (0.6 ppm off the crystal).
	// CLK_28 of the video timing (nx_clk_scale) is the crystal here
	long long dot = llround(comp->nsPerTick / 2.0 * nx_clk_scale(comp) * 65536.0) & ~7LL;
	vid_upd_timings(comp->vid, (double)dot / 65536.0);
	// and the 3.5 MHz tick two of those dots, exactly: comp_update_timings divides it by
	// the speed and rounds it against the dot, which leaves it whole only from here
	comp->nsPerTick = 2.0 * (double)dot / 65536.0;
	// the board's 1 MB or 2 MB, as the machine file says; anything else (a bare
	// compSetHardware) is a 2 MB one. This runs on every clock change: it keeps the size
	if ((comp->mem->ramSize != MEM_1M) && (comp->mem->ramSize != MEM_2M))
		next_ram_size(comp, MEM_2M);
}

static void nx_sync(Computer* comp, int ns) {
	zx_devs_sync_in(comp, ns);
}

// == the HALT run-on ==

static int nx_halt_look(Computer*, int);
static int nxHaltV, nxHaltP3;	// the SRAM byte the fetch leaves (-1: none), the +3 bus byte
// what the look at the fetch found, for the run that goes on right where the last one
// ended - in real time the sound cuts it at every sample: no instruction since, and
// no copper move (the copper's moves step copPc) that could have remapped anything
static struct {
	long long tick;
	int adr, copPc, nr62, speed, t;
} nxHaltLast = {-1, 0, 0, 0, 0, 0};

// hwQuiet.halt: the M1s of a HALT at adr change nothing but the time while nx_cont,
// next_irq_ack and nx_mrd find nothing to do - no speed change, DMA, contention or
// owed wait, divMMC's automap and the NMI and opcode snoopers standing still. Each
// takes 4 T and the SRAM wait at 28 MHz; the last one's INT sample stays short of
// irqDead, its read short of the copper's next move (nx_mrd_in)
static int nx_halt(Computer* comp, int adr, long long* until) {
	if (NX.speedPend || NX.powerFrame || NX.dma.seq || NX.dma.busrq || NX.dma.busak) return 0;
	if ((NX.contDis != ((NX.nr[0x08] >> 6) & 1)) || comp->waitDebt || (NX.irqDead <= 0)) return 0;
	int t;
	if ((nxHaltLast.tick == comp->tickTotal) && (nxHaltLast.adr == adr) && (nxHaltLast.copPc == NX.copPc)
			&& (nxHaltLast.nr62 == NX.nr[0x62]) && (nxHaltLast.speed == NX.speed)) {
		t = nxHaltLast.t;
	} else {
		t = nx_halt_look(comp, adr);
		if (!t) return 0;
		nxHaltLast.adr = adr;
		nxHaltLast.copPc = NX.copPc;
		nxHaltLast.nr62 = NX.nr[0x62];
		nxHaltLast.speed = NX.speed;
		nxHaltLast.t = t;
	}
	long long u = NX.irqDead - t + 1;		// sampled a tick before the step's end
	if ((NX.nr[0x62] & 0xc0) || NX.copN) {
		if (NX.copN || (NX.refT < 0)) return 0;
		long long g = nx_tick28(comp);		// the read at the step start + 2 T
		long long c = NX.refT + nx_fdiv(NX.copWake - 4 - NX.refC + g - 1, g) - 2;
		if (c < u) u = c;
	}
	*until = u;
	return t;
}

// the fetch itself: the T of the M1 when a run of them stands still, else 0
static int nx_halt_look(Computer* comp, int adr) {
	if (nx_cont_on(comp) && nx_page_contended(comp, NX.mmu[(adr >> 13) & 7])) return 0;
	int held = NX.dmHeld, next = NX.dmNext, map = NX.dmMap, btn = NX.dmButton;
	if ((next != held) || (map != held)) return 0;
	int m = nx_dm_fetch(comp, adr);
	int still = (NX.dmHeld == held) && (NX.dmNext == held) && (m == held) && !(held && btn);
	NX.dmHeld = held;
	NX.dmNext = next;
	NX.dmMap = map;
	NX.dmButton = btn;
	if (!still) return 0;
	nxAccess a = nx_decode(comp, adr, 0);
	unsigned char* ram = comp->mem->ramData;
	int v, w = 0;
	nxHaltV = -1;
	switch (a.kind) {
		case NXM_SRAM: case NXM_ROM: v = ram[a.adr]; nxHaltV = v; w = 1; break;
		case NXM_SHADOW: v = NX.shadow[a.adr]; nxHaltV = v; w = 1; break;
		case NXM_BRAM: v = ram[a.adr]; w = (a.pg != 0x0e); break;
		case NXM_BUS: v = 0xff; break;
		case NXM_ABSENT: return 0;	// what the lines hold can move under it: stepped
		default: v = NX.lastSram; break;
	}
	nxHaltP3 = v;
	if (!nx_int_halt_ok(comp, adr, v)) return 0;
	return 4 + ((NX.speed == 3) ? w : 0);
}

// hwQuiet.haltDone: the M1 record of the last steps, the byte the read leaves
static void nx_halt_done(Computer* comp, int adr, long long t0, long long n, int t) {
	int len = (NX.speed == 3) ? 3 : 2;
	for (long long k = (n > 8) ? n - 8 : 0; k < n; k++)
		nx_int_m1(comp, t0 + k * t, len);
	if (nxHaltV >= 0) {
		NX.lastSram = nxHaltV;
		// the last fetch's half of the bus, read 2 T into its M1 as a stepped one is
		nx_lane_rd(comp, adr, nxHaltV, t0 + (n - 1) * t + 2);
	}
	nx_p3_byte(comp, adr, nxHaltP3);
	nx_int_halt_done(comp, nxHaltP3, n);		// the byte read, whatever answered
	nxHaltLast.tick = t0 + n * t;
}

static const hwQuiet nx_quiet = {HWQ_ACK_NEXT, NULL, NULL, zx_devs_sync, NULL, 0, nx_halt, nx_halt_done, nx_snd_late};

// An SZX has put xNextState back over a machine reset for it (filetypes/szx_ext.c):
// what is worked out of the state, again. The clock comes back after this.
void next_snap_loaded(Computer* comp, int vmode) {
	if (NX.vmode != vmode) comp_update_timings(comp);	// the board's CLK_28 (NR 11)
	if (NX.v.live == NXV_LIVE_ANEW) nxv_reset(comp);		// the picture's state, from the registers loaded
	long long idle = NX.spr.idleTo;
	nx_layout(comp);		// the raster of the timing the state runs
	NX.spr.idleTo = idle;		// worked out for that same raster
	nx_map_mem(comp);
	nxHaltLast.tick = -1;
	comp->flgBOOT = 0;		// a reset's, not the state's: NCLK says whether one is due
}

HardWare nxt_hw_core = {HW_ZXNEXT, "ZXNext", "ZX Spectrum Next", MEM_1M | MEM_2M, 1.0, NULL, NULL,
			nx_init, nx_map_mem, nx_out, nx_in, nx_mrd, nx_mwr, nx_irq, nx_ack, nx_reset, nx_sync,
			nx_keyp, nx_keyr, nx_vol, NULL, nx_cont, &nx_quiet, 1, nx_power_on, nx_firmware, 1, 1,
			nx_mem_view, nx_disasm, nx_snd_take, nx_mix, nx_int_vecs};

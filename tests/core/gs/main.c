// Headless General Sound bench: the classic card's mix, from its schematic, and
// the NeoGS from its FPGA sources - the mix (sound_main.v, sound_mulacc.v), the
// memory map (memmap.v) and the ports (ports.v).
//
//	gs-bench

#include <stdio.h>
#include <string.h>

#include "sound/gs.h"

int gsmemrd(int, int, void*);		// sound/gs.c: the card's CPU's own bus
void gsmemwr(int, int, void*);
int gsiord(int, void*);
void gsiowr(int, int, void*);

static int failures;

static void check(const char* name, int ok) {
	printf("%s: %s\n", name, ok ? "ok" : "FAIL");
	if (!ok) failures++;
}

static void set8(GSound* gs, const int* ch, const int* vol) {
	int* c[8] = {&gs->ch1, &gs->ch2, &gs->ch3, &gs->ch4, &gs->ch5, &gs->ch6, &gs->ch7, &gs->ch8};
	int* v[8] = {&gs->vol1, &gs->vol2, &gs->vol3, &gs->vol4, &gs->vol5, &gs->vol6, &gs->vol7, &gs->vol8};
	for (int i = 0; i < 8; i++) {
		*c[i] = ch[i];
		*v[i] = vol[i];
	}
}

static sndPair mix(GSound* gs, int stereo, const int* ch, const int* vol) {
	gs->stereo = stereo;
	set8(gs, ch, vol);
	return gsVolume(gs);
}

static void show(const char* name, sndPair p) {
	printf("%s: %d %d\n", name, p.left, p.right);
}

static const int mid[8] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
static const int top[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const int ch1top[8] = {0xff, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
static const int off[8] = {0};
static const int full[8] = {63, 63, 63, 63, 63, 63, 63, 63};
static const int ch1only[8] = {63, 0, 0, 0, 0, 0, 0, 0};

static void classic_mix(GSound* gs) {
	sndPair a, b;
	printf("== classic\n");
	a = mix(gs, GS_12_34, mid, off);
	b = mix(gs, GS_12_34, mid, full);
	show("mid, vol 0", a);
	show("mid, vol 63", b);
	check("a volume change on #80 moves the level", (b.left - a.left) == 0x80 * 63);
	a = mix(gs, GS_12_34, top, full);
	show("all #FF, vol 63", a);
	check("the peak is the uncrossed one", (a.left == 255 * 63) && (a.right == 255 * 63));
	a = mix(gs, GS_12_34, ch1top, ch1only);
	show("ch1 #FF alone", a);
	check("the right side has 0.47 of the left", (a.right * 100 / a.left) == 47);
	a = mix(gs, GS_MONO, ch1top, ch1only);
	show("mono, ch1 #FF alone", a);
	check("mono has no cross-mix", (a.left == a.right) && (a.left == 255 * 63 / 4));
}

static void neogs_mix(GSound* gs) {
	sndPair a, b;
	printf("== neogs-mix\n");
	gs->cfg0 = 0;
	a = mix(gs, GS_12_34, mid, off);
	b = mix(gs, GS_12_34, mid, full);
	show("mid, vol 0", a);
	show("mid, vol 63", b);
	check("a volume change on #80 moves nothing", (a.left == b.left) && (a.right == b.right) && !b.left);
	a = mix(gs, GS_12_34, ch1top, ch1only);
	show("4 channels, ch1 #FF alone", a);
	check("ch1 left only, counted twice", (a.left == 2 * 127 * 63 / 4) && !a.right);
	show("4 channels, all #FF", mix(gs, GS_12_34, top, full));
	a = mix(gs, GS_MONO, ch1top, ch1only);
	show("mono, ch1 #FF alone", a);
	// 8 channels: 5 and 6 left, 7 and 8 right, each once
	gs->cfg0 = NGS_8CHANS;
	static const int ch5top[8] = {0x80, 0x80, 0x80, 0x80, 0xff, 0x80, 0x80, 0x80};
	static const int ch7top[8] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xff, 0x80};
	a = mix(gs, GS_12_34, ch5top, full);
	b = mix(gs, GS_12_34, ch7top, full);
	show("8 channels, ch5 #FF", a);
	show("8 channels, ch7 #FF", b);
	check("ch5 left, ch7 right, once each", (a.left == 127 * 63 / 4) && !a.right && (b.right == 127 * 63 / 4) && !b.left);
	// panning: ch1's data with vol1 on the left and vol3 on the right
	gs->cfg0 = NGS_PAN4CH;
	static const int pan[8] = {63, 0, 21, 0, 0, 0, 0, 0};
	a = mix(gs, GS_12_34, ch1top, pan);
	show("panned 4 channels, ch1 #FF, vol1 63, vol3 21", a);
	check("ch1 on both sides by vol1 and vol3", (a.left == 127 * 63 / 4) && (a.right == 127 * 21 / 4));
	// INV7B: the byte taken as signed as it is
	gs->cfg0 = NGS_INV7B;
	static const int inv[8] = {0x7f, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
	a = mix(gs, GS_12_34, inv, ch1only);
	show("INV7B, ch1 #7F", a);
	check("#7F is +127 with INV7B", a.left == 2 * 127 * 63 / 4);
	gs->cfg0 = 0;
}

// a byte at the start of every 16K page says which page it is
static void mark_pages(GSound* gs) {
	for (int p = 0; p < 32; p++) gs->romNGS[p << 14] = 0x80 | p;
	for (int p = 0; p < 256; p++) gs->mem->ramData[p << 14] = p;
}

static void neogs_map(GSound* gs) {
	printf("== neogs-map\n");
	gs_set_model(gs, GS_NEOGS);
	mark_pages(gs);
	gs_rom_refresh(gs);
	check("reset: GSCFG0 #30", gsiord(0x0f, gs) == 0x30);
	check("reset: 10 MHz", gs->tickUnits == 12000);
	check("reset: flash page 0 at #0000", gsmemrd(0x0000, 0, gs) == 0x80);
	check("reset: RAM page 3 at #4000", gsmemrd(0x4000, 0, gs) == 3);
	gsiowr(0x00, 2, gs);
	check("flash pages 4 and 5 at #8000 and #C000", (gsmemrd(0x8000, 0, gs) == 0x84) && (gsmemrd(0xc000, 0, gs) == 0x85));
	gsiowr(0x0f, NGS_NOROM | 0x10, gs);		// what the loader sets: RAM, 12 MHz
	check("NOROM: RAM page 0 at #0000", gsmemrd(0x0000, 0, gs) == 0);
	check("12 MHz", gs->tickUnits == 10000);
	gsiowr(0x00, 5, gs);
	check("MPAG 5: RAM pages 10 and 11", (gsmemrd(0x8000, 0, gs) == 10) && (gsmemrd(0xc000, 0, gs) == 11));
	gsiowr(0x10, 7, gs);
	check("MPAGEX does nothing without EXPAG", gsmemrd(0xc000, 0, gs) == 11);
	gsiowr(0x0f, NGS_NOROM | NGS_EXPAG, gs);
	check("24 MHz", gs->tickUnits == 5000);
	gsiowr(0x00, 0x85, gs);
	gsiowr(0x10, 0x03, gs);
	check("EXPAG: MPAG #85 is page 11, MPAGEX #03 page 6", (gsmemrd(0x8000, 0, gs) == 11) && (gsmemrd(0xc000, 0, gs) == 6));
	gsiowr(0x00, 0xc0, gs);
	check("4M: MPAG #C0 is page 129, not page 1 again", gsmemrd(0x8000, 0, gs) == 129);
	gsiowr(0x0f, NGS_NOROM | NGS_RAMRO | 0x20, gs);
	check("20 MHz", gs->tickUnits == 6000);
	gsmemwr(0x0001, 0x55, gs);
	gsmemwr(0x4001, 0x55, gs);
	check("RAMRO: page 0 keeps, page 3 takes", (gs->mem->ramData[1] != 0x55) && (gs->mem->ramData[(3 << 14) + 1] == 0x55));
	gsiowr(0x0f, NGS_RAMRO, gs);
	gsiowr(0x00, 0, gs);
	check("RAMRO without NOROM: flash at #0000", gsmemrd(0x0000, 0, gs) == 0x80);
}

static void neogs_ports(GSound* gs) {
	printf("== neogs-ports\n");
	gs_set_model(gs, GS_NEOGS);
	check("SSTAT: no card, MP3 control idle", gsiord(0x12, gs) == 0x0e);
	check("SD read: #FF", gsiord(0x13, gs) == 0xff);
	check("a port past #3F is not the card's", gsiord(0x4f, gs) == 0xff);
	gsiowr(0x11, 0x80 | 0x05, gs);
	gsiowr(0x11, 0x01, gs);
	check("SCTRL: set #05, clear #01", gsiord(0x11, gs) == 0x04);
	gsiowr(0x09, 0x20, gs);
	gsiord(0x0b, gs);
	check("DAMNPORT2: the command bit from VOL4 bit 5", gsiord(0x04, gs) & 1);
	gsiowr(0x00, 1, gs);				// pg2 even
	gsiord(0x0a, gs);
	check("DAMNPORT1: the data bit from page bit 0", gsiord(0x04, gs) & 0x80);
	gsiowr(0x0f, NGS_8CHANS, gs);
	gsmemrd(0x6500, 0, gs);
	check("8 channels: #65xx latches ch6", gs->ch6 == gsmemrd(0x6500, 0, gs));
	gsiowr(0x0f, 0, gs);
	gs->ch5 = 0;
	gsmemrd(0x6400, 0, gs);
	check("4 channels: #64xx latches ch1 and its copy in ch5", gs->ch1 == gs->ch5);
	// the host's GSCTR
	gsiowr(0x0f, 0x11, gs);
	gsWrite(gs, 0x33, 0x40);
	check("GSCTR #40: an NMI", gs->cpu->intrq & 2);
	gsWrite(gs, 0x33, 0x80);
	check("GSCTR #80: the card reset, GSCFG0 #30", gsiord(0x0f, gs) == 0x30);
	gs_set_model(gs, GS_CLASSIC);
	check("the classic card leaves #33 alone", !gsWrite(gs, 0x33, 0x80));
}

// the listener's separation, over the card's own output
static void separation(GSound* gs) {
	sndPair a;
	printf("== separation\n");
	GSound* fresh = gsCreate();
	check("75% by default", fresh->sep == 75);
	gsDestroy(fresh);
	gs_set_model(gs, GS_NEOGS);
	gs->cfg0 = 0;
	gs->sep = 75;
	a = mix(gs, GS_12_34, ch1top, ch1only);
	show("75%, ch1 #FF alone", a);
	check("7/8 on its side, 1/8 on the other", (a.left == 4000 * 175 / 200) && (a.right == 4000 * 25 / 200));
	gs->sep = 0;
	a = mix(gs, GS_12_34, ch1top, ch1only);
	show("0%, ch1 #FF alone", a);
	check("0% is mono", (a.left == a.right) && (a.left == 2000));
	gs->sep = 100;
}

int main(void) {
	GSound* gs = gsCreate();
	gs->enable = 1;
	gs->sep = 100;			// the cards' own outputs
	classic_mix(gs);
	gs_set_model(gs, GS_NEOGS);
	neogs_mix(gs);
	neogs_map(gs);
	neogs_ports(gs);
	separation(gs);
	gsDestroy(gs);
	return failures ? 1 : 0;
}

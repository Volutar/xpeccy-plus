// Headless General Sound bench: the classic card's mix, from its schematic, and
// the NeoGS from its FPGA sources - the mix (sound_main.v, sound_mulacc.v), the
// memory map (memmap.v) and the ports (ports.v). Given the firmwares, their idle
// loop skipped against not (loop_skip).
//
//	gs-bench [GS.ROM NEOGS.ROM]

#include <stdio.h>
#include <string.h>

#include "sound/gs.h"
#include "cpu/Z80/z80.h"		// regPC: the card set at a wait of the bench's own

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
	check("mono: both sides alike, a quarter of the four", (a.left == a.right) && (a.left >= 255 * 63 / 4 - 1) && (a.left <= 255 * 63 / 4));
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
	check("SCTRL after a reset: #0B", gsiord(0x11, gs) == 0x0b);
	check("SD read: #FF", gsiord(0x13, gs) == 0xff);
	check("a port past #3F is not the card's", gsiord(0x4f, gs) == 0xff);
	gsiowr(0x11, 0x80 | 0x05, gs);
	gsiowr(0x11, 0x01, gs);
	check("SCTRL: #0B, set #05, clear #01", gsiord(0x11, gs) == 0x0e);
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
	gs->ch5 = 0x33;
	gsmemwr(0x6400, 0x5a, gs);
	gsmemrd(0x6400, 0, gs);
	check("4 channels: #64xx latches ch1, ch5 keeps its own", (gs->ch1 == 0x5a) && (gs->ch5 == 0x33));
	// the host's GSCTR
	gsiowr(0x0f, 0x11, gs);
	gsWrite(gs, 0x33, 0x40);
	check("GSCTR #40: an NMI", gs->cpu->intrq & 2);
	gsiowr(0x11, 0x80 | 0x30, gs);
	gsWrite(gs, 0x33, 0x80);
	check("GSCTR #80: the card reset, GSCFG0 #30, SCTRL #0B", (gsiord(0x0f, gs) == 0x30) && (gsiord(0x11, gs) == 0x0b));
	gs_set_model(gs, GS_ORIGINAL);
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

// The firmware's own idle loop, skipped (gs_loop_at) and not: three cards on one
// timeline of sub-samples (709 ns, the mix's at 1.41 MHz) with the host's
// accesses at the same steps - one flushed only at a frame's end, so the skip
// counts the turns; one at every step, so it never can; one at every step for
// the mix, so it parks. Their CPUs, memory and DAC levels must not part.

#define LS_STEP_NS	709
#define LS_FRAME	28209		// steps in 20 ms
#define LS_STEPS	(LS_FRAME * 400)	// 8 s: the boot and RAM test, the idle loop, a command

#define FNV_SEED	0xcbf29ce484222325ULL	// FNV-1a, 64 bits
#define FNV_PRIME	0x100000001b3ULL

static int read_file(const char* path, unsigned char* buf, int size) {
	FILE* f = fopen(path, "rb");
	if (!f) return 0;
	int n = (int)fread(buf, 1, size, f);
	fclose(f);
	return n == size;
}

static unsigned long long fold(unsigned long long h, const void* p, size_t n) {
	const unsigned char* b = (const unsigned char*)p;
	for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * FNV_PRIME;
	return h;
}

static unsigned long long card_hash(GSound* gs) {
	unsigned long long h = FNV_SEED;
	h = fold(h, gs->cpu->regs, sizeof(gs->cpu->regs));
	h = fold(h, gs->cpu->flags, sizeof(gs->cpu->flags));
	h = fold(h, gs->mem->ramData, gs->mem->ramSize);
	int v[16] = {gs->ch1, gs->ch2, gs->ch3, gs->ch4, gs->ch5, gs->ch6, gs->ch7, gs->ch8,
		gs->vol1, gs->vol2, gs->vol3, gs->vol4, gs->vol5, gs->vol6, gs->vol7, gs->vol8};
	h = fold(h, v, sizeof(v));
	h = fold(h, &gs->pstate, 1);
	h = fold(h, &gs->pb3_gs, 1);
	h = fold(h, &gs->due, sizeof(gs->due));
	h = fold(h, &gs->intDue, sizeof(gs->intDue));
	return h;
}

// A wait for the data bit with an EI ... DI window in each turn - DI at its head,
// the firmware's INT handler at #0038 taken in the window - then back to the
// idle loop
#define LS_EI_AT	0x5000
#define LS_EI_HEAD	0x5002
static const unsigned char lsEiLoop[] = {
	0xed, 0x56,		// im 1
	0xfb,			// ei
	0x00,			// nop: an INT comes in here
	0xf3,			// di
	0xdb, 0x04,		// in a,(#04)
	0xe6, 0x80,		// and #80
	0x28, 0xf7,		// jr z,#5002
	0xc3, 0x6e, 0x02	// jp #026E
};

// what the host does at step i: a byte to #B3 at 6 s, a command to #BB at
// 6.4 s, its state polled every 10 us for a frame after and the data read
// back; at 6.6 s the card is set waiting with EI (as a debugger would), and a
// byte to #B3 at 7.2 s ends the wait
static void ls_host(GSound* gs, int i) {
	int d;
	if (i == LS_FRAME * 300) gsWrite(gs, 0xb3, 0x55);
	if (i == LS_FRAME * 320) gsWrite(gs, 0xbb, 0x20);
	if ((i > LS_FRAME * 320) && (i < LS_FRAME * 321) && !(i % 14)) gsRead(gs, 0xbb, &d);
	if (i == LS_FRAME * 321) gsRead(gs, 0xb3, &d);
	if (i == LS_FRAME * 330) {
		gsRead(gs, 0xb3, &d);		// the data bit down, whatever the command left
		gsFlush(gs);
		for (int k = 0; k < (int)sizeof(lsEiLoop); k++)
			gsmemwr(LS_EI_AT + k, lsEiLoop[k], gs);
		gs->cpu->regPC = LS_EI_AT;
		gs_loop_forget(gs);
	}
	if (i == LS_FRAME * 360) gsWrite(gs, 0xb3, 0xaa);
}

enum {LS_FRAMES = 0, LS_STEPS_ALL, LS_PARKED};

typedef struct {
	unsigned long long card;	// the card at the end
	unsigned long long heard;	// the DAC levels at every step
	int head, len;			// the idle loop, before the host comes
	int parked;			// steps parked
	int eiParked;			// ...in the wait with EI, its turns taking INTs
} lsRes;

static GSound* ls_card(int model, const unsigned char* rom) {
	GSound* gs = gsCreate();
	gs->enable = 1;
	if (model == GS_NEOGS) memcpy(gs->romNGS, rom, NGS_ROM_SIZE);
	else memcpy(gs->romGS, rom, GS_ROM_SIZE);
	gs_set_model(gs, model);
	return gs;
}

// The card run one way: the host's time given every burst steps at once (a
// Z80 instruction of the host's spans several sub-samples), a flush at each step
// (how: all, for the mix) or at a frame's end
static lsRes ls_run(int model, const unsigned char* rom, int how, int burst) {
	lsRes r = {0};
	GSound* gs = ls_card(model, rom);
	unsigned long long h = FNV_SEED;
	for (int i = 0; i < LS_STEPS; i++) {
		if (!(i % burst)) gsSync(gs, LS_STEP_NS * ((LS_STEPS - i < burst) ? LS_STEPS - i : burst));
		ls_host(gs, i);
		if (how == LS_STEPS_ALL) gsFlush(gs);
		else if (how == LS_PARKED) gsFlushMix(gs);
		else if (!((i + 1) % LS_FRAME)) gsFlush(gs);
		if (gs->lpPark) r.parked++;
		if (how != LS_FRAMES) {
			sndPair p = gsVolume(gs);
			h = fold(h, &p, sizeof(p));
		}
		if (i == LS_FRAME * 300 - 1) {
			r.head = gs->lpHead;
			r.len = gs->lpLen;
		}
		if (gs->lpPark && (gs->lpHead == LS_EI_HEAD) && gs->lpEi) r.eiParked++;
	}
	gsFlush(gs);
	r.heard = h;
	r.card = card_hash(gs);
	gsDestroy(gs);
	return r;
}

static void loop_skip(const char* name, int model, const char* path, int size) {
	static unsigned char rom[NGS_ROM_SIZE];
	printf("== loop-skip %s\n", name);
	if (!read_file(path, rom, size)) {
		check("the firmware read", 0);
		return;
	}
	lsRes a = ls_run(model, rom, LS_FRAMES, 1);
	lsRes b = ls_run(model, rom, LS_STEPS_ALL, 1);
	lsRes c = ls_run(model, rom, LS_PARKED, 1);
	printf("idle in the loop at #%04X, %d T a turn\n", a.head, a.len);
	check("found at #026E, 51 T, by each", (a.head == 0x026e) && (a.len == 51) && (c.head == a.head) && (c.len == a.len));
	check("frame flushes: the card as at every step", a.card == b.card);
	check("parked: the card as at every step", c.card == b.card);
	check("parked: the DAC levels as at every step", c.heard == b.heard);
	check("parked a good part of the time", c.parked > LS_STEPS / 4);
	check("parked in the wait with EI as well, short of each INT", c.eiParked > 0);
	// the host's time 9 sub-samples at a time, an INT among them
	lsRes b9 = ls_run(model, rom, LS_STEPS_ALL, 9);
	lsRes c9 = ls_run(model, rom, LS_PARKED, 9);
	check("time 9 sub-samples at once, parked: the card as at every step", c9.card == b9.card);
	check("time 9 sub-samples at once, parked: the DAC levels as at every step", c9.heard == b9.heard);
}

// The other model after the card parked for a long time: the new one comes up as
// from the power, at its first instruction, owing nothing, its INT divider at 0 -
// as a fresh card of that model does
static void model_change(const char* gsPath, const char* ngsPath) {
	static unsigned char rom[GS_ROM_SIZE], ngs[NGS_ROM_SIZE];
	printf("== model-change\n");
	if (!read_file(gsPath, rom, GS_ROM_SIZE) || !read_file(ngsPath, ngs, NGS_ROM_SIZE)) {
		check("the firmwares read", 0);
		return;
	}
	GSound* gs = ls_card(GS_ORIGINAL, rom);
	memcpy(gs->romNGS, ngs, NGS_ROM_SIZE);
	for (int i = 0; i < LS_FRAME * 300; i++) {
		gsSync(gs, LS_STEP_NS);
		gsFlushMix(gs);
	}
	check("parked, owed over a second", gs->lpPark && (gs->due > 1000000000LL * GS_UNITS_NS));
	gs_set_model(gs, GS_NEOGS);
	GSound* fresh = ls_card(GS_NEOGS, ngs);
	check("a NeoGS from the power: at #0000, owing nothing, the INT divider at 0, not parked",
		(gs->cpu->regPC == 0) && (gs->due == 0) && (gs->intDue == GS_INT_UNITS) && !gs->lpPark);
	for (int i = 0; i < LS_FRAME; i++) {
		gsSync(gs, LS_STEP_NS);
		gsFlushMix(gs);
		gsSync(fresh, LS_STEP_NS);
		gsFlushMix(fresh);
	}
	check("a frame on: its CPU as a fresh card's", !memcmp(gs->cpu->regs, fresh->cpu->regs, sizeof(gs->cpu->regs))
		&& (gs->due == fresh->due) && (gs->intDue == fresh->intDue));
	gsDestroy(fresh);
	gsDestroy(gs);
}

int main(int argc, char** argv) {
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
	// the firmwares: gs-bench GS.ROM NEOGS.ROM
	if (argc > 2) {
		loop_skip("original", GS_ORIGINAL, argv[1], GS_ROM_SIZE);
		loop_skip("neogs", GS_NEOGS, argv[2], NGS_ROM_SIZE);
		model_change(argv[1], argv[2]);
	}
	return failures ? 1 : 0;
}

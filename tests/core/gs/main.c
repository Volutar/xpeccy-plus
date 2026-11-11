// Headless General Sound mix bench: gsVolume() on fixed samples and volumes, both
// boards. Original: the schematic's, code * vol against GND and ~0.47 of the other
// side; NeoGS: its FPGA's, the sample signed around #80, no cross-mix.
//
//	gs-bench

#include <stdio.h>

#include "sound/gs.h"

static int failures;

static void check(const char* name, int ok) {
	printf("%s: %s\n", name, ok ? "ok" : "FAIL");
	if (!ok) failures++;
}

static sndPair mix(GSound* gs, int board, int stereo, const int* ch, const int* vol) {
	gs->board = board;
	gs->stereo = stereo;
	gs->ch1 = ch[0]; gs->ch2 = ch[1]; gs->ch3 = ch[2]; gs->ch4 = ch[3];
	gs->vol1 = vol[0]; gs->vol2 = vol[1]; gs->vol3 = vol[2]; gs->vol4 = vol[3];
	return gsVolume(gs);
}

static void show(const char* name, sndPair p) {
	printf("%s: %d %d\n", name, p.left, p.right);
}

int main(void) {
	GSound* gs = gsCreate();
	gs->enable = 1;
	static const int mid[4] = {0x80, 0x80, 0x80, 0x80};
	static const int top[4] = {0xff, 0xff, 0xff, 0xff};
	static const int ch1top[4] = {0xff, 0x80, 0x80, 0x80};
	static const int off[4] = {0, 0, 0, 0};
	static const int full[4] = {63, 63, 63, 63};
	static const int ch1only[4] = {63, 0, 0, 0};
	sndPair a, b;

	printf("== neogs\n");
	a = mix(gs, GS_BOARD_NEOGS, GS_12_34, mid, off);
	b = mix(gs, GS_BOARD_NEOGS, GS_12_34, mid, full);
	show("mid, vol 0", a);
	show("mid, vol 63", b);
	check("a volume change on #80 moves nothing", (a.left == b.left) && (a.right == b.right) && !b.left);
	show("all #FF, vol 63", mix(gs, GS_BOARD_NEOGS, GS_12_34, top, full));
	a = mix(gs, GS_BOARD_NEOGS, GS_12_34, ch1top, ch1only);
	show("ch1 #FF alone", a);
	check("ch1 left only", (a.left == 127 * 63 / 2) && !a.right);
	a = mix(gs, GS_BOARD_NEOGS, GS_MONO, ch1top, ch1only);
	show("mono, ch1 #FF alone", a);

	printf("== original\n");
	a = mix(gs, GS_BOARD_ORIGINAL, GS_12_34, mid, off);
	b = mix(gs, GS_BOARD_ORIGINAL, GS_12_34, mid, full);
	show("mid, vol 0", a);
	show("mid, vol 63", b);
	check("a volume change on #80 moves the level", (b.left - a.left) == 0x80 * 63);
	a = mix(gs, GS_BOARD_ORIGINAL, GS_12_34, top, full);
	show("all #FF, vol 63", a);
	check("the peak is the uncrossed one", (a.left == 255 * 63) && (a.right == 255 * 63));
	a = mix(gs, GS_BOARD_ORIGINAL, GS_12_34, ch1top, ch1only);
	show("ch1 #FF alone", a);
	check("the right side has 0.47 of the left", (a.right * 100 / a.left) == 47);
	a = mix(gs, GS_BOARD_ORIGINAL, GS_MONO, ch1top, ch1only);
	show("mono, ch1 #FF alone", a);
	check("mono has no cross-mix", (a.left == a.right) && (a.left == 255 * 63 / 4));

	gsDestroy(gs);
	return failures ? 1 : 0;
}

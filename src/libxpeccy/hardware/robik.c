#include "../spectrum.h"

// Robik (NPO Rotor, Cherkasy): a ZX Spectrum 48K on discrete logic, no ULA.
//
// Memory: the ROM at #0000-#3FFF and 48K of RAM, all in eight 64K chips: the
// 16K left over is the shadow RAM (Board > Shadow RAM). Writes to #0000-#3FFF
// always land in it; reads come from the ROM or from it by a latch that any
// read or write of a port with A0-A7 = #0F flips (measured on an owner's board:
// OUT and IN, A8-A15 not decoded). Reset clears the latch, the page keeps its
// bytes. Not every board has it, so it can be off: a 48K's ROM then.
//
// Timing: the video reads its own copy of the screen, so nothing is contended
// but a write into #4000-#5FFF, the copy's half of the 16K: it waits to the next
// 4 T cell of the video, 1 to 4 T, anywhere in the frame. Reads and fetches never
// wait, and #6000-#7FFF does not either (two owners' boards, ROBIK TEST 1).
//
// Keyboard: the 48K's 40 keys and eleven of its own on D5 and D7 of #FE
// (keyTab). Kempston on #1F, no floating bus, #FF on the bus at INT acknowledge.
// A Beta Disk is no part of it, but one fitted pages TR-DOS in as on the 48K.
//
// 128K: no factory model, but owners' boards were taken there (Faultless #08, 1997:
// #7FFD as the Pentagon's). Here #7FFD as the Pentagon 128's, and a 128K's pair of
// ROMs in banks 2 and 3: the 128 editor calls its own 48 BASIC, which the Robik's
// V03 is not (it hangs there). The Robik's frame, clock and wait stay, writes into
// either screen's 8K waiting. How such a board switched its shadow RAM nobody wrote
// down, so it has none.

#define ROBIK_SHADOW	3		// the RAM page under the ROM (48K uses 5, 2, 0)
#define ROBIK_WAIT_PHASE	4	// dots: where the 8-dot video cell starts against the INT
#define ROBIK_BORDER_LAG	5	// T from an OUT's I/O cycle start to the border's new color
// ROM banks: 0 the Robik's own BASIC, 1 TR-DOS, 2 and 3 the 128K's pair
#define ROBIK_ROM_128	2	// the 128 editor, #7FFD D4 = 0
#define ROBIK_ROM_48_128	3	// its 48 BASIC, D4 = 1

static int robik_is_128(Computer* comp) {
	return comp->mem->ramSize >= MEM_128K;
}

// the shadow page, fitted and on a board that has one: not on a 128K
static int robik_has_shadow(Computer* comp) {
	return comp->flgSHDW && !robik_is_128(comp);
}

void robik_map_mem(Computer* comp) {
	Memory* mem = comp->mem;
	int is128 = robik_is_128(comp);
	mem->ramMask = MEM_128K - 1;
	comp->tape->is48 = !is128;		// a TZX's "stop if 48K"; the size can change without a reset
	if (!robik_has_shadow(comp))
		comp->flgSHDON = 0;		// no page, no latch: switched on again it starts at the ROM
	if (!is128) {				// a 48K's: the 128K's paging, left by a reset, is not there
		comp->flgROM = 1;
		comp->vid->vidPage = 5;
	}
	if (comp->flgDOS) {			// a Beta Disk fitted: TR-DOS as on the 48K
		memSetBank(mem, 0x00, MEM_ROM, 1, MEM_16K, NULL, NULL, NULL);
	} else if (comp->flgSHDON) {
		memSetBank(mem, 0x00, MEM_RAM, ROBIK_SHADOW, MEM_16K, NULL, NULL, NULL);
	} else {
		memSetBank(mem, 0x00, MEM_ROM, !is128 ? 0 : comp->flgROM ? ROBIK_ROM_48_128 : ROBIK_ROM_128,
			MEM_16K, NULL, NULL, NULL);
	}
	memSetBank(mem, 0x40, MEM_RAM, 5, MEM_16K, NULL, NULL, NULL);
	memSetBank(mem, 0x80, MEM_RAM, 2, MEM_16K, NULL, NULL, NULL);
	memSetBank(mem, 0xc0, MEM_RAM, is128 ? (comp->p7FFD & 7) : 0, MEM_16K, NULL, NULL, NULL);
}

// a write into a screen's 8K: #4000-#5FFF, or page 5 or 7 at #C000 on a 128K
static int robik_screen_wr(Computer* comp, int adr) {
	if ((adr & 0xe000) == 0x4000) return 1;
	if (((adr & 0xe000) != 0xc000) || !robik_is_128(comp)) return 0;
	return (comp->p7FFD & 5) == 5;		// page 5 or 7
}

// A write into the screen copy waits for the video's next cell: 1 to 4 T, never 0.
// The phase is the one that gives the owners' figures (ROBIK TEST 1).
static void robik_wait(Computer* comp) {
	comp_vid_to_t(comp);
	int phase = (vid_ray_dots(comp->vid) + ROBIK_WAIT_PHASE) & 7;
	comp->cpu->t += comp->contTicks[8 - phase];
	comp_vid_to_t(comp);
}

void robik_mwr(Computer* comp, int adr, int val) {
	if (adr < 0x4000) {
		if (robik_has_shadow(comp))
			comp->mem->ramData[(ROBIK_SHADOW << 14) | adr] = val & 0xff;
		return;
	}
	if (robik_screen_wr(comp, adr))
		robik_wait(comp);
	mem_wr_fast(comp->mem, adr, val);
}

static void robik_flip(Computer* comp) {
	if (!robik_has_shadow(comp)) return;
	comp->flgSHDON ^= 1;
	robik_map_mem(comp);
}

// #FE: the 48K's bits, then D5 and D7 down for the Robik's own keys
static int robik_in_fe(Computer* comp, int port) {
	int res = xInFE(comp, port);
	int ext = 0xff;
	for (int i = 0; i < 8; i++) {
		if (!(port & (0x8000 >> i)))
			ext &= comp->keyb->map[i];
	}
	return res & (ext | 0x5f);
}

// port 15 flips the latch on a read as well; what the read gives is the joystick's,
// whose port it shares A5-A7 = 0 with
static int robik_in_0f(Computer* comp, int port) {
	robik_flip(comp);
	return zx_in_joy(comp, port);
}

static void robik_out_0f(Computer* comp, int port, int val) {
	robik_flip(comp);
}

// The border takes a new color 4 T later than iowr() brings the OUT here (the I/O
// cycle's start + 1): a T past the cycle's end (ROBIK TEST 2's timed border against
// the paper, on AlexZor's and Sudakov's boards through RGB).
// The ray is walked there first, a T past the OUT's end; the INT check allows for
// a video ahead of the cpu (zx_irq_ack_in), and nothing else reads the ray that soon.
static void robik_out_fe(Computer* comp, int port, int val) {
	comp_vid_to_lazy(comp, comp->cpu->t + ROBIK_BORDER_LAG);
	vid_settle(comp->vid);
	xOutFE(comp, port, val);
}

// #7FFD on a 128K, as the Pentagon 128's: A15 and A1, a read does not page
static void robik_out_7ffd(Computer* comp, int port, int val) {
	if (!robik_is_128(comp) || (comp->p7FFD & 0x20)) return;	// locked until reset
	comp->p7FFD = val & 0xff;
	comp->flgROM = (val & 0x10) ? 1 : 0;
	comp->vid->vidPage = (val & 0x08) ? 7 : 5;
	robik_map_mem(comp);
}

// The AY and the Kempston mouse are add-ons, decoded as the 48K's map has them
static xPort robikPortMap[] = {
	{0x0001,0x00fe,2,2,2,robik_in_fe,	robik_out_fe},
	{0x00ff,0x000f,2,2,2,robik_in_0f,	robik_out_0f},
	{0x8002,0x7ffd,2,2,2,NULL,	robik_out_7ffd},
	{0xc002,0xfffd,2,2,2,xInFFFD,	xOutFFFD},
	{0xc002,0xbffd,2,2,2,NULL,	xOutBFFD},
	{0x00e1,0x001f,2,2,2,zx_in_joy,	NULL},
	{0x0320,0xfadf,2,2,2,xInFADF,	NULL},		// Kempston mouse: A5, A8-A10
	{0x0720,0xfbdf,2,2,2,xInFBDF,	NULL},
	{0x0720,0xffdf,2,2,2,xInFFDF,	NULL},
	{0x0000,0x0000,2,2,2,zx_in_float,NULL}
};

static void robik_out(Computer* comp, int port, int val) {
	difOut(comp->dif, port, val, comp->flgBDI);
	zx_dev_wr(comp, port, val);
	hwOut(robikPortMap, comp, port, val, 1);
}

static int robik_in(Computer* comp, int port) {
	int res;
	if (difIn(comp->dif, port, &res, comp->flgBDI)) return res;
	if (zx_dev_rd(comp, port, &res)) return res;
	return hwIn(robikPortMap, comp, port);
}

static void robik_reset(Computer* comp) {
	comp->flgSHDON = 0;
	comp->flgROM = (comp->p7FFD & 0x10) ? 1 : 0;	// what the reset's target left; a 48K has 1
	comp->vid->vidPage = 5;
	robik_map_mem(comp);
	zx_reset(comp);
}

HardWare robik_hw_core = {HW_ROBIK,"Robik","Robik",MEM_128K | MEM_64K | MEM_48K,1.0,NULL,NULL,
			zx_init,robik_map_mem,robik_out,robik_in,stdMRd,robik_mwr,zx_irq,zx_ack,robik_reset,zx_sync,zx_keyp,zx_keyr,zx_vol,
			NULL,NULL,&hwq_zx, .sndtake = zx_snd_take, .sndmix = zx_mix};

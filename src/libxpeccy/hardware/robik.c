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

#define ROBIK_SHADOW	3		// the RAM page under the ROM (48K uses 5, 2, 0)
#define ROBIK_WAIT_PHASE	4	// dots: where the 8-dot video cell starts against the INT
#define ROBIK_BORDER_LAG	4	// T from an OUT's I/O cycle start to the border's new color

void robik_map_mem(Computer* comp) {
	Memory* mem = comp->mem;
	mem->ramMask = MEM_128K - 1;
	if (!comp->flgSHDW)
		comp->flgSHDON = 0;		// no page, no latch: switched on again it starts at the ROM
	if (comp->flgDOS) {			// a Beta Disk fitted: TR-DOS as on the 48K
		memSetBank(mem, 0x00, MEM_ROM, 1, MEM_16K, NULL, NULL, NULL);
	} else if (comp->flgSHDW && comp->flgSHDON) {
		memSetBank(mem, 0x00, MEM_RAM, ROBIK_SHADOW, MEM_16K, NULL, NULL, NULL);
	} else {
		memSetBank(mem, 0x00, MEM_ROM, 0, MEM_16K, NULL, NULL, NULL);
	}
	memSetBank(mem, 0x40, MEM_RAM, 5, MEM_16K, NULL, NULL, NULL);
	memSetBank(mem, 0x80, MEM_RAM, 2, MEM_16K, NULL, NULL, NULL);
	memSetBank(mem, 0xc0, MEM_RAM, 0, MEM_16K, NULL, NULL, NULL);
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
		if (comp->flgSHDW)
			comp->mem->ramData[(ROBIK_SHADOW << 14) | adr] = val & 0xff;
		return;
	}
	if ((adr & 0xe000) == 0x4000)
		robik_wait(comp);
	mem_wr_fast(comp->mem, adr, val);
}

static void robik_flip(Computer* comp) {
	if (!comp->flgSHDW) return;
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

// The border takes a new color 3 T later than iowr() brings the OUT here (the I/O
// cycle's start + 1): at the cycle's end, where the write strobe ends (ROBIK TEST
// 2's timed border on AlexZor's board, against the paper).
// The ray is walked there first; the I/O cycle is an OUT's last, so it does not
// run ahead of the instruction.
static void robik_out_fe(Computer* comp, int port, int val) {
	comp_vid_to_lazy(comp, comp->cpu->t + ROBIK_BORDER_LAG);
	vid_settle(comp->vid);
	xOutFE(comp, port, val);
}

static xPort robikPortMap[] = {
	{0x0001,0x00fe,2,2,2,robik_in_fe,	robik_out_fe},
	{0x00ff,0x000f,2,2,2,robik_in_0f,	robik_out_0f},
	{0x00e1,0x001f,2,2,2,zx_in_joy,	NULL},
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
	comp->flgROM = 1;
	comp->vid->vidPage = 5;
	robik_map_mem(comp);
	zx_reset(comp);
}

HardWare robik_hw_core = {HW_ROBIK,"Robik","Robik",MEM_64K | MEM_48K,1.0,NULL,NULL,
			zx_init,robik_map_mem,robik_out,robik_in,stdMRd,robik_mwr,zx_irq,zx_ack,robik_reset,zx_sync,zx_keyp,zx_keyr,zx_vol,
			NULL,NULL,&hwq_zx, .sndtake = zx_snd_take, .sndmix = zx_mix};

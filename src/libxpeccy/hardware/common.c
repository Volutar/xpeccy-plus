#include "hardware.h"
#include <math.h>
#include "../xlog.h"
#include "../ldbytes.h"
#include "../filetypes/filetypes.h"
#include "../cpu/Z80/z80.h"
#include "zx_step.h"

int compflags = 0;

// mem

// The 128K memory map, which the Pentagon inherits: the ROM pair at 0x0000
// (doubled by the DOS flag, so a Beta Disk machine reaches its interface ROM
// at banks 2,3), the screen bank at 0x4000, bank 2 at 0x8000 and whatever
// 0x7FFD selects at 0xC000. `extMask` picks up the bits above the 3-bit bank
// field - 0xc0 on the Pentagon, whose 512K extension a real 128K does not have.
void zx128_map_mem(Computer* comp, int extMask) {
	int pg = (comp->flgDOS ? 2 : 0) | (comp->flgROM ? 1 : 0);
	memSetBank(comp->mem, 0x00, MEM_ROM, pg, MEM_16K, NULL, NULL, NULL);
	pg = (comp->p7FFD & 7) | ((comp->p7FFD & extMask) >> 3);
	memSetBank(comp->mem, 0x40, MEM_RAM, 5, MEM_16K, NULL, NULL, NULL);
	memSetBank(comp->mem, 0x80, MEM_RAM, 2, MEM_16K, NULL, NULL, NULL);
	memSetBank(comp->mem, 0xc0, MEM_RAM, pg, MEM_16K, NULL, NULL, NULL);
}

// INT handle/check

void zx_sync(Computer* comp, int ns) {
	zx_sync_in(comp, ns);
}


// Memory contention. The delay for a bus cycle is read from the ULA table at
// the tick the cycle starts on and added whole, the way fuse does it. The
// older "step a tick at a time until the window frees" loop agrees with this
// for the Ferranti pattern, which counts down to zero, but not for the +2A/+3
// one (1,0,7,6,5,4,3,2), whose last slots reach past the end of the window.

// The address passed the pattern test and the video stands at cpu->t.
void zx_contend_adr(Computer* comp, int adr, int mreq) {
	int wdots = vid_wait_dots(comp->vid, adr, mreq);
	if (!wdots) return;
	comp->cpu->t += comp->contTicks[wdots];
	comp_vid_to_t(comp);
}

// The cpu is at T4 of an opcode fetch, with the refresh address on the bus.
// It disturbs the ULA only if that address lands in the memory the ULA is
// reading from - #4000..#7FFF on a 48K, plus an odd bank paged in at #C000 on
// a 128K - which is the same "contended page" test the wait states use.
//
// Which bank the disturbed burst then reads is not simply the screen's: bit 2
// of it comes from the bank the refresh address is in, bit 1 from the screen
// the ULA is showing, and it is always an odd bank. So a 128K with I in bank 1
// or 3 snows with bytes out of bank 1 (screen 0) or bank 3 (screen 1). Table
// measured by Spectramine, hype.retroscene.org/blog/1089.html. What the burst
// does with them is vid_snow's half.
// The 16K bank an address sits in, 0 if it is not ram. The odd ones are the
// slow banks the ULA shares with the cpu.
int zx_bank_of(Computer* comp, int adr) {
	int bank = comp->mem->bank[(adr >> 8) & 0xff];
	return (bank < 0) ? 0 : bank;
}

void zx_snow(Computer* comp) {
	int bank = zx_bank_of(comp, comp->cpu->regI << 8);
	if (!mem_bank_slow(bank)) return;		// not a contended bank
	if (!vid_snow(comp->vid, z80_get_r(comp->cpu),
			(bank & 4) | (comp->vid->vidPage & 2) | 1)) return;
	// Some 128K machines hang or reset under snow and others take it without a
	// murmur. Nobody has published the mechanism; what SpecEmu says about its
	// own switch is that such a machine goes unstable while it runs code out of
	// the slow memory, so that is the shape this takes - the cycle the ULA took
	// over leaves the ram addressed the way the ULA left it, and the next
	// opcode fetched from a slow bank comes back from the wrong place. memrd()
	// is the other half; it wants the same address bits the snow used.
	if (comp->flgSNOWX) {
		comp->snowBad = 0x80 | (z80_get_r(comp->cpu) & 0x7f);
		comp->cpu->rdp = NULL;		// the next read is memrd()'s (comp_mem_direct)
		comp->cpu->m1p = NULL;
	}
}

void zx_irq(Computer* comp, int t) {
	switch(t) {
		case IRQ_VID_INT:			// frame int start
			if (!comp->rzx.play) {		// ignore when playing rzx
				vid_set_int_frame(comp->vid, comp->vid->intsize);
				comp->intVector = 0xff;
				comp->cpu->intrq |= Z80_INT;
			}
			break;
		case IRQ_RZX_INT:
			// A recording's frame ends after the instruction that used up its
			// fetches, and its INT is taken right there or not at all: not one
			// instruction later, and not while the pulse lasts (IRQ_CPU_ACK).
			// An EI just before does not hold it off: the recorders count the
			// EI delay in T, and the frame boundary starts the count again.
			// A snapshot recorded at the frame's end is loaded first, and the
			// INT is the loaded machine's; a frame of no fetches runs nothing.
			do {
				comp->rzx.fCurrent++;
				comp->rzx.fCount--;
				rzxGetFrame(comp);
			} while (comp->rzx.play && (comp->rzx.frm.fetches == 0));
			if (comp->rzx.noint) {		// a join made between two INTs
				comp->rzx.noint = 0;
				break;
			}
			comp->intVector = 0xff;
			comp->cpu->intrq |= Z80_INT;
			comp->cpu->flgACK = 1;
			comp->cpu->flgNOINT = 0;
			vid_set_int_frame(comp->vid, comp->vid->intsize);
			break;
		case IRQ_VID_IEND:			// frame int end (for tsconf see in tslab.c)
			comp->cpu->intrq &= ~Z80_INT;
			break;
		case IRQ_NMI:				// zx_sync takes it from here
			comp->flgNMIRQ = 1;
			break;
	}
}

void zx_devs_sync(Computer* comp, int ns) {
	zx_devs_sync_in(comp, ns);
}

int hwq_no_turbo(Computer* comp) {
	return comp->hwMul < 2;
}

const hwQuiet hwq_zx = {HWQ_ACK_ZX, NULL, NULL, zx_devs_sync, zx_fetch_quiet, 0};
const hwQuiet hwq_zx_nohalt = {HWQ_ACK_ZX, NULL, NULL, zx_devs_sync, NULL, 0};

int zx_ack(Computer* comp) {
	return comp->intVector & 0xff;
}

void zx_init(Computer* comp) {
//	comp->fps = 50;
	dif_align_flps(comp->dif, comp->dif->fdc, 0, 1, 2, 3);
	vid_upd_timings(comp->vid, comp->nsPerTick / 2.0);
	fdc_set_hd(comp->dif->fdc, 0);
	chip_set_xdev(comp->ts->chipA, NULL, NULL, NULL);
	kbd_set_type(comp->keyb, KBD_SPECTRUM);
}

// zx keypress/release

void zx_keyp(Computer* comp, keyEntry* ent) {
	kbd_press(comp->keyb, ent);
}

void zx_keyr(Computer* comp, keyEntry* ent) {
	kbd_release(comp->keyb, ent);
}

// volume

// Every device is mixed here, and each has a dc blocker of its own on the way in.
// The state sits here rather than in Computer on purpose: run-ahead never reaches
// this function - its frame makes no sound - so there is nothing for a snapshot to
// carry, and there is only ever one machine in the process anyway.
sndPair zx_vol(Computer* comp, sndVolume* sv) {
	static sndDC dcBeep, dcAy, dcGs, dcSdrv, dcSaa;
	sndPair vol;
	sndPair svol;
	int dc = sv->dc;		// read once, so every device in one sample agrees
	int lev = 0;
	// 1:tape sound. volPlay is a level around 0x80, not a level above zero -
	// bit 7 of it is the EAR bit the ULA reads - so it is centred here. Read as
	// 0..255 it put half of the tape volume into the mix as a constant, which is
	// what a machine with nothing running was sitting on.
	if (comp->tape->rec) {
		lev = (comp->tape->levRec ? 0x800 : -0x800) * sv->tape / 100;
	} else if (!comp->tape->mute) {
		lev = ((comp->tape->volPlay - 0x80) << 8) * sv->tape / 1600;
	}
	// 2:beeper. The tape reaches the speaker on the same wire and is one level
	// with it here, so the two share a blocker as well
	lev += bc_level(comp->beep, sv->beep);
	svol.left = lev;
	svol.right = lev;
	vol = snd_dc(&dcBeep, svol, dc);
	// 3:turbo sound
	svol = snd_dc(&dcAy, tsGetVolume(comp->ts), dc);
	vol.left += svol.left * sv->ay / 100;
	vol.right += svol.right * sv->ay / 100;
	// 4:general sound
	svol = snd_dc(&dcGs, gsVolume(comp->gs), dc);
	vol.left += svol.left * sv->gs / 100;
	vol.right += svol.right * sv->gs / 100;
	// 5:soundrive
	svol = snd_dc(&dcSdrv, sdrvVolume(comp->sdrv), dc);
	vol.left += svol.left * sv->sdrv / 100;
	vol.right += svol.right * sv->sdrv / 100;
	// 6:saa
	svol = snd_dc(&dcSaa, saaVolume(comp->saa), dc);
	vol.left += svol.left * sv->saa / 100;
	vol.right += svol.right * sv->saa / 100;
	return vol;
}

// set std zx palette

void zx_set_pal(Computer* comp) {
	int i;
//	xColor xcol;
	for (i = 0; i < 16; i++) {
		vid_reset_col(comp->vid, i);
	}
}

// in

int zx_dev_wr(Computer* comp, int adr, int val) {
	if (div_out(comp->div, adr, val)) return 1;
	if (gsWrite(comp->gs, adr, val)) return 1;
	if (!comp->flgBDI && saaWrite(comp->saa, adr, val)) return 1;
	if (!comp->flgBDI && sdrvWrite(comp->sdrv, adr, val)) return 1;
	if (ideOut(comp->ide, adr, val, comp->flgBDI)) return 1;
	if (ula_wr(comp->vid->ula, adr, val)) return 1;
	return 0;
}

int zx_dev_rd(Computer* comp, int adr, int* ptr) {
	if (div_in(comp->div, adr, ptr)) return 1;
	if (gsRead(comp->gs, adr, ptr)) return 1;
	if (ideIn(comp->ide, adr, ptr, comp->flgBDI)) return 1;
	if (ula_rd(comp->vid->ula, adr, ptr)) return 1;
	return 0;
}

// Kempston on #1F - or no Kempston, and the port reads what any port nothing
// answers does, as it does on a machine that came without one
int zx_in_joy(Computer* comp, int port) {
	if (comp->joy->type != XJ_KEMPSTON) return zx_in_float(comp, port);
	return joyInput(comp->joy);
}

int xIn1F(Computer* comp, int port) {
	return zx_in_joy(comp, port);
}

// A port nothing answers. What comes back is `floatbus` in the machine's own
// file; vid_float_bus() is the ULA's half of all three answers.
//
// The Amstrad machines are the awkward one. Their gate array only leaves the
// bus open on ports 1, 5, 9 ... 4093 and only while paging is on, so a +3 in
// 48K mode has no floating bus at all; what comes back always has bit 0 set;
// and between the four reads the bus keeps whichever came last, the gate
// array's own fetch or a byte that went to or came from contended memory,
// instead of going to #FF. Worked out by Ast A. Moore and Hikaru in 2017 -
// sky.relative-path.com/zx/floating_bus.html.
int zx_in_float(Computer* comp, int port) {
	int res, age;
	switch (comp->fbus) {
		case FBUS_ULA:
			res = vid_float_bus(comp->vid, NULL);
			return (res < 0) ? 0xff : res;
		case FBUS_ASIC:
			if (((port & 0xf003) != 1) || (comp->p7FFD & 0x20)) return 0xff;
			res = vid_float_bus(comp->vid, &age);
			if (ticks_to_ns_fixed(comp, comp->tickCount + comp->cpu->t - comp->fbusTick)
					< (long long)age * comp->vid->nsPerDotFixed)
				res = comp->fbusLast;		// the cpu's access came later
			return res | 1;
		case FBUS_ATTR:
			if ((port & 0xff) != 0xff) return 0xff;
			if (comp->vid->vbrd || comp->vid->hbrd) return 0xff;
			return vid_atrbyte(comp->vid);
	}
	return 0xff;
}

// The bus a +2A/+3 hands back between those fetches can be the last byte that
// went to or came from contended memory, so those two machines watch their own
// accesses for it: banks 4-7, the set their gate array contends (mem_bank_asic).
static void asic_latch(Computer* comp, int adr, int val) {
	if (!mem_bank_asic(zx_bank_of(comp, adr))) return;
	comp->fbusLast = val & 0xff;
	comp->fbusTick = comp->tickCount + comp->cpu->t;
}

int asicMRd(Computer* comp, int adr, int m1) {
	int res = stdMRd(comp, adr, m1);
	asic_latch(comp, adr, res);
	return res;
}

void asicMWr(Computer* comp, int adr, int val) {
	asic_latch(comp, adr, val);
	stdMWr(comp, adr, val);
}

// The ear input's RC network, fitted to FE-Delay on a real issue 2 and issue 6A
// 48K: time constants and hold in T at 3.5 MHz, the rest a share of full charge.
static const struct earRC {
	double charge, drain, level, jump, mic, hold;
} earRCTab[] = {
	{475, 2070, 0.58, 0.48, 0.034, 32},	// issue 2
	{500, 1119, 0.62, 0.02, 0.05, 32},	// issue 3
};

// Bit 4 goes to lev, or bit 3 to mic under bit 4: bring the charge up to now,
// and on a drop work out the tick bit 6 falls back to 0 at (earDead).
static void zx_ear_edge(Computer* comp, int lev, int mic) {
	const struct earRC* rc = &earRCTab[(comp->earback == EAR_ISSUE2) ? 0 : 1];
	const double tpt = comp->earTpt;			// machine ticks in a 3.5 MHz T
	int now = comp->tickCount + comp->cpu->t;
	double dt = (double)(unsigned)(now - comp->earTick) / tpt;
	if (comp->beep->lev) {
		double top = 1.0 + (comp->tape->levRec ? rc->mic : 0.0);
		comp->earV = top - (top - comp->earV) * exp(-dt / rc->charge);
	} else {
		comp->earV *= exp(-dt / rc->drain);
	}
	if (lev && !comp->beep->lev)
		comp->earV += (1.0 + (mic ? rc->mic : 0.0) - comp->earV) * rc->jump;
	if (!lev && comp->beep->lev) {
		double hold = rc->hold;
		if (comp->earV > rc->level)
			hold = fmax(hold, rc->drain * log(comp->earV / rc->level));
		comp->earDead = now + (int)(hold * tpt);
	}
	comp->earTick = now;
}

// bit 6 of #FE: a playing tape is the whole of it, otherwise the machine hears
// its own last out #FE, and how much of it is the issue 2 / issue 3 difference.
// A stopped tape is not the tape - volPlay keeps the level it stopped on, which
// this used to read as a permanent 1.
int zx_ear(Computer* comp) {
	int ear;
	if (comp->tape->on && !comp->tape->rec)
		return !!(comp->tape->volPlay & 0x80);
	if (comp->earback == EAR_NONE)
		return 0;
	ear = comp->beep->lev || (comp->earRC && (comp->tickCount + comp->cpu->t - comp->earDead < 0));
	if (comp->earback == EAR_ISSUE2)
		ear |= comp->tape->levRec;
	return ear;
}

static int zx_peek_word(Computer* comp, int adr) {
	return (memRd(comp->mem, adr & 0xffff) & 0xff) | ((memRd(comp->mem, (adr + 1) & 0xffff) & 0xff) << 8);
}

// The rom's edge routine was entered for a loader of its own: where it returns
// to (a word deeper under LD-EDGE-2's own call) is not rom, nor the copy of
// LD-BYTES flash loading answers for. St. Dragon's copy jumps into LD-SAMPLE
// (#05EC) for every edge and never comes back to LD_START for the trap.
static int zx_edge_borrowed(Computer* comp) {
	int sp = comp->cpu->regSP;
	int ret = zx_peek_word(comp, sp);
	if (ret == LD_ROM_BASE + LDC_EDGE2_RET)
		ret = zx_peek_word(comp, sp + 2);
	if (zx_rom_code(comp, ret)) return 0;
	int base = comp->tape->ldBase;
	return (base < 0) || (((ret - base) & 0xffff) >= LDC_LEN);
}

// The rom's own loader polls #FE in exactly the pattern tapDetectLoader looks
// for, and the tape trap serves the rom - so the detector must not answer for
// it, or a flash load gets a tape playing under it and the two fall out of step.
// LD-EDGE-1/2 (#05E3..#05F9) is the only rom code that reads the port this way,
// and the same holds for a copy of LD-BYTES in ram that flash loading answers for.
// The address goes first: one field read, and false on every keyboard poll.
// 1: the trap's, 2: the rom's edge routine run for a loader of its own.
static int zx_ld_edge(Computer* comp) {
	int pc = comp->cpu->regPC;
	if (ld_edge(pc, LD_ROM_BASE) && zx_rom_active(comp)) return zx_edge_borrowed(comp) ? 2 : 1;
	return (comp->tape->ldBase >= 0) && ld_edge(pc, comp->tape->ldBase);
}

// The read is the rom's own, not a loader's: whatever is doing it is running
// from rom. Asked of the memory map rather than of the address, and of the map
// rather than of the 48 rom being in: a 128 sits in its editor rom while it
// starts a tape, and its keyboard poll is no more a loader than #05ED is.
int zx_rom_code(Computer* comp, int pc) {
	return mem_get_page(comp->mem, pc)->type == MEM_ROM;
}

// What the code right after an IN from #FE looks at: the ear bit (AND #40,
// BIT 6,A, RRA and AND #20, maybe XOR C between), which is a loader, else the
// key bits alone (AND with bits 0-4, or any mask without bit 6 before a rotate
// has moved it, BIT 0-4,A, OR #E0, AND (HL) with a mask
// from a table, RRA and a jump on the carry), which is a keyboard scan. The ear
// wins: a loader may test for BREAK first (BIT 0,A, AND #40, the edge loop's
// RRA / RET NC before its XOR C / AND #20). A CALL, JP or JR right after the IN
// is followed: the bits are tested where it goes (Havoc's key scan calls its
// BIT 0,A .. BIT 4,A). An AND r right after the IN is the same test with its
// mask in a register, read as it stands (Joe Blade 2's AND L, Kat Trap's AND D:
// a key bit each). pc is the address after the IN.
int zx_in_use(Computer* comp, int pc) {
	unsigned char b[9];
	int and20 = 0;
	int op = memRd(comp->mem, pc & 0xffff) & 0xff;
	if (((op & 0xf8) == 0xa0) && (op != 0xa6) && (op != 0xa7)) {
		CPU* cpu = comp->cpu;
		int regs[6] = {cpu->regB, cpu->regC, cpu->regD, cpu->regE, cpu->regH, cpu->regL};
		int mask = regs[op & 7] & 0xff;
		if (mask & 0x40) return ZX_IN_EAR;
		if (mask && !(mask & 0xe0)) return ZX_IN_KEYS;
	}
	if ((op == 0xcd) || (op == 0xc3)) {
		pc = (memRd(comp->mem, (pc + 1) & 0xffff) & 0xff) | ((memRd(comp->mem, (pc + 2) & 0xffff) & 0xff) << 8);
	} else if (op == 0x18) {
		pc += 2 + (signed char)memRd(comp->mem, (pc + 1) & 0xffff);
	}
	for (int i = 0; i < 9; i++)
		b[i] = memRd(comp->mem, (pc + i) & 0xffff) & 0xff;
	for (int i = 0; i < 8; i++)
		and20 |= (b[i] == 0xe6) && (b[i + 1] == 0x20);
	for (int i = 0; i < 6; i++) {
		unsigned char x = b[i];
		unsigned char y = b[i + 1];
		if (((x == 0xe6) && (y == 0x40)) || ((x == 0xcb) && (y == 0x77))) return ZX_IN_EAR;
		if ((x == 0x1f) && (((y == 0xe6) && (b[i + 2] == 0x20))
				|| ((y == 0xa9) && (b[i + 2] == 0xe6) && (b[i + 3] == 0x20))))
			return ZX_IN_EAR;
	}
	int rotated = 0;	// a rotate or shift came between the IN and here
	for (int i = 0; i < 6; i++) {
		unsigned char x = b[i];
		unsigned char y = b[i + 1];
		if ((x == 0xe6) && y && !(y & 0xe0)) return ZX_IN_KEYS;
		// a mask that leaves the ear bit out where it still is (St. Dragon's
		// AND #3F / CP #3F per half-row): bit 5 is the ear only after an RRA
		if ((x == 0xe6) && (y & 0x1f) && !(y & 0x40) && !rotated) return ZX_IN_KEYS;
		rotated |= (x == 0x1f) || (x == 0x0f) || (x == 0x17) || (x == 0x07) || (x == 0xcb);
		if ((x == 0xcb) && ((y & 0xc7) == 0x47) && (y < 0x68)) return ZX_IN_KEYS;
		if ((x == 0xf6) && (y == 0xe0)) return ZX_IN_KEYS;
		if (x == 0xa6) return ZX_IN_KEYS;
		if ((x == 0x1f) && !and20 && ((y == 0x30) || (y == 0x38) || ((y & 0xf7) == 0xd2) || ((y & 0xf7) == 0xd0)))
			return ZX_IN_KEYS;
	}
	return 0;
}

// what a #FE read owes the tape, for a machine whose port handler is its own
void zx_tape_detect(Computer* comp) {
	Tape* tap = comp->tape;
	int kind = TAPE_RD_EDGE;
	int ram = 0;
	int edge = zx_ld_edge(comp);
	if (edge == 2) {
		kind = TAPE_RD_EAR;		// the loader's own read of the ear bit, done by rom code
		ram = 1;
	} else if (!edge) {
		// A loader reads from one IN over and over, so what it is is asked
		// once a frame
		int pc = comp->cpu->regPC;
		if ((pc != tap->inPc) || (comp->frmCount != tap->inFrame)) {
			tap->inPc = pc;
			tap->inFrame = comp->frmCount;
			tap->inUse = zx_in_use(comp, pc);
		}
		ram = !zx_rom_code(comp, pc);
		kind = (tap->inUse == ZX_IN_KEYS) ? TAPE_RD_KEYS
			: ((ram && (tap->inUse == ZX_IN_EAR)) ? TAPE_RD_EAR : TAPE_RD_OTHER);
	}
	if (kind != TAPE_RD_KEYS)
		tap->portReads++;	// the rom's own reads too: fast loading counts them
	CPU* cpu = comp->cpu;
	unsigned char regs[7] = {(unsigned char)cpu->regA, (unsigned char)cpu->regB, (unsigned char)cpu->regC,
		(unsigned char)cpu->regD, (unsigned char)cpu->regE, (unsigned char)cpu->regH, (unsigned char)cpu->regL};
	tapDetectLoader(tap, comp->tickCount, cpu->regPC, regs, kind, ram);
}

int xInFE(Computer* comp, int port) {
	comp->keyb->port &= (port >> 8);
	unsigned char res = kbd_rd(comp->keyb, port) | 0xa0;		// set bits 7,5
	if (zx_ear(comp))
		res |= 0x40;
	zx_tape_detect(comp);
	return res;
}

// no chip, a chip not selected, or a mouse switched off: the port is left
// to the floating bus
int xInFFFD(Computer* comp, int port) {
	int res = (comp->ts->curChip->type == SND_NONE) ? -1 : tsIn(comp->ts, 0xfffd);
	return (res < 0) ? zx_in_float(comp, port) : res;
}

int xInFADF(Computer* comp, int port) {
	unsigned char res = 0xff;
	comp->mouse->used = 1;
	if (!comp->mouse->enable) return zx_in_float(comp, port);
	if (comp->mouse->hasWheel) {
		res &= 0x0f;
		res |= ((comp->mouse->wheel & 0x0f) << 4);
	}
	return res ^ mouse_buttons(comp->mouse);
}

int xInFBDF(Computer* comp, int port) {
	comp->mouse->used = 1;
	return comp->mouse->enable ? comp->mouse->xpos * comp->mouse->sensitivity : zx_in_float(comp, port);
}

int xInFFDF(Computer* comp, int port) {
	comp->mouse->used = 1;
	return comp->mouse->enable ? comp->mouse->ypos * comp->mouse->sensitivity : zx_in_float(comp, port);
}

// out

void xOutFE(Computer* comp, int port, int val) {
	int lev = (val & 0x10) ? 1 : 0;
	int mic = (val & 0x08) ? 1 : 0;
	comp->vid->nextbrd = (val & 0x07);
	if (comp->earRC && ((lev != comp->beep->lev) || (lev && (mic != comp->tape->levRec))))
		zx_ear_edge(comp, lev, mic);
	bc_out(comp->beep, lev, mic);
	comp->tape->levRec = mic;
}

void xOutBFFD(Computer* comp, int port, int val) {
	tsOut(comp->ts, 0xbffd, val);
}

void xOutFFFD(Computer* comp, int port, int val) {
	tsOut(comp->ts, 0xfffd, val);
}

#include "hardware.h"
#include "../xlog.h"
#include "../ldbytes.h"
#include "../filetypes/filetypes.h"
#include "../cpu/Z80/z80.h"

int compflags = 0;

// debug

int brkIn(Computer* comp, int port) {
	xlog(XLG_HW, XLL_ERROR, "IN %.4X (dos:rom:cpm = %i:%i:%i)",port,comp->flgDOS,comp->flgROM,comp->flgCPM);
	assert(0);
	//comp->brk = 1;
	return -1;
}

void brkOut(Computer* comp, int port, int val) {
	xlog(XLG_HW, XLL_ERROR, "OUT %.4X,%.2X (dos:rom:cpm = %i:%i:%i)",port,val,comp->flgDOS,comp->flgROM,comp->flgCPM);
	assert(0);
	//comp->brk = 1;
}

int dummyIn(Computer* comp, int port) {
	return -1;
}

void dummyOut(Computer* comp, int port, int val) {

}

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
	// devices. This runs once per instruction, so the ones that are idle or
	// not there at all are asked by the flag they check first, not by a call
	if (comp->dif->fdc->plan || comp->dif->doors)
		difSync(comp->dif, ns);
	if (comp->gs->enable)
		gsSync(comp->gs, ns);
	if (comp->saa->enabled)
		saaSync(comp->saa, ns);
	tsSync(comp->ts, ns);
	tapSync(comp->tape, ns);
	bcSync(comp->beep, ns);
	if (comp->keyb->per) kbd_sync(comp->keyb, ns);		// a key is held: ps/2 auto-repeat
	// nmi
	if ((comp->cpu->regPC > 0x3fff) && comp->flgNMIRQ) {
		comp->cpu->intrq |= Z80_NMI;	// request nmi
		comp->flgDOS = 1;			// set dos page
		comp->flgROM = 1;
		comp->hw->mapMem(comp);
	}
}

extern int res4;

// Memory contention. The delay for a bus cycle is read from the ULA table at
// the tick the cycle starts on and added whole, the way fuse does it. The
// older "step a tick at a time until the window frees" loop agrees with this
// for the Ferranti pattern, which counts down to zero, but not for the +2A/+3
// one (1,0,7,6,5,4,3,2), whose last slots reach past the end of the window.
// Video is already synced to cpu->t by comp_irq before we get here.
void zx_contend(Computer* comp, int mreq) {
	if (!comp->flgCNTM) return;
	MemPage* pg = mem_get_page(comp->mem, comp->cpu->adr);
	if (pg->type != MEM_RAM) return;
	int wdots = vid_wait_dots(comp->vid, pg->num << comp->mem->pgshift, mreq);
	if (!wdots) return;
	comp->cpu->t += ns_fixed_to_ticks_up(comp, (long long)wdots * comp->vid->nsPerDotFixed);
	vid_sync_lazy(comp->vid, ticks_to_ns_fixed(comp, comp->cpu->t - res4));
	res4 = comp->cpu->t;
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
	MemPage* pg = mem_get_page(comp->mem, adr);
	return (pg->type == MEM_RAM) ? ((pg->num << comp->mem->pgshift) >> 14) : 0;
}

void zx_snow(Computer* comp) {
	int bank = zx_bank_of(comp, comp->cpu->regI << 8);
	if (!(bank & 1)) return;		// not a contended bank
	if (!vid_snow(comp->vid, z80_get_r(comp->cpu),
			(bank & 4) | (comp->vid->vidPage & 2) | 1)) return;
	// Some 128K machines hang or reset under snow and others take it without a
	// murmur. Nobody has published the mechanism; what SpecEmu says about its
	// own switch is that such a machine goes unstable while it runs code out of
	// the slow memory, so that is the shape this takes - the cycle the ULA took
	// over leaves the ram addressed the way the ULA left it, and the next
	// opcode fetched from a slow bank comes back from the wrong place. memrd()
	// is the other half; it wants the same address bits the snow used.
	if (comp->flgSNOWX) comp->snowBad = 0x80 | (z80_get_r(comp->cpu) & 0x7f);
}

void zx_irq(Computer* comp, int t) {
	switch(t) {
		case IRQ_VID_INT:			// frame int start
#if HAVEZLIB
			if (!comp->rzx.play) {		// ignore when playing rzx
				vid_set_int_frame(comp->vid, comp->vid->intsize);
				comp->intVector = 0xff;
				comp->cpu->intrq |= Z80_INT;
			}
#else
			vid_set_int_frame(comp->vid, comp->vid->intsize);
			comp->intVector = 0xff;
			comp->cpu->intrq |= Z80_INT;
#endif
			break;
		case IRQ_RZX_INT:
			comp->intVector = 0xff;
			comp->cpu->intrq |= Z80_INT;
			vid_set_int_frame(comp->vid, comp->vid->intsize);
#if HAVEZLIB
			comp->rzx.fCurrent++;
			comp->rzx.fCount--;
			rzxGetFrame(comp);
#endif
			break;
		case IRQ_VID_IEND:			// frame int end (for tsconf see in tslab.c)
			comp->cpu->intrq &= ~Z80_INT;
			break;
		case IRQ_NMI:				// zx_sync takes it from here
			comp->flgNMIRQ = 1;
			break;
		case IRQ_CPU_CONT:			// memory cycle: contend it
			zx_contend(comp, 1);
			break;
		case IRQ_CPU_CONTNM:			// internal cycle, address on the bus
			zx_contend(comp, 0);
			break;
		case IRQ_CPU_ACK: {
			// an instruction that ends in an i/o cycle leaves the video at its
			// end, a tick past the one the cpu samples INT in: a pulse that
			// began inside that tick is not there yet
			int ahead = res4 - comp->cpu->t;
			// lazy: dots still counted cross no event, so they leave intFRAME
			// zero or not as it is, and a step back (ahead) walks them first
			vid_sync_lazy(comp->vid, ticks_to_ns_fixed(comp, comp->cpu->t - res4));
			res4 = comp->cpu->t;
			int act = comp->vid->intFRAME;
			if (act && (ahead > 0) && (comp->vid->intsize - act < ahead * comp->nsPerTickFixed / comp->vid->nsPerDotFixed))
				act = 0;
			comp->cpu->flgACK = !!act;
			// INT is a level: taken early in the pulse, it is taken again as soon
			// as interrupts are back on and the pulse is still there (fuse does
			// the same from EI). Butler's 128K timing tests count on it.
#if HAVEZLIB
			if (act && !comp->rzx.play)
#else
			if (act)
#endif
				comp->cpu->intrq |= Z80_INT;
			break;
		}
	}
}

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
	} else {
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
	if (gsWrite(comp->gs, adr, val)) return 1;
	if (!comp->flgBDI && saaWrite(comp->saa, adr, val)) return 1;
	if (!comp->flgBDI && sdrvWrite(comp->sdrv, adr, val)) return 1;
	if (ideOut(comp->ide, adr, val, comp->flgBDI)) return 1;
	if (ula_wr(comp->vid->ula, adr, val)) return 1;
	return 0;
}

int zx_dev_rd(Computer* comp, int adr, int* ptr) {
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
// and between the four reads the bus keeps the last byte that went to or came
// from contended memory instead of going to #FF. Worked out by Ast A. Moore
// and Hikaru in 2017 - sky.relative-path.com/zx/floating_bus.html.
int zx_in_float(Computer* comp, int port) {
	int res;
	switch (comp->fbus) {
		case FBUS_ULA:
			res = vid_float_bus(comp->vid);
			return (res < 0) ? 0xff : res;
		case FBUS_ASIC:
			if (((port & 0xf003) != 1) || (comp->p7FFD & 0x20)) return 0xff;
			res = vid_float_bus(comp->vid);
			if (res < 0) res = comp->fbusLast;
			return res | 1;
		case FBUS_ATTR:
			if ((port & 0xff) != 0xff) return 0xff;
			if (comp->vid->vbrd || comp->vid->hbrd) return 0xff;
			return vid_atrbyte(comp->vid);
	}
	return 0xff;
}

// The bus a +2A/+3 hands back between those fetches is the last byte that went
// to or came from contended memory, so those two machines watch their own
// accesses for it. `zx_bank_of() & 4` is banks 4-7, the set their gate array
// contends.
int asicMRd(Computer* comp, int adr, int m1) {
	int res = stdMRd(comp, adr, m1);
	if (zx_bank_of(comp, adr) & 4) comp->fbusLast = res & 0xff;
	return res;
}

void asicMWr(Computer* comp, int adr, int val) {
	if (zx_bank_of(comp, adr) & 4) comp->fbusLast = val & 0xff;
	stdMWr(comp, adr, val);
}

// bit 6 of #FE: a playing tape is the whole of it, otherwise the machine hears
// its own last out #FE, and how much of it is the issue 2 / issue 3 difference.
// A stopped tape is not the tape - volPlay keeps the level it stopped on, which
// this used to read as a permanent 1.
int zx_ear(Computer* comp) {
	if (comp->tape->on && !comp->tape->rec)
		return !!(comp->tape->volPlay & 0x80);
	switch (comp->earback) {
		case EAR_ISSUE2: return comp->beep->lev || comp->tape->levRec;
		case EAR_ISSUE3: return !!comp->beep->lev;
	}
	return 0;
}

// The rom's own loader polls #FE in exactly the pattern tapDetectLoader looks
// for, and the tape trap serves the rom - so the detector must not answer for
// it, or a flash load gets a tape playing under it and the two fall out of step.
// LD-EDGE-1/2 (#05E3..#05F9) is the only rom code that reads the port this way,
// and the same holds for a copy of LD-BYTES in ram that flash loading answers for.
// The address goes first: one field read, and false on every keyboard poll.
static int zx_ld_edge(Computer* comp) {
	int pc = comp->cpu->regPC;
	if (ld_edge(pc, LD_ROM_BASE) && zx_rom_active(comp)) return 1;
	return (comp->tape->ldBase >= 0) && ld_edge(pc, comp->tape->ldBase);
}

// The read is the rom's own, not a loader's: whatever is doing it is running
// from rom. Asked of the memory map rather than of the address, and of the map
// rather than of the 48 rom being in: a 128 sits in its editor rom while it
// starts a tape, and its keyboard poll is no more a loader than #05ED is.
static int zx_rom_code(Computer* comp) {
	return mem_get_page(comp->mem, comp->cpu->regPC)->type == MEM_ROM;
}

// What the code right after an IN from #FE looks at: the ear bit (AND #40,
// BIT 6,A, RRA and AND #20, maybe XOR C between), which is a loader, else the
// key bits alone (AND with bits 0-4, BIT 0-4,A, OR #E0), which is a keyboard
// scan. The ear wins: a loader may test for BREAK first (BIT 0,A, AND #40).
// pc is the address after the IN.
int zx_in_use(Computer* comp, int pc) {
	unsigned char b[9];
	for (int i = 0; i < 9; i++)
		b[i] = memRd(comp->mem, (pc + i) & 0xffff) & 0xff;
	for (int i = 0; i < 6; i++) {
		unsigned char x = b[i];
		unsigned char y = b[i + 1];
		if (((x == 0xe6) && (y == 0x40)) || ((x == 0xcb) && (y == 0x77))) return ZX_IN_EAR;
		if ((x == 0x1f) && (((y == 0xe6) && (b[i + 2] == 0x20))
				|| ((y == 0xa9) && (b[i + 2] == 0xe6) && (b[i + 3] == 0x20))))
			return ZX_IN_EAR;
	}
	for (int i = 0; i < 6; i++) {
		unsigned char x = b[i];
		unsigned char y = b[i + 1];
		if ((x == 0xe6) && y && !(y & 0xe0)) return ZX_IN_KEYS;
		if ((x == 0xcb) && ((y & 0xc7) == 0x47) && (y < 0x68)) return ZX_IN_KEYS;
		if ((x == 0xf6) && (y == 0xe0)) return ZX_IN_KEYS;
	}
	return 0;
}

// what a #FE read owes the tape, for a machine whose port handler is its own
void zx_tape_detect(Computer* comp) {
	Tape* tap = comp->tape;
	if (zx_ld_edge(comp)) {
		tap->portReads++;
		return;
	}
	// A keyboard scan is no loader, however it steps B: it neither starts the
	// tape nor counts for fast loading (Black Tiger's key definition). A loader
	// reads from one IN over and over, so what it is is asked once a frame.
	int pc = comp->cpu->regPC;
	if ((pc != tap->inPc) || (comp->frmCount != tap->inFrame)) {
		tap->inPc = pc;
		tap->inFrame = comp->frmCount;
		tap->inUse = zx_in_use(comp, pc);
	}
	int use = tap->inUse;
	if (use == ZX_IN_KEYS) return;
	tap->portReads++;	// the rom's own reads too: fast loading counts them
	int ram = !zx_rom_code(comp);
	// only a stopped tape needs it, only from a loader in ram, and only for a
	// read that came soon enough after the last to count at all
	int ear = ram && !tap->on && tap->detectOn
		&& (comp->tickCount - tap->detectLastTick <= 500) && (use == ZX_IN_EAR);
	tapDetectLoader(tap, comp->tickCount, comp->cpu->regB, ear, ram);
}

int xInFE(Computer* comp, int port) {
	comp->keyb->port &= (port >> 8);
	unsigned char res = kbd_rd(comp->keyb, port) | 0xa0;		// set bits 7,5
	if (zx_ear(comp))
		res |= 0x40;
	zx_tape_detect(comp);
	return res;
}

// no chip, or a mouse switched off: the port is left to the floating bus
int xInFFFD(Computer* comp, int port) {
	if (comp->ts->curChip->type == SND_NONE) return zx_in_float(comp, port);
	return tsIn(comp->ts, 0xfffd);
}

int xInFADF(Computer* comp, int port) {
	unsigned char res = 0xff;
	comp->mouse->used = 1;
	if (!comp->mouse->enable) return zx_in_float(comp, port);
	if (comp->mouse->hasWheel) {
		res &= 0x0f;
		res |= ((comp->mouse->wheel & 0x0f) << 4);
	}
	res ^= comp->mouse->mmb ? 4 : 0;
	if (comp->mouse->swapButtons) {
		res ^= comp->mouse->rmb ? 1 : 0;
		res ^= comp->mouse->lmb ? 2 : 0;
	} else {
		res ^= comp->mouse->lmb ? 1 : 0;
		res ^= comp->mouse->rmb ? 2 : 0;
	}
	return res;
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
	comp->vid->nextbrd = (val & 0x07);
	comp->beep->lev = (val & 0x10) ? 1 : 0;
	comp->tape->levRec = (val & 0x08) ? 1 : 0;
}

void xOutBFFD(Computer* comp, int port, int val) {
	tsOut(comp->ts, 0xbffd, val);
}

void xOutFFFD(Computer* comp, int port, int val) {
	tsOut(comp->ts, 0xfffd, val);
}

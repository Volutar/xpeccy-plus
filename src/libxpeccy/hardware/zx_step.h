#pragma once

// What every instruction of a ZX ends in, inline: for the quiet step, which calls
// them straight, and for the handlers that keep their names (zx_sync).

#include "hardware.h"
#include "../cpu/Z80/z80.h"

// INT sampled at the end of every instruction
static inline void zx_irq_ack_in(Computer* comp) {
	// an instruction that ends in an i/o cycle leaves the video at its
	// end, a tick past the one the cpu samples INT in: a pulse that
	// began inside that tick is not there yet
	int ahead = res4 - comp->cpu->t;
	// lazy: dots still counted cross no event, so they leave intFRAME
	// zero or not as it is, and a step back (ahead) walks them first
	comp_vid_to_t(comp);
	int act = comp->vid->intFRAME;
	if (act && (ahead > 0) && (comp->vid->intlen - act < ahead * comp->nsPerTickFixed / comp->vid->nsPerDotFixed))
		act = 0;
	comp->cpu->flgACK = !!act && !comp->rzx.play;
	// INT is a level: taken early in the pulse, it is taken again as soon
	// as interrupts are back on and the pulse is still there (fuse does
	// the same from EI). Butler's 128K timing tests count on it.
	if (act && !comp->rzx.play)
		comp->cpu->intrq |= Z80_INT;
}

// TSConf's flags (tslab.c names them flgVDOS, flgLINT, flgDINT)
#define TSL_FLG_VDOS	flag[1]
#define TSL_FLG_LINT	flag[2]		// 1 on vid->intLINE, 0 on line int ack
#define TSL_FLG_DINT	flag[3]		// (same for dma int)

// TSConf's (ts_irq): line and dma hold until acked; the cpu drops INT on every
// ack it takes
static inline void ts_irq_ack_in(Computer* comp) {
	zx_irq_ack_in(comp);
	if (comp->TSL_FLG_LINT || comp->TSL_FLG_DINT) {
		comp->cpu->flgACK = 1;
		comp->cpu->intrq |= Z80_INT;
	}
	if (comp->TSL_FLG_VDOS)
		comp->cpu->flgACK = 0;
}

// ATM's (atm2.c names it flgZ_I): the frame INT let through to the cpu
#define ATM_FLG_Z_I	flag[0]

// what atm2_sync() does before the devices: Z_I off masks the frame INT; the
// call costs an unlazy, so only when a pulse is actually up. intlen keeps its
// value either way - a snapshot saves it - this only cuts the live pulse.
static inline void atm2_step_in(Computer* comp) {
	if (!comp->ATM_FLG_Z_I && comp->vid->intFRAME)
		vid_set_int_frame(comp->vid, 0);
}

// The devices whose time can be put off and handed in at once (fast mode's run on):
// each takes it in any pieces alike as it stands now, beside its own sync below.
// Nothing but an i/o access changes that (comp_run_io ends a run, or checks again).
static inline int comp_devs_summable(Computer* comp) {
	if (!dif_quiet(comp->dif)) return 0;			// difSync: a command reads its own time
	// saaSync, tsSync, gsSync: they count the time, linearly - the GS's own cpu
	// runs it at its next flush, an access to its ports or a sub-sample
	// tapSync: at the normal speed; any other rounds per call
	if (comp->tape->on && (comp->tape->speed != 100)) return 0;
	// bcSync: settled where the port put it and no wave made; a transient rounds per call
	bitChan* bc = comp->beep;
	if ((bc->val != (bc->lev ? bc->hi : bc->lo)) || (bc->perH && bc->perL)) return 0;
	if (comp->keyb->per) return 0;				// kbd_sync: ps/2 repeat per step
	// kbd_combo_sync, nx_inp_sync: they count the time, and only an IN sees what they do
	return 1;
}

void nx_inp_sync(Computer*, int);

static inline void zx_devs_sync_in(Computer* comp, int ns) {
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
	if (comp->keyb->cbWait) kbd_combo_sync(comp->keyb, ns);	// a host key's combination: its key or shift to come
	if (comp->keyb->nxScan) nx_inp_sync(comp, ns);		// the Next's scan settling an extended key
}

// what alf_sync() hands the time to: the beeper and the chips (comp_devs_summable's)
static inline void alf_devs_sync_in(Computer* comp, int ns) {
	bcSync(comp->beep, ns);
	tsSync(comp->ts, ns);
}

static inline void zx_sync_in(Computer* comp, int ns) {
	zx_devs_sync_in(comp, ns);
	// nmi: a divMMC's button, or the Beta disk's "magic" one
	if (comp->div->model) {
		if (div_nmi(comp->div, comp->flgNMIRQ))
			comp->cpu->intrq |= Z80_NMI;
	} else if ((comp->cpu->regPC > 0x3fff) && comp->flgNMIRQ) {
		comp->cpu->intrq |= Z80_NMI;	// request nmi
		comp->flgDOS = 1;			// set dos page
		comp->flgROM = 1;
		comp->hw->mapMem(comp);
	}
}

// The end of an instruction (CPU.xend), where INT is sampled: the machine's kind of
// acknowledge (hwQuiet.ack). cpu->t stands one tick back, as z80_exec() leaves it.
void next_irq_ack(Computer*);
void next_nmi_m1(Computer*, int);

static inline void comp_ack_in(Computer* comp, int ack) {
	if (ack == HWQ_ACK_ZX)
		zx_irq_ack_in(comp);
	else if (ack == HWQ_ACK_TS)
		ts_irq_ack_in(comp);
	else if (ack == HWQ_ACK_NEXT)
		next_irq_ack(comp);
}

static inline int comp_ack_kind(Computer* comp) {
	return comp->hw->quiet ? comp->hw->quiet->ack : HWQ_ACK_NONE;
}

// one instruction (or interrupt) by z80_exec_open() and its end, T back
static inline int comp_exec_z80_in(Computer* comp, int ack) {
	CPU* cpu = comp->cpu;
	int end;
	int t = z80_exec_open(cpu, &end);
	if (end) {
		cpu->t--;
		comp_ack_in(comp, ack);
		cpu->t++;
		t = cpu->t;		// the Next's DMA may hold the bus at the instruction's end
	}
	return t;
}

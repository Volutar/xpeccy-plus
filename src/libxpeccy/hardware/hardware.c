#include "hardware.h"
#include <stdlib.h>
#include <string.h>

extern HardWare dum_hw_core;
extern HardWare z48_hw_core;
extern HardWare z128_hw_core;
extern HardWare alf_hw_core;
extern HardWare pnt_hw_core;
extern HardWare p1m_hw_core;
extern HardWare sco_hw_core;
extern HardWare scotp_hw_core;
extern HardWare atm_hw_core;
extern HardWare prf_hw_core;
extern HardWare prf3_hw_core;
extern HardWare phx_hw_core;
extern HardWare evo_hw_core;
extern HardWare tsl_hw_core;
extern HardWare pl2_hw_core;
extern HardWare pl3_hw_core;
extern HardWare nxt_hw_core;

// Order is the lineage: the Sinclair machines, then the clones that came from
// them. A NULL core is a separator in the machine list.
tabHwItem tabHwPtr[] = {
	{HW_DUMMY, &dum_hw_core},
	{HW_ZX48, &z48_hw_core},
	{HW_ZX128, &z128_hw_core},
	{HW_PLUS2A, &pl2_hw_core},
	{HW_PLUS3, &pl3_hw_core},
	{HW_ZXNEXT, &nxt_hw_core},	// the official heir: after the Sinclair/Amstrad line
	{HW_DUMMY, NULL},
	{HW_PENT, &pnt_hw_core},
	{HW_P1024, &p1m_hw_core},
	{HW_SCORP, &sco_hw_core},
	{HW_SCORPTP, &scotp_hw_core},
	{HW_ATM2, &atm_hw_core},
	{HW_PROFI3, &prf3_hw_core},
	{HW_PROFI, &prf_hw_core},
	{HW_PHOENIX, &phx_hw_core},
	{HW_PENTEVO, &evo_hw_core},
	{HW_TSLAB, &tsl_hw_core},
	{HW_DUMMY, NULL},
	{HW_ALF, &alf_hw_core},
	{HW_NULL, NULL},
};

// What each 16K bank of a core's ROM is, and which reset lands in it. A core
// whose reset boots its own firmware takes no reset target at all.

static const struct {
	int hw;
	xRomRole bank[4];
} romRoles[] = {
	{HW_ZX48, {{RES_48, "Basic 48"}, {RES_DOS, "TR-DOS"}, {-1, NULL}, {-1, NULL}}},
	{HW_ZX128, {{RES_128, "Basic 128"}, {RES_48, "Basic 48"}, {RES_SHADOW, "Service"}, {RES_DOS, "TR-DOS"}}},
	{HW_PENT, {{RES_128, "Basic 128"}, {RES_48, "Basic 48"}, {RES_SHADOW, "Service"}, {RES_DOS, "TR-DOS"}}},
	{HW_P1024, {{RES_128, "Basic 128"}, {RES_48, "Basic 48"}, {RES_SHADOW, "Service"}, {RES_DOS, "TR-DOS"}}},
	{HW_SCORP, {{RES_128, "Basic 128"}, {RES_48, "Basic 48"}, {RES_SHADOW, "Service"}, {RES_DOS, "TR-DOS"}}},
	{HW_SCORPTP, {{RES_128, "Basic 128"}, {RES_48, "Basic 48"}, {RES_SHADOW, "Service"}, {RES_DOS, "TR-DOS"}}},
	{HW_PLUS2A, {{RES_128, "Basic 128"}, {-1, "Syntax 128"}, {-1, "+3DOS"}, {RES_48, "Basic 48"}}},
	{HW_PLUS3, {{RES_128, "Basic 128"}, {-1, "Syntax 128"}, {-1, "+3DOS"}, {RES_48, "Basic 48"}}},
	{HW_PROFI, {{RES_SHADOW, "Service"}, {RES_DOS, "TR-DOS"}, {RES_128, "Basic 128"}, {RES_48, "Basic 48"}}},
	{HW_PROFI3, {{RES_SHADOW, "Service"}, {RES_DOS, "TR-DOS"}, {RES_128, "Basic 128"}, {RES_48, "Basic 48"}}},
	// a reset to the service page lands on TR-DOS here, so it is no target
	{HW_PHOENIX, {{-1, NULL}, {RES_DOS, "TR-DOS"}, {RES_128, "Basic 128"}, {RES_48, "Basic 48"}}},
	{HW_ATM2, {{-1, "Firmware"}, {-1, NULL}, {-1, NULL}, {-1, NULL}}},
	{HW_PENTEVO, {{-1, "Firmware"}, {-1, NULL}, {-1, NULL}, {-1, NULL}}},
	{HW_TSLAB, {{-1, "Firmware"}, {-1, NULL}, {-1, NULL}, {-1, NULL}}},
	{HW_ALF, {{-1, "Menu"}, {-1, "Basic 48"}, {-1, NULL}, {-1, NULL}}},
	{HW_ZXNEXT, {{-1, "Firmware"}, {-1, NULL}, {-1, NULL}, {-1, NULL}}},
	{HW_NULL, {{-1, NULL}, {-1, NULL}, {-1, NULL}, {-1, NULL}}}
};

xRomRole hw_rom_role(int hw, int bank) {
	int i = 0;
	while ((romRoles[i].hw != HW_NULL) && (romRoles[i].hw != hw)) i++;
	return romRoles[i].bank[bank & 3];		// an unknown core gets the empty last row
}

// the bank a reset lands in on this core, -1 when it has no such target
int hw_reset_bank(int hw, int res) {
	for (int i = 0; i < 4; i++) {
		if (hw_rom_role(hw, i).res == res) return i;
	}
	return -1;
}

HardWare* findHardwareId(int id) {
	for (tabHwItem* itm = tabHwPtr; itm->id != HW_NULL; itm++) {
		if (itm->core && (itm->core->id == id))
			return itm->core;
	}
	return NULL;
}

HardWare* findHardware(const char* name) {
	tabHwItem* itm = tabHwPtr;
	HardWare* hw = NULL;
	while((itm->id != HW_NULL) && !hw) {
		if (itm->core) {
			if (!strcmp(itm->core->name, name)) {
				hw = itm->core;
			}
		}
		itm++;
	}
	return hw;
}

// mem

int stdMRd(Computer* comp, int adr, int m1) {
	MemPage* pg = mem_get_page(comp->mem, adr);
	if (m1 && (comp->dif->type == DIF_BDI)) {
		// out of TR-DOS first, and then maybe back into it
		if (comp->flgDOS && bdi_fetch_pages(comp, pg, adr)) {
			comp->flgDOS = 0;
			comp->hw->mapMem(comp);
			comp->cpu->m1p = NULL;		// the fetch table was the other one (comp_mem_direct)
		}
		if (!comp->flgDOS && bdi_fetch_pages(comp, pg, adr)) {
			comp->flgDOS = 1;
			comp->hw->mapMem(comp);
			comp->cpu->m1p = NULL;
		}
	}
	return mem_page_rd(pg, adr);
}

void stdMWr(Computer *comp, int adr, int val) {
	mem_wr_fast(comp->mem, adr, val);
}

// io

int hwIn(xPort* ptab, Computer* comp, int port) {
	int res = -1;
	int idx = 0;
	int catch = 0;
	xPort* itm;
	do {
		itm = &ptab[idx];
		if (((port & itm->mask) == (itm->value & itm->mask)) &&\
				(itm->in != NULL) &&\
				((itm->dos & 2) || (itm->dos == comp->flgBDI)) &&\
				((itm->rom & 2) || (itm->rom == comp->flgROM)) &&\
				((itm->cpm & 2) || (itm->cpm == comp->flgCPM))) {
			res = itm->in(comp, port);
			catch = !!itm->mask;
		}
		idx++;
	} while (!catch && (itm->mask != 0));
	if (!catch && (compflags & CFLG_PANIC)) {
		comp_irq(IRQ_STOP, comp);
	}
	return res;
}

void hwOut(xPort* ptab, Computer* comp, int port, int val, int mult) {
	int idx = 0;
	int catch = 0;
	xPort* itm;
	do {
		itm = &ptab[idx];
		if (((port & itm->mask) == (itm->value & itm->mask)) &&\
				(itm->out != NULL) &&\
				((itm->dos & 2) || (itm->dos == comp->flgBDI)) &&\
				((itm->rom & 2) || (itm->rom == comp->flgROM)) &&\
				((itm->cpm & 2) || (itm->cpm == comp->flgCPM))) {
			itm->out(comp, port, val);
			catch |= !mult;
		}
		idx++;
	} while ((itm->mask != 0) && !catch);
	if (!catch && (compflags & CFLG_PANIC)) {
		comp_irq(IRQ_STOP, comp);
	}
}

// The machines a divMMC is made for: /ROMCS on the edge connector, and the
// +2A/+3's second ROM enable through the board's jumper. The Pentagons take it
// on ZX-BUS, esxDOS names them. Their own Beta Disk traps #3Dxx too; the board's
// /ROMCS wins there, as it does over a Beta 128 on a 128K (seen by the user).
// The machines with the SD slot read through comp->sdc: a card in it means nothing to the others
int hw_has_sd(const HardWare* hw) {
	if (!hw) return 0;
	return (hw->id == HW_PENTEVO) || (hw->id == HW_TSLAB) || (hw->id == HW_ZXNEXT);
}

// no Fire 2..4 on the Scorpions' #1F (scrpIn1F)
int hw_kempston8(const HardWare* hw) {
	if (!hw) return 1;
	return (hw->id != HW_SCORP) && (hw->id != HW_SCORPTP);
}

// the Gluk clock: an add-on made for the Pentagon, on the P1024's own board
int hw_takes_gluk(const HardWare* hw) {
	return hw && ((hw->id == HW_PENT) || (hw->id == HW_P1024));
}

// KOE names KAY, Scorpion and Pentagon-1024SL for its slot (ZC.pdf); a Pentagon 128
// has the same bus
int hw_takes_zc(const HardWare* hw) {
	if (!hw) return 0;
	switch (hw->id) {
		case HW_PENT: case HW_P1024: case HW_SCORP: case HW_SCORPTP:
			return 1;
	}
	return 0;
}

int comp_has_sd(const Computer* comp) {
	if (!comp) return 0;
	return hw_has_sd(comp->hw) || comp->zc->on;
}

int hw_takes_divmmc(const HardWare* hw) {
	if (!hw) return 0;
	switch (hw->id) {
		case HW_ZX48: case HW_ZX128: case HW_PLUS2A: case HW_PLUS3:
		case HW_PENT: case HW_P1024:
			return 1;
	}
	return 0;
}

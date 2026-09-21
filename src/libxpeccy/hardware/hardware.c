#include "hardware.h"
#include <stdlib.h>
#include <string.h>

int hwflags = 0;

extern HardWare dum_hw_core;
extern HardWare z48_hw_core;
extern HardWare z128_hw_core;
extern HardWare alf_hw_core;
extern HardWare pnt_hw_core;
extern HardWare p1m_hw_core;
extern HardWare sco_hw_core;
extern HardWare atm_hw_core;
extern HardWare prf_hw_core;
extern HardWare phx_hw_core;
extern HardWare evo_hw_core;
extern HardWare tsl_hw_core;
extern HardWare pl2_hw_core;
extern HardWare pl3_hw_core;

// Order is the lineage: the Sinclair machines, then the clones that came from
// them. A NULL core is a separator in the machine list.
tabHwItem tabHwPtr[] = {
	{HW_DUMMY, &dum_hw_core},
	{HW_ZX48, &z48_hw_core},
	{HW_ZX128, &z128_hw_core},
	{HW_PLUS2A, &pl2_hw_core},
	{HW_PLUS3, &pl3_hw_core},
	{HW_DUMMY, NULL},
	{HW_PENT, &pnt_hw_core},
	{HW_P1024, &p1m_hw_core},
	{HW_SCORP, &sco_hw_core},
	{HW_ATM2, &atm_hw_core},
	{HW_PROFI, &prf_hw_core},
	{HW_PHOENIX, &phx_hw_core},
	{HW_PENTEVO, &evo_hw_core},
	{HW_TSLAB, &tsl_hw_core},
	{HW_DUMMY, NULL},
	{HW_ALF, &alf_hw_core},
	{HW_NULL, NULL},
};

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

static MemPage* pg;

int stdMRd(Computer* comp, int adr, int m1) {
	pg = mem_get_page(comp->mem, adr);	// = &comp->mem->map[(adr >> 8) & 0xff];
	if (m1 && (comp->dif->type == DIF_BDI)) {
		if (comp->flgDOS && (pg->type == MEM_RAM)) {
			comp->flgDOS = 0;
			comp->hw->mapMem(comp);
		}
		if (!comp->flgDOS && ((adr & 0x3f00) == 0x3d00) && comp->flgROM && (pg->type == MEM_ROM)) {
			comp->flgDOS = 1;
			comp->hw->mapMem(comp);
		}
	}
	// the page is in hand, so the read goes straight at it rather than
	// through memRd(), which would look the same page up again
	return pg->rd ? (pg->rd(adr & 0xffff, pg->data) & 0xff) : 0xff;
}

void stdMWr(Computer *comp, int adr, int val) {
	pg = mem_get_page(comp->mem, adr);	// = &comp->mem->map[(adr >> 8) & 0xff];
	if (pg->wr) pg->wr(adr, val, pg->data);
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

// max 32 ports
xPortValue pvTab[33];

xPortValue* hwGetPorts(Computer* comp) {
	int i = 0;
	xPortDsc* tab = comp->hw->portab;
	if (tab) {
		void* ptr;
		while ((tab[i].port > 0) && (i < 33)) {
			pvTab[i].port = tab[i].port;
			if (tab[i].offset) {
				ptr = ((void*)comp) + tab[i].offset;
				switch(tab[i].type) {
					case REG_BYTE: pvTab[i].value = *((unsigned char*)ptr) & 0xff; break;
					case REG_WORD: pvTab[i].value = *((unsigned short*)ptr) & 0xffff; break;
					case REG_32: pvTab[i].value = *((unsigned int*)ptr); break;
					default: pvTab[i].value = *((unsigned char*)ptr) & 0xff; break;
				}
			} else if (comp->hw->in) {
				pvTab[i].value = comp->hw->in(comp, pvTab[i].port);
			}
			i++;
		}
	}
	pvTab[i].port = -1;
	return pvTab;
}

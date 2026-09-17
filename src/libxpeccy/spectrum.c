#include <stdio.h>
#include "xlog.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "spectrum.h"
#include "filetypes/filetypes.h"
#include "cpu/Z80/z80.h"

static int nsTime;
static int res2;
int res4 = 0;		// save last T synced with video (last is res2-res4)

unsigned char* comp_get_memcell_flag_ptr(Computer* comp, int adr) {
	unsigned char* res = NULL;
	xAdr xadr = mem_get_xadr(comp->mem, adr);
	switch (xadr.type) {
		case MEM_RAM: res = comp->brkRamMap + (xadr.abs & comp->mem->ramMask); break;
		case MEM_ROM: res = comp->brkRomMap + (xadr.abs & comp->mem->romMask); break;
		case MEM_SLOT: if (comp->slot->brkMap) {res = comp->slot->brkMap + (xadr.abs & comp->slot->memMask);} break;
	}
	return res;
}

bpChecker comp_check_bp(Computer* comp, int adr, int mask) {
	bpChecker ch;
	ch.t = -1;
	ch.ptr = NULL;
	if (comp->mem->busmask < 0x10000) {
		ch.ptr = comp->brkAdrMap + (adr & comp->mem->busmask);
		if (*ch.ptr & mask) {
			ch.t = BRK_CPUADR;
			ch.a = adr & comp->mem->busmask;
		}
	}
	if (ch.t < 0) {
		xAdr xadr = mem_get_xadr(comp->mem, adr);
		switch (xadr.type) {
			case MEM_RAM: xadr.abs &= comp->mem->ramMask; ch.ptr = comp->brkRamMap + xadr.abs; ch.t = BRK_MEMRAM; break;
			case MEM_ROM: xadr.abs &= comp->mem->romMask; ch.ptr = comp->brkRomMap + xadr.abs; ch.t = BRK_MEMROM; break;
			case MEM_SLOT: if (comp->slot->brkMap) {
					xadr.abs &= comp->slot->memMask;
					ch.ptr = comp->slot->brkMap + xadr.abs;
					ch.t = BRK_MEMSLT;
				} break;
		}
		if (ch.ptr) {
			if (*ch.ptr & mask) {
				// printf("xadr.abs = %X, page=%X, off=%X\n",xadr.abs,xadr.bank,xadr.adr & comp->mem->pgmask);
				ch.a = xadr.abs;
			} else {
				ch.t = -1;
			}
		}
	}
	return ch;
}

// video callbacks

int vid_mrd_cb(int adr, void* ptr) {
	Computer* comp = (Computer*)ptr;
	return comp->mem->ramData[adr & comp->mem->ramMask];
}

// mrw,irw

int memrd(int adr, int m1, void* ptr) {
	Computer* comp = (Computer*)ptr;
	adr &= comp->cpu->busmask;
#ifdef HAVEZLIB
	if (m1 && comp->rzx.play && (comp->rzx.frm.fetches > 0)) {
		comp->rzx.frm.fetches--;
	}
#endif
	unsigned char* fptr = comp_get_memcell_flag_ptr(comp, adr);
	unsigned isExecByte = (cpu_get_pc(comp->cpu) - 1) == adr;
	if (fptr) {
		unsigned char flag = *fptr;
		if (comp->flgMAP) {
			if (isExecByte) {
				flag &= 0x0f;
				flag |= DBG_VIEW_EXEC;
				*fptr = flag;
			} else if (!(flag & 0xf0)) {
				flag |= DBG_VIEW_BYTE;
				*fptr = flag;
			}
		}
	}
	if (comp->flgHEAT && !x_runahead) {	// an ahead frame would count every access twice
		// a byte is part of the instruction stream (opcode, prefix, immediate/displacement
		// operand) whenever it lands right where PC just advanced past it; everything else
		// is a real data read (memory operand, stack pop, etc)
		comp_heat_hit(comp, adr, isExecByte ? HEAT_EX : HEAT_RD);
	}
	bpChecker ch = comp_check_bp(comp, adr, MEM_BRK_RD);
	if (ch.t >= 0) {
		comp->flgBRK = 1;
		comp->brkt = ch.t;
		comp->brka = ch.a;
	}
	// the ULA took a refresh cycle from under this one and this machine's ram
	// cannot take that (see zx_snow): the ram is left addressed the way the ULA
	// left it, so an opcode fetched from a slow bank comes back from the wrong
	// place - bits 6-0 out of R, exactly as the snow itself reads. Only the
	// fetch is spoiled, and only in a slow bank: code running anywhere else is
	// untouched, which is why a game that loads into the upper banks survives.
	int radr = adr;
	if (comp->snowBad) {
		int low = comp->snowBad & 0x7f;
		comp->snowBad = 0;
		if (m1 && (zx_bank_of(comp, adr) & 1))
			radr = (adr & ~0x7f) | low;
	}
	int res = comp->hw->mrd(comp,radr,m1);
	// instruction bytes are not a data read, don't latch them as RD
	if (comp->flgCOND && !isExecByte) {
		comp->brkev.rd = adr;
		comp->brkev.mdt = res & 0xff;
	}
	return res;
}

void memwr(int adr, int val, void* ptr) {
	Computer* comp = (Computer*)ptr;
	adr &= comp->cpu->busmask;
	if (comp->flgHEAT && !x_runahead) {	// an ahead frame would count every access twice
		// writes (incl. stack push/call) are always data traffic, never execution
		comp_heat_hit(comp, adr, HEAT_WR);
	}
	unsigned char* fptr = comp_get_memcell_flag_ptr(comp, adr);
	if (fptr) {
		unsigned char flag = *fptr;
		if (comp->flgMAP) {
			if (!(flag & 0xf0)) {
				flag |= DBG_VIEW_BYTE;
				*fptr = flag;
			}
		}
		bpChecker ch = comp_check_bp(comp, adr, MEM_BRK_WR);
		if (ch.t >= 0) {
			comp->flgBRK = 1;
			comp->brkt = ch.t;
			comp->brka = ch.a;
		}
	}
	if (comp->flgCOND) {
		comp->brkev.wr = adr;
		comp->brkev.mdt = val & 0xff;
	}
	comp->hw->mwr(comp,adr,val);
}

// i/o contention uses the no-mreq table (fuse: ula_contend_port_early/late),
// so the +2A/+3 asic contends no i/o at all. The address is a stand-in for
// "a contended page" - the caller has already looked at the port itself.
//
// The i/o window sits two ticks later than the memory one. Fuse anchors both
// at the same tstate, but ula128.tap - which matches a real 128K and
// Spectaculator pixel for pixel - only comes out right this way, so the test
// wins over the model until we know why they differ.
#define IO_CONT_DOTS (-4)

void zx_cont_delay(Computer* comp) {
	long long wns = vid_wait_dots(comp->vid, 5 << 14, 0, IO_CONT_DOTS) * (long long)comp->vid->nsPerDotFixed;	// video is already at end of wait cycle
	int t0 = comp->cpu->t;
	comp->cpu->t += ns_fixed_to_ticks_up(comp, wns);
	vid_sync_fixed(comp->vid, ticks_to_ns_fixed(comp, comp->cpu->t - t0));
	res4 = comp->cpu->t;
}

void zx_free_ticks(Computer* comp, int t) {
	comp->cpu->t += t;
	vid_sync_fixed(comp->vid, ticks_to_ns_fixed(comp, t));
	res4 = comp->cpu->t;
}

// Contention on T1
void zx_cont_t1(Computer* comp, int port) {
	if ((port & 0xc000) == 0x4000)
		zx_cont_delay(comp);
	zx_free_ticks(comp, 1);
}

// Contention on T2-T4
void zx_cont_tn(Computer* comp, int port) {
	// zx_cont_t1 took the first of the four slots, three are left here
	if ((port & 0xc000) == 0x4000) {
		if (port & 1) {			// C:1 C:1 C:1 C:1
			zx_cont_delay(comp);
			zx_free_ticks(comp, 1);
			zx_cont_delay(comp);
			zx_free_ticks(comp, 1);
			zx_cont_delay(comp);
			zx_free_ticks(comp, 1);
		} else {			// C:1 C:3
			zx_cont_delay(comp);
			zx_free_ticks(comp, 3);
		}
	} else if (port & 1) {			// N:4
		zx_free_ticks(comp, 4-1);
	} else {				// N:1 C:3
		zx_cont_delay(comp);
		zx_free_ticks(comp, 3);
	}
}

static unsigned char ula_levs[8] = {0x00, 0x24, 0x49, 0x6d, 0x92, 0xb6, 0xdb, 0xff};

void zxSetUlaPalete(Computer* comp) {
	int i;
	int col;
	xColor xc;
	for (i = 0; i < 64; i++) {
		col = (comp->vid->ula->pal[i] << 1) & 7;	// blue
		if (col & 2) col |= 1;
		xc.b = ula_levs[col];
		col = (comp->vid->ula->pal[i] >> 2) & 7;	// red
		xc.r = ula_levs[col];
		col = (comp->vid->ula->pal[i] >> 5) & 7;	// green
		xc.g = ula_levs[col];
		vid_set_col(comp->vid, i, xc);
	}
}

// the debugger shows the last value that went through a watched port. Reading
// the port again when the emulation stops is not the same thing: half the ports
// answer with an open bus, and some clear a flag when read. Ports the machine
// keeps a copy of need none of this, so they are left out of the count and the
// hot path costs nothing until a port of one's own is watched

static void pwatch_recount(Computer* comp) {
	int i;
	comp->pwbus = 0;
	for (i = 0; i < comp->pwcount; i++) {
		if (comp->pwatch[i].on && !comp->pwatch[i].offset)
			comp->pwbus++;
	}
}

static void pwatch_hit(Computer* comp, int port, int val) {
	int i;
	for (i = 0; i < comp->pwcount; i++) {
		if (!comp->pwatch[i].on || comp->pwatch[i].offset) continue;
		if ((port & comp->pwatch[i].mask) != comp->pwatch[i].port) continue;
		comp->pwatch[i].val = val & 0xff;
	}
}

void comp_pwatch_clear(Computer* comp) {
	comp->pwcount = 0;
	comp->pwbus = 0;
}

// mask tells a byte port (0xff, matched on the low byte alone) from an address
// on the bus (0xffff); whoever read the port from text decides which it is

void comp_pwatch_add(Computer* comp, int port, int mask, int on) {
	if ((comp->pwcount < PWATCH_MAX) && mask) {
		comp->pwatch[comp->pwcount].port = port & mask;
		comp->pwatch[comp->pwcount].mask = mask;
		comp->pwatch[comp->pwcount].type = 0;
		comp->pwatch[comp->pwcount].offset = 0;
		comp->pwatch[comp->pwcount].on = on ? 1 : 0;
		comp->pwatch[comp->pwcount].hw = 0;
		comp->pwatch[comp->pwcount].val = -1;
		comp->pwcount++;
		pwatch_recount(comp);
	}
}

// the ports this machine keeps by itself belong in the same list, so they can
// be switched off like any other: whatever isn't there yet is added, and the
// ones this machine has no more leave again. Run on a profile load and every
// time the machine changes. A port the user asked for himself stays either way

void comp_pwatch_sync(Computer* comp) {
	xPortDsc* tab = comp->hw ? comp->hw->portab : NULL;
	int i, j;
	for (i = 0; i < comp->pwcount; i++) {
		comp->pwatch[i].type = 0;
		comp->pwatch[i].offset = 0;
	}
	for (i = comp->pwcount - 1; i >= 0; i--) {
		if (!comp->pwatch[i].hw) continue;
		for (j = 0; tab && (tab[j].port > 0) && (tab[j].port != comp->pwatch[i].port); j++);
		if (tab && (tab[j].port > 0)) continue;
		for (j = i; j < comp->pwcount - 1; j++) {
			comp->pwatch[j] = comp->pwatch[j + 1];
		}
		comp->pwcount--;
	}
	for (i = 0; tab && (tab[i].port > 0); i++) {
		for (j = 0; j < comp->pwcount; j++) {
			if (comp->pwatch[j].port == tab[i].port) break;
		}
		if (j == comp->pwcount) {
			comp_pwatch_add(comp, tab[i].port, 0xffff, 1);
			if (j == comp->pwcount) break;		// no room left
		}
		comp->pwatch[j].hw = 1;
		comp->pwatch[j].type = tab[i].type;
		comp->pwatch[j].offset = tab[i].offset;
	}
	pwatch_recount(comp);
}

// what to show for the port: its own register when the machine has one,
// the last value on the bus otherwise

int comp_pwatch_val(Computer* comp, int idx) {
	void* ptr;
	if ((idx < 0) || (idx >= comp->pwcount)) return -1;
	if (!comp->pwatch[idx].offset) return comp->pwatch[idx].val;
	ptr = ((void*)comp) + comp->pwatch[idx].offset;
	switch(comp->pwatch[idx].type) {
		case REG_WORD: return *((unsigned short*)ptr) & 0xffff;
		case REG_32: return *((unsigned int*)ptr);
	}
	return *((unsigned char*)ptr) & 0xff;
}

int iord(int port, void* ptr) {
	Computer* comp = (Computer*)ptr;
	if (comp->flgCNTI) {
		// fuse reads the port after both halves of the i/o cycle have
		// been contended (periph.c readport), so do the waiting first
		vid_sync_fixed(comp->vid, ticks_to_ns_fixed(comp, comp->cpu->t - res4));
		res4 = comp->cpu->t;
		zx_cont_t1(comp, port);
		zx_cont_tn(comp, port);
		comp->cpu->t -= 4;		// z80_iord puts the four back
	} else {
		vid_sync_fixed(comp->vid, ticks_to_ns_fixed(comp, comp->cpu->t + 3 - res4));
		res4 = comp->cpu->t + 3;
	}
// play rzx
#ifdef HAVEZLIB
	int res = 0xff;
	if (comp->rzx.play) {
		if (comp->rzx.frm.pos < comp->rzx.frm.size) {
			res = comp->rzx.frm.data[comp->rzx.frm.pos];
			comp->rzx.frm.pos++;
			return res;
		} else {
			rzxStop(comp);
			xlog(XLG_CORE, XLL_DEBUG, "overIO frame %i pc=%04X port=%04X size=%i fetch-left %i",
				comp->rzx.fCurrent, cpu_get_pc(comp->cpu), port, comp->rzx.frm.size, comp->rzx.frm.fetches);
			comp->rzx.overio = 1;
			return 0xff;
		}
	}
#endif
	comp->flgBDI = (comp->flgDOS && (comp->dif->type == DIF_BDI)) ? 1 : 0;
// brk
	if (comp->brkIOMap[port] & MEM_BRK_RD) {
		comp->flgBRK = 1;
		comp->brkt = BRK_IOPORT;
		comp->brka = port;
	}

	res = comp->hw->in ? comp->hw->in(comp, port) : 0xff;
	if (comp->flgCOND) {
		comp->brkev.in = port;
		comp->brkev.val = res & 0xff;
	}
	if (comp->pwbus) pwatch_hit(comp, port, res);
	return res;
}

void iowr(int port, int val, void* ptr) {
	Computer* comp = (Computer*)ptr;
	comp->flgBDI = (comp->flgDOS && (comp->dif->type == DIF_BDI)) ? 1 : 0;
	// sync video to current T
	vid_sync_fixed(comp->vid, ticks_to_ns_fixed(comp, comp->cpu->t - res4));
	res4 = comp->cpu->t;
	if (comp->flgCNTI) {
		zx_cont_t1(comp, port);
		comp->hw->out(comp, port, val);
		zx_cont_tn(comp, port);
		comp->cpu->t -= 4;
	} else {
		vid_sync_fixed(comp->vid, comp->nsPerTickFixed);
		res4++;
		comp->hw->out(comp, port, val);
	}
	if (comp->vid->ula->palchan) {
		zxSetUlaPalete(comp);
		comp->vid->ula->palchan = 0;
	}
// brk
	if (comp->brkIOMap[port] & MEM_BRK_WR) {
		comp->flgBRK = 1;
		comp->brkt = BRK_IOPORT;
		comp->brka = port;
	}
	if (comp->flgCOND) {
		comp->brkev.out = port;
		comp->brkev.val = val & 0xff;
	}
	if (comp->pwbus) pwatch_hit(comp, port, val);
}

int intrq(void* ptr) {
	Computer* comp = (Computer*)ptr;
	return comp->hw->ack ? comp->hw->ack(comp) : 0xff;
}

void comp_irq(int t, void* ptr) {
	Computer* comp = (Computer*)ptr;
	switch (t) {
		case IRQ_BRK:
			comp_brk(comp, -1);
			break;
		case IRQ_STOP:
			comp_brk(comp, -2);
			break;
		case IRQ_PANIC:
			comp_irq((compflags & CFLG_PANIC) ? IRQ_STOP : IRQ_BRK, comp);		// if --panic, stop, else brk
			break;
		case IRQ_CPU_HALT:
			comp->hCount = comp->frmtCount;			// fix T counter from INT to start of HALT
			break;
		case IRQ_VID_INT:
			comp->fCount = comp_frame_ticks(comp);
			comp->frmtCount = 0;
			if (!comp->cpu->flgHALT) {
				comp->hCount = comp->fCount;		// if not HALT-ed during frame, count all ticks
			}
			break;
		case IRQ_CPU_SYNC:
			vid_sync_fixed(comp->vid, ticks_to_ns_fixed(comp, comp->cpu->t - res4));
			res4 = comp->cpu->t;
			return;			// no machine acts on a plain sync
		case IRQ_CPU_CONT:
		case IRQ_CPU_CONTNM:
			vid_sync_fixed(comp->vid, ticks_to_ns_fixed(comp, comp->cpu->t - res4));
			res4 = comp->cpu->t;
			break;
		case IRQ_CPU_RFSH:
			vid_sync_fixed(comp->vid, ticks_to_ns_fixed(comp, comp->cpu->t - res4));
			res4 = comp->cpu->t;
			zx_snow(comp);
			return;			// not a machine's business
	}
	if (comp->hw->irq) comp->hw->irq(comp, t);
}

// new (for future use)

/*
int comp_rom_rd(Computer* comp, int adr) {
	adr &= comp->mem->romMask;
	if (comp->brkRomMap[adr] & MEM_BRK_RD)
		comp->brk = 1;
	return comp->mem->romData[adr] & 0xff;
}

void comp_rom_wr(Computer* comp, int adr, int val) {
	adr &= comp->mem->romMask;
	if (comp->brkRomMap[adr] & MEM_BRK_WR)
		comp->brk = 1;
	comp->mem->romData[adr] = val & 0xff;
}

int comp_ram_rd(Computer* comp, int adr) {
	adr &= comp->mem->ramMask;
	if (comp->brkRamMap[adr] & MEM_BRK_RD)
		comp->brk = 1;
	return comp->mem->ramData[adr] & 0xff;
}

void comp_ram_wr(Computer* comp, int adr, int val) {
	adr &= comp->mem->ramMask;
	if (comp->brkRamMap[adr] & MEM_BRK_WR)
		comp->brk = 1;
	comp->mem->ramData[adr] = val & 0xff;
}

int comp_slt_rd(Computer* comp, int adr) {
	if (!comp->slot->data) return 0xff;
	adr &= comp->slot->memMask;
	if (comp->slot->brkMap[adr] & MEM_BRK_RD)
		comp->brk = 1;
	return comp->slot->data[adr] & 0xff;
}

void comp_slt_wr(Computer* comp, int adr, int val) {
	if (!comp->slot->data) return;
	adr &= comp->slot->memMask;
	if (comp->slot->brkMap[adr] & MEM_BRK_WR)
		comp->brk = 1;
	comp->slot->data[adr] = val & 0xff;
}
*/

// rzx

void rzxStop(Computer* zx) {
#ifdef HAVEZLIB
	zx->rzx.play = 0;
	if (zx->rzx.file) fclose(zx->rzx.file);
	zx->rzx.file = NULL;
	zx->rzx.fCount = 0;
	zx->rzx.frm.size = 0;
	zx->rzx.stop = 1;
#endif
}

// What the machine answers when asked what it is, through the version block of
// the gluk clock (base configuration manual, 9.6.1): 12 bytes of name, then the
// build date packed as day in b4..0 of byte 12, month across b7..5 of byte 12
// and b0 of byte 13, year-2000 in b6..1 of byte 13, and b7 set for an official
// release. On a real ZX Evo this is the fpga build and the bootloader build; an
// emulator answers with its own, which is what unreal does too. Saying "xEvo
// 09.12.2012" here, as this used to, claimed someone else's 2012 fpga while
// implementing the behaviour of a much later one. Who we are is handed down
// from the app, so the core does not have to know the product's name.
void comp_set_identity(Computer* comp, const char* name, int ymd, int release) {
	int day = ymd % 100;
	int mon = (ymd / 100) % 100;
	int year = ymd / 10000;
	memset(comp->verblk, 0, sizeof(comp->verblk));
	strncpy((char*)comp->verblk, name, 12);
	comp->verblk[12] = (day & 0x1f) | ((mon & 0x07) << 5);
	comp->verblk[13] = ((mon >> 3) & 0x01) | (((year - 2000) & 0x3f) << 1) | (release ? 0x80 : 0);
}

Computer* compCreate() {
	Computer* comp = (Computer*)malloc(sizeof(Computer));
	memset(comp, 0x00, sizeof(Computer));
	comp->resbank = RES_48;
	comp->flgFRN = 1;
	comp->flgDBG = 0;
	comp_brk_newstep(comp);		// vid is still NULL here, the call copes

	comp->cpu = cpuCreate(CPU_Z80,memrd,memwr,iord,iowr,intrq,comp_irq,comp);
	comp->mem = memCreate();
	comp->vid = vidCreate(vid_mrd_cb, comp_irq, comp);
	vid_set_mode(comp->vid, VID_NORMAL);

// input
	comp->keyb = kbd_create(comp_irq, comp);
	// comp->cmos.kbuf = &comp->keyb->kbuf;
	comp->joy = joyCreate();
	comp->joyb = joyCreate();
	comp->mouse = mouseCreate(comp_irq, comp);
// storage
	comp->tape = tape_create(comp_irq, comp);
	comp->dif = difCreate(DIF_NONE, comp_irq, comp);
	comp->ide = ideCreate(IDE_NONE, comp_irq, comp);
	comp->ide->smuc.cmos = &comp->cmos;
	comp->sdc = sdcCreate();
	comp->slot = sltCreate();
// sound
	comp->ts = tsCreate(TS_NONE,SND_AY,SND_NONE);
	comp->gs = gsCreate();
	comp->sdrv = sdrvCreate(SDRV_NONE);
	comp->saa = saaCreate();
	comp->beep = bcCreate();
// baseconf
//tsconf
	comp->tsconf.pwr_up = 1;
// rzx
#ifdef HAVEZLIB
	comp->rzx.file = NULL;
#endif
	compSetHardware(comp, "Dummy");
	gsReset(comp->gs);
	comp->cmos.data[17] = 0xaa;	// 0a?
	comp->frqMul = 1;
	comp->hwMul = 1;
	compSetBaseFrq(comp, 3.5);
//	compReset(comp, RES_DEFAULT);		// Can't reset here, cuz comp->resbank not defined yet
	return comp;
}

void compDestroy(Computer* comp) {
	rzxStop(comp);
	comp_heat_free(comp);
	cpuDestroy(comp->cpu);
	memDestroy(comp->mem);
	vidDestroy(comp->vid);
	kbd_destroy(comp->keyb);
	joyDestroy(comp->joy);
	joyDestroy(comp->joyb);
	mouseDestroy(comp->mouse);
	tape_destroy(comp->tape);
	difDestroy(comp->dif);
	ideDestroy(comp->ide);
	tsDestroy(comp->ts);
	gsDestroy(comp->gs);
	sdrvDestroy(comp->sdrv);
	saaDestroy(comp->saa);
	bcDestroy(comp->beep);
	sltDestroy(comp->slot);
	free(comp);
}

void compReset(Computer* comp,int res) {
	int i;
	comp->frmCount = 0;
	for (i = 0; i < comp->pwcount; i++) {	// what a watched port held is history now
		comp->pwatch[i].val = -1;
	}
#ifdef HAVEZLIB
	if (comp->rzx.play)
		rzxStop(comp);
#endif

	if (res == RES_DEFAULT)
		res = comp->resbank;
	xlog(XLG_CORE, XLL_INFO, "reset %s, bank %i", comp->hw->name, res);
	comp->p7FFD = ((res == RES_DOS) || (res == RES_48)) ? 0x10 : 0x00;
	comp->flgDOS = ((res == RES_DOS) || (res == RES_SHADOW)) ? 1 : 0;
	comp->flgROM = (comp->p7FFD & 0x10) ? 1 : 0;
	comp->flgCPM = 0;
	comp->flgEXT = 0;
	comp->prt2 = 0;
	comp->p1FFD = 0;
	comp->pEFF7 = 0;

	vid_reset(comp->vid);
	// kbdReleaseAll(comp->keyb);
//	kbdSetMode(comp->keyb, KBD_SPECTRUM);
	difReset(comp->dif);
	if (comp->gs->reset)
		gsReset(comp->gs);
	tsReset(comp->ts);
	ideReset(comp->ide);
	saaReset(comp->saa);
	sdcReset(comp->sdc);
	compSetHwTurbo(comp, 1);		// whatever turbo it had switched on
	if (comp->hw->reset)
		comp->hw->reset(comp);
	comp->hw->mapMem(comp);
	cpu_reset(comp->cpu);
	comp_set_snow(comp, comp->flgSNOW);	// the cpu may have been swapped since
	comp_heat_sync(comp);		// ram/rom size may have changed with hardware/romset
}

// T in one frame of this machine, exact given a precise nsPerTick. A call and
// not comp->fCount: that field is only filled at an interrupt, so it is stale
// on a machine that has just been reset or had its timings changed.
int comp_frame_ticks(Computer* comp) {
	if (comp->nsPerTick <= 0) return 0;
	return (int)llround(comp->vid->nsPerFrame / comp->nsPerTick);
}

// Stand the machine at tick T of its frame, counted from the interrupt, as a
// snapshot taken mid-frame asks for. The tick counter and the beam have to
// agree, and how many dots a tick is worth is the video's business. A tick
// before the frame (-1) is the start of one, which is where a reset leaves it.
void comp_set_frame_tick(Computer* comp, int tick) {
	int flen = comp_frame_ticks(comp);
	if ((tick < 0) || (flen < 1)) {
		comp->frmtCount = 0;
		vid_reset_ray(comp->vid);
		return;
	}
	if (tick >= flen) tick = flen - 1;
	comp->frmtCount = tick;
	vid_set_ray(comp->vid, (int)((long long)tick * comp->vid->dotPerFrame / flen));
}

// All a snapshot says about paging is the 7FFD byte, so before one is applied
// the machine has to stand where a 128K stands. A machine that pages through a
// pager of its own comes up from reset under its firmware instead, and there a
// snapshot would run the service rom; it puts its pager where a 128K's is here.
void comp_snap_map(Computer* comp) {
	if (comp->hw->snapmap)
		comp->hw->snapmap(comp);
}

// cpu freq

void comp_update_timings(Computer* comp) {
	comp->nsPerTick = 1e3 / comp->cpuFrq;
	if (comp->hw->init)
		comp->hw->init(comp);
	comp->nsPerTick /= comp->frqMul * comp->hwMul;
	// after hw->init, which reads nsPerTick to set the dot period: the dot
	// comes off the base clock, the turbo multiplier applies to the cpu alone
	// The tick and the dot come off the same crystal - a ZX tick is exactly two
	// dots - so derive the tick period from the dot period instead of rounding
	// each from its own double. Rounded separately they can land one 16.16 unit
	// apart, and vid_sync_fixed then loses that fraction of a dot on every tick:
	// on a Pentagon it slips a whole dot every ~130 frames, sliding the frame
	// interrupt across the 4T boundaries of the HALT loop it lands in, which
	// shows up as the border jumping 4 pixels every few seconds.
	if (comp->vid->nsPerDotExact > 0.0) {
		double dotsPerTick = comp->nsPerTick / comp->vid->nsPerDotExact;
		comp->nsPerTickFixed = (long long)llround(comp->vid->nsPerDotFixed * dotsPerTick);
	} else {
		comp->nsPerTickFixed = NSD_TO_FIXED(comp->nsPerTick);
	}
}

void compSetBaseFrq(Computer* comp, double frq) {
	if (frq > 0)
		comp->cpuFrq = frq;
	comp_update_timings(comp);
}

// Two multipliers, because two parties set one: the user asks for turbo from
// the options page or the hotkey, and a Scorpion, ATM, Pentagon 1024 or ZX
// Evolution turns its own on from a port. Keeping them apart is what lets a
// reset put the machine's back without touching what the user asked for.

void compSetTurbo(Computer* comp, double mult) {
	comp->frqMul = mult;
	comp_update_timings(comp);
}

void compSetHwTurbo(Computer* comp, double mult) {
	comp->hwMul = mult;
	comp_update_timings(comp);
}

void comp_set_layout(Computer* comp, vLayout* lay) {
	if (comp->hw->lay)
		lay = comp->hw->lay;
	vid_set_layout(comp->vid, lay);
}

// The snow effect costs a video sync on every opcode fetch, so the cpu only
// reports the refresh cycle while a machine actually wants it.
void comp_set_snow(Computer* comp, int on) {
	comp->flgSNOW = on ? 1 : 0;
	if (comp->cpu)
		comp->cpu->flgRFSH = comp->flgSNOW;
}

void comp_kbd_release(Computer* comp) {
	kbdReleaseAll(comp->keyb);
}

// hardware

// NOTE: keeps the watched ports in step with the machine (comp_pwatch_sync)

int compSetHardware(Computer* comp, const char* name) {
	HardWare* hw;
	if (name == NULL) {
		hw = comp->hw;
	} else {
		hw = findHardware(name);
	}
	if (hw == NULL) return 0;
	comp->hw = hw;
//	comp->cpu->nod = 0;
	comp->vid->mrd = vid_mrd_cb;
	comp->tape->xen = 0;
	compSetBaseFrq(comp, 0);	// recalculations
	comp_pwatch_sync(comp);
	return 1;
}

// exec 1 opcode, sync devices, return eated ns

int compExec(Computer* comp) {
	comp->vid->time = 0;
// breakpoints. A run-ahead frame is thrown away, so a break there would fire
// twice: leave it to the pass that keeps its result
	if (!comp->flgDBG && !x_runahead) {
		bpChecker ch = comp_check_bp(comp, cpu_get_pc(comp->cpu), MEM_BRK_FETCH | MEM_BRK_TFETCH);
		if (ch.t >= 0) {
			comp->flgBRK = 1;
			comp->brkt = ch.t;
			comp->brka = ch.a;
			if (*ch.ptr & MEM_BRK_TFETCH) {
				*ch.ptr &= ~MEM_BRK_TFETCH;
				comp->brkt = -1;		// temp (not in list)
			}
			return 0;
		}
		if (comp->cpu->intrq && comp->flgIBRK) {
			comp->flgBRK = 1;
			comp->brkt = BRK_IRQ;
			return 0;
		}
	}
// start
	res4 = 0;
// exec cpu opcode OR handle interrupt. get T states back
	res2 = cpu_exec(comp->cpu);
// scorpion WAIT: add 1T to odd-T command
	if (comp->flgEM1 && (res2 & 1))
		res2++;
#ifdef HAVEZLIB
	if (comp->rzx.play) {
		if (comp->rzx.frm.fetches == 0) {
			if (comp->hw->irq)
				comp->hw->irq(comp, IRQ_RZX_INT);
		}
	}
#endif
	vid_sync_fixed(comp->vid, ticks_to_ns_fixed(comp, res2 - res4));
// execution completed : get eated time & translate signals
	nsTime = comp->vid->time;
	comp->tickCount += res2;
	comp->frmtCount += res2;
// sync hardware
	if (comp->hw->sync)
		comp->hw->sync(comp, nsTime);
// new frame
	if (comp->vid->newFrame) {
		comp->vid->newFrame = 0;
		comp->flgFRM = 1;
	}
// return ns eated @ this step
	return nsTime;
}

// cmos

// The flags the avr keeps for the machine, read back through the same block
// (manual 9.6.4, and MODE_* in the avr's main.h, which has moved on since the
// manual was written: b0 vga, b2 caps led, b3 tape out, b4..5 the raster on
// BaseConf). Only cell 0 answers; the rest read as 0xFF. The picture here is
// always progressive, so vga reads as set, and the raster bits stay at
// pentagon - the one this emulator gives BaseConf, its layout being fixed.
#define MODE_VGA	0x01

unsigned char cmsRd(Computer* comp) {
	unsigned char res = 0xff;
	if (comp->cmos.adr >= 0x70) {
		switch(comp->cmos.mode) {
			case 0:					// base configuration version
			case 1: res = comp->verblk[comp->cmos.adr & 0x0f]; break;		// bootloader version
			case 2: res = xt_read(comp->keyb); break; //keyReadCode(comp->keyb); break;		// read PC keyboard keycode (TODO: used here only)
			case 3: if (!(comp->cmos.adr & 0x0f)) res = MODE_VGA; break;	// avr flags
		}
	} else {
		res = cmos_rd(&comp->cmos, CMOS_DATA);
	}
	return res & 0xff;
}

void cmsWr(Computer* comp, int val) {
	switch (comp->cmos.adr) {
		case 0x0c:
			if (val & 1) {
				comp->keyb->outbuf = 0;
			}
			break;
		default:
			if (comp->cmos.adr > 0x6f) {
				comp->cmos.mode = val;	// write to F0..FF : set F0..FF reading mode
				//printf("cmos mode %i\n",val);
			} else {
				cmos_wr(&comp->cmos, CMOS_DATA, val);
			}
			break;
	}
}

// breaks

// end of an instruction as breakpoint conditions see it: the mem/io events it
// caused are spent, and where the beam stands now is where the next one starts

void comp_brk_newstep(Computer* comp) {
	comp->brkev.rd = -1;
	comp->brkev.wr = -1;
	comp->brkev.mdt = -1;
	comp->brkev.in = -1;
	comp->brkev.out = -1;
	comp->brkev.val = -1;
	comp->brkray = comp->vid ? (comp->vid->ray.y * comp->vid->full.x + comp->vid->ray.x) : 0;
}

// activate breakpoint w/o type (exit to debuga)
void comp_brk(Computer* comp, int t) {
	comp->flgBRK = 1;
	comp->brkt = t;
}

static unsigned char dumBrk = 0x00;

unsigned char* getBrkPtr(Computer* comp, int madr) {
	xAdr xadr = mem_get_xadr(comp->mem, madr);
	unsigned char* ptr = NULL;
	switch (xadr.type) {
		case MEM_RAM: ptr = comp->brkRamMap + (xadr.abs & comp->mem->ramMask); break;
		case MEM_ROM: ptr = comp->brkRomMap + (xadr.abs & comp->mem->romMask); break;
		case MEM_SLOT:
			if (comp->slot->brkMap)
				ptr = comp->slot->brkMap + (xadr.abs & comp->slot->memMask);
			break;
	}
	if (!ptr) {
		dumBrk = 0;
		ptr = &dumBrk;
	}
	return ptr;
}

void setBrk(Computer* comp, int adr, unsigned char val) {
	unsigned char* ptr = getBrkPtr(comp, adr);
	if (ptr == NULL) return;
	*ptr = (*ptr & 0xf0) | (val & 0x0f);
}

unsigned char getBrk(Computer* comp, int adr) {
	unsigned char* ptr = getBrkPtr(comp, adr);
	unsigned char res = ptr ? *ptr : 0x00;
	if (comp->mem->busmask < 0x10000) {
		res |= (comp->brkAdrMap[adr & comp->mem->busmask] & 0x0f);
	}
	return res;
}

#include <stdio.h>
#include "xlog.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "spectrum.h"
#include "filetypes/filetypes.h"
#include "cpu/Z80/z80.h"
#include "hardware/zx_step.h"
#include "input/inview.h"
#include "xreplay.h"

static int nsTime;
static int res2;
int res4 = 0;		// save last T synced with video (last is res2-res4)

// The map as it stands, or with FETCH as the opcode fetch at start leaves it: a
// divMMC entry point pages the board in (or 1FF8-1FFF out) for the fetch after
// it, #3Dxx for its own. While the debugger holds the machine, a paging the last
// fetch armed is shown done, unless LIVE asks for the map the machine uses. The
// machine's memory under the board is in mem->under.
static xAdr comp_view_xadr(Computer* comp, int start, int adr, int mode) {
	int dabs;
	int fv = -1;
	if (comp->div) {
		if (mode & MEM_VIEW_FETCH) {
			fv = div_fetch_view(comp->div, start, adr, &dabs);
		} else if (comp->flgDBG && !(mode & MEM_VIEW_LIVE)) {
			fv = div_fetch_view(comp->div, 0x8000, adr, &dabs);	// any fetch but #3Dxx
		}
	}
	if (fv < 0) return mem_get_xadr(comp->mem, adr);
	xAdr xa;
	xa.adr = adr;
	if (fv) {
		xa.type = MEM_DIV;
		xa.abs = dabs;
		xa.bank = dabs >> 8;
		return xa;
	}
	int page = (adr >> comp->mem->pgshift) & 0xff;
	if (!comp->mem->ovl[page]) return mem_get_xadr(comp->mem, adr);
	MemPage* pg = &comp->mem->under[page];
	xa.type = pg->type;
	xa.bank = pg->num;
	xa.abs = (pg->num << comp->mem->pgshift) | (adr & comp->mem->pgmask);
	return xa;
}

static void comp_mem_view_at(Computer* comp, int start, int adr, int mode, int* value, xAdr* xadr) {
	adr &= comp->mem->busmask;
	if (comp->hw->memView) {
		comp->hw->memView(comp, adr, mode, value, xadr);
		return;
	}
	xAdr xa = comp_view_xadr(comp, start & comp->mem->busmask, adr, mode);
	if (xadr) *xadr = xa;
	if (!value) return;
	// a debugger edit: RAM always, ROM with "write to ROM", divMMC where the board
	// lets the CPU write (not bank 3 under MAPRAM, the EEPROM only when enabled)
	if (mode & MEM_VIEW_WRITE) {
		switch (xa.type) {
			case MEM_RAM:
				comp->mem->ramData[xa.abs & comp->mem->ramMask] = *value & 0xff;
				break;
			case MEM_ROM:
				if (mode & MEM_VIEW_ROM_WRITE)
					comp->mem->romData[xa.abs & comp->mem->romMask] = *value & 0xff;
				break;
			case MEM_DIV: {
				int fadr = xa.abs % (DIV_EE_SIZE + DIV_RAM_MAX);
				if (div_view_writable(comp->div, adr) || (mode & MEM_VIEW_ROM_WRITE)) {
					comp->div->data[fadr] = *value & 0xff;
					if (fadr < DIV_EE_SIZE) comp->div->eeDirty = 1;
				}
				break;
			}
		}
		return;
	}
	switch (xa.type) {
		case MEM_RAM: *value = comp->mem->ramData[xa.abs & comp->mem->ramMask]; break;
		case MEM_ROM: *value = comp->mem->romData[xa.abs & comp->mem->romMask]; break;
		case MEM_SLOT: *value = memRd(comp->mem, adr); break;
		case MEM_DIV: *value = comp->div->data[xa.abs % (DIV_EE_SIZE + DIV_RAM_MAX)]; break;
		default: *value = 0xff; break;
	}
}

static void comp_mem_view(Computer* comp, int adr, int mode, int* value, xAdr* xadr) {
	comp_mem_view_at(comp, adr, adr, mode, value, xadr);
}

int comp_peek(Computer* comp, int adr, int access, xAdr* xadr) {
	int value = 0xff;
	comp_mem_view(comp, adr, access & MEM_VIEW_FETCH, &value, xadr);
	return value & 0xff;
}

void comp_poke(Computer* comp, int adr, int value, int romWrite) {
	int mode = MEM_VIEW_WRITE | (romWrite ? MEM_VIEW_ROM_WRITE : 0);
	comp_mem_view(comp, adr, mode, &value, NULL);
}

xAdr comp_get_xadr(Computer* comp, int adr, int access) {
	xAdr xadr = {MEM_EXT, 0, adr & comp->mem->busmask, -1};
	comp_mem_view(comp, adr, access, NULL, &xadr);
	return xadr;
}

int comp_find_xadr(Computer* comp, int type, int abs, int access) {
	if (abs < 0) return -1;
	if (type == MEM_IO) return abs & comp->mem->busmask;
	if (!comp->hw->memView) return memFindAdr(comp->mem, type, abs);
	// the one machine with a view of its own (the Next) pages everything in 8K
	// steps, overlays included: the cell can only be at its offset in one of the
	// eight slots. A condition naming a label asks this on every instruction.
	for (int i = 0; i < 8; i++) {
		int adr = (i << 13) | (abs & 0x1fff);
		xAdr cell = comp_get_xadr(comp, adr, access);
		if ((cell.type == type) && (cell.abs == abs)) return adr;
	}
	return -1;
}

static int comp_is_next(Computer* comp) {
	return comp->hw && (comp->hw->id == HW_ZXNEXT);
}

int comp_cell_unit(Computer* comp, int area) {
	switch (area) {
		case CELL_RAM: return comp_is_next(comp) ? 0x2000 : 0x4000;
		case CELL_ROM: case CELL_ALTROM: case CELL_SLT: return 0x4000;
		case CELL_DIVROM: case CELL_DIV: case CELL_MFROM: case CELL_MFRAM: return 0x2000;
	}
	return 0;
}

int comp_cell_pages(Computer* comp, int area) {
	int next = comp_is_next(comp);
	switch (area) {
		case CELL_RAM: return next ? 0xe0 : ((comp->mem->ramMask + 1) >> 14);
		case CELL_ROM: return next ? 4 : ((comp->mem->romMask + 1) >> 14);
		case CELL_ALTROM: return next ? 2 : 0;
		case CELL_DIV: return next ? 16 : ((comp->div && comp->div->model) ? comp->div->pages : 0);	// as many as the board has
		case CELL_DIVROM: return (next || (comp->div && comp->div->model)) ? 1 : 0;
		case CELL_MFROM: case CELL_MFRAM: return next ? 1 : 0;
		case CELL_SLT: return comp->slot ? ((comp->slot->memMask + 1) >> 14) : 0;
	}
	return 0;
}

xCell comp_cell(Computer* comp, int type, int abs) {
	xCell cell = {CELL_NONE, 0, 0, 0};
	if (abs < 0) return cell;
	if (comp_is_next(comp)) {
		if (type == MEM_RAM) cell.area = next_cell(abs, &cell.page, &cell.ofs);
	} else {
		switch (type) {
			case MEM_RAM: cell.area = CELL_RAM; break;
			case MEM_ROM: cell.area = CELL_ROM; break;
			case MEM_SLOT: cell.area = CELL_SLT; break;
			case MEM_DIV:
				abs %= DIV_EE_SIZE + DIV_RAM_MAX;
				if (abs < DIV_EE_SIZE) {
					cell.area = CELL_DIVROM;
					cell.ofs = abs;
				} else {
					cell.area = CELL_DIV;
					cell.page = (abs - DIV_EE_SIZE) >> 13;
					cell.ofs = (abs - DIV_EE_SIZE) & 0x1fff;
				}
				break;
		}
		if ((cell.area == CELL_RAM) || (cell.area == CELL_ROM) || (cell.area == CELL_SLT)) {
			cell.page = abs >> 14;
			cell.ofs = abs & 0x3fff;
		}
	}
	cell.unit = comp_cell_unit(comp, cell.area);
	return cell;
}

int comp_cell_xadr(Computer* comp, int area, int page, int ofs, int* type, int* abs) {
	int unit = comp_cell_unit(comp, area);
	if (!unit || (page < 0) || (page >= comp_cell_pages(comp, area)) || (ofs < 0) || (ofs >= unit)) return 0;
	if (comp_is_next(comp)) {
		*type = MEM_RAM;
		*abs = next_cell_abs(area, page, ofs);
		return *abs >= 0;
	}
	switch (area) {
		case CELL_RAM: *type = MEM_RAM; *abs = (page << 14) | ofs; return 1;
		case CELL_ROM: *type = MEM_ROM; *abs = (page << 14) | ofs; return 1;
		case CELL_SLT: *type = MEM_SLOT; *abs = (page << 14) | ofs; return 1;
		case CELL_DIVROM: *type = MEM_DIV; *abs = ofs; return 1;
		case CELL_DIV: *type = MEM_DIV; *abs = DIV_EE_SIZE + (page << 13) + ofs; return 1;
	}
	return 0;
}

// The byte behind a cell: a page's offset may run past its end into the next
// pages. NULL: no such cell.
static unsigned char* comp_cell_ptr(Computer* comp, int area, int page, int ofs) {
	int unit = comp_cell_unit(comp, area);
	int type, abs;
	if (!unit || (ofs < 0)) return NULL;
	if (!comp_cell_xadr(comp, area, page + ofs / unit, ofs % unit, &type, &abs)) return NULL;
	switch (type) {
		case MEM_RAM: return comp->mem->ramData + (abs & comp->mem->ramMask);
		case MEM_ROM: return comp->mem->romData + (abs & comp->mem->romMask);
		case MEM_SLOT: return (comp->slot && comp->slot->data) ? comp->slot->data + (abs & comp->slot->memMask) : NULL;
		case MEM_DIV: return comp->div ? comp->div->data + abs : NULL;
	}
	return NULL;
}

int comp_cell_size(Computer* comp, int area) {
	return comp_cell_unit(comp, area) * comp_cell_pages(comp, area);
}

int comp_cell_peek(Computer* comp, int area, int page, int ofs) {
	unsigned char* ptr = comp_cell_ptr(comp, area, page, ofs);
	return ptr ? *ptr : -1;
}

int comp_cell_rom(int area) {
	return (area != CELL_RAM) && (area != CELL_DIV) && (area != CELL_MFRAM);
}

int comp_cell_poke(Computer* comp, int area, int page, int ofs, int val, int romWrite) {
	if (comp_cell_rom(area) && !romWrite) return 0;
	unsigned char* ptr = comp_cell_ptr(comp, area, page, ofs);
	if (!ptr) return 0;
	*ptr = val & 0xff;
	if ((area == CELL_DIVROM) && comp->div && !comp_is_next(comp)) comp->div->eeDirty = 1;
	return 1;
}

static const struct {int area; const char* name;} cell_names[] = {
	{CELL_ALTROM, "ALTROM"}, {CELL_DIVROM, "DIVROM"},	// before the names they start with
	{CELL_ROM, "ROM"}, {CELL_DIV, "DIV"}, {CELL_MFROM, "MFROM"}, {CELL_MFRAM, "MFRAM"},
	{CELL_SLT, "SLT"}
};

const char* cell_token(xCell cell, char* buf) {
	buf[0] = 0;
	for (unsigned i = 0; i < sizeof(cell_names) / sizeof(cell_names[0]); i++) {
		if (cell_names[i].area != cell.area) continue;
		if ((cell.area == CELL_DIVROM) || (cell.area == CELL_MFROM) || (cell.area == CELL_MFRAM))
			strcpy(buf, cell_names[i].name);
		else
			sprintf(buf, "%s%i", cell_names[i].name, cell.page);
		break;
	}
	return buf;
}

static int cell_prefix(const char* str, const char* name) {
	size_t len = 0;
	while (name[len]) {
		char c = str[len];
		if ((c >= 'a') && (c <= 'z')) c -= 0x20;
		if (c != name[len]) return 0;
		len++;
	}
	return (int)len;
}

int cell_token_parse(const char* str, int* area, int* page) {
	for (unsigned i = 0; i < sizeof(cell_names) / sizeof(cell_names[0]); i++) {
		int len = cell_prefix(str, cell_names[i].name);
		if (!len) continue;
		const char* num = str + len;
		*area = cell_names[i].area;
		*page = 0;
		if ((*area == CELL_DIVROM) || (*area == CELL_MFROM) || (*area == CELL_MFRAM)) return *num == 0;
		if ((*num < '0') || (*num > '9')) return 0;
		char* end;
		*page = (int)strtol(num, &end, 10);
		return *end == 0;
	}
	return 0;
}

typedef struct {
	Computer* comp;
	int start;
} compDasmView;

// every byte of the instruction, its operands included, is read in the map its
// opcode fetch leaves: a divMMC only changes it on the next fetch
static int comp_dasm_read(int adr, void* ptr) {
	compDasmView* view = (compDasmView*)ptr;
	int value = 0xff;
	comp_mem_view_at(view->comp, view->start, adr, MEM_VIEW_FETCH, &value, NULL);
	return value & 0xff;
}

xMnem compDisasm(Computer* comp, int adr, char* buf, unsigned char* bytes, int size) {
	if (comp->hw->disasm) return comp->hw->disasm(comp, adr, buf, bytes, size);
	compDasmView view = {comp, adr};
	xMnem mn = cpuDisasm(comp->cpu, adr, buf, comp_dasm_read, &view);
	if (bytes) {
		for (int i = 0; (i < size) && (i < mn.len); i++) bytes[i] = comp_dasm_read(adr + i, &view);
	}
	return mn;
}

static unsigned char* comp_get_memcell_flag_ptr(Computer* comp, int adr, int access) {
	unsigned char* res = NULL;
	xAdr xadr = comp_get_xadr(comp, adr, access);
	switch (xadr.type) {
		case MEM_RAM: res = comp->brkRamMap + (xadr.abs & comp->mem->ramMask); break;
		case MEM_ROM: res = comp->brkRomMap + (xadr.abs & comp->mem->romMask); break;
		case MEM_SLOT: if (comp->slot->brkMap) {res = comp->slot->brkMap + (xadr.abs & comp->slot->memMask);} break;
		case MEM_DIV: if (comp->div) res = comp->div->brkMap + (xadr.abs % (DIV_EE_SIZE + DIV_RAM_MAX)); break;
	}
	return res;
}

static bpChecker comp_check_bp(Computer* comp, int adr, int mask, int access) {
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
		xAdr xadr = comp_get_xadr(comp, adr, access);
		switch (xadr.type) {
			case MEM_RAM: xadr.abs &= comp->mem->ramMask; ch.ptr = comp->brkRamMap + xadr.abs; ch.t = BRK_MEMRAM; break;
			case MEM_ROM: xadr.abs &= comp->mem->romMask; ch.ptr = comp->brkRomMap + xadr.abs; ch.t = BRK_MEMROM; break;
			case MEM_SLOT: if (comp->slot->brkMap) {
					xadr.abs &= comp->slot->memMask;
					ch.ptr = comp->slot->brkMap + xadr.abs;
					ch.t = BRK_MEMSLT;
				} break;
			case MEM_DIV: if (comp->div) {
					xadr.abs %= DIV_EE_SIZE + DIV_RAM_MAX;
					ch.ptr = comp->div->brkMap + xadr.abs;
					ch.t = BRK_MEMDIV;
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

__attribute__((noinline)) static int memrd_watched(Computer*, int, int);

// A divMMC's automapper sees the fetch before its byte is read (div_m1). A
// delayed switch it arms acts on the next fetch, wherever that is: no fetch
// table until then (comp_m1_tab).
static void comp_div_fetch(Computer* comp, int adr) {
	div_m1(comp->div, adr);
	if (div_pending(comp->div)) comp->cpu->m1p = NULL;
}

static void comp_m1x(void* ptr, int adr) {
	Computer* comp = (Computer*)ptr;
	if (comp->div->model) comp_div_fetch(comp, adr);
	if (comp->hw->id == HW_ZXNEXT) next_nmi_m1(comp, adr);
}

// the board's time: its oscillators run whatever the CPU does
static long long comp_div_ns(void* ptr) {
	return comp_ns((Computer*)ptr);
}

int memrd(int adr, int m1, void* ptr) {
	Computer* comp = (Computer*)ptr;
	adr &= comp->cpu->busmask;
	if (m1 && comp->div->model)
		comp_div_fetch(comp, adr);
	// nothing watches the read and nothing spoils it: stdMRd() without the
	// calls, unless the fetch pages TR-DOS
	if (!comp_mem_watched(comp) && !comp->snowBad
			&& !comp->rzx.play && !comp->rzx.rec.on
			) {
		if (comp->hw->mrd != stdMRd)
			return comp->hw->mrd(comp, adr, m1);
		MemPage* pg = mem_get_page(comp->mem, adr);
		if (m1 && (comp->dif->type == DIF_BDI) && bdi_fetch_pages(comp, pg, adr))
			return stdMRd(comp, adr, m1);
		return mem_page_rd(pg, adr);
	}
	return memrd_watched(comp, adr, m1);
}

static int memrd_watched(Computer* comp, int adr, int m1) {
	if (m1 && comp->rzx.play && (comp->rzx.frm.fetches > 0)) {
		comp->rzx.frm.fetches--;
	}
	if (m1 && comp->rzx.rec.on)
		comp->rzx.rec.fetch++;
	// the Next pages for a fetch in its own read, after this: its view is asked
	// for the fetch. A divMMC elsewhere has already paged for it (memrd), so the
	// map as it stands is the fetch's
	int access = ((m1 && comp->hw->memView) ? MEM_VIEW_FETCH : MEM_VIEW_READ) | MEM_VIEW_LIVE;
	// only the debugger's aids want to know whether this is an instruction byte
	unsigned isExecByte = 0;
	if (comp->flgMAP || comp->flgHEAT || comp->flgCOND)
		isExecByte = (cpu_get_pc(comp->cpu) - 1) == adr;
	if (comp->flgMAP) {
		unsigned char* fptr = comp_get_memcell_flag_ptr(comp, adr,
			access);
		if (fptr) {
			unsigned char flag = *fptr;
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
		comp_heat_hit(comp, adr, isExecByte ? HEAT_EX : HEAT_RD,
			access);
	}
	if (comp->flgBRKMEM) {
		bpChecker ch = comp_check_bp(comp, adr, MEM_BRK_RD,
			access);
		if (ch.t >= 0) {
			comp->flgBRK = 1;
			comp->brkt = ch.t;
			comp->brka = ch.a;
			comp->brkev.kind = MEM_BRK_RD;
			comp->brkev.adr = adr;
		}
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

__attribute__((noinline)) static void memwr_watched(Computer*, int, int);

void memwr(int adr, int val, void* ptr) {
	Computer* comp = (Computer*)ptr;
	adr &= comp->cpu->busmask;
	// nothing watches the write: stdMWr() without the two calls
	if (!comp_mem_watched(comp)) {
		if (comp->hw->mwr == stdMWr)
			mem_wr_fast(comp->mem, adr, val);
		else
			comp->hw->mwr(comp, adr, val);
		return;
	}
	memwr_watched(comp, adr, val);
}

static void memwr_watched(Computer* comp, int adr, int val) {
	if (comp->flgHEAT && !x_runahead) {	// an ahead frame would count every access twice
		// writes (incl. stack push/call) are always data traffic, never execution
		comp_heat_hit(comp, adr, HEAT_WR, MEM_VIEW_WRITE | MEM_VIEW_LIVE);
	}
	unsigned char* fptr = comp->flgMAP
		? comp_get_memcell_flag_ptr(comp, adr, MEM_VIEW_WRITE | MEM_VIEW_LIVE) : NULL;
	if (fptr) {
		unsigned char flag = *fptr;
		if (!(flag & 0xf0)) {
			flag |= DBG_VIEW_BYTE;
			*fptr = flag;
		}
	}
	if (comp->flgBRKMEM) {
		bpChecker ch = comp_check_bp(comp, adr, MEM_BRK_WR, MEM_VIEW_WRITE | MEM_VIEW_LIVE);
		if (ch.t >= 0) {
			comp->flgBRK = 1;
			comp->brkt = ch.t;
			comp->brka = ch.a;
			comp->brkev.kind = MEM_BRK_WR;
			comp->brkev.adr = adr;
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

// The slots of an i/o cycle count their dots, as a memory cycle does: the wait
// is read off where the ray stands (vid_lazy_dots), and the port handler gets
// the ray settled (iord/iowr).
void zx_cont_delay(Computer* comp) {
	int wdots = vid_wait_dots(comp->vid, 5 << 14, 0);	// video is already at end of wait cycle
	comp_vid_to_lazy(comp, comp->cpu->t + comp->contTicks[wdots]);
	comp->cpu->t = res4;
}

void zx_free_ticks(Computer* comp, int t) {
	comp_vid_to_lazy(comp, comp->cpu->t + t);
	comp->cpu->t = res4;
}

// A port is slow where a memory address would be: on a 128K the page at #C000
// counts too (Rak, measured on a real 128K, WoS forum 2007)
static int zx_port_slow(Computer* comp, int port) {
	return zx_bank_of(comp, port) & 1;
}

// Contention on T1
void zx_cont_t1(Computer* comp, int port) {
	if (zx_port_slow(comp, port))
		zx_cont_delay(comp);
	zx_free_ticks(comp, 1);
}

// Contention on T2-T4
void zx_cont_tn(Computer* comp, int port) {
	// zx_cont_t1 took the first of the four slots, three are left here
	if (zx_port_slow(comp, port)) {
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
		if (comp->pwatch[i].on && !comp->pwatch[i].hw)
			comp->pwbus++;
	}
}

static void pwatch_hit(Computer* comp, int port, int val) {
	int i;
	for (i = 0; i < comp->pwcount; i++) {
		if (!comp->pwatch[i].on || comp->pwatch[i].hw) continue;
		if ((port & comp->pwatch[i].mask) != comp->pwatch[i].port) continue;
		comp->pwatch[i].val = val & 0xff;
	}
}

// once a frame: a folder mounted writable follows the guest's writes
void comp_media_tick(Computer* comp) {
	if (x_runahead) return;
	vfat_tick(comp->sdc->vfat);
	vfat_tick(comp->div->card[0]->vfat);
	vfat_tick(comp->div->card[1]->vfat);
	vfat_tick(comp->ide->master->vfat);
	vfat_tick(comp->ide->slave->vfat);
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
		comp->pwatch[comp->pwcount].get = NULL;
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
		comp->pwatch[i].get = NULL;
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
		comp->pwatch[j].get = tab[i].get;
	}
	pwatch_recount(comp);
}

// what to show for the port: its own register when the machine has one,
// the last value on the bus otherwise

int comp_pwatch_val(Computer* comp, int idx) {
	void* ptr;
	if ((idx < 0) || (idx >= comp->pwcount)) return -1;
	if (comp->pwatch[idx].get) return comp->pwatch[idx].get(comp);
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
	comp_run_io(comp);
	if (comp->flgCNTI) {
		// fuse reads the port after both halves of the i/o cycle have
		// been contended (periph.c readport), so do the waiting first
		comp_vid_to_t(comp);
		zx_cont_t1(comp, port);
		zx_cont_tn(comp, port);
		comp->cpu->t -= 4;		// z80_iord puts the four back
	} else {
		comp_vid_to_lazy(comp, comp->cpu->t + 3);
	}
// play rzx
	int res = 0xff;
	if (comp->rzx.play) {
		if (comp->rzx.frm.pos < comp->rzx.frm.size) {
			res = comp->rzx.frm.data[comp->rzx.frm.pos];
			comp->rzx.frm.pos++;
			if (inview.on) iview_in(&inview, comp, port, res);
			return res;
		} else {
			rzxStop(comp);
			xlog(XLG_CORE, XLL_DEBUG, "overIO frame %i pc=%04X port=%04X size=%i fetch-left %i",
				comp->rzx.fCurrent, cpu_get_pc(comp->cpu), port, comp->rzx.frm.size, comp->rzx.frm.fetches);
			comp->rzx.overio = 1;
			return 0xff;
		}
	}
	comp->flgBDI = (comp->flgDOS && (comp->dif->type == DIF_BDI)) ? 1 : 0;
// brk
	if (comp->brkIOMap[port] & MEM_BRK_RD) {
		comp->flgBRK = 1;
		comp->brkt = BRK_IOPORT;
		comp->brka = port;
		comp->brkev.kind = MEM_BRK_RD;
		comp->brkev.adr = port;
	}

	vid_settle(comp->vid);		// some port handlers read the ray (Scorpion, Profi)
	res = comp->hw->in ? comp->hw->in(comp, port) : 0xff;
	if (comp->flgCOND) {
		comp->brkev.in = port;
		comp->brkev.val = res & 0xff;
	}
	if (xreplay_mode) res = xreplay_in(res);
	if (comp->pwbus) pwatch_hit(comp, port, res);
	if (comp->rzx.rec.on) rzx_rec_in(comp, res);
	if (inview.on && (xreplay_mode != XRP_FEED)) iview_in(&inview, comp, port, res);
	return res;
}

// the OUT reaching the device, the ray where it is: the page or the mode it may
// change are latched first in an undrawn frame (vid_latch)
static inline void iowr_out(Computer* comp, int port, int val) {
	vid_settle(comp->vid);
	if (comp->vid->nodraw && comp->vid->cb->latch)
		vid_latch(comp->vid);
	comp->hw->out(comp, port, val);
}

void iowr(int port, int val, void* ptr) {
	Computer* comp = (Computer*)ptr;
	comp_run_io(comp);
	comp->flgBDI = (comp->flgDOS && (comp->dif->type == DIF_BDI)) ? 1 : 0;
	if (comp->flgCNTI) {
		comp_vid_to_t(comp);		// video to current T
		zx_cont_t1(comp, port);
		iowr_out(comp, port, val);
		zx_cont_tn(comp, port);
		comp->cpu->t -= 4;
	} else {
		// the ray up to T+1 in one step: the OUT reaches the device mid-cycle
		comp_vid_to_lazy(comp, comp->cpu->t + 1);
		iowr_out(comp, port, val);
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
		comp->brkev.kind = MEM_BRK_WR;
		comp->brkev.adr = port;
	}
	if (comp->flgCOND) {
		comp->brkev.out = port;
		comp->brkev.val = val & 0xff;
	}
	if (comp->pwbus) pwatch_hit(comp, port, val);
}

int intrq(void* ptr) {
	Computer* comp = (Computer*)ptr;
	vid_unlazy(comp->vid);
	return comp->hw->ack ? comp->hw->ack(comp) : 0xff;
}

// The ray up to a write: what the write changes may be drawn next. An undrawn
// frame works out what its dots would have fetched when asked: a write about
// to change that takes it first.
static void comp_wr_sync(CPU* cpu) {
	Computer* comp = (Computer*)cpu->xptr;
	comp_vid_to_t(comp);
	if (comp->vid->nodraw && comp->vid->cb->latch) {
		MemPage* pg = mem_get_page(comp->mem, comp->cpu->adr);
		if (vid_latch_hit(comp->vid, (pg->type == MEM_RAM) ? ((pg->num << comp->mem->pgshift) | (comp->cpu->adr & comp->mem->pgmask)) : -1))
			vid_latch(comp->vid);
	}
}

// The end of an instruction (CPU.xend): the machine's INT sample (hwQuiet.ack)
static void comp_end(CPU* cpu) {
	Computer* comp = (Computer*)cpu->xptr;
	comp_ack_in(comp, comp_ack_kind(comp));
}

void comp_irq(int t, void* ptr) {
	Computer* comp = (Computer*)ptr;
	// a machine's own handler may read or move the video, so the dots counted
	// are drawn first - but not for what no handler needs it for
	int unlazy = 1;
	switch (t) {
		case IRQ_TAP_0:			// no handler acts on these, and unlazying would only
		case IRQ_TAP_1:			// cut short the calm stretch of a loader's edge loop
		case IRQ_TAP_BLK:
			unlazy = 0;
			break;
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
			unlazy = 0;
			break;
		case IRQ_VID_INT:
			comp->fCount = comp_frame_ticks(comp);
			comp->frmtCount = 0;
			if (!comp->cpu->flgHALT) {
				comp->hCount = comp->fCount;		// if not HALT-ed during frame, count all ticks
			}
			break;
		case IRQ_CPU_RFSH:
			comp_vid_to_t(comp);
			zx_snow(comp);
			return;			// not a machine's business
	}
	if (unlazy)
		vid_unlazy(comp->vid);
	if (comp->hw->irq) comp->hw->irq(comp, t);
}

// rzx

unsigned rzx_playing = 0;

void rzxStop(Computer* zx) {
	zx->rzx.play = 0;
	if (x_runahead) return;		// a frame that is thrown away: the file stays for the rest
	rzx_playing = 0;
	if (zx->rzx.file) fclose(zx->rzx.file);
	zx->rzx.file = NULL;
	zx->rzx.fCount = 0;
	zx->rzx.frm.size = 0;
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
	comp->next = (xNextState*)calloc(1, sizeof(xNextState));
	comp->rtc = (DS1307*)calloc(1, sizeof(DS1307));
	ds_init(comp->rtc);
	comp->resbank = RES_48;
	comp->earback = EAR_ISSUE3;
	comp->flgFRN = 1;
	comp->flgDBG = 0;
	comp_brk_newstep(comp);		// vid is still NULL here, the call copes

	comp->cpu = cpuCreate(CPU_Z80,memrd,memwr,iord,iowr,intrq,comp_irq,comp);
	comp->cpu->xend = comp_end;
	comp->cpu->xsync = comp_wr_sync;
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
	// the Next talks to it by the spec (sdc_xfer): over 2 GB an SDHC card, block addressed,
	// or its 32-bit byte addresses stop at 4 GB; the Z-controller's path reads neither flag
	comp->sdc->spec = 1;
	comp->slot = sltCreate();
	comp->div = div_create(comp->mem, comp, comp_div_ns);
	comp->cpu->xm1x = comp_m1x;
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
	comp->rzx.file = NULL;
	// a clock before the first hardware: setting it works the timings out
	comp->cpuFrq = 3.5;
	comp->frqMul = 1;
	comp->hwMul = 1;
	comp->turboStep = 1;
	comp->turboTab[0] = 1;
	comp->turboCount = 1;
	compSetHardware(comp, "Dummy");
	gsReset(comp->gs);
	comp->cmos.data[17] = 0xaa;	// 0a?
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
	sdcDestroy(comp->sdc);
	div_destroy(comp->div);
	tsDestroy(comp->ts);
	gsDestroy(comp->gs);
	sdrvDestroy(comp->sdrv);
	saaDestroy(comp->saa);
	bcDestroy(comp->beep);
	sltDestroy(comp->slot);
	free(comp->next);
	free(comp->rtc);
	free(comp);
}

// A reset the user asked for, not one a loader does: the tape stops where it
// stands, and the automatics follow the rom again whatever Stop said before.
// A divMMC's MAPRAM goes with it, so the firmware can always start.
void compUserReset(Computer* comp, int res) {
	div_user_reset(comp->div);
	compReset(comp, res);
	tapStop(comp->tape);
	comp->tape->userStop = 0;
}

void compReset(Computer* comp,int res) {
	int i;
	comp->frmCount = 0;
	for (i = 0; i < comp->pwcount; i++) {	// what a watched port held is history now
		comp->pwatch[i].val = -1;
	}
	if (comp->rzx.play)
		rzxStop(comp);
	rzx_rec_touch();		// a recording goes on from the machine as the reset leaves it

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
	difReset(comp->dif);
	// a level left high is a dc on the mix, and the ear bit reads it back
	bcReset(comp->beep);
	if (comp->gs->reset)
		gsReset(comp->gs);
	tsReset(comp->ts);
	ideReset(comp->ide);
	saaReset(comp->saa);
	sdcReset(comp->sdc);
	div_reset(comp->div);		// what it holds and shows stays: /RESET reaches only its SPI side
	comp->hwMul = comp->turboStep;		// a port turbo sets its own again, a switch does not
	comp_update_timings(comp);
	if (comp->hw->reset)
		comp->hw->reset(comp);
	comp->hw->mapMem(comp);
	cpu_reset(comp->cpu);
	comp_set_snow(comp, comp->flgSNOW);	// the cpu may have been swapped since
	comp_set_cont(comp, comp->flgCNTM);
	comp_heat_sync(comp);		// ram/rom size may have changed with hardware/romset
}

// T in one frame of this machine, exact given a precise nsPerTick. A call and
// not comp->fCount: that field is only filled at an interrupt, so it is stale
// on a machine that has just been reset or had its timings changed.
double comp_fps(Computer* comp) {
	return (comp->vid->nsPerFrame > 0) ? 1e9 / comp->vid->nsPerFrame : 50;
}

int comp_frame_ticks(Computer* comp) {
	if (comp->nsPerTick <= 0) return 0;
	return (int)llround(comp->vid->nsPerFrame / comp->nsPerTick);
}

// The frame length in T, 0 when the machine has no usable frame to stand in
static int comp_frame_ok(Computer* comp) {
	int flen = comp_frame_ticks(comp);
	return ((flen > 0) && (comp->vid->dotPerFrame > 0)) ? flen : 0;
}

// Ticks and dots past the INT, one the inverse of the other. The cpu's ticks
// start on dots that are a multiple of a tick from where the blanking counters
// start, as they have since the machine was made, so on a layout whose INT dot
// is odd the cpu sees the pulse half a T in - and a snapshot has to be stood on
// the same grid. A tick that is not a whole number of dots is scaled instead.
static int comp_tick_to_dots(Computer* comp, int flen, int tick) {
	Video* vid = comp->vid;
	int dpt = vid->dotPerFrame / flen;
	if (dpt * flen != vid->dotPerFrame)
		return (int)((long long)tick * vid->dotPerFrame / flen);
	return tick * dpt - (vid->intp.y * vid->full.x + vid->intp.x) % dpt;
}

static int comp_dots_to_tick(Computer* comp, int flen, int dots) {
	Video* vid = comp->vid;
	int dpt = vid->dotPerFrame / flen;
	if (dpt * flen != vid->dotPerFrame)
		return (int)((long long)dots * flen / vid->dotPerFrame);
	return ((dots + (vid->intp.y * vid->full.x + vid->intp.x) % dpt) / dpt) % flen;
}

// Where a snapshot with no frame position (.sna, .z80 v1/v2) is put. With
// interrupts off it is taken as saved the way a running machine saves one: at
// the end of the INT acknowledge. The instruction the INT lands in ends 2 to 5 T
// after it, which the file cannot say, so it is taken as 2. With them on it
// was saved at some point mid-frame, and goes just after the pulse: an INT
// taken on load breaks code that times itself to the interrupt.
static int comp_snap_tick(Computer* comp, int flen) {
	if (!comp->cpu->flgIFF1)
		return 2 + ((comp->cpu->regIM == 2) ? 19 : 13);
	return (int)(((long long)comp->vid->intsize * flen + comp->vid->dotPerFrame - 1) / comp->vid->dotPerFrame);
}

// Stand the machine at tick T of its frame, counted from the interrupt, as a
// snapshot taken mid-frame asks for. The tick counter and the beam have to
// agree, and how many dots a tick is worth is the video's business. A snapshot
// that does not say (-1) lands at comp_snap_tick().
void comp_set_frame_tick(Computer* comp, int tick) {
	Video* vid = comp->vid;
	int flen = comp_frame_ok(comp);
	if (!flen) {
		comp->frmtCount = 0;
		vid_set_ray(vid, -1);
		return;
	}
	if (tick < 0) tick = comp_snap_tick(comp, flen);
	if (tick >= flen) tick = flen - 1;
	int dots = comp_tick_to_dots(comp, flen, tick);
	comp->frmtCount = tick;
	vid_set_ray(vid, dots);
	// the INT line as it stands at that tick, not as the reset before left it,
	// and not yet sampled by the cpu: the machine raises it its own way and
	// the pulse is cut to what is left of it
	comp->cpu->flgACK = 0;
	vid_set_int_frame(vid, 0);
	comp->cpu->intrq &= ~Z80_INT;
	int left = vid->intsize - dots;
	if ((dots >= 0) && (left > 0) && (vid->inten & 1) && comp->hw->irq && !comp->rzx.play) {
		comp->hw->irq(comp, IRQ_VID_INT);
		if (vid->intFRAME > left) vid->intFRAME = left;
	}
}

// A core with something of its own to do when the power comes on (the Next's
// firmware loading its ROMs) does it at the reset that follows.
void comp_power_on(Computer* comp) {
	if (comp->hw && comp->hw->power)
		comp->hw->power(comp);
}

// The ULA comes up at any point of its frame (Time to First INT on a real 48K);
// a reset leaves it where it is.
void comp_power_phase(Computer* comp) {
	static unsigned int calls = 0;
	int flen = comp_frame_ok(comp);
	if (!flen) return;
	comp_set_frame_tick(comp, (int)((((xhost_seed() + calls++) * 2654435761u) >> 8) % (unsigned int)flen));
}

// A divMMC takes the place of the disk systems: while it is on, the floppy and
// hard disk interfaces are out of the machine, and they come back with their
// drives and disks when it goes. The board owns #3Dxx, so TR-DOS (and SMUC with
// it) never runs, and a +3 enters ROM 1 through #3D00 too; a Nemo IDE would
// work beside it, but the rule is one for all.
void comp_board_storage(Computer* comp) {
	int out = (comp->div->model != DIV_NONE);
	dif_set_out(comp->dif, out);
	ide_set_out(comp->ide, out);
}

// Where comp_set_frame_tick() would have to put the machine back to. Taken off
// the beam, not frmtCount: that is zeroed inside the instruction the INT lands
// in and then given all of its T, so it runs a few T ahead of the beam.
int comp_get_frame_tick(Computer* comp) {
	int flen = comp_frame_ok(comp);
	return flen ? comp_dots_to_tick(comp, flen, vid_ray_dots(comp->vid)) : 0;
}

// All a snapshot says about paging is the 7FFD byte, so before one is applied
// the machine has to stand where a 128K stands. A machine that pages through a
// pager of its own comes up from reset under its firmware instead, and there a
// snapshot would run the service rom; it puts its pager where a 128K's is here.
void comp_snap_map(Computer* comp) {
	if (comp->hw->snapmap)
		comp->hw->snapmap(comp);
}

// The reset a snapshot loader starts from. A recording being played is left
// playing: the snapshot may be the one inside it, read from its open file.
void comp_snap_reset(Computer* comp, int res) {
	int play = comp->rzx.play;
	comp->rzx.play = 0;
	compReset(comp, res);
	div_snap_clear(comp->div);
	comp->rzx.play = play;
	comp_snap_map(comp);
	comp_heat_reset(comp);
}

// cpu freq

// ns since the machine was made, each stretch at the clock it ran at
long long comp_ns(Computer* comp) {
	long long d = comp->tickTotal + (comp->cpu ? comp->cpu->t : 0) - comp->nsBaseTick;
	// no clock set yet (cpuFrq 0 makes nsPerTick infinite): no time has passed
	if (!d || !isfinite(comp->nsPerTick)) return comp->nsBase;
	return comp->nsBase + (long long)((double)d * comp->nsPerTick);
}

void comp_update_timings(Computer* comp) {
	// the time so far at the old rate, before the new one is set
	comp->nsBase = comp_ns(comp);
	comp->nsBaseTick = comp->tickTotal + (comp->cpu ? comp->cpu->t : 0);
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
	// the video is only ever handed whole ticks: whole dots too, where a tick is
	comp->vid->tickWhole = (comp->nsPerTickFixed > 0) && !(comp->nsPerTickFixed % comp->vid->nsPerDotFixed);
	comp->earTpt = (1e9 / 3.5e6) / comp->nsPerTick;	// machine ticks in a 3.5 MHz T, for zx_ear_edge
	// contention wait, dots -> ticks: both periods are settled just above, and
	// nothing moves either without coming back through here
	for (int i = 0; i < 16; i++)
		comp->contTicks[i] = ns_fixed_to_ticks_up(comp, (long long)i * comp->vid->nsPerDotFixed);
	comp_set_cont(comp, comp->flgCNTM);	// a turbo arbiter follows hwMul
}

void compSetBaseFrq(Computer* comp, double frq) {
	if (frq > 0)
		comp->cpuFrq = frq;
	if (comp->ts->frq <= 0)
		ts_set_frq(comp->ts, 0, comp->cpuFrq);
	comp_update_timings(comp);
}

// Two multipliers, because two parties set one: the user asks for turbo from
// the options page or the hotkey, and a Scorpion, ATM, Pentagon 1024 or ZX
// Evolution turns its own on from a port. Keeping them apart is what lets a
// reset put the machine's back without touching what the user asked for.

void compSetTurbo(Computer* comp, double mult) {
	if (comp->frqMul == mult) return;
	comp->frqMul = mult;
	comp_update_timings(comp);
}

void compSetHwTurbo(Computer* comp, double mult) {
	if (comp->hwMul == mult) return;
	comp->hwMul = mult;
	comp_update_timings(comp);
}

void comp_set_layout(Computer* comp, vLayout* lay) {
	if (comp->hw->ownLay) return;
	if (comp->hw->lay)
		lay = comp->hw->lay;
	vid_set_layout(comp->vid, lay);
}

// The snow effect costs a video sync on every opcode fetch, so the cpu only
// reports the refresh cycle while a machine actually wants it.
// The start of a bus cycle on a machine that contends one: the ray up to it,
// then the wait states. Called straight from the cpu rather than through
// comp_irq and a machine's own irq handler - this is what every one of them
// would do with it, and it runs on every memory access.
// The cheap tests come first: an address the pattern cannot touch needs no
// video sync at all (vid_wait_dots would return 0 before reading the ray),
// so res4 just lags until something that matters syncs. The ray is walked to
// cpu->t only for an address that can actually wait.
static void comp_cont(void* ptr, int mreq) {
	Computer* comp = (Computer*)ptr;
	// one compare instead of a page lookup per tick of every z80_wait
	if (!vid_cont_internal(comp->vid, mreq)) return;
	MemPage* pg = mem_get_page(comp->mem, comp->cpu->adr);
	if (pg->type != MEM_RAM) return;
	int adr = pg->num << comp->mem->pgshift;
	if (!vid_cont_hit(comp->vid, adr, mreq)) return;
	comp_vid_to_t(comp);
	zx_contend_adr(comp, adr, mreq);
}

// A machine that arbitrates the bus itself (hw->cont). What the last opcode
// still owes (waitDebt) is paid first, so the wait is taken from where the
// cycle really starts.
static void comp_cont_hw(void* ptr, int mreq) {
	Computer* comp = (Computer*)ptr;
	comp->cpu->t += comp->waitDebt;
	comp->waitPaid += comp->waitDebt;
	comp->waitDebt = 0;
	if (!comp->hw->contLazy) comp_vid_to_t(comp);
	comp->hw->cont(comp, mreq);
}

// Contended memory. The cpu reports the start of every bus cycle for it, and
// that is a call per memory access, so it only does so when a machine asks.
// A board's own arbiter (hw->cont) is its turbo's, so it is asked only in turbo.
// The pages comp_cont() can contend at all, for the cpu to skip the call
// everywhere else: ram in the slow banks of the pattern, internal cycles only on
// the Ferranti ULA (vid_cont_internal, vid_cont_hit). A board's own arbiter
// wants every cycle.
static void comp_cont_map(Computer* comp) {
	if (!comp->cpu || !comp->vid) return;
	mem_set_cont(comp->mem, vid_cont_bit(comp->vid));
	comp->cpu->contp = (comp->cpu->xcont == comp_cont) ? comp->mem->cont : NULL;
	comp->cpu->contInt = vid_cont_internal(comp->vid, 0);
}

void comp_set_cont(Computer* comp, int on) {
	comp->flgCNTM = on ? 1 : 0;
	if (comp->cpu) {
		int own = comp->hw && comp->hw->cont && ((comp->hwMul > 1) || comp->hw->contAll);
		comp->cpu->flgCONT = comp->flgCNTM || own;
		comp->cpu->xcont = own ? comp_cont_hw : comp_cont;
	}
	comp_cont_map(comp);
}

void comp_set_cont_pattern(Computer* comp, int type) {
	comp->vid->ula->conttype = type;
	comp_cont_map(comp);
}

// the ULA is disturbed only from a slow bank (zx_snow), so only there is the
// refresh reported
void comp_set_snow(Computer* comp, int on) {
	comp->flgSNOW = on ? 1 : 0;
	if (comp->cpu) {
		comp->cpu->flgRFSH = comp->flgSNOW;
		comp->cpu->rfshp = comp->mem ? comp->mem->slow : NULL;
	}
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
	// the Z80N's hooks and what they ran up belong to the Next: no other machine keeps them
	if (hw->id != HW_ZXNEXT) {
		comp->cpu->xinta = NULL;
		comp->cpu->xnextreg = NULL;
		if (comp->hw && (comp->hw->id == HW_ZXNEXT)) comp->waitDebt = 0;
	}
	comp->hw = hw;
	// a host key's combination as the machine's keyboard makes it: the Evo's avr, or
	// for the rest (none has a pc keyboard of its own) the Next's scan
	if ((hw->id == HW_PENTEVO) || (hw->id == HW_TSLAB))
		kbd_set_combo(comp->keyb, KBD_EVO_LEAD, 0);
	else
		kbd_set_combo(comp->keyb, KBD_SCAN_NS, 2 * KBD_SCAN_NS);
//	comp->cpu->nod = 0;
	comp->vid->mrd = vid_mrd_cb;
	comp->tape->xen = 0;
	comp->tape->is48 = (hw->id == HW_ZX48);	// what a TZX's "stop if 48K" means
	compSetBaseFrq(comp, 0);	// recalculations
	comp_pwatch_sync(comp);
	return 1;
}

// exec 1 opcode, sync devices, return eated ns

// The end of a step: the video is synced over the T not synced yet, the counters
// take all of them, the devices follow and a new frame is flagged. Returns the ns
// the step took.
static int comp_step_end(Computer* comp, int unsynced, int t) {
	// counted, not walked: the ns are worked out as the time is handed in, and
	// what reads the ray settles it first
	vid_sync_lazy(comp->vid, ticks_to_ns_fixed(comp, unsynced));
	nsTime = vid_step_ns(comp->vid);
	comp->tickCount += t;
	comp->tickTotal += t;
	comp->frmtCount += t;
	cbHwSnc sync = comp->hw->sync;
	if (sync == zx_sync)
		zx_sync_in(comp, nsTime);
	else if (sync)
		sync(comp, nsTime);
	if (comp->vid->newFrame) {
		comp->vid->newFrame = 0;
		comp->flgFRM = 1;
		if (xreplay_on) xreplay_frame(comp);
	}
	return nsTime;
}

// The firmware's part of a power-on runs before the first instruction, not in the
// reset: the media go in after the machine is built, and the firmware reads them.
static void comp_boot(Computer* comp) {
	comp->flgBOOT = 0;
	if (comp->hw->boot) comp->hw->boot(comp);
}

int compExec(Computer* comp) {
	if (comp->flgBOOT) comp_boot(comp);
	comp->flgBRKPRE = 0;
// breakpoints. A run-ahead frame is thrown away, so a break there would fire
// twice: leave it to the pass that keeps its result
	if (!comp->flgDBG && !x_runahead) {
		bpChecker ch;
		ch.t = -1;
		if (comp->flgBRKMEM)
			ch = comp_check_bp(comp, cpu_get_pc(comp->cpu), MEM_BRK_FETCH | MEM_BRK_TFETCH, MEM_VIEW_FETCH);
		if (ch.t >= 0) {
			comp->flgBRK = 1;
			comp->brkt = ch.t;
			comp->brka = ch.a;
			comp->brkev.kind = MEM_BRK_FETCH;
			comp->brkev.adr = cpu_get_pc(comp->cpu);
			comp->flgBRKPRE = 1;
			if (*ch.ptr & MEM_BRK_TFETCH) {
				*ch.ptr &= ~MEM_BRK_TFETCH;
				comp->brkt = -1;		// temp (not in list)
			}
			return 0;
		}
		if (comp->cpu->intrq && comp->flgIBRK) {
			comp->flgBRK = 1;
			comp->brkt = BRK_IRQ;
			comp->flgBRKPRE = 1;
			return 0;
		}
	}
// start
	res4 = 0;
	if (comp->rzx.rec.on && (rzx_rec_touched || rzx_rec_marking))
		rzx_rec_pre(comp);
// exec cpu opcode OR handle interrupt. get T states back
	res2 = cpu_exec(comp->cpu);
	if (comp->rzx.play) {
		if (comp->rzx.frm.fetches == 0) {
			vid_unlazy(comp->vid);
			if (comp->hw->irq)
				comp->hw->irq(comp, IRQ_RZX_INT);
		}
	} else if (comp->rzx.rec.on) {
		rzx_rec_step(comp, res2);
		if (comp->cpu->flgNMIOK)	// no frame of the format holds an NMI: a join after it
			rzx_rec_touch();
	}
	if (comp->cpu->flgINTOK) comp->intTaken++;
	comp->cpu->flgINTOK = 0;		// asked by the recorder and the count above: no trace of it in a snapshot
	comp->cpu->flgNMIOK = 0;
	return comp_step_end(comp, res2 - res4, res2);
}

// What fast mode asks of any machine before it puts steps together (comp_halt_skip,
// comp_run_quiet): the core is the Z80's own, ending in comp_end(); nothing looks
// between the steps (no watch, breakpoint, debugger, run-ahead pass, recording); no
// NMI or spoiled fetch waits; the devices take their time summed; and the machine's
// own step end allows it (hwQuiet.ok). NULL: step one at a time.
static const hwQuiet* comp_quiet_in(Computer* comp, int ahead) {
	const hwQuiet* q = comp->hw->quiet;
	CPU* cpu = comp->cpu;
	if (!q || !cpu->core || (cpu->core->exec != z80_exec) || (cpu->xend != comp_end)) return NULL;
	// a run-ahead pass only keeps breakpoints and the heat map out (compExec)
	int seen = ahead ? (comp->flgCOND || comp->flgDBG || comp->flgBRKMEM || comp->flgIBRK
		|| comp->rzx.play || comp->rzx.rec.on) : !comp_unwatched(comp);
	if (seen || comp->flgMAP || comp->flgHEAT) return NULL;
	if (comp->flgNMIRQ || comp->snowBad) return NULL;
	if (!comp_devs_summable(comp)) return NULL;
	if (q->ok && !q->ok(comp)) return NULL;
	return q;
}

static const hwQuiet* comp_quiet_ok(Computer* comp) {
	return comp_quiet_in(comp, 0);
}

// An M1 at adr does nothing but fetch, on a machine reading through stdMRd/asicMRd
// (hwq_zx's fetch): it pages no TR-DOS and the ULA does not contend it
int zx_fetch_quiet(Computer* comp, int adr) {
	if (div_m1_acts(comp->div, adr)) return 0;
	MemPage* pg = mem_get_page(comp->mem, adr);
	if ((comp->dif->type == DIF_BDI) && bdi_fetch_pages(comp, pg, adr)) return 0;
	if (!comp->cpu->flgCONT) return 1;
	if (comp->cpu->xcont != comp_cont) return 0;
	return !comp->mem->cont[(adr >> 8) & 0xff];		// the pages vid_cont_hit() hits (comp_cont_map)
}

// A halted cpu runs an M1 every 4 T at the byte after the HALT, and with nothing to
// wake it up nothing but the time moves. Up to the next thing the machine would see
// - the INT, the frame end, a line INT - the steps are taken in one go, leaving what
// they would have left: R counted on, the ticks counted, the video and the devices
// given the time (comp_quiet_ok). Not where the fetch itself does anything
// (hwQuiet.fetch: contention, TR-DOS paging, a cache it changes), nor with the
// refresh snowing from a slow bank.
// Fast mode only, but on a machine with its own hwQuiet.halt: in real time the
// sound takes samples between the steps, so the run ends with the step that takes
// the time past room (fixed ns to the next sample, -1: no sound).
// 1 when it went, *ns the time it took.
static int comp_halt_run(Computer* comp, int* ns, long long room, int rt) {
	CPU* cpu = comp->cpu;
	if (!cpu->flgHALT || (cpu->intrq & cpu->inten) || comp->flgBOOT) return 0;
	// a step before the sample at least, or the run does not pay: at 3.5 MHz the
	// sound samples every step
	if ((room >= 0) && (room < 4 * comp->nsPerTickFixed)) return 0;
	const hwQuiet* q = comp_quiet_in(comp, rt);
	if (!q || (rt && !q->halt)) return 0;
	if (cpu->flgRFSH && (!cpu->rfshp || cpu->rfshp[cpu->regI])) return 0;	// zx_snow's banks
	int adr = (cpu->regPC + 1) & 0xffff;
	int t = 4;
	long long until = 0;
	if (q->halt) {
		t = q->halt(comp, adr, &until);
		if (!t) return 0;
	} else if (!q->fetch || !q->fetch(comp, adr)) {
		return 0;
	}
	Video* vid = comp->vid;
	vid_settle(vid);
	if (vid->nsDrawFixed < 0) return 0;
	long long d = vid_quiet_dots(vid, q->lineInt && (vid->inten & 2));
	long long step = t * comp->nsPerTickFixed;
	if (step <= 0) return 0;
	long long n = (d * vid->nsPerDotFixed - vid->nsDrawFixed - 1) / step;
	if (q->halt) {
		// the steps starting short of until: at tickTotal, then every t
		long long m = (until - comp->tickTotal + t - 1) / t;
		if (until <= comp->tickTotal) m = 0;
		if (m < n) n = m;
	}
	if ((room >= 0) && (n >= 2)) {
		// up to the step after which the sound's next sample is due, that one
		// included: the caller takes it with the machine where that step leaves it
		int k = vid_clock_count(vid, step, room, (int)((n < 0x10000) ? n : 0x10000));
		if (k < n) n = k;
	}
	if (n < 2) return 0;
	if (n > 0x10000) n = 0x10000;
	if (q->haltDone) q->haltDone(comp, adr, comp->tickTotal, n, t);
	cpu->regR = (unsigned char)(cpu->regR + n);
	cpu->flgQ = 0;			// what the steps before left: HALT writes no flags
	cpu->flgFW = 0;
	comp->flgBRKPRE = 0;
	res4 = 0;
	*ns = comp_step_end(comp, (int)(n * t), (int)(n * t));
	return 1;
}

int comp_halt_skip(Computer* comp, int* ns) {
	return comp_halt_run(comp, ns, -1, 0);
}

// The same in real time or a run-ahead pass, for a machine with hwQuiet.halt; room:
// the fixed ns to the sound's next sample, -1 when nothing samples
int comp_halt_skip_rt(Computer* comp, int* ns, long long room) {
	return comp_halt_run(comp, ns, room, 1);
}

// The core's straight way to memory for the step about to run (CPU.rdp/m1p/wrp):
// what memrd() and memwr() come to on a machine whose reads and writes are
// stdMRd/stdMWr and that nothing watches - but for the opcode fetches that page
// TR-DOS in or out (m1p, m1dos leave those pages to stdMRd) and a fetch the
// snow spoils (no table while snowBad stands; zx_snow takes them away when it
// sets it). stdMRd() takes m1p away when it pages, for the rest of the step.
// The +2A/+3's asicMRd/asicMWr are stdMRd/stdMWr outside banks 4-7 (rdpa, wrpa).
// The fetch table, which stdMRd() takes away when it pages TR-DOS:
static inline unsigned char* const* comp_m1_tab(Computer* comp) {
	Memory* mem = comp->mem;
	int bdi = (comp->dif->type == DIF_BDI);
	if (div_pending(comp->div)) return NULL;
	if (comp->hw->mrd == asicMRd)
		return bdi ? NULL : mem->m1pa;
	return (comp->flgDOS && bdi) ? mem->m1dos : mem->m1p;
}

// The rule for the tables: they are picked from hw->mrd/mwr, the watch flags, snowBad,
// the rzx state, the bus mask, and flgDOS with a Beta disk. The pages they point to
// follow every remap by themselves (memSetBank keeps them), so only those inputs
// matter, and inside a step only two can move under them: stdMRd() paging TR-DOS
// (drops m1p) and zx_snow() setting snowBad (drops rdp, m1p). Anything else that sets
// them - an OUT that pages TR-DOS, a debugger flag - comes between steps, where the
// tables are picked again; a quiet run re-picks m1p after a drop and ends at i/o.
static inline void comp_mem_direct(Computer* comp) {
	CPU* cpu = comp->cpu;
	int rd = (comp->hw->mrd == stdMRd);
	int wr = (comp->hw->mwr == stdMWr);
	int asic = (comp->hw->mrd == asicMRd) && (comp->hw->mwr == asicMWr);
	if (!(rd | wr | asic)) return;
	if (comp_mem_watched(comp) || comp->snowBad || comp->rzx.play || comp->rzx.rec.on || (cpu->busmask != 0xffff)) return;
	Memory* mem = comp->mem;
	if (rd | asic) {
		cpu->rdp = asic ? mem->rdpa : mem->rdp;
		cpu->m1p = comp_m1_tab(comp);
	}
	if (wr | asic)
		cpu->wrp = asic ? mem->wrpa : mem->wrp;
}

// compExec() for a machine nothing looks at between its steps: no breakpoint,
// no debugger, no recording playing or being made (the caller makes sure)
int comp_exec_quiet(Computer* comp) {
	if (comp->flgBOOT) comp_boot(comp);
	comp->flgBRKPRE = 0;
	res4 = 0;
	comp_mem_direct(comp);
	CPU* cpu = comp->cpu;
	if (cpu->core && (cpu->core->exec == z80_exec) && (cpu->xend == comp_end))
		res2 = comp_exec_z80_in(comp, comp_ack_kind(comp));		// comp_end() inline
	else
		res2 = cpu_exec(cpu);
	comp->cpu->rdp = NULL;
	comp->cpu->m1p = NULL;
	comp->cpu->wrp = NULL;
	if (comp->cpu->flgINTOK) comp->intTaken++;
	comp->cpu->flgINTOK = 0;
	comp->cpu->flgNMIOK = 0;
	return comp_step_end(comp, res2 - res4, res2);
}

// The devices given the time of the instructions a quiet run has finished, as
// their own step ends would have given it: an i/o access is about to show them
// to the machine or to change them, and the run ends with that instruction. The
// NMI is left to the step end.
void comp_run_io_slow(Computer* comp) {
	comp->runIO = 1;
	long long s = comp->runStep;
	if (!s) return;
	Video* vid = comp->vid;
	comp->runStep = 0;
	vid->stepFixed -= s;
	int ns = vid_clock(vid, s);		// the dots s completes: what the steps' ns add up to
	comp->runNs += ns;
	comp->hw->quiet->devs(comp, ns);
}

// Fast mode: steps run on in one go while nothing but the instructions happens.
// Each one is the core's own - fetch, contention, snow, the INT sample - and only
// its step end is put off: the time made into ns and handed to the devices, which
// take it in any pieces alike (comp_devs_summable) - at the end of the run, or at
// an i/o access in between, which ends the run. It ends as well on a new frame,
// an interrupt the cpu takes next, a HALT (comp_halt_skip's), snow that spoils the
// next fetch, a breakpoint, or a pc in stops while the rom is in.
// 0 when the machine is not one to run on (the caller steps), else 1 and *ns.
int comp_run_quiet(Computer* comp, int* ns, const int* stops, int nstops) {
	CPU* cpu = comp->cpu;
	if (cpu->flgHALT || (cpu->intrq & cpu->inten) || comp->flgBOOT) return 0;
	const hwQuiet* q = comp_quiet_ok(comp);
	if (!q) return 0;
	// the machine's own part of a step end, done after every step but the last,
	// whose step end is the real one (hwQuiet.step)
	void (*step)(Computer*) = q->step;
	int ack = q->ack;
	cbHwSnc sync = comp->hw->sync;
	Video* vid = comp->vid;
	comp->flgBRKPRE = 0;
	comp->runOn = 1;
	comp->runIO = 0;
	comp->runNs = 0;
	comp->runStep = 0;
	comp_mem_direct(comp);
	int direct = (cpu->m1p != NULL);
	for (;;) {
		res4 = 0;
		int t = comp_exec_z80_in(comp, ack);
		// comp_step_end() but for the ns and the devices
		vid_sync_lazy(vid, ticks_to_ns_fixed(comp, t - res4));
		comp->tickCount += t;
		comp->tickTotal += t;
		comp->frmtCount += t;
		if (vid->newFrame || comp->runIO || comp->flgBRK || cpu->flgHALT || (cpu->intrq & cpu->inten) || comp->snowBad) break;
		if (stops && (cpu->regPC < 0x4000) && zx_rom_active(comp)) {
			int i;
			for (i = 0; (i < nstops) && (cpu->regPC != stops[i]); i++);
			if (i < nstops) break;
		}
		if (step) step(comp);
		comp->runStep = vid->stepFixed;
		if (direct && !cpu->m1p)		// stdMRd() paged TR-DOS
			cpu->m1p = comp_m1_tab(comp);
	}
	cpu->rdp = NULL;
	cpu->m1p = NULL;
	cpu->wrp = NULL;
	// what the instructions before the last are owed, then the last one's own step end
	comp_run_io_slow(comp);
	comp->runOn = 0;
	int last = vid_step_ns(vid);
	if (sync == zx_sync)
		zx_sync_in(comp, last);
	else
		sync(comp, last);
	if (vid->newFrame) {
		vid->newFrame = 0;
		comp->flgFRM = 1;
		if (xreplay_on) xreplay_frame(comp);
	}
	*ns = comp->runNs + last;
	return 1;
}

// Time passing with the cpu standing still: what compExec() does once the
// opcode has run, for a caller that has already put the cpu where that time
// leaves it. Syncing video and devices in one piece gives what a run of
// opcodes gives them - every one of them carries its fractions over.
int comp_skip_ticks(Computer* comp, int t) {
	res4 = 0;
	return comp_step_end(comp, t, t);
}

// breaks

// end of an instruction as breakpoint conditions see it: the mem/io events it
// caused are spent, and where the beam stands now is where the next one starts

void comp_brk_newstep(Computer* comp) {
	if (comp->vid) vid_settle(comp->vid);		// the ray as it stands, not as counted
	comp->brkev.rd = -1;
	comp->brkev.wr = -1;
	comp->brkev.mdt = -1;
	comp->brkev.in = -1;
	comp->brkev.out = -1;
	comp->brkev.val = -1;
	comp->brkev.kind = 0;
	comp->brkev.adr = -1;
	comp->brkray = comp->vid ? (comp->vid->ray.y * comp->vid->full.x + comp->vid->ray.x) : 0;
	// the cpu stands in front of this instruction: a breakpoint it fires
	// belongs here, wherever pc has moved on to by the time it is read
	comp->brkpc = comp->cpu ? cpu_get_pc(comp->cpu) : 0;
}

// activate breakpoint w/o type (exit to debuga)
void comp_brk(Computer* comp, int t) {
	comp->flgBRK = 1;
	comp->brkt = t;
}

static unsigned char dumBrk = 0x00;

unsigned char* getBrkPtr(Computer* comp, int madr) {
	unsigned char* ptr = comp_get_memcell_flag_ptr(comp, madr, MEM_VIEW_READ);
	if (!ptr) {
		dumBrk = 0;
		ptr = &dumBrk;
	}
	return ptr;
}

void setBrk(Computer* comp, int adr, unsigned char val) {
	unsigned char* ptr = getBrkPtr(comp, adr);
	if (ptr == NULL) return;
	if (val & 0x0f) comp->flgBRKMEM = 1;
	*ptr = (*ptr & 0xf0) | (val & 0x0f);
}

unsigned char getBrk(Computer* comp, int adr) {
	return getBrkView(comp, adr, MEM_VIEW_READ);
}

unsigned char getBrkView(Computer* comp, int adr, int access) {
	unsigned char* ptr = comp_get_memcell_flag_ptr(comp, adr, access);
	unsigned char res = ptr ? *ptr : 0x00;
	if (comp->mem->busmask < 0x10000) {
		res |= (comp->brkAdrMap[adr & comp->mem->busmask] & 0x0f);
	}
	return res;
}

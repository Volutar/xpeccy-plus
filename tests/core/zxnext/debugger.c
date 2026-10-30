#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spectrum.h"
#include "cpu/Z80/z80.h"

static int failures;

static void check(const char* name, int ok) {
	printf("%s: %s\n", name, ok ? "ok" : "FAIL");
	if (!ok) failures++;
}

static Computer* machine(void) {
	Computer* comp = compCreate();
	compSetHardware(comp, "ZXNext");
	compSetBaseFrq(comp, 3.5);
	compReset(comp, RES_DEFAULT);
	comp->flgBOOT = 0;
	comp->next->configMode = 0;
	comp->next->nr[0x80] = 0;
	comp->next->nr[0x83] = 0;
	comp->next->nr[0x8c] = 0;
	comp->next->p123b = 0;
	return comp;
}

static void automap(Computer* comp, int instant, int vectors) {
	comp->next->nr[0x0a] |= 0x10;
	comp->next->nr[0x83] = 1;
	comp->next->nr[0xb8] = vectors;
	comp->next->nr[0xb9] = vectors;
	comp->next->nr[0xba] = instant ? vectors : 0;
}

static int preview(Computer* comp, int adr, const unsigned char* expected, int length, const char* text) {
	unsigned char bytes[64] = {0};
	char buf[1024];
	xNextState* state = (xNextState*)malloc(sizeof(*state));
	memcpy(state, comp->next, sizeof(*state));
	CPU cpu = *comp->cpu;
	long long ticks = comp->tickTotal;
	xMnem mn = compDisasm(comp, adr, buf, bytes, sizeof(bytes));
	int ok = mn.len == length && !memcmp(bytes, expected, length);
	if (text) ok &= !strcmp(buf, text);
	ok &= !memcmp(state, comp->next, sizeof(*state));
	ok &= !memcmp(&cpu, comp->cpu, sizeof(cpu)) && ticks == comp->tickTotal;
	if (!ok) fprintf(stderr, "preview #%04X: %s, length %d\n", adr, buf, mn.len);
	free(state);
	return ok;
}

int debugger_check(void) {
	printf("== debugger\n");
	Computer* comp = machine();
	int ok = 1;
	for (int slot = 0; slot < 8; slot++) {
		int page = 0x20 + slot;
		int adr = (slot << 13) + 0x123;
		int abs = 0x40000 + (page << 13) + 0x123;
		next_mmu_set(comp, slot, page);
		comp->mem->ramData[abs] = 0xa0 + slot;
		xAdr cell;
		ok &= comp_peek(comp, adr, MEM_VIEW_READ, &cell) == 0xa0 + slot;
		ok &= cell.type == MEM_RAM && cell.abs == abs && next_nr_peek(comp, 0x50 + slot) == page;
		comp_poke(comp, adr, 0x80 + slot, 0);
		ok &= comp->mem->ramData[abs] == 0x80 + slot;
	}
	check("eight independent MMU slots", ok);
	compDestroy(comp);

	comp = machine();
	// LD (#0123),A in RAM, targeting protected ROM or an unmapped window.
	unsigned char store[] = {0x32, 0x23, 0x01};
	memcpy(comp->mem->ramData + 0x40000, store, sizeof(store));
	comp->mem->ramData[0x123] = 0x19;
	comp->cpu->regA = 0x55;
	comp->cpu->regPC = 0xc000;
	comp->brkAdrMap[0x123] = MEM_BRK_WR;
	comp->flgBRKMEM = 1;
	compExec(comp);
	check("CPU write breakpoint on ROM", comp->flgBRK && comp->brkt == BRK_CPUADR
		&& comp->brka == 0x123 && comp->mem->ramData[0x123] == 0x19);
	comp->brkAdrMap[0x123] = 0;
	comp->brkRamMap[0x123] = MEM_BRK_WR;
	comp->cpu->regPC = 0xc000;
	comp->flgBRK = 0;
	compExec(comp);
	check("protected ROM has no physical write", !comp->flgBRK);
	next_mmu_set(comp, 2, 0xff);
	comp->mem->ramData[0x40002] = 0x41;
	comp->brkAdrMap[0x4123] = MEM_BRK_WR;
	comp->cpu->regPC = 0xc000;
	compExec(comp);
	check("CPU write breakpoint on unmapped slot", comp->flgBRK && comp->brka == 0x4123);
	compDestroy(comp);

	comp = machine();
	automap(comp, 1, 2);
	comp->mem->ramData[7] = 0x01;
	comp->mem->ramData[8] = 0x12;
	comp->mem->ramData[9] = 0x34;
	comp->mem->ramData[0x10008] = 0;
	comp_heat_sync(comp);
	comp->flgHEAT = comp->flgMAP = 1;
	comp->cpu->regPC = 7;
	compExec(comp);
	check("operands do not trigger automap", comp->cpu->regBC == 0x3412 && !comp->next->dmHeld
		&& comp->heatRam.ex[8] == 1 && !comp->heatRam.ex[0x10008]
		&& (comp->brkRamMap[8] & 0xf0) == DBG_VIEW_EXEC && !(comp->brkRamMap[0x10008] & 0xf0));
	comp->cpu->regPC = 8;
	comp->brkRamMap[0x10008] |= MEM_BRK_RD;
	comp->flgBRKMEM = 1;
	compExec(comp);
	check("M1 heat and read breakpoint follow automap", comp->next->dmHeld
		&& comp->heatRam.ex[0x10008] == 1 && comp->heatRam.ex[8] == 1
		&& comp->flgBRK && comp->brka == 0x10008);
	compDestroy(comp);

	const unsigned char load[] = {0x01, 0x12, 0x34};
	comp = machine();
	automap(comp, 1, 1);
	comp->mem->ramData[0] = 0x11;
	memcpy(comp->mem->ramData + 0x10000, load, sizeof(load));
	comp->brkRamMap[0x10000] = MEM_BRK_FETCH;
	ok = preview(comp, 0, load, sizeof(load), "ld bc,#3412");
	ok &= (getBrkView(comp, 0, MEM_VIEW_FETCH) & MEM_BRK_FETCH) != 0;
	comp->brkRamMap[0x10000] = 0;
	comp->cpu->regPC = 0;
	compExec(comp);
	check("instant automap instruction and bytes", ok && comp->cpu->regBC == 0x3412);
	compDestroy(comp);

	comp = machine();
	automap(comp, 0, 1);
	comp->mem->ramData[0] = load[0];
	comp->mem->ramData[1] = 0x56;
	comp->mem->ramData[2] = 0x78;
	comp->mem->ramData[0x10000] = 0xc3;
	memcpy(comp->mem->ramData + 0x10001, load + 1, 2);
	ok = preview(comp, 0, load, sizeof(load), "ld bc,#3412");
	comp->cpu->regPC = 0;
	compExec(comp);
	check("delayed automap keeps the fetched opcode", ok && comp->cpu->regBC == 0x3412);
	compDestroy(comp);

	comp = machine();
	automap(comp, 0, 0);
	comp->next->nr[0xbb] = 0x40;
	comp->next->dmHeld = comp->next->dmMap = comp->next->dmNext = 1;
	comp->mem->ramData[0x11ff8] = load[0];
	memcpy(comp->mem->ramData + 0x1ff9, load + 1, 2);
	ok = preview(comp, 0x1ff8, load, sizeof(load), "ld bc,#3412");
	comp->cpu->regPC = 0x1ff8;
	compExec(comp);
	check("automap exit changes the operand view", ok && comp->cpu->regBC == 0x3412 && !comp->next->dmHeld);
	compDestroy(comp);

	comp = machine();
	automap(comp, 1, 2);
	const unsigned char index[] = {0xdd, 0x21, 0x12, 0x34};
	comp->mem->ramData[7] = index[0];
	memcpy(comp->mem->ramData + 0x10008, index + 1, 3);
	ok = preview(comp, 7, index, sizeof(index), "ld ix,#3412");
	comp->cpu->regPC = 7;
	compExec(comp);
	check("prefix M1 can enter automap", ok && comp->cpu->regIX == 0x3412);
	compDestroy(comp);

	comp = machine();
	automap(comp, 1, 2);
	const unsigned char bit[] = {0xdd, 0xcb, 0, 0x46};
	memcpy(comp->mem->ramData + 5, bit, sizeof(bit));
	comp->mem->ramData[0x10008] = 0x86;
	ok = preview(comp, 5, bit, sizeof(bit), "bit 0,(ix+#00)");
	comp->cpu->regPC = 5;
	compExec(comp);
	check("indexed CB final opcode is not M1", ok && comp->cpu->regPC == 9 && !comp->next->dmHeld);
	compDestroy(comp);

	comp = machine();
	automap(comp, 0, 1);
	comp->mem->ramData[0] = 0x3a;
	comp->mem->ramData[0x10000] = 0x5a;
	comp->mem->ramData[0x10001] = comp->mem->ramData[0x10002] = 0;
	unsigned char alias[3];
	char text[1024];
	xMnem mn = compDisasm(comp, 0, text, alias, sizeof(alias));
	check("data read does not reuse cached opcode", mn.len == 3 && mn.mem && mn.mop == 0x5a
		&& alias[0] == 0x3a && !alias[1] && !alias[2] && !comp->next->dmHeld);
	compDestroy(comp);

	comp = machine();
	comp->next->nr[0x83] = 2;
	comp->next->irq.mfNmi = 1;
	comp->mem->ramData[0x66] = 0x31;
	memcpy(comp->mem->ramData + 0x14066, load, sizeof(load));
	xAdr fetch = comp_get_xadr(comp, 0x66, MEM_VIEW_FETCH);
	ok = fetch.type == MEM_RAM && fetch.abs == 0x14066;
	ok &= comp_peek(comp, 0x66, MEM_VIEW_FETCH, NULL) == load[0] && !comp->next->irq.mfEnable;
	ok &= preview(comp, 0x66, load, sizeof(load), "ld bc,#3412");
	comp->cpu->regPC = 0x66;
	comp->brkRamMap[0x14066] = MEM_BRK_FETCH;
	comp->flgBRKMEM = 1;
	compExec(comp);
	ok &= comp->flgBRKPRE && comp->brka == 0x14066 && !comp->next->irq.mfEnable;
	comp->brkRamMap[0x14066] = 0;
	comp->flgBRK = 0;
	compExec(comp);
	check("Multiface NMI fetch and physical breakpoint", ok && comp->cpu->regBC == 0x3412 && comp->next->irq.mfEnable);
	compDestroy(comp);

	// a debugger edit goes to the cell a read shows, where the machine can write
	comp = machine();
	{
		unsigned char* ram = comp->mem->ramData;
		int mmu = 0x40000 + 0x30 * 0x2000 + 0x123;
		int l2 = 0x40000 + 0x20 * 0x2000 + 0x123;
		next_mmu_set(comp, 0, 0x30);
		comp->next->nr[0x12] = 0x10;		// Layer 2 in bank 16 = pages 20/21...
		comp->next->p123b = 1;			// ...mapped for writes only, over 0000-3FFF
		ram[mmu] = ram[l2] = 0;
		comp_poke(comp, 0x0123, 0x55, 0);
		ok = (ram[mmu] == 0x55) && (ram[l2] == 0);
		comp->next->p123b = 0;
		next_mmu_set(comp, 0, 0xff);		// ROM: only with "write to ROM"
		int rom = comp_get_xadr(comp, 0x0123, MEM_VIEW_READ).abs;
		ram[rom] = 0x11;
		comp_poke(comp, 0x0123, 0x66, 0);
		ok &= ram[rom] == 0x11;
		comp_poke(comp, 0x0123, 0x66, 1);
		ok &= ram[rom] == 0x66;
		comp->next->nr[0x83] |= 1;
		comp->next->pe3 = 0xc3;			// divMMC conmem, MAPRAM, bank 3: read only
		int div3 = 0x20000 + 3 * 0x2000 + 0x123;
		ram[div3] = 0x22;
		comp_poke(comp, 0x2123, 0x77, 0);
		ok &= ram[div3] == 0x22;
		comp_poke(comp, 0x2123, 0x77, 1);
		ok &= ram[div3] == 0x77;
		comp->next->pe3 = 0x82;			// bank 2: written
		int div2 = 0x20000 + 2 * 0x2000 + 0x123;
		comp_poke(comp, 0x2123, 0x88, 0);
		ok &= ram[div2] == 0x88;
	}
	check("edits land on the cell shown", ok);
	compDestroy(comp);

	// cells by name: the Next's RAM in 8K pages after its ROM and divMMC areas
	comp = machine();
	{
		static const struct {int abs, area, page, ofs; const char* tok;} cells[] = {
			{0x0123, CELL_ROM, 0, 0x123, "ROM0"},
			{0xc123, CELL_ROM, 3, 0x123, "ROM3"},
			{0x10123, CELL_DIVROM, 0, 0x123, "DIVROM"},
			{0x14123, CELL_MFROM, 0, 0x123, "MFROM"},
			{0x16123, CELL_MFRAM, 0, 0x123, "MFRAM"},
			{0x1c123, CELL_ALTROM, 1, 0x123, "ALTROM1"},
			{0x26123, CELL_DIV, 3, 0x123, "DIV3"},
			{0x40000 + 17 * 0x2000 + 0x123, CELL_RAM, 17, 0x123, ""},
			{0x1fffff, CELL_RAM, 223, 0x1fff, ""}
		};
		ok = 1;
		for (unsigned i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
			xCell c = comp_cell(comp, MEM_RAM, cells[i].abs);
			char buf[16];
			int type, abs, area, page;
			ok &= (c.area == cells[i].area) && (c.page == cells[i].page) && (c.ofs == cells[i].ofs);
			ok &= !strcmp(cell_token(c, buf), cells[i].tok);
			ok &= comp_cell_xadr(comp, c.area, c.page, c.ofs, &type, &abs) && (type == MEM_RAM) && (abs == cells[i].abs);
			if (cells[i].tok[0]) ok &= cell_token_parse(buf, &area, &page) && (area == c.area) && (page == c.page);
			if (!ok) fprintf(stderr, "cell %06X\n", cells[i].abs);
		}
		int type, abs;
		ok &= !comp_cell_xadr(comp, CELL_RAM, 0xe0, 0, &type, &abs) && !comp_cell_xadr(comp, CELL_DIV, 3, 0x2000, &type, &abs);
		ok &= comp_cell_pages(comp, CELL_RAM) == 0xe0 && comp_cell_unit(comp, CELL_RAM) == 0x2000;
	}
	compDestroy(comp);
	comp = compCreate();
	compSetHardware(comp, "ZX128");
	ok &= comp_cell_pages(comp, CELL_DIV) == 0;		// no board, no pages
	div_set_model(comp->div, DIV_MMC, 16);
	{
		xCell c = comp_cell(comp, MEM_RAM, (7 << 14) | 0x123);
		int type, abs;
		ok &= (c.area == CELL_RAM) && (c.page == 7) && (c.ofs == 0x123) && (c.unit == 0x4000);
		c = comp_cell(comp, MEM_DIV, DIV_EE_SIZE + 3 * 0x2000 + 0x10);
		ok &= (c.area == CELL_DIV) && (c.page == 3) && (c.ofs == 0x10);
		ok &= comp_cell_xadr(comp, CELL_DIV, 3, 0x10, &type, &abs) && (type == MEM_DIV) && (abs == DIV_EE_SIZE + 3 * 0x2000 + 0x10);
		c = comp_cell(comp, MEM_DIV, 0x123);
		ok &= (c.area == CELL_DIVROM) && (c.ofs == 0x123);
	}
	check("cells by area and page", ok);
	// a divMMC cell takes a breakpoint of its own on other machines
	div_out(comp->div, 0xe3, 0x82);		// conmem, bank 2 at #2000
	{
		int cell = DIV_EE_SIZE + 2 * 0x2000 + 0x123;
		comp->div->data[cell] = 0x00;		// NOP
		comp->div->brkMap[cell] = MEM_BRK_FETCH;
		comp->flgBRKMEM = 1;
		comp->flgBRK = 0;
		comp->cpu->regPC = 0x2123;
		compExec(comp);
		ok = comp->flgBRK && (comp->brkt == BRK_MEMDIV) && (comp->brka == cell);
		ok &= (getBrk(comp, 0x2123) & MEM_BRK_FETCH) != 0;
		comp->div->brkMap[cell] = 0;
	}
	check("divMMC cell breakpoint", ok);
	// RST 8 has fetched at #0008: the board pages in for the next fetch, at #000B
	div_out(comp->div, 0xe3, 0x00);		// no conmem: the automapper alone
	div_m1(comp->div, 0x0100);		// mapcond and automap down
	div_m1(comp->div, 0x0100);
	div_m1(comp->div, 0x0008);		// the entry point's own fetch: still the ROM
	{
		unsigned char code[] = {0x3e, 0x42};	// LD A,#42 in the board's EEPROM
		memcpy(comp->div->data + 0x000b, code, sizeof(code));
		int mapcond = comp->div->mapcond, automap = comp->div->automap, shown = comp->div->shown;
		unsigned char bytes[4];
		char text[64];
		xMnem mn = compDisasm(comp, 0x000b, text, bytes, sizeof(bytes));
		xAdr fetch = comp_get_xadr(comp, 0x000b, MEM_VIEW_FETCH);
		xAdr now = comp_get_xadr(comp, 0x000b, MEM_VIEW_READ);
		ok = (mn.len == 2) && !strcmp(text, "ld a,#42") && (bytes[0] == 0x3e) && (bytes[1] == 0x42);
		ok &= (fetch.type == MEM_DIV) && (fetch.abs == 0x000b) && (now.type != MEM_DIV);
		ok &= (comp->div->mapcond == mapcond) && (comp->div->automap == automap) && (comp->div->shown == shown);
		// with the debugger holding the machine, every view shows the armed paging done
		comp->flgDBG = 1;
		ok &= (comp_get_xadr(comp, 0x0100, MEM_VIEW_READ).type == MEM_DIV) && (comp_peek(comp, 0x000c, MEM_VIEW_READ, NULL) == 0x42);
		ok &= comp_get_xadr(comp, 0x0100, MEM_VIEW_READ | MEM_VIEW_LIVE).type != MEM_DIV;
		comp->flgDBG = 0;		// breakpoints are not checked while it holds the machine
		comp->div->brkMap[0x000b] = MEM_BRK_FETCH;
		comp->flgBRKMEM = comp->flgMAP = 1;
		comp->flgBRK = 0;
		comp->cpu->regPC = 0x000b;
		compExec(comp);		// stops before it runs
		ok &= comp->flgBRK && (comp->brkt == BRK_MEMDIV) && (comp->brka == 0x000b);
		comp->div->brkMap[0x000b] = 0;
		comp->flgBRK = 0;
		comp->flgBRKMEM = 0;
		comp->cpu->regA = 0;
		comp->cpu->regPC = 0x000b;
		comp->flgDBG = 1;		// a step in the debugger: the machine still reads the live map
		compExec(comp);
		ok &= (comp->cpu->regA == 0x42) && (comp->div->brkMap[0x000b] & DBG_VIEW_EXEC);
		comp->flgMAP = 0;
		comp->flgDBG = 0;
		if (!ok) fprintf(stderr, "entry preview: '%s' len %d, fetch %d:%X, A %02X\n", text, mn.len, fetch.type, fetch.abs, comp->cpu->regA);
	}
	check("divMMC entry point on the PC row", ok);
	compDestroy(comp);

	comp = machine();
	unsigned char longCode[27];
	memset(longCode, 0xdd, 24);
	longCode[24] = 0x21;
	longCode[25] = 0x12;
	longCode[26] = 0x34;
	memcpy(comp->mem->ramData + 0x48000, longCode, sizeof(longCode));
	check("long prefix chain", preview(comp, 0x8000, longCode, sizeof(longCode), "ld ix,#3412"));
	compDestroy(comp);

	// a dump by area: every cell is the byte the core holds, a range runs on into the
	// next pages, and stops at the area's end
	comp = machine();
	{
		static const struct {int area, page, ofs, abs;} cells[] = {
			{CELL_ROM, 0, 0, 0}, {CELL_ROM, 3, 0x3fff, 0xffff},
			{CELL_DIVROM, 0, 0x1fff, 0x11fff}, {CELL_MFROM, 0, 5, 0x14005}, {CELL_MFRAM, 0, 0x1fff, 0x17fff},
			{CELL_ALTROM, 1, 0x10, 0x1c010}, {CELL_DIV, 15, 0x1fff, 0x3ffff},
			{CELL_RAM, 0, 0, 0x40000}, {CELL_RAM, 0xdf, 0x1fff, 0x1fffff},
			{CELL_RAM, 5, 0x2000, 0x40000 + 6 * 0x2000}	// past page 5: page 6
		};
		ok = 1;
		for (unsigned i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
			comp->mem->ramData[cells[i].abs] = 0x40 + i;
			ok &= comp_cell_peek(comp, cells[i].area, cells[i].page, cells[i].ofs) == (int)(0x40 + i);
			if (!ok) fprintf(stderr, "dump cell %i\n", i);
		}
		ok &= comp_cell_peek(comp, CELL_RAM, 0xdf, 0x2000) < 0 && comp_cell_peek(comp, CELL_DIV, 0, 0x20000) < 0;
		ok &= comp_cell_size(comp, CELL_RAM) == 0xe0 * 0x2000 && comp_cell_size(comp, CELL_ROM) == 0x10000;
		ok &= comp_cell_size(comp, CELL_SLT) == 0;
		// ROM areas take a write only with ROM writes allowed
		ok &= !comp_cell_poke(comp, CELL_ROM, 1, 0, 0x99, 0) && comp->mem->ramData[0x4000] != 0x99;
		ok &= comp_cell_poke(comp, CELL_ROM, 1, 0, 0x99, 1) && comp->mem->ramData[0x4000] == 0x99;
		ok &= comp_cell_poke(comp, CELL_RAM, 0x0a, 0x2001, 0x77, 0) && comp->mem->ramData[0x40000 + 0x0b * 0x2000 + 1] == 0x77;
	}
	compDestroy(comp);
	comp = compCreate();
	compSetHardware(comp, "ZX128");
	memSetSize(comp->mem, MEM_128K, MEM_32K);	// as the machine's config sets it
	div_set_model(comp->div, DIV_MMC, 16);
	{
		comp->mem->ramData[7 * 0x4000 + 0x3fff] = 0x17;
		comp->mem->romData[0x4000] = 0x21;
		comp->div->data[DIV_EE_SIZE + 3 * 0x2000 + 0x10] = 0x33;
		comp->div->data[0x123] = 0x44;
		ok &= comp_cell_peek(comp, CELL_RAM, 0, 7 * 0x4000 + 0x3fff) == 0x17 && comp_cell_peek(comp, CELL_RAM, 7, 0x3fff) == 0x17;
		ok &= comp_cell_peek(comp, CELL_RAM, 7, 0x4000) < 0 && comp_cell_size(comp, CELL_RAM) == 0x20000;
		ok &= comp_cell_peek(comp, CELL_ROM, 0, 0x4000) == 0x21 && comp_cell_size(comp, CELL_ROM) == 0x8000;
		ok &= comp_cell_peek(comp, CELL_DIV, 2, 0x2010) == 0x33 && comp_cell_peek(comp, CELL_DIVROM, 0, 0x123) == 0x44;
		ok &= comp_cell_size(comp, CELL_DIV) == 16 * 0x2000 && comp_cell_size(comp, CELL_ALTROM) == 0;
		comp->div->eeDirty = 0;
		ok &= comp_cell_poke(comp, CELL_DIVROM, 0, 0x124, 0x55, 1) && comp->div->data[0x124] == 0x55 && comp->div->eeDirty;
		ok &= comp_cell_poke(comp, CELL_DIV, 3, 0, 0x66, 0) && comp->div->data[DIV_EE_SIZE + 3 * 0x2000] == 0x66;
	}
	compDestroy(comp);
	comp = compCreate();
	compSetHardware(comp, "TSConf");
	memSetSize(comp->mem, MEM_4M, MEM_512K);
	{
		comp->mem->ramData[0x3fffff] = 0x5a;
		ok &= comp_cell_size(comp, CELL_RAM) == 0x400000 && comp_cell_peek(comp, CELL_RAM, 0, 0x3fffff) == 0x5a;
		ok &= comp_cell_peek(comp, CELL_RAM, 0xff, 0x3fff) == 0x5a && comp_cell_peek(comp, CELL_RAM, 0, 0x400000) < 0;
	}
	check("dump by area and page", ok);
	compDestroy(comp);
	return failures ? 1 : 0;
}

#include "memory.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>

long long xhost_time_fixed = 0;

Memory* memCreate() {
	Memory* mem = (Memory*)malloc(sizeof(Memory));
	memset(mem, 0x00, sizeof(Memory));
	mem_set_bus(mem, 16);		// first: memSetBank works out the pages by pgshift
	memSetSize(mem, MEM_128K, MEM_16K);
	memSetBank(mem, 0x00, MEM_ROM, 0, MEM_16K, NULL, NULL, NULL);
	memSetBank(mem, 0x40, MEM_RAM, 5, MEM_16K, NULL, NULL, NULL);
	memSetBank(mem, 0x80, MEM_RAM, 2, MEM_16K, NULL, NULL, NULL);
	memSetBank(mem, 0xc0, MEM_RAM, 0, MEM_16K, NULL, NULL, NULL);
	return mem;
}

void memDestroy(Memory* mem) {
	free(mem);
}

// return nearest 2^n greater than argument
int getNearPower(int src) {
	int dst = 1;
	while (dst < src)
		dst <<= 1;
	return dst;
}

// correct value to min/max limits
int toLimits(int src, int min, int max) {
	if (src < min) return min;
	if (src > max) return max;
	return src;
}

void mem_set_bus(Memory* mem, int bw) {
	bw = toLimits(bw, 8, 24);
	mem->busmask = (1 << bw) - 1;		// FFFF for 16, FFFFF for 20, FFFFFF for 24...
	int sz = 1 << (bw - 8);			// memory map contains 256 pages (each: 256 bytes for 16bit, 4096 for 20bit, 65536 bytes for 24 bit)
	mem->pgsize = sz;
	mem->pgshift = 8;
	mem->pgmask = sz - 1;
	while (sz > 256) {
		mem->pgshift++;
		sz >>= 1;
	}
}

void memSetSize(Memory* mem, int ramSz, int romSz) {
//	printf("setMemSize %i %i\n",ramSz,romSz);
	// a size that is not a power of two (a Profi with 768K) is what is fitted,
	// behind the mask of the next one up
	if (ramSz > 0) {
		ramSz = toLimits(ramSz, MEM_256, MEM_4M);
		mem->ramSize = ramSz;
		mem->ramMask = getNearPower(ramSz) - 1;
	}
	if (romSz > 0) {
		romSz = toLimits(romSz, MEM_256, MEM_512K);
		romSz = getNearPower(romSz);
		mem->romSize = romSz;
		mem->romMask = romSz - 1;
	}
}

size_t mem_ram_extent(Memory* mem) {
	return (size_t)mem->ramMask + 1;
}

// What ram holds when the machine is switched on: the pattern repeated over
// all of it, with a byte here and there coming up with something else instead.
//
// Those specks are not scattered evenly. On a photo of a real Pentagon coming
// up cold, the fifteen bad bytes in the 768 of the attribute area sit at only
// seven addresses mod 128, each one hit again 128 or 256 bytes further on - a
// weak cell answers to the low address bits, so the same few of them go wrong
// all over the memory, and not every time they are addressed. A cell is picked
// weak once per power-on: the picture is the same shape every time but never
// the same bytes, which is how real ram behaves. The pattern usually repeats
// over the same address bits, so a weak cell keeps its place in it - always an
// ff cell or always a 00 one, as on the photo.
//
// `noise` is how many bytes in a thousand come up wrong, which is the machine's
// to say - what a board shows depends on its own ram. One weak cell is 2.7 of
// those bytes, and that is the step: below three there is nothing in between.
#define	COLD_WEAK_MASK	0x7f	// address bits a weak cell is known by
#define	COLD_CELLS	(COLD_WEAK_MASK + 1)
#define	COLD_BAD_PM	350	// how often a weak one is actually wrong, per thousand

static unsigned int cold_rnd(unsigned int* seed) {
	unsigned int s = *seed;
	s ^= s << 13; s ^= s >> 17; s ^= s << 5;
	*seed = s;
	return s;
}

// What varies from one switch-on to the next: the host clock, or the clock --bench froze
unsigned int xhost_seed(void) {
	return (unsigned int)(xhost_time_fixed ? xhost_time_fixed : time(NULL));
}

void mem_cold_fill(Memory* mem, const unsigned char* pat, int len, int noise) {
	unsigned int seed = xhost_seed() | 1;
	size_t size = mem_ram_extent(mem);
	unsigned char weak[COLD_CELLS];
	int pick = toLimits((noise * COLD_CELLS + COLD_BAD_PM / 2) / COLD_BAD_PM, 0, COLD_CELLS);
	int left = COLD_CELLS;		// cells still to choose from
	int k = 0;
	int i;
	if (!pat || (len < 1)) return;
	for (i = 0; i < COLD_CELLS; i++) {	// exactly `pick` of them, evenly spread
		weak[i] = ((int)(cold_rnd(&seed) % left) < pick);
		if (weak[i]) pick--;
		left--;
	}
	for (size_t adr = 0; adr < size; adr++) {
		unsigned char val = pat[k];
		if (weak[adr & COLD_WEAK_MASK]) {
			unsigned int rnd = cold_rnd(&seed);
			if ((rnd % 1000) < COLD_BAD_PM)
				val = (unsigned char)(rnd >> 13);
		}
		mem->ramData[adr] = val;
		if (++k == len) k = 0;
	}
}

// Nothing of what ran before is left, past the reach of this machine too: a
// machine with no pattern of its own (none measured yet) comes up zeroed, as at
// the start of the program.
void mem_power_on(Memory* mem, const unsigned char* pat, int len, int noise) {
	memset(mem->ramData, 0x00, sizeof(mem->ramData));
	mem_cold_fill(mem, pat, len, noise);
}

int memRd(Memory* mem, int adr) {
	int res = -1;
	MemPage* ptr = &mem->map[(adr >> mem->pgshift) & 0xff];
	if (ptr->rd)
		res = ptr->rd(adr, ptr->data);
	return res;
}

void memWr(Memory* mem, int adr, int val) {
	MemPage* ptr = &mem->map[(adr >> mem->pgshift) & 0xff];
	if (ptr->wr)
		ptr->wr(adr, val, ptr->data);
}

// TODO: remove memstdrd/wr, let every hw set standard callbacks by itself
int memStdRd(int adr, void* data) {
	unsigned char* ptr = (unsigned char*)data;
	return ptr[adr & 0xff];
}

void memStdWr(int adr, int val, void* data) {
	unsigned char* ptr = (unsigned char*)data;
	ptr[adr & 0xff] = val & 0xff;
}

// What the cpu and the machine read off a page without looking at it again
// The tables index a page by A15..A8: 256-byte pages, as memCreate() makes them.
static void mem_derive_page(Memory* mem, int i) {
	assert(mem->pgshift == 8);
	MemPage* pg = &mem->map[i];
	unsigned char* p = (pg->rd == memStdRd) ? (unsigned char*)pg->data : NULL;
	int ram = (pg->type == MEM_RAM);
	int bank = ram ? ((pg->num << mem->pgshift) >> 14) : -1;
	mem->bank[i] = (short)bank;
	mem->rdp[i] = p;
	mem->wrp[i] = (pg->wr == memStdWr) ? (unsigned char*)pg->data : NULL;
	mem->m1p[i] = (mem_m1_dos_in(pg, i) || mem->m1trap[i]) ? NULL : p;
	mem->m1dos[i] = mem_m1_dos_out(pg) ? NULL : mem->m1p[i];
	int asic = ram && mem_bank_asic(bank);
	mem->rdpa[i] = asic ? NULL : p;
	mem->wrpa[i] = asic ? NULL : mem->wrp[i];
	mem->m1pa[i] = mem->m1trap[i] ? NULL : mem->rdpa[i];
	mem->cont[i] = ram && ((pg->num << mem->pgshift) & mem->contBit);
	mem->slow[i] = ram && mem_bank_slow(bank);
}

void mem_derive(Memory* mem) {
	for (int i = 0; i < 256; i++)
		mem_derive_page(mem, i);
}

void memSetBank(Memory* mem, int page, int type, int bank, int siz, extmrd rd, extmwr wr, void* data) {
	int cnt = 1;
//	while (siz > MEM_256) {		// calculate 256b pages count and correct bank number to 256b
	while (siz > mem->pgsize) {
		cnt <<= 1;
		bank <<= 1;
		siz >>= 1;
	}
	MemPage pg;
	pg.type = type;
	pg.num = bank;
	if (type == MEM_RAM) {
		pg.rd = rd ? rd : (data ? NULL : memStdRd);	// (rd || data) ?
		pg.wr = wr ? wr : (data ? NULL : memStdWr);
	} else if (type == MEM_ROM) {
		pg.rd = rd ? rd : (data ? NULL : memStdRd);
		pg.wr = wr;
	} else {
		pg.rd = rd;
		pg.wr = wr;
		// pg.data = data;
	}
	while(cnt > 0) {
		page &= 0xff;
		if (data) {
			pg.data = data;
		} else if (type == MEM_RAM) {
			pg.data = mem->ramData + ((pg.num << mem->pgshift) & mem->ramMask);
		} else if (type == MEM_ROM) {
			pg.data = mem->romData + ((pg.num << mem->pgshift) & mem->romMask);
		} else {
			pg.data = NULL;
		}
		// a machine pages all of its map in from every port write: what
		// stays where it was keeps what was worked out of it
		if (mem->ovl[page]) {
			mem->under[page] = pg;		// the cpu still sees the overlay
		} else {
			MemPage* cur = &mem->map[page];
			if ((cur->type != pg.type) || (cur->num != pg.num) || (cur->data != pg.data) || (cur->rd != pg.rd) || (cur->wr != pg.wr)) {
				*cur = pg;
				mem_derive_page(mem, page);
			}
		}
		page++;
		pg.num++;
		cnt--;
	}
}

void mem_ovl_set(Memory* mem, int page, int type, int num, unsigned char* data, extmrd rd, extmwr wr) {
	page &= 0xff;
	MemPage pg;
	pg.type = type;
	pg.num = num;
	pg.data = data;
	pg.rd = rd ? rd : memStdRd;
	pg.wr = wr;
	if (!mem->ovl[page]) {
		mem->under[page] = mem->map[page];
		mem->ovl[page] = 1;
	}
	MemPage* cur = &mem->map[page];
	if ((cur->type != pg.type) || (cur->num != pg.num) || (cur->data != pg.data) || (cur->rd != pg.rd) || (cur->wr != pg.wr)) {
		*cur = pg;
		mem_derive_page(mem, page);
	}
}

void mem_ovl_clear(Memory* mem, int page) {
	page &= 0xff;
	if (!mem->ovl[page]) return;
	mem->ovl[page] = 0;
	mem->map[page] = mem->under[page];
	mem_derive_page(mem, page);
}

void mem_set_m1trap(Memory* mem, int page, int on) {
	page &= 0xff;
	mem->m1trap[page] = on ? 1 : 0;
	mem_derive_page(mem, page);
}

void mem_set_cont(Memory* mem, int bit) {
	mem->contBit = bit;
	mem_derive(mem);
}

// set page data
void memPutData(Memory* mem, int type, int page, int sz, char* src) {
	if (type == MEM_RAM) {
		memcpy(mem->ramData + ((page * sz) & mem->ramMask), src, sz);
	} else if (type == MEM_ROM) {
		memcpy(mem->romData + ((page * sz) & mem->romMask), src, sz);
	}
}

// get cell full address
xAdr mem_get_xadr(Memory* mem, int adr) {
	adr &= mem->busmask;
	xAdr xadr;
	MemPage* ptr = mem_get_page(mem, adr);	// = &mem->map[(adr >> mem->pgshift) & 0xff];
	xadr.type = ptr->type;				// page type
	xadr.bank = ptr->num;				// 256(64K) page number
	xadr.adr = adr;					// cpu adr
	xadr.abs = (ptr->num << mem->pgshift) | (adr & mem->pgmask);	// absolute addr
	return xadr;
}

// convert cpu adr to full memory adr
int mem_get_phys_adr(Memory* mem, int adr) {
	MemPage* pg = mem_get_page(mem, adr);	// = &mem->map[(adr >> mem->pgshift) & 0xff];
	return (pg->num << mem->pgshift) | (adr & mem->pgmask);
}

// convert full memory type/adr to cpu adr, if its visible for cpu. return -1 if not visible
int memFindAdr(Memory* mem, int type, int fadr) {
	int i;
	int adr = -1;
	for (i = 0; i < 256; i++) {
		if ((mem->map[i].type == type) && (mem->map[i].num == (fadr >> mem->pgshift))) {
			adr = (i << mem->pgshift) | (fadr & mem->pgmask);
			break;
		}
	}
	return adr;
}

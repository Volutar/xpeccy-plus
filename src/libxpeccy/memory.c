#include "memory.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

long long xhost_time_fixed = 0;

Memory* memCreate() {
	Memory* mem = (Memory*)malloc(sizeof(Memory));
	memset(mem, 0x00, sizeof(Memory));
	memSetSize(mem, MEM_128K, MEM_16K);
	memSetBank(mem, 0x00, MEM_ROM, 0, MEM_16K, NULL, NULL, NULL);
	memSetBank(mem, 0x40, MEM_RAM, 5, MEM_16K, NULL, NULL, NULL);
	memSetBank(mem, 0x80, MEM_RAM, 2, MEM_16K, NULL, NULL, NULL);
	memSetBank(mem, 0xc0, MEM_RAM, 0, MEM_16K, NULL, NULL, NULL);
	mem_set_bus(mem, 16);
	mem->snapath = NULL;
	return mem;
}

void memDestroy(Memory* mem) {
	free(mem);
}

void mem_set_path(Memory* mem, const char* path) {
	mem->snapath = realloc(mem->snapath, strlen(path) + 1);
	strcpy(mem->snapath, path);
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
	if (ramSz > 0) {
		ramSz = toLimits(ramSz, MEM_256, MEM_4M);
		ramSz = getNearPower(ramSz);
		mem->ramSize = ramSz;
		mem->ramMask = ramSz - 1;
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

void mem_cold_fill(Memory* mem, const unsigned char* pat, int len, int noise) {
	unsigned int seed = (unsigned int)(xhost_time_fixed ? xhost_time_fixed : time(NULL)) | 1;
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
		mem->map[page] = pg;
		page++;
		pg.num++;
		cnt--;
	}
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

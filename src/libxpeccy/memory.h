#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "defines.h"
#include <stddef.h>

// mempage type
enum {
	MEM_RAM	= 1,
	MEM_ROM,
	MEM_SLOT,
	MEM_EXT,
	MEM_IO
};

typedef struct {
    int type;
    int bank;
    int adr;
    int abs;
} xAdr;

typedef int(*extmrd)(int, void*);
typedef void(*extmwr)(int, int, void*);

typedef struct {
	int type;			// type of page (ram/rom/slt/io/ext...)
	int num;			// page number
	void* data;			// ptr for rd/wr func
	extmrd rd;			// external rd
	extmwr wr;			// external wr
} MemPage;

typedef struct {
	unsigned char* data;
	unsigned int size;
	unsigned int mask;
} xMemItem;

typedef struct {
	MemPage map[256];			// 256 x 256 | 256 x 64K
	unsigned char ramData[MEM_4M];		// 4M
	unsigned char romData[MEM_512K];	// 512K
	int ramSize;
	int ramMask;
	int romSize;
	int romMask;
	int pgsize;	// size of page in bytes
	int pgmask;	// number of LSBits in address = page offset (FF or FFFF)
	int pgshift;	// = log2(page size), 8 for 256-pages, 16 for 64K-pages
	int busmask;	// cpu addr bus mask (todo: move to CPU)
	char* snapath;
} Memory;

Memory* memCreate(void);
void memDestroy(Memory*);

int memRd(Memory*, int);
void memWr(Memory*, int, int);
int memStdRd(int, void*);		// the page callbacks memSetBank() gives plain ram and rom
void memStdWr(int, int, void*);

// A byte of a page, and the page an address is in. A page of plain ram or rom
// is read and written straight, which is what memStdRd/memStdWr would do.
static inline MemPage* mem_get_page(Memory* mem, int adr) {
	return &mem->map[(adr >> mem->pgshift) & 0xff];
}
static inline int mem_page_rd(MemPage* pg, int adr) {
	if (pg->rd == memStdRd)
		return ((unsigned char*)pg->data)[adr & 0xff];
	return pg->rd ? (pg->rd(adr & 0xffff, pg->data) & 0xff) : 0xff;
}
static inline void mem_page_wr(MemPage* pg, int adr, int val) {
	if (pg->wr == memStdWr)
		((unsigned char*)pg->data)[adr & 0xff] = val & 0xff;
	else if (pg->wr)
		pg->wr(adr, val, pg->data);
}

void memSetSize(Memory*, int, int);
// How much of ramData a machine can reach. Not ramSize: memSetBank puts a page
// at ramData + (bank << pgshift & ramMask), and ZX48 runs with 64K of ram behind
// a 128K mask (see the hand-set ramMask in profiles.cpp), so its screen bank
// alone sits past ramSize.
size_t mem_ram_extent(Memory*);
void mem_cold_fill(Memory*, const unsigned char*, int, int);
void memSetBank(Memory* mem, int page, int type, int bank, int siz, extmrd rd, extmwr wr, void* data);

void memPutData(Memory*,int,int,int,char*);

xAdr mem_get_xadr(Memory*, int);
int memFindAdr(Memory*, int, int);

void mem_set_path(Memory*, const char*);
void mem_set_bus(Memory*, int);
int mem_get_phys_adr(Memory*, int);

#ifdef __cplusplus
}
#endif

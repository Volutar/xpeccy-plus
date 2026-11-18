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
	MEM_IO,
	MEM_DIV		// a divMMC's EEPROM and SRAM: num counts 256-byte pages of div->mem
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
	// map[] as plain bytes, for the cpu to read and write straight (CPU.rdp/m1p/wrp):
	// a page memSetBank() gave memStdRd/memStdWr, NULL where its callback has to run.
	// Worked out from map[] (mem_derive), so no snapshot holds them.
	unsigned char* rdp[256];
	unsigned char* wrp[256];
	unsigned char* m1p[256];		// rdp less the rom at #3Dxx, whose fetch pages TR-DOS in
	unsigned char* m1dos[256];		// ...and less the ram, whose fetch pages it out
	unsigned char* rdpa[256];		// rdp and wrp less the ram banks 4-7, where the +2A/+3
	unsigned char* wrpa[256];		// gate array latches what passes (asicMRd)
	unsigned char* m1pa[256];		// rdpa for the fetches: less the m1trap pages too
	unsigned char slow[256];		// ram pages in an odd 16K bank: where the ULA snows from
	short bank[256];			// the 16K bank of a ram page, -1 for any other
	// ram pages the ULA contends: a page whose address has contBit set (mem_set_cont,
	// the bit vid_cont_bit() gives); the cpu reports a bus cycle only there (CPU.contp)
	unsigned char cont[256];
	int contBit;
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
	// Pages a device lays over the machine's (divMMC at #0000-#3FFF): map[] holds the
	// device's page and under[] what the machine set there. memSetBank() writes under[]
	// while the overlay stands, so a machine pages as it always does from wherever it
	// pages, and mem_ovl_clear() puts its page back.
	unsigned char ovl[256];
	MemPage under[256];
	// pages whose opcode fetches a device has to see (divMMC's automapper): no m1p there
	unsigned char m1trap[256];
} Memory;

// The rules the tables above are made by, each the one copy its users share:
// a slow 16K bank - the ULA's, which it snows from (zx_snow)
static inline int mem_bank_slow(int bank) {return bank & 1;}
// a bank the +2A/+3 gate array latches what passes in (asic_latch)
static inline int mem_bank_asic(int bank) {return bank & 4;}
// a page an opcode fetch pages TR-DOS in from: the rom at #3Dxx (bdi_fetch_pages),
// and one it pages it out from: ram
static inline int mem_m1_dos_in(const MemPage* pg, int page) {return (pg->type == MEM_ROM) && ((page & 0x3f) == 0x3d);}
static inline int mem_m1_dos_out(const MemPage* pg) {return pg->type == MEM_RAM;}

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
// Both calls in one: for the machines that bypass memRd/memWr - these have no
// -1 answer and no bus mask of their own, which is where the two differ.
static inline int mem_rd_fast(Memory* mem, int adr) {
	return mem_page_rd(mem_get_page(mem, adr), adr);
}
static inline void mem_wr_fast(Memory* mem, int adr, int val) {
	mem_page_wr(mem_get_page(mem, adr), adr, val);
}

void memSetSize(Memory*, int, int);
// How much of ramData a machine can reach. Not ramSize: memSetBank puts a page
// at ramData + (bank << pgshift & ramMask), and ZX48 runs with 64K of ram behind
// a 128K mask (see the hand-set ramMask in profiles.cpp), so its screen bank
// alone sits past ramSize.
size_t mem_ram_extent(Memory*);
void mem_cold_fill(Memory*, const unsigned char*, int, int);
// the ram as the power leaves it: the pattern, or zero with none (len 0)
void mem_power_on(Memory*, const unsigned char*, int, int);
void memSetBank(Memory* mem, int page, int type, int bank, int siz, extmrd rd, extmwr wr, void* data);
// a device's page over the machine's: plain bytes at data, read only without wr
void mem_ovl_set(Memory* mem, int page, int type, int num, unsigned char* data, extmrd rd, extmwr wr);
void mem_ovl_clear(Memory* mem, int page);
void mem_set_m1trap(Memory* mem, int page, int on);
void mem_set_cont(Memory*, int bit);	// the address bit of a contended page, 0: none
void mem_derive(Memory*);		// rdp..slow and cont again, after map[] was written behind memSetBank()

void memPutData(Memory*,int,int,int,char*);

xAdr mem_get_xadr(Memory*, int);
int memFindAdr(Memory*, int, int);

void mem_set_bus(Memory*, int);
int mem_get_phys_adr(Memory*, int);

#ifdef __cplusplus
}
#endif

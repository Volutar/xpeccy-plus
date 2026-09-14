#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

// memory flags
// no room for new break types... bit4-7 is reserved for memory cell type
#define	MEM_BRK_FETCH	(1<<0)	// xxxxxxx1
#define	MEM_BRK_RD	(1<<1)	// xxxxxx1x
#define	MEM_BRK_WR	(1<<2)	// xxxxx1xx
#define	MEM_BRK_ANY	(MEM_BRK_FETCH | MEM_BRK_RD | MEM_BRK_WR)
#define MEM_BRK_TFETCH	(1<<3)	// xxxx1xxx
#define MEM_BRK_RAM	(0<<6)	// 00xxxxxx
#define MEM_BRK_ROM	(1<<6)	// 01xxxxxx
#define MEM_BRK_SLT	(2<<6)	// 10xxxxxx
#define MEM_BRK_TMASK	(3<<6)	// 11xxxxxx

// The cartridge is a plain rom image: Interface II on the 48K, a 16K page at a
// time on the ALF. The machine reads it straight out of data[] through the
// memory map, so there is no mapper here.

typedef struct {
	char* path;
	int memMask;			// data is 2^n bytes, this is 2^n - 1
	unsigned char* data;		// the image (malloc)
	unsigned char* brkMap;		// one breakpoint byte per image byte
} xCartridge;

xCartridge* sltCreate();
void sltDestroy(xCartridge*);
void sltEject(xCartridge*);
void sltSetPath(xCartridge*, const char*);

#ifdef __cplusplus
}
#endif

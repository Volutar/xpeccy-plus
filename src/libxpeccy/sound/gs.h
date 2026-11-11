#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "sndcommon.h"

#include "../cpu/cpu.h"
#include "../memory.h"

// gs stereo
enum {
	GS_MONO = 0,
	GS_12_34
};

// the card: X-Trade's General Sound, or NedoPC's NeoGS
enum {
	GS_CLASSIC = 0,
	GS_NEOGS
};

#define GS_ROM_SIZE	MEM_32K		// the classic card's ROM
#define GS_RAM_SIZE	MEM_2M		// what ours has had
#define NGS_ROM_SIZE	MEM_512K	// the NeoGS flash
#define NGS_RAM_SIZE	MEM_4M		// today's NeoGS: four 1M chips

// Time is counted in 1/120 of a nanosecond: a tick of every clock the cards
// run at (24, 20, 12, 10 MHz) and their INT period are whole numbers of it.
#define GS_UNITS_NS	120
#define GS_INT_UNITS	(1000000000LL * GS_UNITS_NS / 37500)	// INT at 37.5 kHz

// NeoGS GSCFG0 (#0F)
#define NGS_NOROM	0x01
#define NGS_RAMRO	0x02
#define NGS_8CHANS	0x04
#define NGS_EXPAG	0x08
#define NGS_CKSEL	0x30
#define NGS_PAN4CH	0x40
#define NGS_INV7B	0x80
#define NGS_CFG0_RESET	0x30

typedef struct {
	unsigned enable:1;
	unsigned reset:1;

	CPU* cpu;
	Memory* mem;
	unsigned char* romGS;	// both images; the model's own is copied into mem
	unsigned char* romNGS;
	unsigned char pb3_gs;	// gs -> zx
	unsigned char pb3_zx;	// zx -> gs
	unsigned char pbb_zx;
	unsigned char rp0;
	unsigned char pstate;	// state register (d0, d7)
	int vol1,vol2,vol3,vol4;
	int ch1,ch2,ch3,ch4;
	int vol5,vol6,vol7,vol8;	// NeoGS
	int ch5,ch6,ch7,ch8;
	int stereo;
	int model;		// GS_CLASSIC, GS_NEOGS
	// NeoGS
	unsigned char cfg0;	// GSCFG0
	unsigned char pg2, pg3;	// the pages at #8000 and #C000, as memmap.v takes them
	unsigned char vol4b5;	// bit 5 of the last VOL4 write (DAMNPORT2)
	unsigned char sctrl;
	unsigned char led;	// 1: lit
	// time
	long long due;		// units owed to the CPU
	long long intDue;	// units to the next INT
	int tickUnits;		// units in a CPU tick
} GSound;

GSound* gsCreate();
void gsDestroy(GSound*);
void gsReset(GSound*);
void gsSync(GSound*, int);
void gsFlush(GSound*);
sndPair gsVolume(GSound*);
int gsWrite(GSound*, int, int);
int gsRead(GSound*, int, int*);
// the card's model: its image into the ROM space, its map, a reset
void gs_set_model(GSound*, int);
// an image was loaded: the model's own goes into the ROM space again
void gs_rom_refresh(GSound*);
// the memory map from the registers, after a snapshot put them back
void gs_remap(GSound*);

#ifdef __cplusplus
}
#endif

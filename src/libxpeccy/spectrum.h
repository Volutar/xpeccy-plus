#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "cpu/cpu.h"
#include "video/video.h"
#include "memory.h"
#include "heatmap.h"

#include "input/input.h"
#include "tape.h"
#include "fdc.h"
#include "hdd.h"
#include "sdcard.h"
#include "cartridge.h"

#include "sound/ayym.h"
#include "sound/gs.h"
#include "sound/saa1099.h"
#include "sound/soundrive.h"

	#include <zlib.h>

extern int compflags;

#define CFLG_PANIC	1

// hw reset rompage
enum {
	RES_DEFAULT = 0,
	RES_48,
	RES_128,
	RES_DOS,
	RES_SHADOW
};

// what bit 6 of #FE reads with no tape playing: the machine feeds its own last
// out #FE back into the ear input, and how much of it depends on the board
enum {
	EAR_NONE = 0,		// +2A/+3: nothing comes back, the bit stays low
	EAR_ISSUE3,		// bit 4 (the speaker) alone
	EAR_ISSUE2		// bit 4 or bit 3 (the tape output)
};

// What a port nothing answers reads back. See zx_in_float() in hardware/common.c.
enum {
	FBUS_NONE = 0,	// nothing: it reads #FF
	FBUS_ULA,	// the Ferranti ULA of the 48K/128K/+2: the byte it is fetching
	FBUS_ASIC,	// the Amstrad gate array of the +2A/+3: the same, its own way
	FBUS_ATTR	// the clones' port #FF: the attribute of the cell being shown
};

enum {
	DBG_VIEW_CODE = 0x00,
	DBG_VIEW_BYTE = 0x10,
	DBG_VIEW_WORD = 0x20,
	DBG_VIEW_ADDR = 0x30,
	DBG_VIEW_TEXT = 0x40,
	DBG_VIEW_EXEC = 0x50
};

// a TSConf DMA address: 16-bit words, wrapping in its 256/512-byte block with align on
typedef struct {
	int base;	// block part, or the whole address without align
	int low;	// offset in the block the bursts start from
	int pos;	// words into the current burst
	int mask;	// block size - 1, 0 without align
} tsDmaAdr;

typedef struct {
	int t;
	int a;
	unsigned char* ptr;
} bpChecker;

#define p7FFD	reg[0]
#define p1FFD	reg[1]
#define pEFF7	reg[2]
#define prt2	reg[3]

#define flgBRK	sysflag[0]		// breakpoint catched
#define flgDBG	sysflag[1]		// debug execution
#define flgMAP	sysflag[2]		// map memory on runtime
#define flgFRM	sysflag[3]		// new frame ready
#define flgNMIRQ sysflag[4]		// NMI requested
#define flgFRN	sysflag[5]		// need to reset when switch to profile
#define flgDDP	sysflag[6]		// used ATM2+ only, but have options entry
#define flgEM1	sysflag[7]		// (same for Scorpion ZS 256)
#define flgCNTM	sysflag[8]		// contended mem (zx)
#define flgCNTI	sysflag[9]		// contended i/o (zx)
#define flgIBRK	sysflag[10]		// break on interrupt
#define flgROM	sysflag[11]
#define flgDOS	sysflag[12]
#define flgCPM	sysflag[13]
#define flgEXT	sysflag[14]
#define flgBDI	sysflag[15]
#define flgHEAT	sysflag[16]		// collect memory read/write/exec usage stats
#define flgCOND	sysflag[17]		// breakpoint conditions in use: latch mem/io events
#define flgSNOW	sysflag[18]		// ULA snow effect (zx)
#define flgSNOWX sysflag[19]		// ...and this machine's ram cannot take it
#define flgBRKPRE sysflag[20]		// the break was raised before the instruction ran
#define flgBRKMEM sysflag[21]		// a memory breakpoint may be set: 0 lets the core skip the maps

#define PWATCH_MAX	16		// ports the debugger can watch at once

// scale of comp->tickPerNsFixed. Separate from NS_FIXED_BITS: it is a
// reciprocal, and needs the extra bits to stay accurate for short tick periods
#define TICK_FIXED_BITS 32

// how many turbo steps a board may declare
#define TURBO_STEP_MAX 8

typedef struct Computer {
	struct HardWare *hw;	// computer core - misc params, callbacks

	double cpuFrq;
	double frqMul;		// overclock the user asked for, not a real machine's
	double hwMul;		// turbo the machine is running at now
	// The board's own turbo step. A machine that switches turbo from a port
	// overwrites hwMul from there; on one whose turbo is a switch on the case
	// this is all there is, so a reset puts hwMul back to it rather than to 1.
	double turboStep;
	// the steps this board has, x1 first, from its definition
	double turboTab[TURBO_STEP_MAX];
	int turboCount;
	unsigned char intVector;

	int brkt;		// breakpoint type (cpu, ram, rom...)
	int brka;		// breakpoint addr
	// last mem/io event, for breakpoint conditions (-1 = didn't happen)
	struct {
		int rd, wr, mdt;
		int in, out, val;
		int kind;	// MEM_BRK_* of the access that fired it
		int adr;	// cpu address it was made at (-1 = no access)
	} brkev;
	int brkray;		// beam position (dots from frame start) before this instruction
	int brkpc;		// pc before this instruction: where a breakpoint says it is
	// ports the debugger watches. A hit is (bus & mask) == port; a port the
	// machine keeps a copy of is read from there instead of the bus, the rest
	// hold the last value that went through them (-1 = nothing yet). A port
	// switched off keeps its place in the list, it is just not watched
	struct {
		int port;
		int mask;
		int type;		// what the machine keeps it in...
		size_t offset;		// ...and where, 0 = watch the bus
		int on;
		int hw;			// came from the machine's own table
		int val;
	} pwatch[PWATCH_MAX];
	int pwcount;
	int pwbus;			// how many of them the bus has to be watched for

	char* msg;		// message ptr for displaying outside
	int resbank;		// rompart active after reset
	int snapModel;		// SNAP_HW_* a snapshot names this machine as, 0: by its core
	int romCustom;		// bytes of rom a snapshot put in, until the machine's own are loaded again
	int earback;		// EAR_*: the ear input with no tape playing
	int earRC;		// the ear input holds its charge after bit 4 drops (zx_ear)
	double earV;		// that charge, 0..1, as of earTick
	int earTick;		// tickCount + cpu->t when earV was
	int earDead;		// ...and when bit 6 reads 0 again after bit 4 went down
	int fbus;		// FBUS_*: what a port nothing answers reads back
	unsigned char fbusLast;	// last byte to or from contended memory (FBUS_ASIC)
	int fbusTick;		// tickCount + cpu->t of fbusLast

	int snowBad;		// the ULA took a refresh cycle: the next opcode out of
				// slow memory comes back wrong (flgSNOWX)

	int frmCount;		// frames since reset (for breakpoint conditions)
	int tickCount;		// accumulate T
	int frmtCount;		// accumulate T, but reset each INT
	int hCount;		// T before HALT = frmtCount @ HALT
	int waitDebt;		// T a board with its own arbiter still owes the next bus cycle
	int waitPaid;		// what comp_cont_hw() last paid of it, for comp_owe()
	int fCount;		// T in last frame
	double nsPerTick;	// real ns/T, kept precise: truncating this to int was the
				// root cause of a systemic ~0.3-0.9% emulation speed error
				// (see nsPerSampleFixed-driven pacing in ethread.cpp)
	// nsPerTick in 16.16, and its inverse in 32.32. The per-instruction sync
	// paths run on these: they sit on every memory access, and llround() is an
	// out-of-line CRT call with gcc at -O2. Both derived by
	// comp_update_timings(), never set by hand.
	long long nsPerTickFixed;
	long long tickPerNsFixed;

	unsigned char verblk[16];	// what the machine says it is (comp_set_identity)
	bool flag[128];			// each machine have its own flags
	bool sysflag[32];		// some common flags used by several machines or debuga
	unsigned char reg[512];		// internal registers
	xreg32 xreg[32];		// combined 32/16/8 bits registers
// base
	CPU* cpu;
// TODO: align memory/brkMaps, respect ram/rom size
	Memory* mem;
	Video* vid;
// input
	Keyboard* keyb;
	Joystick* joy;
	Joystick* joyb;
	Mouse* mouse;
// data storage
	Tape* tape;
	DiskIF* dif;
	IDE* ide;
	SDCard* sdc;
	xCartridge* slot;		// cartridge slot (Interface II, ALF)
// sound
	bitChan* beep;
	TSound* ts;
	GSound* gs;
	SDrive* sdrv;
	saaChip* saa;
// misc
	CMOS cmos;


	struct {
		unsigned start:1;
		unsigned stop:1;
		unsigned play:1;
		unsigned overio:1;
		int fTotal;
		int fCurrent;
		int fCount;
		FILE* file;		// converted tmp-file
		struct {
			int fetches;
			int size;
			unsigned char data[0x10000];
			int pos;
		} frm;
	} rzx;


	// brkRamMap..brkIOMap is skipped by the state snapshot: 4.6 MB of debugger
	// bookkeeping that no rollback needs, so xstate.c takes this struct as the
	// ranges around it. A field that DOES belong in a snapshot has to go before
	// brkRamMap or after brkIOMap - one put between them would be left out with
	// nothing to say so. (The rzx frame buffer above is skipped the same way.)
	unsigned char brkRamMap[MEM_4M];	// ram brk/type : b0..3:brk flags, b4..7:type
	unsigned char brkRomMap[MEM_512K];	// rom brk/type : b0..3:brk flags, b4..7:type
	unsigned char brkAdrMap[MEM_64K];	// adr brk
	unsigned char brkIOMap[MEM_64K];	// io brk
	xHeatBank heatRam;			// ram read/write/exec hit counters, sized to mem->ramMask+1
	xHeatBank heatRom;			// rom read/write/exec hit counters, sized to mem->romMask+1
	// TODO: try to move this somewhere
	struct {
		unsigned char Page0;
		unsigned char p21af;
		unsigned char pwr_up;		// 1 on 1st run, 0 after reading 00AF
		unsigned char cacheConf;	// CacheConfig: b0..3 cache windows 0..3
		unsigned short cache[256];	// tags of the 256-word cache: b15 valid, {page, A13..A9}
		int cpuDram;			// DRAM cycles the cpu took since the last sync
		struct {			// a DMA transfer in progress
			unsigned char act, dev, asz, sat;
			int len, bursts, word, data;
			tsDmaAdr s, d;
			long long free;		// DRAM cycles banked, in 1/448ths
			int time;		// dots banked, for the device side of SPI and IDE
			long long frac;		// ns (16.16) not yet a whole dot
		} dma;
	} tsconf;
} Computer;

// The two conversions the per-instruction sync paths need. Not inverses of each
// other: one produces fixed point ns, the other consumes it.

static inline long long ticks_to_ns_fixed(Computer* comp, int t) {
	return (long long)t * comp->nsPerTickFixed;
}

// wait states end on a tick boundary, so round up. Exact: the dot and the
// tick come off the same crystal, so the quotient is whole on every zx.
static inline int ns_fixed_to_ticks_up(Computer* comp, long long ns_fixed) {
	return (int)((ns_fixed + comp->nsPerTickFixed - 1) / comp->nsPerTickFixed);
}

// A board that drops its turbo for a while owes the next bus cycle what a step
// took once more: the step's own ticks, not the debt it paid on the way, or the
// debt grows with every step. Called after each step, owing or not.
static inline void comp_owe(Computer* comp, int ns, int owe) {
	if (owe) {
		int t = (int)((NS_TO_FIXED(ns) + comp->nsPerTickFixed / 2) / comp->nsPerTickFixed);
		if (t > comp->waitPaid) comp->waitDebt += t - comp->waitPaid;
	}
	comp->waitPaid = 0;
}

// Something of the debugger's looks at every memory access: the map, the heat
// map, a condition or a memory breakpoint
static inline int comp_mem_watched(Computer* comp) {
	return comp->flgMAP | comp->flgHEAT | comp->flgCOND | comp->flgBRKMEM;
}

#include "hardware/hardware.h"

Computer* compCreate();
void comp_set_identity(Computer*, const char*, int, int);
void compDestroy(Computer*);
void compReset(Computer*,int);
void compUserReset(Computer*,int);
int compExec(Computer*);
int comp_skip_ticks(Computer*, int);
void comp_irq(int, void*);

//void compKeyPress(Computer*, int, keyEntry*);
//void compKeyRelease(Computer*, int, keyEntry*);
void comp_kbd_release(Computer*);

void compSetBaseFrq(Computer*,double);
void compSetTurbo(Computer*,double);
void compSetHwTurbo(Computer*,double);
void comp_update_timings(Computer*);
int compSetHardware(Computer*,const char*);
void comp_set_layout(Computer*, vLayout*);
void comp_set_snow(Computer*, int);
void comp_set_cont(Computer*, int);
void comp_snap_map(Computer*);
void comp_snap_reset(Computer*, int);
int comp_frame_ticks(Computer*);
double comp_fps(Computer*);		// frames a second of emulated time
void comp_set_frame_tick(Computer*, int);
void comp_power_phase(Computer*);	// the frame position a machine is switched on at
int comp_get_frame_tick(Computer*);

void rzxStop(Computer*);

void comp_brk(Computer*, int);
void comp_brk_newstep(Computer*);

void comp_pwatch_clear(Computer*);
void comp_pwatch_add(Computer*, int, int, int);
void comp_pwatch_sync(Computer*);
int comp_pwatch_val(Computer*, int);

unsigned char* getBrkPtr(Computer*, int);
unsigned char getBrk(Computer*, int);
void setBrk(Computer*, int, unsigned char);

#ifdef __cplusplus
}
#endif

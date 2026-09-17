#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "../spectrum.h"
#include <assert.h>

// hw type
enum {
	HW_NULL = 0,
	HW_DUMMY,	// nothing-to-do-here
	HW_ZX48,	// ZX Spectrum 48K
	HW_ZX128,	// ZX Spectrum 128K (and the grey +2, which is one)
	HW_PENT,	// Pentagon
	HW_P1024,	// Pentagon1024SL
	HW_SCORP,	// ZS Scorpion
	HW_PLUS2A,	// ZX Spectrum +2A
	HW_PLUS3,	// ZX Spectrum +3
	HW_ATM1,	// ATM 1
	HW_ATM2,	// ATM 2+
	HW_PENTEVO,	// ZX Evolution (BaseConf)
	HW_TSLAB,	// ZX Evolution (TSConf)
	HW_PROFI,	// Profi
	HW_PHOENIX,	// ZXM Phoenix
	HW_ALF		// ALF TV Game (a ZX48 clone console)
};

// Hardware callbacks

// std callback
typedef void(*cbhwcomp)(Computer*);
// sync: calls after every CPU command
typedef void(*cbHwSnc)(Computer*, int);
// memory read
typedef int(*cbHwMrd)(Computer*, int, int);
// memory write
typedef void(*cbHwMwr)(Computer*, int, int);
// io read
typedef int(*cbHwIrd)(Computer*, int);
// io write
typedef void(*cbHwIwr)(Computer*, int, int);
// int request
typedef void(*cbHwIrq)(Computer*, int);
// int ack
typedef int(*cbHwAck)(Computer*);
// key press/release
typedef void(*cbHwKey)(Computer*, keyEntry*);
// get volume
typedef sndPair(*cbHwVol)(Computer*, sndVolume*);

typedef struct {
	int port;
	int type;
	size_t offset;
} xPortDsc;

struct HardWare {
	int id;			// id
	const char* name;	// name used in conf file
	const char* optName;	// name used in options window
	int mask;		// mem size bits (see memory.h)
	double xscale;		// pixel ratio (x:y)
	vLayout* lay;		// fixed layout ptr. if NULL, use from config
	xPortDsc* portab;	// tab of ports descriptors
	cbhwcomp init;		// init (call on setting comp hardware)
	cbhwcomp mapMem;	// map memory
	cbHwIwr out;		// io wr
	cbHwIrd in;		// io rd
	cbHwMrd mrd;		// mem rd
	cbHwMwr mwr;		// mem wr
	cbHwIrq irq;		// int rq
	cbHwAck ack;		// int ack
	cbhwcomp reset;		// reset
	cbHwSnc sync;		// sync time
	cbHwKey keyp;		// key press
	cbHwKey keyr;		// key release
	cbHwVol vol;		// read volume
	cbhwcomp snapmap;	// stand where a snapshot expects the machine (NULL: nothing to do)
};
typedef struct HardWare HardWare;

typedef struct {
	int mask;		// if (port & mask == value & mask) port is catched
	int value;
	unsigned dos:2;		// 00:!dos only; 01:dos only; 1x:nevermind
	unsigned rom:2;		// same: b4,7FFD
	unsigned cpm:2;		// cpm mode (for profi)
	int (*in)(Computer*, int);
	void (*out)(Computer*, int, int);
} xPort;

typedef struct {
	int port;
	int value;
} xPortValue;

typedef struct {
	int id;
	HardWare* core;
} tabHwItem;

int hwIn(xPort* ptab, Computer* comp, int port);
void hwOut(xPort* ptab, Computer* comp, int port, int val, int mult);
xPortValue* hwGetPorts(Computer*);

// extern HardWare hwTab[];

HardWare* findHardware(const char*);
int stdMRd(Computer*, int, int);
void stdMWr(Computer*, int, int);
int asicMRd(Computer*, int, int);	// stdMRd/stdMWr plus the +2A/+3 bus latch
void asicMWr(Computer*, int, int);

// debug IO

int brkIn(Computer*, int);
void brkOut(Computer*, int, int);

int dummyIn(Computer*, int);
void dummyOut(Computer*, int, int);

// common IO

int zx_dev_wr(Computer*, int, int);
int zx_dev_rd(Computer*, int, int*);
void zx_irq(Computer*, int);
void zx_snow(Computer*);
int zx_bank_of(Computer*, int);
int zx_ack(Computer*);
int zx_ear(Computer*);

void xOutFE(Computer*, int, int);
void xOutBFFD(Computer*, int, int);
void xOutFFFD(Computer*, int, int);

int xIn1F(Computer*, int);
int zx_in_float(Computer*, int);	// a port nothing answers: the floating bus
int xInFE(Computer*, int);
int xInFFFD(Computer*, int);
int xInFADF(Computer*, int);
int xInFBDF(Computer*, int);
int xInFFDF(Computer*, int);

// common_zx calls
void zx_init(Computer*);
void zx_reset(Computer*);
void zx128_map_mem(Computer*, int);	// 128K paging, shared with the Pentagon; arg is the bank extension mask
void zx_set_vmode(Computer*);	// pick the screen drawer for this ula
void zx_keyp(Computer*, keyEntry*);
void zx_keyr(Computer*, keyEntry*);
void zx_sync(Computer*, int);
sndPair zx_vol(Computer*, sndVolume*);
void zx_set_pal(Computer*);	// todo: called from zx_reset only

// tsconf SYSCONF: the cpu speed (the .spg loader sets it too)
void tsOut20AF(Computer*, int, int);

#ifdef __cplusplus
}
#endif

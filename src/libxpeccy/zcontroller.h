#pragma once

// Z-Controller (Alexey Zhabin, KOE, 2007): a ZX-BUS card with a PS/2 keyboard and
// mouse, a Nemo IDE port and an SD card slot. Only the SD part is here, after the
// card's own CPLD source (zcont.tdf, "SPI Interface for ZX-Spectrum ver. 1.0 by
// KOE") and its fitter report, from github.com/koe1234/Z-controller.
//
// Two ports, decoded by A7..A5, A1 and A0 alone: #77 the control (and any port
// 011xxx11), #57 the data (010xxx11). They are on only while the bus's DOS/ is
// high - with TR-DOS paged in they would be Beta Disk's #7F and #5F - and the card
// holds IORQGE on them, which shuts the machine's own port decoders out.

#ifdef __cplusplus
extern "C" {
#endif

#include "sdcard.h"

// a byte on the SPI port: 8 clocks of the card's 8 MHz oscillator, after the first
// edge that follows the I/O cycle (0..125 ns, taken as the whole of it)
#define ZC_SPI_NS	1125

// #77 written: b0 the card's supply (1 on), b1 its /CS (1 not selected)
#define ZC_POWER	1
#define ZC_CS		2

typedef struct {
	int on;			// fitted
	int cfg;		// #77 as written (b1..b0), cleared by the bus RESET
	int tx;			// the transmit register: the last byte written to #57
	int rx;			// what the last exchange got from the card
	long long spiEnd;	// the exchange going on ends (ns)
	SDCard* card;		// the slot's card (the machine's comp->sdc)
	void* host;
	long long (*ns)(void*);	// ns since the machine started
} ZCtrl;

ZCtrl* zc_create(SDCard* card, void* host, long long (*ns)(void*));
void zc_destroy(ZCtrl*);
void zc_reset(ZCtrl*);		// the bus RESET (power-on too): #77 to 0, the card's supply off

// ports, while the TR-DOS ROM is not paged (dos = 0); 1 when the port is the card's
// (*val: what an IN reads)
int zc_out(ZCtrl*, int port, int val, int dos);
int zc_in(ZCtrl*, int port, int* val, int dos);

#ifdef __cplusplus
}
#endif

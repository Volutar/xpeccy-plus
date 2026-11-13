#pragma once

// Z-Controller's SD port: #77 control, #57 data; its IORQGE shuts the machine's own decoders out

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
	int lat;		// what an IN reads: taken from rx by the IN that started one
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

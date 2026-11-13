// Z-Controller's SD port, after KOE's CPLD source (zcont.tdf) and its fitter
// report (zcont.rpt); the names in the comments are the source's.

#include "zcontroller.h"

#include <stdlib.h>
#include <string.h>

ZCtrl* zc_create(SDCard* card, void* host, long long (*ns)(void*)) {
	ZCtrl* zc = (ZCtrl*)calloc(1, sizeof(ZCtrl));
	if (!zc) return NULL;
	zc->card = card;
	zc->host = host;
	zc->ns = ns;
	zc_reset(zc);
	return zc;
}

void zc_destroy(ZCtrl* zc) {
	free(zc);
}

// sdcontrwr is cleared by the bus RESET: the card's supply off, /CS low
void zc_reset(ZCtrl* zc) {
	zc->cfg = 0;
	zc->tx = 0;
	zc->rx = 0xff;
	zc->lat = 0xff;
	zc->spiEnd = 0;
	if (zc->on) zc->card->cs = 0;
}

// contrsd / datasd: A7=0, A6=1, A1=1, A0=1 and DOS/ high; A5 picks the port
static int zc_port(int port, int dos) {
	if (dos || ((port & 0xc3) != 0x43)) return -1;
	return (port & 0x20) ? 0x77 : 0x57;
}

static int zc_card_xfer(ZCtrl* zc, int mosi) {
	if (!(zc->cfg & ZC_POWER)) return 0xff;		// no supply: nothing drives MISO
	return sdc_xfer(zc->card, mosi);
}

// An exchange starts as the I/O cycle ends and runs on the card's own clock; no
// WAIT holds the CPU. An OUT loads sddatawr and sends it. An IN puts sddatard (what
// the exchange before it got) on the bus and starts one with sddatawr as it was
// left: bit 0 is never shifted, so 8 copies of the last written byte's bit 0 go
// out (#FF after the odd byte every driver writes before it reads). What an access
// that comes before the exchange has ended does is not designed for: here, as on
// the divMMC, an OUT then is lost and an IN reads the old byte and starts nothing.
static int zc_spi(ZCtrl* zc, int out, int val) {
	long long now = zc->ns(zc->host);
	if (now < zc->spiEnd) return zc->lat;
	if (out) zc->tx = val & 0xff;
	else zc->lat = zc->rx;
	zc->rx = zc_card_xfer(zc, out ? zc->tx : ((zc->tx & 1) ? 0xff : 0x00)) & 0xff;
	zc->spiEnd = now + ZC_SPI_NS;
	return zc->lat;
}

int zc_out(ZCtrl* zc, int port, int val, int dos) {
	if (!zc || !zc->on) return 0;
	switch (zc_port(port, dos)) {
		case 0x77: {
			int was = zc->cfg;
			zc->cfg = val & (ZC_POWER | ZC_CS);
			// sdpower: the card's regulator; a card that comes up starts over
			if ((zc->cfg & ZC_POWER) && !(was & ZC_POWER)) sdc_power_on(zc->card);
			zc->card->cs = (zc->cfg & ZC_CS) ? 1 : 0;
			return 1;
		}
		case 0x57:
			zc_spi(zc, 1, val);
			return 1;
	}
	return 0;
}

int zc_in(ZCtrl* zc, int port, int* val, int dos) {
	if (!zc || !zc->on) return 0;
	switch (zc_port(port, dos)) {
		case 0x77: {
			// b0 sdabsent and b1 readonly, the socket's switches; b7..b2 read 0
			SDCard* sdc = zc->card;
			int res = 0;
			if (!sdc->image || sdc_unread(sdc)) res |= 1;
			if (sdc_protected(sdc)) res |= 2;
			*val = res;
			return 1;
		}
		case 0x57:
			*val = zc_spi(zc, 0, 0);
			return 1;
	}
	return 0;
}

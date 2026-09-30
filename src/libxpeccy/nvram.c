#include <stdlib.h>
#include <string.h>
#include "nvram.h"

// The 24C16 bit by bit, the way UnrealSpeccy has it - ProfROM keeps its
// settings here and is known to work with that model. SMUC's WC line is not
// modelled: the chip always takes the write.

nvRam* nvCreate() {
	nvRam* nv = (nvRam*)malloc(sizeof(nvRam));
	if (nv == NULL) return NULL;
	memset(nv, 0x00, sizeof(nvRam));	// NV_IDLE
	nv->sda = 1;
	nv->scl = 1;
	nv->out = 1;
	nv->outz = 1;
	return nv;
}

void nvDestroy(nvRam* nv) {
	free(nv);
}

// the byte at the current address goes out next
static void nv_load(nvRam* nv) {
	nv->dataout = nv->mem[nv->adr];
	nv->adr = (nv->adr + 1) & (NV_SIZE - 1);
	nv->bitsout = 0;
	nv->state = NV_SEND_DATA;
}

void nvWr(nvRam* nv, int sda, int scl) {
	sda = !!sda;
	scl = !!scl;
	if (scl != nv->scl) {
		if (scl) {				// rising SCL: the chip reads SDA
			if (nv->state == NV_RD_ACK) {
				if (sda) {		// no ACK from the host: the read is over
					nv->state = NV_IDLE;
					nv->outz = 1;
				} else {		// next byte out
					nv_load(nv);
				}
			} else if ((nv->state == NV_RCV_CMD) || (nv->state == NV_RCV_ADR) || (nv->state == NV_RCV_DATA)) {
				if (nv->outz) {		// not the chip's own ACK slot
					nv->datain = (nv->datain << 1) | sda;
					nv->bitsin++;
				}
			}
		} else {				// falling SCL: the chip sets SDA
			if (nv->bitsin == 8) {		// a byte in: take it and ACK
				nv->bitsin = 0;
				switch (nv->state) {
					case NV_RCV_CMD:
						if ((nv->datain & 0xf0) != 0xa0) {
							nv->state = NV_IDLE;
							nv->outz = 1;
							break;
						}
						nv->adr = (nv->adr & 0xff) | ((nv->datain << 7) & 0x700);	// block
						if (nv->datain & 1) {		// read from the current address
							nv_load(nv);
						} else {
							nv->state = NV_RCV_ADR;
						}
						break;
					case NV_RCV_ADR:
						nv->adr = (nv->adr & 0x700) | nv->datain;
						nv->state = NV_RCV_DATA;
						break;
					case NV_RCV_DATA:
						nv->mem[nv->adr] = nv->datain;
						nv->adr = (nv->adr & 0x7f0) | ((nv->adr + 1) & 0x0f);	// 16-byte page
						break;
				}
				if (nv->state != NV_IDLE) {
					nv->out = 0;
					nv->outz = 0;
				}
			} else if (nv->state == NV_SEND_DATA) {
				if (nv->bitsout == 8) {
					nv->state = NV_RD_ACK;
					nv->outz = 1;
				} else {
					nv->out = (nv->dataout & 0x80) ? 1 : 0;
					nv->dataout <<= 1;
					nv->bitsout++;
					nv->outz = 0;
				}
			} else {
				nv->outz = 1;
			}
		}
	} else if (scl && (sda != nv->sda)) {		// SDA moved with SCL high
		nv->state = sda ? NV_IDLE : NV_RCV_CMD;	// stop : start
		nv->bitsin = 0;
		nv->outz = 1;
	}
	if (nv->outz) nv->out = sda;
	nv->sda = sda;
	nv->scl = scl;
}

int nvRd(nvRam* nv) {
	return nv->out;
}

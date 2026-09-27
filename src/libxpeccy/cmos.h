#pragma once

// #include "input/input.h"

enum {
	CMOS_ADR = 0,
	CMOS_DATA
};

typedef struct {
	unsigned char adr;
	int mode;			// ZX Evo: what F0..FF read as
	unsigned char data[256];	// a DS12887 has 128, the ZX Evo's clock 256
} CMOS;

unsigned char cmos_rd(CMOS*, int);
void cmos_wr(CMOS*, int, int);

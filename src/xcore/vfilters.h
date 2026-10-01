#pragma once

#include <stdint.h>

enum {
	AF_2C_FULL = 0,
	AF_2C_ADAPTIVE,
    AF_3C_FULL,
	AF_3C_ADAPTIVE
};

#define AF_AHEAD	2	// frames the look ahead runs

// ahead: the AF_AHEAD frames after this one, wid x hei each, back to back, or NULL
void scrMix(unsigned char *src, unsigned char *p0, int wid, int hei, int stride, double mass, float gamma, int mode, int reset,
	const uint32_t* ahead);

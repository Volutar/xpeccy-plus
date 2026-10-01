#pragma once

enum {
	AF_2C_FULL = 0,
	AF_2C_ADAPTIVE,
    AF_3C_FULL,
	AF_3C_ADAPTIVE
};

void scrMix(unsigned char *src, unsigned char *p0, int wid, int hei, int stride, double mass, float gamma, int mode, int reset);

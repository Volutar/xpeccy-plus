#include "sndcommon.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// mixer
#define XMAXVOL 16384

// A soft clip for two levels in 0..XMAXVOL. A signed one - the FM half of a
// YM2203 - drops the divisor instead of raising it, to zero when the two are
// full scale and opposite, so it has a floor. Summing the FM outside this,
// the way zx_vol() sums everything else, would suit it better.
sndPair mixer(sndPair vol1, sndPair vol2) {
	int div = XMAXVOL + (vol1.left * vol2.left) / XMAXVOL;
	if (div < XMAXVOL / 16) div = XMAXVOL / 16;
	vol1.left = (vol1.left + vol2.left) * XMAXVOL / div;
	div = XMAXVOL + (vol1.right * vol2.right) / XMAXVOL;
	if (div < XMAXVOL / 16) div = XMAXVOL / 16;
	vol1.right = (vol1.right + vol2.right) * XMAXVOL / div;
	return vol1;
}

// 1-bit channel with transient response

#define OVERDIV 88			// ns/256 : transient const (ns to rise/lower sound level 1 step)
#define OVERLIM (OVERDIV * 256)		// ns to full sound level restore

bitChan* bcCreate() {
	bitChan* ch = malloc(sizeof(bitChan));
	memset(ch, 0x00, sizeof(bitChan));
	return ch;
}

void bcDestroy(bitChan* ch) {
	free(ch);
}

void bcTransient(bitChan* ch, int ns) {
	if (ch->lev) {
		if (ns > OVERLIM) {
			ch->val = 0xff;
		} else {
			ch->val += ns / OVERDIV;
			if (ch->val > 0xff)
				ch->val = 0xff;
		}
	} else {
		if (ns > OVERLIM) {
			ch->val = 0;
		} else {
			ch->val -= ns / OVERDIV;
			if (ch->val < 0)
				ch->val = 0;
		}
	}
}

void bcSync(bitChan* ch, int ns) {
	int per;
//	if (ns < 1) {
//		ns = ch->accum;
//		ch->accum = 0;
//	}
	if (ns < 1) return;

	bcTransient(ch, ns);		// transient process of current wave
	if (ch->perH && ch->perL) {	// emulate waves
		// printf("%i %i %i\n",ch->pcount, ch->perH, ch->perL);
		ch->pcount -= ns;
		while (ch->pcount < 1) {
			ch->lev ^= 1;
			ch->step++;
			per = ch->lev ? ch->perH : ch->perL;
			ch->pcount += per;
			if (ch->pcount > 0) per -= ch->pcount;
			bcTransient(ch, per);
		}
	}
}

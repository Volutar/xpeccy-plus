#include "sndcommon.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// A soft clip for two levels in 0..XMAXVOL. Only for those: a signed level
// would drop the divisor instead of raising it, and go loud rather than clip.
sndPair mixer(sndPair vol1, sndPair vol2) {
	int div = XMAXVOL + (vol1.left * vol2.left) / XMAXVOL;
	vol1.left = (vol1.left + vol2.left) * XMAXVOL / div;
	div = XMAXVOL + (vol1.right * vol2.right) / XMAXVOL;
	vol1.right = (vol1.right + vol2.right) * XMAXVOL / div;
	return vol1;
}

// 1-bit channel with transient response

#define OVERDIV 88			// ns/256 : transient const (ns to rise/lower sound level 1 step)
#define OVERLIM (OVERDIV * 256)		// ns to full sound level restore

void bcReset(bitChan* ch) {
	memset(ch, 0x00, sizeof(bitChan));
}

bitChan* bcCreate() {
	bitChan* ch = malloc(sizeof(bitChan));
	bcReset(ch);
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

void bc_sync_slow(bitChan* ch, int ns) {
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

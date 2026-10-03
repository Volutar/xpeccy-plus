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
	int mic = ch->mic;
	memset(ch, 0x00, sizeof(bitChan));
	ch->mic = mic;
	ch->hi = 0xff;
}

bitChan* bcCreate() {
	bitChan* ch = calloc(1, sizeof(bitChan));
	bcReset(ch);
	return ch;
}

void bcDestroy(bitChan* ch) {
	free(ch);
}

void bcTransient(bitChan* ch, int ns) {
	int top = ch->lev ? ch->hi : ch->lo;
	if (ns > OVERLIM) {
		ch->val = top;
	} else if (ch->val < top) {
		ch->val += ns / OVERDIV;
		if (ch->val > top)
			ch->val = top;
	} else {
		ch->val -= ns / OVERDIV;
		if (ch->val < top)
			ch->val = top;
	}
}

// EAR (bit 4) and MIC (bit 3) leave a ULA on one pin, at 0.34, 0.73, 3.66 and
// 3.79 V for none, MIC, EAR and both (issue 3), so MIC alone is a tenth of EAR.
// Scaled so EAR alone stays where the beeper always was.
void bc_out(bitChan* ch, int ear, int mic) {
	ch->lev = ear;
	ch->lo = (ch->mic && mic) ? 30 : 0;
	ch->hi = (ch->mic && mic) ? 265 : 0xff;
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

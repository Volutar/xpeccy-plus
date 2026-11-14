#include "sndcommon.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// A soft clip for two levels in 0..XMAXVOL. Only for those: a signed level
// would drop the divisor instead of raising it, and go loud rather than clip.
sndPair mixer(sndPair vol1, sndPair vol2) {
	int div = XMAXVOL + (vol1.left * vol2.left) / XMAXVOL;
	vol1.left = (vol1.left + vol2.left) * XMAXVOL / div;
	div = XMAXVOL + (vol1.right * vol2.right) / XMAXVOL;
	vol1.right = (vol1.right + vol2.right) * XMAXVOL / div;
	return vol1;
}

// The beeper's way out, by type (sources: the lab's specs/beeper-lpf/sources.md).
// Cut-offs in Hz, 0 where a stage is not there.
static const struct {
	double up, dn;		// one-pole low-pass, rising and falling edges
	double hp;		// one-pole high-pass
	double res, q;		// resonant high-pass: the piezo
} beepFltTab[BEEP_TYPES] = {
	{0, 0, 0, 0, 0},		// BEEP_LINE
	// ULA pin 28 rises faster than it falls (FE-Delay); the size is a fit to
	// recordings of real machines (tau 36.5 / 68.9 us), none of ours
	{4356, 2311, 0, 0, 0},		// BEEP_SPEAKER
	{3183, 3183, 0, 0, 0},		// BEEP_TV: 50 us
	{8600, 8600, 0, 0, 0},		// BEEP_EVO: 2K2 + 10 nF into 12K
	{0, 0, 530, 0, 0},		// BEEP_SCORPION: 0.1 uF into ~3K
	{0, 0, 80, 0, 0},		// BEEP_ATM: 0.1 uF into 20K
	// ZP-3: 4.1 kHz; its Q is not in the datasheet
	{0, 0, 0, 4100, 3},		// BEEP_PIEZO
};

#define BF_ONE	(1LL << 24)
#define BF_PI	3.14159265358979323846

static long long bf_pole(double hz, int rate) {
	if (hz <= 0) return 0;
	return llround(-expm1(-2 * BF_PI * hz / rate) * BF_ONE);
}

// work the coefficients out for a type at a rate, and settle where the level is now
static void bf_setup(sndBeepFlt* st, int lev, int type, int rate) {
	st->type = type;
	st->rate = rate;
	st->kUp = bf_pole(beepFltTab[type].up, rate);
	st->kDn = bf_pole(beepFltTab[type].dn, rate);
	st->kHp = bf_pole(beepFltTab[type].hp, rate);
	st->kF = 0;
	st->kQ = 0;
	if (beepFltTab[type].res > 0) {
		st->kF = llround(2 * sin(BF_PI * beepFltTab[type].res / rate) * BF_ONE);
		st->kQ = llround(BF_ONE / beepFltTab[type].q);
	}
	// settled: the low-passes on the level, a high-pass has taken it all away
	st->lp = lev * 65536LL;		// a level can be under 0: no shift of it
	st->hp = st->lp;
	st->low = st->lp;
	st->band = 0;
}

// One sub-sample of the beeper's level through its type's filter. A rate of 0 (no
// sound thread behind the mix) or a type out of range is let through as it is.
int snd_beep_flt(sndBeepFlt* st, int lev, int type, int rate) {
	if ((type <= BEEP_LINE) || (type >= BEEP_TYPES) || (rate <= 0)) {
		st->type = BEEP_LINE;	// a filter taken again settles where the level is then
		return lev;
	}
	if ((type != st->type) || (rate != st->rate)) bf_setup(st, lev, type, rate);
	long long x = lev * 65536LL;
	if (st->kUp) {
		long long k = (x > st->lp) ? st->kUp : st->kDn;
		st->lp += ((x - st->lp) * k + (BF_ONE >> 1)) >> 24;
		x = st->lp;
	}
	if (st->kHp) {
		st->hp += ((x - st->hp) * st->kHp + (BF_ONE >> 1)) >> 24;
		x -= st->hp;
	}
	if (st->kF) {
		// Chamberlin's state-variable filter, its high-pass out: the sounder
		// barely moves below its resonance, rings at it and passes what is
		// above. Scaled by 1/Q, so the peak is where the level was
		st->low += (st->band * st->kF) >> 24;
		long long high = x - st->low - ((st->band * st->kQ) >> 24);
		st->band += (high * st->kF) >> 24;
		x = (high * st->kQ) >> 24;
	}
	return (int)((x + (1 << 15)) >> 16);
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

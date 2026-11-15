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

// Cut-offs in Hz by beeper type, 0: no such stage
typedef struct {
	double up, dn;		// one-pole low-pass, rising and falling edges
	double hp;		// one-pole high-pass
	double res, q;		// resonant high-pass (Chamberlin)
	double bp;		// ...its band added, at this share: 6 dB an octave under f0
	double lp2;		// 2-pole low-passes, Butterworth
	int lpn;		// ...of this order: 2 or 4
} bfChain;

static const struct {
	bfChain beep;		// the beeper and the tape
	bfChain mix;		// the PSG, where it is mixed in before the filter (snd_beep_mixes)
} beepFltTab[BEEP_ALL] = {
	{{0, 0, 0, 0, 0}, {0}},			// BEEP_LINE
	{{4356, 2311, 0, 500, 0.7071067811865476}, {0}},	// BEEP_SPEAKER: pin 28's edges; 500 Hz a guess
	{{3183, 3183, 0, 0, 0}, {3183, 3183, 0, 0, 0}},	// BEEP_TV: 50 us, all the modulator gets
	{{2500, 2500, 340, 0, 0}, {2500, 2500, 150, 0, 0}},	// BEEP_PLUS3: C37 on TR5; PSG's 150 Hz a guess
	{{8600, 8600, 0, 0, 0}, {0}},		// BEEP_EVO: 2K2 + 10 nF into 12K
	{{0, 0, 530, 0, 0}, {0}},		// BEEP_SCORPION: 0.1 uF into ~3K
	{{0, 0, 80, 0, 0}, {0}},		// BEEP_ATM: 0.1 uF into 20K
	{{0, 0, 0, 4100, 3}, {0}},		// BEEP_PIEZO: ZP-3; Q not in the datasheet
	// experimental: the speaker's bass cut, a softer piezo, PWM music's 5 kHz
	{{4356, 2311, 0, 0, 0}, {0}},		// BEEP_X_SPK0
	{{4356, 2311, 0, 250, 0.7071067811865476}, {0}},
	{{4356, 2311, 0, 750, 0.7071067811865476}, {0}},	// Fuse's
	{{0, 0, 0, 4100, 3, 1}, {0}},		// BEEP_X_PZSOFT: 6 dB an octave under f0
	{{0, 0, 0, 4100, 2, 1}, {0}},
	{{0, 0, 0, 4100, 1.5, 1}, {0}},
	{{5000, 5000, 0, 0, 0}, {0}},		// BEEP_X_LP5_1
	{{0, 0, 0, 0, 0, 0, 5000, 2}, {0}},
	{{0, 0, 0, 0, 0, 0, 5000, 4}, {0}},
};

#define BF_ONE	(1LL << 24)
#define BF_PI	3.14159265358979323846

static long long bf_pole(double hz, int rate) {
	if (hz <= 0) return 0;
	return llround(-expm1(-2 * BF_PI * hz / rate) * BF_ONE);
}

// work the coefficients out for a type at a rate; the state stays as it is
static void bf_coefs(sndBeepFlt* st, int type, int rate, int chain) {
	const bfChain* c = chain ? &beepFltTab[type].mix : &beepFltTab[type].beep;
	st->type = type;
	st->rate = rate;
	st->kUp = bf_pole(c->up, rate);
	st->kDn = bf_pole(c->dn, rate);
	st->kHp = bf_pole(c->hp, rate);
	st->kF = 0;
	st->kQ = 0;
	if (c->res > 0) {
		st->kF = llround(2 * sin(BF_PI * c->res / rate) * BF_ONE);
		st->kQ = llround(BF_ONE / c->q);
	}
	st->kBp = llround(c->bp * BF_ONE);
	// Butterworth: Q 0.7071 for 2 poles, 0.5412 and 1.3066 for 4
	static const double q2[1] = {0.7071067811865476}, q4[2] = {0.5411961001461970, 1.3065629648763766};
	for (int i = 0; i < 2; i++) {
		st->kL[i] = 0;
		st->kLQ[i] = 0;
		if ((c->lp2 <= 0) || (i * 2 >= c->lpn)) continue;
		st->kL[i] = llround(2 * sin(BF_PI * c->lp2 / rate) * BF_ONE);
		st->kLQ[i] = llround(BF_ONE / ((c->lpn == 4) ? q4[i] : q2[0]));
	}
}

// a type taken anew: its coefficients, and settled where the level is now
static void bf_setup(sndBeepFlt* st, int lev, int type, int rate, int chain) {
	bf_coefs(st, type, rate, chain);
	// settled: the low-passes on the level, a high-pass has taken it all away
	st->lp = lev * 65536LL;		// a level can be under 0: no shift of it
	st->hp = st->lp;
	st->low = st->lp;
	st->band = 0;
	st->ll[0] = st->ll[1] = st->lp;
	st->lb[0] = st->lb[1] = 0;
}

// the filtering types only: snd_beep_flt() and snd_mix_flt() let the rest through.
// A state serves one chain, so its type alone says when to settle anew
int snd_beep_run(sndBeepFlt* st, int lev, int type, int rate, int chain) {
	// a speed change keeps the state: each stage's output is in it
	if (type != st->type)
		bf_setup(st, lev, type, rate, chain);
	else if (rate != st->rate)
		bf_coefs(st, type, rate, chain);
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
		// Chamberlin SVF, high-pass out: unity above, Q times at the resonance
		st->low += (st->band * st->kF) >> 24;
		long long high = x - st->low - ((st->band * st->kQ) >> 24);
		st->band += (high * st->kF) >> 24;
		x = high + ((st->band * st->kBp) >> 24);
	}
	for (int i = 0; (i < 2) && st->kL[i]; i++) {
		st->ll[i] += (st->lb[i] * st->kL[i]) >> 24;
		long long high = x - st->ll[i] - ((st->lb[i] * st->kLQ[i]) >> 24);
		st->lb[i] += (high * st->kL[i]) >> 24;
		x = st->ll[i];
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

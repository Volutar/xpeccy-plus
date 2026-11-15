// Headless beeper filter bench: snd_beep_flt() and snd_mix_flt() for every beeper type
// at the sound thread's sub-sample rates (44100 and 48000 Hz, 32 sub-samples each),
// against the cut-offs they are built from (sound/sndcommon.c, beepFltTab).
//
//	beeper-bench
//
// A step gives a one-pole's time constant (63 % of the way), a sine its gain at a
// frequency, a level held long enough what is left of it: nothing drifts and nothing
// is left over, so an idle machine's sound is the same with any type.

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "sound/sndcommon.h"

#define PI	3.14159265358979323846
#define LEV	8192		// a beeper's swing, about what the mix gives it

static const int outRates[2] = {44100 * 32, 48000 * 32};	// the sound thread's at 1x

static int failures;

static void check(const char* name, int ok) {
	printf("%s: %s\n", name, ok ? "ok" : "FAIL");
	if (!ok) failures++;
}

static int near(double got, double want, double tol) {
	return fabs(got - want) <= want * tol;
}

static int chain = BF_BEEP;	// which of a type's chains the helpers below run

static int flt(sndBeepFlt* st, int x, int type, int rate) {
	return (chain == BF_MIX) ? snd_beep_run(st, x, type, rate, BF_MIX) : snd_beep_flt(st, x, type, rate);
}

// a fresh filter, settled on a level
static void settle(sndBeepFlt* st, int type, int rate, int lev) {
	st->type = -1;
	flt(st, lev, type, rate);
}

// sub-samples a step from 'from' to 'to' takes to go 63.2 % of the way, as Hz
static double step_fc(int type, int rate, int from, int to) {
	sndBeepFlt st;
	settle(&st, type, rate, from);
	double mark = from + (to - from) * (1 - exp(-1));
	double was = from;
	for (int n = 1; n < rate; n++) {
		double y = flt(&st, to, type, rate);
		// a fast one is a few dozen sub-samples long: the crossing between two
		if ((to > from) ? (y >= mark) : (y <= mark))
			return rate / (2 * PI * (n - 1 + (mark - was) / (y - was)));
		was = y;
	}
	return 0;
}

// a high-pass: how long a step takes to fall back to 36.8 % of itself
static double hp_fc(int type, int rate) {
	sndBeepFlt st;
	settle(&st, type, rate, 0);
	double mark = LEV * exp(-1);
	double was = LEV;
	for (int n = 1; n < rate; n++) {
		double y = flt(&st, LEV, type, rate);
		if (y <= mark)
			return rate / (2 * PI * (n - 1 + (mark - was) / (y - was)));
		was = y;
	}
	return 0;
}

// a resonant high-pass's gain: 1 well above f0, about Q at it
static double res_hp(double hz, double f0, double q) {
	double r = hz / f0;
	return r * r / sqrt((1 - r * r) * (1 - r * r) + (r / q) * (r / q));
}

// one-poles' gains
static double lp1(double hz, double fc) {
	return 1 / sqrt(1 + (hz / fc) * (hz / fc));
}

static double hp1(double hz, double fc) {
	return (hz / fc) * lp1(hz, fc);
}

// gain at a frequency: a sine's peak after it has settled, over its own
static double gain(int type, int rate, double hz) {
	sndBeepFlt st;
	settle(&st, type, rate, 0);
	int skip = rate / 20;		// 50 ms for the filter to settle
	int span = rate / 20;
	int peak = 0;
	for (int n = 0; n < skip + span; n++) {
		int x = (int)lround(LEV * sin(2 * PI * hz * n / rate));
		int y = flt(&st, x, type, rate);
		if ((n >= skip) && (abs(y) > peak)) peak = abs(y);
	}
	return (double)peak / LEV;
}

// a level held for a second: what comes out at the end
static int held(int type, int rate, int lev) {
	sndBeepFlt st;
	settle(&st, type, rate, 0);
	int y = 0;
	for (int n = 0; n < rate; n++)
		y = flt(&st, lev, type, rate);
	return y;
}

static void lowpass(const char* name, int type, double up, double dn) {
	printf("== %s\n", name);
	for (int i = 0; i < 2; i++) {
		int rate = outRates[i];
		double fu = step_fc(type, rate, 0, LEV);
		double fd = step_fc(type, rate, LEV, 0);
		printf("%d: rise %.0f Hz, fall %.0f Hz\n", rate / 32, fu, fd);
		check("rise as built", near(fu, up, 0.01));
		check("fall as built", near(fd, dn, 0.01));
		if (up == dn) {
			double g = gain(type, rate, up);
			printf("%d: gain at %.0f Hz %.3f\n", rate / 32, up, g);
			check("-3 dB there", near(g, sqrt(0.5), 0.02));
		}
		check("a held level comes out as it went in", (held(type, rate, LEV) == LEV) && (held(type, rate, -LEV) == -LEV));
	}
}

static void highpass(const char* name, int type, double hp) {
	printf("== %s\n", name);
	for (int i = 0; i < 2; i++) {
		int rate = outRates[i];
		double f = hp_fc(type, rate);
		double g = gain(type, rate, hp);
		printf("%d: %.0f Hz, gain there %.3f\n", rate / 32, f, g);
		check("cut-off as built", near(f, hp, 0.01));
		check("-3 dB there", near(g, sqrt(0.5), 0.02));
		check("a held level dies away to nothing", held(type, rate, LEV) == 0);
	}
}

// a low-pass from 'lp' Hz and a high-pass from 'hp' Hz in one chain: the gain where it
// matters against the two one-poles', and a held level gone
static void band(const char* name, int type, double lp, double hp) {
	static const double at[5] = {0.5, 1, 3, 1, 4};	// * hp, * hp, * hp, * lp, * lp
	printf("== %s\n", name);
	for (int i = 0; i < 2; i++) {
		int rate = outRates[i];
		int ok = 1;
		printf("%d:", rate / 32);
		for (int k = 0; k < 5; k++) {
			double hz = at[k] * ((k < 3) ? hp : lp);
			double g = gain(type, rate, hz);
			printf(" %.0f Hz %.3f", hz, g);
			ok &= near(g, lp1(hz, lp) * hp1(hz, hp), 0.02);
		}
		printf("\n");
		check("the gains as built", ok);
		check("a held level dies away to nothing", held(type, rate, LEV) == 0);
	}
}

// the 48K: the pin's edges, slower falling, then the speaker's guessed low end
static void speaker(void) {
	printf("== speaker\n");
	for (int i = 0; i < 2; i++) {
		int rate = outRates[i];
		sndBeepFlt st;
		settle(&st, BEEP_SPEAKER, rate, 0);
		check("pin edges as built", (st.kUp == llround(-expm1(-2 * PI * 4356 / rate) * (1 << 24)))
			&& (st.kDn == llround(-expm1(-2 * PI * 2311 / rate) * (1 << 24))));
		double g100 = gain(BEEP_SPEAKER, rate, 100);
		double g500 = gain(BEEP_SPEAKER, rate, 500);
		double g2k = gain(BEEP_SPEAKER, rate, 2000);
		printf("%d: 100 Hz %.3f, 500 Hz %.3f, 2 kHz %.3f\n", rate / 32, g100, g500, g2k);
		check("the bass thinned, 2nd order", near(g100, res_hp(100, 500, sqrt(0.5)), 0.05));
		// the uneven edges shift a sine's peak a little: within 2 %, as the other -3 dB checks
		check("-3 dB at 500 Hz", near(g500, sqrt(0.5), 0.02));
		check("the middle through the edges", (g2k < lp1(2000, 4356) * 1.01) && (g2k > lp1(2000, 2311) * 0.99));
		check("a held level dies away to nothing", held(BEEP_SPEAKER, rate, LEV) == 0);
	}
}

// the PSG: only TV and +2A/+3 filter it, each channel on its own
static void mix(void) {
	printf("== mix\n");
	int rate = 48000 * 32;
	sndBeepFlt st[2], a;
	sndPair in = {1234, -567};
	st[0].type = st[1].type = BEEP_TV;
	int ok = 1;
	for (int t = BEEP_LINE; t < BEEP_TYPES; t++) {
		if (snd_beep_mixes(t)) continue;
		sndPair out = snd_mix_flt(st, in, t, rate);
		ok &= (out.left == in.left) && (out.right == in.right) && (st[0].type == BEEP_LINE) && (st[1].type == BEEP_LINE);
	}
	check("the other types let the PSG through", ok);
	// TV: the mix chain is the beeper's, a channel each
	st[0].type = st[1].type = a.type = -1;
	unsigned s = 1;
	ok = 1;
	for (int n = 0; n < rate / 10; n++) {
		s = s * 1103515245u + 12345u;
		int x = (int)((s >> 16) & 0x3fff) - 0x2000;
		sndPair p = {x, -x / 2};
		sndPair y = snd_mix_flt(st, p, BEEP_TV, rate);
		int b = snd_beep_flt(&a, x, BEEP_TV, rate);
		ok &= (y.left == b) && (abs(y.right + b / 2) <= 1);
	}
	check("TV: the PSG through the beeper's filter, left and right apart", ok);
	chain = BF_MIX;
	lowpass("mix tv", BEEP_TV, 3183, 3183);
	band("mix plus3", BEEP_PLUS3, 2500, 150);
	chain = BF_BEEP;
}

// a resonant high-pass with its band added: (s^2 + s) / (s^2 + s/Q + 1)
static double res_soft(double hz, double f0, double q) {
	double r = hz / f0, re = 1 - r * r, im = r / q;
	return r * sqrt(r * r + 1) / sqrt(re * re + im * im);
}

// a Butterworth low-pass of n poles
static double butter(double hz, double fc, int n) {
	return 1 / sqrt(1 + pow(hz / fc, 2 * n));
}

// the experimental types (BEEP_X_*): their Q24 coefficients, and their gains as built
static void experimental(void) {
	printf("== experimental\n");
	for (int i = 0; i < 2; i++) {
		int rate = outRates[i];
		for (int t = BEEP_TYPES; t < BEEP_ALL; t++) {
			sndBeepFlt st;
			settle(&st, t, rate, 0);
			printf("%d %d: %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld\n", rate / 32, t, st.kUp, st.kDn,
				st.kHp, st.kF, st.kQ, st.kBp, st.kL[0], st.kLQ[0], st.kL[1], st.kLQ[1]);
		}
		check("speaker, no bass cut: a held level through", held(BEEP_X_SPK0, rate, LEV) == LEV);
		// as the speaker's 500 Hz: the uneven edges shift a sine's peak a little
		check("speaker 250 / 750: -3 dB there", near(gain(BEEP_X_SPK250, rate, 250), sqrt(0.5), 0.02)
			&& near(gain(BEEP_X_SPK750, rate, 750), sqrt(0.5), 0.02));
		int ok = 1;
		static const double pq[3] = {3, 2, 1.5};
		for (int k = 0; k < 3; k++) {
			for (double hz = 500; hz <= 8000; hz *= 2)
				ok &= near(gain(BEEP_X_PZSOFT + k, rate, hz), res_soft(hz, 4100, pq[k]), 0.05);
			ok &= (held(BEEP_X_PZSOFT + k, rate, LEV) == 0);
		}
		check("piezo, 6 dB/oct: the gains as built, a held level gone", ok);
		ok = 1;
		static const int poles[3] = {1, 2, 4};
		for (int k = 0; k < 3; k++) {
			for (double hz = 1250; hz <= 20000; hz *= 2)
				ok &= (fabs(gain(BEEP_X_LP5_1 + k, rate, hz) - butter(hz, 5000, poles[k])) < 0.005);	// a few levels of 8192
			ok &= (held(BEEP_X_LP5_1 + k, rate, LEV) == LEV) && (held(BEEP_X_LP5_1 + k, rate, -LEV) == -LEV);
		}
		check("low-pass 5 kHz of 1, 2, 4 poles: the gains as built, a held level through", ok);
	}
}

static void piezo(void) {
	printf("== piezo\n");
	for (int i = 0; i < 2; i++) {
		int rate = outRates[i];
		double best = 0, at = 0;
		for (double hz = 2000; hz <= 8000; hz += 50) {
			double g = gain(BEEP_PIEZO, rate, hz);
			if (g > best) {
				best = g;
				at = hz;
			}
		}
		double lo = gain(BEEP_PIEZO, rate, 1000);
		double hi = gain(BEEP_PIEZO, rate, 12000);
		printf("%d: peak %.3f at %.0f Hz, 1 kHz %.3f, 12 kHz %.3f\n", rate / 32, best, at, lo, hi);
		// a resonant high-pass peaks a little over its f0: 1.03 of it at Q 3
		check("rings at 4.1 kHz", near(at, 4100, 0.05));
		check("Q times the level at the peak", near(best, res_hp(at, 4100, 3), 0.05));
		check("little bass", near(lo, res_hp(1000, 4100, 3), 0.05));
		check("the highs on their way to the level", near(hi, res_hp(12000, 4100, 3), 0.02));
		check("a held level dies away to nothing", held(BEEP_PIEZO, rate, LEV) == 0);
	}
}

// the Q24 coefficients at both offered rates: libm-made, so pinned here per platform
static void coefficients(void) {
	printf("== coefficients\n");
	for (int i = 0; i < 2; i++) {
		for (int t = BEEP_LINE + 1; t < BEEP_TYPES; t++) {
			sndBeepFlt st;
			settle(&st, t, outRates[i], 0);
			printf("%d %d: %lld %lld %lld %lld %lld\n", outRates[i] / 32, t, st.kUp, st.kDn, st.kHp, st.kF, st.kQ);
			if (!snd_beep_mixes(t)) continue;
			st.type = -1;
			snd_beep_run(&st, 0, t, outRates[i], BF_MIX);
			printf("%d %d mix: %lld %lld %lld %lld %lld\n", outRates[i] / 32, t, st.kUp, st.kDn, st.kHp, st.kF, st.kQ);
		}
	}
}

// the speeds offered: the filter's rate is the output's over the speed, its cut-offs heard at fc * speed
static const struct { int num, den; } speeds[7] = {
	{1, 8}, {1, 4}, {1, 2}, {2, 1}, {4, 1}, {8, 1}, {16, 1},
};

// as sndUpdateSpeed() works it out
static int speed_rate(int rate, int i) {
	return (int)llround(rate / ((double)speeds[i].num / speeds[i].den));
}

// the Q24 coefficients at every other speed, both rates
static void coefficients_by_speed(void) {
	printf("== coefficients by speed\n");
	for (int s = 0; s < 7; s++) {
		for (int i = 0; i < 2; i++) {
			for (int t = BEEP_LINE + 1; t < BEEP_TYPES; t++) {
				sndBeepFlt st;
				settle(&st, t, speed_rate(outRates[i], s), 0);
				printf("%d %d/%d %d: %lld %lld %lld %lld %lld\n", outRates[i] / 32, speeds[s].num, speeds[s].den,
					t, st.kUp, st.kDn, st.kHp, st.kF, st.kQ);
				if (!snd_beep_mixes(t)) continue;
				st.type = -1;
				snd_beep_run(&st, 0, t, speed_rate(outRates[i], s), BF_MIX);
				printf("%d %d/%d %d mix: %lld %lld %lld %lld %lld\n", outRates[i] / 32, speeds[s].num, speeds[s].den,
					t, st.kUp, st.kDn, st.kHp, st.kF, st.kQ);
			}
		}
	}
}

// a one-pole's cut-off from a step, interpolated on the log of what is left: exact even a sub-sample long
static double pole_fc(int type, int rate, int from, int to, int hp) {
	sndBeepFlt st;
	settle(&st, type, rate, from);
	double was = 0;
	for (int n = 1; n < rate; n++) {
		double y = flt(&st, to, type, rate);
		double left = hp ? y / to : (to - y) / (double)(to - from);
		if (left <= 0) return 0;
		double l = log(left);
		if (l <= -1)
			return rate / (2 * PI * (n - 1 + (was + 1) / (was - l)));
		was = l;
	}
	return 0;
}

// every type as built at every other speed: Q24 holds from 11 MHz to 88 kHz
static void by_speed(void) {
	printf("== by speed\n");
	for (int s = 0; s < 7; s++) {
		for (int i = 0; i < 2; i++) {
			int rate = speed_rate(outRates[i], s);
			double sp = gain(BEEP_SPEAKER, rate, 100), p3 = gain(BEEP_PLUS3, rate, 1000);
			double tv = pole_fc(BEEP_TV, rate, 0, LEV, 0), evo = pole_fc(BEEP_EVO, rate, 0, LEV, 0);
			double sc = pole_fc(BEEP_SCORPION, rate, 0, LEV, 1), atm = pole_fc(BEEP_ATM, rate, 0, LEV, 1);
			double pz = gain(BEEP_PIEZO, rate, 4100);
			printf("%d/%d %d: speaker at 100 Hz %.3f, plus3 at 1 kHz %.3f, tv %.0f, evo %.0f, scorpion %.0f, atm %.0f, piezo at f0 %.3f\n",
				speeds[s].num, speeds[s].den, outRates[i] / 32, sp, p3, tv, evo, sc, atm, pz);
			check("cut-offs as built", near(sp, res_hp(100, 500, sqrt(0.5)), 0.05)
				&& near(p3, lp1(1000, 2500) * hp1(1000, 340), 0.02) && near(tv, 3183, 0.01)
				&& near(evo, 8600, 0.01) && near(sc, 530, 0.01) && near(atm, 80, 0.01)
				&& near(pz, res_hp(4100, 4100, 3), 0.05));
			int lp = 1, hp = 1;
			for (int t = BEEP_SPEAKER; t <= BEEP_PIEZO; t++) {
				if ((t == BEEP_TV) || (t == BEEP_EVO))
					lp &= (held(t, rate, LEV) == LEV) && (held(t, rate, -LEV) == -LEV);
				else
					hp &= (held(t, rate, LEV) == 0);
			}
			check("held levels: through the low-passes, gone from the high-passes", lp && hp);
		}
	}
}

// a 1 kHz square in the machine's time; the rate goes from 'from' to 'to' (> 0) at sub-sample 'at'
static int tone_at(sndBeepFlt* st, int type, int n, int at, int from, int to) {
	int rate = ((to > 0) && (n >= at)) ? to : from;
	double sec = (rate == from) ? (double)n / from : (double)at / from + (double)(n - at) / to;
	int x = ((long long)(sec * 2000) & 1) ? LEV : 0;
	return snd_beep_flt(st, x, type, rate);
}

// 1x -> 1/8, 1x -> x16, x16 -> 1x 20 us after an edge: no step past a sub-sample's move, no swell after
static void speed_change(void) {
	int rate = 44100 * 32;
	printf("== speed change mid-note\n");
	for (int t = BEEP_LINE + 1; t < BEEP_TYPES; t++) {
		int ok = 1;
		for (int p = 0; p < 3; p++) {
			int from = (p == 2) ? rate / 16 : rate;
			int to = (p == 0) ? rate * 8 : ((p == 1) ? rate / 16 : rate);
			int at = (int)(7LL * from / 2000) + from / 50000 + 1;
			sndBeepFlt a, b;
			settle(&a, t, from, 0);
			settle(&b, t, from, 0);
			int ya = 0, yb = 0, pb = 0, peak = 0;
			for (int n = 0; n <= at; n++) {
				pb = yb;
				ya = tone_at(&a, t, n, at, from, to);
				yb = tone_at(&b, t, n, at, from, 0);
				if ((n < at) && (abs(yb) > peak)) peak = abs(yb);
			}
			int na = tone_at(&a, t, at + 1, at, from, to);
			int move = abs(yb - pb);
			if (abs(na - ya) > move) move = abs(na - ya);
			ok &= (abs(ya - yb) <= 2 * move + 2);
			// and the ms after: no swell either, the resonator's kept state included
			int pa = abs(na);
			for (int n = at + 1; n < at + from / 1000; n++) {
				int y = tone_at(&b, t, n, at, from, 0);
				if (abs(y) > peak) peak = abs(y);
				y = tone_at(&a, t, n + 1, at, from, to);
				if (abs(y) > pa) pa = abs(y);
			}
			ok &= (pa <= peak * 5 / 4 + 2);
		}
		printf("%d: ", t);
		check("no step", ok);
	}
}

int main(void) {
	sndBeepFlt st;
	coefficients();
	coefficients_by_speed();
	printf("== line\n");
	settle(&st, BEEP_LINE, 48000 * 32, 0);
	int same = 1;
	for (int n = 0; n < 1000; n++) {
		int x = (n & 64) ? LEV : -LEV / 3;
		same &= (snd_beep_flt(&st, x, BEEP_LINE, 48000 * 32) == x);
	}
	check("line out lets the level through", same);
	check("no rate: let through", snd_beep_flt(&st, 1234, BEEP_TV, 0) == 1234);
	// a high-pass that held a level, line out for a while, then the same type again:
	// it starts from where the level is now, not from what it held
	settle(&st, BEEP_SCORPION, 48000 * 32, 0);
	for (int n = 0; n < 48000 * 32 / 10; n++) snd_beep_flt(&st, LEV, BEEP_SCORPION, 48000 * 32);
	for (int n = 0; n < 1000; n++) snd_beep_flt(&st, 0, BEEP_LINE, 48000 * 32);
	check("a type taken again settles anew", snd_beep_flt(&st, 0, BEEP_SCORPION, 48000 * 32) == 0);
	speaker();
	lowpass("tv", BEEP_TV, 3183, 3183);
	band("plus3", BEEP_PLUS3, 2500, 340);
	lowpass("evo", BEEP_EVO, 8600, 8600);
	highpass("scorpion", BEEP_SCORPION, 530);
	highpass("atm", BEEP_ATM, 80);
	piezo();
	mix();
	experimental();
	by_speed();
	speed_change();
	return failures ? 1 : 0;
}

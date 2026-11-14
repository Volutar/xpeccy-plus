// Headless beeper filter bench: snd_beep_flt() for every beeper type at the sound
// thread's sub-sample rates (44100 and 48000 Hz, 32 sub-samples each), against the
// cut-offs it is built from (sound/sndcommon.c, beepFltTab).
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

// a fresh filter, settled on a level
static void settle(sndBeepFlt* st, int type, int rate, int lev) {
	st->type = -1;
	snd_beep_flt(st, lev, type, rate);
}

// sub-samples a step from 'from' to 'to' takes to go 63.2 % of the way, as Hz
static double step_fc(int type, int rate, int from, int to) {
	sndBeepFlt st;
	settle(&st, type, rate, from);
	double mark = from + (to - from) * (1 - exp(-1));
	double was = from;
	for (int n = 1; n < rate; n++) {
		double y = snd_beep_flt(&st, to, type, rate);
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
		double y = snd_beep_flt(&st, LEV, type, rate);
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

// gain at a frequency: a sine's peak after it has settled, over its own
static double gain(int type, int rate, double hz) {
	sndBeepFlt st;
	settle(&st, type, rate, 0);
	int skip = rate / 20;		// 50 ms for the filter to settle
	int span = rate / 20;
	int peak = 0;
	for (int n = 0; n < skip + span; n++) {
		int x = (int)lround(LEV * sin(2 * PI * hz * n / rate));
		int y = snd_beep_flt(&st, x, type, rate);
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
		y = snd_beep_flt(&st, lev, type, rate);
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
		double y = snd_beep_flt(&st, to, type, rate);
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
			double su = pole_fc(BEEP_SPEAKER, rate, 0, LEV, 0), sd = pole_fc(BEEP_SPEAKER, rate, LEV, 0, 0);
			double tv = pole_fc(BEEP_TV, rate, 0, LEV, 0), evo = pole_fc(BEEP_EVO, rate, 0, LEV, 0);
			double sc = pole_fc(BEEP_SCORPION, rate, 0, LEV, 1), atm = pole_fc(BEEP_ATM, rate, 0, LEV, 1);
			double pz = gain(BEEP_PIEZO, rate, 4100);
			printf("%d/%d %d: speaker %.0f/%.0f, tv %.0f, evo %.0f, scorpion %.0f, atm %.0f, piezo at f0 %.3f\n",
				speeds[s].num, speeds[s].den, outRates[i] / 32, su, sd, tv, evo, sc, atm, pz);
			check("cut-offs as built", near(su, 4356, 0.01) && near(sd, 2311, 0.01) && near(tv, 3183, 0.01)
				&& near(evo, 8600, 0.01) && near(sc, 530, 0.01) && near(atm, 80, 0.01)
				&& near(pz, res_hp(4100, 4100, 3), 0.05));
			int lp = 1, hp = 1;
			for (int t = BEEP_SPEAKER; t <= BEEP_EVO; t++)
				lp &= (held(t, rate, LEV) == LEV) && (held(t, rate, -LEV) == -LEV);
			for (int t = BEEP_SCORPION; t <= BEEP_PIEZO; t++)
				hp &= (held(t, rate, LEV) == 0);
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
	lowpass("speaker", BEEP_SPEAKER, 4356, 2311);
	lowpass("tv", BEEP_TV, 3183, 3183);
	lowpass("evo", BEEP_EVO, 8600, 8600);
	highpass("scorpion", BEEP_SCORPION, 530);
	highpass("atm", BEEP_ATM, 80);
	piezo();
	by_speed();
	speed_change();
	return failures ? 1 : 0;
}

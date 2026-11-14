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

// a resonant high-pass's gain, scaled by 1/Q as the filter is
static double res_hp(double hz, double f0, double q) {
	double r = hz / f0;
	return r * r / sqrt((1 - r * r) * (1 - r * r) + (r / q) * (r / q)) / q;
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
	static const int rates[2] = {44100 * 32, 48000 * 32};
	printf("== %s\n", name);
	for (int i = 0; i < 2; i++) {
		int rate = rates[i];
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
	static const int rates[2] = {44100 * 32, 48000 * 32};
	printf("== %s\n", name);
	for (int i = 0; i < 2; i++) {
		int rate = rates[i];
		double f = hp_fc(type, rate);
		double g = gain(type, rate, hp);
		printf("%d: %.0f Hz, gain there %.3f\n", rate / 32, f, g);
		check("cut-off as built", near(f, hp, 0.01));
		check("-3 dB there", near(g, sqrt(0.5), 0.02));
		check("a held level dies away to nothing", held(type, rate, LEV) == 0);
	}
}

static void piezo(void) {
	static const int rates[2] = {44100 * 32, 48000 * 32};
	printf("== piezo\n");
	for (int i = 0; i < 2; i++) {
		int rate = rates[i];
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
		check("the peak is where the level was", near(best, 1.0, 0.05));
		check("little bass", near(lo, res_hp(1000, 4100, 3), 0.05));
		check("the highs on their way to 1/Q", near(hi, res_hp(12000, 4100, 3), 0.02));
		check("a held level dies away to nothing", held(BEEP_PIEZO, rate, LEV) == 0);
	}
}

int main(void) {
	sndBeepFlt st;
	printf("== line\n");
	settle(&st, BEEP_LINE, 48000 * 32, 0);
	int same = 1;
	for (int n = 0; n < 1000; n++) {
		int x = (n & 64) ? LEV : -LEV / 3;
		same &= (snd_beep_flt(&st, x, BEEP_LINE, 48000 * 32) == x);
	}
	check("line out lets the level through", same);
	check("no rate: let through", snd_beep_flt(&st, 1234, BEEP_TV, 0) == 1234);
	lowpass("speaker", BEEP_SPEAKER, 4356, 2311);
	lowpass("tv", BEEP_TV, 3183, 3183);
	lowpass("evo", BEEP_EVO, 8600, 8600);
	highpass("scorpion", BEEP_SCORPION, 530);
	highpass("atm", BEEP_ATM, 80);
	piezo();
	return failures ? 1 : 0;
}

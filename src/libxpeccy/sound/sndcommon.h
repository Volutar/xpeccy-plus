#pragma once

typedef  struct {
	int master;
	int beep;
	int tape;
	int ay;
	int gs;
	int sdrv;
	int saa;
	unsigned dc:1;		// take each device's dc offset out before mixing
} sndVolume;

typedef struct {
	signed int left;
	signed int right;
} sndPair;

// What a unipolar level is worth at full scale: one AY channel at volume 15
// already fills the whole of it, and the soft clip holds any sum of them there.
// The beeper is given half, which is where Volutar put it - below that it is a
// dwarf beside an AY, and it has to leave the chips room in the same int16.
#define XMAXVOL		16384
#define BEEP_MAX	(XMAXVOL / 2)

extern char noizes[0x20000];

typedef struct {
	unsigned lev:1;			// 1/0 target level
	int val;			// current sound level (0..255)
//	long accum;			// ns accumulator
	int step;			// halfperiod counter
	unsigned int perH;		// halfperiod for lev=1
	unsigned int perL;		// halfperiod for lev=0
	int pcount;			// current halfperiod counter
} bitChan;

// The beeper's share of the mix: val is the 0..255 above, vol the percent
// from the options.
static inline int bc_level(bitChan* ch, int vol) {
	return ch->val * vol * BEEP_MAX / (0xff * 100);
}

bitChan* bcCreate();
void bcReset(bitChan*);
void bcDestroy(bitChan*);
void bcSync(bitChan*, int);

sndPair mixer(sndPair, sndPair);

// A dc blocker, one per device, kept by whoever mixes them. None of the chips swing
// about zero - an AY sits between silence and full - and that offset is not sound:
// it is a level the speaker holds, and it eats the headroom every device shares. So
// it comes off each of them on the way in, not off the sum, because a device that
// walks its own level up has already taken what the others needed by then.
//
// One pole. The corner is under a hertz - 1<<18 samples is about a fifth of a second
// at the rate a mixer is called - which leaves the lowest note alone. Inline because
// it is called for every device on every sub-sample, better than a million times a
// second, and it is off by default: that case has to cost a branch and nothing else.
#define SND_DC_BITS	18

typedef struct {
	long long acc[2];
	int on;			// what it was called with last
} sndDC;

// What a sample has to fit in by the time it leaves. Written out by hand in four
// places before this. Spelled out rather than through toLimits(), which lives in
// xcore and has no business being reached for from here.
static inline sndPair snd_clip16(sndPair lev) {
	if (lev.left < -0x8000) lev.left = -0x8000;
	else if (lev.left > 0x7fff) lev.left = 0x7fff;
	if (lev.right < -0x8000) lev.right = -0x8000;
	else if (lev.right > 0x7fff) lev.right = 0x7fff;
	return lev;
}

static inline sndPair snd_dc(sndDC* st, sndPair lev, int on) {
	if (on != st->on) {	// switched either way: settle on this sample,
		st->on = on;	// rather than slide in from whatever it held
		st->acc[0] = (long long)lev.left << SND_DC_BITS;
		st->acc[1] = (long long)lev.right << SND_DC_BITS;
	}
	if (!on) return lev;
	long long m = st->acc[0] >> SND_DC_BITS;
	st->acc[0] += lev.left - m;
	lev.left -= (int)m;
	m = st->acc[1] >> SND_DC_BITS;
	st->acc[1] += lev.right - m;
	lev.right -= (int)m;
	return lev;
}

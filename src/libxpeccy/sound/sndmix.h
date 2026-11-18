#pragma once

// What a machine's mix reads, gathered out of the machine. A machine's vol function
// fills one from its own devices and mixes from it (zx_mix, nx_mix, alf_mix); the mix
// itself looks at nothing else, so it can as well be run from a copy of these devices
// kept somewhere else. What the mix keeps from one sample to the next - the dc
// blockers, the levels it worked out last - is in here too.

#include "sndcommon.h"
#include "ayym.h"
#include "gs.h"

#ifdef __cplusplus
extern "C" {
#endif

// What a mix hears at a sample, every byte meant: it is compared and copied as bytes
// (sound.cpp sends a copy only when it changed), so there is no padding in it
typedef struct {
	int beep;		// the beeper's level, bitChan.val
	int tapeRec;		// the tape: recording, and the level it records
	int tapeLevRec;
	int tapeMute;		// ...or playing, unless muted, at this level
	int tapeVolPlay;
	int beepType;		// BEEP_*: the filter the two of them (and on some types the PSG) go out through
	sndPair gs;		// zx_mix's other devices, as their own volume functions give them
	sndPair sdrv;
	sndPair saa;
	int sel;		// the Next: the chip selected, the one heard with TurboSound off
	unsigned char nr06, nr08, nr09;	// ...the mixer's nextregs: AY or YM table, TurboSound, ACB, mono
	unsigned char pan[3];	// ...each chip's L/R enables
	unsigned char dac[4];	// ...the DACs' outputs
	unsigned char pad[2];
} sndMixIn;

typedef struct {
	sndMixIn in;
	TSound* ts;		// the PSGs
	int late;		// the Next: ns before the chips' time the sample is due
	int rate;		// sub-samples a second of the machine's time; 0: none (the beeper's filter steps aside)
	// kept by the mix
	sndBeepFlt bflt;
	sndBeepFlt pflt[2];	// the PSG's left and right, on the types that mix it in first
	sndDC dc[5];
	int bVal, bVol, bLev;	// the beeper's share, for the val and volume it was worked out for
	unsigned int dOut;	// the DACs' share, likewise
	int dVol, dL, dR;
	gsKept gsk;		// the General Sound's level as the take last asked it (zx_snd_take)
} sndMix;

// the tape's share of a sample, in the beeper's units: what both mixes start from
static inline int snd_tape_level(const sndMix* m, const sndVolume* sv) {
	if (m->in.tapeRec)
		return (m->in.tapeLevRec ? 0x800 : -0x800) * sv->tape / 100;
	if (!m->in.tapeMute)
		return ((m->in.tapeVolPlay - 0x80) << 8) * sv->tape / 1600;
	return 0;
}

// the PSGs' share of a sample (zx_mix, alf_mix): By machine through the board's mix filter,
// if it has one (snd_beep_mixes); then their dc blocker and volume
static inline sndPair snd_psg_part(sndMix* m, const sndVolume* sv, int type, int all, int dc) {
	sndPair v = tsGetVolume(m->ts, sv->psgLay);
	if (!all) v = snd_mix_flt(m->pflt, v, type, m->rate);
	v = snd_dc(&m->dc[1], v, dc);
	v.left = v.left * sv->ay / 100;
	v.right = v.right * sv->ay / 100;
	return v;
}

#ifdef __cplusplus
}
#endif

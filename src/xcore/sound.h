#pragma once

#include <vector>
#include <string>

#if defined(__linux) || defined(__BSD)
	#include <sys/ioctl.h>
	#include <sys/soundcard.h>
	#include <fcntl.h>
#endif

#include "xcore.h"

enum xSoundOutput {
	xOutputNone = 0,
	xOutputOss,
	xOutputAlsa,
	xOutputSDL,
	xOutputWave
};

typedef struct {
	int id;
	const char* name;
	int (*open)();
	void (*play)();		// start playing what has been buffered so far
	void (*close)();
} OutSys;

// The ring is sbuf in sound.cpp, 4 bytes to a frame - 341 ms at 48kHz. It is
// sized well over the latency ceiling: a bluetooth headset needs far more of a
// buffer than a wired one, and the ring has to hold the worst of them.
#define SND_RING_SIZE	0x10000
#define SND_RING_MASK	(SND_RING_SIZE - 1)

// Filling stops here. Past it the writer would overwrite sound that has not
// been played yet.
#define SND_RING_HIGH_WATER	(SND_RING_SIZE * 7 / 8)

// One audio callback block, in ms. The callback asks for a whole block at once,
// so the ring can never hold less than this without clicking: the block is the
// floor under conf.snd.latency, and the target is at least two of them.
#define SND_BLOCK_MS	5
#define SND_LATENCY_MIN	(2 * SND_BLOCK_MS)
#define SND_LATENCY_MAX	150
// where a fresh install starts. Above where the regulator comes to rest, on
// purpose: Auto walks a too-high value quietly down, while too low a one is
// heard before it climbs back.
#define SND_LATENCY_DEF	30

// the highest rate the app offers. the ring holds fewer ms the higher this
// goes, so it is what the size has to be checked against (see pacing.cpp)
#define SND_MAX_RATE		48000

// Output rates offered, highest first, 0 terminates. It lives here rather than
// in the options dialog because the config parser has to check against the same
// list - a rate this build does not offer would leave the box with nothing
// selected. 11025 and 22050 were dropped in 2026.4: every audio engine now runs
// at 44100 or 48000, so a lower rate only bought a resample on the way out plus
// the fold-back our box decimation cannot filter.
//
// Auto is bounded by the same list, so a device running at anything else - 96000,
// say - is still asked for the nearest of these and still resamples. Following it
// exactly would mean sizing the ring at run time, which SND_MAX_RATE rules out.
extern const int sndRateTab[];
int sndRateSupported(int);

extern OutSys sndTab[];
extern OutSys* sndOutput;

extern long long nsPerSampleFixed;

// The scope's own capture, at the sub-sample rate - 32 of these go into one sample
// of what reaches the speakers. The output ring cannot serve the scope on its own:
// it is decimated, and it stops being filled the moment the debugger takes the
// machine, which is exactly when a waveform is worth looking at.
int snd_scope(short*, int);		// the newest sub-samples, oldest first
double snd_scope_rate();		// how many of them a second
void snd_scope_step(Computer*, int);	// carry the capture over ns of a held machine

void sndInit();
void addOutput(std::string, bool(*)(),void(*)(),void(*)());
void setOutput(const char*);

void sndClose();
int sndSync(Computer*);
int sndGetRingDistance();
int sndGetRingTargetBytes();
int sndPlaybackActive();
void sndAutoTick(long long);

int snd_wav_open(const char*);
void snd_wav_close();
void snd_wav_write();

std::string sndGetName();
void sndDebug();

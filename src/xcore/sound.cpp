#include <stdio.h>

#define _USE_MATH_DEFINES
#include <cmath>

#include "sound.h"
#include "xcore.h"

#include <iostream>
#include <QMutex>
#include <QWaitCondition>

#include <SDL.h>

// new
static unsigned char sbuf[SND_RING_SIZE];
static int posf = 0x0004;			// fill pos
static int posp = 0x0004;			// play pos

// A frame is four bytes here, stereo 16 bit, always. That four is what the 250
// is: a millisecond holds rate/1000 frames, so rate/250 bytes.
static int snd_ms_to_bytes(int ms) {
	return ms * conf.snd.rate / 250;
}

static int snd_bytes_to_ms(int bytes) {
	return conf.snd.rate ? (bytes * 250 / conf.snd.rate) : 0;
}

// The scope's ring, mono, at the sub-sample rate: 0x20000 of them is about 90ms at
// 44100. Written by the emulation thread while the machine plays and by the gui
// thread while the debugger steps it - never both, since a held machine's thread is
// not running - and read without a lock, where a torn sample is one wrong pixel.
#define SND_SCOPE_SIZE	0x20000
#define SND_SCOPE_MASK	(SND_SCOPE_SIZE - 1)
static short scopeBuf[SND_SCOPE_SIZE];
static int scopePos = 0;
static long long scopeNsFixed = 0;	// emulated time not yet turned into samples

// The scope takes the offset out for itself, or the trace floats above the zero
// line with the whole lower half of the box dead and the line saying nothing. Only
// while nothing is filtered on the way to the speakers, though: two of these in
// series would bend a wave twice. The master volume is deliberately not applied
// either, or turning the wheel down would flatten the picture along with the sound.
static sndDC scopeDc;

static void scope_put(int lev) {
	sndPair p;
	p.left = lev;
	p.right = lev;
	p = snd_clip16(snd_dc(&scopeDc, p, !conf.snd.vol.dc));
	scopeBuf[scopePos & SND_SCOPE_MASK] = (short)p.left;
	scopePos++;
}

static int smpCount = 0;
OutSys *sndOutput = NULL;
// static int sndChunks = 882;

#define DISCRATE 32
// ns per sub-sample, 16.16. This turns emulated time into the sample budget the
// pacer hands out, so the fraction matters: at 44100Hz the whole-ns value was
// 708 against a true 708.6168.
static long long ns_per_sample_fixed(int rate) {
	return NSD_TO_FIXED(1e9 / rate / DISCRATE);
}

long long nsPerSampleFixed = ns_per_sample_fixed(44100);
static int wavRate = 0;			// rate the open recording's header says
static sndPair sndLev;

OutSys* findOutSys(const char*);

// Decimation, DISCRATE oversampled samples down to one output sample.
//
// With the filter off this is the plain average of a block of DISCRATE samples,
// the way it has always been done here. That is a rectangular window: first
// sidelobe 13 dB down, so nearly everything above the output Nyquist folds back
// into the audible band. Bit-banged beeper music packs its energy right up at
// the oversample Nyquist, and that folding is what makes it crunch.
//
// With it on, a windowed-sinc lowpass runs over a sliding window of the last
// SND_FIR_TAPS oversampled samples. Measured against the average, the worst band
// that folds into 0..16 kHz drops from -7 dB to -83 dB. Fewer taps are not worth
// having: at 128 that figure is -11 dB, no better than the average it replaces.
//
// The taps are fixed point (Q.SND_FIR_SHIFT) because the decimator runs on the
// emulation thread; snd_init_filter() builds them once at startup, in double.
// Measured, the filter costs about 1.5% of one core at 48 kHz against 0.08%
// for the average.
#define SND_FIR_TAPS	512
#define SND_FIR_MASK	(SND_FIR_TAPS - 1)
#define SND_FIR_SHIFT	24
static int firTaps[SND_FIR_TAPS / 2] = {0};	// symmetric: half is enough

static int sb_pos = 0;
static sndPair smpBuf[SND_FIR_TAPS] = {{0,0}};

#if defined(HAVESDL2)
static SDL_AudioDeviceID sdldevid;
#endif

// output

// Playback is held until the emulation has put a target's worth of sound in the
// ring. Started the other way round - playing at once from an empty ring - the
// whole of the app's startup went out as one held sample, and the ring stayed
// one block short of underrunning from then on. Only a device that really plays
// the ring is ever held; setOutput() sets this when it opens one.
static int sndHeld = 0;

// Auto latency. What makes a click is a callback finding less than its own block
// left in the ring, so the thing to watch is not the underrun but how close we
// came to one: sndLowMark is the least the ring was left with after any block
// since the last look. Raise at the first sign of that running thin, lower only
// after a long clean spell - stepping down eagerly would walk the setting back
// onto the edge and click there again and again. A step down asks for two spare
// blocks after a callback and a step up for less than one, so both stop of their
// own accord - but not at the setting the arithmetic suggests: the ring swings
// well below its target, so the mark reads lower than that and it rests higher.
#define SND_AUTO_WINDOW_MS	1000
#define SND_AUTO_HALF_BLOCK	((SND_BLOCK_MS + 1) / 2)	// rounded up: never nothing
#define SND_AUTO_UP_MS		SND_AUTO_HALF_BLOCK	// a near miss: a nudge
#define SND_AUTO_UP_CLICK_MS	SND_BLOCK_MS		// it clicked: a whole block
#define SND_AUTO_DOWN_MS	SND_AUTO_HALF_BLOCK	// the smallest step down
#define SND_AUTO_DOWN_AFTER	60	// windows with room to spare before a step down
#define SND_AUTO_DRY_HOLD	15	// windows before another dry one is answered
#define SND_AUTO_GRACE		3	// windows to skip after a device starts: it takes
									// its own fill out of the ring in one go
#define SND_LOW_NONE		0x7fffffff	// no block was played in the window

// The low water mark is what the ring was left with after the emptiest block
// of the window, and it is kept unclamped on purpose: below zero says the
// block ran out and by how much, which is all the underrun line needs.
static int sndLowMark = SND_LOW_NONE;	// written by the audio callback
static int sndUnderruns = 0;		// blocks it found short, this window
static int sndAutoGood = 0;
static int sndAutoSkip = 0;
static int sndAutoDryHold = 0;	// windows to sit out after answering a dry one
static long long sndAutoLastNs = 0;
// what the log was last told, so a change can be spotted whoever made it.
// no setting can hold -1, so the first look states where the run starts.
static int sndLogLat = 0;
static int sndLogAuto = -1;

static void snd_start_playback() {
	sndHeld = 0;
	sndLowMark = SND_LOW_NONE;
	sndAutoSkip = SND_AUTO_GRACE;
	sndAutoDryHold = 0;
	if (sndOutput && sndOutput->play)
		sndOutput->play();
}

// return 1 when buffer is full
// NOTE: need sync|flush devices if debug
int sndSync(Computer* comp) {
	if (!conf.emu.pause || comp->flgDBG) {
		gsFlush(comp->gs);
//		saaFlush(comp->saa);
		if (!conf.emu.fast && !conf.emu.pause) {
			sndLev = comp->hw->vol(comp, &conf.snd.vol);
			scope_put((sndLev.left + sndLev.right) / 2);
			sndLev.left = sndLev.left * conf.snd.vol.master / 100;
			sndLev.right = sndLev.right * conf.snd.vol.master / 100;
			// both ends: FM is signed, and a sample under the low one
			// wraps to the top of the range on the way into the ring
			sndLev = snd_clip16(sndLev);

			smpBuf[sb_pos & SND_FIR_MASK] = sndLev;
			sb_pos++;
			if ((sb_pos % DISCRATE) == 0) {
				if (conf.snd.filter) {
					// The window ends at the newest sample, so it starts SND_FIR_TAPS
					// back - which is where sb_pos points now. The taps are symmetric,
					// so the two ends of the window share one and half the multiplies
					// go: measured, a third off the time, and the same answer.
					long long accL = 0;
					long long accR = 0;
					int lo = sb_pos & SND_FIR_MASK;
					int hi = (sb_pos - 1) & SND_FIR_MASK;
					for (int i = 0; i < SND_FIR_TAPS / 2; i++) {
						accL += (long long)(smpBuf[lo].left + smpBuf[hi].left) * firTaps[i];
						accR += (long long)(smpBuf[lo].right + smpBuf[hi].right) * firTaps[i];
						lo = (lo + 1) & SND_FIR_MASK;
						hi = (hi - 1) & SND_FIR_MASK;
					}
					// the sinc overshoots a step a little, so the result can
					// come out past what went in
					sndLev.left = (int)(accL >> SND_FIR_SHIFT);
					sndLev.right = (int)(accR >> SND_FIR_SHIFT);
					sndLev = snd_clip16(sndLev);
				} else {
					int sumL = 0;
					int sumR = 0;
					int idx = (sb_pos - DISCRATE) & SND_FIR_MASK;
					for (int i = 0; i < DISCRATE; i++) {
						sumL += smpBuf[idx].left;
						sumR += smpBuf[idx].right;
						idx = (idx + 1) & SND_FIR_MASK;
					}
					sndLev.left = sumL / DISCRATE;
					sndLev.right = sumR / DISCRATE;
				}

				if (!conf.snd.enabled) {
					sndLev.left = 0;
					sndLev.right = 0;
				}
				if (conf.snd.need > 0)
					conf.snd.need--;

				// the wav takes the sample here, where it is made. The old
				// accumulator in ethread.cpp ran off its own 44100 Hz clock, a
				// few ns per sample away from this one, and the two beat audibly.
				if (conf.snd.wavout)
					snd_wav_write();

				sbuf[posf & SND_RING_MASK] = sndLev.left & 0xff;
				posf++;
				sbuf[posf & SND_RING_MASK] = (sndLev.left >> 8) & 0xff;
				posf++;
				sbuf[posf & SND_RING_MASK] = sndLev.right & 0xff;
				posf++;
				sbuf[posf & SND_RING_MASK] = (sndLev.right >> 8) & 0xff;
				posf++;

				if (sndHeld && (sndGetRingDistance() >= sndGetRingTargetBytes()))
					snd_start_playback();
			}
			smpCount++;
		}
	}
#if NEW_SMP_METHOD
	if (conf.snd.need > 0) return 0;
#else
	if (smpCount < sndChunks) return 0;
#endif
	conf.snd.fill = 0;
	smpCount = 0;
	return 1;
}

std::string sndGetOutputName() {
	std::string res = "NULL";
	if (sndOutput != NULL) {
		res = sndOutput->name;
	}
	return res;
}

void setOutput(const char* name) {
	if (sndOutput != NULL) {
		sndOutput->close();
	}
	sndOutput = findOutSys(name);
	if (sndOutput == NULL) {
		xlog(XLG_SOUND, XLL_WARN, "no sound system '%s', falling back to none", name);
		setOutput("NULL");
	} else if (!sndOutput->open()) {
		xlog(XLG_SOUND, XLL_WARN, "can't open sound system '%s', falling back to none", name);
		setOutput("NULL");
	}
	sndHeld = sndPlaybackActive();
	nsPerSampleFixed = ns_per_sample_fixed(conf.snd.rate);
	// a recording's rate is in its header and cannot change, so it ends here
	// rather than play back at the wrong speed from this point on
	if (conf.snd.wavfile && (conf.snd.rate != wavRate)) {
		xlog(XLG_SOUND, XLL_INFO, "wav output stopped: rate is now %i Hz", conf.snd.rate);
		snd_wav_close();
	}
}

void sndClose() {
	if (sndOutput != NULL)
		sndOutput->close();
}

std::string sndGetName() {
	std::string res = "NULL";
	if (sndOutput != NULL) {
		res = sndOutput->name;
	}
	return res;
}

// true when the current output really plays the ring buffer. the NULL device
// doesn't: nothing moves the play position, so whoever waits for the buffer to
// drain would wait forever (see pacing.cpp).
int sndPlaybackActive() {
	return (sndOutput != NULL) && (sndOutput->id != xOutputNone);
}

static void snd_auto_step(int low) {
	if (!conf.snd.latauto || !sndPlaybackActive() || conf.emu.fast || conf.emu.pause) {
		sndAutoSkip = SND_AUTO_GRACE;
		sndAutoDryHold = 0;
		return;
	}
	if (sndAutoSkip > 0) {
		sndAutoSkip--;
		return;
	}
	if (low == SND_LOW_NONE) return;
	int block = snd_ms_to_bytes(SND_BLOCK_MS);	// one callback block, in bytes
	// A step only moves the target; the pacer then walks the ring to it, and
	// slower the closer it gets, so arriving takes tens of seconds. Reading the
	// walk itself as a shortage and stepping again ran the setting from 30 to
	// 130 ms in under a minute, so the near miss below waits for the ring.
	//
	// Running out is different: that is a fact about the machine, not about the
	// walk, and waiting swallowed it - the ring sits under its target exactly
	// when a machine is in trouble, so the wait was shut for most of the
	// emergencies on the machines that need this most. So answer at once, but
	// not on every one: a tall step leaves the ring thin for a while, and
	// answering that would climb to the ceiling on its own. So hold off until
	// the ring has arrived, or a few seconds have passed - whichever is sooner,
	// since a machine that never lets it arrive still has to be answered.
	int arrived = (sndGetRingDistance() + block >= sndGetRingTargetBytes());
	if (arrived) sndAutoDryHold = 0;
	else if (sndAutoDryHold > 0) sndAutoDryHold--;
	int lat = conf.snd.latency;
	if (low < 0) {			// it ran out: the click was heard
		if (sndAutoDryHold) return;
		sndAutoDryHold = SND_AUTO_DRY_HOLD;
		sndAutoGood = 0;
		lat += SND_AUTO_UP_CLICK_MS;
	} else if (!arrived) {
		return;
	} else if (low < block) {		// one late callback away from a click
		sndAutoGood = 0;
		lat += SND_AUTO_UP_MS;
	} else if (low <= 2 * block) {	// no spare block: hold where it is
		sndAutoGood = 0;
	} else if (++sndAutoGood >= SND_AUTO_DOWN_AFTER) {
		// give back half of what was never touched. creeping down by the
		// smallest step from a hand set 100 ms took a quarter of an hour.
		int spare = snd_bytes_to_ms(low - 2 * block);
		lat -= (spare / 2 > SND_AUTO_DOWN_MS) ? spare / 2 : SND_AUTO_DOWN_MS;
		sndAutoGood = 0;
	}
	conf.snd.latency = toLimits(lat, SND_LATENCY_MIN, SND_LATENCY_MAX);
}

// The setting moves under Auto and by hand from the options dialog. Comparing
// against what the log was last told catches both without hooking either.
static void snd_log_setting() {
	if ((conf.snd.latency == sndLogLat) && (conf.snd.latauto == sndLogAuto)) return;
	xlog(XLG_SOUND, XLL_INFO, "latency %i ms, auto %s", conf.snd.latency,
		conf.snd.latauto ? "on" : "off");
	sndLogLat = conf.snd.latency;
	sndLogAuto = conf.snd.latauto;
}

// Called from the pacer, which hands over its own reading of the clock. The
// window is measured in time rather than in ticks so that a late or coarse SDL
// timer cannot stretch it.
void sndAutoTick(long long nowNs) {
	if (nowNs - sndAutoLastNs < SND_AUTO_WINDOW_MS * 1000000LL) return;
	sndAutoLastNs = nowNs;
	int low = sndLowMark;
	int under = sndUnderruns;
	sndLowMark = SND_LOW_NONE;
	sndUnderruns = 0;
	// the audio thread only counts; the line is put together here, where a
	// lock and a flush cost nothing that anybody hears
	if (under > 0) {
		// low went below zero to get here, so put the block back to say what
		// the ring actually held
		int had = low + snd_ms_to_bytes(SND_BLOCK_MS);
		xlog(XLG_SOUND, XLL_WARN, "buffer ran out %i time%s: %i ms left, target %i ms",
			under, (under == 1) ? "" : "s", snd_bytes_to_ms(had), conf.snd.latency);
	}
	snd_auto_step(low);
	snd_log_setting();
}

//------------------------
// Sound output
//------------------------

#if USEMUTEX
extern QMutex emutex;
extern QWaitCondition qwc;
#else
extern int sleepy;
#endif

// NULL

int null_open() {
	xlog(XLG_SOUND, XLL_INFO, "no sound device");
//	sndChunks = conf.snd.rate / 50 * DISCRATE;
	return 1;
}

void null_close() {
}

// SDL

#include <QDebug>

// gap in ring-buffer bytes between the write position (posf) and the play
// position (posp). used here for overfill, and by pacing.cpp to avoid
// running ahead of real playback.
int sndGetRingDistance() {
	return (posf - posp) & SND_RING_MASK;
}

double snd_scope_rate() {
	return (double)conf.snd.rate * DISCRATE;
}

// Carry the capture across ns of emulated time on a machine the debugger is holding.
// Nothing is being played then - sndSync() is not even called - but the mixer still
// has an output for every moment of it, which is the whole point of tracing a beeper.
void snd_scope_step(Computer* comp, int ns) {
	if ((ns <= 0) || !comp->hw->vol) return;
	scopeNsFixed += NS_TO_FIXED(ns);
	while (scopeNsFixed > nsPerSampleFixed) {
		sndPair lev = comp->hw->vol(comp, &conf.snd.vol);
		scope_put((lev.left + lev.right) / 2);
		scopeNsFixed -= nsPerSampleFixed;
	}
}

// The newest `len` sub-samples, oldest first.
int snd_scope(short* buf, int len) {
	if (len > SND_SCOPE_SIZE) len = SND_SCOPE_SIZE;
	if (len < 1) return 0;
	// two runs at most, the ring being a power of two - a window is tens of
	// thousands of samples and this is asked for once a frame
	int pos = (scopePos - len) & SND_SCOPE_MASK;
	int run = SND_SCOPE_SIZE - pos;
	if (run > len) run = len;
	memcpy(buf, scopeBuf + pos, run * sizeof(short));
	if (run < len)
		memcpy(buf + run, scopeBuf, (len - run) * sizeof(short));
	return len;
}

// How much sound we aim to keep in the ring. Under one callback block it
// clicks by definition, so the setting is clamped well above that; the pacer
// (pacing.cpp) trims emulated time to hold the ring here.
int sndGetRingTargetBytes() {
	return snd_ms_to_bytes(toLimits(conf.snd.latency, SND_LATENCY_MIN, SND_LATENCY_MAX));
}

void sdlPlayAudio(void*, Uint8* stream, int len) {
	int want = len;
//	printf("len = %i\n",len);
	// conf.snd.need is no longer filled here - it is filled from a steady
	// timer in pacing.cpp, so frame making does not run in audio-buffer sized
	// bursts. See pacing.h for why. This callback only plays the ring buffer.
	int dist = sndGetRingDistance();
	int idle = conf.emu.fast || conf.emu.pause;
	if ((dist < len) || idle) {				// overfill : fill with last sample of previous buf
//		printf("overfill : %i %i\n", posf, posp);
		while(len > 0) {
			*(stream++) = sbuf[(posp - 4) & SND_RING_MASK];
			*(stream++) = sbuf[(posp - 3) & SND_RING_MASK];
			*(stream++) = sbuf[(posp - 2) & SND_RING_MASK];
			*(stream++) = sbuf[(posp - 1) & SND_RING_MASK];
			len -= 4;
		}
	} else {						// normal : play buffer
		while(len > 0) {
			*(stream++) = sbuf[posp & SND_RING_MASK];
			posp++;
			len--;
		}
	}
	// how little the ring was left with. while idle it is not drained at all,
	// so leave the mark alone.
	if (!idle) {
		int left = dist - want;
		if (left < 0) sndUnderruns++;	// nothing to play: this is the click
		if (left < sndLowMark) sndLowMark = left;
	}
#if USEMUTEX
	qwc.wakeAll();
#elif NEW_SMP_METHOD
	sleepy = conf.snd.need ? 0 : 1;
#else
	sleepy = 0;
#endif
}

const int sndRateTab[] = {48000, 44100, 0};

// The offered rate closest to what was asked. Auto has to land on one of them:
// the ring is sized against SND_MAX_RATE, and a rate outside the list would
// leave the options box with nothing selected and be refused on the next load.
static int snd_nearest_rate(int rate) {
	int res = sndRateTab[0];
	for (int i = 0; sndRateTab[i]; i++) {
		if (abs(sndRateTab[i] - rate) < abs(res - rate))
			res = sndRateTab[i];
	}
	return res;
}

int sndRateSupported(int rate) {
	return snd_nearest_rate(rate) == rate;
}

// Auto rate: ask the default device what it runs at, and ask for that.
//
// Any other rate makes something in the chain resample, and on Windows that is
// the audio engine's own converter at default quality - SDL sets
// AUTOCONVERTPCM | SRC_DEFAULT_QUALITY as soon as the rate differs from the
// device's mix format - which is audible as a whistle under silence. The
// mismatch buys nothing either way: the device's period is a fixed number of
// milliseconds whatever rate it is fed.
//
// Every backend the project ships on answers this - wasapi, directsound,
// pulseaudio, alsa, pipewire, coreaudio - so there is no second mechanism for
// the ones that don't: they simply keep the rate they were given.
static void snd_pick_auto_rate() {
	if (!conf.snd.rateauto) return;
#if defined(HAVESDL2) && SDL_VERSION_ATLEAST(2, 24, 0)
	SDL_AudioSpec dev;
	memset(&dev, 0, sizeof(dev));
	if (SDL_GetDefaultAudioInfo(NULL, &dev, 0) < 0) {
		xlog(XLG_SOUND, XLL_INFO, "auto rate: the driver won't say (%s), keeping %i Hz",
			SDL_GetError(), conf.snd.rate);
		return;
	}
	int rate = snd_nearest_rate(dev.freq);
	if (rate != dev.freq)
		xlog(XLG_SOUND, XLL_INFO, "auto rate: device runs at %i Hz, asking for %i",
			dev.freq, rate);
	conf.snd.rate = rate;
#endif
}

int sdlopen() {
	int res;
	SDL_AudioSpec asp;
	SDL_AudioSpec dsp;
	snd_pick_auto_rate();
	asp.freq = conf.snd.rate;
	asp.format = AUDIO_S16LSB;
	asp.channels = conf.snd.chans;
	asp.samples = conf.snd.rate * SND_BLOCK_MS / 1000;
	asp.callback = &sdlPlayAudio;
	asp.userdata = NULL;
	conf.snd.need = 0;
#if defined(HAVESDL2)
	// Nothing is allowed to change: a rate granted behind our back could be one
	// the ring is not sized for, and it would be written to the config as if
	// the user had picked it. snd_pick_auto_rate() above is the one place the
	// rate is chosen.
	sdldevid = SDL_OpenAudioDevice(NULL, 0, &asp, &dsp, 0);
	if (sdldevid == 0) {
#else
	res = SDL_OpenAudio(&asp, &dsp);
	if (res != 0) {
#endif
		xlog(XLG_SOUND, XLL_ERROR, "SDL audio device failed to open: %s", SDL_GetError());
		res = 0;
	} else {
		xlog(XLG_SOUND, XLL_INFO, "SDL audio open: %i Hz, %i samples, format %i (wanted %i)", dsp.freq, dsp.samples, dsp.format, AUDIO_S16LSB);
//		sndChunks = dsp.samples * DISCRATE;
		conf.snd.need = dsp.samples;
		res = 1;			// the device stays paused: sndSync starts it once the ring is full
	}
	memset(sbuf, 0x00, SND_RING_SIZE);
	posp = 0x0004;
	posf = posp;
	return res;
}

void sdlplay() {
#if defined(HAVESDL2)
	SDL_PauseAudioDevice(sdldevid, 0);
#else
	SDL_PauseAudio(0);
#endif
}

void sdlclose() {
#if defined(HAVESDL2)
	SDL_CloseAudioDevice(sdldevid);
#else
	SDL_CloseAudio();
#endif
}

// init

OutSys sndTab[] = {
	{xOutputNone,"NULL",&null_open,NULL,&null_close},
#if defined(HAVESDL1) || defined(HAVESDL2)
	{xOutputSDL,"SDL",&sdlopen,&sdlplay,&sdlclose},
#endif
	{0,NULL,NULL,NULL,NULL}
};

OutSys* findOutSys(const char* name) {
	OutSys* res = NULL;
	int idx = 0;
	while (sndTab[idx].name != NULL) {
		if (strcmp(sndTab[idx].name,name) == 0) {
			res = &sndTab[idx];
			break;
		}
		idx++;
	}
	return res;
}

// modified Bessel function of the first kind, order 0, for the Kaiser window.
// the series converges quickly for the arguments used here (up to beta = 8).
static double bessel_i0(double x) {
	double sum = 1.0;
	double term = 1.0;
	for (int k = 1; k < 64; k++) {
		double t = x / (2 * k);
		term *= t * t;
		sum += term;
		if (term < sum * 1e-17) break;
	}
	return sum;
}

// Kaiser-windowed sinc for the decimator above. The cutoff is 0.9 of the output
// Nyquist, which as a fraction of the oversampled rate (rate * DISCRATE) is
// 0.9 / (2 * DISCRATE) whatever the output rate is - so one set of taps serves
// every rate and nothing has to be rebuilt when the rate changes. What holds that
// ratio at DISCRATE is nsPerSampleFixed, which setOutput() keeps in step with the
// rate: decouple the two and this filter is quietly designed for the wrong band.
// beta = 8 puts
// the stopband around 80 dB down and leaves 16 kHz half a dB short of flat.
static void snd_init_filter() {
	const double fc = 0.9 / (2.0 * DISCRATE);
	const double beta = 8.0;
	const double center = (SND_FIR_TAPS - 1) / 2.0;
	const double i0beta = bessel_i0(beta);
	double h[SND_FIR_TAPS];
	double sum = 0;
	for (int i = 0; i < SND_FIR_TAPS; i++) {
		// SND_FIR_TAPS is even, so center is a half-integer and x never hits 0
		double x = i - center;
		double r = x / center;
		double win = bessel_i0(beta * sqrt(1.0 - r * r)) / i0beta;
		h[i] = sin(2 * M_PI * fc * x) / (M_PI * x) * win;
		sum += h[i];
	}
	// unity gain at DC, so switching the filter on and off does not step the
	// volume. Only the first half is kept: the convolution pairs the window up
	// and uses each tap twice.
	for (int i = 0; i < SND_FIR_TAPS / 2; i++)
		firTaps[i] = (int)llround(h[i] / sum * (1 << SND_FIR_SHIFT));
}

void sndInit() {
	conf.snd.rate = 44100;
	conf.snd.rateauto = 1;
	conf.snd.chans = 2;
	conf.snd.latency = SND_LATENCY_DEF;
	conf.snd.latauto = 1;
	conf.snd.enabled = 1;
	conf.snd.filter = 0;	// measured: no audible gain here, and it is not free
	sndOutput = NULL;
	conf.snd.vol.beep = 100;
	conf.snd.vol.tape = 100;
	conf.snd.vol.ay = 100;
	conf.snd.vol.gs = 100;
	conf.snd.wavout = 0;
	conf.snd.wavfile = NULL;
	initNoise();
	snd_init_filter();
}

// output to wav

wavHead wav_prepare(unsigned int rate, unsigned short chans) {
	wavHead hd;
	memcpy(hd.chunkId, "RIFF", 4);
	hd.chunkSize = 0;
	memcpy(hd.format, "WAVE", 4);
	memcpy(hd.subchunk1Id, "fmt ", 4);
	hd.subchunk1Size = 16;
	hd.audioFormat = 1;
	hd.numChannels = chans;
	hd.sampleRate = rate;
	hd.byteRate = rate * chans * 2;
	hd.blockAlign = chans * 2;
	hd.bitsPerSample = 16;
	memcpy(hd.subchunk2Id, "data", 4);
	hd.subchunk2Size = 0;				// later
	return hd;
}

void snd_wav_close() {
	if (conf.snd.wavfile) {
		int sz = ftell(conf.snd.wavfile);			// file size
		fseek(conf.snd.wavfile, 4, SEEK_SET);
		fputi(sz - 8, conf.snd.wavfile);
		fseek(conf.snd.wavfile, sizeof(wavHead) - 4, SEEK_SET);
		fputi(sz - sizeof(wavHead), conf.snd.wavfile);
		fclose(conf.snd.wavfile);
		conf.snd.wavfile = NULL;
		conf.snd.wavout = 0;
	}
}

int snd_wav_open(const char* path) {
	int res = ERR_OK;
	wavHead hd = wav_prepare(conf.snd.rate, 2);
	snd_wav_close();
	conf.snd.wavfile = fopen(path, "wb");
	if (conf.snd.wavfile) {
		fwrite(&hd, sizeof(wavHead), 1, conf.snd.wavfile);
		conf.snd.wavout = 1;
		wavRate = conf.snd.rate;
	} else {
		res = ERR_CANT_OPEN;
	}
	return res;
}

void snd_wav_write() {
	if (conf.snd.wavfile) {
		// the same S16LE the live output gets, not just the top byte
		fputw(sndLev.left & 0xffff, conf.snd.wavfile);
		fputw(sndLev.right & 0xffff, conf.snd.wavfile);
	}
}

// debug

void sndDebug() {
	xlog(XLG_SOUND, XLL_DEBUG, "ring: %i - %i = %i", posf, posp, posf - posp);
}

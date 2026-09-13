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
// static int disCount = 0;
static sndPair tmpLev = {0, 0};
static sndPair sndLev;

OutSys* findOutSys(const char*);

static long double H[DISCRATE] = {0};

static int sb_pos = 0;
static int sp_pos = 0;
static sndPair smpBuf[128] = {{0,0}};

#if defined(HAVESDL2)
static SDL_AudioDeviceID sdldevid;
#endif

// output

#define USEKIH 0

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
		if (comp->hw->grp == HWG_ZX)
			gsFlush(comp->gs);
//		saaFlush(comp->saa);
		if (!conf.emu.fast && !conf.emu.pause) {
			sndLev = comp->hw->vol(comp, &conf.snd.vol);
			sndLev.left = sndLev.left * conf.snd.vol.master / 100;
			sndLev.right = sndLev.right * conf.snd.vol.master / 100;
			// both ends: FM is signed, and a sample under the low one
			// wraps to the top of the range on the way into the ring
			sndLev.left = toLimits(sndLev.left, -0x8000, 0x7fff);
			sndLev.right = toLimits(sndLev.right, -0x8000, 0x7fff);

			smpBuf[sb_pos & 127] = sndLev;
			sb_pos++;
			if ((sb_pos % DISCRATE) == 0) {
				tmpLev.left = 0;
				tmpLev.right = 0;
#if USEKIH
				sp_pos = sb_pos - DISCRATE;
				for (int i = 0; i < DISCRATE; i++) {
					tmpLev.left += smpBuf[sp_pos & 127].left * H[i];
					tmpLev.right += smpBuf[sp_pos & 127].right * H[i];
					sp_pos++;
				}
#else
				sp_pos = sb_pos - DISCRATE;
				for (int i = 0; i < DISCRATE; i++) {
					tmpLev.left += smpBuf[sp_pos & 127].left;
					tmpLev.right += smpBuf[sp_pos & 127].right;
					sp_pos++;
				}
				tmpLev.left /= DISCRATE;
				tmpLev.right /= DISCRATE;
#endif
				sndLev = tmpLev;
				tmpLev.left = 0;
				tmpLev.right = 0;
//				disCount = 0;

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

void init_kih() {
	long double Fd = conf.snd.rate * DISCRATE;
	long double Fs = 20;
	long double Fx = 40;
	long double H_id [DISCRATE] = {0};
	long double W[DISCRATE] = {0};
	long double Fc = (Fs + Fx) / (2 * Fd);
	int i;
	for (i = 0; i < DISCRATE; i++) {
		if (i == 0) {
			H_id[i] = 2 * M_PI * Fc;
		} else {
			H_id[i] = sinl(2 * M_PI * Fc * i) / (M_PI * i);
		}
		W[i] = 0.42 + 0.5 * cosl((2 * M_PI * i) / (DISCRATE - 1)) + 0.08 * cosl((4 * M_PI * i) / (DISCRATE - 1));
		H[i] = H_id[i] * W[i];
	}
	double sum = 0;
	for (i = 0; i < DISCRATE; i++) sum += H[i];
	for (i = 0; i < DISCRATE; i++) H[i] /= sum;
}

void sndInit() {
	conf.snd.rate = 44100;
	conf.snd.rateauto = 1;
	conf.snd.chans = 2;
	conf.snd.latency = SND_LATENCY_DEF;
	conf.snd.latauto = 1;
	conf.snd.enabled = 1;
	sndOutput = NULL;
	conf.snd.vol.beep = 100;
	conf.snd.vol.tape = 100;
	conf.snd.vol.ay = 100;
	conf.snd.vol.gs = 100;
	conf.snd.wavout = 0;
	conf.snd.wavfile = NULL;
	initNoise();
	init_kih();
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

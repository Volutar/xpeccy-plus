#include <SDL.h>
#include <QElapsedTimer>

#include <atomic>

#include "pacing.h"
#include "sound.h"
#include "xcore.h"

#if USEMUTEX
#include <QMutex>
#include <QWaitCondition>
extern QMutex emutex;
extern QWaitCondition qwc;
#else
extern int sleepy;
#endif

// wake period in ms, independent of the audio buffer size
#define PACE_TICK_MS 2

// biggest backlog we let build up, in ms of sound. after a long stall (window
// drag, profile switch) the emulation should catch up a bit, not sprint
// through everything it missed
#define PACE_MAX_BACKLOG_MS 100

// The ring has to hold the latency ceiling and a full backlog at once, at the
// highest rate offered. The three numbers live in two files and nothing else
// would notice if they stopped fitting.
static_assert(SND_LATENCY_MAX + PACE_MAX_BACKLOG_MS < SND_RING_SIZE * 250 / SND_MAX_RATE,
	"sound ring too small for the latency ceiling plus the pacer backlog");

// how far the pacer may stretch or squeeze emulated time to hold the ring at
// its target. 0.1% covers any crystal mismatch between this clock and the sound
// card's own, and is far too little to hear or to move the frame rate
#define PACE_TRIM_PPM 1000

// VSync: no vblank for this long and the clock takes over again, ns
#define LOCK_STALL_NS	100000000LL
// the most a stall leaves owed when the vblanks come back, in frames
#define LOCK_MAX_FRAMES	2
// how far the lock steers the sound's speed to hold the ring at its target:
// 0.5 % one target away, a pitch change nobody hears. It only has to take up
// what the display's clock and the sound card's disagree by, and what the rate
// asked of the system is off by.
#define LOCK_TRIM_PPM	5000
// the ring's fill is smoothed over about this many vblanks before it steers:
// the card takes it out a block at a time, and steering on every block jitters
#define LOCK_TRIM_AVG	64

static std::atomic<int> lockN{0};
static std::atomic<double> lockSpeed{1.0};
static std::atomic<long long> lockLastNs{-1};	// the last vblank
static double lockOwedNs = 0;		// emulated time handed out, not yet a sample; vblank thread
static double lockErr = 0;		// the ring's smoothed shortfall, bytes; vblank thread
static std::atomic<int> lockReset{0};	// the vblank thread starts its sums again

static SDL_TimerID paceTimerId = 0;
static QElapsedTimer paceClock;
static qint64 paceLastNs = 0;
static qint64 paceRemainderNs = 0;

// The sound card runs off its own crystal, this clock off the host's, and the
// two never quite agree - left alone the ring slowly empties (a click) or fills
// (added lag). Pull it back towards the target: make time run a touch fast when
// the ring is running dry, a touch slow when it is over full. Nothing plays the
// ring under the NULL device, so there is nothing to steer against there.
static qint64 pace_trim(qint64 dtNs) {
	if (!sndPlaybackActive()) return dtNs;
	int target = sndGetRingTargetBytes();
	if (target < 1) return dtNs;
	int err = target - sndGetRingDistance();			// > 0: ring is running dry
	int ppm = toLimits(err * PACE_TRIM_PPM / target, -PACE_TRIM_PPM, PACE_TRIM_PPM);	// full trim one target away
	return dtNs + dtNs * ppm / 1000000;
}

static void pace_wake() {
#if USEMUTEX
	qwc.wakeAll();
#else
	sleepy = 0;
#endif
}

bool pacing_lock_stalled() {
	long long last = lockLastNs;
	return (last < 0) || (paceClock.nsecsElapsed() - last > LOCK_STALL_NS);
}

static Uint32 pace_tick(Uint32 interval, void*) {
	qint64 nowNs = paceClock.nsecsElapsed();
	qint64 dtNs = nowNs - paceLastNs + paceRemainderNs;
	paceLastNs = nowNs;

	if (lockN && !pacing_lock_stalled()) {
		// the vblanks hand the time out (pacing_vblank)
		paceRemainderNs = 0;
	} else if (!conf.emu.fast && !conf.emu.pause) {
		dtNs = pace_trim(dtNs);
		// samples for the elapsed time; keep the remainder to avoid drift
		qint64 samples = (dtNs * conf.snd.rate) / 1000000000LL;
		paceRemainderNs = dtNs - (samples * 1000000000LL) / conf.snd.rate;
		// the ring buffer is only drained by a device that plays it. the NULL
		// device never does, so waiting for it to drain would stop the
		// emulation for good - skip the check when nothing is playing.
		int wait = sndPlaybackActive() && (sndGetRingDistance() >= SND_RING_HIGH_WATER);
		if ((samples > 0) && !wait) {
			int maxNeed = conf.snd.rate * PACE_MAX_BACKLOG_MS / 1000;
			conf.snd.need += (int)samples;
			if (conf.snd.need > maxNeed)
				conf.snd.need = maxNeed;
		}
	} else {
		conf.snd.need = 0;
		paceRemainderNs = 0;
	}
	sndAutoTick(nowNs);
	pace_wake();
	return interval;
}

void pacing_lock(int n, double speed) {
	if (n < 0) n = 0;
	if ((n == lockN) && (speed == lockSpeed)) return;
	if (n != lockN) lockReset = 1;
	lockSpeed = speed;
	lockN = n;
	// at once, not on the next vblank: the frame made meanwhile is at the new speed
	sndSetLockScale(n ? speed : 1.0);
}

int pacing_locked() {
	return lockN;
}

double pacing_lock_speed() {
	return lockN ? lockSpeed.load() : 1.0;
}

// One vblank: 1/n of a frame of emulated time, as samples to make. The time is
// kept in emulated ns, not samples: the steer below changes how much one sample
// stands for, and the frame's end has to stay where it is against the vblank.
void pacing_vblank(long long ns) {
	int n = lockN;
	if (!n) return;
	long long before = lockLastNs.exchange(ns);
	// back from a stall the clock has paid for the time meanwhile
	if (lockReset.exchange(0) || (before < 0) || (ns - before > LOCK_STALL_NS)) {
		lockOwedNs = 0;
		lockErr = 0;
	}
	Computer* comp = conf.zx;
	if (!comp || conf.emu.fast || conf.emu.pause) {
		lockOwedNs = 0;
		return;
	}
	double speed = lockSpeed;
	if (sndPlaybackActive()) {
		int target = sndGetRingTargetBytes();
		if (target > 0) {
			lockErr += ((target - sndGetRingDistance()) - lockErr) / LOCK_TRIM_AVG;
			double e = lockErr / target;
			if (e > 1.0) e = 1.0;
			if (e < -1.0) e = -1.0;
			double trim = e * LOCK_TRIM_PPM / 1e6;
			static long long told = -1;
			if (xlog_on(XLG_SOUND, XLL_DEBUG) && ((told < 0) || (ns - told >= 10000000000LL))) {
				told = ns;
				xlog(XLG_SOUND, XLL_DEBUG, "vsync: the ring at %i of %i bytes, sound steered %+.0f ppm",
					sndGetRingDistance(), target, -trim * 1e6);
			}
			// running dry: each sample spans less emulated time, so more of them
			// are made for the same frame
			sndSetLockScale(speed * (1.0 - trim));
			// let go meanwhile (pacing_lock): its 1.0 is not to be lost under ours
			if (!lockN) sndSetLockScale(1.0);
		}
	}
	lockOwedNs += (double)comp->vid->nsPerFrame / n;
	double per = sndNsPerOutSample();
	if (per <= 0) return;
	int samples = (int)(lockOwedNs / per);
	lockOwedNs -= samples * per;
	// nothing plays what is made past the high water: no more of it
	if (sndPlaybackActive() && (sndGetRingDistance() >= SND_RING_HIGH_WATER)) return;
	int maxNeed = (int)(LOCK_MAX_FRAMES * comp->vid->nsPerFrame / per);
	conf.snd.need += samples;
	if (conf.snd.need > maxNeed) conf.snd.need = maxNeed;
	pace_wake();
}

long long paceClockNs() {
	return paceClock.nsecsElapsed();
}

void pacingInit() {
	paceClock.start();
	xlog_set_clock(paceClockNs);	// only now does it read anything sensible
	paceLastNs = paceClock.nsecsElapsed();
	paceRemainderNs = 0;
	paceTimerId = SDL_AddTimer(PACE_TICK_MS, pace_tick, NULL);
}

void pacingClose() {
	if (paceTimerId) {
		SDL_RemoveTimer(paceTimerId);
		paceTimerId = 0;
	}
}

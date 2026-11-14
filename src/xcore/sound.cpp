#include <stdio.h>

#define _USE_MATH_DEFINES
#include <cmath>

#include "sound.h"
#include "vidrec.h"
#include "xcore.h"
#include "rewind.h"
#include "../libxpeccy/xstate.h"

#include <atomic>
#include <climits>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
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

// The scope can take the offset out for itself, which is the Fit switch on the
// panel: without it the trace floats wherever the level puts it and the lower
// half of the box is dead. With the sound's own dc filter on it is a second
// pass over an already centred signal, which costs nothing and does nothing.
// The master volume is deliberately not applied either, or turning the wheel
// down would flatten the picture along with the sound.
static sndDC scopeDc;

static void scope_put(int lev) {
	sndPair p;
	p.left = lev;
	p.right = lev;
	p = snd_clip16(snd_dc(&scopeDc, p, conf.dbg.sndfit));
	scopeBuf[scopePos & SND_SCOPE_MASK] = (short)p.left;
	scopePos++;
}

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

// Slow motion is a shorter sample period in emulated time, not a smaller budget
// from the pacer: the machine then advances slower while the card still gets all
// the samples it asks for, so the ring keeps up and the sound drops in pitch the
// way a tape slowed down does. Starving the ring instead only made it crackle.
// VSync's scale goes the same way: a machine pulled up to the display's rate
// spans more emulated time per sample, and its sound rises by as much.
static std::atomic<double> sndLockScale{1.0};

void sndUpdateSpeed(void) {
	double s = (conf.emu.speed > 0.0) ? conf.emu.speed : 1.0;
	s *= sndLockScale.load();
	nsPerSampleFixed = (long long)(ns_per_sample_fixed(conf.snd.rate) * s);
	ay_set_speed(s);
}

void sndSetLockScale(double x) {
	sndLockScale = (x > 0.0) ? x : 1.0;
	sndUpdateSpeed();
}

double sndNsPerOutSample() {
	return (double)nsPerSampleFixed * DISCRATE / NS_FIXED_ONE;
}

static int wavRate = 0;			// rate the open recording's header says

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
// The taps are fixed point (Q.SND_FIR_SHIFT), so the result is the same on every
// host and build; snd_init_filter() builds them once at startup, in double.
// Measured, the filter costs about 1.5% of one core at 48 kHz against 0.08%
// for the average.
#define SND_FIR_TAPS	512
#define SND_FIR_MASK	(SND_FIR_TAPS - 1)
#define SND_FIR_SHIFT	24
static int firTaps[SND_FIR_TAPS / 2] = {0};	// symmetric: half is enough

// The window is held twice over, so the last SND_FIR_TAPS samples are always
// one straight stretch of memory whatever the write position: the filter loop
// then has no wrapping in it and the compiler can take it apart.
static int sb_pos = 0;
static sndPair smpBuf[SND_FIR_TAPS * 2] = {{0,0}};

// The sound thread. The emulation thread mixes the machine (hw->vol) at every
// sub-sample and queues the level; everything after the mix - the scope, the
// decimation, the wav, the ring - runs here, on a core of its own. The queue is
// one producer, one consumer: the emulation thread, or whoever holds emu_lock.
//
// What does not wait for the queue stays with the producer: the pacer's count
// (conf.snd.need, snd_need_room). Rewind's sound is made here too; a snapshot
// marks its place in it with a call in the queue (snd_q_call). Those who read what the
// sound thread makes - the bench's hash, the video recording, the scope of a
// held machine, the wav - wait for it to catch up first (snd_sync_wait).
typedef struct {
	int l, r;		// the mix as hw->vol gave it; with the mirror on, adv and late
} sndSub;

#define SND_Q_SIZE	0x4000		// sub-samples, about 12 ms at 44100
#define SND_Q_MASK	(SND_Q_SIZE - 1)
#define SND_Q_BATCH	1024		// handed over at least this often
// ...and the sound thread woken this often, or at the cycle's end. A wake is a
// call into the system on the emulation thread: once a batch, a machine run flat
// out spent a tenth of its time waking it
#define SND_Q_WAKE	4096
static sndSub sndQ[SND_Q_SIZE];
// Each side's counters on cache lines of their own. A line one thread writes at
// every sub-sample and the other reads goes back and forth between the cores
// each time: next to what the sound thread reads, the producer's count cost the
// emulation thread a quarter of its time. Hence one block for the producer, its
// size a whole number of lines, and the consumer's counters apart; the producer
// reads the consumer's only when its own copy says the queue is full.
static struct alignas(64) {
	long long put;		// queued so far
	long long pub;		// ...handed over so far
	long long tailSeen;	// ...and taken, as it last looked
	long long woken;	// ...and where the sound thread was last woken
	long long dropAt;	// where the last drop went in (snd_drop)
	long long rwNsSent;	// the time a sub-sample stands for, as rewind was last told
	int subPos;		// sub-samples made since the start: sb_pos as the sound thread will have it
	int smpCount;
	long long quiet;	// sub-samples of silence (Enabled off) not handed over yet
	long long advSent;	// the devices' time handed over to the copy (snd_mirror_hold)
} sp = {0, 0, 0, 0, -1, -1, 0, 0, 0, 0};
alignas(64) static std::atomic<long long> sqHead(0);	// what the consumer may take
alignas(64) static std::atomic<long long> sqTail(0);	// what it has taken

// What has to happen between two sub-samples, in order with them: `at` is the
// count of sub-samples queued before it.
// The mirror's (see "the machine's mix on the sound thread" below) carry adv, the
// devices' time since the item before them.
// SND_T_QUIET: val sub-samples of silence, which were not made at all
enum {SND_T_DROP, SND_T_RESET, SND_T_PUT, SND_T_NS, SND_T_CALL,
	SND_T_SEED, SND_T_UNMIRROR, SND_T_ADV, SND_T_POKE, SND_T_RETUNE, SND_T_IN, SND_T_QUIET};
typedef struct {
	long long at;
	int kind;
	sndPair lev;		// SND_T_PUT
	long long val;		// SND_T_NS, SND_T_CALL's argument
	void (*fn)(long long);	// SND_T_CALL
	long long adv;		// the mirror's
	int chip, what, a, b;	// SND_T_POKE: chip 0..2, AYL_ and its two; SND_T_RETUNE: chip, type in a
	double frq;		// SND_T_RETUNE
	sndMixIn in;		// SND_T_IN
	void* blob;		// SND_T_SEED: a sndSeedCopy, the sound thread's to free
} sndTok;

#define SND_T_SIZE	0x2000
#define SND_T_MASK	(SND_T_SIZE - 1)
static sndTok sndT[SND_T_SIZE];
alignas(64) static std::atomic<long long> stHead(0);
alignas(64) static std::atomic<long long> stTail(0);

// Never destroyed: a std::thread still running when the statics go would end the
// process with terminate() on any way out that does not pass sndClose()
static std::thread* sndThread = NULL;
static std::atomic<int> sndQuit(0);
static std::atomic<int> sndSleeping(0);
static std::mutex sndWakeLock;
static std::condition_variable sndWake;
// held while the sound thread works: the output, the ring and the wav are not
// to change under it (setOutput, snd_wav_open/close). Recursive: setOutput
// falls back to NULL through itself.
static std::recursive_mutex sndWork;

static SDL_AudioDeviceID sdldevid;

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

// What is queued is dropped where the machine's sound stops following on from
// it: after fast mode, a rewind, a load or a reset. Only the callback moves the
// play position, so the emulation marks where the new sound starts, at the
// first sample made after snd_drop(), and the callback jumps there. It then
// holds until the ring is back at its target - played at once, the ring would
// sit a block from running dry from then on - and fades from the frame it held
// into the new sound. A pause drops nothing: the machine stood still, and what
// is queued is just what comes next.
static int sndDropWant = 0;			// the sound thread's (snd_drop queues it)
static std::atomic<int> sndDropMark(-1);	// a ring position, -1 for none
static std::atomic<int> sndDropBytes(0);	// dropped, for the pacer's tick to log
static int sndRefill = 0;	// the callback is waiting for the ring to fill
static int sndFade = 0;		// frames of the fade still to go
static short sndOutL = 0, sndOutR = 0;	// the last frame the callback sent
static short sndFadeL = 0, sndFadeR = 0;	// ...and the one the fade starts from

#define SND_FADE_MS	SND_BLOCK_MS

// --- the producer's side (the emulation thread, or under emu_lock)


static void snd_kick() {
	if (sndSleeping.load()) {
		std::lock_guard<std::mutex> lk(sndWakeLock);
		sndWake.notify_one();
	}
}

// what is queued becomes the sound thread's to take; an awake one takes it at
// once, a sleeping one at the next kick
static void snd_q_publish() {
	if (sp.pub == sp.put) return;
	sp.pub = sp.put;
	sqHead.store(sp.put);
}


// A token goes in after what is queued: taken (snd_tok_new), filled, handed over
// (snd_tok_put). The sound thread is not woken for it - the cycle's end does that
// (snd_q_flush): rewind puts one in for every sample it plays
static void snd_quiet_out();

static sndTok* snd_tok_new(int kind) {
	if (sp.quiet) snd_quiet_out();		// the silence before it goes first
	snd_q_publish();
	long long h = stHead.load(std::memory_order_relaxed);
	while ((h - stTail.load(std::memory_order_acquire) >= SND_T_SIZE) && !sndQuit.load()) {
		snd_kick();
		std::this_thread::yield();
	}
	sndTok* t = &sndT[h & SND_T_MASK];
	t->at = sp.put;
	t->kind = kind;
	t->adv = 0;
	return t;
}

static void snd_tok_put() {
	long long h = stHead.load(std::memory_order_relaxed);
	int kind = sndT[h & SND_T_MASK].kind;
	stHead.store(h + 1);
	// a drop queued before still says it all, unless this one puts sound in the ring
	if ((kind == SND_T_PUT) || (kind == SND_T_QUIET)) sp.dropAt = -1;
}

static void snd_tok(int kind, sndPair lev = sndPair(), long long val = 0, void (*fn)(long long) = NULL) {
	sndTok* t = snd_tok_new(kind);
	t->lev = lev;
	t->val = val;
	t->fn = fn;
	snd_tok_put();
}

// Fast mode asks for a drop on every step: one already queued at this point, with
// nothing put into the ring after it, says it all
void snd_drop() {
	if ((sp.dropAt == sp.put) && !sp.quiet) return;
	snd_tok(SND_T_DROP);
	sp.dropAt = sp.put;
}

// the silence counted so far, handed over as one token
static void snd_quiet_out() {
	long long n = sp.quiet;
	sp.quiet = 0;
	sndTok* t = snd_tok_new(SND_T_QUIET);
	t->val = n;
	snd_tok_put();
}

static void snd_q_push(int l, int r) {
	if (sp.quiet) snd_quiet_out();
	while (sp.put - sp.tailSeen >= SND_Q_SIZE) {
		sp.tailSeen = sqTail.load(std::memory_order_acquire);
		if ((sp.put - sp.tailSeen < SND_Q_SIZE) || sndQuit.load()) break;
		snd_q_publish();
		snd_kick();
		std::this_thread::yield();
	}
	sndSub* q = &sndQ[sp.put & SND_Q_MASK];
	q->l = l;
	q->r = r;
	sp.put++;
	if (sp.put - sp.pub >= SND_Q_BATCH) {
		snd_q_publish();
		if (sp.put - sp.woken >= SND_Q_WAKE) {
			sp.woken = sp.put;
			snd_kick();
		}
	}
}

void snd_q_call(void (*fn)(long long), long long arg) {
	snd_tok(SND_T_CALL, sndPair(), arg, fn);
}

// --- the machine's mix on the sound thread (the mirror)
//
// A machine whose sound is split (hw->sndtake, hw->sndmix) is not mixed here: at
// every sub-sample it does what is its own (the Next: the copper, the AYs' clock, the
// DACs' queue) and gathers what its mix hears; the sound thread holds a copy of its
// chips and mixes. The copy follows the machine's: its time (TSound.advNs, handed
// over with every item), the registers written (ay_log), the tuning; what the mix
// hears goes over when it changed. A state put back or replaced (Computer.sndSeed)
// is copied over whole. Run-ahead's frames send nothing: they are thrown away, and
// TSound.advNs goes back with the state. A YM2203's fm half keeps its state outside
// the struct: it goes over packed, and the copy's chips have cores of their own.
//
// The fm is made here only. The machine's core runs its timers and nothing else
// (ym2203_lite: the status byte is all the cpu sees of it) and is marked stale; the
// copy's is the chip's state. It is taken back (snd_fm_back) where something keeps
// the machine's: a save (xstate_fm_back), a seed of the same machine (options, a
// mute). Rewind keeps the copy's beside each snapshot instead (snd_fm_capture), as a
// wait at every one would put the two threads back in step.

// the copy to start from
typedef struct {
	TSound ts;
	aymChip chip[4];
	std::vector<unsigned char> fm[4];	// a YM2203's core, packed; empty: the chip has none
	long long extra[4];	// time on top of the chip's own: one taken from a capture
	sndPair (*mix)(sndMix*, sndVolume*);
} sndSeedCopy;

// the copy's YM2203s at a point, for a seed to start from later (rewind)
typedef struct {
	int has[4];
	aymChip chip[4];
	std::vector<unsigned char> fm[4];
} sndFmCap;

// the producer's
static struct alignas(64) {
	Computer* comp;		// the machine the sound thread holds a copy of; NULL: none
	sndMix take;		// what its take gathered at this sub-sample
	sndMixIn sent;		// ...and what the sound thread was told last
} mp;

// the sound thread's
static struct alignas(64) {
	int on;
	sndMix m;
	TSound ts;
	aymChip chip[4];
	sndPair (*mix)(sndMix*, sndVolume*);
	long long advDone;	// the devices' time worked through, sndAdvDone when published
} sm;

// Can the sound thread mix this machine itself. The SAA's and the GS's levels go
// over when they move (sndMixIn); the GS's own cpu runs here, at the sample
static int snd_mirror_fits(Computer* comp) {
	if (!comp->hw->sndtake || !comp->hw->sndmix) return 0;
	return 1;
}

// Fast mode can outrun the copy's fm: the time handed over (sp.advSent) and the time
// the copy has worked through, so the machine waits while the copy is too far behind
// (snd_mirror_hold). The one moves at every sub-sample, the other once a pass of the
// sound thread's (snd_work): each on a line of its own
static struct alignas(64) {
	std::atomic<long long> v;
} sndAdvDone = {{0}};

// the devices' time handed over with an item
static long long snd_mirror_adv(Computer* comp) {
	long long adv = comp->ts->advNs;
	comp->ts->advNs = 0;
	sp.advSent += adv;
	return adv;
}

// ym2203_lite: the machine's chip, while the copy follows it. Not while a seed waits
// to copy a core that is the chip's own (a load, a reset): that one stays whole
static int snd_fm_lite(aymChip* chip) {
	if ((chip >= &sm.chip[0]) && (chip < &sm.chip[4])) return 0;	// the copy's own
	Computer* comp = mp.comp;
	if (!comp || (comp != conf.zx)) return 0;
	int i = 0;
	while ((i < 4) && (ts_chip(comp->ts, i) != chip)) i++;
	if (i > 3) return 0;
	return !comp->sndSeed || chip->fm_stale;
}

// The time goes to the chips in pieces a chip's int count takes: it settles only past
// 1 << 30 (ay_sync_ns), so a piece of that size on top of one would wrap it. Where the
// pieces end is where the chips flush, so the hashes hold this size
#define SND_TIME_PIECE	(1 << 29)

static void snd_chip_add_time(aymChip* chip, long long ns) {
	while (ns > 0) {
		int k = (ns > SND_TIME_PIECE) ? SND_TIME_PIECE : (int)ns;
		ay_sync_ns(chip, k);
		ns -= k;
	}
}

static void snd_fm_pack(aymChip* chip, std::vector<unsigned char>& out) {
	void* p;
	ym2203_state_pack(chip);
	int n = ym2203_state_size(chip, &p);
	out.assign((unsigned char*)p, (unsigned char*)p + n);
}

// A packed core into a chip whose struct came with it (was), and the time it had not
// been handed yet on top. A core made new sets the status bits: back as they came
static void snd_fm_load(aymChip* chip, const aymChip* was, const std::vector<unsigned char>& fm, long long extra) {
	ym2203_state_load(chip, fm.data(), (int)fm.size());
	chip->reg[0xff] = was->reg[0xff];
	chip->wait = was->wait;
	chip->fm_stale = 0;
	snd_chip_add_time(chip, extra);
}

// The copy's fm core (src, packed in fm) into the machine's chip, nothing else of the
// copy's: what the gui sets on the machine's chip may not have reached the copy yet.
// Carried on in a chip of its own by the time the copy was not handed (TSound.advNs),
// the machine's counted up to the same point: different pieces, the same sum
static void snd_fm_adopt(Computer* comp, aymChip* dst, const aymChip* src, const std::vector<unsigned char>& fm) {
	aymChip tmp = *src;
	tmp.fm = NULL;
	tmp.xrd = NULL;
	tmp.xwr = NULL;
	tmp.xptr = NULL;
	snd_fm_load(&tmp, src, fm, comp->ts->advNs);
	ay_flush(&tmp);
	ay_flush(dst);
	std::vector<unsigned char> now;
	snd_fm_pack(&tmp, now);
	ym2203_free(&tmp);
	ym2203_state_load(dst, now.data(), (int)now.size());
	dst->fm_stale = 0;
}

// what the gui sets on a chip, from the machine's onto a copy of it
static void snd_chip_user_from(aymChip* dst, const aymChip* src) {
	dst->stereo = src->stereo;
	dst->sep = src->sep;
	memcpy(dst->fm_off, src->fm_off, sizeof(src->fm_off));
	dst->chanA.mute = src->chanA.mute;
	dst->chanB.mute = src->chanB.mute;
	dst->chanC.mute = src->chanC.mute;
	dst->chanN.mute = src->chanN.mute;
	dst->chanE.mute = src->chanE.mute;
}

// xstate_fm_back, and before a seed of the same machine: a stale fm back from the
// copy, once it has taken everything handed over. Emulation thread, or under emu_lock
static void snd_fm_back(Computer* comp) {
	if (!sndThread || !comp || (comp != mp.comp)) return;
	int any = 0;
	for (int i = 0; i < 4; i++) {
		aymChip* chip = ts_chip(comp->ts, i);
		if (chip && chip->fm && chip->fm_stale) any = 1;
	}
	if (!any) return;
	snd_q_flush();
	snd_sync_wait();
	std::lock_guard<std::recursive_mutex> lk(sndWork);
	if (!sm.on) return;
	for (int i = 0; i < 4; i++) {
		aymChip* chip = ts_chip(comp->ts, i);
		if (!chip || !chip->fm || !chip->fm_stale || !sm.chip[i].fm) continue;
		std::vector<unsigned char> fm;
		snd_fm_pack(&sm.chip[i], fm);
		snd_fm_adopt(comp, chip, &sm.chip[i], fm);
		xlog(XLG_SOUND, XLL_DEBUG, "chip %i: fm taken back from the sound thread", i);
	}
}

static void snd_mirror_seed(Computer* comp, const sndFmCap* cap = NULL) {
	TSound* ts = comp->ts;
	if (!cap) snd_fm_back(comp);		// options, a mute: the copy has followed it
	sndSeedCopy* c = new sndSeedCopy;
	c->ts = *ts;
	for (int i = 0; i < 4; i++) {
		aymChip* chip = ts_chip(ts, i);
		// as it stands, its time not yet counted left in it: the samples still to
		// come in this step were due before its end (late), and they count up to
		// there, not to the end
		c->chip[i] = *chip;
		c->extra[i] = 0;
		if (!chip->fm) continue;
		if (cap && cap->has[i] && chip->fm_stale) {
			// the copy's as it was there, and the time it had not been handed yet;
			// what the gui sets, as the machine's has it
			c->chip[i] = cap->chip[i];
			snd_chip_user_from(&c->chip[i], chip);
			c->fm[i] = cap->fm[i];
			c->extra[i] = ts->advNs;
			continue;
		}
		if (chip->fm_stale)
			xlog(XLG_SOUND, XLL_WARN, "sound: chip %i seeded with an fm half that only ran its timers", i);
		// and its fm core as it stands with it: at that same last flush
		snd_fm_pack(chip, c->fm[i]);
	}
	c->mix = comp->hw->sndmix;
	ts->advNs = 0;
	sndTok* t = snd_tok_new(SND_T_SEED);
	t->blob = c;
	snd_tok_put();
	memset(&mp.sent, 0xff, sizeof(mp.sent));	// what the mix hears goes over at the sample
	mp.comp = comp;
	comp->sndSeed = 0;
}

static void snd_mirror_off() {
	mp.comp = NULL;
	snd_tok(SND_T_UNMIRROR);
}

// a chip's register written (ay_poke_reg), its envelope counter set (the Next), its
// type or clock set (chip_set_type: the machine's clock moved, the Next's AYs retuned)
static void snd_ay_log(aymChip* chip, int what, int a, int b) {
	if ((chip >= &sm.chip[0]) && (chip < &sm.chip[4])) return;	// the copy's own, on the sound thread
	Computer* comp = mp.comp;
	if (!comp || (comp != conf.zx) || x_runahead) return;	// not the machine there is a copy of
	int i;
	for (i = 0; (i < 3) && (ts_chip(comp->ts, i) != chip); i++);
	if (i > 2) return;
	if (what == AYL_TYPE) {
		sndTok* t = snd_tok_new(SND_T_RETUNE);
		t->adv = snd_mirror_adv(comp);
		t->chip = i;
		t->a = chip->type;
		t->frq = chip->frq;
		snd_tok_put();
		return;
	}
	sndTok* t = snd_tok_new(SND_T_POKE);
	t->adv = snd_mirror_adv(comp);
	t->chip = i;
	t->what = what;
	t->a = a;
	t->b = b;
	snd_tok_put();
}

// Where no sub-samples carry the time (fast mode, sound off), it goes over in pieces, so
// the copy works its chips through it alongside and not all at once after (the ring dry)
#define SND_ADV_SLICE	(1LL << 24)	// ns, about 17 ms
// ...and the machine waits while the copy is more than this of its time behind. Bounded,
// not small: a chip keeps up to 2^30 ns unworked (pendNs), so what is left when the sound
// comes back, or a save waits for, is up to a second of fm per chip - tens of ms of work
#define SND_ADV_BEHIND	(1LL << 28)	// ns, about a quarter of a second

static void snd_mirror_hold() {
	if (sp.advSent - sndAdvDone.v.load(std::memory_order_acquire) <= SND_ADV_BEHIND) return;
	snd_q_flush();
	while ((sp.advSent - sndAdvDone.v.load() > SND_ADV_BEHIND / 2) && !sndQuit.load()) {
		if ((stTail.load() == stHead.load()) && (sqTail.load() == sqHead.load())) {
			sp.advSent = sndAdvDone.v.load();		// all taken: the two counts agree
			break;
		}
		snd_kick();
		std::this_thread::yield();
	}
}

void snd_mirror_keep_up(Computer* comp) {
	if (mp.comp != comp) return;
	// a state put back (a load, a reset) goes over now, not after the copy has worked
	// through the old one's time - which the machine would wait for
	if (comp->sndSeed && snd_mirror_fits(comp)) snd_mirror_seed(comp);
	if (comp->ts->advNs >= SND_ADV_SLICE) {
		sndTok* t = snd_tok_new(SND_T_ADV);
		t->adv = snd_mirror_adv(comp);
		snd_tok_put();
	}
	snd_mirror_hold();		// the registers written carry time too
}

// one sub-sample of a mirrored machine; scope: the debugger's step, heard by the scope only
static void snd_mirror_sample(Computer* comp, int scope) {
	if ((mp.comp != comp) || comp->sndSeed) snd_mirror_seed(comp);
	comp->hw->sndtake(comp, &mp.take);
	if (memcmp(&mp.take.in, &mp.sent, sizeof(mp.sent))) {
		memcpy(&mp.sent, &mp.take.in, sizeof(mp.sent));
		sndTok* t = snd_tok_new(SND_T_IN);
		memcpy(&t->in, &mp.take.in, sizeof(t->in));
		snd_tok_put();
	}
	long long adv = snd_mirror_adv(comp);
	if (adv > INT_MAX) {			// fast mode ran on: a time too long for an item
		sndTok* t = snd_tok_new(SND_T_ADV);
		t->adv = adv;
		snd_tok_put();
		adv = 0;
	}
	int late = mp.take.late;
	snd_q_push((int)adv, scope ? (-1 - late) : late);
}

// --- the sound thread's side of it

// the time an item carries, to the copy's chips (SND_TIME_PIECE)
static void snd_mirror_time(long long ns) {
	if (ns <= 0) return;
	sm.advDone += ns;
	while (ns > 0) {
		int k = (ns > SND_TIME_PIECE) ? SND_TIME_PIECE : (int)ns;
		tsSync(&sm.ts, k);
		ns -= k;
	}
}

static void snd_mirror_take_seed(sndSeedCopy* c) {
	sm.ts = c->ts;
	aymChip* was[4] = {c->ts.chipA, c->ts.chipB, c->ts.chipC, c->ts.chipD};
	for (int i = 0; i < 4; i++) {
		void* own = sm.chip[i].fm;		// the copy's core, never the machine's
		sm.chip[i] = c->chip[i];
		sm.chip[i].fm = own;
		// its ports are the machine's: a write here goes nowhere
		sm.chip[i].xrd = NULL;
		sm.chip[i].xwr = NULL;
		sm.chip[i].xptr = NULL;
		if (c->fm[i].empty()) {
			ym2203_free(&sm.chip[i]);
		} else {
			snd_fm_load(&sm.chip[i], &c->chip[i], c->fm[i], c->extra[i]);
		}
	}
	sm.ts.chipA = &sm.chip[0];
	sm.ts.chipB = &sm.chip[1];
	sm.ts.chipC = &sm.chip[2];
	sm.ts.chipD = &sm.chip[3];
	sm.ts.curChip = &sm.chip[0];
	for (int i = 0; i < 4; i++)
		if (c->ts.curChip == was[i]) sm.ts.curChip = &sm.chip[i];
	sm.ts.advNs = 0;
	sm.m.ts = &sm.ts;
	sm.mix = c->mix;
	sm.on = 1;
	delete c;
}

// the debugger's view of a chip: the copy the sound thread mixes, when there is one
aymChip* snd_heard_chip(Computer* comp, int i) {
	if (sm.on && (mp.comp == comp) && (i >= 0) && (i < 4)) return &sm.chip[i];
	return ts_chip(comp->ts, i);
}

void snd_fm_view(Computer* comp, aymChip* chip, fmChan* out) {
	int i = 0;
	while ((i < 4) && (ts_chip(comp->ts, i) != chip)) i++;
	if ((i < 4) && sndThread && (mp.comp == comp)) {
		// the sound thread works it under this lock, and nothing else does
		std::lock_guard<std::recursive_mutex> lk(sndWork);
		if (sm.on && sm.chip[i].fm) {
			ym2203_fm_view(&sm.chip[i], out);
			return;
		}
	}
	ym2203_fm_view(chip, out);
}

// sound thread: rewind's mark of a snapshot
void* snd_fm_capture() {
	if (!sm.on) return NULL;
	sndFmCap* c = NULL;
	for (int i = 0; i < 4; i++) {
		// a chip made a YM2203 has no core before its first flush, which fast mode
		// puts off for a second: the machine's may have one by then, stale
		if (!sm.chip[i].fm && (sm.chip[i].type == SND_YM2203)) ym2203_make(&sm.chip[i]);
		if (!sm.chip[i].fm) continue;
		if (!c) c = new sndFmCap();
		c->has[i] = 1;
		c->chip[i] = sm.chip[i];
		snd_fm_pack(&sm.chip[i], c->fm[i]);
	}
	return c;
}

void snd_fm_capture_free(void* p) {
	delete (sndFmCap*)p;
}

size_t snd_fm_capture_bytes(const void* p) {
	const sndFmCap* c = (const sndFmCap*)p;
	if (!c) return 0;
	size_t n = sizeof(sndFmCap);
	for (int i = 0; i < 4; i++) n += c->fm[i].capacity();
	return n;
}

// Rewind put a snapshot back. A machine not mirrored, or another one, is seeded at
// its next sample as it stands
void snd_fm_reseed(Computer* comp, const void* cap) {
	if (!sndThread || !cap || (mp.comp != comp) || !snd_mirror_fits(comp)) return;
	snd_mirror_seed(comp, (const sndFmCap*)cap);
}

// Rewind shows a snapshot: the machine's stale fm made whole from the capture, so the
// frame it runs - and the machine, should it go on from there - has the chip's own
void snd_fm_restore(Computer* comp, const void* cap) {
	const sndFmCap* c = (const sndFmCap*)cap;
	if (!c) return;
	for (int i = 0; i < 4; i++) {
		aymChip* chip = ts_chip(comp->ts, i);
		if (chip && chip->fm && chip->fm_stale && c->has[i])
			snd_fm_adopt(comp, chip, &c->chip[i], c->fm[i]);
	}
}

// the end of an emulation cycle: what it made goes to the sound thread
void snd_q_flush() {
	if (sp.quiet) snd_quiet_out();
	snd_q_publish();
	if ((sp.woken != sp.put) || (stTail.load(std::memory_order_relaxed) != stHead.load(std::memory_order_relaxed))) {
		sp.woken = sp.put;
		snd_kick();
	}
}

// Until the sound thread has taken what was handed over so far. Any thread; the
// producer flushes first if what it has just made is to be waited for.
void snd_sync_wait() {
	if (!sndThread) return;
	long long h = sqHead.load();
	long long t = stHead.load();
	while (((sqTail.load() < h) || (stTail.load() < t)) && !sndQuit.load()) {
		snd_kick();
		std::this_thread::yield();
	}
}

// handed over and not yet made into samples: the pacer counts it as in the ring
int snd_queued_bytes() {
	long long n = sqHead.load(std::memory_order_relaxed) - sqTail.load(std::memory_order_relaxed);
	return (n > 0) ? (int)(n / DISCRATE * 4) : 0;
}

// --- the sound thread's side

static void snd_start_playback();

static void snd_ring_put(sndPair lev) {
	if (sndDropWant) {
		sndDropWant = 0;
		sndDropMark.store(posf & SND_RING_MASK);
	}
	sbuf[posf & SND_RING_MASK] = lev.left & 0xff;
	posf++;
	sbuf[posf & SND_RING_MASK] = (lev.left >> 8) & 0xff;
	posf++;
	sbuf[posf & SND_RING_MASK] = lev.right & 0xff;
	posf++;
	sbuf[posf & SND_RING_MASK] = (lev.right >> 8) & 0xff;
	posf++;
	vrec_sample(lev);		// what was heard, whatever made it
	if (sndHeld && (sndGetRingDistance() >= sndGetRingTargetBytes()))
		snd_start_playback();
}

// A sample made by something other than the machine: rewind plays the
// history back through it.
void snd_put(sndPair lev) {
	if (conf.snd.need > 0)
		conf.snd.need--;
	snd_tok(SND_T_PUT, lev);
}

static void snd_start_playback() {
	sndHeld = 0;
	sndLowMark = SND_LOW_NONE;
	sndAutoSkip = SND_AUTO_GRACE;
	sndAutoDryHold = 0;
	if (sndOutput && sndOutput->play)
		sndOutput->play();
}

// The master volume and the clip. Both ends: FM is signed, and a sample under
// the low one wraps to the top of the range on the way into the ring
static long long rwNs = 0;		// the time a sub-sample stands for, as rewind takes it

static inline sndPair snd_master(sndPair lev) {
	lev.left = lev.left * conf.snd.vol.master / 100;
	lev.right = lev.right * conf.snd.vol.master / 100;
	return snd_clip16(lev);
}

// one sub-sample through the scope and the window; every DISCRATE-th makes a sample
static void snd_sub(const sndSub* q) {
	sndPair lev;
	if (sm.on) {			// the mirror mixes it: adv, late (a step's: -1 - late)
		snd_mirror_time(q->l);
		int late = q->r;
		int scope = (late < 0);
		sm.m.late = scope ? (-1 - late) : late;
		sm.m.rate = conf.snd.rate * DISCRATE;	// what the beeper's filter is timed by
		lev = sm.mix(&sm.m, &conf.snd.vol);
		if (scope) {
			scope_put((lev.left + lev.right) / 2);
			return;
		}
	} else {
		lev.left = q->l;
		lev.right = q->r;
	}
	scope_put((lev.left + lev.right) / 2);
	lev = snd_master(lev);
	// rewind's history, as it is heard; by emulated time, which the output is
	// not at another speed
	if (conf.emu.rewind.on)
		rewind_sound(lev, rwNs);
	smpBuf[sb_pos & SND_FIR_MASK] = lev;
	smpBuf[(sb_pos & SND_FIR_MASK) + SND_FIR_TAPS] = lev;
	sb_pos++;
	if ((sb_pos % DISCRATE) != 0) return;
	if (conf.snd.filter) {
		// The window ends at the newest sample, so it starts SND_FIR_TAPS
		// back - which is where sb_pos points now. The taps are symmetric,
		// so the two ends of the window share one and half the multiplies
		// go: measured, a third off the time, and the same answer.
		long long accL = 0;
		long long accR = 0;
		const sndPair* win = smpBuf + (sb_pos & SND_FIR_MASK);
		const sndPair* end = win + SND_FIR_TAPS - 1;
		for (int i = 0; i < SND_FIR_TAPS / 2; i++) {
			accL += (long long)(win[i].left + end[-i].left) * firTaps[i];
			accR += (long long)(win[i].right + end[-i].right) * firTaps[i];
		}
		// the sinc overshoots a step a little, so the result can
		// come out past what went in
		lev.left = (int)(accL >> SND_FIR_SHIFT);
		lev.right = (int)(accR >> SND_FIR_SHIFT);
		lev = snd_clip16(lev);
	} else {
		int sumL = 0;
		int sumR = 0;
		int idx = (sb_pos - DISCRATE) & SND_FIR_MASK;
		for (int i = 0; i < DISCRATE; i++) {
			sumL += smpBuf[idx].left;
			sumR += smpBuf[idx].right;
			idx = (idx + 1) & SND_FIR_MASK;
		}
		lev.left = sumL / DISCRATE;
		lev.right = sumR / DISCRATE;
	}
	if (!conf.snd.enabled) {
		lev.left = 0;
		lev.right = 0;
	}
	// the wav takes the sample here, where it is made. The old
	// accumulator in ethread.cpp ran off its own 44100 Hz clock, a
	// few ns per sample away from this one, and the two beat audibly.
	if (conf.snd.wavout)
		snd_wav_write(lev);
	snd_ring_put(lev);
}

// n sub-samples of silence: the samples they make are silent, the scope and the
// window take silence, rewind's history moves on by their time
static void snd_quiet_play(long long n) {
	if (n <= 0) return;
	if (conf.emu.rewind.on) rewind_sound_quiet(n, rwNs);
	long long w = (n < SND_SCOPE_SIZE) ? n : SND_SCOPE_SIZE;
	for (long long k = 0; k < w; k++)
		scopeBuf[(scopePos + (int)(n - w + k)) & SND_SCOPE_MASK] = 0;
	scopePos += (int)n;
	// the window's last SND_FIR_TAPS: what the filter sees when the sound comes back
	unsigned pos = (unsigned)sb_pos;
	long long m = (n < SND_FIR_TAPS) ? n : SND_FIR_TAPS;
	sndPair none = sndPair();
	for (long long k = n - m; k < n; k++) {
		unsigned i = (pos + (unsigned)k) & SND_FIR_MASK;
		smpBuf[i] = none;
		smpBuf[i + SND_FIR_TAPS] = none;
	}
	// a sample at every DISCRATE-th sub-sample, as snd_sub makes them
	long long first = DISCRATE - (pos % DISCRATE);
	long long count = (n >= first) ? 1 + (n - first) / DISCRATE : 0;
	sb_pos = (int)(pos + (unsigned)n);
	for (long long k = 0; k < count; k++) {
		if (conf.snd.wavout)
			snd_wav_write(none);
		snd_ring_put(none);
	}
}

static void snd_do_tok(const sndTok* t) {
	switch (t->kind) {
		case SND_T_DROP:
			sndDropWant = 1;
			break;
		case SND_T_RESET:
			memset(smpBuf, 0, sizeof(smpBuf));
			sb_pos = 0;
			break;
		case SND_T_PUT:
			snd_ring_put(conf.snd.enabled ? t->lev : sndPair());
			break;
		case SND_T_NS:
			rwNs = t->val;
			break;
		case SND_T_CALL:
			t->fn(t->val);
			break;
		case SND_T_SEED:
			snd_mirror_take_seed((sndSeedCopy*)t->blob);
			break;
		case SND_T_UNMIRROR:
			sm.on = 0;
			break;
		case SND_T_ADV:
			snd_mirror_time(t->adv);
			break;
		case SND_T_POKE:
			snd_mirror_time(t->adv);
			if (t->what == AYL_POKE)
				ay_poke_reg(&sm.chip[t->chip], t->a, t->b);
			else if (t->what == AYL_ECNT)
				sm.chip[t->chip].chanE.cnt = t->a;
			else if (t->what == AYL_WR)		// through the chip's own: an AY's or a YM2203's
				sm.chip[t->chip].wr(&sm.chip[t->chip], t->a, t->b);
			else if (t->what == AYL_FMPOKE)
				ym2203_poke_reg(&sm.chip[t->chip], t->a, t->b);
			else if (t->what == AYL_BLK) {		// flushed first, as the machine's was
				ay_flush(&sm.chip[t->chip]);
				sm.chip[t->chip].blk_fm = t->a;
			}
			break;
		case SND_T_RETUNE:
			snd_mirror_time(t->adv);
			sm.chip[t->chip].frq = t->frq;
			chip_set_type(&sm.chip[t->chip], t->a);
			break;
		case SND_T_IN:
			memcpy(&sm.m.in, &t->in, sizeof(sm.m.in));
			break;
		case SND_T_QUIET:
			snd_quiet_play(t->val);
			break;
	}
}

// what is there to take, sub-samples and tokens in their order. The head is read
// before the tokens: a token seen is never one that should have come before a
// sub-sample already taken
static int snd_work() {
	long long h = sqHead.load();
	long long th = stHead.load();
	long long pos = sqTail.load(std::memory_order_relaxed);
	long long tt = stTail.load(std::memory_order_relaxed);
	if ((pos == h) && (tt == th)) return 0;
	std::lock_guard<std::recursive_mutex> lk(sndWork);
	while (pos < h) {
		while ((tt < th) && (sndT[tt & SND_T_MASK].at <= pos)) {
			snd_do_tok(&sndT[tt & SND_T_MASK]);
			tt++;
		}
		// up to the next token, or the end
		long long stop = h;
		if ((tt < th) && (sndT[tt & SND_T_MASK].at < stop))
			stop = sndT[tt & SND_T_MASK].at;
		while (pos < stop) {
			snd_sub(&sndQ[pos & SND_Q_MASK]);
			pos++;
		}
		sndAdvDone.v.store(sm.advDone, std::memory_order_release);	// before the tails
		sqTail.store(pos);
		stTail.store(tt);
	}
	while ((tt < th) && (sndT[tt & SND_T_MASK].at <= pos)) {
		snd_do_tok(&sndT[tt & SND_T_MASK]);
		tt++;
	}
	sndAdvDone.v.store(sm.advDone, std::memory_order_release);
	stTail.store(tt);
	return 1;
}

static void snd_thread_run() {
	while (!sndQuit.load()) {
		if (snd_work()) continue;
		std::unique_lock<std::mutex> lk(sndWakeLock);
		sndSleeping.store(1);
		// asked again after saying so: a producer that queued meanwhile has seen
		// the flag, or this sees its work
		if ((sqTail.load() == sqHead.load()) && (stTail.load() == stHead.load()) && !sndQuit.load())
			sndWake.wait(lk);
		sndSleeping.store(0);
	}
}

static void snd_thread_start() {
	if (sndThread) return;
	// what the mirror's mix keeps starts as the machine's own does (nx_vol)
	sm.m.bVal = -1;
	sm.m.bVol = -1;
	sm.m.dOut = 0xffffffff;
	sm.m.dVol = -1;
	ay_log = snd_ay_log;
	ym2203_lite = snd_fm_lite;
	xstate_fm_back = snd_fm_back;
	sndQuit = 0;
	sndThread = new std::thread(snd_thread_run);
}

static void snd_thread_stop() {
	if (!sndThread) return;
	if (conf.zx) snd_fm_back(conf.zx);	// the copy's fm is the chip's: kept past it
	ym2203_lite = NULL;
	snd_sync_wait();
	sndQuit = 1;
	{
		std::lock_guard<std::mutex> lk(sndWakeLock);
		sndWake.notify_one();
	}
	sndThread->join();
	delete sndThread;
	sndThread = NULL;
}

// --- the emulation thread

// A sample is made at every DISCRATE-th sub-sample (sndSync), and the need is met
// with the need-th of them from here: a run that takes its sub-samples after it
// stops there at most, and the cycle ends on the step it would have
long long snd_need_room(long long acc) {
	int need = conf.snd.need;
	if (conf.emu.fast || (need <= 0)) return nsPerSampleFixed - acc;
	long long j = (DISCRATE - (sp.subPos % DISCRATE)) + (long long)(need - 1) * DISCRATE;
	return j * nsPerSampleFixed - acc;
}

// return 1 when buffer is full
// NOTE: need sync|flush devices if debug
int sndSync(Computer* comp) {
	if (!conf.emu.pause || comp->flgDBG) {
		if (comp->gs->enable) gsFlushMix(comp->gs);
//		saaFlush(comp->saa);
		if (!conf.emu.fast && !conf.emu.pause) {
			// the time this sub-sample stands for, for rewind: it moves with
			// the speed and with VSync's steer, so it goes along when it does
			if (conf.emu.rewind.on && (nsPerSampleFixed != sp.rwNsSent)) {
				sp.rwNsSent = nsPerSampleFixed;
				snd_tok(SND_T_NS, sndPair(), sp.rwNsSent);
			}
			if (!conf.snd.enabled) {
				// nothing to hear: only what the machine itself does at a sample
				// (the Next's copper, AY clock, DACs) is done, the mix is not made
				if (comp->hw->sndtake) comp->hw->sndtake(comp, &mp.take);
				snd_mirror_keep_up(comp);	// the copy keeps following, fm and all
				sp.quiet++;
			} else if (snd_mirror_fits(comp)) {
				snd_mirror_sample(comp, 0);
			} else {
				if (mp.comp == comp) snd_fm_back(comp);	// the copy goes, its fm stays
				if (mp.comp) snd_mirror_off();
				sndPair lev = comp->hw->vol(comp, &conf.snd.vol);
				snd_q_push(lev.left, lev.right);
			}
			sp.subPos++;
			if (((sp.subPos % DISCRATE) == 0) && (conf.snd.need > 0))
				conf.snd.need--;
			sp.smpCount++;
		} else if (conf.emu.fast) {
			snd_drop();		// the GS's flush is all fast mode runs here
			snd_mirror_keep_up(comp);
		}
	}
#if NEW_SMP_METHOD
	// fast mode makes no sound to wait for: emuCycle() runs it a frame at a
	// time instead, rather than handing the machine back after every sample
	if (conf.emu.fast) return 0;
	if (conf.snd.need > 0) return 0;
#else
	if (sp.smpCount < sndChunks) return 0;
#endif
	conf.snd.fill = 0;
	sp.smpCount = 0;
	return 1;
}

std::string sndGetOutputName() {
	std::string res = "NULL";
	if (sndOutput != NULL) {
		res = sndOutput->name;
	}
	return res;
}

static void snd_wav_close_now();

void setOutput(const char* name) {
	std::lock_guard<std::recursive_mutex> lk(sndWork);
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
	sndUpdateSpeed();
	// a recording's rate is in its header and cannot change, so it ends here
	// rather than play back at the wrong speed from this point on
	if (conf.snd.wavfile && (conf.snd.rate != wavRate)) {
		xlog(XLG_SOUND, XLL_INFO, "WAV output stopped: rate is now %i Hz", conf.snd.rate);
		snd_wav_close_now();
	}
}

void sndClose() {
	snd_thread_stop();
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
	int arrived = (sndGetRingDistance() + snd_queued_bytes() + block >= sndGetRingTargetBytes());
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
	int dropped = sndDropBytes.exchange(0);
	if (dropped > 0)
		xlog(XLG_SOUND, XLL_DEBUG, "dropped %i ms of queued sound", snd_bytes_to_ms(dropped));
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

// the bench hashes what went into the ring: where the fill stands, and a byte
int snd_ring_fill_pos() {
	return posf;
}

int snd_ring_byte(int pos) {
	return sbuf[pos & SND_RING_MASK];
}

double snd_scope_rate() {
	return (double)conf.snd.rate * DISCRATE;
}

// the mixer's history, back to what a start leaves it at: here, and in order with
// what is queued on the sound thread
void snd_pipe_reset() {
	sp.subPos = 0;
	sp.smpCount = 0;
	snd_tok(SND_T_RESET);
	snd_q_flush();
}

// Carry the capture across ns of emulated time on a machine the debugger is holding.
// Nothing is being played then - sndSync() is not even called - but the mixer still
// has an output for every moment of it, which is the whole point of tracing a beeper.
// The machine is held, so nothing more is queued: what the sound thread still had
// goes into the scope first.
void snd_scope_step(Computer* comp, int ns) {
	if ((ns <= 0) || !comp->hw->vol) return;
	if (snd_mirror_fits(comp)) {	// the mirror mixes them for the scope
		scopeNsFixed += NS_TO_FIXED(ns);
		while (scopeNsFixed > nsPerSampleFixed) {
			snd_mirror_sample(comp, 1);
			scopeNsFixed -= nsPerSampleFixed;
		}
		snd_q_flush();
		snd_sync_wait();
		return;
	}
	snd_sync_wait();
	std::lock_guard<std::recursive_mutex> lk(sndWork);
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
	Uint8* out = stream;
//	printf("len = %i\n",len);
	// conf.snd.need is no longer filled here - it is filled from a steady
	// timer in pacing.cpp, so frame making does not run in audio-buffer sized
	// bursts. See pacing.h for why. This callback only plays the ring buffer.
	int mark = sndDropMark.exchange(-1);
	if (mark >= 0) {
		int ahead = (mark - posp) & SND_RING_MASK;
		if (ahead <= sndGetRingDistance()) {	// else it was played already
			posp += ahead;
			sndDropBytes += ahead;
		}
		sndRefill = 1;
	}
	int dist = sndGetRingDistance();
	// the target is what the ring keeps once this block is taken out of it
	if (sndRefill && (dist >= sndGetRingTargetBytes() + len)) {
		sndRefill = 0;
		sndFade = conf.snd.rate * SND_FADE_MS / 1000;
		sndFadeL = sndOutL;
		sndFadeR = sndOutR;
	}
	int idle = conf.emu.fast || conf.emu.pause || sndRefill;
	if ((dist < len) || idle) {
		// nothing to play: hold the frame last sent. Not the one behind the
		// play position - after a drop that is a sample nobody heard.
		while (len >= 4) {
			*(stream++) = sndOutL & 0xff;
			*(stream++) = (sndOutL >> 8) & 0xff;
			*(stream++) = sndOutR & 0xff;
			*(stream++) = (sndOutR >> 8) & 0xff;
			len -= 4;
		}
	} else {
		int fadeLen = conf.snd.rate * SND_FADE_MS / 1000;
		while (len >= 4) {
			int l = (short)(sbuf[posp & SND_RING_MASK] | (sbuf[(posp + 1) & SND_RING_MASK] << 8));
			int r = (short)(sbuf[(posp + 2) & SND_RING_MASK] | (sbuf[(posp + 3) & SND_RING_MASK] << 8));
			posp += 4;
			if (sndFade > 0) {		// from the held frame into the new sound
				l += (sndFadeL - l) * sndFade / fadeLen;
				r += (sndFadeR - r) * sndFade / fadeLen;
				sndFade--;
			}
			sndOutL = (short)l;
			sndOutR = (short)r;
			*(stream++) = l & 0xff;
			*(stream++) = (l >> 8) & 0xff;
			*(stream++) = r & 0xff;
			*(stream++) = (r >> 8) & 0xff;
			len -= 4;
		}
	}
	while (len-- > 0) *(stream++) = 0;
	if (conf.snd.mute && (want > 0)) memset(out, 0, (size_t)want);	// signed 16 bit: silence is zero
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
#if SDL_VERSION_ATLEAST(2, 24, 0)
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
	// Nothing is allowed to change: a rate granted behind our back could be one
	// the ring is not sized for, and it would be written to the config as if
	// the user had picked it. snd_pick_auto_rate() above is the one place the
	// rate is chosen.
	sdldevid = SDL_OpenAudioDevice(NULL, 0, &asp, &dsp, 0);
	if (sdldevid == 0) {
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
	sndDropMark = -1;		// the ring it marked is gone
	sndRefill = 0;
	sndFade = 0;
	sndOutL = sndOutR = 0;
	return res;
}

void sdlplay() {
	SDL_PauseAudioDevice(sdldevid, 0);
}

void sdlclose() {
	SDL_CloseAudioDevice(sdldevid);
}

// init

OutSys sndTab[] = {
	{xOutputNone,"NULL",&null_open,NULL,&null_close},
	{xOutputSDL,"SDL",&sdlopen,&sdlplay,&sdlclose},
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
	conf.snd.mute = 0;
	conf.snd.filter = 1;	// the block average it replaces folds everything above 22 kHz back in
	sndOutput = NULL;
	conf.snd.wavout = 0;
	conf.snd.wavfile = NULL;
	initNoise();
	snd_init_filter();
	snd_thread_start();
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

static void snd_wav_close_now() {
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

// what was handed to the sound thread before the call goes in, nothing after it
void snd_wav_close() {
	snd_sync_wait();
	std::lock_guard<std::recursive_mutex> lk(sndWork);
	snd_wav_close_now();
}

int snd_wav_open(const char* path) {
	int res = ERR_OK;
	std::lock_guard<std::recursive_mutex> lk(sndWork);
	wavHead hd = wav_prepare(conf.snd.rate, 2);
	snd_wav_close_now();
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

void snd_wav_write(sndPair lev) {
	if (conf.snd.wavfile) {
		// the same S16LE the live output gets, not just the top byte
		fputw(lev.left & 0xffff, conf.snd.wavfile);
		fputw(lev.right & 0xffff, conf.snd.wavfile);
	}
}

// debug

void sndDebug() {
	xlog(XLG_SOUND, XLL_DEBUG, "ring: %i - %i = %i", posf, posp, posf - posp);
}

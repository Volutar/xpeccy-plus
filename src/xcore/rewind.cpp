#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <vector>

#include "rewind.h"
#include "xcore.h"
#include "fastload.h"
#include "autostart.h"
#include "sound.h"
#include "../libxpeccy/xstate.h"
#include "../libxpeccy/xlog.h"

// A snapshot is megabytes on the bigger machines (4.2M on a ZX Evo), and one
// taken a few frames after another differs from it in a few pages. So the bytes
// are kept in pages, and a page that did not change since the snapshot before
// is that snapshot's page, counted twice.

#define RW_PAGE		4096
#define RW_MEM_MAX	((size_t)512 << 20)	// whatever secs asks for, never more than this

typedef struct {
	int refs;
	unsigned char data[RW_PAGE];
} rwPage;

typedef struct {
	std::vector<unsigned char> meta;
	std::vector<rwPage*> pages;
	size_t size;
	Tape tape;		// where the tape stood; put back only onto the same tape
	int frame;		// comp->frmCount when it was taken
	long long phase;	// see rewind_frame()
	long long snd;		// rw_snd_w when it was taken
} rwEntry;

static std::deque<rwEntry> rw_ring;
static size_t rw_mem = 0;
static std::atomic<int> rw_clear_req(0);
static Computer* rw_comp = NULL;	// what the history was taken from
static unsigned rw_writes = 0;		// x_media_writes the history started at
static int rw_wait = 0;			// frames since the last snapshot
static xState* rw_st = NULL;		// every save and every load goes through it

// The machine's sound as a ring, at the output rate but in emulated time: the
// sub-samples are averaged into it by the time they cover, so a stretch of
// fast forward or slow motion takes as much of it as the frames it holds, and a
// snapshot's place in it is where its frames are. rw_snd_w counts every sample.
#define RW_SND_EXTRA	8	// seconds of sound over the history, since a snapshot waits for a safe frame
typedef struct {
	short left, right;	// already clipped to 16 bits
} rwSample;
static std::vector<rwSample> rw_snd;
static long long rw_snd_w = 0;
static long long rw_snd_per = 0;	// emulated time of one sample, 16.16 ns
static int rw_snd_rate = 0, rw_snd_secs = 0;	// what size and period were worked out for
// the sub-samples gathered for the next sample
static struct {
	long long ns;
	int left, right, n;
} rw_acc;
// playing back: the stretch between the snapshot shown and the one before it,
// read from pos down to lo, ratio samples to an output sample
static struct {
	double pos;
	long long lo;
	double ratio;
} rw_seg;

static void page_drop(rwPage* pg) {
	if (--pg->refs > 0) return;
	free(pg);
	rw_mem -= sizeof(rwPage);
}

static void entry_drop(rwEntry& e) {
	for (rwPage* pg : e.pages)
		page_drop(pg);
	e.pages.clear();
}

static void rw_pop_front() {
	entry_drop(rw_ring.front());
	rw_ring.pop_front();
}

static void rw_drop_all() {
	while (!rw_ring.empty())
		rw_pop_front();
}

// a medium was written: nothing taken before that can be put back
static int rw_media_changed() {
	if (x_media_writes == rw_writes) return 0;
	rw_writes = x_media_writes;
	rw_drop_all();
	return 1;
}

static rwEntry* rw_at(int back) {
	int n = (int)rw_ring.size();
	if ((back < 0) || (back >= n)) return NULL;
	return &rw_ring[n - 1 - back];
}

// The most the history has held on this machine, for the log. Filling up is
// growth by definition, so nothing is said until it is full; then once, and
// again each time a heavier stretch raises the mark by a megabyte. A write to a
// medium empties the history but keeps the mark; another machine starts over.
#define RW_LOG_STEP	((size_t)1 << 20)
static size_t rw_peak = 0;		// 0: not told yet on this machine
static bool rw_capped = false;		// the cap has been told about

static void rw_log_forget() {
	rw_peak = 0;
	rw_capped = false;
}

static void rw_log_size(Computer* comp, size_t max, bool capped) {
	if (rw_ring.empty()) return;
	const rwEntry& e = rw_ring.back();
	double secs = (e.frame - rw_ring.front().frame) / comp_fps(comp);
	if (capped && !rw_capped) {
		rw_capped = true;
		xlog(XLG_CORE, XLL_WARN, "rewind: history hit the %u MB cap, %.1f s kept",
			(unsigned)(RW_MEM_MAX >> 20), secs);
	}
	if ((rw_ring.size() < max) && !capped) return;
	size_t count = rw_ring.size();
	size_t tables = count * (sizeof(rwEntry) + e.meta.size() + e.pages.size() * sizeof(rwPage*));
	size_t sound = rw_snd.size() * sizeof(rwSample);
	size_t total = rw_mem + tables + sound + e.size;	// e.size: the scratch snapshot
	if (rw_peak && (total < rw_peak + RW_LOG_STEP)) return;
	rw_peak = total;
	const double mb = 1 << 20;
	xlog(XLG_CORE, XLL_INFO, "rewind: history peak %.1f MB - %.1f MB in %u snapshots (%u KB each), "
		"%.1f MB sound, %.1f MB tables; %.1f s",
		total / mb, rw_mem / mb, (unsigned)count, (unsigned)(rw_mem / count >> 10),
		sound / mb, tables / mb, secs);
}

static void rw_trim(Computer* comp) {
	size_t max = (size_t)(conf.emu.rewind.secs * comp_fps(comp) / conf.emu.rewind.step) + 1;
	bool capped = false;
	while ((rw_ring.size() > max) || ((rw_mem > RW_MEM_MAX) && (rw_ring.size() > 1))) {
		capped |= (rw_ring.size() <= max);
		rw_pop_front();
	}
	rw_log_size(comp, max, capped);
}

static void rw_take(Computer* comp, long long phase) {
	if (!rw_st) rw_st = xstate_create();
	if (!rw_st || !xstate_save(rw_st, comp)) return;
	const unsigned char* data = NULL;
	rwEntry e;
	e.meta.resize(xstate_meta_size());
	e.size = xstate_bytes(rw_st, &data, e.meta.data());
	// a history in another layout could not be put back into this one
	rwEntry* prev = rw_ring.empty() ? NULL : &rw_ring.back();
	if (prev && !xstate_same_layout(prev->meta.data(), e.meta.data())) {
		rw_drop_all();
		prev = NULL;
	}
	size_t n = (e.size + RW_PAGE - 1) / RW_PAGE;
	e.pages.resize(n);
	for (size_t i = 0; i < n; i++) {
		size_t off = i * RW_PAGE;
		size_t len = std::min((size_t)RW_PAGE, e.size - off);
		if (prev && !memcmp(prev->pages[i]->data, data + off, len)) {
			e.pages[i] = prev->pages[i];
			e.pages[i]->refs++;
			continue;
		}
		rwPage* pg = (rwPage*)malloc(sizeof(rwPage));
		if (!pg) {
			e.pages.resize(i);
			entry_drop(e);
			return;
		}
		pg->refs = 1;
		memcpy(pg->data, data + off, len);
		rw_mem += sizeof(rwPage);
		e.pages[i] = pg;
	}
	e.tape = *comp->tape;
	e.frame = comp->frmCount;
	e.phase = phase;
	e.snd = rw_snd_w;
	rw_ring.push_back(std::move(e));
	rw_trim(comp);
}

// Playing the history back. While the key is held every picture shown is the
// next snapshot back: loaded and run for one frame that is thrown away
// (xstate_run_frame), which draws what the screen showed then - multicolour
// and all - for the price of one ordinary frame. Each picture plays the sound
// between its snapshot and the one shown before it, backwards. Let go, and the
// machine carries on from the snapshot on screen, with everything newer dropped.

enum {
	RW_REC = 0,	// taking snapshots
	RW_PLAY,	// going back
	RW_END		// on the oldest snapshot, standing still
};

static std::atomic<int> rw_key(0);
static std::atomic<int> rw_mode(RW_REC);
static int rw_back = 0;		// the snapshot on screen
static int rw_from = 0;		// comp->frmCount when the rewind began
static std::atomic<int> rw_tenths(0);	// see rewind_back_tenths
static double rw_smp = 0;	// output samples until the next picture

// called for every sub-sample, so the size and the period are only worked out
// again when the rate or the length has moved
void rewind_sound(sndPair lev, long long nsFixed) {
	if (!conf.emu.rewind.on || (rw_mode != RW_REC)) return;
	if ((conf.snd.rate != rw_snd_rate) || (conf.emu.rewind.secs != rw_snd_secs)) {
		if (conf.snd.rate < 1) return;
		rw_snd_rate = conf.snd.rate;
		rw_snd_secs = conf.emu.rewind.secs;
		rw_snd_per = NSD_TO_FIXED(1e9 / rw_snd_rate);
		rw_snd.assign((size_t)(rw_snd_secs + RW_SND_EXTRA) * rw_snd_rate, rwSample());
		rw_snd_w = 0;
		rw_acc = {};
	}
	rw_acc.left += lev.left;
	rw_acc.right += lev.right;
	rw_acc.n++;
	rw_acc.ns += nsFixed;
	if (rw_acc.ns < rw_snd_per) return;
	rw_acc.ns -= rw_snd_per;
	rw_snd[rw_snd_w % rw_snd.size()] = {(short)(rw_acc.left / rw_acc.n), (short)(rw_acc.right / rw_acc.n)};
	rw_snd_w++;
	rw_acc.left = rw_acc.right = rw_acc.n = 0;
}

// the next output sample going back: the samples its share of the stretch
// covers, averaged, so the sound is sped up rather than thinned out. Silence
// where there is none: past the oldest one kept, or a stretch with no sound in
// it (fast mode makes none).
static sndPair rw_snd_back() {
	sndPair res = sndPair();
	if (rw_seg.ratio <= 0) return res;
	long long cap = (long long)rw_snd.size();
	double next = std::max(rw_seg.pos - rw_seg.ratio, (double)rw_seg.lo);
	long long a = (long long)next;
	long long b = std::max((long long)rw_seg.pos, a + 1);
	rw_seg.pos = next;
	int left = 0, right = 0, n = 0;
	for (long long i = a; i < b; i++) {
		if ((i < 0) || (i < rw_snd_w - cap) || (i >= rw_snd_w)) continue;
		const rwSample& p = rw_snd[i % cap];
		left += p.left;
		right += p.right;
		n++;
	}
	if (n) {
		res.left = left / n;
		res.right = right / n;
	}
	return res;
}

static void rw_play_stop() {
	rw_mode = RW_REC;
	rw_wait = 0;
}

// the picture of snapshot back: 1 when there is one to show
static int rw_show(Computer* comp, int back, long long* phase) {
	return rewind_load(comp, back, phase) && xstate_run_frame(comp);
}

static void rw_let_go(Computer* comp, long long* phase) {
	int from = comp->frmCount;
	rwEntry* e = rw_at(rw_back);
	if (e) rw_snd_w = e->snd;
	rw_acc = {};
	if (rewind_restore(comp, rw_back, phase))
		xlog(XLG_CORE, XLL_INFO, "rewind: played on from frame %i, key let go at %i", comp->frmCount, from);
	rw_play_stop();
}

#ifdef XBENCH
static int rw_held = 0;
void rewind_hold(int on) {
	rw_held = on;
}
#else
#define rw_held 0
#endif

void rewind_frame(Computer* comp, long long* phase) {
	if (rw_clear_req.exchange(0) || (comp != rw_comp)) {
		rw_drop_all();
		rw_play_stop();
		rw_log_forget();
		rw_comp = comp;
	}
	if (!conf.emu.rewind.on) {
		rw_drop_all();
		return;
	}
	if (rw_held || (rw_mode != RW_REC)) return;
	rw_media_changed();
	if (rw_key && !rw_ring.empty() && !comp->tape->rec) {
		xlog(XLG_CORE, XLL_INFO, "rewind: %i snapshots back to frame %i", rewind_count(), rewind_frame_of(rewind_count() - 1));
		fastload_stop(comp);		// it holds the picture back, and the speed
		rw_back = -1;
		rw_from = comp->frmCount;
		rw_tenths = 0;
		rw_smp = 0;
		rw_mode = RW_PLAY;
		return;
	}
	if (++rw_wait < conf.emu.rewind.step) return;
	// a load runs flat out, and a history of it is worth nothing
	if (fastload_busy() || autostart_busy() || comp->tape->rec) return;
	if (!xstate_safe_tape_aside(comp)) return;	// tried again next frame
	rw_wait = 0;
	rw_take(comp, *phase);
}

int rewind_back_tenths() {
	return rw_tenths;
}

int rewind_play(Computer* comp, long long* phase) {
	if (rw_mode == RW_REC) return 0;
	if (!rw_key) {
		rw_let_go(comp, phase);
		return 0;
	}
	int shown = 0;
	if (rw_smp <= 0) {
		double spf = conf.snd.rate / comp_fps(comp);
		rw_smp += spf;
		if ((rw_mode == RW_PLAY) && (rw_back + 1 < rewind_count()) && rw_show(comp, rw_back + 1, phase)) {
			// the sound between this picture and the one shown before, backwards
			rwEntry* e = rw_at(rw_back);
			long long hi = e ? e->snd : rw_snd_w;
			rw_back++;
			shown = 1;
			rw_tenths = (int)((rw_from - rw_at(rw_back)->frame) * 10 / comp_fps(comp) + 0.5);
			rw_seg.lo = rw_at(rw_back)->snd;
			rw_seg.pos = (double)hi;
			rw_seg.ratio = (hi - rw_seg.lo) / spf;
		} else {
			rw_mode = RW_END;
		}
	}
	rw_smp--;
	snd_put((rw_mode == RW_PLAY) ? rw_snd_back() : sndPair());
	return shown ? 2 : 1;
}

void rewind_want(int on) {
	rw_key = on;
}

int rewind_active() {
	return rw_mode != RW_REC;
}

void rewind_clear() {
	rw_clear_req = 1;
}

int rewind_count() {
	return (int)rw_ring.size();
}

size_t rewind_bytes() {
	return rw_mem;
}

int rewind_frame_of(int back) {
	rwEntry* e = rw_at(back);
	return e ? e->frame : -1;
}

// the bytes of a snapshot, in one piece
static void rw_unpage(const rwEntry* e, unsigned char* dst) {
	for (size_t i = 0; i < e->pages.size(); i++) {
		size_t off = i * RW_PAGE;
		memcpy(dst + off, e->pages[i]->data, std::min((size_t)RW_PAGE, e->size - off));
	}
}

int rewind_load(Computer* comp, int back, long long* phase) {
	rwEntry* e = rw_at(back);
	if (!e || !rw_st || comp->tape->rec) return 0;
	if (rw_media_changed()) return 0;
	unsigned char* dst = xstate_put_begin(rw_st, e->meta.data());
	if (!dst) return 0;
	rw_unpage(e, dst);
	if (!xstate_load(rw_st, comp)) {
		rw_drop_all();
		return 0;
	}
	if (phase) *phase = e->phase;
	Tape* tap = comp->tape;
	if (tap_same_image(tap, &e->tape)) {
		tap_copy_pos(tap, &e->tape);
		tap->portReads = 0;
		tap->loaderReads = 0;
	}
	return 1;
}

int rewind_restore(Computer* comp, int back, long long* phase) {
	if (!rewind_load(comp, back, phase)) return 0;
	while (back-- > 0) {
		entry_drop(rw_ring.back());
		rw_ring.pop_back();
	}
	rw_wait = 0;
	return 1;
}

#ifdef XBENCH
int rewind_matches(Computer* comp, int back) {
	rwEntry* e = rw_at(back);
	if (!e || !rw_st || !xstate_save(rw_st, comp)) return 0;
	std::vector<unsigned char> meta(xstate_meta_size());
	size_t size = xstate_bytes(rw_st, NULL, meta.data());
	if (!xstate_same_layout(meta.data(), e->meta.data())) {
		xlog(XLG_CORE, XLL_WARN, "rewind check: frame %i has another layout", e->frame);
		return 0;
	}
	std::vector<unsigned char> then(size);
	rw_unpage(e, then.data());
	size_t first = 0;
	size_t diff = xstate_diff(rw_st, comp, then.data(), &first);
	if (!diff) return 1;
	size_t inner = 0;
	int chunk = xstate_chunk_at(rw_st, first, &inner);
	xlog(XLG_CORE, XLL_WARN, "rewind check: frame %i differs in %u bytes, first in part %i at +%u (was %02X)",
		e->frame, (unsigned)diff, chunk, (unsigned)inner, then[first]);
	return 0;
}
#endif

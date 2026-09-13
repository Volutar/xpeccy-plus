#include <stdlib.h>
#include <string.h>

#include "xstate.h"
#include "xlog.h"

// The machine is a tree of structs allocated one by one, so a snapshot is a
// list of ranges rather than one block. The list is rebuilt on every save and
// on every load: it costs a few dozen stores against a memcpy of hundreds of
// kilobytes, and comparing the two is what catches a machine that changed
// under us instead of writing over the new one.

int x_runahead = 0;

#define XST_MAX_CHUNKS	40

typedef struct {
	void* ptr;
	size_t size;
} xStateChunk;

struct xState {
	xStateChunk chunk[XST_MAX_CHUNKS];
	int count;			// 0 = nothing saved
	unsigned char* data;
	size_t cap;			// bytes allocated
};

#define ADD(_p, _s) do { \
		void* _ptr = (void*)(_p); \
		size_t _sz = (size_t)(_s); \
		if (_ptr && _sz) { \
			if (n >= XST_MAX_CHUNKS) return -1; \
			list[n].ptr = _ptr; \
			list[n].size = _sz; \
			n++; \
		} \
	} while (0)

// the page map and the ram behind it. Pages point into mem->ramData, which does
// not move, so the pointers in the map stay good. The rom is left out: a rom
// page has no write callback, so it can never change.
static int add_memory(xStateChunk* list, int n, Memory* mem) {
	if (!mem) return n;
	ADD(mem->map, sizeof(mem->map));
	ADD(mem->ramData, mem_ram_extent(mem));
	ADD((char*)mem + offsetof(Memory, ramSize), sizeof(Memory) - offsetof(Memory, ramSize));
	return n;
}

static int xst_build(Computer* comp, xStateChunk* list) {
	int n = 0;
	int i;

	if (!comp || !comp->cpu || !comp->mem || !comp->vid) return -1;

	// The machine itself, around the breakpoint maps: 4.6 MB of debugger
	// bookkeeping in the middle of the struct that no rollback needs. The rzx
	// frame buffer is skipped the same way - 64K of the head range, and the
	// only thing that reads it is a recording being played, which never runs
	// ahead. See the note beside brkRamMap in spectrum.h.
#ifdef HAVEZLIB
	ADD(comp, offsetof(Computer, rzx.frm.data));
	ADD((char*)comp + offsetof(Computer, rzx.frm.pos),
		offsetof(Computer, brkRamMap) - offsetof(Computer, rzx.frm.pos));
#else
	ADD(comp, offsetof(Computer, brkRamMap));
#endif
	ADD((char*)comp + offsetof(Computer, heatRam), sizeof(Computer) - offsetof(Computer, heatRam));

	ADD(comp->cpu, sizeof(CPU));
	n = add_memory(list, n, comp->mem);

	ADD(comp->vid, sizeof(Video));
	ADD(comp->vid->ula, sizeof(ulaPlus));

	ADD(comp->beep, sizeof(bitChan));
	// A chip can hold state outside its own struct - a YM2203's fm half
	// does - and only the struct is here to copy. ts_state_range() hands
	// that state over as bytes; xstate_save/_load fill and read them.
	if (comp->ts) {
		ADD(comp->ts, sizeof(TSound));
		ADD(comp->ts->chipA, sizeof(aymChip));
		ADD(comp->ts->chipB, sizeof(aymChip));
		ADD(comp->ts->chipC, sizeof(aymChip));
		ADD(comp->ts->chipD, sizeof(aymChip));
		for (i = 0; i < 4; i++) {
			void* p = NULL;
			int sz = ts_state_range(comp->ts, i, &p);
			ADD(p, sz);
		}
	}
	// the General Sound is a whole second machine with 2M of its own. Every way
	// into it returns at once while it is switched off, so a switched-off one
	// has nothing that can change.
	if (comp->gs && comp->gs->enable) {
		ADD(comp->gs, sizeof(GSound));
		ADD(comp->gs->cpu, sizeof(CPU));
		n = add_memory(list, n, comp->gs->mem);
	}
	ADD(comp->sdrv, sizeof(SDrive));
	ADD(comp->saa, sizeof(saaChip));

	// the storage controllers, their heads and their timers - but not the
	// media. A floppy's own struct is taken up to its track data only, and an
	// fdc up to its sector list: both are 400K of the track being transferred,
	// and xstate_safe() refuses a frame while a transfer is running.
	if (comp->dif) {
		ADD(comp->dif, sizeof(DiskIF));
		ADD(comp->dif->fdc, offsetof(FDC, slst));
		for (i = 0; i < 4; i++)
			ADD(comp->dif->flp[i], offsetof(Floppy, data));
	}
	if (comp->ide) {
		ADD(comp->ide, sizeof(IDE));
		ADD(comp->ide->master, sizeof(ATADev));
		ADD(comp->ide->slave, sizeof(ATADev));
	}
	ADD(comp->sdc, sizeof(SDCard));

	return n;
}

#undef ADD

// A command in progress walks the track data and the fdc sector list, and
// neither is in the snapshot. The motor bit would be the obvious test and is no
// good: it sticks on for good on a drive with no disk in it. Idle means waiting
// for a command - the vg93 leaves no plan behind at all, the upd765 parks on a
// do-nothing plan and raises idle, and a freshly reset vg93 has neither yet.
static int fdc_running(FDC* fdc) {
	return fdc && fdc->plan && !fdc->idle;
}

int xstate_safe(Computer* comp) {
	if (!comp || !comp->hw) return 0;
	if (comp->tape && comp->tape->on) return 0;	// the tape signal is not in the snapshot
#ifdef HAVEZLIB
	if (comp->rzx.play) return 0;			// a recording is read forwards only
#endif
	if (comp->dif) {
		if (fdc_running(comp->dif->fdc)) return 0;
	}
	return 1;
}

static size_t xst_total(xStateChunk* list, int count) {
	size_t res = 0;
	int i;
	for (i = 0; i < count; i++)
		res += list[i].size;
	return res;
}

xState* xstate_create(void) {
	xState* st = (xState*)malloc(sizeof(xState));
	if (!st) return NULL;
	memset(st, 0x00, sizeof(xState));
	return st;
}

void xstate_destroy(xState* st) {
	if (!st) return;
	free(st->data);
	free(st);
}

int xstate_save(xState* st, Computer* comp) {
	if (!st) return 0;
	st->count = 0;
	if (comp->ts) ts_state_capture(comp->ts);
	int count = xst_build(comp, st->chunk);
	if (count < 0) {
		xlog(XLG_CORE, XLL_WARN, "state: the machine has more parts than the snapshot holds");
		return 0;
	}
	size_t total = xst_total(st->chunk, count);
	if (total > st->cap) {
		unsigned char* buf = (unsigned char*)realloc(st->data, total);
		if (!buf) {
			xlog(XLG_CORE, XLL_ERROR, "state: no room for a %u byte snapshot", (unsigned)total);
			return 0;
		}
		st->data = buf;
		st->cap = total;
		xlog(XLG_CORE, XLL_INFO, "state: snapshot is %u bytes over %i parts", (unsigned)total, count);
	}
	unsigned char* dst = st->data;
	int i;
	for (i = 0; i < count; i++) {
		memcpy(dst, st->chunk[i].ptr, st->chunk[i].size);
		dst += st->chunk[i].size;
	}
	st->count = count;
	return 1;
}

int xstate_load(xState* st, Computer* comp) {
	if (!st || (st->count < 1)) return 0;
	xStateChunk now[XST_MAX_CHUNKS];
	int count = xst_build(comp, now);
	int i;
	int same = (count == st->count);
	// a different machine (profile or hardware switched while we held this):
	// the ranges would land in the wrong places, so drop the snapshot instead
	for (i = 0; same && (i < count); i++) {
		if ((now[i].ptr != st->chunk[i].ptr) || (now[i].size != st->chunk[i].size))
			same = 0;
	}
	if (!same) {
		xlog(XLG_CORE, XLL_INFO, "state: the machine changed, snapshot dropped");
		st->count = 0;
		return 0;
	}
	unsigned char* src = st->data;
	for (i = 0; i < count; i++) {
		memcpy(st->chunk[i].ptr, src, st->chunk[i].size);
		src += st->chunk[i].size;
	}
	if (comp->ts) ts_state_restore(comp->ts);
	return 1;
}

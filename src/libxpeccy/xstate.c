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
unsigned x_media_writes = 0;

#define XST_MAX_CHUNKS	48

typedef struct {
	void* ptr;
	size_t size;
} xStateChunk;

// where the bytes of a snapshot belong: all a caller keeping them elsewhere
// has to keep with them
typedef struct {
	xStateChunk chunk[XST_MAX_CHUNKS];
	int count;			// 0 = nothing saved
	size_t total;			// bytes over all the chunks
	long ray, line;			// the ray's offsets into the current image buffer
} xStateMeta;

struct xState {
	xStateMeta m;
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
	// frame buffer is skipped the same way - 64K of the head range, which
	// xstate_load() reads back from the recording (rzx_reread). See the note
	// beside brkRamMap in spectrum.h.
	ADD(comp, offsetof(Computer, rzx.frm.data));
	ADD((char*)comp + offsetof(Computer, rzx.frm.pos),
		offsetof(Computer, brkRamMap) - offsetof(Computer, rzx.frm.pos));
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
	// What names the medium - its path, open file, folder volume and size - is
	// left out too: an older snapshot would put back a path and a file that
	// were freed when the medium was changed since.
	if (comp->dif) {
		ADD(comp->dif, sizeof(DiskIF));
		ADD(comp->dif->fdc, offsetof(FDC, slst));
		for (i = 0; i < 4; i++)
			ADD(comp->dif->flp[i], offsetof(Floppy, path));
	}
	if (comp->ide) {
		ATADev* dev[2] = {comp->ide->master, comp->ide->slave};
		ADD(comp->ide, sizeof(IDE));
		for (i = 0; i < 2; i++) {
			if (!dev[i]) continue;
			ADD(dev[i], offsetof(ATADev, maxlba));
			ADD((char*)dev[i] + offsetof(ATADev, buf), offsetof(ATADev, pass) - offsetof(ATADev, buf));
		}
		if (comp->ide->smuc.nv)		// SMUC's NVRAM is spoken to bit by bit
			ADD(comp->ide->smuc.nv, sizeof(nvRam));
	}
	if (comp->sdc) {
		ADD(comp->sdc, offsetof(SDCard, capacity));
		ADD((char*)comp->sdc + offsetof(SDCard, buf), sizeof(SDCard) - offsetof(SDCard, buf));
	}

	return n;
}

#undef ADD

// A command in progress walks the track data and the fdc sector list, and
// neither is in the snapshot. The motor bit would be the obvious test and is no
// good: it sticks on for good on a drive with no disk in it. Idle means waiting
// for a command - the wd1793 leaves no plan behind at all, the upd765 parks on a
// do-nothing plan and raises idle, and a freshly reset wd1793 has neither yet.
static int fdc_running(FDC* fdc) {
	return fdc && fdc->plan && !fdc->idle;
}

int xstate_safe(Computer* comp) {
	if (comp && comp->tape && comp->tape->on) return 0;	// the tape signal is not in the snapshot
	return xstate_safe_tape_aside(comp);
}

int xstate_safe_tape_aside(Computer* comp) {
	if (!comp || !comp->hw) return 0;
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
	st->m.count = 0;
	if (comp->ts) ts_state_capture(comp->ts);
	int count = xst_build(comp, st->m.chunk);
	if (count < 0) {
		xlog(XLG_CORE, XLL_WARN, "state: the machine has more parts than the snapshot holds");
		return 0;
	}
	size_t total = xst_total(st->m.chunk, count);
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
		memcpy(dst, st->m.chunk[i].ptr, st->m.chunk[i].size);
		dst += st->m.chunk[i].size;
	}
	st->m.count = count;
	st->m.total = total;
	st->m.ray = comp->vid->ray.ptr - scrimg;
	st->m.line = comp->vid->ray.lptr - scrimg;
	return 1;
}

int xstate_load(xState* st, Computer* comp) {
	if (!st || (st->m.count < 1)) return 0;
	xStateChunk now[XST_MAX_CHUNKS];
	int count = xst_build(comp, now);
	int i;
	int same = (count == st->m.count);
	// a different machine (profile or hardware switched while we held this):
	// the ranges would land in the wrong places, so drop the snapshot instead
	for (i = 0; same && (i < count); i++) {
		if ((now[i].ptr != st->m.chunk[i].ptr) || (now[i].size != st->m.chunk[i].size))
			same = 0;
	}
	if (!same) {
		xlog(XLG_CORE, XLL_INFO, "state: the machine changed, snapshot dropped");
		st->m.count = 0;
		return 0;
	}
	// nodraw is set from outside the machine (fast loading holds it), so
	// whoever holds the machine keeps it
	int nodraw = comp->vid->nodraw;
	unsigned char* src = st->data;
	for (i = 0; i < count; i++) {
		memcpy(st->m.chunk[i].ptr, src, st->m.chunk[i].size);
		src += st->m.chunk[i].size;
	}
	comp->vid->nodraw = nodraw;
	if (comp->ts) ts_state_restore(comp->ts);
	// The image buffers are outside the snapshot and may have been swapped
	// since, so the ray goes back by its offset into whichever buffer is
	// current, not by the address it held before.
	comp->vid->ray.ptr = scrimg + st->m.ray;
	comp->vid->ray.lptr = scrimg + st->m.line;
	rzx_reread(comp);
	return 1;
}

// The bound is only there to let go of a machine that never finishes a frame -
// dummy hardware, say - instead of spinning on it every frame forever.
int xstate_run_frame(Computer* comp) {
	int guard = 200000;		// a frame is ~20000 opcodes
	x_runahead = 1;
	while (!comp->flgFRM && (guard-- > 0))
		compExec(comp);
	comp->flgFRM = 0;
	x_runahead = 0;
	return guard > 0;
}

size_t xstate_meta_size(void) {
	return sizeof(xStateMeta);
}

size_t xstate_bytes(const xState* st, const unsigned char** data, void* meta) {
	if (!st || (st->m.count < 1)) return 0;
	if (data) *data = st->data;
	if (meta) memcpy(meta, &st->m, sizeof(xStateMeta));
	return st->m.total;
}

int xstate_same_layout(const void* meta1, const void* meta2) {
	const xStateMeta* a = (const xStateMeta*)meta1;
	const xStateMeta* b = (const xStateMeta*)meta2;
	if ((a->count != b->count) || (a->total != b->total)) return 0;
	return !memcmp(a->chunk, b->chunk, a->count * sizeof(xStateChunk));
}

unsigned char* xstate_put_begin(xState* st, const void* meta) {
	const xStateMeta* m = (const xStateMeta*)meta;
	if (!st) return NULL;
	st->m.count = 0;
	if (m->total > st->cap) {
		unsigned char* buf = (unsigned char*)realloc(st->data, m->total);
		if (!buf) return NULL;
		st->data = buf;
		st->cap = m->total;
	}
	memcpy(&st->m, m, sizeof(xStateMeta));
	return st->data;
}

size_t xstate_diff(const xState* st, Computer* comp, const unsigned char* other, size_t* first) {
	if (!st || (st->m.count < 1)) return 0;
	// the ray points into whichever image buffer is current, and those swap
	// every frame: the pointers are not state, their offsets (in the meta) are
	const char* skip[2] = {(const char*)&comp->vid->ray.ptr, (const char*)&comp->vid->ray.lptr};
	size_t off = 0;
	size_t diff = 0;
	int i, k;
	for (i = 0; i < st->m.count; i++) {
		const char* base = (const char*)st->m.chunk[i].ptr;
		size_t n;
		for (n = 0; n < st->m.chunk[i].size; n++) {
			if (st->data[off + n] == other[off + n]) continue;
			for (k = 0; k < 2; k++)
				if ((base + n >= skip[k]) && (base + n < skip[k] + sizeof(void*))) break;
			if (k < 2) continue;
			if (!diff && first) *first = off + n;
			diff++;
		}
		off += st->m.chunk[i].size;
	}
	return diff;
}

size_t xstate_chunk_size(const xState* st, int i) {
	return (st && (i >= 0) && (i < st->m.count)) ? st->m.chunk[i].size : 0;
}

int xstate_chunk_at(const xState* st, size_t off, size_t* inner) {
	int i;
	for (i = 0; st && (i < st->m.count); i++) {
		if (off < st->m.chunk[i].size) {
			if (inner) *inner = off;
			return i;
		}
		off -= st->m.chunk[i].size;
	}
	return -1;
}

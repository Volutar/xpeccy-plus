// RZX recording: the input log of the running machine, written out as rzx.c reads it.
// The log is kept here, outside the machine, but how much of it counts is machine
// state (comp->rzx.rec.on*): a rollback of the machine - rewind, run-ahead - takes the
// log back with it, and whatever was logged past that point is written over.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "filetypes.h"
#include "szx.h"
#include "../cpu/Z80/z80.h"

typedef struct {
	int fetches;
	int ins;
} rrFrame;

typedef struct {
	int frame;		// frames logged before it
	int tstart;		// T into the frame it was taken in
	int noint;		// taken with interrupts on, between two of them
	unsigned char* data;	// the machine as raw SZX
	size_t len;
} rrSnap;

static unsigned char* rr_in = NULL;	// IN bytes of every frame, one after another
static size_t rr_inCap = 0;
static rrFrame* rr_frm = NULL;
static size_t rr_frmCap = 0;
static rrSnap* rr_snap = NULL;		// the start and each join
static size_t rr_snapCap = 0;
static unsigned rr_serial = 0;

unsigned rzx_recording = 0;
int rzx_rec_touched = 0;

static int rr_grow(void** buf, size_t* cap, size_t need, size_t elem) {
	if (need <= *cap) return 1;
	size_t n = *cap ? *cap : 256;
	while (n < need) n *= 2;
	unsigned char* p = (unsigned char*)realloc(*buf, n * elem);
	if (!p) return 0;
	memset(p + *cap * elem, 0, (n - *cap) * elem);
	*buf = p;
	*cap = n;
	return 1;
}

static void rr_snap_drop(size_t from) {
	for (size_t i = from; i < rr_snapCap; i++) {
		free(rr_snap[i].data);
		rr_snap[i].data = NULL;
	}
}

// the machine as it stands, as the snapshot block the playback starts from or
// joins at
static int rr_snapshot(Computer* comp, int noint) {
	size_t at = comp->rzx.rec.snaps;
	if (!rr_grow((void**)&rr_snap, &rr_snapCap, at + 1, sizeof(rrSnap))) return ERR_RZX_REC;
	szxBuf b;
	int err = szx_build(comp, &b);
	if (err != ERR_OK) {
		sb_free(&b);
		return err;
	}
	rr_snap_drop(at);		// what a rollback left behind
	rr_snap[at].frame = comp->rzx.rec.frames;
	rr_snap[at].tstart = comp_get_frame_tick(comp);
	rr_snap[at].noint = noint;
	rr_snap[at].data = b.data;
	rr_snap[at].len = b.len;
	comp->rzx.rec.snaps++;
	return ERR_OK;
}

// One frame ends: the playback raises its INT here. An empty frame is not one.
static void rr_boundary(Computer* comp) {
	comp->rzx.rec.t = 0;
	if (!comp->rzx.rec.fetch && !comp->rzx.rec.ins) return;
	if (!rr_grow((void**)&rr_frm, &rr_frmCap, comp->rzx.rec.frames + 1, sizeof(rrFrame))) return;
	rr_frm[comp->rzx.rec.frames].fetches = comp->rzx.rec.fetch;
	rr_frm[comp->rzx.rec.frames].ins = comp->rzx.rec.ins;
	comp->rzx.rec.inLen += comp->rzx.rec.ins;
	comp->rzx.rec.frames++;
	comp->rzx.rec.fetch = 0;
	comp->rzx.rec.ins = 0;
}

int rzx_rec_start(Computer* comp) {
	if (comp->rzx.play) return ERR_RZX_REC;
	comp->rzx.rec.on = 0;
	comp->rzx.rec.fetch = 0;
	comp->rzx.rec.ins = 0;
	comp->rzx.rec.frames = 0;
	comp->rzx.rec.inLen = 0;
	comp->rzx.rec.snaps = 0;
	comp->rzx.rec.t = 0;
	comp->cpu->flgINTOK = 0;
	rzx_rec_touched = 0;
	int err = rr_snapshot(comp, 0);		// playback raises no INT at the start
	if (err == ERR_OK) {
		comp->rzx.rec.on = 1;
		rzx_recording = ++rr_serial;
	}
	return err;
}

void rzx_rec_stop(Computer* comp) {
	comp->rzx.rec.on = 0;
	rzx_recording = 0;
}

// The machine is about to be changed from outside, or has been: the join is
// made before the next opcode, so edits between two steps of the debugger make
// one join, and the steps after them are logged on the edited machine.
void rzx_rec_touch(void) {
	if (rzx_recording) rzx_rec_touched = 1;
}

void rzx_rec_in(Computer* comp, int val) {
	size_t at = (size_t)comp->rzx.rec.inLen + comp->rzx.rec.ins;
	if (!rr_grow((void**)&rr_in, &rr_inCap, at + 1, 1)) return;
	rr_in[at] = val & 0xff;
	comp->rzx.rec.ins++;
}

// After each exec. A frame of the recording ends where the INT was taken; with
// interrupts off it ends once a frame's worth of T has gone, which keeps a frame
// within what other players allow (Fuse's sentinel is at 79000 T).
void rzx_rec_step(Computer* comp, int t) {
	CPU* cpu = comp->cpu;
	if (cpu->flgINTOK) {
		cpu->flgINTOK = 0;
		rr_boundary(comp);
	}
	comp->rzx.rec.t += t;
	if ((comp->rzx.rec.t >= comp_frame_ticks(comp)) && !cpu->flgIFF1 && !cpu->flgNOINT)
		rr_boundary(comp);
}

// The machine was changed from outside - a reset, a snapshot, a poke: the log
// goes on from a snapshot of it. With interrupts on the playback must not raise
// the INT it raises after a snapshot block, so the block says so.
int rzx_rec_join(Computer* comp) {
	rzx_rec_touched = 0;
	if (!comp->rzx.rec.on) return ERR_OK;
	comp->cpu->flgINTOK = 0;
	rr_boundary(comp);
	return rr_snapshot(comp, comp->cpu->flgIFF1 ? 1 : 0);
}

int rzx_rec_frames(Computer* comp) {
	return comp->rzx.rec.frames;
}

int rzx_rec_joins(Computer* comp) {
	return comp->rzx.rec.snaps > 0 ? comp->rzx.rec.snaps - 1 : 0;
}

// writing

struct rzxRecImage {
	int frames;
	rrFrame* frm;
	unsigned char* in;
	size_t inLen;
	int snaps;
	rrSnap* snap;
};

rzxRecImage* rzx_rec_take(Computer* comp) {
	rzxRecImage* img = (rzxRecImage*)calloc(1, sizeof(rzxRecImage));
	if (!img) return NULL;
	img->frames = comp->rzx.rec.frames;		// the frame under way is left out
	img->inLen = comp->rzx.rec.inLen;
	img->snaps = comp->rzx.rec.snaps;
	img->frm = (rrFrame*)malloc(img->frames * sizeof(rrFrame) + 1);
	img->in = (unsigned char*)malloc(img->inLen + 1);
	img->snap = (rrSnap*)calloc(img->snaps + 1, sizeof(rrSnap));
	int ok = img->frm && img->in && img->snap;
	if (ok) {
		memcpy(img->frm, rr_frm, img->frames * sizeof(rrFrame));
		memcpy(img->in, rr_in, img->inLen);
		for (int i = 0; ok && (i < img->snaps); i++) {
			img->snap[i] = rr_snap[i];
			img->snap[i].data = (unsigned char*)malloc(rr_snap[i].len);
			ok = (img->snap[i].data != NULL);
			if (ok) memcpy(img->snap[i].data, rr_snap[i].data, rr_snap[i].len);
		}
	}
	if (!ok) {
		rzx_rec_image_free(img);
		return NULL;
	}
	return img;
}

void rzx_rec_image_free(rzxRecImage* img) {
	if (!img) return;
	if (img->snap) {
		for (int i = 0; i < img->snaps; i++)
			free(img->snap[i].data);
	}
	free(img->snap);
	free(img->frm);
	free(img->in);
	free(img);
}

int rzx_rec_image_frames(rzxRecImage* img) {
	return img ? img->frames : 0;
}

static void rr_put(szxBuf* b, unsigned v, int n) {
	unsigned char x[4];
	for (int i = 0; i < n; i++)
		x[i] = (v >> (i * 8)) & 0xff;
	sb_put(b, x, n);
}

// zlib's own stream, as the format has it; the bytes raw when packing gains nothing
static int rr_pack(const unsigned char* src, size_t len, unsigned char** dst, size_t* dlen) {
	uLongf n = compressBound(len);
	*dst = (unsigned char*)malloc(n);
	if (!*dst) return 0;
	if ((compress2(*dst, &n, src, len, Z_BEST_COMPRESSION) != Z_OK) || (n >= len)) {
		free(*dst);
		*dst = NULL;
		return 0;
	}
	*dlen = n;
	return 1;
}

#define RZX_SNAP_PACKED		2
#define RZX_INPUT_PACKED	2

static void rr_block_snap(szxBuf* out, const rrSnap* s) {
	unsigned char* pk = NULL;
	size_t plen = 0;
	int packed = rr_pack(s->data, s->len, &pk, &plen);
	const unsigned char* body = packed ? pk : s->data;
	size_t blen = packed ? plen : s->len;
	rr_put(out, 0x30, 1);
	rr_put(out, (unsigned)(5 + 12 + blen), 4);
	rr_put(out, (packed ? RZX_SNAP_PACKED : 0) | (s->noint ? RZX_SNAP_NOINT : 0), 4);
	sb_put(out, "SZX\0", 4);
	rr_put(out, (unsigned)s->len, 4);
	sb_put(out, body, blen);
	free(pk);
}

// Frames as the format has them: fetches, IN count, the bytes - or a count of
// 0xffff for the very bytes of the frame before that carried any, which is how
// a frame spent waiting for a key comes out at four bytes.
static void rr_block_input(szxBuf* out, rzxRecImage* img, int from, int to, int tstart, size_t inAt) {
	szxBuf raw;
	memset(&raw, 0, sizeof(raw));
	const unsigned char* last = NULL;
	int lastN = -1;
	for (int i = from; i < to; i++) {
		int n = img->frm[i].ins;
		const unsigned char* p = img->in + inAt;
		rr_put(&raw, img->frm[i].fetches & 0xffff, 2);
		if ((n > 0) && (n == lastN) && !memcmp(p, last, n)) {
			rr_put(&raw, 0xffff, 2);
		} else {
			rr_put(&raw, n, 2);
			sb_put(&raw, p, n);
			last = p;
			lastN = n;
		}
		inAt += n;
	}
	unsigned char* pk = NULL;
	size_t plen = 0;
	int packed = rr_pack(raw.data, raw.len, &pk, &plen);
	const unsigned char* body = packed ? pk : raw.data;
	size_t blen = packed ? plen : raw.len;
	rr_put(out, 0x80, 1);
	rr_put(out, (unsigned)(5 + 13 + blen), 4);
	rr_put(out, to - from, 4);
	rr_put(out, 0, 1);
	rr_put(out, tstart, 4);
	rr_put(out, packed ? RZX_INPUT_PACKED : 0, 4);
	sb_put(out, body, blen);
	free(pk);
	sb_free(&raw);
}

int rzx_rec_image_write(rzxRecImage* img, const char* path, const char* name, int major, int minor, const char* custom) {
	if (!img || (img->snaps < 1)) return ERR_CANT_OPEN;
	szxBuf out;
	memset(&out, 0, sizeof(out));
	sb_put(&out, "RZX!", 4);
	rr_put(&out, 0, 1);
	rr_put(&out, 12, 1);
	rr_put(&out, 0, 4);
	// creator: the program, its version, and what else it has to say
	char nam[20];
	memset(nam, 0, sizeof(nam));
	strncpy(nam, name, sizeof(nam) - 1);
	size_t clen = custom ? strlen(custom) + 1 : 0;
	rr_put(&out, 0x10, 1);
	rr_put(&out, (unsigned)(5 + 20 + 4 + clen), 4);
	sb_put(&out, nam, 20);
	rr_put(&out, major, 2);
	rr_put(&out, minor, 2);
	if (clen) sb_put(&out, custom, clen);
	// each snapshot with the frames that follow it
	size_t inAt = 0;
	for (int s = 0; s < img->snaps; s++) {
		rr_block_snap(&out, &img->snap[s]);
		int from = img->snap[s].frame;
		int to = (s + 1 < img->snaps) ? img->snap[s + 1].frame : img->frames;
		if (to > img->frames) to = img->frames;
		if (to > from) {
			rr_block_input(&out, img, from, to, img->snap[s].tstart, inAt);
			for (int i = from; i < to; i++)
				inAt += img->frm[i].ins;
		}
	}
	int err = out.fail ? ERR_RZX_REC : ERR_OK;
	if (err == ERR_OK) {
		FILE* file = fopen(path, "wb");
		if (!file) {
			err = ERR_CANT_OPEN;
		} else {
			if (fwrite(out.data, 1, out.len, file) != out.len) err = ERR_CANT_OPEN;
			if (fclose(file)) err = ERR_CANT_OPEN;
		}
	}
	sb_free(&out);
	return err;
}

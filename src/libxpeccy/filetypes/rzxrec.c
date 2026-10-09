// RZX recording: the input log of the running machine, written out as rzx.c reads it.
// The log is kept here, outside the machine, but how much of it counts is machine
// state (comp->rzx.rec): a rollback of the machine - rewind, run-ahead - takes the
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

enum {
	RR_SNA = 0,		// as rzx.c's scratch file numbers them
	RR_Z80,
	RR_SZX
};

typedef struct {
	int frame;		// frames logged before it
	int tstart;		// T into the frame it was taken in
	int noint;		// taken with interrupts on, between two of them
	int mark;		// a bookmark: a point to roll back to, which finalizing drops
	int type;		// RR_*
	unsigned char* data;	// the machine, unpacked
	size_t len;
} rrSnap;

static unsigned char* rr_in = NULL;	// IN bytes of every frame, one after another
static size_t rr_inCap = 0;
static rrFrame* rr_frm = NULL;
static size_t rr_frmCap = 0;
static rrSnap* rr_snap = NULL;		// the start, the joins and the bookmarks
static size_t rr_snapCap = 0;
static unsigned rr_serial = 0;

unsigned rzx_recording = 0;
int rzx_rec_touched = 0;
int rzx_rec_marking = 0;

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

// a snapshot block, the data taken over (malloc'd)
static int rr_snap_put(Computer* comp, int type, unsigned char* data, size_t len, int noint, int mark) {
	size_t at = comp->rzx.rec.snaps;
	if (!rr_grow((void**)&rr_snap, &rr_snapCap, at + 1, sizeof(rrSnap))) {
		free(data);
		return ERR_RZX_REC;
	}
	rr_snap_drop(at);		// what a rollback left behind
	rr_snap[at].frame = comp->rzx.rec.frames;
	rr_snap[at].tstart = comp_get_frame_tick(comp);
	rr_snap[at].noint = noint;
	rr_snap[at].mark = mark;
	rr_snap[at].type = type;
	rr_snap[at].data = data;
	rr_snap[at].len = len;
	comp->rzx.rec.snaps++;
	return ERR_OK;
}

// the machine as it stands, as the snapshot block the playback starts from or
// joins at
static int rr_snapshot(Computer* comp, int noint, int mark) {
	szxBuf b;
	// The NMI key is held for a while, but every NMI taken makes a join of its
	// own: one left pending in the snapshot would be taken again on playback.
	int nmi = comp->flgNMIRQ;
	comp->flgNMIRQ = 0;
	int err = szx_build(comp, &b);
	comp->flgNMIRQ = nmi;
	if (err != ERR_OK) {
		sb_free(&b);
		return err;
	}
	return rr_snap_put(comp, RR_SZX, b.data, b.len, noint, mark);
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

static void rr_begin(Computer* comp) {
	memset(&comp->rzx.rec, 0, sizeof(rzxRecPos));
	rzx_rec_touched = 0;
	rzx_rec_marking = 0;
}

static void rr_go(Computer* comp) {
	comp->rzx.rec.on = 1;
	rzx_recording = ++rr_serial;
}

int rzx_rec_start(Computer* comp) {
	if (comp->rzx.play) return ERR_RZX_REC;
	rr_begin(comp);
	int err = rr_snapshot(comp, 0, 0);		// playback raises no INT at the start
	if (err == ERR_OK) rr_go(comp);
	return err;
}

// A recording being played goes on as this one: what was played to here is
// the log already, the frame under way included as far as it has got, and the
// machine stands where those frames took it. What came after is left out.
int rzx_rec_take_over(Computer* comp) {
	FILE* file = comp->rzx.file;
	if (!comp->rzx.play || !file) return ERR_RZX_REC;
	long long here = x_ftell(file);
	rr_begin(comp);
	unsigned char last[0x10000];		// a repeat frame's bytes: the last that carried any
	int lastN = 0;
	int target = comp->rzx.fCurrent;
	int done = 0;
	int err = ERR_OK;
	int found = 0;
	rewind(file);
	while (!found && (err == ERR_OK)) {
		int type = fgetc(file);
		if ((type == EOF) || (type == 0xff)) break;
		int len = fgeti(file);
		long long pos = x_ftell(file);
		if (type == 0x30) {
			int kind = fgetc(file);
			unsigned char* data = (len > 1) ? (unsigned char*)malloc(len - 1) : NULL;
			if (!data || (fread(data, len - 1, 1, file) != 1)) {
				free(data);
				err = ERR_RZX_REC;
				break;
			}
			comp->rzx.rec.frames = done;
			err = rr_snap_put(comp, kind & 0x3f, data, len - 1, (kind & 0x80) ? 1 : 0, (kind & 0x40) ? 1 : 0);
		} else if (type == 0x80) {
			int count = fgeti(file);
			int tstart = fgeti(file);
			if (comp->rzx.rec.snaps && (rr_snap[comp->rzx.rec.snaps - 1].frame == done))
				rr_snap[comp->rzx.rec.snaps - 1].tstart = tstart;
			for (int i = 0; (i < count) && (err == ERR_OK); i++) {
				int fetch = fgetw(file);
				int n = fgetw(file);
				if (n == 0xffff) {
					n = lastN;
				} else {
					if (n && (fread(last, n, 1, file) != 1)) err = ERR_RZX_REC;
					lastN = n;
				}
				size_t at = (size_t)comp->rzx.rec.inLen;
				int take = (done == target) ? comp->rzx.frm.pos : n;	// the frame under way: as far as played
				if (take > n) take = n;
				if (!rr_grow((void**)&rr_in, &rr_inCap, at + take + 1, 1)) err = ERR_RZX_REC;
				if (err != ERR_OK) break;
				memcpy(rr_in + at, last, take);
				if (done == target) {
					comp->rzx.rec.frames = done;
					comp->rzx.rec.fetch = fetch - comp->rzx.frm.fetches;
					comp->rzx.rec.ins = take;
					found = 1;
					break;
				}
				// as the file has it, an empty frame too: the playback counts those
				if (!rr_grow((void**)&rr_frm, &rr_frmCap, done + 1, sizeof(rrFrame))) {
					err = ERR_RZX_REC;
					break;
				}
				rr_frm[done].fetches = fetch;
				rr_frm[done].ins = n;
				comp->rzx.rec.inLen += n;
				done++;
				comp->rzx.rec.frames = done;
			}
		}
		if (!found) x_fseek(file, pos + len, SEEK_SET);
	}
	x_fseek(file, here, SEEK_SET);
	if ((err == ERR_OK) && (comp->rzx.rec.snaps < 1)) err = ERR_RZX_REC;
	if (err != ERR_OK) {
		memset(&comp->rzx.rec, 0, sizeof(rzxRecPos));
		return err;
	}
	rr_go(comp);
	return ERR_OK;
}

void rzx_rec_stop(Computer* comp) {
	comp->rzx.rec.on = 0;
	rzx_recording = 0;
	rzx_rec_marking = 0;
	rzx_rec_touched = 0;
}

// The machine is about to be changed from outside, or has been: the join is
// made before the next opcode, so edits between two steps of the debugger make
// one join, and the steps after them are logged on the edited machine.
void rzx_rec_touch(void) {
	if (rzx_recording) rzx_rec_touched = 1;
}

// A bookmark is made where a frame ends, as the frame's INT is about to be
// taken: what the playback does at a snapshot block - load it, raise the INT -
// is then what the machine did.
void rzx_rec_bookmark(void) {
	if (rzx_recording) rzx_rec_marking = 1;
}

void rzx_rec_in(Computer* comp, int val) {
	size_t at = (size_t)comp->rzx.rec.inLen + comp->rzx.rec.ins;
	if (!rr_grow((void**)&rr_in, &rr_inCap, at + 1, 1)) return;
	rr_in[at] = val & 0xff;
	comp->rzx.rec.ins++;
}

static int rr_int_now(CPU* cpu) {
	int req = cpu->intrq & cpu->inten;
	return (req & Z80_INT) && !(req & Z80_NMI) && cpu->flgIFF1 && !cpu->flgNOINT && cpu->flgACK;
}

// The machine was changed from outside - a reset, a snapshot, a poke: the log
// goes on from a snapshot of it. With interrupts on the playback must not raise
// the INT it raises after a snapshot block, so the block says so.
static int rr_join(Computer* comp) {
	rzx_rec_touched = 0;
	if (!comp->rzx.rec.on) return ERR_OK;
	rr_boundary(comp);
	return rr_snapshot(comp, comp->cpu->flgIFF1 ? 1 : 0, 0);
}

// Before an exec, when a join or a bookmark is wanted.
void rzx_rec_pre(Computer* comp) {
	if (rzx_rec_touched) rr_join(comp);
	if (rzx_rec_marking && rr_int_now(comp->cpu)) {
		rzx_rec_marking = 0;
		rr_boundary(comp);
		rr_snapshot(comp, 0, 1);
	}
}

// After each exec. A frame of the recording ends where the INT was taken; with
// interrupts off it ends once a frame's worth of T has gone, which keeps a frame
// within what other players allow (Fuse's sentinel is at 79000 T).
void rzx_rec_step(Computer* comp, int t) {
	CPU* cpu = comp->cpu;
	if (cpu->flgINTOK)
		rr_boundary(comp);
	comp->rzx.rec.t += t;
	if (!cpu->flgIFF1 && !cpu->flgNOINT && (comp->rzx.rec.t >= comp_frame_ticks(comp))) {
		rr_boundary(comp);
		if (rzx_rec_marking) {		// interrupts off: the INT the playback raises is not taken
			rzx_rec_marking = 0;
			rr_snapshot(comp, 0, 1);
		}
	}
}

static int rr_load(Computer* comp, const rrSnap* s) {
	if (s->type == RR_SZX) return loadSZX_buf(comp, s->data, s->len);
	FILE* file = fopen_tmp();
	if (!file) return ERR_CANT_OPEN;
	int err = (fwrite(s->data, s->len, 1, file) == 1) ? ERR_OK : ERR_CANT_OPEN;
	rewind(file);
	if (err == ERR_OK)
		err = (s->type == RR_Z80) ? loadZ80_f(comp, file) : loadSNA_f(comp, file, s->len);
	fclose(file);
	return err;
}

// Back to the last bookmark, or to the start: the machine as it was there and
// the log cut to there. Frames back, -1 when it could not be done.
int rzx_rec_rollback(Computer* comp) {
	if (!comp->rzx.rec.on) return -1;
	int i = comp->rzx.rec.snaps - 1;
	while ((i > 0) && !rr_snap[i].mark) i--;
	if (i < 0) return -1;
	rzxRecPos pos = comp->rzx.rec;
	if (rr_load(comp, &rr_snap[i]) != ERR_OK) {
		comp->rzx.rec = pos;
		return -1;
	}
	int back = pos.frames - rr_snap[i].frame;
	pos.frames = rr_snap[i].frame;
	pos.inLen = 0;
	for (int f = 0; f < pos.frames; f++)
		pos.inLen += rr_frm[f].ins;
	pos.snaps = i + 1;
	pos.fetch = 0;
	pos.ins = 0;
	pos.t = 0;
	comp->rzx.rec = pos;
	rzx_rec_touched = 0;
	rzx_rec_marking = 0;
	return back;
}

int rzx_rec_frames(Computer* comp) {
	return comp->rzx.rec.frames;
}

static int rr_marks(Computer* comp) {
	int n = 0;
	for (int i = 1; i < comp->rzx.rec.snaps; i++)
		if (rr_snap[i].mark) n++;
	return n;
}

int rzx_rec_joins(Computer* comp) {
	return comp->rzx.rec.snaps > 0 ? comp->rzx.rec.snaps - 1 - rr_marks(comp) : 0;
}

// writing

struct rzxRecImage {
	int frames;
	rrFrame* frm;
	unsigned char* in;
	size_t inLen;
	int snaps;
	rrSnap* snap;
	unsigned char* creator;		// a file read back keeps its creator block as it was
	size_t creatorLen;
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
	free(img->creator);
	free(img);
}

int rzx_rec_image_frames(rzxRecImage* img) {
	return img ? img->frames : 0;
}

// reading a file back, to finalize it

// an input block's stream, of a size not known beforehand
static unsigned char* rr_unpack(const unsigned char* src, size_t len, size_t* out) {
	size_t cap = len * 4 + 1024;
	unsigned char* dst = (unsigned char*)malloc(cap);
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	if (!dst || (inflateInit(&zs) != Z_OK)) {
		free(dst);
		return NULL;
	}
	zs.next_in = (unsigned char*)src;
	zs.avail_in = len;
	int res;
	do {
		if (zs.total_out == cap) {
			unsigned char* p = (unsigned char*)realloc(dst, cap * 2);
			if (!p) break;
			dst = p;
			cap *= 2;
		}
		zs.next_out = dst + zs.total_out;
		zs.avail_out = cap - zs.total_out;
		res = inflate(&zs, Z_NO_FLUSH);
	} while (res == Z_OK);
	*out = zs.total_out;
	inflateEnd(&zs);
	if (res != Z_STREAM_END) {
		free(dst);
		return NULL;
	}
	return dst;
}

static int rr_img_snap(rzxRecImage* img, int type, unsigned char* data, size_t len, unsigned flags) {
	rrSnap* s = (rrSnap*)realloc(img->snap, (img->snaps + 1) * sizeof(rrSnap));
	if (!s) return 0;
	img->snap = s;
	s += img->snaps++;
	memset(s, 0, sizeof(rrSnap));
	s->frame = img->frames;
	s->type = type;
	s->data = data;
	s->len = len;
	s->noint = (flags & RZX_SNAP_NOINT) ? 1 : 0;
	s->mark = (flags & RZX_SNAP_MARK) ? 1 : 0;
	return 1;
}

// one walk to check the block and size it, one to take its frames
static int rr_img_frames(rzxRecImage* img, const unsigned char* data, size_t len, int count) {
	size_t total = 0;
	for (int pass = 0; pass < 2; pass++) {
		const unsigned char* p = data;
		const unsigned char* end = data + len;
		const unsigned char* last = NULL;
		int lastN = 0;
		if (pass) {
			rrFrame* f = (rrFrame*)realloc(img->frm, (img->frames + count + 1) * sizeof(rrFrame));
			if (f) img->frm = f;
			unsigned char* in = (unsigned char*)realloc(img->in, img->inLen + total + 1);
			if (in) img->in = in;
			if (!f || !in) return 0;
		}
		for (int i = 0; i < count; i++) {
			if (p + 4 > end) return 0;
			int fetch = rd_word(p);
			int n = rd_word(p + 2);
			p += 4;
			const unsigned char* bytes;
			if (n == 0xffff) {
				n = lastN;
				bytes = last;
			} else {
				if (p + n > end) return 0;
				bytes = p;
				last = p;
				lastN = n;
				p += n;
			}
			if (!pass) {
				total += n;
				continue;
			}
			img->frm[img->frames].fetches = fetch;
			img->frm[img->frames].ins = n;
			if (n) memcpy(img->in + img->inLen, bytes, n);
			img->inLen += n;
			img->frames++;
		}
	}
	return 1;
}

rzxRecImage* rzx_rec_image_read(const char* path, int* err) {
	*err = ERR_CANT_OPEN;
	FILE* file = fopen(path, "rb");
	if (!file) return NULL;
	size_t size = fgetSize(file);
	unsigned char* buf = (unsigned char*)malloc(size + 1);
	size_t got = buf ? fread(buf, 1, size, file) : 0;
	fclose(file);
	rzxRecImage* img = (rzxRecImage*)calloc(1, sizeof(rzxRecImage));
	if (!buf || !img || (got != size) || (size < 10) || memcmp(buf, "RZX!", 4)) {
		*err = (buf && (got == size)) ? ERR_RZX_SIGN : ERR_CANT_OPEN;
		free(buf);
		rzx_rec_image_free(img);
		return NULL;
	}
	*err = ERR_OK;
	size_t at = 10;
	int tsPending = 0;		// the input block after a snapshot gives it its T
	while ((*err == ERR_OK) && (at + 5 <= size)) {
		int id = buf[at];
		size_t len = rd_dword(buf + at + 1);
		if ((len < 5) || (at + len > size)) break;	// what follows is junk
		const unsigned char* b = buf + at + 5;
		size_t blen = len - 5;
		if ((id == 0x10) && !img->creator) {
			img->creator = (unsigned char*)malloc(len);
			if (img->creator) {
				memcpy(img->creator, buf + at, len);
				img->creatorLen = len;
			}
		} else if ((id == 0x30) && (blen >= 12)) {
			unsigned flags = rd_dword(b);
			size_t usl = rd_dword(b + 8);
			int type = rzxGetSnapType((char*)b + 4);
			if ((flags & 1) || (type > RR_SZX)) {
				*err = ERR_RZX_REC;		// a snapshot kept in a file of its own
				break;
			}
			size_t dlen = (flags & 2) ? usl : blen - 12;
			unsigned char* data = (unsigned char*)malloc(dlen + 1);
			int ok = (data != NULL);
			if (ok && (flags & 2)) {
				ok = szx_inflate(b + 12, blen - 12, data, usl);
			} else if (ok) {
				memcpy(data, b + 12, dlen);
			}
			if (!ok || !rr_img_snap(img, type, data, dlen, flags)) {
				free(data);
				*err = ERR_RZX_UNPACK;
				break;
			}
			tsPending = 1;
		} else if ((id == 0x80) && (blen >= 13)) {
			int count = rd_dword(b);
			int tstart = rd_dword(b + 5);
			unsigned flags = rd_dword(b + 9);
			if (flags & 1) {
				*err = ERR_RZX_CRYPT;
				break;
			}
			if (tsPending && img->snaps) img->snap[img->snaps - 1].tstart = tstart;
			tsPending = 0;
			size_t dlen = blen - 13;
			unsigned char* data = (flags & 2) ? rr_unpack(b + 13, blen - 13, &dlen) : NULL;
			if (((flags & 2) && !data) || !rr_img_frames(img, data ? data : b + 13, dlen, count))
				*err = ERR_RZX_UNPACK;
			free(data);
		}
		at += len;
	}
	free(buf);
	if ((*err == ERR_OK) && (img->snaps < 1)) *err = ERR_RZX_REC;
	if (*err != ERR_OK) {
		rzx_rec_image_free(img);
		return NULL;
	}
	return img;
}

#define RZX_SNAP_PACKED		2
#define RZX_INPUT_PACKED	2

static const char* rr_ext[] = {"SNA", "Z80", "SZX"};

// zlib's own stream, as the format has it; the bytes raw when packing gains nothing
static void rr_block_snap(szxBuf* out, const rrSnap* s) {
	szxBuf body;
	memset(&body, 0, sizeof(body));
	int packed = sb_deflate(&body, s->data, s->len);
	char ext[4] = {0, 0, 0, 0};
	memcpy(ext, rr_ext[(s->type >= RR_SNA) && (s->type <= RR_SZX) ? s->type : RR_SZX], 3);
	sb_byte(out, 0x30);
	sb_dword(out, (unsigned)(5 + 12 + body.len));
	sb_dword(out, (packed ? RZX_SNAP_PACKED : 0) | (s->noint ? RZX_SNAP_NOINT : 0) | (s->mark ? RZX_SNAP_MARK : 0));
	sb_put(out, ext, 4);
	sb_dword(out, (unsigned)s->len);
	sb_put(out, body.data, body.len);
	sb_free(&body);
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
		sb_word(&raw, img->frm[i].fetches & 0xffff);
		if ((n > 0) && (n == lastN) && !memcmp(p, last, n)) {
			sb_word(&raw, 0xffff);
		} else {
			sb_word(&raw, n);
			sb_put(&raw, p, n);
			last = p;
			lastN = n;
		}
		inAt += n;
	}
	szxBuf body;
	memset(&body, 0, sizeof(body));
	int packed = sb_deflate(&body, raw.data, raw.len);
	sb_byte(out, 0x80);
	sb_dword(out, (unsigned)(5 + 13 + body.len));
	sb_dword(out, to - from);
	sb_byte(out, 0);
	sb_dword(out, tstart);
	sb_dword(out, packed ? RZX_INPUT_PACKED : 0);
	sb_put(out, body.data, body.len);
	sb_free(&body);
	sb_free(&raw);
}

// The file: the creator, then each snapshot with the frames that follow it.
// Finalized, the bookmarks go and so does a snapshot nothing is played after;
// a join stays, as the machine does not get there by playing.
int rzx_rec_image_write(rzxRecImage* img, const char* path, const char* name, int major, int minor, const char* custom, int finalize) {
	if (!img || (img->snaps < 1)) return ERR_CANT_OPEN;
	szxBuf out;
	memset(&out, 0, sizeof(out));
	sb_put(&out, "RZX!", 4);
	sb_byte(&out, 0);
	sb_byte(&out, 12);
	sb_dword(&out, 0);
	if (img->creator) {
		sb_put(&out, img->creator, img->creatorLen);
	} else {
		char nam[20];
		memset(nam, 0, sizeof(nam));
		strncpy(nam, name, sizeof(nam) - 1);
		size_t clen = custom ? strlen(custom) + 1 : 0;
		sb_byte(&out, 0x10);
		sb_dword(&out, (unsigned)(5 + 20 + 4 + clen));
		sb_put(&out, nam, 20);
		sb_word(&out, major);
		sb_word(&out, minor);
		if (clen) sb_put(&out, custom, clen);
	}
	int* keep = (int*)malloc(img->snaps * sizeof(int));
	int kept = 0;
	for (int s = 0; keep && (s < img->snaps); s++) {
		if (finalize && (s > 0) && (img->snap[s].mark || (img->snap[s].frame >= img->frames)))
			continue;
		keep[kept++] = s;
	}
	size_t inAt = 0;
	for (int k = 0; keep && (k < kept); k++) {
		rrSnap* s = &img->snap[keep[k]];
		rr_block_snap(&out, s);
		int from = s->frame;
		int to = (k + 1 < kept) ? img->snap[keep[k + 1]].frame : img->frames;
		if (to > img->frames) to = img->frames;
		if (to > from) {
			rr_block_input(&out, img, from, to, s->tstart, inAt);
			for (int i = from; i < to; i++)
				inAt += img->frm[i].ins;
		}
	}
	int err = (out.fail || !keep) ? ERR_RZX_REC : ERR_OK;
	free(keep);
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

// What the recording holds so far, as rzx_info() lists a file: the creator
// given, each snapshot and the frames after it. The frame under way is left out.
int rzx_rec_info(Computer* comp, rzxInfo* inf, const rzxBlock* creator) {
	memset(inf, 0, sizeof(rzxInfo));
	if (!comp->rzx.rec.on) return ERR_RZX_REC;
	rzxBlock* blk = creator ? rzx_info_add(inf) : NULL;
	if (blk) *blk = *creator;
	int snaps = comp->rzx.rec.snaps;
	for (int i = 0; i < snaps; i++) {
		const rrSnap* s = &rr_snap[i];
		if (!(blk = rzx_info_add(inf))) break;
		blk->id = 0x30;
		blk->frame = s->frame;
		blk->flags = (s->noint ? RZX_SNAP_NOINT : 0) | (s->mark ? RZX_SNAP_MARK : 0);
		blk->usl = (int)s->len;
		memcpy(blk->ext, rr_ext[(s->type >= RR_SNA) && (s->type <= RR_SZX) ? s->type : RR_SZX], 3);
		blk->hw = rzx_snap_hardware(s->type, (int)s->len, s->data, (int)s->len);
		inf->snaps++;
		int to = (i + 1 < snaps) ? rr_snap[i + 1].frame : comp->rzx.rec.frames;
		if ((to <= s->frame) && (i + 1 < snaps)) continue;
		if (!(blk = rzx_info_add(inf))) break;
		blk->id = 0x80;
		blk->frame = s->frame;
		blk->frames = to - s->frame;
		blk->tstart = s->tstart;
		inf->frames += blk->frames;
	}
	return ERR_OK;
}

#include "filetypes.h"

// +0x00 signature (23)
// +0x17 DiskInfo\r\n
// +0x22 disk info block

#pragma pack (push, 1)

typedef struct {
	char byte0d0f[4];	// unused
	unsigned char track;
	unsigned char side;
	unsigned char dataRate;	// unknown, SD, HD, ED
	unsigned char recMode;	// unknown, FM, MFM
	unsigned char secSize;
	unsigned char secCount;
	unsigned char gap3Size;
	unsigned char filler;
	char sectorInfo[0x100 - 0x18];	// DSK_MAXSEC x 8 bytes
} TrackInfBlock;

typedef struct {
	unsigned char track;
	unsigned char side;
	unsigned char sector;
	unsigned char size;
	unsigned char sr1;
	unsigned char sr2;
	unsigned char bslo;
	unsigned char bshi;
} SectorInfBlock;	// 8 bytes

#pragma pack (pop)

//static const char edsksgn[] =	"EXTENDED CPC DSK File\r\n";
//static const char dsksgn[] =	"MV - CPCEMU Disk-File\r\n";
static const char edsksgn[] =	"EXTENDED";
static const char dsksgn[] =	"MV - CPC";

#define DSK_MAXSEC	29	// the sectors a Track-Info block has room for

// A sector as the image gives it
typedef struct {
	unsigned char c, h, r, n;
	unsigned char st1, st2;	// what the dumping controller answered reading it
	int len;		// bytes of one read
	int copies;		// reads kept: more than one is a weak sector
	int made;		// the reads after the first made up here
	const unsigned char* data;	// copies x len, in the file's buffer
} dskSec;

// The track as the uPD765's Format lays it out in MFM: gap 4a, the index mark,
// gap 1, then for each sector sync and ID, gap 2, sync and data, gap 3
#define DSK_GAP4A	80
#define DSK_GAP1	50
#define DSK_GAP2	22
#define DSK_SYNC	12
#define DSK_GAP3_P3	0x52	// gap 3 +3DOS formats with: its format spec, +3 ROM 2 at 1C5C
#define DSK_LEAD	(DSK_GAP4A + DSK_SYNC + 4 + DSK_GAP1)
#define DSK_SEC_FIX	(DSK_SYNC + 4 + 4 + 2 + DSK_GAP2 + DSK_SYNC + 4 + 2)	// a sector but its data and gap 3

typedef struct {
	flpTrack* t;
	int pos;
	int len;
} dskPen;

static void pen_put(dskPen* p, unsigned char b, unsigned char fld) {
	if (p->pos >= p->len) return;		// past the index: the rest is not on the track
	p->t->byte[p->pos] = b;
	p->t->field[p->pos] = fld;
	p->pos++;
}

static void pen_run(dskPen* p, unsigned char b, int n) {
	while (n-- > 0) pen_put(p, b, 0);
}

// sync, A1 A1 A1 and the mark; returns where its CRC starts counting
static int pen_mark(dskPen* p, unsigned char mark) {
	pen_run(p, 0x00, DSK_SYNC);
	int st = p->pos;
	pen_put(p, 0xa1, 0x80);
	pen_put(p, 0xa1, 0x0f);
	pen_put(p, 0xa1, 0x0f);
	pen_put(p, mark, 0x0f);
	return st;
}

// the CRC of what is laid down from st, here; bad: written wrong, as the image says
static void pen_crc(dskPen* p, int st, int bad) {
	if (p->pos + 2 > p->len) {		// the field runs off the track: no CRC on it
		p->pos = p->len;
		return;
	}
	unsigned short crc = getCrc(p->t->byte + st, p->pos - st);
	if (bad) crc ^= 0xffff;
	pen_put(p, (crc >> 8) & 0xff, 4);
	pen_put(p, crc & 0xff, 4);
}

// a sector's ID field and its CRC
static void pen_id(dskPen* p, const dskSec* x) {
	int st = pen_mark(p, 0xfe);
	pen_put(p, x->c, 1);
	pen_put(p, x->h, 1);
	pen_put(p, x->r, 1);
	pen_put(p, x->n, 1);
	pen_crc(p, st, (x->st1 & F_SR1_DE) && !(x->st2 & F_SR2_DD));	// a CRC error in the ID
}

// Lay a track out from its sectors. What does not fit takes gap 3 first, then the
// lead-in; a data field longer than the room left (N 6 and up) runs to the index
// with no CRC after it, as the Format command leaves one.
static void dsk_form(Floppy* flp, int tr, dskSec* s, int n, int gap3) {
	if ((tr < 0) || (tr > 255) || (n < 1)) return;
	dskPen p = {&flp->data[tr], 0, flp->trklen};
	memset(p.t->byte, 0x4e, TRKLEN_HD);
	memset(p.t->field, 0, TRKLEN_HD);
	memset(p.t->map, 0, sizeof(p.t->map));
	long need = 0;
	for (int i = 0; i < n; i++)
		need += DSK_SEC_FIX + s[i].len;
	int lead = DSK_LEAD;
	if (lead + need + (long)n * gap3 > p.len) {
		long g = (p.len - lead - need) / n;
		gap3 = (g > 0) ? (int)g : 0;
		if (lead + need > p.len) lead = 0;
	}
	if (lead) {
		pen_run(&p, 0x4e, DSK_GAP4A);
		pen_run(&p, 0x00, DSK_SYNC);
		pen_run(&p, 0xc2, 3);
		pen_run(&p, 0xfc, 1);
		pen_run(&p, 0x4e, DSK_GAP1);
	}
	for (int i = 0; (i < n) && (p.pos < p.len); i++) {
		dskSec* x = &s[i];
		pen_id(&p, x);
		pen_run(&p, 0x4e, DSK_GAP2);
		if (!(x->st2 & F_SR2_MD)) {			// there is a data field
			int del = (x->st2 & F_SR2_CM) ? 1 : 0;
			int st = pen_mark(&p, del ? 0xf8 : 0xfb);
			int dpos = p.pos;
			if ((dpos < p.len) && !p.t->map[x->r])
				p.t->map[x->r] = dpos;
			for (int k = 0; k < x->len; k++)
				pen_put(&p, x->data[k], del ? 3 : 2);
			if ((x->copies > 1) && (dpos + x->len <= p.len))
				flp_add_weak(flp, tr, dpos, x->len, x->copies, x->data, x->made);
			if (x->len >= flp_seclen(x->n))
				pen_crc(&p, st, x->st2 & F_SR2_DD);	// a CRC error in the data
		}
		pen_run(&p, 0x4e, gap3);
	}
}

static int dsk_is_mark(const unsigned char* p, int fe) {
	return (p[0] == 0xa1) && (p[1] == 0xa1) && (p[2] == 0xa1)
		&& (fe ? (p[3] == 0xfe) : ((p[3] & 0xfc) == 0xf8));	// F8..FB: a data mark
}

// How many of the track's other IDs a sector's dumped data holds: read with a
// large N, a sector of a track whose sectors overlap brings the rest along
static int dsk_ids_in(const dskSec* x, const dskSec* s, int n) {
	int cnt = 0;
	if (x->copies > 1) return 0;
	for (int i = 0; i + 8 <= x->len; i++) {
		if (!dsk_is_mark(x->data + i, 1)) continue;
		for (int k = 0; k < n; k++) {
			if ((&s[k] != x) && !memcmp(x->data + i + 4, &s[k].c, 4)) {
				cnt++;
				break;
			}
		}
	}
	return cnt;
}

// The marks on a track laid out byte by byte: an ID's four bytes and CRC, a data
// field up to the sync before the next mark
static void dsk_mark_fields(flpTrack* t, int len) {
	memset(t->field, 0, TRKLEN_HD);
	memset(t->map, 0, sizeof(t->map));
	int r = -1;
	for (int i = 0; i + 4 <= len; i++) {
		if (!dsk_is_mark(t->byte + i, 1) && !dsk_is_mark(t->byte + i, 0)) continue;
		t->field[i] = 0x80;
		t->field[i + 1] = t->field[i + 2] = t->field[i + 3] = 0x0f;
		if (t->byte[i + 3] == 0xfe) {
			for (int k = 4; (k < 10) && (i + k < len); k++)
				t->field[i + k] = (k < 8) ? 1 : 4;
			r = (i + 6 < len) ? t->byte[i + 6] : -1;
			i += 9;
		} else {
			int f = ((t->byte[i + 3] & 0xfe) == 0xf8) ? 3 : 2;
			int st = i + 4, end = st;
			while ((end + 3 <= len) && !((t->byte[end] == 0xa1) && (t->byte[end + 1] == 0xa1) && (t->byte[end + 2] == 0xa1)))
				end++;
			end = (end + 3 <= len) ? end - DSK_SYNC : len;
			for (int k = st; k < end; k++)
				t->field[k] = f;
			if ((r >= 0) && !t->map[r]) t->map[r] = st;
			r = -1;
			i = (end > i) ? end - 1 : i + 3;
		}
	}
}

// A track whose sectors overlap: the sector whose dumped data holds the others is
// laid down with its own ID in front, and its bytes are the rest of the track as
// they came off the disk - IDs, gaps and CRCs of the others included
static void dsk_form_stream(Floppy* flp, int tr, dskSec* x) {
	dskPen p = {&flp->data[tr], 0, flp->trklen};
	memset(p.t->byte, 0x4e, TRKLEN_HD);
	pen_id(&p, x);
	pen_run(&p, 0x4e, DSK_GAP2);
	pen_mark(&p, (x->st2 & F_SR2_CM) ? 0xf8 : 0xfb);
	for (int k = 0; k < x->len; k++)
		pen_put(&p, x->data[k], 0);
	dsk_mark_fields(p.t, p.len);
}

// Speedlock +3's weak sector (T0 R2 N2, data CRC error) kept as one read: the k-th read
// made up is it shifted by k bits after its opening run, as dumps of several reads show
#define DSK_WEAK_READS	8
#define DSK_WEAK_RUN	16	// an opening run shorter than this is part of the weak bits

static const unsigned char* dsk_weak_reads(unsigned char* dst, const unsigned char* src) {
	int from = 1;
	while ((from < 0x200) && (src[from] == src[0])) from++;
	if (from < DSK_WEAK_RUN) from = 0;
	for (int k = 0; k < DSK_WEAK_READS; k++) {
		unsigned char* p = dst + k * 0x200;
		memcpy(p, src, 0x200);
		for (int i = from; k && (i < 0x200); i++)
			p[i] = (((i ? src[i - 1] : 0) << (8 - k)) | (src[i] >> k)) & 0xff;
	}
	return dst;
}

static unsigned char* dsk_file(const char* name, long* len) {
	FILE* file = fopen(name, "rb");
	if (!file) return NULL;
	unsigned char* buf = NULL;
	size_t sz = fgetSize(file);
	if ((sz > 0) && (sz < 0x4000000) && (buf = (unsigned char*)malloc(sz))) {
		if (fread(buf, 1, sz, file) == sz) {
			*len = (long)sz;
		} else {
			free(buf);
			buf = NULL;
		}
	}
	fclose(file);
	return buf;
}

int dsk_read(Floppy* flp, const char* name) {
	long len = 0;
	unsigned char* buf = dsk_file(name, &len);
	if (!buf) return ERR_CANT_OPEN;
	int ext = -1;
	if (len < 0x100) {
		ext = -1;
	} else if (!strncmp((char*)buf, edsksgn, strlen(edsksgn))) {
		ext = 1;
	} else if (!strncmp((char*)buf, dsksgn, strlen(dsksgn))) {
		ext = 0;
	}
	if (ext < 0) {
		free(buf);
		return ERR_DSK_SIGN;
	}
	int err = ERR_OK;
	diskClear(flp);
	int trkcnt = buf[0x30];
	int sidcnt = buf[0x31];
	int tlen = buf[0x32] | (buf[0x33] << 8);
	int tracks = trkcnt * sidcnt;
	if (ext && (tracks > 0x100 - 0x34)) tracks = 0x100 - 0x34;	// all the size table holds
	long pos = 0x100;
	int tr = 0;
	int cut = 0;
	dskSec secs[DSK_MAXSEC];
	unsigned char (*wbuf)[DSK_WEAK_READS * 0x200] = NULL;	// made-up weak reads, once one is met
	for (int i = 0; (i < tracks) && !cut; i++) {
		long rtlen = ext ? (buf[0x34 + i] << 8) : tlen;
		if (rtlen > 0) {
			if (pos + 0x100 > len) break;			// the file ends: keep what is loaded
			if (strncmp((char*)buf + pos, "Track-Info\r\n", 12)) {
				err = ERR_DSK_SIGN;
				break;
			}
			TrackInfBlock* tib = (TrackInfBlock*)(buf + pos + 12);
			SectorInfBlock* sib = (SectorInfBlock*)tib->sectorInfo;
			int nsec = tib->secCount;
			if (nsec > DSK_MAXSEC) nsec = DSK_MAXSEC;	// all the sector info there is
			long dp = pos + 0x100;
			int sc;
			int wn = 0;					// made-up weak reads on this track
			for (sc = 0; (sc < nsec) && !cut; sc++) {
				dskSec* x = &secs[sc];
				x->c = sib[sc].track;
				x->h = sib[sc].side;
				x->r = sib[sc].sector;
				x->n = sib[sc].size;
				x->st1 = sib[sc].sr1;
				x->st2 = sib[sc].sr2;
				int nom = flp_seclen(x->n);
				long slen;
				if (ext) {
					slen = sib[sc].bslo | (sib[sc].bshi << 8);
				} else {					// the track's size code stands for every sector
					slen = (tib->secSize > 5) ? 0x1800 : (0x80 << tib->secSize);
				}
				if (dp + slen > len) {				// the file ends inside this sector
					slen = (len > dp) ? len - dp : 0;
					cut = 1;
				}
				x->data = buf + dp;
				x->copies = 1;
				x->made = 0;
				x->len = (int)slen;
				if (ext && (slen > nom) && !(slen % nom)) {	// several reads of a weak sector
					x->copies = (int)(slen / nom);
					x->len = nom;
				} else if (slen > nom) {
					x->len = nom;
				} else if ((i == 0) && (x->r == 2) && (x->n == 2) && (slen == nom) && (x->st1 & F_SR1_DE)
						&& ((x->st2 & (F_SR2_CM | F_SR2_DD)) == F_SR2_DD) && (wn < DSK_MAXSEC)
						&& (wbuf || (wbuf = malloc(DSK_MAXSEC * sizeof(*wbuf))))) {
					x->data = dsk_weak_reads(wbuf[wn++], x->data);	// one read of a weak sector
					x->copies = DSK_WEAK_READS;
					x->made = 1;
				}
				dp += slen;
			}
			// 4E is what the dumping tools put there for any track, not a length
			// they measured: such a track gets the gap +3DOS formats with
			int gap3 = (tib->gap3Size == 0x4e) ? DSK_GAP3_P3 : tib->gap3Size;
			int whole = -1, most = 1;
			for (int k = 0; k < sc; k++) {
				int m = dsk_ids_in(&secs[k], secs, sc);
				if (m > most) {
					whole = k;
					most = m;
				}
			}
			if ((whole >= 0) && (tr < 256)) {
				dsk_form_stream(flp, tr, &secs[whole]);
			} else {
				dsk_form(flp, tr, secs, sc, gap3);
			}
			if (ext && (tr < 256) && (pos + rtlen <= len) && !cut)	// saved back as it came, until written
				flp_keep_raw(flp, tr, buf + pos, (int)rtlen);
			tr += (sidcnt == 1) ? 2 : 1;
			pos += rtlen;
		} else {
			tr += (sidcnt == 1) ? 2 : 1;
		}
	}
	free(wbuf);
	free(buf);
	flp_insert(flp, name);		// what was read up to a broken track goes in, as before
	return err;
}

int loadDSK(Computer* comp, const char *name, int drv) {
	return dsk_read(comp->dif->flp[drv & 3], name);
}

// the image built in memory, grown as it goes
typedef struct {
	unsigned char* data;
	size_t len;
	size_t cap;
	int fail;
} dskBuf;

static void db_put(dskBuf* b, const void* src, size_t n) {
	if (b->fail) return;
	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap : 0x10000;
		while (cap < b->len + n) cap <<= 1;
		unsigned char* nd = (unsigned char*)realloc(b->data, cap);
		if (!nd) {
			b->fail = 1;
			return;
		}
		b->data = nd;
		b->cap = cap;
	}
	memcpy(b->data + b->len, src, n);
	b->len += n;
}

// the first data byte of the field after an ID that ends at pos, 0: none before the next ID
static int dsk_data_after(flpTrack* t, int pos, int tlen) {
	for (int i = pos; (i < pos + 60) && (i < tlen); i++) {
		int f = t->field[i];
		if (f == 1) return 0;
		if (((f == 2) || (f == 3)) && (t->field[i - 1] != f)) return i;
	}
	return 0;
}

static int dsk_crc_bad(flpTrack* t, int st, int len, int tlen) {
	if (st + len + 2 > tlen) return 1;
	unsigned short crc = getCrc(t->byte + st, len);
	return crc != ((t->byte[st + len] << 8) | t->byte[st + len + 1]);
}

// A track the way the image file had it, while nothing wrote it; else what is
// on it: every ID in track order, ST1/ST2 from what its marks and CRCs say, a
// data field cut by the index stored up to it (6K at most), a weak sector's reads
static size_t dsk_save_track(Floppy* flp, int trk, int side, dskBuf* b) {
	int rtrk = (trk << 1) | (side & 1);
	flpTrack* t = &flp->data[rtrk];
	if (t->raw) {
		db_put(b, t->raw, t->rawlen);
		return t->rawlen;
	}
	TrackInfBlock tinf;
	SectorInfBlock* si = (SectorInfBlock*)tinf.sectorInfo;
	const unsigned char* src[DSK_MAXSEC];
	int slen[DSK_MAXSEC];
	int tlen = flp->trklen;
	int sec = 0;
	int maxn = 0;
	memset(&tinf, 0, sizeof(TrackInfBlock));
	for (int pos = 4; (pos + 4 <= tlen) && (sec < DSK_MAXSEC); pos++) {
		if ((t->field[pos] != 1) || (t->field[pos - 1] == 1)) continue;
		SectorInfBlock* s = &si[sec];
		s->track = t->byte[pos];
		s->side = t->byte[pos + 1];
		s->sector = t->byte[pos + 2];
		s->size = t->byte[pos + 3];
		if (dsk_crc_bad(t, pos - 4, 8, tlen)) s->sr1 |= F_SR1_DE;	// CRC error in the ID
		if (s->size > maxn) maxn = s->size;
		int dp = dsk_data_after(t, pos + 6, tlen);
		int len = 0;
		src[sec] = t->byte + dp;
		if (!dp) {					// no data mark
			s->sr1 |= F_SR1_MA;
			s->sr2 |= F_SR2_MD;
		} else {
			if (t->field[dp] == 3) s->sr2 |= F_SR2_CM;	// deleted
			int nom = flp_seclen(s->size);
			if (dp + nom + 2 > tlen) {		// the index cuts it: its CRC is never read
				len = tlen - dp;
				if (len > 0x1800) len = 0x1800;
				s->sr1 |= F_SR1_DE;
				s->sr2 |= F_SR2_DD;
			} else {
				len = nom;
				if (dsk_crc_bad(t, dp - 4, nom + 4, tlen)) {
					s->sr1 |= F_SR1_DE;
					s->sr2 |= F_SR2_DD;
				}
			}
			for (int i = 0; i < t->nweak; i++) {
				if (t->weak[i].pos == dp) {	// all its reads
					src[sec] = t->weak[i].data;
					len = t->weak[i].len * (t->weak[i].made ? 1 : t->weak[i].copies);
				}
			}
		}
		slen[sec] = len;
		s->bslo = len & 0xff;
		s->bshi = (len >> 8) & 0xff;
		sec++;
		pos += 5;
	}
	if (sec < 1) return 0;		// no sectors: not written, size 0
	size_t fpos = b->len;
	tinf.track = trk & 0xff;
	tinf.side = side & 1;
	tinf.secSize = maxn;
	tinf.secCount = sec;
	tinf.gap3Size = 0x4e;		// what dumping tools write: no length measured
	tinf.filler = 0xe5;
	db_put(b, "Track-Info\r\n", 12);
	db_put(b, &tinf, sizeof(TrackInfBlock));
	for (int i = 0; i < sec; i++)
		db_put(b, src[i], slen[i]);
	static const unsigned char pad[0x100] = {0};
	size_t sz = b->len - fpos;
	if (sz & 0xff) db_put(b, pad, 0x100 - (sz & 0xff));	// the size table counts in 256s
	return b->len - fpos;
}

static const char sign[] = "EXTENDED CPC DSK File\r\nDisk-Info\r\nXpeccy        ";

unsigned char* dsk_build(Floppy* flp, size_t* len) {
	if (flp_wide(flp)) return NULL;		// 86 cylinders are written: none, not a cut one
	dskBuf b;
	unsigned char buf[256];
	memset(&b, 0, sizeof(b));
	memset(buf, 0, 0x100);
	memcpy(buf, sign, 0x22 + 0x0e);
	// 30 = track count, will be updated later
	// 31: 1 | 2 sides
	// 32,33 = 0 @ extend format
	// 34+N = size of track N (from Track-Info to Track-Info)
	db_put(&b, buf, 256);		// signature + disk info: filled in at the end
	int cpos = 0x34;
	size_t isize;
	int ds = 0;
	int tcount = 0;
	for (int i = 0; i < 86; i++) {
		isize = dsk_save_track(flp, i, 0, &b);
		buf[cpos] = (isize >> 8) & 0xff;
		cpos++;
		if (isize > 0)
			tcount = i + 1;
		if (flp->doubleSide) {
			isize = dsk_save_track(flp, i, 1, &b);
			if (isize > 0)
				ds = 1;
			buf[cpos] = (isize >> 8) & 0xff;
			cpos++;
			if (isize > 0)
				tcount = i + 1;
		}
	}
	buf[0x30] = (tcount < 43) ? 42 : 84; // = tcount & 0xff;
	buf[0x31] = ds ? 2 : 1;
	if (!ds) {
		cpos = 0x36;		// 0x34 doesn't changed
		tcount = 0x35;
		while(cpos < 256) {
			buf[tcount] = buf[cpos];
			buf[cpos] = 0;
			tcount++;
			cpos += 2;
		}
	}
	if (b.fail) {
		free(b.data);
		return NULL;
	}
	memcpy(b.data, buf, 256);
	*len = b.len;
	return b.data;
}

int saveDSK(Computer* comp, const char* name, int drv) {
	Floppy* flp = comp->dif->flp[drv & 3];
	if (flp_wide(flp)) return ERR_DSK_WIDE;	// dsk_build writes 86: refused, not cut
	size_t len = 0;
	unsigned char* img = dsk_build(flp, &len);
	if (!img) return ERR_CANT_OPEN;
	FILE* file = fopen(name, "wb");
	int err = ERR_CANT_OPEN;
	if (file) {
		if (fwrite(img, 1, len, file) == len)
			err = ERR_OK;
		fclose(file);
	}
	free(img);
	if (err == ERR_OK) {
		// update name
		flp_set_path(flp, name);
		flp->changed = 0;
	}
	return err;
}

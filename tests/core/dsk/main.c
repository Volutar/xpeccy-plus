// Headless +3 disk bench: EXTENDED DSK images read into a drive, and the uPD765
// asked for them the way a program on the +3 asks, through its two ports. A
// section each:
//
//	read/<case>	a command, its result bytes, and which sector each block of
//			data the CPU took came from
//	roundtrip	the image saved back, compared with the file it was read from
//	trd-128		a 1 MB TRD through the Beta disk's WD1793: its last cylinder
//			read and written, saved as TRD and UDI and read back
//
//	dsk-bench			the cases, on an image the bench writes
//	dsk-bench -sweep file.dsk...	load + save for each file, one line a file

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spectrum.h"
#include "filetypes/filetypes.h"

// ---- the image

typedef struct {
	unsigned char c, h, r, n, st1, st2;
	int len;		// bytes of one read stored
	int copies;
} tSec;

typedef struct {
	int nsec;
	unsigned char n, gap;
	tSec s[29];
} tTrk;

static int img_b = 0;		// writing the second image: Speedlock's weak sector

static unsigned char pattern(int t, int r, int k, int copy) {
	if (img_b && (r == 2) && (k < 256)) return 0xe5;	// the filler before its weak bits
	return (t * 59 + r * 31 + k * 7 + copy * 101 + (k >> 8) * 13) & 0xff;
}

static int track_bytes(tTrk* t) {
	int sz = 0x100;
	for (int i = 0; i < t->nsec; i++)
		sz += t->s[i].len * t->s[i].copies;
	return t->nsec ? ((sz + 0xff) & ~0xff) : 0;
}

static int write_edsk(const char* path, tTrk* trk, int ntrk) {
	FILE* f = fopen(path, "wb");
	if (!f) return 0;
	unsigned char hdr[0x100];
	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, "EXTENDED CPC DSK File\r\nDisk-Info\r\n", 34);
	memcpy(hdr + 0x22, "dsk-bench     ", 14);
	hdr[0x30] = ntrk;
	hdr[0x31] = 1;
	for (int i = 0; i < ntrk; i++)
		hdr[0x34 + i] = track_bytes(&trk[i]) >> 8;
	fwrite(hdr, 1, 0x100, f);
	for (int i = 0; i < ntrk; i++) {
		tTrk* t = &trk[i];
		if (!t->nsec) continue;
		unsigned char tib[0x100];
		memset(tib, 0, sizeof(tib));
		memcpy(tib, "Track-Info\r\n", 12);
		tib[0x10] = i;
		tib[0x14] = t->n;
		tib[0x15] = t->nsec;
		tib[0x16] = t->gap;
		tib[0x17] = 0xe5;
		for (int k = 0; k < t->nsec; k++) {
			tSec* s = &t->s[k];
			unsigned char* p = tib + 0x18 + 8 * k;
			int len = s->len * s->copies;
			p[0] = s->c; p[1] = s->h; p[2] = s->r; p[3] = s->n;
			p[4] = s->st1; p[5] = s->st2;
			p[6] = len & 0xff; p[7] = (len >> 8) & 0xff;
		}
		fwrite(tib, 1, 0x100, f);
		int sz = 0x100;
		for (int k = 0; k < t->nsec; k++) {
			tSec* s = &t->s[k];
			for (int c = 0; c < s->copies; c++)
				for (int b = 0; b < s->len; b++)
					fputc(pattern(i, s->r, b, c), f);
			sz += s->len * s->copies;
		}
		for (; sz < track_bytes(t); sz++)
			fputc(0, f);
	}
	fclose(f);
	return 1;
}

static tTrk trk[5];

static void add(int t, int c, int h, int r, int n, int st1, int st2, int len, int copies) {
	tSec* s = &trk[t].s[trk[t].nsec++];
	s->c = c; s->h = h; s->r = r; s->n = n;
	s->st1 = st1; s->st2 = st2;
	s->len = len;
	s->copies = copies;
}

static void make_image(void) {
	memset(trk, 0, sizeof(trk));
	for (int t = 0; t < 5; t++) {
		trk[t].n = 2;
		trk[t].gap = 0x4e;
	}
	for (int r = 1; r <= 9; r++)
		add(0, 0, 0, r, 2, 0, 0, 512, 1);
	add(1, 1, 0, 1, 2, 0, 0, 512, 1);
	add(1, 1, 0, 2, 2, 0, 0x40, 512, 1);		// deleted data mark
	add(1, 1, 0, 3, 2, 0x20, 0x20, 512, 1);		// CRC error in the data: weak, one read
	add(1, 0x11, 0, 4, 2, 0, 0, 512, 1);		// C is not the track's
	add(1, 0xff, 0, 5, 2, 0, 0, 512, 1);		// C = FF
	add(1, 1, 0, 6, 2, 0x20, 0, 512, 1);		// CRC error in the ID
	add(1, 1, 0, 7, 2, 0x01, 0x01, 0, 1);		// no data mark
	add(1, 1, 0, 8, 1, 0, 0, 256, 1);		// N 1 on a track of N 2
	add(1, 1, 0, 9, 2, 0x20, 0x20, 512, 3);		// weak: three reads
	trk[2].n = 6;
	add(2, 2, 0, 1, 6, 0x20, 0x20, 6144, 1);	// N 6: runs to the index
	// track 3 unformatted
	trk[4].n = 0;
	for (int r = 1; r <= 4; r++)
		add(4, 4, 0, r, 0, 0, 0, 128, 1);
}

// ---- the +3's ports

static DiskIF* dif;

static void dummy_irq(int id, void* p) {(void)id; (void)p;}

// in steps of an odd length, as the CPU's are: a door's delay run down to
// exactly 0 would never close it (flp_sync waits for below 0)
static void run(int ns) {
	while (ns > 0) {
		int n = (ns > 3999) ? 3999 : ns;
		difSync(dif, n);
		ns -= n;
	}
}

static int msr(void) {
	int r = 0xff;
	difIn(dif, 0x2ffd, &r, 0);
	return r;
}

static int rd(void) {
	int r = 0xff;
	difIn(dif, 0x3ffd, &r, 0);
	return r;
}

static int send_dos = 0;		// difOut's last argument: DIF_FROM_RAM for a game's own loader

static int send(const unsigned char* b, int n) {
	for (int i = 0; i < n; i++) {
		int t = 0;
		while ((msr() & 0xc0) != 0x80) {
			run(2000);
			if (++t > 500000) return 0;
		}
		difOut(dif, 0x3ffd, b[i], send_dos);
		run(2000);
	}
	return 1;
}

// the execution phase, the CPU taking a byte every cpu_ns until it has taken max
static int take(unsigned char* buf, int max, int cpu_ns) {
	int got = 0;
	for (long t = 0; t < 4000000000L; t += cpu_ns) {
		int m = msr();
		if (!(m & 0x20)) break;			// EXM off: the result phase
		if (((m & 0xc0) == 0xc0) && (got < max)) buf[got++] = rd();
		run(cpu_ns);
	}
	return got;
}

static int result(unsigned char* res) {
	int n = 0;
	for (int t = 0; (t < 1000000) && (n < 16); t++) {
		int m = msr();
		if ((m & 0xd0) == 0xd0) {
			res[n++] = rd();
		} else if (!(m & 0x10)) {
			break;
		}
		run(2000);
	}
	return n;
}

static void seek(int cyl) {
	unsigned char c[] = {0x0f, 0x00, cyl};
	unsigned char s[] = {0x08};
	unsigned char res[16];
	send(c, 3);
	run(200000000);
	send(s, 1);
	result(res);
}

// which sectors of the image the bytes taken are, a block of 128 << n each
static void describe(unsigned char* buf, int got, int n) {
	int blk = (n > 7) ? 0x4000 : (0x80 << n);
	printf("  data %d:", got);
	for (int off = 0; off < got; off += blk) {
		int len = (got - off < blk) ? got - off : blk;
		int best = 0;
		char name[32] = "?";
		for (int t = 0; t < 5; t++) {
			for (int k = 0; k < trk[t].nsec; k++) {
				tSec* s = &trk[t].s[k];
				for (int c = 0; c < s->copies; c++) {
					int m = 0;
					while ((m < len) && (m < s->len) && (buf[off + m] == pattern(t, s->r, m, c))) m++;
					if (m > best) {
						best = m;
						if (s->copies > 1) {
							sprintf(name, "T%dR%d/%d", t, s->r, c);
						} else {
							sprintf(name, "T%dR%d", t, s->r);
						}
					}
				}
			}
		}
		if (best == len) {
			printf(" %s", name);
		} else if (best > 0) {
			printf(" %s(%d)", name, best);
		} else {
			printf(" ?");
		}
	}
	printf("\n");
}

static unsigned char data[0x10000];

// one read command: its bytes, the CPU taking at most max bytes
static void cmd(const char* name, int op, int cyl, int c, int h, int r, int n, int eot, int dtl, int max) {
	unsigned char b[9] = {op, 0x00, c, h, r, n, eot, 0x2a, dtl};
	unsigned char res[16];
	seek(cyl);
	printf("== read/%s\n", name);
	printf("  cmd %.2X: C%.2X H%.2X R%.2X N%.2X EOT%.2X DTL%.2X\n", op, c, h, r, n, eot, dtl);
	if (!send(b, 9)) {
		printf("  command not taken\n");
		return;
	}
	int got = take(data, max, 10000);
	int nr = result(res);
	printf("  result");
	for (int i = 0; i < nr; i++) printf(" %.2X", res[i]);
	printf("\n");
	if (got) describe(data, got, n);
}

static void read_id(const char* name, int cyl) {
	unsigned char b[2] = {0x4a, 0x00};
	unsigned char res[16];
	seek(cyl);
	printf("== read/%s\n", name);
	send(b, 2);
	int nr = result(res);
	printf("  result");
	for (int i = 0; i < nr && i < 3; i++) printf(" %.2X", res[i]);
	if ((nr == 7) && !(res[0] & 0xc0)) printf(" - %.2X %.2X %.2X %.2X", res[3], res[4], res[5], res[6]);
	printf("\n");
}

// three reads of the weak sector: a different read each time, all CRC errors
static void weak(void) {
	unsigned char b[9] = {0x46, 0x00, 1, 0, 9, 2, 9, 0x2a, 0xff};
	unsigned char res[16];
	int copy[3];
	seek(1);
	printf("== read/weak\n");
	for (int i = 0; i < 3; i++) {
		send(b, 9);
		int got = take(data, 512, 10000);
		result(res);
		copy[i] = -1;
		for (int c = 0; c < 3; c++) {
			int m = 0;
			while ((m < got) && (data[m] == pattern(1, 9, m, c))) m++;
			if (m == 512) copy[i] = c;
		}
		printf("  result %.2X %.2X %.2X, read %d\n", res[0], res[1], res[2], copy[i]);
	}
	printf("  all three differ: %s\n", ((copy[0] != copy[1]) && (copy[1] != copy[2]) && (copy[0] != copy[2])) ? "yes" : "no");
}

// ---- saving back

static unsigned char* read_all(const char* path, long* len) {
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char* buf = malloc(*len > 0 ? *len : 1);
	if (fread(buf, 1, *len, f) != (size_t)*len) *len = 0;
	fclose(f);
	return buf;
}

// The image saved against the file, past the creator's name and the track count
// (42 or 84 when saved): "same", or where it first differs and in which track.
// derived: what is on the tracks alone, the blocks kept from the file dropped.
static void compare(Floppy* flp, const unsigned char* file, long flen, int derived, char* out, size_t osz) {
	if (derived) {
		for (int i = 0; i < 256; i++) {
			free(flp->data[i].raw);
			flp->data[i].raw = NULL;
			flp->data[i].rawlen = 0;
		}
	}
	size_t len = 0;
	unsigned char* img = dsk_build(flp, &len);
	if (!img) {
		snprintf(out, osz, "not built");
		return;
	}
	long n = ((long)len < flen) ? (long)len : flen;
	long at = -1;
	for (long i = 0x31; i < n; i++) {
		if (img[i] != file[i]) {
			at = i;
			break;
		}
	}
	if ((at < 0) && ((long)len != flen)) at = n;
	if (at < 0) {
		snprintf(out, osz, "same");
	} else {
		int trk = -1;
		long pos = 0x100;
		for (int t = 0; (t < 0xcc) && (pos <= at); t++) {
			long sz = file[0x34 + t] << 8;
			if ((at >= pos) && (at < pos + sz)) trk = t;
			pos += sz;
		}
		snprintf(out, osz, "differs at %lX (track %d), %ld bytes against %ld", at, trk, (long)len, flen);
	}
	free(img);
}

// A disk read into a drive that held one with more tracks: nothing of the old one
// stays past the new one's tracks
static void cleared(const char* path) {
	Floppy* flp = flpCreate(0, dummy_irq, NULL);
	unsigned char raw[16] = {0};
	memset(flp->data[200].byte, 0x55, flp->trklen);
	flp_keep_raw(flp, 200, raw, sizeof(raw));
	dsk_read(flp, path);
	printf("== cleared\n  track 200 after a 5-track disk: byte %.2X, raw %d\n", flp->data[200].byte[0], flp->data[200].rawlen);
	flpDestroy(flp);
}

static void roundtrip(const char* path, const char* name) {
	long flen = 0;
	unsigned char* file = read_all(path, &flen);
	char res[2][128];
	for (int k = 0; k < 2; k++) {
		Floppy* flp = flpCreate(0, dummy_irq, NULL);
		dsk_read(flp, path);
		compare(flp, file, flen, k, res[k], sizeof(res[k]));
		flpDestroy(flp);
	}
	if (name) {
		printf("== roundtrip\n  kept: %s\n  derived: %s\n", res[0], res[1]);
	} else {
		printf("%s\tkept: %s\tderived: %s\n", path, res[0], res[1]);
	}
	free(file);
}

// Drive B: track 0 as Speedlock +3 has it - sector 2 with a data CRC error, stored
// as one read: taken for a weak one, its opening filler reads as stored every time,
// the rest differently
static tTrk trk_b[1];

static int write_image_b(const char* path) {
	memset(trk_b, 0, sizeof(trk_b));
	trk_b[0].n = 2;
	trk_b[0].gap = 0x4e;
	tSec* s = &trk_b[0].s[0];
	s[0] = (tSec){0, 0, 1, 2, 0, 0, 512, 1};
	s[1] = (tSec){0, 0, 2, 2, 0x20, 0x20, 512, 1};
	trk_b[0].nsec = 2;
	img_b = 1;
	int ok = write_edsk(path, trk_b, 1);
	img_b = 0;
	return ok;
}

static void weak_made(void) {
	unsigned char sk[] = {0x0f, 0x01, 0};
	unsigned char si[] = {0x08};
	unsigned char b[9] = {0x46, 0x01, 0, 0, 2, 2, 2, 0x2a, 0xff};
	unsigned char res[16];
	unsigned char got[3][512];
	send(sk, 3);
	run(200000000);
	send(si, 1);
	result(res);
	printf("== read/weak-made\n");
	for (int i = 0; i < 3; i++) {
		send(b, 9);
		take(got[i], 512, 10000);
		result(res);
		int first = 0;
		while ((first < 256) && (got[i][first] == 0xe5)) first++;
		printf("  result %.2X %.2X %.2X, first half as stored: %s\n", res[0], res[1], res[2], (first == 256) ? "yes" : "no");
	}
	int differ = memcmp(got[0] + 256, got[1] + 256, 256) && memcmp(got[1] + 256, got[2] + 256, 256) && memcmp(got[0] + 256, got[2] + 256, 256);
	printf("  second halves all differ: %s\n", differ ? "yes" : "no");
}

// Format Track of sc sectors R 1.. on a track; how many IDs the FDC asked for,
// its result, and what the track then holds: Read ID, and a read of the last
// sector that fits and of the one after
static void format(const char* name, int cyl, int n, int sc, int fill, int fits) {
	unsigned char b[6] = {0x4d, 0x00, n, sc, 0x2a, fill};
	unsigned char res[16];
	seek(cyl);
	printf("== format/%s\n", name);
	send(b, 6);
	int ids = 0, k = 0;
	for (int t = 0; t < 400000; t++) {		// 4 s at most
		int m = msr();
		if (!(m & 0x20)) break;
		if ((m & 0xc0) == 0x80) {
			unsigned char id[4] = {cyl, 0, ids + 1, n};
			difOut(dif, 0x3ffd, id[k], 0);
			if (++k == 4) {
				k = 0;
				ids++;
			}
		}
		run(10000);
	}
	int nr = result(res);
	printf("  IDs taken %d of %d, result", ids, sc);
	for (int i = 0; i < nr && i < 3; i++) printf(" %.2X", res[i]);
	printf("\n");
	unsigned char ri[2] = {0x4a, 0x00};
	send(ri, 2);
	nr = result(res);
	printf("  Read ID: %.2X %.2X %.2X - %.2X %.2X %.2X %.2X\n", res[0], res[1], res[2], res[3], res[4], res[5], res[6]);
	for (int r = fits; fits && (r <= fits + 1); r++) {		// fits 0: no reads
		int len = flp_seclen(n);
		unsigned char c[9] = {0x46, 0x00, cyl, 0, r, n, r, 0x2a, n ? 0xff : 0x80};
		send(c, 9);
		int got = take(data, len, 10000);
		nr = result(res);
		int same = 0;
		while ((same < got) && (data[same] == fill)) same++;
		printf("  R%d: result %.2X %.2X %.2X, %d bytes, %d of them %.2X\n", r, res[0], res[1], res[2], got, same, fill);
	}
}

// a Beta disk machine with fast access on, drive A out of reset, side 0
static void beta_on(void) {
	difSetHW(dif, DIF_BDI);
	fdcFlag = FDC_FAST;
	difOut(dif, 0xff, 0x3c, 1);
}

// back to the +3, reset as a machine switch resets it
static void back_to_p3(void) {
	fdcFlag = 0;
	difSetHW(dif, DIF_P3DOS);
	difReset(dif);
	unsigned char spec[] = {0x03, 0xaf, 0x03};
	unsigned char cal[] = {0x07, 0x00};
	send(spec, 3);
	send(cal, 2);
	run(200000000);
}

// A +3 game's own loader reads in real time; the machine then turns into a Beta
// disk one with fast access on: its WD1793 seeks fast, not as the +3 left it
static void beta_after_p3(void) {
	unsigned char b[9] = {0x46, 0x00, 0, 0, 1, 2, 1, 0x2a, 0xff};
	unsigned char res[16];
	printf("== beta-after-p3\n");
	seek(0);
	send_dos = DIF_FROM_RAM;
	send(b, 9);
	send_dos = 0;
	take(data, 512, 10000);
	result(res);
	beta_on();
	difOut(dif, 0x7f, 40, 1);		// the track to seek to
	difOut(dif, 0x1f, 0x10, 1);		// seek, 6 ms a step
	long ns = 0;
	int st = 0x01;
	while ((st & 0x01) && (ns < 2000000000L)) {
		run(9999);
		ns += 9999;
		difIn(dif, 0x1f, &st, 1);
	}
	printf("  seek to 40: %ld ms, fast: %s\n", ns / 1000000, (ns < 50000000L) ? "yes" : "no");
	back_to_p3();
}

// INTRQ on #FF d7, as TR-DOS waits for it
static int beta_irq(void) {
	int v = 0xff;
	difIn(dif, 0xff, &v, 1);
	return (v & 0x80) ? 1 : 0;
}

// a command, and for one that is not a Force Interrupt its end awaited on #FF
static void beta_com(int com) {
	difOut(dif, 0x1f, com, 1);
	run(1000000);
	if ((com & 0xf0) == 0xd0) return;
	for (long ns = 0; (ns < 2000000000L) && !beta_irq(); ns += 9999)
		run(9999);
}

// INTRQ: none after #D0, held by #D8; #D8 then #D0 is checked only after a status
// read (the datasheet holds it until then, the die resets it with the write)
static void beta_intrq(void) {
	int st;
	printf("== beta-intrq\n");
	beta_on();
	beta_com(0x00);				// restore: the track register known
	difOut(dif, 0x7f, 2, 1);
	beta_com(0x10);				// seek
	printf("  seek done: %d\n", beta_irq());
	difIn(dif, 0x1f, &st, 1);
	printf("  status read: %d\n", beta_irq());
	difOut(dif, 0x7f, 4, 1);
	beta_com(0x10);
	beta_com(0xd0);
	printf("  #D0 after a seek: %d\n", beta_irq());
	fdcFlag = 0;				// a seek of 36 steps, 6 ms each
	difOut(dif, 0x7f, 40, 1);
	difOut(dif, 0x1f, 0x10, 1);
	run(10000000);
	difIn(dif, 0x1f, &st, 1);
	printf("  seek running: busy %d\n", st & 1);
	beta_com(0xd0);
	difIn(dif, 0x1f, &st, 1);
	printf("  #D0 during it: %d, busy %d\n", beta_irq(), st & 1);
	beta_com(0xd8);
	printf("  #D8: %d\n", beta_irq());
	difIn(dif, 0x1f, &st, 1);
	printf("  #D8, status read: %d\n", beta_irq());
	beta_com(0xd0);
	difIn(dif, 0x1f, &st, 1);
	printf("  #D8, #D0, status read: %d\n", beta_irq());
	back_to_p3();
}

static void start(const char* path, const char* path_b) {
	fdcFlag = 0;
	dif = difCreate(DIF_P3DOS, dummy_irq, NULL);
	difReset(dif);
	for (int i = 0; i < 4; i++) dif->flp[i]->motor = 1;
	int err = dsk_read(dif->flp[0], path);
	if (err) printf("dsk_read: %d\n", err);
	err = dsk_read(dif->flp[1], path_b);
	if (err) printf("dsk_read B: %d\n", err);
	run(600000000);			// the door closes
	unsigned char spec[] = {0x03, 0xaf, 0x03};	// as +3DOS: non-DMA
	unsigned char cal[] = {0x07, 0x00};
	send(spec, 3);
	send(cal, 2);
	run(200000000);
	seek(0);
}

// ---- a TRD of 128 cylinders, 1 MB: the Beta disk's WD1793 reads and writes its
// last cylinder, and it saves back whole as TRD and UDI

static unsigned char trd_byte(int t, int s, int k) {
	return (t * 13 + s * 7 + k * 3 + (k >> 8)) & 0xff;
}

// side-tracks 0..tracks-1 of a TR-DOS disk whose files end on side-track 200:
// 4096 sectors in all, as the system sector counts them
static void trd_wide_image(unsigned char* img, int tracks) {
	for (int t = 0; t < tracks; t++)
		for (int s = 0; s < 16; s++)
			for (int k = 0; k < 256; k++)
				img[(t * 16 + s) * 256 + k] = trd_byte(t, s, k);
	unsigned char* sys = img + 8 * 256;
	sys[0xe1] = 0;			// first free sector
	sys[0xe2] = 200;		// first free side-track
	sys[0xe3] = 0x16;		// 80 DS
	sys[0xe4] = 0;
	sys[0xe5] = (4096 - 3200) & 0xff;	// free sectors
	sys[0xe6] = (4096 - 3200) >> 8;
	sys[0xe7] = 0x10;		// TR-DOS
}

static int write_file(const char* path, const unsigned char* d, size_t len) {
	FILE* f = fopen(path, "wb");
	if (!f) return 0;
	size_t n = fwrite(d, 1, len, f);
	fclose(f);
	return n == len;
}

static size_t read_file(const char* path, unsigned char* d, size_t max) {
	FILE* f = fopen(path, "rb");
	if (!f) return 0;
	size_t n = fread(d, 1, max, f);
	fclose(f);
	return n;
}

// the door closed on the disk, the head at 0
static void beta_disk(void) {
	run(600000000);
	beta_on();
	beta_com(0x08);			// restore
}

static void beta_seek(int cyl) {
	difOut(dif, 0x7f, cyl, 1);
	beta_com(0x18);
	int tr = 0xff;
	difIn(dif, 0x3f, &tr, 1);
	printf("  seek to %d: track register %d, head at %d\n", cyl, tr & 0xff, dif->flp[0]->trk);
}

// a sector read (0x80) or written (0xA0), each byte on DRQ; the status at the end
static int beta_sector(int com, int side, int sec, unsigned char* buf, int* moved) {
	difOut(dif, 0xff, side ? 0x2c : 0x3c, 1);	// #FF b4 low: side 1
	difOut(dif, 0x5f, sec, 1);
	difOut(dif, 0x1f, com, 1);
	int n = 0;
	for (long ns = 0; ns < 2000000000L; ns += 999) {
		int st = 0xff;
		difIn(dif, 0xff, &st, 1);
		if (st & 0x40) {
			if (com & 0x20) {
				difOut(dif, 0x7f, (n < 256) ? buf[n] : 0, 1);
			} else {
				int v = 0xff;
				difIn(dif, 0x7f, &v, 1);
				if (n < 256) buf[n] = v & 0xff;
			}
			n++;
		}
		if (st & 0x80) break;
		run(999);
	}
	*moved = n;
	int st = 0xff;
	difIn(dif, 0x1f, &st, 1);
	return st & 0xff;
}

static Floppy* beta_drive(void) {
	dif = difCreate(DIF_BDI, dummy_irq, NULL);
	difReset(dif);
	dif->flp[0]->motor = 1;
	return dif->flp[0];
}

static void trd_wide(const char* tmp) {
	char trd[1100], back[1100], udi[1100];
	snprintf(trd, sizeof(trd), "%s.trd", tmp);
	snprintf(back, sizeof(back), "%s.back.trd", tmp);
	snprintf(udi, sizeof(udi), "%s.udi", tmp);
	unsigned char* img = malloc(0x100000);
	unsigned char* got = malloc(0x100000);
	unsigned char buf[256];
	size_t len;
	int n;

	printf("== trd-128\n");
	trd_wide_image(img, 256);
	write_file(trd, img, 0x100000);
	Floppy* flp = beta_drive();
	int err = trd_read(flp, trd);
	printf("  read: err %d, %d cylinders\n", err, flp->cyls);
	beta_disk();
	beta_seek(127);
	int st = beta_sector(0x80, 1, 16, buf, &n);
	printf("  read C127 H1 R16: status %.2X, %d bytes, as the file: %s\n", st, n,
	       ((n == 256) && !memcmp(buf, img + 0xfff00, 256)) ? "yes" : "no");
	for (int k = 0; k < 256; k++) img[0xfff00 + k] = buf[k] = 255 - k;
	st = beta_sector(0xa0, 1, 16, buf, &n);
	printf("  write C127 H1 R16: status %.2X, %d bytes\n", st, n);
	err = trd_write(flp, back);
	len = read_file(back, got, 0x100000);
	printf("  saved as TRD: err %d, %d bytes, as written: %s\n", err, (int)len,
	       ((len == 0x100000) && !memcmp(got, img, len)) ? "yes" : "no");
	unsigned char* u = udi_build(flp, &len);
	printf("  UDI: last cylinder %d\n", u ? u[9] : -1);
	if (u) write_file(udi, u, len);
	free(u);
	difDestroy(dif);

	// both files back, each into a drive of its own
	for (int f = 0; f < 2; f++) {
		flp = beta_drive();
		err = f ? udi_read(flp, udi) : trd_read(flp, back);
		int same = 1;
		for (int t = 0; (t < 256) && same; t++)
			for (int s = 0; (s < 16) && same; s++)
				same = diskGetSectorData(flp, t, s + 1, buf, 256) && !memcmp(buf, img + (t * 16 + s) * 256, 256);
		printf("  %s back: err %d, %d cylinders, every sector as written: %s\n", f ? "UDI" : "TRD", err, flp->cyls, same ? "yes" : "no");
		beta_disk();
		beta_seek(127);
		st = beta_sector(0x80, 1, 16, buf, &n);
		printf("  read C127 H1 R16: status %.2X, as written: %s\n", st,
		       ((n == 256) && !memcmp(buf, img + 0xfff00, 256)) ? "yes" : "no");
		difDestroy(dif);
	}

	// past 1 MB: refused
	FILE* fl = fopen(back, "ab");
	if (fl) {
		fputc(0, fl);
		fclose(fl);
	}
	flp = beta_drive();
	printf("  1 MB and a byte: err %d\n", trd_read(flp, back));
	difDestroy(dif);

	// a 640K disk: the head stops at 86 as always, and its UDI is the one it always was
	printf("== trd-128/640k\n");
	trd_wide_image(img, 160);
	img[8 * 256 + 0xe2] = 100;
	img[8 * 256 + 0xe5] = (2560 - 1600) & 0xff;
	img[8 * 256 + 0xe6] = (2560 - 1600) >> 8;
	write_file(trd, img, 0xa0000);
	flp = beta_drive();
	err = trd_read(flp, trd);
	printf("  read: err %d, %d cylinders\n", err, flp->cyls);
	beta_disk();
	beta_seek(100);
	u = udi_build(flp, &len);
	unsigned int sum = 0;
	for (size_t i = 0; u && (i < len); i++) sum = sum * 31 + u[i];
	printf("  UDI: %d bytes, last cylinder %d, sum %.8X\n", (int)len, u ? u[9] : -1, sum);
	free(u);
	// a new blank disk after a wide one: nothing of it left past cylinder 80
	difDestroy(dif);
	flp = beta_drive();
	trd_wide_image(img, 256);
	write_file(trd, img, 0x100000);
	trd_read(flp, trd);
	flp_insert(flp, NULL);
	printf("  blank after 1 MB: %d cylinders, side-track 200 holds a sector: %s\n", flp->cyls,
	       diskGetSectorData(flp, 200, 1, buf, 256) ? "yes" : "no");
	beta_disk();
	beta_seek(120);
	// a 640K TRD read into the same drive after a 1 MB one, as an open does
	trd_read(flp, trd);
	trd_wide_image(img, 160);
	write_file(trd, img, 0xa0000);
	err = trd_read(flp, trd);
	printf("  640K after 1 MB: err %d, %d cylinders, side-track 200 holds a sector: %s\n", err, flp->cyls,
	       diskGetSectorData(flp, 200, 1, buf, 256) ? "yes" : "no");
	beta_disk();
	beta_seek(120);
	// a 40-track drive stops at 43 with it, as always
	flp->trk80 = 0;
	beta_com(0x08);
	beta_seek(60);
	difDestroy(dif);

	// 1 MB with no TR-DOS system sector: saved whole all the same
	trd_wide_image(img, 256);
	img[8 * 256 + 0xe7] = 0;
	write_file(trd, img, 0x100000);
	flp = beta_drive();
	trd_read(flp, trd);
	err = trd_write(flp, back);
	len = read_file(back, got, 0x100000);
	printf("  1 MB, not TR-DOS, saved: err %d, %d bytes, as read: %s\n", err, (int)len,
	       ((len == 0x100000) && !memcmp(got, img, len)) ? "yes" : "no");
	difDestroy(dif);

	remove(trd);
	remove(back);
	remove(udi);
	free(img);
	free(got);
}

static int cases(const char* tmp) {
	char tmp_b[1100];
	snprintf(tmp_b, sizeof(tmp_b), "%s.b.dsk", tmp);
	make_image();
	if (!write_edsk(tmp, trk, 5) || !write_image_b(tmp_b)) {
		printf("can't write %s\n", tmp);
		return 1;
	}
	start(tmp, tmp_b);
	read_id("id-t0", 0);
	cmd("eot", 0x46, 0, 0, 0, 1, 2, 1, 0xff, 0x10000);
	cmd("multi", 0x46, 0, 0, 0, 1, 2, 3, 0xff, 0x10000);
	cmd("past-eot", 0x46, 0, 0, 0, 9, 2, 5, 0xff, 0x10000);
	cmd("overrun", 0x46, 0, 0, 0, 1, 2, 9, 0xff, 100);
	cmd("deleted", 0x46, 1, 1, 0, 2, 2, 9, 0xff, 0x10000);
	cmd("deleted-sk", 0x66, 1, 1, 0, 2, 2, 2, 0xff, 0x10000);
	cmd("deleted-sk-next", 0x66, 1, 1, 0, 1, 2, 3, 0xff, 0x10000);
	cmd("read-deleted-normal", 0x4c, 1, 1, 0, 1, 2, 1, 0xff, 0x10000);
	cmd("read-deleted", 0x4c, 1, 1, 0, 2, 2, 2, 0xff, 0x10000);
	cmd("data-crc", 0x46, 1, 1, 0, 3, 2, 9, 0xff, 0x10000);
	cmd("wrong-cyl", 0x46, 1, 1, 0, 4, 2, 4, 0xff, 0x10000);
	cmd("its-cyl", 0x46, 1, 0x11, 0, 4, 2, 4, 0xff, 0x10000);
	cmd("bad-cyl", 0x46, 1, 1, 0, 5, 2, 5, 0xff, 0x10000);
	cmd("id-crc", 0x46, 1, 1, 0, 6, 2, 6, 0xff, 0x10000);
	cmd("no-data-mark", 0x46, 1, 1, 0, 7, 2, 7, 0xff, 0x10000);
	cmd("n-differs", 0x46, 1, 1, 0, 8, 2, 8, 0xff, 0x10000);
	cmd("n-1", 0x46, 1, 1, 0, 8, 1, 8, 0xff, 0x10000);
	weak();
	weak_made();
	cmd("long", 0x46, 2, 2, 0, 1, 6, 1, 0xff, 0x10000);
	read_id("id-unformatted", 3);
	cmd("unformatted", 0x46, 3, 3, 0, 1, 2, 1, 0xff, 0x10000);
	cmd("dtl", 0x46, 4, 4, 0, 1, 0, 1, 0x40, 0x10000);
	cmd("track-t0", 0x42, 0, 0, 0, 1, 2, 9, 0xff, 0x10000);
	cmd("track-t1", 0x42, 1, 1, 0, 1, 2, 9, 0xff, 0x10000);
	fdcFlag = FDC_FAST;
	cmd("turbo-multi", 0x46, 0, 0, 0, 1, 2, 3, 0xff, 0x10000);
	cmd("turbo-overrun", 0x46, 0, 0, 0, 1, 2, 9, 0xff, 100);
	fdcFlag = 0;
	beta_after_p3();
	beta_intrq();
	format("sc11", 3, 2, 11, 0x5a, 10);
	format("sc255", 3, 0, 255, 0xa5, 29);
	format("n-ff", 3, 0xff, 64, 0x33, 0);	// N past 23 counts as 0 bytes, yet 1K each is written
	difDestroy(dif);
	cleared(tmp);
	roundtrip(tmp, "bench");
	trd_wide(tmp);
	remove(tmp);
	remove(tmp_b);
	return 0;
}

int main(int ac, char** av) {
	const char* tdir = getenv("TEMP");
	if (!tdir) tdir = getenv("TMPDIR");
	if (!tdir) tdir = "/tmp";
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s/dsk-bench-%d.dsk", tdir, (int)(sizeof(void*)));
	if ((ac > 1) && !strcmp(av[1], "-sweep")) {
		for (int i = 2; i < ac; i++)
			roundtrip(av[i], NULL);
		return 0;
	}
	return cases(tmp);
}

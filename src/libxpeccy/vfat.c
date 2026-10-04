#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vfat.h"

// Synthetic read-only FAT32 volume over a host directory.
//
// Layout: MBR at LBA 0, one partition at LBA 2048, 4K clusters. Every node gets
// a contiguous run of clusters at build time, so the FAT chains are arithmetic
// and nothing but directory entries has to be built on the fly.

#define VF_PARTSTART	2048
#define VF_RESERVED	32
#define VF_SPC		8			// 4K clusters
#define VF_MINVOL	(512 * 2048)		// 512 Mb, in sectors
#define VF_MAXVOL	(32u * 1024 * 2048)	// 32 Gb, in sectors
#define VF_EOC		0x0fffffff
#define VF_LABEL	"XPECCY SD  "		// 11 chars, space padded

// helpers

static void wr16(unsigned char* p, unsigned int v) {
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
}

static void wr32(unsigned char* p, unsigned int v) {
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

static unsigned int divup(unsigned int a, unsigned int b) {
	return (a + b - 1) / b;
}

// UTF-8 to UTF-16, returns the number of code units written
static int utf16(const char* src, unsigned short* dst, int max) {
	const unsigned char* s = (const unsigned char*)src;
	int len = 0;
	unsigned int cp;
	int ext;
	while (*s && (len < max)) {
		cp = *s++;
		if (cp < 0x80) {
			ext = 0;
		} else if ((cp & 0xe0) == 0xc0) {
			cp &= 0x1f; ext = 1;
		} else if ((cp & 0xf0) == 0xe0) {
			cp &= 0x0f; ext = 2;
		} else if ((cp & 0xf8) == 0xf0) {
			cp &= 0x07; ext = 3;
		} else {
			cp = '_'; ext = 0;		// stray continuation byte
		}
		while (ext > 0) {
			if ((*s & 0xc0) != 0x80) { cp = '_'; break; }
			cp = (cp << 6) | (*s++ & 0x3f);
			ext--;
		}
		if (cp >= 0x10000) {
			if (len + 2 > max) break;
			cp -= 0x10000;
			dst[len++] = 0xd800 | (cp >> 10);
			dst[len++] = 0xdc00 | (cp & 0x3ff);
		} else {
			dst[len++] = cp;
		}
	}
	dst[len] = 0;
	return len;
}

static int sname_bad(int c) {
	if (c < 0x20) return 1;
	return (strchr("\"*+,/:;<=>?[\\]|", c) != NULL);
}

// one character of an 8.3 name: uppercase ASCII, cyrillic folded into cp866
// (that is what a DOS side expects to see), anything else becomes '_'.
// -1 means the character is dropped; lossy is raised whenever the result
// doesn't repeat the original.
static int sname_char(unsigned int chr, int* lossy) {
	if ((chr == ' ') || (chr == '.')) {
		*lossy = 1;
		return -1;
	}
	if (chr < 0x80) {
		if ((chr >= 'a') && (chr <= 'z')) {
			*lossy = 1;
			return chr - 0x20;
		}
		if (sname_bad(chr)) {
			*lossy = 1;
			return '_';
		}
		return chr;
	}
	*lossy = 1;
	if ((chr >= 0x430) && (chr <= 0x44f)) chr -= 0x20;	// lowercase cyrillic
	if (chr == 0x451) chr = 0x401;				// yo
	if ((chr >= 0x410) && (chr <= 0x42f)) return 0x80 + chr - 0x410;
	if (chr == 0x401) return 0xf0;
	return '_';
}

// build a space padded 8.3 name; returns 1 if it doesn't match the real name,
// *ulen is the length of the real one in UTF-16
static int sname_make(const char* name, char* dst, int* ulen) {
	unsigned short buf[VF_MAXNAME + 1];
	int len = utf16(name, buf, VF_MAXNAME);
	int lossy = 0;
	int dot = -1;
	int i, c, pos;
	*ulen = len;
	memset(dst, ' ', 11);
	dst[11] = 0;
	for (i = len - 1; i > 0; i--) {			// last dot, a leading one belongs to the base
		if (buf[i] == '.') {
			dot = i;
			break;
		}
	}
	pos = 0;
	for (i = 0; i < ((dot < 0) ? len : dot); i++) {
		c = sname_char(buf[i], &lossy);
		if (c < 0) continue;
		if (pos < 8) dst[pos++] = c;
		else lossy = 1;
	}
	if (pos == 0) {
		dst[0] = '_';
		lossy = 1;
	}
	if (dot >= 0) {
		pos = 8;
		for (i = dot + 1; i < len; i++) {
			c = sname_char(buf[i], &lossy);
			if (c < 0) continue;
			if (pos < 11) dst[pos++] = c;
			else lossy = 1;
		}
	}
	return lossy;
}

static unsigned char sname_sum(const char* sname) {
	unsigned char sum = 0;
	int i;
	for (i = 0; i < 11; i++)
		sum = ((sum & 1) << 7) + (sum >> 1) + (unsigned char)sname[i];
	return sum;
}

// the key of an 8.3 name in the table: siblings share a parent
static unsigned int sname_hash(int parent, const char* sname) {
	unsigned int res = (unsigned int)parent;
	int i;
	for (i = 0; i < 11; i++)
		res = res * 31 + (unsigned char)sname[i];
	return res * 0x9e3779b1u;
}

static unsigned int stab_pos(vFat* vf, unsigned int hash) {
	return (hash ^ (hash >> 16)) & (vf->scap - 1);
}

static int sname_taken(vFat* vf, int parent, const char* sname) {
	unsigned int hash = sname_hash(parent, sname);
	unsigned int pos;
	int idx;
	for (pos = stab_pos(vf, hash); (idx = vf->stab[pos]) >= 0; pos = (pos + 1) & (vf->scap - 1)) {
		if ((vf->node[idx].shash == hash) && (vf->node[idx].parent == parent) && !memcmp(vf->node[idx].sname, sname, 11))
			return 1;
	}
	return 0;
}

static void stab_put(vFat* vf, int idx) {
	unsigned int pos = stab_pos(vf, vf->node[idx].shash);
	while (vf->stab[pos] >= 0)
		pos = (pos + 1) & (vf->scap - 1);
	vf->stab[pos] = idx;
}

// keep the table at most half full; 0 if out of memory
static int stab_room(vFat* vf) {
	unsigned int cap;
	int* tab;
	int i;
	if ((unsigned int)(vf->nodes + 1) * 2 <= vf->scap) return 1;
	cap = vf->scap ? (vf->scap * 2) : 1024;
	tab = (int*)malloc(cap * sizeof(int));
	if (!tab) return 0;
	memset(tab, 0xff, cap * sizeof(int));
	free(vf->stab);
	vf->stab = tab;
	vf->scap = cap;
	for (i = 0; i < vf->nodes; i++)
		stab_put(vf, i);
	return 1;
}

// make the 8.3 name unique among the siblings of parent: BASE~1..~4 first,
// then two letters, a hash of the long name and ~1..~9, as Windows does - a
// folder of names alike would otherwise try every number for every name
static void sname_uniq(vFat* vf, int parent, char* sname, const char* name) {
	const unsigned char* s;
	char base[12];
	char tail[16];
	unsigned int lhash = 0x811c9dc5;
	int len, pos, n;
	for (s = (const unsigned char*)name; *s; s++)
		lhash = (lhash ^ *s) * 0x01000193;
	memcpy(base, sname, 12);
	for (n = 0; n < 1000000; n++) {
		if (!sname_taken(vf, parent, sname)) return;
		memcpy(sname, base, 11);
		if (n < 4)
			sprintf(tail, "~%i", n + 1);
		else
			sprintf(tail, "%04X~%i", (lhash + (n - 4) / 9) & 0xffff, (n - 4) % 9 + 1);
		len = strlen(tail);
		pos = 8 - len;
		while ((pos > 0) && (sname[pos - 1] == ' ')) pos--;
		memcpy(sname + pos, tail, len);
		memset(sname + pos + len, ' ', 8 - pos - len);
	}
}

// tree

vFat* vfat_create(void) {
	vFat* vf = (vFat*)malloc(sizeof(vFat));
	if (!vf) return NULL;
	memset(vf, 0x00, sizeof(vFat));
	vf->spc = VF_SPC;
	vf->cnode = -1;
	vf->hddport = -1;				// only an IDE disk serves a boot sector
	vf->serial = 0x58504543;			// XPEC
	return vf;
}

void vfat_free(vFat* vf) {
	int i;
	if (!vf) return;
	if (vf->cfile) fclose(vf->cfile);
	for (i = 0; i < vf->nodes; i++) {
		free(vf->node[i].name);
		free(vf->node[i].host);
	}
	free(vf->node);
	free(vf->stab);
	free(vf->order);
	free(vf);
}

static char* xstrdup(const char* s) {
	char* res;
	if (!s) return NULL;
	res = (char*)malloc(strlen(s) + 1);
	if (res) strcpy(res, s);
	return res;
}

int vfat_add(vFat* vf, int parent, const char* name, const char* host, unsigned int size, unsigned int mtime, int isdir) {
	vfNode* nod;
	char sname[12];
	int idx, prev, lfn, ecnt, ulen;
	time_t tim;
	struct tm* tms;
	if (!vf) return -1;
	if (!name) name = "";
	lfn = sname_make(name, sname, &ulen);
	ecnt = lfn ? (divup(ulen, 13) + 1) : 1;		// the 8.3 entry and 13 characters per long one
	if ((parent >= 0) && (vf->node[parent].dents + ecnt > VF_MAXDENTS)) return VF_FULL;
	if (!stab_room(vf)) return -1;
	if (vf->nodes >= vf->cap) {
		vf->cap = vf->cap ? (vf->cap * 2) : 64;
		vf->node = (vfNode*)realloc(vf->node, vf->cap * sizeof(vfNode));
		if (!vf->node) return -1;
	}
	idx = vf->nodes++;
	nod = vf->node + idx;
	memset(nod, 0x00, sizeof(vfNode));
	nod->isdir = isdir ? 1 : 0;
	nod->name = xstrdup(name);
	nod->host = xstrdup(host);
	nod->parent = parent;
	nod->child = -1;
	nod->last = -1;
	nod->next = -1;
	nod->size = isdir ? 0 : size;
	nod->lfn = lfn ? 1 : 0;
	nod->ecnt = ecnt;
	nod->dents = (parent < 0) ? 2 : 3;		// the label or dot and dotdot, and the end marker
	memcpy(nod->sname, sname, 12);
	sname_uniq(vf, parent, nod->sname, name);
	nod->shash = sname_hash(parent, nod->sname);
	stab_put(vf, idx);
	tim = (time_t)mtime;
	tms = localtime(&tim);
	if (tms && (tms->tm_year >= 80)) {
		nod->date = ((tms->tm_year - 80) << 9) | ((tms->tm_mon + 1) << 5) | tms->tm_mday;
		nod->time = (tms->tm_hour << 11) | (tms->tm_min << 5) | (tms->tm_sec >> 1);
	} else {
		nod->date = (1 << 5) | 1;		// 1980-01-01
		nod->time = 0;
	}
	if (parent >= 0) {				// append, keeping the order the scanner used
		vf->node[parent].dents += ecnt;
		prev = vf->node[parent].last;
		if (prev < 0)
			vf->node[parent].child = idx;
		else
			vf->node[prev].next = idx;
		vf->node[parent].last = idx;
	}
	return idx;
}

// layout

static unsigned int dir_bytes(vFat* vf, int idx) {
	return vf->node[idx].dents * 32;
}

static void vfat_geometry(vFat* vf, unsigned int volume) {
	unsigned int fsz, nsz;
	int i;
	vf->volume = volume;
	vf->partStart = VF_PARTSTART;
	vf->partSize = volume - VF_PARTSTART;
	vf->reserved = VF_RESERVED;
	fsz = divup((vf->partSize / vf->spc + 2) * 4, VF_SECSIZE);
	for (i = 0; i < 8; i++) {			// FAT size depends on the cluster count and back
		vf->clusters = (vf->partSize - vf->reserved - 2 * fsz) / vf->spc;
		nsz = divup((vf->clusters + 2) * 4, VF_SECSIZE);
		if (nsz == fsz) break;
		fsz = nsz;
	}
	vf->fatSize = fsz;
	vf->dataStart = vf->reserved + 2 * fsz;
	vf->clusters = (vf->partSize - vf->dataStart) / vf->spc;
}

// NedoOS boots from an HDD through the IS-DOS boot sector at LBA 2..3, which
// hddfdisk takes from ZXLDR and points at that file's first sector; do the same.
// ZXLDR carries the sector twice: for Nemo IDE ports first, then for ATM ones.
#define VF_ZXLDR_MAX	0x10000

static void vfat_hddboot(vFat* vf) {
	static const unsigned char sig[] = {0x18, 0x38, 0x00, 0xc3, 0x83, 0x5e, 0x18, 0x40};
	unsigned char* buf;
	unsigned char* sec;
	vfNode* nod;
	FILE* file;
	unsigned int len, i;
	int idx;
	vf->hddboots = 0;
	for (idx = vf->node[0].child; idx >= 0; idx = vf->node[idx].next) {
		if (!vf->node[idx].isdir && !memcmp(vf->node[idx].sname, "ZXLDR      ", 11)) break;
	}
	if (idx < 0) return;
	nod = vf->node + idx;
	if (!nod->host || !nod->clust || (nod->size > VF_ZXLDR_MAX)) return;
	file = fopen(nod->host, "rb");
	if (!file) return;
	buf = (unsigned char*)malloc(nod->size);
	len = buf ? fread(buf, 1, nod->size, file) : 0;
	fclose(file);
	for (i = 0; (i + VF_SECSIZE <= len) && (vf->hddboots < VF_HDDBOOTS); i++) {
		if (memcmp(buf + i, sig, sizeof(sig))) continue;
		sec = vf->hddboot[vf->hddboots++];
		memcpy(sec, buf + i, VF_SECSIZE);
		memset(sec + 8, 0x00, 0x2e - 8);		// LBA mode, no IS-DOS partitions
		wr32(sec + 10, vf->partStart + vf->dataStart + (nod->clust - 2) * vf->spc);
		i += VF_SECSIZE - 1;
	}
	free(buf);
}

int vfat_build(vFat* vf, unsigned int minsec) {
	unsigned int volume, cbytes, need;
	int i;
	if (!vf || (vf->nodes < 1)) return 0;
	volume = VF_MINVOL;
	while (volume < minsec)
		volume <<= 1;
	cbytes = vf->spc * VF_SECSIZE;
	while (1) {
		vfat_geometry(vf, volume);
		need = 0;
		for (i = 0; i < vf->nodes; i++) {
			if (vf->node[i].isdir)
				need += divup(dir_bytes(vf, i), cbytes);
			else
				need += divup(vf->node[i].size, cbytes);
		}
		if (need <= vf->clusters) break;
		if (volume >= VF_MAXVOL) return 0;	// doesn't fit even on the largest card
		volume <<= 1;
	}
	// hand out contiguous runs; node 0 is the root and takes cluster 2
	free(vf->order);
	vf->order = (int*)malloc(vf->nodes * sizeof(int));
	if (!vf->order) return 0;
	vf->ordcnt = 0;
	vf->used = 0;
	for (i = 0; i < vf->nodes; i++) {
		need = vf->node[i].isdir ? divup(dir_bytes(vf, i), cbytes) : divup(vf->node[i].size, cbytes);
		if (need == 0) {
			vf->node[i].clust = 0;
			vf->node[i].nclust = 0;
			continue;
		}
		vf->node[i].clust = 2 + vf->used;
		vf->node[i].nclust = need;
		vf->used += need;
		vf->order[vf->ordcnt++] = i;
	}
	vfat_hddboot(vf);
	return 1;
}

// find the node owning a cluster, -1 if the cluster is free
static int clust_node(vFat* vf, unsigned int clu) {
	int a = 0;
	int b = vf->ordcnt - 1;
	int m, idx;
	while (a <= b) {
		m = (a + b) / 2;
		idx = vf->order[m];
		if (clu < vf->node[idx].clust) b = m - 1;
		else if (clu >= vf->node[idx].clust + vf->node[idx].nclust) a = m + 1;
		else return idx;
	}
	return -1;
}

// sectors

static void vfat_mbr(vFat* vf, unsigned char* dst) {
	unsigned char* p = dst + 446;
	p[0] = 0x00;					// not bootable
	p[1] = 0x01; p[2] = 0x01; p[3] = 0x00;		// CHS start (fake)
	p[4] = 0x0c;					// FAT32 LBA
	p[5] = 0xfe; p[6] = 0xff; p[7] = 0xff;		// CHS end (fake)
	wr32(p + 8, vf->partStart);
	wr32(p + 12, vf->partSize);
	dst[510] = 0x55;
	dst[511] = 0xaa;
}

static void vfat_boot(vFat* vf, unsigned char* dst) {
	dst[0] = 0xeb; dst[1] = 0x58; dst[2] = 0x90;
	memcpy(dst + 3, "MSWIN4.1", 8);
	wr16(dst + 11, VF_SECSIZE);
	dst[13] = vf->spc;
	wr16(dst + 14, vf->reserved);
	dst[16] = 2;					// two FATs
	wr16(dst + 17, 0);				// no fixed root directory on FAT32
	wr16(dst + 19, 0);
	dst[21] = 0xf8;					// fixed disk
	wr16(dst + 22, 0);
	wr16(dst + 24, 63);				// sectors per track
	wr16(dst + 26, 255);				// heads
	wr32(dst + 28, vf->partStart);
	wr32(dst + 32, vf->partSize);
	wr32(dst + 36, vf->fatSize);
	wr16(dst + 40, 0);				// both FATs are live
	wr16(dst + 42, 0);
	wr32(dst + 44, 2);				// root directory cluster
	wr16(dst + 48, 1);				// FSInfo sector
	wr16(dst + 50, 6);				// backup boot sector
	dst[64] = 0x80;
	dst[66] = 0x29;
	wr32(dst + 67, vf->serial);
	memcpy(dst + 71, VF_LABEL, 11);
	memcpy(dst + 82, "FAT32   ", 8);
	dst[510] = 0x55;
	dst[511] = 0xaa;
}

static void vfat_fsinfo(vFat* vf, unsigned char* dst) {
	wr32(dst, 0x41615252);
	wr32(dst + 484, 0x61417272);
	wr32(dst + 488, vf->clusters - vf->used);	// free clusters
	wr32(dst + 492, vf->used + 2);			// next free cluster
	dst[510] = 0x55;
	dst[511] = 0xaa;
}

static void vfat_fat(vFat* vf, unsigned int sec, unsigned char* dst) {
	unsigned int first = sec * (VF_SECSIZE / 4);
	unsigned int clu, val;
	int i, idx;
	for (i = 0; i < VF_SECSIZE / 4; i++) {
		clu = first + i;
		if (clu == 0) val = 0x0ffffff8;
		else if (clu == 1) val = VF_EOC;
		else if (clu >= vf->clusters + 2) val = 0;
		else {
			idx = clust_node(vf, clu);
			if (idx < 0) val = 0;		// free
			else if (clu == vf->node[idx].clust + vf->node[idx].nclust - 1) val = VF_EOC;
			else val = clu + 1;
		}
		wr32(dst + i * 4, val);
	}
}

// one 32-byte 8.3 entry
static void vfat_entry(unsigned char* dst, const char* sname, int attr, unsigned int clust, unsigned int size, unsigned short date, unsigned short tim) {
	memcpy(dst, sname, 11);
	dst[11] = attr;
	wr16(dst + 14, tim);
	wr16(dst + 16, date);
	wr16(dst + 18, date);
	wr16(dst + 20, (clust >> 16) & 0xffff);
	wr16(dst + 22, tim);
	wr16(dst + 24, date);
	wr16(dst + 26, clust & 0xffff);
	wr32(dst + 28, size);
}

// one long name entry: part is 1..n, last marks the one holding the tail
static void vfat_lfn(unsigned char* dst, unsigned short* name, int len, int part, int last, unsigned char sum) {
	static const int pos[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
	int i, chr;
	int base = (part - 1) * 13;
	dst[0] = part | (last ? 0x40 : 0x00);
	dst[11] = 0x0f;
	dst[13] = sum;
	for (i = 0; i < 13; i++) {
		if (base + i < len) chr = name[base + i];
		else if (base + i == len) chr = 0x0000;
		else chr = 0xffff;
		wr16(dst + pos[i], chr);
	}
}

// fill one sector of a directory
static void vfat_dir(vFat* vf, int idx, unsigned int sec, unsigned char* dst) {
	unsigned short name[VF_MAXNAME + 1];
	unsigned int first = sec * 16;			// first entry in this sector
	unsigned int ent = 0;
	char dots[12];
	int chd, len, parts, i, part;
	vfNode* nod;

	if (idx == 0) {					// the label lives in the root directory too
		if ((ent >= first) && (ent < first + 16))
			vfat_entry(dst + (ent - first) * 32, VF_LABEL, 0x08, 0, 0, 0, 0);
		ent++;
	} else {					// dot and dotdot for a non-root directory
		nod = vf->node + idx;
		memset(dots, ' ', 11); dots[11] = 0; dots[0] = '.';
		if ((ent >= first) && (ent < first + 16))
			vfat_entry(dst + (ent - first) * 32, dots, 0x10, nod->clust, 0, nod->date, nod->time);
		ent++;
		dots[1] = '.';
		if ((ent >= first) && (ent < first + 16)) {
			i = nod->parent;
			vfat_entry(dst + (ent - first) * 32, dots, 0x10, (i > 0) ? vf->node[i].clust : 0, 0, nod->date, nod->time);
		}
		ent++;
	}
	chd = vf->node[idx].child;
	while (chd >= 0) {
		nod = vf->node + chd;
		if (ent + nod->ecnt > first) {		// something of this child lands here
			if (nod->lfn) {
				len = utf16(nod->name, name, VF_MAXNAME);
				parts = divup(len, 13);
				for (i = 0; i < parts; i++) {
					part = parts - i;	// long name entries come tail first
					if ((ent >= first) && (ent < first + 16))
						vfat_lfn(dst + (ent - first) * 32, name, len, part, (i == 0), sname_sum(nod->sname));
					ent++;
				}
			}
			if ((ent >= first) && (ent < first + 16))
				vfat_entry(dst + (ent - first) * 32, nod->sname, nod->isdir ? 0x10 : 0x21, nod->clust, nod->size, nod->date, nod->time);
			ent++;
		} else {
			ent += nod->ecnt;
		}
		if (ent >= first + 16) return;		// sector filled
		chd = nod->next;
	}
}

// fill one sector of a file
static void vfat_file(vFat* vf, int idx, unsigned int sec, unsigned char* dst) {
	unsigned int off = sec * VF_SECSIZE;
	size_t rd;
	if (off >= vf->node[idx].size) return;
	if (vf->cnode != idx) {				// keep the last file open: sectors come in runs
		if (vf->cfile) fclose(vf->cfile);
		vf->cfile = vf->node[idx].host ? fopen(vf->node[idx].host, "rb") : NULL;
		vf->cnode = vf->cfile ? idx : -1;
	}
	if (!vf->cfile) return;
	if (fseek(vf->cfile, off, SEEK_SET)) return;
	rd = vf->node[idx].size - off;
	if (rd > VF_SECSIZE) rd = VF_SECSIZE;
	rd = fread(dst, 1, rd, vf->cfile);		// a short read leaves the tail zeroed
}

int vfat_read(vFat* vf, unsigned int lba, unsigned char* dst) {
	unsigned int rel, clu, sec;
	int idx;
	if (!vf || !dst) return 0;
	memset(dst, 0x00, VF_SECSIZE);
	if (lba >= vf->volume) return 0;
	if (lba == 0) {
		vfat_mbr(vf, dst);
	} else if (lba < vf->partStart) {
		// gap between the MBR and the partition
		if (((lba == 2) || (lba == 3)) && (vf->hddport >= 0) && (vf->hddport < vf->hddboots))
			memcpy(dst, vf->hddboot[vf->hddport], VF_SECSIZE);
	} else {
		rel = lba - vf->partStart;
		if (rel < vf->reserved) {
			switch (rel) {
				case 0:
				case 6: vfat_boot(vf, dst); break;	// + backup copy
				case 1:
				case 7: vfat_fsinfo(vf, dst); break;
			}
		} else if (rel < vf->dataStart) {
			vfat_fat(vf, (rel - vf->reserved) % vf->fatSize, dst);
		} else {
			clu = (rel - vf->dataStart) / vf->spc + 2;
			sec = (rel - vf->dataStart) % vf->spc;
			idx = clust_node(vf, clu);
			if (idx >= 0) {
				sec += (clu - vf->node[idx].clust) * vf->spc;
				if (vf->node[idx].isdir)
					vfat_dir(vf, idx, sec, dst);
				else
					vfat_file(vf, idx, sec, dst);
			}
		}
	}
	return 1;
}

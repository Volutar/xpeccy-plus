#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vfat_int.h"
#include "defines.h"
#include "xlog.h"

// The guest's writes to a folder volume, and the folder following them.
//
// What the guest reads is always what it wrote. From the first write on, both
// FAT copies are held as arrays and every directory sector as written data;
// file data is read from the host files through a map of clusters, and a sector
// the guest wrote is kept as written until the host file gives it back byte for
// byte. The folder follows in vfat_sync(), after a quiet spell: the FAT and the
// directories are read back as the guest left them, matched against the tree of
// the last sync, and the difference is done to the host - everything needed read
// first, moved things parked under temporary names, deletions, then everything
// put in place. A file is deleted only once its clusters are free; a format or a
// mass delete stops the folder following, and the volume lives on in memory.

#define VFW_QUIET	25		// frames with no write before the folder follows
#define VFW_FREE	-1		// own[]: a cluster no node holds
#define VFW_MAXDEPTH	64
#define VFW_MAXENTS	(1 << 20)
#define VFW_MASSDEL	32		// deleting more than this and nine tenths of the tree at once is a format
#define VFW_TMPNAME	200		// bytes of the host name a temporary name carries

typedef struct {
	unsigned int lba;
	int next;			// in its bucket, or in the free list
	unsigned int gen;		// the write that left it
	unsigned char data[VF_SECSIZE];
} vfSec;

struct vfWr {
	int detached;
	int quiet;
	int pinwrote;			// the guest changed a pinned folder: 1, 2 once told (vfat_pin_news)
	unsigned int wgen;		// writes taken
	unsigned int sgen;		// ...as of the last sync
	int pending;
	unsigned int fatEnt;		// entries in one FAT copy
	unsigned int* fat[2];
	int* own;			// per cluster: the node whose chain holds it, VFW_FREE
	unsigned int* oidx;		// its place in that chain
	unsigned int* mark;		// per cluster: the sync that found it in a chain
	unsigned int* freed;		// per cluster: the write that freed it in the FAT
	unsigned int stamp;
	vfSec* sec;			// sectors the host does not give back (yet)
	int nsec;
	int capsec;
	int freesec;
	int* bucket;
	unsigned int nbucket;		// a power of two
	unsigned char boot[VF_SECSIZE];	// the boot sector and the MBR as built: a write that
	unsigned char mbr[VF_SECSIZE];	// changes their layout is a format
	int tmpn;			// temporary names handed out
	int errors;			// host operations the last sync could not do
	char fnames[400];		// the first few of what they were for, for the user
	int toldErrors;			// the user heard of them
	int toldDetach;			// ...and of the detach
	int quietDetach;		// the user's own word: nothing to tell
	const char* why;		// what stopped the folder following
};

// == sectors kept in memory ==

static unsigned int ovl_hash(vfWr* w, unsigned int lba) {
	return (lba * 2654435761u) & (w->nbucket - 1);
}

static int ovl_find(vfWr* w, unsigned int lba) {
	int i;
	for (i = w->bucket[ovl_hash(w, lba)]; i >= 0; i = w->sec[i].next)
		if (w->sec[i].lba == lba) return i;
	return -1;
}

static int ovl_grow(vfWr* w) {
	unsigned int nb = w->nbucket * 2;
	int* b = (int*)malloc(nb * sizeof(int));
	int i;
	if (!b) return 0;
	memset(b, 0xff, nb * sizeof(int));
	free(w->bucket);
	w->bucket = b;
	w->nbucket = nb;
	for (i = 0; i < w->capsec; i++) {
		if (w->sec[i].lba == 0xffffffff) continue;
		unsigned int h = ovl_hash(w, w->sec[i].lba);
		w->sec[i].next = w->bucket[h];
		w->bucket[h] = i;
	}
	return 1;
}

static int ovl_put(vfWr* w, unsigned int lba, const unsigned char* data, unsigned int gen) {
	int i = ovl_find(w, lba);
	if (i < 0) {
		if ((unsigned int)w->nsec + 1 > w->nbucket * 2) {
			// the free list is threaded through next, which a rehash rewrites
			int f;
			for (f = w->freesec; f >= 0; f = w->sec[f].next) w->sec[f].lba = 0xffffffff;
			if (!ovl_grow(w)) return 0;
			w->freesec = -1;
			for (f = w->capsec - 1; f >= 0; f--) {
				if (w->sec[f].lba != 0xffffffff) continue;
				w->sec[f].next = w->freesec;
				w->freesec = f;
			}
		}
		if (w->freesec < 0) {
			int cap = w->capsec ? w->capsec * 2 : 256;
			vfSec* s = (vfSec*)realloc(w->sec, cap * sizeof(vfSec));
			if (!s) return 0;
			w->sec = s;
			for (i = cap - 1; i >= w->capsec; i--) {
				w->sec[i].lba = 0xffffffff;
				w->sec[i].next = w->freesec;
				w->freesec = i;
			}
			w->capsec = cap;
		}
		i = w->freesec;
		w->freesec = w->sec[i].next;
		unsigned int h = ovl_hash(w, lba);
		w->sec[i].lba = lba;
		w->sec[i].next = w->bucket[h];
		w->bucket[h] = i;
		w->nsec++;
	}
	w->sec[i].gen = gen;
	memcpy(w->sec[i].data, data, VF_SECSIZE);
	return 1;
}

static void ovl_drop(vfWr* w, int i) {
	unsigned int h = ovl_hash(w, w->sec[i].lba);
	int* p = &w->bucket[h];
	while (*p != i) p = &w->sec[*p].next;
	*p = w->sec[i].next;
	w->sec[i].lba = 0xffffffff;
	w->sec[i].next = w->freesec;
	w->freesec = i;
	w->nsec--;
}

// == the volume as the guest sees it ==

static unsigned int clu_lba(vFat* vf, unsigned int clu, unsigned int sec) {
	return vf->partStart + vf->dataStart + (clu - 2) * vf->spc + sec;
}

// a file sector by the cluster map
static void vfw_gen_data(vFat* vf, unsigned int clu, unsigned int sec, unsigned char* dst) {
	vfWr* w = vf->wr;
	int o;
	memset(dst, 0x00, VF_SECSIZE);
	if (clu >= vf->clusters + 2) return;
	o = w->own[clu];
	if ((o < 0) || vf->node[o].isdir || vf->node[o].dead) return;
	vf_gen_file(vf, o, w->oidx[clu] * vf->spc + sec, dst);
}

int vfw_read(vFat* vf, unsigned int lba, unsigned char* dst) {
	vfWr* w = vf->wr;
	unsigned int rel, s, copy, k;
	int i;
	memset(dst, 0x00, VF_SECSIZE);
	if (lba >= vf->volume) return 0;
	if ((i = ovl_find(w, lba)) >= 0) {
		memcpy(dst, w->sec[i].data, VF_SECSIZE);
		return 1;
	}
	rel = lba - vf->partStart;
	if ((lba < vf->partStart) || (rel < vf->reserved)) {
		vf_gen_reserved(vf, lba, dst);
	} else if (rel < vf->dataStart) {
		copy = (rel - vf->reserved) / vf->fatSize;
		s = (rel - vf->reserved) % vf->fatSize;
		for (k = 0; k < VF_SECSIZE / 4; k++)
			vf_wr32(dst + k * 4, w->fat[copy][s * (VF_SECSIZE / 4) + k]);
	} else {
		rel -= vf->dataStart;
		vfw_gen_data(vf, rel / vf->spc + 2, rel % vf->spc, dst);
	}
	return 1;
}

// the first write: the FATs into arrays, the directories into memory, the map of clusters
static int vfw_live(vFat* vf) {
	unsigned char buf[VF_SECSIZE];
	vfWr* w = (vfWr*)calloc(1, sizeof(vfWr));
	unsigned int s, k, c, n;
	int i;
	if (!w) return 0;
	w->fatEnt = vf->fatSize * (VF_SECSIZE / 4);
	w->fat[0] = (unsigned int*)malloc(w->fatEnt * sizeof(unsigned int));
	w->fat[1] = (unsigned int*)malloc(w->fatEnt * sizeof(unsigned int));
	n = vf->clusters + 2;
	w->own = (int*)malloc(n * sizeof(int));
	w->oidx = (unsigned int*)calloc(n, sizeof(unsigned int));
	w->mark = (unsigned int*)calloc(n, sizeof(unsigned int));
	w->freed = (unsigned int*)calloc(n, sizeof(unsigned int));
	w->nbucket = 1024;
	w->bucket = (int*)malloc(w->nbucket * sizeof(int));
	w->freesec = -1;
	if (!w->fat[0] || !w->fat[1] || !w->own || !w->oidx || !w->mark || !w->freed || !w->bucket) {
		free(w->fat[0]); free(w->fat[1]); free(w->own); free(w->oidx); free(w->mark); free(w->freed); free(w->bucket);
		free(w);
		return 0;
	}
	memset(w->bucket, 0xff, w->nbucket * sizeof(int));
	for (s = 0; s < vf->fatSize; s++) {
		vf_gen_fat(vf, s, buf);
		for (k = 0; k < VF_SECSIZE / 4; k++)
			w->fat[0][s * (VF_SECSIZE / 4) + k] = vf_rd32(buf + k * 4);
	}
	memcpy(w->fat[1], w->fat[0], w->fatEnt * sizeof(unsigned int));
	for (c = 0; c < n; c++) w->own[c] = VFW_FREE;
	for (i = 0; i < vf->nodes; i++) {
		for (k = 0; k < vf->node[i].nclust; k++) {
			w->own[vf->node[i].clust + k] = i;
			w->oidx[vf->node[i].clust + k] = k;
		}
	}
	memset(buf, 0x00, VF_SECSIZE);
	vf_gen_reserved(vf, vf->partStart, w->boot);
	vf_gen_reserved(vf, 0, w->mbr);
	vf->wr = w;
	for (i = 0; i < vf->nodes; i++) {
		if (!vf->node[i].isdir) continue;
		for (s = 0; s < vf->node[i].nclust * vf->spc; s++) {
			memset(buf, 0x00, VF_SECSIZE);
			vf_gen_dir(vf, i, s, buf);
			if (!ovl_put(w, clu_lba(vf, vf->node[i].clust + s / vf->spc, s % vf->spc), buf, 0)) {
				vfw_free(vf);
				return 0;
			}
		}
	}
	return 1;
}

static void vfw_detach(vFat* vf, const char* why, int byUser) {
	if (vf->wr->detached) return;
	vf->wr->detached = 1;
	vf->wr->why = why;
	vf->wr->quietDetach = byUser;
	xlog(XLG_DISK, XLL_WARN, "folder %s: %s - no longer following the machine, its writes stay in memory", vf->root ? vf->root : "?", why);
}

int vfat_write(vFat* vf, unsigned int lba, const unsigned char* src) {
	vfWr* w;
	unsigned int rel, copy, s, k;
	if (!vf || !src || !vf->writable || (lba >= vf->volume)) return 0;
	if (!vf->wr && !vfw_live(vf)) return 0;
	if (vf->frozen) vfw_detach(vf, vf->frozen, 1);
	w = vf->wr;
	w->wgen++;
	rel = lba - vf->partStart;
	if ((lba >= vf->partStart) && (rel >= vf->reserved) && (rel < vf->dataStart)) {
		copy = (rel - vf->reserved) / vf->fatSize;
		s = (rel - vf->reserved) % vf->fatSize;
		for (k = 0; k < VF_SECSIZE / 4; k++) {
			unsigned int c = s * (VF_SECSIZE / 4) + k;
			unsigned int v = vf_rd32(src + k * 4);
			// a cluster the guest frees: what was written to it before is garbage now
			if (!copy && (c < vf->clusters + 2) && (w->fat[0][c] & 0x0fffffff) && !(v & 0x0fffffff))
				w->freed[c] = w->wgen;
			w->fat[copy][c] = v;
		}
	} else {
		if ((lba == vf->partStart) || (lba == vf->partStart + 6)) {
			// the layout and the serial: what a format writes. The label may change.
			if (memcmp(src + 11, w->boot + 11, 71 - 11)) vfw_detach(vf, "the machine formatted it", 0);
		} else if (lba == 0) {
			if (memcmp(src + 446, w->mbr + 446, 64) || (src[510] != 0x55) || (src[511] != 0xaa))
				vfw_detach(vf, "the machine rewrote its partition table", 0);
		}
		if (!ovl_put(w, lba, src, w->wgen)) {
			vfw_detach(vf, "there was no memory left for what the machine writes", 0);
			return 0;
		}
	}
	w->quiet = 0;
	return 1;
}

int vfat_tick(vFat* vf) {
	vfWr* w;
	if (!vf || vf->hold || !(w = vf->wr) || w->detached || (w->wgen == w->sgen)) return 0;
	if (++w->quiet < VFW_QUIET) return 0;
	return vfat_sync(vf, 0);
}

int vfat_pending(vFat* vf) {
	if (!vf || !vf->wr) return 0;
	return vf->wr->pending + ((vf->wr->wgen != vf->wr->sgen) ? 1 : 0);
}

int vfat_detached(vFat* vf) {
	return (vf && vf->wr) ? vf->wr->detached : 0;
}

int vfat_pin_news(vFat* vf) {
	if (!vf || !vf->wr || (vf->wr->pinwrote != 1)) return 0;
	vf->wr->pinwrote = 2;
	return 1;
}

void vfat_flush(vFat* vf) {
	vfWr* w;
	if (!vf || !(w = vf->wr) || vf->hold || w->detached || (w->wgen == w->sgen)) return;
	vfat_sync(vf, 1);
}

int vfat_news(vFat* vf, const char** why, int* errors) {
	vfWr* w;
	if (!vf || !(w = vf->wr)) return 0;
	if (w->detached && !w->quietDetach && !w->toldDetach) {
		w->toldDetach = 1;
		if (why) *why = w->why;
		return 1;
	}
	if (w->errors && !w->toldErrors) {
		w->toldErrors = 1;
		if (errors) *errors = w->errors;
		if (why) *why = w->fnames;
		return 2;
	}
	return 0;
}

int vfat_waiting(vFat* vf) {
	return (vf && vf->hold && vf->wr && !vf->wr->detached && (vf->wr->wgen != vf->wr->sgen)) ? 1 : 0;
}

void vfat_release(vFat* vf) {
	if (vf) vf->hold = 0;
}

void vfat_read_only(vFat* vf, const char* why) {
	if (!vf || !vf->writable) return;
	vfat_flush(vf);		// what was written under R/W reaches the folder first; on hold, nothing does
	vf->hold = 0;
	vf->frozen = why;
	if (vf->wr) vfw_detach(vf, why, 1);
}

void vfw_free(vFat* vf) {
	vfWr* w = vf->wr;
	if (!w) return;
	if (vf->hold && (w->wgen != w->sgen))
		xlog(XLG_DISK, XLL_WARN, "folder %s: writing to it was not allowed yet, the guest's writes are dropped", vf->root ? vf->root : "?");
	else
		vfat_flush(vf);
	if (w->pending)
		xlog(XLG_DISK, XLL_WARN, "folder %s: %i changes of the guest's were not carried over", vf->root ? vf->root : "?", w->pending);
	free(w->fat[0]);
	free(w->fat[1]);
	free(w->own);
	free(w->oidx);
	free(w->mark);
	free(w->freed);
	free(w->sec);
	free(w->bucket);
	free(w);
	vf->wr = NULL;
}

// == reading the guest's tree back ==

// cp866 0x80..0xFF, the 8.3 names' code page
static const unsigned short cp866[128] = {
	0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417,0x0418,0x0419,0x041a,0x041b,0x041c,0x041d,0x041e,0x041f,
	0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427,0x0428,0x0429,0x042a,0x042b,0x042c,0x042d,0x042e,0x042f,
	0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,0x0438,0x0439,0x043a,0x043b,0x043c,0x043d,0x043e,0x043f,
	0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255d,0x255c,0x255b,0x2510,
	0x2514,0x2534,0x252c,0x251c,0x2500,0x253c,0x255e,0x255f,0x255a,0x2554,0x2569,0x2566,0x2560,0x2550,0x256c,0x2567,
	0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256b,0x256a,0x2518,0x250c,0x2588,0x2584,0x258c,0x2590,0x2580,
	0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,0x0448,0x0449,0x044a,0x044b,0x044c,0x044d,0x044e,0x044f,
	0x0401,0x0451,0x0404,0x0454,0x0407,0x0457,0x040e,0x045e,0x00b0,0x2219,0x00b7,0x221a,0x2116,0x00a4,0x25a0,0x00a0
};

static int utf8_put(char* dst, unsigned int cp) {
	if (cp < 0x80) { dst[0] = cp; return 1; }
	if (cp < 0x800) { dst[0] = 0xc0 | (cp >> 6); dst[1] = 0x80 | (cp & 0x3f); return 2; }
	if (cp < 0x10000) { dst[0] = 0xe0 | (cp >> 12); dst[1] = 0x80 | ((cp >> 6) & 0x3f); dst[2] = 0x80 | (cp & 0x3f); return 3; }
	dst[0] = 0xf0 | (cp >> 18); dst[1] = 0x80 | ((cp >> 12) & 0x3f); dst[2] = 0x80 | ((cp >> 6) & 0x3f); dst[3] = 0x80 | (cp & 0x3f);
	return 4;
}

// UTF-16 to UTF-8, dst at least 4 * len + 1
static void utf16_to8(const unsigned short* src, int len, char* dst) {
	int i;
	unsigned int cp;
	for (i = 0; i < len; i++) {
		cp = src[i];
		if ((cp >= 0xd800) && (cp < 0xdc00) && (i + 1 < len) && (src[i + 1] >= 0xdc00) && (src[i + 1] < 0xe000)) {
			cp = 0x10000 + ((cp - 0xd800) << 10) + (src[i + 1] - 0xdc00);
			i++;
		}
		dst += utf8_put(dst, cp);
	}
	*dst = 0;
}

// an 8.3 name as it reads, NT's lowercase flags taken (b3 base, b4 extension)
static void sname_decode(const unsigned char* e, char* dst) {
	int i, n, k;
	unsigned int c;
	for (n = 8; (n > 0) && (e[n - 1] == ' '); n--);
	for (i = 0; i < n; i++) {
		c = ((i == 0) && (e[0] == 0x05)) ? 0xe5 : e[i];
		if ((e[12] & 0x08) && (c >= 'A') && (c <= 'Z')) c += 0x20;
		dst += utf8_put(dst, (c < 0x80) ? c : cp866[c - 0x80]);
	}
	for (k = 11; (k > 8) && (e[k - 1] == ' '); k--);
	if (k > 8) {
		*dst++ = '.';
		for (i = 8; i < k; i++) {
			c = e[i];
			if ((e[12] & 0x10) && (c >= 'A') && (c <= 'Z')) c += 0x20;
			dst += utf8_put(dst, (c < 0x80) ? c : cp866[c - 0x80]);
		}
	}
	*dst = 0;
}

// ASCII letters alike in either case: what a FAT name is the same as
static int name_eq(const char* a, const char* b) {
	unsigned char x, y;
	for (;; a++, b++) {
		x = *a; y = *b;
		if ((x >= 'a') && (x <= 'z')) x -= 0x20;
		if ((y >= 'a') && (y <= 'z')) y -= 0x20;
		if (x != y) return 0;
		if (!x) return 1;
	}
}

enum {VFA_NONE = 0, VFA_PATCH, VFA_FULL};

typedef struct {
	unsigned int off;		// in the file
	unsigned char data[VF_SECSIZE];
} vfStale;

typedef struct {
	char* name;			// as the guest has it, UTF-8
	char* hname;			// what it is to be called in the folder
	unsigned char sname[11];
	unsigned char lfn;
	unsigned char isdir;
	unsigned char ok;		// its chain bears its size out
	unsigned char act;		// VFA_*
	unsigned char moved;		// its host name or folder changes
	unsigned char failed;
	unsigned char pin;		// in a pinned folder: the host side leaves it be
	unsigned char conflict;		// its host file was changed outside the emulator since
	unsigned int clu, size;
	unsigned short date, time;
	unsigned int chain, nch;	// in the pool
	int parent;			// entry index, -1 for the root
	int node;			// what it was at the last sync, -1 new
	int tmp;			// the temporary file its content waits in, -1 none
	char* phname;			// its host name before it was parked
	vfStale* stale;			// a patch's sectors the guest did not write
	int nstale;
} vfEnt;

typedef struct {
	vfEnt* ent;
	int n, cap;
	unsigned int* pool;
	unsigned int npool, cappool;
	int bad;			// an entry is unfinished
} vfScan;

static int scan_add(vfScan* sc) {
	if (sc->n >= sc->cap) {
		int cap = sc->cap ? sc->cap * 2 : 256;
		vfEnt* e = (vfEnt*)realloc(sc->ent, cap * sizeof(vfEnt));
		if (!e) return -1;
		sc->ent = e;
		sc->cap = cap;
	}
	memset(sc->ent + sc->n, 0x00, sizeof(vfEnt));
	sc->ent[sc->n].node = -1;
	sc->ent[sc->n].tmp = -1;
	return sc->n++;
}

static int pool_add(vfScan* sc, unsigned int v) {
	if (sc->npool >= sc->cappool) {
		unsigned int cap = sc->cappool ? sc->cappool * 2 : 4096;
		unsigned int* p = (unsigned int*)realloc(sc->pool, cap * sizeof(unsigned int));
		if (!p) return 0;
		sc->pool = p;
		sc->cappool = cap;
	}
	sc->pool[sc->npool++] = v;
	return 1;
}

// the chain from first into the pool; 0 if it breaks, loops or crosses another
static int scan_chain(vFat* vf, vfScan* sc, vfEnt* e) {
	vfWr* w = vf->wr;
	unsigned int c = e->clu;
	unsigned int v, n = 0;
	e->chain = sc->npool;
	e->nch = 0;
	if (!c) return 1;
	while (1) {
		if ((c < 2) || (c >= vf->clusters + 2)) return 0;
		if (w->mark[c] == w->stamp) return 0;		// crossed: two chains or a loop
		w->mark[c] = w->stamp;
		if (!pool_add(sc, c)) return 0;
		n++;
		v = w->fat[0][c] & 0x0fffffff;
		if (v >= 0x0ffffff8) break;
		if ((v < 2) || (v == 0x0ffffff7)) return 0;
		c = v;
	}
	e->nch = n;
	return 1;
}

// the entries of the directory entry d, appended; 0 when the tree is past every limit
static int scan_dir(vFat* vf, vfScan* sc, int d) {
	unsigned char sec[VF_SECSIZE];
	unsigned short lname[VF_MAXNAME + 14];
	static const int pos[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
	char name[VF_MAXNAME * 4 + 16];
	int lfnOk = 0, lfnNext = 0, lfnLen = 0;
	unsigned char lfnSum = 0;
	unsigned int k, s, i;
	for (k = 0; k < sc->ent[d].nch; k++) {
		unsigned int clu = sc->pool[sc->ent[d].chain + k];
		for (s = 0; s < vf->spc; s++) {
			vfw_read(vf, clu_lba(vf, clu, s), sec);
			for (i = 0; i < VF_SECSIZE; i += 32) {
				unsigned char* e = sec + i;
				if (e[0] == 0x00) return 1;		// the end of the directory
				if (e[0] == 0xe5) { lfnOk = 0; continue; }
				if (e[11] == 0x0f) {
					int ord = e[0] & 0x3f;
					int j;
					if (e[0] & 0x40) {
						lfnOk = (ord >= 1) && (ord <= 20);
						lfnSum = e[13];
						lfnLen = ord * 13;
						lfnNext = ord;
					} else if (!lfnOk || (ord != lfnNext) || (e[13] != lfnSum)) {
						lfnOk = 0;
					}
					if (!lfnOk) continue;
					for (j = 0; j < 13; j++)
						lname[(ord - 1) * 13 + j] = vf_rd16(e + pos[j]);
					lfnNext = ord - 1;
					continue;
				}
				if (e[11] & 0x08) { lfnOk = 0; continue; }	// the label
				if ((e[0] == '.') && ((e[1] == ' ') || ((e[1] == '.') && (e[2] == ' ')))) { lfnOk = 0; continue; }
				if (lfnOk && !lfnNext && (vf_sname_sum((const char*)e) == lfnSum)) {
					int len = 0;
					while ((len < lfnLen) && lname[len] && (lname[len] != 0xffff)) len++;
					utf16_to8(lname, len, name);
				} else {
					sname_decode(e, name);
				}
				int was = lfnOk && !lfnNext;
				lfnOk = 0;
				if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) continue;
				if (sc->n >= VFW_MAXENTS) return 0;
				int x = scan_add(sc);
				if (x < 0) return 0;
				vfEnt* n = sc->ent + x;
				n->name = vf_strdup(name);
				memcpy(n->sname, e, 11);
				n->lfn = was;
				n->isdir = (e[11] & 0x10) ? 1 : 0;
				n->clu = (vf_rd16(e + 20) << 16) | vf_rd16(e + 26);
				n->size = n->isdir ? 0 : vf_rd32(e + 28);
				n->time = vf_rd16(e + 22);
				n->date = vf_rd16(e + 24);
				n->parent = d;
			}
		}
	}
	return 1;
}

static void scan_free(vfScan* sc) {
	int i;
	for (i = 0; i < sc->n; i++) {
		free(sc->ent[i].name);
		free(sc->ent[i].hname);
		free(sc->ent[i].phname);
		free(sc->ent[i].stale);
	}
	free(sc->ent);
	free(sc->pool);
}

// the whole tree; 0 past the limits
static int scan_tree(vFat* vf, vfScan* sc) {
	unsigned int cbytes = vf->spc * VF_SECSIZE;
	int i, depth;
	int r = scan_add(sc);
	if (r < 0) return 0;
	sc->ent[r].isdir = 1;
	sc->ent[r].clu = 2;
	sc->ent[r].parent = -1;
	sc->ent[r].node = 0;
	// entries come out parent first, so a single pass over the growing list is breadth first
	for (i = 0; i < sc->n; i++) {
		vfEnt* e = sc->ent + i;
		e->ok = scan_chain(vf, sc, e);
		e = sc->ent + i;
		if (e->ok) {
			if (e->isdir) e->ok = (e->nch > 0);
			else e->ok = ((unsigned long long)e->nch * cbytes >= e->size) && (e->size || !e->clu || e->nch);
		}
		if (!e->ok) {
			sc->bad = 1;
			continue;
		}
		if (!e->isdir) continue;
		for (depth = 0, r = i; r > 0; r = sc->ent[r].parent) depth++;
		if (depth >= VFW_MAXDEPTH) return 0;
		if (!scan_dir(vf, sc, i)) return 0;
	}
	return 1;
}

// == the host side ==

// a guest name the host can take: what Windows refuses becomes '_', and its device
// names and our temporary names get a leading '_'
static char* host_name(const char* name) {
	static const char* dev[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
		"COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
	int len = strlen(name);
	char* s = (char*)malloc(len + 3);
	char base[8];
	int i, n, pre;
	if (!s) return NULL;
	for (i = 0; i < len; i++) {
		unsigned char c = name[i];
		s[i] = ((c < 0x20) || strchr("<>:\"/\\|?*", c)) ? '_' : c;
	}
	s[len] = 0;
	for (i = len - 1; (i >= 0) && ((s[i] == ' ') || (s[i] == '.')); i--) s[i] = '_';
	if (!len) {
		strcpy(s, "_");
		return s;
	}
	for (n = 0; (n < 7) && s[n] && (s[n] != '.'); n++)
		base[n] = ((s[n] >= 'a') && (s[n] <= 'z')) ? s[n] - 0x20 : s[n];
	base[n] = 0;
	pre = !strncmp(s, VF_TMPMARK, strlen(VF_TMPMARK));
	for (i = 0; !pre && (i < (int)(sizeof(dev) / sizeof(dev[0]))); i++)
		if (((s[n] == 0) || (s[n] == '.')) && !strcmp(base, dev[i])) pre = 1;
	if (pre) {
		memmove(s + 1, s, len + 1);
		s[0] = '_';
	}
	return s;
}

// a guest with no clock leaves the date empty (month or day 0): the host's own time
// stands then, which for a file written just now is now
static int fat_date_set(unsigned short date) {
	return ((date >> 5) & 15) && (date & 31);
}

static unsigned int fat_time(unsigned short date, unsigned short tim) {
	struct tm t;
	time_t r;
	memset(&t, 0x00, sizeof(t));
	t.tm_year = (date >> 9) + 80;
	t.tm_mon = ((date >> 5) & 15) - 1;
	t.tm_mday = date & 31;
	t.tm_hour = tim >> 11;
	t.tm_min = (tim >> 5) & 63;
	t.tm_sec = (tim & 31) * 2;
	t.tm_isdst = -1;
	r = mktime(&t);
	return (r == (time_t)-1) ? 0 : (unsigned int)r;
}

static int host_kind(vFat* vf, const char* path) {
	return vf->host.kind(vf->host.ctx, path);
}

// a free name in dir for want: want, then "base (2).ext" and on
static int host_free_name(vFat* vf, const char* dir, const char* want, char* path, int max, char** chosen) {
	char name[1200];
	const char* dot = strrchr(want, '.');
	int blen = (dot && (dot != want)) ? (int)(dot - want) : (int)strlen(want);
	int n;
	for (n = 1; n < 1000; n++) {
		if (n == 1) snprintf(name, sizeof(name), "%s", want);
		else snprintf(name, sizeof(name), "%.*s (%i)%s", blen, want, n, want + blen);
		if (snprintf(path, max, "%s/%s", dir, name) >= max) return 0;
		if (!host_kind(vf, path)) {
			*chosen = vf_strdup(name);
			return *chosen != NULL;
		}
	}
	return 0;
}

static int node_new(vFat* vf, const char* name, const char* hname, int isdir) {
	int idx;
	if (vf->nodes >= vf->cap) {
		int cap = vf->cap ? vf->cap * 2 : 64;
		vfNode* nd = (vfNode*)realloc(vf->node, cap * sizeof(vfNode));
		if (!nd) return -1;
		vf->node = nd;
		vf->cap = cap;
	}
	idx = vf->nodes++;
	memset(vf->node + idx, 0x00, sizeof(vfNode));
	vf->node[idx].isdir = isdir ? 1 : 0;
	vf->node[idx].name = vf_strdup(name);
	vf->node[idx].hname = vf_strdup(hname);
	vf->node[idx].parent = -1;
	vf->node[idx].child = vf->node[idx].last = vf->node[idx].next = -1;
	return idx;
}

static void node_unlink(vFat* vf, int idx) {
	int p = vf->node[idx].parent;
	int* q;
	if (p < 0) return;
	for (q = &vf->node[p].child; *q >= 0; q = &vf->node[*q].next) {
		if (*q == idx) {
			*q = vf->node[idx].next;
			break;
		}
	}
	vf->node[p].last = -1;
	for (q = &vf->node[p].child; *q >= 0; q = &vf->node[*q].next)
		vf->node[p].last = *q;
	vf->node[idx].next = -1;
	vf->node[idx].parent = -1;
}

static void node_append(vFat* vf, int p, int idx) {
	vf->node[idx].parent = p;
	vf->node[idx].next = -1;
	if (vf->node[p].last < 0) vf->node[p].child = idx;
	else vf->node[vf->node[p].last].next = idx;
	vf->node[p].last = idx;
}

// move a node within the folder; its children go with it
static int node_move(vFat* vf, int idx, int parent, const char* hname) {
	char from[2048], dir[2048], to[2048];
	if (!vf_path(vf, idx, from, sizeof(from)) || !vf_path(vf, parent, dir, sizeof(dir))) return 0;
	if (snprintf(to, sizeof(to), "%s/%s", dir, hname) >= (int)sizeof(to)) return 0;
	if (host_kind(vf, to) || vf->host.rename(vf->host.ctx, from, to)) {
		xlog(XLG_DISK, XLL_WARN, "folder: can't move %s to %s", from, to);
		return 0;
	}
	node_unlink(vf, idx);
	node_append(vf, parent, idx);
	free(vf->node[idx].hname);
	vf->node[idx].hname = vf_strdup(hname);
	return 1;
}

// the bytes of s that fit in max, not cutting a character
static int utf8_cut(const char* s, int max) {
	int n = strlen(s);
	if (n <= max) return n;
	for (n = max; (n > 0) && ((s[n] & 0xc0) == 0x80); n--);
	return n;
}

// our temporary name for a file's content ('f') or a parked entry ('m'): the host name it
// stands for comes after it, so an unfinished sync leaves something to restore it by
static int tmp_path(vFat* vf, int n, char kind, const char* hname, char* buf, int max) {
	return snprintf(buf, max, "%s/" VF_TMPMARK "%c%i~%.*s", vf->root, kind, n, utf8_cut(hname, VFW_TMPNAME), hname) < max;
}

// "base (recovered).ext" of a host name
static void recovered_name(const char* hn, char* out, int max) {
	const char* dot = strrchr(hn, '.');
	int blen = (dot && (dot != hn)) ? (int)(dot - hn) : (int)strlen(hn);
	snprintf(out, max, "%.*s (recovered)%s", blen, hn, hn + blen);
}

char* vfat_tmp_restore(vFat* vf, const char* name) {
	size_t ml = strlen(VF_TMPMARK);
	char from[2048], to[2048], want[1100];
	char* chosen = NULL;
	const char* p;
	char kind;
	if (!vf || !vf->root || !vf->host.rename || !vf->host.kind || !name || strncmp(name, VF_TMPMARK, ml)) return NULL;
	kind = name[ml];
	if ((kind != 'm') && (kind != 'f')) return NULL;
	for (p = name + ml + 1; (*p >= '0') && (*p <= '9'); p++);
	if ((*p == '~') && p[1]) {
		p++;
		if (kind == 'f') recovered_name(p, want, sizeof(want));
		else snprintf(want, sizeof(want), "%s", p);
	} else {
		snprintf(want, sizeof(want), "recovered");
	}
	if (snprintf(from, sizeof(from), "%s/%s", vf->root, name) >= (int)sizeof(from)) return NULL;
	if (!host_free_name(vf, vf->root, want, to, sizeof(to), &chosen)) return NULL;
	if (vf->host.rename(vf->host.ctx, from, to)) {
		xlog(XLG_DISK, XLL_WARN, "folder %s: can't give %s a visible name", vf->root, name);
		free(chosen);
		return NULL;
	}
	xlog(XLG_DISK, XLL_WARN, "folder %s: %s, left by a sync that did not finish, is %s", vf->root, name, chosen);
	return chosen;
}

static int sec_in_ovl(vFat* vf, unsigned int clu, unsigned int s) {
	return ovl_find(vf->wr, clu_lba(vf, clu, s)) >= 0;
}

static int node_pinned(vFat* vf, int idx) {
	for (; idx > 0; idx = vf->node[idx].parent)
		if (vf->node[idx].pinned) return 1;
	return 0;
}

static int depth_of(vFat* vf, int idx) {
	int d = 0;
	for (; idx > 0; idx = vf->node[idx].parent) d++;
	return d;
}

// a file's content as the guest has it, into a temporary file of the folder's root
static int write_full(vFat* vf, vfScan* sc, vfEnt* e) {
	unsigned char buf[VF_SECSIZE];
	char path[2048];
	unsigned long long cbytes = (unsigned long long)vf->spc * VF_SECSIZE;
	unsigned long long off;		// a file just under 4G would wrap a 32-bit one
	unsigned int n;
	FILE* f;
	e->tmp = vf->wr->tmpn++;
	if (!tmp_path(vf, e->tmp, 'f', e->hname, path, sizeof(path))) {
		e->tmp = -1;
		return 0;
	}
	f = vf_open(vf, path, "wb");
	if (!f) return 0;
	for (off = 0; off < e->size; off += VF_SECSIZE) {
		unsigned int clu = sc->pool[e->chain + off / cbytes];
		vfw_read(vf, clu_lba(vf, clu, (off % cbytes) / VF_SECSIZE), buf);
		n = (e->size - off > VF_SECSIZE) ? VF_SECSIZE : (unsigned int)(e->size - off);
		if (fwrite(buf, 1, n, f) != n) {
			fclose(f);
			vf->host.remove(vf->host.ctx, path, 0);
			e->tmp = -1;
			return 0;
		}
	}
	if (fclose(f)) {
		vf->host.remove(vf->host.ctx, path, 0);
		e->tmp = -1;
		return 0;
	}
	return 1;
}

// the sectors of a patch the guest did not write, read while the old files stand
static int keep_stale(vFat* vf, vfScan* sc, vfEnt* e, unsigned int oldClusters) {
	unsigned char buf[VF_SECSIZE];
	unsigned long long cbytes = (unsigned long long)vf->spc * VF_SECSIZE;
	unsigned long long off;
	vfStale* st;
	int k;
	for (off = oldClusters * cbytes; off < e->size; off += VF_SECSIZE) {
		unsigned int clu = sc->pool[e->chain + off / cbytes];
		unsigned int s = (off % cbytes) / VF_SECSIZE;
		if (sec_in_ovl(vf, clu, s)) continue;
		vfw_read(vf, clu_lba(vf, clu, s), buf);
		for (k = 0; (k < VF_SECSIZE) && !buf[k]; k++);
		if (k == VF_SECSIZE) continue;			// the host's truncate gives zeros too
		st = (vfStale*)realloc(e->stale, (e->nstale + 1) * sizeof(vfStale));
		if (!st) return 0;
		e->stale = st;
		st[e->nstale].off = (unsigned int)off;
		memcpy(st[e->nstale].data, buf, VF_SECSIZE);
		e->nstale++;
	}
	return 1;
}

static int put_sector(FILE* f, unsigned int off, const unsigned char* data, unsigned int size) {
	unsigned int n = size - off;
	if (n > VF_SECSIZE) n = VF_SECSIZE;
	return !x_fseek(f, off, SEEK_SET) && (fwrite(data, 1, n, f) == n);
}

// a file whose old chain is a prefix of the new one or the other way round: the size,
// then what the guest wrote, in place
static int apply_patch(vFat* vf, vfScan* sc, vfEnt* e, int idx) {
	char path[2048];
	unsigned long long cbytes = (unsigned long long)vf->spc * VF_SECSIZE;
	unsigned long long off;
	int i, o;
	FILE* f;
	if (!vf_path(vf, idx, path, sizeof(path))) return 0;
	if ((e->size != vf->node[idx].size) && vf->host.truncate(vf->host.ctx, path, e->size)) return 0;
	f = vf_open(vf, path, "r+b");
	if (!f) return 0;
	for (off = 0; off < e->size; off += VF_SECSIZE) {
		unsigned int clu = sc->pool[e->chain + off / cbytes];
		o = ovl_find(vf->wr, clu_lba(vf, clu, (off % cbytes) / VF_SECSIZE));
		if ((o >= 0) && !put_sector(f, (unsigned int)off, vf->wr->sec[o].data, e->size)) {
			fclose(f);
			return 0;
		}
	}
	for (i = 0; i < e->nstale; i++) {
		if (!put_sector(f, e->stale[i].off, e->stale[i].data, e->size)) {
			fclose(f);
			return 0;
		}
	}
	return fclose(f) == 0;
}

// how the new chain stands against the node's old one: 2 the same, 1 one a prefix of
// the other, 0 neither
static int chain_rel(vFat* vf, vfScan* sc, vfEnt* e, int idx) {
	vfWr* w = vf->wr;
	unsigned int k, m = (e->nch < vf->node[idx].nclust) ? e->nch : vf->node[idx].nclust;
	for (k = 0; k < m; k++) {
		unsigned int c = sc->pool[e->chain + k];
		if ((w->own[c] != idx) || (w->oidx[c] != k)) return 0;
	}
	return (e->nch == vf->node[idx].nclust) ? 2 : 1;
}

// per node: a sector the guest wrote over its file that the host file does not hold yet,
// the tail past its end aside. One pass over the kept sectors, whatever the files' sizes.
static unsigned char* nodes_dirty(vFat* vf) {
	vfWr* w = vf->wr;
	unsigned char buf[VF_SECSIZE];
	unsigned long long cbytes = (unsigned long long)vf->spc * VF_SECSIZE;
	unsigned char* d = (unsigned char*)calloc(vf->nodes + 1, 1);
	int i, o;
	if (!d) return NULL;
	for (i = 0; i < w->capsec; i++) {
		unsigned int lba = w->sec[i].lba;
		unsigned int rel, c, s, len;
		unsigned long long off;
		if ((lba == 0xffffffff) || (lba < vf->partStart)) continue;
		rel = lba - vf->partStart;
		if (rel < vf->dataStart) continue;
		rel -= vf->dataStart;
		c = rel / vf->spc + 2;
		s = rel % vf->spc;
		if (c >= vf->clusters + 2) continue;
		o = w->own[c];
		if ((o < 0) || d[o] || vf->node[o].isdir || vf->node[o].dead) continue;
		off = w->oidx[c] * cbytes + s * VF_SECSIZE;
		if (off >= vf->node[o].size) continue;
		len = (vf->node[o].size - off > VF_SECSIZE) ? VF_SECSIZE : (unsigned int)(vf->node[o].size - off);
		vfw_gen_data(vf, c, s, buf);
		if (memcmp(buf, w->sec[i].data, len)) d[o] = 1;
	}
	return d;
}

// the host file against what the scan found or the last sync left: 0 the same, 1 changed
// outside the emulator since, 2 gone
static int host_changed(vFat* vf, int idx) {
	char path[2048];
	long long size, mtime;
	if (!vf->host.stat || !vf_path(vf, idx, path, sizeof(path))) return 0;
	if (vf->host.stat(vf->host.ctx, path, &size, &mtime)) return 2;
	return ((size != vf->node[idx].hsize) || ((unsigned int)mtime != vf->node[idx].hmtime)) ? 1 : 0;
}

// the host file as the sync left it, so the next one does not take its own work for a
// change from outside
static void host_note(vFat* vf, int idx) {
	char path[2048];
	long long size, mtime;
	if (!vf->host.stat || !vf_path(vf, idx, path, sizeof(path))) return;
	if (vf->host.stat(vf->host.ctx, path, &size, &mtime)) return;
	vf->node[idx].hmtime = (unsigned int)mtime;
	vf->node[idx].hsize = (unsigned int)size;
}

// a host operation the sync could not do; the first few names are kept to be told
static void vfw_fail(vFat* vf, const char* name) {
	vfWr* w = vf->wr;
	size_t len = strlen(w->fnames);
	w->errors++;
	if (!name || strstr(w->fnames, "...")) return;
	if (len + strlen(name) + 8 < sizeof(w->fnames))
		snprintf(w->fnames + len, sizeof(w->fnames) - len, "%s%s", len ? ", " : "", name);
	else
		snprintf(w->fnames + len, sizeof(w->fnames) - len, ", ...");
}

// The guest's content of a file that could not be put in place is never thrown away: it
// goes to the folder's top level as "name (recovered)". A file new to the folder is
// followed there from then on; a known one's copy is replaced the next time, not piled up.
static void tmp_recover(vFat* vf, vfEnt* e) {
	char tmp[2048], path[2048], want[1100];
	char* chosen = NULL;
	vfNode* nd = (e->node >= 0) ? vf->node + e->node : NULL;
	int ok = 0;
	if (!tmp_path(vf, e->tmp, 'f', e->hname, tmp, sizeof(tmp))) return;
	recovered_name(e->hname, want, sizeof(want));
	if (nd && nd->rname && (snprintf(path, sizeof(path), "%s/%s", vf->root, nd->rname) < (int)sizeof(path))
			&& (host_kind(vf, path) == 1) && !vf->host.replace(vf->host.ctx, tmp, path)) {
		ok = 1;
	} else if (host_free_name(vf, vf->root, want, path, sizeof(path), &chosen) && !vf->host.rename(vf->host.ctx, tmp, path)) {
		ok = 1;
		if (nd) {
			free(nd->rname);
			nd->rname = chosen;
		} else if (!e->isdir) {
			int idx = node_new(vf, chosen, chosen, 0);
			if (idx >= 0) {
				nd = vf->node + idx;
				node_append(vf, 0, idx);
				memcpy(nd->sname, e->sname, 11);
				nd->lfn = e->lfn;
				nd->size = e->size;
				nd->clust = e->clu;
				nd->nclust = e->nch;
				nd->date = e->date;
				nd->time = e->time;
				host_note(vf, idx);
				e->node = idx;
				e->failed = 0;		// done: the folder follows it where it is
			}
			free(chosen);
		} else {
			free(chosen);
		}
		chosen = NULL;
	}
	free(chosen);
	vfw_fail(vf, e->name);
	if (ok) xlog(XLG_DISK, XLL_WARN, "folder %s: %s could not be put in place, kept as %s", vf->root, e->name, path);
	else xlog(XLG_DISK, XLL_ERROR, "folder %s: %s could not be put in place, its content stays in %s", vf->root, e->name, tmp);
	e->tmp = -1;		// either way not ours to remove: a hidden one is made visible at the next mount
}

// an entry parked in (b1) that did not get to its place takes a visible name in the top
// level: under our temporary name no scan would see it
static void park_restore(vFat* vf, vfEnt* e) {
	char path[2048];
	char* chosen = NULL;
	vfNode* nd = vf->node + e->node;
	if (nd->dead || (nd->parent != 0) || !nd->hname || strncmp(nd->hname, VF_TMPMARK, strlen(VF_TMPMARK))) return;
	vfw_fail(vf, e->name);
	if (host_free_name(vf, vf->root, e->phname, path, sizeof(path), &chosen) && node_move(vf, e->node, 0, chosen))
		xlog(XLG_DISK, XLL_WARN, "folder %s: %s could not be moved, left as %s", vf->root, e->name, path);
	else
		xlog(XLG_DISK, XLL_ERROR, "folder %s: %s could not be moved, left as %s", vf->root, e->name, vf->node[e->node].hname);
	free(chosen);
}

typedef struct {
	unsigned int lba;
	unsigned char data[VF_SECSIZE];
} vfSnap;

// an entry done this sync: its node takes its chain
#define ENT_DONE(e)	((e)->ok && !(e)->failed && ((e)->node >= 0))

// The files going. One whose clusters another entry took (claim) waits for that entry to be
// in place (late): until then it is the only copy of what that entry was made from. One
// changed outside the emulator since is left on the host and only forgotten.
static void del_files(vFat* vf, vfScan* sc, unsigned char* gone, const int* claim, int oldNodes, int late, int* failed) {
	char path[2048];
	int i;
	for (i = oldNodes - 1; i > 0; i--) {
		if (!gone[i] || vf->node[i].isdir || vf->node[i].dead) continue;
		if (late != (claim[i] >= 0)) continue;
		if (!vf_path(vf, i, path, sizeof(path))) {
			gone[i] = 0;
			(*failed)++;
			continue;
		}
		if (late && !ENT_DONE(sc->ent + claim[i])) {
			xlog(XLG_DISK, XLL_WARN, "folder: kept %s, what the machine wrote over it could not be put in place", path);
			gone[i] = 0;
			(*failed)++;
			continue;
		}
		if (host_changed(vf, i) == 1) {
			xlog(XLG_DISK, XLL_WARN, "folder: %s was changed outside the emulator, not deleted", path);
			vfw_fail(vf, vf->node[i].name);
		} else if (vf->host.remove(vf->host.ctx, path, 0) && host_kind(vf, path)) {
			xlog(XLG_DISK, XLL_WARN, "folder: can't remove %s", path);
			vfw_fail(vf, vf->node[i].name);
			gone[i] = 0;
			(*failed)++;
			continue;
		} else {
			xlog(XLG_DISK, XLL_INFO, "folder: removed %s", path);
		}
		node_unlink(vf, i);
		vf->node[i].dead = 1;
	}
}

// the folders going, from the bottom, each only once empty; one still holding an entry is
// passed over until a later call
static void del_dirs(vFat* vf, unsigned char* gone, int oldNodes, int* failed) {
	char path[2048];
	int i, again = 1;
	while (again) {
		again = 0;
		for (i = oldNodes - 1; i > 0; i--) {
			if (!gone[i] || !vf->node[i].isdir || vf->node[i].dead || (vf->node[i].child >= 0)) continue;
			if (!vf_path(vf, i, path, sizeof(path))) continue;
			if (!vf->host.empty(vf->host.ctx, path) || vf->host.remove(vf->host.ctx, path, 1)) {
				xlog(XLG_DISK, XLL_WARN, "folder: left %s, not empty or not removable", path);
				vfw_fail(vf, vf->node[i].name);
				gone[i] = 0;
				(*failed)++;
				continue;
			}
			xlog(XLG_DISK, XLL_INFO, "folder: removed %s", path);
			node_unlink(vf, i);
			vf->node[i].dead = 1;
			again = 1;
		}
	}
}

int vfat_sync(vFat* vf, int force) {
	vfWr* w;
	vfScan sc;
	vfSnap* snap = NULL;
	int nsnap = 0;
	int* match = NULL;		// per old node: the entry it became, -1 none
	unsigned char* gone = NULL;	// per old node: deleted this sync
	unsigned char* rechain = NULL;	// per node: takes a new chain
	unsigned char* held = NULL;	// per old node: a cluster of its chain is still allocated and nobody else's
	unsigned char* dirty = NULL;	// per old node: the guest wrote over its content
	int* claim = NULL;		// per old node going: an entry whose chain took a cluster of it, -1 none
	int* ord = NULL;
	unsigned long long cbytes;
	unsigned int c, k, n, s, wgen;
	int i, j, o, oldNodes, live, dels, no, failed = 0, res = 0;
	char path[2048], dir[2048], tmp[2048], was[2048];
	if (!vf || !(w = vf->wr) || w->detached) return 0;
	memset(&sc, 0x00, sizeof(sc));
	if (!vf->root || !vf->host.open || !vf->host.rename || !vf->host.replace || !vf->host.mkdir
			|| !vf->host.remove || !vf->host.truncate || !vf->host.kind || !vf->host.empty) {
		w->sgen = w->wgen;		// nowhere to carry it: the volume is the memory's
		return 0;
	}
	wgen = w->wgen;
	cbytes = (unsigned long long)vf->spc * VF_SECSIZE;
	n = vf->clusters + 2;
	if (++w->stamp == 0) {
		memset(w->mark, 0x00, n * sizeof(unsigned int));
		w->stamp = 1;
	}
	if (!scan_tree(vf, &sc)) {
		vfw_detach(vf, "it grew past what a FAT disk holds", 0);
		goto out;
	}
	if (sc.bad && !force) goto out;		// mid-operation: later
	w->errors = 0;
	w->fnames[0] = 0;
	oldNodes = vf->nodes;
	match = (int*)malloc(oldNodes * sizeof(int));
	gone = (unsigned char*)calloc(oldNodes, 1);
	claim = (int*)malloc(oldNodes * sizeof(int));
	ord = (int*)malloc(sc.n * sizeof(int));
	dirty = nodes_dirty(vf);
	if (!match || !gone || !claim || !ord || !dirty) goto out;
	for (i = 0; i < oldNodes; i++) {
		match[i] = -1;
		claim[i] = -1;
	}
	match[0] = 0;
	// == match: the first cluster and something more, wherever the entry is now - a folder
	// made in the same spell included - then the name in the same folder. An unfinished
	// entry is matched too, so its file is not taken for deleted.
	for (i = 1; i < sc.n; i++) {
		vfEnt* e = sc.ent + i;
		if (!e->clu || (e->clu >= n)) continue;
		o = w->own[e->clu];
		if ((o <= 0) || (o >= oldNodes) || vf->node[o].dead || (match[o] >= 0) || (vf->node[o].clust != e->clu)) continue;
		if (vf->node[o].isdir != e->isdir) continue;
		// another name is a rename only when nothing else changed: a file deleted, and one of
		// its size saved over its clusters by a guest with no clock, is not one
		if (!e->isdir && !name_eq(vf->node[o].name, e->name) && ((vf->node[o].size != e->size)
				|| (vf->node[o].date != e->date) || (vf->node[o].time != e->time) || dirty[o])) continue;
		match[o] = i;
		e->node = o;
	}
	for (i = 1; i < sc.n; i++) {
		vfEnt* e = sc.ent + i;
		int pn = sc.ent[e->parent].node;
		if ((e->node >= 0) || (pn < 0)) continue;
		for (j = vf->node[pn].child; j >= 0; j = vf->node[j].next) {
			if ((j < oldNodes) && (match[j] < 0) && !vf->node[j].dead && (vf->node[j].isdir == e->isdir) && name_eq(vf->node[j].name, e->name)) {
				match[j] = i;
				e->node = j;
				break;
			}
		}
	}
	for (i = 1; i < sc.n; i++) {
		vfEnt* e = sc.ent + i;
		// a pinned folder and what is in it: what the guest does there stays in the volume
		e->pin = sc.ent[e->parent].pin || ((e->node >= 0) && node_pinned(vf, e->node));
		if (!e->ok || e->pin) e->failed = 1;	// left as it is
		// new there, or a file changed: it lives only as long as the volume
		if (e->pin && e->ok && !w->pinwrote) {
			vfNode* nd = (e->node >= 0) ? vf->node + e->node : NULL;
			if (!nd || (!e->isdir && ((e->size != nd->size) || (chain_rel(vf, &sc, e, e->node) != 2) || dirty[e->node])))
				w->pinwrote = 1;
		}
	}
	// == what goes: an old child of a folder read back that nothing became, and what a
	// going folder held; a file only once its clusters are free or someone else's
	live = 0;
	for (i = 1; i < oldNodes; i++) if (!vf->node[i].dead) live++;
	for (i = 0; i < sc.n; i++) {
		vfEnt* e = sc.ent + i;
		if (!e->isdir || !e->ok || (e->node < 0)) continue;
		for (j = vf->node[e->node].child; j >= 0; j = vf->node[j].next)
			if ((j < oldNodes) && (match[j] < 0) && !vf->node[j].dead && !node_pinned(vf, j)) gone[j] = 1;
	}
	for (i = 1; i < oldNodes; i++) {
		if (vf->node[i].dead || (match[i] >= 0) || gone[i]) continue;
		if (node_pinned(vf, i)) {
			if (!w->pinwrote) w->pinwrote = 1;	// deleted there: back on the next mount
			continue;
		}
		for (j = vf->node[i].parent; j > 0; j = vf->node[j].parent) {
			if (gone[j]) { gone[i] = 1; break; }
			if (match[j] >= 0) break;
		}
	}
	// what goes keeps its file while any of its chain stands: a move half done, or a
	// delete freeing a long chain one FAT sector at a time
	held = (unsigned char*)calloc(oldNodes, 1);
	if (!held) goto out;
	for (c = 2; c < n; c++) {
		o = w->own[c];
		if ((o > 0) && (o < oldNodes) && gone[o] && (w->fat[0][c] & 0x0fffffff) && (w->mark[c] != w->stamp)) held[o] = 1;
	}
	dels = 0;
	for (i = 1; i < oldNodes; i++) {
		if (!gone[i]) continue;
		if (held[i]) {
			gone[i] = 0;
			failed++;
			continue;
		}
		dels++;
	}
	if ((dels > VFW_MASSDEL) && ((long long)dels * 10 > (long long)live * 9)) {
		vfw_detach(vf, "the machine deleted most of it at once", 0);
		goto out;
	}
	for (i = 1; i < sc.n; i++) {
		vfEnt* e = sc.ent + i;
		if (!e->ok || e->isdir || e->pin) continue;	// a pinned one stays in memory, whatever it took
		for (k = 0; k < e->nch; k++) {
			o = w->own[sc.pool[e->chain + k]];
			if ((o > 0) && (o < oldNodes) && (o != e->node) && gone[o] && (claim[o] < 0)) claim[o] = i;
		}
	}
	// == host names, and what each file needs
	for (i = 1; i < sc.n; i++) {
		vfEnt* e = sc.ent + i;
		vfEnt* p = sc.ent + e->parent;
		if (e->failed) continue;
		if (e->node >= 0) {
			vfNode* nd = vf->node + e->node;
			e->hname = !strcmp(nd->name, e->name) ? vf_strdup(nd->hname) : host_name(e->name);
			e->moved = !e->hname || (p->node != nd->parent) || strcmp(e->hname, nd->hname);
			if (!e->isdir) {
				int rel = chain_rel(vf, &sc, e, e->node);
				if ((rel == 2) && (e->size == nd->size) && !dirty[e->node]) e->act = VFA_NONE;
				else if (rel) e->act = VFA_PATCH;
				else e->act = VFA_FULL;
			}
		} else {
			e->hname = host_name(e->name);
			e->moved = 1;
			if (!e->isdir) e->act = VFA_FULL;
		}
		if (!e->hname) e->failed = 1;
	}
	// == what the guest sees of every cluster that changes hands, the root's included,
	// and of the tail past a changed file's end in its last cluster, which the host does not
	// keep: none of it may move
	for (i = 0; i < sc.n; i++) {
		vfEnt* e = sc.ent + i;
		if (!e->ok || e->failed) continue;
		unsigned long long fl = (unsigned long long)e->size / VF_SECSIZE * VF_SECSIZE;	// past this the host has no say
		for (k = 0; k < e->nch; k++) {
			int moved;
			c = sc.pool[e->chain + k];
			moved = (e->node < 0) || (w->own[c] != e->node) || (w->oidx[c] != k);
			if (!moved && (e->isdir || (e->act == VFA_NONE) || ((k + 1) * cbytes <= fl))) continue;
			for (s = 0; s < vf->spc; s++) {
				vfSnap* sn;
				if (!moved && (k * cbytes + s * VF_SECSIZE < fl)) continue;	// the host keeps it
				if (sec_in_ovl(vf, c, s)) continue;
				sn = (vfSnap*)realloc(snap, (nsnap + 1) * sizeof(vfSnap));
				if (!sn) goto out;
				snap = sn;
				snap[nsnap].lba = clu_lba(vf, c, s);
				vfw_read(vf, snap[nsnap].lba, snap[nsnap].data);
				nsnap++;
			}
		}
	}
	// == (a) everything the host files are to hold, read while the old ones stand. A file
	// changed outside the emulator since stays: the guest's version goes beside it.
	for (i = 1; i < sc.n; i++) {
		vfEnt* e = sc.ent + i;
		if (e->failed || e->isdir) continue;
		if ((e->node >= 0) && (e->act != VFA_NONE) && host_changed(vf, e->node)) {
			e->conflict = 1;
			e->act = VFA_FULL;
		}
		if (e->act == VFA_FULL) {
			if (!write_full(vf, &sc, e)) {
				xlog(XLG_DISK, XLL_WARN, "folder %s: can't write the content of %s", vf->root, e->name);
				e->failed = 1;
				failed++;
				vfw_fail(vf, e->name);
			}
		} else if (e->act == VFA_PATCH) {
			if (!keep_stale(vf, &sc, e, vf->node[e->node].nclust)) {
				e->failed = 1;
				failed++;
			}
		}
	}
	vf_close_cached(vf);
	// == (b1) whatever moves is parked in the root, the deepest first, under a name that
	// still says what it was
	no = 0;
	for (i = 1; i < sc.n; i++)
		if (!sc.ent[i].failed && (sc.ent[i].node >= 0) && sc.ent[i].moved) ord[no++] = i;
	for (i = 0; i < no; i++) {
		for (j = i + 1; j < no; j++) {
			if (depth_of(vf, sc.ent[ord[j]].node) > depth_of(vf, sc.ent[ord[i]].node)) {
				int x = ord[i];
				ord[i] = ord[j];
				ord[j] = x;
			}
		}
	}
	for (i = 0; i < no; i++) {
		vfEnt* e = sc.ent + ord[i];
		const char* hn = vf->node[e->node].hname;
		char tn[300];
		snprintf(tn, sizeof(tn), VF_TMPMARK "m%i~%.*s", w->tmpn++, utf8_cut(hn, VFW_TMPNAME), hn);
		e->phname = vf_strdup(hn);
		if (!e->phname || !node_move(vf, e->node, 0, tn)) {
			e->failed = 1;
			failed++;
			vfw_fail(vf, e->name);
		}
	}
	// == (d) deletions: the files, then the folders from the bottom, each only when empty
	del_files(vf, &sc, gone, claim, oldNodes, 0, &failed);
	del_dirs(vf, gone, oldNodes, &failed);
	// == (b2) everything into place, parents first
	for (i = 1; i < sc.n; i++) {
		vfEnt* e = sc.ent + i;
		vfEnt* p = sc.ent + e->parent;
		char* chosen = NULL;
		vfNode* nd;
		int touched = 0;
		if (e->failed) continue;
		if ((p->node < 0) || p->failed || !vf_path(vf, p->node, dir, sizeof(dir))) {
			e->failed = 1;
			failed++;
			continue;
		}
		if (e->moved) {
			if (!host_free_name(vf, dir, e->hname, path, sizeof(path), &chosen)) {
				e->failed = 1;
				failed++;
				vfw_fail(vf, e->name);
				continue;
			}
			if (e->node >= 0) {
				if (!node_move(vf, e->node, p->node, chosen)) {
					e->failed = 1;
				} else if (strcmp(chosen, e->hname) || strcmp(vf->node[e->node].name, e->name)) {
					xlog(XLG_DISK, XLL_INFO, "folder: renamed to %s", path);
				}
			} else if (e->isdir) {
				if (vf->host.mkdir(vf->host.ctx, path)) {
					xlog(XLG_DISK, XLL_WARN, "folder: can't make %s", path);
					e->failed = 1;
				} else {
					e->node = node_new(vf, e->name, chosen, 1);
					if (e->node >= 0) node_append(vf, p->node, e->node);
					xlog(XLG_DISK, XLL_INFO, "folder: made %s", path);
				}
			} else {
				if ((e->tmp < 0) || !tmp_path(vf, e->tmp, 'f', e->hname, tmp, sizeof(tmp)) || vf->host.rename(vf->host.ctx, tmp, path)) {
					xlog(XLG_DISK, XLL_WARN, "folder: can't put %s in place", path);
					e->failed = 1;
				} else {
					e->tmp = -1;
					e->node = node_new(vf, e->name, chosen, 0);
					if (e->node >= 0) node_append(vf, p->node, e->node);
					touched = 1;
					xlog(XLG_DISK, XLL_INFO, "folder: wrote %s", path);
				}
			}
			free(chosen);
			if (e->failed || (e->node < 0)) {
				e->failed = 1;
				failed++;
				vfw_fail(vf, e->name);
				continue;
			}
		}
		if (!e->isdir && (e->act == VFA_FULL) && (e->tmp >= 0)) {
			// a known file rewritten whole: the new content replaces it - or, when the host file
			// was changed outside the emulator since, goes beside it and is followed there
			if (!vf_path(vf, e->node, was, sizeof(was)) || !tmp_path(vf, e->tmp, 'f', e->hname, tmp, sizeof(tmp))) {
				e->failed = 1;
				failed++;
				vfw_fail(vf, e->name);
				continue;
			}
			if (e->conflict) {
				vfNode* cn = vf->node + e->node;
				chosen = NULL;
				if (!vf_path(vf, cn->parent, dir, sizeof(dir)) || !host_free_name(vf, dir, cn->hname, path, sizeof(path), &chosen)
						|| vf->host.rename(vf->host.ctx, tmp, path)) {
					free(chosen);
					xlog(XLG_DISK, XLL_WARN, "folder: can't write the machine's version of %s", was);
					e->failed = 1;
					failed++;
					vfw_fail(vf, e->name);
					continue;
				}
				if (strcmp(path, was)) {
					xlog(XLG_DISK, XLL_WARN, "folder: %s was changed outside the emulator, the machine's version is %s", was, path);
					vfw_fail(vf, e->name);
				}
				free(cn->hname);
				cn->hname = chosen;
			} else if (vf->host.replace(vf->host.ctx, tmp, was)) {
				xlog(XLG_DISK, XLL_WARN, "folder: can't replace %s", was);
				e->failed = 1;
				failed++;
				vfw_fail(vf, e->name);
				continue;
			} else {
				xlog(XLG_DISK, XLL_INFO, "folder: rewrote %s", was);
			}
			e->tmp = -1;
			touched = 1;
		} else if (!e->isdir && (e->act == VFA_PATCH)) {
			if (!apply_patch(vf, &sc, e, e->node)) {
				xlog(XLG_DISK, XLL_WARN, "folder: can't update %s", e->name);
				host_note(vf, e->node);		// what it did get is ours, not a change from outside
				e->failed = 1;
				failed++;
				vfw_fail(vf, e->name);
				continue;
			}
			touched = 1;
			xlog(XLG_DISK, XLL_DEBUG, "folder: updated %s", e->name);
		}
		nd = vf->node + e->node;
		if (strcmp(nd->name, e->name)) {
			free(nd->name);
			nd->name = vf_strdup(e->name);
		}
		memcpy(nd->sname, e->sname, 11);
		nd->lfn = e->lfn;
		if (!e->isdir && vf->host.settime && ((nd->date != e->date) || (nd->time != e->time) || (e->act != VFA_NONE))
				&& fat_date_set(e->date) && vf_path(vf, e->node, path, sizeof(path))) {
			vf->host.settime(vf->host.ctx, path, fat_time(e->date, e->time));
			touched = 1;
		}
		nd->size = e->size;
		nd->clust = e->clu;
		nd->nclust = e->nch;
		nd->date = e->date;
		nd->time = e->time;
		if (!e->isdir && touched) host_note(vf, e->node);
	}
	// == what could not get to its place: content kept visible, parked entries named again,
	// then what waited for them to be deleted
	for (i = 1; i < sc.n; i++) {
		vfEnt* e = sc.ent + i;
		if (e->tmp >= 0) tmp_recover(vf, e);
		if ((e->node >= 0) && e->phname) park_restore(vf, e);
	}
	del_files(vf, &sc, gone, claim, oldNodes, 1, &failed);
	del_dirs(vf, gone, oldNodes, &failed);
	// == the map of clusters follows what got done; what the guest saw of it stays
	rechain = (unsigned char*)calloc(vf->nodes, 1);
	if (!rechain) goto out;
	for (i = 0; i < sc.n; i++)
		if (ENT_DONE(sc.ent + i)) rechain[sc.ent[i].node] = 1;
	for (c = 0; c < n; c++) {
		o = w->own[c];
		if ((o >= 0) && (vf->node[o].dead || rechain[o])) w->own[c] = VFW_FREE;
	}
	for (i = 0; i < sc.n; i++) {
		vfEnt* e = sc.ent + i;
		if (!ENT_DONE(e)) continue;
		for (k = 0; k < e->nch; k++) {
			c = sc.pool[e->chain + k];
			w->own[c] = e->node;
			w->oidx[c] = k;
		}
	}
	for (i = 0; i < nsnap; i++) {
		unsigned char buf[VF_SECSIZE];
		vfw_read(vf, snap[i].lba, buf);
		if (memcmp(buf, snap[i].data, VF_SECSIZE)) ovl_put(w, snap[i].lba, snap[i].data, 0);
	}
	// == a sector the host gives back now goes, and one whose cluster the guest freed after
	// writing it. Data in a free cluster the guest has not freed since stays: a driver
	// writes a file's data before the FAT that claims it, and may take its time.
	for (i = 0; i < w->capsec; i++) {
		unsigned char buf[VF_SECSIZE];
		unsigned int lba = w->sec[i].lba;
		unsigned int rel;
		if ((lba == 0xffffffff) || (lba < vf->partStart)) continue;
		rel = lba - vf->partStart;
		if (rel < vf->dataStart) continue;
		rel -= vf->dataStart;
		c = rel / vf->spc + 2;
		s = rel % vf->spc;
		if (c >= n) continue;
		o = w->own[c];
		if (o >= 0) {
			if (vf->node[o].isdir || ((w->oidx[c] * cbytes + s * VF_SECSIZE) >= vf->node[o].size)) continue;
			vfw_gen_data(vf, c, s, buf);
			if (!memcmp(buf, w->sec[i].data, VF_SECSIZE)) ovl_drop(w, i);
		} else if (!(w->fat[0][c] & 0x0fffffff) && (w->freed[c] > w->sec[i].gen)) {
			ovl_drop(w, i);
		}
	}
	vf_close_cached(vf);
	for (i = 1; i < sc.n; i++)
		if (!sc.ent[i].ok && !sc.ent[i].pin) failed++;
	w->pending = failed;
	w->sgen = wgen;
	res = 1;
out:
	free(match);
	free(gone);
	free(held);
	free(dirty);
	free(claim);
	free(rechain);
	free(ord);
	free(snap);
	scan_free(&sc);
	w->quiet = 0;
	return res;
}

// == the folder changed on the host: Refresh ==
// Only the difference goes in, as a guest would write it: what stayed keeps its clusters and
// entries, so a guest's open file and current directory read on.

typedef struct {
	vFat* vf;
	const vFat* fresh;
	int* tab;		// live nodes by parent and 8.3 name, -1 free
	int* htab;		// ...and by parent and host name
	unsigned int cap;	// both tables' size, a power of two
	int parent;		// whose children rf_taken asks about
	unsigned int per;	// entries in a directory's cluster
	unsigned char* busy;	// per cluster: not to be handed out (data the guest wrote ahead of its FAT, freed now)
	unsigned int hi;	// the lowest cluster a run was taken at: the next run is looked for below it
	unsigned int lo;	// where the look for single free clusters goes on, from the top down
	unsigned int norun;	// a run this long or longer is not there below hi
	unsigned int* lhint;	// per node: the slot of its directory a free run is looked for from
	unsigned int* fhint;	// ...and an entry, wrapping round
	int ncap;		// nodes the hints hold
	int* fmap;		// per fresh node: the volume's node it is, -1 new, -2 left out
	int* rmap;		// per node: the fresh node it is, -1 none
	int* fents;		// per fresh folder new to the volume: the entries it gets
	int* vents;		// per node: the entries it gets
	int* gone;		// nodes to go, each the top of what goes
	int ngone;
	int news, changes;
	unsigned int need;	// clusters the changes take at most
} vfRf;

static unsigned int rf_nclu(vFat* vf, unsigned int size) {
	unsigned long long cbytes = (unsigned long long)vf->spc * VF_SECSIZE;
	return (unsigned int)(((unsigned long long)size + cbytes - 1) / cbytes);
}

static unsigned int rf_hhash(int parent, const char* s) {
	unsigned int res = 0x811c9dc5 ^ (unsigned int)parent;
	for (; *s; s++)
		res = (res ^ (unsigned char)*s) * 0x01000193;
	return res * 0x9e3779b1u;
}

static void rf_index(vfRf* rf, int idx) {
	vfNode* nd = rf->vf->node + idx;
	unsigned int m = rf->cap - 1;
	unsigned int pos;
	for (pos = vf_sname_hash(nd->parent, nd->sname) & m; rf->tab[pos] >= 0; pos = (pos + 1) & m);
	rf->tab[pos] = idx;
	if (!nd->hname) return;
	for (pos = rf_hhash(nd->parent, nd->hname) & m; rf->htab[pos] >= 0; pos = (pos + 1) & m);
	rf->htab[pos] = idx;
}

static int rf_taken(void* ctx, const char* sname) {
	vfRf* rf = (vfRf*)ctx;
	vFat* vf = rf->vf;
	unsigned int m = rf->cap - 1;
	unsigned int pos;
	int i;
	for (pos = vf_sname_hash(rf->parent, sname) & m; (i = rf->tab[pos]) >= 0; pos = (pos + 1) & m) {
		if (!vf->node[i].dead && (vf->node[i].parent == rf->parent) && !memcmp(vf->node[i].sname, sname, 11))
			return 1;
	}
	return 0;
}

// a live child of parent by its host name, -1 none
static int rf_child(vfRf* rf, int parent, const char* hname) {
	vFat* vf = rf->vf;
	unsigned int m = rf->cap - 1;
	unsigned int pos;
	int i;
	for (pos = rf_hhash(parent, hname) & m; (i = rf->htab[pos]) >= 0; pos = (pos + 1) & m) {
		if (!vf->node[i].dead && (vf->node[i].parent == parent) && vf->node[i].hname && !strcmp(vf->node[i].hname, hname))
			return i;
	}
	return -1;
}

// a pinned folder of the root the guest knows by this name (pins live in the root only)
static int rf_pin_named(vFat* vf, const char* name) {
	int i;
	for (i = vf->node[0].child; i >= 0; i = vf->node[i].next)
		if (vf->node[i].pinned && name_eq(vf->node[i].name, name)) return 1;
	return 0;
}

static int rf_free_clu(vfRf* rf, unsigned int c) {
	vfWr* w = rf->vf->wr;
	return !(w->fat[0][c] & 0x0fffffff) && !(w->fat[1][c] & 0x0fffffff) && !rf->busy[c];
}

// clusters the refresh may hand out
static unsigned int rf_count_free(vfRf* rf) {
	unsigned int c, n = 0;
	for (c = 2; c < rf->vf->clusters + 2; c++)
		if (rf_free_clu(rf, c)) n++;
	return n;
}

// clusters the FAT has free, for FSInfo
static unsigned int rf_fat_free(vFat* vf) {
	unsigned int c, n = 0;
	for (c = 2; c < vf->clusters + 2; c++)
		if (!(vf->wr->fat[0][c] & 0x0fffffff)) n++;
	return n;
}

// a sector into memory; with no memory for it the volume stops following the folder, as a
// guest's write does then
static void rf_ovl(vFat* vf, unsigned int lba, const unsigned char* sec) {
	if (!ovl_put(vf->wr, lba, sec, 0)) vfw_detach(vf, "there was no memory left to refresh it", 0);
}

static void rf_drop_clu(vFat* vf, unsigned int c) {
	unsigned int s;
	int i;
	for (s = 0; s < vf->spc; s++)
		if ((i = ovl_find(vf->wr, clu_lba(vf, c, s))) >= 0) ovl_drop(vf->wr, i);
}

static void rf_set(vFat* vf, unsigned int c, unsigned int v) {
	vf->wr->fat[0][c] = (vf->wr->fat[0][c] & 0xf0000000) | v;
	vf->wr->fat[1][c] = (vf->wr->fat[1][c] & 0xf0000000) | v;
}

// a cluster freed by this refresh: out of the FAT and the map, and kept from new files
static void rf_free(vfRf* rf, unsigned int c) {
	rf_set(rf->vf, c, 0);
	rf->vf->wr->own[c] = VFW_FREE;
	rf->vf->wr->freed[c] = rf->vf->wr->wgen;
	rf->busy[c] = 1;
}

// n free clusters, ascending, into out: a run below the last one taken if there is one,
// else single ones from the top down; 0 when there are not that many
static int rf_alloc(vfRf* rf, unsigned int n, unsigned int* out) {
	unsigned int c, run = 0, k;
	if (!n) return 1;
	if (n < rf->norun) {
		for (c = rf->hi; c-- > 2;) {
			if (!rf_free_clu(rf, c)) {
				run = 0;
				continue;
			}
			if (++run < n) continue;
			for (k = 0; k < n; k++) out[k] = c + k;
			rf->hi = c;
			return 1;
		}
		rf->norun = n;
	}
	k = n;
	for (c = rf->lo; (c-- > 2) && k;) {
		if (rf_free_clu(rf, c)) out[--k] = c;
		rf->lo = c;
	}
	if (k) return 0;
	if (out[0] < rf->hi) rf->hi = out[0];
	return 1;
}

// clusters linked in order, the last one ending the chain; each is the node's, from its
// k-th cluster on
static void rf_link(vFat* vf, const unsigned int* cl, unsigned int n, int node, unsigned int k) {
	unsigned int i;
	for (i = 0; i < n; i++) {
		rf_set(vf, cl[i], (i + 1 < n) ? cl[i + 1] : VF_EOC);
		vf->wr->own[cl[i]] = node;
		vf->wr->oidx[cl[i]] = k + i;
	}
}

// the chain from first (malloc'd, *n its length); NULL when there is no memory
static unsigned int* rf_chain(vFat* vf, unsigned int first, unsigned int* n) {
	unsigned int cap = 16, c = first, v;
	unsigned int* ch = (unsigned int*)malloc(cap * sizeof(unsigned int));
	*n = 0;
	if (!ch) return NULL;
	while ((c >= 2) && (c < vf->clusters + 2)) {
		if (*n >= vf->clusters) break;			// a loop
		if (*n >= cap) {
			unsigned int* p = (unsigned int*)realloc(ch, cap * 2 * sizeof(unsigned int));
			if (!p) { free(ch); return NULL; }
			ch = p;
			cap *= 2;
		}
		ch[(*n)++] = c;
		v = vf->wr->fat[0][c] & 0x0fffffff;
		if ((v >= 0x0ffffff8) || (v < 2)) break;
		c = v;
	}
	return ch;
}

// a slot of a directory: its sector's LBA and the entry's offset in it; 0 past its chain
static int rf_slot(vfRf* rf, const unsigned int* ch, unsigned int n, unsigned int slot, unsigned int* lba, int* off) {
	*lba = 0;
	*off = 0;
	if (slot / rf->per >= n) return 0;
	*lba = clu_lba(rf->vf, ch[slot / rf->per], (slot % rf->per) / (VF_SECSIZE / 32));
	*off = (slot % (VF_SECSIZE / 32)) * 32;
	return 1;
}

// the slot of a node's 8.3 entry (by its name and first cluster clu) in its parent, and the
// first of the long name entries before it; looked for from the parent's hint on, then from
// the start up to it. 0 if not found.
static int rf_find(vfRf* rf, const unsigned int* ch, unsigned int n, int idx, unsigned int clu, unsigned int* at, unsigned int* from) {
	vFat* vf = rf->vf;
	unsigned char sec[VF_SECSIZE];
	unsigned int all = n * rf->per;
	unsigned int s0 = rf->fhint[vf->node[idx].parent];
	unsigned int slot, end, lba, cur, lfn;
	int off, pass;
	vfNode* nd = vf->node + idx;
	if (s0 >= all) s0 = 0;
	for (pass = 0; pass < 2; pass++) {
		slot = pass ? 0 : s0;
		end = pass ? s0 : all;
		cur = lfn = 0xffffffff;
		for (; slot < end; slot++) {
			rf_slot(rf, ch, n, slot, &lba, &off);
			if (lba != cur) {
				vfw_read(vf, lba, sec);
				cur = lba;
			}
			if (sec[off] == 0x00) break;
			if (sec[off] == 0xe5) { lfn = 0xffffffff; continue; }
			if (sec[off + 11] == 0x0f) {
				if (lfn == 0xffffffff) lfn = slot;
				continue;
			}
			if (!memcmp(sec + off, nd->sname, 11) && ((sec[off + 11] & 0x10) == (nd->isdir ? 0x10 : 0x00))
					&& (((vf_rd16(sec + off + 20) << 16) | vf_rd16(sec + off + 26)) == clu)) {
				*at = slot;
				*from = (lfn == 0xffffffff) ? slot : lfn;
				rf->fhint[nd->parent] = slot + 1;
				return 1;
			}
			lfn = 0xffffffff;
		}
	}
	return 0;
}

// one 32-byte entry written into a directory's slot
static void rf_put_slot(vfRf* rf, const unsigned int* ch, unsigned int n, unsigned int slot, const unsigned char* ent) {
	unsigned char sec[VF_SECSIZE];
	unsigned int lba;
	int off;
	if (!rf_slot(rf, ch, n, slot, &lba, &off)) return;
	vfw_read(rf->vf, lba, sec);
	memcpy(sec + off, ent, 32);
	rf_ovl(rf->vf, lba, sec);
}

// a node's entries in its parent marked deleted; 0 when they are not where the node says
static int rf_unlist(vfRf* rf, int idx) {
	vFat* vf = rf->vf;
	unsigned char sec[VF_SECSIZE];
	unsigned int n, at, from, s, lba;
	int off, found;
	unsigned int* ch = rf_chain(vf, vf->node[vf->node[idx].parent].clust, &n);
	if (!ch) return 0;
	found = rf_find(rf, ch, n, idx, vf->node[idx].clust, &at, &from);
	if (found) {
		for (s = from; s <= at; s++) {
			rf_slot(rf, ch, n, s, &lba, &off);
			vfw_read(vf, lba, sec);
			sec[off] = 0xe5;
			rf_ovl(vf, lba, sec);
		}
	}
	free(ch);
	return found;
}

// a node's entries put into a free run of its parent's slots, looked for from the parent's
// hint on; the parent grows by a cluster when it has none. 0 when it cannot.
static int rf_list(vfRf* rf, int idx) {
	vFat* vf = rf->vf;
	unsigned char ents[21 * 32];
	unsigned char zero[32];
	unsigned char sec[VF_SECSIZE];
	unsigned short name[VF_MAXNAME + 1];
	unsigned int n, slot, run, start, lba, cur, cl;
	int cnt = 0, len, parts, i, p, grown, off, atend;
	unsigned int* ch;
	vfNode* nd = vf->node + idx;
	p = nd->parent;
	memset(ents, 0x00, sizeof(ents));
	memset(zero, 0x00, sizeof(zero));
	if (nd->lfn) {
		len = vf_utf16(nd->name, name, VF_MAXNAME);
		parts = (len + 12) / 13;
		for (i = 0; i < parts; i++)
			vf_lfn(ents + 32 * cnt++, name, len, parts - i, (i == 0), vf_sname_sum(nd->sname));
	}
	vf_entry(ents + 32 * cnt++, nd->sname, nd->isdir ? 0x10 : (vf->writable ? 0x20 : 0x21), nd->clust, nd->size, nd->date, nd->time);
	for (grown = 0; grown < 2; grown++) {
		ch = rf_chain(vf, vf->node[p].clust, &n);
		if (!ch) return 0;
		run = start = 0;
		cur = 0xffffffff;
		atend = 0;
		for (slot = rf->lhint[p]; slot < n * rf->per; slot++) {
			rf_slot(rf, ch, n, slot, &lba, &off);
			if (lba != cur) {
				vfw_read(vf, lba, sec);
				cur = lba;
			}
			if (!atend && (sec[off] == 0x00)) atend = 1;	// the end: every slot past it is free
			if (!atend && (sec[off] != 0xe5)) {
				run = 0;
				continue;
			}
			if (!run) start = slot;
			if (++run < (unsigned int)cnt) continue;
			for (i = 0; i < cnt; i++)
				rf_put_slot(rf, ch, n, start + i, ents + 32 * i);
			if (atend)		// the end moves past the run; whatever lay there is not entries
				rf_put_slot(rf, ch, n, start + cnt, zero);
			rf->lhint[p] = start + cnt;
			free(ch);
			return 1;
		}
		// no room: one more cluster, zero, at the end of its chain
		if ((n * rf->per >= VF_MAXDENTS) || !rf_alloc(rf, 1, &cl)) {
			free(ch);
			return 0;
		}
		rf_set(vf, ch[n - 1], cl);
		rf_link(vf, &cl, 1, p, n);
		vf->node[p].nclust = n + 1;
		free(ch);
	}
	return 0;
}

// a node and everything in it gone from the volume: clusters freed, nodes dead; how many
static int rf_drop(vfRf* rf, int idx) {
	vFat* vf = rf->vf;
	unsigned int n, k;
	unsigned int* ch;
	int c, cnt = 1;
	if (vf->node[idx].clust && (ch = rf_chain(vf, vf->node[idx].clust, &n))) {
		for (k = 0; k < n; k++) {
			rf_free(rf, ch[k]);
			rf_drop_clu(vf, ch[k]);
		}
		free(ch);
	}
	for (c = vf->node[idx].child; c >= 0; c = vf->node[c].next)
		cnt += rf_drop(rf, c);
	vf->node[idx].dead = 1;
	return cnt;
}

// a file the host changed: its chain fitted to the new size, its entry made new, its old
// content gone from memory - the host's wins. 0 when there is no room, or its entry is not
// where the node says (then nothing changes).
static int rf_change(vfRf* rf, int idx, unsigned int size, unsigned int mtime) {
	vFat* vf = rf->vf;
	vfNode* nd = vf->node + idx;
	unsigned char sec[VF_SECSIZE];
	unsigned int n0 = 0, n1 = rf_nclu(vf, size);
	unsigned int was = nd->clust;
	unsigned int k, extra, last, pn, at, from, lba;
	unsigned int* pch;
	unsigned int* ch = NULL;
	unsigned int* cl;
	int off, ok = 0;
	if (!(pch = rf_chain(vf, vf->node[nd->parent].clust, &pn))) return 0;
	if (!rf_find(rf, pch, pn, idx, was, &at, &from)) goto out;
	if (was && !(ch = rf_chain(vf, was, &n0))) goto out;
	if (n1 > n0) {
		extra = n1 - n0;
		if (!(cl = (unsigned int*)malloc(extra * sizeof(unsigned int)))) goto out;
		last = n0 ? ch[n0 - 1] : 0;
		for (k = 0; last && (k < extra) && (last + 1 + k < vf->clusters + 2) && rf_free_clu(rf, last + 1 + k); k++)
			cl[k] = last + 1 + k;		// on from its end if that is free
		if ((k < extra) && !rf_alloc(rf, extra, cl)) {
			free(cl);
			goto out;
		}
		rf_link(vf, cl, extra, idx, n0);
		if (n0) rf_set(vf, ch[n0 - 1], cl[0]);
		else nd->clust = cl[0];
		free(cl);
	} else if (n1 < n0) {
		for (k = n1; k < n0; k++) rf_free(rf, ch[k]);
		if (n1) rf_set(vf, ch[n1 - 1], VF_EOC);
		else nd->clust = 0;
	}
	for (k = 0; k < n0; k++) rf_drop_clu(vf, ch[k]);
	nd->nclust = n1;
	nd->size = nd->hsize = size;
	nd->hmtime = mtime;
	vf_fat_stamp(mtime, &nd->date, &nd->time);
	rf_slot(rf, pch, pn, at, &lba, &off);
	vfw_read(vf, lba, sec);
	vf_wr16(sec + off + 20, (nd->clust >> 16) & 0xffff);
	vf_wr16(sec + off + 26, nd->clust & 0xffff);
	vf_wr32(sec + off + 28, nd->size);
	vf_wr16(sec + off + 22, nd->time);
	vf_wr16(sec + off + 24, nd->date);
	rf_ovl(vf, lba, sec);
	ok = 1;
out:
	free(ch);
	free(pch);
	return ok;
}

// a node new to the volume, in parent: clusters, its own dot entries if a folder, its
// entries in the parent; -1 when there is no room
static int rf_add(vfRf* rf, int parent, const vfNode* fn) {
	vFat* vf = rf->vf;
	unsigned int n = fn->isdir ? 1 : rf_nclu(vf, fn->size);
	unsigned char sec[VF_SECSIZE];
	char sname[12], base[12];
	unsigned int* cl = NULL;
	int idx, ulen;
	vfNode* nd;
	if (vf->nodes >= rf->ncap) return -1;
	if (n && (!(cl = (unsigned int*)malloc(n * sizeof(unsigned int))) || !rf_alloc(rf, n, cl))) {
		free(cl);
		return -1;
	}
	idx = node_new(vf, fn->name, fn->hname, fn->isdir);
	if (idx < 0) {
		free(cl);
		return -1;
	}
	node_append(vf, parent, idx);
	nd = vf->node + idx;
	vf_sname_make(fn->name, base, &ulen);
	memcpy(sname, base, 12);
	rf->parent = parent;
	vf_sname_uniq(sname, fn->name, rf_taken, rf);
	memcpy(nd->sname, sname, 12);
	// a name made unique here needs its long name as much as one the 8.3 form loses
	nd->lfn = (fn->lfn || memcmp(sname, base, 11)) ? 1 : 0;
	nd->ecnt = nd->lfn ? ((ulen + 12) / 13 + 1) : 1;
	nd->size = nd->hsize = fn->isdir ? 0 : fn->size;
	nd->hmtime = fn->hmtime;
	nd->date = fn->date;
	nd->time = fn->time;
	if (n) {
		rf_link(vf, cl, n, idx, 0);
		nd->clust = cl[0];
		nd->nclust = n;
	}
	free(cl);
	if (fn->isdir) {		// its dot and dotdot
		memset(sec, 0x00, VF_SECSIZE);
		vf_gen_dir(vf, idx, 0, sec);
		rf_ovl(vf, clu_lba(vf, nd->clust, 0), sec);
	}
	rf_index(rf, idx);
	if (!rf_list(rf, idx)) {
		rf_drop(rf, idx);
		node_unlink(vf, idx);
		return -1;
	}
	return idx;
}

static void rf_close(vfRf* rf) {
	free(rf->tab);
	free(rf->htab);
	free(rf->busy);
	free(rf->lhint);
	free(rf->fhint);
	free(rf->fmap);
	free(rf->rmap);
	free(rf->fents);
	free(rf->vents);
	free(rf->gone);
}

// the tables, and the volume's nodes indexed; 0 when there is no memory
static int rf_open(vfRf* rf, vFat* vf, const vFat* fresh) {
	int v;
	memset(rf, 0x00, sizeof(vfRf));
	rf->vf = vf;
	rf->fresh = fresh;
	rf->per = vf->spc * (VF_SECSIZE / 32);
	for (rf->cap = 1024; rf->cap < 2u * (unsigned int)(vf->nodes + fresh->nodes); rf->cap <<= 1);
	rf->hi = rf->lo = vf->clusters + 2;
	rf->norun = 0xffffffff;
	rf->ncap = vf->nodes + fresh->nodes;
	rf->tab = (int*)malloc(rf->cap * sizeof(int));
	rf->htab = (int*)malloc(rf->cap * sizeof(int));
	rf->busy = (unsigned char*)calloc(vf->clusters + 2, 1);
	rf->lhint = (unsigned int*)calloc(rf->ncap, sizeof(unsigned int));
	rf->fhint = (unsigned int*)calloc(rf->ncap, sizeof(unsigned int));
	rf->fmap = (int*)malloc(fresh->nodes * sizeof(int));
	rf->rmap = (int*)malloc(vf->nodes * sizeof(int));
	rf->fents = (int*)calloc(fresh->nodes, sizeof(int));
	rf->vents = (int*)calloc(vf->nodes, sizeof(int));
	rf->gone = (int*)malloc(vf->nodes * sizeof(int));
	if (!rf->tab || !rf->htab || !rf->busy || !rf->lhint || !rf->fhint || !rf->fmap || !rf->rmap || !rf->fents
			|| !rf->vents || !rf->gone) return 0;
	memset(rf->tab, 0xff, rf->cap * sizeof(int));
	memset(rf->htab, 0xff, rf->cap * sizeof(int));
	memset(rf->rmap, 0xff, vf->nodes * sizeof(int));
	for (v = 1; v < vf->nodes; v++)
		if (!vf->node[v].dead && (vf->node[v].parent >= 0)) rf_index(rf, v);
	return 1;
}

// what is what: the fresh tree from the root down, by host names within each folder; what
// is new and changed counted, and the clusters they take at most
static void rf_match(vfRf* rf) {
	vFat* vf = rf->vf;
	const vFat* fresh = rf->fresh;
	int j, v, fp;
	rf->fmap[0] = 0;
	rf->rmap[0] = 0;
	for (j = 1; j < fresh->nodes; j++) {
		const vfNode* fn = fresh->node + j;
		fp = rf->fmap[fn->parent];
		rf->fmap[j] = -1;
		if (fp == -2) {
			rf->fmap[j] = -2;
			continue;
		}
		if (fp >= 0) {
			v = rf_child(rf, fp, fn->hname);
			if ((v >= 0) && !node_pinned(vf, v) && (vf->node[v].isdir == fn->isdir)) {
				rf->fmap[j] = v;
				rf->rmap[v] = j;
				if (!fn->isdir && ((fn->size != vf->node[v].hsize) || (fn->hmtime != vf->node[v].hmtime))) {
					unsigned int n1 = rf_nclu(vf, fn->size);
					if (n1 > vf->node[v].nclust) rf->need += n1 - vf->node[v].nclust;
					rf->changes++;
				}
				continue;
			}
			if (!fp && rf_pin_named(vf, fn->name)) {	// the Next's program folder has that name
				rf->fmap[j] = -2;
				continue;
			}
			rf->vents[fp] += fn->lfn ? fn->ecnt : 2;	// the volume may find its 8.3 name taken
		} else {
			rf->fents[fn->parent] += fn->ecnt;
		}
		rf->need += fn->isdir ? 0 : rf_nclu(vf, fn->size);
		rf->news++;
	}
	// a folder's growth, at most: its new entries and an end marker, cluster by cluster
	for (j = 1; j < fresh->nodes; j++)
		if ((rf->fmap[j] == -1) && fresh->node[j].isdir) rf->need += (rf->fents[j] + 2 + 1 + rf->per - 1) / rf->per;
	for (v = 0; v < vf->nodes; v++)
		if (rf->vents[v]) rf->need += (rf->vents[v] + 1 + rf->per - 1) / rf->per;
}

// what went from the folder, each from the top of what goes. Left be: what lies deeper than
// the scan looks, and what the host still has spelled just so though the scan did not list it
// (a hidden name, a link) - another case of it, or an 8.3 alias, a case-blind host also
// answers for is gone.
static void rf_gone(vfRf* rf, int maxdepth) {
	vFat* vf = rf->vf;
	char path[2048];
	int v, a, d, top;
	for (v = 1; v < vf->nodes; v++) {
		vfNode* nd = vf->node + v;
		if (nd->dead || (nd->parent < 0) || (rf->rmap[v] >= 0) || node_pinned(vf, v)) continue;
		d = depth_of(vf, v);
		if ((d > maxdepth + 1) || (nd->isdir && (d > maxdepth))) continue;
		top = 1;
		for (a = nd->parent; a > 0; a = vf->node[a].parent)
			if ((rf->rmap[a] < 0) && !vf->node[a].dead) { top = 0; break; }
		if (!top) continue;
		if (nd->hname && vf->host.exact && vf_path(vf, v, path, sizeof(path)) && vf->host.exact(vf->host.ctx, path)) continue;
		rf->gone[rf->ngone++] = v;
	}
}

// a cluster holding data the guest wrote ahead of the FAT that will claim it is not free
static void rf_busy(vfRf* rf) {
	vFat* vf = rf->vf;
	vfWr* w = vf->wr;
	unsigned int i, c, lba;
	for (i = 0; i < (unsigned int)w->capsec; i++) {
		lba = w->sec[i].lba;
		if ((lba == 0xffffffff) || (lba < vf->partStart + vf->dataStart)) continue;
		c = (lba - vf->partStart - vf->dataStart) / vf->spc + 2;
		if (c < vf->clusters + 2) rf->busy[c] = 1;
	}
}

// FSInfo and its copy tell the truth; a guest that read it before keeps its own count
static void rf_fsinfo(vFat* vf) {
	static const unsigned int at[2] = {1, 7};
	unsigned char sec[VF_SECSIZE];
	unsigned int have = rf_fat_free(vf);
	int k;
	vf->used = vf->clusters - have;
	for (k = 0; k < 2; k++) {
		vfw_read(vf, vf->partStart + at[k], sec);
		vf_wr32(sec + 488, have);
		rf_ovl(vf, vf->partStart + at[k], sec);
	}
}

int vfat_refresh_ready(vFat* vf) {
	if (!vf || !vf->wr) return VFR_OK;
	if (vf->wr->detached) return VFR_DETACHED;
	if (vf->wr->wgen != vf->wr->sgen) return VFR_WRITES;
	if (vf->wr->pending) return VFR_STUCK;
	return VFR_OK;
}

const char* vfat_stuck(vFat* vf) {
	return (vf && vf->wr) ? vf->wr->fnames : "";
}

int vfat_refresh(vFat* vf, const vFat* fresh, int maxdepth, vfRefresh* out) {
	vfRf rf;
	vfRefresh cnt;
	int i, j, v, res, failed = 0;
	memset(&cnt, 0x00, sizeof(cnt));
	if (out) *out = cnt;
	if (!vf || !fresh || (fresh->nodes < 1)) return VFR_MEMORY;
	if ((res = vfat_refresh_ready(vf)) != VFR_OK) return res;
	res = VFR_MEMORY;
	if (!rf_open(&rf, vf, fresh)) goto out;
	rf_match(&rf);
	rf_gone(&rf, maxdepth);
	if (!rf.news && !rf.changes && !rf.ngone) {	// the volume need not go live for nothing
		res = VFR_OK;
		goto out;
	}
	if (!vf->wr && !vfw_live(vf)) goto out;
	vf_close_cached(vf);
	rf_busy(&rf);
	if (rf.need > rf_count_free(&rf)) {
		res = VFR_FULL;
		goto out;
	}
	for (i = 0; i < rf.ngone; i++) {
		v = rf.gone[i];
		if (!rf_unlist(&rf, v)) {		// its entry is not where the node says: left be
			failed++;
			continue;
		}
		cnt.removed += rf_drop(&rf, v);
		node_unlink(vf, v);
	}
	for (j = 1; j < fresh->nodes; j++) {
		const vfNode* fn = fresh->node + j;
		v = rf.fmap[j];
		if ((v <= 0) || fn->isdir) continue;
		if ((fn->size == vf->node[v].hsize) && (fn->hmtime == vf->node[v].hmtime)) continue;
		if (rf_change(&rf, v, fn->size, fn->hmtime)) cnt.changed++;
		else failed++;
	}
	// new ones, parents first: the scan put them in that order
	for (j = 1; j < fresh->nodes; j++) {
		if (rf.fmap[j] != -1) continue;
		v = (rf.fmap[fresh->node[j].parent] < 0) ? -1 : rf_add(&rf, rf.fmap[fresh->node[j].parent], fresh->node + j);
		if (v < 0) {			// no room, or its folder could not be put on
			rf.fmap[j] = -2;
			failed++;
			continue;
		}
		rf.fmap[j] = v;
		cnt.added++;
	}
	if (cnt.added || cnt.removed || cnt.changed) rf_fsinfo(vf);
	vf_close_cached(vf);
	cnt.left = failed;
	xlog(XLG_DISK, XLL_INFO, "folder %s: refreshed, %i new, %i gone, %i changed, %i left out", vf->root ? vf->root : "?",
		cnt.added, cnt.removed, cnt.changed, failed);
	res = vf->wr->detached ? VFR_MEMORY : failed ? VFR_PART : VFR_OK;
	if (out) *out = cnt;
out:
	rf_close(&rf);
	return res;
}

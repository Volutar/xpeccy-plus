// vfat-bench: a folder volume taking writes, driven by FatFs as the guest.
//
// A scratch folder is built, mounted, and changed through FatFs at random -
// files made, grown, patched, cut, renamed and moved, folders made, moved and
// removed - while a model keeps what the volume should hold. After every sync
// the host folder must equal the model byte for byte, the guest must read the
// model back, and a fresh mount of the folder must too.
//
//	vfat-bench WORKDIR [seeds] [ops]
//
// Prints one line per case; the quick tier compares them with expected.txt.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/utime.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <utime.h>
#endif

// FatFs has a DIR of its own, which POSIX's dirent.h has taken
#define DIR FF_DIR
#include "ff.h"
#include "diskio.h"
#undef DIR
#include "vfat.h"
#include "xlog.h"

// == the host side ==

#ifdef _WIN32
static wchar_t* wide(const char* s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t* w = (wchar_t*)malloc(n * sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return w;
}

static char* narrow(const wchar_t* w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char* s = (char*)malloc(n);
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	return s;
}
#endif

static int trash_on = 1;
static const char* fail_pat = NULL;	// host operations on a path holding this refuse
static int torture = 0;			// a sync after every sector the guest writes
static int sync_checked(int force);
static void fpath(char* out, int max, const char* rel);

static int refused(const char* path) {
	return fail_pat && strstr(path, fail_pat);
}
static char trash_dir[1024];
static int trash_n = 0;
static int host_ops = 0;

static FILE* h_open(void* ctx, const char* path, const char* mode) {
	if ((mode[0] != 'r' || strchr(mode, '+')) && refused(path)) return NULL;
#ifdef _WIN32
	wchar_t* p = wide(path);
	wchar_t* m = wide(mode);
	FILE* f = _wfopen(p, m);
	free(p); free(m);
	return f;
#else
	return fopen(path, mode);
#endif
}

static int h_kind(void* ctx, const char* path) {
#ifdef _WIN32
	wchar_t* p = wide(path);
	DWORD a = GetFileAttributesW(p);
	free(p);
	if (a == INVALID_FILE_ATTRIBUTES) return 0;
	return (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
#else
	struct stat st;
	if (lstat(path, &st)) return 0;
	return S_ISDIR(st.st_mode) ? 2 : 1;
#endif
}

static int h_mkdir(void* ctx, const char* path) {
	host_ops++;
	if (refused(path)) return -1;
#ifdef _WIN32
	wchar_t* p = wide(path);
	int r = CreateDirectoryW(p, NULL) ? 0 : -1;
	free(p);
	return r;
#else
	return mkdir(path, 0777);
#endif
}

static int h_rename(void* ctx, const char* from, const char* to) {
	host_ops++;
	if (refused(from) || refused(to)) return -1;
	if (h_kind(ctx, to)) return -1;
#ifdef _WIN32
	wchar_t* a = wide(from);
	wchar_t* b = wide(to);
	int r = MoveFileW(a, b) ? 0 : -1;
	free(a); free(b);
	return r;
#else
	return rename(from, to);
#endif
}

static int h_replace(void* ctx, const char* from, const char* to) {
	host_ops++;
	if (refused(from) || refused(to)) return -1;
#ifdef _WIN32
	wchar_t* a = wide(from);
	wchar_t* b = wide(to);
	int r = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
	free(a); free(b);
	return r;
#else
	return rename(from, to);
#endif
}

static int rm_plain(const char* path, int isdir) {
#ifdef _WIN32
	wchar_t* p = wide(path);
	int r = (isdir ? RemoveDirectoryW(p) : DeleteFileW(p)) ? 0 : -1;
	free(p);
	return r;
#else
	return isdir ? rmdir(path) : unlink(path);
#endif
}

// the trash is a folder beside the scratch one, so what went there can be looked at
static int h_remove(void* ctx, const char* path, int isdir) {
	char to[1200];
	host_ops++;
	if (refused(path)) return -1;
	if (!trash_on) return rm_plain(path, isdir);
	snprintf(to, sizeof(to), "%s/%i", trash_dir, trash_n++);
#ifdef _WIN32
	wchar_t* a = wide(path);
	wchar_t* b = wide(to);
	int r = MoveFileW(a, b) ? 0 : -1;
	free(a); free(b);
	return r;
#else
	return rename(path, to);
#endif
}

static int h_truncate(void* ctx, const char* path, long long size) {
	host_ops++;
	if (refused(path)) return -1;
#ifdef _WIN32
	FILE* f = h_open(ctx, path, "r+b");
	int r;
	if (!f) return -1;
	r = _chsize_s(_fileno(f), size) ? -1 : 0;
	fclose(f);
	return r;
#else
	return truncate(path, size);
#endif
}

typedef struct {
	char** name;
	int* isdir;
	int n;
} hList;

static void hl_free(hList* l) {
	int i;
	for (i = 0; i < l->n; i++) free(l->name[i]);
	free(l->name);
	free(l->isdir);
	memset(l, 0x00, sizeof(hList));
}

static void hl_add(hList* l, const char* name, int isdir) {
	l->name = (char**)realloc(l->name, (l->n + 1) * sizeof(char*));
	l->isdir = (int*)realloc(l->isdir, (l->n + 1) * sizeof(int));
	l->name[l->n] = strdup(name);
	l->isdir[l->n] = isdir;
	l->n++;
}

static int hl_list(const char* path, hList* l) {
	memset(l, 0x00, sizeof(hList));
#ifdef _WIN32
	char pat[1200];
	WIN32_FIND_DATAW fd;
	HANDLE h;
	snprintf(pat, sizeof(pat), "%s/*", path);
	wchar_t* p = wide(pat);
	h = FindFirstFileW(p, &fd);
	free(p);
	if (h == INVALID_HANDLE_VALUE) return 0;
	do {
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
		char* s = narrow(fd.cFileName);
		hl_add(l, s, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0);
		free(s);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
#else
	DIR* d = opendir(path);
	struct dirent* e;
	char full[1200];
	if (!d) return 0;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
		hl_add(l, e->d_name, h_kind(NULL, full) == 2);
	}
	closedir(d);
#endif
	return 1;
}

static int h_empty(void* ctx, const char* path) {
	hList l;
	int r;
	hl_list(path, &l);
	r = (l.n == 0);
	hl_free(&l);
	return r;
}

static void h_settime(void* ctx, const char* path, unsigned int mtime) {
#ifdef _WIN32
	struct _utimbuf t;
	wchar_t* p = wide(path);
	t.actime = t.modtime = mtime;
	_wutime(p, &t);
	free(p);
#else
	struct utimbuf t;
	t.actime = t.modtime = mtime;
	utime(path, &t);
#endif
}

static int h_stat(void* ctx, const char* path, long long* size, long long* mtime) {
#ifdef _WIN32
	struct _stat64 st;
	wchar_t* p = wide(path);
	int r = _wstat64(p, &st);
	free(p);
#else
	struct stat st;
	int r = stat(path, &st);
#endif
	if (r) return -1;
	*size = st.st_size;
	*mtime = st.st_mtime;
	return 0;
}

// the name as its folder lists it
static int h_exact(void* ctx, const char* path) {
	char dir[1200];
	const char* name = strrchr(path, '/');
	hList l;
	int i, res = 0;
	if (!name || !h_kind(ctx, path) || (name - path >= (int)sizeof(dir))) return 0;
	memcpy(dir, path, name - path);
	dir[name - path] = 0;
	hl_list(dir, &l);
	for (i = 0; i < l.n; i++)
		if (!strcmp(l.name[i], name + 1)) res = 1;
	hl_free(&l);
	return res;
}

static const vfHost host = {NULL, h_open, h_mkdir, h_rename, h_replace, h_remove, h_truncate, h_kind, h_empty, h_settime, h_stat, h_exact};

// remove a host tree for good
static void rm_tree(const char* path) {
	hList l;
	char full[1200];
	int i;
	if (h_kind(NULL, path) != 2) {
		if (h_kind(NULL, path)) rm_plain(path, 0);
		return;
	}
	hl_list(path, &l);
	for (i = 0; i < l.n; i++) {
		snprintf(full, sizeof(full), "%s/%s", path, l.name[i]);
		if (l.isdir[i]) rm_tree(full);
		else rm_plain(full, 0);
	}
	hl_free(&l);
	rm_plain(path, 1);
}

static int cmp_name(const void* a, const void* b) {
	return strcmp(*(char* const*)a, *(char* const*)b);
}

// == a volume from a host folder, as the app's scan makes it ==

static int scan_depth = 1000;		// folders this deep and deeper have their folders left out, as the app's scan
static int scan_hide = 0;		// names starting with a dot left out, as the app's scan off Windows

// the tree alone, as vfat_refresh takes it; restore: an interrupted sync's files given their
// names back, as at a mount; *bytes what the files take
static vFat* scan_folder(const char* root, int restore, unsigned long long* bytes) {
	vFat* vf = vfat_create();
	int queue[4096];
	char* qpath[4096];
	int qdepth[4096];
	int qn = 0, q, i;
	if (bytes) *bytes = 0;
	vfat_set_root(vf, root, &host);
	vfat_add(vf, -1, "", 0, 0, 1);
	queue[qn] = 0;
	qdepth[qn] = 0;
	qpath[qn++] = strdup(root);
	for (q = 0; q < qn; q++) {
		hList l;
		char full[1200];
		hl_list(qpath[q], &l);
		// dirs first, then by name, as QDir's listing
		char** dirs = (char**)malloc((l.n + 1) * sizeof(char*));
		char** files = (char**)malloc((l.n + 1) * sizeof(char*));
		int nd = 0, nf = 0;
		for (i = 0; i < l.n; i++) {
			if (!strncmp(l.name[i], VF_TMPMARK, strlen(VF_TMPMARK))) {	// as the app's scan: made visible again
				char* back = (restore && !q) ? vfat_tmp_restore(vf, l.name[i]) : NULL;
				if (!back) continue;
				free(l.name[i]);
				l.name[i] = back;
				snprintf(full, sizeof(full), "%s/%s", qpath[q], back);
				l.isdir[i] = (h_kind(NULL, full) == 2);
			}
			if (scan_hide && (l.name[i][0] == '.')) continue;
			if (l.isdir[i]) dirs[nd++] = l.name[i]; else files[nf++] = l.name[i];
		}
		qsort(dirs, nd, sizeof(char*), cmp_name);
		qsort(files, nf, sizeof(char*), cmp_name);
		for (i = 0; (i < nd) && (qdepth[q] < scan_depth); i++) {
			long long sz, mt = 1600000000;
			snprintf(full, sizeof(full), "%s/%s", qpath[q], dirs[i]);
			h_stat(NULL, full, &sz, &mt);
			int idx = vfat_add(vf, queue[q], dirs[i], 0, (unsigned int)mt, 1);
			queue[qn] = idx;
			qdepth[qn] = qdepth[q] + 1;
			qpath[qn++] = strdup(full);
		}
		for (i = 0; i < nf; i++) {
			long long sz = 0, mt = 1600000000;
			snprintf(full, sizeof(full), "%s/%s", qpath[q], files[i]);
			h_stat(NULL, full, &sz, &mt);		// 64 bits: a file can be just under 4G
			vfat_add(vf, queue[q], files[i], (unsigned int)sz, (unsigned int)mt, 0);
			if (bytes) *bytes += sz;
		}
		free(dirs);
		free(files);
		hl_free(&l);
	}
	for (q = 0; q < qn; q++) free(qpath[q]);
	return vf;
}

static vFat* mount_folder(const char* root, int writable) {
	unsigned long long bytes;
	vFat* vf = scan_folder(root, writable, &bytes);
	vfat_build(vf, writable ? (unsigned int)((bytes / 512) * 2 + 128 * 2048) : 0);
	vf->writable = writable;
	return vf;
}

// == FatFs over the volume ==

static vFat* gvf = NULL;

DSTATUS disk_status(BYTE pdrv) { return 0; }
DSTATUS disk_initialize(BYTE pdrv) { return 0; }

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
	UINT i;
	for (i = 0; i < count; i++)
		if (!vfat_read(gvf, sector + i, buff + i * 512)) return RES_ERROR;
	return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
	UINT i;
	for (i = 0; i < count; i++) {
		if (!vfat_write(gvf, sector + i, buff + i * 512)) return RES_WRPRT;
		if (torture) sync_checked(0);
	}
	return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
	switch (cmd) {
		case CTRL_SYNC: return RES_OK;
		case GET_SECTOR_COUNT: *(LBA_t*)buff = gvf->volume; return RES_OK;
		case GET_SECTOR_SIZE: *(WORD*)buff = 512; return RES_OK;
		case GET_BLOCK_SIZE: *(DWORD*)buff = 1; return RES_OK;
	}
	return RES_PARERR;
}

// == the model ==

typedef struct {
	char* path;			// from the root, '/' parted, no leading '/'
	int isdir;
	unsigned char* data;
	unsigned int size;
} mEnt;

static mEnt* model = NULL;
static int mn = 0;
static unsigned int rng;
static int fails = 0;
static char failmsg[4096];

static unsigned int rnd(unsigned int n) {
	rng = rng * 1103515245u + 12345u;
	return n ? ((rng >> 8) % n) : 0;
}

static void fail(const char* fmt, ...) {
	va_list ap;
	if (!fails) {
		va_start(ap, fmt);
		vsnprintf(failmsg, sizeof(failmsg), fmt, ap);
		va_end(ap);
	}
	fails++;
}

static int m_find(const char* path) {
	int i;
	for (i = 0; i < mn; i++) if (!strcmp(model[i].path, path)) return i;
	return -1;
}

static int m_add(const char* path, int isdir) {
	model = (mEnt*)realloc(model, (mn + 1) * sizeof(mEnt));
	memset(model + mn, 0x00, sizeof(mEnt));
	model[mn].path = strdup(path);
	model[mn].isdir = isdir;
	return mn++;
}

static void m_del(int i) {
	free(model[i].path);
	free(model[i].data);
	model[i] = model[--mn];
}

static void m_clear(void) {
	while (mn) m_del(0);
}

static int m_children(const char* dir) {
	int i, n = 0;
	size_t l = strlen(dir);
	for (i = 0; i < mn; i++) {
		if (!l) { if (!strchr(model[i].path, '/')) n++; continue; }
		if (!strncmp(model[i].path, dir, l) && (model[i].path[l] == '/') && !strchr(model[i].path + l + 1, '/')) n++;
	}
	return n;
}

static void m_pick_path(char* out, int max, const char* dir, const char* name) {
	if (dir[0]) snprintf(out, max, "%s/%s", dir, name);
	else snprintf(out, max, "%s", name);
}

static const char* words[] = {"game", "Readme", "data", "LOADER", "tap", "screen", "intro music",
	"\xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb", "\xd0\x98\xd0\xb3\xd1\x80\xd0\xb0", "Very Long Name Of A File", "x", "a.b", "save-01", "notes"};
static const char* exts[] = {"", ".tap", ".TXT", ".sna", ".bin", ".z80", ".scr"};

// a name not yet in dir, case aside (FAT's rule)
static void m_new_name(const char* dir, char* out, int max, int isdir) {
	char name[256], path[1024];
	int tries, i, clash;
	for (tries = 0; tries < 100; tries++) {
		snprintf(name, sizeof(name), "%s%u%s", words[rnd(sizeof(words) / sizeof(words[0]))], rnd(1000),
			isdir ? "" : exts[rnd(sizeof(exts) / sizeof(exts[0]))]);
		m_pick_path(path, sizeof(path), dir, name);
		clash = 0;
		for (i = 0; i < mn; i++) {
			const char* a = model[i].path;
			const char* b = path;
			for (; *a && *b; a++, b++) {
				char x = *a, y = *b;
				if (x >= 'a' && x <= 'z') x -= 32;
				if (y >= 'a' && y <= 'z') y -= 32;
				if (x != y) break;
			}
			if (!*a && !*b) clash = 1;
		}
		if (!clash) break;
	}
	snprintf(out, max, "%s", path);
}

static int m_rand_of(int wantDir) {
	int i, c = 0, pick;
	for (i = 0; i < mn; i++) if (model[i].isdir == wantDir) c++;
	if (!c) return -1;
	pick = rnd(c);
	for (i = 0; i < mn; i++) if ((model[i].isdir == wantDir) && !pick--) return i;
	return -1;
}

static const char* m_rand_dir(void) {
	int i = rnd(3) ? m_rand_of(1) : -1;
	return (i < 0) ? "" : model[i].path;
}

static void fill_rand(unsigned char* p, unsigned int n) {
	unsigned int i;
	unsigned int x = rnd(0x7fffffff) | 1;
	for (i = 0; i < n; i++) {
		x = x * 1664525u + 1013904223u;
		p[i] = x >> 24;
	}
}

// == checks ==

// what the guest reads of everything in use: the reserved sectors, both FATs, and every
// cluster the FAT holds - a sync must not move any of it
static unsigned long long view_hash(void) {
	unsigned char sec[512];
	unsigned long long h = 1469598103934665603ULL;
	unsigned int lba, s, k, c;
	unsigned int fat0 = gvf->partStart + gvf->reserved;
	unsigned int data = gvf->partStart + gvf->dataStart;
	for (lba = 0; lba < data; lba++) {
		vfat_read(gvf, lba, sec);
		for (k = 0; k < 512; k++) h = (h ^ sec[k]) * 1099511628211ULL;
	}
	for (s = 0; s < gvf->fatSize; s++) {
		unsigned char fat[512];
		vfat_read(gvf, fat0 + s, fat);
		for (k = 0; k < 128; k++) {
			c = s * 128 + k;
			if ((c < 2) || (c >= gvf->clusters + 2)) continue;
			if (!((fat[k * 4] | (fat[k * 4 + 1] << 8) | (fat[k * 4 + 2] << 16) | ((unsigned)fat[k * 4 + 3] << 24)) & 0x0fffffff)) continue;
			unsigned int j;
			for (j = 0; j < gvf->spc; j++) {
				vfat_read(gvf, data + (c - 2) * gvf->spc + j, sec);
				h = (h ^ c) * 1099511628211ULL;
				for (k = 0; k < 512; k++) h = (h ^ sec[k]) * 1099511628211ULL;
			}
		}
	}
	return h;
}

// every sector in use with its hash, for telling which one moved
typedef struct { unsigned int lba; unsigned long long h; unsigned char d[512]; } vSec;

static vSec* view_list(int* n) {
	unsigned char sec[512], fat[512];
	unsigned int s, k, c, j;
	unsigned int fat0 = gvf->partStart + gvf->reserved;
	unsigned int data = gvf->partStart + gvf->dataStart;
	vSec* v = NULL;
	int cap = 0;
	*n = 0;
	for (s = 0; s < gvf->fatSize; s++) {
		vfat_read(gvf, fat0 + s, fat);
		for (k = 0; k < 128; k++) {
			c = s * 128 + k;
			if ((c < 2) || (c >= gvf->clusters + 2)) continue;
			if (!((fat[k * 4] | (fat[k * 4 + 1] << 8) | (fat[k * 4 + 2] << 16) | ((unsigned)fat[k * 4 + 3] << 24)) & 0x0fffffff)) continue;
			for (j = 0; j < gvf->spc; j++) {
				unsigned long long h = 1469598103934665603ULL;
				unsigned int b;
				if (*n >= cap) { cap = cap ? cap * 2 : 4096; v = (vSec*)realloc(v, cap * sizeof(vSec)); }
				v[*n].lba = data + (c - 2) * gvf->spc + j;
				vfat_read(gvf, v[*n].lba, sec);
				for (b = 0; b < 512; b++) h = (h ^ sec[b]) * 1099511628211ULL;
				v[*n].h = h;
				memcpy(v[*n].d, sec, 512);
				(*n)++;
			}
		}
	}
	return v;
}

static int sync_checked(int force) {
	unsigned long long a = view_hash();
	int na = 0;
	vSec* va = getenv("VFDIAG") ? view_list(&na) : NULL;
	int r = vfat_sync(gvf, force);
	if (view_hash() != a) {
		fail("a sync changed what the guest reads");
		if (va) {
			unsigned char sec[512];
			int i;
			for (i = 0; i < na; i++) {
				vfat_read(gvf, va[i].lba, sec);
				if (memcmp(sec, va[i].d, 512)) {
					int b;
					for (b = 0; (b < 512) && (sec[b] == va[i].d[b]); b++);
					unsigned int clu = (va[i].lba - gvf->partStart - gvf->dataStart) / gvf->spc + 2;
					fprintf(stderr, "moved: lba %u cluster %u sector %u, first byte %i: was %02X now %02X\n", va[i].lba,
						clu, (va[i].lba - gvf->partStart - gvf->dataStart) % gvf->spc, b, va[i].d[b], sec[b]);
					// whose: the model's files walked cluster by cluster
					int q;
					for (q = 0; q < mn; q++) {
						char fp[1100];
						FIL f;
						unsigned int off;
						if (model[q].isdir) continue;
						fpath(fp, sizeof(fp), model[q].path);
						if (f_open(&f, fp, FA_READ) != FR_OK) continue;
						for (off = 0; off < model[q].size; off += 4096) {
							f_lseek(&f, off + 1);
							if (f.clust == clu) fprintf(stderr, "   in %s, size %u, cluster at offset %u\n", model[q].path, model[q].size, off);
						}
						f_close(&f);
					}
					break;
				}
			}
		}
	}
	free(va);
	return r;
}

static void fpath(char* out, int max, const char* rel) {
	snprintf(out, max, "/%s", rel);
}

// the guest reads the model back
static void check_guest(const char* when) {
	int i;
	for (i = 0; i < mn; i++) {
		char p[1100];
		FILINFO fi;
		fpath(p, sizeof(p), model[i].path);
		if (f_stat(p, &fi) != FR_OK) { fail("%s: guest lost %s", when, model[i].path); continue; }
		if (((fi.fattrib & AM_DIR) ? 1 : 0) != model[i].isdir) { fail("%s: guest kind of %s", when, model[i].path); continue; }
		if (model[i].isdir) continue;
		if (fi.fsize != model[i].size) { fail("%s: guest size of %s %u, model %u", when, model[i].path, (unsigned)fi.fsize, model[i].size); continue; }
		FIL f;
		UINT got;
		unsigned char* buf = (unsigned char*)malloc(model[i].size + 1);
		if (f_open(&f, p, FA_READ) != FR_OK) { fail("%s: guest can't open %s", when, model[i].path); free(buf); continue; }
		f_read(&f, buf, model[i].size, &got);
		f_close(&f);
		if ((got != model[i].size) || memcmp(buf, model[i].data, model[i].size)) fail("%s: guest content of %s", when, model[i].path);
		free(buf);
	}
}

// the host folder holds the model and nothing else
static int host_walk(const char* root, const char* rel, int* count, const char* when) {
	hList l;
	char full[1200], r2[1100];
	int i;
	if (rel[0]) snprintf(full, sizeof(full), "%s/%s", root, rel);
	else snprintf(full, sizeof(full), "%s", root);
	hl_list(full, &l);
	for (i = 0; i < l.n; i++) {
		m_pick_path(r2, sizeof(r2), rel, l.name[i]);
		int m = m_find(r2);
		if (m < 0) { fail("%s: host has %s, the model not", when, r2); continue; }
		(*count)++;
		if (model[m].isdir != l.isdir[i]) { fail("%s: host kind of %s", when, r2); continue; }
		if (l.isdir[i]) {
			host_walk(root, r2, count, when);
		} else {
			char hp[1300];
			FILE* f;
			snprintf(hp, sizeof(hp), "%s/%s", root, r2);
			f = h_open(NULL, hp, "rb");
			if (!f) { fail("%s: host can't read %s", when, r2); continue; }
			unsigned char* buf = (unsigned char*)malloc(model[m].size + 2);
			size_t got = fread(buf, 1, model[m].size + 1, f);
			fclose(f);
			if ((got != model[m].size) || memcmp(buf, model[m].data, model[m].size))
				fail("%s: host content of %s (%u bytes, model %u)", when, r2, (unsigned)got, model[m].size);
			free(buf);
		}
	}
	hl_free(&l);
	return 1;
}

static void check_host(const char* root, const char* when) {
	int count = 0;
	host_walk(root, "", &count, when);
	if (count != mn) fail("%s: host has %i entries, the model %i", when, count, mn);
}

// == operations ==

static FATFS fs;
static int ops_done[16];

static int op_create(void) {
	char p[1024], fp[1100];
	FIL f;
	UINT w;
	unsigned int size = rnd(4) ? rnd(9000) : rnd(300000);
	m_new_name(m_rand_dir(), p, sizeof(p), 0);
	fpath(fp, sizeof(fp), p);
	if (f_open(&f, fp, FA_CREATE_NEW | FA_WRITE) != FR_OK) { fail("create %s", p); return 0; }
	int i = m_add(p, 0);
	model[i].data = (unsigned char*)malloc(size + 1);
	model[i].size = size;
	fill_rand(model[i].data, size);
	// in pieces, as a program writes
	unsigned int off = 0;
	while (off < size) {
		unsigned int n = 1 + rnd(20000);
		if (n > size - off) n = size - off;
		if ((f_write(&f, model[i].data + off, n, &w) != FR_OK) || (w != n)) { fail("write %s", p); break; }
		off += n;
	}
	f_close(&f);
	return 1;
}

static int op_append(void) {
	int i = m_rand_of(0);
	char fp[1100];
	FIL f;
	UINT w;
	if (i < 0) return 0;
	unsigned int add = rnd(3) ? rnd(5000) : rnd(100000);
	fpath(fp, sizeof(fp), model[i].path);
	if (f_open(&f, fp, FA_OPEN_APPEND | FA_WRITE) != FR_OK) { fail("append open %s", model[i].path); return 0; }
	model[i].data = (unsigned char*)realloc(model[i].data, model[i].size + add + 1);
	fill_rand(model[i].data + model[i].size, add);
	if ((f_write(&f, model[i].data + model[i].size, add, &w) != FR_OK) || (w != add)) fail("append %s", model[i].path);
	model[i].size += add;
	f_close(&f);
	return 1;
}

static int op_patch(void) {
	int i = m_rand_of(0);
	char fp[1100];
	FIL f;
	UINT w;
	if ((i < 0) || !model[i].size) return 0;
	unsigned int off = rnd(model[i].size);
	unsigned int n = 1 + rnd(model[i].size - off);
	if (n > 3000) n = 1 + rnd(3000);
	fpath(fp, sizeof(fp), model[i].path);
	{ FRESULT fr = f_open(&f, fp, FA_OPEN_EXISTING | FA_WRITE); if (fr != FR_OK) { fail("patch open %s: %i", model[i].path, fr); return 0; } }
	fill_rand(model[i].data + off, n);
	f_lseek(&f, off);
	if ((f_write(&f, model[i].data + off, n, &w) != FR_OK) || (w != n)) fail("patch %s", model[i].path);
	f_close(&f);
	return 1;
}

static int op_cut(void) {
	int i = m_rand_of(0);
	char fp[1100];
	FIL f;
	if ((i < 0) || !model[i].size) return 0;
	unsigned int size = rnd(model[i].size);
	fpath(fp, sizeof(fp), model[i].path);
	if (f_open(&f, fp, FA_OPEN_EXISTING | FA_WRITE) != FR_OK) { fail("cut open %s", model[i].path); return 0; }
	f_lseek(&f, size);
	if (f_truncate(&f) != FR_OK) fail("cut %s", model[i].path);
	model[i].size = size;
	f_close(&f);
	return 1;
}

// rename or move an entry, a folder with all under it
static int op_move(int wantDir) {
	int i = m_rand_of(wantDir);
	char np[1024], a[1100], b[1100];
	const char* dir;
	int j;
	if (i < 0) return 0;
	dir = rnd(2) ? m_rand_dir() : NULL;
	if (dir) {
		size_t l = strlen(model[i].path);
		if (!strncmp(dir, model[i].path, l) && ((dir[l] == '/') || !dir[l])) dir = NULL;	// not into itself
	}
	if (!dir) {
		// same folder, new name
		static char par[1024];
		const char* s = strrchr(model[i].path, '/');
		if (s) { snprintf(par, sizeof(par), "%.*s", (int)(s - model[i].path), model[i].path); dir = par; }
		else dir = "";
	}
	m_new_name(dir, np, sizeof(np), wantDir);
	fpath(a, sizeof(a), model[i].path);
	fpath(b, sizeof(b), np);
	if (f_rename(a, b) != FR_OK) { fail("rename %s -> %s", model[i].path, np); return 0; }
	char* old = strdup(model[i].path);
	size_t l = strlen(old);
	for (j = 0; j < mn; j++) {
		if (!strcmp(model[j].path, old) || (!strncmp(model[j].path, old, l) && model[j].path[l] == '/')) {
			char tmp[2048];
			snprintf(tmp, sizeof(tmp), "%s%s", np, model[j].path + l);
			free(model[j].path);
			model[j].path = strdup(tmp);
		}
	}
	free(old);
	return 1;
}

static int op_delete(void) {
	int i = m_rand_of(0);
	char fp[1100];
	if (i < 0) return 0;
	fpath(fp, sizeof(fp), model[i].path);
	if (f_unlink(fp) != FR_OK) { fail("delete %s", model[i].path); return 0; }
	m_del(i);
	return 1;
}

static int op_mkdir(void) {
	char p[1024], fp[1100];
	m_new_name(m_rand_dir(), p, sizeof(p), 1);
	fpath(fp, sizeof(fp), p);
	if (f_mkdir(fp) != FR_OK) { fail("mkdir %s", p); return 0; }
	m_add(p, 1);
	return 1;
}

// a folder and all in it, deepest first, as a guest's delete-tree does
static int op_rmtree(void) {
	int i = m_rand_of(1);
	char fp[1100];
	if (i < 0) return 0;
	char* top = strdup(model[i].path);
	size_t l = strlen(top);
	int again = 1;
	while (again) {
		again = 0;
		int j;
		for (j = 0; j < mn; j++) {
			if (strncmp(model[j].path, top, l) || (model[j].path[l] != '/' && model[j].path[l])) continue;
			if (model[j].isdir && m_children(model[j].path)) continue;
			fpath(fp, sizeof(fp), model[j].path);
			if (f_unlink(fp) != FR_OK) { fail("rmtree %s", model[j].path); free(top); return 0; }
			m_del(j);
			again = 1;
			break;
		}
	}
	free(top);
	return 1;
}

// == runs ==

static void build_tree(const char* root) {
	static const char* dirs[] = {"games", "games/arcade", "docs", "\xd0\x9f\xd1\x80\xd0\xbe\xd0\xb3\xd1\x80\xd0\xb0\xd0\xbc\xd0\xbc\xd1\x8b", "empty"};
	static const char* files[] = {"README.TXT", "games/manic.tap", "games/arcade/Jet Set Willy.tap",
		"docs/manual of everything.txt", "\xd0\x9f\xd1\x80\xd0\xbe\xd0\xb3\xd1\x80\xd0\xb0\xd0\xbc\xd0\xbc\xd1\x8b/\xd0\xb8\xd0\xb3\xd1\x80\xd0\xb0.tap", "zero.bin", "games/boot.sna"};
	char full[1200];
	unsigned int i;
	rm_tree(root);
	h_mkdir(NULL, root);
	m_clear();
	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		snprintf(full, sizeof(full), "%s/%s", root, dirs[i]);
		h_mkdir(NULL, full);
		m_add(dirs[i], 1);
	}
	for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		int m = m_add(files[i], 0);
		unsigned int size = (i == 5) ? 0 : 100 + rnd(50000);
		model[m].data = (unsigned char*)malloc(size + 1);
		model[m].size = size;
		fill_rand(model[m].data, size);
		snprintf(full, sizeof(full), "%s/%s", root, files[i]);
		FILE* f = h_open(NULL, full, "wb");
		fwrite(model[m].data, 1, size, f);
		fclose(f);
	}
	host_ops = 0;
}

static void remount_check(const char* root, const char* when) {
	f_unmount("");
	vfat_free(gvf);
	gvf = mount_folder(root, 1);
	if (f_mount(&fs, "", 1) != FR_OK) { fail("%s: remount", when); return; }
	check_guest(when);
}

static int run_random(const char* root, unsigned int seed, int ops) {
	char when[64];
	int k;
	rng = seed;
	fails = 0;
	memset(ops_done, 0, sizeof(ops_done));
	build_tree(root);
	gvf = mount_folder(root, 1);
	if (f_mount(&fs, "", 1) != FR_OK) { fail("mount"); return 0; }
	for (k = 0; (k < ops) && !fails; k++) {
		int r = rnd(100), op;
		if (r < 22) op = 0;
		else if (r < 34) op = 1;
		else if (r < 46) op = 2;
		else if (r < 54) op = 3;
		else if (r < 66) op = 4;
		else if (r < 72) op = 5;
		else if (r < 82) op = 6;
		else if (r < 90) op = 7;
		else op = 8;
		int done = 0;
		switch (op) {
			case 0: done = op_create(); break;
			case 1: done = op_append(); break;
			case 2: done = op_patch(); break;
			case 3: done = op_cut(); break;
			case 4: done = op_move(0); break;
			case 5: done = op_move(1); break;
			case 6: done = op_delete(); break;
			case 7: done = op_mkdir(); break;
			case 8: done = (rnd(4) == 0) ? op_rmtree() : op_mkdir(); break;
		}
		ops_done[op] += done;
		// a sync now and then, as quiet spells come: several operations in one
		if (rnd(4) == 0) {
			snprintf(when, sizeof(when), "seed %u op %i", seed, k);
			if (!sync_checked(0)) fail("%s: sync refused, detached %i pending %i", when, vfat_detached(gvf), vfat_pending(gvf));
			check_host(root, when);
			check_guest(when);
			if (vfat_detached(gvf)) fail("%s: detached", when);
		}
	}
	snprintf(when, sizeof(when), "seed %u end", seed);
	sync_checked(0);
	check_host(root, when);
	check_guest(when);
	snprintf(when, sizeof(when), "seed %u remount", seed);
	remount_check(root, when);
	check_host(root, when);
	f_unmount("");
	vfat_free(gvf);
	gvf = NULL;
	return !fails;
}

// == scenarios: the cases a random run is unlikely to hit ==

static char sroot[1024];

// a scenario's volume; a second one in the same scenario keeps what the first part failed
static void s_begin(unsigned int seed, int writable) {
	rng = seed;
	build_tree(sroot);
	gvf = mount_folder(sroot, writable);
	if (f_mount(&fs, "", 1) != FR_OK) fail("mount");
}

static void s_end(void) {
	f_unmount("");
	vfat_free(gvf);
	gvf = NULL;
}

// a file of size bytes written through the guest, into the model
static void s_write(const char* path, unsigned int size) {
	char fp[1100];
	FIL f;
	UINT w;
	int i = m_find(path);
	if (i < 0) i = m_add(path, 0);
	free(model[i].data);
	model[i].data = (unsigned char*)malloc(size + 1);
	model[i].size = size;
	fill_rand(model[i].data, size);
	fpath(fp, sizeof(fp), path);
	if (f_open(&f, fp, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) { fail("s_write open %s", path); return; }
	if ((f_write(&f, model[i].data, size, &w) != FR_OK) || (w != size)) fail("s_write %s", path);
	f_close(&f);
}

static void s_ren(const char* a, const char* b) {
	char fa[1100], fb[1100];
	int j;
	size_t l = strlen(a);
	fpath(fa, sizeof(fa), a);
	fpath(fb, sizeof(fb), b);
	if (f_rename(fa, fb) != FR_OK) { fail("s_ren %s %s", a, b); return; }
	for (j = 0; j < mn; j++) {
		if (!strcmp(model[j].path, a) || (!strncmp(model[j].path, a, l) && model[j].path[l] == '/')) {
			char tmp[2048];
			snprintf(tmp, sizeof(tmp), "%s%s", b, model[j].path + l);
			free(model[j].path);
			model[j].path = strdup(tmp);
		}
	}
}

static void s_del(const char* path) {
	char fp[1100];
	fpath(fp, sizeof(fp), path);
	if (f_unlink(fp) != FR_OK) { fail("s_del %s", path); return; }
	m_del(m_find(path));
}

static void s_mkdir(const char* path) {
	char fp[1100];
	fpath(fp, sizeof(fp), path);
	if (f_mkdir(fp) != FR_OK) { fail("s_mkdir %s", path); return; }
	m_add(path, 1);
}

static void s_sync(const char* when) {
	if (!sync_checked(0)) fail("%s: sync refused, detached %i", when, vfat_detached(gvf));
	check_host(sroot, when);
	check_guest(when);
}

static void s_remount(const char* when) {
	remount_check(sroot, when);
	check_host(sroot, when);
}

static int file_eq(const char* path, const unsigned char* data, unsigned int size) {
	FILE* f = h_open(NULL, path, "rb");
	unsigned char* buf;
	size_t got;
	int ok;
	if (!f) return 0;
	buf = (unsigned char*)malloc(size + 2);
	got = fread(buf, 1, size + 1, f);
	fclose(f);
	ok = (got == size) && !memcmp(buf, data, size);
	free(buf);
	return ok;
}

static void sc_swap(void) {
	s_begin(101, 1);
	s_ren("games/manic.tap", "games/t.tmp");
	s_ren("games/boot.sna", "games/manic.tap");
	s_ren("games/t.tmp", "games/boot.sna");
	s_sync("swap");
	s_remount("swap remount");
	s_end();
}

static void sc_replace(void) {
	s_begin(102, 1);
	s_del("README.TXT");
	s_ren("zero.bin", "README.TXT");
	s_write("docs/new.txt", 5000);
	s_del("docs/manual of everything.txt");
	s_ren("docs/new.txt", "docs/manual of everything.txt");
	s_sync("replace");
	s_remount("replace remount");
	s_end();
}

// many small files, every other one deleted, then a file across the holes
static void sc_fragment(void) {
	char p[64];
	int i;
	s_begin(103, 1);
	s_mkdir("frag");
	for (i = 0; i < 60; i++) {
		snprintf(p, sizeof(p), "frag/f%02i.bin", i);
		s_write(p, 4096 + rnd(4000));
	}
	s_sync("fragment fill");
	for (i = 0; i < 60; i += 2) {
		snprintf(p, sizeof(p), "frag/f%02i.bin", i);
		s_del(p);
	}
	s_write("frag/big.bin", 200000);
	s_sync("fragment big");
	// patched in place, then grown across more holes
	{
		char fp[1100];
		FIL f;
		UINT w;
		int m = m_find("frag/big.bin");
		fpath(fp, sizeof(fp), "frag/big.bin");
		f_open(&f, fp, FA_OPEN_EXISTING | FA_WRITE);
		fill_rand(model[m].data + 70000, 9000);
		f_lseek(&f, 70000);
		f_write(&f, model[m].data + 70000, 9000, &w);
		f_close(&f);
	}
	s_sync("fragment patch");
	for (i = 1; i < 60; i += 4) {
		snprintf(p, sizeof(p), "frag/f%02i.bin", i);
		s_del(p);
	}
	{
		char fp[1100];
		FIL f;
		UINT w;
		int m = m_find("frag/big.bin");
		fpath(fp, sizeof(fp), "frag/big.bin");
		model[m].data = (unsigned char*)realloc(model[m].data, model[m].size + 50000);
		fill_rand(model[m].data + model[m].size, 50000);
		f_open(&f, fp, FA_OPEN_APPEND | FA_WRITE);
		f_write(&f, model[m].data + model[m].size, 50000, &w);
		f_close(&f);
		model[m].size += 50000;
	}
	s_sync("fragment grow");
	s_remount("fragment remount");
	s_end();
}

// a folder of hundreds of entries: its directory spans clusters, then shrinks
static void sc_bigdir(void) {
	char p[128];
	int i;
	s_begin(104, 1);
	s_mkdir("many");
	for (i = 0; i < 400; i++) {
		snprintf(p, sizeof(p), "many/a rather long file name number %03i.dat", i);
		s_write(p, rnd(700));
	}
	s_sync("bigdir fill");
	for (i = 0; i < 400; i += 3) {
		snprintf(p, sizeof(p), "many/a rather long file name number %03i.dat", i);
		s_del(p);
	}
	s_sync("bigdir thin");
	s_remount("bigdir remount");
	s_end();
}

// nested: a folder moves while files inside it are renamed, in one sync
static void sc_nested(void) {
	s_begin(105, 1);
	s_ren("games/arcade/Jet Set Willy.tap", "games/arcade/JSW.tap");
	s_ren("games/arcade", "docs/arcade games");
	s_ren("games", "old games");
	s_write("old games/added.tap", 3000);
	s_sync("nested");
	s_remount("nested remount");
	s_end();
}

// a format of the volume: the folder stops following, nothing on the host changes
static void sc_format(void) {
	static BYTE work[4096];
	MKFS_PARM opt = {FM_FAT32, 1, 0, 0, 0};
	s_begin(106, 1);
	f_unmount("");
	if (f_mkfs("", &opt, work, sizeof(work)) != FR_OK) fail("mkfs");
	if (!vfat_detached(gvf)) fail("format: not detached");
	f_mount(&fs, "", 1);
	{
		char fp[1100];
		FIL f;
		UINT w;
		fpath(fp, sizeof(fp), "after.txt");
		f_open(&f, fp, FA_CREATE_NEW | FA_WRITE);
		f_write(&f, "x", 1, &w);
		f_close(&f);
	}
	vfat_sync(gvf, 0);
	check_host(sroot, "format");		// the model is the tree as built
	{
		const char* why = NULL;
		if ((vfat_news(gvf, &why, NULL) != 1) || !why) fail("format: not told");
		if (vfat_news(gvf, &why, NULL)) fail("format: told twice");
	}
	s_end();
	check_host(sroot, "format unmount");
}

// a file left open half written: the sync takes what the guest committed; the rest
// comes with the close
static void sc_open(void) {
	char fp[1100];
	FIL f;
	UINT w;
	int m;
	s_begin(107, 1);
	m = m_add("open.bin", 0);
	model[m].data = (unsigned char*)malloc(100001);
	model[m].size = 0;
	fill_rand(model[m].data, 100000);
	fpath(fp, sizeof(fp), "open.bin");
	f_open(&f, fp, FA_CREATE_NEW | FA_WRITE);
	f_sync(&f);
	f_write(&f, model[m].data, 60000, &w);
	vfat_sync(gvf, 0);
	check_host(sroot, "open half");		// still empty for the folder: the entry says 0
	f_write(&f, model[m].data + 60000, 40000, &w);
	f_close(&f);
	model[m].size = 100000;
	s_sync("open closed");
	// written, never closed, and the folder taken away: what the entry says is what stays
	m = m_add("open2.bin", 0);
	model[m].data = (unsigned char*)malloc(50001);
	fill_rand(model[m].data, 50000);
	model[m].size = 0;
	fpath(fp, sizeof(fp), "open2.bin");
	f_open(&f, fp, FA_CREATE_NEW | FA_WRITE);
	f_sync(&f);
	f_write(&f, model[m].data, 50000, &w);
	f_unmount("");
	vfat_free(gvf);
	gvf = NULL;
	check_host(sroot, "open unmounted");
	gvf = mount_folder(sroot, 1);
	f_mount(&fs, "", 1);
	check_guest("open remount");
	s_end();
}

// names Windows refuses get a '_' on the host; the guest keeps its own until a remount
static void sc_names(void) {
	char full[1200];
	s_begin(108, 1);
	s_write("CON.txt", 100);
	s_write("lpt1", 50);
	s_write("Aux.Bin", 10);
	vfat_sync(gvf, 0);
	snprintf(full, sizeof(full), "%s/_CON.txt", sroot);
	if (!file_eq(full, model[m_find("CON.txt")].data, 100)) fail("names: _CON.txt");
	snprintf(full, sizeof(full), "%s/_lpt1", sroot);
	if (!file_eq(full, model[m_find("lpt1")].data, 50)) fail("names: _lpt1");
	snprintf(full, sizeof(full), "%s/_Aux.Bin", sroot);
	if (!file_eq(full, model[m_find("Aux.Bin")].data, 10)) fail("names: _Aux.Bin");
	check_guest("names");
	s_end();
}

// what is deleted lands in the trash whole
static void sc_trash(void) {
	char full[1200];
	int m, n0 = trash_n;
	unsigned char* keep;
	unsigned int size;
	s_begin(109, 1);
	m = m_find("games/arcade/Jet Set Willy.tap");
	size = model[m].size;
	keep = (unsigned char*)malloc(size + 1);
	memcpy(keep, model[m].data, size);
	s_del("games/arcade/Jet Set Willy.tap");
	s_sync("trash");
	snprintf(full, sizeof(full), "%s/%i", trash_dir, n0);
	if ((trash_n != n0 + 1) || !file_eq(full, keep, size)) fail("trash: not there whole");
	free(keep);
	s_end();
}

// a file the volume does not know of, inside a folder the guest removes: the folder stays
static void sc_unknown(void) {
	char full[1200];
	FILE* f;
	s_begin(110, 1);
	snprintf(full, sizeof(full), "%s/empty/late.txt", sroot);
	f = h_open(NULL, full, "wb");
	fputs("added on the host after the mount", f);
	fclose(f);
	{
		char fp[1100];
		fpath(fp, sizeof(fp), "empty");
		if (f_unlink(fp) != FR_OK) fail("unknown: unlink");
		m_del(m_find("empty"));
	}
	vfat_sync(gvf, 0);
	if (h_kind(NULL, full) != 1) fail("unknown: the host's file went");
	check_guest("unknown");
	s_end();
}

// read only: the guest's writes are refused and the host stays
static void sc_readonly(void) {
	char fp[1100];
	FIL f;
	s_begin(111, 0);
	fpath(fp, sizeof(fp), "new.txt");
	// FatFs writes the entry lazily: the refusal comes with the close
	if ((f_open(&f, fp, FA_CREATE_NEW | FA_WRITE) == FR_OK) && (f_close(&f) == FR_OK)) fail("readonly: created");
	check_host(sroot, "readonly");
	s_end();
}

// host operations refused on some paths: the guest still reads what it wrote, the
// rest goes through, and the refused part follows once the host lets it
static void sc_hostfail(void) {
	char fp[1100];
	FIL f;
	UINT w;
	int m;
	s_begin(112, 1);
	m = m_find("games/manic.tap");
	if (model[m].size < 2100) {		// the patch grows it
		model[m].data = (unsigned char*)realloc(model[m].data, 2101);
		model[m].size = 2100;
	}
	fpath(fp, sizeof(fp), "games/manic.tap");
	f_open(&f, fp, FA_OPEN_EXISTING | FA_WRITE);
	fill_rand(model[m].data + 100, 2000);
	f_lseek(&f, 100);
	f_write(&f, model[m].data + 100, 2000, &w);
	f_close(&f);
	s_write("lockme.bin", 30000);
	s_write("docs/fine.txt", 3000);
	s_ren("games/boot.sna", "games/manic2.sna");
	fail_pat = "manic";
	vfat_sync(gvf, 0);
	fail_pat = NULL;
	if (!vfat_pending(gvf)) fail("hostfail: nothing pending");
	{
		int errors = 0;
		if ((vfat_news(gvf, NULL, &errors) != 2) || !errors) fail("hostfail: not told");
		if (vfat_news(gvf, NULL, &errors)) fail("hostfail: told twice");
	}
	check_guest("hostfail refused");
	{
		char full[1200];
		snprintf(full, sizeof(full), "%s/docs/fine.txt", sroot);
		if (!file_eq(full, model[m_find("docs/fine.txt")].data, 3000)) fail("hostfail: the rest did not go through");
	}
	fail_pat = "lockme";
	vfat_sync(gvf, 0);
	fail_pat = NULL;
	check_guest("hostfail refused 2");
	vfat_sync(gvf, 0);
	vfat_sync(gvf, 0);
	if (vfat_pending(gvf)) fail("hostfail: still pending %i", vfat_pending(gvf));
	check_host(sroot, "hostfail after");
	check_guest("hostfail after");
	s_remount("hostfail remount");
	s_end();
}

// the root entry of an 8.3 name, through the volume itself: 0 if not found
static unsigned int root_entry(const char* n83, int* pos, unsigned char* sec) {
	unsigned int s, lba;
	int i;
	for (s = 0; s < gvf->spc; s++) {
		lba = gvf->partStart + gvf->dataStart + s;
		vfat_read(gvf, lba, sec);
		for (i = 0; i < 512; i += 32) {
			if (!memcmp(sec + i, n83, 11)) {
				*pos = i;
				return lba;
			}
		}
	}
	return 0;
}

// a move cut in two: the old entry gone, nothing new yet, the chain still allocated.
// The file stays on the host; put back, it is what it was.
static void sc_halfmove(void) {
	unsigned char sec[512];
	unsigned char keep;
	int pos;
	unsigned int lba;
	s_begin(113, 1);
	s_write("docs/touch.txt", 10);		// the volume takes writes from here on
	vfat_sync(gvf, 0);
	f_unmount("");
	lba = root_entry("README  TXT", &pos, sec);
	if (!lba) { fail("halfmove: no entry"); s_end(); return; }
	keep = sec[pos];
	sec[pos] = 0xe5;
	vfat_write(gvf, lba, sec);
	vfat_sync(gvf, 0);
	{
		char full[1200];
		int m = m_find("README.TXT");
		snprintf(full, sizeof(full), "%s/README.TXT", sroot);
		if (!file_eq(full, model[m].data, model[m].size)) fail("halfmove: the host file went");
	}
	sec[pos] = keep;
	vfat_write(gvf, lba, sec);
	vfat_sync(gvf, 0);
	f_mount(&fs, "", 1);
	check_host(sroot, "halfmove");
	check_guest("halfmove");
	s_remount("halfmove remount");
	s_end();
}

// a file's entry made a folder's by hand: the file goes, a folder comes, and what the
// guest reads in that cluster stays what it was
static void sc_retype(void) {
	unsigned char sec[512];
	int pos, m;
	unsigned int lba;
	s_begin(114, 1);
	m = m_add("RETYPE.BIN", 0);
	model[m].size = 4096;
	model[m].data = (unsigned char*)malloc(4097);
	fill_rand(model[m].data, 4096);
	model[m].data[0] = 0;			// read as a folder: empty, and the rest is not
	{
		char fp[1100];
		FIL f;
		UINT w;
		fpath(fp, sizeof(fp), "RETYPE.BIN");
		f_open(&f, fp, FA_CREATE_NEW | FA_WRITE);
		f_write(&f, model[m].data, 4096, &w);
		f_close(&f);
	}
	sync_checked(0);
	f_unmount("");
	lba = root_entry("RETYPE  BIN", &pos, sec);
	if (!lba) { fail("retype: no entry"); s_end(); return; }
	sec[pos + 11] = 0x10;
	sec[pos + 28] = sec[pos + 29] = sec[pos + 30] = sec[pos + 31] = 0;
	vfat_write(gvf, lba, sec);
	sync_checked(0);
	{
		char full[1200];
		snprintf(full, sizeof(full), "%s/RETYPE.BIN", sroot);
		if (h_kind(NULL, full) != 2) fail("retype: no folder on the host");
	}
	s_end();
}

// on hold: the guest's writes stay in memory until the user's word. Allowed, the
// folder follows; refused, it never does; unmounted first, it keeps what it had.
static void sc_hold(void) {
	char held[1200], readme[1200];
	int pass, t, bad = 0;
	snprintf(held, sizeof(held), "%s/held.bin", sroot);
	snprintf(readme, sizeof(readme), "%s/README.TXT", sroot);
	for (pass = 0; pass < 3; pass++) {
		s_begin(113 + pass, 1);
		gvf->hold = 1;
		s_write("held.bin", 5000);
		s_del("README.TXT");
		for (t = 0; t < 100; t++) vfat_tick(gvf);
		if (!vfat_waiting(gvf)) fail("hold: not waiting");
		if (h_kind(NULL, held) || (h_kind(NULL, readme) != 1)) fail("hold: the folder changed before the word");
		check_guest("hold");
		if (pass == 0) {
			vfat_release(gvf, 1);
			for (t = 0; t < 100; t++) vfat_tick(gvf);
			if (vfat_waiting(gvf) || vfat_pending(gvf)) fail("hold: still waiting after a yes");
			check_host(sroot, "hold allowed");
		} else if (pass == 1) {
			vfat_release(gvf, 0);
			s_write("later.bin", 3000);
			for (t = 0; t < 100; t++) vfat_tick(gvf);
			vfat_sync(gvf, 0);
			if (!vfat_detached(gvf)) fail("hold: a no did not detach");
			if (h_kind(NULL, held) || (h_kind(NULL, readme) != 1)) fail("hold: the folder changed after a no");
			check_guest("hold refused");
		} else {
			s_end();
			if (h_kind(NULL, held) || (h_kind(NULL, readme) != 1)) fail("hold: the unmount wrote it");
		}
		if (gvf) s_end();
		bad += fails;
	}
	fails = bad;
}

// a pinned folder (the Next's .nex overlay): what the guest does there stays in the
// volume and is told once; the rest of the volume follows as ever
static void sc_pin(void) {
	char games[1200], full[1200];
	int pass, i, t, gi, bad = 0;
	snprintf(games, sizeof(games), "%s/games", sroot);
	for (pass = 0; pass < 2; pass++) {
		s_begin(116 + pass, 1);
		gi = -1;
		for (i = 1; i < gvf->nodes; i++)
			if ((gvf->node[i].parent == 0) && !strcmp(gvf->node[i].name, "games")) gi = i;
		if (gi < 0) {
			fail("pin: no games folder");
		} else {
			vfat_pin(gvf, gi, games);
			s_write("docs/fine.txt", 3000);
			for (t = 0; t < 100; t++) vfat_tick(gvf);
			if (vfat_pin_news(gvf)) fail("pin: told with nothing changed there");
			snprintf(full, sizeof(full), "%s/docs/fine.txt", sroot);
			if (!file_eq(full, model[m_find("docs/fine.txt")].data, 3000)) fail("pin: the rest did not follow");
			if (pass == 0) {
				s_write("games/save.dat", 2000);
				snprintf(full, sizeof(full), "%s/save.dat", games);
			} else {
				s_del("games/manic.tap");
				snprintf(full, sizeof(full), "%s/manic.tap", games);
			}
			for (t = 0; t < 100; t++) vfat_tick(gvf);
			if (h_kind(NULL, full) != pass) fail("pin: the host's folder changed");
			if (!vfat_pin_news(gvf)) fail("pin: not told");
			if (vfat_pin_news(gvf)) fail("pin: told twice");
			check_guest("pin");
		}
		s_end();
		bad += fails;
	}
	fails = bad;
}

// == the review's cases ==

// a host name in the folder's top level holding our temporary mark
static int tmp_left(void) {
	hList l;
	int i, n = 0;
	hl_list(sroot, &l);
	for (i = 0; i < l.n; i++)
		if (!strncmp(l.name[i], VF_TMPMARK, strlen(VF_TMPMARK))) n++;
	hl_free(&l);
	return n;
}

// a host file of the folder holds this model entry's content
static int host_has(const char* rel, const char* model_path) {
	char full[1200];
	int m = m_find(model_path);
	snprintf(full, sizeof(full), "%s/%s", sroot, rel);
	return (m >= 0) && file_eq(full, model[m].data, model[m].size);
}

static unsigned int first_cluster(const char* path) {
	char fp[1100];
	FIL f;
	unsigned int c = 0;
	fpath(fp, sizeof(fp), path);
	if (f_open(&f, fp, FA_READ) == FR_OK) {
		c = f.obj.sclust;
		f_close(&f);
	}
	return c;
}

// moved into a folder made in the same spell: matched where it went, so a folder that
// cannot be made leaves the entries in sight - and a delete that waits on content put over
// its clusters waits until that content is somewhere
static void sc_newdir(void) {
	char p[64], q[64];
	int i;
	unsigned int clu;
	s_begin(115, 1);
	trash_on = 0;
	s_mkdir("box");
	s_ren("games/manic.tap", "box/manic.tap");
	s_ren("docs", "box/docs");
	fail_pat = "box";
	vfat_sync(gvf, 0);
	fail_pat = NULL;
	if (!host_has("manic.tap", "box/manic.tap")) fail("newdir: the moved file is not in sight");
	if (!host_has("docs/manual of everything.txt", "box/docs/manual of everything.txt")) fail("newdir: the moved folder is not in sight");
	if (tmp_left()) fail("newdir: a temporary name left");
	check_guest("newdir refused");
	s_sync("newdir");
	// many at once into a new folder is a move, not a delete
	for (i = 0; i < 40; i++) {
		snprintf(p, sizeof(p), "f%02i.bin", i);
		s_write(p, 300 + rnd(3000));
	}
	s_sync("newdir fill");
	s_mkdir("bulk");
	for (i = 0; i < 40; i++) {
		snprintf(p, sizeof(p), "f%02i.bin", i);
		snprintf(q, sizeof(q), "bulk/f%02i.bin", i);
		s_ren(p, q);
	}
	s_sync("newdir bulk");
	// deleted, and a new file in a new folder put over its clusters; the folder refused
	s_write("keep.bin", 20000);
	s_sync("newdir keep");
	clu = first_cluster("keep.bin");
	s_del("keep.bin");
	s_mkdir("nd");
	fs.last_clst = clu - 1;
	s_write("nd/new.bin", 20000);
	if (first_cluster("nd/new.bin") != clu) fail("newdir: setup, not on the freed clusters");
	fail_pat = "nd";
	vfat_sync(gvf, 0);
	fail_pat = NULL;
	if (!host_has("new (recovered).bin", "nd/new.bin")) fail("newdir: the new content went");
	if (tmp_left()) fail("newdir: a temporary name left");
	check_guest("newdir claim refused");
	s_sync("newdir claim");
	s_remount("newdir remount");
	trash_on = 1;
	s_end();
}

// what the guest wrote reaches the folder before a remount reads it, as the app does it
static void sc_flush(void) {
	vFat* nv;
	s_begin(116, 1);
	s_write("late.bin", 7000);
	s_ren("games/boot.sna", "games/later.sna");
	f_unmount("");
	vfat_flush(gvf);
	nv = mount_folder(sroot, 1);
	vfat_free(gvf);
	gvf = nv;
	if (f_mount(&fs, "", 1) != FR_OK) fail("flush: remount");
	check_guest("flush");
	check_host(sroot, "flush");
	s_end();
}

// a host file just under 4G: no 32-bit offset may wrap
static void sc_bigfile(void) {
	char full[1200], fp[1100];
	unsigned char head[1000], back[1000];
	long long sz = 0, mt;
	FIL f;
	UINT w;
	FILE* h;
	const long long big = 0xfffffe01LL;
	rng = 117;
	fails = 0;
	build_tree(sroot);
	snprintf(full, sizeof(full), "%s/big.bin", sroot);
#ifdef _WIN32
	{
		wchar_t* wp = wide(full);
		HANDLE fh = CreateFileW(wp, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD got;
		LARGE_INTEGER at;
		free(wp);
		if (fh == INVALID_HANDLE_VALUE) { fail("bigfile: create"); return; }
		DeviceIoControl(fh, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &got, NULL);
		at.QuadPart = big;
		SetFilePointerEx(fh, at, NULL, FILE_BEGIN);
		SetEndOfFile(fh);
		CloseHandle(fh);
	}
#else
	h = fopen(full, "wb");
	if (h) fclose(h);
	if (truncate(full, big)) { fail("bigfile: create"); return; }
#endif
	gvf = mount_folder(sroot, 1);
	if (f_mount(&fs, "", 1) != FR_OK) { fail("bigfile: mount"); return; }
	fill_rand(head, sizeof(head));
	fpath(fp, sizeof(fp), "big.bin");
	if (f_open(&f, fp, FA_OPEN_EXISTING | FA_WRITE) != FR_OK) { fail("bigfile: open"); return; }
	f_write(&f, head, sizeof(head), &w);
	f_close(&f);
	if (!vfat_sync(gvf, 0)) fail("bigfile: sync");
	h_stat(NULL, full, &sz, &mt);
	h = h_open(NULL, full, "rb");
	if (!h || (fread(back, 1, sizeof(back), h) != sizeof(back)) || memcmp(back, head, sizeof(head))) fail("bigfile: the patch did not land");
	if (h) fclose(h);
	if (sz != big) fail("bigfile: size %lld", sz);
	s_end();
	rm_plain(full, 0);
}

// a sync cut short leaves nothing hidden: a move that cannot finish puts the entry back in
// sight at once, and names left by a sync that never ended are made visible at the mount
static void sc_parked(void) {
	static const struct { const char* name; const char* shows; } left[] = {
		{VF_TMPMARK "m3~notes.txt", "notes.txt"}, {VF_TMPMARK "f9~data.bin", "data (recovered).bin"}, {VF_TMPMARK "m4", "recovered"}
	};
	char full[1200];
	unsigned int i;
	s_begin(118, 1);
	s_ren("games/manic.tap", "docs/manic.tap");
	fail_pat = "docs/manic";
	vfat_sync(gvf, 0);
	fail_pat = NULL;
	if (!host_has("manic.tap", "docs/manic.tap")) fail("parked: not in sight");
	if (tmp_left()) fail("parked: a temporary name left");
	s_sync("parked");
	s_end();
	for (i = 0; i < sizeof(left) / sizeof(left[0]); i++) {
		int m = m_add(left[i].shows, 0);
		FILE* f;
		model[m].size = 100 + i;
		model[m].data = (unsigned char*)malloc(model[m].size);
		fill_rand(model[m].data, model[m].size);
		snprintf(full, sizeof(full), "%s/%s", sroot, left[i].name);
		f = h_open(NULL, full, "wb");
		fwrite(model[m].data, 1, model[m].size, f);
		fclose(f);
	}
	gvf = mount_folder(sroot, 1);
	if (f_mount(&fs, "", 1) != FR_OK) fail("parked: mount");
	if (tmp_left()) fail("parked: a name left hidden at the mount");
	check_host(sroot, "parked mount");
	check_guest("parked mount");
	s_end();
}

// the room asked for is cut to the largest card, and the volume still goes live
static void sc_cap(void) {
	unsigned char sec[512];
	vFat* v = vfat_create();
	vfat_set_root(v, sroot, &host);
	vfat_add(v, -1, "", 0, 0, 1);
	fails = 0;
	if (!vfat_build(v, 0xffffffffu)) fail("cap: not built");
	else if (v->volume > 32u * 1024 * 2048) fail("cap: %u sectors", v->volume);
	else if (v->clusters >= 0x0ffffff5u) fail("cap: %u clusters", v->clusters);
	v->writable = 1;
	vfat_read(v, v->partStart, sec);
	if (!fails && !vfat_write(v, v->partStart + 1, sec)) fail("cap: no write taken");
	vfat_free(v);
}

// a host file changed outside the emulator while mounted keeps what was done to it: the
// machine's version goes beside it, and a delete leaves it
static void sc_outside(void) {
	char full[1200], fp[1100];
	unsigned char add[300], patch[200];
	FILE* f;
	FIL g;
	UINT w;
	int m, n0;
	unsigned int osize;
	unsigned char* outside;
	s_begin(119, 1);
	s_write("docs/touch.txt", 10);
	s_sync("outside live");
	m = m_find("games/manic.tap");
	osize = model[m].size + sizeof(add);
	outside = (unsigned char*)malloc(osize);
	memcpy(outside, model[m].data, model[m].size);
	fill_rand(add, sizeof(add));
	memcpy(outside + model[m].size, add, sizeof(add));
	snprintf(full, sizeof(full), "%s/games/manic.tap", sroot);
	f = h_open(NULL, full, "ab");
	fwrite(add, 1, sizeof(add), f);
	fclose(f);
	h_settime(NULL, full, 1700000000);
	fill_rand(patch, sizeof(patch));
	memcpy(model[m].data + 50, patch, sizeof(patch));
	fpath(fp, sizeof(fp), "games/manic.tap");
	f_open(&g, fp, FA_OPEN_EXISTING | FA_WRITE);
	f_lseek(&g, 50);
	f_write(&g, patch, sizeof(patch), &w);
	f_close(&g);
	vfat_sync(gvf, 0);
	if (!file_eq(full, outside, osize)) fail("outside: the host's change went");
	if (!host_has("games/manic (2).tap", "games/manic.tap")) fail("outside: the machine's version is not beside it");
	check_guest("outside");
	// the next write goes to the machine's own copy
	fill_rand(patch, sizeof(patch));
	memcpy(model[m].data + 10, patch, sizeof(patch));
	f_open(&g, fp, FA_OPEN_EXISTING | FA_WRITE);
	f_lseek(&g, 10);
	f_write(&g, patch, sizeof(patch), &w);
	f_close(&g);
	vfat_sync(gvf, 0);
	if (!file_eq(full, outside, osize)) fail("outside: the host's change went later");
	if (!host_has("games/manic (2).tap", "games/manic.tap")) fail("outside: the machine's copy did not follow");
	// changed outside, deleted inside: it stays
	snprintf(full, sizeof(full), "%s/README.TXT", sroot);
	f = h_open(NULL, full, "wb");
	fputs("rewritten on the host", f);
	fclose(f);
	n0 = trash_n;
	s_del("README.TXT");
	vfat_sync(gvf, 0);
	if ((h_kind(NULL, full) != 1) || (trash_n != n0)) fail("outside: deleted the host's version");
	check_guest("outside delete");
	free(outside);
	s_end();
}

// deleted, and another file of its size saved over its clusters with the same date: not a
// rename - the old one goes to the trash whole
static void sc_sameslot(void) {
	char full[1200];
	unsigned char* keep;
	unsigned int clu, size = 9000;
	int n0;
	s_begin(120, 1);
	s_write("old.bin", size);
	s_sync("sameslot old");
	keep = (unsigned char*)malloc(size);
	memcpy(keep, model[m_find("old.bin")].data, size);
	clu = first_cluster("old.bin");
	s_del("old.bin");
	fs.last_clst = clu - 1;
	s_write("other.bin", size);
	if (first_cluster("other.bin") != clu) fail("sameslot: setup, not on the freed clusters");
	n0 = trash_n;
	s_sync("sameslot");
	snprintf(full, sizeof(full), "%s/%i", trash_dir, n0);
	if ((trash_n != n0 + 1) || !file_eq(full, keep, size)) fail("sameslot: the old file is not in the trash");
	free(keep);
	s_remount("sameslot remount");
	s_end();
}

// == Refresh: the folder changed on the host while the guest has the volume mounted ==

// the volume as fsck sees it: both FATs alike, every chain whole and nobody else's, a
// file's chain as long as its size, no cluster in use that no entry reaches; with fsinfo,
// FSInfo's free count true
static unsigned int *vc_fat, *vc_mark;

static unsigned int le32(const unsigned char* p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void vc_dir(unsigned int clu, int depth, const char* when) {
	unsigned char sec[512];
	unsigned int c, s, i, n;
	unsigned int chain[4096];
	unsigned int nc = 0;
	if (depth > 32) { fail("%s: volume too deep", when); return; }
	for (c = clu; (c >= 2) && (c < gvf->clusters + 2); c = vc_fat[c]) {
		if (vc_mark[c]) { fail("%s: volume, directory cluster %u crossed", when, c); return; }
		vc_mark[c] = 1;
		if (nc < 4096) chain[nc++] = c;
		if (vc_fat[c] >= 0x0ffffff8) break;
	}
	for (n = 0; n < nc; n++) {
		for (s = 0; s < gvf->spc; s++) {
			vfat_read(gvf, gvf->partStart + gvf->dataStart + (chain[n] - 2) * gvf->spc + s, sec);
			for (i = 0; i < 512; i += 32) {
				unsigned char* e = sec + i;
				unsigned int fc, size, len = 0;
				if (e[0] == 0x00) return;
				if ((e[0] == 0xe5) || (e[11] == 0x0f) || (e[11] & 0x08) || (e[0] == '.')) continue;
				fc = (e[20] | (e[21] << 8)) << 16 | e[26] | (e[27] << 8);
				size = le32(e + 28);
				if (e[11] & 0x10) {
					vc_dir(fc, depth + 1, when);
					continue;
				}
				for (c = fc; (c >= 2) && (c < gvf->clusters + 2); c = vc_fat[c]) {
					if (vc_mark[c]) { fail("%s: volume, cluster %u crossed (%.11s)", when, c, e); return; }
					vc_mark[c] = 1;
					len++;
					if (vc_fat[c] >= 0x0ffffff8) break;
				}
				if ((unsigned long long)len * gvf->spc * 512 < size || (len && ((unsigned long long)(len - 1) * gvf->spc * 512 >= size)))
					fail("%s: volume, %.11s holds %u clusters for %u bytes", when, e, len, size);
			}
		}
	}
}

static void vol_check(const char* when, int fsinfo) {
	unsigned char sec[512], sec1[512];
	unsigned int s, k, c, fre = 0;
	unsigned int n = gvf->clusters + 2;
	vc_fat = (unsigned int*)calloc(gvf->fatSize * 128, sizeof(unsigned int));
	vc_mark = (unsigned int*)calloc(n, sizeof(unsigned int));
	for (s = 0; s < gvf->fatSize; s++) {
		vfat_read(gvf, gvf->partStart + gvf->reserved + s, sec);
		vfat_read(gvf, gvf->partStart + gvf->reserved + gvf->fatSize + s, sec1);
		for (k = 0; k < 128; k++) {
			unsigned int v = le32(sec + k * 4) & 0x0fffffff;
			unsigned int v1 = le32(sec1 + k * 4) & 0x0fffffff;
			if (v != v1) fail("%s: volume, FAT copies differ at %u", when, s * 128 + k);
			vc_fat[s * 128 + k] = v;
		}
	}
	vc_dir(2, 0, when);
	for (c = 2; c < n; c++) {
		if (!vc_fat[c]) fre++;
		else if (!vc_mark[c]) { fail("%s: volume, cluster %u in use by nothing", when, c); break; }
	}
	if (fsinfo) {
		vfat_read(gvf, gvf->partStart + 1, sec);
		k = le32(sec + 488);
		if (k != fre) fail("%s: volume, FSInfo says %u free, %u are", when, k, fre);
	}
	free(vc_fat);
	free(vc_mark);
}

// a host file of size bytes, into the model too (a changed one keeps its place there)
static void h_put(const char* path, unsigned int size, unsigned int mtime) {
	char full[1200];
	FILE* f;
	int i = m_find(path);
	if (i < 0) i = m_add(path, 0);
	free(model[i].data);
	model[i].data = (unsigned char*)malloc(size + 1);
	model[i].size = size;
	fill_rand(model[i].data, size);
	snprintf(full, sizeof(full), "%s/%s", sroot, path);
	f = h_open(NULL, full, "wb");
	fwrite(model[i].data, 1, size, f);
	fclose(f);
	h_settime(NULL, full, mtime);
}

static void h_dir(const char* path) {
	char full[1200];
	snprintf(full, sizeof(full), "%s/%s", sroot, path);
	h_mkdir(NULL, full);
	m_add(path, 1);
}

// a host file or folder gone, with all in it
static void h_gone(const char* path) {
	char full[1200];
	size_t l = strlen(path);
	int i;
	snprintf(full, sizeof(full), "%s/%s", sroot, path);
	rm_tree(full);
	for (i = mn - 1; i >= 0; i--)
		if (!strcmp(model[i].path, path) || (!strncmp(model[i].path, path, l) && (model[i].path[l] == '/'))) m_del(i);
}

// Refresh as the app does it: what the guest wrote first, then the fresh scan put in
static int s_refresh(const char* when, int expect, vfRefresh* r) {
	vFat* fresh;
	int res;
	vfat_flush(gvf);
	if (vfat_refresh_ready(gvf) == VFR_STUCK) vfat_sync(gvf, 1);	// once more, as the app does
	fresh = scan_folder(sroot, 0, NULL);
	res = vfat_refresh(gvf, fresh, scan_depth, r);
	vfat_free(fresh);
	if (res != expect) fail("%s: refresh gave %i, not %i", when, res, expect);
	fs.winsect = (LBA_t)0 - 1;		// a guest that reads its sectors again, as esxDOS does
	return res;
}

// the folder changed every way while a file and a directory are open in the guest; the
// guest reads on, sees the change, writes, and the folder follows
static void sc_refresh(void) {
	char fp[1100];
	unsigned char buf[3000];
	unsigned int before[3], k;
	FIL f;
	FF_DIR d;
	FILINFO fi;
	UINT got;
	vfRefresh r;
	int m, seen = 0;
	s_begin(121, 1);
	s_write("games/guest.bin", 7000);
	s_sync("refresh live");
	before[0] = first_cluster("games/manic.tap");
	before[1] = first_cluster("games/arcade/Jet Set Willy.tap");
	before[2] = first_cluster("games/guest.bin");
	// a file and a folder half read
	m = m_find("games/arcade/Jet Set Willy.tap");
	fpath(fp, sizeof(fp), "games/arcade/Jet Set Willy.tap");
	if (f_open(&f, fp, FA_READ) != FR_OK) fail("refresh: open");
	f_read(&f, buf, 1000, &got);
	if (f_opendir(&d, "/games") != FR_OK) fail("refresh: opendir");
	f_readdir(&d, &fi);
	// the host: new files and folders, a long cyrillic name, a file changed, one cut, gone ones
	h_put("new.txt", 5000, 1700000000);
	h_put("games/new2.tap", 12000, 1700000000);
	h_dir("newdir");
	h_put("newdir/a.bin", 70000, 1700000000);
	h_dir("newdir/deeper");
	h_put("newdir/deeper/b.bin", 300, 1700000000);
	h_put("\xd0\x9d\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x8f \xd0\xb8\xd0\xb3\xd1\x80\xd0\xb0 with a long name.tap", 9000, 1700000000);
	h_put("README.TXT", 20000, 1700000100);
	h_put("docs/manual of everything.txt", 100, 1700000100);
	h_gone("games/boot.sna");
	h_gone("empty");
	s_refresh("refresh", VFR_OK, &r);
	if ((r.added != 7) || (r.removed != 2) || (r.changed != 2)) fail("refresh: %i new, %i gone, %i changed", r.added, r.removed, r.changed);
	vol_check("refresh", 1);
	// the open ones read on as before
	f_read(&f, buf + 1000, sizeof(buf) - 1000, &got);
	if ((got != sizeof(buf) - 1000) || memcmp(buf, model[m].data, sizeof(buf))) fail("refresh: the open file reads other data");
	f_close(&f);
	while ((f_readdir(&d, &fi) == FR_OK) && fi.fname[0]) {
		if (!strcmp(fi.fname, "new2.tap")) seen = 1;
		if (!strcmp(fi.fname, "boot.sna")) fail("refresh: the open folder still lists a gone file");
	}
	f_closedir(&d);
	if (!seen) fail("refresh: the open folder does not list the new file");
	if ((first_cluster("games/manic.tap") != before[0]) || (first_cluster("games/arcade/Jet Set Willy.tap") != before[1])
			|| (first_cluster("games/guest.bin") != before[2])) fail("refresh: a file that stayed moved");
	check_guest("refresh");
	// the next sync has nothing to do but what the guest writes now
	k = host_ops;
	s_sync("refresh, no writes");
	if (host_ops != k) fail("refresh: a sync after it did %i host operations", host_ops - k);
	s_write("after.bin", 30000);
	s_write("newdir/a.bin", 1000);
	s_del("new.txt");
	s_ren("newdir/deeper/b.bin", "games/b.bin");
	s_sync("refresh, guest after");
	vol_check("refresh, guest after", 0);
	s_remount("refresh remount");
	s_end();
}

// a second refresh with nothing changed does nothing; one of a file grown in place and of a
// folder that outgrows its cluster
static void sc_refresh_grow(void) {
	char p[64];
	unsigned long long h;
	vfRefresh r;
	int i;
	s_begin(122, 1);
	s_refresh("grow none", VFR_OK, &r);
	if (gvf->wr) fail("grow: a refresh with nothing to do made the volume live");
	h = view_hash();
	s_refresh("grow none again", VFR_OK, &r);
	if (r.added || r.removed || r.changed || (view_hash() != h)) fail("grow: a refresh with nothing to do changed something");
	for (i = 0; i < 300; i++) {
		snprintf(p, sizeof(p), "file number %03i of many.bin", i);
		h_put(p, 10 + i, 1700000000);
	}
	h_put("games/manic.tap", 200000, 1700000200);
	s_refresh("grow", VFR_OK, &r);
	if ((r.added != 300) || (r.changed != 1)) fail("grow: %i new, %i changed", r.added, r.changed);
	s_refresh("grow again", VFR_OK, &r);
	if (r.added || r.removed || r.changed) fail("grow again: %i new, %i gone, %i changed", r.added, r.removed, r.changed);
	vol_check("grow", 1);
	check_guest("grow");
	for (i = 0; i < 300; i += 2) {
		snprintf(p, sizeof(p), "file number %03i of many.bin", i);
		h_gone(p);
	}
	s_refresh("grow thinned", VFR_OK, &r);
	vol_check("grow thinned", 1);
	check_guest("grow thinned");
	s_write("guest wrote.bin", 4000);
	s_sync("grow guest");
	s_remount("grow remount");
	s_end();
}

// no room: nothing changes; the guest's writes not yet carried: refused; a read-only volume
// takes it too
static void sc_refresh_refuse(void) {
	unsigned long long h;
	vfRefresh r;
	s_begin(123, 1);
	s_write("held.bin", 3000);
	gvf->hold = 1;
	vfat_tick(gvf);
	h = view_hash();
	if (vfat_refresh(gvf, gvf, 1000, &r) != VFR_WRITES) fail("refuse: a refresh over writes on hold");
	if (view_hash() != h) fail("refuse: a refused refresh changed the volume");
	gvf->hold = 0;
	s_sync("refuse released");
	h = view_hash();
	{
		char full[1200];
		FILE* f;
		snprintf(full, sizeof(full), "%s/huge.bin", sroot);
		f = h_open(NULL, full, "wb");
		fclose(f);
		h_truncate(NULL, full, (long long)gvf->clusters * gvf->spc * 512 - 4096);	// no room for it, written or not
	}
	s_refresh("refuse full", VFR_FULL, &r);
	if (view_hash() != h) fail("refuse: a refresh with no room changed the volume");
	h_gone("huge.bin");
	// a sync the host refuses part of: refused, and told what
	s_write("lockme.bin", 3000);
	fail_pat = "lockme";
	s_refresh("refuse stuck", VFR_STUCK, &r);
	if (!strstr(vfat_stuck(gvf), "lockme")) fail("refuse: the stuck file is not named (%s)", vfat_stuck(gvf));
	fail_pat = NULL;		// the cause cleared, nothing written since: the refresh's own retry heals it
	s_refresh("refuse unstuck", VFR_OK, &r);
	s_sync("refuse unstuck");
	s_end();
	// read only: the volume goes live for the refresh, the guest only reads
	s_begin(124, 0);
	h_put("ro new.txt", 777, 1700000000);
	h_gone("games/manic.tap");
	s_refresh("refuse read only", VFR_OK, &r);
	vol_check("refuse read only", 1);
	check_guest("refuse read only");
	s_end();
}

// on hold with nothing written, a refresh asks nothing; what the guest made deeper than the
// scan looks stays
static void sc_refresh_hold(void) {
	vfRefresh r;
	s_begin(125, 1);
	gvf->hold = 1;
	h_put("new on hold.txt", 100, 1700000000);
	s_refresh("hold", VFR_OK, &r);
	if (vfat_waiting(gvf)) fail("hold: a refresh with nothing written asks about writing");
	vol_check("hold first", 1);
	gvf->hold = 0;
	s_mkdir("games/arcade/deep");
	s_mkdir("games/arcade/deep/deeper");
	s_write("games/arcade/deep/deeper/f.bin", 3000);
	s_write("games/arcade/deep/g.bin", 2000);
	s_sync("hold deep");
	scan_depth = 2;			// games 1, arcade 2: deep is past what the scan reads
	s_refresh("hold deep refresh", VFR_OK, &r);
	scan_depth = 1000;
	if (r.removed) fail("hold: a refresh took %i deep entries for gone", r.removed);
	check_guest("hold deep");
	vol_check("hold deep", 0);		// FSInfo: the guest's count, which FatFs here does not keep
	s_end();
}

// a file of that name and size in the guest's listing of dir
static int dir_has(const char* dir, const char* name, unsigned int size) {
	FF_DIR d;
	FILINFO fi;
	int ok = 0;
	if (f_opendir(&d, dir) != FR_OK) return 0;
	while ((f_readdir(&d, &fi) == FR_OK) && fi.fname[0])
		if (!strcmp(fi.fname, name) && (fi.fsize == size)) ok = 1;
	f_closedir(&d);
	return ok;
}

// the names and a sync that has nothing to do; by listing, as a lookup of AGAME.TAP is
// the 8.3 name of "A game.tap" first
static void names_check(const char* when) {
	int k;
	if (!dir_has("/", "AGAME.TAP", 2000) || !dir_has("/", "A game.tap", 1000)) fail("%s: the guest does not list both", when);
	k = host_ops;
	sync_checked(0);		// 0 after a mount: nothing written, nothing live
	if (host_ops != k) fail("%s: the sync after did %i host operations", when, host_ops - k);
	check_host(sroot, when);
}

// a name whose 8.3 form another file has already takes ~1 and keeps its long name: at a
// refresh and at a mount, and the sync after takes it for no rename
static void sc_refresh_names(void) {
	vfRefresh r;
	s_begin(126, 1);
	h_put("A game.tap", 1000, 1700000000);		// AGAME.TAP is its 8.3 name
	h_put("a cart.tap", 3000, 1700000000);		// ACART.TAP
	s_refresh("names first", VFR_OK, &r);
	// no long name of their own: ~1 needs one. The fresh scan lists ACART.TAP before
	// "a cart.tap", so only the volume finds its 8.3 name taken.
	h_put("AGAME.TAP", 2000, 1700000000);
	h_put("ACART.TAP", 4000, 1700000000);
	s_refresh("names second", VFR_OK, &r);
	if (r.added != 2) fail("names: %i new", r.added);
	if (!dir_has("/", "ACART.TAP", 4000) || !dir_has("/", "a cart.tap", 3000)) fail("names: the guest does not list both carts");
	names_check("names");
	f_unmount("");
	vfat_free(gvf);
	gvf = mount_folder(sroot, 1);		// the mount makes the same names
	if (f_mount(&fs, "", 1) != FR_OK) fail("names: remount");
	names_check("names remount");
	s_end();
}

// a host rename, of the case alone too
static void host_mv(const char* from, const char* to) {
#ifdef _WIN32
	wchar_t* a = wide(from);
	wchar_t* b = wide(to);
	MoveFileW(a, b);
	free(a);
	free(b);
#else
	rename(from, to);
#endif
}

// what the scan does not list but the host has (a hidden name) stays; gone from the host, it
// goes. A name the host has in another case now is the new one.
static void sc_refresh_hidden(void) {
	char a[1200], b[1200];
	vfRefresh r;
	FILINFO fi;
	int m;
	s_begin(127, 1);
	s_write(".hidden.txt", 1500);
	s_mkdir(".cfg");
	s_write(".cfg/a.bin", 800);
	s_sync("hidden made");
	scan_hide = 1;
	h_put("seen.txt", 100, 1700000000);
	s_refresh("hidden", VFR_OK, &r);
	if (r.removed || (r.added != 1)) fail("hidden: %i gone, %i new", r.removed, r.added);
	check_guest("hidden");
	h_gone(".hidden.txt");
	s_refresh("hidden gone", VFR_OK, &r);
	if (r.removed != 1) fail("hidden gone: %i gone", r.removed);
	if (f_stat("/.hidden.txt", &fi) == FR_OK) fail("hidden gone: the guest still has it");
	scan_hide = 0;
	snprintf(a, sizeof(a), "%s/games/manic.tap", sroot);
	snprintf(b, sizeof(b), "%s/games/MANIC.TAP", sroot);
	host_mv(a, b);
	m = m_find("games/manic.tap");
	free(model[m].path);
	model[m].path = strdup("games/MANIC.TAP");
	s_refresh("hidden case", VFR_OK, &r);
	if ((r.removed != 1) || (r.added != 1)) fail("hidden case: %i gone, %i new", r.removed, r.added);
	check_guest("hidden case");
	// a case only a Unicode fold sees: one entry for the one host file, not two
	h_put("\xd0\x98\xd0\xb3\xd1\x80\xd0\xb0.tap", 2500, 1700000000);		// Igra.tap in Cyrillic
	s_refresh("hidden cyrillic", VFR_OK, &r);
	snprintf(a, sizeof(a), "%s/\xd0\x98\xd0\xb3\xd1\x80\xd0\xb0.tap", sroot);
	snprintf(b, sizeof(b), "%s/\xd0\x98\xd0\x93\xd0\xa0\xd0\x90.tap", sroot);
	host_mv(a, b);
	m = m_find("\xd0\x98\xd0\xb3\xd1\x80\xd0\xb0.tap");
	free(model[m].path);
	model[m].path = strdup("\xd0\x98\xd0\x93\xd0\xa0\xd0\x90.tap");
	s_refresh("hidden cyrillic case", VFR_OK, &r);
	if ((r.removed != 1) || (r.added != 1)) fail("hidden cyrillic case: %i gone, %i new", r.removed, r.added);
	if (!dir_has("/", "\xd0\x98\xd0\x93\xd0\xa0\xd0\x90.tap", 2500) || dir_has("/", "\xd0\x98\xd0\xb3\xd1\x80\xd0\xb0.tap", 2500))
		fail("hidden cyrillic case: the guest lists the old spelling");
	check_guest("hidden cyrillic case");
	s_sync("hidden sync");
	s_remount("hidden remount");
	s_end();
}

static int run_scenarios(const char* root) {
	static const struct { const char* name; void (*fn)(void); } list[] = {
		{"swap", sc_swap}, {"replace", sc_replace}, {"fragment", sc_fragment}, {"bigdir", sc_bigdir},
		{"nested", sc_nested}, {"format", sc_format}, {"open", sc_open}, {"names", sc_names},
		{"trash", sc_trash}, {"unknown", sc_unknown}, {"readonly", sc_readonly}, {"hostfail", sc_hostfail},
		{"halfmove", sc_halfmove}, {"retype", sc_retype}, {"hold", sc_hold}, {"pin", sc_pin}, {"newdir", sc_newdir},
		{"flush", sc_flush}, {"bigfile", sc_bigfile}, {"parked", sc_parked}, {"cap", sc_cap},
		{"outside", sc_outside}, {"sameslot", sc_sameslot}, {"refresh", sc_refresh}, {"refresh_grow", sc_refresh_grow},
		{"refresh_refuse", sc_refresh_refuse}, {"refresh_hold", sc_refresh_hold}, {"refresh_names", sc_refresh_names},
		{"refresh_hidden", sc_refresh_hidden}
	};
	unsigned int i;
	int bad = 0;
	snprintf(sroot, sizeof(sroot), "%s", root);
	printf("== scenarios\n");
	for (i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
		fails = 0;
		list[i].fn();
		if (gvf) s_end();
		printf("%s: %s%s\n", list[i].name, fails ? "FAIL " : "ok", fails ? failmsg : "");
		if (fails) bad++;
	}
	return bad;
}

int main(int argc, char** argv) {
	char root[1024];
	unsigned int seeds = 20, s;
	int ops = 300, bad = 0;
	if (argc < 2) {
		fprintf(stderr, "usage: vfat-bench WORKDIR [seeds] [ops]\n");
		return 2;
	}
	if (argc > 2) seeds = atoi(argv[2]);
	if (argc > 3) ops = atoi(argv[3]);
	snprintf(root, sizeof(root), "%s/vol", argv[1]);
	snprintf(trash_dir, sizeof(trash_dir), "%s/trash", argv[1]);
	h_mkdir(NULL, argv[1]);
	rm_tree(trash_dir);
	h_mkdir(NULL, trash_dir);
	if (getenv("VFLOG")) {		// what the volume does, on stderr
		xlog_init();
		xlog_flush_early();
		xlog_lev[XLG_DISK] = XLL_DEBUG;
	}
	if (getenv("VFTORT")) {		// one torture seed alone
		torture = 1;
		s = atoi(getenv("VFTORT"));
		if (!run_random(root, s, ops)) printf("seed %u: FAIL %s (%i)\n", s, failmsg, fails);
		return fails ? 1 : 0;
	}
	printf("== random\n");
	for (s = 1; s <= seeds; s++) {
		int ok = run_random(root, s, ops);
		if (!ok) {
			printf("seed %u: FAIL %s (%i)\n", s, failmsg, fails);
			bad++;
		}
	}
	printf("%u seeds, %i failed\n", seeds, bad);
	printf("== torture\n");
	torture = 1;
	for (s = 1; s <= (seeds + 4) / 5; s++) {
		if (!run_random(root, 1000 + s, ops / 4)) {
			printf("seed %u: FAIL %s (%i)\n", 1000 + s, failmsg, fails);
			bad++;
		}
	}
	torture = 0;
	printf("%u seeds\n", (seeds + 4) / 5);
	bad += run_scenarios(root);
	return bad ? 1 : 0;
}

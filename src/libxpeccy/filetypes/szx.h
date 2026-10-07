#pragma once

// The parts of the zx-state reader and writer that live in more than one file.
// The entry points the rest of the program uses are in filetypes.h.

#include "../spectrum.h"

#ifdef __cplusplus
extern "C" {
#endif

// A medium the last loaded file links to or carries. The core does not open
// it: inserting a tape or a disk is the program's business (it asks before a
// changed disk is thrown out), so szx_media() hands the list over.
enum {
	SZX_MED_TAPE = 1,
	SZX_MED_BETA,
	SZX_MED_PLUS3
};

#define SZX_MEDIA_MAX	8

typedef struct {
	int kind;		// SZX_MED_*
	int drive;		// a disk's drive, from A
	int block;		// the tape's block
	int cylinder;		// where a drive's heads are
	int protect;		// a disk written protected
	char path[1024];	// the file linked to, empty for one carried
	char ext[16];		// a carried image's type, by extension
	unsigned char* data;	// ...and the image
	size_t size;
} szxMedia;

int szx_media(const szxMedia** list);

// Our own data, in the creator block (szx_ext.c). find: where it starts in a
// creator block's own data, NULL when the block is someone else's.
const unsigned char* szx_ext_find(const unsigned char* data, size_t len, size_t* extlen);
int szx_ext_hardware(const unsigned char* file, size_t len);	// SNAP_HW_* of a file of ours
void szx_ext_load(Computer*, const unsigned char* ext, size_t len);
int szx_ext_tick(Computer*, int tick);		// where in the frame to stand the machine
int szx_ext_can_save(Computer*);		// a machine the spec cannot name, but we can
int szx_libspectrum_swap(const unsigned char* data, size_t len);

typedef struct szxBuf szxBuf;
void szx_ext_save(szxBuf*, Computer*);

// General Sound (szx_ext.c too)
void szx_rd_gs(Computer*, const unsigned char*, size_t);
void szx_rd_gsrp(Computer*, const unsigned char*, size_t);
void szx_wr_gs(szxBuf* file, szxBuf* body, Computer*);

// the buffer a file is written into
struct szxBuf {
	unsigned char* data;
	size_t len;
	size_t cap;
	int fail;
};

void sb_put(szxBuf*, const void*, size_t);
void sb_byte(szxBuf*, int);
void sb_word(szxBuf*, int);
void sb_dword(szxBuf*, unsigned);
void sb_block(szxBuf*, unsigned id, szxBuf* body);
int sb_deflate(szxBuf*, const unsigned char*, size_t);
void sb_free(szxBuf*);

#define BID(a,b,c,d)	((unsigned)(a) | ((unsigned)(b) << 8) | ((unsigned)(c) << 16) | ((unsigned)(d) << 24))

static inline int rd_word(const unsigned char* p) {
	return p[0] | (p[1] << 8);
}

static inline unsigned rd_dword(const unsigned char* p) {
	return (unsigned)rd_word(p) | ((unsigned)rd_word(p + 2) << 16);
}

int szx_inflate(const unsigned char* src, size_t n, unsigned char* dst, size_t len);

#ifdef __cplusplus
}
#endif

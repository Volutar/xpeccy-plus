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
void szx_media_clear(void);

// Our own data, in the creator block (szx_ext.c). find: where it starts in a
// creator block's own data, NULL when the block is someone else's.
const unsigned char* szx_ext_find(const unsigned char* data, size_t len, size_t* extlen);
int szx_ext_hardware(const unsigned char* file, size_t len);	// SNAP_HW_* of a file of ours
// 1 when it stood the machine where in the frame it was, exactly
int szx_ext_load(Computer*, const unsigned char* ext, size_t len);
int szx_ext_can_save(Computer*);		// a machine the spec cannot name, but we can
// what the tape and the drives were doing, once the program has put the media
// of szx_media() back in
void szx_ext_media(Computer*);
// the id of the running machine, written into the file with it
void szx_set_machine(const char* id);

// What a snapshot was taken with that the machine it went into lacks or has
// set otherwise. The load goes on regardless; the gui says so. A joystick, a
// mouse and a General Sound are only logged: writers put in whatever they
// were set to, used or not.
enum {
	SZN_RELATIVE = 1,	// a model we do not have, loaded on its nearest one
	SZN_TIMINGS = 2,	// early against late ULA timings
	SZN_ISSUE = 4,		// issue 2 against issue 3
	SZN_ULAPLUS = 8,
	SZN_AY = 16,
	SZN_BETA = 32,
	SZN_COVOX = 64,
	SZN_DEVICE = 128	// one we do not emulate at all, switched on
};
int szx_notes(void);		// SZN_* of the last load

typedef struct szxBuf szxBuf;
void szx_ext_save(szxBuf*, Computer*);

// the Z80's registers from F to IM, laid out alike in Z80R and GS (szx.c);
// swapaf: A before F
void szx_rd_regs(CPU*, const unsigned char*, int swapaf);
void szx_wr_regs(szxBuf*, CPU*);
void szx_wr_page(szxBuf*, szxBuf* body, unsigned id, int page, const unsigned char* src, size_t n);

// Saving in two steps: the file built in memory, its pages raw - quick, so it
// can be done while the machine is held - and then packed and written.
// szx_write() frees the buffer whatever happens.
int szx_build(Computer*, szxBuf*);
int szx_write(szxBuf*, const char* name);

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

#define SZR_COMPRESSED	1	// RAMP, ROM, GSRP: the data is zlib

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

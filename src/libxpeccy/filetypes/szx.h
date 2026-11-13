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

// The Next's SD card as the last loaded file names it: the program mounts it, puts the
// folders a program from elsewhere stood in back on it, and compares the print
// (sdc_dir_print) - the card is never in the file.
#define SZX_PINS	4
#define SZX_CARD_CONTENT	0x02	// the record's flags: the print is of what the card holds
typedef struct {
	int on;			// the file names a card
	int folder;		// ...a folder, not an image
	unsigned long long print;	// what it held when saved (fat_dir_print), 0 unknown
	char path[4096];
	int pins;
	struct {
		char name[256];		// the folder's name in the card's root, UTF-8
		char host[4096];	// ...and where it is on the host (or the program's file), UTF-8
	} pin[SZX_PINS];
} szxCard;
const szxCard* szx_card(void);
// a carried image's name as it was on the host, from our own record
void szx_media_link(int kind, int drive, const unsigned char* src, size_t n);

// An image to carry in the file being saved instead of only naming it: the
// tape's file as it is, a disk as the drive holds it (ext: its type; a +3
// disk is always a .dsk, the only image its block can be read as).
// The link set with szx_set_links stays, in our own record. Kept until the
// next call; the bytes are the caller's, read by szx_build.
typedef struct {
	const unsigned char* data;
	size_t size;
	char ext[8];
} szxCarry;
void szx_set_carry(const szxCarry* tape, const szxCarry* disks);	// disks: 4 or NULL
// the link of a medium being carried, NULL when it is only linked or not there
const char* szx_carried_link(int kind, int drive);

// Our own data, in the creator block (szx_ext.c). find: where it starts in a
// creator block's own data, NULL when the block is someone else's.
const unsigned char* szx_ext_find(const unsigned char* data, size_t len, size_t* extlen);
int szx_ext_hardware(const unsigned char* file, size_t len);	// SNAP_HW_* of a file of ours
// 1 when it stood the machine where in the frame it was, exactly
int szx_ext_load(Computer*, const unsigned char* ext, size_t len);
void szx_ext_forget(void);	// the last file's card and media state dropped
int szx_ext_can_save(Computer*);		// a machine the spec cannot name, but we can
// what the tape and the drives were doing, once the program has put the media
// of szx_media() back in; carried: a bit per drive whose disk came out of the
// file, which takes the saved one's "changed" too; tape_too: 0 when the tape in
// is not the snapshot's
void szx_ext_media(Computer*, int carried, int tape_too);// the id of the running machine, written into the file with it
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
	SZN_DEVICE = 128,	// one we do not emulate at all, switched on
	SZN_ZC = 256		// a Z-Controller (szx_ext.c's record ZC)
};
int szx_notes(void);		// SZN_* of the last load
void szx_note(int);		// ...one more of them, as the load goes

typedef struct szxBuf szxBuf;
void szx_ext_save(szxBuf*, Computer*);
// The Next's ESP-01, which the program runs outside the machine (xcore/esp.cpp): save adds
// its record's bytes, none when no module is fitted; load takes them back, or n = 0 from a
// file without the module. NULL: no such part.
extern void (*szx_esp_save)(Computer*, szxBuf*);
extern void (*szx_esp_load)(Computer*, const unsigned char*, size_t);

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

#define SZR_COMPRESSED	1	// RAMP, ROM, GSRP, DMRP: the data is zlib
// DMMC (divMMC, Fuse's block; DMRP holds its 8K RAM pages as RAMP does 16K)
#define SZDM_EPROM_WP	1	// the EEPROM jumper closed
#define SZDM_PAGED	2	// the board's memory is in
#define SZDM_COMPRESSED	4	// the EEPROM is zlib

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

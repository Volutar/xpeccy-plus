#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "defines.h"

// A tape tick is one T state of the machine playing the tape - the real period
// comes from the cpu clock through tape_set_tick_ns(), this is only the fallback.
#define TAPCPUNS	284		// ns per cpu tick @ 3.49MHz
#define TAPTICKNS	TAPCPUNS	// ns in one tape tick
// The three below are counts of ticks, not durations: they are read against sigLen
// and never turned back into seconds.
#define TAPTPS		(1000000000/TAPTICKNS)	// ticks per second
#define TAPE_PAUSE_TICKS (TAPTPS / 5)	// a gap this long is the end of a block
#define TAPE_TAIL_TICKS  (TAPTPS / 10)	// run-out played after the last pulse
#define TAPE_RATE_BITS	32		// fraction bits of ticksPerNsFixed

// ZX spectrum signal timings
// 1T ~ 284ns ~ 0.284mks @ 3.51MHz
// pilot	2168T	615 mks
// sync1	667T	189 mks
// sync2	735T	208 mks
// 0		855T	242 mks
// 1		1710T	485 mks
// sync3	954T	270 mks
#define	PILOTLEN	2168*TAPCPUNS/TAPTICKNS
#define	SYNC1LEN	667*TAPCPUNS/TAPTICKNS
#define	SYNC2LEN	735*TAPCPUNS/TAPTICKNS
#define	SIGN0LEN	855*TAPCPUNS/TAPTICKNS
#define	SIGN1LEN	1710*TAPCPUNS/TAPTICKNS

// A pulse carries its level in bit 7 of TapeSignal.vol. A pause is marked by
// a volume at the middle, and still carries the level change that closes the
// pulse before it. The lengths above are T states at the standard clock,
// whatever clock the machine playing the tape runs at.
#define TAP_VOL_PAUSE(v)	(((v) == 0x80) || ((v) == 0x7f))
#define TAP_VOL_LEV(v)		(((v) & 0x80) ? 1 : 0)
#define TAP_PAUSE_VOL(lev)	((lev) ? 0x80 : 0x7f)
#define TAPE_STD_TPS		3500000

// A pulse carries its level in bit 7 of TapeSignal.vol. A pause is marked by
// a volume at the middle, and still carries the level change that closes the
// pulse before it. The lengths above are T states at the standard clock,
// whatever clock the machine playing the tape runs at.
#define TAP_VOL_PAUSE(v)	(((v) == 0x80) || ((v) == 0x7f))
#define TAP_VOL_LEV(v)		(((v) & 0x80) ? 1 : 0)
#define TAP_PAUSE_VOL(lev)	((lev) ? 0x80 : 0x7f)
#define TAPE_STD_TPS		3500000

enum {
	TAPE_HEAD = 0,
	TAPE_DATA
};

#define	TAPE_TEXT_LEN	64	// room for a block label out of a tape image
#define	TAPE_NAME_LEN	10	// the name a standard header carries

// what a standard header says it carries (2nd byte of the block)
enum {
	TAPE_HT_NONE = -1,
	TAPE_HT_PROG = 0,
	TAPE_HT_NUMARR,
	TAPE_HT_CHRARR,
	TAPE_HT_CODE
};

typedef struct {
	unsigned breakPoint:1;
	unsigned stopMark:1;		// the image asks for the tape to stop here
	unsigned stop48:1;		// ...but only on a 48K
	unsigned hasBytes:1;		// block holds bytes, not just a signal

	int type;			// TAPE_HEAD / TAPE_DATA
	int htype;			// header: TAPE_HT_*
	unsigned char name[TAPE_NAME_LEN];	// header: the name, as the bytes it is - tokens and control codes included
	int dlen;			// header: length of the data block it announces
	int par1;			// header: LINE for a program, address for code, name for an array
	char text[TAPE_TEXT_LEN];	// what the image itself calls the block
	int size;
	int time;
} TapeBlockInfo;

typedef struct {
	unsigned int size;		// tape ticks
	unsigned char vol;
} TapeSignal;

typedef struct {
	unsigned breakPoint:1;
	unsigned stopMark:1;
	unsigned stop48:1;		// the image asks a 48K to stop the tape here (TZX #2A)
	unsigned hasBytes:1;
	unsigned isHeader:1;
	unsigned vol:1;

	int plen;
	int s1len;
	int s2len;
	int len0;
	int len1;
	int pdur;
	int dataPos;
	int sigCount;
	int time;
	int crc;
	char text[TAPE_TEXT_LEN];
	TapeSignal* data;
} TapeBlock;

typedef struct {
	unsigned on:1;
	unsigned rec:1;
	unsigned isData:1;
	unsigned wait:1;
	unsigned blkChange:1;
	unsigned newBlock:1;
	int levRec;	// signal to tape
	unsigned oldRec:1;	// previous rec signal
	unsigned char speed;	// 95 to 105

	unsigned armed:1;	// play as soon as a loader outside the rom asks for the tape
	unsigned tail:1;	// playing out the level change the last pulse ends on
	unsigned is48:1;	// the machine is a 48K, for stop48
	unsigned userStop:1;	// stopped by hand: the automatics may not start it again
	unsigned autorew:1;	// play starts the tape over once it has run to the end
	unsigned changed:1;	// blocks added, moved or taken out since the image was read or saved
	unsigned detectOn:1;	// auto play by CPU port-0xFE polling pattern (for loaders that bypass the ROM)
	int detectLastTick;
	int detectLastB;
	int detectReads;
	int portReads;		// reads of the tape port, cleared by the one counting them per frame
	int ldBase;		// a copy of LD-BYTES in ram flash loading answers for, -1: none
	int ldDir;		// its INC IX (+1) or DEC IX (-1)
	int ldBlock;		// the block its edge routine was last checked on
	int inPc;		// the last IN from the tape port looked at (zx_in_use)
	int inFrame;		// on which frame
	int inUse;		// and what it was

	long long ticksPerNsFixed;	// ticks in one ns, TAPE_RATE_BITS fraction bits
	long long tickAcc;		// ticks not played yet, same fraction
	int nsLazy;		// ns tapSync() was handed and has not made into ticks yet
	int nsCalm;		// nsLazy may grow up to this with no pulse ending
	unsigned char volPlay;
	int block;
	int pos;
	int sigLen;
	char* path;
	char blkText[TAPE_TEXT_LEN];	// label the next blocks read in will get
	TapeBlock tmpBlock;
	int blkCount;
	TapeBlock* blkData;

	unsigned xen:1;
	cbirq xirq;
	void* xptr;
} Tape;

// tape signal edge detector: a stream of samples in, level changes out. It
// tracks the recording's own centre line, so a dc offset or a level that
// drifts over the tape does not matter.
typedef struct {
	int lev;		// level the signal stands at now
	int first;		// the centre line has not been primed yet
	int shift;		// time constant of the averages: 1 << shift samples
	int dc;			// the centre line, 8 fraction bits
	int env;		// how far the signal usually swings from it
} TapeEdge;

void tape_edge_reset(TapeEdge*, int rate);
int tape_edge_step(TapeEdge*, int amp);		// !0 when the level changed

Tape* tape_create(cbirq, void*);
void tape_destroy(Tape*);

void tape_set_path(Tape*, const char*);
void tape_set_tick_ns(Tape*, double);

void tapEject(Tape*);
int tapPlay(Tape*);
int tapUserPlay(Tape*);
void tapRec(Tape*);
void tapStop(Tape*);
void tapUserStop(Tape*);
void tapRewind(Tape*,int);
int tap_rewind_at_end(Tape*);

void tap_sync_slow(Tape*,int);
// Time for the tape. Up to the end of the pulse it stands in, nothing but a
// count would change, so the ns are only added up - at normal speed, where
// making them into ticks has no rounding. Whatever reads or moves where the
// tape stands calls tape_settle() first; the level (volPlay) is always right.
static inline void tapSync(Tape* tap, int ns) {
	if ((long long)tap->nsLazy + ns < tap->nsCalm) {
		tap->nsLazy += ns;
		return;
	}
	tap_sync_slow(tap, ns);
}
void tape_settle(Tape*);
int tape_sig_len(Tape*);
// where the tape stands, settled first
typedef struct {
	int block;
	int pos;
	int sigLen;
	long long tickAcc;
} TapePos;
TapePos tape_pos(Tape*);
void tape_set_sig_len(Tape*, int);
int tap_play_on(Tape*, int);
void tape_set_speed(Tape*, int);
void tapNextBlock(Tape*);
void tap_copy_pos(Tape*, const Tape*);
void tapDetectLoader(Tape*, int tick, int regB, int earTest, int fromUser);
void tapArmPlay(Tape*);

TapeBlockInfo tapGetBlockInfo(Tape*,int);
int tapGetBlocksInfo(Tape*,TapeBlockInfo*);
int tapGetBlockData(Tape*,int,unsigned char*,int);
int tapGetBlockTime(Tape*,int,int);

void tap_add_block(Tape*,TapeBlock);
void tap_set_text(Tape*,const char*);
void tapDelBlock(Tape*,int);
void tapSwapBlocks(Tape*,int,int);

void tapAddFile(Tape*,const char*,int,unsigned short,unsigned short,unsigned short,unsigned char*,int);
TapeBlock makeTapeBlock(unsigned char*, int, int);

void blkClear(TapeBlock*);
void blkAddPulse(TapeBlock* blk, int len, int vol);
void blkAddPulseLev(TapeBlock* blk, int len, int lev);
void blkAddWave(TapeBlock*, int);
void blkAddByte(TapeBlock*, unsigned char, int, int);
void blkAddPause(TapeBlock*, int);

#ifdef __cplusplus
}
#endif

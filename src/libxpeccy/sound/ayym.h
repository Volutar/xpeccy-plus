#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "sndcommon.h"

// ay_type
enum {
	SND_NONE = 0,
	SND_AY,
	SND_YM,
	SND_YM2203,	// FM
	SND_END
};
// ay_stereo
enum {
	AY_MONO = 0,
	AY_ABC,
	AY_ACB,
	AY_BAC,
	AY_BCA,
	AY_CAB,
	AY_CBA
};
// ts_type
enum {
	TS_NONE = 0,
	TS_NEDOPC,
	TS_ZXNEXT
};

#include "sndcommon.h"

typedef struct aymChip aymChip;

// ay-3-8910
void ay_reset(aymChip*);
int ay_rd(aymChip*, int);
void ay_wr(aymChip*, int, int);
void ay_sync(aymChip*, int);
void ay_tick_n(aymChip*, int);		// that many half periods of the chip clock
void ay_set_reg(aymChip*, int);
void ay_flush(aymChip*);
sndPair ay_vol(aymChip*);

// yamaha-2149
//void ym_reset(aymChip*);
int ym_rd(aymChip*, int);
void ym_wr(aymChip*, int, int);
//void ym_sync(aymChip*, int);
sndPair ym_vol(aymChip*);

// yamaha-2203
void ym2203_reset(aymChip*);
int ym2203_rd(aymChip*, int);
void ym2203_wr(aymChip*, int, int);
void ym2203_flush(aymChip*);		// count the time sync has put by, see ay_flush()
int ym2203_fm_out(aymChip*);		// the fm half; the SSG one is ym_vol()
void ym2203_free(aymChip*);		// drop the core, if this chip ever had one
int ym2203_state_size(aymChip*, void**);	// its state as bytes: how many, where
void ym2203_state_pack(aymChip*);	// the core into those bytes
void ym2203_state_unpack(aymChip*);	// and back out of them

typedef void(*sccbwr)(aymChip*, int, int);
typedef int(*sccbrd)(aymChip*, int);
typedef void(*sccbsync)(aymChip*, int);
typedef sndPair(*sccbvol)(aymChip*);
typedef void(*sccbcmn)(aymChip*);

typedef int(*ayxrd)(int, void*);
typedef void(*ayxwr)(int, int, void*);

typedef struct {
	int id;
	const char* name;
	const char* short_name;
	double frq;
	sccbcmn res;
	sccbrd rd;
	sccbwr wr;
	sccbsync sync;
	sccbvol vol;
} scDesc;

typedef struct {
	unsigned tdis:1;	// tone off
	unsigned ndis:1;	// noise off
	unsigned een:1;		// envelope on
	unsigned lev:1;		// current signal level
	unsigned mute:1;	// silenced from the debugger
	int vol;
	int per;		// period in ticks (0:channel off)
	int cnt;		// ticks countdown
	int step;		// env:vol change direction (+1 -1); noise:seed
	int levpk;		// loudest level since the debugger last looked
} aymChan;

// what the debugger asks the AY code for
int ay_chan_lev(aymChip*, aymChan*);	// what it is putting out this instant
void ay_set_speed(double);		// host speed, for the sub-period limit
int ay_chan_peak(aymChan*);		// the loudest since the last call, and start again
int ay_chan_dac(aymChip*, aymChan*, const int*);	// that level through a DAC table
sndPair ay_mix_tab(aymChip*, const int*);	// all three, mixed, through one
void ay_env_shape(int, unsigned char*, int);	// an envelope form, level by level
void ay_poke_reg(aymChip*, int, int);	// write a register the way a port write does

// The FM half of a YM2203 runs on ymfm (sound/ymfm/), which keeps its own
// state. What follows is a view of it for the debugger's FM page, filled by
// ym2203_fm_view() into storage the page owns.

enum {
	OPST_OFF = 0,
	OPST_ATK,
	OPST_DEC,
	OPST_SUS,
	OPST_REL
};

typedef struct {
	int tlev;				// total level, 0 loudest .. 127
	struct {				// phase generator
		unsigned phase;			// 10.10
		int freq;			// f-number
		int block;			// octave
		int pstep;			// phase step
	} pg;
	struct {				// envelope generator
		int state;			// OPST_*
		int ks;				// key scale
		int atk, dec, sus, rel;		// rates, as the registers hold them
		int suslev;			// sustain level, 0..15
		int att;			// current attenuation, 0 loudest .. 1023
	} eg;
	int key;			// keyed on: ymfm keeps this one to itself
	int rofs;			// this operator's offset into the register file:
					// the registers hold them 1,3,2,4, not 1,2,3,4
} fmOper;

typedef struct {
	fmOper op[4];		// in algorithm order op1..op4; the registers hold
				// them 1,3,2,4 and the core sorts that out
	int algo;		// ops connection (algorithm)
	int out;		// what the channel is putting out, signed
} fmChan;

void ym2203_fm_view(aymChip*, fmChan*);	// fill one of those from the core
void ym2203_poke_reg(aymChip*, int, int);	// write an FM register from outside

struct aymChip {
	unsigned coarse:1;	// 4-bit DAC volume
	unsigned blk_fm:1;	// 1:block fm output
	int stereo;
	int sep;		// stereo separation: 0 = mono, 100 = full panorama
	int wait;		// waiting. chip is busy when >0

	int type;
	double frq;		// in MHz
	sccbcmn res;
	sccbrd rd;
	sccbwr wr;
	sccbsync sync;
	sccbvol vol;

	ayxrd xrd;		// read/write callbacks for ports 14,15
	ayxwr xwr;
	void* xptr;

	aymChan chanA;		// psg/ssg channels
	aymChan chanB;
	aymChan chanC;
	aymChan chanN;
	aymChan chanE;
	int eForm;		// envelope form
	// half periods per nanosecond, 32.32, with the leftover kept between
	// calls. Whole nanoseconds left every chip sharp by a different
	// amount: 500/1.773447 truncated to 281, 500/1.75 to 285.
	long long tickFx;
	long long tickAcc;
	int pendNs;		// time handed in but not yet counted into ticks, see ay_flush()

	void* fm;		// the fm core of a YM2203 (nothing for the other chips)
	unsigned char fm_off[3];// fm channels the debugger mutes

	unsigned char curReg;
	unsigned char reg[256];
} ;

typedef struct {
	unsigned mute_l:1;
	unsigned mute_r:1;
	unsigned r_stat:1;	// read status reg instead of chip regs
	int type;
	double frq;		// the clock as set, an AY's in MHz, 0 = Auto (see ts_set_frq)

	struct {
		unsigned char* data;
		int size;
		int mask;
	} rom;

	aymChip* chipA;
	aymChip* chipB;
	aymChip* chipC;
	aymChip* chipD;
	aymChip* curChip;
} TSound;

void initNoise();

const scDesc* find_chip_type(int);	// its nominal clock, for one
void chip_set_type(aymChip*, int);
int chip_frq_mul(int);
void ts_set_frq(TSound*, double, double);
void chip_set_xdev(aymChip*, ayxrd, ayxwr, void*);

TSound* tsCreate(int,int,int);
void tsDestroy(TSound*);
void tsReset(TSound*);
int tsIn(TSound*,int);
void tsOut(TSound*,int,int);
// ay_sync(), which every chip type syncs with: the time is only counted, and
// the ticks made of it when something reads the chip (ay_flush)
static inline void ay_sync_ns(aymChip* ay, int ns) {
	if (ns > 0)
		ay->pendNs += ns;
	// nothing may look at the chip for a long while - fast mode takes no
	// samples - and the count is an int: settle it before it can wrap
	if (ay->pendNs > (1 << 30))
		ay_flush(ay);
}
// this runs on every instruction: ay_sync() without the call
static inline void ts_chip_sync(aymChip* chip, int ns) {
	if (chip->type == SND_NONE) return;
	if (chip->sync == ay_sync)
		ay_sync_ns(chip, ns);
	else
		chip->sync(chip, ns);
}
static inline void tsSync(TSound* ts, int ns) {
	ts_chip_sync(ts->chipA, ns);
	ts_chip_sync(ts->chipB, ns);
	ts_chip_sync(ts->chipC, ns);
	ts_chip_sync(ts->chipD, ns);
}
void tsSetRomSize(TSound*, int);
void tsLoadRom(TSound*, const char*);
int tsReadRom(TSound*, int);

sndPair tsGetVolume(TSound*);
aymChip* ts_chip(TSound*, int);		// chip 0..3, NULL for anything else

// Run-ahead: a chip can keep state outside its own struct, which is all a
// snapshot copies. These hand that state over as a range of bytes instead.
int ts_state_range(TSound*, int, void**);	// chip 0..3: its size, and where
void ts_state_capture(TSound*);			// into those ranges
void ts_state_restore(TSound*);			// and back out of them

#ifdef __cplusplus
}
#endif

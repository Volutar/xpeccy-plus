#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ayym.h"
#include "../defines.h"

// tables

char noizes[0x20000];		// here iz noize values 1/0 [generated at start] TODO:obsolete, rewrite parts using it

// ayDACvol/ymDACvol in each own files
#if 0
int ayDACvol[32] = {0x0000,0x0000,0x001D,0x003A,0x0052,0x0065,0x007D,0x009A,
			   0x00C2,0x00F0,0x0122,0x0146,0x0186,0x01D6,0x0226,0x0274,
			   0x02EA,0x037E,0x041C,0x04B2,0x0594,0x06AF,0x07C8,0x08EB,
			   0x0AA0,0x0CC4,0x0EE6,0x1108,0x143A,0x1820,0x1C14,0x1FFF};
#elif 0
int ayDACvol[32] = {0x0000,0x0000,0x003B,0x0074,0x00A4,0x00CA,0x00FB,0x0134,
			   0x0184,0x01E0,0x0244,0x028D,0x030C,0x03AD,0x044C,0x04E8,
			   0x05D4,0x06FD,0x0838,0x0965,0x0B28,0x0D5F,0x0F91,0x11D7,
			   0x1540,0x1988,0x1DCC,0x2211,0x2874,0x3040,0x3828,0x3FFF};
#endif

// AY/YM sound chip common

void sc_dum_res(aymChip* chip) {}
int sc_dum_rd(aymChip* chip, int adr) {return -1;}
void sc_dum_wr(aymChip* chip, int adr, int data) {}
void sc_dum_sync(aymChip* chip, int ns) {}
sndPair sc_dum_vol(aymChip* chip) {sndPair vol = {0,0}; return vol;}

static const scDesc snd_chip_tab[] = {
	{SND_AY, "AY-3-8910", "AY8910", 1.773447, ay_reset, ay_rd, ay_wr, ay_sync, ay_vol},
	{SND_YM, "Yamaha-2149", "YM2149", 1.75, ay_reset, ym_rd, ym_wr, ay_sync, ym_vol},
	{SND_YM2203, "Yamaha-2203", "YM2203", 3.5, ym2203_reset, ym2203_rd, ym2203_wr, ym2203_sync, ym_vol},
	{SND_NONE, "Dummy", "NULL", 1.0, sc_dum_res, sc_dum_rd, sc_dum_wr, sc_dum_sync, sc_dum_vol}
};

const scDesc* find_chip_type(int id) {
	int i = 0;
	while((snd_chip_tab[i].id != SND_NONE) && (snd_chip_tab[i].id != id))
		i++;
	return &snd_chip_tab[i];
}

void chip_set_type(aymChip* chip, int id) {
	const scDesc* dsc = find_chip_type(id);
	ay_flush(chip);			// the tick rate is about to move
	chip->type = dsc->id;
	chip->res = dsc->res;
	chip->rd = dsc->rd;
	chip->wr = dsc->wr;
	chip->sync = dsc->sync;
	chip->vol = dsc->vol;
	if (chip->frq == 0)
		chip->frq = dsc->frq;
	if (chip->frq == 0)
		chip->frq = 1.0;
	// a tick is a half period, so twice the clock; frq is in MHz, and the
	// 4294967.296 is 2^32 / 1000 (see tickFx)
	chip->tickFx = (long long)(chip->frq * 2 * 4294967.296);
	chip->tickAcc = 0;
}

// A chip's clock against an AY's: the SSG half of a YM2203 divides its own by two.
int chip_frq_mul(int type) {
	return (type == SND_YM2203) ? 2 : 1;
}

// The clock of all the chips, in MHz, as an AY's: 0 is Auto, half the cpu's
// base crystal, the way the boards divide it.
void ts_set_frq(TSound* ts, double frq, double base) {
	aymChip* chip[3] = {ts->chipA, ts->chipB, ts->chipC};
	ts->frq = (frq > 0) ? frq : 0;
	for (int i = 0; i < 3; i++) {
		double f = ((ts->frq > 0) ? ts->frq : base / 2) * chip_frq_mul(chip[i]->type);
		chip[i]->frq = (f > 0) ? f : 0;		// 0: the type's own clock
		chip_set_type(chip[i], chip[i]->type);
	}
}

void chip_set_xdev(aymChip* chip, ayxrd rcb, ayxwr wcb, void* ptr) {
	chip->xptr = ptr;
	chip->xrd = rcb;
	chip->xwr = wcb;
}

aymChip* aymCreate(int tp) {
	aymChip* chip = (aymChip*)malloc(sizeof(aymChip));
	memset(chip, 0x00, sizeof(aymChip));
	chip_set_type(chip, tp); // aymSetType(ay,tp);
	chip->stereo = AY_MONO;
	chip->sep = 75;		// see ay_mix_stereo()
	chip->xrd = NULL;
	chip->xwr = NULL;
	return chip;
}

void aymDestroy(aymChip* chip) {
	ym2203_free(chip);
	free(chip);
}

void aymResetChan(aymChan* ch) {
	ch->per = 16;		// period 1: 16T
	ch->step = 0;
	ch->vol = 0;
	ch->cnt = 0;
	ch->lev = 0;
	ch->tdis = 1;
	ch->ndis = 1;
	ch->een = 0;
}

// TurboSound

TSound* tsCreate(int tp,int tpA,int tpB) {
	TSound* ts = (TSound*)malloc(sizeof(TSound));
	if (ts) {
		memset(ts, 0x00, sizeof(TSound));
		ts->type = tp;
		ts->chipA = aymCreate(tpA);
		ts->chipB = aymCreate(tpB);
		ts->chipC = aymCreate(SND_NONE);
		ts->chipD = aymCreate(SND_NONE);
		ts->mute_l = 0;
		ts->mute_r = 0;
	}
	return ts;
}

void tsDestroy(TSound* ts) {
	aymDestroy(ts->chipA);
	aymDestroy(ts->chipB);
	aymDestroy(ts->chipC);
	aymDestroy(ts->chipD);
	free(ts);
}

// an empty socket has nothing to count, and this runs once per instruction
void tsSync(TSound* ts, int ns) {
	if (ts->chipA->type != SND_NONE) ts->chipA->sync(ts->chipA, ns);
	if (ts->chipB->type != SND_NONE) ts->chipB->sync(ts->chipB, ns);
	if (ts->chipC->type != SND_NONE) ts->chipC->sync(ts->chipC, ns);
	if (ts->chipD->type != SND_NONE) ts->chipD->sync(ts->chipD, ns);
}

// The one chip type with state of its own outside the struct is the YM2203;
// xstate.c asks here rather than knowing that.

aymChip* ts_chip(TSound* ts, int n) {
	switch (n) {
		case 0: return ts->chipA;
		case 1: return ts->chipB;
		case 2: return ts->chipC;
		case 3: return ts->chipD;
	}
	return NULL;
}

int ts_state_range(TSound* ts, int n, void** ptr) {
	aymChip* chip = ts_chip(ts, n);
	return chip ? ym2203_state_size(chip, ptr) : 0;
}

void ts_state_capture(TSound* ts) {
	int i;
	for (i = 0; i < 4; i++)
		ym2203_state_pack(ts_chip(ts, i));
}

void ts_state_restore(TSound* ts) {
	int i;
	for (i = 0; i < 4; i++)
		ym2203_state_unpack(ts_chip(ts, i));
}

// The SSG halves are levels in 0..XMAXVOL and go through the soft clip, the way
// two AYs have always been mixed here. The FM halves are signed and sit on top
// of it: on the board they reach the summing node through their own resistor,
// the same one a hard panned SSG channel has.
sndPair tsGetVolume(TSound* ts) {
	aymChip* chip = ts->chipA;
	sndPair res = chip->vol(chip);
	int i, fm = ym2203_fm_out(chip);
	// an empty socket puts out nothing, and the soft clip leaves a level it is
	// mixed with exactly as it was, so it is left out of the sum
	for (i = 1; i < 4; i++) {
		chip = ts_chip(ts, i);
		if (chip->type == SND_NONE) continue;
		res = mixer(res, chip->vol(chip));
		fm += ym2203_fm_out(chip);
	}
	res.left += fm;
	res.right += fm;
	return res;
}

void tsReset(TSound* ts) {
	ts->chipA->res(ts->chipA);
	ts->chipB->res(ts->chipB);
	ts->chipC->res(ts->chipC);
	ts->chipD->res(ts->chipD);
	ts->curChip = ts->chipA;
	ts->mute_l = 0;
	ts->mute_r = 0;
	ts->r_stat = 0;		// read chip registers, as any AY does. TSFM switches this on itself
}

void tsSetRomSize(TSound* ts, int sz) {
	if (sz < 1) {
		if (ts->rom.data) {
			free(ts->rom.data);
			ts->rom.data = NULL;
			ts->rom.size = 0;
			ts->rom.mask = 0;
		}
	} else {
		if (sz < MEM_256) {
			sz = MEM_256;
		} else if (sz > MEM_128K) {
			sz = MEM_128K;
		} else {
			int s = MEM_256;
			while (s < sz) {
				s <<= 1;
			}
			sz = s;
		}
		ts->rom.data = realloc(ts->rom.data, sz);
		ts->rom.size = sz;
		ts->rom.mask = sz - 1;
	}
}

void tsLoadRom(TSound* ts, const char* path) {
	FILE* file = fopen(path, "rb");
	if (file) {
		fseek(file, 0, SEEK_END);
		int sz = ftell(file);
		rewind(file);
		tsSetRomSize(ts, sz);
		fread(ts->rom.data, ts->rom.size, 1, file);
		fclose(file);
	} else {
		tsSetRomSize(ts, 0);
	}
}

// fffd (a14 = 1)	wr:reg.num	rd:reg.data
// bffd (a14 = 0)	wr:reg.data	rd:FF

int tsIn(TSound* ts, int port) {
	int res = -1;
	if (ts->r_stat && (ts->type == TS_NEDOPC)) {	// read status (TSFM only)
		res = ts->curChip->reg[0xff] & 3;
		if (ts->curChip->wait > 0) res |= 0x80;
	} else {				// read registers
		res = ts->curChip->rd(ts->curChip, (port >> 14) & 1);
	}
	return res;
}

void tsOut(TSound* ts, int port, int val) {
	port >>= 14;
	if (port & 1) {
		switch (ts->type) {
			case TS_NEDOPC:
				if ((val & 0xf8) == 0xf8) {
					// bit 0 is the chip number, not a flag: #FE picks the first and
					// #FF the second, which is what UnrealSpeccy does with the same
					// scheme and what nedoPC's own FM test expects of #F8.
					ts->curChip = (val & 1) ? ts->chipB : ts->chipA;
					ts->r_stat = !(val & 2);
					ts->chipA->blk_fm = !!(val & 4);
					ts->chipB->blk_fm = !!(val & 4);
					ts->chipC->blk_fm = !!(val & 4);
				} else {
					ts->curChip->wr(ts->curChip, port, val);
				}
				break;
			case TS_ZXNEXT:
				if ((val & 0x9c) == 0x9c) {
					// The scheme came from the ZX Next, the SID on index 0
					// did not: chipD is a chip with no type, so what is
					// written to it goes nowhere - which is what an empty
					// socket does.
					switch (val & 3) {
						case 0: ts->curChip = ts->chipD; break;		// sid, never fitted
						case 1: ts->curChip = ts->chipC; break;		// psg 2
						case 2: ts->curChip = ts->chipB; break;		// psg 1
						case 3: ts->curChip = ts->chipA; break;		// psg 0 (default)
					}
					ts->mute_l = (val & 0x40) ? 0 : 1;
					ts->mute_r = (val & 0x20) ? 0 : 1;
				} else {
					ts->curChip->wr(ts->curChip, port, val);
				}
				break;
			default:
				ts->curChip->wr(ts->curChip, port, val);
				break;
		}

	} else {
		ts->curChip->wr(ts->curChip, port, val);
	}
}

int tsReadRom(TSound* ts, int adr) {
	int r;
	if (ts->rom.size > adr) {
		r = ts->rom.data[adr];
	} else {
		r = 0x00;
	}
	return r;
}

// overall

// obsolete
void initNoise() {
	int cur = 0xffff;
	int lev;
	int i;
	for (i=0; i<0x20000; i++) {
		lev = (cur & 0x10000) ? 1 : 0;
		noizes[i] = lev;
		cur = ((cur << 1) + ((lev == (((cur & 0x2000) == 0x2000) ? 1 : 0)) ? 0 : 1)) & 0x1ffff;
	}
}

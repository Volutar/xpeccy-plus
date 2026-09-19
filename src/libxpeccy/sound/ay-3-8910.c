#include <string.h>

#include "ayym.h"

extern void aymResetChan(aymChan* ch);
static const int ay_val_mask[16] = {0xff,0x0f,0xff,0x0f,0xff,0x0f,0x1f,0xff,0x1f,0x1f,0x1f,0xff,0xff,0x0f,0xff,0xff};

static int ayDACvol[32] = {0x0000,0x0000,0x00D0,0x00D0,0x0130,0x0130,0x01BC,0x01BC,
                    0x0291,0x0291,0x03C4,0x03C4,0x0544,0x0544,0x089F,0x089F,
                    0x0A27,0x0A27,0x1053,0x1053,0x16C8,0x16C8,0x1C96,0x1C96,
                    0x2417,0x2417,0x2D54,0x2D54,0x35E8,0x35E8,0x3FFF,0x3FFF};

void ay_reset(aymChip* chip) {
	memset(chip->reg, 0x00, 256);
	aymResetChan(&chip->chanA);
	aymResetChan(&chip->chanB);
	aymResetChan(&chip->chanC);
	aymResetChan(&chip->chanE);
	aymResetChan(&chip->chanN);
	chip->chanN.per = 1;
	chip->chanN.step = 0xffff;
}

int ay_rd(aymChip* ay, int adr) {
	unsigned char res = 0xff;
	if (adr & 1) {
		switch(ay->curReg & 0x0f) {					// AY:16 registers + mirrors
			case 14:
				if (!(ay->reg[7] & 0x40)) {
					//res = ay->reg[14];
					res = ay->xrd ? ay->xrd(0, ay->xptr) : 0xff;
				} else {
					res = 0x00;
				}
				break;
			case 15:
				if (!(ay->reg[7] & 0x80)) {
					// res = ay->reg[15];
					res = ay->xrd ? ay->xrd(1, ay->xptr) : 0xff;
				} else {
					res = 0x00;
				}
				break;
			default:
				res = ay->reg[ay->curReg];
				res &= ay_val_mask[ay->curReg & 0x0f];		// AY:reset unused bits
				break;
		}
	}
	return res;
}

void ay_set_reg(aymChip* chip, int val) {
	int tone;
	if ((chip->curReg != 14) && (chip->curReg != 15))
		chip->reg[chip->curReg] = val & 0xff;
	switch (chip->curReg) {
		case 0x00:
		case 0x01:
			tone = chip->reg[0] | ((chip->reg[1] & 0x0f) << 8);
			if (tone == 0) tone++;
			chip->chanA.per = tone << 4;		// min 16T on half-period
			break;
		case 0x02:
		case 0x03:
			tone = chip->reg[2] | ((chip->reg[3] & 0x0f) << 8);
			if (tone == 0) tone++;
			chip->chanB.per = tone << 4;
			break;
		case 0x04:
		case 0x05:
			tone = chip->reg[4] | ((chip->reg[5] & 0x0f) << 8);
			if (tone == 0) tone++;
			chip->chanC.per = tone << 4;
			break;
		case 0x06:					// noise
			tone = val & 0x1f;
			if (tone == 0) tone++;
			chip->chanN.per = tone << 5;		// min 16T x2 half-periods
			break;
		case 0x07:
			chip->chanA.tdis = (val & 1) ? 1 : 0;
			chip->chanB.tdis = (val & 2) ? 1 : 0;
			chip->chanC.tdis = (val & 4) ? 1 : 0;
			chip->chanA.ndis = (val & 8) ? 1 : 0;
			chip->chanB.ndis = (val & 16) ? 1 : 0;
			chip->chanC.ndis = (val & 32) ? 1 : 0;
			break;
		case 0x08:
			chip->chanA.vol = ((val & 15) << 1) | 1;
			chip->chanA.een = (val & 16) ? 1 : 0;
			break;
		case 0x09:
			chip->chanB.vol = ((val & 15) << 1) | 1;
			chip->chanB.een = (val & 16) ? 1 : 0;
			break;
		case 0x0a:
			chip->chanC.vol = ((val & 15) << 1) | 1;
			chip->chanC.een = (val & 16) ? 1 : 0;
			break;
		case 0x0b:
		case 0x0c:
			tone = chip->reg[11] | (chip->reg[12] << 8);
			if (tone == 0) tone++;
			chip->chanE.per = tone << 4;
			break;
		case 0x0d:
			chip->eForm = val & 0x0f;
			chip->chanE.cnt = 0;					// only if form changed?
			chip->chanE.vol = (val & 4) ? 0 : 31;
			chip->chanE.step = (val & 4) ? 1 : -1;
			break;
		case 0x0e:
			if (chip->reg[7] & 0x40) {
				chip->reg[14] = val & 0xff;
				if (chip->xwr) chip->xwr(0, val, chip->xptr);
			}
			break;
		case 0x0f:
			if (chip->reg[7] & 0x80) {
				chip->reg[15] = val & 0xff;
				if (chip->xwr) chip->xwr(0, val, chip->xptr);
			}
			break;
	}
}

void ay_wr(aymChip* chip, int adr, int val) {
	if (adr & 1) {								// set current reg
		chip->curReg = val & 0x0f;					// AY:16 registers + mirrors
	} else {								// write data
		ay_set_reg(chip, val);
	}
}

void ay_tick(aymChip* ay) {
	if (++ay->chanA.cnt >= ay->chanA.per) {
		ay->chanA.cnt = 0;
		ay->chanA.lev ^= 1;
	}
	if (++ay->chanB.cnt >= ay->chanB.per) {
		ay->chanB.cnt = 0;
		ay->chanB.lev ^= 1;
	}
	if (++ay->chanC.cnt >= ay->chanC.per) {
		ay->chanC.cnt = 0;
		ay->chanC.lev ^= 1;
	}
	if (++ay->chanN.cnt >= ay->chanN.per) {
		ay->chanN.cnt = 0;
		ay->chanN.step = (ay->chanN.step << 1) | ((((ay->chanN.step >> 13) ^ (ay->chanN.step >> 16)) & 1) ^ 1);
		ay->chanN.lev = (ay->chanN.step >> 16) & 1;
	}
	if (++ay->chanE.cnt >= ay->chanE.per) {
		ay->chanE.cnt = 0;
		ay->chanE.vol += ay->chanE.step;
		if (ay->chanE.vol & ~31) {				// 32 || -1
			if (ay->eForm & 8) {				// 1xxx
				if (ay->eForm & 1) {			// 1xx1 : 9,B,D,F : stop
					ay->chanE.vol -= ay->chanE.step;
					ay->chanE.step = 0;
					if (ay->eForm & 2) {		// 1x11 : B,F : invert volume
						ay->chanE.vol ^= 0x1f;
					}
				} else if (ay->eForm & 2) {		// 1x10 : A,E : change direction (wave)
					ay->chanE.step = -ay->chanE.step;
					ay->chanE.vol += ay->chanE.step;
				} else {				// 1x00 : 8,C : repeat (saw)
					ay->chanE.vol &= 0x1f;
				}
			} else {					// 0xxx : silent, stop
				ay->chanE.vol = 0;
				ay->chanE.step = 0;
			}
		}
	}
}

void ay_sync(aymChip* ay, int ns) {
	if ((ay->tickFx < 1) || (ns < 1)) return;
	ay->tickAcc += (long long)ns * ay->tickFx;
	long long cnt = ay->tickAcc >> 32;
	ay->tickAcc &= 0xffffffffLL;
	while (cnt-- > 0) {
		ay_tick(ay);
	}
}

sndPair ay_mix_stereo(int volA, int volB, int volC, int id, int sep) {
	int lef,cen,rig;
	sndPair res;
	switch (id) {
		case AY_ABC:
			lef = volA;
			cen = volB;
			rig = volC;
			break;
		case AY_ACB:
			lef = volA;
			cen = volC;
			rig = volB;
			break;
		case AY_BAC:
			lef = volB;
			cen = volA;
			rig = volC;
			break;
		case AY_BCA:
			lef = volB;
			cen = volC;
			rig = volA;
			break;
		case AY_CAB:
			lef = volC;
			cen = volA;
			rig = volB;
			break;
		case AY_CBA:
			lef = volC;
			cen = volB;
			rig = volA;
			break;
		default:
			lef = (volA + volB + volC) / 3;
			cen = lef;
			rig = lef;
			break;
	}
	// sep is how far apart the side channels are: 100 keeps them apart, 0
	// bleeds them into each other until it is mono. the center channel is
	// always split evenly. weights add up to 1024, so the volume stays put
	if (sep < 0) sep = 0;
	if (sep > 100) sep = 100;
	int cenw = 1024 / 3;			// center channel weight
	int bleed = cenw * (100 - sep) / 100;
	int own = 2 * cenw - bleed;
	res.left = (own * lef + cenw * cen + bleed * rig) >> 10;
	res.right = (own * rig + cenw * cen + bleed * lef) >> 10;
	return res;
}

// A period this short is not a gate any more but a tone of its own, so the
// square is replaced by its mean level - in ay_chan_lev() below, and by the
// half in ay_chan_dac(). Both ask here, so it is asked in one place.
// The limit is a frequency and not a register value (period 5 is 22 kHz at
// 1.75 MHz, and per is the register << 4), so slowing the emulation down has
// to move it or tones that have come down into hearing stay silent.
#define AY_SUB_LIMIT	0x60

static int ay_sub_limit = AY_SUB_LIMIT;

void ay_set_speed(double speed) {
	ay_sub_limit = (speed > 0.0) ? (int)(AY_SUB_LIMIT * speed) : AY_SUB_LIMIT;
}

static int ay_sub_period(aymChan* ch) {
	return (ch->per < ay_sub_limit) && !ch->tdis;
}

// The level a channel is putting out, 0..31, before the DAC curve: the
// envelope or the register volume, with whichever of the mixer gates is shut
// silencing it. This is what the debugger shows; ay_chan_dac() below turns it
// into what the DAC does with it.
int ay_chan_lev(aymChip* ay, aymChan* ch) {
	if (ch->mute) return 0;
	if (!(ch->ndis || ay->chanN.lev)) return 0;
	if (!ay_sub_period(ch) && !ch->tdis && !ch->lev) return 0;
	return (ch->een ? ay->chanE.vol : ch->vol) & 0x1f;
}

// One period of every envelope shape, as levels 0..31, for the debugger to
// draw. It is run on a scratch chip rather than written out as a table of its
// own: the shapes then cannot drift from what ay_tick() actually does.
void ay_env_shape(int form, unsigned char* out, int len) {
	aymChip tmp;
	int i;
	memset(&tmp, 0, sizeof(tmp));
	tmp.chanA.per = tmp.chanB.per = tmp.chanC.per = tmp.chanN.per = 0x7fffffff;
	tmp.chanE.per = 1;
	ay_poke_reg(&tmp, 13, form);		// where the form is decoded
	for (i = 0; i < len; i++) {
		out[i] = tmp.chanE.vol & 0x1f;
		ay_tick(&tmp);
	}
}

// Write a register as a port write would, so everything derived from it is set
// too. curReg is put back: the machine may be halfway through its own
// select-then-write pair.
void ay_poke_reg(aymChip* chip, int reg, int val) {
	unsigned char was = chip->curReg;
	chip->curReg = reg & 0xff;
	ay_set_reg(chip, val & 0xff);
	chip->curReg = was;
}

// A level through a DAC table, which is the only thing the two chips do
// differently - the AY and the YM gate a channel alike. On the way it keeps the
// loudest level seen, for the meter in the debugger: a channel is a square wave,
// and a level read once a frame is the volume on one half of it and nothing on
// the other, which reads as a meter falling to zero on a note that is still playing.
int ay_chan_dac(aymChip* ay, aymChan* ch, const int* tab) {
	int lev = ay_chan_lev(ay, ch);
	if (lev > ch->levpk) ch->levpk = lev;
	int vol = tab[lev];
	if (ay_sub_period(ch)) vol >>= 1;			// half
	return vol;
}

int ay_chan_peak(aymChan* ch) {
	int res = ch->levpk;
	ch->levpk = 0;
	return res;
}

// Three channels through a DAC table and into the stereo mix. The AY and the YM
// differ in that table and in nothing else, so this is the whole of both.
sndPair ay_mix_tab(aymChip* chip, const int* tab) {
	int volA = ay_chan_dac(chip, &chip->chanA, tab);
	int volB = ay_chan_dac(chip, &chip->chanB, tab);
	int volC = ay_chan_dac(chip, &chip->chanC, tab);
	return ay_mix_stereo(volA, volB, volC, chip->stereo, chip->sep);
}

sndPair ay_vol(aymChip* chip) {
	return ay_mix_tab(chip, ayDACvol);
}

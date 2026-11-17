// The ZX Spectrum Next's sound: three YM2149s behind the TurboSound select
// (audio/turbosound.vhd, ym2149.vhd), four 8-bit DACs (audio/soundrive.vhd), the
// beeper, and the mixer that adds them up (audio/audio_mixer.vhd). The chips' tone,
// noise and envelope are the AY code's; what is the Next's is here: the ports, the
// register read rules, the level tables and the mix.

#include <string.h>

#include "zxnext.h"
#include "../sound/ayym.h"
#include "../sound/sndcommon.h"

#define SND (NX.snd)

// ym2149.vhd 60-90: the YM's 32 levels and the AY's 16, zero a true zero on both
static const int nxYmTab[32] = {
	0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x06, 0x07, 0x09, 0x0a, 0x0c, 0x0e, 0x11, 0x13,
	0x17, 0x1b, 0x20, 0x25, 0x2c, 0x35, 0x3e, 0x47, 0x54, 0x66, 0x77, 0x88, 0xa1, 0xc0, 0xe0, 0xff
};
static const int nxAyTab16[16] = {
	0x00, 0x03, 0x04, 0x06, 0x0a, 0x0f, 0x15, 0x22, 0x28, 0x41, 0x5b, 0x72, 0x90, 0xb5, 0xd7, 0xff
};
static int nxAyTab[32];

// one Next unit of the 13-bit mix in the app's levels: the beeper's 512 then sits
// where every other machine's beeper does (BEEP_MAX)
#define NX_GAIN		(BEEP_MAX / 512)

static aymChip* nx_ay(Computer* comp, int i) {
	return (i == 0) ? comp->ts->chipA : (i == 1) ? comp->ts->chipB : comp->ts->chipC;
}

// audio_ay_reset (zxnext.vhd 6379): reset, or psg_mode 11 holds the chips in it
static int nx_ay_held(Computer* comp) {
	return (NX.nr[0x06] & 3) == 3;
}

// The registers as a reset leaves them: all 0 but R7, whose I/O ports come up as
// outputs (ym2149.vhd 179-200). The envelope and noise state are left alone.
static void nx_ay_regs_reset(aymChip* chip) {
	for (int r = 0; r < 16; r++)
		ay_poke_reg(chip, r, (r == 7) ? 0xff : 0x00);
	chip->curReg = 0;
}

static void nx_ay_reset(Computer* comp) {
	for (int i = 0; i < 3; i++)
		nx_ay_regs_reset(nx_ay(comp, i));
	SND.sel = 0;
	SND.pan[0] = SND.pan[1] = SND.pan[2] = 3;
	comp->ts->curChip = comp->ts->chipA;
}

static void nx_dac_reset(Computer* comp) {
	memset(SND.dac, 0x80, sizeof(SND.dac));
}

// The DACs changed on edge w. The mixer plays them as they stood when the sample it
// takes was due (comp->sndAgo), not at the end of the step: the copper or the DMA may
// write them many times while the DMA holds the bus, and that is one step.
static void nx_dac_push(Computer* comp, long long w) {
	int n = SND.qN;
	int last = (SND.qHead + n - 1) & (NX_DACQ - 1);
	if (n && (SND.q[last].e >= w)) {
		memcpy(SND.q[last].v, SND.dac, 4);
		return;
	}
	if (n == NX_DACQ) {		// nothing has sampled for a while (fast mode)
		memcpy(SND.out, SND.q[SND.qHead].v, 4);
		SND.qHead = (SND.qHead + 1) & (NX_DACQ - 1);
		n--;
	}
	int i = (SND.qHead + n) & (NX_DACQ - 1);
	SND.q[i].e = w;
	memcpy(SND.q[i].v, SND.dac, 4);
	SND.qN = n + 1;
}

// the changes up to edge t into what the mixer plays
static void nx_dac_play(Computer* comp, long long t) {
	while (SND.qN && (SND.q[SND.qHead].e <= t)) {
		memcpy(SND.out, SND.q[SND.qHead].v, 4);
		SND.qHead = (SND.qHead + 1) & (NX_DACQ - 1);
		SND.qN--;
	}
}

// A reset between steps (w < 0) drops the DACs' queue; one inside a step (NR 02 b0, F4)
// queues their change at edge w, what came before it still played
void nx_snd_reset(Computer* comp, long long w) {
	for (int i = 0; i < 32; i++) nxAyTab[i] = nxAyTab16[i >> 1];
	nx_ay_reset(comp);
	nx_dac_reset(comp);
	if (w < 0) {
		memcpy(SND.out, SND.dac, 4);
		SND.qHead = SND.qN = 0;
	} else {
		nx_dac_push(comp, w);
	}
	nx_snd_fe(comp, NX.pfe);	// the beeper's levels are the Next's
}

// nr_06 or nr_08 written on edge w: entering psg_mode 11 resets the chips, nr_08 b3 = 0
// holds the DACs in reset (zxnext.vhd 6436); the beeper's exclusive and issue 2 terms follow
void nx_snd_nr(Computer* comp, int r, long long w) {
	if ((r == 0x06) && nx_ay_held(comp)) nx_ay_reset(comp);
	if ((r == 0x08) && !(NX.nr[0x08] & 8)) {
		nx_dac_reset(comp);
		nx_dac_push(comp, w);
	}
	nx_snd_fe(comp, NX.pfe);
}

// nr_2C / nr_2D / nr_2E on edge w: DAC B, A and D, C (soundrive.vhd 81-103), not
// gated by nr_84
void nx_snd_dac_nr(Computer* comp, int r, int v, long long w) {
	if (!(NX.nr[0x08] & 8)) return;
	switch (r) {
		case 0x2c: SND.dac[1] = v; break;
		case 0x2d: SND.dac[0] = v; SND.dac[3] = v; break;
		case 0x2e: SND.dac[2] = v; break;
	}
	nx_dac_push(comp, w);
}

// What the selected chip answers with (ym2149.vhd 217-254): at BFF5 its id and
// register number; R16-R31 FF in YM mode, a mirror in AY mode; R14/R15 FF while their
// port is an input; in AY mode the bits an AY does not have read 0
static int nx_ay_read(Computer* comp, int bff5) {
	aymChip* chip = nx_ay(comp, SND.sel);
	int adr = chip->curReg & 0x1f;
	int ay = NX.nr[0x06] & 1;
	if (bff5) return ((3 - SND.sel) << 6) | adr;
	if ((adr & 0x10) && !ay) return 0xff;
	int r = adr & 15;
	int v = chip->reg[r];
	if ((r == 14) && !(chip->reg[7] & 0x40)) return 0xff;
	if ((r == 15) && !(chip->reg[7] & 0x80)) return 0xff;
	if (ay) {
		switch (r) {
			case 1: case 3: case 5: case 13: v &= 0x0f; break;
			case 6: case 8: case 9: case 10: v &= 0x1f; break;
		}
	}
	return v;
}

// An i/o read of the sound ports (zxnext.vhd 2647-2649, 2771-2773); -1 when none answers
int nx_snd_in(Computer* comp, int port) {
	if (!(NX.nr[0x84] & 1)) return -1;
	if ((port & 0xc007) == 0xc005) return nx_ay_read(comp, 0);		// FFFD
	if ((port & 0xc00f) == 0x8005) return nx_ay_read(comp, 1);		// BFF5
	if (((port & 0xc00f) == 0x800d) && (NX.timing == NXT_P3))		// BFFD, +3 timing only
		return nx_ay_read(comp, 0);
	return -1;
}

static void nx_ay_data(Computer* comp, int val) {
	aymChip* chip = nx_ay(comp, SND.sel);
	int r = chip->curReg;
	if (r & 0x10) return;			// R16-R31 take nothing
	ay_poke_reg(chip, r, val);
	// env_reset starts the envelope on its first value and the step process takes the
	// next at the following enable (ym2149.vhd 337-403): the first value is out for at
	// most one PSG clock, two of the AY code's ticks
	if (r == 13) {
		chip->chanE.cnt = (chip->chanE.per > 2) ? chip->chanE.per - 2 : 0;
		if (ay_log) ay_log(chip, AYL_ECNT, chip->chanE.cnt, 0);
	}
}

static void nx_dac_out(Computer* comp, int lo, int val, long long w) {
	unsigned char* nr = NX.nr;
	if (!(nr[0x08] & 8)) return;
	int m = 0;			// bit n: channel n (A B C D)
	switch (lo) {
		case 0x1f: if (nr[0x84] & 0x02) m = 1; break;
		case 0x0f: if (nr[0x84] & 0x12) m = 2; break;
		case 0x4f: if (nr[0x84] & 0x12) m = 4; break;
		case 0x5f: if (nr[0x84] & 0x0a) m = 8; break;
		case 0x3f: if (nr[0x84] & 0x08) m = 1; break;
		case 0xf1: if (nr[0x84] & 0x04) m = 1; break;
		case 0xf3: if (nr[0x84] & 0x04) m = 2; break;
		case 0xf9: if (nr[0x84] & 0x04) m = 4; break;
		case 0xfb: m = (nr[0x84] & 0x04) ? 8 : (nr[0x84] & 0x20) ? 9 : 0; break;
		case 0xb3: if (nr[0x84] & 0x40) m = 6; break;
		case 0xdf: if (nr[0x84] & 0x80) m = 9; break;
	}
	if (!m) return;
	for (int i = 0; i < 4; i++)
		if (m & (1 << i)) SND.dac[i] = val;
	nx_dac_push(comp, w);
}

// An i/o write to the sound ports. FFFD: D7:5 = 000 selects a register on the chip
// selected; with TurboSound on, 1xx111nn picks the chip and its L/R enables
// (turbosound.vhd 118-150). BFFD (and BFF5, inside its decode) writes the register.
// w: the CLK_28 edge iowr is taken on.
void nx_snd_out(Computer* comp, int port, int val, long long w) {
	unsigned char* nr = NX.nr;
	nx_dac_out(comp, port & 0xff, val, w);
	if (!(nr[0x84] & 1) || nx_ay_held(comp)) return;
	int dffd = ((port & 0xf003) == 0xd001) && (nr[0x82] & 4);
	if (((port & 0xc007) == 0xc005) && !dffd) {
		if (!(val & 0xe0)) {
			nx_ay(comp, SND.sel)->curReg = val & 0x1f;
		} else if (((val & 0x9c) == 0x9c) && (nr[0x08] & 2)) {
			int n = val & 3;
			SND.sel = (n == 2) ? 1 : (n == 1) ? 2 : 0;
			SND.pan[SND.sel] = (val >> 5) & 3;
			comp->ts->curChip = nx_ay(comp, SND.sel);
		}
	} else if ((port & 0xc007) == 0x8005) {
		nx_ay_data(comp, val);
	}
}

// Port FE: the beeper (bit 4, 512 in the mix) and MIC (bit 3, 128; on issue 3
// keyboards only, zxnext.vhd 6503), both gone from the main mix in exclusive internal
// speaker mode (6504). The bit channel's 0..255 stands for 0..640.
void nx_snd_fe(Computer* comp, int val) {
	bitChan* ch = comp->beep;
	int excl = (NX.nr[0x06] & 0x40) && (NX.nr[0x08] & 0x10);
	int mic = (val & 8) && !(NX.nr[0x08] & 1) && !excl;
	ch->lev = ((val >> 4) & 1) && !excl;
	ch->lo = mic ? 51 : 0;
	ch->hi = 204 + ch->lo;
}

// A channel's level through the table. A fixed volume 0 is level 0 on the Next
// (ym2149.vhd: V = 0 -> 0, else 2V+1), where the AY code keeps index 1.
static int nx_chan(aymChip* chip, aymChan* ch, int i, const int* tab) {
	if (!ch->een && !(chip->reg[8 + i] & 15)) return 0;
	return ay_chan_dac(chip, ch, tab);
}

// all three channels at a fixed volume 0: nothing to hear, nothing to count yet
static int nx_ay_silent(aymChip* chip) {
	return !((chip->reg[8] | chip->reg[9] | chip->reg[10]) & 0x1f);
}

// The chip counted up to late ns before the time handed to it so far; the rest stays
// for the next look. Their clock runs off CLK_28 whatever holds the bus (zxnext_top:
// CLK_28_PSG_EN), so a sample inside a DMA transfer, which is one step here, hears
// them where they were at its own time, not at the transfer's end.
static void nx_ay_flush(aymChip* chip, int late) {
	int keep = (late < chip->pendNs) ? late : chip->pendNs;
	if (keep <= 0) {
		ay_flush(chip);
		return;
	}
	chip->pendNs -= keep;
	ay_flush(chip);
	chip->pendNs = keep;
}

// The chips' clock comes off CLK_28 too: the machine's AY clock at VGA 0, faster with
// the video timing (Options set the first; the second only the board knows). Set
// at every sample, before the chips are heard, and worked out again only when one
// of those moves
static void nx_ay_retune(Computer* comp) {
	static double wantTs = -1, wantCpu = -1, want = 0;
	static int wantMode = -1;
	if ((comp->ts->frq != wantTs) || (comp->cpuFrq != wantCpu) || (NX.vmode != wantMode)) {
		wantTs = comp->ts->frq;
		wantCpu = comp->cpuFrq;
		wantMode = NX.vmode;
		want = ((comp->ts->frq > 0) ? comp->ts->frq : comp->cpuFrq / 2) / nx_clk_scale(comp);
	}
	for (int i = 0; i < 3; i++) {
		aymChip* chip = nx_ay(comp, i);
		double f = want * ((chip->type == SND_YM2203) ? 2 : 1);	// chip_frq_mul()
		if (chip->frq != f) {
			chip->frq = f;
			chip_set_type(chip, chip->type);
		}
	}
}

// the mixer's settings as they stand, for the mix
static void nx_mix_take_cfg(sndMix* m, Computer* comp) {
	m->ts = comp->ts;
	m->in.nr06 = NX.nr[0x06];
	m->in.nr08 = NX.nr[0x08];
	m->in.nr09 = NX.nr[0x09];
	m->in.pan[0] = SND.pan[0];
	m->in.pan[1] = SND.pan[1];
	m->in.pan[2] = SND.pan[2];
	m->in.sel = SND.sel;
}

// The AYs' sum on each side (turbosound.vhd 186-205, 323-337), in the mixer's units,
// late ns before the time the chips have been handed
static void nx_ay_sum(const sndMix* m, int late, int* l, int* r) {
	const int* tab = (m->in.nr06 & 1) ? nxAyTab : nxYmTab;
	int ts = m->in.nr08 & 2;
	int acb = m->in.nr08 & 0x20;
	int al = 0, ar = 0;
	for (int i = 0; i < 3; i++) {
		aymChip* chip = (i == 0) ? m->ts->chipA : (i == 1) ? m->ts->chipB : m->ts->chipC;
		if ((!ts && (i != m->in.sel)) || nx_ay_silent(chip)) continue;
		nx_ay_flush(chip, late);
		int a = nx_chan(chip, &chip->chanA, 0, tab);
		int b = nx_chan(chip, &chip->chanB, 1, tab);
		int c = nx_chan(chip, &chip->chanC, 2, tab);
		int mono = (m->in.nr09 >> (5 + i)) & 1;
		int lsum = ((acb || mono) ? c : b) + a;
		int rsum = (mono ? lsum : c) + b;
		if (m->in.pan[i] & 2) al += mono ? rsum : lsum;
		if (m->in.pan[i] & 1) ar += rsum;
	}
	*l = al;
	*r = ar;
}

static void nx_ay_mix(Computer* comp, int late, int* l, int* r) {
	sndMix m;
	nx_ay_retune(comp);
	nx_mix_take_cfg(&m, comp);
	nx_ay_sum(&m, late, l, r);
}

// What audio_mixer.vhd adds up, pcm_L/R: the beeper's ear 512 and mic 128, the AYs,
// 4 x the DACs and the Pi's I2S at its silence of 512; with psg0's env_vol, for a bench
void next_audio(Computer* comp, int* l, int* r, int* env) {
	bitChan* bc = comp->beep;
	int beep = (bc->lev ? bc->hi : bc->lo) * 640 / 255;
	int al, ar;
	nx_ay_mix(comp, 0, &al, &ar);
	*l = beep + al + (SND.dac[0] + SND.dac[1]) * 4 + 512;
	*r = beep + ar + (SND.dac[2] + SND.dac[3]) * 4 + 512;
	*env = comp->ts->chipA->chanE.vol;
}

// hwQuiet.sndLate: nx_vol hears the AYs and the DACs at the sample's own time, and
// in a HALT run-on nothing writes them; the beeper settled and a tape standing still
// read the same at any time. The run may then pass the samples. Not with the copper
// running: nx_vol runs it up to the time it is called, and a late sample would see
// the mixer's nextregs the copper writes after its own time
int nx_snd_late(Computer* comp) {
	if (nx_cop_on(comp)) return 0;
	bitChan* bc = comp->beep;
	if ((bc->val != (bc->lev ? bc->hi : bc->lo)) || (bc->perH && bc->perL)) return 0;
	return !comp->tape->on;
}

// The mix (audio_mixer.vhd 83-103): ear + mic + the AYs + 4 x the DACs (+ the Pi's
// I2S, a constant 512 while it is off), each side 13 bits unsigned. Each part takes
// its own volume from the options and its own dc blocker, as zx_vol does.
// The mix of what nx_vol gathered: the tape and the beeper, the AYs at m->late, the DACs
sndPair nx_mix(sndMix* m, sndVolume* sv) {
	int dc = sv->dc;
	int type = snd_board_type(m->in.beepType, sv);	// the Next's own is line out
	sndPair res, part;
	// the tape and the beeper
	int lev = snd_tape_level(m, sv);
	// the beeper's share and the DACs' below move only with what they are made of:
	// worked out again then, not at every sub-sample
	if ((m->in.beep != m->bVal) || (sv->beep != m->bVol)) {
		m->bVal = m->in.beep;
		m->bVol = sv->beep;
		m->bLev = m->bVal * 640 * NX_GAIN / 255 * m->bVol / 100;
	}
	lev += m->bLev;
	// ...out through the Board filter picked on the Sound page, as zx_mix's
	lev = snd_beep_flt(&m->bflt, lev, type, m->rate);
	part.left = part.right = lev;
	res = snd_dc(&m->dc[0], part, dc);
	// the AYs, at the time the sample was due
	int al, ar;
	nx_ay_sum(m, m->late, &al, &ar);
	// the Sound page's separation (chipA->sep); the layout stays what the software set
	sndPair ay = {al, ar};
	ay = snd_sep(ay, m->ts->chipA->sep);
	part.left = ay.left * NX_GAIN * sv->ay / 100;
	part.right = ay.right * NX_GAIN * sv->ay / 100;
	part = snd_dc(&m->dc[1], snd_mix_flt(m->pflt, part, type, m->rate), dc);
	res.left += part.left;
	res.right += part.right;
	// the DACs (soundrive.vhd 109-115), around their idle 80+80
	unsigned int out = m->in.dac[0] | (m->in.dac[1] << 8) | (m->in.dac[2] << 16) | ((unsigned int)m->in.dac[3] << 24);
	if ((out != m->dOut) || (sv->sdrv != m->dVol)) {
		m->dOut = out;
		m->dVol = sv->sdrv;
		m->dL = (m->in.dac[0] + m->in.dac[1] - 0x100) * 4 * NX_GAIN * m->dVol / 100;
		m->dR = (m->in.dac[2] + m->in.dac[3] - 0x100) * 4 * NX_GAIN * m->dVol / 100;
	}
	part.left = m->dL;
	part.right = m->dR;
	part = snd_dc(&m->dc[2], part, dc);
	res.left += part.left;
	res.right += part.right;
	return res;
}

// What changes the machine is done here, at the sample's time: the copper run up to
// it, the AYs retuned, the DACs played up to it; then what the mix reads is gathered
void nx_snd_take(Computer* comp, sndMix* m) {
	// the copper writes the DACs and the mixer's nextregs: its moves up to the dot under
	// the ray count here, as the video, which runs it, may not have walked that far yet
	// (the clock counted from the T says when it is still asleep, without the ray)
	if (nx_cop_on(comp)) {
		long long ts = comp->tickTotal + comp->cpu->t;
		if (NX.copN || (NX.refT < 0) || (NX.refC + (ts - NX.refT) * (8 >> NX.speed) + 8 >= NX.copWake))
			nx_cop_run(comp, nx_now(comp) & ~3LL);
	}
	snd_mix_take_wire(m, comp);
	// the AYs are heard at the time the sample was due (comp->sndAgo)
	m->late = (comp->sndAgo > 0) ? (int)FIXED_TO_NS(comp->sndAgo) : 0;
	nx_ay_retune(comp);
	nx_mix_take_cfg(m, comp);
	// the DACs as they stood then
	if (SND.qN) {
		long long t = nx_now(comp);
		if (comp->sndAgo > 0) t -= comp->sndAgo * 4 / comp->vid->nsPerDotFixed;
		nx_dac_play(comp, t);
	}
	memcpy(m->in.dac, SND.out, sizeof(m->in.dac));
}

sndPair nx_vol(Computer* comp, sndVolume* sv) {
	static sndMix m = {.bVal = -1, .bVol = -1, .dOut = 0xffffffff, .dVol = -1};
	nx_snd_take(comp, &m);
	return nx_mix(&m, sv);
}

// YM2203 (OPN): the FM half on ymfm, the SSG half on the AY code next door.
//
// The class used here is ymfm's fm_engine_base, not its ymfm::ym2203. That one
// bolts an SSG onto the engine and resamples both halves to one output rate -
// work that would only have to be undone, because a YM2203 here has to stay
// "the AY code plus FM": the panning, the volume, the register view and the
// debugger's AY page all read the aymChip the AY half fills in. The engine on
// its own also takes a channel mask on every clock and output call, which is
// what the debugger's per-channel mute needs.

#include <stdint.h>
#include <string.h>

#include "ymfm/ymfm_opn.h"	// instantiates the engine for us, at its end

extern "C" {
#include "ayym.h"

// the SSG half, from ay-3-8910.c
void ay_tick(aymChip*);
void ay_set_reg(aymChip*, int);
}

// ymfm hands out the three FM channels already summed, while ay_mix_stereo()
// averages its three - so an AY here is worth one channel at full, not three,
// and the sum has to be divided by three to sit on the same scale. Taken at
// face value one chip reaches 40956, past what the 16 bit output holds.
#define FM_DIV	3

namespace {

class xfm : public ymfm::ymfm_interface {
public:
	typedef ymfm::fm_engine_base<ymfm::opn_registers> engine;

	xfm(aymChip* chip) : m_chip(chip), m_fm(*this) {
		reset();
	}

	// --- ymfm_interface. The durations are in master clocks: update_timer()
	// multiplies the register period by OPERATORS * prescale, and the busy
	// count is 32 * prescale.

	void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override {
		if (duration_in_clocks < 0) {
			m_timer_on[tnum] = 0;
		} else {
			m_timer_at[tnum] = m_clocks + duration_in_clocks;
			m_timer_on[tnum] = 1;
		}
	}

	void ymfm_set_busy_end(uint32_t clocks) override {
		m_busy_end = m_clocks + clocks;
	}

	bool ymfm_is_busy() override {
		return (int64_t)(m_busy_end - m_clocks) > 0;
	}

	// TSFM has no wire from the chip's IRQ pin to anything: the status byte
	// is read back through #fffd instead.
	void ymfm_update_irq(bool) override {}

	// --- the chip

	void reset() {
		// the clock first: the engine's own reset writes the mode register,
		// which is where a timer would be set from
		m_clocks = 0;
		m_frac = 0;
		m_fmcnt = 0;
		m_busy_end = 0;
		m_timer_on[0] = m_timer_on[1] = 0;
		m_adr = 0;
		m_last = 0;
		m_fm.set_clock_prescale(6);	// a reset picks /6; regs 2d..2f change it
		set_period();
		m_fm.reset();
		status();
	}

	void write_addr(int val) {
		m_adr = val & 0xff;
		// the prescaler answers to the address alone, no data write needed
		if (m_adr == 0x2d) set_prescale(6);
		else if ((m_adr == 0x2e) && (m_fm.clock_prescale() == 6)) set_prescale(3);
		else if (m_adr == 0x2f) set_prescale(2);
	}

	void write_data(int val) {
		m_fm.write(m_adr, val & 0xff);
		ymfm_set_busy_end(32 * m_fm.clock_prescale());
		status();
	}

	// the status byte as the chip would give it, into the two fields tsIn()
	// builds its answer out of
	void status() {
		int st = m_fm.status();
		if (ymfm_is_busy()) st |= engine::STATUS_BUSY;
		m_chip->reg[0xff] = st & 3;
		m_chip->wait = (st & engine::STATUS_BUSY) ? 1 : 0;
	}

	// advance by ns nanoseconds of emulated time
	void run(int ns) {
		if (ns <= 0) return;
		// tickFx is half periods per ns in 32.32, so half of it is the
		// master clock (see chip_set_type)
		m_frac += (uint64_t)ns * (uint64_t)(m_chip->tickFx >> 1);
		uint64_t left = m_frac >> 32;
		int fired = 0;
		m_frac &= 0xffffffffULL;
		while (left > 0) {
			uint64_t step = m_fmper - m_fmcnt;	// to the next fm sample
			if (step > left) step = left;
			m_clocks += step;
			m_fmcnt += step;
			left -= step;
			// the SSG: ay_tick() is a half period of its own clock, and
			// that clock is the master one over prescale/3 (so the /6 the
			// chip resets into gives the 1.75 MHz an AY has at 3.5)
			for (uint64_t n = step * m_ssgmul; n > 0; n--)
				ay_tick(m_chip);
			if (m_fmcnt < m_fmper) continue;
			m_fmcnt = 0;
			for (int t = 0; t < 2; t++) {
				if (m_timer_on[t] && ((int64_t)(m_clocks - m_timer_at[t]) >= 0)) {
					m_timer_on[t] = 0;	// engine_timer_expired re-arms it
					m_fm.engine_timer_expired(t);
					fired = 1;
				}
			}
			// every channel is clocked whatever the debugger mutes, so a
			// muted one comes back where it would have been
			m_fm.clock(engine::ALL_CHANNELS);
			if (m_chip->blk_fm) {		// TSFM has the fm half muted
				m_last = 0;
				continue;
			}
			m_out.clear();
			m_fm.output(m_out, 0, 32767, chanmask());
			m_out.roundtrip_fp();		// the DAC's 10.3 float
			m_last = m_out.data[0];
		}
		// nothing else can have moved the status byte, and this runs once
		// per cpu instruction
		if (fired || m_chip->wait) status();
	}

	int out() const { return m_last; }

	// fill the debugger's view of the fm state
	void view(fmChan* out);

private:
	void set_prescale(uint32_t pre) {
		if (pre == m_fm.clock_prescale()) return;
		m_fm.set_clock_prescale(pre);
		set_period();
	}

	void set_period() {
		uint32_t pre = m_fm.clock_prescale();
		m_fmper = pre * engine::OPERATORS;
		// the SSG clock is master * 2 / (prescale * 2 / 3), and ay_tick() is
		// a half period of it, so that many ticks per master clock
		m_ssgmul = (pre == 2) ? 4 : (pre == 3) ? 2 : 1;
		if (m_fmcnt >= m_fmper) m_fmcnt = 0;
	}

	uint32_t chanmask() const {
		uint32_t mask = 0;
		for (int i = 0; i < 3; i++)
			if (!m_chip->fm_off[i]) mask |= 1 << i;
		return mask;
	}

	aymChip* m_chip;
	engine m_fm;
	engine::output_data m_out;

	uint64_t m_frac;		// ns left over, 32.32
	uint64_t m_clocks;		// master clocks since reset
	uint64_t m_timer_at[2];
	uint64_t m_busy_end;
	uint32_t m_fmcnt;
	uint32_t m_fmper;		// master clocks in one fm sample
	uint32_t m_ssgmul;		// ay_tick() calls per master clock
	uint8_t m_timer_on[2];
	uint8_t m_adr;
	int32_t m_last;			// last fm sample
};

// the register slot an operator is written through: the OPN holds them
// 1,3,2,4, so op2 and op3 are the other way round in the register file
const int op_slot[4] = {0, 2, 1, 3};

void xfm::view(fmChan* out) {
	for (int c = 0; c < 3; c++) {
		fmChan* vch = &out[c];
		ymfm::fm_channel<ymfm::opn_registers>* ch = m_fm.debug_channel(c);
		engine::output_data sum;	// the channel's own alias is private
		vch->algo = m_chip->reg[0xb0 + c] & 7;
		sum.clear();
		ch->output_4op(sum, 0, 32767);	// reads the operators, changes nothing
		vch->out = sum.data[0];
		for (int o = 0; o < 4; o++) {
			fmOper* v = &vch->op[o];
			ymfm::fm_operator<ymfm::opn_registers>* op = ch->debug_operator(o);
			int r = (op_slot[o] << 2) | c;		// 0x30 + r, 0x40 + r, ...
			ymfm::opdata_cache& cache = op->debug_cache();
			v->pg.freq = cache.block_freq & 0x7ff;
			v->pg.block = (cache.block_freq >> 11) & 7;
			v->pg.pstep = cache.phase_step;
			v->pg.phase = op->phase() << 10;
			v->tlev = m_chip->reg[0x40 + r] & 0x7f;
			v->eg.ks = (m_chip->reg[0x50 + r] >> 6) & 3;
			v->eg.atk = m_chip->reg[0x50 + r] & 0x1f;
			v->eg.dec = m_chip->reg[0x60 + r] & 0x1f;
			v->eg.sus = m_chip->reg[0x70 + r] & 0x1f;
			v->eg.rel = m_chip->reg[0x80 + r] & 0x0f;
			v->eg.suslev = (m_chip->reg[0x80 + r] >> 4) & 0x0f;
			v->eg.att = op->debug_eg_attenuation();
			// an OPN operator at rest is in release with nothing left to
			// release, which is what the page should call off
			switch (op->debug_eg_state()) {
				case ymfm::EG_ATTACK: v->eg.state = OPST_ATK; break;
				case ymfm::EG_DECAY: v->eg.state = OPST_DEC; break;
				case ymfm::EG_SUSTAIN: v->eg.state = OPST_SUS; break;
				case ymfm::EG_RELEASE:
					v->eg.state = (v->eg.att >= 0x3ff) ? OPST_OFF : OPST_REL;
					break;
				default: v->eg.state = OPST_OFF; break;
			}
		}
	}
}

xfm* fm_of(aymChip* chip) {
	if (!chip->fm)
		chip->fm = new xfm(chip);
	return (xfm*)chip->fm;
}

}	// namespace

extern "C" {

void ym2203_free(aymChip* chip) {
	if (!chip->fm) return;
	delete (xfm*)chip->fm;
	chip->fm = NULL;
}

void ym2203_reset(aymChip* chip) {
	ay_reset(chip);
	chip->reg[0xff] = 0;
	chip->wait = 0;
	fm_of(chip)->reset();
}

void ym2203_sync(aymChip* chip, int ns) {
	fm_of(chip)->run(ns);
}

void ym2203_wr(aymChip* chip, int adr, int val) {
	xfm* fm = fm_of(chip);
	val &= 0xff;
	if (adr & 1) {			// #fffd: the register number
		chip->curReg = val;
		fm->write_addr(val);
	} else if (chip->curReg < 0x10) {	// #bffd: the SSG half answers
		ay_set_reg(chip, val);
	} else if (chip->curReg < 0xff) {	// ff is where the status is kept
		chip->reg[chip->curReg] = val;
		fm->write_data(val);
	}
}

int ym2203_rd(aymChip* chip, int adr) {
	// only the SSG registers read back; the status byte goes out through
	// tsIn(), which is the one that knows when the port means status
	return (chip->curReg < 0x10) ? ym_rd(chip, adr) : -1;
}

sndPair ym2203_vol(aymChip* chip) {
	sndPair v = ym_vol(chip);
	if (!chip->blk_fm) {
		int fm = fm_of(chip)->out() / FM_DIV;
		v.left += fm;
		v.right += fm;
	}
	return v;
}

void ym2203_fm_view(aymChip* chip, fmChan* out) {
	if (chip->type == SND_YM2203)
		fm_of(chip)->view(out);
}

}	// extern "C"

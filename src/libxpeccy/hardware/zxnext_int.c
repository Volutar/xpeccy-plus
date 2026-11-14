// The ZX Spectrum Next's interrupts: the shared INT pulse, the hardware IM2 daisy
// chain (device/im2_peripheral.vhd, im2_device.vhd, im2_control.vhd), the CTC
// (device/ctc_chan.vhd), the NMI (zxnext.vhd 2087-2170) and the Multiface
// (device/multiface.vhd). Times are CLK_28 edges (nx_now) unless said otherwise.

#include <string.h>
#include <limits.h>

#include "zxnext.h"
#include "../cpu/Z80/z80.h"

#define IRQ (NX.irq)
#define CTC (NX.ctc)

enum {DEV_0 = 0, DEV_REQ, DEV_ISR};
enum {CTC_RESET = 0, CTC_RESET_TC, CTC_TRIGGER, CTC_RUN, CTC_RUN_TC};
enum {NMI_IDLE = 0, NMI_FETCH, NMI_HOLD};

static int nx_tau(Computer* comp) {
	return 8 >> NX.speed;
}

// == the pulse every pulsed source shares (zxnext.vhd 2012-2044) ==

// pulse_count steps on the rising edges of the cpu clock, and 48K/128K contention holds
// that clock (zxula.vhd 587-595): a held T-state has no edge. A hold of d T-states
// starting on the edge at CLK_28 c takes away the edges at c + 8, ..., c + 8d.
void nx_hold_note(Computer* comp, long long c, int d) {
	if (d <= 0) return;
	NX.holdHead = (NX.holdHead + 1) & (NX_HOLDS - 1);
	NX.holdC[NX.holdHead] = c;
	NX.holdD[NX.holdHead] = d;
}

static int nx_holds_after(Computer* comp, long long x) {
	for (int i = 0; i < NX_HOLDS; i++)
		if (NX.holdD[i] && (NX.holdC[i] + 8 * NX.holdD[i] > x)) return 1;
	return 0;
}

// the edge of the grid at c is one a hold took away
int nx_held(Computer* comp, long long c) {
	for (int i = 0; i < NX_HOLDS; i++)
		if (NX.holdD[i] && (c > NX.holdC[i]) && (c <= NX.holdC[i] + 8 * NX.holdD[i])) return 1;
	return 0;
}

// The CLK_28 edge of the n-th cpu clock edge (n >= 1) after x, on the grid through the
// edge a of the clock running now
static long long nx_edge_nth(Computer* comp, long long x, int n, long long a) {
	int tau = nx_tau(comp);
	long long k = x - a;
	long long m = (k >= 0) ? k / tau : -((-k + tau - 1) / tau);
	long long c = a + (m + 1) * tau;
	if (!nx_holds_after(comp, x)) return c + (long long)(n - 1) * tau;
	for (;;) {
		if (!nx_held(comp, c) && (--n <= 0)) return c;
		c += tau;
	}
}

// an edge of the cpu clock: the last one INT was looked at on (dflt: none known)
static long long nx_grid(Computer* comp, long long dflt) {
	return (NX.refT >= 0) ? NX.refC : dflt;
}

// INT_n low from the source firing (E) for N rising edges of the cpu clock: an edge at
// S sees it while E < S <= the end, the N-th edge after E. A blip (N = 0) is low for
// the one CLK_28 after E, seen only by an edge at E + 1.
static long long nx_pulse_end(Computer* comp, long long a) {
	if (!NX.pulseN) return NX.pulseE + 1;
	return nx_edge_nth(comp, NX.pulseE, NX.pulseN, a);
}

int nx_pulse_at(Computer* comp, long long s) {
	if ((NX.pulseE < 0) || (s <= NX.pulseE)) return 0;
	return s <= nx_pulse_end(comp, nx_grid(comp, s));
}

// The pulse as a register read sees it at CLK_28 edge r: low from the falling edge
// after E, high again from the falling edge after its end (the read's own TW edge, a
// few CLK_28 before r, is on the grid)
int nx_pulse_read(Computer* comp, long long r) {
	if ((NX.pulseE < 0) || (r <= NX.pulseE)) return 0;
	int tau = nx_tau(comp);
	return r <= nx_pulse_end(comp, nx_grid(comp, r - tau + ((tau + 1) >> 1) + 2));
}

// A source fired on edge e. Running, the pulse ignores it, and so does the edge it
// ends on; after that pulse_count stays at its end until a cpu edge sees INT_n high
// and clears it, so a source firing before that edge gets a blip.
void nx_pulse_start(Computer* comp, long long e) {
	if ((NX.pulseE >= 0) && (e >= NX.pulseE)) {
		long long a = nx_grid(comp, e + 1);
		long long end = nx_pulse_end(comp, a);
		if (e <= end) return;
		if (e < nx_edge_nth(comp, end, 1, a)) {
			NX.pulseE = e;
			NX.pulseN = 0;
			return;
		}
	}
	NX.pulseE = e;
	NX.pulseN = nx_tim(comp)->pulse;
}

// The clock changes speed on cpu edge s (zxnext.vhd 5809-5820): the edges the pulse
// has had are taken off, the rest counted from s in the new clock
void nx_pulse_rebase(Computer* comp, long long s) {
	if (NX.pulseE < 0) return;
	long long end = nx_pulse_end(comp, s);
	if (end < s) {
		NX.pulseE = -1;			// over, and the edge after it has cleared the count
	} else if (!NX.pulseN) {
		return;
	} else if (end == s) {
		NX.pulseE = s - 1;		// ended on s: the count stands at its end to the next edge
		NX.pulseN = 0;
	} else {
		int k = 0;
		for (long long c = nx_edge_nth(comp, NX.pulseE, 1, s); c <= s; c = nx_edge_nth(comp, c, 1, s)) k++;
		NX.pulseE = s;
		NX.pulseN -= k;
	}
}

// == the interrupt controller ==

// the IM mode im2_control snooped, which the devices go by (not the cpu's own)
static int nx_im2(Computer* comp) {
	return comp->cpu->regIMN == 2;
}

// Source idx fired on edge e: its int_req is high for the CLK_28 after it. en: its
// enable; unq: an NR 20 software request, which needs neither enable nor edge. In
// pulse mode it starts the shared pulse; in hardware mode it is the device's one
// request, a second one before its RETI is lost - only the ULA, with the cpu not in
// IM 2, still pulses (the EXCEPTION of zxnext.vhd 1964).
void nx_int_event(Computer* comp, int idx, long long e, int en, int unq) {
	NX.irqDead = 0;
	NX.dmaIntDead = 0;
	IRQ.stat |= 1 << idx;
	if (!en && !unq) return;
	int hw = NX.nr[0xc0] & 1;
	if (!hw || ((idx == 11) && !nx_im2(comp)))
		nx_pulse_start(comp, e);
	if (hw && !(IRQ.pend & (1 << idx))) {
		IRQ.pend |= 1 << idx;
		IRQ.pendT[idx] = e + 1;
	}
}

// An M1 cycle with T1 at t (tickTotal units): M1_n is low on the next len rising edges
void nx_int_m1(Computer* comp, long long t, int len) {
	IRQ.m1Head = (IRQ.m1Head + 1) & 7;
	IRQ.m1[IRQ.m1Head].t = t;
	IRQ.m1[IRQ.m1Head].len = len;
}

static int nx_m1_low(Computer* comp, long long t) {
	for (int k = 0; k < 8; k++) {
		long long d = t - IRQ.m1[k].t;
		if ((d >= 1) && (d <= IRQ.m1[k].len)) return 1;
	}
	return 0;
}

// Whether device i's request has moved it to S_REQ by cpu edge s (tickTotal ts there):
// that takes a rising edge after its CLK_28 edge on which M1_n is high.
static int nx_dev_req(Computer* comp, int i, long long s, long long ts) {
	long long d = s - IRQ.pendT[i];
	if (d <= 0) return 0;
	int tau = nx_tau(comp);
	int n = 0;
	for (long long j = 1; j * tau < d; j++) {
		if (nx_held(comp, s - j * tau)) continue;	// a held clock: no edge there
		if (++n > 16) return 1;		// M1 is never low that long
		if (!nx_m1_low(comp, ts - j)) return 1;
	}
	return 0;
}

// The devices' INT_n at cpu edge s: the first device not in S_0 holds the chain,
// and pulls INT_n if it is the one requesting (S_REQ) with the cpu in IM 2.
int nx_int_level(Computer* comp, long long s, long long ts) {
	if (!(NX.nr[0xc0] & 1)) return 0;
	for (int i = 0; i < NX_IRQ_DEVS; i++) {
		if (IRQ.st[i] == DEV_0) {
			if (!(IRQ.pend & (1 << i)) || !nx_dev_req(comp, i, s, ts)) continue;
			IRQ.st[i] = DEV_REQ;
			NX.dmaIntDead = 0;
			NX.irqDead = 0;
		}
		return (IRQ.st[i] == DEV_REQ) && nx_im2(comp);
	}
	return 0;
}

// The interrupt acknowledge's data (zxnext.vhd 1868-1878): the vector of the device
// it acknowledges, NR C0's top bits alone while any device holds the chain without
// being acknowledged, FF when none does.
int nx_int_ack(Computer* comp) {
	if (!(NX.nr[0xc0] & 1)) return 0xff;
	for (int i = 0; i < NX_IRQ_DEVS; i++) {
		if (IRQ.st[i] == DEV_0) continue;
		if ((IRQ.st[i] == DEV_REQ) && nx_im2(comp)) {
			IRQ.st[i] = DEV_ISR;
			NX.dmaIntDead = 0;
			NX.irqDead = 0;
			return (NX.nr[0xc0] & 0xe0) | (i << 1);
		}
		return NX.nr[0xc0] & 0xe0;
	}
	return 0xff;
}

// nx_int_ack()'s byte, not acked (zxnext.vhd 1832-1835, 1868-1878)
_Static_assert(NX_IRQ_DEVS <= INT_VEC_MAX, "the Next's devices do not fit xIntVec");
int nx_int_vecs(Computer* comp, xIntVec* vec) {
	static const char* name[NX_IRQ_DEVS] = {"line", "UART 0 Rx", "UART 1 Rx",
		"CTC 0", "CTC 1", "CTC 2", "CTC 3", "CTC 4", "CTC 5", "CTC 6", "CTC 7",
		"ULA", "UART 0 Tx", "UART 1 Tx"};
	int hw = NX.nr[0xc0] & 1;
	if (!hw || !nx_im2(comp)) {		// outside IM 2 a device's request still takes the chain
		vec[0].name = "INT";
		vec[0].vec = hw ? (NX.nr[0xc0] & 0xe0) : 0xff;
		vec[0].driven = hw || !(NX.expbusEff && !(NX.nr[0x80] & 0x20));
		return 1;
	}
	int head = -1;		// the first device not in S_0 holds the chain
	for (int i = 0; (i < NX_IRQ_DEVS) && (head < 0); i++)
		if (IRQ.st[i] != DEV_0) head = i;
	int wait = ((head >= 0) && (IRQ.st[head] == DEV_REQ)) ? head : -1;	// acked next
	for (int i = 0; (head < 0) && (wait < 0) && (i < NX_IRQ_DEVS); i++)
		if (IRQ.pend & (1 << i)) wait = i;
	int last = ((head >= 0) && (IRQ.st[head] == DEV_ISR)) ? head : NX_IRQ_DEVS;	// before RETI only those ahead of it
	int n = 0;
	if (wait >= 0) {
		vec[n].name = name[wait];
		vec[n].vec = (NX.nr[0xc0] & 0xe0) | (wait << 1);
		vec[n].driven = 1;
		n++;
	}
	for (int i = 0; i < last; i++) {
		if (i == wait) continue;
		int on;
		switch (i) {
			case 0: on = NX.nr[0xc4] & 2; break;
			case 1: on = NX.nr[0xc6] & 3; break;
			case 2: on = (NX.nr[0xc6] >> 4) & 3; break;
			case 11: on = !(NX.pff & 0x40); break;
			case 12: on = NX.nr[0xc6] & 4; break;
			case 13: on = NX.nr[0xc6] & 0x40; break;
			default: on = (i < 7) && ((CTC.ch[i - 3].cr >> 4) & 1); break;	// CTC 4-7 are not built
		}
		if (!on) continue;
		vec[n].name = name[i];
		vec[n].vec = (NX.nr[0xc0] & 0xe0) | (i << 1);
		vec[n].driven = 1;
		n++;
	}
	return n;
}

// RETI: the innermost device in service (the first one past those in S_0 and the
// requesting ones, which pass IEI while RETI is decoded) goes back to S_0, and its
// request with it
static void nx_int_reti(Computer* comp) {
	if (!(NX.nr[0xc0] & 1) || !nx_im2(comp)) return;
	for (int i = 0; i < NX_IRQ_DEVS; i++) {
		if (IRQ.st[i] != DEV_ISR) continue;
		IRQ.st[i] = DEV_0;
		IRQ.pend &= ~(1 << i);
		NX.dmaIntDead = 0;
		NX.irqDead = 0;
		return;
	}
}

// INT from the chain (nx_int_level) as it stands at s: s while a device requests, else
// the first CLK_28 a pending one could request on, LLONG_MAX with none pending. One in
// service holds the chain, and INT off, until its RETI.
long long nx_int_req_until(Computer* comp, long long s) {
	long long u = LLONG_MAX;
	for (int i = 0; i < NX_IRQ_DEVS; i++) {
		if (IRQ.st[i] == DEV_REQ) return s;
		if ((IRQ.st[i] == DEV_0) && (IRQ.pend & (1 << i)) && (IRQ.pendT[i] < u)) u = IRQ.pendT[i];
	}
	return (u <= s) ? s : u;
}

// a device requesting or in service, or a CTC channel counting: INT is looked at always
int nx_int_busy(Computer* comp) {
	if (IRQ.pend) return 1;
	for (int i = 0; i < NX_IRQ_DEVS; i++) if (IRQ.st[i]) return 1;
	return 0;
}

static int nx_ctc_trg(Computer* comp, int i);

// The first CLK_28 edge a CTC channel's ZC/TO can come on, from the timers' prescalers
// and counts (a counter only steps on another channel's ZC/TO); one in a TRIGGER or
// with an edge in flight: CTC.t itself. LLONG_MAX when no timer runs.
long long nx_ctc_quiet_until(Computer* comp) {
	long long u = LLONG_MAX;
	if (CTC.t < 0) return u;
	for (int i = 0; i < 4; i++) {
		nxCtcChan* c = &CTC.ch[i];
		if (c->zcD || (c->reqD != c->zcD) || (c->trgD != nx_ctc_trg(comp, i)) || (c->state == CTC_TRIGGER)) return CTC.t;
		if (((c->state != CTC_RUN) && (c->state != CTC_RUN_TC)) || ((c->cr >> 3) & 1)) continue;
		int p = ((c->cr >> 2) & 1) ? 256 : 16;
		long long d0 = ((p - 1) - (c->pre & (p - 1))) & (p - 1);
		long long n = c->cnt ? c->cnt : 256;
		long long z = CTC.t + d0 + (n - 1) * p;
		if (z < u) u = z;
	}
	return u;
}

// NR C0 b0 written: pulse mode holds every device in reset
void nx_int_mode(Computer* comp) {
	NX.dmaIntDead = 0;
	NX.irqDead = 0;
	if (NX.nr[0xc0] & 1) return;
	IRQ.pend = 0;
	memset(IRQ.st, 0, sizeof(IRQ.st));
}

// status as NR 20 and C8-CA read it: the sticky bits, or a request not yet serviced
int nx_int_status(Computer* comp) {
	return IRQ.stat | IRQ.pend;
}

void nx_int_clear(Computer* comp, int mask) {
	IRQ.stat &= ~mask;
}

// == NMI ==

static int nx_div_hold(Computer* comp) {
	return NX.dmMap || NX.dmHeld || NX.dmButton;	// divmmc.vhd o_disable_nmi
}

static int nx_mf_on(Computer* comp) {
	return NX.nr[0x83] & 2;				// port_multiface_io_en
}

static void nx_mf_reset(Computer* comp) {
	IRQ.mfNmi = 0;
	IRQ.mfInvisible = 1;
	IRQ.mfEnable = 0;
}

// NR 83 written: with its port off the Multiface is held in reset (multiface.vhd 103)
void nx_mf_enable(Computer* comp) {
	if (!nx_mf_on(comp)) nx_mf_reset(comp);
}

// nmi_hold of the owner of the NMI under way
static int nx_nmi_hold(Computer* comp) {
	if (IRQ.nmiMf) return IRQ.mfNmi;
	if (IRQ.nmiDiv) return nx_div_hold(comp);
	return 0;
}

// The button of a source, or its NR 02 bit: latched only while no NMI is active
// (Multiface first), gated by NR 06. The NMI is taken at the end of the
// instruction under way.
void nx_nmi_raise(Computer* comp, int mf, int div) {
	if (IRQ.nmiMf || IRQ.nmiDiv) return;
	if (mf && (NX.nr[0x06] & 0x08) && !(NX.pe3 & 0x80) && !nx_div_hold(comp)) {
		IRQ.nmiMf = 1;
		if (nx_mf_on(comp) && !IRQ.mfNmi) {		// the Multiface's button_pulse
			IRQ.mfNmi = 1;
			IRQ.mfInvisible = 0;
		}
	} else if (div && (NX.nr[0x06] & 0x10) && !IRQ.mfEnable && !IRQ.mfNmi) {
		IRQ.nmiDiv = 1;
		if ((NX.nr[0x0a] & 0x10) && (NX.nr[0x83] & 1))	// automap_reset holds button_nmi off
			NX.dmButton = 1;
	} else {
		return;
	}
	IRQ.nmiState = NMI_FETCH;
	comp->cpu->intrq |= Z80_NMI;
}

// The i/o trap (NR D8 b0): 2FFD/3FFD reads and 3FFD writes raise the Multiface NMI;
// the cause and a written byte are kept while an NMI could be taken (zxnext.vhd 3866-3895)
void nx_nmi_trap(Computer* comp, int port, int wr, int val) {
	if (!(NX.nr[0xd8] & 1)) return;
	int fd = port & 0xf003;
	if ((fd != 0x2001) && (fd != 0x3001)) return;
	if (wr && (fd == 0x2001)) return;		// 2FFD writes are not trapped
	if (IRQ.nmiState != NMI_HOLD) {
		IRQ.trapCause = wr ? 3 : (fd == 0x2001) ? 1 : 2;
		if (wr) NX.nr[0xd9] = val;
	}
	nx_nmi_raise(comp, 1, 0);
}

int nx_nmi_cause(Computer* comp) {
	return IRQ.trapCause;
}

void nx_nmi_cause_clear(Computer* comp) {
	IRQ.trapCause = 0;
}

// NR 02 b3/b2 read back as set while an NMI could still be taken (idle or fetch)
int nx_nmi_accepts(Computer* comp) {
	return IRQ.nmiState != NMI_HOLD;
}

// the acknowledge pushes the pc: NR C2/C3 take it, and with the stackless NMI on
// memory does not
void nx_nmi_ack(Computer* comp) {
	IRQ.nmiPush = 2;
	if (NX.nr[0xc0] & 8) IRQ.retnC2 = 1;
}

// the next memory cycle is a stackless push or pop: MREQ is masked (zxnext.vhd 1828)
int nx_nmi_stack(Computer* comp) {
	return (IRQ.nmiPush && (NX.nr[0xc0] & 8)) || IRQ.retnPop;
}

// a write by the cpu: 1 if it was the acknowledge's push and is not to land
int nx_nmi_push(Computer* comp, int val) {
	if (!IRQ.nmiPush) return 0;
	NX.nr[(IRQ.nmiPush == 2) ? 0xc3 : 0xc2] = val;
	IRQ.nmiPush--;
	return (NX.nr[0xc0] & 8) ? 1 : 0;
}

// a read by the cpu that is a stackless RETN's pop: NR C2 then C3, else -1
int nx_nmi_pop(Computer* comp) {
	if (!IRQ.retnPop) return -1;
	int v = NX.nr[(IRQ.retnPop == 2) ? 0xc2 : 0xc3];
	if (--IRQ.retnPop == 0) IRQ.retnC2 = 0;
	return v;
}

// The opcode fetch at adr, before it is decoded: the NMI state machine's step to
// hold at 0066 and back to idle when its owner lets go, and the Multiface paging in
// for that fetch
void nx_nmi_fetch(Computer* comp, int adr) {
	if ((IRQ.nmiState == NMI_HOLD) && !nx_nmi_hold(comp)) {
		IRQ.nmiState = NMI_IDLE;
		IRQ.nmiMf = IRQ.nmiDiv = 0;
	}
	if (adr != 0x0066) return;
	if (IRQ.nmiState == NMI_FETCH) IRQ.nmiState = NMI_HOLD;
	if (IRQ.mfNmi && nx_mf_on(comp)) IRQ.mfEnable = 1;
}

// The HALT run-on (nx_halt): the NMI state machine stands still under fetches of v
// at adr, and the opcode decoder sees no RETI or RETN - a prefix byte only takes it
// round, which nx_int_halt_done() follows
int nx_int_halt_ok(Computer* comp, int adr, int v) {
	if (adr == 0x0066) return 0;
	if ((IRQ.nmiState == NMI_HOLD) && !nx_nmi_hold(comp)) return 0;
	return !(((IRQ.dec == 1) || (IRQ.dec == 4)) && ((v & 0xc7) == 0x45));
}

// nx_int_opcode()'s next state, for a byte that does nothing else there
static int nx_int_dec_step(int dec, int v) {
	switch (dec) {
		case 0: return (v == 0xed) ? 1 : (v == 0xcb) ? 2 : ((v == 0xdd) || (v == 0xfd)) ? 3 : 0;
		case 3: return ((v == 0xdd) || (v == 0xfd)) ? 3 : (v == 0xed) ? 4 : 0;
	}
	return 0;
}

// the decoder after n fetches of v: within two it is on a cycle of one or two states
void nx_int_halt_done(Computer* comp, int v, long long n) {
	int d = IRQ.dec;
	for (long long k = (n < 8) ? n : 8 + ((n - 8) & 1); k > 0; k--)
		d = nx_int_dec_step(d, v);
	IRQ.dec = d;
}

// config mode holds the NMI in idle
void nx_nmi_config(Computer* comp) {
	IRQ.nmiState = NMI_IDLE;
	IRQ.nmiMf = IRQ.nmiDiv = 0;
}

// == the DMA's hold-off ==

static int nx_dma_int(Computer* comp) {
	unsigned char* nr = NX.nr;
	if ((IRQ.nmiMf || IRQ.nmiDiv) && (nr[0xcc] & 0x80)) return 1;	// nmi_activated and NR CC b7
	if (!(nr[0xc0] & 1)) return 0;		// pulse mode: the devices are held in reset
	if (!(nr[0xcc] & 3) && !nr[0xcd] && !(nr[0xce] & 0x77)) return 0;
	// the DMA asks on every clock edge while it waits. Its last answer stands, with no
	// catching up, until the first edge a source could fire on (dmaIntDead: positive the
	// answer was no, negative yes - a device it waits for is in service): a catch-up
	// before it would find nothing. Every change of the devices' state, the enables or a
	// source's schedule clears dmaIntDead.
	long long t = nx_now(comp);
	if ((NX.dmaIntDead > 0) && (t < NX.dmaIntDead)) return 0;
	if ((NX.dmaIntDead < 0) && (t < -NX.dmaIntDead)) return 1;
	nx_int_catch_up(comp);
	int en = ((nr[0xcc] & 2) ? 1 : 0) | ((nr[0xcc] & 1) << 11) | ((nr[0xcd] & 0x0f) << 3)
		| ((nr[0xce] & 0x40) ? 1 << 13 : 0) | ((nr[0xce] & 0x04) ? 1 << 12 : 0)
		| ((nr[0xce] & 0x30) ? 1 << 2 : 0) | ((nr[0xce] & 0x03) ? 1 << 1 : 0);
	long long dead = nx_int_quiet_until(comp);	// the line compare, the CTC, the UARTs, the frame end
	for (int i = 0; i < NX_IRQ_DEVS; i++) {
		if (!((en >> i) & 1)) continue;
		if (IRQ.st[i] != DEV_0) {
			NX.dmaIntDead = -dead;
			return 1;
		}
		if ((IRQ.pend >> i) & 1) {
			if (IRQ.pendT[i] < t) return 1;	// S_REQ by the next edge (M1 is high under the DMA)
			if (IRQ.pendT[i] + 1 < dead) dead = IRQ.pendT[i] + 1;
		}
	}
	NX.dmaIntDead = dead;
	return 0;
}

// im2_dma_delay (zxnext.vhd 2001-2010): the DMA keeps off the bus while a source with
// its DMA enable (NR CC-CE) is requesting or in service, or while an NMI is active with
// NR CC b7, and for the three edges after the RETI/RETN that ended it. e: the cpu
// clock edge asked about (nx_edge units)
int nx_dma_delay(Computer* comp, long long e) {
	if (IRQ.dmaHold && (e < IRQ.retiT + 4)) return 1;
	return nx_dma_int(comp);
}

// == im2_control's opcode decoder: IM shadow (in the Z80N tables), RETI, RETN ==

// v: the opcode byte of an M1 fetch
void nx_int_opcode(Computer* comp, int v) {
	switch (IRQ.dec) {
		case 0:
			IRQ.dec = (v == 0xed) ? 1 : (v == 0xcb) ? 2 : ((v == 0xdd) || (v == 0xfd)) ? 3 : 0;
			break;
		case 1:
			IRQ.dec = 0;
			if ((v == 0x4d) || (v == 0x45)) {
				IRQ.dmaHold = nx_dma_int(comp);
				IRQ.retiT = nx_edge(comp);
			}
			if (v == 0x4d) {
				nx_int_reti(comp);
			} else if (v == 0x45) {
				// z80_retn_seen: divMMC lets go before its own stack reads, the Multiface too
				NX.dmHeld = NX.dmNext = NX.dmMap = NX.dmButton = 0;
				IRQ.mfNmi = 0;
				IRQ.mfEnable = 0;
			}
			// every RETN the T80 has pops NR C2/C3 after a stackless NMI
			if (IRQ.retnC2 && ((v & 0xc7) == 0x45) && (v != 0x4d))
				IRQ.retnPop = 2;
			break;
		case 3:
			IRQ.dec = ((v == 0xdd) || (v == 0xfd)) ? 3 : (v == 0xed) ? 4 : 0;
			break;
		case 4:
			// ED after DD/FD: nothing for the snooper, but the T80 runs RETN all the same
			IRQ.dec = 0;
			if (IRQ.retnC2 && ((v & 0xc7) == 0x45) && (v != 0x4d))
				IRQ.retnPop = 2;
			break;
		default:
			IRQ.dec = 0;
			break;
	}
}

// == the Multiface ==

static int nx_mf_type(Computer* comp) {
	return (NX.nr[0x0a] >> 6) & 3;			// 00 +3, 11 48, else 128
}

int nx_mf_port(Computer* comp, int lo, int* dis) {
	int t = nx_mf_type(comp);
	int en = (t & 2) ? 0x9f : (t & 1) ? 0xbf : 0x3f;
	int off = (t & 2) ? 0x1f : (t & 1) ? 0x3f : 0xbf;
	if (!nx_mf_on(comp)) return 0;
	*dis = (lo == off);
	return (lo == en) || (lo == off);
}

// a read of its ports: -1 if it does not answer
int nx_mf_in(Computer* comp, int port) {
	int dis;
	if (!nx_mf_port(comp, port & 0xff, &dis)) return -1;
	int p3 = (nx_mf_type(comp) == 0);
	int vis = !IRQ.mfInvisible || (nx_mf_type(comp) == 3);
	if (dis) {
		IRQ.mfEnable = 0;
		if (p3) IRQ.mfNmi = 0;
		return -1;
	}
	IRQ.mfEnable = vis;
	if (!vis || (nx_mf_type(comp) == 3)) return -1;
	if (!p3) return ((NX.p7ffd & 8) << 4) | 0x7f;
	switch ((port >> 12) & 15) {
		case 0x1: return (NX.p1ffdMtr << 3) | (NX.p1ffd & 7);
		case 0x7: return NX.p7ffd;
		case 0xd: return NX.pdffd & 0x5f;
		case 0xe: return NX.peff7 & 0x0c;
	}
	return NX.pfe & 7;
}

void nx_mf_out(Computer* comp, int port) {
	int dis;
	if (!nx_mf_port(comp, port & 0xff, &dis)) return;
	int p3 = (nx_mf_type(comp) == 0);
	IRQ.mfNmi = 0;
	if (dis ? !p3 : p3) IRQ.mfInvisible = 1;
}

int nx_mf_mem(Computer* comp) {
	return IRQ.mfEnable;
}

// == the CTC ==

static int nx_ctc_trg(Computer* comp, int i) {
	return CTC.ch[(i + 3) & 3].zcD;		// the ZC/TO ring: ch0 <- ch3 (zxnext.vhd 4084)
}

// One CLK_28 edge, CTC.t (ctc_chan.vhd 117-289). wch/wval: an i/o write in the
// cycle before it, c5 >= 0: an NR C5 write landing on it.
static void nx_ctc_edge(Computer* comp, int wch, int wval, int c5) {
	nxCtcChan old[4];
	if ((wch >= 0) || (c5 >= 0)) NX.dmaIntDead = 0;
	memcpy(old, CTC.ch, sizeof(old));
	for (int i = 0; i < 4; i++) {
		nxCtcChan* o = &old[i];
		nxCtcChan* c = &CTC.ch[i];
		int trg = old[(i + 3) & 3].zcD;
		int iowr = (wch == i);
		int d = iowr ? wval : 0;
		int running = (o->state == CTC_RUN) || (o->state == CTC_RUN_TC);
		int tcwait = (o->state == CTC_RESET_TC) || (o->state == CTC_RUN_TC);
		int wtc = iowr && tcwait;
		int wcw = iowr && !tcwait && (d & 1);
		int d4 = (o->cr >> 1) & 1;
		int chg = wcw && (((d >> 4) & 1) != d4);
		int cte = (d4 ? (trg && !o->trgD) : (o->trgD && !trg)) || chg;
		int plo = (o->pre & 15) == 15;
		int phi = (o->pre >> 4) == 15;
		int presc = ((o->cr >> 2) & 1) ? (plo && phi) : plo;
		int ten = ((o->cr >> 3) & 1) ? cte : presc;
		int tn = (o->cnt - 1) & 0xff;
		int zc = (tn == 0) && ten && running;
		int sn = o->state;
		if (wcw && (d & 2)) {
			sn = (d & 4) ? CTC_RESET_TC : CTC_RESET;
		} else {
			switch (o->state) {
				case CTC_RESET: if (wcw && (d & 4)) sn = CTC_RESET_TC; break;
				case CTC_RESET_TC: if (iowr) sn = CTC_TRIGGER; break;
				case CTC_TRIGGER:
					if (wcw && (d & 4)) sn = CTC_RESET_TC;
					else if (!((o->cr >> 3) & 1) && (o->cr & 1) && !cte) sn = CTC_TRIGGER;
					else sn = CTC_RUN;
					break;
				case CTC_RUN: if (wcw && (d & 4)) sn = CTC_RUN_TC; break;
				default: if (iowr) sn = CTC_RUN; break;
			}
		}
		c->trgD = trg;
		c->pre = running ? (o->pre + 1) & 0xff : 0;
		c->cnt = (!running || zc) ? o->tc : (ten ? tn : o->cnt);
		c->zcD = zc;
		c->state = sn;
		if (wcw) c->cr = d >> 3;
		else if (c5 >= 0) c->cr = (o->cr & 0x0f) | (((c5 >> i) & 1) << 4);
		if (wtc) c->tc = d;
		// im2_peripheral: ZC/TO's rising edge is the channel's interrupt request
		if (o->zcD && !o->reqD)
			nx_int_event(comp, 3 + i, CTC.t - 1, (o->cr >> 4) & 1, 0);
		c->reqD = o->zcD;
	}
	CTC.t++;
}

// the first edge from CTC.t on where anything but the prescalers moves
static long long nx_ctc_next(Computer* comp) {
	long long n = -1;
	for (int i = 0; i < 4; i++) {
		nxCtcChan* c = &CTC.ch[i];
		if (c->zcD || (c->reqD != c->zcD) || (c->trgD != nx_ctc_trg(comp, i))) return CTC.t;
		long long e = -1;
		int run = (c->state == CTC_RUN) || (c->state == CTC_RUN_TC);
		if (!run && ((c->cnt != c->tc) || c->pre)) return CTC.t;	// stopped: the edge that reloads it
		switch (c->state) {
			case CTC_TRIGGER:
				if (((c->cr >> 3) & 1) || !(c->cr & 1)) return CTC.t;	// leaves it on the next edge
				break;
			case CTC_RUN:
			case CTC_RUN_TC:
				if (!((c->cr >> 3) & 1))
					e = CTC.t + (((c->cr >> 2) & 1) ? ((255 - c->pre) & 255) : ((15 - c->pre) & 15));
				break;
		}
		if ((e >= 0) && ((n < 0) || (e < n))) n = e;
	}
	return n;
}

// The edges from CTC.t, short of t, on which the running timers only count down: up
// to the first edge where one of them reaches zero. Asked where nx_ctc_next() found
// an edge that moves something, at CTC.t: that is a prescaler's, or something else
// moves and the answer is CTC.t itself.
static long long nx_ctc_count_end(Computer* comp, long long t) {
	long long b = t;
	for (int i = 0; i < 4; i++) {
		nxCtcChan* c = &CTC.ch[i];
		if (c->zcD || (c->reqD != c->zcD) || (c->trgD != nx_ctc_trg(comp, i))) return CTC.t;
		int run = (c->state == CTC_RUN) || (c->state == CTC_RUN_TC);
		if (!run) {
			if ((c->cnt != c->tc) || c->pre || (c->state != CTC_RESET && c->state != CTC_RESET_TC
					&& !((c->state == CTC_TRIGGER) && !((c->cr >> 3) & 1) && (c->cr & 1)))) return CTC.t;
			continue;
		}
		if ((c->cr >> 3) & 1) continue;		// a counter: its trigger stands still
		int p = ((c->cr >> 2) & 1) ? 256 : 16;
		long long j0 = (p - 1 - c->pre) & (p - 1);
		long long m = c->cnt ? c->cnt - 1 : 255;	// the decrements before the one reaching zero
		long long z = CTC.t + j0 + m * p;
		if (z < b) b = z;
	}
	return b;
}

// every edge before t
void nx_ctc_run(Computer* comp, long long t) {
	nx_uart_run(comp, t);		// the other source that fires by itself
	if (CTC.t < 0) CTC.t = t;
	while (CTC.t < t) {
		long long n = nx_ctc_next(comp);
		if ((n < 0) || (n > t)) n = t;
		if (n > CTC.t) {
			int k = (int)((n - CTC.t) & 0xff);
			for (int i = 0; i < 4; i++) {
				nxCtcChan* c = &CTC.ch[i];
				if ((c->state == CTC_RUN) || (c->state == CTC_RUN_TC)) c->pre = (c->pre + k) & 0xff;
			}
			CTC.t = n;
			continue;
		}
		long long b = nx_ctc_count_end(comp, t);
		if (b > CTC.t + 1) {
			// what nx_ctc_edge() does on each of them, at once
			long long d = b - CTC.t;
			for (int i = 0; i < 4; i++) {
				nxCtcChan* c = &CTC.ch[i];
				if ((c->state != CTC_RUN) && (c->state != CTC_RUN_TC)) continue;
				if (!((c->cr >> 3) & 1)) {
					int p = ((c->cr >> 2) & 1) ? 256 : 16;
					long long j0 = (p - 1 - c->pre) & (p - 1);
					long long k = (d > j0) ? (d - j0 + p - 1) / p : 0;
					c->cnt = (c->cnt - (int)(k & 0xff)) & 0xff;
				}
				c->pre = (c->pre + (int)(d & 0xff)) & 0xff;
			}
			CTC.t = b;
			continue;
		}
		nx_ctc_edge(comp, -1, 0, -1);
	}
}

// An OUT to channel ch, taken on edge e (the CLK_28 edge after T2 starts)
void nx_ctc_out(Computer* comp, int ch, int val, long long e) {
	nx_ctc_run(comp, e);
	if (ch < 4) nx_ctc_edge(comp, ch, val, -1);
}

// An IN from channel ch: t_count as it was before the CLK_28 edge at the falling cpu
// edge in TW (zxnext.vhd 4095-4100). tw: TW's rising edge.
int nx_ctc_in(Computer* comp, int ch, long long tw) {
	nx_ctc_run(comp, tw + (nx_tau(comp) + 1) / 2);
	return (ch < 4) ? CTC.ch[ch].cnt : 0x00;
}

// NR C5, which is the four channels' D7 (ctc_chan.vhd 263-276)
void nx_ctc_c5(Computer* comp, int val, long long e) {
	nx_ctc_run(comp, e);
	nx_ctc_edge(comp, -1, 0, val & 0x0f);
}

int nx_ctc_c5_read(Computer* comp) {
	int v = 0;
	for (int i = 0; i < 4; i++) v |= ((CTC.ch[i].cr >> 4) & 1) << i;
	return v;
}

// == reset ==

// the reset every source and device takes (hard and soft alike)
void nx_int_reset(Computer* comp) {
	memset(&IRQ, 0, sizeof(IRQ));
	NX.irqDead = 0;
	NX.dmaIntDead = 0;
	NX.refT = -1;
	NX.copWake = 0;
	memset(NX.holdD, 0, sizeof(NX.holdD));
	memset(&CTC, 0, sizeof(CTC));
	CTC.t = -1;
	// both UART Tx FIFOs are empty at reset release: an edge their status sees
	IRQ.stat = (1 << 12) | (1 << 13);
	nx_mf_reset(comp);
}

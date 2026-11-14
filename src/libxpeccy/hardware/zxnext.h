// The ZX Spectrum Next: what the machine (zxnext.c) and its picture (zxnext_vid.c)
// share. Not for anything outside the two.

#ifndef X_ZXNEXT_H
#define X_ZXNEXT_H

#include "hardware.h"

#define NX (*comp->next)

enum {NXT_48 = 0, NXT_128, NXT_P3, NXT_PENT};

// zxula_timing.vhd, per machine timing
typedef struct {
	int L;			// dots per line (c_max_hc + 1)
	int F50, F60;		// lines per frame
	int hact;		// raw H of paper pixel 0 (c_min_hactive)
	int vact50, vact60;	// raw V of paper line 0 (c_min_vactive)
	int hblank, vblank;	// blanking from raw 0
	int inth, intv50, intv60;	// the ULA interrupt pixel, raw
	int pulse;		// INT pulse length in cpu clocks
} nxTiming;

extern const nxTiming nxTim[4];

static inline const nxTiming* nx_tim(Computer* comp) {
	return &nxTim[NX.timing & 3];
}

static inline int nx_lines(Computer* comp) {
	const nxTiming* t = nx_tim(comp);
	return (NX.hz60 && (NX.timing != NXT_PENT)) ? t->F60 : t->F50;
}

static inline int nx_vact(Computer* comp) {
	const nxTiming* t = nx_tim(comp);
	return (NX.hz60 && (NX.timing != NXT_PENT)) ? t->vact60 : t->vact50;
}
long long nx_now(Computer*);
void nx_raw(Computer*, int*, int*);
void nx_ula(Computer*, int, int, int*, int*, int*);
void nx_ula_seen(Computer*, int, int, int*, int*);
void nx_raw_at(Computer*, long long, int*, int*);
// a / b rounded down, b > 0
static inline long long nx_fdiv(long long a, long long b) {
	return (a >= 0) ? a / b : -((b - 1 - a) / b);
}

void nx_cop_run(Computer*, long long);
// set by a copper write landing that fills the picture's queues (nxv_room): nx_cop_run
// stops at that edge, its copT there
extern int nx_cop_hold;
// the copper runs (or has a write still to make)
static inline int nx_cop_on(Computer* comp) {
	return (NX.nr[0x62] & 0xc0) || NX.copLast || NX.copN;
}

// the picture (zxnext_vid.c)
void nxv_reset(Computer*);
void nxv_regs(Computer*, long long);
void nxv_pal(Computer*, int, int, long long);
void nxv_memw(Computer*, int, int, long long);
int nxv_float(Computer*, long long);
long long nxv_l2_lane(Computer*, int lane, long long since, long long now, int* byte);
void nxv_retime(Computer*, long long, int);
void nxv_line(Computer*);		// a raw line starts: the one before is drawn
long long nxv_before_reset(Computer*, long long);	// a soft reset at clk28 w (-1: now)
void nxv_after_reset(Computer*, long long);
void nxv_touch(Computer*);		// something the picture reads moved: a still frame ends

// the board's CLK_28 against 28 MHz, as a period: 1 at VGA 0 (zxnext.c, NR 11)
double nx_clk_scale(Computer*);

// sound (zxnext_snd.c)
void nx_inp_power(Computer*);
void nx_inp_nr(Computer*, int, int);
int nx_inp_nr_read(Computer*, int);
int nx_inp_fe(Computer*, int);
int nx_inp_in(Computer*, int, int*);
void nx_keyp(Computer*, keyEntry*);
void nx_keyr(Computer*, keyEntry*);

void nx_snd_reset(Computer*, long long);
void nx_snd_nr(Computer*, int, long long);
void nx_snd_dac_nr(Computer*, int, int, long long);
int nx_snd_in(Computer*, int);
void nx_snd_out(Computer*, int, int, long long);
void nx_snd_fe(Computer*, int);
sndPair nx_vol(Computer*, sndVolume*);
void nx_snd_take(Computer*, sndMix*);
int nx_snd_late(Computer*);

// interrupts, CTC, NMI, Multiface (zxnext_int.c)
void nx_hold_note(Computer*, long long, int);
int nx_held(Computer*, long long);
void nx_pulse_rebase(Computer*, long long);
int nx_pulse_at(Computer*, long long);
int nx_pulse_read(Computer*, long long);
void nx_pulse_start(Computer*, long long);
void nx_int_event(Computer*, int, long long, int, int);
void nx_int_m1(Computer*, long long, int);
int nx_int_level(Computer*, long long, long long);
int nx_int_ack(Computer*);
int nx_int_vecs(Computer*, xIntVec*);
void nx_int_mode(Computer*);
int nx_int_status(Computer*);
int nx_int_busy(Computer*);
void nx_int_clear(Computer*, int);
void nx_int_opcode(Computer*, int);
void nx_int_reset(Computer*);
void nx_nmi_raise(Computer*, int, int);
void nx_board_key(Computer*, int);	// F1..F10 as 0..9, made
int nx_nmi_accepts(Computer*);
void nx_nmi_ack(Computer*);
int nx_nmi_push(Computer*, int);
int nx_nmi_pop(Computer*);
void nx_nmi_fetch(Computer*, int);
int nx_int_halt_ok(Computer*, int, int);
void nx_int_halt_done(Computer*, int, long long);
long long nx_int_quiet_until(Computer*);
long long nx_int_req_until(Computer*, long long);
void nx_nmi_config(Computer*);
int nx_mf_in(Computer*, int);
void nx_mf_out(Computer*, int);
int nx_mf_mem(Computer*);
void nx_mf_enable(Computer*);
void nx_nmi_trap(Computer*, int, int, int);
int nx_nmi_stack(Computer*);
int nx_nmi_cause(Computer*);
void nx_nmi_cause_clear(Computer*);
void nx_ctc_run(Computer*, long long);
void nx_ctc_out(Computer*, int, int, long long);
int nx_ctc_in(Computer*, int, long long);
void nx_ctc_c5(Computer*, int, long long);
int nx_ctc_c5_read(Computer*);
void nx_int_catch_up(Computer*);

// the zxnDMA (zxnext_dma.c) and its side of the bus (zxnext.c)
long long nx_edge(Computer*);
int nx_dma_cycle(Computer*);
void nx_dma_port_wr(Computer*, int, int);
int nx_dma_port_rd(Computer*, int);
void nx_dma_reset(Computer*);
void nx_dma_power(Computer*);
int nx_dma_delay(Computer*, long long);
long long nx_ctc_quiet_until(Computer*);

// the UARTs (zxnext_uart.c)
void nx_uart_power(Computer*);
void nx_uart_reset(Computer*);
void nx_uart_run(Computer*, long long);
long long nx_uart_quiet_until(Computer*, int);
void nx_uart_out(Computer*, int, int, long long);
int nx_uart_in(Computer*, int, long long);
int nx_dma_bus_rd(Computer*, int, int);
void nx_dma_bus_wr(Computer*, int, int, int, long long);
int nx_dma_waits(Computer*, int, int, int, int, long long);
void nx_dma_stretch(Computer*, int);

// the sprites (zxnext_spr.c)
void nxs_run(Computer*, long long);
// nxs_run, but nothing to do while the pass is idle short of the next swap
static inline void nxs_run_to(Computer* comp, long long t2) {
	if (t2 >= NX.spr.idleTo) nxs_run(comp, t2);
}
// The line buffer the display reads during dot d: the one after the last swap, or
// one the pass cleared at a swap since, kept for the picture drawn after the ray
static inline unsigned short* nxs_line(Computer* comp, long long d) {
	if (d > NX.spr.dispSwap) return NX.spr.buf[NX.spr.dispNew];
	int k = NX.v.sprKeepNew;
	return NX.v.sprKeep[(d > NX.v.sprKeepTo[k ^ 1]) ? k : k ^ 1];
}
void nxs_nr(Computer*, int, int, long long);
void nxs_out(Computer*, int, int, long long);
int nxs_status(Computer*, long long);
int nxs_mirror(Computer*);
void nxs_reset(Computer*);

#endif

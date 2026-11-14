// The Next's two UARTs, ESP (0) and Pi (1): serial/uart.vhd, uart_tx.vhd, uart_rx.vhd,
// fifop.vhd, misc/debounce.vhd; ports 133B (Tx / status), 143B (Rx / prescaler), 153B
// (select), 163B (frame), zxnext.vhd 2639, 3335-3424. Time is in CLK_28 edges, as the
// CTC's. The Rx side is the core's own, clock by clock: its line comes from a list of
// level changes - bytes the host hands over (nx_uart_feed), or a test's schedule. What
// is sent goes to the host (next_uart_host_tx) and into the event log.

#include <string.h>
#include <limits.h>

#include "zxnext.h"
#include "../xlog.h"

#define UART		(NX.uart)
#define UART_TXF	64		// the Tx FIFO (fifop, DEPTH_BITS 6)
#define UART_RXF	512		// the Rx FIFO (DEPTH_BITS 9)
#define UART_SCH	256		// level changes waiting to come on a line

enum {RX_IDLE = 0, RX_START, RX_BITS, RX_PARITY, RX_STOP1, RX_STOP2, RX_ERROR, RX_PAUSE};

void (*next_uart_host_tx)(Computer*, int, int, long long) = NULL;

// == the receiver, uart_rx.vhd and the Rx half of uart.vhd ==

static int rx_near(nxUart* u) {
	return u->rxStored >= 384;		// stored(9) or (stored(8) and stored(7))
}

static void rx_fifo_reset(nxUart* u) {
	u->rxRd = u->rxWr = u->rxStored = 0;
	u->wrD = 0;
	u->ovf = u->ferr = 0;
}

// what the core's int_req is for this Rx: near full, or a byte in with NR C6 b1/b5 clear
static int rx_req(Computer* comp, int i) {
	nxUart* u = &UART[i];
	int only_near = (NX.nr[0xc6] >> (i ? 5 : 1)) & 1;
	return u->onfReg || (u->oavReg && !only_near);
}

// One CLK_28 rising edge, every register from the values before it
static void rx_step(Computer* comp, int i, long long k) {
	nxUart* u = &UART[i];
	int rx = u->dbo;
	int rx_e = rx ^ u->rxd;
	int noise = u->b0 ^ u->b1;
	int texp = (u->timer >> 1) == 0;
	int bexp = (u->bitc == 0);
	int fr = u->frame;
	int st = u->rst, nx = st;
	// the receiver's next state (uart_rx.vhd)
	switch (st) {
		case RX_IDLE: nx = (fr & 0x40) ? RX_PAUSE : (!rx ? RX_START : RX_IDLE); break;
		case RX_START: nx = rx ? RX_IDLE : (texp ? RX_BITS : RX_START); break;
		case RX_BITS: nx = (!bexp || !texp) ? RX_BITS : (u->fpar ? RX_PARITY : RX_STOP1); break;
		case RX_PARITY: nx = !texp ? RX_PARITY : ((rx == u->rpar) ? RX_STOP1 : RX_ERROR); break;
		case RX_STOP1: nx = !texp ? RX_STOP1 : (!rx ? RX_ERROR : (u->fstop ? RX_STOP2 : RX_IDLE)); break;
		case RX_STOP2: nx = !texp ? RX_STOP2 : (!rx ? RX_ERROR : RX_IDLE); break;
		case RX_ERROR: nx = !rx ? RX_ERROR : RX_IDLE; break;
		case RX_PAUSE: nx = ((fr & 0x40) || !rx) ? RX_PAUSE : RX_IDLE; break;
	}
	int errp = (st == RX_PARITY) && (nx == RX_ERROR);
	int errf = ((st == RX_STOP1) || (st == RX_STOP2)) && (nx == RX_ERROR);
	// uart.vhd's side, with the outputs as they stand before the edge
	int stored0 = u->rxStored;		// what the registered outputs take
	int full = (u->rxStored >= UART_RXF);
	int we = u->avd && !full;		// (the cpu's reads block it for their strobe: not kept)
	int wr_adv = u->wrD && !we && !full;
	int serr = u->ovf || u->ferr;
	// == the edge ==
	if (we) u->rxf[u->rxWr] = u->byteD;
	if (wr_adv) {
		u->rxWr = (u->rxWr + 1) & (UART_RXF - 1);
		u->rxStored++;
	}
	u->wrD = we;
	u->ovf |= u->oav && u->avd;
	u->ferr |= errf || errp;
	if (we) {
		u->avd = 0;
		u->byteD = 0;
	} else if (u->oav && !u->avd) {
		u->avd = 1;
		u->byteD = (serr << 8) | u->shift;
	}
	u->oav = ((st == RX_STOP1) || (st == RX_STOP2)) && (nx == RX_IDLE);
	// shift, timer, bit count (uart_rx.vhd)
	if ((st == RX_START) || (nx == RX_ERROR)) {
		u->shift = 0xff;
	} else if (((st == RX_BITS) || (st == RX_ERROR)) && texp) {
		u->shift = (rx << 7) | (u->shift >> 1);
	} else if ((st == RX_STOP1) && texp) {
		switch (u->fbits) {
			case 2: u->shift >>= 1; break;
			case 1: u->shift >>= 2; break;
			case 0: u->shift >>= 3; break;
		}
	}
	if (st == RX_IDLE) {
		u->timer = u->fpresc >> 1;
	} else if (texp) {
		u->timer = u->fpresc;
		u->upd = 0;
	} else if ((st != RX_START) && rx_e && !u->upd) {
		u->timer = u->fpresc >> 1;
		u->upd = 1;
	} else {
		u->timer--;
	}
	if (st == RX_IDLE) {
		u->bitc = 4 | u->fbits;
		u->rpar = (fr >> 1) & 1;
	} else if ((st == RX_BITS) && texp) {
		u->bitc--;
		u->rpar ^= rx;
	}
	u->rst = (fr & 0x80) ? RX_PAUSE : nx;
	// the debounce and Rx_d
	u->dbo = (u->dcnt >= 4) ? u->b1 : u->dbo;
	u->dcnt = noise ? 0 : ((u->dcnt < 4) ? u->dcnt + 1 : 4);
	u->b1 = u->b0;
	u->b0 = u->lin;
	u->rxd = rx;
	// the frame is taken on the falling edge while IDLE
	if (u->rst == RX_IDLE) {
		u->fbits = (fr >> 3) & 3;
		u->fpar = (fr >> 2) & 1;
		u->fstop = fr & 1;
		u->fpresc = u->presc;
	}
	// the registered outputs, the interrupt edge and RTR
	int req0 = rx_req(comp, i);
	u->oavReg = (stored0 > 0);
	u->onfReg = (stored0 >= 384);
	u->rtrN = (fr & 0x20) && (stored0 >= UART_RXF - 2);
	if (rx_req(comp, i) && !req0)
		nx_int_event(comp, 1 + i, k, (NX.nr[0xc6] >> (i ? 4 : 0)) & 3 ? 1 : 0, 0);
}

static long long rx_next_change(nxUart* u) {
	return u->schN ? u->sch[u->schHead].e : LLONG_MAX;
}

// the bytes waiting on the host's side onto the line, one frame at a time at the guest's
// own rate: start bit, data from b0, parity, stop bits
static void rx_feed_line(Computer* comp, nxUart* u, long long now) {
	while (u->hqN && (u->schN + 12 <= UART_SCH)) {
		int b = u->hq[u->hqHead];
		// the far end's own rate and frame: a mismatch is what the receiver gets wrong
		long long p = u->farBit ? u->farBit : ((long long)((u->presc > 1) ? u->presc : 1) << 16);
		int fr = u->farBit ? u->farFrame : u->frame;
		int bits = 5 + ((fr >> 3) & 3);
		int par = (fr >> 2) & 1;
		long long t = (u->lineFree > now) ? u->lineFree : now;
		if (t < u->feedAt) t = u->feedAt;
		int lv[12], n = 0;
		int parity = (fr >> 1) & 1;
		lv[n++] = 0;
		for (int k = 0; k < bits; k++) {
			lv[n++] = (b >> k) & 1;
			parity ^= (b >> k) & 1;
		}
		if (par) lv[n++] = parity;
		lv[n++] = 1;
		if (fr & 1) lv[n++] = 1;
		int cur = u->schN ? u->sch[(u->schHead + u->schN - 1) % UART_SCH].lv : u->lin;
		for (int k = 0; k < n; k++) {
			if (lv[k] != cur) {
				if (u->schN >= UART_SCH) break;
				u->sch[(u->schHead + u->schN) % UART_SCH].e = t + ((k * p) >> 16);
				u->sch[(u->schHead + u->schN) % UART_SCH].lv = lv[k];
				u->schN++;
				cur = lv[k];
			}
		}
		u->lineFree = t + ((n * p) >> 16);
		u->hqHead = (u->hqHead + 1) % (int)sizeof(u->hq);
		u->hqN--;
	}
}

// A settled IDLE receiver's step that would leave every register as it is: the reloads
// (timer, bit count, parity, the frame and the prescaler taken) done by an earlier one,
// nothing written to the frame or the prescaler since, RTR as the FIFO has it
static int rx_idle_still(nxUart* u) {
	int fr = u->frame;
	return !(fr & 0x80) && (u->fpresc == u->presc) && (u->timer == (u->fpresc >> 1))
		&& (u->fbits == ((fr >> 3) & 3)) && (u->bitc == (4 | u->fbits))
		&& (u->rpar == ((fr >> 1) & 1)) && (u->fpar == ((fr >> 2) & 1)) && (u->fstop == (fr & 1))
		&& (u->rtrN == ((fr & 0x20) && (u->rxStored >= UART_RXF - 2)));
}

// the receiver up to edge e, a stretch where nothing moves at a time
static void rx_run(Computer* comp, int i, long long e) {
	nxUart* u = &UART[i];
	if (u->rt < 0) u->rt = e;
	while (u->rt < e) {
		if (u->hqN) rx_feed_line(comp, u, u->rt);
		while (u->schN && (u->sch[u->schHead].e < u->rt)) {
			u->lin = u->sch[u->schHead].lv;
			u->schHead = (u->schHead + 1) % UART_SCH;
			u->schN--;
		}
		long long nc = rx_next_change(u);
		int settled = (u->lin == u->b0) && (u->b0 == u->b1) && (u->b1 == u->dbo) && (u->dbo == u->rxd) && (u->dcnt >= 4)
			&& !u->oav && !u->avd && !u->wrD && (u->oavReg == (u->rxStored > 0)) && (u->onfReg == rx_near(u));
		if (settled) {
			long long until = (nc == LLONG_MAX) ? e : ((nc + 1 < e) ? nc + 1 : e);
			if ((u->rst == RX_IDLE) && u->lin && !(u->frame & 0x40)) {
				// nothing comes: the idle reloads stand as they are (taken by one step,
				// none at all when an earlier one took them)
				if (until > u->rt + 1) {
					if (!rx_idle_still(u)) rx_step(comp, i, u->rt);
					u->rt++;
					if (u->hqN) continue;
					u->rt = until;
					continue;
				}
			} else if ((u->rst >= RX_START) && (u->rst <= RX_STOP2) && (u->timer >= 3)) {
				long long k = u->timer - 2;
				if (u->rt + k > until) k = until - u->rt;
				if (k > 0) {
					u->timer -= k;
					u->rt += k;
					continue;
				}
			}
		}
		rx_step(comp, i, u->rt);
		u->rt++;
	}
}

// == the transmitter ==

// one byte's frame in CLK_28 edges: start, 5-8 bits, parity, 1-2 stops, each a prescaler
// count long (a count of 0 or 1 is one edge: the timer has expired at once)
static long long nx_uart_frame(nxUart* u) {
	long long p = (u->presc > 1) ? u->presc : 1;
	int bits = 1 + 5 + ((u->frame >> 3) & 3) + ((u->frame >> 2) & 1) + 1 + (u->frame & 1);
	return bits * p;
}

// what was sent, a line at a time into the event log
static void nx_uart_log(Computer* comp, int i, int val) {
	nxUart* u = &UART[i];
	if ((val == 0x0d) || (val == 0x0a) || (u->len >= (int)sizeof(u->line) - 1)) {
		if (u->len) {
			u->line[u->len] = 0;
			xlog(XLG_HW, XLL_INFO, "uart%i: %s", i, u->line);
			u->len = 0;
		}
		if ((val == 0x0d) || (val == 0x0a)) return;
	}
	u->line[u->len++] = ((val >= 0x20) && (val < 0x7f)) ? val : '.';
}

// a byte leaves the FIFO for the shifter, START from edge s: tx_en on the edge before,
// the read address advancing on the edge after (rd_advance), o_Tx_empty a further edge
// on. With flow control on and CTS high it waits (S_RTR) for the host's word.
static void nx_uart_start(Computer* comp, int i, long long s) {
	nxUart* u = &UART[i];
	if ((u->frame & 0x20) && u->ctsN) {
		u->txWait = 1;
		return;
	}
	u->txWait = 0;
	u->busy = 1;
	u->end = s + nx_uart_frame(u);
	int b = u->txq[u->txHead];
	u->txHead = (u->txHead + 1) & (UART_TXF - 1);
	u->fifo--;
	if (!u->fifo) u->emptyT = s + 2;
	if (!x_runahead) {			// run ahead, it is sent again for real
		nx_uart_log(comp, i, b);
		if (next_uart_host_tx) next_uart_host_tx(comp, i, b, s);
	}
}

static void tx_run(Computer* comp, int i, long long e) {
	nxUart* u = &UART[i];
	while (1) {
		long long next = LLONG_MAX;
		if (u->emptyT >= 0) next = u->emptyT;
		if (u->busy && (u->end < next)) next = u->end;	// a byte under way ends, break or not
		if (next >= e) break;
		if (next == u->emptyT) {
			nx_int_event(comp, 12 + i, u->emptyT, (NX.nr[0xc6] >> (i ? 6 : 2)) & 1, 0);
			u->emptyT = -1;
		} else {
			u->busy = 0;			// IDLE at end; tx_en the edge after if there is more
			if (u->fifo && !(u->frame & 0xc0)) nx_uart_start(comp, i, u->end + 1);
		}
	}
}

// == both ==

// the reset every reset takes (zxnext.vhd's reset into the module): the select, both
// FIFOs, the transmitters, the receivers to PAUSE; the frame and the prescaler only come
// with the core's load. The line stays what the host drives.
void nx_uart_reset(Computer* comp) {
	NX.uartSel = 0;
	for (int i = 0; i < 2; i++) {
		nxUart* u = &UART[i];
		u->fifo = 0;
		u->txHead = 0;
		u->busy = 0;
		u->txWait = 0;
		u->end = 0;
		u->emptyT = -1;
		rx_fifo_reset(u);
		u->rst = RX_PAUSE;
		u->oav = u->avd = 0;
		u->byteD = 0;
		u->rt = -1;
	}
}

void nx_uart_power(Computer* comp) {
	for (int i = 0; i < 2; i++) {
		nxUart* u = &UART[i];
		memset(u, 0x00, sizeof(nxUart));
		u->frame = 0x18;		// 8 bits, no parity, 1 stop
		u->presc = 243;			// 115200 at 28 MHz
		u->lin = u->b0 = u->b1 = u->dbo = u->rxd = 1;	// the idle line
		u->dcnt = 4;
		u->ctsN = 0;
	}
	nx_uart_reset(comp);
}

// everything before edge e: bytes finished and the next ones started, bytes received,
// and the edges the interrupt controller sees (Rx 1/2, Tx 12/13)
void nx_uart_run(Computer* comp, long long e) {
	for (int i = 0; i < 2; i++) {
		tx_run(comp, i, e);
		rx_run(comp, i, e);
	}
}

// the first edge something here could fire on. A receiver at work with its interrupt
// enabled is looked at on every edge; one without is read when the cpu asks, unless
// stat: it sets its status bit all the same
long long nx_uart_quiet_until(Computer* comp, int stat) {
	long long q = LLONG_MAX;
	for (int i = 0; i < 2; i++) {
		nxUart* u = &UART[i];
		if ((u->emptyT >= 0) && (u->emptyT < q)) q = u->emptyT;
		if (u->busy && u->fifo && !(u->frame & 0xc0) && (u->end < q)) q = u->end;
		if (stat || ((NX.nr[0xc6] >> (i ? 4 : 0)) & 3)) {
			int busy = u->hqN || u->schN || (u->rst != RX_IDLE) || u->oav || u->avd || u->wrD
				|| (u->oavReg != (u->rxStored > 0)) || (u->onfReg != rx_near(u));
			if (busy && (u->rt >= 0) && (u->rt < q)) q = u->rt;
		}
	}
	return q;
}

// A write to 1x3B (x = 3..6) on edge w, the write strobe's first
void nx_uart_out(Computer* comp, int reg, int val, long long w) {
	int i = NX.uartSel;
	nxUart* u = &UART[i];
	nx_uart_run(comp, w);
	NX.dmaIntDead = 0;		// a new schedule for the transmitter or the receiver
	switch (reg) {
		case 0:				// 143B: the prescaler's low 14 bits, 7 at a time
			if (val & 0x80) u->presc = (u->presc & ~(0x7fL << 7)) | ((val & 0x7fL) << 7);
			else u->presc = (u->presc & ~0x7fL) | (val & 0x7f);
			break;
		case 1:				// 153B: the select, and with b4 the selected one's top 3 bits
			NX.uartSel = (val >> 6) & 1;
			if (val & 0x10) {
				nxUart* s = &UART[NX.uartSel];
				s->presc = (s->presc & 0x3fffL) | ((long)(val & 7) << 14);
			}
			break;
		case 2:				// 163B: the frame; b7 holds the FIFOs, both halves in reset
			u->frame = val;
			if (val & 0x80) {
				u->fifo = 0;
				u->txHead = 0;
				u->busy = 0;
				u->txWait = 0;
				u->emptyT = -1;
				rx_fifo_reset(u);
				u->rst = RX_PAUSE;
			} else if (!(val & 0x40) && !u->busy && u->fifo) {
				nx_uart_start(comp, i, w + 2 * (8 >> NX.speed) + 1);	// a break ended over bytes waiting
			}
			break;
		case 3:				// 133B: a byte into the Tx FIFO, stored as the strobe ends
			if ((u->frame & 0x80) || (u->fifo >= UART_TXF)) break;
			u->txq[(u->txHead + u->fifo) & (UART_TXF - 1)] = val;
			u->fifo++;
			if (!u->busy && !(u->frame & 0x40)) nx_uart_start(comp, i, w + 2 * (8 >> NX.speed) + 1);
			break;
	}
}

// A read of 1x3B at edge e (port_uart_dat, latched on the falling cpu edge). 143B takes
// the byte at the head of the Rx FIFO and lets it go as the strobe ends; 133B clears the
// sticky errors the same way.
int nx_uart_in(Computer* comp, int reg, long long e) {
	int i = NX.uartSel;
	nxUart* u = &UART[i];
	nx_uart_run(comp, e + 1);
	NX.dmaIntDead = 0;		// a byte taken from the Rx FIFO
	switch (reg) {
		case 0: {
			int v = 0;
			if (u->rxStored) {
				v = u->rxf[u->rxRd] & 0xff;
				u->rxRd = (u->rxRd + 1) & (UART_RXF - 1);
				u->rxStored--;
			}
			return v;
		}
		case 1: return (i << 6) | ((u->presc >> 14) & 7);
		case 2: return u->frame;
	}
	int busy = u->busy || u->txWait || (u->frame & 0xc0);
	int empty = !u->fifo && !busy;
	int avail = (u->rxStored > 0);
	int err9 = avail && ((u->rxf[u->rxRd] >> 8) & 1);
	int brk = (u->rst == RX_ERROR) && !u->shift;
	int v = (brk << 7) | (u->ferr << 6) | (err9 << 5) | (empty << 4) | (rx_near(u) << 3) | (u->ovf << 2)
		| ((u->fifo >= UART_TXF) << 1) | avail;
	u->ovf = u->ferr = 0;
	return v;
}

// == the host's side ==

// bytes arriving on a UART's line, sent at the far end's rate (nx_uart_far) from now on
void nx_uart_feed(Computer* comp, int i, const unsigned char* b, int n) {
	nx_uart_feed_at(comp, i, b, n, 0);
}

// the same, the first of them starting no sooner than CLK_28 edge e. An RZX being made holds
// what the CPU reads, not what comes in on a line (the ESP-01's replies, a host port's
// bytes): they will not come again when it plays - said once a recording
void nx_uart_feed_at(Computer* comp, int i, const unsigned char* b, int n, long long e) {
	static unsigned told = 0;	// the recording it was said for
	if (n && rzx_recording && (told != rzx_recording)) {
		told = rzx_recording;
		comp->msg = (char*)" UART input: this RZX will not replay it ";
		xlog(XLG_HW, XLL_WARN, "rzx: bytes from outside reached UART %i while recording: the recording will not play back the same", i);
	}
	nxUart* u = &UART[i];
	NX.dmaIntDead = 0;		// bytes on the way in
	if (!u->hqN && (e > u->feedAt)) u->feedAt = e;
	for (int k = 0; (k < n) && (u->hqN < (int)sizeof(u->hq)); k++) {
		u->hq[(u->hqHead + u->hqN) % (int)sizeof(u->hq)] = b[k];
		u->hqN++;
	}
}

// room on the host's side for more: what is still to go onto the line
int nx_uart_feed_room(Computer* comp, int i) {
	return (int)sizeof(UART[i].hq) - UART[i].hqN;
}

// a level change of the line itself after edge e (a test's schedule)
void nx_uart_line(Computer* comp, int i, long long e, int lv) {
	nxUart* u = &UART[i];
	if (u->schN >= UART_SCH) return;
	NX.dmaIntDead = 0;
	u->sch[(u->schHead + u->schN) % UART_SCH].e = e;
	u->sch[(u->schHead + u->schN) % UART_SCH].lv = lv ? 1 : 0;
	u->schN++;
}

// how the far end sends: a bit of bit16 CLK_28 edges (16.16), frame as uart_framing_r;
// 0 sends at whatever the guest's UART is set to
void nx_uart_far(Computer* comp, int i, long long bit16, int frame) {
	NX.dmaIntDead = 0;
	UART[i].farBit = bit16;
	UART[i].farFrame = frame & 0x1f;
}

// what is still waiting to go onto the line from the far end
int nx_uart_feed_left(Computer* comp, int i) {
	return UART[i].hqN;
}

// the UARTs run up to now, which is returned: what the guest has sent by now is out
long long nx_uart_sync(Computer* comp) {
	long long e = nx_now(comp);
	nx_uart_run(comp, e);
	return e;
}

// CLK_28 in Hz: a quarter of a dot
double nx_clk28_hz(Computer* comp) {
	return 4.0e9 * NS_FIXED_ONE / (double)comp->vid->nsPerDotFixed;
}

// the host's CTS (0 = go on) and what the guest asks of the host: its rate and frame, RTR
void nx_uart_cts(Computer* comp, int i, int cts_n) {
	nxUart* u = &UART[i];
	u->ctsN = cts_n ? 1 : 0;
	NX.dmaIntDead = 0;
	if (!u->ctsN && u->txWait && !u->busy && u->fifo)
		nx_uart_start(comp, i, nx_now(comp) + 1);
}

void nx_uart_cfg(Computer* comp, int i, long* presc, int* frame, int* rtr_n) {
	nxUart* u = &UART[i];
	if (presc) *presc = u->presc;
	if (frame) *frame = u->frame;
	if (rtr_n) *rtr_n = u->rtrN;
}

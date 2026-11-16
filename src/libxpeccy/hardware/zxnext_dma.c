// The ZX Spectrum Next's zxnDMA (device/dma.vhd, core 3.02.03), stepped one cpu clock
// edge at a time as the VHDL steps it. It runs on the cpu clock and asks for the bus
// with BUSRQ; the Z80 grants it at the end of any M-cycle (t80n.vhd 1738-1747). While
// the DMA holds the bus the cpu is frozen and the clocks go on in cpu->t, so the
// machine sees them as the cpu's: the ray, the copper, the frame.

#include <string.h>

#include "zxnext.h"

#define D (NX.dma)

enum {NXD_IDLE = 0, NXD_START, NXD_WAITACK, NXD_R1, NXD_R2, NXD_R3, NXD_R4,
	NXD_W1, NXD_W2, NXD_W3, NXD_W4, NXD_WAITC, NXD_FINISH};

// the register write sequence: what the next byte is
enum {NXDW_IDLE = 0, NXDW_A_LO, NXDW_A_HI, NXDW_LEN_LO, NXDW_LEN_HI, NXDW_R1_TIM, NXDW_R1_PRE,
	NXDW_R2_TIM, NXDW_R2_PRE, NXDW_MASK, NXDW_MATCH, NXDW_B_LO, NXDW_B_HI, NXDW_LOCK, NXDW_RDMASK};

// a run that never lets the bus go (continuous, auto restart) is cut after this many
// clocks: the cpu then runs one M-cycle, an emulator's compromise
#define NXD_CAP		(1 << 17)

#define NXD_NOWAIT	0xff		// D.wait: the clock's waits are still to be worked out

static int dma_cyc(int t) {
	return (t == 1) ? 3 : (t == 2) ? 2 : 4;
}

static int dma_step(int mode) {
	return (mode == 0) ? -1 : (mode == 1) ? 1 : 0;
}

#define SRC_IO		(D.dirAB ? D.ioA : D.ioB)
#define DST_IO		(D.dirAB ? D.ioB : D.ioA)
#define SRC_LEN		dma_cyc(D.dirAB ? D.lenA : D.lenB)
#define DST_LEN		dma_cyc(D.dirAB ? D.lenB : D.lenA)

// one rising edge of the cpu clock (dma.vhd 250-497); busak: BUSAK was low before it.
// The compares see the timer as it was before the edge.
static void dma_edge(Computer* comp, int busak) {
	unsigned int t0 = D.timer;
	int p = D.presc;
	D.busrqOld = D.busrq;
	D.timer = (t0 + (8 >> NX.speed)) & 0x3fff;
	switch (D.seq) {
		case NXD_IDLE:
			D.busrq = 0;
			D.T = 0;
			break;
		case NXD_START:
			if (busak || nx_dma_delay(comp, D.edge)) {
				D.busrq = 0;
			} else {
				D.busrq = 1;
				D.seq = NXD_WAITACK;
			}
			break;
		case NXD_WAITACK:
			D.a = D.src;
			D.io = SRC_IO;
			if (busak) D.seq = NXD_R1;
			break;
		case NXD_R1:
			D.timer = 0;
			switch (SRC_LEN) {
				case 2: D.seq = NXD_R4; D.wait = NXD_NOWAIT; break;
				case 3: D.seq = NXD_R3; break;
				default: D.seq = NXD_R2; break;
			}
			break;
		case NXD_R2:
			D.seq = NXD_R3;
			break;
		case NXD_R3:
			D.seq = NXD_R4;
			D.wait = NXD_NOWAIT;
			break;
		case NXD_R4:
			if (D.wait && (D.wait != NXD_NOWAIT)) {
				D.wait--;
				break;
			}
			D.data = nx_dma_bus_rd(comp, D.a, D.io);
			D.seq = NXD_W1;
			D.a = D.dst;
			D.io = DST_IO;
			D.wpend = 1;
			break;
		case NXD_W1:
			D.counter++;
			switch (DST_LEN) {
				case 2: D.seq = NXD_W4; D.wait = NXD_NOWAIT; break;
				case 3: D.seq = NXD_W3; break;
				default: D.seq = NXD_W2; break;
			}
			D.src += dma_step(D.dirAB ? D.modeA : D.modeB);
			D.dst += dma_step(D.dirAB ? D.modeB : D.modeA);
			break;
		case NXD_W2:
			D.seq = NXD_W3;
			break;
		case NXD_W3:
			D.seq = NXD_W4;
			D.wait = NXD_NOWAIT;
			break;
		case NXD_W4:
			if (D.wait && (D.wait != NXD_NOWAIT)) {
				D.wait--;
				break;
			}
			D.T = 1;
			D.a = D.src;
			D.io = SRC_IO;
			// the prescaler first, so the last byte waits too
			if (p && (p > (int)(t0 >> 5)))
				D.seq = NXD_WAITC;
			else if (D.counter < D.blen)
				D.seq = nx_dma_delay(comp, D.edge) ? NXD_START : NXD_R1;
			else
				D.seq = NXD_FINISH;
			break;
		case NXD_WAITC: {
			int w = p > (int)(t0 >> 5);
			int rq = D.busrq;
			if ((D.r4mode == 2) && w) D.busrq = 0;		// burst: the cpu has the bus meanwhile
			if (!w) {
				if (D.counter < D.blen)
					D.seq = rq ? NXD_WAITACK : NXD_START;
				else
					D.seq = NXD_FINISH;
			}
			break;
		}
		case NXD_FINISH:
			D.E = 0;
			if (D.restart) {
				D.src = D.dirAB ? D.startA : D.startB;
				D.dst = D.dirAB ? D.startB : D.startA;
				D.counter = D.zmode ? 0xffff : 0;
				D.seq = D.busrq ? NXD_WAITACK : NXD_START;
			} else {
				D.seq = NXD_IDLE;
			}
			break;
	}
}

// the edges up to upto with the bus the cpu's
static void dma_run(Computer* comp, long long upto) {
	if (D.busak || (D.edge >= upto)) return;
	if ((D.seq == NXD_IDLE) && !D.busrq && !D.busrqOld) {
		D.T = 0;
		D.edge = upto;
		return;
	}
	while (D.edge < upto) {
		if ((D.seq == NXD_WAITACK) && D.busrq && D.busrqOld) {
			// waiting for the grant: only the timer runs
			D.timer = (D.timer + (upto - D.edge) * (8 >> NX.speed)) & 0x3fff;
			D.edge = upto;
			break;
		}
		dma_edge(comp, 0);
		D.edge++;
	}
}

// The clock starting at edge e, the bus held: WAIT sampled in READ_4/WRITE_4, the
// 48K/128K stretch of a clock that starts with MREQ high, the write strobe in WRITE_1
static void dma_clock(Computer* comp, long long e) {
	int s = D.seq;
	if ((D.wait == NXD_NOWAIT) && ((s == NXD_R4) || (s == NXD_W4)))
		D.wait = nx_dma_waits(comp, D.a, D.io, s == NXD_R4, (s == NXD_R4) ? SRC_LEN : DST_LEN, e);
	int low = ((s >= NXD_R2) && (s <= NXD_R4)) || ((s >= NXD_W2) && (s <= NXD_W4));
	if (!low || D.io) nx_dma_stretch(comp, D.a);
	if ((s == NXD_W1) && D.wpend) {
		D.wpend = 0;
		nx_dma_bus_wr(comp, D.a, D.io, D.data, e);
	}
}

// The SPI master's last transfer (spiT) is over for good: nx_dma_spi answers 0 to a clock
// with no port EB strobe. The ray may stand behind cpu->t, so nx_now is early at most.
// *seen: the transfer this was found for.
static inline int dma_spi_over(Computer* comp, long long* seen) {
	if ((NX.spiT < 0) || (NX.spiT == *seen)) return 1;
	if (nx_now(comp) - NX.spiT <= 32) return 0;
	*seen = NX.spiT;
	return 1;
}

// A byte from memory to memory at 28 MHz, both ports' cycles 3 clocks, the SPI master
// done: READ_1, 3, 4 and WRITE_1, 3, 4 hold no wait and no stretch (nx_dma_waits is 0
// past a 2-clock read, nx_dma_stretch is the 3.5 MHz contention's), so the six clocks
// are dma_clock and dma_edge written out, the read and the write at their own cpu->t.
static inline int dma_plain(Computer* comp, long long* spiSeen) {
	return (D.seq == NXD_R1) && (NX.speed == 3) && !D.ioA && !D.ioB && (D.lenA == 1) && (D.lenB == 1)
		&& D.busrq && (D.rel < 0) && dma_spi_over(comp, spiSeen);
}

// e: the edge READ_1 starts on; the edge after WRITE_4
static long long dma_byte(Computer* comp, long long e) {
	CPU* cpu = comp->cpu;
	int p = D.presc;
	// once the SPI master has run, nx_dma_spi brings the ray up to the clock it is asked
	// at (READ_4's and WRITE_4's start): nx_dma_delay's look reads the time off the ray
	int spi = (NX.spiT >= 0);
	D.busrqOld = D.busrq;
	cpu->t += 2;					// READ_1, READ_3
	if (spi && (cpu->t > res4)) comp_vid_to_t(comp);
	cpu->t++;					// READ_4
	D.data = nx_dma_bus_rd(comp, D.a, 0);		// at its end
	D.a = D.dst;
	D.io = 0;
	nx_dma_bus_wr(comp, D.a, 0, D.data, e + 3);	// WRITE_1's strobe, at its start
	D.wpend = 0;
	cpu->t += 2;					// WRITE_1, WRITE_3
	if (spi && (cpu->t > res4)) comp_vid_to_t(comp);
	cpu->t++;					// WRITE_4
	D.counter++;
	D.src += dma_step(D.dirAB ? D.modeA : D.modeB);
	D.dst += dma_step(D.dirAB ? D.modeB : D.modeA);
	D.wait = 0;
	D.timer = 5;					// 0 at READ_1's end, a clock each since; 4 where WRITE_4 looks
	D.T = 1;
	D.a = D.src;
	D.io = 0;
	if (p)						// p > (4 >> 5)
		D.seq = NXD_WAITC;
	else if (D.counter < D.blen)
		D.seq = nx_dma_delay(comp, D.edge) ? NXD_START : NXD_R1;
	else
		D.seq = NXD_FINISH;
	return e + 6;
}

// The bus granted on edge g: the DMA runs until BUSRQ has been let go and the Z80 has
// seen it (BusAck drops two edges after the release, the cpu's T1 starts there)
static void dma_hold(Computer* comp, long long g) {
	CPU* cpu = comp->cpu;
	long long e = g;
	if (!D.busak) {
		// BUSRQ may be gone already (a DISABLE after the request): the Z80 grants on what
		// it registered and lets go two edges after the release
		D.rel = D.busrq ? -1 : e - 1;
		dma_edge(comp, 0);		// the grant edge
		D.busak = 1;
		if ((D.rel < 0) && !D.busrq) D.rel = e;
	}
	long long spiSeen = -1;
	for (int left = NXD_CAP; left > 0; left--) {
		if ((left >= 6) && dma_plain(comp, &spiSeen)) {
			e = dma_byte(comp, e);
			left -= 5;
			continue;
		}
		dma_clock(comp, e);
		cpu->t++;
		e++;
		dma_edge(comp, 1);
		if ((D.rel < 0) && !D.busrq) D.rel = e;
		if ((D.rel >= 0) && (e >= D.rel + 2)) {
			D.busak = 0;
			break;
		}
	}
	D.edge = e;
	comp_vid_to_t(comp);
}

// A cpu M-cycle starts (its T1 at cpu->t): the one before ended on this edge, and the
// Z80 grants the bus there if it saw BUSRQ on the edge before. 1 if the DMA took it.
int nx_dma_cycle(Computer* comp) {
	long long g = nx_edge(comp);
	if (D.busak) {				// a run cut short: it goes on at once
		D.edge = g;
		dma_hold(comp, g);
		return 1;
	}
	dma_run(comp, g - 1);
	if (!D.busrqOld) return 0;
	dma_hold(comp, g);
	return 1;
}

// == registers (dma.vhd 499-1142) ==

static int dma_rd_first(Computer* comp) {
	for (int b = 0; b < 7; b++) {
		if (D.mask & (1 << b)) return b;
	}
	return 0;
}

// the next pointer after a read: the next mask bit set, upward and round; status if none
static void dma_rd_next(Computer* comp) {
	for (int k = 1; k <= 7; k++) {
		int q = (D.rdPtr + k) % 7;
		if (D.mask & (1 << q)) {
			D.rdPtr = q;
			return;
		}
	}
	D.rdPtr = 0;
}

static int dma_wr0_next(int v, int bit) {
	static const unsigned char st[4] = {NXDW_A_LO, NXDW_A_HI, NXDW_LEN_LO, NXDW_LEN_HI};
	for (int b = bit; b < 7; b++) {
		if (v & (1 << b)) return st[b - 3];
	}
	return NXDW_IDLE;
}

static void dma_load(Computer* comp) {
	D.counter = D.zmode ? 0xffff : 0;
}

static void dma_write(Computer* comp, int v) {
	switch (D.wrSeq) {
		case NXDW_IDLE: break;
		case NXDW_LOCK: return;		// R4_BYTE_2 has no branch: only a reset ends it
		case NXDW_A_LO: D.startA = (D.startA & 0xff00) | v; D.wrSeq = dma_wr0_next(D.temp, 4); return;
		case NXDW_A_HI: D.startA = (D.startA & 0x00ff) | (v << 8); D.wrSeq = dma_wr0_next(D.temp, 5); return;
		case NXDW_LEN_LO: D.blen = (D.blen & 0xff00) | v; D.wrSeq = dma_wr0_next(D.temp, 6); return;
		case NXDW_LEN_HI: D.blen = (D.blen & 0x00ff) | (v << 8); D.wrSeq = NXDW_IDLE; return;
		case NXDW_R1_TIM: D.lenA = v & 3; D.wrSeq = (v & 0x20) ? NXDW_R1_PRE : NXDW_IDLE; return;
		case NXDW_R1_PRE: D.wrSeq = NXDW_IDLE; return;
		case NXDW_R2_TIM: D.lenB = v & 3; D.wrSeq = (v & 0x20) ? NXDW_R2_PRE : NXDW_IDLE; return;
		case NXDW_R2_PRE: D.presc = v; D.wrSeq = NXDW_IDLE; return;
		case NXDW_MASK: D.wrSeq = (D.temp & 0x10) ? NXDW_MATCH : NXDW_IDLE; return;
		case NXDW_MATCH: D.wrSeq = NXDW_IDLE; return;
		case NXDW_B_LO: D.startB = (D.startB & 0xff00) | v; D.wrSeq = (D.temp & 8) ? NXDW_B_HI : NXDW_IDLE; return;
		case NXDW_B_HI: D.startB = (D.startB & 0x00ff) | (v << 8); D.wrSeq = NXDW_IDLE; return;
		case NXDW_RDMASK: D.mask = v; D.rdPtr = dma_rd_first(comp); D.wrSeq = NXDW_IDLE; return;
	}
	D.temp = v;
	if (!(v & 0x80) && (v & 3)) {				// WR0
		D.dirAB = (v >> 2) & 1;
		D.wrSeq = dma_wr0_next(v, 3);
	} else if (!(v & 0x80) && ((v & 7) == 4)) {		// WR1
		D.ioA = (v >> 3) & 1;
		D.modeA = (v >> 4) & 3;
		D.wrSeq = (v & 0x40) ? NXDW_R1_TIM : NXDW_IDLE;
	} else if (!(v & 0x80) && ((v & 7) == 0)) {		// WR2
		D.ioB = (v >> 3) & 1;
		D.modeB = (v >> 4) & 3;
		D.wrSeq = (v & 0x40) ? NXDW_R2_TIM : NXDW_IDLE;
	} else if ((v & 0x83) == 0x80) {			// WR3: d6 enables, d6 = 0 does not disable
		if (v & 0x40) D.seq = NXD_START;
		D.wrSeq = (v & 8) ? NXDW_MASK : (v & 0x10) ? NXDW_MATCH : NXDW_IDLE;
	} else if ((v & 0x83) == 0x81) {			// WR4
		D.r4mode = (v >> 5) & 3;
		D.wrSeq = (v & 4) ? NXDW_B_LO : (v & 8) ? NXDW_B_HI : (v & 0x10) ? NXDW_LOCK : NXDW_IDLE;
	} else if ((v & 0xc7) == 0x82) {			// WR5
		D.ceWait = (v >> 4) & 1;
		D.restart = (v >> 5) & 1;
	} else if ((v & 0x83) == 0x83) {			// WR6
		switch (v) {
			case 0xc3:
				D.seq = NXD_IDLE;
				D.E = 1;
				D.T = 0;
				D.lenA = D.lenB = 1;
				D.presc = 0;
				D.ceWait = 0;
				D.restart = 0;
				break;
			case 0xc7: D.lenA = 1; break;
			case 0xcb: D.lenB = 1; break;
			case 0xcf:
				D.E = 1;
				D.src = D.dirAB ? D.startA : D.startB;
				D.dst = D.dirAB ? D.startB : D.startA;
				dma_load(comp);
				break;
			case 0xd3: D.E = 1; dma_load(comp); break;
			case 0xbf: D.rdPtr = 0; break;
			case 0x8b: D.E = 1; D.T = 0; break;
			case 0xa7: D.rdPtr = dma_rd_first(comp); break;
			case 0x87: D.seq = NXD_START; break;
			case 0x83: D.seq = NXD_IDLE; break;
			case 0xbb: D.wrSeq = NXDW_RDMASK; break;
		}
	}
}

static int dma_read(Computer* comp) {
	unsigned short a = D.dirAB ? D.src : D.dst;
	unsigned short b = D.dirAB ? D.dst : D.src;
	int v = 0;
	switch (D.rdPtr) {
		case 0: v = 0x1a | (D.E << 5) | D.T; break;
		case 1: v = D.counter & 0xff; break;
		case 2: v = D.counter >> 8; break;
		case 3: v = a & 0xff; break;
		case 4: v = a >> 8; break;
		case 5: v = b & 0xff; break;
		case 6: v = b >> 8; break;
	}
	dma_rd_next(comp);
	return v;
}

// A cpu access to port 0x6B or 0x0B, its i/o cycle's T1 at cpu->t: dma_mode is
// latched on the falling edge after the strobe, the access is taken on the next
// rising edge, after the engine's own step on that edge
static void dma_access(Computer* comp, int port) {
	long long s = nx_edge(comp);
	dma_run(comp, s + 1);
	D.zmode = ((port & 0xff) == 0x0b);
	if (D.edge < s + 2) {
		dma_edge(comp, 0);
		D.edge = s + 2;
	}
}

void nx_dma_port_wr(Computer* comp, int port, int val) {
	dma_access(comp, port);
	dma_write(comp, val & 0xff);
}

int nx_dma_port_rd(Computer* comp, int port) {
	dma_access(comp, port);
	return dma_read(comp);
}

// reset (dma.vhd 211-245); direction, port modes, start addresses, length, src and
// dest have no reset
void nx_dma_reset(Computer* comp) {
	D.seq = NXD_IDLE;
	D.a = 0;
	D.counter = 0;
	D.busrq = D.busrqOld = D.busak = 0;
	D.wrSeq = NXDW_IDLE;
	D.timer = 0;
	D.lenA = D.lenB = 1;
	D.presc = 0;
	D.r4mode = 1;
	D.ceWait = 0;
	D.restart = 0;
	D.mask = 0x7f;
	D.T = 0;
	D.E = 1;
	D.rdPtr = 0;
	D.zmode = 0;
	D.wait = 0;
	D.wpend = 0;
	D.rel = -1;
	D.edge = comp->cpu ? nx_edge(comp) : 0;
}

void nx_dma_power(Computer* comp) {
	memset(&D, 0, sizeof(D));
	nx_dma_reset(comp);
}

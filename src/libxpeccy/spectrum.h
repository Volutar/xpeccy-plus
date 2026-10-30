#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "cpu/cpu.h"
#include "video/video.h"
#include "memory.h"
#include "heatmap.h"

#include "input/input.h"
#include "tape.h"
#include "fdc.h"
#include "hdd.h"
#include "sdcard.h"
#include "divmmc.h"
#include "cartridge.h"
#include "ds1307.h"

#include "sound/ayym.h"
#include "sound/gs.h"
#include "sound/saa1099.h"
#include "sound/soundrive.h"

	#include <zlib.h>

extern int compflags;

#define CFLG_PANIC	1

// hw reset rompage
enum {
	RES_DEFAULT = 0,
	RES_48,
	RES_128,
	RES_DOS,
	RES_SHADOW
};

// what bit 6 of #FE reads with no tape playing: the machine feeds its own last
// out #FE back into the ear input, and how much of it depends on the board
enum {
	EAR_NONE = 0,		// +2A/+3: nothing comes back, the bit stays low
	EAR_ISSUE3,		// bit 4 (the speaker) alone
	EAR_ISSUE2		// bit 4 or bit 3 (the tape output)
};

// What a port nothing answers reads back. See zx_in_float() in hardware/common.c.
enum {
	FBUS_NONE = 0,	// nothing: it reads #FF
	FBUS_ULA,	// the Ferranti ULA of the 48K/128K/+2: the byte it is fetching
	FBUS_ASIC,	// the Amstrad gate array of the +2A/+3: the same, its own way
	FBUS_ATTR	// the clones' port #FF: the attribute of the cell being shown
};

enum {
	DBG_VIEW_CODE = 0x00,
	DBG_VIEW_BYTE = 0x10,
	DBG_VIEW_WORD = 0x20,
	DBG_VIEW_ADDR = 0x30,
	DBG_VIEW_TEXT = 0x40,
	DBG_VIEW_EXEC = 0x50
};

// a TSConf DMA address: 16-bit words, wrapping in its 256/512-byte block with align on
typedef struct {
	int base;	// block part, or the whole address without align
	int low;	// offset in the block the bursts start from
	int pos;	// words into the current burst
	int mask;	// block size - 1, 0 without align
} tsDmaAdr;

// a TSConf DMA transfer as the debugger lists it
typedef struct {
	int fcnt;		// vid->fcnt of the frame it was started in
	int line, dot;		// where the beam was then
	unsigned char ctrl;	// DMACtrl as written
	int src, dst;		// byte addresses, 22 bits
	int len, num;		// words a burst, bursts
	long long start;	// tickTotal then
	int dur;		// T it took, -1 while it runs
	int cut;		// a new start cut it short
} tsDmaRec;

#define TS_DMALOG	64

typedef struct {
	int t;
	int a;
	unsigned char* ptr;
} bpChecker;

#define p7FFD	reg[0]
#define p1FFD	reg[1]
#define pEFF7	reg[2]
#define prt2	reg[3]

#define flgBRK	sysflag[0]		// breakpoint catched
#define flgDBG	sysflag[1]		// debug execution
#define flgMAP	sysflag[2]		// map memory on runtime
#define flgFRM	sysflag[3]		// new frame ready
#define flgNMIRQ sysflag[4]		// NMI requested
#define flgFRN	sysflag[5]		// need to reset when switch to profile
#define flgDDP	sysflag[6]		// used ATM2+ only, but have options entry
#define flgEM1	sysflag[7]		// (same for Scorpion ZS 256)
#define flgCNTM	sysflag[8]		// contended mem (zx)
#define flgCNTI	sysflag[9]		// contended i/o (zx)
#define flgIBRK	sysflag[10]		// break on interrupt
#define flgROM	sysflag[11]
#define flgDOS	sysflag[12]
#define flgCPM	sysflag[13]
#define flgEXT	sysflag[14]
#define flgBDI	sysflag[15]
#define flgHEAT	sysflag[16]		// collect memory read/write/exec usage stats
#define flgCOND	sysflag[17]		// breakpoint conditions in use: latch mem/io events
#define flgSNOW	sysflag[18]		// ULA snow effect (zx)
#define flgSNOWX sysflag[19]		// ...and this machine's ram cannot take it
#define flgBRKPRE sysflag[20]		// the break was raised before the instruction ran
#define flgBRKMEM sysflag[21]		// a memory breakpoint may be set: 0 lets the core skip the maps
#define flgBOOT	sysflag[22]		// hw->boot is due before the next instruction

#define PWATCH_MAX	16		// ports the debugger can watch at once

// how many turbo steps a board may declare
#define TURBO_STEP_MAX 8

// How much of the RZX log being recorded counts (rzxrec.c). In the machine, so
// a rollback of it - rewind, run-ahead, a quick load - takes the log back too.
typedef struct {
	unsigned on:1;
	int fetch;		// fetches in the frame under way
	int ins;		// its IN bytes
	int t;			// its T, to end a frame with interrupts off
	int frames;		// frames before it
	int inLen;		// their IN bytes
	int snaps;		// the start and the joins
} rzxRecPos;

// The Next's picture (hardware/zxnext_vid.c): the ULA's fetch as it runs with the
// ray, and the mixer a few dots behind it, with every register change queued for
// the pixel the core shows it on
#define NXV_ULA		64		// dots of ULA output kept
#define NXV_EVQ		256		// register changes waiting for their pixel
#define NXV_WRQ		1024		// memory writes kept, for a read the core made before them
#define NXV_HIST	128		// changes kept of each value the ULA fetch samples (a power of 2)
#define NXV_FIELDS	56

typedef struct {
	long long t;		// the first half-pixel it shows on
	unsigned short f;	// NXF_*
	unsigned short x;	// a palette write's palette and index
	unsigned short v;
} nxvEvent;

typedef struct {
	long long t[NXV_HIST];	// when each change is seen (clk28: by a flop clocked after it)
	unsigned short v[NXV_HIST];
	unsigned short v0;	// the value before the oldest kept
	unsigned char n, head;
} nxvHist;

typedef struct {
	long long w;		// clk28 of the write
	int key;		// the byte: ramData offset, or NXV_SHADOW + offset in shadow[]
	unsigned char old;	// what it held before
} nxvWrite;

typedef struct {
	unsigned short vramA;	// vram_a, and the bank it reads (1: bank 7)
	unsigned char vramSh;
	unsigned char pb00, pb01, pb10, pb11, ab00, ab01, ab10, ab11;
	unsigned short px;	// the sample of n = 3 / B: px, py, screen_mode, ula_shadow
	unsigned char py, smode, sshadow;
	unsigned char mode0, mode1, scroll0, scroll1;
	unsigned char fbR, fbEn;	// floating_bus_r / _en
} nxvFetch;

// what the mixer works out of the fields at the render cursor, again only when one moves
typedef struct {
	unsigned char ulaOnly;	// no layer 2, tilemap, LoRes, ULANext, ULA+ or blend
	unsigned char plain;	// no tilemap, LoRes, ULA+ or blend: ULA (ULANext too), layer 2, sprites
	unsigned char flashOn;	// FLASH inverts (not under ULANext or ULA+)
	unsigned char upal, spal, tmpal, l2pal;	// the palettes each layer takes
	unsigned short fb9, ucx1, ucx2, ucy1, ucy2;
	short sxs, sxe, sys, sye;	// the sprite window in whc/wvc
	short l2cx1, l2cx2;		// layer 2's clip in its own column units
} nxvDerived;

typedef struct {
	nxvFetch fe;
	unsigned char flash;	// flash_cnt
	struct {
		unsigned char attr[2];
		unsigned char bits;	// b0, b1: shift_reg(15) per half; b2, b3: screen_mode_r(2)
	} ula[NXV_ULA];
	nxvHist hBrd, hPff, hShadow, hSx, hSy, hFine;
	nxvHist hTmSx, hTmSy, hTmMap, hTmTile, hTmCtl, hTmAttr;	// what the tilemap's fetch latches
	long long tmKey;		// the 8-pixel group in tmPix: its fetch's clk28
	unsigned short tmPix[8];	// b7:0 pixel, b8 below, b9 text mode
	unsigned short q[NXV_FIELDS];	// the values queued last (what the registers hold)
	unsigned short f[NXV_FIELDS];	// the values at the render cursor
	unsigned short pal[8][256];	// the palettes at the render cursor
	nxvEvent ev[NXV_EVQ];
	int evHead, evN;		// ev[evHead..evN-1], in the order they show
	int late;			// changes that came after their pixel was drawn (should stay 0)
	nxvWrite wr[NXV_WRQ];
	int wrHead;
	long long lastDot;	// the dot the fetch did last
	long long cur;		// the next dot to draw
	int rx, ry;		// its raw counters
	unsigned char live;	// set once the state is in step with the machine
	unsigned char dirty;	// f[] moved since dv was worked out
	nxvDerived dv;
} nxVideo;

// The Next's sprites (hardware/zxnext_spr.c, video/sprites.vhd)
typedef struct {
	unsigned char attr[128][5];
	unsigned char pat[16384];
	unsigned short buf[2][320];	// the line buffers: b8 a pixel there, b7:0 its index
	unsigned short attrIdx;		// attr_index: sprite << 3 | byte (port 0x57)
	unsigned short patIdx;		// pattern_index, 14 bits (port 0x5B)
	unsigned char mirror;		// mirror_sprite_q, b7 kept
	long long tieW;			// the edge the mirror last moved the tied port pointer on
	unsigned short tieOld;		// ...and where the pointer was before
	long long flagT0[2], flagT1[2];	// collision (0) and overtime (1): the first and the last event
					// a read of 0x303B has not taken yet (half CLK_28), -1 none
	// the line pass
	long long passR;		// whc became 511 on this CLK_28 edge, the pass started two later
	long long passSw;		// passR + 2
	int k;				// the next state's number (START is 1)
	int lastK;			// the last state run that was not IDLE
	long long f2;			// the falling edge of the state being run (half CLK_28)
	unsigned char st;		// the next state: IDLE, START, QUALIFY, PROCESS
	unsigned char passBuf;		// the buffer the pass draws into
	unsigned char dispOld, dispNew;	// the buffer the display reads up to the swap dot, and after
	long long dispSwap;		// the swap dot (whc 511)
	long long swPass, swBase, swVal;	// nxs_swap_after(passSw) as worked out for passSw and frameBase
	long long idleTo;		// the pass is idle and starts nothing before this half CLK_28 (0: ask)
	int swGeo;			// ...and the raster (L, hact) it was worked out on
	unsigned short vcount;		// the line the pass draws
	// the sprite being drawn (spr_cur_*)
	unsigned char idx;
	unsigned short hcount, addr, adelta;
	unsigned char width, wdelta, paloff, visible, yoff, xwrap, bits4;
	// the last anchor
	unsigned char aType, aH, aVis, aPal, aRot, aXm, aYm, aXs, aYs, aPat;
	unsigned short aX, aY;
} nxSprites;

// one of the Next's UARTs (hardware/zxnext_uart.c, serial/uart.vhd, uart_rx.vhd)
typedef struct {
	unsigned char frame;		// uart_framing_r: reset, break/pause, flow control, bits, parity, stop
	long presc;			// the 17-bit prescaler, msb_r & lsb_r
	// Tx
	unsigned char txq[64];		// the Tx FIFO
	unsigned char fifo, txHead;	// bytes in it, the first
	unsigned char busy;		// the transmitter is sending one
	unsigned char txWait;		// one waits for the host's CTS (S_RTR)
	unsigned char ctsN;		// the host's CTS, 0 = go on
	long long end;			// the CLK_28 edge it is back in IDLE
	long long emptyT;		// the edge o_Tx_empty rises on, -1 none to come
	char line[96];			// what was sent, for the event log
	int len;
	// Rx: the line, the debounce, the receiver, uart.vhd's FIFO side
	unsigned char lin, b0, b1, dcnt, dbo, rxd;
	unsigned char rst, upd, bitc, rpar, shift, fbits, fpar, fstop;
	long timer, fpresc;
	unsigned char oav, avd, wrD, ovf, ferr, oavReg, onfReg, rtrN;
	unsigned short byteD;
	unsigned short rxf[512];	// the Rx FIFO, 9 bits: the error flag and the byte
	int rxRd, rxWr, rxStored;
	long long rt;			// the receiver run up to here, -1 not yet
	struct {
		long long e;		// the line takes lv after this edge
		unsigned char lv;
	} sch[256];
	int schHead, schN;
	long long lineFree;		// the edge the bytes put on the line so far end on
	long long feedAt;		// the next byte from the far end starts no sooner
	long long farBit;		// the far end's bit in CLK_28 edges, 16.16; 0: the guest's own rate
	unsigned char farFrame;		// and its frame, as uart_framing_r (with farBit)
	unsigned char hq[8192];		// bytes from the far end still to go onto the line
	int hqHead, hqN;
} nxUart;

// The Next's interrupt controller (device/im2_*.vhd), its NMI (zxnext.vhd 2087-2170)
// and the Multiface (device/multiface.vhd)
#define NX_IRQ_DEVS	14		// line, UART0/1 Rx, CTC 0-7, ULA, UART0/1 Tx: index = priority
#define NX_HOLDS	16		// contention holds remembered for the INT pulse (a power of 2)
typedef struct {
	unsigned short stat;		// int_status, the sticky part (NR 20, C8-CA)
	unsigned short pend;		// im2_int_req: one request per source, cleared by its RETI
	unsigned char st[NX_IRQ_DEVS];	// im2_device: 0 S_0, 1 S_REQ, 2 S_ISR (S_ACK folded in)
	long long pendT[NX_IRQ_DEVS];	// CLK_28 edge the request was taken on
	struct {
		long long t;		// T1 of an M1 cycle, in tickTotal units...
		int len;		// ...and the rising edges M1 is seen low on after it
	} m1[8];			// the last ones: a request moves to S_REQ on an edge with M1 high
	int m1Head;
	unsigned char dec;		// im2_control's opcode decoder: 0, 1 after ED, 2 after CB, 3 after DD/FD
	// NMI
	unsigned char nmiState;		// 0 idle, 1 fetch, 2 hold (end is folded into idle)
	unsigned char nmiMf, nmiDiv;	// nmi_mf, nmi_divmmc: who raised the one under way
	unsigned char nmiPush;		// the acknowledge's push still to come, high byte first
	unsigned char retnC2;		// z80_stackless_retn_en: the first RETN pops NR C2/C3
	unsigned char retnPop;		// ...its reads still to come
	// Multiface
	unsigned char mfNmi;		// nmi_active
	unsigned char mfInvisible;
	unsigned char mfEnable;		// its memory paged over 0000-3FFF
	unsigned char trapCause;	// nr_da_iotrap_cause: 1 2FFD read, 2 3FFD read, 3 3FFD write
	// the DMA's hold-off held through a RETI/RETN decode (im2_control's dma_delay)
	unsigned char dmaHold;
	long long retiT;		// T4 of the 4D/45 fetch, a cpu clock edge (nx_edge)
} nxIrq;

// one CTC channel (device/ctc_chan.vhd), every flip-flop on CLK_28
typedef struct {
	unsigned char state;		// S_RESET, S_RESET_TC, S_TRIGGER, S_RUN, S_RUN_TC
	unsigned char cr;		// control_reg: D7..D3
	unsigned char tc, cnt, pre;	// time constant, t_count, p_count
	unsigned char trgD, zcD;	// clk_trg_d, zc_to_d
	unsigned char reqD;		// im2_peripheral's int_req_d for its ZC/TO
} nxCtcChan;

typedef struct {
	nxCtcChan ch[4];
	long long t;			// the next CLK_28 edge to run: every one before it is done
} nxCtc;

// The Next's zxnDMA (hardware/zxnext_dma.c, device/dma.vhd); time in cpu clock edges
typedef struct {
	unsigned char seq;		// dma_seq_s, NXD_*
	unsigned char wrSeq;		// the register write sequence (follow bytes), NXDW_*
	unsigned char temp;		// reg_temp: the base byte whose follow bytes are coming
	unsigned char rdPtr;		// the read-back pointer: 0 status, 1-6 counter lo/hi, A lo/hi, B lo/hi
	unsigned char mask;		// R6 read mask
	unsigned char dirAB;		// R0: 1 A->B
	unsigned char ioA, ioB;		// R1/R2: the port is i/o
	unsigned char modeA, modeB;	// address modes: 0 dec, 1 inc, 2/3 fixed
	unsigned char lenA, lenB;	// cycle length bits
	unsigned char presc;		// R2 prescaler
	unsigned char r4mode;		// 2 = burst
	unsigned char restart;		// R5 auto restart
	unsigned char ceWait;		// R5 CE/WAIT (stored only)
	unsigned char zmode;		// dma_mode: the last port used was 0x0B
	unsigned char T, E;		// status: at least one byte, end of block not reached
	unsigned char busrq;		// BUSRQ asserted after the last edge...
	unsigned char busrqOld;		// ...and after the one before (what the Z80 registers)
	unsigned char busak;		// BUSAK low: the DMA holds the bus
	unsigned char wait;		// READ_4/WRITE_4 clocks still to repeat
	unsigned char data;		// the byte read
	unsigned short a;		// dma_a_s: the address of the cycle under way...
	unsigned char io;		// ...an i/o one
	unsigned char wpend;		// the byte is to be written in this WRITE_1
	unsigned short startA, startB, blen;
	unsigned short src, dst, counter;
	unsigned short timer;		// 14 bits, 28 MHz periods
	long long edge;			// the last edge the engine has done
	long long rel;			// with the bus held: the edge BUSRQ was let go, -1 still held
} nxDma;

// The Next's keyboard maps (hardware/zxnext_inp.c): block RAMs, set at power-on only
typedef struct {
	unsigned char keymap[512];	// ps/2 set 2 code (b8 E0): b7 SYM, b6 CAPS, b5:3 row, b2:0 column (7 none)
	unsigned char joymap[64];	// key joysticks, 32 a joystick: b5:3 row, b2:0 column (7 none)
	unsigned char sel;		// nr_28 b7: NR 2B writes the joystick map
	unsigned short adr;		// nr_28 b0, nr_29: 9 bits
	double mx, my;			// the ps/2 mouse's counters, a fraction kept
	int hx, hy;			// the host's position they last took the moves up to
} nxInput;

// DAC changes the mixer has still to reach (hardware/zxnext_snd.c), a power of two
#define NX_DACQ		256

// The ZX Spectrum Next's state (hardware/zxnext.c), all of it in one block the snapshot
// takes on the Next, so a rollback takes it back. Names follow the core's (zxnext.vhd).
typedef struct {
	unsigned char powered;		// set at the first reset: the power-on values are in
	unsigned char firmware;		// that reset hands over as TBBLUE.FW does (the power came on)
	unsigned char nr[256];		// nextreg values as written (nr_xx)
	// what every bus cycle and every instruction end looks at, kept together
	// effective copies the core latches at a moment of its own
	unsigned char timing;		// eff_nr_03_machine_timing: 0 48K, 1 128K, 2 +3, 3 Pentagon
	unsigned char hz60;		// eff_nr_05_5060
	unsigned char speed;		// cpu_speed in effect: 0 3.5, 1 7, 2 14, 3 28 MHz
	unsigned char contDis;		// eff_nr_08_contention_disable
	unsigned char speedPend;	// a nr_07 write waiting for the next idle bus edge
	long long speedT;		// ...made at this time (CLK_28): nr_07 holds it 2 CLK_28 later
	unsigned char powerFrame;	// the first line after power-on: hc_ula still counts from its configuration value
	long long frameBase;		// dots before the current video frame (an absolute time base)
	long long pulseE;		// INT pulse: where it started (clk28 units), -1 none
	long long irqDead;		// INT stays off until this T (tickTotal + cpu->t): 0 look every time
	long long dmaIntDead;		// nx_dma_int() finds nothing before this CLK_28 edge: 0 look every time
	long long refT, refC;		// a T (tickTotal + cpu->t) and its CLK_28 edge: the clock at another T (-1: none)
	long long copWake;		// the copper does nothing before this CLK_28 edge (0: may at once)
	// the copper (device/copper.vhd)
	unsigned short copPc;		// its execution address, 10 bits
	unsigned char copDout;		// a MOVE went out on the last edge: this one is idle
	unsigned char copLast;		// last_state: the mode it ran on the last edge
	long long copT;			// the CLK_28 edges done so far
	struct {
		long long w;		// the edge its nextreg write is latched on
		unsigned char reg, val;
	} copQ[4];
	int copN;
	nxDma dma;
	unsigned char nrsel;		// port 243B
	unsigned char mmu[8];
	unsigned char p7ffd, pdffd, p1ffd, peff7;
	unsigned char p1ffdMtr;		// the +3 disk motor latch, 1FFD b3 (zxnext.vhd 3743-3757)
	unsigned char p123b, l2ofs;	// layer 2 mapping port and its bank offset
	unsigned char pe3;		// divMMC control
	unsigned char dmHeld;		// divMMC automap_held
	unsigned char dmNext;		// automap_hold, worked out at the fetch under way
	unsigned char dmMap;		// the automap output: held, or an instant entry during its fetch
	unsigned char dmButton;		// button_nmi (the divMMC NMI is not raised yet)
	unsigned char pff;		// Timex port FF
	unsigned char pfe;		// last OUT FE
	unsigned char pe7;		// SPI slave select (b0 SD0, b1 SD1, b2/b3 Pi, b7 flash; 0 = selected)
	unsigned char spiMiso;		// the byte the last SPI transfer brings in...
	unsigned char spiOld;		// ...and miso_dat until that transfer ends
	long long spiT;			// CLK_28 time the last transfer began (-1: idle long since)
	unsigned char spiDma;		// the byte a DMA write to EB puts on the bus
	unsigned char ulapSel, ulapMode;	// ULA+ BF3B (b7:6 mode group, b5:0 index)
	unsigned char resetType;	// nr_02_reset_type, 3 bits, never reset
	unsigned char configMode;	// nr_03_config_mode (the firmware leaves it off)
	unsigned char vmode;		// the video timing the board runs (NR 11): its CLK_28, 0 = 28 MHz
	unsigned char lastSram;		// the byte of the last port A SRAM read (unmapped reads give it)
	unsigned char busRomcs;		// the expansion bus ROMCS pin asks the internal ROM off
	unsigned char palPhase;		// nr_44 first/second byte
	unsigned char pal44;		// nr_44's first byte, kept for the second (NR 28 reads it)
	unsigned char clipIdx[4];	// L2, sprites, ULA, tilemap clip window indices
	unsigned char clip[4][4];
	unsigned short coppAdr;		// copper upload address, 11 bits
	unsigned char coppStored;	// nr_63's first byte
	unsigned char coppMem[2048];
	unsigned short pal[8][256];	// palettes: 9-bit colour + 2 priority bits (b10:9)
	unsigned char eff05, eff09;	// eff_nr_05 scandoubler (b0), eff_nr_09 scanlines (b1:0)
	long long holds;		// cpu clocks held by 48K/128K contention: T counted, no clock edge
	long long holdC[NX_HOLDS];	// the last holds: no cpu edge at holdC + 8, ..., holdC + 8 * holdD (CLK_28)
	unsigned char holdD[NX_HOLDS];
	unsigned char holdHead;
	unsigned char cuOffset;		// nr_64 as cvc took it at the start of the paper
	unsigned short vcLine0, cvcLine0;	// vc_ula and cvc on the last line of the frame before (they count on)
	unsigned short wvcLast;			// wvc there, likewise
	int vsyncCount;			// frames as the simulation counts them (vsync edges)
	int pulseN;			// ...and how many cpu edges it lasts (0: one CLK_28, the count at its end)
	long long lineChk;		// the line interrupt compare is done up to here (clk28 units)
	unsigned char expbusEff;	// expbus_eff_en: the expansion bus as the cpu side has taken it
	nxIrq irq;
	nxCtc ctc;
	struct {
		unsigned char sel;		// the AY the TurboSound select points at (0..2)
		unsigned char pan[3];		// its L (b1) and R (b0) enables
		unsigned char dac[4];		// DAC channels A..D
		unsigned char out[4];		// ...as the mixer plays them, the changes below still to come
		struct {
			long long e;		// the CLK_28 edge a DAC write landed on
			unsigned char v[4];
		} q[NX_DACQ];
		int qHead, qN;
	} snd;
	unsigned char shadow[0x6000];	// the SRAM copies of pages 0A, 0B, 0E (layer 2 sees these)
	unsigned char p3fb;		// p3_floating_bus_dat: the last byte of a contended cycle in +3 timing
	long long nrLastW;		// the edge of the last nextreg write, whoever made it
	long long memChg0, memChg1;	// port_memory_change_dly is high on these edges: an MMU write is dropped
	int inVid;			// a nextreg write made from inside the video tick (the copper's)
	struct {
		long long w;		// a copper write's edge, and what its register read before it:
		unsigned char reg, old;	// the copper runs to the end of the dot, a 253B read samples earlier
	} copLog[8];
	int copLogHead;
	nxSprites spr;
	nxVideo v;
	nxInput inp;
	unsigned char i2cScl, i2cSda;	// the bit-banged I2C master's lines, 1 released (ports 103B, 113B)
	nxUart uart[2];			// ESP, Pi
	unsigned char uartSel;		// uart_select_r: which one the ports reach
} xNextState;

typedef struct Computer {
	struct HardWare *hw;	// computer core - misc params, callbacks

	double cpuFrq;
	double frqMul;		// overclock the user asked for, not a real machine's
	double hwMul;		// turbo the machine is running at now
	// The board's own turbo step. A machine that switches turbo from a port
	// overwrites hwMul from there; on one whose turbo is a switch on the case
	// this is all there is, so a reset puts hwMul back to it rather than to 1.
	double turboStep;
	// the steps this board has, x1 first, from its definition
	double turboTab[TURBO_STEP_MAX];
	int turboCount;
	unsigned char intVector;

	int brkt;		// breakpoint type (cpu, ram, rom...)
	int brka;		// breakpoint addr
	// last mem/io event, for breakpoint conditions (-1 = didn't happen)
	struct {
		int rd, wr, mdt;
		int in, out, val;
		int kind;	// MEM_BRK_* of the access that fired it
		int adr;	// cpu address it was made at (-1 = no access)
	} brkev;
	int brkray;		// beam position (dots from frame start) before this instruction
	int brkpc;		// pc before this instruction: where a breakpoint says it is
	// ports the debugger watches. A hit is (bus & mask) == port; a port the
	// machine keeps a copy of is read from there instead of the bus, the rest
	// hold the last value that went through them (-1 = nothing yet). A port
	// switched off keeps its place in the list, it is just not watched
	struct {
		int port;
		int mask;
		int type;		// what the machine keeps it in...
		size_t offset;		// ...and where, 0 = watch the bus
		int (*get)(struct Computer*);	// or what works it out, NULL none
		int on;
		int hw;			// came from the machine's own table
		int val;
	} pwatch[PWATCH_MAX];
	int pwcount;
	int pwbus;			// how many of them the bus has to be watched for

	char* msg;		// message ptr for displaying outside
	int resbank;		// rompart active after reset
	int snapModel;		// SNAP_HW_* a snapshot names this machine as, 0: by its core
	int romCustom;		// bytes of rom a snapshot put in, until the machine's own are loaded again
	int earback;		// EAR_*: the ear input with no tape playing
	int earRC;		// the ear input holds its charge after bit 4 drops (zx_ear)
	double earV;		// that charge, 0..1, as of earTick
	int earTick;		// tickCount + cpu->t when earV was
	int earDead;		// ...and when bit 6 reads 0 again after bit 4 went down
	int fbus;		// FBUS_*: what a port nothing answers reads back
	unsigned char fbusLast;	// last byte to or from contended memory (FBUS_ASIC)
	int fbusTick;		// tickCount + cpu->t of fbusLast

	int snowBad;		// the ULA took a refresh cycle: the next opcode out of
				// slow memory comes back wrong (flgSNOWX)

	int frmCount;		// frames since reset (for breakpoint conditions)
	int tickCount;		// accumulate T
	long long tickTotal;	// T since the machine was made: tickCount wraps in minutes
	int intTaken;		// maskable interrupts the cpu has taken, for the debugger's spans
	int frmtCount;		// accumulate T, but reset each INT
	int hCount;		// T before HALT = frmtCount @ HALT
	// a quiet run going (comp_run_quiet): the devices are owed the time of the
	// instructions it has finished, runStep (16.16 ns of the video's step not made
	// into ns yet); an i/o access gives it to them first (comp_run_io)
	int runOn;
	int runIO;		// ...and the run ends with that instruction
	int runNs;		// ns given to them so far
	long long runStep;
	int waitDebt;		// T a board with its own arbiter still owes the next bus cycle
	int waitPaid;		// what comp_cont_hw() last paid of it, for comp_owe()
	int fCount;		// T in last frame
	double nsPerTick;	// real ns/T, kept precise: truncating this to int was the
				// root cause of a systemic ~0.3-0.9% emulation speed error
				// (see nsPerSampleFixed-driven pacing in ethread.cpp)
	// nsPerTick in 16.16. The per-instruction sync paths run on it: it sits on
	// every memory access, and llround() is an out-of-line CRT call with gcc
	// at -O2. Derived by comp_update_timings(), never set by hand.
	long long nsPerTickFixed;
	// fixed ns: how long before now the sound sample being taken was due - a step can
	// run past several (a DMA holding the bus), and a machine that keeps the times of
	// its level changes plays them where they were
	long long sndAgo;
	// time as the machine lived it (comp_ns): ns at nsBaseTick, where the rate last
	// changed; a turbo switch moves only what follows it
	long long nsBase;
	long long nsBaseTick;
	// contention wait, dots -> ticks, derived by comp_update_timings() off the
	// two periods. vid_wait_dots() tops out at 14, so 16 entries cover it.
	// xstate copies it with the rest of the head: same periods, same table.
	int contTicks[16];
	double earTpt;		// ticks in a 3.5 MHz T (comp_update_timings), zx_ear_edge's scale

	unsigned char verblk[16];	// what the machine says it is (comp_set_identity)
	bool flag[128];			// each machine have its own flags
	bool sysflag[32];		// some common flags used by several machines or debuga
	unsigned char reg[512];		// internal registers
	xreg32 xreg[32];		// combined 32/16/8 bits registers
// base
	CPU* cpu;
// TODO: align memory/brkMaps, respect ram/rom size
	Memory* mem;
	Video* vid;
// input
	Keyboard* keyb;
	Joystick* joy;
	Joystick* joyb;
	Mouse* mouse;
// data storage
	Tape* tape;
	DiskIF* dif;
	IDE* ide;
	SDCard* sdc;
	xCartridge* slot;		// cartridge slot (Interface II, ALF)
	DivMMC* div;			// divMMC on the edge connector
// sound
	bitChan* beep;
	TSound* ts;
	GSound* gs;
	SDrive* sdrv;
	saaChip* saa;
// misc
	CMOS cmos;


	struct {
		unsigned start:1;	// opened: the emulation thread starts it (rzx_begin)
		unsigned play:1;
		unsigned overio:1;
		unsigned noint:1;	// the snapshot just played in was taken between two INTs
		rzxRecPos rec;
		int fTotal;
		int fCurrent;
		int fCount;
		FILE* file;		// converted tmp-file
		// Where in it the frame after this one starts and where this one's IN
		// bytes are (a repeat frame's are an earlier frame's): a rollback puts
		// the file back by them, see rzx_reread()
		long long next;
		long long dataAt;
		struct {
			int fetches;
			int size;
			unsigned char data[0x10000];
			int pos;
		} frm;
	} rzx;


	// brkRamMap..brkIOMap is skipped by the state snapshot: 4.6 MB of debugger
	// bookkeeping that no rollback needs, so xstate.c takes this struct as the
	// ranges around it. A field that DOES belong in a snapshot has to go before
	// brkRamMap or after brkIOMap - one put between them would be left out with
	// nothing to say so. (The rzx frame buffer above is skipped the same way.)
	unsigned char brkRamMap[MEM_4M];	// ram brk/type : b0..3:brk flags, b4..7:type
	unsigned char brkRomMap[MEM_512K];	// rom brk/type : b0..3:brk flags, b4..7:type
	unsigned char brkAdrMap[MEM_64K];	// adr brk
	unsigned char brkIOMap[MEM_64K];	// io brk
	xHeatBank heatRam;			// ram read/write/exec hit counters, sized to mem->ramMask+1
	xHeatBank heatRom;			// rom read/write/exec hit counters, sized to mem->romMask+1
	// the ZX Spectrum Next's ~120K, apart from the struct: a snapshot takes them only on the Next
	xNextState* next;			// the ZX Spectrum Next
	DS1307* rtc;				// the Next's clock, on its I2C bus: on its own battery, not reset with it
	// TODO: try to move this somewhere
	struct {
		unsigned char Page0;
		unsigned char p21af;
		unsigned char pwr_up;		// 1 on 1st run, 0 after reading 00AF
		unsigned char cacheConf;	// CacheConfig: b0..3 cache windows 0..3
		unsigned short cache[256];	// tags of the 256-word cache: b15 valid, {page, A13..A9}
		int cpuDram;			// DRAM cycles the cpu took since the last sync
		struct {			// a DMA transfer in progress
			unsigned char act, dev, asz, sat;
			int len, bursts, word, data;
			tsDmaAdr s, d;
			long long free;		// DRAM cycles banked, in 1/448ths
			int time;		// dots banked, for the device side of SPI and IDE
			long long frac;		// ns (16.16) not yet a whole dot
		} dma;
		struct {			// the transfers started lately, for the debugger
			tsDmaRec rec[TS_DMALOG];
			unsigned count;		// started so far: rec[count % TS_DMALOG] is the next
		} dmalog;
	} tsconf;
} Computer;

// The two conversions the per-instruction sync paths need. Not inverses of each
// other: one produces fixed point ns, the other consumes it.

static inline long long ticks_to_ns_fixed(Computer* comp, int t) {
	return (long long)t * comp->nsPerTickFixed;
}

// wait states end on a tick boundary, so round up. Exact: the dot and the
// tick come off the same crystal, so the quotient is whole on every zx.
static inline int ns_fixed_to_ticks_up(Computer* comp, long long ns_fixed) {
	return (int)((ns_fixed + comp->nsPerTickFixed - 1) / comp->nsPerTickFixed);
}

// Walk the video up to the cpu's t. res4 tracks the t the video stands at, so
// a second call in the same place costs nothing. On a contended machine the
// miss path of comp_cont leaves res4 lagging cpu->t mid-instruction: whatever
// reads the ray inside an instruction walks the video first (comp_vid_to_t),
// vid_settle() alone does not cover the lag.
extern int res4;

static inline void comp_vid_to(Computer* comp, int t) {		// dots drawn through
	vid_sync_fixed(comp->vid, ticks_to_ns_fixed(comp, t - res4));
	res4 = t;
}

static inline void comp_vid_to_t(Computer* comp) {		// counting allowed
	vid_sync_lazy(comp->vid, ticks_to_ns_fixed(comp, comp->cpu->t - res4));
	res4 = comp->cpu->t;
}

// the same as comp_vid_to(), the dots only counted where a frame is not drawn:
// for what reads the ray through vid_settle() or vid_lazy_dots() alone
static inline void comp_vid_to_lazy(Computer* comp, int t) {
	vid_sync_lazy(comp->vid, ticks_to_ns_fixed(comp, t - res4));
	res4 = t;
}

// A board that drops its turbo for a while owes the next bus cycle what a step
// took once more: the step's own ticks, not the debt it paid on the way, or the
// debt grows with every step. Called after each step, owing or not.
static inline void comp_owe(Computer* comp, int ns, int owe) {
	if (owe) {
		int t = (int)((NS_TO_FIXED(ns) + comp->nsPerTickFixed / 2) / comp->nsPerTickFixed);
		if (t > comp->waitPaid) comp->waitDebt += t - comp->waitPaid;
	}
	comp->waitPaid = 0;
}

// Something of the debugger's looks at every memory access: the map, the heat
// map, a condition or a memory breakpoint
// Nothing looks between the steps: no breakpoint, condition or debugger, no run-ahead
// pass, no recording made or played - what fast mode's quiet loop and its run on need
static inline int comp_unwatched(Computer* comp) {
	return !comp->flgCOND && !comp->flgDBG && !comp->flgBRKMEM && !comp->flgIBRK && !x_runahead
		&& !comp->rzx.play && !comp->rzx.rec.on;
}

static inline int comp_mem_watched(Computer* comp) {
	return comp->flgMAP | comp->flgHEAT | comp->flgCOND | comp->flgBRKMEM;
}

// A side-effect-free view of the byte the cpu sees. Hardware with overlays that
// do not live in Memory.map supplies the callback at the end of HardWare.
enum {
	MEM_VIEW_READ = 0,
	MEM_VIEW_WRITE = 1,
	MEM_VIEW_FETCH = 2,
	MEM_VIEW_ROM_WRITE = 4,
	// the map as it stands, for the machine's own accesses. Without it, while the
	// debugger holds the machine, a divMMC paging armed by the last fetch is shown
	// as done: the next fetch, wherever it is, does it (8 is the Next's own)
	MEM_VIEW_LIVE = 16
};

// Physical memory by the names people use for it (labels, breakpoints, MEMMAP):
// an area, a page in the unit the machine pages that area by, an offset in it.
// RAM is 16K banks, but the Next's 8K pages; divMMC banks are 8K.
enum {
	CELL_NONE = 0,
	CELL_RAM,
	CELL_ROM,
	CELL_ALTROM,	// the Next's alt ROMs
	CELL_DIVROM,	// divMMC's ROM (EEPROM): one page
	CELL_DIV,	// divMMC's RAM banks
	CELL_MFROM,	// the Next's Multiface ROM and RAM: one page each
	CELL_MFRAM,
	CELL_SLT
};

typedef struct {
	int area;	// CELL_*
	int page;
	int ofs;
	int unit;	// the page size
} xCell;

#include "hardware/hardware.h"

Computer* compCreate();
void comp_set_identity(Computer*, const char*, int, int);
void compDestroy(Computer*);
void compReset(Computer*,int);
void compUserReset(Computer*,int);
int compExec(Computer*);
int comp_peek(Computer*, int, int, xAdr*);
void comp_poke(Computer*, int, int, int);
xAdr comp_get_xadr(Computer*, int, int);
int comp_find_xadr(Computer*, int, int, int);
xCell comp_cell(Computer*, int type, int abs);			// a cell (MEM_* and abs) by its area
int comp_cell_xadr(Computer*, int area, int page, int ofs, int* type, int* abs);	// ...and back, 0: no such cell
int comp_cell_unit(Computer*, int area);			// the page size of an area
int comp_cell_pages(Computer*, int area);			// how many pages it has, 1: no number
int comp_cell_size(Computer*, int area);			// all its pages, in bytes
int comp_cell_peek(Computer*, int area, int page, int ofs);	// a cell's byte, -1: none. ofs may run into the next pages
int comp_cell_rom(int area);				// ROM, alt ROM, divMMC's EEPROM, Multiface ROM, the slot
int comp_cell_poke(Computer*, int area, int page, int ofs, int val, int romWrite);	// ...a write: ROM areas with romWrite only, 0: not written
const char* cell_token(xCell, char* buf);	// a named area's page as text: ROM0, DIV3, DIVROM; "" for RAM
int cell_token_parse(const char*, int* area, int* page);	// ...and back, 0: not a named area
// Preview one instruction and up to size raw bytes without changing the machine.
xMnem compDisasm(Computer*, int, char*, unsigned char*, int);
int comp_skip_ticks(Computer*, int);
int comp_halt_skip(Computer*, int* ns);		// HALT steps up to the next event at once, fast mode
int comp_halt_skip_rt(Computer*, int* ns, long long room);	// the same in real time, a machine with hwQuiet.halt
int comp_exec_quiet(Computer*);			// compExec() with no breakpoint, debugger or recording
int comp_run_quiet(Computer*, int* ns, const int* stops, int nstops);	// steps run on in one go, fast mode
void comp_run_io_slow(Computer*);
static inline void comp_run_io(Computer* comp) {	// before a port is touched
	if (comp->runOn) comp_run_io_slow(comp);
}
void comp_irq(int, void*);

//void compKeyPress(Computer*, int, keyEntry*);
//void compKeyRelease(Computer*, int, keyEntry*);
void comp_kbd_release(Computer*);

void compSetBaseFrq(Computer*,double);
void compSetTurbo(Computer*,double);
void compSetHwTurbo(Computer*,double);
void comp_update_timings(Computer*);
long long comp_ns(Computer*);
int compSetHardware(Computer*,const char*);
void comp_set_layout(Computer*, vLayout*);
void comp_set_snow(Computer*, int);
void comp_set_cont(Computer*, int);
void comp_set_cont_pattern(Computer*, int);	// CONT_*: which pattern the ULA contends with
void comp_snap_map(Computer*);
void comp_snap_reset(Computer*, int);
int comp_frame_ticks(Computer*);
double comp_fps(Computer*);		// frames a second of emulated time
void comp_set_frame_tick(Computer*, int);
void comp_power_phase(Computer*);	// the frame position a machine is switched on at
void comp_board_storage(Computer*);	// disks and hard disks out while a divMMC is on
void comp_power_on(Computer*);		// the power comes on: before the reset that follows
int comp_get_frame_tick(Computer*);

void rzxStop(Computer*);
// After a rollback: the file back where the machine's frame stands, and the
// frame's IN bytes, which the state snapshot leaves out
void rzx_reread(Computer*);
// The recording open for playback, 0 for none. Not in the machine, so a
// rollback cannot bring back one that has been closed since.
extern unsigned rzx_playing;
// The same for the recording being made (rzxrec.c)
extern unsigned rzx_recording;

void comp_brk(Computer*, int);
void comp_brk_newstep(Computer*);

void comp_media_tick(Computer*);
void comp_pwatch_clear(Computer*);
void comp_pwatch_add(Computer*, int, int, int);
void comp_pwatch_sync(Computer*);
int comp_pwatch_val(Computer*, int);

unsigned char* getBrkPtr(Computer*, int);
unsigned char getBrk(Computer*, int);
unsigned char getBrkView(Computer*, int, int);
void setBrk(Computer*, int, unsigned char);

#ifdef __cplusplus
}
#endif

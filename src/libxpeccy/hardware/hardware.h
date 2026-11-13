#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "../spectrum.h"
#include "../sound/sndmix.h"
#include <assert.h>

// hw type
enum {
	HW_NULL = 0,
	HW_DUMMY,	// nothing-to-do-here
	HW_ZX48,	// ZX Spectrum 48K
	HW_ZX128,	// ZX Spectrum 128K (and the grey +2, which is one)
	HW_PENT,	// Pentagon
	HW_P1024,	// Pentagon1024SL
	HW_SCORP,	// Scorpion ZS 256
	HW_PLUS2A,	// ZX Spectrum +2A
	HW_PLUS3,	// ZX Spectrum +3
	HW_ATM1,	// ATM 1
	HW_ATM2,	// ATM 2+
	HW_PENTEVO,	// ZX Evolution (BaseConf)
	HW_TSLAB,	// ZX Evolution (TSConf)
	HW_PROFI,	// Profi
	HW_PHOENIX,	// ZXM Phoenix
	HW_ALF,		// ALF TV Game (a ZX48 clone console)
	HW_SCORPTP,	// Scorpion ZS 256 Turbo+
	HW_PROFI3,	// Profi v3.x (HW_PROFI is v5)
	HW_ZXNEXT	// ZX Spectrum Next
};

// Hardware callbacks

// std callback
typedef void(*cbhwcomp)(Computer*);
// sync: calls after every CPU command
typedef void(*cbHwSnc)(Computer*, int);
// memory read
typedef int(*cbHwMrd)(Computer*, int, int);
// memory write
typedef void(*cbHwMwr)(Computer*, int, int);
// io read
typedef int(*cbHwIrd)(Computer*, int);
// io write
typedef void(*cbHwIwr)(Computer*, int, int);
// int request
typedef void(*cbHwIrq)(Computer*, int);
// int ack
typedef int(*cbHwAck)(Computer*);
// key press/release
typedef void(*cbHwKey)(Computer*, keyEntry*);
// get volume
typedef sndPair(*cbHwVol)(Computer*, sndVolume*);
// side-effect-free memory for the debugger: value is output on reads and input
// on writes; NULL asks only which physical cell the access reaches
typedef void(*cbHwMemView)(Computer*, int, int, int*, xAdr*);
typedef xMnem(*cbHwDisasm)(Computer*, int, char*, unsigned char*, int, int fmt);

typedef int(*cbHwIntVecs)(Computer*, xIntVec*);

// a port the machine keeps by itself: where in Computer, or a function when
// it is not a field there (offset 0 then)
typedef struct {
	int port;
	int type;
	size_t offset;
	int (*get)(Computer*);
} xPortDsc;

// Fast mode puts a machine's steps together (comp_halt_skip, comp_run_quiet): the
// devices are handed the time of many steps at once and the per-step part of its
// sync runs on its own. What a machine's step end is made of, for that: anything its
// sync or its INT acknowledge does at every step has to be named here, or a run on
// skips it. A machine with none is stepped one at a time.
enum {
	HWQ_ACK_NONE = 0,	// no INT sample at the end of an instruction
	HWQ_ACK_ZX,		// zx_irq_ack_in() - zx_irq and the handlers that ack as it does
	HWQ_ACK_TS,		// ts_irq_ack_in() - the line and dma INTs held until acked
	HWQ_ACK_NEXT		// next_irq_ack() - the Next's pulse of its own (zxnext.c)
};
typedef struct {
	int ack;			// HWQ_ACK_*, also what comp_end() does for the core
	int (*ok)(Computer*);		// steps may go together now (no board turbo, no dma); NULL: always
	void (*step)(Computer*);	// the sync's own per-step work beside the devices; NULL: none
	void (*devs)(Computer*, int);	// the sync's devices, given the summed time (comp_run_io)
	int (*fetch)(Computer*, int);	// an M1 at adr changes nothing (the HALT run-on); NULL: no run-on
	int lineInt;			// a line INT bounds the HALT run-on as the frame INT does (TSConf)
	// The HALT run-on its own way, in real time and run-ahead passes too: the T of one
	// M1 at adr when a run of them changes nothing but the time, R and what haltDone
	// puts back (0: step), and the step start the last one has to stay short of
	int (*halt)(Computer*, int adr, long long* until);
	void (*haltDone)(Computer*, int adr, long long t0, long long n, int t);
	// The real-time HALT run-on may pass the sound's samples, taken after it: what the
	// machine's mix reads stands still meanwhile or is read at the sample's own time
	// (comp->sndAgo). NULL or 0: the run stops at every sample
	int (*sndLate)(Computer*);
} hwQuiet;

struct HardWare {
	int id;			// id
	const char* name;	// name used in conf file
	const char* optName;	// name used in options window
	int mask;		// mem size bits (see memory.h)
	double xscale;		// pixel ratio (x:y)
	vLayout* lay;		// fixed layout ptr. if NULL, use from config
	xPortDsc* portab;	// tab of ports descriptors
	cbhwcomp init;		// init (call on setting comp hardware)
	cbhwcomp mapMem;	// map memory
	cbHwIwr out;		// io wr
	cbHwIrd in;		// io rd
	// mem rd/wr. A memory-mapped port behind them (TSConf's FM window) calls
	// comp_run_io() first, as iord/iowr do: a quiet run owes the devices time
	cbHwMrd mrd;		// mem rd
	cbHwMwr mwr;		// mem wr
	cbHwIrq irq;		// int rq
	cbHwAck ack;		// int ack
	cbhwcomp reset;		// reset
	cbHwSnc sync;		// sync time
	cbHwKey keyp;		// key press
	cbHwKey keyr;		// key release
	cbHwVol vol;		// read volume
	cbhwcomp snapmap;	// stand where a snapshot expects the machine (NULL: nothing to do)
	cbHwIrq cont;		// a bus cycle starts in turbo, the ray synced to it (NULL: the ULA's contention)
	const hwQuiet* quiet;	// its step end, for fast mode's run on (NULL: none)
	int contAll;		// cont is asked at every speed, not only in turbo (the Next contends by itself)
	cbhwcomp power;		// the power comes on, before the reset that follows (NULL: the reset is all)
	cbhwcomp boot;		// the firmware's work, before the first instruction after flgBOOT is set
	int ownLay;		// the machine lays its raster out itself (the Next's follows its timing): a layout asked for is not taken
	int contLazy;		// cont walks the ray itself, when it needs it (comp_cont_hw leaves it)
	cbHwMemView memView;	// cpu-visible memory behind overlays (NULL: Memory.map is the view)
	cbHwDisasm disasm;	// instruction preview with paging across its M1 cycles
	// The sound split in two (NULL: vol does it all on the emulation thread): what the
	// machine itself does at a sample and gathers for the mix there, and the mix of
	// that, which can run on a copy of the devices (sound.cpp's mirror)
	void (*sndtake)(Computer*, sndMix*);
	sndPair (*sndmix)(sndMix*, sndVolume*);
	// the INT sources on now and their ack bytes, nothing acked (NULL: one floating #FF)
	cbHwIntVecs intvecs;
};
typedef struct HardWare HardWare;

typedef struct {
	int mask;		// if (port & mask == value & mask) port is catched
	int value;
	unsigned dos:2;		// 00:!dos only; 01:dos only; 1x:nevermind
	unsigned rom:2;		// same: b4,7FFD
	unsigned cpm:2;		// cpm mode (for profi)
	int (*in)(Computer*, int);
	void (*out)(Computer*, int, int);
} xPort;

typedef struct {
	int id;
	HardWare* core;
} tabHwItem;

int hwIn(xPort* ptab, Computer* comp, int port);
void hwOut(xPort* ptab, Computer* comp, int port, int val, int mult);

// extern HardWare hwTab[];

HardWare* findHardware(const char*);
HardWare* findHardwareId(int);

// the ZX Evo's clock, BaseConf and TSConf alike (pentevo.c)
void evo_cmos_adr(Computer*, int);
int evo_cmos_rd(Computer*);
void evo_cmos_wr(Computer*, int);

typedef struct {
	int res;		// RES_* that lands in this bank, -1 none
	const char* name;	// what the bank holds, NULL unknown
} xRomRole;

xRomRole hw_rom_role(int hw, int bank);
int hw_reset_bank(int hw, int res);
// A fetch on a Beta Disk machine that pages TR-DOS: out, from ram while it is
// in; in, from #3Dxx of the basic rom while it is out
static inline int bdi_fetch_pages(Computer* comp, MemPage* pg, int adr) {
	return comp->flgDOS ? mem_m1_dos_out(pg) : (comp->flgROM && mem_m1_dos_in(pg, (adr >> 8) & 0xff));
}
int stdMRd(Computer*, int, int);
void stdMWr(Computer*, int, int);
int asicMRd(Computer*, int, int);	// stdMRd/stdMWr plus the +2A/+3 bus latch
void asicMWr(Computer*, int, int);
int scoMRd(Computer*, int, int);	// stdMRd plus the Scorpion's even M1 and ROM switch
// the DOS flag an opcode fetch at adr leaves, by the machine's own rule
int evo_fetch_dos(Computer*, int adr);
int tsl_fetch_dos(Computer*, int adr);

// the ZX Spectrum Next (zxnext.c): a test bench's hooks on every i/o and nextreg write
extern void (*next_log_out)(Computer*, int, int);
extern void (*next_log_nr)(Computer*, int, int, int);	// reg, value, cpu ticks the request is past cpu->t
void next_counters(Computer*, int* hc, int* vc, int* cvc);	// hc_ula, vc_ula, cvc where the ray stands
void next_counters_ahead(Computer*, int clk28, int* hc, int* vc, int* cvc);	// ...that many CLK_28 later
int next_vsync_frame(Computer*, int clk28);	// frames by the VS_n output edge, that many CLK_28 later
extern int next_reset_open;	// the core's reset outputs lead nowhere (the simulation's testbench)
extern void (*next_board_reset)(Computer*);	// a reset key of the board (F1, F4, F9 with a digit) reset the machine: the app's part of a user's reset
extern int next_ear_tied;	// the EAR input held at 0, no board around the core (the same)
// the Next's UARTs from outside (hardware/zxnext_uart.c): 0 ESP, 1 Pi
// a byte the guest sends, as it goes: its start bit begins at CLK_28 edge start
extern void (*next_uart_host_tx)(Computer*, int uart, int byte, long long start);
void nx_uart_feed(Computer*, int uart, const unsigned char*, int);	// bytes arriving on its line
void nx_uart_feed_at(Computer*, int uart, const unsigned char*, int, long long edge);
int nx_uart_feed_room(Computer*, int uart);
int nx_uart_feed_left(Computer*, int uart);
void nx_uart_far(Computer*, int uart, long long bit16, int frame);	// the far end's rate and frame
double nx_clk28_hz(Computer*);
extern int next_esp_fitted;		// an ESP-01 is in its socket (none: its GPIO pins read 0)
int nx_esp_reset(Computer*);		// NR 02 b7: the ESP held in reset
extern int next_esp_pins;		// what the ESP drives: b0 GPIO0 out, b1 its level, b2 GPIO2 out, b3 its level
int nx_esp_gpio0(Computer*);		// the level on the ESP's GPIO0
int nx_esp_gpio2(Computer*);		// the level on the ESP's GPIO2
long long nx_uart_sync(Computer*);	// the UARTs run up to now, the CLK_28 edge returned
void nx_uart_line(Computer*, int uart, long long edge, int level);	// a level change after a CLK_28 edge
void nx_uart_cts(Computer*, int uart, int cts_n);
void nx_uart_cfg(Computer*, int uart, long* presc, int* frame, int* rtr_n);
long long nx_now(Computer*);
void nxv_settle(Computer*);	// a still frame: flash_cnt counted up to the ray, for a snapshot
extern long long next_log_at;	// a copper write being logged: its CLK_28 edge (-1: the cpu's)
extern long long next_log_t;	// a DMA i/o write being logged: the CLK_28 time standing for the ray (-1: none)...
extern long long next_log_cyc;	// ...and its cpu clock count
void next_counters_at(Computer*, long long clk28, int* hc, int* vc, int* cvc);	// ...at that CLK_28 time
int next_vsync_frame_at(Computer*, long long clk28);
void next_audio(Computer*, int* l, int* r, int* env);	// the mixer's pcm_L/R (13 bits), psg0's env_vol
void next_snap_loaded(Computer*, int vmode);	// a snapshot put the board back; vmode: NR 11's before
int next_mmu_get(Computer*, int);
void next_mmu_set(Computer*, int, int);
void next_ram_size(Computer*, int size);	// the SRAM fitted, MEM_1M or MEM_2M; the map is the caller's
int next_ram_pages(Computer*);		// 8K RAM pages the cpu reaches: 0x60 on 1 MB, 0xE0 on 2 MB
int next_cell(int abs, int* page, int* ofs);		// an SRAM image cell's area (CELL_*), page and offset
int next_cell_abs(int area, int page, int ofs);		// ...and back, -1: no such cell
int next_shadow_sram(int abs);		// the SRAM image place of a layer 2 byte behind a BRAM page, -1: none
// the Next for a viewer (the debugger's panel): nothing here moves the machine
int next_nr_peek(Computer*, int reg);		// a nextreg as a read gives it, nothing run up to now first
void next_nr_poke(Computer*, int reg, int val);	// ...and written at once, for a debugger
void next_os_version(Computer*, char* buf, int len);	// "v2.09": NextZXOS's, from its ROM in SRAM; "" with none
int next_ram(Computer*, int page, int ofs);	// a byte of 8K RAM page as the cpu reads it
int next_l2ram(Computer*, int page, int ofs);	// ...as layer 2 reads it: the SRAM behind the BRAM pages
int next_rom(Computer*);			// the ROM slots 0 and 1 show with MMU FF
unsigned int next_rgb(int rgb9);		// RGB333 as R | G << 8 | B << 16
typedef struct {
	int x, y;		// 9 bits, a relative sprite's resolved
	int pat;		// in 128-byte halves: an 8-bit pattern is pat >> 1
	int bits4;		// a 4-bit pattern
	int pal;		// palette offset
	int xm, ym, rot;
	int xs, ys;		// scale 1, 2, 4, 8 as 0..3
	int vis;		// visible, the anchor's visibility applied
	int rel;		// a relative sprite...
	int anchor;		// ...of this anchor; -1 for an anchor
	int unified;		// the anchor's relatives are unified, not composite
	int ext;		// attribute byte 4 is in use
} nxSpriteView;
void next_sprites(Computer*, nxSpriteView* out);	// all 128, as the sprite pass decodes them

// common IO

int zx_dev_wr(Computer*, int, int);
// a write a Z-Controller takes: 1 when its IORQGE keeps it from the board's own ports
// (another card on the bus still sees it)
int zx_zc_wr(Computer*, int, int);
int zx_dev_rd(Computer*, int, int*);
void zx_irq(Computer*, int);
// a divMMC can sit on this machine's edge connector (the Sinclair models)
int hw_takes_divmmc(const HardWare*);
int hw_takes_gluk(const HardWare*);
int hw_kempston8(const HardWare*);	// a Kempston with Fire 2..4 on d5..d7 can be fitted
int hw_has_sd(const HardWare*);		// the SD slot (comp->sdc): ZX Evo, TSConf, the Next
// a Z-Controller can sit on this machine's ZX-BUS (the Pentagons, the Scorpions)
int hw_takes_zc(const HardWare*);
// comp->sdc is in the machine: the board's own slot, or a Z-Controller's
int comp_has_sd(const Computer*);
void zx_snow(Computer*);
void zx_contend_adr(Computer*, int, int);	// address already past vid_cont_hit, video at cpu->t
int zx_bank_of(Computer*, int);
int zx_ack(Computer*);
int zx_ear(Computer*);
// the 48 basic rom itself is running: paged in at #0000, with neither TR-DOS nor
// an extension holding the window. The tape trap asks the same question.
static inline int zx_rom_active(Computer* comp) {
	return comp->flgROM && !comp->flgDOS && !comp->flgEXT
		&& (comp->mem->map[0].type == MEM_ROM);
}
void zx_tape_detect(Computer*);
enum {ZX_IN_EAR = 1, ZX_IN_KEYS};
int zx_in_use(Computer*, int);
int zx_rom_code(Computer*, int pc);

void xOutFE(Computer*, int, int);
void xOutBFFD(Computer*, int, int);
void xOutFFFD(Computer*, int, int);

int xIn1F(Computer*, int);
int zx_in_float(Computer*, int);	// a port nothing answers: the floating bus
int zx_in_joy(Computer*, int);		// #1F: Kempston, or the floating bus without one
int xInFE(Computer*, int);
int xInFFFD(Computer*, int);
int xInFADF(Computer*, int);
int xInFBDF(Computer*, int);
int xInFFDF(Computer*, int);

// common_zx calls
void zx_init(Computer*);
void zx_reset(Computer*);
void zx128_map_mem(Computer*, int);	// 128K paging, shared with the Pentagon; arg is the bank extension mask
void zx_set_vmode(Computer*);	// pick the screen drawer for this ula
void zx_keyp(Computer*, keyEntry*);
void zx_keyr(Computer*, keyEntry*);
void zx_sync(Computer*, int);
// the step ends of the plain ZX machines (hwQuiet): zx_sync's devices, and an M1
// that changes nothing (stdMRd/asicMRd, the ULA's contention)
extern const hwQuiet hwq_zx;		// with the HALT run-on
extern const hwQuiet hwq_zx_nohalt;	// a read of its own (scoMRd, alf_mrd): steps run on, HALT is stepped
void zx_devs_sync(Computer*, int);
int zx_fetch_quiet(Computer*, int);
int hwq_no_turbo(Computer*);		// ok() of a board turbo: a run only at 3.5 MHz
sndPair zx_vol(Computer*, sndVolume*);
// The mixes themselves, over what they read gathered in a sndMix (sound/sndmix.h)
sndPair zx_mix(sndMix*, sndVolume*);
sndPair alf_mix(sndMix*, sndVolume*);
sndPair nx_mix(sndMix*, sndVolume*);
void snd_mix_take_wire(sndMix*, Computer*);	// the tape and the beeper into a mix
void zx_snd_take(Computer*, sndMix*);
void alf_snd_take(Computer*, sndMix*);
void zx_set_pal(Computer*);	// todo: called from zx_reset only

// tsconf SYSCONF: the cpu speed (the .spg loader sets it too)
void tsOut20AF(Computer*, int, int);

// TSConf registers where the machine keeps them, for the debugger. The ones it
// keeps no byte of are given as what they set (clock, dmactrl from the last start).
typedef struct {
	int vconf, vpage, gxoffs, gyoffs, palsel, border;
	int tsconf, tmpage, t0gpage, t1gpage, sgpage, t0xoffs, t0yoffs, t1xoffs, t1yoffs;
	int page[4];		// Page0 as written, Page1..3 as mapped
	int memconf, fmaddr, fmen, cacheconf, fddvirt;
	int clock;		// ZCLK the cpu runs at: 0 3.5, 1 7, 2 14 MHz
	int intmask, hsint, vsint, vsinc;
	int dmasrc, dmadst;	// byte addresses, 22 bits
	int dmalen, dmanum;	// as written, one less than the count
	int dmactrl;		// the last DMACtrl that started one, -1 none yet
	int dmaact;
	// a transfer running: where it is at, the burst it is in (from 1) of how many,
	// and the words of it done
	int dmaCurSrc, dmaCurDst, dmaBurst, dmaBursts, dmaWord, dmaWords;
	// written, waiting for the next line start (TSL_LAT_*), and what they will be
	// then: the value now for one not waiting
	unsigned short lat;
	int latVconf, latVpage, latGx, latGy, latPal, latT0x, latT1x, latT0g, latT1g;
} tsRegs;
void ts_get_regs(Computer*, tsRegs*);

// What a cpu write to adr lands in besides memory: the FMAPS window of FMAddr (zmaps.v)
enum {TS_FM_NONE = 0, TS_FM_CRAM, TS_FM_SFILE, TS_FM_REGS};
int ts_fmaps_part(Computer*, int adr);

// The ZX Evolution BaseConf state, for the debugger: what the #xxBE/BD readback block
// gives, taken from the same places
typedef struct {
	int p7ffd, peff7, pbf;
	int p77;		// a14.a9.a8.0.d3..d0 of the last write to #xx77
	int dos, vdos, vnmi;
	int clock;		// 0 3.5, 1 7, 2 14 MHz
	int pg[8];		// the pager's eight entries, (ROM bit of 7FFD) * 4 + window:
	int pgflag[8];		// b7 7FFD/DOS mixed in, b6 RAM, b5 write protect
	int pgset;		// 0 or 4: the four entries in use now
	int trap;		// the breakpoint address of #BF b4
	int pal[16];		// as last written through #FF
	int border;		// the palette cell the border is
	int virt;		// virtual drives, b3..0
	int ext[4];		// #2F #4F #6F #8F
} evoRegs;
void evo_get_regs(Computer*, evoRegs*);

#ifdef __cplusplus
}
#endif

// emulation thread (non-GUI)

#include <QWaitCondition>
#include <QThread>
#include <QTime>
#include <atomic>
#include <vector>

#include "ethread.h"
#include "xcore/xcore.h"
#include "xgui/xgui.h"
#include "xcore/sound.h"
#include "xcore/pacing.h"
#include "xcore/autostart.h"
#include "xcore/fastload.h"
#include "xcore/rzxseek.h"
#include "xcore/rzxrecord.h"
#include "xcore/rewind.h"
#include "xcore/vidrec.h"
#include "xcore/tapetrap.h"
#include "xcore/vfilters.h"
#include "xcore/serial.h"
#include "libxpeccy/cpu/Z80/z80.h"
#include "libxpeccy/xstate.h"
#include "libxpeccy/xreplay.h"
#include "libxpeccy/filetypes/szx.h"
#include "version.h"

#if USEMUTEX
QMutex emutex;
QWaitCondition qwc;
#else
int sleepy = 1;
#endif

// Guards the emulated machine against the GUI thread. The GUI takes it while it
// switches profile, hardware or romset: those rebuild the machine (and even
// create it, profiles are initialized on first use), so the emulation must not
// run at the same time. Recursive: the profile calls nest into each other.
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
static QRecursiveMutex emuGuard;
#else
static QMutex emuGuard(QMutex::Recursive);
#endif

// Threads waiting for the lock. The emulation thread takes it back the moment it
// lets go, and in fast mode it never sleeps in between, so a waiter it does not
// step aside for gets it only when fast mode ends.
static std::atomic<int> emuWaiters(0);

void emu_lock() {
	if (emuGuard.tryLock()) return;
	emuWaiters.fetch_add(1);
	emuGuard.lock();
	emuWaiters.fetch_sub(1);
}

void emu_unlock() {
	emuGuard.unlock();
}

// What the gui shows is a copy of bufimg taken at the frame's end, not bufimg
// itself: antiflicker mixes into it in place and run-ahead draws over it, so a
// gui running late - a window being dragged - read a frame half made.
static std::vector<unsigned char> shownImg;
static QMutex shownGuard;
static int shownSwaps = -1;		// bufSwaps of the copy, -1: none yet
static long long shownNs = 0;
static InState shownInput;
static int shownInputOn = 0;

// fast mode and fast forward make frames far quicker than anyone looks at them
#define SHOWN_FAST_NS 10000000LL

void frame_publish() {
	QMutexLocker lock(&shownGuard);
	shownImg.resize(bufSize);
	memcpy(shownImg.data(), bufimg, bufSize);
	shownSwaps = bufSwaps;
	shownNs = paceClockNs();
	shownInputOn = iosd_state(conf.zx, &shownInput);
}

int frame_shown_input(InState* st) {
	QMutexLocker lock(&shownGuard);
	*st = shownInput;
	return shownInputOn;
}

// Set from the frame signal until the gui has taken it. A gui held up for a while
// would otherwise work through a queue of frames long gone, one repaint each.
static std::atomic<int> frameOwed{0};

void frame_taken() {
	frameOwed = 0;
}

// 1 when the gui is to be told: it shows the newest copy whenever it gets there
static int frame_hand_over() {
	bool quick = conf.emu.fast || (conf.emu.speed > 1.0);
	if ((shownSwaps != bufSwaps) && (!quick || (paceClockNs() - shownNs >= SHOWN_FAST_NS)))
		frame_publish();
	return !frameOwed.exchange(1);
}

// A paused machine is only drawn by the gui thread itself, so bufimg is safe then.
const unsigned char* frame_shown_lock() {
	shownGuard.lock();
	// a copy from a machine with a smaller raster is not read past its end
	if (conf.emu.pause || ((int)shownImg.size() < bufSize)) return bufimg;
	return shownImg.data();
}

void frame_shown_unlock() {
	shownGuard.unlock();
}

#define LOG_OUTPUT 0
#if LOG_OUTPUT
static FILE* file = nullptr;
#endif

// unsigned char* blkData = NULL;

xThread::xThread() {
	sndNsFixed = 0;
	benchStop = -1;
	benchRzx = 0;
	benchRzxSeek = -1;
	benchRec = NULL;
	benchRecFrames = 0;
	benchRecBack = 0;
	benchRecJoin = 0;
	benchRecMark = 0;
	benchRecRoll = 0;
	benchRecTake = 0;
	benchRecFinal = 0;
	benchRecNmi = 0;
	benchKeys = NULL;
	benchHashEvery = 1;
	benchDrawEvery = 0;
	earBlock = -1;
	conf.emu.fast = 0;
	finish = 0;
}

void xThread::stop() {
	finish = 1;
#if USEMUTEX
	qwc.wakeAll();
#else
	sleepy = 0;
#endif
}

// The rom's load has returned with the tape near the end of the block, not in
// the middle of it: a loader that has the rom read only part of a block reads
// the rest itself, so the tape must go on. A few pulses are the checksum's
// last edges and the closing one.
static int tap_block_done(Tape* tap) {
	if (tap->block >= tap->blkCount) return 1;
	return ((int)tap->blkData[tap->block].sigCount - tap->pos) < 32;
}

// The tape is into the block's data: its pilot is behind it, so the rom back at
// LD_START can only find the next block's. A loader that had the rom read just
// the head of a block (Chuckie Egg, Cosmopolice) leaves the tape there.
static int tap_past_pilot(Tape* tap) {
	TapeBlock* blk = &tap->blkData[tap->block];
	return (blk->dataPos > 0) && (tap->pos > blk->dataPos);
}

// The return address on top of the stack - or the one under it, when the top
// one is skip: a routine the loader's own code calls through on the way
static int tap_caller(Computer* comp, int skip) {
	int sp = comp->cpu->regSP;
	int ret = cpu_peek_word(tap_peek, comp, sp);
	return (ret == skip) ? cpu_peek_word(tap_peek, comp, sp + 2) : ret;
}

// the SA/LD-RET address the LD-BYTES at base pushes
static int tap_ld_ret(Computer* comp, int base) {
	return cpu_peek_word(tap_peek, comp, base + LDC_RET_OP);
}

// The edge routine was called by the LD-BYTES it belongs to, not by a loader of
// its own (Krakout, calling the rom's), which never comes back to LD_START to be
// handed a block and is played to instead. LD-EDGE-2 calls LD-EDGE-1 itself: its
// caller is a word up. Nor does LD-8-BITS (05CD): a timeout there returns to
// whoever called LD-BYTES, or to a loader that jumped straight into it for a
// block with no sync (Tutankhamun). Nor LD-SYNC (0594, 059E), which a loader
// that found the pilot itself jumps into (Jasper): a stopped tape reads as the
// machine's own #FE, often an edge, and the second call then times out of
// LD-BYTES instead of going back to LD_START.
static int tap_rom_caller(Computer* comp, int base) {
	int ret = tap_caller(comp, (base + LDC_EDGE2_RET) & 0xffff);
	int off = (ret - base) & 0xffff;
	if ((off == LDC_BITS_RET) || (off == LDC_SYNC1_RET) || (off == LDC_SYNC2_RET)) return 0;
	if (base == LD_ROM_BASE) return ret < 0x4000;
	return off < LDC_LEN;		// a copy is called by itself
}

// atStart says the rom is at LD_START, the top of LD_BYTES, rather than inside
// LD_EDGE_1: only there does the stack hold what LD_BYTES itself pushed, so only
// there may a block be handed over and the rom sent to its own exit. Doing it
// from LD_EDGE_1 returned into the middle of LD_BYTES with the block eaten - the
// first block of a load went missing that way.
// base is where LD-BYTES starts, #0556 or a copy's; dir is its INC or DEC IX.
void xThread::tap_catch_load(Computer* comp, int atStart, int base, int dir) {
	Tape* tap = comp->tape;
	// Stop, pressed by hand, keeps the automatics off the tape - flash loading
	// included, which moves it on without ever playing it. Play, a rewind or
	// another tape hands it back.
	if (tap->userStop) return;
	// A load asks for a tape that has run out: "Rewind at end" winds it back here
	// too, not only under the Play button - to the block asked for, its flag (A')
	// and its length. Nothing to rewind for if neither of the automatics is on -
	// Play does it.
	if (atStart && (tape_flash() || conf.tape.autostart))
		tap_rewind_for(tap, comp->cpu->regAa & 0xff, comp->cpu->regDE);
	// A tape stopped past its block's pilot holds no pilot the rom can find
	// before the next block's - the detector stops it between the rom's
	// header and its data (Eye of Vartan's LOAD "" takes three frames there)
	if (atStart && !tap->on && (tap->block < tap->blkCount) && (tap->pos > 0)
			&& (tap_block_done(tap) || tap_past_pilot(tap)))
		tapNextBlock(tap);
	// The block it comes to cannot be the one asked for, and that one is further
	// on: wound on to it. The rom's own LD-BYTES only - a copy may not take its
	// flag and length as the rom does.
	if (atStart && (base == LD_ROM_BASE) && (tape_flash() || conf.tape.autostart))
		tap_wind_on_for(tap, comp->cpu->regAa & 0xff, comp->cpu->regDE);
	int blk = tap->block;
	if (blk >= tap->blkCount) return;
	// A loader that has the rom read only the first part of a block and reads
	// the rest itself (Technician Ted) needs the block played, not handed over:
	// the rom is left to read that part by ear.
	if (atStart)
		earBlock = (tap->blkData[blk].hasBytes && (tapGetBlockInfo(tap, blk).size > comp->cpu->regDE)) ? blk : -1;
	if (tape_flash() && tap->blkData[blk].hasBytes && (blk != earBlock)
			&& (atStart || tap_rom_caller(comp, base))) {
		// A playing tape and a flash load get out of step: the rom reads the
		// block by ear and moves on while the tape still stands on it, and the
		// next block is then answered with this one. Flash loading owns the
		// tape, so stop it; the rom's edge loop gives up within a few hundred
		// T and comes back to LD_START, where the block is handed over.
		if (!atStart || tap->on) {
			// a block the tape has already played out (to a loader of its
			// own) is not the one the rom asks for now
			if (tap->on && (tap_block_done(tap) || (atStart && tap_past_pilot(tap))))
				tapNextBlock(tap);
			tapStop(tap);
			return;
		}
		tap_hand_over(comp, blk, base, dir);
	// A block with no bytes (#15, #18, #19) cannot be handed over: under flash
	// loading it is played for the rom to read, not waited on.
	} else if ((conf.tape.autostart || (blk == earBlock)
			|| (atStart && tape_flash() && !tap->blkData[blk].hasBytes)) && !tap->on) {
		// 05E7 is LD-EDGE-1, which the rom calls for every edge, so this is
		// reached thousands of times per block - hence the guard. The window
		// is not told: it refreshes itself, and a signal per edge once left
		// stale ones in the queue that restarted a tape already stopped.
		tapPlay(tap);
	}
}

// F after AND/OR/XOR leaves a: S, Z, the two undocumented bits, P/V as parity
static int tap_logic_f(unsigned char a) {
	return (a & 0xa8) | (a ? 0 : 0x40) | (parity(a) ? 0x04 : 0);
}

// The block goes into memory as LD-BYTES would have read it, and the cpu leaves
// LD-BYTES the way it would have after that.
void xThread::tap_hand_over(Computer* comp, int blk, int base, int dir) {
	Tape* tap = comp->tape;
	int copy = (base != LD_ROM_BASE);
	unsigned short de = comp->cpu->regDE;
	unsigned short ix = comp->cpu->regIX;
	// read before the block lands: it may cover the stack
	int ldret = (cpu_peek_word(tap_peek, comp, comp->cpu->regSP) == tap_ld_ret(comp, base));
	TapeBlock* tb = &tap->blkData[blk];
	int bytes = tap_block_bytes(tb);
	unsigned char* blkData = (unsigned char*)malloc(bytes + 2);
	tapGetBlockData(tap,blk,blkData, bytes + 2);
	// Every way out of LD-BYTES (#05A9..#05E2) as its code leaves it: the flag
	// byte, DE bytes, the parity; H is the XOR of all, L the last, B #B0 after
	// each. Where it leaves sets PC.
	CPU* cpu = comp->cpu;
	int load = cpu->regFa & 1;		// carry in F': LOAD, else VERIFY
	unsigned char flag = blkData[0];
	unsigned char h = flag;
	unsigned char l = flag;
	int avail = bytes - 1;			// after the flag, the parity among them
	int n = 0;
	int exitPc = LDC_TAIL;			// LD A,H / CP 1 / RET
	int fl = -1;				// else A and F as they come out, through the RET
	unsigned char a = 0;
	cpu->regB = 0xb0;
	if (de == 0) {
		// asked for nothing: it reads the flag byte, never compares it, and
		// leaves with A = H = the flag (the rom's zero-length bug)
	} else if (flag != (cpu->regAa & 0xff)) {
		// LD-FLAG RL C / XOR L / RET NZ: A is the difference, AF' what the
		// loop's EX AF,AF' left there - LD A,D / OR E. Nothing is loaded, and
		// the tape plays the rest of the block to no one: how a loader passes
		// over the blocks it does not want (Popeye 3's levels)
		a = (cpu->regAa & 0xff) ^ flag;
		fl = tap_logic_f(a);
		unsigned char de8 = (de >> 8) | (de & 0xff);
		cpu->regAa = de8;
		cpu->regFa = tap_logic_f(de8);
		exitPc = LDC_TAIL + 3;
	} else {
		// LD-FLAG's RL C / XOR L / LD A,C / RRA / LD C,A leaves C as the edge
		// loop had it after the flag byte, bit 7 dropped, and EX AF,AF' keeps
		// that A with the flags of XOR and RRA to the end. Every edge CPLs C,
		// so its bit 5 is the ear level then, the one the first data pulse
		// starts on.
		if (tb->dataPos >= 0) {
			int idx = tb->dataPos + 16;
			int lev = (idx < tb->sigCount) ? TAP_VOL_LEV(tb->data[idx].vol) : !TAP_VOL_LEV(tb->data[idx - 1].vol);
			unsigned char c = cpu->regC ^ tap_peek((base + LDC_SYNC_XOR) & 0xffff, comp);
			if (!!(c & 0x20) != lev) c = ~c;
			c &= 0x7f;
			cpu->regC = c;
			cpu->regAa = c;
			cpu->regFa = 0x44 | (c & 0x28) | load;
		}
		// The loop: a byte is read and XORed into H, then stored - or, when
		// verifying, LD A,(IX+0) / XOR L / RET NZ.
		while ((de > 0) && (n < avail)) {
			unsigned char byte = blkData[n + 1];
			h ^= byte;
			l = byte;
			if (!load) {
				unsigned char mem = memRd(comp->mem, ix);
				if (mem != byte) {
					a = mem ^ byte;
					fl = tap_logic_f(a);
					exitPc = LDC_TAIL + 3;
					break;
				}
			} else {
				memWr(comp->mem, ix, byte);
			}
			ix += dir;
			de--;
			n++;
		}
		if (fl >= 0) {
			// a byte that does not verify
		} else if (de == 0 && n < avail) {
			// the next byte is read as the parity
			l = blkData[n + 1];
			h ^= l;
		} else if (de > 0) {
			// asked for more than the block holds: the parity went in as data
			// and the edge loop times out in the pause - INC B to 0, A = 0, Z
			// and H set, NC (The Balrog and the Cat loads a block of unknown
			// length so)
			cpu->regB = 0;
			l = 1;
			a = 0;
			fl = 0x50;
			exitPc = LDC_BITS_RET;
		}
	}
	cpu->regL = l;
	cpu->regH = h;
	cpu->regIX = ix;
	cpu->regDE = de;
	if (fl >= 0) {
		cpu->regA = a;
		cpu_set_flag(cpu, fl);
	}
	// the block is in memory and the tape never moved for it, so a loader of
	// its own that reads the next one is handed the tape when it asks - one
	// the trap reads waits for the rom after a real pause (Fuse leaves it at
	// the block's pause)
	int nxt = blk + 1;
	int arm = (nxt < tap->blkCount) && !tap_block_rom(&tap->blkData[nxt]);
	// A block with no pause after it, or one too short to end a block, runs
	// on into the next as on a deck: a loader reading that one times it from
	// here. It plays now, after that pause, from the level the block ended on.
	// A block the trap reads too: a program may let it play by unread (Loco-
	// Motion's dummy, under its puzzle), and the trap, asked with the tape
	// past that block's pilot, moves on to the next.
	int last = tb->sigCount ? tb->data[tb->sigCount - 1].vol : 0x80;
	int gap = 0;
	for (int i = tb->sigCount - 1; (i >= 0) && TAP_VOL_PAUSE(tb->data[i].vol); i--)
		gap += tb->data[i].size;
	tapNextBlock(tap);
	fastload_forget();
	if ((tap->block < tap->blkCount) && (!TAP_VOL_PAUSE(last) || (gap < TAPE_PAUSE_TICKS))) {
		tap_play_on(tap, last);
		tap->sigLen = gap;
	} else if (arm) {
		tapArmPlay(tap);
	}
	if (copy)
		xlog(XLG_TAPE, XLL_INFO, "block %i handed to the copy of LD-BYTES at %04X, the tape %s at block %i",
			blk, base, tap->on ? "plays on" : "stands", tap->block);
	cpu_set_pc(comp->cpu, (base + exitPc) & 0xffff);
	// A loader that enters LD-BYTES past its PUSH of SA/LD-RET (JP #0562)
	// keeps the border the edge loop left: blue, the data phase's colour
	// for a tape back at the level it started on, not the lead-in's. A
	// copy has colours of its own, and keeps what it has.
	if (!ldret && !copy)
		comp->hw->out(comp, 0x09fe, 0x09);
	free(blkData);
}

void xThread::tap_catch_save(Computer* comp) {
	if (tape_flash()) {
		unsigned short de = comp->cpu->regDE;	// len
		unsigned short ix = comp->cpu->regIX;	// adr
		unsigned char crc = comp->cpu->regA;	// block type
		unsigned char* buf = (unsigned char*)malloc(de + 2);

		buf[0] = crc;
		for(int i = 0; i < de; i++) {
			buf[i + 1] = memRd(comp->mem, (ix + i) & 0xffff);
			crc ^= buf[i + 1];
		}
		buf[de + 1] = crc;

		TapeBlock blk = tapDataToBlock((char*)buf, de + 2, NULL);
		tap_add_block(comp->tape, blk);
		blkClear(&blk);
		free(buf);
		cpu_set_pc(comp->cpu, 0x53e);
		// comp->cpu->regPC = 0x053e;
		comp->cpu->regBC = 0x000e;
		comp->cpu->regDE = 0xffff;
		comp->cpu->regHL = 0x0000;
		comp->cpu->regA = 0x00;
		cpu_set_flag(comp->cpu, 0x51); // comp->cpu->f = 0x51;
	} else if (conf.tape.autostart) {
		emit tapeSignal(TW_STATE, TWS_REC);
	}
}

// what to do when a breakpoint fired

void xThread::brkAction(Computer* comp, xBrkPoint* ptr, int* brkskip) {
	QString fnams;
	QFile file;
	int idx;
	brk_log_hit(ptr, comp);		// logged whatever else it does, the debugger included
	// TODO: fetch break continues to repeat, comp->brk=0 is not enough?
	switch (ptr->action) {
		case BRK_ACT_COUNT:			// counted by the caller, just go on
			break;
		case BRK_ACT_SCR:
			fnams = shot_dir().append(SLASH);
			fnams.append(QString("xpeccy_%0").arg(QTime::currentTime().toString("HHmmss_zzz")));	// TODO: counter-based name (1ms is not enough)
			idx = 0;
			do {
				file.setFileName(QString("%0_%1.scr").arg(fnams).arg(idx));
				idx++;
			} while (file.exists());
			if (!file.open(QFile::WriteOnly) || (file.write((char*)(comp->mem->ramData + (5 << 14)), 0x1b00) != 0x1b00))
				xlog(XLG_APP, XLL_WARN, "can't save screenshot %s", file.fileName().toLocal8Bit().data());
			file.close();
			break;
		default:					// BRK_ACT_DBG
			conf.emu.pause |= PR_DEBUG;
			emit dbgRequest();
			return;
	}
	// everything but the debugger goes on; a break raised before the cpu got
	// to run - a fetch, or an interrupt about to be taken - has to step over
	// itself, or the same check fires again and the machine stands still
	comp->flgBRK = 0;
	if (comp->flgBRKPRE) *brkskip = 1;
}

// Run-ahead.
//
// A Spectrum reads the keyboard early in a frame and has drawn its answer by
// the end of it, so a key press reaches the screen a frame or two after it
// happened. That is the machine's own share of the input lag, and no display
// or sound tuning can reach it. Run-ahead pays it off with processor time:
// copy the machine, run the copy on to the frame the player would only see
// later, show that frame, then put the machine back and carry on.
//
// Two things stay out of the copy on purpose. Input, so a key pressed during
// the ahead frame is not undone by the rollback - that is the whole point. And
// sound: the ahead pass writes nothing to the ring, so the pacer and the rest
// of the sound path work exactly as before. The price of that is an A/V
// offset - the picture now runs one frame in front of the sound.
//
// Both passes draw. The buffers swap twice per shown frame, so each pass keeps
// to a buffer of its own and the one shown is always the ahead pass's; the real
// pass redraws a picture nobody sees, which is the biggest single waste here.
// vid->nodraw would save it, but the ray is drawn during a frame and the
// decision to run ahead is taken at the end of one, so suppressing it needs the
// answer a frame early - and a wrong guess (a tape starting, a disk command
// beginning mid-frame) shows an undrawn frame.

static xState* raState = NULL;
static Computer* raOwner = NULL;	// what raBroken was decided about
static int mixFrame = -2;		// conf.vid.fcount of the last frame antiflicker mixed
static int raBroken = 0;		// the snapshot cannot be taken at all: stop trying
// The keyboard is not in the snapshot - a rewind must not bring back keys let go
// since - but a frame run ahead reads the ps/2 log and runs its repeat, so it is
// put back with the machine
static Keyboard raKbd;

// The snapshot run-ahead and the antiflicker's look ahead wind back to, when this
// frame allows one. xstate_safe() answers for everything the snapshot does not
// carry; what is left here is this side's own policy.
static int ra_snapshot(Computer* comp, int finish) {
	if (comp != raOwner) {		// a new machine may well fit where the last one did not
		raOwner = comp;
		raBroken = 0;
	}
	if (finish || raBroken) return 0;
	// fast forward already runs several frames per frame shown: no room for a second pass
	if (conf.emu.fast || (conf.emu.speed > 1.0) || conf.emu.pause || comp->flgDBG || rewind_active()) return 0;
	if (autostart_busy()) return 0;		// the typist counts frames of its own
	if (!xstate_safe(comp)) return 0;
	if (!raState) raState = xstate_create();
	if (!raState || !xstate_save(raState, comp)) {
		raBroken = 1;
		return 0;
	}
	raKbd = *comp->keyb;
	return 1;
}

static void ra_wind_back(Computer* comp) {
	xstate_load(raState, comp);
	*comp->keyb = raKbd;
}

static int ra_run_frame(Computer* comp) {
	if (xstate_run_frame(comp)) return 1;
	xlog(XLG_CORE, XLL_WARN, "run ahead: the machine does not finish a frame, giving up");
	raBroken = 1;
	return 0;
}

// The frames run ahead at a frame end: run-ahead shows the last of conf.emu.runahead
// of them, the adaptive antiflicker's look ahead mixes with the AF_AHEAD after it.
// *shown: how many run-ahead takes.
static int ra_frames(Computer* comp, int* shown) {
	// a recording takes no input to win time on, and fast forward shows a frame in so many
	if (conf.emu.tmode == XTM_FFWD) return *shown = 0;
	int r = comp->rzx.play ? 0 : conf.emu.runahead;
	int l = 0;
	if (!conf.emu.fast && (noflic > 0) && noflicAhead
			&& ((noflicMode == AF_2C_ADAPTIVE) || (noflicMode == AF_3C_ADAPTIVE))
			&& (comp->hw->id != HW_ZXNEXT))		// too dear there
		l = AF_AHEAD;
	*shown = (r > 0) ? r : 0;
	return *shown + l;
}

// They are kept from one frame end to the next. A frame run ahead from the machine as
// it stood at the last frame end is its own next frame too, unless something reached it
// from outside meanwhile (x_input_gen) - so when the frame it has just made is the one
// run ahead for it (registers, T, picture), only the frames past the last one kept are
// run. The machine itself still runs every frame: input lands where it always did.
#define RA_KEEP		(2 + AF_AHEAD)		// run-ahead's most and the look ahead
static struct raKeptT {
	Computer* owner;
	unsigned int gen;
	int k, w, h, cx, cy, cw, ch;	// how many, of a raster this size and this crop
	int first, count;		// the frames kept, by the comp->frmCount each one stands for
	std::vector<unsigned char> pic[RA_KEEP];	// whole rasters, as bufimg holds one
	unsigned long long print[RA_KEEP];
	xState* state;			// the machine at the end of the last one
	Keyboard kbd;
} raKept;
static std::vector<unsigned char> raShown;	// the frame made, while the ones ahead draw over it

static unsigned long long ra_mix(unsigned long long h, const unsigned char* ptr, size_t len) {
	while (len >= 8) {
		unsigned long long v;
		memcpy(&v, ptr, 8);
		h = (h ^ v) * 0x100000001b3ULL;
		h ^= h >> 29;
		ptr += 8;
		len -= 8;
	}
	while (len-- > 0)
		h = (h ^ *ptr++) * 0x100000001b3ULL;
	return h;
}

// a frame as the machine stands at its end: the registers, T, the picture shown -
// the rest of the raster is not drawn on every machine, and keeps what the buffer held
static unsigned long long ra_print(Computer* comp) {
	Video* vid = comp->vid;
	unsigned long long h = 0xcbf29ce484222325ULL;
	h = ra_mix(h, (const unsigned char*)comp->cpu->regs, sizeof(comp->cpu->regs));
	h = ra_mix(h, (const unsigned char*)comp->cpu->flags, sizeof(comp->cpu->flags));
	h = ra_mix(h, (const unsigned char*)&comp->tickTotal, sizeof(comp->tickTotal));
	for (int y = 0; y < vid->vsze.y; y++)
		h = ra_mix(h, bufimg + (vid->lcut.y + y) * bytesPerLine + vid->lcut.x * 8, vid->vsze.x * 8);
	return h;
}

static void ra_raster_get(std::vector<unsigned char>& to, Video* vid) {
	size_t row = vid->full.x * 8;
	to.resize(row * vid->full.y);
	for (int y = 0; y < vid->full.y; y++)
		memcpy(to.data() + y * row, bufimg + y * bytesPerLine, row);
}

static void ra_raster_put(const std::vector<unsigned char>& from, Video* vid) {
	size_t row = vid->full.x * 8;
	for (int y = 0; y < vid->full.y; y++)
		memcpy(bufimg + y * bytesPerLine, from.data() + y * row, row);
}

static void ra_kept_drop() {
	raKept.owner = NULL;
	raKept.count = 0;
}

// Frames n + 1 .. n + k of the machine that has just made frame n, kept or run now,
// the machine left as it was. 0 when they cannot be run; 2 when bufimg holds frame
// n + shown already - the last one run when nothing is looked at past it - else 1,
// bufimg as it was.
static int ra_kept_frames(Computer* comp, int k, int shown, int finish) {
	Video* vid = comp->vid;
	int n = comp->frmCount;
	int last = (shown == k);
	unsigned int gen = __atomic_load_n(&x_input_gen, __ATOMIC_RELAXED);
	bool keep = (k > 1) && (raKept.owner == comp) && (raKept.gen == gen) && (raKept.k == k)
		&& (raKept.w == vid->full.x) && (raKept.h == vid->full.y) && (raKept.cx == vid->lcut.x)
		&& (raKept.cy == vid->lcut.y) && (raKept.cw == vid->vsze.x) && (raKept.ch == vid->vsze.y)
		&& (raKept.count > 0) && (raKept.first == n) && (raKept.print[n % RA_KEEP] == ra_print(comp));
	if (keep) {
		raKept.first++;
		raKept.count--;
	} else {
		raKept.first = n + 1;
		raKept.count = 0;
	}
	if (raKept.count < k) {
		if (!ra_snapshot(comp, finish)) {
			ra_kept_drop();
			return 0;
		}
		if (!last) ra_raster_get(raShown, vid);
		if (raKept.count > 0) {
			if (xstate_load(raKept.state, comp)) {
				*comp->keyb = raKept.kbd;
			} else {		// not the machine it was taken from: run them all from this one
				raKept.first = n + 1;
				raKept.count = 0;
			}
		}
		while (raKept.count < k) {
			if (!ra_run_frame(comp)) {
				ra_wind_back(comp);
				if (!last) ra_raster_put(raShown, vid);
				ra_kept_drop();
				return 0;
			}
			int f = (raKept.first + raKept.count) % RA_KEEP;
			if (k > 1) {		// one frame ahead is run again at the next frame end anyway
				ra_raster_get(raKept.pic[f], vid);
				raKept.print[f] = ra_print(comp);
			}
			raKept.count++;
		}
		// where the next frame end goes on from, when it keeps any of these: with one
		// frame ahead it runs from the machine as it stands then
		if (k > 1) {
			if (!raKept.state) raKept.state = xstate_create();
			if (!raKept.state || !xstate_save(raKept.state, comp)) {
				ra_wind_back(comp);
				if (!last) ra_raster_put(raShown, vid);
				ra_kept_drop();
				return 0;
			}
			raKept.kbd = *comp->keyb;
		}
		ra_wind_back(comp);
		if (!last) ra_raster_put(raShown, vid);
	}
	raKept.owner = comp;
	raKept.gen = gen;
	raKept.k = k;
	raKept.w = vid->full.x;
	raKept.h = vid->full.y;
	raKept.cx = vid->lcut.x;
	raKept.cy = vid->lcut.y;
	raKept.cw = vid->vsze.x;
	raKept.ch = vid->vsze.y;
	return last ? 2 : 1;
}

// the raster of frame n + j (j from 1)
static const std::vector<unsigned char>& ra_kept_pic(Computer* comp, int j) {
	return raKept.pic[(comp->frmCount + j) % RA_KEEP];
}

// The adaptive antiflicker's look ahead: the frames after the one shown, as crops
static std::vector<uint32_t> afFrames;

static uint32_t* af_crop(Video* vid) {
	return (uint32_t*)(bufimg + vid->lcut.y * bytesPerLine + vid->lcut.x * 8);
}

static const uint32_t* af_look_ahead(Computer* comp, int from, int l) {
	if (l < AF_AHEAD) return NULL;
	Video* vid = comp->vid;
	int wid = vid->vsze.x * 2;
	int hei = vid->vsze.y;
	size_t row = vid->full.x * 8;
	afFrames.resize(AF_AHEAD * wid * hei);
	for (int i = 0; i < AF_AHEAD; i++) {
		const unsigned char* src = ra_kept_pic(comp, from + i).data() + vid->lcut.y * row + vid->lcut.x * 8;
		uint32_t* dst = afFrames.data() + i * wid * hei;
		for (int y = 0; y < hei; y++)
			memcpy(dst + y * wid, src + y * row, wid * 4);
	}
	return afFrames.data();
}

// The rom's tape entry points the hooks act on, with the rom in: LD-BYTES from its
// start and from its edge loop, and SA-BYTES. Fast mode's run on stops on them.
enum {TAPE_HOOK_LOAD, TAPE_HOOK_EDGE, TAPE_HOOK_SAVE, TAPE_HOOKS};
static const int tapeHookPc[TAPE_HOOKS] = {LD_ROM_BASE + LDC_START, LD_ROM_BASE + LDC_EDGE1, 0x4d0};

static int tape_hook_at(Computer* comp) {
	if (!zx_rom_active(comp)) return -1;
	int pc = comp->cpu->regPC;
	for (int i = 0; i < TAPE_HOOKS; i++)
		if (pc == tapeHookPc[i]) return i;
	return -1;
}

void xThread::emuCycle(Computer* comp) {
	int tm;
	int brkskip = 0;
	// the mode only changes at a frame end on this thread, so it is asked there
	// and not on every opcode
	bool rewinding = rewind_active();
	// sndNsFixed is deliberately not cleared here: it holds the part of a sample
	// not yet made, and a cycle can end anywhere. Clearing it dropped that
	// remainder on every wake-up, and wake-ups come every 2ms.
	conf.snd.fill = 1;
	xreplay_hold = conf.emu.fast;	// no redraw snapshots of a machine run fast
	while (!comp->flgBRK && conf.snd.fill && !finish && !conf.emu.pause) {
		// the rewind history playing back stands in for the machine
		if (rewinding) {
			int rw = rewind_play(comp, &sndNsFixed);
			if (rw) {
				if (rw > 1) {
					conf.vid.fctime = paceClockNs();
					conf.vid.fcount++;
					vrec_frame(comp);
					if (frame_hand_over()) emit s_frame();
				}
				if (conf.snd.need <= 0) conf.snd.fill = 0;
				continue;
			}
			rewinding = false;
		}
		// exec 1 opcode (or handle INT, NMI)
		int hooks = 1;		// the tape hooks below are still to look at the last step
		// read once for the step: the gui can switch fast mode off while the run
		// on below is still going to its frame end, and that frame's time must be
		// thrown away as fast mode's, not made into sound all at once
		bool fast = conf.emu.fast;
		if (conf.emu.pause) {
			sndNsFixed += NS_TO_FIXED(1000);
		} else {
			if (brkskip) {
				brkskip = comp->flgDBG;
				comp->flgDBG = 1;		// block breakpoints checking
				tm = compExec(comp);
				comp->flgDBG = brkskip;
				brkskip = 0;
			} else if (fast && !comp->gs->enable && !brk_cond_n && comp_unwatched(comp)
					&& !vrecWatch.load(std::memory_order_relaxed)) {
				// Fast mode with nothing to look at between the steps: run them on
				// until one ends a frame, breaks, or leaves the cpu where a tape hook
				// below has something to do; the hooks then see that last one, as
				// they would have. Fast loading's edge loop is looked at here, after
				// every step, as below. A HALT is run on to the next event at once,
				// but not under fast loading, which looks at every step of it.
				// Between those, the steps run on in one go (comp_run_quiet) up to
				// the rom's tape entry points the hooks below look for.
				Tape* tap = comp->tape;
				for (;;) {
					if (fastload_on) {
						tm = comp_exec_quiet(comp);
					} else if (comp->cpu->flgHALT) {
						if (!comp_halt_skip(comp, &tm))
							tm = comp_exec_quiet(comp);
					} else if (!comp_run_quiet(comp, &tm, tapeHookPc, TAPE_HOOKS)) {
						tm = comp_exec_quiet(comp);
					}
					if (comp->flgFRM || comp->flgBRK || conf.emu.pause) break;
					if (tape_hook_at(comp) >= 0) break;
					if ((tap->ldBase >= 0) || (tape_flash() && (tap->portReads != ldcReads))) break;
					sndNsFixed += NS_TO_FIXED(tm);
					if (fastload_on) {
						sndNsFixed += NS_TO_FIXED(fastload_step(comp));
						if (comp->flgFRM || comp->flgBRK) {
							tm = 0;
							hooks = 0;		// all of them done for this step
							break;
						}
					}
				}
			} else if (comp->cpu->flgHALT
					&& (nsPerSampleFixed - sndNsFixed >= 4 * comp->nsPerTickFixed)
					&& comp->hw->quiet && comp->hw->quiet->halt && !conf.emu.fast && !brk_cond_n
					&& !fastload_on && !vrecWatch.load(std::memory_order_relaxed) && (tape_hook_at(comp) < 0)
					&& !(tape_flash() && (comp->tape->ldBase >= 0))
					&& comp_halt_skip_rt(comp, &tm, nsPerSampleFixed - sndNsFixed)) {
				// a HALT run on to the step the next sample follows, taken below as ever
			} else {
				tm = compExec(comp);			// TODO: it exits when fetch-brk is occured, pc doesn't changed
			}
			sndNsFixed += NS_TO_FIXED(tm);
			int watch = vrecWatch.load(std::memory_order_relaxed);
			if (watch) vrec_auto_pc(comp, watch);
			// None of the tape tricks while a recording plays or is made: the
			// machine takes its input from the recording alone, and a recording
			// holds only what came in through a port.
			if (hooks && !comp->rzx.play && !comp->rzx.rec.on) {
				// tape trap	TODO: rework it as a system breakpoint
				// this runs on every instruction, and the rom is paged in for most
				// of them: the pc straight from the Z80, not through the cpu's
				// register table.
				int hook = tape_hook_at(comp);
				if ((hook == TAPE_HOOK_LOAD) || (hook == TAPE_HOOK_EDGE)) {	// load: ix:addr, de:len
					tap_catch_load(comp, hook == TAPE_HOOK_LOAD);
				} else if (hook == TAPE_HOOK_SAVE) {		// save: ix:addr, de:len, a:block type(b7), hl:pilot len (1f80/0c98)?
					tap_catch_save(comp);
				}
				// a copy of LD-BYTES in ram is trapped as the rom's is, once seen
				if (tape_flash()) {
					int start;
					if (ldc_step(comp, &start))
						tap_catch_load(comp, start, comp->tape->ldBase, comp->tape->ldDir);
				} else if (comp->tape->ldBase >= 0) {
					ldc_forget(comp);
				}
				// a loader's edge loop, counted instead of run
				if (fastload_on)
					sndNsFixed += NS_TO_FIXED(fastload_step(comp));
			}
		}
		// sound buffer update. In fast mode there is nothing to mix - the
		// only thing sndSync() still does there is run the GS, so a machine
		// without one skips the call and keeps the count. What is queued to play
		// is then from before the time that went by unheard.
		if (fast && !comp->gs->enable) {
			while (sndNsFixed > nsPerSampleFixed)
				sndNsFixed -= nsPerSampleFixed;
			snd_drop();
		} else {
			while (sndNsFixed > nsPerSampleFixed) {
				comp->sndAgo = sndNsFixed - nsPerSampleFixed;
				sndSync(comp);
				sndNsFixed -= nsPerSampleFixed;
			}
			comp->sndAgo = 0;
		}
		if (comp->flgFRM) {
			comp->flgFRM = 0;
			if (conf.emu.fast) conf.snd.fill = 0;	// see sndSync()
			if ((benchStop >= 0) && (conf.vid.fcount + 1 >= benchStop))
				conf.snd.fill = 0;		// the bench stops on a frame, not on a sample
			conf.vid.fctime = paceClockNs();	// for the fps readout
			conf.vid.fcount++;
			comp->frmCount++;
			int mdx, mdy;
			pad_mouse_frame(comp->vid->nsPerFrame, &mdx, &mdy);
			if (mdx || mdy) x_input_bump();
			comp->mouse->xpos += mdx;
			comp->mouse->ypos += mdy;
			ldc_frame();
			autostart_frame(comp);
			fastload_frame(comp);
			rzx_seek_frame(comp);
			rzxr_frame(comp);
			comp_media_tick(comp);
			rewind_frame(comp, &sndNsFixed);
			rewinding = rewind_active();
			fast_picture_frame(comp);
			brk_trace_flush();
			serial_frame(comp);
			// before run-ahead: the debugger's screen view wants the machine as
			// it really stands, not the frame it is about to guess at
			vid_scr_snap(comp->vid);
			// the frame just made is not the one to show: run on to the one
			// the player's last key press is already in
			int shown;
			int k = ra_frames(comp, &shown);
			int ahead_ok = 0;
			if (k > 0) {
				ahead_ok = ra_kept_frames(comp, k, shown, finish);
				if ((ahead_ok == 1) && shown) ra_raster_put(ra_kept_pic(comp, shown), comp->vid);
			} else {
				ra_kept_drop();
			}
// process noflic/scanlines (if !fast ???)
// buffers is already switches, bufimg - just painted (greyscale, if flag is set), scrimg - new
			// fast forward's undrawn frames are not mixed, and its drawn ones follow on
			int ffwd = fast_ffwd_frame();
			if (!conf.emu.fast && (noflic > 0) && (ffwd >= 0)) {
				const uint32_t* ahead = ahead_ok ? af_look_ahead(comp, shown + 1, k - shown) : NULL;
				scrMix(pscr, (unsigned char*)af_crop(comp->vid),
					comp->vid->vsze.x * 2, comp->vid->vsze.y, bytesPerLine,
					noflic / 100.0, noflicGamma, noflicMode, !ffwd && (conf.vid.fcount != mixFrame + 1), ahead);
				mixFrame = conf.vid.fcount;
			}

			vrec_frame(comp);
			if (frame_hand_over()) emit s_frame();
			// a machine slower than real time never pays its sound debt off: let
			// whoever waits for the lock in at a frame end, or the window hangs
			if (emuWaiters.load(std::memory_order_relaxed)) conf.snd.fill = 0;
		}
#if LOG_OUTPUT
// ...
#endif
		if (comp->flgBRK) {
			// printf("brkt = %i, brka = %X\n", comp->brkt, comp->brka);
			if (comp->brkt == -1) {			// irq or tmp
				conf.emu.pause |= PR_DEBUG;
				emit dbgRequest();
			} else if (comp->brkt == -2) {
				emit s_close();
			} else {				// others
				xBrkPoint* ptr = brk_find(comp->brkt, comp->brka);
				if (!ptr) {			// breakpoint didn't found, but bit is set (?)
					comp->flgBRK = 0;
				} else {
					if (!brk_hit(ptr, comp)) {	// condition is false: go on
						comp->flgBRK = 0;
						if (comp->flgBRKPRE) brkskip = 1;
					} else {
						brkAction(comp, ptr, &brkskip);
					}
				}
			}
		} else if (brk_cond_n && brk_check_cond(comp)) {	// conditions not bound to an address
			int stop = 0;
			comp->brkt = BRK_COND;
			comp->brka = 0;
			// each one gets its own action, and one asking to stop is enough
			for (auto it = conf.brk.list.begin(); it != conf.brk.list.end(); it++) {
				if (!it->fired) continue;
				it->count++;
				comp->flgBRK = 1;
				brkAction(comp, &(*it), &brkskip);
				if (comp->flgBRK) stop = 1;
			}
			comp->flgBRK = stop;
		}
		if (comp->flgCOND)
			comp_brk_newstep(comp);
	}
	// whoever looks next - the debugger, a snapshot, the gui - sees the ray where
	// it stands, not the dots an undrawn frame has only counted
	vid_unlazy(comp->vid);
	comp->flgBRK = 0;
	comp->flgNMIRQ = 0;
}

// a recording opened since the last cycle starts playing here
void xThread::rzx_begin(Computer* comp) {
	if (comp->rzx.start) {
		comp->rzx.start = 0;
		comp->rzx.play = 1;
		comp->rzx.fCount = 0;
		comp->rzx.fCurrent = 0;
		rewind(comp->rzx.file);
		rzxGetFrame(comp);
		comp->rzx.noint = 0;		// it is for a join; the start raises no INT anyway
	}
}

void xThread::run() {
	Computer* comp;
	conf.snd.need = 0;		// reset sound buffer
	do {
#if !USEMUTEX
		sleepy = 1;
#endif
		emu_lock();
		comp = conf.zx;
		if (comp) {
			rzx_begin(comp);
			if (!conf.emu.pause) {
				emuCycle(comp);
			}
		}
		emu_unlock();
		while (emuWaiters.load() && !finish)
			QThread::yieldCurrentThread();
		// paused, fast mode is still set by whoever holds it: the next frame
		// sets it again, so it cannot stand for "run on" here
		bool idle = !conf.emu.fast || conf.emu.pause;
#if USEMUTEX
		if (idle && !finish) {
			emutex.lock();
			qwc.wait(&emutex);
			emutex.unlock();
		}
#else
		while (idle && sleepy && !finish)
			usleep(10);
#endif
	} while (!finish);
	xstate_destroy(raState);
	raState = NULL;
	exit(0);
}

// Headless benchmark (--bench): runs the machine on this thread with no window,
// the way run() does, and reports emulated frames per second of host time.
// full = 0 is fast mode (no sound mixing), full = 1 mixes sound as real time
// play does, without the pacer's waits. prof names a file for a flat profile
// of this thread, sampled from another one.

#ifdef _WIN32
#include <windows.h>
#include <map>

typedef struct {
	HANDLE target;
	volatile int stop;
	int hz;
	std::map<unsigned long long, int> hits;
	int total;
} benchProf;

static DWORD WINAPI bench_prof_thread(LPVOID p) {
	benchProf* bp = (benchProf*)p;
	LARGE_INTEGER frq, now, next;
	QueryPerformanceFrequency(&frq);
	long long step = frq.QuadPart / bp->hz;
	QueryPerformanceCounter(&next);
	while (!bp->stop) {
		next.QuadPart += step;
		do {
			YieldProcessor();
			QueryPerformanceCounter(&now);
		} while (now.QuadPart < next.QuadPart);
		if (SuspendThread(bp->target) == (DWORD)-1) break;
		CONTEXT ctx;
		ctx.ContextFlags = CONTEXT_CONTROL;
		if (GetThreadContext(bp->target, &ctx)) {
#ifdef _WIN64
			bp->hits[ctx.Rip]++;
#else
			bp->hits[ctx.Eip]++;
#endif
			bp->total++;
		}
		ResumeThread(bp->target);
	}
	return 0;
}
#endif

// 64-bit mix over a block: only there to tell two runs apart
static unsigned long long bench_mix(unsigned long long h, const unsigned char* ptr, int len) {
	while (len >= 8) {
		unsigned long long v;
		memcpy(&v, ptr, 8);
		h = (h ^ v) * 0x100000001b3ULL;
		h ^= h >> 29;
		ptr += 8;
		len -= 8;
	}
	while (len-- > 0)
		h = (h ^ *ptr++) * 0x100000001b3ULL;
	return h;
}

// --bench-keys "F:keys[/N],...": pressed at frame F counted from the start of
// the bench, held N frames (4). keys are matrix names as autostart types them,
// '_' for space; "joy:" or "joyb:" and joystick rows as a .pad names them (up,
// fire2...) joined by '+' are the two joysticks, "nmi" is the NMI button, and
// "pc:" and a keymap name (UP, F1) is a PC key, scancodes and all.
enum {BK_KEYS = 0, BK_JOY, BK_JOYB, BK_NMI, BK_PC, BK_POWER};

typedef struct benchKey {
	int at = 0;
	int hold = 4;
	int kind = BK_KEYS;
	int mask = 0;		// XJ_* for a joystick
	int id = 0;		// the key id of a PC key
	int state = 0;		// 0: waiting, 1: down, 2: done
	std::string keys;
} benchKey;

static std::vector<benchKey> bkList;

static void bench_keys_parse(const char* spec) {
	bkList.clear();
	std::string s(spec ? spec : "");
	size_t pos = 0;
	while (pos < s.size()) {
		size_t end = s.find(',', pos);
		if (end == std::string::npos) end = s.size();
		std::string tok = s.substr(pos, end - pos);
		pos = end + 1;
		size_t col = tok.find(':');
		if (col == std::string::npos) continue;
		benchKey k;
		k.at = atoi(tok.c_str());
		std::string key = tok.substr(col + 1);
		size_t sl = key.rfind('/');
		if ((sl != std::string::npos) && (sl > 0)) {
			k.hold = atoi(key.c_str() + sl + 1);
			key = key.substr(0, sl);
		}
		if (key == "nmi") {
			k.kind = BK_NMI;
		} else if (key == "power") {
			k.kind = BK_POWER;
		} else if (!key.compare(0, 3, "pc:")) {
			k.kind = BK_PC;
			k.id = getKeyIdByName(key.substr(3).c_str());
		} else if (!key.compare(0, 4, "joy:") || !key.compare(0, 5, "joyb:")) {
			k.kind = (key[3] == 'b') ? BK_JOYB : BK_JOY;
			std::string rows = key.substr(key.find(':') + 1) + "+";
			size_t p = 0, q;
			while ((q = rows.find('+', p)) != std::string::npos) {
				k.mask |= pad_role_bit(rows.substr(p, q - p).c_str());
				p = q + 1;
			}
		} else {
			for (size_t i = 0; i < key.size(); i++)
				if (key[i] == '_') key[i] = ' ';
			k.keys = key;
		}
		bkList.push_back(k);
	}
}

static void bench_key_set(Computer* comp, benchKey& k, int press) {
	switch (k.kind) {
		case BK_JOY:
		case BK_JOYB: {
			Joystick* joy = (k.kind == BK_JOYB) ? comp->joyb : comp->joy;
			if (press) joyPress(joy, k.mask);
			else joyRelease(joy, k.mask);
			break;
		}
		case BK_NMI:
			if (press) comp_irq(IRQ_NMI, comp);
			break;
		case BK_POWER:
			if (press) xm_power_cycle();	// the Hard Reset: this thread is the emulation's
			break;
		case BK_PC: {
			keyEntry ent = getKeyEntry(k.id);
			x_key(comp, &ent, press);
			break;
		}
		default:
			autostart_key(comp, k.keys.c_str(), press);
			break;
	}
}

static void bench_keys_step(Computer* comp, int frame) {
	for (benchKey& k : bkList) {
		if ((k.state == 0) && (frame >= k.at)) {
			bench_key_set(comp, k, 1);
			k.state = 1;
		}
		if ((k.state == 1) && (frame >= k.at + k.hold)) {
			bench_key_set(comp, k, 0);
			k.state = 2;
		}
	}
}

// Runs the timed part: fast mode as run() does it, or full sound mixing with
// a budget of 256 samples per cycle, the way the pacer hands them out.
// hash folds every finished frame and every sample into one number, so two
// builds can be shown to run the machine identically.
int xThread::bench(int frames, int skip, int full, int hash, const char* prof, const char* shot, int nodraw, int heat, int rw, int steps, int split) {
	Computer* comp = conf.zx;
	if (!comp) return 0;
	blockSignals(true);
	setOutput("NULL");
	pacingClose();		// the budget is handed out here, not by the timer
	conf.emu.pause = 0;
	// the bench picks its mode itself: fast loading would switch it under a tape,
	// and it draws the frames fast mode would leave out
	fastload_hold(1);
	fast_picture_hold(1);
	int rwOn = conf.emu.rewind.on;
	conf.emu.rewind.on = (rw > 0);
	rzx_begin(comp);
	// Recording: N frames into a file, which --bench-rzx N then plays back to
	// the same state; a rollback on the way, as a rewind makes, if asked for
	if (benchRec) {
		szx_set_machine(conf.macId.c_str());
		int err;
		if (comp->rzx.play && (benchRecTake > 0)) {
			// a recording played to frame F and a bit, then taken over there
			while (comp->rzx.play && (comp->rzx.fCurrent < benchRecTake))
				compExec(comp);
			for (int i = 0; (i < 1000) && comp->rzx.play; i++)
				compExec(comp);
			err = rzx_rec_take_over(comp);
			fprintf(stdout, "rzx rec: taken over at frame %i, %i fetches in\n", rzx_rec_frames(comp), comp->rzx.rec.fetch);
			rzxStop(comp);
		} else {
			err = rzx_rec_start(comp);
		}
		xState* back = NULL;
		int poke = benchRecJoin + 500;
		while (!err && (rzx_rec_frames(comp) < benchRecFrames)) {
			compExec(comp);
			if ((benchRecMark > 0) && (rzx_rec_frames(comp) >= benchRecMark)) {
				fprintf(stdout, "rzx rec: bookmark at frame %i\n", rzx_rec_frames(comp));
				rzx_rec_bookmark();
				benchRecMark = 0;
			} else if ((benchRecRoll > 0) && (rzx_rec_frames(comp) >= benchRecRoll)) {
				fprintf(stdout, "rzx rec: roll back from frame %i", rzx_rec_frames(comp));
				int n = rzx_rec_rollback(comp);
				fprintf(stdout, " by %i to %i\n", n, rzx_rec_frames(comp));
				benchRecRoll = 0;
			}
			if ((benchRecNmi > 0) && (rzx_rec_frames(comp) >= benchRecNmi) && (comp->cpu->regPC > 0x3fff)) {
				fprintf(stdout, "rzx rec: nmi at frame %i\n", rzx_rec_frames(comp));
				comp_irq(IRQ_NMI, comp);
				benchRecNmi = -(rzx_rec_frames(comp) + 1);
			} else if ((benchRecNmi < 0) && (rzx_rec_frames(comp) >= -benchRecNmi)) {
				comp->flgNMIRQ = 0;		// let go after a frame, as emuCycle does
				benchRecNmi = 0;
			}
			if ((benchRecJoin > 0) && (rzx_rec_frames(comp) >= benchRecJoin)) {
				fprintf(stdout, "rzx rec: reset at frame %i\n", rzx_rec_frames(comp));
				compReset(comp, RES_DEFAULT);
				benchRecJoin = 0;
			} else if ((poke > 500) && !benchRecJoin && (rzx_rec_frames(comp) >= poke) && comp->cpu->flgIFF1) {
				fprintf(stdout, "rzx rec: poke at frame %i, iff1 %i\n", rzx_rec_frames(comp), comp->cpu->flgIFF1 ? 1 : 0);
				memWr(comp->mem, 0x5800, 0x47);
				rzx_rec_touch();
				poke = 0;
			}
			if (benchRecBack <= 0) continue;
			if (!back && (rzx_rec_frames(comp) >= benchRecBack / 2)) {
				back = xstate_create();
				xstate_save(back, comp);
			} else if (back && (rzx_rec_frames(comp) >= benchRecBack)) {
				fprintf(stdout, "rzx rec: back from frame %i", rzx_rec_frames(comp));
				xstate_load(back, comp);
				fprintf(stdout, " to %i\n", rzx_rec_frames(comp));
				benchRecBack = 0;
			}
		}
		if (back) xstate_destroy(back);
		rzxRecImage* img = err ? NULL : rzx_rec_take(comp);
		rzx_rec_stop(comp);
		if (img) err = rzx_rec_image_write(img, benchRec, XPRODUCT, 0, 0, "bench", 0);
		fprintf(stdout, "rzx rec: %i frames, %i joins, %s\n", rzx_rec_image_frames(img), rzx_rec_joins(comp), err ? "failed" : "written");
		rzx_rec_image_free(img);
		if (!err && benchRecFinal) {
			// read back and finalized beside it
			std::string fin = std::string(benchRec) + ".fin.rzx";
			img = rzx_rec_image_read(benchRec, &err);
			if (img) err = rzx_rec_image_write(img, fin.c_str(), NULL, 0, 0, NULL, 1);
			fprintf(stdout, "rzx rec: finalized %i frames, %s\n", rzx_rec_image_frames(img), err ? "failed" : "written");
			rzx_rec_image_free(img);
		}
		fflush(stdout);
		conf.emu.rewind.on = rwOn;
		fastload_hold(0);
		fast_picture_hold(0);
		return 0;
	}
	// A recording's frame N, to compare with another player stopped there: the
	// frames it counts, not the video's, and past the INT that ends it if the
	// cpu takes one
	if (benchRzx > 0) {
		for (int pass = (benchRzxSeek >= 0) ? 2 : 1; pass > 0; pass--) {
			while (comp->rzx.play && (comp->rzx.fCurrent < benchRzx))
				compExec(comp);
			if (pass > 1)
				fprintf(stdout, "rzx: seek to %i stands at %i\n", benchRzxSeek, rzx_seek(comp, benchRzxSeek));
		}
		CPU* cpu = comp->cpu;
		if ((cpu->intrq & Z80_INT) && cpu->flgACK && cpu->flgIFF1 && !cpu->flgNOINT)
			compExec(comp);
		fprintf(stdout, "rzx: frame %i, %s\n", comp->rzx.fCurrent, comp->rzx.play ? "playing" : "stopped");
		fflush(stdout);
		conf.emu.rewind.on = rwOn;
		fastload_hold(0);
		fast_picture_hold(0);
		return 0;
	}
	// warm up: a tape or disk being started, a demo getting to its part. In
	// the mode that is measured: where fast mode hands the machine back is not
	// where a cycle with sound does, so a switch between them would leave the
	// two builds being compared on different instructions
	conf.emu.fast = full ? 0 : 1;
	int f0 = conf.vid.fcount;
	int fk = f0;			// --bench-keys counts from here
	bench_keys_parse(benchKeys);
	benchStop = f0 + skip;		// and on a frame, so both builds start measuring on the same one
	while ((conf.vid.fcount - f0 < skip) && !conf.emu.pause) {
		conf.snd.need = full ? 256 : 0;
		emu_lock();
		emuCycle(comp);
		bench_keys_step(comp, conf.vid.fcount - fk);
		emu_unlock();
	}
	// and on to an instruction anywhere in the frame, where a snapshot is
	// harder to take than at a frame boundary. A frame ended on the way is not
	// counted, so the frames below are counted from here, as after a load.
	while ((steps-- > 0) && !conf.emu.pause) {
		emu_lock();
		snd_scope_step(comp, compExec(comp));
		comp->flgFRM = 0;
		emu_unlock();
	}
	conf.emu.fast = full ? 0 : 1;
	if (nodraw) vid_set_nodraw(comp->vid, 1);	// what the picture itself costs
	if (heat) {
		comp->flgHEAT = 1;
		comp_heat_sync(comp);
		comp_heat_reset(comp);
	}
#ifdef _WIN32
	benchProf* bp = NULL;
	HANDLE pth = NULL;
	if (prof) {
		bp = new benchProf;
		bp->stop = 0;
		bp->hz = 4000;
		bp->total = 0;
		DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &bp->target,
			THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
		pth = CreateThread(NULL, 0, bench_prof_thread, bp, 0, NULL);
	}
#endif
	unsigned long long hFrm = 0xcbf29ce484222325ULL;
	unsigned long long hSnd = 0xcbf29ce484222325ULL;
	// a run cut here by a snapshot and one that goes on have to hash alike: the
	// mixer starts over and the frame drawn across the cut is left out
	if (split) {
		sndNsFixed = 0;
		snd_pipe_reset();
	}
	int spos = snd_ring_fill_pos();
	long long t0 = paceClockNs();
	int tk0 = comp->tickCount;
	f0 = conf.vid.fcount;
	int fl = f0;
	// --bench-draw-every N: frame j of the run (0 is the one already started) is
	// drawn when j % N is 0, switched at the frame start as fast mode switches it.
	// --bench-hash-every N alone draws them all and hashes the same ones, so the
	// two runs compare frame for frame.
	int every = (benchHashEvery > 1) ? benchHashEvery : 1;
	int fd = f0;
	if (benchDrawEvery > 1)
		vid_set_nodraw_next(comp->vid, 1 % every != 0);
	benchStop = f0 + frames;	// the last cycle ends on the frame, whatever the budget
	while ((conf.vid.fcount - f0 < frames) && !conf.emu.pause) {
		conf.snd.need = full ? 256 : 0;
		emu_lock();
		emuCycle(comp);
		bench_keys_step(comp, conf.vid.fcount - fk);
		if ((benchDrawEvery > 1) && (conf.vid.fcount != fd)) {
			fd = conf.vid.fcount;
			vid_set_nodraw_next(comp->vid, (fd - f0 + 1) % every != 0);
		}
		emu_unlock();
		if (hash) {
			if (conf.vid.fcount != fl) {
				int first = (fl == f0);
				int done = fl - f0;		// the frame just finished
				fl = conf.vid.fcount;
				if ((split && first) || (done % every)) continue;
				Video* vid = comp->vid;
				for (int y = 0; y < vid->full.y; y++)
					hFrm = bench_mix(hFrm, bufimg + y * bytesPerLine, vid->full.x * 8);
			}
			int epos = snd_ring_fill_pos();
			while (spos != epos) {
				unsigned char b = snd_ring_byte(spos++);
				hSnd = bench_mix(hSnd, &b, 1);
			}
		}
	}
	long long t1 = paceClockNs();
#ifdef _WIN32
	if (bp) {
		bp->stop = 1;
		WaitForSingleObject(pth, INFINITE);
		CloseHandle(pth);
		CloseHandle(bp->target);
		FILE* file = fopen(prof, "wb");
		if (file) {
			fprintf(file, "base %llx\n", (unsigned long long)(size_t)GetModuleHandle(NULL));
			fprintf(file, "total %i\n", bp->total);
			for (auto it = bp->hits.begin(); it != bp->hits.end(); it++)
				fprintf(file, "%llx %i\n", it->first, it->second);
			fclose(file);
		}
		delete bp;
	}
#endif
	if (conf.emu.pause & PR_DEBUG)
		xlog(XLG_BRK, XLL_INFO, "bench stopped by a breakpoint, PC=%04X", comp->cpu->regPC & 0xffff);
	int done = conf.vid.fcount - f0;
	double sec = (t1 - t0) / 1e9;
	double fps = (sec > 0) ? done / sec : 0;
	double rt = comp_fps(comp);
	int ticks = comp->tickCount - tk0;
	fprintf(stdout, "bench: machine %s, %s, %i frames in %.3f s: %.1f fps, x%.2f real time, %.2f ns/T%s\n",
		conf.macId.c_str(), full ? "full" : "fast", done, sec, fps, fps / rt,
		(t1 - t0) / (double)(ticks ? ticks : 1), conf.emu.pause ? " (stopped early)" : "");
	if (hash) {
		CPU* cpu = comp->cpu;
		unsigned long long hMem = bench_mix(0xcbf29ce484222325ULL, comp->mem->ramData, comp->mem->ramMask + 1);
		int regs[] = {cpu->regPC, cpu->regSP, cpu->regAF, cpu->regBC, cpu->regDE, cpu->regHL,
			cpu->regIX, cpu->regIY, cpu->regI, cpu->regR, comp->tickCount, comp->frmtCount};
		unsigned long long hCpu = bench_mix(0xcbf29ce484222325ULL, (unsigned char*)regs, sizeof(regs));
		fprintf(stdout, "hash: frames %016llx sound %016llx ram %016llx cpu %016llx pc %04X T %i\n",
			hFrm, hSnd, hMem, hCpu, cpu->regPC & 0xffff, comp->frmtCount);
		if (heat) {
			unsigned long long hHeat = 0xcbf29ce484222325ULL;
			xHeatBank* banks[] = {&comp->heatRam, &comp->heatRom};
			for (xHeatBank* bk : banks) {
				unsigned int* cnt[] = {bk->rd, bk->wr, bk->ex};
				for (unsigned int* c : cnt)
					if (bk->size) hHeat = bench_mix(hHeat, (unsigned char*)c, bk->size * sizeof(unsigned int));
			}
			fprintf(stdout, "heat: %016llx\n", hHeat);
		}
	}
	// the last finished frame, whole raster, as a binary ppm
	if (shot) {
		FILE* file = fopen(shot, "wb");
		if (file) {
			Video* vid = comp->vid;
			fprintf(file, "P6\n%i %i\n255\n", vid->full.x * 2, vid->full.y);
			for (int y = 0; y < vid->full.y; y++) {
				unsigned char* ptr = bufimg + y * bytesPerLine;
				for (int x = 0; x < vid->full.x * 2; x++) {
					fputc(ptr[0], file);
					fputc(ptr[1], file);
					fputc(ptr[2], file);
					ptr += 4;
				}
			}
			fclose(file);
		}
	}
	if (rw > 1)
		rewindCheck(comp, full);
	else if (rw > 0)
		fprintf(stdout, "rewind: %i snapshots, %u KB\n", rewind_count(), (unsigned)(rewind_bytes() >> 10));
	conf.emu.rewind.on = rwOn;
	fflush(stdout);
	fastload_hold(0);
	fast_picture_hold(0);
	return done;
}

// Every snapshot but the newest is put back and the machine run on to the frame
// of the next one, which it then has to be byte for byte. State that matters and
// is outside the snapshot sends the two apart.
void xThread::rewindCheck(Computer* comp, int full) {
	rewind_hold(1);
	int n = rewind_count();
	int done = 0, bad = 0, lost = 0;
	size_t bytes = rewind_bytes();
	for (int back = n - 1; back > 0; back--) {
		int to = rewind_frame_of(back - 1);
		if (!rewind_load(comp, back, &sndNsFixed)) {
			lost++;
			continue;
		}
		// a cycle can end on every sample (sound on, or a GS to run), so the
		// bound is in frames, not in cycles
		int last = to + 1000;
		while ((rewind_clock() != to) && (rewind_clock() < last) && !conf.emu.pause) {
			conf.snd.need = full ? 256 : 0;
			benchStop = conf.vid.fcount + 1;
			emu_lock();
			emuCycle(comp);
			emu_unlock();
		}
		done++;
		if (rewind_clock() != to) {
			xlog(XLG_CORE, XLL_WARN, "rewind check: frame %i reached %i instead of %i", rewind_frame_of(back), rewind_clock(), to);
			bad++;
		} else if (!rewind_matches(comp, back - 1)) {
			bad++;
		}
	}
	rewind_hold(0);
	fprintf(stdout, "rewind: %i snapshots, %u KB, %i checked, %i differ, %i not loaded\n",
		n, (unsigned)(bytes >> 10), done, bad, lost);
}

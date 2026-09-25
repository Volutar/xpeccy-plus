// Fast loading.
//
// While a loader is reading a playing tape, the machine runs as fast as the
// host allows - the same fast mode Insert gives, with no sound mixed. Nothing
// about the loader is touched: it runs opcode for opcode as it would at normal
// speed, so whatever loads at all loads this way too, turbo and direct
// recordings included. What makes it stop at the right moment is the tape: the
// automatics, a stop mark in the image or its end stop the deck, and the machine
// is let go a few frames later - wound back to the frame the loader left, so
// the game does not start at the host's speed.
//
// A loader is told from anything else by how often it reads the tape port. The
// rom's edge loop reads it every ~60 T, DeciLoad once a bit, over a hundred times
// a frame either way; a keyboard poll is eight reads a frame. It also runs
// while the tape is armed: flash loading has handed over the rom's blocks and
// the loader they started will ask for the rest - Speedlock after seven
// seconds of decrypting itself.
//
// The picture is held: the video draws nothing, and vid_frame() leaves the
// buffers alone while it does, so the last frame drawn stays on screen. A new
// one is drawn 30 times a second of host time, loader's stripes and all.
//
// On top of that, a loader's edge loop is skipped through. Most of a load is
// spent in a few instructions that read the port and count B until the level
// changes. The loops that are known by their code are learnt from two turns
// in a row that leave nothing but B and R moved; from then on, every time the
// loop reads a level it will go round on, the turns up to the tape's next
// pulse are counted instead of run: B, R, the flags and MEMPTR are set as the
// turns leave them, and the machine takes the time they would have taken,
// video and devices at one go - the same state, only sooner, which is what
// --bench-loops 3 checks. Where the ULA contends the bus a turn can take
// longer, so there that is only done in the border lines above and below the
// screen. Edge detection skips the time as well: the loader counts the same B,
// but the rest of the machine is not moved on, so a load takes a fraction of
// the emulated time - and is no longer what the machine would have done.

#include <string.h>
#include <algorithm>

#include "xcore.h"
#include "fastload.h"
#include "autostart.h"
#include "../libxpeccy/cpu/Z80/z80.h"
#include "../libxpeccy/hardware/hardware.h"
#include "../libxpeccy/xstate.h"

#define FL_READS	100	// tape port reads in a frame that make a loader
// Frames without them, with the tape playing, before the machine is let go: the
// rom waits a second before each block, a loader unpacks between parts. With the
// tape stopped nothing more comes in, and it is let go at once - a tape the rom
// trap has armed waits under FL_ARMED instead.
#define FL_IDLE		100
// Frames run flat out with the tape armed - flash loading has handed over the
// rom's blocks and the loader they started has not asked for the rest yet.
// Speedlock spends seven seconds decrypting itself there.
#define FL_ARMED	750
#define FL_GONE		25	// frames without the loader's reads that count as it having left
#define FL_REFRESH	33333333LL	// ns of host time between pictures, 30 a second

static int fl_held = 0;
static int fl_idle = 0;
static int fl_armed = 0;		// frames the tape has been armed for
static long long fl_drawn_at = 0;	// host time the picture on screen was drawn
static int fl_loading = 0;		// the last frame had the loader's reads
// Where the machine stood the frame the loader left, to be run again from at
// normal speed once the timeouts above have said it is gone for good
static struct {
	xState* st;
	int ok;
	Tape tap;			// where the tape stood, which the snapshot does not carry
	int frame;
} fl_back;

// What an edge loop does with the byte its IN read: the tape bit is bit 6,
// moved to bit 5 by an RRA or not, and the loop goes round while it matches
// that bit of C. A loop with RET NC after the RRA also leaves on SPACE (bit 0).
typedef struct {
	int kind;		// +1: B counts up, -1: down, 0: not an edge loop
	int rra;
	int mask;
	int space;
} flShape;

// The edge loop being watched, keyed by the address after its IN.
static struct {
	int pc;
	flShape shape;
	// learnt once from two turns alike, and good for every later entry to
	// the loop: what a turn takes and what it leaves behind
	int period;		// T a turn takes where the bus is not contended, 0: not seen
	int pmin;		// the shortest turn seen, 0: the loop is not learnt yet
	int rstep;		// R a turn adds
	int wz;			// MEMPTR as the IN leaves it
	bool carry;		// the flags a turn leaves that do not follow B
	bool fw;
	bool q;
	// the last turn, while it is being learnt
	int tick;		// comp->tickCount at the IN
	int blk;		// where the tape stood: a pulse that began during a turn
	int pos;		// is not in what its IN read, but the next IN reads it
	xreg32 regs[64];
	bool last[64];
} fl_loop;
static int fl_reads = 0;		// tape->portReads at the last look
// Auto stop for a loader that runs from ram: seen reading the tape from a known
// edge loop since the tape started, and on which frame it last did
static int fl_loop_seen = 0;
static int fl_loop_frame = 0;
static int fl_block = -1;		// the block the tape stood on last frame
static int fl_bench = 0;		// --bench-loops
static int fl_hold = 0;			// fastload_hold()
int fastload_on = 0;			// fastload_step() has anything to do

static int fl_byte(Computer* comp, int adr) {
	return memRd(comp->mem, adr & 0xffff) & 0xff;
}

void fastload_bench(int mode) {
	fl_bench = mode;
	fastload_on = fl_held || fl_bench;
}

// The edge loops that are known, Fuse's list (loader.c): the rom's LD-SAMPLE and
// its copies, Speedlock, Bleepload, Microsphere, Paul Owens, Dinaload, Search
// Loader, Alkatraz and Digital Integration. pc is the address after the
// IN A,(#FE).
static flShape fl_loop_shape(Computer* comp, int pc) {
	flShape sh = {0, 0, 0, 0};
	auto b = [&](int ofs) { return fl_byte(comp, pc + ofs); };
	if ((b(-2) != 0xdb) || (b(-1) != 0xfe)) return sh;
	int p;
	// INC B / RET Z / LD A,n / IN A,(#FE) ... JR Z back to the INC B; the LD A,n
	// may be left out, the loop's own AND leaving A at 0 for the next IN
	int head = 0;
	if ((b(-6) == 0x04) && (b(-5) == 0xc8) && (b(-4) == 0x3e)
			&& ((b(-3) == 0x00) || (b(-3) == 0x7f) || (b(-3) == 0xff)))
		head = 6;
	else if ((b(-4) == 0x04) && (b(-3) == 0xc8))
		head = 4;
	if (head) {
		if (b(0) == 0x1f) {			// RRA, maybe NOP / AND A / RET Z / RET NC / OR 0
			int x = b(1);
			if ((x == 0xf6) && (b(2) == 0x00))
				p = 3;			// TOPSOFT's loader: RET NC made harmless
			else
				p = ((x == 0x00) || (x == 0xa7) || (x == 0xc8) || (x == 0xd0)) ? 2 : 1;
			if ((b(p) != 0xa9) || (b(p + 1) != 0xe6) || (b(p + 2) != 0x20)) return sh;	// XOR C / AND #20
			sh.rra = 1;
			sh.mask = 0x20;
			sh.space = (x == 0xd0);
			p += 3;
		} else if ((b(0) == 0xa9) && (b(1) == 0xe6) && (b(2) == 0x40)) {		// XOR C / AND #40
			sh.mask = 0x40;
			p = ((b(3) == 0xd8) && (b(4) == 0x00)) ? 5 : 3;		// RET C / NOP
		} else {
			return sh;
		}
		if ((b(p) == 0x28) && (((pc + p + 2 + (signed char)b(p + 1)) & 0xffff) == ((pc - head) & 0xffff)))
			sh.kind = 1;
		return sh;
	}
	// Alkatraz: JR NZ,+3 / JP nn, or INC B / JR NZ,+1 / RET, then IN A,(#FE) /
	// RRA / RET Z / XOR C / AND #20 / JR Z back
	if (((b(-6) == 0x03) && (b(-5) == 0xc3))
			|| ((b(-6) == 0x04) && (b(-5) == 0x20) && (b(-4) == 0x01) && (b(-3) == 0xc9))) {
		if ((b(0) == 0x1f) && (b(1) == 0xc8) && (b(2) == 0xa9) && (b(3) == 0xe6)
				&& (b(4) == 0x20) && (b(5) == 0x28) && ((b(6) == 0xf1) || (b(6) == 0xf3))) {
			sh.kind = 1;
			sh.rra = 1;
			sh.mask = 0x20;
		}
		return sh;
	}
	// Digital Integration: DEC B / RET Z / IN A,(#FE) / XOR C / AND #40 / JP Z back
	if ((b(-4) == 0x05) && (b(-3) == 0xc8) && (b(0) == 0xa9) && (b(1) == 0xe6)
			&& (b(2) == 0x40) && (b(3) == 0xca)
			&& (b(4) == ((pc - 4) & 0xff)) && (b(5) == (((pc - 4) >> 8) & 0xff))) {
		sh.kind = -1;
		sh.mask = 0x40;
	}
	return sh;
}

// The code after an IN looks at the ear bit (AND #40, BIT 6,A, or RRA, maybe
// XOR C, AND #20), which a keyboard poll does not.
static int fl_tests_ear(Computer* comp, int pc) {
	for (int i = 0; i < 6; i++) {
		int x = fl_byte(comp, pc + i);
		int y = fl_byte(comp, pc + i + 1);
		if (((x == 0xe6) && (y == 0x40)) || ((x == 0xcb) && (y == 0x77))) return 1;
		if ((x == 0x1f) && (((y == 0xe6) && (fl_byte(comp, pc + i + 2) == 0x20))
				|| ((y == 0xa9) && (fl_byte(comp, pc + i + 2) == 0xe6) && (fl_byte(comp, pc + i + 3) == 0x20))))
			return 1;
	}
	return 0;
}

// The flags INC B or DEC B leave for a B of b: the loop's last flag-setting
// opcode before its IN, so they are what a turn leaves behind - and they follow
// B, bit 3 and 5 of it included, so they have to be worked out for the B a skip
// lands on. Carry is left alone, as the two opcodes do.
static void fl_bflags(bool* f, int kind, unsigned char b) {
	f[7] = !!(b & 0x80);		// S
	f[6] = (b == 0);		// Z
	f[5] = !!(b & 0x20);		// F5
	f[3] = !!(b & 0x08);		// F3
	if (kind > 0) {
		f[4] = ((b & 0x0f) == 0x00);	// H
		f[2] = (b == 0x80);		// PV
		f[1] = 0;			// N
	} else {
		f[4] = ((b & 0x0f) == 0x0f);
		f[2] = (b == 0x7f);
		f[1] = 1;
	}
}

static void fl_loop_take(Computer* comp) {
	fl_loop.tick = comp->tickCount;
	fl_loop.blk = comp->tape->block;
	fl_loop.pos = comp->tape->pos;
	memcpy(fl_loop.regs, comp->cpu->regs, sizeof(fl_loop.regs));
	memcpy(fl_loop.last, comp->cpu->flags, sizeof(fl_loop.last));
}

// A turn has passed since the last look: is the cpu where the last one left it,
// but for B moved by one the loop's way and R moved on?
static int fl_loop_steady(Computer* comp) {
	CPU* cpu = comp->cpu;
	if ((comp->tape->block != fl_loop.blk) || (comp->tape->pos != fl_loop.pos)) return 0;
	xreg32 regs[64];
	memcpy(regs, fl_loop.regs, sizeof(regs));
	if ((unsigned char)(cpu->regB - regs[1].h) != (unsigned char)fl_loop.shape.kind) return 0;
	regs[1].h = cpu->regB;
	regs[9].h = cpu->regR;
	if (memcmp(regs, cpu->regs, sizeof(regs))) return 0;
	bool flags[64];
	memcpy(flags, fl_loop.last, sizeof(flags));
	fl_bflags(flags, fl_loop.shape.kind, cpu->regB);
	return !memcmp(flags, cpu->flags, sizeof(flags));
}

// T until the tape could change the level the loop is waiting on. Any pulse
// boundary counts, even one that keeps the level.
static long long fl_tape_room(Computer* comp) {
	Tape* tap = comp->tape;
	tape_settle(tap);
	if (!tap->on || tap->rec || (tap->sigLen < 2)) return 0;
	// tape ticks in one T of the machine, the way tapSync() counts them
	double tpt = (double)tap->ticksPerNsFixed / (1LL << TAPE_RATE_BITS) * tap->speed / 100.0
		* comp->nsPerTickFixed / NS_FIXED_ONE;
	if (tpt <= 0) return 0;
	return (long long)((tap->sigLen - 1) / tpt);
}

// T the machine can be moved on in one piece, exactly: to the interrupt or the
// end of the frame, and where the bus is contended, only while the ray stays in
// the border lines above or below the screen - with a line to spare, so that a
// turn measured here lies there as well.
static long long fl_time_room(Computer* comp, int per) {
	Video* vid = comp->vid;
	long long dots;
	int line = vid->ray.y;
	long long toEnd = (long long)(vid->full.y - line) * vid->full.x - vid->ray.x;
	if (comp->cpu->flgCONT || comp->flgSNOW) {
		if ((long long)per * 2 * comp->nsPerTickFixed > (long long)vid->full.x * vid->nsPerDotFixed)
			return 0;		// the turns measured would not fit in the line to spare
		if (line < vid->bord.y - 1)
			dots = (long long)(vid->bord.y - 1 - line) * vid->full.x - vid->ray.x;
		else if (line > vid->send.y)
			dots = toEnd;
		else
			dots = 0;
	} else {
		dots = toEnd;
	}
	// nor past the interrupt: the frame's T count restarts there, mid-opcode. It
	// is placed from the blanking edge (ray.xb/yb), not from the frame's top.
	long long pos = (long long)vid->ray.yb * vid->full.x + vid->ray.xb;
	long long intp = (long long)vid->intp.y * vid->full.x + vid->intp.x;
	long long toInt = intp - pos;
	if (toInt <= 0) toInt += (long long)vid->full.x * vid->full.y;
	if (toInt < dots)
		dots = toInt;
	if (dots <= 0) return 0;
	return dots * vid->nsPerDotFixed / comp->nsPerTickFixed;
}

// Nothing but the loop itself may be going on: an interrupt, a breakpoint, a
// recording or a device that runs code of its own would see the skip.
static int fl_quiet(Computer* comp) {
	if (comp->cpu->flgIFF1 || comp->flgNMIRQ) return 0;
	if (comp->flgBRKMEM || comp->flgCOND || comp->flgIBRK || comp->flgHEAT) return 0;
#ifdef HAVEZLIB
	if (comp->rzx.play) return 0;
#endif
	return 1;
}

#ifdef XBENCH
// --bench-loops 3: every skip is run for real first, then rolled back and
// skipped, and the two compared - what shows the skip exact. The tape and the
// beeper are not in the snapshot, so they are carried by hand.
typedef struct {
	xreg32 regs[64];
	bool flags[64];
	int tick, frm, rx, ry, blk, pos, sig;
	long long acc;
} flProbe;

static void fl_probe(Computer* comp, flProbe* p) {
	memset(p, 0, sizeof(*p));		// the padding is compared too
	tape_settle(comp->tape);
	memcpy(p->regs, comp->cpu->regs, sizeof(p->regs));
	memcpy(p->flags, comp->cpu->flags, sizeof(p->flags));
	p->tick = comp->tickCount;
	p->frm = comp->frmtCount;
	p->rx = comp->vid->ray.x;
	p->ry = comp->vid->ray.y;
	p->blk = comp->tape->block;
	p->pos = comp->tape->pos;
	p->sig = comp->tape->sigLen;
	p->acc = comp->tape->tickAcc;
}

static void fl_run_real(Computer* comp, int t, flProbe* p) {
	static xState* st = NULL;
	if (!st) st = xstate_create();
	xstate_save(st, comp);
	Tape tap = *comp->tape;
	bitChan beep = *comp->beep;
	int end = comp->tickCount + t;
	while (comp->tickCount < end)
		compExec(comp);
	fl_probe(comp, p);
	xstate_load(st, comp);
	*comp->tape = tap;
	*comp->beep = beep;
}

static void fl_compare(Computer* comp, flProbe* real) {
	static int checks = 0, bad = 0;
	flProbe skip;
	fl_probe(comp, &skip);
	checks++;
	if (memcmp(real, &skip, sizeof(skip))) {
		bad++;
		xlog(XLG_TAPE, XLL_WARN, "loop skip differs at %04X: T %i/%i, tape %i:%i/%i:%i",
			comp->cpu->regPC, real->tick, skip.tick, real->blk, real->pos, skip.blk, skip.pos);
	}
	if (!(checks % 1000))
		xlog(XLG_TAPE, XLL_INFO, "loop skip checked %i times, %i different", checks, bad);
}
#endif

// Does the loop go round again on what its IN just read? Only if the byte has
// the tape bit the loop waits past, the tape has not moved on to another pulse
// since the IN read it, and SPACE is not down where the loop looks for it.
static int fl_loop_waits(Computer* comp) {
	CPU* cpu = comp->cpu;
	flShape* sh = &fl_loop.shape;
	int a = cpu->regA;
	if (!!(a & 0x40) != !!(comp->tape->volPlay & 0x80)) return 0;
	if (sh->space && !(a & 0x01)) return 0;
	return !(((sh->rra ? (a >> 1) : a) ^ cpu->regC) & sh->mask);
}

// A loader's delay loop, which edge detection also counts instead of running:
// DJNZ to itself, or DEC A with a JR NZ or JP NZ back to it. Between two edges
// these take as long as the edge loop itself - the rom waits 358 T before it
// looks for each one. Keyed by the loop's first opcode, where the pc stands
// every time the loop goes round.
enum {
	FL_DELAY_NONE = 0,
	FL_DELAY_DJNZ,
	FL_DELAY_DEC_A
};

static struct {
	int pc;
	int kind;
	int tick;		// comp->tickCount the last time round
	int r;			// and R
} fl_delay;
static int fl_pc1 = -1;			// the pc after the last opcode, and the one before
static int fl_pc2 = -1;

static int fl_delay_shape(Computer* comp, int pc) {
	auto b = [&](int ofs) { return fl_byte(comp, pc + ofs); };
	if ((b(0) == 0x10) && (b(1) == 0xfe)) return FL_DELAY_DJNZ;
	if (b(0) != 0x3d) return FL_DELAY_NONE;
	if ((b(1) == 0x20) && (b(2) == 0xfd)) return FL_DELAY_DEC_A;
	if ((b(1) == 0xc2) && (b(2) == (pc & 0xff)) && (b(3) == ((pc >> 8) & 0xff))) return FL_DELAY_DEC_A;
	return FL_DELAY_NONE;
}

// The pc is back at the head of a loop. Two rounds measure what a round takes;
// every round after that but the last is counted: the counter is left at 1, R
// and the tape are moved on, and for DEC A the flags are those of a DEC A that
// leaves 1. Nothing else in the machine moves, as with the edge loop.
static void fl_delay_step(Computer* comp, int pc) {
	CPU* cpu = comp->cpu;
	int kind = fl_delay_shape(comp, pc);
	if (!kind) return;		// the other opcode of a two-opcode loop
	if ((pc != fl_delay.pc) || (kind != fl_delay.kind)) {
		fl_delay.pc = pc;
		fl_delay.kind = kind;
		fl_delay.tick = comp->tickCount;
		fl_delay.r = cpu->regR;
		return;
	}
	int per = comp->tickCount - fl_delay.tick;
	int rstep = (cpu->regR - fl_delay.r) & 0x7f;
	fl_delay.tick = comp->tickCount;
	fl_delay.r = cpu->regR;
	if ((per <= 0) || (per > 32) || !fl_quiet(comp)) return;
	int n = (kind == FL_DELAY_DJNZ) ? cpu->regB : cpu->regA;
	int k = n - 1;
	if (k < 1) return;
	if (kind == FL_DELAY_DJNZ) {
		cpu->regB = 1;
	} else {
		cpu->regA = 1;
		fl_bflags(cpu->flags, -1, 1);		// DEC A sets them as DEC B does
	}
	cpu->regR += rstep * k;
	tapSync(comp->tape, (int)FIXED_TO_NS(ticks_to_ns_fixed(comp, per * k)));
}

// After every opcode in fast mode. Returns the ns the machine was moved on by,
// 0 when nothing was skipped.
int fastload_step(Computer* comp) {
	Tape* tap = comp->tape;
	CPU* cpu = comp->cpu;
	int pc = cpu->regPC;
	int skip = fl_held || fl_bench;		// else only watching for auto stop
	int edge = fl_bench ? (fl_bench == 2) : tape_edge();
	if (skip && edge && tap->on) {
		// back where it was one or two opcodes ago: the head of a tight loop
		if ((pc == fl_pc1) || (pc == fl_pc2))
			fl_delay_step(comp, pc);
		fl_pc2 = fl_pc1;
		fl_pc1 = pc;
	}
	if (tap->portReads == fl_reads) return 0;	// not an IN from the tape port
	fl_reads = tap->portReads;
	if (pc != fl_loop.pc) {				// a loop not seen before, or none
		fl_loop.pc = pc;
		fl_loop.shape = fl_loop_shape(comp, pc);
		// a loader not known by its code has taken over from one that was (the
		// rom's, Ninja Scooter): auto stop no longer knows when it is done
		if (fl_loop_seen && !fl_loop.shape.kind && fl_tests_ear(comp, pc))
			fl_loop_seen = 0;
		fl_loop.period = 0;
		fl_loop.pmin = 0;
		fl_loop_take(comp);
		return 0;
	}
	if (!fl_loop.shape.kind) return 0;
	fl_loop_seen = 1;
	fl_loop_frame = comp->frmCount;
	if (!fl_loop.period) {
		// learning the loop: two turns in a row with nothing but B and R moved
		int per = comp->tickCount - fl_loop.tick;
		if (fl_loop_steady(comp) && (per > 0)) {
			if (!fl_loop.pmin) {
				fl_loop.rstep = (cpu->regR - fl_loop.regs[9].h) & 0x7f;
				fl_loop.wz = cpu->regWZ;
				fl_loop.carry = cpu->flgC;
				fl_loop.fw = cpu->flgFW;
				fl_loop.q = cpu->flgQ;
			}
			if (!fl_loop.pmin || (per < fl_loop.pmin))
				fl_loop.pmin = per;
			// a turn measured where nothing contends the bus is every such turn
			if (fl_time_room(comp, per) > 0)
				fl_loop.period = per;
		}
		fl_loop_take(comp);
		if (!fl_loop.pmin || !skip) return 0;
	} else if (!skip) {
		return 0;
	}
	int per = edge ? (fl_loop.period ? fl_loop.period : fl_loop.pmin) : fl_loop.period;
	if ((per <= 0) || !fl_quiet(comp) || !fl_loop_waits(comp)) return 0;
	// a frame that ended in this opcode is dealt with before anything moves on
	if (comp->flgFRM) return 0;
	// any number of turns that ends before the pulse does: the next IN then comes
	// when it would have, and sees the edge on the turn it would have
	long long room = (fl_tape_room(comp) - 1) / per;
	if (!edge) {
		// a device that runs on its own would have to be stepped the way the
		// opcodes step it
		if ((comp->hw->sync != zx_sync) || comp->gs->enable || comp->saa->enabled
				|| comp->dif->fdc->plan || comp->dif->doors
				|| comp->keyb->per)
			return 0;
		// inside the interrupt pulse the cpu latches it at every opcode's end
		// (flgACK), and a skip past the pulse's end would miss it dropping
		if (comp->vid->intFRAME) return 0;
		room = std::min(room, fl_time_room(comp, per) / per - 1);
	}
	// the turn that brings B to 0 leaves the loop, so it is never skipped
	int kind = fl_loop.shape.kind;
	int bmax = (kind > 0) ? (255 - cpu->regB) : (cpu->regB - 1);
	int k = (int)std::min(room, (long long)bmax);
	if (k < 1) return 0;
	if (fl_loop_shape(comp, pc).kind != kind) {
		fl_loop.pc = -1;			// the code has changed under the loop
		return 0;
	}
	int t = per * k;
#ifdef XBENCH
	flProbe real;
	if (fl_bench == 3) fl_run_real(comp, t, &real);
#endif
	// what k turns leave: B and R on, and the flags and MEMPTR of a turn
	cpu->regB += kind * k;
	cpu->regR += fl_loop.rstep * k;
	cpu->regWZ = fl_loop.wz;
	cpu->flgC = fl_loop.carry;
	cpu->flgFW = fl_loop.fw;
	cpu->flgQ = fl_loop.q;
	fl_bflags(cpu->flags, kind, cpu->regB);
	int ns = 0;
	if (edge)
		tapSync(tap, (int)FIXED_TO_NS(ticks_to_ns_fixed(comp, t)));
	else {
		ns = comp_skip_ticks(comp, t);
		cpu->flgACK = 0;		// every opcode skipped ended outside the pulse
	}
#ifdef XBENCH
	if (fl_bench == 3) fl_compare(comp, &real);
#endif
	fl_loop_take(comp);
	return ns;
}

// The loader has just left: keep the machine as it is, since the game starts
// running now and would do so at the host's speed.
static void fl_back_take(Computer* comp) {
	if (!xstate_safe_tape_aside(comp)) return;
	if (!fl_back.st) fl_back.st = xstate_create();
	if (!fl_back.st || !xstate_save(fl_back.st, comp)) return;
	fl_back.tap = *comp->tape;
	fl_back.frame = comp->frmCount;
	fl_back.ok = 1;
}

// It is gone for good: put the machine back where it left. A tape still playing
// goes back with it; one stopped since stays where it stopped, at the block
// boundary the automatics chose - played back to there, a rom call the game
// makes on the way would be handed the rest of the block instead.
static void fl_back_put(Computer* comp) {
	Tape* tap = comp->tape;
	Tape* old = &fl_back.tap;
	if (!fl_back.ok || !xstate_safe_tape_aside(comp)) return;
	if ((tap->blkData != old->blkData) || (tap->blkCount != old->blkCount) || tap->rec) return;
	int frames = comp->frmCount - fl_back.frame;
	if (!xstate_load(fl_back.st, comp)) return;
	xlog(XLG_TAPE, XLL_INFO, "fast loading goes back %i frames, to where the loader left", frames);
	if (tap->on)
		tap_copy_pos(tap, old);
	tap->portReads = 0;
	fl_block = tap->block;
}

void fastload_forget() {
	fl_back.ok = 0;
}

void fastload_stop(Computer* comp) {
	fl_back.ok = 0;
	fl_loading = 0;
	if (!fl_held) return;
	xlog(XLG_TAPE, XLL_INFO, "fast loading off, block %i of %i", comp->tape->block, comp->tape->blkCount);
	fl_loop.pc = -1;
	fl_held = 0;
	fastload_on = fl_bench;
	conf.emu.fast = 0;
	vid_set_nodraw(comp->vid, 0);
}

int fastload_busy() {
	return fl_held;
}

void fastload_hold(int on) {
	fl_hold = on;
}

// The tape has moved on to another block with the loader gone: the game has
// what it wanted, so the tape stops there, as the rom's own load stops it. Only
// for a loader known by its code - one that is not might be reading the tape in
// a way that is not seen - and never inside a block. Auto play starts it again
// when the loader comes back.
static void fl_auto_stop(Computer* comp) {
	Tape* tap = comp->tape;
	if (!tap->on || tap->rec) {
		fl_loop_seen = 0;
		fl_block = tap->block;
		return;
	}
	if (!conf.tape.autostart || !fl_loop_seen || (comp->frmCount - fl_loop_frame < FL_GONE)) {
		fl_block = tap->block;
		return;
	}
	// past the last block's data there is only its pause: nothing more to load
	if ((tap->block == tap->blkCount - 1) && (tap->pos >= (int)tap->blkData[tap->block].sigCount)
			&& TAP_VOL_PAUSE(tap->volPlay)) {
		xlog(XLG_TAPE, XLL_INFO, "auto stop: the loader has left, the last block's pause");
		tapStop(tap);
		fl_loop_seen = 0;
		return;
	}
	if (tap->block == fl_block) return;
	fl_block = tap->block;
	xlog(XLG_TAPE, XLL_INFO, "auto stop: the loader has left, block %i of %i", tap->block, tap->blkCount);
	tapStop(tap);
	fl_loop_seen = 0;
}

static void fl_frame(Computer* comp) {
	Tape* tap = comp->tape;
	int reads = tap->portReads;
	tap->portReads = 0;
	fl_reads = 0;
	// Insert pressed by hand is the user's own fast mode, and autostart runs one
	// of its own until the load begins
	int may = conf.tape.fast && !fl_hold && !tap->rec && !comp->flgDBG
		&& !autostart_busy() && (fl_held || !conf.emu.fast);
	if (!tap->armed)
		fl_armed = 0;
	int loading = may && tap->on && (reads >= FL_READS);
	if (loading) {
		fl_idle = 0;
		fl_back.ok = 0;
	} else if (may && tap->armed && !tap->on && (++fl_armed < FL_ARMED)) {
		fl_idle = 0;		// the rom's part is in, and the loader it ran asks for the tape soon
	} else if (!may || !fl_held) {
		fastload_stop(comp);
		return;
	} else if (!tap->on || (++fl_idle >= FL_IDLE)) {
		// back to where the loader left, if that was taken (fl_back_take)
		fl_back_put(comp);
		fastload_stop(comp);
		return;
	} else if (fl_loading) {
		fl_back_take(comp);
	}
	fl_loading = loading;
	if (!fl_held) {
		xlog(XLG_TAPE, XLL_INFO, "fast loading on, block %i of %i, %i reads", tap->block, tap->blkCount, reads);
		fl_held = 1;
		fastload_on = 1;
		fl_drawn_at = conf.vid.fctime;
	}
	conf.emu.fast = 1;		// again every frame: a pause or a menu clears it
	// the next frame is drawn, and its end swaps it into bufimg
	// nothing past the frame a wind back would return to, or the picture would
	// step back when it does
	int draw = !fl_back.ok && (conf.vid.fctime - fl_drawn_at >= FL_REFRESH);
	if (draw)
		fl_drawn_at = conf.vid.fctime;
	vid_set_nodraw(comp->vid, !draw);
}

void fastload_frame(Computer* comp) {
	fl_auto_stop(comp);
	fl_frame(comp);
	Tape* tap = comp->tape;
	fastload_on = fl_held || fl_bench || (tap->on && conf.tape.autostart);
}

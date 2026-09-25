// emulation thread (non-GUI)

#include <QWaitCondition>
#include <QTime>

#include "ethread.h"
#include "xcore/xcore.h"
#include "xgui/xgui.h"
#include "xcore/sound.h"
#include "xcore/pacing.h"
#include "xcore/autostart.h"
#include "xcore/fastload.h"
#include "xcore/vfilters.h"
#include "libxpeccy/cpu/Z80/z80.h"
#include "libxpeccy/xstate.h"

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

void emu_lock() {
	emuGuard.lock();
}

void emu_unlock() {
	emuGuard.unlock();
}

#define LOG_OUTPUT 0
#if LOG_OUTPUT
static FILE* file = nullptr;
#endif

// unsigned char* blkData = NULL;

xThread::xThread() {
	sndNsFixed = 0;
	benchStop = -1;
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

// A block with no bytes in it is a signal the rom cannot read: a custom
// loader's data, a lead-out tone, a recording that did not decode. Nothing here
// will ever start the tape for one - the port-#FE detector only knows the rom's
// edge loop, and a loader like DeciLoad holds B still - so the automatics have
// to run the tape on into such a block instead of stopping at it.
static int tap_next_is_signal(Tape* tap) {
	int n = tap->block + 1;
	return (n < tap->blkCount) && !tap->blkData[n].hasBytes;
}

// The rom's load has returned with the tape near the end of the block, not in
// the middle of it: a loader that has the rom read only part of a block reads
// the rest itself, so the tape must go on. A few pulses are the checksum's
// last edges and the closing one.
static int tap_block_done(Tape* tap) {
	if (tap->block >= tap->blkCount) return 1;
	return ((int)tap->blkData[tap->block].sigCount - tap->pos) < 32;
}

static int tap_peek(int adr, void* data) {
	return memRd(((Computer*)data)->mem, adr);
}

// The edge routine was called by the rom's own LD-BYTES, not by a copy of it in
// ram (Krakout), which never comes back to LD_START to be handed a block and is
// played to instead. LD-EDGE-2 calls LD-EDGE-1 itself: its caller is a word up.
static int tap_rom_caller(Computer* comp) {
	int sp = comp->cpu->regSP;
	int ret = cpu_peek_word(tap_peek, comp, sp);
	if (ret == 0x05e6)
		ret = cpu_peek_word(tap_peek, comp, sp + 2);
	return ret < 0x4000;
}

// atStart says the rom is at LD_START, the top of LD_BYTES, rather than inside
// LD_EDGE_1: only there does the stack hold what LD_BYTES itself pushed, so only
// there may a block be handed over and the rom sent to its own exit. Doing it
// from LD_EDGE_1 returned into the middle of LD_BYTES with the block eaten - the
// first block of a load went missing that way.
void xThread::tap_catch_load(Computer* comp, int atStart) {
	Tape* tap = comp->tape;
	// Stop, pressed by hand, keeps the automatics off the tape - flash loading
	// included, which moves it on without ever playing it. Play, a rewind or
	// another tape hands it back.
	if (tap->userStop) return;
	// the rom is asking for a tape that has run out: "Rewind at end" puts it back
	// to the start here too, not only under the Play button. Nothing to rewind
	// for if neither of the automatics is on - Play does it then.
	if (atStart && (tape_flash() || conf.tape.autostart))
		tap_rewind_at_end(tap);
	int blk = tap->block;
	if (blk >= tap->blkCount) return;
	// A loader that has the rom read only the first part of a block and reads
	// the rest itself (Technician Ted) needs the block played, not handed over:
	// the rom is left to read that part by ear.
	if (atStart)
		earBlock = (tap->blkData[blk].hasBytes && (tapGetBlockInfo(tap, blk).size > comp->cpu->regDE)) ? blk : -1;
	if (tape_flash() && tap->blkData[blk].hasBytes && (blk != earBlock)
			&& (atStart || tap_rom_caller(comp))) {
		// A playing tape and a flash load get out of step: the rom reads the
		// block by ear and moves on while the tape still stands on it, and the
		// next block is then answered with this one. Flash loading owns the
		// tape, so stop it; the rom's edge loop gives up within a few hundred
		// T and comes back to LD_START, where the block is handed over.
		if (!atStart || tap->on) {
			// a block the tape has already played out (to a loader of its
			// own) is not the one the rom asks for now
			if (tap->on && tap_block_done(tap))
				tapNextBlock(tap);
			tapStop(tap);
			return;
		}
		unsigned short de = comp->cpu->regDE;
		unsigned short ix = comp->cpu->regIX;
		TapeBlockInfo inf = tapGetBlockInfo(tap,blk);
		unsigned char* blkData = (unsigned char*)malloc(inf.size + 2);
		tapGetBlockData(tap,blk,blkData, inf.size + 2);
#if 1
		unsigned char data = 0x01;
		unsigned char crc = blkData[0];
		bool overdata = (inf.size < de);
		int len = overdata ? inf.size : de;
		int i;
		for (i = 0; i < len; i++) {
			data = blkData[i + 1];		// 1st data byte is type, not data
			crc ^= data;
			memWr(comp->mem, ix, data);
			ix++;
			de--;
		}
		if (!overdata) {
			crc ^= blkData[i + 1];		// xor with tape crc (next byte after de|inf.size bytes)
		}
		comp->cpu->regL = data;			// last readed byte
		comp->cpu->regH = crc;			// all bytes xored (0 if no errors)
		comp->cpu->regIX = ix;			// next address
		comp->cpu->regDE = de;			// remaining size (0 if all is good)
#else
		if (inf.size >= de) {
			for (int i = 0; i < de; i++) {
				memWr(comp->mem,ix,blkData[i + 1]);
				ix++;
			}
			comp->cpu->regIX = ix;
			comp->cpu->regDE = 0x0000;	// remaining len
			comp->cpu->regHL = 0x0000;	// h = calculated_crc ^ tape_crc (=0 if no errors), l = last readed byte
		} else {
			comp->cpu->regHL = 0xff00;		// error
		}
#endif
		// the block is in memory and the tape never moved for it, so the loader
		// that comes next would be handed silence: give it the tape when it asks
		int sig = tap_next_is_signal(tap);
		tapNextBlock(tap);
		fastload_forget();
		if (sig)
			tapArmPlay(tap);
		cpu_set_pc(comp->cpu, 0x5df);
		free(blkData);
	} else if ((conf.tape.autostart || (blk == earBlock)) && !tap->on) {
		// 05E7 is LD-EDGE-1, which the rom calls for every edge, so this is
		// reached thousands of times per block - hence the guard. The window
		// is not told: it refreshes itself, and a signal per edge once left
		// stale ones in the queue that restarted a tape already stopped.
		tapPlay(tap);
	}
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
			fnams = QString(conf.scrShot.dir.c_str()).append(SLASH);
			fnams.append(QString("xpeccy_%0").arg(QTime::currentTime().toString("HHmmss_zzz")));	// TODO: counter-based name (1ms is not enough)
			idx = 0;
			do {
				file.setFileName(QString("%0_%1.scr").arg(fnams).arg(idx));
				idx++;
			} while (file.exists());
			file.open(QFile::WriteOnly);
			file.write((char*)(comp->mem->ramData + (5 << 14)), 0x1b00);
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
static int raBroken = 0;		// the snapshot cannot be taken at all: stop trying

// One frame, no breakpoints, no sound, nothing that reaches outside the
// machine. The bound is only there to let go of a machine that never finishes a
// frame - dummy hardware, say - instead of spinning on it every frame forever.
static int ra_frame(Computer* comp) {
	int guard = 200000;		// a frame is ~20000 opcodes
	while (!comp->flgFRM && (guard-- > 0))
		compExec(comp);
	comp->flgFRM = 0;
	return guard > 0;
}

// 1 when the machine has been run on and has to be wound back afterwards.
// xstate_safe() answers for everything the snapshot does not carry; what is
// left here is this side's own policy.
int xThread::runAhead(Computer* comp) {
	if (comp != raOwner) {		// a new machine may well fit where the last one did not
		raOwner = comp;
		raBroken = 0;
	}
	if (finish || raBroken) return 0;
	if (conf.emu.runahead < 1) return 0;
	if (conf.emu.fast || conf.emu.pause || comp->flgDBG) return 0;
	if (autostart_busy()) return 0;		// the typist counts frames of its own
	if (!xstate_safe(comp)) return 0;
	if (!raState) raState = xstate_create();
	if (!raState || !xstate_save(raState, comp)) {
		raBroken = 1;
		return 0;
	}
	x_runahead = 1;
	for (int i = 0; i < conf.emu.runahead; i++) {
		if (!ra_frame(comp)) {
			xlog(XLG_CORE, XLL_WARN, "run ahead: the machine does not finish a frame, giving up");
			raBroken = 1;
			break;
		}
	}
	x_runahead = 0;
	return 1;
}

void xThread::emuCycle(Computer* comp) {
	int tm;
	int brkskip = 0;
	// sndNsFixed is deliberately not cleared here: it holds the part of a sample
	// not yet made, and a cycle can end anywhere. Clearing it dropped that
	// remainder on every wake-up, and wake-ups come every 2ms.
	conf.snd.fill = 1;
	while (!comp->flgBRK && conf.snd.fill && !finish && !conf.emu.pause) {
		// exec 1 opcode (or handle INT, NMI)
		if (conf.emu.pause) {
			sndNsFixed += NS_TO_FIXED(1000);
		} else {
			if (brkskip) {
				brkskip = comp->flgDBG;
				comp->flgDBG = 1;		// block breakpoints checking
				tm = compExec(comp);
				comp->flgDBG = brkskip;
				brkskip = 0;
			} else {
				tm = compExec(comp);			// TODO: it exits when fetch-brk is occured, pc doesn't changed
			}
			sndNsFixed += NS_TO_FIXED(tm);
			// tape trap	TODO: rework it as a system breakpoint
			// the rom check first: it is three flags, where asking the cpu for
			// its pc is a call, and this runs on every instruction
			if (zx_rom_active(comp)) {
				int pc = cpu_get_pc(comp->cpu);
				if ((pc == 0x56c) || (pc == 0x5e7)) {	// load: ix:addr, de:len (0x580 ?) 56c/559
					tap_catch_load(comp, pc == 0x56c);
				} else if (pc == 0x4d0) {				// save: ix:addr, de:len, a:block type(b7), hl:pilot len (1f80/0c98)?
					tap_catch_save(comp);
				}
				if (conf.tape.autostart && !tape_flash() && ((pc == 0x5df) || (pc == 0x53a))
						&& !tap_next_is_signal(comp->tape) && tap_block_done(comp->tape)) {
					comp->tape->sigLen = 1e6;
					tapNextBlock(comp->tape);
					tapStop(comp->tape);
				}
			}
			// a loader's edge loop, counted instead of run
			if (fastload_on)
				sndNsFixed += NS_TO_FIXED(fastload_step(comp));
		}
		// sound buffer update. In fast mode there is nothing to mix - the
		// only thing sndSync() still does there is run the GS, so a machine
		// without one skips the call and keeps the count
		if (conf.emu.fast && !comp->gs->enable) {
			while (sndNsFixed > nsPerSampleFixed)
				sndNsFixed -= nsPerSampleFixed;
		} else {
			while (sndNsFixed > nsPerSampleFixed) {
				sndSync(comp);
				sndNsFixed -= nsPerSampleFixed;
			}
		}
		if (comp->flgFRM) {
			comp->flgFRM = 0;
			if (conf.emu.fast) conf.snd.fill = 0;	// see sndSync()
			if ((benchStop >= 0) && (conf.vid.fcount + 1 >= benchStop))
				conf.snd.fill = 0;		// the bench stops on a frame, not on a sample
			conf.vid.fctime = paceClockNs();	// for the fps readout
			conf.vid.fcount++;
			comp->frmCount++;
			autostart_frame(comp);
			fastload_frame(comp);
			// before run-ahead: the debugger's screen view wants the machine as
			// it really stands, not the frame it is about to guess at
			vid_scr_snap(comp->vid);
			// the frame just made is not the one to show: run on to the one
			// the player's last key press is already in
			int wound = runAhead(comp);
// process noflic/scanlines (if !fast ???)
// buffers is already switches, bufimg - just painted (greyscale, if flag is set), scrimg - new
			if (!conf.emu.fast && (noflic > 0))
				scrMix(pscr, bufimg + comp->vid->lcut.y * bytesPerLine + comp->vid->lcut.x * 8,
					comp->vid->vsze.x * 2, comp->vid->vsze.y, bytesPerLine,
					noflic / 100.0, noflicGamma, noflicMode);

			// printf("s_frame\n");
			emit s_frame();
			if (wound) xstate_load(raState, comp);
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
					ptr->hits++;		// counted before the condition, HITS uses it
					if (!brk_cond_true(ptr, comp)) {	// condition is false: go on
						comp->flgBRK = 0;
						if (comp->flgBRKPRE) brkskip = 1;
					} else {
						ptr->count++;
						brkAction(comp, ptr, &brkskip);
					}
				}
			}
		} else if (brk_cond_count() && brk_check_cond(comp)) {	// conditions not bound to an address
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
	comp->flgBRK = 0;
	comp->flgNMIRQ = 0;
}

// a recording opened since the last cycle starts playing here
void xThread::rzx_begin(Computer* comp) {
#if HAVEZLIB
	if (comp->rzx.start) {
		comp->rzx.start = 0;
		comp->rzx.play = 1;
		comp->rzx.fCount = 0;
		comp->rzx.fCurrent = 0;
		rewind(comp->rzx.file);
		rzxGetFrame(comp);
	}
#else
	(void)comp;
#endif
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
#if USEMUTEX
		if (!conf.emu.fast && !finish) {
			emutex.lock();
			qwc.wait(&emutex);
			emutex.unlock();
		}
#else
		while (!conf.emu.fast && sleepy && !finish)
			usleep(10);
#endif
	} while (!finish);
	xstate_destroy(raState);
	raState = NULL;
	exit(0);
}

#ifdef XBENCH

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

// Runs the timed part: fast mode as run() does it, or full sound mixing with
// a budget of 256 samples per cycle, the way the pacer hands them out.
// hash folds every finished frame and every sample into one number, so two
// builds can be shown to run the machine identically.
int xThread::bench(int frames, int skip, int full, int hash, const char* prof, const char* shot, int nodraw) {
	Computer* comp = conf.zx;
	if (!comp) return 0;
	blockSignals(true);
	setOutput("NULL");
	pacingClose();		// the budget is handed out here, not by the timer
	conf.emu.pause = 0;
	// the bench picks its mode itself: fast loading would switch it under a tape
	fastload_hold(1);
	rzx_begin(comp);
	// warm up: a tape or disk being started, a demo getting to its part. In
	// the mode that is measured: where fast mode hands the machine back is not
	// where a cycle with sound does, so a switch between them would leave the
	// two builds being compared on different instructions
	conf.emu.fast = full ? 0 : 1;
	int f0 = conf.vid.fcount;
	benchStop = f0 + skip;		// and on a frame, so both builds start measuring on the same one
	while ((conf.vid.fcount - f0 < skip) && !conf.emu.pause) {
		conf.snd.need = full ? 256 : 0;
		emu_lock();
		emuCycle(comp);
		emu_unlock();
	}
	conf.emu.fast = full ? 0 : 1;
	if (nodraw) vid_set_nodraw(comp->vid, 1);	// what the picture itself costs
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
	int spos = snd_ring_fill_pos();
	long long t0 = paceClockNs();
	int tk0 = comp->tickCount;
	f0 = conf.vid.fcount;
	int fl = f0;
	benchStop = f0 + frames;	// the last cycle ends on the frame, whatever the budget
	while ((conf.vid.fcount - f0 < frames) && !conf.emu.pause) {
		conf.snd.need = full ? 256 : 0;
		emu_lock();
		emuCycle(comp);
		emu_unlock();
		if (hash) {
			if (conf.vid.fcount != fl) {
				fl = conf.vid.fcount;
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
	int done = conf.vid.fcount - f0;
	double sec = (t1 - t0) / 1e9;
	double fps = (sec > 0) ? done / sec : 0;
	double rt = (comp->vid->nsPerFrame > 0) ? 1e9 / comp->vid->nsPerFrame : 50;
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
	fflush(stdout);
	fastload_hold(0);
	return done;
}

#endif

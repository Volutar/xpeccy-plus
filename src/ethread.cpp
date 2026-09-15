// emulation thread (non-GUI)

#include <QWaitCondition>
#include <QTime>

#include "ethread.h"
#include "xcore/xcore.h"
#include "xgui/xgui.h"
#include "xcore/sound.h"
#include "xcore/pacing.h"
#include "xcore/autostart.h"
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

void xThread::tap_catch_load(Computer* comp) {
	int blk = comp->tape->block;
	if (blk >= comp->tape->blkCount) return;
	if (conf.tape.fast && comp->tape->blkData[blk].hasBytes) {
		unsigned short de = comp->cpu->regDE;
		unsigned short ix = comp->cpu->regIX;
		TapeBlockInfo inf = tapGetBlockInfo(comp->tape,blk);
		unsigned char* blkData = (unsigned char*)malloc(inf.size + 2);
		tapGetBlockData(comp->tape,blk,blkData, inf.size + 2);
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
		tapNextBlock(comp->tape);
		cpu_set_pc(comp->cpu, 0x5df);
		free(blkData);
	} else if (conf.tape.autostart) {
		emit tapeSignal(TW_STATE,TWS_AUTOPLAY);
	}
}

void xThread::tap_catch_save(Computer* comp) {
	if (conf.tape.fast) {
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
	// everything but the debugger goes on; a fetch break has to re-run the
	// instruction it stopped in front of
	comp->flgBRK = 0;
	if (ptr->fetch && (ptr->type != BRK_COND)) *brkskip = 1;
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

static void ra_back(Computer* comp, long rayOff, long lineOff) {
	if (!xstate_load(raState, comp)) return;
	// The image buffers are outside the snapshot and the ahead frame swaps
	// them, so the ray goes back by its offset into whichever buffer is current
	// after the rollback, not by the address it held before.
	comp->vid->ray.ptr = scrimg + rayOff;
	comp->vid->ray.lptr = scrimg + lineOff;
}

// 1 when the machine has been run on and has to be wound back afterwards.
// xstate_safe() answers for everything the snapshot does not carry; what is
// left here is this side's own policy.
int xThread::runAhead(Computer* comp, long* rayOff, long* lineOff) {
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
	*rayOff = comp->vid->ray.ptr - scrimg;
	*lineOff = comp->vid->ray.lptr - scrimg;
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
			int pc = cpu_get_pc(comp->cpu);
			if ((comp->mem->map[0].type == MEM_ROM) && comp->flgROM && !comp->flgDOS && !comp->flgEXT) {
				if ((pc == 0x56c) || (pc == 0x5e7)) {	// load: ix:addr, de:len (0x580 ?) 56c/559
					tap_catch_load(comp);
				} else if (pc == 0x4d0) {				// save: ix:addr, de:len, a:block type(b7), hl:pilot len (1f80/0c98)?
					tap_catch_save(comp);
				}
				if (conf.tape.autostart && !conf.tape.fast && ((pc == 0x5df) || (pc == 0x53a))) {
					comp->tape->sigLen = 1e6;
					tapNextBlock(comp->tape);
					tapStop(comp->tape);
					emit tapeSignal(TW_STATE,TWS_STOP);
				}
			}
		}
		// sound buffer update
		while (sndNsFixed > nsPerSampleFixed) {
			sndSync(comp);
			sndNsFixed -= nsPerSampleFixed;
		}
		if (comp->flgFRM) {
			comp->flgFRM = 0;
			conf.vid.fctime = paceClockNs();	// for the fps readout
			conf.vid.fcount++;
			comp->frmCount++;
			autostart_frame(comp);
			// before run-ahead: the debugger's screen view wants the machine as
			// it really stands, not the frame it is about to guess at
			vid_scr_snap(comp->vid);
			// the frame just made is not the one to show: run on to the one
			// the player's last key press is already in
			long rayOff = 0, lineOff = 0;
			int wound = runAhead(comp, &rayOff, &lineOff);
// process noflic/scanlines (if !fast ???)
// buffers is already switches, bufimg - just painted (greyscale, if flag is set), scrimg - new
			if (!conf.emu.fast && (noflic > 0))
				scrMix(pscr, bufimg + comp->vid->lcut.y * bytesPerLine + comp->vid->lcut.x * 8,
					comp->vid->vsze.x * 2, comp->vid->vsze.y, bytesPerLine,
					noflic / 100.0, noflicGamma, noflicMode);

			// printf("s_frame\n");
			emit s_frame();
			if (wound) ra_back(comp, rayOff, lineOff);
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
						if (ptr->fetch) brkskip = 1;
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
#if HAVEZLIB
			if (comp->rzx.start) {
				comp->rzx.start = 0;
				comp->rzx.play = 1;
				comp->rzx.fCount = 0;
				comp->rzx.fCurrent = 0;
				rewind(comp->rzx.file);
				rzxGetFrame(comp);
			}
#endif
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

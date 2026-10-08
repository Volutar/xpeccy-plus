#pragma once

#include <QThread>
#include <QMutex>

#include "xcore/xcore.h"
#include "libxpeccy/ldbytes.h"

class xThread : public QThread {
	Q_OBJECT
	public:
		xThread();
		unsigned finish:1;
		long long sndNsFixed;
#ifdef XBENCH
		int bench(int frames, int skip, int full, int hash, const char* prof, const char* shot, int nodraw, int heat, int rewind, int steps, int split);
#endif
		int benchStop;		// the bench ends the cycle at this frame, -1: never
		int benchRzx;		// the bench stops after the INT that ends this frame of a recording, 0: no
		int benchRzxSeek;	// ...gets there, goes back to this frame and gets there again, -1: no
		int earBlock;		// the block the rom reads only part of, -1: none
	public slots:
		void stop();
	signals:
		void s_close();
		void s_frame();
		void dbgRequest();
		void scrRequest();
		void tapeSignal(int,int);
	private:
		void run();
		void emuCycle(Computer*);
		void rzx_begin(Computer*);
		int runAhead(Computer*);
#ifdef XBENCH
		void rewindCheck(Computer*, int full);
#endif
		void brkAction(Computer*, xBrkPoint*, int*);
		void tap_catch_load(Computer*, int, int base = LD_ROM_BASE, int dir = 1);
		void tap_hand_over(Computer*, int blk, int base, int dir);
		void tap_catch_save(Computer*);
};

#pragma once

#include <QThread>
#include <QMutex>

#include "xcore/xcore.h"

class xThread : public QThread {
	Q_OBJECT
	public:
		xThread();
		unsigned finish:1;
		long long sndNsFixed;
#ifdef XBENCH
		int bench(int frames, int skip, int full, int hash, const char* prof, const char* shot, int nodraw, int heat);
#endif
		int benchStop;		// the bench ends the cycle at this frame, -1: never
		int earBlock;		// the block the rom reads only part of, -1: none
		int ldCopy;		// where a ram copy of LD-BYTES starts, -1: none seen
		int ldCopyDir;		// its INC IX (+1) or DEC IX (-1)
		int ldReads;		// tape->portReads at the last look for a copy
		int ldProbed;		// the pc after an IN already looked at this frame
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
		void brkAction(Computer*, xBrkPoint*, int*);
		void tap_catch_load(Computer*, int, int base = 0x0556, int dir = 1);
		void tap_find_copy(Computer*);
		void tap_catch_copy(Computer*);
		void tap_catch_save(Computer*);
};

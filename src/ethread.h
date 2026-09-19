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
		int runAhead(Computer*, long*, long*);
		void brkAction(Computer*, xBrkPoint*, int*);
		void tap_catch_load(Computer*, int);
		void tap_catch_save(Computer*);
};

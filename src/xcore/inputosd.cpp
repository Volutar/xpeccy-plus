// the input overlay: which input it shows, handed from the emulation to the gui

#include <QMutex>
#include <string.h>

#include "xcore.h"

static QMutex shownGuard;
static InState shownState;
static int shownReads = 0;

// the program's reads are kept only while something may show them
void iosd_apply() {
	int want = conf.iosd.on && (conf.iosd.source != IOSD_SRC_HOST);
	if (want && !inview.on)
		iview_reset(&inview);
	inview.on = want;
}

// A replay has nothing pressed on the host, only what the program read. Live,
// Auto shows the host: a key lights the moment it goes down, scanned or not.
static int iosd_reads(Computer* comp) {
	switch (conf.iosd.source) {
		case IOSD_SRC_READS: return 1;
		case IOSD_SRC_HOST: return 0;
	}
	return comp->rzx.play ? 1 : 0;
}

static void iosd_host(Computer* comp, InState* st) {
	memset(st, 0, sizeof(InState));
	// kbd->map is by half-row from A15 down, a clear bit is a key down
	for (int i = 0; i < 8; i++)
		st->keys[i] = ~comp->keyb->map[7 - i] & 0x1f;
	st->known = 0xff;
	Joystick* joy = comp->joy;
	if (joy->type == XJ_KEMPSTON) {
		st->joyLive = 1;
		st->joy = joy->state & (joy->extbuttons ? 0xff : 0x1f);
	}
	Mouse* mou = comp->mouse;
	if (mou->enable) {
		st->mouseLive = 1;
		st->mbtn = (mou->lmb ? IVM_LEFT : 0) | (mou->rmb ? IVM_RIGHT : 0) | (mou->mmb ? IVM_MIDDLE : 0);
	}
	st->mwheel = mou->wheel & 0x0f;
	st->mx = (unsigned char)(int)(mou->xpos * mou->sensitivity);
	st->my = (unsigned char)(int)(mou->ypos * mou->sensitivity);
}

// at the frame's end, beside the picture it goes with
void iosd_publish(Computer* comp) {
	if (!conf.iosd.on) return;
	InState st;
	int reads = iosd_reads(comp);
	if (reads) {
		iview_state(&inview, comp->frmCount, &st);
	} else {
		iosd_host(comp, &st);
	}
	QMutexLocker lock(&shownGuard);
	shownState = st;
	shownReads = reads;
}

// 1: what the program read, 0: what the host has down
int iosd_shown(InState* st) {
	QMutexLocker lock(&shownGuard);
	*st = shownState;
	return shownReads;
}

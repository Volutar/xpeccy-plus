// the input overlay: which input it shows

#include <string.h>

#include "xcore.h"

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
	for (int i = 0; i < 8; i++)
		st->keys[i] = ~comp->keyb->map[i] & 0x1f;		// a clear bit is a key down
	Joystick* joy = comp->joy;
	if (joy->type == XJ_KEMPSTON) {
		st->joyLive = 1;
		st->joy = joy->state & (joy->extbuttons ? 0xff : 0x1f);
	}
	Mouse* mou = comp->mouse;
	if (mou->enable) {
		st->mouseLive = 1;
		st->mbtn = mouse_buttons(mou);
	}
	st->mwheel = mou->wheel & 0x0f;
	st->mx = mouseGetX(mou);
	st->my = mouseGetY(mou);
}

// What the overlay shows at the frame's end, taken with the picture it goes
// with. 0: the overlay is off
int iosd_state(Computer* comp, InState* st) {
	if (!conf.iosd.on) return 0;
	if (iosd_reads(comp)) {
		iview_state(&inview, comp->frmCount, st);
	} else {
		iosd_host(comp, st);
	}
	st->ext = (comp->joy->type == XJ_KEMPSTON) && comp->joy->extbuttons;
	return 1;
}

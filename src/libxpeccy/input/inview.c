#include <string.h>

#include "inview.h"
#include "../spectrum.h"

InView inview;

void iview_reset(InView* iv) {
	int on = iv->on;
	memset(iv, 0, sizeof(InView));
	iv->on = on;
	for (int i = 0; i < 8; i++)
		iv->keyFrm[i] = -IV_STALE - 1;
	iv->joyFrm = -IV_STALE - 1;
	iv->mouseFrm = -IV_STALE - 1;
}

// A read of #FE answers for every half-row its high byte selects, ANDed. One row
// selected is an exact answer - the rom and nearly every game scan that way. With
// several, a key up is up in all of them, but a key down could be in any, so it
// only stands where one of those rows already had it.
static void iview_keys(InView* iv, int frm, int port, int val) {
	unsigned char sel = ~(port >> 8) & 0xff;
	unsigned char down = ~val & 0x1f;
	if (!sel) return;
	if (!(sel & (sel - 1))) {
		for (int i = 0; i < 8; i++) {
			if (sel & (1 << i)) {
				iv->keys[i] = down;
				iv->keyFrm[i] = frm;
			}
		}
		return;
	}
	for (int i = 0; i < 8; i++) {
		if (!(sel & (1 << i))) continue;
		iv->keys[i] &= down;
		if (!down)
			iv->keyFrm[i] = frm;
	}
}

void iview_in(InView* iv, Computer* comp, int port, int val) {
	int frm = comp->frmCount;
	if ((port & 0xff) == 0xfe) {
		iview_keys(iv, frm, port, val);
	} else if (((port & 0xff) == 0x1f) && !comp->flgDOS) {		// TR-DOS has the FDC there
		val &= comp->joy->extbuttons ? 0xff : 0x1f;
		// both ways at once is a port nothing answers, not a joystick
		if (((val & 3) == 3) || ((val & 12) == 12)) return;
		iv->joy = val;
		iv->joyFrm = frm;
	} else if ((port & 0x05a3) == 0x0083) {		// #FADF, the decode a kempston mouse has
		iv->mbtn = ~val & 7;
		iv->mwheel = (val >> 4) & 0x0f;
		iv->mouseFrm = frm;
	} else if ((port & 0x05a3) == 0x0183) {		// #FBDF
		iv->mx = val;
		iv->mouseFrm = frm;
	} else if ((port & 0x05a3) == 0x0583) {		// #FFDF
		iv->my = val;
		iv->mouseFrm = frm;
	}
}

static int iview_fresh(int frm, int at) {
	int age = frm - at;
	// the frame counter goes back with a rewind or a seek: anything ahead is stale
	return (age >= 0) && (age <= IV_STALE);
}

void iview_state(InView* iv, int frm, InState* st) {
	memset(st, 0, sizeof(InState));
	for (int i = 0; i < 8; i++) {
		if (!iview_fresh(frm, iv->keyFrm[i])) continue;
		st->keys[i] = iv->keys[i];
		st->known |= (1 << i);
	}
	if (iview_fresh(frm, iv->joyFrm)) {
		st->joyLive = 1;
		st->joy = iv->joy;
	}
	// the position and the wheel counter stay where they were; only the
	// buttons go up when the program stops looking
	st->mx = iv->mx;
	st->my = iv->my;
	st->mwheel = iv->mwheel;
	if (iview_fresh(frm, iv->mouseFrm)) {
		st->mouseLive = 1;
		st->mbtn = iv->mbtn;
	}
}

#include <string.h>

#include "inview.h"
#include "../spectrum.h"

InView inview;

void iview_reset(InView* iv) {
	memset(iv, 0, sizeof(InView));
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
	unsigned char sel = ~(port >> 8) & 0xff;	// bit n is A(8+n), row 7-n of the map
	unsigned char down = ~val & 0x1f;
	if (!sel) return;
	int one = !(sel & (sel - 1));
	for (int i = 0; i < 8; i++) {
		if (!(sel & (1 << i))) continue;
		int row = 7 - i;
		if (one) {
			iv->s.keys[row] = down;
		} else {
			iv->s.keys[row] &= down;
		}
		if (one || !down)
			iv->keyFrm[row] = frm;
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
		iv->s.joy = val;
		iv->joyFrm = frm;
	} else if ((port & 0x05a3) == 0x0083) {		// #FADF, the decode a kempston mouse has
		iv->s.mbtn = ~val & 7;
		iv->s.mwheel = (val >> 4) & 0x0f;
		iv->mouseFrm = frm;
	} else if ((port & 0x05a3) == 0x0183) {		// #FBDF
		iv->s.mx = val;
		iv->mouseFrm = frm;
	} else if ((port & 0x05a3) == 0x0583) {		// #FFDF
		iv->s.my = val;
		iv->mouseFrm = frm;
	}
}

// Run-ahead leaves stamps a few frames ahead of the counter it rolls back; a
// rewind or a seek leaves them far ahead, and those are stale
static int iview_fresh(int frm, int at) {
	int age = frm - at;
	return (age >= -IV_STALE) && (age <= IV_STALE);
}

void iview_state(InView* iv, int frm, InState* st) {
	*st = iv->s;
	for (int i = 0; i < 8; i++) {
		if (!iview_fresh(frm, iv->keyFrm[i]))
			st->keys[i] = 0;
	}
	// the position and the wheel counter stay where they were; only the
	// buttons go up when the program stops looking
	st->joyLive = iview_fresh(frm, iv->joyFrm);
	if (!st->joyLive) st->joy = 0;
	st->mouseLive = iview_fresh(frm, iv->mouseFrm);
	if (!st->mouseLive) st->mbtn = 0;
}

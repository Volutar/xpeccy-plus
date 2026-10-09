#include "xcore.h"
#include "rzxseek.h"
#include "fastload.h"
#include "../libxpeccy/filetypes/filetypes.h"

static int rs_target = -1;		// the frame to stop at, -1: not seeking
static int rs_pause = 0;		// playback was paused, and is again once there
static int rs_fast = 0;			// the user's own fast mode, given back after
static long long rs_drawn = 0;

static void rs_stop(Computer* comp, int there) {
	rs_target = -1;
	fast_hold_release(comp);
	conf.emu.fast = rs_fast;
	if (there && rs_pause) conf.emu.pause |= PR_PAUSE;
}

int rzx_seek_start(Computer* comp, int frame) {
	int paused = (rs_target >= 0) ? rs_pause : !!(conf.emu.pause & PR_PAUSE);
	int at = rzx_seek(comp, frame);
	// paused, a frame is still run: it is what puts the picture there
	if ((at >= frame) && paused)
		frame = at + 1;
	if ((at < 0) || (at >= frame)) {
		if (rs_target >= 0) rs_stop(comp, at >= 0);
		return at;
	}
	if (rs_target < 0) {
		rs_fast = conf.emu.fast;
		rs_drawn = 0;
	}
	rs_pause = paused;
	rs_target = frame;
	conf.emu.pause &= ~PR_PAUSE;
	conf.emu.fast = 1;
	return at;
}

void rzx_seek_frame(Computer* comp) {
	if (rs_target < 0) return;
	if (!comp->rzx.play) {
		rs_stop(comp, 0);
	} else if (comp->rzx.fCurrent >= rs_target) {
		rs_stop(comp, 1);
	} else {
		fast_hold_frame(comp, &rs_drawn, 1);
	}
}

int rzx_seeking() {
	return rs_target >= 0;
}

void rzx_seek_cancel(Computer* comp) {
	if (rs_target >= 0) rs_stop(comp, 0);
}

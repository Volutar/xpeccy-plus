#include "emulwin.h"
#include "xcore/vscalers.h"
#include "xcore/pacing.h"

#include <QMenu>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

// how long after the last read of the mouse a click still takes it
#define MOUSE_READ_NS	1000000000LL

void MainWin::mousePressEvent(QMouseEvent *ev){
	Computer* comp = conf.zx;
	if (comp->flgDBG) {
		if (ev->button() == Qt::LeftButton) {
			calcCoords(ev);
		}
		ev->ignore();
	} else if ((grabMice ? ((ev->button() == Qt::LeftButton) || (ev->button() == Qt::RightButton))
			: ((ev->button() == Qt::LeftButton) && !(ev->modifiers() & Qt::AltModifier)))
			&& inputResume()) {
		clickResumed = true;	// only the resume: no fire, and its release takes no mouse
	} else {
		switch (ev->button()) {
			case Qt::LeftButton:
				if (grabMice) {
					comp->mouse->lmb = 1;
				} else if (ev->modifiers() & Qt::AltModifier) {	// print dot address
					calcCoords(ev);
				}
				break;
			case Qt::RightButton:
				if (grabMice) {
					comp->mouse->rmb = 1;
				} else {
					// opened on the press, so the release of the same button
					// lands in the menu - xRootMenu drops it (see emulwin.cpp)
					popupUserMenu(QPoint(ev->xGlobalX,ev->xGlobalY));
				}
				break;
			default: break;
		}
	}
}

void MainWin::mouseReleaseEvent(QMouseEvent *ev) {
	if (clickResumed) {
		clickResumed = false;
		return;
	}
	if (conf.emu.pause) return;
	Computer* comp = conf.zx;
	if (comp->flgDBG) {
		ev->ignore();
	} else {
		switch (ev->button()) {
			case Qt::LeftButton:
				if (grabMice) {
					comp->mouse->lmb = 0;
				} else if (!(ev->modifiers() & Qt::AltModifier) && comp->mouse->enable
						&& (comp->mouse->used || (paceClockNs() - mouseReadAt < MOUSE_READ_NS))) {
					mouseGrabOn();		// a click takes it only while a program reads it
				}
				break;
			case Qt::RightButton:
				if (grabMice) {
					comp->mouse->rmb = 0;
				}
				break;
			case X_MidButton:
				if (grabMice) {
					mouseGrabOff();
				} else {
					mouseGrabOn();
				}
				break;
			default: break;
		}
	}
}

void MainWin::wheelEvent(QWheelEvent* ev) {
	Computer* comp = conf.zx;
	if (comp->flgDBG) {
		ev->ignore();
	} else if (grabMice) {
		if (comp->mouse->hasWheel)
			mousePress(comp->mouse, (ev->yDelta < 0) ? XM_WHEELDN : XM_WHEELUP, 0);
	} else if (ev->modifiers() & Qt::AltModifier) {
		// the volume, with Alt: a bare wheel is too easily turned. Alt turns the
		// wheel sideways on Windows and X11, so either axis is taken
		int d = ev->angleDelta().y() ? ev->angleDelta().y() : ev->angleDelta().x();
		if (!d) return;
		conf.snd.vol.master = toLimits(conf.snd.vol.master + ((d > 0) ? 5 : -5), 0, 100);
		volumeChanged();
	} else {
		ev->ignore();
	}
}

// How many readings where the pointer was before a recentering counts as
// undone, and how many recenterings a host may undo before we stop trying.
#define MOUSE_WARP_TTL	8
#define MOUSE_WARP_GIVEUP 3
// A hand cannot do this in one event: a recentering that was not absorbed, a
// window that moved, a screen that changed. Host pixels.
#define MOUSE_JUMP	256
// How long the hand is taken to keep its speed, ms.
#define MOUSE_LEAD_MS	30.0

QPoint MainWin::winCenter() {
	return mapToGlobal(rect().center());
}

// on the window's own screen: QCursor::setPos(QPoint) goes by the primary one,
// which is another scale factor when the two differ
void MainWin::winCursorTo(QPoint pos) {
#ifdef WIDGET_SCREEN
	QCursor::setPos(WIDGET_SCREEN, pos);
#else
	QCursor::setPos(pos);
#endif
}

// The one way in and out of the grab: from here the pointer is the machine's,
// and the state mouseRead keeps has to start over on every new grab
void MainWin::mouseGrabOn() {
	grabMice = 1;
	mbHintShow(false);
	grabMouse(QCursor(Qt::BlankCursor));
	setMessage(" grab mouse, " XREL_KEYS " lets go ");
	mouseRecenter(1);
}

void MainWin::mouseGrabOff() {
	grabMice = 0;
	releaseMouse();
	setMessage(" release mouse ");
	winCursorTo(winCenter());		// it was hidden: show it somewhere sane
}

// Put the pointer back in the middle of the window, so that it has room to
// move before it runs into a screen edge. This is a request, not a fact: a
// pointer that reports an absolute position - a virtual machine, a remote
// desktop, a tablet, some kvm switches - is put back where the device holds
// it, and SetCursorPos still reports success. So the pointer may next be seen
// either here or where it was, and mouseRead keeps both answers open.
// 'fresh' is a new grab, where what an earlier one learned no longer holds.
void MainWin::mouseRecenter(int fresh) {
	if (fresh) {
		warpFail = 0;
		mouseVel = QPointF();
	}
	mouseLast = QCursor::pos();
	mouseAt = paceClockNs();
	if (warpFail >= MOUSE_WARP_GIVEUP) return;
	warpAt = winCenter();
	warpTtl = MOUSE_WARP_TTL;
	winCursorTo(warpAt);
}

void MainWin::mouseMoveEvent(QMouseEvent *ev) {
	if (!grabMice || conf.emu.pause) {
		if (ev->buttons() & Qt::LeftButton) {
			calcCoords(ev);
		}
		return;
	}
	mouseRead();
}

// Over another program's window a grab gets no moves: on Windows Qt passes on
// the first and drops the rest, on macOS a grab holds inside the frame alone.
// A pointer that got out before a move could recenter it is caught here, a
// timer tick later.
void MainWin::mouseWatch() {
	if (!grabMice || conf.emu.pause) return;
	if (rect().contains(mapFromGlobal(QCursor::pos()))) return;
	mouseRead();
}

void MainWin::mouseRead() {
	Computer* comp = conf.zx;
	// Where the pointer is now, not where the event saw it: events arrive in
	// batches, and one taken before a recentering but handled after it would
	// read as the host putting the pointer back.
	QPoint gpos = QCursor::pos();
	qint64 now = paceClockNs();
	double ms = qBound(0.0, (now - mouseAt) / 1e6, MOUSE_LEAD_MS);
	// The pointer moved by the difference from where it was - but a pending
	// recentering leaves two answers to where that is, and the nearer one is
	// it: the two candidates are a third of the window apart. Nearer once the
	// hand's own way since the last reading is added to both: an event loop
	// held up for a few milliseconds sees a fast hand far from either.
	QPoint was = mouseLast;
	bool hand = true;	// the reading is the hand's, not the recentering's
	if (warpTtl) {
		QPoint off = gpos - warpAt;
		QPoint lead = (mouseVel * ms).toPoint();
		if ((qAbs(off.x()) <= 1) && (qAbs(off.y()) <= 1)) {
			// on the target: no answer yet - a host that undoes the
			// recentering reads back the target too, until its device speaks
			was = warpAt;
			warpAt = gpos;
			hand = false;
		} else if (gpos == mouseLast) {
			// nothing moved since the last reading: no answer either
		} else if ((off - lead).manhattanLength() <= (gpos - mouseLast - lead).manhattanLength()) {
			was = warpAt;			// it landed: this is the pointer now
			mouseLast = gpos;
			warpTtl = 0;
			warpFail = 0;
		} else {
			// where it was: the device holds it, or the recentering is not
			// applied yet - the system queues it behind the moves before it
			mouseLast = gpos;
			if (!--warpTtl && (++warpFail >= MOUSE_WARP_GIVEUP))
				xlog(XLG_INPUT, XLL_INFO, "mouse: this host puts the pointer straight back, recentering dropped");
		}
	} else {
		mouseLast = gpos;
	}
	// readings that come together say nothing of the speed, and one a
	// millisecond apart catches a 1000 Hz mouse between its steps: averaged
	if (hand && (ms >= 0.5))
		mouseVel = (mouseVel + QPointF(gpos - was) / ms) / 2;
	mouseAt = now;
	int dx = toLimits(gpos.x() - was.x(), -MOUSE_JUMP, MOUSE_JUMP);
	int dy = toLimits(gpos.y() - was.y(), -MOUSE_JUMP, MOUSE_JUMP);
	// The counter is in emulated pixels, the reading is in host ones: divide by
	// the zoom, so the same move of the hand takes the emulated pointer the
	// same way across the monitor whatever size the picture is drawn at. The
	// division leaves a remainder, which the next reading takes.
	double kx = 1.0;
	double ky = 1.0;
	if ((drawW > 0) && (drawH > 0)) {
		kx = comp->vid->vsze.x / (double)drawW;
		ky = comp->vid->vsze.y / (double)drawH;
	}
	double fx = dx * kx + mouseRemX;
	double fy = dy * ky + mouseRemY;
	// TODO: sensitivity is applied when the port is read - ps/2, which works
	// with the delta, would need it here
	comp->mouse->xdelta = (int)fx;
	comp->mouse->ydelta = (int)fy;
	mouseRemX = fx - comp->mouse->xdelta;
	mouseRemY = fy - comp->mouse->ydelta;
	if (comp->mouse->xdelta || comp->mouse->ydelta) x_input_bump();
	comp->mouse->xpos += comp->mouse->xdelta;
	comp->mouse->ypos -= comp->mouse->ydelta;	// axis is reverted
	// far enough from the middle to be worth putting back, on a host that
	// lets it be put back at all. Not again while the last one is unanswered,
	// unless the pointer is already out: no move may come to say so later
	if (warpFail < MOUSE_WARP_GIVEUP) {
		QPoint cen = winCenter();
		if (((qAbs(gpos.x() - cen.x()) > width() / 3) || (qAbs(gpos.y() - cen.y()) > height() / 3))
				&& (!warpTtl || !rect().contains(mapFromGlobal(gpos))))
			mouseRecenter();
	}
}

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

QPoint MainWin::winCursorPos() {
#ifdef WIDGET_SCREEN
	return QCursor::pos(WIDGET_SCREEN);
#else
	return QCursor::pos();
#endif
}

bool MainWin::winHolds(QPoint pos) {
	return rect().contains(mapFromGlobal(pos));
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
	mouseLast = winCursorPos();
	mouseAt = paceClockNs();
	mouseWarp();
}

// A recentering asked again before the last is answered keeps its count of readings
void MainWin::mouseWarp() {
	if (warpFail >= MOUSE_WARP_GIVEUP) return;
	warpAim = warpAt = winCenter();
	if (!warpTtl) warpTtl = MOUSE_WARP_TTL;
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

// Qt sends no moves over another program's window: one that got out is caught here
void MainWin::mouseWatch() {
	if (!grabMice || conf.emu.pause) return;
	if (winHolds(winCursorPos())) return;
	mouseRead();
}

void MainWin::mouseRead() {
	Computer* comp = conf.zx;
	// not the event's position: one queued before a recentering reads as undoing it
	QPoint gpos = winCursorPos();
	qint64 now = paceClockNs();
	double ms = (now - mouseAt) / 1e6;
	if (ms > MOUSE_LEAD_MS) {		// the hand has stopped since
		mouseVel = QPointF();
		ms = MOUSE_LEAD_MS;
	}
	// A pending recentering leaves two answers to where the pointer was, a third
	// of the window apart: the nearer one, once the hand's way since is added
	QPoint was = mouseLast;
	bool hand = true;	// the reading is the hand's, not the recentering's
	if (warpTtl) {
		QPoint off = gpos - warpAt;
		QPoint aim = gpos - warpAim;
		QPoint lead = (mouseVel * ms).toPoint();
		if ((qAbs(aim.x()) <= 1) && (qAbs(aim.y()) <= 1)) {
			// on the target: no answer, an undoing host reads it back too
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
			// the old place: the device holds it, or the system has not applied it yet
			mouseLast = gpos;
			if (!--warpTtl && (++warpFail >= MOUSE_WARP_GIVEUP))
				xlog(XLG_INPUT, XLL_INFO, "mouse: this host puts the pointer straight back, recentering dropped");
		}
	} else {
		mouseLast = gpos;
	}
	// readings that come together say nothing of the speed; a 1000 Hz mouse is caught between steps
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
	// far enough out to put back; while one is pending, only once out of the window
	QPoint cen = winCenter();
	if (((qAbs(gpos.x() - cen.x()) > width() / 3) || (qAbs(gpos.y() - cen.y()) > height() / 3))
			&& (!warpTtl || !winHolds(gpos)))
		mouseWarp();
}

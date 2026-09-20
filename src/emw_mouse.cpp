#include "emulwin.h"
#include "xcore/vscalers.h"

#include <QMenu>
#if QT_VERSION >= QT_VERSION_CHECK(5,0,0)
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#endif

void MainWin::mousePressEvent(QMouseEvent *ev){
	Computer* comp = conf.zx;
	if (comp->flgDBG) {
		if (ev->button() == Qt::LeftButton) {
			calcCoords(ev);
		}
		ev->ignore();
	} else {
		switch (ev->button()) {
			case Qt::LeftButton:
				if (grabMice) {
					comp->mouse->lmb = 1;
				} else if (ev->modifiers() & Qt::ControlModifier) {	// print dot address
					calcCoords(ev);
				}
				break;
			case Qt::RightButton:
				if (grabMice) {
					comp->mouse->rmb = 1;
				} else {
					// opened on the press, so the release of the same button
					// lands in the menu - xRootMenu drops it (see emulwin.cpp)
					fillUserMenu();
					userMenu->popup(QPoint(ev->xGlobalX,ev->xGlobalY));
					userMenu->setFocus();
				}
				break;
			default: break;
		}
	}
}

void MainWin::mouseReleaseEvent(QMouseEvent *ev) {
	if (conf.emu.pause) return;
	Computer* comp = conf.zx;
	if (comp->flgDBG) {
		ev->ignore();
	} else {
		switch (ev->button()) {
			case Qt::LeftButton:
				if (grabMice) {
					comp->mouse->lmb = 0;
#ifdef __APPLE__
				} else if (comp->mouse->enable) {
					mouseGrabOn();
#endif
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
	} else {
		if (ev->yDelta < 0) {
			conf.snd.vol.master -= 5;
			if (conf.snd.vol.master < 0)
				conf.snd.vol.master = 0;
		} else {
			conf.snd.vol.master += 5;
			if (conf.snd.vol.master > 100)
				conf.snd.vol.master = 100;
		}
		setMessage(QString(" volume %0% ").arg(conf.snd.vol.master));
	}
}

// How many events the recentering stays a candidate for where the pointer is,
// and how many recenterings a host may undo before we stop trying.
#define MOUSE_WARP_TTL	2
#define MOUSE_WARP_GIVEUP 3
// A hand cannot do this in one event: a recentering that was not absorbed, a
// window that moved, a screen that changed. Host pixels.
#define MOUSE_JUMP	256

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
// and the state mouseMoveEvent keeps has to start over on every new grab
void MainWin::mouseGrabOn() {
	grabMice = 1;
	grabMouse(QCursor(Qt::BlankCursor));
	setMessage(" grab mouse ");
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
// either here or where it was, and mouseMoveEvent keeps both answers open.
// 'fresh' is a new grab, where what an earlier one learned no longer holds.
void MainWin::mouseRecenter(int fresh) {
	if (fresh) warpFail = 0;
	mouseLast = QCursor::pos();
	if (warpFail >= MOUSE_WARP_GIVEUP) return;
	warpAt = winCenter();
	warpTtl = MOUSE_WARP_TTL;
	winCursorTo(warpAt);
}

void MainWin::mouseMoveEvent(QMouseEvent *ev) {
	Computer* comp = conf.zx;
	if (!grabMice || conf.emu.pause) {
		if (ev->buttons() & Qt::LeftButton) {
			calcCoords(ev);
		}
		return;
	}
	QPoint gpos(ev->xGlobalX, ev->xGlobalY);
	// The pointer moved by the difference from where it was - but a pending
	// recentering leaves two answers to where that is, and the nearer one is
	// it: the hand moves a few pixels an event, the two candidates are a
	// third of the window apart.
	QPoint was = mouseLast;
	if (warpTtl) {
		warpTtl--;
		if ((gpos - warpAt).manhattanLength() <= (gpos - mouseLast).manhattanLength()) {
			was = warpAt;			// where we put it
			warpAt = gpos;
			if (!warpTtl) {			// it stayed: this is the pointer now
				mouseLast = gpos;
				warpFail = 0;
			}
		} else {				// where the device holds it
			mouseLast = gpos;
			warpTtl = 0;
			if (++warpFail >= MOUSE_WARP_GIVEUP)
				xlog(XLG_INPUT, XLL_INFO, "mouse: this host puts the pointer straight back, recentering dropped");
		}
	} else {
		mouseLast = gpos;
	}
	int dx = toLimits(gpos.x() - was.x(), -MOUSE_JUMP, MOUSE_JUMP);
	int dy = toLimits(gpos.y() - was.y(), -MOUSE_JUMP, MOUSE_JUMP);
	// The counter is in emulated pixels, the event is in host ones: divide by
	// the zoom, so the same move of the hand takes the emulated pointer the
	// same way across the monitor whatever size the picture is drawn at. The
	// division leaves a remainder, which the next event takes.
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
	comp->mouse->xpos += comp->mouse->xdelta;
	comp->mouse->ypos -= comp->mouse->ydelta;	// axis is reverted
	// far enough from the middle to be worth putting back, on a host that
	// lets it be put back at all
	if (warpFail < MOUSE_WARP_GIVEUP) {
		QPoint cen = winCenter();
		if ((qAbs(gpos.x() - cen.x()) > width() / 3) || (qAbs(gpos.y() - cen.y()) > height() / 3))
			mouseRecenter();
	}
}

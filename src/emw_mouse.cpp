#include "emulwin.h"
#include "xcore/vscalers.h"
#include "xcore/pacing.h"

#include <QApplication>
#include <QMenu>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

#ifdef _WIN32
#include <windows.h>
#endif

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

#define MOUSE_WARP_TTL	8		// readings at the old place that count a recentering undone
#define MOUSE_WARP_GIVEUP 3		// recenterings undone in a row before we stop trying
// A hand cannot do this in one event: a recentering that was not absorbed, a
// window that moved, a screen that changed. Host pixels.
#define MOUSE_JUMP	256
#define MOUSE_LEAD_MS	30.0		// how long the hand is taken to keep its speed, ms

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

// on X11 a server round trip, at up to the mouse's rate while grabbed
QPoint MainWin::winCursorPos() {
#ifdef WIDGET_SCREEN
	return QCursor::pos(WIDGET_SCREEN);
#else
	return QCursor::pos();
#endif
}

// in and off the edge: a pointer the system clips to the window stops on the
// edge instead of getting out
bool MainWin::winInner(QPoint pos) {
	return rect().adjusted(2, 2, -2, -2).contains(mapFromGlobal(pos));
}

// past the middle third: worth putting back
bool MainWin::winFar(QPoint pos) {
	QPoint cen = winCenter();
	return (qAbs(pos.x() - cen.x()) > width() / 3) || (qAbs(pos.y() - cen.y()) > height() / 3);
}

// grabbed and taking moves: pause() lets the pointer go at once, before its pause lands
bool MainWin::mouseHeld() {
	return grabMice && !conf.emu.pause && !conf.emu.pauseReq;
}

// A new grab starts the reading over. Not from a tool window: no focus out would end it
void MainWin::mouseGrabOn() {
	if (!isActiveWindow()) return;
	grabMice = 1;
	mbHintShow(false);
	ptrIdle.stop();
	ptrShow();		// the window's own pointer comes back with a release, so not a blank one
	grabMouse(QCursor(Qt::BlankCursor));
	setMessage(" grab mouse, " XREL_KEYS " lets go ");
	rawMouse(true);
	if (xlog_on(XLG_INPUT, XLL_DEBUG)) {
		QPoint org = mapToGlobal(QPoint(0, 0));
		QPoint cen = winCenter();
		xlog_put(XLG_INPUT, XLL_DEBUG, "mouse: grab on, window %d,%d %dx%d, centre %d,%d, dpr %.2f",
			org.x(), org.y(), width(), height(), cen.x(), cen.y(), devicePixelRatioF());
	}
	mouseRecenter(1);
}

void MainWin::mouseGrabOff() {
	grabMice = 0;
	releaseMouse();
	mouseClip(false);
	rawMouse(false);
	xlog(XLG_INPUT, XLL_DEBUG, "mouse: grab off, raw reports %u by step, %u absolute", rawRel, rawAbs);
	setMessage(" release mouse ");
	winCursorTo(winCenter());		// it was hidden: show it somewhere sane
	ptrArm();
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
		warpTtl = 0;
		warpFail = 0;
		mouseVel = QPointF();
		rawRel = 0;
		rawAbs = 0;
	}
	mouseLast = winCursorPos();
	mouseAt = paceClockNs();
	mouseWarp();
}

// A recentering asked again before the last is answered keeps its count of readings
void MainWin::mouseWarp() {
	if (warpFail >= MOUSE_WARP_GIVEUP) return;
	warpAim = warpAt = winCenter();
	if (!warpTtl)
		warpTtl = MOUSE_WARP_TTL;
	winCursorTo(warpAt);
}

// Windows tells a mouse that moves by steps from a device that gives where to
// be - a virtual machine, a remote desktop, a tablet as a mouse
// (MOUSE_MOVE_ABSOLUTE). Asked while grabbed; the messages go on as before
void MainWin::rawMouse(bool on) {
#ifdef _WIN32
	RAWINPUTDEVICE dev;
	dev.usUsagePage = 1;	// generic desktop
	dev.usUsage = 2;		// mouse
	dev.dwFlags = on ? 0 : RIDEV_REMOVE;
	dev.hwndTarget = on ? (HWND)winId() : NULL;
	if (!RegisterRawInputDevices(&dev, 1, sizeof(dev)))
		xlog(XLG_INPUT, XLL_WARN, "mouse: raw input %s failed, error %lu", on ? "on" : "off", GetLastError());
#else
	(void)on;
#endif
}

// a mouse by steps and nothing absolute this grab: a recentering it shows late is only late
bool MainWin::mouseByStep() {
	return rawRel && !rawAbs;
}

// A mouse by steps is kept in the window by the system too, as games do: a
// recentering that lands late no longer lets it out. Put again each watch tick,
// which follows a moved or resized window and a clip the system dropped. Never
// for a VM or a remote desktop: their pointer must be free to leave
void MainWin::mouseClip(bool on) {
#ifdef _WIN32
	if (!on) {
		if (mouseClipped) {
			ClipCursor(NULL);
			mouseClipped = false;
			xlog(XLG_INPUT, XLL_DEBUG, "mouse: clip off");
		}
		return;
	}
	HWND wnd = (HWND)winId();
	RECT r;
	if (!GetClientRect(wnd, &r)) return;
	POINT tl = {r.left, r.top};
	POINT br = {r.right, r.bottom};
	ClientToScreen(wnd, &tl);
	ClientToScreen(wnd, &br);
	RECT want = {tl.x, tl.y, br.x, br.y};
	RECT cur;
	if (mouseClipped && GetClipCursor(&cur) && EqualRect(&cur, &want)) return;
	if (ClipCursor(&want)) {
		if (!mouseClipped)
			xlog(XLG_INPUT, XLL_DEBUG, "mouse: clip on %ld,%ld-%ld,%ld", want.left, want.top, want.right, want.bottom);
		mouseClipped = true;
	}
#else
	(void)on;
#endif
}

#ifdef _WIN32
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
bool MainWin::nativeEvent(const QByteArray& type, void* msg, qintptr* res) {
#else
bool MainWin::nativeEvent(const QByteArray& type, void* msg, long* res) {
#endif
	MSG* m = (MSG*)msg;
	if (m->message == WM_INPUT) {
		RAWINPUT raw;
		UINT size = sizeof(raw);
		if ((GetRawInputData((HRAWINPUT)m->lParam, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1)
				&& (raw.header.dwType == RIM_TYPEMOUSE)) {
			if (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
				rawAbs++;
			} else if (raw.data.mouse.lLastX || raw.data.mouse.lLastY) {
				rawRel++;
			}
		}
	}
	return QWidget::nativeEvent(type, msg, res);	// unhandled: DefWindowProc still sees WM_INPUT
}
#endif

void MainWin::mouseMoveEvent(QMouseEvent *ev) {
	if (!mouseHeld()) {
		if (ev->buttons() & Qt::LeftButton) {
			calcCoords(ev);
		}
		return;
	}
	mouseRead();
}

// Qt sends no moves over another program's window: one that got out is caught
// here, and one held on the clip's edge, which sends no moves either
void MainWin::mouseWatch() {
	bool held = mouseHeld() && isActiveWindow();
	mouseClip(held && mouseByStep());
	if (!held) return;
	QPoint pos = winCursorPos();
	if (winInner(pos)) return;
	xlog(XLG_INPUT, XLL_DEBUG, "mouse: watch caught it on the edge or out at %d,%d, pending %d, fails %d",
		pos.x(), pos.y(), warpTtl, warpFail);
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
	// a pending recentering: here or at the old place - the nearer, with the hand's way added
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
			if (!winFar(gpos)) {
				warpTtl = 0;		// in the middle either way: nothing left to judge
			} else if (!--warpTtl && !mouseByStep() && (++warpFail >= MOUSE_WARP_GIVEUP)) {
				// a mouse by steps is never given up on: there the moves in flight undo
				// some recenterings, and giving up would pin it on the clip's edge
				xlog(XLG_INPUT, XLL_INFO, "mouse: this host puts the pointer straight back, recentering dropped (raw %u/%u)",
					rawRel, rawAbs);
			}
		}
	} else {
		mouseLast = gpos;
	}
	// readings in one go carry no speed, and one step of a fast mouse is noisy: averaged
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
	// while one is pending, again only once on the edge or out: the moves in flight
	// can put the pointer back over a recentering, a mouse by steps too
	if (winFar(gpos) && (!warpTtl || !winInner(gpos)))
		mouseWarp();
}

// THE POINTER HIDING ITSELF

#define PTR_IDLE_MS	2000	// still this long over the picture: the pointer goes

// Every move over the window, whatever widget of it is under the pointer, and the
// menus and dialogs of the program coming and going
class xPtrFilter : public QObject {
	public:
		xPtrFilter(MainWin* m) : QObject(m), mw(m) {}
	protected:
		bool eventFilter(QObject* obj, QEvent* ev) override {
			switch (ev->type()) {
				case QEvent::MouseMove: {
					QWidget* w = qobject_cast<QWidget*>(obj);
					if (w && (w->window() == mw->frame)) {
						QMouseEvent* mev = static_cast<QMouseEvent*>(ev);
						mw->ptrMoved(QPoint(int(mev->xGlobalX), int(mev->xGlobalY)));
					}
					break;
				}
				case QEvent::Show:
				case QEvent::Hide: {
					QWidget* w = qobject_cast<QWidget*>(obj);
					if (!w || !w->isWindow() || (w == mw->frame)) break;
					Qt::WindowType t = w->windowType();
					if ((t != Qt::Popup) && (t != Qt::Dialog)) break;	// a tip changes nothing
					if (ev->type() == QEvent::Show) {
						mw->ptrIdle.stop();
						mw->ptrShow();
					} else {
						mw->ptrArm();		// gone: the pointer may be still over the picture
					}
					break;
				}
				default: break;
			}
			return false;
		}
	private:
		MainWin* mw;
};

void MainWin::ptrInit() {
	ptrIdle.setSingleShot(true);
	ptrIdle.setInterval(PTR_IDLE_MS);
	connect(&ptrIdle, &QTimer::timeout, this, &MainWin::ptrIdleOut);
	qApp->installEventFilter(new xPtrFilter(this));
	ptrLast = winCursorPos();	// where it rests before any move, as well as it is known

}

// Only a move that moved it: Windows sends moves with none when a window is shown,
// activated or its cursor changes - the hiding itself would bring it back.
// The event's position: on Wayland QCursor::pos() is not kept.
void MainWin::ptrMoved(QPoint gpos) {
	if (gpos == ptrLast) return;
	ptrLast = gpos;
	ptrShow();
	ptrArm();
}

// The grab has a blank pointer of its own as long as it holds
void MainWin::ptrArm() {
	if (conf.win.ptrHide && !grabMice) {
		ptrIdle.start();
	} else {
		ptrIdle.stop();
	}
}

void MainWin::ptrShow() {
	if (!ptrHidden) return;
	ptrHidden = false;
	unsetCursor();
}

// fullscreen in or out, or the option changed: shown, and counting again
void MainWin::ptrSync() {
	int mode = (conf.win.ptrHide ? 1 : 0) | (conf.vid.fullScreen ? 2 : 0);
	if (mode == ptrMode) return;
	ptrMode = mode;
	ptrShow();
	ptrArm();
}

// Not over a bar, nor while a menu or a dialog is up; not with a button down,
// a drag going on; not for a window in the background
void MainWin::ptrIdleOut() {
	if (ptrHidden || !conf.win.ptrHide || grabMice) return;
	if (!frame->isActiveWindow() || QApplication::activePopupWidget()
			|| QApplication::activeModalWidget())
		return;
	if (QApplication::mouseButtons()) {		// a release brings no move: counted again
		ptrIdle.start();
		return;
	}
	// the widget under it, the bars over the picture counted: the picture or its view.
	// Where the last move left it: a window that moved under a still pointer is mapped
	// anew, and one gone elsewhere since shows a pointer of its own there anyway
	QWidget* under = frame->childAt(frame->mapFromGlobal(ptrLast));
	if (!under || ((under != this) && (under != glw))) return;
	ptrHidden = true;
	setCursor(Qt::BlankCursor);
}

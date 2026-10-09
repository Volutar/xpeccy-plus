// virtual keyboard

#include "vkeyboard.h"
#include "xcore/xcore.h"

#include <QApplication>
#include <QIcon>
#include <QPainter>
#include <QMenu>
#include <QTimer>

#if defined(_WIN32)
#include <windows.h>
#endif

typedef struct {
	int x;
	int y;
	int dx;
	int dy;
} xRect;

typedef struct {
	unsigned char ch;
	xRect rect;
	xRect rect2;
} xVKeyMap;

static const xVKeyMap vkZxMap[] = {
	{'1',{2,10,46,60},{0,0,0,0}},{'2',{2 + 49,10,46,60},{0,0,0,0}},{'3',{2 + 49*2,10,46,60},{0,0,0,0}},{'4',{2 + 49*3,10,46,60},{0,0,0,0}},{'5',{2 + 49*4,10,46,60},{0,0,0,0}},
	{'6',{2 + 49*5,10,46,60},{0,0,0,0}},{'7',{2 + 49*6,10,46,60},{0,0,0,0}},{'8',{2 + 49*7,10,46,60},{0,0,0,0}},{'9',{2 + 49*8,10,46,60},{0,0,0,0}},{'0',{2 + 49*9,10,46,60},{0,0,0,0}},
	{'q',{26,70,46,60},{0,0,0,0}},{'w',{26 + 49,70,46,60},{0,0,0,0}},{'e',{26 + 49*2,70,46,60},{0,0,0,0}},{'r',{26 + 49*3,70,46,60},{0,0,0,0}},{'t',{26 + 49*4,70,46,60},{0,0,0,0}},
	{'y',{26 + 49*5,70,46,60},{0,0,0,0}},{'u',{26 + 49*6,70,46,60},{0,0,0,0}},{'i',{26 + 49*7,70,46,60},{0,0,0,0}},{'o',{26 + 49*8,70,46,60},{0,0,0,0}},{'p',{26 + 49*9,70,46,60},{0,0,0,0}},
	{'a',{38,130,46,60},{0,0,0,0}},{'s',{38 + 49,130,46,60},{0,0,0,0}},{'d',{38 + 49*2,130,46,60},{0,0,0,0}},{'f',{38 + 49*3,130,46,60},{0,0,0,0}},{'g',{38 + 49*4,130,46,60},{0,0,0,0}},
	{'h',{38 + 49*5,130,46,60},{0,0,0,0}},{'j',{38 + 49*6,130,46,60},{0,0,0,0}},{'k',{38 + 49*7,130,46,60},{0,0,0,0}},{'l',{38 + 49*8,130,46,60},{0,0,0,0}},{'E',{38 + 49*9,130,46,60},{0,0,0,0}},
	{'C',{2,190,46+12,60},{0,0,0,0}},{'z',{14 + 49,190,46,60},{0,0,0,0}},{'x',{14 + 49*2,190,46,60},{0,0,0,0}},{'c',{14 + 49*3,190,46,60},{0,0,0,0}},{'v',{14 + 49*4,190,46,60},{0,0,0,0}},
	{'b',{14 + 49*5,190,46,60},{0,0,0,0}},{'n',{14 + 49*6,190,46,60},{0,0,0,0}},{'m',{14 + 49*7,190,46,60},{0,0,0,0}},{'S',{14 + 49*8,190,46,60},{0,0,0,0}},{' ',{14 + 49*9,190,46+24,60},{0,0,0,0}},
	{0,{0,0,0,0},{0,0,0,0}}
};

keyWindow::keyWindow(QWidget* p):QDialog(p) {
	kb = NULL;
	dock = 0;
	memset(&xent, 0, sizeof(xent));
	xent.key = ENDKEY;
	pxm = QPixmap(":/images/keymap_volutar.png");
	// The picture is drawn to sit on a ground of its own: the keys are opaque and
	// only a thin rim around each one is clear, so what shows through it is the
	// pressed colour. Take that ground from the picture's own corner - under a
	// light theme the window colour would outline every key all the time.
	ground = pxm.toImage().pixelColor(0, 0);
	if (ground.alpha() < 255)
		ground = QColor(Qt::black);
	setModal(false);
	setWindowModality(Qt::NonModal);
	setSizeGripEnabled(true);
	setMinimumSize(pxm.width() / 2, pxm.height() / 2);
	setZoom(storedZoom());
	setWindowIcon(QIcon(":/images/keyboard.png"));
	setWindowTitle("Virtual Keyboard - ZX Spectrum");
	if (conf.keywin.dock)
		setDock(true);
}

// window pixels per picture pixel. The picture keeps its shape, so a window of
// another aspect gets an empty strip beside it
double keyWindow::scale() {
	double sx = width() / (double)pxm.width();
	double sy = height() / (double)pxm.height();
	return (sx < sy) ? sx : sy;
}

// the picture's shape, from one side to the other
int keyWindow::higFor(int wid) {
	return qRound(wid * pxm.height() / (double)pxm.width());
}

int keyWindow::widFor(int hig) {
	return qRound(hig * pxm.width() / (double)pxm.height());
}

// the size the window was left at last time
double keyWindow::storedZoom() {
	return (conf.keywin.width > 0) ? (conf.keywin.width / (double)pxm.width()) : 1.0;
}

QPoint keyWindow::imgPos(QPoint pos) {
	double sc = scale();
	if (sc <= 0.0) return QPoint(-1, -1);
	int x = (pos.x() - (width() - pxm.width() * sc) / 2) / sc;
	int y = (pos.y() - (height() - pxm.height() * sc) / 2) / sc;
	return QPoint(x, y);
}

void keyWindow::setZoom(double z) {
	resize(qRound(pxm.width() * z), qRound(pxm.height() * z));
}

// docked: no frame of its own, the emulator window drags it around
void keyWindow::setDock(bool d) {
	bool vis = isVisible();
	dock = d;
	conf.keywin.dock = dock;
	setWindowFlags(d ? (Qt::Tool | Qt::FramelessWindowHint) : Qt::Dialog);
	setSizeGripEnabled(!d);
	// the geometry is set before the window comes up as well as after: set
	// beforehand it goes to the window manager as the position asked for, which
	// is the one thing that can stop it placing the window somewhere of its own
	snap();
	if (vis) show();		// changing the flags takes the window off screen
	if (!d)
		setZoom(storedZoom());
}

void keyWindow::snap() {
	if (!dock) return;
	QWidget* par = parentWidget() ? parentWidget()->window() : nullptr;
	// A window that is not on screen yet has no geometry worth reading: this
	// window is built before the emulator window is shown, and taking its size
	// then put the keyboard off the screen entirely. Whatever is missed here
	// comes back from showEvent and from every move of the emulator window.
	if (!par || !par->isVisible()) return;
	QRect rc = par->frameGeometry();
	int wid = rc.width();
	int hig = higFor(wid);
	int y = rc.bottom() + 1;
	// fullscreen, or the window sits too low: lay the keyboard over the picture
	if (par->isFullScreen() || (y + hig > SCREENSIZE.height()))
		y = rc.bottom() - hig + 1;
	QRect want(rc.left(), y, wid, hig);
	if (geometry() != want)
		setGeometry(want);
}

// A window manager puts the window where it wants when it maps it, and that
// happens after show() has returned - so while docked, every move it makes is
// answered by putting the window back. snap() does nothing once the geometry
// is right, which is what stops this from bouncing.
void keyWindow::moveEvent(QMoveEvent* ev) {
	QDialog::moveEvent(ev);
	if (dock)
		snap();
}

void keyWindow::showMenu(QPoint gpos) {
	QMenu mnu(this);
	QAction* act;
	const double zval[] = {1.0, 1.5, 2.0, 3.0};
	for (int i = 0; i < 4; i++) {
		act = mnu.addAction(QString("Zoom x%1").arg(zval[i]));
		act->setData(zval[i]);
		act->setDisabled(dock);
	}
	mnu.addSeparator();
	QAction* dck = mnu.addAction("Dock under the emulator");
	dck->setCheckable(true);
	dck->setChecked(dock);
	mnu.addSeparator();
	QAction* rall = mnu.addAction("Release all keys");
	act = mnu.exec(gpos);
	if (act == NULL) return;
	if (act == dck) {
		setDock(!dock);
	} else if (act == rall) {
		if (kb) kbdReleaseAll(kb);
		xent.zxKey[0] = 0;
		update();
	} else if (act->data().isValid()) {
		setZoom(act->data().toDouble());
	}
}

// The window is free to take any shape here; the picture keeps its own and sits
// in the middle of its ground. Only Windows squares the window itself, in
// nativeEvent below, where it can be done before anything is drawn - putting the
// size back afterwards means fighting the window manager for the frame, and the
// manager wins: the picture ends up drawn for one size while the frame is
// another, so the keyboard is cut off or a stale one is left behind.
void keyWindow::resizeEvent(QResizeEvent* ev) {
	QDialog::resizeEvent(ev);
	if (!dock)
		conf.keywin.width = ev->size().width();
}

void keyWindow::showEvent(QShowEvent* ev) {
	QDialog::showEvent(ev);
#if defined(__APPLE__)
	// the shape has to be told to the window itself, and a new one is made
	// whenever the flags change, so it is set every time it comes up
	vkbd_set_aspect(this, pxm.width(), pxm.height());
#endif
	snap();
	// see moveEvent: a window manager can also map the window in its own place
	// without a move worth reporting, so the geometry is put back a few times
	// while the mapping settles - how long that takes is the manager's business
	if (dock) {
		QTimer::singleShot(0, this, SLOT(snap()));
		QTimer::singleShot(150, this, SLOT(snap()));
		QTimer::singleShot(400, this, SLOT(snap()));
	}
}

#if defined(_WIN32)
// Windows asks what size the window may take while the frame is still being
// dragged, so the shape is fixed here, before anything is drawn. Correcting it
// afterwards in resizeEvent works too, but the window then flickers between the
// size the mouse asked for and the one it is put back to for the whole drag.
// The rectangle is in real pixels and Qt's own sizes are not, so the frame is
// measured off the window itself.
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
bool keyWindow::nativeEvent(const QByteArray& type, void* msg, qintptr* res) {
#else
bool keyWindow::nativeEvent(const QByteArray& type, void* msg, long* res) {
#endif
	MSG* wmsg = (MSG*)msg;
	if (wmsg->message == WM_SIZING) {
		RECT* rct = (RECT*)wmsg->lParam;
		RECT wrc, crc;
		GetWindowRect(wmsg->hwnd, &wrc);
		GetClientRect(wmsg->hwnd, &crc);
		int fw = (wrc.right - wrc.left) - crc.right;	// frame around the picture
		int fh = (wrc.bottom - wrc.top) - crc.bottom;
		int wid = (rct->right - rct->left) - fw;
		int hig = (rct->bottom - rct->top) - fh;
		// the side being dragged stays where the mouse is, the other one follows
		switch(wmsg->wParam) {
			case WMSZ_TOP:
			case WMSZ_BOTTOM:
				wid = widFor(hig);
				rct->right = rct->left + wid + fw;
				break;
			default:
				hig = higFor(wid);
				if ((wmsg->wParam == WMSZ_TOPLEFT) || (wmsg->wParam == WMSZ_TOPRIGHT)) {
					rct->top = rct->bottom - hig - fh;
				} else {
					rct->bottom = rct->top + hig + fh;
				}
				break;
		}
		if (res) *res = TRUE;
		return true;
	}
	// the shape fixed above leaves the mouse inside the window, not on the frame,
	// and Qt's window class has no cursor to put back over the frame's sizing arrows
	if ((wmsg->message == WM_SETCURSOR) && (LOWORD(wmsg->lParam) == HTCLIENT)
			&& !QApplication::overrideCursor() && !childAt(mapFromGlobal(QCursor::pos()))) {
		SetCursor(LoadCursor(NULL, IDC_ARROW));
		if (res) *res = TRUE;
		return true;
	}
	return QDialog::nativeEvent(type, msg, res);
}
#endif

void keyWindow::switcher() {
	if (isVisible())
		hide();
	else
		show();
}

void keyWindow::upd(Keyboard* k) {
	kb = k;
	if (isVisible())
		repaint();
}

void keyWindow::rall(Keyboard* k) {
	if (!isVisible()) {
		kbdReleaseAll(k);
	}
}

// TODO: untide from ZX-keyboard (row,pos calculation)
void keyWindow::paintEvent(QPaintEvent*) {
	QPainter pnt;
	const xRect* prct;
	unsigned char val;
	int row, pos;
	double sc = scale();
	pnt.begin(this);
	pnt.fillRect(rect(), ground);
	// everything below is in the picture's own pixels, the painter sizes it
	pnt.translate((width() - pxm.width() * sc) / 2, (height() - pxm.height() * sc) / 2);
	pnt.scale(sc, sc);
	pnt.setRenderHint(QPainter::SmoothPixmapTransform, true);
	if (kb) {
		for(int i = 0; i < 8; i++) {
			pos = (i & 4) ? 0 : 9;
			row = (i & 4) ? (i & 3) : (~i & 3);
			val = ~kb->map[i] & 0x1f;
			while(val) {
				if (val & 1) {
					prct = &vkZxMap[row * 10 + pos].rect;
					pnt.fillRect(prct->x, prct->y, prct->dx, prct->dy, qRgb(0,200,255));
				}
				val >>= 1;
				pos += (i & 4) ? 1 : -1;
			}
		}
	}
	pnt.drawPixmap(0, 0, pxm);
	pnt.end();
}

void keyWindow::mousePressEvent(QMouseEvent* ev) {
	if (!kb) return;

	const xRect* prct = NULL;
	const xRect* rctp;
	int idx = 0;
	QPoint ipos = imgPos(QPoint(ev->xEventX, ev->xEventY));
	int x = ipos.x();
	int y = ipos.y();
	int dx,dy;
	while(vkZxMap[idx].ch != 0) {
		rctp = &vkZxMap[idx].rect;
		dx = x - rctp->x;
		dy = y - rctp->y;
		if ((dx >= 0) && (dy >= 0) && (dx < rctp->dx) && (dy < rctp->dy)) {
			prct = rctp;
			xent.zxKey[0] = vkZxMap[idx].ch;
		} else {
			rctp = &vkZxMap[idx].rect2;
			dx = x - rctp->x;
			dy = y - rctp->y;
			if ((dx >= 0) && (dy >= 0) && (dx < rctp->dx) && (dy < rctp->dy)) {
				prct = rctp;
				xent.zxKey[0] = vkZxMap[idx].ch;
			}
		}
		idx++;
	}
	if (prct == NULL) {			// no hit: nothing to release on mouse up either
		xent.zxKey[0] = 0;
		// the right button holds a key down, so the menu is on a miss
		if (ev->button() == Qt::RightButton)
			showMenu(mapToGlobal(QPoint(ev->xEventX, ev->xEventY)));
		return;
	}
	xent.zxKey[1] = 0;
	switch(ev->button()) {
		case Qt::LeftButton:
			kbd_press(kb, &xent);
			update();
			break;
		case Qt::RightButton:
			kbdTrigger(kb, &xent);
			update();
			break;
		case Qt::MiddleButton:
			kbdReleaseAll(kb);
			xent.zxKey[0] = 0;
			update();
			break;
		default:
			break;
	}
}

void keyWindow::mouseReleaseEvent(QMouseEvent* ev) {
	if (!kb) return;
	if (ev->button() == Qt::LeftButton) {
		kbd_release(kb, &xent);
	}
	update();
}

void keyWindow::keyPressEvent(QKeyEvent* ev) {
	if (ev->key() == Qt::Key_Escape) {
		hide();
	} else {
		emit s_key_press(ev);
	}
}

void keyWindow::keyReleaseEvent(QKeyEvent* ev) {
	if (ev->key() != Qt::Key_Escape)
		emit s_key_release(ev);
}

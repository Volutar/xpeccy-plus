// virtual keyboard

#include "vkeyboard.h"
#include "xcore/xcore.h"
#include "xgui/xgui.h"

#include <algorithm>

#include <QActionGroup>
#include <QApplication>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QMenu>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#if defined(_WIN32)
#include <windows.h>
#ifndef SPI_GETFONTSMOOTHINGORIENTATION
#define SPI_GETFONTSMOOTHINGORIENTATION 0x2012
#endif
#ifndef FE_FONTSMOOTHINGORIENTATIONRGB
#define FE_FONTSMOOTHINGORIENTATIONRGB 1
#endif
#endif

// the skins there are, as named in the config and in res/vkbd/zx-<name>.json
static const struct {
	const char* name;
	const char* title;
} vkSkins[] = {{"black", "48K (Classic)"}, {"white", "48K (White)"}, {"128", "128K (Classic)"}, {"128-white", "128K (White)"},
	{"next", "Next (Black)"}, {"next-white", "Next (White)"}, {"next-magenta", "Next (Magenta)"}};

// a skin's tone: white, magenta (the Next's), or black for the classic ones
static std::string skinTone(const std::string& name) {
	if (name.find("white") != std::string::npos) return "white";
	return (name.find("magenta") != std::string::npos) ? "magenta" : "black";
}

// where a layout of the user's own is looked for
static QString ownSkinDir() {
	return QString::fromLocal8Bit(conf.path.confDir.c_str()) + "/vkbd";
}

// whether a skin has a layout to draw: built in, or the user's own
static bool skinThere(const QString& name) {
	return QFile::exists(":/res/vkbd/zx-" + name + ".json") || QFile::exists(ownSkinDir() + "/zx-" + name + ".json");
}

// The skin to show: the one picked by hand, or by the machine - the Next's on the
// Next, the 128's on a Sinclair 128K, +2, +2A or +3, the 48K's on the rest, in the
// tone picked last (magenta only the Next has: black elsewhere)
static QString wantedSkin() {
	if (!conf.keywin.bymac) return QString::fromStdString(conf.keywin.skin);
	int hw = (conf.zx && conf.zx->hw) ? conf.zx->hw->id : HW_NULL;
	bool is128 = (hw == HW_ZX128) || (hw == HW_PLUS2A) || (hw == HW_PLUS3);
	bool white = (conf.keywin.plain == "white");
	if (hw == HW_ZXNEXT) return white ? QString("next-white") : (conf.keywin.plain == "magenta") ? QString("next-magenta") : QString("next");
	if (is128) return white ? QString("128-white") : QString("128");
	return white ? QString("white") : QString("black");
}

// whether every one of the keys is down
static bool allIn(const QByteArray& keys, const QByteArray& down) {
	for (char c : keys) {
		if (!down.contains(c)) return false;
	}
	return !keys.isEmpty();
}

// a key's string with its keys in one order, so CS+0 and 0+CS compare equal
static QByteArray zxSorted(QByteArray zx) {
	std::sort(zx.begin(), zx.end());
	return zx;
}

// A PC key's keys are in the matrix from the moment it is pressed: a combination's
// shift at once, its key after (kbd_seq_press). When its shift is gone it was let go of
// some other way - a release of all keys - and the key is not held any more.
static bool hostHolding(const QByteArray& zx, const QByteArray& down) {
	QByteArray shifts;
	for (char c : zx) {
		if ((c == 'C') || (c == 'S')) shifts.append(c);
	}
	return allIn(shifts.isEmpty() ? zx : shifts, down);
}

static bool isNext() {
	return conf.zx && conf.zx->hw && (conf.zx->hw->id == HW_ZXNEXT);
}

// how long a let-go PC key may still claim its keys as a combination's: they go up
// within a scan or two of the machine's time
#define VK_HOST_FADE_MS	200
// how long the matrix is watched closely after a PC key, and how often
#define VK_FOLLOW_MS	100
#define VK_FOLLOW_STEP	2
// the light of a key held as part of a combination lit as a key of its own, and of
// the key under the pointer
#define VK_DIM		0.45

// Up to this size subpixel text is worth it; above it the triads are too small
// against the letters to see, and only fringe the colours
#define VK_LCD_MAX	1.5

// Subpixel text needs a screen with its subpixels in RGB stripes, and only
// Windows says what the screen has: ClearType on, RGB order. A screen turned on
// its side has its stripes running the other way.
static bool hostLcd(QWidget* w) {
#if defined(_WIN32)
	BOOL on = FALSE;
	UINT type = 0;
	UINT order = 0;
	if (!SystemParametersInfo(SPI_GETFONTSMOOTHING, 0, &on, 0) || !on) return false;
	if (!SystemParametersInfo(SPI_GETFONTSMOOTHINGTYPE, 0, &type, 0) || (type != FE_FONTSMOOTHINGCLEARTYPE)) return false;
	if (!SystemParametersInfo(SPI_GETFONTSMOOTHINGORIENTATION, 0, &order, 0) || (order != FE_FONTSMOOTHINGORIENTATIONRGB)) return false;
	// QWidget::screen() is Qt 5.14 and later
	QScreen* scr = w->windowHandle() ? w->windowHandle()->screen() : QGuiApplication::primaryScreen();
	return !scr || (scr->geometry().width() >= scr->geometry().height());
#else
	(void)w;
	return false;
#endif
}

keyWindow::keyWindow(QWidget* p):QDialog(p) {
	kb = NULL;
	dock = 0;
	hover = -1;
	held = -1;
	picScale = 0;
	setMouseTracking(true);		// for the key under the pointer
	memset(&xent, 0, sizeof(xent));
	xent.key = ENDKEY;
	drawTimer.setSingleShot(true);
	drawTimer.setInterval(60);
	connect(&drawTimer, SIGNAL(timeout()), this, SLOT(redraw()));
	hoverTimer.setInterval(40);
	connect(&hoverTimer, SIGNAL(timeout()), this, SLOT(pollHover()));
	followClock.start();
	followEnd = 0;
	followTimer.setTimerType(Qt::PreciseTimer);
	followTimer.setInterval(VK_FOLLOW_STEP);
	connect(&followTimer, SIGNAL(timeout()), this, SLOT(follow()));
	connect(&watcher, SIGNAL(fileChanged(QString)), this, SLOT(loadSkin()));
	connect(&watcher, SIGNAL(directoryChanged(QString)), this, SLOT(loadSkin()));
	loadSkin();
	setModal(false);
	setWindowModality(Qt::NonModal);
	setSizeGripEnabled(true);
	setMinimumSize(qRound(cnv.width() / 2), qRound(cnv.height() / 2));
	setZoom(storedZoom());
	setWindowIcon(QIcon(":/images/vkeys.png"));
	setWindowTitle("Virtual Keyboard - ZX Spectrum");
	if (conf.keywin.dock)
		setDock(true);
}

// The skin from the config. A file of the same name in the config folder's vkbd/
// is taken instead of the built-in one, and taken again whenever it is saved, so
// a layout can be tuned while the window shows it.
void keyWindow::loadSkin() {
	QString name = wantedSkin();
	skinWant = name;
	bool known = false;
	for (const auto& sk : vkSkins)
		known |= (name == sk.name);
	// one that is not there (a 128 with no layout yet): the plain one, or the first
	if (!known || !skinThere(name)) {
		name = QString::fromStdString(conf.keywin.plain);
		if (((name != "black") && (name != "white")) || !skinThere(name)) name = vkSkins[0].name;
	}
	skinShown = name;
	QString dir = ownSkinDir();
	QString own = dir + "/zx-" + name + ".json";
	if (!watcher.files().isEmpty()) watcher.removePaths(watcher.files());
	if (!watcher.directories().isEmpty()) watcher.removePaths(watcher.directories());
	if (QFile::exists(dir)) watcher.addPath(dir);	// a file put there later
	VkSkin sk;
	QString err;
	QFile file(own);
	if (file.exists()) {
		watcher.addPath(own);
		if (!file.open(QIODevice::ReadOnly) || !sk.load(file.readAll(), &err)) {
			qWarning("vkbd: %s: %s", own.toLocal8Bit().constData(), err.toLocal8Bit().constData());
			sk = VkSkin();
		}
	}
	if (sk.isNull()) {
		QFile res(":/res/vkbd/zx-" + name + ".json");
		if (res.open(QIODevice::ReadOnly) && !sk.load(res.readAll(), &err))
			qWarning("vkbd: built-in %s: %s", name.toLocal8Bit().constData(), err.toLocal8Bit().constData());
	}
	if (sk.isNull() && skin.isNull()) return;	// nothing to show at all
	if (!sk.isNull()) skin = sk;
	QSizeF old = cnv;
	cnv = skin.canvas();
	hover = -1;
	held = -1;		// heldZx stays: the button's release lets go of it
	const QVector<VkKey>& keys = skin.keys();
	latched = QVector<bool>(keys.size(), false);
	firstCopy = QVector<int>(keys.size(), -1);
	lastCopy = QVector<int>(keys.size(), -1);
	for (int n = 0; n < keys.size(); n++) {
		if (!keys[n].ch) continue;
		int f = n;
		int l = n;
		for (int m = 0; m < keys.size(); m++) {
			if (keys[m].ch != keys[n].ch) continue;
			if (keys[m].box.left() < keys[f].box.left()) f = m;
			if (keys[m].box.left() > keys[l].box.left()) l = m;
		}
		firstCopy[n] = f;
		lastCopy[n] = l;
	}
	picScale = 0;
	pic = QPixmap();
	litPic = QPixmap();
	// a layout of another shape: the window takes it, as wide as it was
	if (!old.isEmpty() && (old != cnv)) {
		setMinimumSize(qRound(cnv.width() / 2), qRound(cnv.height() / 2));
#if defined(__APPLE__)
		if (isVisible()) vkbd_set_aspect(this, qRound(cnv.width()), qRound(cnv.height()));
#endif
		if (dock) {
			snap();
		} else {
			setZoom(storedZoom());
		}
	}
	update();
}

// window pixels per canvas unit. The picture keeps its shape, so a window of
// another aspect gets an empty strip beside it
double keyWindow::scale() {
	if (cnv.isEmpty()) return 0;
	double sx = width() / cnv.width();
	double sy = height() / cnv.height();
	return (sx < sy) ? sx : sy;
}

// the same in the screen's own pixels: what the picture is drawn at
double keyWindow::devScale() {
	double dpr = devicePixelRatioF();
	return qRound(cnv.width() * scale() * dpr) / cnv.width();
}

// The picture's top left corner, on a whole device pixel, so it is put down 1:1
QPointF keyWindow::picOrigin() {
	double dpr = devicePixelRatioF();
	double sd = devScale();
	int x = (int)((width() * dpr - qRound(cnv.width() * sd)) / 2);
	int y = (int)((height() * dpr - qRound(cnv.height() * sd)) / 2);
	return QPointF(x / dpr, y / dpr);
}

// the picture's shape, from one side to the other
int keyWindow::higFor(int wid) {
	return qRound(wid * cnv.height() / cnv.width());
}

int keyWindow::widFor(int hig) {
	return qRound(hig * cnv.width() / cnv.height());
}

// the size the window was left at last time
double keyWindow::storedZoom() {
	return (conf.keywin.width > 0) ? (conf.keywin.width / cnv.width()) : 1.0;
}

// a point of the window in canvas units
QPointF keyWindow::imgPos(QPoint pos) {
	double sc = devScale() / devicePixelRatioF();
	if (sc <= 0.0) return QPointF(-1, -1);
	QPointF org = picOrigin();
	return QPointF((pos.x() - org.x()) / sc, (pos.y() - org.y()) / sc);
}

void keyWindow::setZoom(double z) {
	resize(qRound(cnv.width() * z), qRound(cnv.height() * z));
}

// the picture at the size the window has now
void keyWindow::redraw() {
	double sd = devScale();
	if (sd <= 0) return;
	bool lcd = (sd <= VK_LCD_MAX + 1e-6) && hostLcd(this);
	QImage img = skin.render(sd, lcd);
	pic = QPixmap::fromImage(img);
	pic.setDevicePixelRatio(devicePixelRatioF());
	litPic = QPixmap();
	if (skin.padLight()) {
		litPic = QPixmap::fromImage(skin.render(sd, lcd, true));
		litPic.setDevicePixelRatio(devicePixelRatioF());
	}
	picScale = sd;
	update();
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
	placeUndocked();
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
	if (dock) {
		snap();
	} else if (isVisible()) {
		conf.keywin.xpos = pos().x();
		conf.keywin.ypos = pos().y();
		conf.keywin.haspos = 1;
	}
}

// Undocked, the window comes back where it was left: a dialog shown again is
// put in the middle of its parent otherwise. A position on a screen that is not
// there any more is left to the system; one half off a screen is pulled onto it.
void keyWindow::placeUndocked() {
	if (dock || !conf.keywin.haspos) return;
	QPoint at(conf.keywin.xpos, conf.keywin.ypos);
	QScreen* scr = QGuiApplication::screenAt(at + QPoint(40, 10));
	if (!scr) return;
	QRect avail = scr->availableGeometry();
	QRect fg = frameGeometry();
	at.setX(std::max(avail.left(), std::min(at.x(), avail.right() + 1 - fg.width())));
	at.setY(std::max(avail.top(), std::min(at.y(), avail.bottom() + 1 - fg.height())));
	move(at);
}

void keyWindow::showMenu(QPoint gpos) {
	QMenu mnu(this);
	fillMenu(&mnu);
	mnu.exec(gpos);
}

// The keyboard's settings, added to a menu: its own on a right click between the
// rows, and the Virtual Keyboard submenus of the main window, filled as they open
void keyWindow::fillMenu(QMenu* mnu) {
	QAction* act;
	// the main window's Show item: ticked while the keyboard is up
	foreach(QAction* a, mnu->actions()) {
		if (a->data().toString() == "keywin.show") a->setChecked(isVisible());
	}
	const double zval[] = {1.0, 1.5, 2.0, 3.0};
	for (int i = 0; i < 4; i++) {
		double z = zval[i];
		act = mnu->addAction(QString("Zoom x%1").arg(z), this, [this, z]() {setZoom(z);});
		act->setDisabled(dock);
	}
	mnu->addSeparator();
	QMenu* skm = mnu->addMenu("Skin");
	act = skm->addAction("By machine", this, [this]() {
		conf.keywin.bymac = !conf.keywin.bymac;
		loadSkin();
	});
	act->setCheckable(true);
	act->setChecked(conf.keywin.bymac);
	skm->addSeparator();
	QActionGroup* skg = new QActionGroup(skm);
	for (const auto& sk : vkSkins) {
		if (!skinThere(sk.name)) continue;
		std::string name = sk.name;
		act = skm->addAction(sk.title, this, [this, name]() {
			// a skin picked by hand stays, whatever the machine
			conf.keywin.skin = name;
			conf.keywin.plain = skinTone(name);
			conf.keywin.bymac = 0;
			loadSkin();
		});
		act->setCheckable(true);
		act->setChecked(skinShown == sk.name);
		act->setActionGroup(skg);
	}
	act = mnu->addAction("Dock under the emulator", this, [this]() {setDock(!dock);});
	act->setCheckable(true);
	act->setChecked(dock);
	act = mnu->addAction("Highlight the key under the pointer", this, [this]() {
		conf.keywin.hover = !conf.keywin.hover;
		hover = -1;		// the next move finds it again
		update();
	});
	act->setCheckable(true);
	act->setChecked(conf.keywin.hover);
	mnu->addSeparator();
	mnu->addAction("Release all keys", this, [this]() {
		if (kb) x_kbd_release_all(kb);
		latched.fill(false);
		update();
	});
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
	// drawing the picture anew on every step of a drag would hold the drag up:
	// the old one is stretched meanwhile, the exact one comes when it settles
	if (!pic.isNull())
		drawTimer.start();
}

void keyWindow::showEvent(QShowEvent* ev) {
	QDialog::showEvent(ev);
#if defined(__APPLE__)
	// the shape has to be told to the window itself, and a new one is made
	// whenever the flags change, so it is set every time it comes up
	vkbd_set_aspect(this, qRound(cnv.width()), qRound(cnv.height()));
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
	// The shape fixed above leaves the mouse inside the window when a corner is let
	// go of, and Qt passes the window no mouse moves after that until the next
	// click: the key under the pointer is found by where the pointer is meanwhile
	if (wmsg->message == WM_EXITSIZEMOVE) {
		// A drag by the size grip began as a press inside the window, and the sizing
		// loop ate its release: Qt still sees the button down and sends every move to
		// the grip until the next click. Give Qt the release it missed
		if (QGuiApplication::mouseButtons() & Qt::LeftButton) {
			POINT cp;
			GetCursorPos(&cp);
			ScreenToClient(wmsg->hwnd, &cp);
			PostMessage(wmsg->hwnd, WM_LBUTTONUP, 0, MAKELPARAM(cp.x, cp.y));
		}
		pollHover();
		hoverTimer.start();
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
	if (isVisible()) {
		hide();
	} else {
		placeUndocked();
		show();
	}
}

// Comes on a switch of machine as well as on keys: a machine of another kind
// may want another skin
void keyWindow::upd(Keyboard* k) {
	kb = k;
	if (wantedSkin() != skinWant)
		loadSkin();
	if (isVisible())
		repaint();
}

// What the lights are read from (keysDown() and the Next's own keys), as bytes to
// compare: a combination from the PC keyboard lets its shift go after its key, and
// autostart or a gamepad press keys with no event here
QByteArray keyWindow::matrixSig() {
	QByteArray sig;
	if (!kb) return sig;
	sig.append((const char*)kb->map, sizeof(kb->map));
	sig.append((const char*)kb->nxKeys, sizeof(kb->nxKeys));
	sig.append((char)kb->nxCaps);
	sig.append((char)kb->nxSym);
	return sig;
}

// From the main window's 50 Hz tick
void keyWindow::watch(Keyboard* k) {
	if ((k != kb) || !isVisible()) return;
	int had = host.size();
	hostTidy(keysDown());
	if ((matrixSig() != shownSig) || (host.size() != had))
		update();
}

// From the main window, as a PC key goes to the machine - before its repaint. A
// combination's shift goes down at once and its key a little later, up to 22 ms on the
// Evo, so the matrix is watched closely for a while instead of at the 50 Hz tick
void keyWindow::hostKey(QByteArray zx, bool down, bool right) {
	if (!isVisible()) {		// nothing tidies them while hidden; a key held as it opens lights as pressed
		host.clear();
		return;
	}
	if (zx.isEmpty()) return;
	zx = zxSorted(zx);
	hostTidy(keysDown());
	if (down) {
		host.append({zx, true, right, 0});
	} else {
		for (HostKey& h : host) {
			if (h.down && (h.zx == zx) && (h.right == right)) {
				h.down = false;
				h.upAt = followClock.elapsed();
				break;
			}
		}
	}
	followEnd = followClock.elapsed() + VK_FOLLOW_MS;
	if (!followTimer.isActive())
		followTimer.start();
}

void keyWindow::follow() {
	if (!isVisible() || (followClock.elapsed() > followEnd)) {
		followTimer.stop();
		if (!isVisible()) return;
	}
	int had = host.size();
	hostTidy(keysDown());
	if ((matrixSig() != shownSig) || (host.size() != had))
		update();
}

// PC keys let go of some other way go, and let-go ones once their keys are up
void keyWindow::hostTidy(const QByteArray& down) {
	qint64 now = followClock.elapsed();
	for (int i = host.size() - 1; i >= 0; i--) {
		const HostKey& h = host[i];
		bool gone = h.down ? !hostHolding(h.zx, down)
			: ((now - h.upAt > VK_HOST_FADE_MS) || !std::any_of(h.zx.begin(), h.zx.end(), [&](char c) {return down.contains(c);}));
		if (gone) host.removeAt(i);
	}
}

void keyWindow::rall(Keyboard* k) {
	if (!isVisible()) {
		x_kbd_release_all(k);
	}
}

// The Next's extended keys (membrane.vhd): what the 48K has as a combination - EDIT,
// the arrows, ; and the rest - is a key of its own on the Next, at columns 5 and 6 of
// its 8x7 matrix. Pressed there, the machine makes the combination itself, unless NR 68
// b4 says not, and NR B0/B1 see it. 0 for a key that is none of them, or another machine.
static unsigned char* nextExtKey(Keyboard* kb, const QByteArray& zx, int* bit) {
	static const struct {const char* zx; int row, col;} tab[] = {
		{"CS", 0, 5}, {"C7", 0, 6}, {"C2", 1, 5}, {"C9", 1, 6}, {"C3", 2, 5}, {"C4", 2, 6},
		{"C ", 3, 5}, {"C1", 3, 6}, {"So", 4, 5}, {"Sp", 4, 6}, {"Sn", 5, 5}, {"Sm", 5, 6},
		{"C0", 6, 5}, {"C8", 6, 6}, {"C5", 7, 5}, {"C6", 7, 6}
	};
	if (!kb || !conf.zx || !conf.zx->hw || (conf.zx->hw->id != HW_ZXNEXT) || (zx.size() != 2)) return NULL;
	// the shift first, CAPS SHIFT before SYMBOL SHIFT
	int s = zx.contains('C') ? zx.indexOf('C') : zx.indexOf('S');
	if (s < 0) return NULL;
	QByteArray k = QByteArray(1, zx[s]) + zx[1 - s];
	for (const auto& e : tab) {
		if (k == e.zx) {
			*bit = 1 << e.col;
			return &kb->nxKeys[e.row];
		}
	}
	return NULL;
}

// the Spectrum keys down in the machine's matrix now, as keyEntry::zxKey names them
QByteArray keyWindow::keysDown() {
	QByteArray res;
	if (!kb) return res;
	// the Next takes the host's keys on its ps/2 matrix, a row per address line where
	// the map runs from A15 down (CAPS and SYM SHIFT counted apart), and the membrane's
	// on the map
	bool next = conf.zx && conf.zx->hw && (conf.zx->hw->id == HW_ZXNEXT);
	for (int i = 0; i < 8; i++) {
		int pos = (i & 4) ? 0 : 9;
		int row = (i & 4) ? (i & 3) : (~i & 3);
		unsigned char val = ~kb->map[i] & 0x1f;
		if (next) {
			val |= kb->nxKeys[7 - i] & 0x1f;
			if ((i == 7) && kb->nxCaps) val |= 1;
			if ((i == 0) && kb->nxSym) val |= 2;
		}
		while (val) {
			if (val & 1) res.append((char)VkSkin::zxKeyAt(row, pos));
			val >>= 1;
			pos += (i & 4) ? 1 : -1;
		}
	}
	return res;
}

// How bright each key is: 1 pressed, VK_DIM held as part of a combination that is lit as
// a key of its own, 0 up.
// - A key that is one Spectrum key is lit by the matrix, wherever the press came from;
//   of a key the skin has twice (the shifts), the copy clicked, else the one on the
//   side of the PC key that holds it, else the left one.
// - A combination - EDIT is CAPS SHIFT and 1 - is lit while a PC key that is that
//   combination is held, or this window holds it: the matrix cannot tell it from its
//   keys pressed one by one, which light fully. Its keys light dimly then, unless one is
//   pressed by itself too. A skin with no key for it lights its keys fully, as pressed.
// - The Next's own keys light from wherever they were pressed.
QVector<double> keyWindow::lights(const QByteArray& down) {
	const QVector<VkKey>& keys = skin.keys();
	QVector<double> lv(keys.size(), 0);
	bool next = isNext();
	QByteArray parts;	// keys in the matrix for a combination lit here
	QByteArray alone;	// keys pressed by themselves
	QByteArray clicked;	// one-key keys this window holds: that copy lights
	QByteArray byLeft;	// one-key keys a PC key holds, on its left side or the right
	QByteArray byRight;
	for (const HostKey& h : host) {		// the Next too: its own keymap's shifts
		if (!h.down || (h.zx.size() != 1)) continue;
		alone.append(h.zx);
		(h.right ? byRight : byLeft).append(h.zx);
	}
	for (int n = 0; n < keys.size(); n++) {
		const VkKey& key = keys[n];
		bool mine = (n == held) || ((n < latched.size()) && latched[n]);
		int bit;
		if (unsigned char* ext = nextExtKey(kb, key.zx, &bit)) {
			lv[n] = (*ext & bit) ? 1 : 0;
		} else if (key.ch) {
			if (mine && down.contains((char)key.ch)) {	// a latch let go of elsewhere is none
				alone.append((char)key.ch);
				clicked.append((char)key.ch);
			}
		} else if (!key.zx.isEmpty()) {
			bool on = (n == held) || (mine && allIn(key.zx, down));
			bool claim = on;
			if (!next) {
				QByteArray zx = zxSorted(key.zx);
				for (const HostKey& h : host) {
					if (h.zx != zx) continue;
					claim = true;			// a let-go one too, while its keys go up
					on |= h.down;
				}
			}
			if (on) lv[n] = 1;
			if (claim) parts.append(key.zx);
		}
	}
	for (int n = 0; n < keys.size(); n++) {
		char ch = (char)keys[n].ch;
		if (!ch || !down.contains(ch)) continue;
		bool mine = (n == held) || ((n < latched.size()) && latched[n]);
		double level = (parts.contains(ch) && !alone.contains(ch)) ? VK_DIM : 1;
		if (clicked.contains(ch)) {
			if (mine) lv[n] = level;
			continue;
		}
		// the copy on the PC key's side; one held some other way, the left one
		bool r = byRight.contains(ch);
		if ((r && (lastCopy.value(n, n) == n)) || ((!r || byLeft.contains(ch)) && (firstCopy.value(n, n) == n)))
			lv[n] = level;
	}
	return lv;
}

// a key's string pressed or let go of all at once, as one entry
void keyWindow::pressKeys(const QByteArray& zx, bool down) {
	if (!kb || zx.isEmpty()) return;
	int bit;
	if (unsigned char* row = nextExtKey(kb, zx, &bit)) {
		emu_lock();
		if (down) *row |= bit;
		else *row &= ~bit;
		emu_unlock();
		return;
	}
	memset(xent.zxKey, 0, sizeof(xent.zxKey));
	memcpy(xent.zxKey, zx.constData(), std::min((int)zx.size(), KEYSEQ_MAXLEN - 1));
	emu_lock();		// see x_key()
	if (down) {
		kbd_press(kb, &xent);
	} else {
		kbd_release(kb, &xent);
	}
	emu_unlock();
}

void keyWindow::paintEvent(QPaintEvent*) {
	QPainter pnt;
	double sd = devScale();
	// the first picture is drawn at once; a stale one is stretched until the
	// timer draws the exact one (a drag, or the window moved to another screen)
	if (pic.isNull() && (sd > 0))
		redraw();
	else if ((sd != picScale) && !drawTimer.isActive())
		drawTimer.start();
	const QVector<VkKey>& keys = skin.keys();
	pnt.begin(this);
	// The picture is drawn to sit on a ground of its own: the keys are opaque and
	// only a thin contour around each one is clear, so what shows through it is
	// a contour light - under a light theme the window colour would outline
	// every key all the time
	pnt.fillRect(rect(), skin.ground());
	// The picture drawn for this size is in device pixels, and so are its
	// contours - the lit keys are put there in the same pixels. A stale one being
	// stretched takes them in canvas units.
	bool exact = (sd == picScale);
	double dpr = devicePixelRatioF();
	pnt.translate(picOrigin());
	if (exact) {
		pnt.scale(1 / dpr, 1 / dpr);
	} else {
		pnt.scale(sd / dpr, sd / dpr);
	}
	// The signature first: a change between the two is caught by the next watch()
	shownSig = matrixSig();
	QByteArray down = keysDown();
	hostTidy(down);
	QVector<double> lv = lights(down);
	// the key under the pointer, only one that is up: the dim light
	if (conf.keywin.hover && (hover >= 0) && (hover < keys.size()) && (lv[hover] <= 0))
		lv[hover] = VK_DIM;
	// a contour light is the lit color under the picture; a pad light is the lit
	// picture over the plain one, on that key alone - the two differ only on the
	// caps
	bool pad = !litPic.isNull();
	for (int n = 0; (n < keys.size()) && !pad; n++) {
		if (lv[n] <= 0) continue;
		pnt.setOpacity(lv[n]);
		pnt.fillPath(exact ? skin.fillAt(n, sd) : keys[n].fill, skin.litColor());
	}
	pnt.setOpacity(1.0);
	QTransform onPic = pnt.transform();
	auto drawPic = [&](const QPixmap& pm) {
		if (exact) {
			pnt.resetTransform();		// 1:1, the picture is in device pixels already
			pnt.drawPixmap(picOrigin(), pm);
		} else if (!pm.isNull()) {
			pnt.setRenderHint(QPainter::SmoothPixmapTransform, true);
			pnt.drawPixmap(QRectF(0, 0, cnv.width(), cnv.height()), pm, QRectF(pm.rect()));
		}
	};
	drawPic(pic);
	for (int n = 0; (n < keys.size()) && pad; n++) {
		if (lv[n] <= 0) continue;
		pnt.save();
		pnt.setTransform(onPic);
		pnt.setClipPath(exact ? skin.fillAt(n, sd) : keys[n].fill);
		pnt.setOpacity(lv[n]);
		drawPic(litPic);
		pnt.restore();
	}
	pnt.end();
}

// the key a click at that point of the picture takes, -1 for none
int keyWindow::keyAt(QPointF ipos) {
	const QVector<VkKey>& keys = skin.keys();
	for (int n = 0; n < keys.size(); n++) {
		const VkKey& key = keys[n];
		if (key.zx.isEmpty()) continue;		// lettering, not a key
		if (key.hitPoly.isEmpty() ? key.hit.contains(ipos) : key.hitPoly.containsPoint(ipos, Qt::OddEvenFill))
			return n;
	}
	return -1;
}

// The key under the pointer by the pointer's place on the screen, while Qt passes
// no moves; none when the pointer is outside the window or the window is hidden
void keyWindow::pollHover() {
	if (!isVisible() || !conf.keywin.hover) {
		hoverTimer.stop();
		return;
	}
	QPoint at = mapFromGlobal(QCursor::pos());
	int n = rect().contains(at) ? keyAt(imgPos(at)) : -1;
	if (n != hover) {
		hover = n;
		update();
	}
}

void keyWindow::mouseMoveEvent(QMouseEvent* ev) {
	QDialog::mouseMoveEvent(ev);
	hoverTimer.stop();		// moves come again
	if (!conf.keywin.hover) return;
	int n = keyAt(imgPos(QPoint(ev->xEventX, ev->xEventY)));
	if (n != hover) {
		hover = n;
		update();
	}
}

void keyWindow::leaveEvent(QEvent* ev) {
	QDialog::leaveEvent(ev);
	if (hover >= 0) {
		hover = -1;
		update();
	}
}

void keyWindow::mousePressEvent(QMouseEvent* ev) {
	hoverTimer.stop();		// a click puts Qt's moves back
	if (!kb) return;

	int n = keyAt(imgPos(QPoint(ev->xEventX, ev->xEventY)));
	if (n < 0) {
		// the right button holds a key down, so the menu is on a miss
		if (ev->button() == Qt::RightButton)
			showMenu(mapToGlobal(QPoint(ev->xEventX, ev->xEventY)));
		return;
	}
	QByteArray zx = skin.keys()[n].zx;
	// a shift held for a click and let go would do nothing: the left button
	// latches it, as the right one latches any key
	bool shift = (zx == "C") || (zx == "S");
	switch(ev->button()) {
		case Qt::LeftButton:
			if (shift) {
				latchKey(n, zx);
				break;
			}
			pressKeys(heldZx, false);	// a press whose release was missed
			held = n;
			heldZx = zx;
			pressKeys(zx, true);
			update();
			break;
		case Qt::RightButton:
			latchKey(n, zx);
			break;
		case Qt::MiddleButton:
			x_kbd_release_all(kb);
			latched.fill(false);
			update();
			break;
		default:
			break;
	}
}

// Down for good, or up again if all its keys are down: a combination as one
// key, so a CAPS SHIFT down by itself stays down when EDIT is let go
void keyWindow::latchKey(int n, const QByteArray& zx) {
	int bit;
	unsigned char* ext = nextExtKey(kb, zx, &bit);
	bool all = ext ? ((*ext & bit) != 0) : allIn(zx, keysDown());
	pressKeys(zx, !all);
	if (n < latched.size()) latched[n] = !all;
	update();
}

void keyWindow::mouseReleaseEvent(QMouseEvent* ev) {
	if (!kb) return;
	if (ev->button() == Qt::LeftButton) {
		pressKeys(heldZx, false);
		heldZx.clear();
		held = -1;
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

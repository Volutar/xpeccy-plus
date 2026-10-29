// The toolbar and the status bar of the main window

#include <QApplication>
#include <QToolBar>
#include <QToolButton>
#include <QStatusBar>
#include <QMenu>
#include <QMenuBar>
#include <QDrag>
#include <QMimeData>
#include <QPainter>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QTimer>
#include <QSlider>
#include <QLabel>
#include <QWidgetAction>
#include <QFileInfo>
#include <QStyleOptionSlider>
#include <functional>

#include "emulwin.h"
#include "xcore/rzxrecord.h"
#include "filer.h"
#include "xcore/vidrec.h"
#include "xcore/pacing.h"
#include "xcore/vscalers.h"

#define TB_MIME "application/x-xpeccy-toolbar-item"

// a status light's picture, every pixel put through f
static QPixmap sb_map(const QString& path, std::function<QRgb(QRgb)> f) {
	QImage img = QImage(path).convertToFormat(QImage::Format_ARGB32);
	for (int y = 0; y < img.height(); y++)
		for (int x = 0; x < img.width(); x++)
			img.setPixel(x, y, f(img.pixel(x, y)));
	return QPixmap::fromImage(img);
}

// A status light in another state: the same picture tinted, so the set stays one.
// alpha < 1 is a slot with nothing in it.
static QPixmap sb_tint(const QString& path, QColor col, double alpha) {
	return sb_map(path, [col, alpha](QRgb p) {
		double l = std::min(1.0, qGray(p) / 160.0);
		return qRgba(int(col.red() * l), int(col.green() * l), int(col.blue() * l), int(qAlpha(p) * alpha));
	});
}

// the same picture, lit up: every channel times gain
static QPixmap sb_bright(const QString& path, double gain) {
	return sb_map(path, [gain](QRgb p) {
		return qRgba(std::min(255, int(qRed(p) * gain)), std::min(255, int(qGreen(p) * gain)),
			std::min(255, int(qBlue(p) * gain)), qAlpha(p));
	});
}

// the same picture, lit: its bright face goes toward white by k, the dark outline stays
static QPixmap sb_lit(const QString& path, double k) {
	return sb_map(path, [k](QRgb p) {
		if (qMax(qRed(p), qMax(qGreen(p), qBlue(p))) < 200) return p;
		return qRgba(int(qRed(p) + (255 - qRed(p)) * k), int(qGreen(p) + (255 - qGreen(p)) * k),
			int(qBlue(p) + (255 - qBlue(p)) * k), qAlpha(p));
	});
}

// struck through: a light for something out of the machine for now
static QPixmap sb_strike(QPixmap pix) {
	QPainter p(&pix);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(QPen(QColor(200, 70, 60), 2));
	p.drawLine(QPointF(1.5, pix.height() - 1.5), QPointF(pix.width() - 1.5, 1.5));
	return pix;
}

// the divMMC's card light, out of the 32 px card picture
static QPixmap sb_card(QColor col, double alpha = 1.0) {
	return sb_tint(":/images/sdcard.png", col, alpha).scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

#define TB_SEPARATOR "|"
#define TB_SPACE "~"		// takes what room is left, so what follows sits at the right edge

// what a toolbar holds until the user says otherwise
static const char* tbDefault = "key.load,key.save,|,menu.reset,menu.machine,|,key.tapewin,menu.disks,|,"
	"key.fullscreen,key.keywin,key.padwin,|,menu.debug,key.options,|,key.mute,~,key.rewind,key.slowmo,key.pause,key.ffwd,key.fast";

enum {TB_PLAIN = 0, TB_LIST, TB_SPLIT};	// a button; a list that opens on a click; a button with a list beside it

// The window is as wide as the picture, not as these bars: what does not fit
// the toolbar goes under its arrow, and the status bar is cut at the edge.
class xToolBar : public QToolBar {
	public:
		xToolBar(QWidget* p) : QToolBar(p) {setAcceptDrops(true);}
		std::function<void(const QPoint&, int)> onMenu;		// at a point of the screen, on an item or -1
		std::function<void(int, int)> onMove;			// an item dragged from one place to another
		std::function<void()> onRestyle;			// a style or a font: the buttons have new widths
		void watch();
		QSize sizeHint() const override {return QSize(0, QToolBar::sizeHint().height());}
		QSize minimumSizeHint() const override {return QSize(0, QToolBar::minimumSizeHint().height());}
	protected:
		bool eventFilter(QObject*, QEvent*) override;
		void changeEvent(QEvent* ev) override {
			QToolBar::changeEvent(ev);
			if (onRestyle && ((ev->type() == QEvent::StyleChange) || (ev->type() == QEvent::FontChange)))
				onRestyle();
		}
		void contextMenuEvent(QContextMenuEvent*) override;
		void dragEnterEvent(QDragEnterEvent*) override;
		void dragMoveEvent(QDragMoveEvent*) override;
		void dragLeaveEvent(QDragLeaveEvent*) override;
		void dropEvent(QDropEvent*) override;
		void paintEvent(QPaintEvent*) override;
	private:
		int indexOf(QObject*) const;
		int indexAt(const QPoint&) const;
		int dropAt(const QPoint&, int* x) const;
		QPoint pressAt;
		int pressIdx = -1;
		bool pressEaten = false;
		int dropX = -1;
};

class xStatusBar : public QStatusBar {
	public:
		xStatusBar(QWidget* p) : QStatusBar(p) {}
		QSize sizeHint() const override {return QSize(0, QStatusBar::sizeHint().height());}
		QSize minimumSizeHint() const override {return QSize(0, QStatusBar::minimumSizeHint().height());}
};

// the buttons are made by the bar, after every rebuild
void xToolBar::watch() {
	foreach(QAction* act, actions()) {
		QWidget* w = widgetForAction(act);
		if (w) w->installEventFilter(this);
	}
}

int xToolBar::indexOf(QObject* obj) const {
	QList<QAction*> acts = actions();
	for (int i = 0; i < acts.size(); i++)
		if (widgetForAction(acts[i]) == obj) return i;
	return -1;
}

int xToolBar::indexAt(const QPoint& pos) const {
	QList<QAction*> acts = actions();
	for (int i = 0; i < acts.size(); i++) {
		QWidget* w = widgetForAction(acts[i]);
		if (w && w->isVisible() && w->geometry().contains(pos)) return i;
	}
	return -1;
}

// the item a drop at pos goes before, and where the mark for it is drawn
int xToolBar::dropAt(const QPoint& pos, int* x) const {
	QList<QAction*> acts = actions();
	int last = -1;
	for (int i = 0; i < acts.size(); i++) {
		QWidget* w = widgetForAction(acts[i]);
		if (!w || !w->isVisible()) break;		// what is under the arrow goes after the rest
		QRect rc = w->geometry();
		if (pos.x() < rc.center().x()) {
			*x = rc.left();
			return i;
		}
		last = rc.right();
	}
	*x = last + 1;
	return acts.size();
}

// A press is a drag once it moves. A list button opens on the press, so the
// press is held back from it and the list opened on a release that did not move.
// A locked bar drags nothing, and its buttons take their presses as Qt gives them.
bool xToolBar::eventFilter(QObject* obj, QEvent* ev) {
	if (conf.win.tbLocked) return QToolBar::eventFilter(obj, ev);
	QToolButton* btn = qobject_cast<QToolButton*>(obj);
	bool list = btn && (btn->popupMode() == QToolButton::InstantPopup) && btn->menu();
	switch (ev->type()) {
		case QEvent::MouseButtonPress: {
			QMouseEvent* me = static_cast<QMouseEvent*>(ev);
			if (me->button() != Qt::LeftButton) break;
			pressAt = QPoint(me->xGlobalX, me->xGlobalY);
			pressIdx = indexOf(obj);
			pressEaten = list;
			if (list) return true;
			break;
		}
		case QEvent::MouseMove: {
			QMouseEvent* me = static_cast<QMouseEvent*>(ev);
			if ((pressIdx < 0) || !(me->buttons() & Qt::LeftButton)) break;
			if ((QPoint(me->xGlobalX, me->xGlobalY) - pressAt).manhattanLength() < QApplication::startDragDistance()) break;
			int from = pressIdx;
			pressIdx = -1;
			pressEaten = false;
			QWidget* w = static_cast<QWidget*>(obj);
			if (btn && btn->isDown()) {
				btn->setDown(false);
				emit btn->released();		// a held button would never see its release
			}
			QDrag* drag = new QDrag(this);
			QMimeData* mime = new QMimeData;
			mime->setData(TB_MIME, QByteArray::number(from));
			drag->setMimeData(mime);
			drag->setPixmap(w->grab());
			drag->setHotSpot(w->mapFromGlobal(pressAt));
			drag->exec(Qt::MoveAction);
			return true;
		}
		case QEvent::MouseButtonRelease: {
			bool eaten = pressEaten && (pressIdx >= 0);
			pressIdx = -1;
			pressEaten = false;
			if (eaten && list) {
				btn->showMenu();
				return true;
			}
			break;
		}
		default:
			break;
	}
	return QToolBar::eventFilter(obj, ev);
}

void xToolBar::contextMenuEvent(QContextMenuEvent* ev) {
	if (onMenu) onMenu(ev->globalPos(), indexAt(mapFromGlobal(ev->globalPos())));
	ev->accept();
}

void xToolBar::dragEnterEvent(QDragEnterEvent* ev) {
	if (ev->mimeData()->hasFormat(TB_MIME)) ev->acceptProposedAction();
}

void xToolBar::dragMoveEvent(QDragMoveEvent* ev) {
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
	QPoint pos = ev->position().toPoint();
#else
	QPoint pos = ev->pos();
#endif
	dropAt(pos, &dropX);
	update();
	ev->acceptProposedAction();
}

void xToolBar::dragLeaveEvent(QDragLeaveEvent*) {
	dropX = -1;
	update();
}

void xToolBar::dropEvent(QDropEvent* ev) {
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
	QPoint pos = ev->position().toPoint();
#else
	QPoint pos = ev->pos();
#endif
	int x;
	int to = dropAt(pos, &x);
	int from = ev->mimeData()->data(TB_MIME).toInt();
	dropX = -1;
	update();
	ev->acceptProposedAction();
	// not from here: the rebuild deletes the button the drag is still running from
	if (onMove) QTimer::singleShot(0, this, [this, from, to]() {onMove(from, to);});
}

void xToolBar::paintEvent(QPaintEvent* ev) {
	// A style sheet gives a bar no ground of its own, and both bars are seen over the
	// picture: fullscreen's, and the window's one when its arrow lays the rest out under it
	{
		QPainter pnt(this);
		pnt.fillRect(rect(), parentWidget()->palette().window());
	}
	QToolBar::paintEvent(ev);
	if (dropX < 0) return;
	QPainter pnt(this);
	pnt.fillRect(dropX - 1, 2, 2, height() - 4, palette().highlight());
}

// A volume is set by eye: a click puts the handle where it lands, and the press
// then drags it, instead of the slider stepping towards the click a page at a time.
class xVolSlider : public QSlider {
	public:
		xVolSlider(QWidget* p) : QSlider(Qt::Horizontal, p) {}
	protected:
		void mousePressEvent(QMouseEvent* ev) override {
			if (ev->button() == Qt::LeftButton) {
				QStyleOptionSlider opt;
				initStyleOption(&opt);
				QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
				QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
				int x = int(ev->xEventX) - groove.x() - handle.width() / 2;
				setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), x, groove.width() - handle.width(), opt.upsideDown));
			}
			QSlider::mousePressEvent(ev);
		}
};

// the slider and the figure beside it: in the button's list, and in the pop-up
// Alt+wheel brings up
class xVolBox : public QFrame {
	public:
		xVolBox(QWidget* p, Qt::WindowFlags f = Qt::WindowFlags()) : QFrame(p, f) {
			QHBoxLayout* lay = new QHBoxLayout(this);
			lay->setContentsMargins(8, 4, 8, 4);
			sld = new xVolSlider(this);
			sld->setRange(0, 100);
			sld->setPageStep(10);
			sld->setMinimumWidth(120);
			lab = new QLabel(this);
			lab->setMinimumWidth(lab->fontMetrics().boundingRect("100%").width());
			lab->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			lay->addWidget(sld);
			lay->addWidget(lab);
			connect(sld, &QSlider::valueChanged, this, [this](int v) {
				conf.snd.vol.master = v;
				figure();
			});
		}
		void load() {
			sld->setValue(conf.snd.vol.master);
			figure();		// the value may not have moved
		}
	private:
		QSlider* sld;
		QLabel* lab;
		void figure() {lab->setText(QString("%0%").arg(conf.snd.vol.master));}
};

// MainWin

void MainWin::initBars() {
	frame->setContextMenuPolicy(Qt::PreventContextMenu);	// its own one lists the bars
	auto makeBar = [this]() {
		xToolBar* tb = new xToolBar(frame);
		tb->setObjectName("toolbar");
		tb->setMovable(false);
		tb->setFloatable(false);
		tb->setFocusPolicy(Qt::NoFocus);
		tb->onMenu = [this](const QPoint& pos, int idx) {tbMenu(pos, idx);};
		tb->onMove = [this](int from, int to) {tbMove(from, to);};
		tb->onRestyle = [this]() {tbRestyled.start();};
		return tb;
	};
	// once the new style is on every widget, not at the first of them
	tbRestyled.setSingleShot(true);
	tbRestyled.setInterval(0);
	connect(&tbRestyled, &QTimer::timeout, this, &MainWin::tbAuto);
	toolBar = makeBar();
	frame->addToolBar(Qt::TopToolBarArea, toolBar);
	// fullscreen's own, out of the layout like fsBar, so the picture keeps its place
	if (fsBar) {
		xToolBar* tb = makeBar();
		fsTool = tb;
		fsTool->hide();
	}

	// the catalog: everything a button can be, in the groups the Add menu shows
	auto add = [this](const char* id, const char* grp, QAction* act, int kind, QMenu* list = nullptr) {
		tbCatalog.append({QString(id), QString(grp), act, kind, list});
	};
	// a speed mode: the menu item switches it, the toolbar's button is held as the key
	// is (see tbFill), and the click Qt makes of its release is not one more switch
	auto speed = [this](const char* name, int xcut, int mode, const char* icon) {
		QAction* act = new QAction(QIcon(QString(":/images/%0.png").arg(icon)), name, this);
		act->setCheckable(true);
		connect(act, &QAction::triggered, this, [this, mode]() {
			if (tbDown) return;
			xspeed_toggle(mode);
		});
		cutActs.append({act, QString(name), xcut});
		return act;
	};
	auto cut = [this](const char* name, int xcut, const char* icon) {return cutAct(name, xcut, icon);};
	add("key.load", "File", cut("Open...", XCUT_LOAD, "fileopen"), TB_PLAIN);
	// a button of its own: the menu bar took over the list's own action, which has no icon there
	// a click opens the list itself, Organize... is at its foot
	QAction* favAct = new QAction(QIcon(":/images/star.png"), "Favorites", this);
	add("menu.favorites", "File", favAct, TB_LIST, bookmarkMenu);
	add("key.reload", "File", cut("Reload", XCUT_RELOAD, "refresh"), TB_PLAIN);
	add("key.save", "File", cut("Save...", XCUT_SAVE, "save_all"), TB_PLAIN);
	add("key.fastsave", "File", cut("Save Changed Disks", XCUT_FASTSAVE, "floppy"), TB_PLAIN);
	add("key.scrshot", "Capture", cut("Screenshot", XCUT_SCRSHOT, "grp-screenshot"), TB_PLAIN);
	add("key.video.rec", "Capture", recAct, TB_PLAIN);
	add("key.write.wav", "Capture", wavAct, TB_PLAIN);
	add("key.rzx.rec", "Capture", rzxRecAct, TB_PLAIN);
	add("key.options", "File", cut("Options...", XCUT_OPTIONS, "other"), TB_PLAIN);
	add("menu.machine", "Machine", profileMenu->menuAction(), TB_LIST);
	add("menu.reset", "Machine", resMenu->menuAction(), TB_SPLIT);
	add("key.nmi", "Machine", cut("NMI", XCUT_NMI, "target"), TB_PLAIN);
	add("menu.turbo", "Machine", turboMenu->menuAction(), TB_LIST);
	pauseAct = cut("Pause", XCUT_PAUSE, "time-pause");
	pauseAct->setCheckable(true);
	add("key.pause", "Time", pauseAct, TB_PLAIN);
	// the menu item switches it whatever the key's hold mode: a press and release
	// of a key that has to be held would undo itself. The button is held, as speed's
	fastAct = cut("Fast Mode", XCUT_FAST, "time-fast");
	fastAct->setCheckable(true);
	fastAct->disconnect(SIGNAL(triggered(bool)));		// the key's press and release
	connect(fastAct, &QAction::triggered, this, [this]() {
		if (tbDown) return;
		if (!conf.emu.pause) fast_toggle();		// as the key: not while paused
		syncActions();
	});
	add("key.fast", "Time", fastAct, TB_PLAIN);
	ffAct = speed("Fast Forward", XCUT_FFWD, XTM_FFWD, "time-ffwd");
	add("key.ffwd", "Time", ffAct, TB_PLAIN);
	slowAct = speed("Slow Motion", XCUT_SLOWMO, XTM_SLOW, "time-slow");
	speedIcons();
	add("key.slowmo", "Time", slowAct, TB_PLAIN);
	// held like its key: the button's press and release drive it, see tbFill
	rewAct = new QAction(QIcon(":/images/time-rewind.png"), "Rewind", this);
	cutActs.append({rewAct, QString("Rewind"), XCUT_REWIND});
	cutById[XCUT_REWIND] = rewAct;
	add("key.rewind", "Time", rewAct, TB_PLAIN);
	// not the key's own action: a tape armed to start by itself counts as playing
	// there, so a click on a button that shows it stopped would stop it
	tapeAct = new QAction(QIcon(":/images/tape-play.png"), "Tape Play/Stop", this);
	connect(tapeAct, &QAction::triggered, this, [this]() {
		tapStateChanged(TW_STATE, conf.zx->tape->on ? TWS_STOP : TWS_PLAY);
	});
	cutActs.append({tapeAct, QString("Tape Play/Stop"), XCUT_TAPLAY});
	tapeAct->setCheckable(true);
	add("key.tape.play", "Media", tapeAct, TB_PLAIN);
	tapeRecAct = cut("Tape Record/Stop", XCUT_TAPREC, "tape-rec");
	tapeRecAct->setCheckable(true);
	add("key.tape.rec", "Media", tapeRecAct, TB_PLAIN);
	add("key.tapewin", "Media", cut("Tape Player", XCUT_TAPWIN, "tape"), TB_PLAIN);
	// a button of its own, not the menu's: that one is hidden on a machine with no drives
	diskAct = new QAction(QIcon(":/images/fdd_disk.png"), "Disk Manager", this);
	connect(diskAct, &QAction::triggered, this, [this]() {diskWin->showWindow();});
	add("menu.disks", "Media", diskAct, TB_SPLIT, dskMenu);
	add("menu.cartridge", "Media", cartMenu->menuAction(), TB_LIST);
	add("key.divmmc", "Media", divAct, TB_SPLIT, divMenu);	// the arrow: its cards
	add("menu.sdcard", "Media", sdcMenu->menuAction(), TB_LIST);
	add("menu.hdd", "Media", hddMenu->menuAction(), TB_LIST);
	add("key.rzxwin", "Capture", cut("RZX Player", XCUT_RZXWIN, "video"), TB_PLAIN);
	add("key.fullscreen", "View", fullAct, TB_PLAIN);
	add("key.ratio", "View", ratioAct, TB_PLAIN);
	add("menu.size", "View", sizeMenu->menuAction(), TB_LIST);
	add("menu.shaders", "View", shdMenu->menuAction(), TB_LIST);
	add("menu.palette", "View", palMenu->menuAction(), TB_LIST);
	add("key.keywin", "View", cut("Virtual Keyboard", XCUT_KEYBOARD, "vkeys"), TB_PLAIN);
	add("key.keyboard.grab", "Input", pckAct, TB_PLAIN);
	mouseAct = cut("Grab Mouse", XCUT_MOUSE, "mouse");
	mouseAct->setCheckable(true);
	add("key.mouse.grab", "Input", mouseAct, TB_PLAIN);
	add("menu.keymap", "Input", keyMenu->menuAction(), TB_LIST);
	add("key.padwin", "Input", cut("Gamepads", XCUT_PADWIN, "gamepad"), TB_SPLIT, padMenu);
	add("key.cheats", "Tools", cut("Cheats...", XCUT_CHEATS, "cheats"), TB_PLAIN);
	add("menu.debug", "Tools", dbgMenu->menuAction(), TB_SPLIT);
	add("key.scrwin", "Tools", cut("Screen", XCUT_SCRWIN, "rulers"), TB_PLAIN);
	add("key.sndwin", "Tools", cut("Sound Chips", XCUT_SNDWIN, "note"), TB_PLAIN);
	muteAct = cut("Mute", XCUT_MUTE, "speaker");
	muteAct->setCheckable(true);
	{
		QIcon icon(":/images/speaker.png");
		icon.addFile(":/images/speaker-mute.png", QSize(), QIcon::Normal, QIcon::On);
		muteAct->setIcon(icon);
	}
	add("key.mute", "Sound", muteAct, TB_SPLIT, volumeMenu());
	// shown, not used: it takes no focus and no clicks, so the machine keeps the keys
	volPop = new xVolBox(this, Qt::ToolTip | Qt::FramelessWindowHint);
	volPop->setFrameShape(QFrame::StyledPanel);
	volPop->setAttribute(Qt::WA_ShowWithoutActivating);
	volPop->setAttribute(Qt::WA_TransparentForMouseEvents);
	volTimer.setSingleShot(true);
	volTimer.setInterval(1500);
	connect(&volTimer, &QTimer::timeout, this, [this]() {
		volPop->hide();
		saveConfig();
	});
	foreach(const xTbItem& it, tbCatalog) {
		// a list opened from here has had no right-click menu fill it; one opened
		// inside a menu has, and refilling everything on each hover costs disk reads
		if (QMenu* list = it.list ? it.list : it.act->menu())
			connect(list, &QMenu::aboutToShow, this, [this]() {
				if (!QApplication::activePopupWidget()) fillUserMenu();
			});
	}

	tbList = QString::fromStdString((conf.win.tbItems == "*") ? std::string(tbDefault) : conf.win.tbItems).split(',', X_SkipEmptyParts);
	tbBuild();
	initMachineMenus();

	xStatusBar* sb = new xStatusBar(frame);
	statusBar = sb;
	sb->setSizeGripEnabled(false);
	sbMachine = new QLabel;
	sbClock = new QLabel;
	sbFps = new QLabel;
	// the name opens the Model menu; only the name, not the bar beside it
	sbMachine->setToolTip("Choose model");
	sbMachine->installEventFilter(this);
	sb->addWidget(sbMachine);
	sb->addWidget(new QWidget, 1);
	// tape and drives in one set: dim with nothing in, gray with something, green reading, red writing
	sbPix[SB_TAPE_EMPTY] = sb_tint(":/images/tapeGreen.png", QColor(170, 170, 170), 0.45);
	sbPix[SB_TAPE_IN] = sb_tint(":/images/tapeGreen.png", QColor(200, 200, 200), 1.0);
	sbPix[SB_TAPE_RD] = QPixmap(":/images/tapeGreen.png");
	sbPix[SB_TAPE_WR] = QPixmap(":/images/tapeRed.png");
	sbPix[SB_TAPE_RD_HI] = sb_lit(":/images/tapeGreen.png", 0.5);	// the other half of a blink
	sbPix[SB_TAPE_WR_HI] = sb_lit(":/images/tapeRed.png", 0.5);
	sbPix[SB_DISK_EMPTY] = sb_tint(":/images/diskGreen.png", QColor(170, 170, 170), 0.45);
	sbPix[SB_DISK_IN] = sb_tint(":/images/diskGreen.png", QColor(200, 200, 200), 1.0);
	sbPix[SB_DISK_RD] = QPixmap(":/images/diskGreen.png");
	sbPix[SB_DISK_WR] = QPixmap(":/images/diskRed.png");
	sbPix[SB_DISK_OUT] = sb_strike(sbPix[SB_DISK_EMPTY]);	// out under the divMMC
	// the divMMC's cards, one light for both: dim with none in, its own blue with one,
	// green read, red written
	sbPix[SB_CARD_EMPTY] = sb_card(QColor(170, 170, 170), 0.45);
	sbPix[SB_CARD_IN] = QPixmap(":/images/sdcard.png").scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	sbPix[SB_CARD_RD] = sb_card(QColor(60, 200, 60));
	sbPix[SB_CARD_WR] = sb_card(QColor(220, 60, 50));
	sbPix[SB_JOY_READ] = QPixmap(":/images/joystick.png");
	sbPix[SB_JOY_IDLE] = sb_tint(":/images/joystick.png", QColor(150, 150, 150), 0.3);
	sbPix[SB_MOUSE_READ] = sb_bright(":/images/mouse.png", 1.3);	// gray of its own, so lit up instead of coloured
	sbPix[SB_MOUSE_IDLE] = sb_tint(":/images/mouse.png", QColor(150, 150, 150), 0.3);
	// a light and what it is about, side by side; the pair takes the clicks
	auto pair = [this, sb](QLabel* text, QLabel* icon) {
		QWidget* box = new QWidget;
		QHBoxLayout* lay = new QHBoxLayout(box);
		lay->setContentsMargins(4, 0, 4, 0);
		lay->setSpacing(3);
		lay->addWidget(text);
		lay->addWidget(icon);
		box->installEventFilter(this);
		sb->addPermanentWidget(box);
		return box;
	};
	sbTape = new QLabel;
	sbTapeIcon = new QLabel;
	sbTapeBox = pair(sbTapeIcon, sbTape);		// the light first: the count is what follows it
	// then where the tape stands in the block it plays
	sbTapeBar = new QProgressBar;
	sbTapeBar->setTextVisible(false);
	sbTapeBar->setFixedSize(36, 8);
	sbTapeBox->layout()->addWidget(sbTapeBar);
	// the divMMC, while it is on: its cards at work
	sbCard = new QLabel("SD");
	sbCardIcon = new QLabel;
	sbCardBox = pair(sbCard, sbCardIcon);
	for (int i = 0; i < 4; i++) {
		sbDisk[i] = new QLabel(QString("%0:").arg(QChar('A' + i)));
		sbDiskIcon[i] = new QLabel;
		sbDiskBox[i] = pair(sbDisk[i], sbDiskIcon[i]);
	}
	// lit while the machine reads them, so the slot is kept while they are idle
	sbJoyIcon = new QLabel;
	sbJoyIcon->setToolTip("Kempston joystick");
	sbMouseIcon = new QLabel;
	sbMouseIcon->setToolTip("Kempston mouse");
	sbInputBox = pair(sbJoyIcon, sbMouseIcon);
	sbInputBox->layout()->setSpacing(6);		// two lights, not a light and its text
	// as wide as the longest they can say, or every change of clock or rate
	// would shift the lights to their left
	auto fixed = [sb](QLabel* lab, const char* longest) {
		lab->ensurePolished();
		lab->setMinimumWidth(lab->fontMetrics().horizontalAdvance(longest));
		lab->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		sb->addPermanentWidget(lab);
	};
	fixed(sbClock, " 88.888 MHz ");
	fixed(sbFps, " 8888.8 fps ");
	frame->setStatusBar(sb);
	updateStatus();
	cutTexts();
}

const MainWin::xTbItem* MainWin::tbFind(const QString& id) {
	foreach(const xTbItem& it, tbCatalog)
		if (it.id == id) return &it;
	return nullptr;
}

// the menus' own names, without what the menu bar adds to them
static QString tb_name(QAction* act) {
	QString name = act->text().section('\t', 0, 0);
	name.remove("...");
	return name;
}

// both bars hold the same actions, so a switch shows the same on either
void MainWin::tbBuild() {
	tbLetGo();		// the button held down goes with the old bar
	tbFill(toolBar);
	if (fsTool) tbFill(fsTool);
}

void MainWin::tbFill(QToolBar* bar) {
	bar->clear();
	foreach(const QString& id, tbList) {
		if (id == TB_SEPARATOR) {
			bar->addSeparator();
			continue;
		}
		if (id == TB_SPACE) {
			QWidget* space = new QWidget;
			space->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
			bar->addWidget(space);
			continue;
		}
		const xTbItem* it = tbFind(id);
		if (!it) {
			// an id this build does not know is kept, so another build still finds it
			bar->addSeparator()->setVisible(false);
			continue;
		}
		bar->addAction(it->act);
		QToolButton* btn = qobject_cast<QToolButton*>(bar->widgetForAction(it->act));
		if (!btn) continue;
		btn->setFocusPolicy(Qt::NoFocus);
		if (it->list)
			btn->setMenu(it->list);
		if (it->kind != TB_PLAIN)
			btn->setPopupMode((it->kind == TB_LIST) ? QToolButton::InstantPopup : QToolButton::MenuButtonPopup);
		if (it->act->menu())
			btn->setToolTip(tb_name(it->act));
		// the time buttons are their keys: pressed and released, so they work as the
		// key's Toggle, Hold or Tap or hold says
		int held = (it->act == rewAct) ? XCUT_REWIND : (it->act == fastAct) ? XCUT_FAST
			: (it->act == ffAct) ? XCUT_FFWD : (it->act == slowAct) ? XCUT_SLOWMO : 0;
		if (held) {
			connect(btn, &QToolButton::pressed, this, [this, held]() {
				tbDown = true;
				tbHeld = held;
				xkey_press(held, true);
				syncActions();
			});
			connect(btn, &QToolButton::released, this, [this, held]() {
				tbDown = false;
				if (tbHeld == held) tbLetGo();
			});
		}
	}
	bar->setAcceptDrops(!conf.win.tbLocked);
	static_cast<xToolBar*>(bar)->watch();
	// what there is to fit is known now; fullscreen's bar is as wide as the screen
	tbSize(bar, (bar == fsTool) ? frame->width() : drawW);
}

// The user's size, or Auto: 24 while every button fits the width at 24, else 16. The
// width the bar wants at 24 is taken from the bar itself - its own hint counts the
// buttons under the arrow too - with the size put back in the same call, so nothing is
// drawn in between. One width always gives one size: nothing flips at the edge.
void MainWin::tbSize(QToolBar* bar, int width) {
	int px = conf.win.tbIcons;
	if (!px) {
		QSize was = bar->iconSize();
		if (was.width() != 24) bar->setIconSize(QSize(24, 24));
		int need = bar->QToolBar::sizeHint().width();	// xToolBar's own says 0
		if (was.width() != 24) bar->setIconSize(was);
		px = (need <= width) ? 24 : 16;
	}
	if (bar->iconSize().width() != px)
		bar->setIconSize(QSize(px, px));
}

// Auto, looked at again: the window follows a change of the bar's height. The width
// comes from the picture alone, so updateWindow decides the same size and stops there.
void MainWin::tbAuto() {
	int was = toolBar->iconSize().width();
	tbSize(toolBar, drawW);
	if (fsTool && fsTool->isVisible()) tbSize(fsTool, frame->width());
	if ((toolBar->iconSize().width() != was) && !conf.vid.fullScreen)
		updateWindow();
}

// the list as the user left it, and the bar rebuilt from it
void MainWin::tbApply() {
	int was = toolBar->iconSize().width();
	conf.win.tbItems = tbList.join(",").toStdString();
	tbBuild();
	saveConfig();
	if ((toolBar->iconSize().width() != was) && !conf.vid.fullScreen)
		updateWindow();		// a button more or less can change Auto's size
}

void MainWin::tbMove(int from, int to) {
	if ((from < 0) || (from >= tbList.size())) return;
	if (to > from) to--;
	if (to == from) return;
	tbList.move(from, std::min(to, (int)tbList.size() - 1));
	tbApply();
}

void MainWin::tbMenu(const QPoint& pos, int idx) {
	QMenu menu(this);
	if ((idx >= 0) && (idx < tbList.size())) {
		const xTbItem* it = tbFind(tbList[idx]);
		QString name = it ? tb_name(it->act) : QString((tbList[idx] == TB_SPACE) ? "space" : "separator");
		menu.addAction(QIcon(":/images/cancel.png"), QString("Remove \"%0\"").arg(name), this, [this, idx]() {
			tbList.removeAt(idx);
			tbApply();
		});
		menu.addAction("Add Separator Here", this, [this, idx]() {
			tbList.insert(idx, TB_SEPARATOR);
			tbApply();
		});
		// the rest of the bar goes to the right edge
		menu.addAction("Add Space Here", this, [this, idx]() {
			tbList.insert(idx, TB_SPACE);
			tbApply();
		});
		menu.addSeparator();
	}
	// everything there is, a tick on what the bar holds; a new one goes after the item clicked on
	QMenu* add = menu.addMenu(QIcon(":/images/add.png"), "Buttons");
	QMap<QString, QMenu*> groups;
	foreach(const xTbItem& it, tbCatalog) {
		QMenu* grp = groups.value(it.group);
		if (!grp) {
			grp = add->addMenu(it.group);
			groups[it.group] = grp;
		}
		QAction* act = grp->addAction(it.act->icon(), tb_name(it.act));
		act->setCheckable(true);
		act->setChecked(tbList.contains(it.id));
		QString id = it.id;
		connect(act, &QAction::triggered, this, [this, id, idx](bool on) {
			if (on) {
				tbList.insert(((idx >= 0) && (idx < tbList.size())) ? idx + 1 : tbList.size(), id);
			} else {
				tbList.removeAll(id);
			}
			tbApply();
		});
	}
	// Auto: as big as the window's width allows
	QMenu* size = menu.addMenu("Icon Size");
	QActionGroup* pick = new QActionGroup(size);
	foreach(int px, QList<int>() << 0 << 16 << 24) {
		QAction* act = size->addAction(px ? QString("%0 x %0").arg(px) : QString("Auto"), this, [this, px]() {
			conf.win.tbIcons = px;
			tbApply();		// and the window follows the bar's height
		});
		act->setCheckable(true);
		act->setChecked(conf.win.tbIcons == px);
		pick->addAction(act);
	}
	menu.addAction("Restore Default Buttons", this, [this]() {
		tbList = QString(tbDefault).split(',', X_SkipEmptyParts);
		tbApply();
	});
	menu.addSeparator();
	// as the taskbar's: a slip of the mouse moves no button
	QAction* lock = menu.addAction("Lock Toolbar", this, [this](bool on) {
		conf.win.tbLocked = on;
		tbBuild();
		saveConfig();
	});
	lock->setCheckable(true);
	lock->setChecked(conf.win.tbLocked);
	menu.addAction("Hide Toolbar", tbShowAct, &QAction::trigger);
	menu.exec(pos);
	setFocus();
}

// what the settings and the screen mode say is shown; updateWindow sizes the window after it
void MainWin::showBars() {
	bool full = conf.vid.fullScreen;
	// hidden, not just covered: a widget off screen costs the frame nothing
	bool native = frame->menuBar()->isNativeMenuBar();
	if (!native)
		frame->menuBar()->setVisible(conf.win.menubar && !full);
	if (mbShowAct) {
		mbShowAct->setVisible(!native);
		mbShowAct->setChecked(conf.win.menubar);
	}
	if (mbBackAct) mbBackAct->setVisible(mbHidden());
	bool tb = conf.win.toolbar && !full && !mbHidden();	// a hidden menu bar takes it along
	if (!tb) tbLetGo();		// hidden, its button lets go of the mode
	toolBar->setVisible(tb);
	statusBar->setVisible(conf.win.statusbar && !full);
	if (!full) fsHide();
	mbHintShow(false);
	tbShowAct->setChecked(conf.win.toolbar);
	sbShowAct->setChecked(conf.win.statusbar);
}

// the master volume, beside the mute button
QMenu* MainWin::volumeMenu() {
	QMenu* menu = new QMenu(this);
	xVolBox* box = new xVolBox(menu);
	QWidgetAction* wact = new QWidgetAction(menu);
	wact->setDefaultWidget(box);
	menu->addAction(wact);
	connect(menu, &QMenu::aboutToShow, box, &xVolBox::load);	// the wheel moves it too
	connect(menu, &QMenu::aboutToHide, this, []() {saveConfig();});
	return menu;
}

// Alt+wheel moved the volume: the slider comes up under the button for a moment,
// where the toolbar shows one, and the picture says it otherwise
void MainWin::volumeChanged() {
	QWidget* btn = toolBar->isVisible() ? toolBar->widgetForAction(muteAct) : nullptr;
	if (btn && btn->isVisible()) {
		volPop->load();
		volPop->adjustSize();
		volPop->move(btn->mapToGlobal(QPoint(0, btn->height())));
		volPop->show();
	} else {
		setMessage(QString(" volume %0% ").arg(conf.snd.vol.master));
	}
	volTimer.start();	// and the file is written once the wheel stops
}

// Slow Motion's and Fast Forward's icons show their speeds, 1/2 1/4 1/8 and x2 x4 x8;
// time-slow and time-ffwd are the defaults', 1/4 and x4
void MainWin::speedIcons() {
	auto set = [](QAction* act, const char* name, int n, int& shown) {
		if (n == shown) return;
		shown = n;
		act->setIcon(QIcon((n == 4) ? QString(":/images/%0.png").arg(name) : QString(":/images/%0%1.png").arg(name).arg(n)));
	};
	set(slowAct, "time-slow", conf.emu.slowDiv, slowShown);
	set(ffAct, "time-ffwd", conf.emu.ffMul, ffShown);
}

// a time button held down lets go of its mode: released, or hidden or gone. tbDown
// stays: a hidden button still gets its release, and the click Qt makes of it must
// not switch the mode back on (the Machine menu clears it, should none come)
void MainWin::tbLetGo() {
	if (!tbHeld) return;
	int id = tbHeld;
	tbHeld = 0;
	xcut_release(id);
	syncActions();
}

// the buttons that are switches, read from what they switch
void MainWin::syncActions() {
	Computer* comp = conf.zx;
	pauseAct->setChecked(conf.emu.pause & PR_PAUSE);
	fastAct->setChecked(conf.emu.fast);
	ffAct->setChecked(conf.emu.tmode == XTM_FFWD);
	slowAct->setChecked(conf.emu.tmode == XTM_SLOW);
	speedIcons();		// Options may have changed the speeds
	rewAct->setEnabled(conf.emu.rewind.on);
	diskAct->setEnabled(!dskMenu->isEmpty());
	// on a machine that takes no board the switch stays, off, and says why
	bool fits = hw_takes_divmmc(comp->hw);
	divAct->setEnabled(fits);
	divAct->setChecked(fits && comp->div->model);
	QString divTip = "No divMMC on this machine";
	if (fits) {
		divTip = comp->div->model ? "divMMC off - a Hard Reset" : "divMMC on - a Hard Reset";
		QString key = shortcut_key(XCUT_DIVMMC);
		if (!key.isEmpty()) divTip += QString(" (%0)").arg(key);
	}
	if (divAct->toolTip() != divTip) divAct->setToolTip(divTip);
	tapeAct->setChecked(comp->tape->on && !comp->tape->rec);	// moving, not waiting to be started
	tapeRecAct->setChecked(comp->tape->on && comp->tape->rec);
	mouseAct->setChecked(grabMice);
	recAct->setChecked(vrec_state() == VREC_RUN);
	rzxRecAct->setChecked(rzxr_on());
	wavAct->setChecked(conf.snd.wavout);
	muteAct->setChecked(conf.snd.mute);
	// the button is the volume too: what a click does, then where the volume stands
	QString tip = conf.snd.mute ? "Unmute" : "Mute";
	QString key = shortcut_key(XCUT_MUTE);
	if (!key.isEmpty()) tip += QString(" (%0)").arg(key);
	tip += QString(" - volume %0%").arg(conf.snd.vol.master);
	if (muteAct->toolTip() != tip) muteAct->setToolTip(tip);	// a change repaints the button
	fullAct->setChecked(conf.vid.fullScreen);
	ratioAct->setChecked(conf.vid.keepRatio);
	for (int i = 0; i < sizeActs.size(); i++)
		sizeActs[i]->setChecked(conf.vid.scale == i + 1);
}

// the deck at hand: what the tape player's buttons do, without the window
void MainWin::tapeMenu(const QPoint& pos) {
	Tape* tape = conf.zx->tape;
	syncActions();
	QMenu menu(this);
	menu.addAction(tapeAct);
	menu.addAction(tapeRecAct);
	menu.addAction(QIcon(":/images/tape-rewind.png"), "Rewind", this, [this]() {tapStateChanged(TW_REWIND, 0);});
	menu.addSeparator();
	menu.addAction(QIcon(":/images/fileopen.png"), "Open Tape...", this, [this]() {openMedia(QString(), FG_TAPE, -1, 0);});
	QAction* act = menu.addAction(QIcon(":/images/tape-eject.png"), "Eject", this, &MainWin::tapeEject);
	act->setEnabled(!tape->on);
	menu.addSeparator();
	menu.addAction(cutById.value(XCUT_TAPWIN));
	menu.exec(pos);
}

// Five times a second, for the status bar and the lights over the picture alike. Something
// going on blinks: a moving tape between its color and a brighter one every other look, never
// gray while it works; a drive reading or writing is lit, and out for one look after its head
// stepped - never two looks running, so a seek does not put it out.
void MainWin::lightLook() {
	Computer* comp = conf.zx;
	Tape* tape = comp->tape;
	lightPhase ^= 1;
	// the tape counts as playing when it moves, not when it waits to be started or the machine stands
	if (tape->on && !conf.emu.pause) {
		tapeLight = tape->rec ? (lightPhase ? SB_TAPE_WR_HI : SB_TAPE_WR) : (lightPhase ? SB_TAPE_RD_HI : SB_TAPE_RD);
	} else {
		tapeLight = (tape->blkCount > 0) ? SB_TAPE_IN : SB_TAPE_EMPTY;
	}
	for (int i = 0; i < 4; i++) {
		Floppy* flp = comp->dif->flp[i];
		int seen = flpSeen[i];
		flpSeen[i] = 0;
		bool busy = (seen & 3);
		bool dark = busy && (seen & 4) && !diskDark[i];
		diskDark[i] = dark;
		if (comp->dif->out) {
			diskLight[i] = SB_DISK_OUT;
		} else if (!flp->insert) {
			diskLight[i] = SB_DISK_EMPTY;
		} else if (!busy || dark) {
			diskLight[i] = SB_DISK_IN;
		} else {
			// a write reads the sector headers on the way, so it wins over a read
			diskLight[i] = (seen & 2) ? SB_DISK_WR : SB_DISK_RD;
		}
	}
	// the divMMC's cards: lit for a look after a block went either way
	int card = 0;
	bool any = false;
	for (int i = 0; i < 2; i++) {
		SDCard* sdc = comp->div->card[i];
		if (sdc->reads != cardLook[i][0]) card |= 1;
		if (sdc->writes != cardLook[i][1]) card |= 2;
		cardLook[i][0] = sdc->reads;
		cardLook[i][1] = sdc->writes;
		if (sdc->image) any = true;
	}
	cardLight = (card & 2) ? SB_CARD_WR : (card & 1) ? SB_CARD_RD : any ? SB_CARD_IN : SB_CARD_EMPTY;
}

// a few times a second: QLabel leaves the text alone when it has not changed
void MainWin::updateStatus() {
	if (!statusBar->isVisible()) return;
	Computer* comp = conf.zx;
	const xMachine* mac = xm_find(conf.macId);
	sbMachine->setText(mac ? QString::fromLocal8Bit(mac->name.c_str()) : QString());
	sbClock->setText(QString(" %0 MHz ").arg(xspeed_clock(), 0, 'g', 4));
	// VSync said while it holds the machine to the display's rate
	sbFps->setText(conf.emu.pause ? QString(" paused ") : QString(" %0 fps%1 ").arg(conf.vid.curfps, 0, 'f', 1)
		.arg(pacing_locked() ? ", VSync" : ""));
	// the light stays with no tape in: a click on it is the way to one
	Tape* tape = comp->tape;
	if (tapeLight != sbTapeShown) {
		sbTapeIcon->setPixmap(sbPix[tapeLight]);
		sbTapeShown = tapeLight;
	}
	bool got = (tape->blkCount > 0);
	// a tape played out stands past its last block (tapNextBlock)
	bool inBlock = got && !tape->rec && (tape->block < tape->blkCount);
	sbTape->setVisible(got);
	sbTapeBar->setVisible(got);
	if (got)
		sbTape->setText(QString("%0/%1").arg(qMin(tape->block + 1, tape->blkCount)).arg(tape->blkCount));
	if (inBlock) {
		sbTapeBar->setMaximum(qMax(1, tape->blkData[tape->block].sigCount));
		sbTapeBar->setValue(tape->pos);
	} else {
		sbTapeBar->setValue(0);		// past the last block, or recording
	}
	QString tip = got ? QString::fromLocal8Bit(tape->path ? tape->path : "") : QString("empty");
	if (sbTapeBox->toolTip() != tip) sbTapeBox->setToolTip(tip);	// a change is an event
	// the drives the machine is set to have; out under the divMMC, dim and struck through
	bool out = comp->dif->out;
	for (int i = 0; i < 4; i++) {
		Floppy* flp = comp->dif->flp[i];
		sbDiskBox[i]->setVisible(flp->fitted && (comp->dif->cfg != DIF_NONE));
		sbDisk[i]->setEnabled(!out);
		int st = diskLight[i];
		if (st != sbDiskShown[i]) {
			sbDiskIcon[i]->setPixmap(sbPix[st]);
			sbDiskShown[i] = st;
		}
		QString tip = out ? QString("Off while the divMMC is on")
			: flp->insert ? QString::fromLocal8Bit(flp->path ? flp->path : "") : QString("empty");
		if (sbDiskBox[i]->toolTip() != tip) sbDiskBox[i]->setToolTip(tip);
	}
	sbCardBox->setVisible(comp->div->model);
	if (comp->div->model) {
		if (cardLight != sbCardShown) {
			sbCardIcon->setPixmap(sbPix[cardLight]);
			sbCardShown = cardLight;
		}
		QStringList cards;
		for (int i = 0; i < 2; i++) {
			const char* img = comp->div->card[i]->image;
			cards << QString("Card %0: %1").arg(i + 1).arg(img ? QFileInfo(QString::fromLocal8Bit(img)).fileName() : QString("(no card)"));
		}
		QString tip = "divMMC\n" + cards.join("\n");
		if (sbCardBox->toolTip() != tip) sbCardBox->setToolTip(tip);
	}
	// lit if the machine read it since the last look
	auto light = [this](QLabel* icon, bool fitted, bool& seen, int& shown, int idle) {
		icon->setVisible(fitted);
		int st = seen ? idle + 1 : idle;
		seen = false;
		if (st != shown) {
			icon->setPixmap(sbPix[st]);
			shown = st;
		}
	};
	bool joy = (comp->joy->type == XJ_KEMPSTON);
	bool mouse = comp->mouse->enable;
	light(sbJoyIcon, joy, joySeen, sbJoyShown, SB_JOY_IDLE);
	light(sbMouseIcon, mouse, mouseSeen, sbMouseShown, SB_MOUSE_IDLE);
	sbInputBox->setVisible(joy || mouse);
}

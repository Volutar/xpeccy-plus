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
#include <functional>

#include "emulwin.h"
#include "filer.h"
#include "xcore/vidrec.h"

#define TB_MIME "application/x-xpeccy-toolbar-item"
enum {SB_TAPE_IDLE = 0, SB_TAPE_PLAY, SB_TAPE_REC, SB_DISK_EMPTY, SB_DISK_IN, SB_DISK_RD, SB_DISK_WR};

// A status light in another state: the same picture tinted, so the set stays one.
// alpha < 1 is a slot with nothing in it.
static QPixmap sb_tint(const QString& path, QColor col, double alpha) {
	QImage img = QImage(path).convertToFormat(QImage::Format_ARGB32);
	for (int y = 0; y < img.height(); y++) {
		for (int x = 0; x < img.width(); x++) {
			QRgb p = img.pixel(x, y);
			double l = std::min(1.0, qGray(p) / 160.0);
			img.setPixel(x, y, qRgba(int(col.red() * l), int(col.green() * l), int(col.blue() * l), int(qAlpha(p) * alpha)));
		}
	}
	return QPixmap::fromImage(img);
}

#define TB_SEPARATOR "|"
#define TB_SPACE "~"		// takes what room is left, so what follows sits at the right edge

// what a toolbar holds until the user says otherwise
static const char* tbDefault = "key.load,key.save,|,menu.reset,menu.machine,|,key.tapewin,menu.disks,|,"
	"key.fullscreen,|,menu.debug,key.options,~,key.slowmo,key.pause,key.ffwd,key.fast";

enum {TB_PLAIN = 0, TB_LIST, TB_SPLIT};	// a button; a list that opens on a click; a button with a list beside it

// The window is as wide as the picture, not as these bars: what does not fit
// the toolbar goes under its arrow, and the status bar is cut at the edge.
class xToolBar : public QToolBar {
	public:
		xToolBar(QWidget* p) : QToolBar(p) {setAcceptDrops(true);}
		std::function<void(const QPoint&, int)> onMenu;		// at a point of the screen, on an item or -1
		std::function<void(int, int)> onMove;			// an item dragged from one place to another
		void watch();
		QSize sizeHint() const override {return QSize(0, QToolBar::sizeHint().height());}
		QSize minimumSizeHint() const override {return QSize(0, QToolBar::minimumSizeHint().height());}
	protected:
		bool eventFilter(QObject*, QEvent*) override;
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
bool xToolBar::eventFilter(QObject* obj, QEvent* ev) {
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
			if (btn) btn->setDown(false);
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
	QToolBar::paintEvent(ev);
	if (dropX < 0) return;
	QPainter pnt(this);
	pnt.fillRect(dropX - 1, 2, 2, height() - 4, palette().highlight());
}

// MainWin

void MainWin::initBars() {
	frame->setContextMenuPolicy(Qt::PreventContextMenu);	// its own one lists the bars
	xToolBar* tb = new xToolBar(frame);
	toolBar = tb;
	tb->setObjectName("toolbar");
	tb->setMovable(false);
	tb->setFloatable(false);
	tb->setFocusPolicy(Qt::NoFocus);
	tb->onMenu = [this](const QPoint& pos, int idx) {tbMenu(pos, idx);};
	tb->onMove = [this](int from, int to) {tbMove(from, to);};
	frame->addToolBar(Qt::TopToolBarArea, tb);

	// the catalog: everything a button can be, in the groups the Add menu shows
	auto add = [this](const char* id, const char* grp, QAction* act, int kind, QMenu* list = nullptr) {
		tbCatalog.append({QString(id), QString(grp), act, kind, list});
	};
	// a speed mode switched by a click: the key may be one that has to be held
	auto speed = [this](const char* name, int xcut, int mode, const char* icon) {
		QAction* act = new QAction(QIcon(QString(":/images/%0.png").arg(icon)), name, this);
		act->setCheckable(true);
		connect(act, &QAction::triggered, this, [this, mode]() {
			if (conf.zx->rzx.play) {
				setMessage(" not in RZX ");
			} else {
				xspeed_toggle(mode);
			}
		});
		cutActs.append({act, QString(name), xcut});
		return act;
	};
	auto cut = [this](const char* name, int xcut, const char* icon) {return cutAct(name, xcut, icon);};
	add("key.load", "File", cut("Open...", XCUT_LOAD, "fileopen"), TB_PLAIN);
	add("menu.favorites", "File", bookmarkMenu->menuAction(), TB_SPLIT);
	add("key.reload", "File", cut("Reload", XCUT_RELOAD, "refresh"), TB_PLAIN);
	add("key.save", "File", cut("Save...", XCUT_SAVE, "save_all"), TB_PLAIN);
	add("key.fastsave", "File", cut("Save changed disks", XCUT_FASTSAVE, "floppy"), TB_PLAIN);
	add("key.scrshot", "File", cut("Screenshot", XCUT_SCRSHOT, "grp-screenshot"), TB_PLAIN);
	add("key.video.rec", "File", recAct, TB_PLAIN);
	add("key.write.wav", "File", wavAct, TB_PLAIN);
	add("key.options", "File", cut("Options...", XCUT_OPTIONS, "other"), TB_PLAIN);
	add("menu.machine", "Machine", profileMenu->menuAction(), TB_LIST);
	add("menu.reset", "Machine", resMenu->menuAction(), TB_SPLIT);
	add("key.nmi", "Machine", cut("NMI", XCUT_NMI, "target"), TB_PLAIN);
	add("menu.turbo", "Machine", turboMenu->menuAction(), TB_LIST);
	pauseAct = cut("Pause", XCUT_PAUSE, "time-pause");
	pauseAct->setCheckable(true);
	add("key.pause", "Time", pauseAct, TB_PLAIN);
	fastAct = cut("Fast mode", XCUT_FAST, "time-fast");
	fastAct->setCheckable(true);
	add("key.fast", "Time", fastAct, TB_PLAIN);
	ffAct = speed("Fast forward", XCUT_FFWD, XTM_FFWD, "time-ffwd");
	add("key.ffwd", "Time", ffAct, TB_PLAIN);
	slowAct = speed("Slow motion", XCUT_SLOWMO, XTM_SLOW, "time-slow");
	add("key.slowmo", "Time", slowAct, TB_PLAIN);
	// not the key's own action: a tape armed to start by itself counts as playing
	// there, so a click on a button that shows it stopped would stop it
	tapeAct = new QAction(QIcon(":/images/tape-play.png"), "Tape play", this);
	connect(tapeAct, &QAction::triggered, this, [this]() {
		tapStateChanged(TW_STATE, conf.zx->tape->on ? TWS_STOP : TWS_PLAY);
	});
	cutActs.append({tapeAct, QString("Tape play"), XCUT_TAPLAY});
	tapeAct->setCheckable(true);
	add("key.tape.play", "Media", tapeAct, TB_PLAIN);
	tapeRecAct = cut("Tape record", XCUT_TAPREC, "tape-rec");
	tapeRecAct->setCheckable(true);
	add("key.tape.rec", "Media", tapeRecAct, TB_PLAIN);
	add("key.tapewin", "Media", cut("Tape player", XCUT_TAPWIN, "tape"), TB_PLAIN);
	// a button of its own, not the menu's: that one is hidden on a machine with no drives
	diskAct = new QAction(QIcon(":/images/fdd_disk.png"), "Disk manager", this);
	connect(diskAct, &QAction::triggered, this, [this]() {diskWin->showWindow();});
	add("menu.disks", "Media", diskAct, TB_SPLIT, dskMenu);
	add("menu.cartridge", "Media", cartMenu->menuAction(), TB_LIST);
	add("menu.sdcard", "Media", sdcMenu->menuAction(), TB_LIST);
	add("menu.hdd", "Media", hddMenu->menuAction(), TB_LIST);
	add("key.rzxwin", "Media", cut("RZX player", XCUT_RZXWIN, "video"), TB_PLAIN);
	add("key.fullscreen", "View", fullAct, TB_PLAIN);
	add("key.ratio", "View", ratioAct, TB_PLAIN);
	add("menu.size", "View", sizeMenu->menuAction(), TB_LIST);
	add("menu.shaders", "View", shdMenu->menuAction(), TB_LIST);
	add("menu.palette", "View", palMenu->menuAction(), TB_LIST);
	add("key.keywin", "View", cut("Virtual keyboard", XCUT_KEYBOARD, "keyboardzx"), TB_PLAIN);
	add("key.keyboard.grab", "Input", pckAct, TB_PLAIN);
	mouseAct = cut("Grab mouse", XCUT_MOUSE, "mouse");
	mouseAct->setCheckable(true);
	add("key.mouse.grab", "Input", mouseAct, TB_PLAIN);
	add("menu.keymap", "Input", keyMenu->menuAction(), TB_LIST);
	add("menu.debug", "Debug", dbgMenu->menuAction(), TB_SPLIT);
	add("key.scrwin", "Debug", cut("Screen", XCUT_SCRWIN, "rulers"), TB_PLAIN);
	add("key.sndwin", "Debug", cut("Sound chips", XCUT_SNDWIN, "note"), TB_PLAIN);
	foreach(const xTbItem& it, tbCatalog) {
		// a list opened from here has had no right-click menu fill it; one opened
		// inside a menu has, and refilling everything on each hover costs disk reads
		if (it.act->menu())
			connect(it.act->menu(), &QMenu::aboutToShow, this, [this]() {
				if (!QApplication::activePopupWidget()) fillUserMenu();
			}, Qt::UniqueConnection);
		// a switch shows its tick in a menu, which its icon would take the place of
		if (it.act->isCheckable())
			it.act->setIconVisibleInMenu(false);
	}

	tbList = QString::fromStdString((conf.win.tbItems == "*") ? std::string(tbDefault) : conf.win.tbItems).split(',', X_SkipEmptyParts);
	tbBuild();

	xStatusBar* sb = new xStatusBar(frame);
	statusBar = sb;
	sb->setSizeGripEnabled(false);
	sbMachine = new QLabel;
	sbClock = new QLabel;
	sbFps = new QLabel;
	sb->addWidget(sbMachine, 1);
	sbPix[SB_TAPE_PLAY] = QPixmap(":/images/tapeYellow.png");
	sbPix[SB_TAPE_REC] = QPixmap(":/images/tapeRed.png");
	sbPix[SB_TAPE_IDLE] = sb_tint(":/images/tapeYellow.png", QColor(200, 200, 200), 1.0);
	sbPix[SB_DISK_RD] = QPixmap(":/images/diskGreen.png");
	sbPix[SB_DISK_WR] = QPixmap(":/images/diskRed.png");
	sbPix[SB_DISK_IN] = sb_tint(":/images/diskGreen.png", QColor(90, 150, 255), 1.0);
	sbPix[SB_DISK_EMPTY] = sb_tint(":/images/diskGreen.png", QColor(170, 170, 170), 0.45);
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
	for (int i = 0; i < 4; i++) {
		sbDisk[i] = new QLabel(QString("%0:").arg(QChar('A' + i)));
		sbDiskIcon[i] = new QLabel;
		sbDiskBox[i] = pair(sbDisk[i], sbDiskIcon[i]);
	}
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

void MainWin::tbBuild() {
	toolBar->clear();
	toolBar->setIconSize(QSize(conf.win.tbIcons, conf.win.tbIcons));
	foreach(const QString& id, tbList) {
		if (id == TB_SEPARATOR) {
			toolBar->addSeparator();
			continue;
		}
		if (id == TB_SPACE) {
			QWidget* space = new QWidget;
			space->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
			toolBar->addWidget(space);
			continue;
		}
		const xTbItem* it = tbFind(id);
		if (!it) {
			// an id this build does not know is kept, so another build still finds it
			toolBar->addSeparator()->setVisible(false);
			continue;
		}
		toolBar->addAction(it->act);
		QToolButton* btn = qobject_cast<QToolButton*>(toolBar->widgetForAction(it->act));
		if (!btn) continue;
		btn->setFocusPolicy(Qt::NoFocus);
		if (it->list)
			btn->setMenu(it->list);
		if (it->kind != TB_PLAIN)
			btn->setPopupMode((it->kind == TB_LIST) ? QToolButton::InstantPopup : QToolButton::MenuButtonPopup);
		if (it->act->menu())
			btn->setToolTip(tb_name(it->act));
	}
	static_cast<xToolBar*>(toolBar)->watch();
}

// the list as the user left it, and the bar rebuilt from it
void MainWin::tbApply() {
	conf.win.tbItems = tbList.join(",").toStdString();
	tbBuild();
	saveConfig();
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
		menu.addAction("Add separator here", this, [this, idx]() {
			tbList.insert(idx, TB_SEPARATOR);
			tbApply();
		});
		// the rest of the bar goes to the right edge
		menu.addAction("Add space here", this, [this, idx]() {
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
	QMenu* size = menu.addMenu("Icon size");
	foreach(int px, QList<int>() << 16 << 24) {
		QAction* act = size->addAction(QString("%0 x %0").arg(px), this, [this, px]() {
			conf.win.tbIcons = px;
			tbApply();
			updateWindow();		// the window follows the bar's height
		});
		act->setCheckable(true);
		act->setChecked(conf.win.tbIcons == px);
	}
	menu.addAction("Restore default buttons", this, [this]() {
		tbList = QString(tbDefault).split(',', X_SkipEmptyParts);
		tbApply();
	});
	menu.addSeparator();
	menu.addAction("Hide toolbar", tbShowAct, &QAction::trigger);
	menu.exec(pos);
	setFocus();
}

// what the settings and the screen mode say is shown; updateWindow sizes the window after it
void MainWin::showBars() {
	bool full = conf.vid.fullScreen;
	// hidden, not just covered: a widget off screen costs the frame nothing
	if (!frame->menuBar()->isNativeMenuBar())
		frame->menuBar()->setVisible(!full);
	toolBar->setVisible(conf.win.toolbar && !full);
	statusBar->setVisible(conf.win.statusbar && !full);
	if (!full) fsHide();
	tbShowAct->setChecked(conf.win.toolbar);
	sbShowAct->setChecked(conf.win.statusbar);
}

// the buttons that are switches, read from what they switch
void MainWin::syncActions() {
	Computer* comp = conf.zx;
	pauseAct->setChecked(conf.emu.pause & PR_PAUSE);
	fastAct->setChecked(conf.emu.fast);
	ffAct->setChecked(conf.emu.tmode == XTM_FFWD);
	slowAct->setChecked(conf.emu.tmode == XTM_SLOW);
	diskAct->setEnabled(!dskMenu->isEmpty());
	tapeAct->setChecked(comp->tape->on && !comp->tape->rec);	// moving, not waiting to be started
	tapeRecAct->setChecked(comp->tape->on && comp->tape->rec);
	mouseAct->setChecked(grabMice);
	recAct->setChecked(vrec_state() == VREC_RUN);
	wavAct->setChecked(conf.snd.wavout);
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
	menu.addAction(QIcon(":/images/fileopen.png"), "Open tape...", this, [this]() {openMedia(QString(), FG_TAPE, -1, 0);});
	QAction* act = menu.addAction(QIcon(":/images/tape-eject.png"), "Eject", this, [this]() {
		tapEject(conf.zx->tape);
		emit s_tape_upd(conf.zx->tape);
		emit s_tape_blk(conf.zx->tape);
	});
	act->setEnabled(!tape->on);
	menu.addSeparator();
	menu.addAction(cutById.value(XCUT_TAPWIN));
	menu.exec(pos);
}

// a few times a second: QLabel leaves the text alone when it has not changed
void MainWin::updateStatus() {
	if (!statusBar->isVisible()) return;
	Computer* comp = conf.zx;
	const xMachine* mac = xm_find(conf.macId);
	sbMachine->setText(mac ? QString::fromLocal8Bit(mac->name.c_str()) : QString());
	sbClock->setText(QString(" %0 MHz ").arg(xspeed_clock(), 0, 'g', 4));
	sbFps->setText(conf.emu.pause ? QString(" paused ") : QString(" %0 fps ").arg(conf.vid.curfps, 0, 'f', 1));
	// the tape counts as playing when it moves, not when it waits to be started
	Tape* tape = comp->tape;
	sbTapeBox->setVisible(tape->blkCount > 0);
	if (tape->blkCount > 0) {
		int st = tape->on ? (tape->rec ? SB_TAPE_REC : SB_TAPE_PLAY) : SB_TAPE_IDLE;
		if (st != sbTapeShown) {
			sbTapeIcon->setPixmap(sbPix[st]);
			sbTapeShown = st;
		}
		sbTape->setText(QString("%0/%1").arg(tape->block + 1).arg(tape->blkCount));
		QString tip = QString::fromLocal8Bit(tape->path ? tape->path : "");
		if (sbTapeBox->toolTip() != tip) sbTapeBox->setToolTip(tip);	// a change is an event
	}
	for (int i = 0; i < 4; i++) {
		Floppy* flp = comp->dif->flp[i];
		sbDiskBox[i]->setVisible(flp->fitted && (comp->dif->type != DIF_NONE));
		int st = !flp->insert ? SB_DISK_EMPTY : (flpSeen[i] & 2) ? SB_DISK_WR : (flpSeen[i] & 1) ? SB_DISK_RD : SB_DISK_IN;
		flpSeen[i] = 0;
		if (st != sbDiskShown[i]) {
			sbDiskIcon[i]->setPixmap(sbPix[st]);
			sbDiskShown[i] = st;
		}
		QString tip = flp->insert ? QString::fromLocal8Bit(flp->path ? flp->path : "") : QString("empty");
		if (sbDiskBox[i]->toolTip() != tip) sbDiskBox[i]->setToolTip(tip);
	}
}

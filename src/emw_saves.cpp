// Quick saves from the window: the keys, the picture beside each save and the
// File menu's list of them. Where they go is xcore/saves.cpp; the files
// themselves are filer.cpp's.

#include "emulwin.h"
#include "filer.h"
#include "xcore/xcore.h"
#include "xcore/saves.h"
#include "libxpeccy/filetypes/filetypes.h"
#include "xgui/savehistory.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QMenu>
#include <QToolTip>
#include <QUrl>

// the picture of a save, no bigger than a Spectrum's screen: that one 1:1
#define SAVE_PIC_W	256
#define SAVE_PIC_H	192

// The screen alone on a Spectrum; the window as shown on the machines whose
// modes draw over the border too: the Evo, the ATM it comes from, the Next.
static QImage save_picture() {
	Computer* comp = conf.zx;
	Video* vid = comp->vid;
	int id = comp->hw->id;
	bool whole = (id == HW_PENTEVO) || (id == HW_TSLAB) || (id == HW_ATM1) || (id == HW_ATM2) || (id == HW_ZXNEXT);
	QImage img(frame_shown_lock(), vid->full.x * 2, bufSize / bytesPerLine, bytesPerLine, QImage::Format_RGBA8888);
	if (whole)
		img = img.copy(vid->lcut.x * 2, vid->lcut.y, vid->vsze.x * 2, vid->vsze.y);
	else
		img = img.copy(vid->bord.x * 2, vid->bord.y, vid->scrn.x * 2, vid->scrn.y);
	frame_shown_unlock();
	if (img.isNull()) return img;
	QSize size = whole ? QSize(vid->vsze.x, vid->vsze.y) : QSize(vid->scrn.x, vid->scrn.y);
	QSize nat(qMax(1, (int)(size.width() * comp->hw->xscale)), qMax(1, size.height()));
	return img.scaled(nat.scaled(SAVE_PIC_W, SAVE_PIC_H, Qt::KeepAspectRatio), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
		.convertToFormat(QImage::Format_RGB32);
}

static int save_keep(int kind) {
	return (kind == SAVE_EXIT) ? conf.saves.exitKeep : (kind == SAVE_TIMED) ? conf.saves.timedKeep : conf.saves.quickKeep;
}

// a new save of that kind in the folder of the game: 2 written, 1 held in
// memory (a machine with no .szx), 0 failed
int MainWin::saveState(int kind) {
	QImage pic = save_picture();		// the frame the key was pressed on
	if (!szxCanSave(conf.zx)) return quick_save(conf.zx, QString());	// held in memory: no folder
	QString dir = saves_dir(true);
	QString path = dir.isEmpty() ? QString() : saves_new_path(dir, kind);
	int res = quick_save(conf.zx, path);
	if (res == 2) {
		if (!pic.isNull()) pic.save(QFileInfo(path).path() + "/" + QFileInfo(path).completeBaseName() + ".png", "PNG");
		saves_trim(dir, kind, save_keep(kind));
	}
	return res;
}

// a save put back, on the machine it was taken on
bool MainWin::loadState(const QString& path) {
	viewEnd();
	char id[256] = "";
	szx_machine_of(path.toLocal8Bit().constData(), id, sizeof(id));
	quick_keep_undo(conf.zx);
	if (*id && (conf.macId != id) && xm_find(id)) setMachine(id);
	if (!quick_load(conf.zx, path)) return false;
	if (game_image().isEmpty()) showMedia(QString(), MEDIA_SNAP);	// a machine's save: no game runs
	return true;
}

void MainWin::quickSave() {
	int x = saveState(SAVE_QUICK);
	setMessage((x == 2) ? " quick save " : (x == 1) ? " quick save, until exit " : " can't quick save ");
}

// the newest quick save of the game, else the one held in memory
void MainWin::quickLoad() {
	viewEnd();
	QList<xSave> list = saves_list(saves_dir(false), SAVE_QUICK);
	bool done = false;
	if (!list.isEmpty()) {
		done = loadState(list.first().path);
	} else {
		quick_keep_undo(conf.zx);
		done = quick_load_held(conf.zx) != 0;
	}
	setMessage(done ? " quick load " : " nothing quick saved ");
}

void MainWin::quickUndo() {
	std::string mac = quick_undo_machine();
	bool done = false;
	if (!mac.empty()) {
		quick_undo_prepare(conf.zx);
		if ((mac != conf.macId) && xm_find(mac)) setMachine(mac);
		done = quick_undo(conf.zx) != 0;
	}
	setMessage(done ? " quick load undone " : " nothing to undo ");
}

// An autosave, on exit or every few minutes: into the folder of the game, or
// of the machine, as a quick save goes; the config says which is the last
// session
void MainWin::autoSave(int kind) {
	if (scrView.on || !szxCanSave(conf.zx)) return;	// its picture would be the screen viewed
	QImage pic = save_picture();
	QString dir = saves_dir(true);
	if (dir.isEmpty()) return;
	QString path = saves_new_path(dir, kind);
	if (quick_save(conf.zx, path) != 2) return;
	if (!pic.isNull()) pic.save(QFileInfo(path).path() + "/" + QFileInfo(path).completeBaseName() + ".png", "PNG");
	saves_trim(dir, kind, save_keep(kind));
	conf.saves.last = path;
}

void MainWin::exitSave() {
	if (conf.saves.exitOn) autoSave(SAVE_EXIT);
}

// every 0.2 s from the window's timer: running time only, so a pause, a menu
// or the Debugger hold it
void MainWin::timedSave(int ms) {
	if (!conf.saves.timedOn) {
		timedMs = 0;
		return;
	}
	if (conf.emu.pause) return;
	timedMs += ms;
	if (timedMs < conf.saves.timedMin * 60000LL) return;
	timedMs = 0;
	autoSave(SAVE_TIMED);
}

// the last autosave; that one deleted, the newest left of any game
bool MainWin::resumeLast() {
	QString path = saves_resume().path;
	bool done = !path.isEmpty() && loadState(path);
	setMessage(done ? " last session resumed " : " no last session to resume ");
	return done;
}

// File > Resume Last Session, on each show: on when there is a save to resume,
// its picture and its game's name and time the tip (xMenu shows it, the File
// menu's other items keep theirs for the toolbar)
void MainWin::syncResume() {
	QString name;
	xSave sv = saves_resume(&name);
	resumeAct->setEnabled(!sv.path.isEmpty());
	QString tip;
	if (!sv.path.isEmpty()) {
		QString time = saves_time_text(sv.time);
		int width = sv.pic.isEmpty() ? 0 : QImageReader(sv.pic).size().width();
		if (!name.isEmpty()) {
			QFontMetrics fm(QToolTip::font());
			if (width > 0)		// one line under the picture
				name = fm.elidedText(name, Qt::ElideRight, qMax(width - fm.horizontalAdvance(", " + time), fm.horizontalAdvance("M...")));
			time = name + ", " + time;
		}
		// a tag makes it rich text, with or without the picture: a name's own '&' or '<' as it is
		tip = "<nobr>" + time.toHtmlEscaped() + "</nobr>";
		if (!sv.pic.isEmpty())
			tip = QString("<img src=\"%0\"><br>%1").arg(QUrl::fromLocalFile(sv.pic).toString().toHtmlEscaped(), tip);
	}
	resumeAct->setProperty("xTip", tip);
}

// Pause in menus holds the machine here too: the window is where a save is
// picked, as from the menu. A reason of its own - the menu's goes as the menu
// that opened the window closes.
void MainWin::saveHistory() {
	bool hold = conf.pauseMenu;
	if (hold) pause(true, PR_SAVES);
	QString path = save_history(this, saves_dir(false));
	if (hold) pause(false, PR_SAVES);
	if (!path.isEmpty())
		setMessage(loadState(path) ? " quick load " : " can't load the save ");
	setFocus();
}

// a folder's saves as menu items, newest first, the picture shown as the tip
void MainWin::recentSaves(QMenu* menu, const QString& dir) {
	for (const xSave& sv : saves_list(dir)) {
		QString text = saves_time_text(sv.time);
		if (sv.kind != SAVE_QUICK) text += "  " + saves_kind_name(sv.kind);
		QAction* act = menu->addAction(text);
		QString path = sv.path;
		connect(act, &QAction::triggered, this, [this, path]() {
			setMessage(loadState(path) ? " quick load " : " can't load the save ");
			setFocus();
		});
		if (!sv.pic.isEmpty()) {
			act->setIcon(QIcon(sv.pic));
			act->setToolTip(QString("<img src=\"%0\">").arg(QUrl::fromLocalFile(sv.pic).toString()));
		}
	}
}

// File > Quick Saves: the game's saves, newest first; empty when there is none
void MainWin::fillQuickSaves() {
	quickMenu->clear();
	QString dir = saves_dir(false);
	QList<xSave> list = saves_list(dir);
	QString title = game_name();
	if (title.isEmpty())
		title = QString::fromLocal8Bit(xm_find(conf.macId) ? xm_find(conf.macId)->name.c_str() : conf.macId.c_str());
	quickMenu->addAction(title.replace("&", "&&"))->setEnabled(false);
	if (list.isEmpty())
		quickMenu->addAction("No saves yet")->setEnabled(false);
	recentSaves(quickMenu, dir);
	quickMenu->addSeparator();
	QAction* show = quickMenu->addAction("Show Saves Folder");
	show->setEnabled(!dir.isEmpty());
	connect(show, &QAction::triggered, this, [dir]() {
		QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
	});
}

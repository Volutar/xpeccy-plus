#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QPushButton>
#include <QActionGroup>
#include <QMenu>
#include <QComboBox>
#include <QMessageBox>

#include "emulwin.h"
#include "filer.h"
#include "xcore/arcpath.h"
#include "xcore/vfat_scan.h"
#include "libxpeccy/filetypes/filetypes.h"

// THE DRIVES MENU
//
// What is in the machine's drives, and doing things with it: the state of the
// machine, where Options only holds what the drives are. Each action pauses
// the machine and takes the emulation lock around the change, as openMedia does.

int testSlotOn(Computer*);

// a menu item's name for what is in a drive
QString drive_media(const char* path, bool in) {
	QString nam = !in ? QString("(empty)") : (path && *path) ? arc_name(QString::fromLocal8Bit(path)) : QString("(new disk)");
	return nam.replace("&", "&&");
}

// one file, however its path was written
static bool same_path(const QString& a, const QString& b) {
#ifdef _WIN32
	const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
	const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
	return !QDir::fromNativeSeparators(a).compare(QDir::fromNativeSeparators(b), cs);
}

// The set an image belongs to, for a slot: the archive's images when it came from one
// holding several, else its siblings in the folder (the other sides by name); cur the
// image's place, title what a submenu of them is called. False when it has none.
static bool image_set(const QString& image, int id, int drv, QStringList* names, QStringList* paths, int* cur, QString* title) {
	QString archive, entry, err;
	names->clear();
	paths->clear();
	*cur = -1;
	if (arc_split(image, &archive, &entry)) {
		*names = media_candidates(conf.zx, archive, id, drv, &err);
		if (names->size() > 1) {
			*cur = int(names->indexOf(entry));
			for (const QString& name : *names) paths->append(arc_join(archive, name));
			*title = QFileInfo(archive).fileName();
			return true;
		}
		names->clear();
	}
	for (const xSibling& sib : media_siblings(conf.zx, image, id, drv, cur)) {
		names->append(sib.name);
		paths->append(sib.path);
	}
	*title = QFileInfo(QFileInfo(archive.isEmpty() ? image : archive).absolutePath()).fileName();
	return names->size() > 1;
}

// The other images of the set the one in the deck or a drive belongs to, the one in now
// marked: a click puts that one in, as the drive's own Open does. The drives share a
// set: an empty drive, or one whose disk has none, offers the set of a disk in another
// drive, as a program's second disk goes into B; a disk in another drive is shown with
// its letter and cannot be picked. Nor can a side while the tape plays, as Eject. More
// than inlineMax go into a submenu named after the archive or the folder. Put in before
// the action before, or at the end.
void MainWin::otherImages(QMenu* m, int id, int drv, QAction* before, int inlineMax) {
	Computer* comp = conf.zx;
	QStringList names, paths;
	QString title;
	QString own[4];		// the drives' disks
	int cur = -1;
	bool locked = false;
	if (id == FG_TAPE) {
		Tape* tape = comp->tape;
		if ((tape->blkCount < 1) || !tape->path || !*tape->path) return;
		locked = tape->on;
		image_set(QString::fromLocal8Bit(tape->path), id, drv, &names, &paths, &cur, &title);
	} else {
		for (int i = 0; i < 4; i++) {
			Floppy* flp = comp->dif->flp[i];
			if (flp->insert && flp->path && *flp->path) own[i] = QString::fromLocal8Bit(flp->path);
		}
		bool got = !own[drv].isEmpty() && image_set(own[drv], id, drv, &names, &paths, &cur, &title);
		for (int i = 0; (i < 4) && !got; i++) {
			if ((i == drv) || own[i].isEmpty()) continue;
			got = image_set(own[i], id, drv, &names, &paths, &cur, &title);
			cur = -1;
		}
	}
	if (names.size() < 2) return;
	m->insertSeparator(before);
	QMenu* to = m;
	if (names.size() > inlineMax) {
		to = new QMenu(title.replace("&", "&&"), m);
		m->insertMenu(before, to);
	}
	QActionGroup* grp = new QActionGroup(to);
	for (int i = 0; i < names.size(); i++) {
		QString text = QString(names[i]).replace("&", "&&");
		int in = -1;
		for (int j = 0; j < 4; j++)
			if ((j != drv) && !own[j].isEmpty() && same_path(own[j], paths[i])) in = j;
		if (in >= 0) text += QString("\t%1:").arg(QChar('A' + in));
		QAction* act = new QAction(text, to);
		to->insertAction((to == m) ? before : nullptr, act);
		act->setCheckable(true);
		act->setChecked(i == cur);
		act->setEnabled((i == cur) || (!locked && (in < 0)));
		grp->addAction(act);
		if (i == cur) continue;
		const QString open = paths[i];
		connect(act, &QAction::triggered, this, [this, open, id, drv]() {
			openMedia(open, id, drv, AR_MOUNT, true, true);
			if (diskWin->isVisible()) diskWin->refresh();
		});
	}
	m->insertSeparator(before);
}

int drive_count(Computer* comp) {
	return (comp->dif->type == DIF_BDI) ? 4 : (comp->dif->type == DIF_P3DOS) ? 2 : 0;
}

// THE SIDES OF A SET
//
// The Next/Previous Side keys, and the Tape Player's and the Disk Manager's buttons: the
// next place of the set (Side A -> B, round past the end) in the deck or a drive. It goes
// in as a pick from the menu does, a playing tape stopped first. A disk in another drive
// is not taken.

// what the deck or a drive holds: empty for none, or a drive the machine has not
QString MainWin::sideHeld(int src) {
	Computer* comp = conf.zx;
	const char* raw = nullptr;
	if (src == MEDIA_TAPE) {
		if (comp->tape->blkCount > 0) raw = comp->tape->path;
	} else if ((src >= 0) && (src < drive_count(comp)) && !comp->dif->out) {
		Floppy* flp = comp->dif->flp[src];
		if (flp->insert) raw = flp->path;
	}
	return QString::fromLocal8Bit((raw && *raw) ? raw : "");
}

// the image step places on in the deck or a drive; empty when there is none
QString MainWin::sideNext(int src, int step, QString* place) {
	Computer* comp = conf.zx;
	QString path = sideHeld(src);
	if (path.isEmpty()) return QString();
	// an image still being read for this slot: a later one would drop it, so it is the
	// one that goes in, and the press steps on from it
	if ((src == mountSrc) && !mountPath.isEmpty()) path = mountPath;
	if (src == MEDIA_TAPE) return media_next_side(comp, path, FG_TAPE, -1, tapeSeen, step, {}, place, true);
	QStringList skip;
	for (int i = 0; i < 4; i++) {
		const QString other = (i == src) ? QString() : sideHeld(i);
		if (!other.isEmpty()) skip.append(other);
	}
	return media_next_side(comp, path, FH_DRIVE_A + src, src, {}, step, skip, place, true);
}

// Whether a drive's disk has another side to step to, for its buttons. The deck's tape
// is looked at once a tape, by watchTapeSet.
bool MainWin::hasSides(int src) {
	return (src == MEDIA_TAPE) ? tapeSides : !sideNext(src, 1, nullptr).isEmpty();
}

// the next side into the deck or a drive, or why not
void MainWin::sideStepIn(int src, int step) {
	Computer* comp = conf.zx;
	if (comp->rzx.play) {
		setMessage(" not in RZX ");
		return;
	}
	QString place;
	const QString path = sideNext(src, step, &place);
	if (path.isEmpty()) {
		setMessage(" no other side ");
		return;
	}
	sidePut(src, path, place);
}

// The keys: in the slot of the image in use - the tape or the drive the title names, or
// named last while a side changes in it; none yet, the tape if it can be stepped, else drive A
void MainWin::sideStep(int step) {
	int src = sideSrc;
	if (sideHeld(src).isEmpty())
		src = tapeSides ? MEDIA_TAPE : 0;
	sideStepIn(src, step);
}

void MainWin::sidePut(int src, const QString& path, const QString& place) {
	sideSrc = src;
	// its place in the set (media_next_side), "side B"
	if (src == MEDIA_TAPE) {
		if (conf.zx->tape->on) tapStateChanged(TW_STATE, TWS_STOP);
		openMedia(path, FG_TAPE, -1, AR_MOUNT, true, true, QString(" tape: %0 ").arg(place));
	} else {
		openMedia(path, FH_DRIVE_A + src, src, AR_MOUNT, true, true,
			QString(" drive %0: %1 ").arg(QChar('A' + src)).arg(place));
	}
}

// an SD card by its place: -1 the Z-controller's, 0 and 1 a divMMC's slots
static SDCard* sd_card(Computer* comp, int card) {
	return (card < 0) ? comp->sdc : comp->div->card[card & 1];
}

// a card's own actions: what is in it, putting one in or taking it out, its lock
void MainWin::fillCardMenu(QMenu* m, int card) {
	SDCard* sdc = sd_card(conf.zx, card);
	bool dir = sdc->image && QFileInfo(QString::fromLocal8Bit(sdc->image)).isDir();
	QAction* act;
	if (card < 0) {
		act = m->addAction(sdc->image ? drive_media(sdc->image, true) : QString("(no card)"));
		act->setEnabled(false);
		m->addSeparator();
	}
	m->addAction(QIcon(":/images/sdcard.png"), "Open Image...", this, [this, card]() {sdcOpen(card, false);});
	m->addAction(QIcon(":/images/fileopen.png"), "Open Folder...", this, [this, card]() {sdcOpen(card, true);});
	if (card >= 0)		// a divMMC's: esxDOS is its firmware
		m->addAction(QIcon(":/images/doc-new.png"), "New esxDOS Folder...", this, [this, card]() {sdcNewEsx(card);});
	else if (conf.zx->hw && (conf.zx->hw->id == HW_ZXNEXT))
		m->addAction(QIcon(":/images/doc-new.png"), "System/Next Card...", this, [this]() {nextCard();});
	act = m->addAction(QIcon(":/images/tape-eject.png"), "Eject", this, [this, card]() {cardEject(card);});
	act->setEnabled(sdc->image != NULL);
	act = m->addAction(QIcon(":/images/refresh.png"), "Refresh", this, [this, card]() {folderRefresh(false, card);});
	act->setEnabled(sdc->vfat != NULL);
	m->addSeparator();
	// the card's lock switch; a folder is read only whatever it says unless Options lets
	// folders take writes
	act = m->addAction("Write Protect");
	act->setCheckable(true);
	act->setChecked(sdc_protected(sdc));
	act->setEnabled(!dir || (sdc->vfat && sdc->vfat->writable));
	connect(act, &QAction::toggled, this, [card](bool on) {sdcSetLock(sd_card(conf.zx, card), on ? 1 : 0);});
}

void MainWin::fillDrivesMenu() {
	Computer* comp = conf.zx;
	QAction* act;
	QMenu* menus[] = {dskMenu, drvMenu, hddMenu, sdcMenu, cartMenu};
	for (QMenu* m : menus) {
		// a drive's submenu is a child of its menu, which clear() leaves alive
		for (QAction* a : m->actions())
			if (a->menu()) a->menu()->deleteLater();
		m->clear();
	}
	for (int i = 0; i < 4; i++) flpMenu[i] = nullptr;
	// out under the divMMC: the drives the machine is set to have, grayed, under a
	// line that says why
	auto outLine = [](QMenu* m) {
		m->addAction("Unavailable while divMMC is enabled")->setEnabled(false);
		m->addSeparator();
	};
	bool flpOut = comp->dif->out;
	int dif = comp->dif->cfg;
	int drives = (dif == DIF_BDI) ? 4 : (dif == DIF_P3DOS) ? 2 : 0;
	if (flpOut && drives) outLine(dskMenu);
	for (int i = 0; i < drives; i++) {
		Floppy* flp = comp->dif->flp[i];
		if (!flp->fitted) continue;
		// the drive's letter is on its icon
		QMenu* m = dskMenu->addMenu(QIcon(QString(":/images/fdd_disk_%0.png").arg(QChar('A' + i))),
			drive_media(flp->path, flp->insert) + ((flp->insert && flp->changed) ? " *" : ""));
		if (flpOut) {
			m->menuAction()->setEnabled(false);
			continue;
		}
		flpMenu[i] = m;
		m->addAction(QIcon(":/images/fileopen.png"), "Open...", this, [this, i]() {diskOp(DW_OPEN, i);});
		act = m->addAction(QIcon(":/images/doc-new.png"), "New", this, [this, i]() {diskOp(DW_NEW, i);});
		// the other images of its set, when the menu opens: the list reads a folder, and
		// this one is filled each time Media or Machine opens
		connect(m, &QMenu::aboutToShow, this, [this, m, i, act]() {
			if (m->property("others").toBool()) return;
			m->setProperty("others", true);
			otherImages(m, FH_DRIVE_A + i, i, act);
		});
		act = m->addAction(QIcon(":/images/floppy.png"), "Save", this, [this, i]() {diskOp(DW_SAVE, i);});
		act->setEnabled(flp->insert && flp->path && *flp->path);
		act = m->addAction(QIcon(":/images/save-as.png"), "Save As...", this, [this, i]() {diskOp(DW_SAVE_AS, i);});
		act->setEnabled(flp->insert);
		act = m->addAction(QIcon(":/images/tape-eject.png"), "Eject", this, [this, i]() {diskOp(DW_EJECT, i);});
		act->setEnabled(flp->insert);
		m->addSeparator();
		act = m->addAction("Write Protect");
		act->setCheckable(true);
		act->setChecked(flp->protect);
		connect(act, &QAction::toggled, this, [i](bool on) {conf.zx->dif->flp[i]->protect = on ? 1 : 0;});
	}

	if (comp->ide->cfg != IDE_NONE) {
		bool hddOut = comp->ide->out;
		if (hddOut) outLine(hddMenu);
		ATADev* dev[2] = {comp->ide->master, comp->ide->slave};
		for (int i = 0; i < 2; i++) {
			if (dev[i]->type == IDE_NONE) continue;
			int id = i ? IDE_SLAVE : IDE_MASTER;
			QMenu* m = hddMenu->addMenu(QIcon(":/images/hdd.png"),
				QString("%0: %1").arg(i ? "Slave" : "Master").arg(drive_media(dev[i]->image, dev[i]->image != NULL)));
			if (hddOut) {
				m->menuAction()->setEnabled(false);
				continue;
			}
			m->addAction(QIcon(":/images/hdd.png"), "Open Image...", this, [this, id]() {hddOpen(id, false);});
			m->addAction(QIcon(":/images/fileopen.png"), "Open Folder...", this, [this, id]() {hddOpen(id, true);});
			act = m->addAction(QIcon(":/images/tape-eject.png"), "Eject", this, [this, id]() {
				driveOp([id]() {ide_mount(conf.zx->ide, id, QString());});
			});
			act->setEnabled(dev[i]->image != NULL);
			act = m->addAction(QIcon(":/images/refresh.png"), "Refresh", this, [this, id]() {folderRefresh(true, id);});
			act->setEnabled(dev[i]->vfat != NULL);
			m->addSeparator();
			act = m->addAction("Properties...", this, [this, id]() {hddProps(id);});
			act->setEnabled(dev[i]->image != NULL);
		}
	}

	int hw = comp->hw->id;
	if (comp_has_sd(comp))
		fillCardMenu(sdcMenu, -1);
	fillDivMenu();

	if ((hw == HW_ZX48) || (hw == HW_ALF)) {
		xCartridge* slot = comp->slot;
		act = cartMenu->addAction(drive_media(slot->path, slot->data != NULL));
		act->setEnabled(false);
		cartMenu->addSeparator();
		cartMenu->addAction(QIcon(":/images/fileopen.png"), "Open...", this, [this]() {
			openMedia(QString(), FH_SLOTS, 0, 0);
		});
		act = cartMenu->addAction(QIcon(":/images/tape-eject.png"), "Eject", this, [this]() {
			driveOp([]() {
				Computer* comp = conf.zx;
				sltEject(comp->slot);
				if (testSlotOn(comp)) compReset(comp, RES_DEFAULT);
			});
		});
		act->setEnabled(slot->data != NULL);
	}
	drvMenu->addActions(dskMenu->actions());
	for (QMenu* m : menus) m->menuAction()->setVisible(!m->isEmpty());
}

// The divMMC in the menus: off, the item that puts it on; on, a menu with the item
// that takes it off and its two slots. The toolbar button's arrow opens the menu
// either way, so it always has the switch.
void MainWin::fillDivMenu() {
	Computer* comp = conf.zx;
	for (QAction* a : divMenu->actions())
		if (a->menu()) a->menu()->deleteLater();
	divMenu->clear();
	bool fits = hw_takes_divmmc(comp->hw);
	bool on = fits && comp->div->model;
	divItem->setText(divAct->text());		// with its key
	divItem->setEnabled(fits);
	divItem->setVisible(!on);
	divMenu->menuAction()->setVisible(on);
	QAction* sw = divMenu->addAction(QIcon(":/images/sdcard.png"), divAct->text(), this, [this]() {divToggle();});
	sw->setCheckable(true);
	sw->setChecked(on);
	sw->setEnabled(fits);
	if (!on) return;
	divMenu->addSeparator();
	addDivCards(divMenu);
}

// the board's two slots, a submenu each
void MainWin::addDivCards(QMenu* menu) {
	for (int i = 0; i < 2; i++) {
		SDCard* sdc = conf.zx->div->card[i];
		QMenu* m = menu->addMenu(QIcon(":/images/sdcard.png"),
			QString("Card %0: %1").arg(i + 1).arg(sdc->image ? drive_media(sdc->image, true) : QString("(no card)")));
		fillCardMenu(m, i);
	}
}

// The board in or out, as the divMMC row of Options puts it: with the power off,
// so the machine comes up as after a Hard Reset, and into the machine's own file.
// Its RAM and jumper stay as they were; the drives and the hard disk go out with
// it on and come back with their disks (comp_board_storage). The restart is
// asked about first, unless the caller has asked already.
void MainWin::divToggle(bool ask) {
	Computer* comp = conf.zx;
	if (!hw_takes_divmmc(comp->hw)) {
		setMessage(" no divMMC on this machine ");
		return;
	}
	bool on = !comp->div->model;
	if (ask) {
		QString text = "Performs a Hard Reset. Memory contents are lost.";
		if (on && ((comp->dif->cfg != DIF_NONE) || (comp->ide->cfg != IDE_NONE)))
			text += "\nFloppy drives and hard disk are disabled while divMMC is on.";
		if (on && comp->zc->on)
			text += "\nThe Z-Controller is removed: one SD card board at a time.";
		QMessageBox box(QMessageBox::Question, on ? "Switch divMMC on" : "Switch divMMC off",
			text, QMessageBox::NoButton, this);
		QPushButton* go = box.addButton(on ? "Switch On" : "Switch Off", QMessageBox::AcceptRole);
		box.addButton(QMessageBox::Cancel);
		box.setDefaultButton(go);
		pause(true, PR_EXTRA);
		box.exec();
		pause(false, PR_EXTRA);
		syncActions();		// a click on the button ticked it already
		if (box.clickedButton() != go) return;
	}
	emu_lock();
	div_set_model(comp->div, on ? DIV_MMC : DIV_NONE, comp->div->pages);
	comp_board_storage(comp);
	xm_power_cycle();
	emu_unlock();
	xm_save_over();
	setMessage(on ? " divMMC on " : " divMMC off ");
	fillDrivesMenu();
	if (diskWin->isVisible()) diskWin->refresh();
	syncActions();
}

// esxDOS starts only when the EEPROM and the card's system files are the same
// release, and says nothing useful when they are not. Looked at once a power
// cycle and once a card: a folder in slot 1 whose SYS is a bundled release other
// than the chip's is told. An image card is not looked into.
void MainWin::esxCheck() {
	Computer* comp = conf.zx;
	if (!comp->div->model) {
		esxSeen.clear();
		return;
	}
	const char* img = comp->div->card[0]->image;
	QString card = img ? QString::fromLocal8Bit(img) : QString();
	QString key = QString("%0|%1").arg(xm_power_count()).arg(card);
	if (key == esxSeen) return;
	esxSeen = key;
	if (card.isEmpty() || !QFileInfo(card).isDir()) return;
	std::string chip = div_esx_release_of(comp->div->data);
	std::string sys = div_esx_card_release(img);
	if (chip.empty() || sys.empty() || (chip == sys)) return;
	pause(true, PR_EXTRA);
	QMessageBox::warning(this, "esxDOS releases differ",
		QString("Card 1 has the system files of esxDOS %0, the EEPROM holds esxDOS %1. "
		"esxDOS starts only when both are the same release.\n\n"
		"Write esxDOS %0 to the EEPROM in Options (divMMC, Firmware), or make the card again "
		"with New esxDOS Folder.").arg(sys.c_str()).arg(chip.c_str()));
	pause(false, PR_EXTRA);
}

// A folder mounted read/write changes the user's own files, so the first time
// the machine writes to one, it is held in memory until this is answered. Asked
// once for good: the answer is the setting in Options.
void MainWin::folderAsk() {
	static bool asking = false;
	if (asking) return;
	if (vfat_pin_news(conf.zx->sdc->vfat))
		setMessage(" saves lost on reset: put the game on the card ", 8.0);
	if (xm_next_wants_card()) nextCard(true);
	vFat* vf = fold_waiting(conf.zx);
	if (!vf) return;
	asking = true;
	pause(true, PR_EXTRA);
	QMessageBox box(QMessageBox::Warning, "Writing to a folder",
		QString("The machine is writing to the folder\n%0\n\n"
		"With Read/write on, what it saves, changes or deletes there changes the files "
		"in the folder itself, and a deleted file %1. Keep a copy of anything you cannot "
		"afford to lose.\n\n"
		"Read-only keeps what the machine writes in memory until the folder is mounted "
		"again. Both can be changed in Options (Emulation, Folder as a disk): Read-only "
		"takes effect at once, Read/write at the next Hard Reset.")
		.arg(QDir::toNativeSeparators(QString::fromUtf8(vf->root ? vf->root : "")))
		.arg(conf.fold.trash ? "goes to the trash" : "is deleted permanently"), QMessageBox::NoButton, this);
	QPushButton* allow = box.addButton("Allow writing", QMessageBox::AcceptRole);
	QPushButton* ro = box.addButton("Read-only", QMessageBox::RejectRole);
	box.setDefaultButton(allow);
	box.setEscapeButton(ro);
	box.exec();
	bool yes = (box.clickedButton() == allow);
	emu_lock();
	if (yes)
		fold_release(conf.zx);
	else
		fold_read_only(conf.zx, "writing to it was not allowed");	// every folder in use, as Options' R/O
	emu_unlock();
	conf.fold.asked = 1;
	if (!yes) conf.fold.rw = 0;
	saveConfig();
	pause(false, PR_EXTRA);
	asking = false;
}

// The Next's own card, which it takes whenever the slot has none it can start from.
// Asked for when the Next has nothing to start from, or from the SD Card menu; a card
// chosen goes in at once, with the power off and on.
void MainWin::nextCard(bool asked) {
	static bool open = false;
	if (open) return;
	open = true;
	QString path;
	bool own = false;
	pause(true, PR_FILE);
	bool ok = next_card_dialog(this, &path, &own, asked);
	pause(false, PR_FILE);
	open = false;
	if (!ok) return;
	conf.next.card = path.toLocal8Bit().constData();
	conf.next.own = own ? 1 : 0;
	saveConfig();
	driveOp([]() {
		xm_next_card(true);
		xm_power_cycle();
	});
}

// A folder that stopped following the machine, or could not take some of what it wrote,
// says so once: what the machine wrote there may otherwise be lost without a word.
void MainWin::folderNews() {
	static bool telling = false;
	if (telling) return;
	QString msg = fold_news(conf.zx);
	if (msg.isEmpty()) return;
	telling = true;
	pause(true, PR_EXTRA);
	QMessageBox::warning(this, "Writing to a folder", msg);
	pause(false, PR_EXTRA);
	telling = false;
}

// what a floppy drive's menu and the Disk manager's buttons do
void MainWin::diskOp(int op, int drv) {
	switch (op) {
		case DW_OPEN:
			// a disk put into a named drive, as the Tape Player's open: nothing starts
			openMedia(QString(), FH_DRIVE_A + drv, drv, AR_MOUNT, true);
			if (diskWin->isVisible()) diskWin->refresh();
			break;
		case DW_NEW: diskNew(drv); break;
		case DW_SAVE: diskSave(drv, false); break;
		case DW_SAVE_AS: diskSave(drv, true); break;
		case DW_EJECT: diskEject(drv); break;
	}
}

// the question about a changed tape can still keep it in
void MainWin::tapeEject() {
	pause(true, PR_FILE);
	int safe = saveChangedTape(conf.zx);
	pause(false, PR_FILE);
	if (safe != ERR_OK) return;
	tapEject(conf.zx->tape);
	emit s_tape_upd(conf.zx->tape);
	emit s_tape_blk(conf.zx->tape);
}

// a change under the running machine
void MainWin::driveOp(std::function<void()> fn) {
	pause(true, PR_FILE);
	emu_lock();
	fn();
	emu_unlock();
	pause(false, PR_FILE);
	if (diskWin->isVisible()) diskWin->refresh();
}

void MainWin::cardEject(int card) {
	driveOp([card]() {
		sdc_mount(sd_card(conf.zx, card), QString());
		xm_next_card();		// the Next takes its own card back
	});
}

// the SD Card: Eject key's card: the slot's on a machine with one, else the first of the
// divMMC's that holds one
void MainWin::cardEjectKey() {
	Computer* comp = conf.zx;
	int card = -2;
	if (comp_has_sd(comp)) {
		card = -1;
	} else if (comp->div->model) {
		card = comp->div->card[0]->image ? 0 : 1;
	}
	if ((card < -1) || !sd_card(comp, card)->image) {
		setMessage(" no card ");
		return;
	}
	cardEject(card);
	// the Next's own card goes back in
	setMessage(sd_card(comp, card)->image ? " own card back in " : " card ejected ");
}

// what a refresh brought in, or why some of it was not done
void MainWin::folderSaid(const vfRefresh& r, const QString& why) {
	if (!why.isEmpty()) {
		pause(true, PR_EXTRA);
		QMessageBox::warning(this, "Refresh", why);
		pause(false, PR_EXTRA);
	}
	if (r.added || r.removed || r.changed)
		setMessage(QString(" refreshed: %0 new, %1 gone, %2 changed ").arg(r.added).arg(r.removed).arg(r.changed));
	else if (why.isEmpty())
		setMessage(" refreshed: no changes ");
}

// the folder's changes into the card or disk, the machine running on (a real card put back is dead to it)
void MainWin::folderRefresh(bool ide, int which) {
	vfRefresh r;
	QString why;
	driveOp([&]() {
		why = ide ? ide_refresh(conf.zx->ide, which, &r) : sdc_refresh(sd_card(conf.zx, which), &r);
	});
	folderSaid(r, why);
}

// The SD Card: Refresh key: every folder card the machine has in use - one with no change
// writes nothing, so which one was meant needs no asking
void MainWin::folderRefreshAll() {
	Computer* comp = conf.zx;
	struct {vFat* vf; const char* name;} fold[3];
	int n = 0;
	if (comp_has_sd(comp) && comp->sdc->vfat)
		fold[n++] = {comp->sdc->vfat, "SD card"};
	if (comp->div->model) {
		if (comp->div->card[0]->vfat) fold[n++] = {comp->div->card[0]->vfat, "divMMC card 1"};
		if (comp->div->card[1]->vfat) fold[n++] = {comp->div->card[1]->vfat, "divMMC card 2"};
	}
	if (!n) {
		setMessage(" no folder card ");
		return;
	}
	vfRefresh sum = {0, 0, 0, 0};
	QStringList whys;
	driveOp([&]() {
		for (int i = 0; i < n; i++) {
			vfRefresh r;
			QString why = fold_refresh(fold[i].vf, &r);
			sum.added += r.added;
			sum.removed += r.removed;
			sum.changed += r.changed;
			if (!why.isEmpty()) whys << ((n > 1) ? QString("%0: %1").arg(fold[i].name).arg(why) : why);
		}
	});
	folderSaid(sum, whys.join("\n\n"));
}

void MainWin::diskNew(int drv) {
	Computer* comp = conf.zx;
	pause(true, PR_FILE);
	if (saveChangedDisk(comp, drv) == ERR_OK) {
		bool fmt = (comp->dif->type == DIF_BDI) && areSure("Format for TR-DOS?");
		emu_lock();
		Floppy* flp = comp->dif->flp[drv];
		flp_insert(flp, NULL);
		flp->changed = 1;
		if (fmt) trd_format(flp);
		emu_unlock();
	}
	pause(false, PR_FILE);
	if (diskWin->isVisible()) diskWin->refresh();
}

// Save writes back to the file the disk came from, Save as asks where
void MainWin::diskSave(int drv, bool ask) {
	Computer* comp = conf.zx;
	Floppy* flp = comp->dif->flp[drv];
	if (!flp->insert) return;
	pause(true, PR_FILE);
	if (ask)
		save_file(comp, "", FG_DISK_A + drv, drv);
	else
		save_back(comp, flp->path, FG_DISK_A + drv, drv);
	pause(false, PR_FILE);
	if (diskWin->isVisible()) diskWin->refresh();
}

// the question about a changed disk can still keep it in the drive
void MainWin::diskEject(int drv) {
	Computer* comp = conf.zx;
	pause(true, PR_FILE);
	if (saveChangedDisk(comp, drv) == ERR_OK) {
		emu_lock();
		flp_eject(comp->dif->flp[drv]);
		emu_unlock();
	}
	pause(false, PR_FILE);
	if (diskWin->isVisible()) diskWin->refresh();
}

void MainWin::hddOpen(int dev, bool dir) {
	pause(true, PR_FILE);
	QString path = dir
		? QFileDialog::getExistingDirectory(this, "Folder to serve as the disk", "", QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly)
		: QFileDialog::getOpenFileName(this, "Hard disk image", "", "All files (*)", NULL, QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite);
	pause(false, PR_FILE);
	if (!path.isEmpty()) driveOp([dev, path]() {ide_mount(conf.zx->ide, dev, path);});
}

void MainWin::sdcOpen(int card, bool dir) {
	pause(true, PR_FILE);
	QString path = dir
		? QFileDialog::getExistingDirectory(this, "Folder to serve as the card", "", QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly)
		: QFileDialog::getOpenFileName(this, "SD card image", "", "All files (*)", NULL, QFileDialog::DontUseNativeDialog);
	pause(false, PR_FILE);
	if (!path.isEmpty()) driveOp([card, path]() {
		sdc_mount(sd_card(conf.zx, card), path);
		xm_next_card();		// one the Next cannot boot from is set aside
	});
}

static bool copy_tree(const QString& src, const QString& dst) {
	if (!QDir().mkpath(dst)) return false;
	QDirIterator it(src, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		QString path = it.next();
		QString to = dst + "/" + QDir(src).relativeFilePath(path);
		if (it.fileInfo().isDir()) {
			if (!QDir().mkpath(to)) return false;
		} else if (!QFile::copy(path, to)) {
			return false;
		}
	}
	return true;
}

// the bundled release a new card takes when the EEPROM holds none of them
static QString esx_ask_release(QWidget* parent) {
	QDialog dlg(parent);
	dlg.setWindowTitle("New esxDOS Folder");
	xOptSheet sheet;
	QComboBox* box = new QComboBox;
	for (int i = 0; div_esx_releases[i]; i++)
		box->addItem(QString("esxDOS %0").arg(div_esx_releases[i]), QString(div_esx_releases[i]));
	sheet.field("Release", box, "The EEPROM holds none of them: write the same one there in Options");
	QVBoxLayout* lay = new QVBoxLayout(&dlg);
	lay->addWidget(sheet.body);
	QDialogButtonBox* bbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	bbox->button(QDialogButtonBox::Ok)->setIcon(QIcon(":/images/ok-apply.png"));
	bbox->button(QDialogButtonBox::Cancel)->setIcon(QIcon(":/images/cancel.png"));
	lay->addWidget(bbox);
	QObject::connect(bbox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect(bbox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	return (dlg.exec() == QDialog::Accepted) ? box->currentData().toString() : QString();
}

// A card ready for esxDOS: a folder with the system files of the release the
// EEPROM holds (both must be the same one), for the user to fill. Put in at
// once, with the power off and on, so esxDOS finds it.
void MainWin::sdcNewEsx(int card) {
	pause(true, PR_FILE);
	QString rel = QString::fromStdString(div_esx_release_of(conf.zx->div->data));
	if (rel.isEmpty()) rel = esx_ask_release(this);
	pause(false, PR_FILE);
	if (rel.isEmpty()) return;
	QString esx = QString::fromLocal8Bit(div_esx_dir(rel.toStdString()).c_str());
	if (!QDir(esx + "/SYS").exists() || !QDir(esx + "/BIN").exists()) {
		shitHappens("The esxDOS files are missing from the config folder");
		return;
	}
	pause(true, PR_FILE);
	QString path = QFileDialog::getExistingDirectory(this, QString("Folder for the esxDOS %0 card").arg(rel), "", QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly);
	pause(false, PR_FILE);
	if (path.isEmpty()) return;
	QDir dir(path);
	if (dir.exists("SYS") || dir.exists("BIN")) {
		shitHappens("This folder has SYS or BIN already. Open it with Open Folder");
		return;
	}
	if (!copy_tree(esx + "/SYS", path + "/SYS") || !copy_tree(esx + "/BIN", path + "/BIN") || !dir.mkpath("TMP")) {
		shitHappens("Can't copy the esxDOS files into this folder");
		return;
	}
	driveOp([card, path]() {
		sdc_mount(sd_card(conf.zx, card), path);
		xm_power_cycle();
	});
}

// What the image in a hard disk is. The geometry comes from the image and is
// only shown; LBA is the drive's to take or not.
void MainWin::hddProps(int id) {
	IDE* ide = conf.zx->ide;
	ATADev* dev = (id == IDE_SLAVE) ? ide->slave : ide->master;
	if (!dev->image) return;
	ATAPassport pass = ideGetPassport(ide, id);
	QDialog dlg(this);
	dlg.setWindowTitle(QString("Hard Disk: %0").arg((id == IDE_SLAVE) ? "Slave" : "Master"));
	xOptSheet sheet;
	QLabel* img = new QLabel(QFileInfo(QString::fromLocal8Bit(dev->image)).fileName());
	img->setToolTip(QString::fromLocal8Bit(dev->image));
	sheet.field("Image", img);
	sheet.field("C / H / S", new QLabel(QString("%0 / %1 / %2").arg(pass.cyls).arg(pass.hds).arg(pass.spt)));
	sheet.field("Capacity", new QLabel(QString("%0 MB, %1 sectors").arg(dev->maxlba >> 11).arg(dev->maxlba)));
	sheet.line();
	QCheckBox* lba = new QCheckBox;
	lba->setChecked(dev->hasLBA);
	sheet.check(lba, "LBA", "The drive takes sector numbers, not only cylinder, head and sector");
	QVBoxLayout* lay = new QVBoxLayout(&dlg);
	lay->addWidget(sheet.body);
	QDialogButtonBox* bbox = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
	bbox->button(QDialogButtonBox::Close)->setIcon(QIcon(":/images/cancel.png"));
	lay->addWidget(bbox);
	connect(bbox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	pause(true, PR_FILE);
	dlg.exec();
	pause(false, PR_FILE);
	emu_lock();
	dev->hasLBA = lba->isChecked() ? 1 : 0;
	emu_unlock();
}

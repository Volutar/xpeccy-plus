#include <QFileDialog>
#include <QFileInfo>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QPushButton>
#include <QMenu>

#include "emulwin.h"
#include "filer.h"
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
	QString nam = !in ? QString("(empty)") : (path && *path) ? QFileInfo(QString::fromLocal8Bit(path)).fileName() : QString("(new disk)");
	return nam.replace("&", "&&");
}

int drive_count(Computer* comp) {
	return (comp->dif->type == DIF_BDI) ? 4 : (comp->dif->type == DIF_P3DOS) ? 2 : 0;
}

void MainWin::fillDrivesMenu() {
	Computer* comp = conf.zx;
	QAction* act;
	QMenu* menus[] = {dskMenu, hddMenu, sdcMenu, cartMenu};
	for (QMenu* m : menus) m->clear();
	int drives = drive_count(comp);
	for (int i = 0; i < drives; i++) {
		Floppy* flp = comp->dif->flp[i];
		if (!flp->fitted) continue;
		// the drive's letter is on its icon
		QMenu* m = dskMenu->addMenu(QIcon(QString(":/images/fdd_disk_%0.png").arg(QChar('A' + i))),
			drive_media(flp->path, flp->insert) + ((flp->insert && flp->changed) ? " *" : ""));
		m->addAction(QIcon(":/images/fileopen.png"), "Open...", this, [this, i]() {diskOp(DW_OPEN, i);});
		m->addAction(QIcon(":/images/doc-new.png"), "New", this, [this, i]() {diskOp(DW_NEW, i);});
		act = m->addAction(QIcon(":/images/save_all.png"), "Save", this, [this, i]() {diskOp(DW_SAVE, i);});
		act->setEnabled(flp->insert && flp->path && *flp->path);
		act = m->addAction(QIcon(":/images/floppy.png"), "Save as...", this, [this, i]() {diskOp(DW_SAVE_AS, i);});
		act->setEnabled(flp->insert);
		act = m->addAction(QIcon(":/images/tape-eject.png"), "Eject", this, [this, i]() {diskOp(DW_EJECT, i);});
		act->setEnabled(flp->insert);
		m->addSeparator();
		act = m->addAction("Write protect");
		act->setCheckable(true);
		act->setChecked(flp->protect);
		connect(act, &QAction::toggled, this, [i](bool on) {conf.zx->dif->flp[i]->protect = on ? 1 : 0;});
	}

	if (comp->ide->type != IDE_NONE) {
		ATADev* dev[2] = {comp->ide->master, comp->ide->slave};
		for (int i = 0; i < 2; i++) {
			if (dev[i]->type == IDE_NONE) continue;
			int id = i ? IDE_SLAVE : IDE_MASTER;
			QMenu* m = hddMenu->addMenu(QIcon(":/images/hdd.png"),
				QString("%0: %1").arg(i ? "Slave" : "Master").arg(drive_media(dev[i]->image, dev[i]->image != NULL)));
			m->addAction(QIcon(":/images/fileopen.png"), "Open image...", this, [this, id]() {hddOpen(id, false);});
			m->addAction("Open folder...", this, [this, id]() {hddOpen(id, true);});
			act = m->addAction(QIcon(":/images/tape-eject.png"), "Eject", this, [this, id]() {
				driveOp([id]() {ide_mount(conf.zx->ide, id, QString());});
			});
			act->setEnabled(dev[i]->image != NULL);
			m->addSeparator();
			act = m->addAction("Properties...", this, [this, id]() {hddProps(id);});
			act->setEnabled(dev[i]->image != NULL);
		}
	}

	int hw = comp->hw->id;
	if ((hw == HW_PENTEVO) || (hw == HW_TSLAB)) {
		SDCard* sdc = comp->sdc;
		bool dir = sdc->image && QFileInfo(QString::fromLocal8Bit(sdc->image)).isDir();
		act = sdcMenu->addAction(sdc->image ? drive_media(sdc->image, true) : QString("(no card)"));
		act->setEnabled(false);
		sdcMenu->addSeparator();
		sdcMenu->addAction(QIcon(":/images/fileopen.png"), "Open image...", this, [this]() {sdcOpen(false);});
		sdcMenu->addAction("Open folder...", this, [this]() {sdcOpen(true);});
		act = sdcMenu->addAction(QIcon(":/images/tape-eject.png"), "Eject", this, [this]() {
			driveOp([]() {sdc_mount(conf.zx->sdc, QString());});
		});
		act->setEnabled(sdc->image != NULL);
		sdcMenu->addSeparator();
		// the card's lock switch; a folder is served read only whatever it says
		act = sdcMenu->addAction("Write protect");
		act->setCheckable(true);
		act->setChecked(sdc->lock || dir);
		act->setEnabled(!dir);
		connect(act, &QAction::toggled, this, [](bool on) {sdcSetLock(conf.zx->sdc, on ? 1 : 0);});
	}

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
	for (QMenu* m : menus) m->menuAction()->setVisible(!m->isEmpty());
}

// what a floppy drive's menu and the Disk manager's buttons do
void MainWin::diskOp(int op, int drv) {
	switch (op) {
		case DW_OPEN:
			openMedia(QString(), FH_DRIVE_A + drv, drv, conf.autorun);
			if (diskWin->isVisible()) diskWin->refresh();
			break;
		case DW_NEW: diskNew(drv); break;
		case DW_SAVE: diskSave(drv, false); break;
		case DW_SAVE_AS: diskSave(drv, true); break;
		case DW_EJECT: diskEject(drv); break;
	}
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

void MainWin::diskNew(int drv) {
	Computer* comp = conf.zx;
	pause(true, PR_FILE);
	if (saveChangedDisk(comp, drv) != ERR_CANCEL) {
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
	save_file(comp, (ask || !flp->path) ? "" : flp->path, FG_DISK_A + drv, drv);
	pause(false, PR_FILE);
	if (diskWin->isVisible()) diskWin->refresh();
}

// the question about a changed disk can still keep it in the drive
void MainWin::diskEject(int drv) {
	Computer* comp = conf.zx;
	pause(true, PR_FILE);
	if (saveChangedDisk(comp, drv) != ERR_CANCEL) {
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
		? QFileDialog::getExistingDirectory(this, "Folder to serve as the disk (read only)", "", QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly)
		: QFileDialog::getOpenFileName(this, "Hard disk image", "", "All files (*)", NULL, QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite);
	pause(false, PR_FILE);
	if (!path.isEmpty()) driveOp([dev, path]() {ide_mount(conf.zx->ide, dev, path);});
}

void MainWin::sdcOpen(bool dir) {
	pause(true, PR_FILE);
	QString path = dir
		? QFileDialog::getExistingDirectory(this, "Folder to serve as the card (read only)", "", QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly)
		: QFileDialog::getOpenFileName(this, "SD card image", "", "All files (*)", NULL, QFileDialog::DontUseNativeDialog);
	pause(false, PR_FILE);
	if (!path.isEmpty()) driveOp([path]() {sdc_mount(conf.zx->sdc, path);});
}

// What the image in a hard disk is. The geometry comes from the image and is
// only shown; LBA is the drive's to take or not.
void MainWin::hddProps(int id) {
	IDE* ide = conf.zx->ide;
	ATADev* dev = (id == IDE_SLAVE) ? ide->slave : ide->master;
	if (!dev->image) return;
	ATAPassport pass = ideGetPassport(ide, id);
	QDialog dlg(this);
	dlg.setWindowTitle(QString("Hard disk: %0").arg((id == IDE_SLAVE) ? "slave" : "master"));
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

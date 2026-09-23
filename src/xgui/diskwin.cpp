#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QFileInfo>

#include "diskwin.h"
#include "xgui.h"
#include "../xcore/xcore.h"
#include "../libxpeccy/filetypes/filetypes.h"
#include "../filer.h"

static QToolButton* diskButton(const char* icon, const char* tip) {
	QToolButton* btn = new QToolButton;
	btn->setIcon(QIcon(icon));
	btn->setToolTip(tip);
	return btn;
}

xDiskWin::xDiskWin(QWidget* p):QDialog(p) {
	setWindowTitle("Disk manager");
	setWindowIcon(QIcon(":/images/fdd_disk.png"));
	tabs = new QTabBar;
	tabs->setExpanding(false);
	list = new xDiskCatTable;
	// as the list on the old Disk tab was: whole rows, several at once, no row numbers
	list->setEditTriggers(QAbstractItemView::NoEditTriggers);
	list->setAlternatingRowColors(true);
	list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	list->setSelectionBehavior(QAbstractItemView::SelectRows);
	list->verticalHeader()->hide();
	list->horizontalHeader()->setStretchLastSection(false);
	// a selected row does not make the headings bold
	list->horizontalHeader()->setHighlightSections(false);
	// what is in the drive, above what is on it
	path = new QLineEdit;
	path->setReadOnly(true);
	btnOpen = diskButton(":/images/fileopen.png", "Open a disk image");
	btnNew = diskButton(":/images/doc-new.png", "Insert a new disk");
	btnSave = diskButton(":/images/save_all.png", "Save to the file it came from");
	btnSaveAs = diskButton(":/images/floppy.png", "Save as...");
	btnEject = diskButton(":/images/tape-eject.png", "Eject");
	protect = new QCheckBox("Write protect");
	QHBoxLayout* top = new QHBoxLayout;
	top->addWidget(path, 1);
	top->addWidget(btnOpen);
	top->addWidget(btnNew);
	top->addWidget(btnSave);
	top->addWidget(btnSaveAs);
	top->addWidget(btnEject);
	top->addWidget(protect);
	note = new QLabel;
	toTape = diskButton(":/images/tape.png", "Copy to tape");
	toHobeta = diskButton(":/images/dollar.png", "Save as Hobeta");
	toRaw = diskButton(":/images/filebin.png", "Save as raw");
	QVBoxLayout* btns = new QVBoxLayout;
	btns->addWidget(toTape);
	btns->addWidget(toHobeta);
	btns->addWidget(toRaw);
	btns->addStretch(1);
	QHBoxLayout* mid = new QHBoxLayout;
	mid->addWidget(list, 1);
	mid->addLayout(btns);
	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->addWidget(tabs);
	lay->addLayout(top);
	lay->addLayout(mid, 1);
	head = new QLabel;
	QHBoxLayout* bottom = new QHBoxLayout;
	bottom->addWidget(note, 1);
	bottom->addWidget(head);
	lay->addLayout(bottom);
	resize(560, 360);
	watch = new QTimer(this);
	connect(watch, &QTimer::timeout, this, [this]() {watchDrives();});
	watch->start(200);
	connect(tabs, &QTabBar::currentChanged, this, [this]() {fill();});
	connect(list->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {pickedChanged();});
	connect(btnOpen, &QToolButton::released, this, [this]() {doOp(DW_OPEN);});
	connect(btnNew, &QToolButton::released, this, [this]() {doOp(DW_NEW);});
	connect(btnSave, &QToolButton::released, this, [this]() {doOp(DW_SAVE);});
	connect(btnSaveAs, &QToolButton::released, this, [this]() {doOp(DW_SAVE_AS);});
	connect(btnEject, &QToolButton::released, this, [this]() {doOp(DW_EJECT);});
	connect(protect, &QCheckBox::clicked, this, [this](bool on) {
		int drv = drive();
		if (drv >= 0) conf.zx->dif->flp[drv]->protect = on ? 1 : 0;
	});
	connect(toTape, &QToolButton::released, this, [this]() {copyToTape();});
	connect(toHobeta, &QToolButton::released, this, [this]() {saveFiles(true);});
	connect(toRaw, &QToolButton::released, this, [this]() {saveFiles(false);});
}

// The debugger's font, and the columns worked out from it: the name takes
// what is left, the two 16-bit figures share a width, and so do the three that
// never run past three digits. A new catalog resets the model and with it every
// section's size, so this runs after each one.
void xDiskWin::setColumns() {
	list->setFont(conf.dbg.font);
	QHeaderView* hdr = list->horizontalHeader();
	// a theme that styles the headings draws them in its own font whatever
	// setFont says, so the font goes in as a rule of the header's own
	QFont fnt = conf.dbg.font;
	QString size = (fnt.pointSizeF() > 0) ? QString("%0pt").arg(fnt.pointSizeF()) : QString("%0px").arg(fnt.pixelSize());
	hdr->setStyleSheet(QString("QHeaderView::section {font-family: \"%0\"; font-size: %1; font-weight: bold;}").arg(fnt.family()).arg(size));
	QFontMetrics fm(conf.dbg.font);
	QFont bold = conf.dbg.font;
	bold.setBold(true);
	QFontMetrics fmb(bold);		// the headings are bold
	int wide = qMax(fmb.horizontalAdvance("Length"), fm.horizontalAdvance("65535")) + 12;
	int narrow = qMax(fmb.horizontalAdvance("SecLen"), fm.horizontalAdvance("000")) + 12;
	hdr->setSectionResizeMode(0, QHeaderView::Stretch);
	hdr->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	for (int i = 2; i < 7; i++) {
		hdr->setSectionResizeMode(i, QHeaderView::Fixed);
		hdr->resizeSection(i, (i < 4) ? wide : narrow);
	}
	list->verticalHeader()->setMinimumSectionSize(fm.height() + 4);
	list->verticalHeader()->setDefaultSectionSize(fm.height() + 4);
}

// one tab per drive the interface has, up to the last one fitted
static int tab_count() {
	Computer* comp = conf.zx;
	int cnt = drive_count(comp);
	while ((cnt > 0) && !comp->dif->flp[cnt - 1]->fitted) cnt--;
	return cnt;
}

// The TR-DOS system sector: track 0, sector 9. NULL on a disk that has none.
static unsigned char* trd_sys(Floppy* flp, unsigned char* buf) {
	if (!flp->insert || (diskGetType(flp) != DISK_TYPE_TRD)) return NULL;
	return diskGetSectorData(flp, 0, 9, buf, 256) ? buf : NULL;
}

// The disk's own name. TR-DOS keeps 8 bytes for it, but prints the 3 after them
// as well, and some disks use all 11.
static QString trd_label(const unsigned char* sys) {
	QString res;
	for (int i = 0xf5; i < 0x100; i++) {
		if (sys[i] == 0) break;
		res.append(((sys[i] > 0x1f) && (sys[i] < 0x7f)) ? QChar(sys[i]) : QChar('?'));
	}
	return res.trimmed();
}

// all a drive shows: what is in it and its catalog, so a change to either is seen
QByteArray xDiskWin::driveState(int drv) {
	Floppy* flp = conf.zx->dif->flp[drv];
	QByteArray res;
	res.append(char(flp->insert | (flp->changed << 1) | (flp->protect << 2)));
	if (flp->path) res.append(flp->path);
	unsigned char buf[256];
	if (trd_sys(flp, buf)) {
		res.append((char*)buf, 256);
		for (int sec = 1; sec < 9; sec++) {
			if (!diskGetSectorData(flp, 0, sec, buf, 256)) break;
			res.append((char*)buf, 256);
		}
	}
	return res;
}

// The disk's name, or the file's if it has none; the drive's letter is on the
// tab's icon, and the file itself in the line under the tabs.
QString xDiskWin::driveName(int drv) {
	Floppy* flp = conf.zx->dif->flp[drv];
	QString nam;
	unsigned char buf[256];
	if (trd_sys(flp, buf)) nam = trd_label(buf).replace("&", "&&");
	if (nam.isEmpty()) nam = drive_media(flp->path, flp->insert);
	nam = QFontMetrics(tabs->font()).elidedText(nam, Qt::ElideMiddle, 160);
	return (flp->insert && flp->changed) ? nam + " *" : nam;
}

void xDiskWin::refresh() {
	int cnt = tab_count();
	int cur = tabs->currentIndex();
	tabs->blockSignals(true);
	while (tabs->count() > 0) tabs->removeTab(0);
	seen.clear();
	for (int i = 0; i < cnt; i++) {
		tabs->addTab(QIcon(QString(":/images/fdd_disk_%0.png").arg(QChar('A' + i))), driveName(i));
		seen.append(driveState(i));
	}
	tabs->setCurrentIndex(qBound(0, cur, cnt - 1));
	tabs->blockSignals(false);
	fill();
}

// The machine writes to its disks and other windows put files on them; what
// changed is shown again, and only that.
void xDiskWin::watchDrives() {
	if (!isVisible()) return;
	if (tab_count() != tabs->count()) {
		refresh();
		return;
	}
	for (int i = 0; i < tabs->count(); i++) {
		QByteArray st = driveState(i);
		if (st == seen[i]) continue;
		seen[i] = st;
		tabs->setTabText(i, driveName(i));
		if (i == drive()) fill();
	}
	showHead();
}

// Where the head of the drive shown stands while its motor runs, and the file
// under it in bold. Trk in the catalog is TR-DOS's logical track, both sides
// counted, so the head's is worked out the same way.
void xDiskWin::showHead() {
	int drv = drive();
	Floppy* flp = (drv < 0) ? NULL : conf.zx->dif->flp[drv];
	FDC* fdc = conf.zx->dif->fdc;
	if (!flp || !flp->insert || !flp->motor || (fdc->flp != flp)) {
		head->clear();
		list->setLive(-1);
		return;
	}
	int trk = (sides > 1) ? (flp->trk * 2 + fdc->side) : flp->trk;
	head->setText(QString("Head on track %0").arg(trk));
	int row = -1;
	for (int i = 0; (i < files.size()) && (row < 0); i++) {
		int first = files[i].trk * 16 + files[i].sec;
		int last = first + files[i].slen - 1;
		if ((last >= trk * 16) && (first < (trk + 1) * 16)) row = i;
	}
	list->setLive(row);
}

void xDiskWin::showWindow() {
	refresh();
	show();
	raise();
	activateWindow();
}

int xDiskWin::drive() {
	return tabs->currentIndex();
}

void xDiskWin::doOp(int op) {
	int drv = drive();
	if ((drv < 0) || !diskOp) return;
	diskOp(op, drv);
}

void xDiskWin::fillDrive(Floppy* flp) {
	bool fit = flp && flp->fitted;	// a drive left out takes nothing
	bool in = flp && flp->insert;
	bool file = in && flp->path && *flp->path;
	path->setText(!in ? QString() : file ? QString::fromLocal8Bit(flp->path) : QString("(new disk)"));
	path->setCursorPosition(0);
	btnOpen->setEnabled(fit);
	btnNew->setEnabled(fit);
	btnSave->setEnabled(file);
	btnSaveAs->setEnabled(in);
	btnEject->setEnabled(in);
	protect->setEnabled(fit);
	protect->setChecked(flp && flp->protect);
}

void xDiskWin::fill() {
	QList<TRFile> cat;
	QStringList txt;
	rows.clear();
	sides = 2;
	int drv = drive();
	Floppy* flp = (drv < 0) ? NULL : conf.zx->dif->flp[drv];
	fillDrive(flp);
	unsigned char buf[256];
	if (!flp || !flp->insert) {
		txt.append("No disk in the drive");
	} else if (!trd_sys(flp, buf)) {
		txt.append("Not a TR-DOS disk: there is no catalog to show");
	} else {
		TRFile ct[128];
		int cnt = diskGetTRCatalog(flp, ct);
		for (int i = 0; i < cnt; i++) {
			if (ct[i].name[0] > 0x1f) {
				cat.append(ct[i]);
				rows.append(i);
			}
		}
		QString lab = trd_label(buf);
		if (!lab.isEmpty()) txt.append(QString("\"%0\"").arg(lab));
		txt.append(QString("%0 files, %1 deleted").arg(cat.size()).arg(buf[0xf4]));
		txt.append(QString("%0 sectors free").arg(buf[0xe5] | (buf[0xe6] << 8)));
		// 0x16 and 0x17 are the two double sided types
		sides = ((buf[0xe3] == 0x16) || (buf[0xe3] == 0x17)) ? 2 : 1;
	}
	if (flp && flp->insert && flp->changed)
		txt.append("modified");
	note->setText(txt.join(", "));
	files = cat;
	list->setCatalog(cat);
	setColumns();
	list->setEnabled(!cat.isEmpty());
	pickedChanged();
	showHead();
}

QList<int> xDiskWin::picked() {
	QList<int> res;
	foreach(QModelIndex idx, list->selectionModel()->selectedRows()) {
		if (idx.row() < rows.size()) res.append(rows[idx.row()]);
	}
	return res;
}

void xDiskWin::pickedChanged() {
	bool any = !picked().isEmpty();
	toTape->setEnabled(any);
	toHobeta->setEnabled(any);
	toRaw->setEnabled(any);
}

void xDiskWin::copyToTape() {
	QList<int> sel = picked();
	Computer* comp = conf.zx;
	Floppy* flp = comp->dif->flp[drive()];
	TRFile cat[128];
	diskGetTRCatalog(flp, cat);
	unsigned char* buf = new unsigned char[0xffff];
	char name[10];
	int saved = 0;
	foreach(int n, sel) {
		TRFile& f = cat[n];
		if (!diskGetSectorsData(flp, f.trk, f.sec + 1, buf, f.slen)) {
			shitHappens("Can't get file data, skip");
		} else if (f.slen != (f.hlen + ((f.llen == 0) ? 0 : 1))) {
			shitHappens("File seems to be joined, skip");
		} else {
			unsigned short start = ((f.hst << 8) + f.lst) & 0xffff;
			unsigned short len = ((f.hlen << 8) + f.llen) & 0xffff;
			unsigned short line = (f.ext == 'B') ? (buf[start] + (buf[start + 1] << 8)) & 0xffff : 0x8000;
			memset(name, 0x20, 10);
			memcpy(name, (char*)f.name, 8);
			tapAddFile(comp->tape, name, (f.ext == 'B') ? 0 : 3, start, len, line, buf, true);
			saved++;
		}
	}
	delete[] buf;
	if (tapeChanged) tapeChanged();
	showInfo(QString("%0 of %1 files copied").arg(saved).arg(sel.size()).toLocal8Bit().constData());
}

void xDiskWin::saveFiles(bool hobeta) {
	QList<int> sel = picked();
	QString dir = QFileDialog::getExistingDirectory(this, "Save file(s) to...", "", QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly);
	if (dir.isEmpty()) return;
	std::string sdir = std::string(dir.toLocal8Bit().data()) + SLASH;
	Floppy* flp = conf.zx->dif->flp[drive()];
	int saved = 0;
	foreach(int n, sel) {
		int err = hobeta ? saveHobetaFile(flp, n, sdir.c_str()) : saveRawFile(flp, n, sdir.c_str());
		if (err == ERR_OK) saved++;
	}
	showInfo(QString("%0 of %1 files saved").arg(saved).arg(sel.size()).toLocal8Bit().constData());
}

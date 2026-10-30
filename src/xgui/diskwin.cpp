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
	setWindowTitle("Disk Manager");
	setProperty("xCenterOnce", true);
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
	btnSave = diskButton(":/images/floppy.png", "Save the changes to the disk's own file");
	btnSaveAs = diskButton(":/images/save-as.png", "Save the disk to a new file");
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
	toRaw = diskButton(":/images/filebin.png", "Save as RAW");
	QVBoxLayout* btns = new QVBoxLayout;
	btns->addWidget(toTape);
	btns->addWidget(toHobeta);
	btns->addWidget(toRaw);
	btns->addStretch(1);
	QHBoxLayout* mid = new QHBoxLayout;
	mid->addWidget(list, 1);
	mid->addLayout(btns);
	// all the drives', so beside their tabs and not under them
	fast = new QCheckBox("Fast disk access");
	fast->setToolTip("No head-seek and rotation delays");
	QHBoxLayout* row = new QHBoxLayout;
	row->addWidget(tabs, 1);
	row->addWidget(fast);
	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->addLayout(row);
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
	connect(fast, &QCheckBox::clicked, this, [](bool on) {
		setFlagBit(on, &fdcFlag, FDC_FAST);
		saveConfig();
	});
	connect(toTape, &QToolButton::released, this, [this]() {copyToTape();});
	connect(toHobeta, &QToolButton::released, this, [this]() {saveFiles(true);});
	connect(toRaw, &QToolButton::released, this, [this]() {saveFiles(false);});
}

// The debugger's font, and the columns worked out from it: the name takes
// what is left, the other text fits its contents, the two 16-bit figures share
// a width, and so do the ones that never run past three digits. A new catalog
// resets the model and with it every section's size, so this runs after each one.
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
	int ntext = list->textColumns();
	hdr->setSectionResizeMode(0, QHeaderView::Stretch);
	for (int i = 1; i < hdr->count(); i++) {
		if (i < ntext) {
			hdr->setSectionResizeMode(i, QHeaderView::ResizeToContents);
		} else {
			hdr->setSectionResizeMode(i, QHeaderView::Fixed);
			hdr->resizeSection(i, (i < ntext + 2) ? wide : narrow);
		}
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
	p3Disk dsk;
	if (trd_sys(flp, buf)) {
		res.append((char*)buf, 256);
		for (int sec = 1; sec < 9; sec++) {
			if (!diskGetSectorData(flp, 0, sec, buf, 256)) break;
			res.append((char*)buf, 256);
		}
	} else if (p3_disk(flp, &dsk)) {
		QByteArray dir(dsk.dirblk * dsk.blklen, 0);
		dir.resize(p3_dir_raw(flp, &dsk, (unsigned char*)dir.data(), dir.size()));
		res.append(dir);
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
	fast->setChecked(fdcFlag & FDC_FAST);	// the menu and Options set it too
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

// Trk in the catalog is the file system's logical track, both sides counted,
// so the head's is worked out the same way
int xDiskWin::headTrack(Floppy* flp, int side) {
	if (kind == DK_P3) return p3_logical(&p3dsk, flp->trk, side);
	return (sides > 1) ? (flp->trk * 2 + side) : flp->trk;
}

// Where the head of the drive shown stands while its motor runs, and the file
// under it in bold.
void xDiskWin::showHead() {
	int drv = drive();
	Floppy* flp = (drv < 0) ? NULL : conf.zx->dif->flp[drv];
	FDC* fdc = conf.zx->dif->fdc;
	if (!flp || !flp->insert || !flp->motor || (fdc->flp != flp)) {
		head->clear();
		list->setLive(-1);
		return;
	}
	int trk = headTrack(flp, fdc->side);
	head->setText(QString("Head on track %0").arg(trk));
	int row = -1;
	for (int i = 0; (i < rowTrks.size()) && (row < 0); i++) {
		if (rowTrks[i].contains(trk)) row = i;
	}
	list->setLive(row);
}

void xDiskWin::showWindow() {
	fast->setChecked(fdcFlag & FDC_FAST);
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

static const QStringList trdHeads = {"Name", "Ext", "Start", "Length", "SecLen", "Trk", "Sec"};

void xDiskWin::fillTRD(Floppy* flp, const unsigned char* buf, QStringList& txt) {
	kind = DK_TRD;
	TRFile ct[128];
	QList<QVariantList> lst;
	int cnt = diskGetTRCatalog(flp, ct);
	for (int i = 0; i < cnt; i++) {
		TRFile& f = ct[i];
		if (f.name[0] < 0x20) continue;
		lst.append({QString(QByteArray((char*)f.name, 8)), QString(QChar(f.ext)), f.lst | (f.hst << 8),
			f.llen | (f.hlen << 8), f.slen, f.trk, f.sec});
		rows.append(i);
		QList<int> trks;
		for (int t = f.trk; t <= f.trk + (f.sec + f.slen - 1) / 16; t++) trks.append(t);
		rowTrks.append(trks);
	}
	list->setRows(trdHeads, 2, lst);
	QString lab = trd_label(buf);
	if (!lab.isEmpty()) txt.append(QString("\"%0\"").arg(lab));
	txt.append(QString("%0 files, %1 deleted").arg(lst.size()).arg(buf[0xf4]));
	txt.append(QString("%0 sectors free").arg(buf[0xe5] | (buf[0xe6] << 8)));
	// 0x16 and 0x17 are the two double sided types
	sides = ((buf[0xe3] == 0x16) || (buf[0xe3] == 0x17)) ? 2 : 1;
}

// Start is the autostart line of a program, the address of code. Size is what
// the file takes on the disk; Trk and Sec are where its first block is.
void xDiskWin::fillP3(Floppy* flp, QStringList& txt) {
	static const char* types[] = {"Program", "Numbers", "Chars", "Bytes"};
	static const char* formats[] = {"+3DOS disk", "CPC system disk", "CPC data disk", "+3DOS disk"};
	kind = DK_P3;
	p3files.resize(p3dsk.dirblk * p3dsk.blklen / 32);		// a file takes an entry at least
	int cnt = p3_catalog(flp, &p3dsk, p3files.data(), p3files.size());
	p3files.resize(cnt);
	QList<QVariantList> lst;
	for (int i = 0; i < cnt; i++) {
		p3File& f = p3files[i];
		QString nam = QString::fromLatin1(f.name);
		if (f.user) nam.prepend(QString("%0:").arg(f.user));		// the user area, when not 0
		QVariantList row = {nam, QString::fromLatin1(f.ext)};
		if (f.hdr && (f.type < 4)) {
			row.append(types[f.type]);
			bool line = (f.type == 0) && (f.param1 < 10000);
			row.append(((f.type == 3) || line) ? QVariant(f.param1) : QVariant());
			row.append(f.len);
		} else {
			row.append({QVariant(), QVariant(), QVariant()});
		}
		QList<int> trks;
		for (int k = 0; k < f.nblk; k++) {
			if (!f.blk[k]) continue;
			int last;
			for (int t = p3_block_track(&p3dsk, f.blk[k], &last, NULL); t <= last; t++) {
				if (trks.isEmpty() || (trks.last() != t)) trks.append(t);	// blocks mostly run in order
			}
		}
		row.append(QString("%0K").arg(f.used * p3dsk.blklen / 1024));
		if ((f.nblk > 0) && f.blk[0]) {
			int sec;
			int trk = p3_block_track(&p3dsk, f.blk[0], NULL, &sec);
			row.append({trk, sec});
		}
		lst.append(row);
		rows.append(i);
		rowTrks.append(trks);
	}
	list->setRows({"Name", "Ext", "Type", "Start", "Length", "Size", "Trk", "Sec"}, 3, lst);
	txt.append(formats[p3dsk.format & 3]);
	txt.append(QString("%0 files").arg(cnt));
	txt.append(QString("%0K free").arg(p3_free_blocks(&p3dsk, p3files.data(), cnt) * p3dsk.blklen / 1024));
}

void xDiskWin::fill() {
	QStringList txt;
	rows.clear();
	rowTrks.clear();
	p3files.clear();
	kind = DK_NONE;
	sides = 2;
	int drv = drive();
	Floppy* flp = (drv < 0) ? NULL : conf.zx->dif->flp[drv];
	fillDrive(flp);
	unsigned char buf[256];
	DiskIF* dif = conf.zx->dif;
	if (!flp && dif->out && (dif->cfg != DIF_NONE)) {
		txt.append("The drives are off while the divMMC is on");
	} else if (!flp || !flp->insert) {
		txt.append("No disk in the drive");
	} else if (trd_sys(flp, buf)) {
		fillTRD(flp, buf, txt);
	} else if (p3_disk(flp, &p3dsk)) {
		fillP3(flp, txt);
	} else {
		txt.append("Neither TR-DOS nor +3DOS: there is no catalog to show");
	}
	if (kind == DK_NONE)
		list->setRows(trdHeads, 2, {});
	if (flp && flp->insert && flp->changed)
		txt.append("modified");
	note->setText(txt.join(", "));
	setColumns();
	list->setEnabled(!rows.isEmpty());
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
	if (kind == DK_P3) {
		p3ToTape(sel);
		return;
	}
	Computer* comp = conf.zx;
	Floppy* flp = comp->dif->flp[drive()];
	TRFile cat[128];
	diskGetTRCatalog(flp, cat);
	unsigned char* buf = new unsigned char[0x10000]();
	char name[9];
	int saved = 0;
	foreach(int n, sel) {
		TRFile& f = cat[n];
		if (!diskGetSectorsData(flp, f.trk, f.sec + 1, buf, f.slen)) {
			shitHappens("Can't get file data, skip");
			continue;
		}
		int line;
		int size = trd_file_len(&f, buf, &line);
		if (size < 0) {
			shitHappens("File seems to be joined, skip");
			continue;
		}
		memcpy(name, (char*)f.name, 8);
		name[8] = 0;
		int len = f.llen | (f.hlen << 8);
		if (f.ext == 'B') {
			tapAddFile(comp->tape, name, 0, size, line, len, buf, true);
		} else {
			tapAddFile(comp->tape, name, 3, size, f.lst | (f.hst << 8), 0x8000, buf, true);
		}
		saved++;
	}
	delete[] buf;
	if (tapeChanged) tapeChanged();
	showInfo(QString("%0 of %1 files copied").arg(saved).arg(sel.size()).toLocal8Bit().constData());
}

void xDiskWin::saveFiles(bool hobeta) {
	QList<int> sel = picked();
	QString dir = QFileDialog::getExistingDirectory(this, "Save file(s) to...", "", QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly);
	if (dir.isEmpty()) return;
	if (kind == DK_P3) {
		p3Save(sel, dir, hobeta);
		return;
	}
	std::string sdir = std::string(dir.toLocal8Bit().data()) + SLASH;
	Floppy* flp = conf.zx->dif->flp[drive()];
	int saved = 0;
	foreach(int n, sel) {
		int err = hobeta ? saveHobetaFile(flp, n, sdir.c_str()) : saveRawFile(flp, n, sdir.c_str());
		if (err == ERR_OK) saved++;
	}
	showInfo(QString("%0 of %1 files saved").arg(saved).arg(sel.size()).toLocal8Bit().constData());
}

// +3DOS

// Only a file with a +3DOS header has the type, address and length a tape
// header needs; it is the same 7 bytes.
void xDiskWin::p3ToTape(QList<int> sel) {
	Computer* comp = conf.zx;
	Floppy* flp = comp->dif->flp[drive()];
	int saved = 0;
	foreach(int n, sel) {
		p3File& f = p3files[n];
		if (!f.hdr || (f.type > 3)) {
			shitHappens("File has no +3DOS header, skip");
			continue;
		}
		QByteArray buf(f.records * 128, 0);
		unsigned char* ptr = (unsigned char*)buf.data();
		if (p3_read(flp, &p3dsk, &f, ptr, buf.size()) < f.len + 128) {
			shitHappens("Can't get file data, skip");
			continue;
		}
		tapAddFile(comp->tape, f.name, f.type, f.len, f.param1, f.param2, ptr + 128, true);
		saved++;
	}
	if (tapeChanged) tapeChanged();
	showInfo(QString("%0 of %1 files copied").arg(saved).arg(sel.size()).toLocal8Bit().constData());
}

// a file with no +3DOS header saves raw only
void xDiskWin::p3Save(QList<int> sel, QString dir, bool hobeta) {
	Floppy* flp = conf.zx->dif->flp[drive()];
	std::string sdir = dir.toLocal8Bit().data();
	int saved = 0;
	foreach(int n, sel) {
		p3File* f = &p3files[n];
		int err = hobeta ? p3_save_hobeta(flp, &p3dsk, f, sdir.c_str()) : p3_save_raw(flp, &p3dsk, f, sdir.c_str());
		if (err == ERR_OK) saved++;
	}
	showInfo(QString("%0 of %1 files saved").arg(saved).arg(sel.size()).toLocal8Bit().constData());
}

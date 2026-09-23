#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QFileInfo>

#include "diskwin.h"
#include "xgui.h"
#include "../xcore/xcore.h"
#include "../libxpeccy/filetypes/filetypes.h"

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
	lay->addLayout(mid, 1);
	lay->addWidget(note);
	resize(560, 360);
	connect(tabs, &QTabBar::currentChanged, this, [this]() {fill();});
	connect(list->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {pickedChanged();});
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

// one tab per drive the interface has: four on a Beta Disk, two on a +3
void xDiskWin::refresh() {
	Computer* comp = conf.zx;
	int cnt = (comp->dif->type == DIF_BDI) ? 4 : (comp->dif->type == DIF_P3DOS) ? 2 : 0;
	int cur = tabs->currentIndex();
	tabs->blockSignals(true);
	while (tabs->count() > 0) tabs->removeTab(0);
	while ((cnt > 0) && !comp->dif->flp[cnt - 1]->fitted) cnt--;
	for (int i = 0; i < cnt; i++) {
		Floppy* flp = comp->dif->flp[i];
		QString nam = flp->insert ? (flp->path ? QFileInfo(QString::fromLocal8Bit(flp->path)).fileName() : QString("(new disk)")) : QString("(empty)");
		tabs->addTab(QIcon(QString(":/images/fdd_disk_%0.png").arg(QChar('A' + i))), nam);
	}
	tabs->setCurrentIndex(qBound(0, cur, cnt - 1));
	tabs->blockSignals(false);
	fill();
}

void xDiskWin::showDrive(int drv) {
	refresh();
	tabs->setCurrentIndex(drv);
	show();
	raise();
	activateWindow();
}

int xDiskWin::drive() {
	return tabs->currentIndex();
}

void xDiskWin::fill() {
	QList<TRFile> cat;
	rows.clear();
	int drv = drive();
	Floppy* flp = (drv < 0) ? NULL : conf.zx->dif->flp[drv];
	if (!flp || !flp->insert) {
		note->setText("No disk in the drive");
	} else if (diskGetType(flp) != DISK_TYPE_TRD) {
		note->setText("Not a TR-DOS disk: there is no catalog to show");
	} else {
		TRFile ct[128];
		int cnt = diskGetTRCatalog(flp, ct);
		for (int i = 0; i < cnt; i++) {
			if (ct[i].name[0] > 0x1f) {
				cat.append(ct[i]);
				rows.append(i);
			}
		}
		note->setText(QString("%0 files").arg(cat.size()));
	}
	list->setCatalog(cat);
	setColumns();
	list->setEnabled(!cat.isEmpty());
	pickedChanged();
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

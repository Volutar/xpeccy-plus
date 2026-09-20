#include <QApplication>
#include <QFileDialog>
#include <QStyleOptionButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "xcore/xcore.h"
#include "dbg_brkpoints.h"

// Model

xBreakListModel::xBreakListModel(QObject* par):xTableModel(par) {
	lastRows = 0;
}

int xBreakListModel::rowCount(const QModelIndex&) const {
		return conf.brk.list.size();
}

int xBreakListModel::columnCount(const QModelIndex&) const {
	return 7;
}

QString brkGetString(xBrkPoint brk) {
	QString res;
	switch(brk.type) {
		case BRK_CPUADR:
			res = QString("CPU:%0").arg(gethexword(brk.adr));
			if (brk.eadr > brk.adr) {
				res.append(QString("-%0").arg(gethexword(brk.eadr)));
			}
			break;
		case BRK_IOPORT:
			res = QString("IO:%0 mask %1").arg(gethexword(brk.adr)).arg(gethexword(brk.mask));
			break;
		case BRK_MEMRAM:
			if (brk.eadr > brk.adr) {
				res = QString("RAM:%0-%1 [%2:%3-%4:%5]").arg(gethex6(brk.adr)).arg(gethex6(brk.eadr)).arg(gethexbyte(brk.adr >> 14)).arg(gethexword(brk.adr & 0x3fff)).arg(gethexbyte(brk.eadr >> 14)).arg(gethexword(brk.eadr & 0x3fff));
			} else {
				res = QString("RAM:%0 [%1:%2]").arg(gethex6(brk.adr)).arg(gethexbyte(brk.adr >> 14)).arg(gethexword(brk.adr & 0x3fff));
			}
			break;
		case BRK_MEMROM:
			if (brk.eadr > brk.adr) {
				res = QString("ROM:%0-%1 [%2:%3-%4:%5]").arg(gethex6(brk.adr)).arg(gethex6(brk.eadr)).arg(gethexbyte(brk.adr >> 14)).arg(gethexword(brk.adr & 0x3fff)).arg(gethexbyte(brk.eadr >> 14)).arg(gethexword(brk.eadr & 0x3fff));
			} else {
				res = QString("ROM:%0 [%1:%2]").arg(gethex6(brk.adr)).arg(gethexbyte(brk.adr >> 14)).arg(gethexword(brk.adr & 0x3fff));
			}
			break;
		case BRK_MEMSLT:
			res = QString("SLT:%0 [%1:%2]").arg(gethex6(brk.adr)).arg(gethexbyte(brk.adr >> 14)).arg(gethexword(brk.adr & 0x3fff));
			break;
		case BRK_MEMEXT:
			res = QString("EXT:%0 [%1:%2]").arg(gethex6(brk.adr)).arg(gethexbyte(brk.adr >> 14)).arg(gethexword(brk.adr & 0x3fff));
			break;
		case BRK_IRQ:
			res = QString("IRQ");
			break;
		case BRK_COND:
			res = brk.onchg ? QString("COND, on change") : QString("COND");
			break;
	}
	return res;
}

QVariant xBreakListModel::data(const QModelIndex& idx, int role) const {
	QVariant res;
	if (!idx.isValid()) return res;
	int row = idx.row();
	int col = idx.column();
	if ((col < 0) || (col >= columnCount())) return res;
	if ((row < 0) || (row >= rowCount())) return res;
	xBrkPoint brk = conf.brk.list[row];
	switch (role) {
		case Qt::CheckStateRole:
			switch(col) {
				case 0: res = brk.off ? Qt::Unchecked : Qt::Checked; break;
				case 1: if ((brk.type != BRK_IRQ) && (brk.type != BRK_IOPORT) && (brk.type != BRK_COND)) res = brk.fetch ? Qt::Checked : Qt::Unchecked; break;
				case 2: if ((brk.type != BRK_IRQ) && (brk.type != BRK_COND)) res = brk.read ? Qt::Checked : Qt::Unchecked; break;
				case 3: if ((brk.type != BRK_IRQ) && (brk.type != BRK_COND)) res = brk.write ? Qt::Checked : Qt::Unchecked; break;
			}
			break;
		case Qt::TextAlignmentRole:
			if (col == 6) res = (int)(Qt::AlignRight | Qt::AlignVCenter);
			break;
		case Qt::DisplayRole:
			switch(col) {
				case 4: res = brkGetString(brk); break;
				case 5: res = QString(brk.cond.c_str()); break;
				case 6: res = brk.count; break;
			}
			break;
	}
	return res;
}

QVariant xBreakListModel::headerData(int sect, Qt::Orientation ornt, int role) const {
	QVariant res;
	switch(ornt) {
		case Qt::Horizontal:
			if (sect < 0) break;
			if (sect >= columnCount()) break;
			switch(role) {
				case Qt::DisplayRole:
					switch(sect) {
						case 0: res = "On"; break;
						case 1: res = "F"; break;
						case 2: res = "R"; break;
						case 3: res = "W"; break;
						case 4: res = "Addr"; break;
						case 5: res = "Cond"; break;
						case 6: res = "Cnt"; break;
					}
					break;
			}
			break;
		case Qt::Vertical:
			break;
	}
	return res;
}

bool xbsOff(const xBrkPoint bpa, const xBrkPoint bpb) {return (bpa.off && !bpb.off);}
bool xbsFe(const xBrkPoint bpa, const xBrkPoint bpb) {return (bpa.fetch && !bpb.fetch);}
bool xbsRd(const xBrkPoint bpa, const xBrkPoint bpb) {return (bpa.read && !bpb.read);}
bool xbsWr(const xBrkPoint bpa, const xBrkPoint bpb) {return (bpa.write && !bpb.write);}
bool xbsCnt(const xBrkPoint bpa, const xBrkPoint bpb) {return (bpa.count < bpb.count);}
bool xbsName(const xBrkPoint bpa, const xBrkPoint bpb) {
	return brkGetString(bpa) < brkGetString(bpb);
}
bool xbsCond(const xBrkPoint bpa, const xBrkPoint bpb) {return (bpa.cond < bpb.cond);}

void xBreakListModel::sort(int col, Qt::SortOrder ord) {
		switch(col) {
		case 0: std::sort(conf.brk.list.begin(), conf.brk.list.end(), xbsOff); break;
		case 1: std::sort(conf.brk.list.begin(), conf.brk.list.end(), xbsFe); break;
		case 2: std::sort(conf.brk.list.begin(), conf.brk.list.end(), xbsRd); break;
		case 3: std::sort(conf.brk.list.begin(), conf.brk.list.end(), xbsWr); break;
		case 4: std::sort(conf.brk.list.begin(), conf.brk.list.end(), xbsName); break;
		case 5: std::sort(conf.brk.list.begin(), conf.brk.list.end(), xbsCond); break;
		case 6: std::sort(conf.brk.list.begin(), conf.brk.list.end(), xbsCnt); break;
	}
	emit dataChanged(index(0,0), index(rowCount() - 1, columnCount() - 1));
}

// a full reset makes the view forget the column widths, so do it only when the
// list really changed size

void xBreakListModel::update() {
	int rows = rowCount();
	if (rows == lastRows) {
		if (rows > 0)
			emit dataChanged(index(0, 0), index(rows - 1, columnCount() - 1));
	} else {
		beginResetModel();
		lastRows = rows;
		endResetModel();
	}
}

// Widget

xBreakTable::xBreakTable(QWidget* p):QTableView(p) {
	model = new xBreakListModel();
	setModel(model);
	xCheckItem* chk = new xCheckItem(this);
	for (int i = 0; i < 4; i++)
		setItemDelegateForColumn(i, chk);
	addrWidth = 0;		// no width from the user yet: share the room with Cond
	setcol = 0;
	applyColumns();
	connect(model, &QAbstractItemModel::modelReset, this, &xBreakTable::applyColumns);
	connect(horizontalHeader(), &QHeaderView::sectionResized, this, &xBreakTable::colResized);
	connect(this, SIGNAL(clicked(QModelIndex)), this, SLOT(onCellClick(QModelIndex)));
	connect(this, SIGNAL(doubleClicked(QModelIndex)), this, SLOT(onDoubleClick(QModelIndex)));
}

// Addr and Cond start out the same width, except that Addr never falls below
// what the addresses in it need: one added from the disasm has to fit whole,
// and it is the longer of the two. Cond keeps a minimum whatever they ask for

int xBreakTable::defaultAddrWidth() {
	int rest = viewport()->width() - 4 * BRK_CHK_WIDTH - columnWidth(6);
	int wid = rest / 2;
	int need = sizeHintForColumn(4);	// the widest address in the list
	if (wid < need) wid = need;
	if (wid > rest - BRK_COND_WIDTH) wid = rest - BRK_COND_WIDTH;
	if (wid < BRK_ADR_WIDTH) wid = BRK_ADR_WIDTH;	// a panel too narrow for both: Addr wins
	return wid;
}

// checkboxes and the counter keep their width whatever happens to the model,
// Addr keeps the width the user gave it - or the default above until they give
// one - and Cond takes the rest

void xBreakTable::applyColumns() {
	QHeaderView* hdr = horizontalHeader();
	int i;
	setcol = 1;
	hdr->setStretchLastSection(false);	// Cond stretches instead, Cnt fits its value
	for (i = 0; i < 4; i++) {
		hdr->setSectionResizeMode(i, QHeaderView::Fixed);
		setColumnWidth(i, BRK_CHK_WIDTH);
	}
	hdr->setSectionResizeMode(6, QHeaderView::ResizeToContents);
	hdr->setSectionResizeMode(4, QHeaderView::Interactive);
	setColumnWidth(4, addrWidth ? addrWidth : defaultAddrWidth());
	hdr->setSectionResizeMode(5, QHeaderView::Stretch);
	setcol = 0;
}

// Qt hands every section a default width of its own accord while the view is
// built, and that is not a width the user chose. Only a drag on the edge is, so
// take one only while the button that does the dragging is down

void xBreakTable::colResized(int col, int, int siz) {
	if (setcol || !(QApplication::mouseButtons() & Qt::LeftButton)) return;
	if (col != 4) return;
	if (siz > 0) addrWidth = siz;
}

// until the user sets a width of their own the two halves follow the panel

void xBreakTable::resizeEvent(QResizeEvent* ev) {
	QTableView::resizeEvent(ev);
	if (!addrWidth) applyColumns();
}

void xBreakTable::update() {
	model->update();
	QTableView::update();
}

// the header drops the widths set here when the font or style changes, and the
// debugger hands its font to every panel after the window is built

void xBreakTable::changeEvent(QEvent* ev) {
	QTableView::changeEvent(ev);
	switch (ev->type()) {
		case QEvent::FontChange:
		case QEvent::StyleChange:
			applyColumns();
			break;
		default:
			break;
	}
}

void xBreakTable::keyPressEvent(QKeyEvent* ev) {
	ev->ignore();
}

void xBreakTable::onCellClick(QModelIndex idx) {
	if (!idx.isValid()) return;
	int row = idx.row();
	int col = idx.column();
	xBrkPoint* brk = &conf.brk.list[row];
	// no checkbox means the flag doesn't apply here (or the column isn't one) - a click does nothing
	if (!idx.data(Qt::CheckStateRole).isValid()) return;
	switch(col) {
		case 0: brk->off ^= 1; break;
		case 1: brk->fetch ^= 1; break;
		case 2: brk->read ^= 1; break;
		case 3: brk->write ^= 1; break;
	}
	// brkInstall(conf.brk.list[row], 0);
	brkInstallAll();
	model->updateCell(row, col);
	emit rqDasmDump();
}

void xBreakTable::onDoubleClick(QModelIndex idx) {
	if (!idx.isValid()) return;
	int row = idx.row();
	xBrkPoint bp = conf.brk.list[row];
	int adr = -1;
	switch(bp.type) {
		case BRK_CPUADR: adr = bp.adr; break;
		case BRK_MEMRAM: adr = memFindAdr(conf.zx->mem, MEM_RAM, bp.adr); break;
		case BRK_MEMROM: adr = memFindAdr(conf.zx->mem, MEM_ROM, bp.adr); break;
	}
	if (adr < 0) return;
	emit rqDisasm(adr);
}

// Dialog

xBrkManager::xBrkManager(QWidget* p):QDialog(p) {
	ui.setupUi(this);
	helpWin = nullptr;
	ui.labCondErr->setWordWrap(true);

	ui.brkType->addItem("ADR bus (MEM)", BRK_CPUADR);
	ui.brkType->addItem("ADR bus (IO)", BRK_IOPORT);
	ui.brkType->addItem("RAM cell", BRK_MEMRAM);
	ui.brkType->addItem("ROM cell", BRK_MEMROM);
	ui.brkType->addItem("SLT cell", BRK_MEMSLT);
	ui.brkType->addItem("IRQ", BRK_IRQ);
	ui.brkType->addItem("Condition (global)", BRK_COND);

	ui.brkAdrEnd->setMin(0x0000);
	ui.brkAdrEnd->setMax(0xffffff);
	ui.brkAdrHex->setMin(0x0000);
	ui.brkAdrHex->setMax(0xffffff);

	ui.leStartOffset->setMin(0);
	ui.leStartOffset->setMax(0x3fff);
	ui.leEndOffset->setMin(0);
	ui.leEndOffset->setMax(0x3fff);
	ui.leValue->setMin(0);
	ui.leValue->setMax(0xff);
	ui.leValMask->setMin(0);
	ui.leValMask->setMax(0xff);
	ui.leValMask->setValue(0xff);

	ui.brkAction->addItem("Debugger", BRK_ACT_DBG);
	ui.brkAction->addItem("Counter", BRK_ACT_COUNT);
	ui.brkAction->addItem("Screen dump (ZX only)", BRK_ACT_SCR);

	connect(ui.brkBank, SIGNAL(valueChanged(int)), this, SLOT(bnkChanged(int)));
	connect(ui.leStartOffset,SIGNAL(valueChanged(int)),this,SLOT(startOffChanged(int)));
	connect(ui.brkAdrHex,SIGNAL(valueChanged(int)),this,SLOT(startAbsChanged(int)));
	connect(ui.leEndOffset,SIGNAL(valueChanged(int)),this,SLOT(endOffChanged(int)));
	connect(ui.brkAdrEnd,SIGNAL(valueChanged(int)),this,SLOT(endAbsChanged(int)));

	connect(ui.brkType, SIGNAL(currentIndexChanged(int)), this, SLOT(chaType(int)));
	connect(ui.leCond, SIGNAL(textChanged(QString)), this, SLOT(chaCond(QString)));
	connect(ui.tbCondHelp, SIGNAL(clicked()), this, SLOT(condHelp()));
//	connect(ui.brkAdrHex, SIGNAL(valueChanged(int)), ui.brkAdrEnd, SLOT(setMin(int)));
	connect(ui.pbOK, SIGNAL(clicked()), this, SLOT(confirm()));
}

void xBrkManager::bnkChanged(int v) {
//	ui.brkAdrHex->blockSignals(true);
//	ui.brkAdrEnd->blockSignals(true);
	ui.brkAdrHex->setValue((v << 14) | (ui.leStartOffset->getValue() & 0x3fff));
	ui.brkAdrEnd->setMin((v << 14) | (ui.leStartOffset->getValue() & 0x3fff));
	ui.brkAdrEnd->setValue((v << 14) | (ui.leEndOffset->getValue() & 0x3fff));
//	ui.brkAdrHex->blockSignals(false);
//	ui.brkAdrEnd->blockSignals(false);
}

void xBrkManager::startOffChanged(int v) {
//	ui.brkAdrHex->blockSignals(true);
	ui.leEndOffset->setMin(v);
	v = (ui.brkBank->value() << 14) | (v & 0x3fff);
	ui.brkAdrHex->setValue(v);
	ui.brkAdrEnd->setMin(v);
//	ui.brkAdrEnd->setValue(v);
//	ui.brkAdrHex->blockSignals(false);
}

void xBrkManager::startAbsChanged(int v) {
//	ui.brkBank->blockSignals(true);
//	ui.leStartOffset->blockSignals(true);
	ui.brkBank->setValue(v >> 14);
	ui.leStartOffset->setValue(v & 0x3fff);
//	ui.brkBank->blockSignals(false);
//	ui.leStartOffset->blockSignals(false);
	ui.brkAdrEnd->setMin(v);
	ui.leEndOffset->setMin(v & 0x3fff);
//	ui.brkAdrEnd->setValue(v);
}

void xBrkManager::endOffChanged(int v) {
//	ui.brkAdrEnd->blockSignals(true);
	ui.brkAdrEnd->setValue((ui.brkBank->value() << 14) | (v & 0x3fff));
//	ui.brkAdrEnd->blockSignals(false);
}

void xBrkManager::endAbsChanged(int v) {
//	ui.leEndOffset->blockSignals(true);
	ui.leEndOffset->setValue(v & 0x3fff);
//	ui.leEndOffset->blockSignals(true);
}

#define EL_CHG 2048
#define EL_CND 1024
#define EL_VAL 512
#define EL_FE 256
#define EL_RD 128
#define EL_WR 64
#define EL_BNK 32
#define EL_SOF 16
#define EL_SAD 8
#define EL_EOF 4
#define EL_EAD 2
#define EL_MSK 1

void xBrkManager::setElements(int mask) {
	ui.brkFetch->setVisible(mask & EL_FE);
	ui.brkRead->setVisible(mask & EL_RD);
	ui.brkWrite->setVisible(mask & EL_WR);
	ui.brkOnChange->setVisible(mask & EL_CHG);
	ui.labFlags->setVisible(mask & (EL_FE | EL_RD | EL_WR | EL_CHG));
	ui.brkBank->setVisible(mask & EL_BNK);
	ui.labBank->setVisible(mask & EL_BNK);
	ui.leStartOffset->setVisible(mask & EL_SOF);
	ui.labStartOff->setVisible(mask & EL_SOF);
	ui.brkAdrHex->setVisible(mask & EL_SAD);
	ui.labStartAbs->setVisible(mask & EL_SAD);
	ui.leEndOffset->setVisible(mask & EL_EOF);
	ui.labEndOff->setVisible(mask & EL_EOF);
	ui.brkAdrEnd->setVisible(mask & EL_EAD);
	ui.labEndAbs->setVisible(mask & EL_EAD);
	ui.brkMaskHex->setVisible(mask & EL_MSK);
	ui.labMask->setVisible(mask & EL_MSK);

	ui.labValue->setVisible(mask & EL_VAL);
	ui.labValMask->setVisible(mask & EL_VAL);
	ui.leValue->setVisible(mask & EL_VAL);
	ui.leValMask->setVisible(mask & EL_VAL);

	ui.labCond->setVisible(mask & EL_CND);
	ui.leCond->setVisible(mask & EL_CND);
	ui.tbCondHelp->setVisible(mask & EL_CND);
	ui.labCondErr->setVisible(mask & EL_CND);
}

// max address depends on what the address means: a cpu/io one is limited by the
// bus, a memory cell one by the size of ram/rom/slot

void xBrkManager::setLimits(int t) {
	Computer* comp = conf.zx;
	int max;
	switch (t) {
		case BRK_CPUADR:
		case BRK_IOPORT: max = comp->mem->busmask; break;
		case BRK_MEMRAM: max = comp->mem->ramMask; break;
		case BRK_MEMROM: max = comp->mem->romMask; break;
		case BRK_MEMSLT: max = comp->slot->memMask; break;
		default: max = 0xffffff; break;
	}
	if (max < 0xffff) max = 0xffff;
	ui.brkAdrHex->setMax(max);
	ui.brkAdrEnd->setMax(max);
	ui.brkBank->setMaximum(max >> 14);
}

void xBrkManager::chaType(int i) {
	int t = ui.brkType->itemData(i).toInt();
	setLimits(t);
	chaCond(ui.leCond->text());		// the note about HITS depends on the type
	switch (t) {
		case BRK_IRQ:
			setElements(EL_CND);
			break;
		case BRK_COND:				// the condition and how it fires
			setElements(EL_CND | EL_CHG);
			break;
		case BRK_IOPORT:
			setElements(EL_CND | EL_RD | EL_WR | EL_SAD | EL_MSK);
			break;
		case BRK_CPUADR:
			setElements(EL_CND | EL_FE | EL_RD | EL_WR | EL_SAD | EL_EAD);
			break;
		default:
			setElements(EL_CND | EL_FE | EL_RD | EL_WR | EL_BNK | EL_SOF | EL_SAD | EL_EOF | EL_EAD);
			break;
	}
}

// check the expression as it is typed

void xBrkManager::chaCond(QString str) {
	xExpr exp = xexpr_compile(str.trimmed().toLocal8Bit().data());
	if (str.trimmed().isEmpty()) {
		ui.labCondErr->setStyleSheet("");
		ui.labCondErr->setText("");
	} else if (xexpr_ok(exp)) {
		// show how it was understood: a wrong base or priority shows up at once
		QString res = QString("ok: %0").arg(xexpr_text(exp).c_str());
		// HITS of a global condition counts its own firings, so it can't be used
		// to skip the first N of anything - that needs an address breakpoint
		if ((ui.brkType->itemData(ui.brkType->currentIndex()).toInt() == BRK_COND)
			&& xexpr_uses_var(exp, XV_HITS)) {
			ui.labCondErr->setStyleSheet("color:#c17d11");
			res.append(QString(QChar(10)) + "HITS counts what this condition fired, so it can limit"
				" (HITS < 3) but not skip. To stop on the Nth hit of an"
				" address, make it an address breakpoint with HITS > N-1");
		} else {
			ui.labCondErr->setStyleSheet("");
		}
		ui.labCondErr->setText(res);
	} else {
		ui.labCondErr->setStyleSheet("color:#ef2929");
		ui.labCondErr->setText(QString("%0 at %1").arg(exp.err.c_str()).arg(exp.errpos));
	}
}

void xBrkManager::condHelp() {
	if (!helpWin) {
		// the text lives in res/help/cond-syntax.html, built into the binary
		QFile file(":/res/help/cond-syntax.html");
		QString txt = "no help text in this build";
		if (file.open(QFile::ReadOnly)) {
			txt = QString::fromUtf8(file.readAll());
			file.close();
		}
		helpWin = new QDialog(this);
		helpWin->setWindowTitle("Expression syntax");
		helpWin->resize(620, 640);
		QTextBrowser* brw = new QTextBrowser(helpWin);
		brw->document()->setDefaultStyleSheet("td {padding-right:16px;} p {margin:2px 0px;} li {margin:2px 0px;}");
		brw->setHtml(txt);
		QVBoxLayout* lay = new QVBoxLayout(helpWin);
		lay->setContentsMargins(4, 4, 4, 4);
		lay->addWidget(brw);
	}
	helpWin->show();
	helpWin->raise();
}

void xBrkManager::edit(xBrkPoint* sbrk) {
	if (sbrk) {
		obrk = *sbrk;
		obrk.off = 0;
	} else {
		obrk.type = BRK_MEMRAM;
		obrk.adr = 0;
		obrk.mask = 0;
		obrk.eadr = 0;
		obrk.off = 0;
		obrk.fetch = 1;
		obrk.read = 0;
		obrk.write = 0;
		obrk.off = 0;
		obrk.temp = 0;
		obrk.last = 0;
		obrk.fired = 0;
		obrk.onchg = 0;
		obrk.log = 0;
		obrk.hits = 0;
		obrk.count = 0;
		obrk.action = BRK_ACT_DBG;
		obrk.cond.clear();
	}
	ui.leCond->setText(QString(obrk.cond.c_str()));
	chaCond(ui.leCond->text());
	ui.brkAction->setCurrentIndex(ui.brkAction->findData(obrk.action));
	ui.brkType->setCurrentIndex(ui.brkType->findData(obrk.type));
	ui.brkFetch->setChecked(obrk.fetch);
	ui.brkOnChange->setChecked(obrk.onchg);
	ui.brkLog->setChecked(obrk.log);
	ui.brkRead->setChecked(obrk.read);
	ui.brkWrite->setChecked(obrk.write);
	setLimits(obrk.type);
	switch(obrk.type) {
		case BRK_IOPORT:
			ui.brkBank->setValue(0);
			ui.brkAdrHex->setValue(obrk.adr);
			ui.brkMaskHex->setValue(obrk.mask);
			break;
		case BRK_CPUADR:
			ui.brkBank->setValue(0);
			ui.brkAdrHex->setValue(obrk.adr);
			ui.brkAdrEnd->setValue(obrk.eadr);
			ui.brkMaskHex->setText("FFFF");
			break;
		default:
			ui.brkBank->setValue(obrk.adr >> 14);
			ui.brkAdrHex->setValue(obrk.adr);	// &0x3fff ?
			ui.brkAdrEnd->setValue(obrk.eadr);
			ui.brkMaskHex->setText("FFFF");
			break;
	}
	chaType(ui.brkType->currentIndex());
	show();
}

void xBrkManager::confirm() {
	xBrkPoint brk;
	QString cond = ui.leCond->text().trimmed();
	if (!cond.isEmpty() && !xexpr_ok(xexpr_compile(cond.toLocal8Bit().data()))) {
		ui.leCond->setFocus();
		return;			// don't accept a broken condition
	}
	brk.off = 0;
	brk.temp = 0;
	brk.last = 0;
	brk.fired = 0;
	brk.onchg = ui.brkOnChange->isChecked() ? 1 : 0;
	brk.log = ui.brkLog->isChecked() ? 1 : 0;
	brk_set_cond(&brk, cond.toLocal8Bit().data());
	brk.type = ui.brkType->itemData(ui.brkType->currentIndex()).toInt();
	brk.fetch = ui.brkFetch->isChecked() ? 1 : 0;
	brk.read = ui.brkRead->isChecked() ? 1 : 0;
	brk.write = ui.brkWrite->isChecked() ? 1 : 0;
	brk.action = ui.brkAction->itemData(ui.brkAction->currentIndex()).toInt();
	brk.hits = obrk.hits;
	brk.count = obrk.count;
	int bnk = ui.brkBank->value();
	switch (brk.type) {
		case BRK_COND:			// no address, no flags
			brk.fetch = 0;
			brk.read = 0;
			brk.write = 0;
			brk.adr = 0;
			brk.eadr = 0;
			break;
		case BRK_CPUADR:
			brk.adr = ui.brkAdrHex->getValue();
			brk.eadr = ui.brkAdrEnd->getValue();
			break;
		case BRK_IOPORT:
			brk.adr = ui.brkAdrHex->getValue();
			brk.eadr = brk.adr;
			break;
		default:
			brk.adr = ui.brkAdrHex->getValue() | (bnk << 14);
			brk.eadr = ui.brkAdrEnd->getValue() | (bnk << 14);
			break;
	}
	brk.mask = ui.brkMaskHex->getValue();
	emit completed(obrk, brk);
	hide();
}

// widget

xBreakWidget::xBreakWidget(QString i, QString t, QWidget* p):xDockWidget(i,t,p) {
	QWidget* wid = new QWidget;
	setWidget(wid);
	ui.setupUi(wid);
	setObjectName("BRKWIDGET");

	ui.bpList->setContextMenuPolicy(Qt::ActionsContextMenu);
	ui.bpList->addAction(ui.brkActReset);

	brkManager = new xBrkManager(this);
	connect(brkManager, &xBrkManager::completed, this, &xBreakWidget::confirmBrk);

	connect(ui.tbAddBrk, &QToolButton::clicked, this, &xBreakWidget::addBrk);
	connect(ui.tbEditBrk, &QToolButton::clicked, this, &xBreakWidget::editBrk);
	connect(ui.tbDelBrk, &QToolButton::clicked, this, &xBreakWidget::delBrk);
	connect(ui.tbBrkOpen, &QToolButton::clicked, this, &xBreakWidget::openBrk);
	connect(ui.tbBrkSave, &QToolButton::clicked, this, &xBreakWidget::saveBrk);
	connect(ui.bpList, &xBreakTable::rqDisasm, this, &xBreakWidget::rqDisasm);
	connect(ui.bpList, &xBreakTable::rqDasmDump, this, &xBreakWidget::updated);
	connect(ui.brkActReset, &QAction::triggered, this, &xBreakWidget::resetBrk);
}

void xBreakWidget::draw() {
	ui.bpList->update();
}

void xBreakWidget::addBrk() {
	brkManager->edit(NULL);
}

void xBreakWidget::editBrk() {
	QModelIndexList idxl = ui.bpList->selectionModel()->selectedRows();
	if (idxl.size() < 1) return;
	int row = idxl.first().row();
	xBrkPoint* brk = &conf.brk.list[row];
	brkManager->edit(brk);
}

void xBreakWidget::confirmBrk(xBrkPoint obrk, xBrkPoint brk) {
	brkDelete(obrk);
	brkAdd(brk);
	emit updated();
	ui.bpList->update();
}

bool qmidx_greater(const QModelIndex idx1, const QModelIndex idx2) {
	return (idx1.row() > idx2.row());
}

void xBreakWidget::delBrk() {
	QModelIndexList idxl = ui.bpList->selectionModel()->selectedRows();
	std::sort(idxl.begin(), idxl.end(), qmidx_greater);
	QModelIndex idx;
	xBrkPoint brk;
	foreach(idx, idxl) {
		brk = conf.brk.list[idx.row()];
		brkDelete(brk);
	}
	ui.bpList->update();
	emit updated();		// fill disasm/dump
}

void xBreakWidget::resetBrk() {
	QModelIndexList idxl = ui.bpList->selectionModel()->selectedRows();
	QModelIndex idx;
	foreach(idx, idxl) {
		conf.brk.list[idx.row()].hits = 0;
		conf.brk.list[idx.row()].count = 0;
	}
	ui.bpList->update();
	emit updated();		// fill disasm/dump
}

void xBreakWidget::openBrk() {
	QString path = QFileDialog::getOpenFileName(this, "Open breakpoints list", "", "Debugger breakpoints (*.xbrk)",nullptr,QFileDialog::DontUseNativeDialog);
	if (path.isEmpty()) return;
	if (brk_load_list(path.toLocal8Bit().data())) {
		ui.bpList->update();
		emit updated();
	} else {
		shitHappens("Can't open file for reading");
	}
}

void xBreakWidget::saveBrk() {
	QString path = QFileDialog::getSaveFileName(this, "Save breakpoints", "", "Debugger breakpoints (*.xbrk)",nullptr,QFileDialog::DontUseNativeDialog);
	if (path.isEmpty())
		return;
	if (!path.endsWith(".xbrk", Qt::CaseInsensitive))
		path.append(".xbrk");
	if (!brk_save_list(path.toLocal8Bit().data()))
		shitHappens("Can't open file for writing");
}

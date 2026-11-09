#include <QApplication>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFile>
#include <QRegularExpression>
#include <QStyleOptionButton>
#include <QUrl>

#include "xcore/xcore.h"
#include "dbg_brkpoints.h"

// Model

// what an opening point has measured, in T
QString brk_span_text(const xBrkPoint* brk) {
	if (brk->spanN < 1) return QString("No span closed yet");
	return QString("%0 spans: min %1, avg %2, max %3 T, %4 with an interrupt")
		.arg(brk->spanN).arg(brk->spanMin).arg((double)brk->spanSum / brk->spanN, 0, 'f', 1)
		.arg(brk->spanMax).arg(brk->spanInt);
}

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
			res = QString("CPU:%0").arg(dbg_word(brk.adr));
			if (brk.eadr > brk.adr) {
				res.append(QString("-%0").arg(dbg_word(brk.eadr)));
			}
			break;
		case BRK_IOPORT:
			// the port in the form picked; its mask hex, with the sign in decimal too:
			// a mask is bits, not a number
			res = QString("IO:%0 mask %1").arg(dbg_port(brk.adr, 0xffff)).arg(dbg_hex(brk.mask & 0xffff, 4));
			break;
		case BRK_MEMRAM:
		case BRK_MEMROM:
		case BRK_MEMSLT:
		case BRK_MEMDIV: {
			// the cell in the machine's own pages: the Next's are 8K
			const char* nm = (brk.type == BRK_MEMRAM) ? "RAM" : (brk.type == BRK_MEMROM) ? "ROM"
				: (brk.type == BRK_MEMDIV) ? "DIV" : "SLT";
			int mt = brk_mem_type(brk.type);
			if (brk.eadr > brk.adr) {
				res = QString("%0:%1-%2 [%3-%4]").arg(nm).arg(dbg_num(brk.adr, 6)).arg(dbg_num(brk.eadr, 6))
					.arg(cell_view_text(mt, brk.adr)).arg(cell_view_text(mt, brk.eadr));
			} else {
				res = QString("%0:%1 [%2]").arg(nm).arg(dbg_num(brk.adr, 6)).arg(cell_view_text(mt, brk.adr));
			}
			break;
		}
		case BRK_MEMEXT:
			res = QString("EXT:%0 [%1:%2]").arg(dbg_num(brk.adr, 6)).arg(gethexbyte(brk.adr >> 14)).arg(dbg_word(brk.adr & 0x3fff));
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
	const xBrkPoint& brk = conf.brk.list[row];
	switch (role) {
		case Qt::ToolTipRole:
			if ((brk.span == BRK_SPAN_OPEN) && (col >= 4)) res = brk_span_text(&brk);
			break;
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
// by the kind, then the address: the text sorts only in hex, and only padded
bool xbsName(const xBrkPoint bpa, const xBrkPoint bpb) {
	if (bpa.type != bpb.type) return brkGetString(bpa).section(':', 0, 0) < brkGetString(bpb).section(':', 0, 0);
	if (bpa.adr != bpb.adr) return bpa.adr < bpb.adr;
	return bpa.eadr < bpb.eadr;
}
bool xbsCond(const xBrkPoint bpa, const xBrkPoint bpb) {return (bpa.cond < bpb.cond);}

void xBreakListModel::sort(int col, Qt::SortOrder ord) {
	EmuLock lock;		// the emulation's and DeZog's threads walk the list
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
		case BRK_MEMRAM: adr = comp_find_xadr(conf.zx, MEM_RAM, bp.adr, MEM_VIEW_FETCH); break;
		case BRK_MEMROM: adr = comp_find_xadr(conf.zx, MEM_ROM, bp.adr, MEM_VIEW_FETCH); break;
		case BRK_MEMDIV: adr = comp_find_xadr(conf.zx, MEM_DIV, bp.adr, MEM_VIEW_FETCH); break;
	}
	if (adr < 0) return;
	emit rqDisasm(adr);
}

// Dialog

xBrkManager::xBrkManager(QWidget* p):QDialog(p) {
	ui.setupUi(this);
	helpWin = nullptr;
	ui.labCondErr->setWordWrap(true);

	ui.brkType->addItem("CPU address", BRK_CPUADR);
	ui.brkType->addItem("I/O port", BRK_IOPORT);
	ui.brkType->addItem("RAM cell", BRK_MEMRAM);
	ui.brkType->addItem("ROM cell", BRK_MEMROM);
	ui.brkType->addItem("Slot cell", BRK_MEMSLT);
	ui.brkType->addItem("divMMC cell", BRK_MEMDIV);
	ui.brkType->addItem("Interrupt", BRK_IRQ);
	ui.brkType->addItem("Condition (global)", BRK_COND);

	ui.brkAction->addItem("Debugger", BRK_ACT_DBG);
	ui.brkAction->addItem("Counter", BRK_ACT_COUNT);
	ui.brkAction->addItem("Screen dump", BRK_ACT_SCR);

	ui.brkSpan->addItem("None", BRK_SPAN_NONE);
	ui.brkSpan->addItem("Opens a span", BRK_SPAN_OPEN);
	ui.brkSpan->addItem("Closes a span", BRK_SPAN_CLOSE);

	label_complete(ui.leStart);
	label_complete(ui.leEnd);
	dbg_num_field(ui.leStart);
	dbg_num_field(ui.leEnd);
	label_complete(ui.leCond);
	label_complete(ui.leLogExp);
	connect(ui.leStart, &QLineEdit::textEdited, this, &xBrkManager::chaStart);
	connect(ui.leStart, &QLineEdit::textChanged, this, &xBrkManager::chaAdr);
	connect(ui.leEnd, &QLineEdit::textChanged, this, &xBrkManager::chaAdr);
	connect(ui.brkBank, SIGNAL(valueChanged(int)), this, SLOT(chaAdr()));
	connect(ui.brkType, SIGNAL(currentIndexChanged(int)), this, SLOT(chaType(int)));
	connect(ui.leCond, SIGNAL(textChanged(QString)), this, SLOT(chaCond(QString)));
	connect(ui.tbCondHelp, SIGNAL(clicked()), this, SLOT(condHelp()));
	connect(ui.brkLog, &QCheckBox::toggled, ui.leLogExp, &QWidget::setEnabled);
	connect(ui.pbOK, SIGNAL(clicked()), this, SLOT(confirm()));
	connect(ui.pbCancel, SIGNAL(clicked()), this, SLOT(hide()));
}

static bool brk_is_cell(int type) {
	return (type == BRK_MEMRAM) || (type == BRK_MEMROM) || (type == BRK_MEMSLT) || (type == BRK_MEMDIV);
}

static const char* brk_cell_name(int type) {
	switch (type) {
		case BRK_MEMRAM: return "RAM";
		case BRK_MEMROM: return "ROM";
		case BRK_MEMDIV: return "DIV";
		default: return "SLT";
	}
}

int xBrkManager::curType() {
	return ui.brkType->itemData(ui.brkType->currentIndex()).toInt();
}

// the area a cell type's page field counts in
static int brk_base_area(int type) {
	switch (type) {
		case BRK_MEMRAM: return CELL_RAM;
		case BRK_MEMROM: return CELL_ROM;
		case BRK_MEMDIV: return CELL_DIV;
		default: return CELL_SLT;
	}
}

// One address field as the type reads it. A cpu address is a number by the
// debugger's one rule (dbg_read_adr) or a label; a port a number by the same
// rule (readRange reads its mask). A cell is an offset in the page of the page
// field, page:offset (the page in hex, or an area's token: ROM0, DIV3), a
// label, or a cpu address - which brings the cell mapped there now, when it is
// the kind of memory the type watches. *cell is that cell, -1 for an offset alone
bool xBrkManager::readAdr(QString txt, int* adr, int* cell) {
	int type = curType();
	int val;
	xAdr lab;
	*cell = -1;
	txt = txt.trimmed();
	if (txt.isEmpty()) return false;
	int colon = txt.indexOf(':');
	if (colon > 0) {
		// the page is hex, as it is shown; the offset by the one rule - and a
		// sign in front of the page, the one the typing keeps, counts for an
		// offset with none of its own
		bool oko;
		QString page = txt.left(colon);
		QString ofs = txt.mid(colon + 1).trimmed();
		if ((page != dbg_num_unsign(page)) && (ofs == dbg_num_unsign(ofs))) ofs.prepend('#');
		int off = dbg_num_parse(ofs, &oko);
		if (!brk_is_cell(type) || !oko) return false;
		if (!cell_from_text(brk_mem_type(type), dbg_num_unsign(page), off, cell)) return false;
		*adr = off;
		return true;
	}
	if (type == BRK_IOPORT) {
		bool ok;
		val = dbg_num_parse(txt, &ok);
		*adr = val;
		return ok && (val >= 0) && (val <= 0xffff);
	}
	if (!dbg_read_adr(txt, &val, &lab)) return false;
	if (!brk_is_cell(type)) {
		*adr = val;
		return (val >= 0) && (val <= 0xffff);
	}
	if (lab.type == brk_mem_type(type)) {		// a label carries its page
		*cell = lab.abs;
		*adr = comp_cell(conf.zx, lab.type, lab.abs).ofs;
		return true;
	}
	if ((val < 0) || (val > 0xffff)) return false;
	*adr = val;
	if ((lab.type < 0) && (val < comp_cell_unit(conf.zx, brk_base_area(type)))) return true;
	xAdr xa = comp_get_xadr(conf.zx, val, MEM_VIEW_READ);
	if (xa.type != brk_mem_type(type)) {		// other memory there: an offset in the page field's page
		*adr = val & (comp_cell_unit(conf.zx, brk_base_area(type)) - 1);
		return true;
	}
	*cell = xa.abs;
	*adr = comp_cell(conf.zx, xa.type, xa.abs).ofs;
	return true;
}

// The address fields into the point, for OK and for the line under them alike.
// false: *err says why, *fld is the field to blame
bool xBrkManager::readRange(xBrkPoint* brk, QString* err, QLineEdit** fld) {
	int adr;
	int eadr;
	int cell;
	int ecell;
	bool haveEnd = !ui.leEnd->text().trimmed().isEmpty();
	*fld = ui.leStart;
	if (!readAdr(ui.leStart->text(), &adr, &cell)) {
		*err = ui.leStart->text().trimmed().isEmpty() ? "an address is needed" : "not an address or a label";
		return false;
	}
	*fld = ui.leEnd;
	if (brk->type == BRK_IOPORT) {
		brk->adr = adr;
		brk->eadr = adr;
		brk->mask = 0xffff;
		if (!haveEnd) return true;
		// the mask is bits: hex, with or without a sign, as the list files have it
		if (!brk_list_adr(ui.leEnd->text().trimmed(), &eadr, NULL) || (eadr < 0) || (eadr > 0xffff)) {
			*err = "the mask is not a number";
			return false;
		}
		brk->mask = eadr;
		return true;
	}
	eadr = adr;
	ecell = cell;
	if (haveEnd && !readAdr(ui.leEnd->text(), &eadr, &ecell)) {
		*err = "not an address or a label";
		return false;
	}
	if (brk->type == BRK_CPUADR) {
		if (eadr < adr) {
			*err = "the range ends before it starts";
			return false;
		}
		brk->adr = adr;
		brk->eadr = eadr;
		return true;
	}
	// an offset alone is in the page the page field has, of the type's own memory
	int mt = brk_mem_type(brk->type);
	int pg = ui.brkBank->value();
	if (cell < 0) {
		*fld = ui.leStart;
		if (!comp_cell_xadr(conf.zx, brk_base_area(brk->type), pg, adr, &mt, &cell)) {
			*err = "the machine has no such page or offset";
			return false;
		}
	}
	xCell c0 = comp_cell(conf.zx, mt, cell);
	if (ecell < 0) {
		*fld = ui.leEnd;
		if (!comp_cell_xadr(conf.zx, c0.area, c0.page, eadr, &mt, &ecell)) {
			*err = "the offset is past the page";
			return false;
		}
	}
	xCell c1 = comp_cell(conf.zx, mt, ecell);
	*fld = ui.leEnd;
	if ((c1.area != c0.area) || (c1.page != c0.page)) {
		*err = "the end is in another page";
		return false;
	}
	if (ecell < cell) {
		*err = "the range ends before it starts";
		return false;
	}
	brk->adr = cell;
	brk->eadr = ecell;
	return true;
}

// a cpu address typed into a cell's field moves the page to the one there
void xBrkManager::chaStart(QString txt) {
	int adr;
	int cell;
	int type = curType();
	if (!brk_is_cell(type)) return;
	if (!readAdr(txt, &adr, &cell) || (cell < 0)) return;
	xCell c = comp_cell(conf.zx, brk_mem_type(type), cell);
	if ((c.area == brk_base_area(type)) && (c.page <= ui.brkBank->maximum()))
		ui.brkBank->setValue(c.page);
}

// the address as it was understood, the way the condition check does it
void xBrkManager::chaAdr() {
	xBrkPoint brk;
	QString err;
	QString res;
	QLineEdit* fld;
	brk.type = curType();
	if ((brk.type != BRK_CPUADR) && !brk_is_cell(brk.type)) {
		ui.labAdrHint->setText(QString());
		return;
	}
	if (ui.leStart->text().trimmed().isEmpty()) {
		res.clear();
	} else if (!readRange(&brk, &err, &fld)) {
		res = err;
	} else {
		if (brk.type == BRK_CPUADR) {
			res = dbg_word(brk.adr);
			if (brk.eadr > brk.adr) res.append("-").append(dbg_word(brk.eadr));
		} else {
			int ofs;
			res = QString("%0 %1").arg(brk_cell_name(brk.type)).arg(cell_view_text(brk_mem_type(brk.type), brk.adr));
			if (brk.eadr > brk.adr) {
				cell_page_text(brk_mem_type(brk.type), brk.eadr, &ofs);
				res.append("-").append(dbg_word(ofs));
			}
			int cpu = comp_find_xadr(conf.zx, brk_mem_type(brk.type), brk.adr, MEM_VIEW_READ);
			res.append((cpu < 0) ? QString(", not mapped now") : QString(", at %0 now").arg(dbg_word(cpu)));
		}
		QString lab = brk_label(brk.type, brk.adr);
		if (!lab.isEmpty()) res.append(QString(", %0").arg(lab));
	}
	ui.labAdrHint->setStyleSheet(err.isEmpty() ? "" : "color:#ef2929");
	ui.labAdrHint->setText(res);
}

#define EL_SPAN 64
#define EL_CHG 32
#define EL_FE 16
#define EL_RD 8
#define EL_WR 4
#define EL_BNK 2
#define EL_ADR 1

void xBrkManager::setElements(int mask) {
	ui.brkFetch->setVisible(mask & EL_FE);
	ui.brkRead->setVisible(mask & EL_RD);
	ui.brkWrite->setVisible(mask & EL_WR);
	ui.labFlags->setVisible(mask & (EL_FE | EL_RD | EL_WR));
	ui.brkOnChange->setVisible(mask & EL_CHG);
	ui.brkBank->setVisible(mask & EL_BNK);
	ui.labBank->setVisible(mask & EL_BNK);
	ui.labStart->setVisible(mask & EL_ADR);
	ui.leStart->setVisible(mask & EL_ADR);
	ui.labTo->setVisible(mask & EL_ADR);
	ui.leEnd->setVisible(mask & EL_ADR);
	ui.labAdrHint->setVisible(mask & EL_ADR);
	ui.labSpan->setVisible(mask & EL_SPAN);
	ui.brkSpan->setVisible(mask & EL_SPAN);
	ui.labSpanStat->setVisible(mask & EL_SPAN);
}

// the pages there are of what the type watches
void xBrkManager::setLimits(int t) {
	Computer* comp = conf.zx;
	int max;
	max = brk_is_cell(t) ? comp_cell_pages(comp, brk_base_area(t)) : 0;
	ui.brkBank->setMaximum((max > 1) ? (max - 1) : 0);
}

void xBrkManager::chaType(int i) {
	int t = ui.brkType->itemData(i).toInt();
	setLimits(t);
	chaCond(ui.leCond->text());		// the note about HITS depends on the type
	bool port = (t == BRK_IOPORT);
	// a port is a number like an address; its mask is bits and stays hex
	ui.leStart->setProperty(DBG_NUM_PORT, port);
	ui.leEnd->setProperty(DBG_NUM_OFF, port);
	ui.labStart->setText(port ? "Port" : "Address");
	ui.labTo->setText(port ? "mask" : "to");
	ui.leStart->setToolTip(port ? "The port number" : brk_is_cell(t) ? "Offset, CPU address, page:offset or label" : "An address or a label");
	ui.leEnd->setToolTip(port ? "The bits of the port that must match" : "The last address of a range. Empty: one address");
	ui.brkRead->setToolTip(port ? "Fires on IN" : "Fires on a read");
	ui.brkWrite->setToolTip(port ? "Fires on OUT" : "Fires on a write");
	switch (t) {
		case BRK_IRQ:
			setElements(EL_SPAN);
			break;
		case BRK_COND:				// the condition and how it fires
			setElements(EL_CHG);
			break;
		case BRK_IOPORT:
			setElements(EL_RD | EL_WR | EL_ADR | EL_SPAN);
			break;
		case BRK_CPUADR:
			setElements(EL_FE | EL_RD | EL_WR | EL_ADR | EL_SPAN);
			break;
		default:
			setElements(EL_FE | EL_RD | EL_WR | EL_BNK | EL_ADR | EL_SPAN);
			break;
	}
	chaAdr();
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
				" (HITS <= 3) but not skip. To stop on the Nth hit of an"
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

// the text lives in res/help/cond-syntax.html, built into the binary
void xBrkManager::condHelp() {
	help_window(this, &helpWin, ":/res/help/cond-syntax.html", "Expression Syntax");
}

// an address the way a person wrote it: its label when it has one
static QString brk_adr_text(int type, int abs) {
	QString lab = brk_label(type, abs);
	if (!lab.isEmpty()) return lab;
	if (!brk_is_cell(type)) return dbg_word(abs);
	// an offset in the page field's page, or page:offset for any other area
	xCell c = comp_cell(conf.zx, brk_mem_type(type), abs);
	return (c.area == brk_base_area(type)) ? dbg_word(c.ofs) : cell_view_text(brk_mem_type(type), abs);
}

void xBrkManager::edit(xBrkPoint* sbrk) {
	if (sbrk) {
		obrk = *sbrk;
		obrk.off = 0;
	} else {
		obrk = brkCreate(BRK_CPUADR, MEM_BRK_FETCH, 0, -1);
		obrk.mask = 0xffff;
		brk_set_cond(&obrk, "");
		brk_set_logx(&obrk, "");
	}
	ui.brkType->blockSignals(true);
	ui.brkType->setCurrentIndex(ui.brkType->findData(obrk.type));
	ui.brkType->blockSignals(false);
	setLimits(obrk.type);
	ui.leCond->setText(QString(obrk.cond.c_str()));
	ui.leLogExp->setText(QString::fromLocal8Bit(obrk.logx.c_str()));
	ui.leLogExp->setEnabled(obrk.log);
	ui.brkSpan->setCurrentIndex(ui.brkSpan->findData(obrk.span));
	ui.labSpanStat->setText((sbrk && (obrk.span == BRK_SPAN_OPEN)) ? brk_span_text(&obrk) : QString());
	ui.brkAction->setCurrentIndex(ui.brkAction->findData(obrk.action));
	ui.brkFetch->setChecked(obrk.fetch);
	ui.brkOnChange->setChecked(obrk.onchg);
	ui.brkLog->setChecked(obrk.log);
	ui.brkRead->setChecked(obrk.read);
	ui.brkWrite->setChecked(obrk.write);
	int page = 0;
	if (brk_is_cell(obrk.type)) {
		xCell c = comp_cell(conf.zx, brk_mem_type(obrk.type), obrk.adr);
		if (c.area == brk_base_area(obrk.type)) page = c.page;
	}
	ui.brkBank->setValue(page);
	if (!sbrk) {
		ui.leStart->clear();
		ui.leEnd->clear();
	} else if (obrk.type == BRK_IOPORT) {
		ui.leStart->setText(dbg_num(obrk.adr & 0xffff, 4));
		ui.leEnd->setText(gethexword(obrk.mask & 0xffff));
	} else {
		ui.leStart->setText(brk_adr_text(obrk.type, obrk.adr));
		ui.leEnd->setText((obrk.eadr > obrk.adr) ? brk_adr_text(obrk.type, obrk.eadr) : QString());
	}
	chaType(ui.brkType->currentIndex());
	show();
	ui.leStart->setFocus();
}

// a field that is not understood keeps the dialog open, with the reason under it
void xBrkManager::adrError(QLineEdit* fld, QString msg) {
	ui.labAdrHint->setStyleSheet("color:#ef2929");
	ui.labAdrHint->setText(msg);
	fld->setFocus();
}

void xBrkManager::confirm() {
	xBrkPoint brk = obrk;		// what it has counted and measured stays
	QString err;
	QLineEdit* fld;
	brk.type = curType();
	if ((brk.type != BRK_IRQ) && (brk.type != BRK_COND) && !readRange(&brk, &err, &fld)) {
		adrError(fld, err);
		return;
	}
	QString cond = ui.leCond->text().trimmed();
	if (!cond.isEmpty() && !xexpr_ok(xexpr_compile(cond.toLocal8Bit().data()))) {
		ui.leCond->setFocus();
		return;			// don't accept a broken condition
	}
	brk_set_logx(&brk, ui.leLogExp->text().trimmed().toLocal8Bit().data());
	for (auto it = brk.logexp.begin(); it != brk.logexp.end(); it++) {
		if (xexpr_ok(*it)) continue;
		ui.labCondErr->setStyleSheet("color:#ef2929");
		ui.labCondErr->setText(QString("Log: %0: %1").arg(it->src.c_str()).arg(it->err.c_str()));
		ui.leLogExp->setFocus();
		return;
	}
	brk.span = ui.brkSpan->itemData(ui.brkSpan->currentIndex()).toInt();
	brk.off = 0;
	brk.temp = 0;
	brk.last = 0;
	brk.fired = 0;
	brk.onchg = ((brk.type == BRK_COND) && ui.brkOnChange->isChecked()) ? 1 : 0;
	brk.log = ui.brkLog->isChecked() ? 1 : 0;
	brk_set_cond(&brk, cond.toLocal8Bit().data());
	brk.fetch = ui.brkFetch->isChecked() ? 1 : 0;
	brk.read = ui.brkRead->isChecked() ? 1 : 0;
	brk.write = ui.brkWrite->isChecked() ? 1 : 0;
	brk.action = ui.brkAction->itemData(ui.brkAction->currentIndex()).toInt();
	switch (brk.type) {
		case BRK_COND:			// no address, no flags, no span
			brk.fetch = 0;
			brk.read = 0;
			brk.write = 0;
			brk.adr = 0;
			brk.eadr = 0;
			brk.span = BRK_SPAN_NONE;
			break;
		case BRK_IRQ:
			brk.adr = 0;
			brk.eadr = 0;
			break;
		case BRK_IOPORT:
			brk.fetch = 0;
			break;
	}
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
	// the panel's buttons first, as they stand, with their icons
	QAction* actAdd = new QAction(ui.tbAddBrk->icon(), "Add breakpoint", this);
	ui.bpList->addAction(actAdd);
	connect(actAdd, &QAction::triggered, this, &xBreakWidget::addBrk);
	QAction* actEdit = new QAction(ui.tbEditBrk->icon(), "Edit breakpoint", this);
	ui.bpList->addAction(actEdit);
	connect(actEdit, &QAction::triggered, this, &xBreakWidget::editBrk);
	// the delete button, in the menu and on the key while the list has the focus
	actDel = new QAction(ui.tbDelBrk->icon(), "Delete breakpoint", this);
	actDel->setShortcut(QKeySequence(Qt::Key_Delete));
	actDel->setShortcutContext(Qt::WidgetShortcut);
	ui.bpList->addAction(actDel);
	ui.tbDelBrk->setToolTip("Delete the selected breakpoints (Del)");
	QAction* sep = new QAction(this);
	sep->setSeparator(true);
	ui.bpList->addAction(sep);
	ui.bpList->addAction(ui.brkActReset);
	actLog = new QAction("Open breakpoint log", this);
	ui.bpList->addAction(actLog);
	connect(actLog, &QAction::triggered, this, &xBreakWidget::openLog);

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
	connect(actDel, &QAction::triggered, this, &xBreakWidget::delBrk);
}

void xBreakWidget::draw() {
	actLog->setEnabled(!brk_trace_file().isEmpty());
	ui.bpList->update();
}

void xBreakWidget::openLog() {
	QString path = brk_trace_file();
	if (path.isEmpty()) return;
	brk_trace_flush();
	QDesktopServices::openUrl(QUrl::fromLocalFile(path));
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
		brk_reset_stats(&conf.brk.list[idx.row()]);
	}
	ui.bpList->update();
	emit updated();		// fill disasm/dump
}

void xBreakWidget::openBrk() {
	// a bpx.ini is read too: its lines are breakpoints as well
	QString path = QFileDialog::getOpenFileName(this, "Open breakpoints list", "", "Breakpoints (*.xbrk *.ini);;All files (*)",nullptr,QFileDialog::DontUseNativeDialog);
	if (path.isEmpty()) return;
	// any .ini is offered: one with no breakpoint line keeps the list as it is
	if (path.endsWith(".ini", Qt::CaseInsensitive)) {
		QFile file(path);
		bool any = false;
		QRegularExpression bpx("^\\s*[xrw]0=", QRegularExpression::CaseInsensitiveOption);	// as bpx.ini writes it
		if (file.open(QFile::ReadOnly)) {
			while (!any && !file.atEnd())
				any = bpx.match(QString::fromLocal8Bit(file.readLine())).hasMatch();
		}
		if (!any) {
			shitHappens("No breakpoints in this file");
			return;
		}
	}
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

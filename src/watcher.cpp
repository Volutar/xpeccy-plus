#include "watcher.h"
#include "xcore/xcore.h"

#include <QInputDialog>
#include <QDebug>

// expression evaluation moved to xcore/xexpr.cpp

// wutcha

extern QString getStyleString(QString, QString, int = 0, int = 100);

// the two headers follow the debugger's own, so a style with a .pal of its own
// reaches them too

void xWatcher::updateStyle() {
	QString str = getStyleString("dbg.header.bg", "dbg.header.txt");
	ui.label_15->setStyleSheet(str);
	ui.label_14->setStyleSheet(str);
}

xWatcher::xWatcher(QWidget* p):QDialog(p) {
	int i;
	ui.setupUi(this);
	updateStyle();
	model = new xWatchModel;
	ui.wchMemTab->setModel(model);
	ui.wchMemTab->addAction(ui.actAddWatcher);
	ui.wchMemTab->addAction(ui.actDelWatcher);

// like in deBUGa: pairs regName/regValue
	QLabel* lp;
	xHexSpin* hp;
	for(i = 0; i < 32; i++) {
		lp = new QLabel;
		hp = new xHexSpin;
		hp->setMaximumWidth(50);
		regLabels.append(lp);
		regValues.append(hp);
		ui.regGrid->addWidget(lp, i >> 1, (i & 1) << 1);
		ui.regGrid->addWidget(hp, i >> 1, ((i & 1) << 1) | 1);
	}
//	ui.regGrid->setRowStretch(32, 10);

	labswin = new xLabeList;

	newWch = new QDialog(this);
	nui.setupUi(newWch);
	nui.cbType->addItem("CPU addr", WUT_CPU);
	nui.cbType->addItem("RAM addr", WUT_RAM);
	nui.cbType->addItem("ROM addr", WUT_ROM);
	connect(nui.tbLabel, SIGNAL(released()), labswin, SLOT(show()));
	label_complete(nui.leExpression);
	connect(labswin, SIGNAL(labSelected(QString)), this, SLOT(insertLabel(QString)));

	for(i = 0; i < 14; i++) ui.wchMemTab->setColumnWidth(i, 30);

	connect(ui.actAddWatcher, SIGNAL(triggered(bool)), this, SLOT(newWatcher()));
	connect(ui.actDelWatcher, SIGNAL(triggered(bool)), this, SLOT(delWatcher()));
	connect(ui.wchMemTab, SIGNAL(doubleClicked(QModelIndex)), this, SLOT(edtWatcher()));

	connect(nui.pbOK, SIGNAL(released()), this, SLOT(confirmNew()));
}

void xWatcher::show() {

	xRegBunch regs = cpuGetRegs(conf.zx->cpu);
	int work = 1;
	for(int i = 0; i < 32; i++) {
		if (work) {
			if (regs.regs[i].id == REG_EOT) {
				work = 0;
				regLabels[i]->setVisible(false);
				regValues[i]->setVisible(false);
			} else {
				regLabels[i]->setVisible(true);
				regValues[i]->setVisible(true);
				regLabels[i]->setText(regs.regs[i].name);
				regValues[i]->setValue(regs.regs[i].value);
			}
		} else {
			regLabels[i]->setVisible(false);
			regValues[i]->setVisible(false);
		}
	}

	QDialog::show();
}

QString getBankType(int type) {
	QString res;
	switch(type) {
		case MEM_ROM: res = "ROM"; break;
		case MEM_RAM: res = "RAM"; break;
		case MEM_SLOT: res = "SLT"; break;
		default: res = "EXT"; break;
	}
	return res;
}

QString getBankName(MemPage pg) {
	return QString("%0:%1").arg(getBankType(pg.type), gethexbyte(pg.num >> 6));
}

void xWatcher::fillFields(Computer* comp) {
	if (!isVisible()) return;
	if (comp == NULL) return;
	model->comp = comp;

	xRegBunch regs = cpuGetRegs(comp->cpu);
	int i = 0;
	while ((i < 32) && (regs.regs[i].id != REG_EOT)) {
		// regLabels[i]->setText(regs.regs[i].name);
		regValues[i]->setValue(regs.regs[i].value);
		i++;
	}
	ui.wchBank0->setText(getBankName(comp->mem->map[0x00]));
	ui.wchBank1->setText(getBankName(comp->mem->map[0x40]));
	ui.wchBank2->setText(getBankName(comp->mem->map[0x80]));
	ui.wchBank3->setText(getBankName(comp->mem->map[0xc0]));

	model->update();
}

int xWatcher::getCurRow() {
	int res = -1;
	QModelIndexList lst = ui.wchMemTab->selectionModel()->selectedRows();
	if (lst.size() == 1)
		res = lst.first().row() / 2;
	return res;
}

void xWatcher::newWatcher() {
	curwch = -1;
	newWch->show();
}

void xWatcher::confirmNew() {
	int type = nui.cbType->currentData().toInt();
	QString str = nui.leExpression->text();
	if (str.isEmpty()) return;
	xResult res = xEval(str.toLocal8Bit().data());		// check syntax
	if (res.err) return;
	newWch->close();
	if (curwch < 0) {
		model->addItem(type, str);
		for (int i = 0; i < model->getItemCount() * 2; i += 2) {
			ui.wchMemTab->setSpan(i, 0, 1, 11);
			ui.wchMemTab->setSpan(i, 11, 1, 2);
		}
	} else {
		model->setItem(curwch, type, str);
	}
}

void xWatcher::insertLabel(QString lab) {
	nui.leExpression->insert(lab);
}

void xWatcher::delWatcher() {
	int row = getCurRow();
	if (row < 0) return;
	model->delItem(row);
}

void xWatcher::edtWatcher() {
	curwch = getCurRow();
	if (curwch < 0) return;
	xWatchItem itm = model->getItem(curwch);
	nui.cbType->setCurrentIndex(nui.cbType->findData(itm.type));
	nui.leExpression->setText(itm.exp);
	newWch->show();
}

// watcher view model

xWatchModel::xWatchModel() {
}

void xWatchModel::update() {
	emit QAbstractItemModel::dataChanged(index(0, 0), index(rowCount(), columnCount()));
}

QModelIndex xWatchModel::index(int row, int col, const QModelIndex&) const {
	QModelIndex res = createIndex(row, col, (void*)this);
	return res;
}

QModelIndex xWatchModel::parent(const QModelIndex&) const {
	return QModelIndex();
}

int xWatchModel::rowCount(const QModelIndex&) const {
	return explist.size() * 2;
}

int xWatchModel::columnCount(const QModelIndex&) const {
	return 13;
}

void xWatchModel::insertRow(int row, const QModelIndex& idx) {
	emit beginInsertRows(idx,row,row);
	emit endInsertRows();
}

void xWatchModel::removeRow(int row, const QModelIndex& idx) {
	emit beginRemoveRows(idx,row*2,row*2+1);
	emit endRemoveRows();
}

int xWatchModel::getItemCount() {
	return explist.size();
}

xWatchItem xWatchModel::getItem(int row) {
	return explist.at(row);
}

void xWatchModel::addItem(int type, QString exp) {
	xWatchItem itm;
	itm.type = type;
	itm.exp = exp;
	explist.append(itm);
	insertRow(explist.size() - 1);
	insertRow(explist.size() - 1);
}

void xWatchModel::setItem(int idx, int type, QString exp) {
	if (idx < 0) return;
	if (idx >= explist.size()) return;
	xWatchItem itm;
	itm.type = type;
	itm.exp = exp;
	explist[idx] = itm;
	emit QAbstractItemModel::dataChanged(index(idx, 0), index(idx, columnCount()));
}

void xWatchModel::delItem(int idx) {
	if (idx < explist.size()) {
		explist.removeAt(idx);
		removeRow(idx);
	}
}

QVariant xWatchModel::data(const QModelIndex& idx, int role) const {
	QVariant res;
	if (!idx.isValid()) return res;
	int row = idx.row();
	int col = idx.column();
	if ((row < 0) || (row >= rowCount(idx))) return res;
	if ((col < 0) || (col >= columnCount(idx))) return res;
	xWatchItem itm;
	xResult xr;
	switch (role) {
		case Qt::DisplayRole:
			itm = explist.at(row >> 1);
			xr = xEval(itm.exp.toLocal8Bit().data());
			if (row & 1) {
				if (xr.err) {
					res = "??";
				} else {
					res = gethexbyte(memRd(comp->mem, (xr.value + col) & comp->mem->busmask));
				}
			} else if (col == 0) {
				switch(itm.type) {
					case WUT_CPU: res = "CPU: "+itm.exp; break;
					case WUT_RAM: res = "RAM: "+itm.exp; break;
					case WUT_ROM: res = "ROM: "+itm.exp; break;
					default: res = "Error"; break;
				}
			} else if (col == 11) {
				if (xr.err) {
					res = "????";
				} else {
					res = gethexword(xr.value);
				}
			}
	}
	return res;
}

#include <QHeaderView>
#include "opt_gamepad.h"

#include "../xgui.h"
#include "../../xcore/xcore.h"

#include <QInputDialog>
#include <QLabel>
#include <QLayout>

// Gamepad map table model

xPadMapModel::xPadMapModel(xGamepad* gp, QObject* p):QAbstractItemModel(p) {
	gpad = gp;
}

QModelIndex xPadMapModel::index(int row, int col, const QModelIndex& idx) const {
	return createIndex(row, col);
}

QModelIndex xPadMapModel::parent(const QModelIndex& idx) const {
	return QModelIndex();
}

int xPadMapModel::rowCount(const QModelIndex& idx) const {
	if (idx.isValid()) return 0;
	return gpad->mapSize();
}

int xPadMapModel::columnCount(const QModelIndex& par) const {
	if (par.isValid()) return 0;
	return 3;
}

QVariant xPadMapModel::data(const QModelIndex& idx, int role) const {
	QVariant res;
	if (!idx.isValid()) return res;
	int row = idx.row();
	int col = idx.column();
	if ((row < 0) || (row >= rowCount())) return res;
	if ((col < 0) || (col >= columnCount())) return res;
	xJoyMapEntry jent = gpad->mapItem(row);
	QString str;
	switch (role) {
		case Qt::DisplayRole:
			switch(col) {
				case 0:
					res = xGamepad::getEntryName(jent);
					break;
				case 1:
					switch(jent.dev) {
						case JMAP_KEY:
#if USE_SEQ_BIND
							str = QString("Key: %0").arg(jent.seq.toString());
#else
							str = QString("Key %0").arg(getKeyNameById(jent.key));
#endif
							break;
						case JMAP_JOY:
							str = "Joystick ";
							switch (jent.dir) {
								case XJ_UP: str.append("up"); break;
								case XJ_DOWN: str.append("down"); break;
								case XJ_RIGHT: str.append("right"); break;
								case XJ_LEFT: str.append("left"); break;
								case XJ_FIRE: str.append("fire"); break;
								case XJ_BUT2: str.append("button2"); break;
								case XJ_BUT3: str.append("button3"); break;
								case XJ_BUT4: str.append("button4"); break;
								default: str.append("??"); break;
							}
							break;
						case JMAP_MOUSE:
							str = "Mouse ";
							switch (jent.dir) {
								case XM_UP: str.append("up"); break;
								case XM_DOWN: str.append("down"); break;
								case XM_LEFT: str.append("left"); break;
								case XM_RIGHT: str.append("right"); break;
								case XM_LMB: str.append("LB"); break;
								case XM_MMB: str.append("MB"); break;
								case XM_RMB: str.append("RB"); break;
								case XM_WHEELUP: str.append("wheel up"); break;
								case XM_WHEELDN: str.append("wheel down"); break;
								default: str.append("??"); break;
							}
					}
					res = str;
					break;
				case 2:
					if (jent.rpt > 0)
						res = QString("%0 sec").arg(jent.rpt / 50.0);
					break;
			}
			break;
	}
	return res;
}

void xPadMapModel::update() {
	endResetModel();
}

// Gamepad setings widget

xGamepadWidget::xGamepadWidget(xGamepad* gp, QWidget* p):QWidget(p) {
	gpad = gp;
	ui.setupUi(this);
	padmodel = new xPadMapModel(gp);
	ui.tvMapView->setModel(padmodel);
	// the input and the repeat fit what they hold, the action takes the rest
	QHeaderView* hdr = ui.tvMapView->horizontalHeader();
	hdr->setStretchLastSection(false);
	hdr->setMinimumSectionSize(55);
	hdr->setSectionResizeMode(0, QHeaderView::Interactive);
	hdr->setSectionResizeMode(1, QHeaderView::Stretch);
	hdr->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	// the input's column fits what it holds, never under the width it always had
	connect(padmodel, &QAbstractItemModel::modelReset, this, [this]() {
		ui.tvMapView->resizeColumnToContents(0);
		QHeaderView* hdr = ui.tvMapView->horizontalHeader();
		if (hdr->sectionSize(0) < 90) hdr->resizeSection(0, 90);
	});
	hdr->resizeSection(0, 90);
	connect(ui.cbGPName, SIGNAL(currentIndexChanged(int)), this, SLOT(devChanged(int)));
	connect(ui.cbMapFile, SIGNAL(currentIndexChanged(int)), this, SLOT(mapChanged(int)));
	connect(ui.tbAddMap, SIGNAL(released()), this, SLOT(addMap()));
	connect(ui.tbDelMap, SIGNAL(released()), this, SLOT(delMap()));
	connect(ui.tbAddEntry, SIGNAL(released()), this, SLOT(addEntry()));
	connect(ui.tbEditEntry, SIGNAL(released()), this, SLOT(editEntry()));
	connect(ui.tbDelEntry, SIGNAL(released()), this, SLOT(delEntry()));
	connect(ui.tvMapView, SIGNAL(doubleClicked(QModelIndex)), this, SLOT(editEntry()));
}

// Every connected pad, and - when the one this slot remembers is not among
// them - that one too, marked as away. A pad asleep in a drawer must still
// be the selected item, or closing this page would throw it away.
void xGamepadWidget::updateList() {
	QList<xPadDev> devs = xGamepad::devList();
	xPadId want = gpad->padId();
	int sel = 0;
	ui.cbGPName->blockSignals(true);			// don't call devChanged automaticly
	ui.cbGPName->clear();
	ui.cbGPName->addItem("none");
	for (int i = 0; i < devs.size(); i++) {
		ui.cbGPName->addItem(devs.at(i).label, devs.at(i).id.toConfig());
		if (devs.at(i).id.sameAs(want))
			sel = ui.cbGPName->count() - 1;
	}
	if (!sel && !want.isEmpty()) {
		ui.cbGPName->addItem(QString("%0 (not connected)").arg(want.title()), want.toConfig());
		sel = ui.cbGPName->count() - 1;
	}
	ui.cbGPName->setEnabled(true);
	ui.cbGPName->setCurrentIndex(sel);
	ui.cbGPName->blockSignals(false);
}

void xGamepadWidget::update(std::string mapname) {
	QStringList lst;
	int i;
	updateList();
	ui.sldDeadZone->setValue(gpad->deadZone());
	QDir dir(conf.path.confDir.c_str());
	ui.cbMapFile->clear();
	lst = dir.entryList(QStringList() << "*.pad",QDir::Files,QDir::Name);
	lst.prepend("none");
	ui.cbMapFile->addItems(lst);
	if (mapname.empty()) {
		ui.cbMapFile->setCurrentIndex(0);
	} else {
		i = ui.cbMapFile->findText(mapname.c_str());
		if (i < 0) i = 0;
		ui.cbMapFile->setCurrentIndex(i);
	}

	padmodel->update();
}

// Only an explicit 'none' clears the slot. Anything else is a device the
// user named, connected or not, and the controller works out which pad each
// slot ends up on.
void xGamepadWidget::setDevFromCombo() {
	if (ui.cbGPName->currentIndex() < 1) {
		gpad->close();
		gpad->setPadId(xPadId());
	} else {
		gpad->setPadId(xPadId::fromConfig(ui.cbGPName->currentData().toString()));
	}
	conf.gpctrl->rescan();
}

void xGamepadWidget::apply() {
	gpad->setDeadZone(ui.sldDeadZone->value());
	setDevFromCombo();
}

std::string xGamepadWidget::getMapName() {
	std::string str;
	if (ui.cbMapFile->currentIndex() == 0) return str;
	str = ui.cbMapFile->currentText().toStdString();
	return str;
}

// take effect at once, so the pad can be tried out without leaving the page
void xGamepadWidget::devChanged(int idx) {
	setDevFromCombo();
}

void xGamepadWidget::mapChanged(int idx) {
	if (idx < 1) {
		gpad->mapClear();
	} else {
		gpad->loadMap(ui.cbMapFile->currentText().toStdString());
	}
	padmodel->update();
}

void xGamepadWidget::addMap() {
	QString nam = QInputDialog::getText(this,"Enter...","New gamepad map name");
	if (nam.isEmpty()) return;
	nam.append(".pad");
	std::string name = nam.toStdString();
	if (padCreate(name)) {
		ui.cbMapFile->addItem(nam, nam);
		ui.cbMapFile->setCurrentIndex(ui.cbMapFile->count() - 1);
	} else {
		showInfo("Map with that name already exists");
	}
}

void xGamepadWidget::delMap() {
	if (ui.cbMapFile->currentIndex() == 0) return;
	QString name = ui.cbMapFile->currentText();
	if (name.isEmpty()) return;
	if (!areSure("Delete this map?")) return;
	padDelete(name.toStdString());
	ui.cbMapFile->removeItem(ui.cbMapFile->currentIndex());
	ui.cbMapFile->setCurrentIndex(0);
}

void xGamepadWidget::addEntry() {
	if (ui.cbMapFile->currentIndex() == 0) return;
	bindidx = -1;
	xJoyMapEntry jent;
	jent.dev = JOY_NONE;
	jent.dev = JMAP_JOY;
#if USE_SEQ_BIND
	jent.seq = QKeySequence();
#else
	jent.key = ENDKEY;
#endif
	jent.dir = XJ_NONE;
	jent.rpt = 0;
	emit s_edit_entry(gpad, jent);
//	padial->start(jent);
}

void xGamepadWidget::entryReady(xJoyMapEntry ent) {
	if (ent.type == JOY_NONE) return;
	if (ent.dev == JMAP_NONE) return;
	gpad->setItem(bindidx, ent);
	gpad->saveMap(ui.cbMapFile->currentText().toStdString());
	padmodel->update();
}

void xGamepadWidget::editEntry() {
	bindidx = ui.tvMapView->currentIndex().row();
	if (bindidx < 0) return;
	emit s_edit_entry(gpad, gpad->mapItem(bindidx));
//	padial->start(conf.joy.gpad->mapItem(bindidx));
}

extern bool qmidx_greater(const QModelIndex, const QModelIndex);

void xGamepadWidget::delEntry() {
	QModelIndexList lst = ui.tvMapView->selectionModel()->selectedRows();
	if (!lst.isEmpty()) {
		std::sort(lst.begin(), lst.end(), qmidx_greater);
		if (areSure("Delete this binding(s)?")) {
			int row;
			foreach(QModelIndex idx, lst) {
				row = idx.row();
				gpad->delItem(row); // map.erase(conf.joy.gpad->map.begin() + row);
			}
			padmodel->update();
			gpad->saveMap(ui.cbMapFile->currentText().toStdString());
		}
	}
}

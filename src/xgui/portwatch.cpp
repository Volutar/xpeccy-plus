#include "portwatch.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QToolButton>
#include <QVBoxLayout>

#include "../xcore/xcore.h"
#include "xgui.h"

// the ports as text, one line each, edited in place, with a tick for the ones
// actually watched. What a line says is checked when the list is taken back,
// see parsePort()

xPortWatch::xPortWatch(QWidget* p) : QWidget(p) {
	list = new QListWidget(this);
	list->setItemDelegate(new xItemDelegate(XTYPE_ADR));	// hex only, 4 digits
	QToolButton* tbAdd = new QToolButton(this);
	tbAdd->setIcon(QIcon(":/images/add.png"));
	tbAdd->setToolTip("Watch one more port");
	QToolButton* tbDel = new QToolButton(this);
	tbDel->setIcon(QIcon(":/images/cancel.png"));
	tbDel->setToolTip("Drop this port from the list");

	QVBoxLayout* buts = new QVBoxLayout;
	buts->addWidget(tbAdd);
	buts->addWidget(tbDel);
	buts->addStretch();

	QHBoxLayout* lay = new QHBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->addWidget(list);
	lay->addLayout(buts);

	connect(tbAdd, &QToolButton::released, this, [this](){
		if (list->count() >= PWATCH_MAX) return;
		addPort("7FFD", true);
		list->setCurrentRow(list->count() - 1);
		list->editItem(list->currentItem());
	});
	connect(tbDel, &QToolButton::released, this, [this](){
		delete list->takeItem(list->currentRow());
	});
}

void xPortWatch::addPort(QString port, bool on) {
	QListWidgetItem* itm = new QListWidgetItem(port);
	itm->setFlags(itm->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
	itm->setCheckState(on ? Qt::Checked : Qt::Unchecked);
	itm->setToolTip("The port in hex: four digits are the address as it is, two are\n"
			"a byte port, matched on the low byte alone. An unticked port\n"
			"keeps its place in the list and is not watched");
	list->addItem(itm);
}

// "7FFD" is watched, "-7FFD" is switched off - the same text the profile holds

void xPortWatch::setPorts(QStringList ports) {
	list->clear();
	foreach(QString port, ports) {
		bool on = !port.startsWith('-');
		addPort(on ? port : port.mid(1), on);
	}
}

// what can't be read is dropped here, so nothing invalid leaves the editor

QStringList xPortWatch::getPorts() {
	QStringList res;
	QStringList seen;
	int port, mask;
	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem* itm = list->item(i);
		if (!parsePort(itm->text(), &port, &mask)) continue;
		QString str = getPortString(port, mask);
		if (seen.contains(str)) continue;
		seen << str;
		res << ((itm->checkState() == Qt::Checked) ? str : "-" + str);
	}
	return res;
}

xPortWatchDialog::xPortWatchDialog(QWidget* p) : QDialog(p) {
	setWindowTitle("Watched Ports");
	wid = new xPortWatch(this);
	QDialogButtonBox* bbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->addWidget(wid);
	lay->addWidget(bbox);
	connect(bbox, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(bbox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

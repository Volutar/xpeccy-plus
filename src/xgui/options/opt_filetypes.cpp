#include "opt_filetypes.h"
#include "../xgui.h"
#include "../../xcore/xcore.h"
#include "../../xcore/filemachine.h"

#include <QComboBox>
#include <QHeaderView>
#include <QTableWidget>
#include <QVBoxLayout>

enum {
	FTC_FORMAT = 0,
	FTC_NEEDS,
	FTC_MACHINE
};

xFileTypesBox::xFileTypesBox(QWidget* par) : QWidget(par) {
	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	int cnt = 0;
	while (fm_rows()[cnt].key)
		cnt++;
	table = new QTableWidget(cnt, 3, this);
	table->setHorizontalHeaderLabels(QStringList() << "Format" << "Needs" << "Opened on");
	table->verticalHeader()->hide();
	// a line of text and a little air: sized to its contents a row takes the
	// combo box's height, which a style sheet pads out, and the header's own
	// default is taller still
	int rowh = table->fontMetrics().height() + 6;
	table->verticalHeader()->setMinimumSectionSize(rowh);
	table->verticalHeader()->setDefaultSectionSize(rowh);
	table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
	table->horizontalHeader()->setSectionResizeMode(FTC_FORMAT, QHeaderView::ResizeToContents);
	table->horizontalHeader()->setSectionResizeMode(FTC_NEEDS, QHeaderView::ResizeToContents);
	table->horizontalHeader()->setSectionResizeMode(FTC_MACHINE, QHeaderView::Stretch);
	table->setSelectionMode(QAbstractItemView::NoSelection);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setFocusPolicy(Qt::NoFocus);
	// tall enough for every row, so the window opens with no scroll bar
	table->ensurePolished();
	table->horizontalHeader()->ensurePolished();
	table->setMinimumHeight(table->horizontalHeader()->sizeHint().height() + cnt * rowh + 2 * table->frameWidth());
	lay->addWidget(table);
}

static void ft_add(QComboBox* box, const QString& text, const char* val, const char* tip) {
	box->addItem(text, QString(val));
	box->setItemData(box->count() - 1, QString(tip), Qt::ToolTipRole);
}

// A tape runs on anything, so Ask would never ask there and Auto never
// switches: such a row only keeps the machine, which is what Auto does, or
// names one. The others offer the machines that can take the format.
void xFileTypesBox::fill() {
	const xFileMac* rows = fm_rows();
	for (int i = 0; i < table->rowCount(); i++) {
		table->setItem(i, FTC_FORMAT, new QTableWidgetItem(rows[i].name));
		table->setItem(i, FTC_NEEDS, new QTableWidgetItem(fm_need_text(rows[i].need)));
		QComboBox* box = new QComboBox;
		if (rows[i].need == FMN_ANY) {
			ft_add(box, "Keep current", FM_AUTO, "Never switch the machine");
		} else {
			ft_add(box, "Auto", FM_AUTO, "Switch only when this machine cannot run the file");
			ft_add(box, "Ask", FM_ASK, "When this machine cannot run the file, ask which one to use");
			ft_add(box, "Keep current", FM_KEEP, "Never switch the machine");
		}
		box->insertSeparator(box->count());
		foreach(const xMachine& mac, xm_list()) {
			if (fm_runs(mac.id, rows[i].need))
				ft_add(box, xm_list_name(mac), mac.id.c_str(), "Always open on this machine");
		}
		// a machine that is not there any more comes out as Auto
		setRFIndex(box, QString::fromLocal8Bit(fm_pref(rows[i].key).c_str()), 0);
		table->setCellWidget(i, FTC_MACHINE, box);
	}
}

void xFileTypesBox::apply() {
	const xFileMac* rows = fm_rows();
	fm_clear();
	for (int i = 0; i < table->rowCount(); i++) {
		QComboBox* box = qobject_cast<QComboBox*>(table->cellWidget(i, FTC_MACHINE));
		fm_load(rows[i].key, box->currentData().toString().toLocal8Bit().data());
	}
}

void xFileTypesBox::defaults() {
	for (int i = 0; i < table->rowCount(); i++)
		qobject_cast<QComboBox*>(table->cellWidget(i, FTC_MACHINE))->setCurrentIndex(0);
}

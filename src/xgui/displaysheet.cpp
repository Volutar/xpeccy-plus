#include "displaysheet.h"

#include "xgui.h"

#include "../xcore/xcore.h"

#include <QComboBox>
#include <QGroupBox>
#include <QShowEvent>
#include <QTimer>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

#define	KNOB_WIDTH	200		// a slider, as wide as a list
#define	KNOB_STEPS	100		// of a parameter that names no step

namespace {

float knob_step(const xShdParam& p) {
	return (p.step > 0) ? p.step : (p.max - p.min) / KNOB_STEPS;
}

int knob_count(const xShdParam& p) {
	const float step = knob_step(p);
	return (step > 0) ? qMax(1, qRound((p.max - p.min) / step)) : 1;
}

// as many decimals as the step has
QString knob_text(const xShdParam& p, float val) {
	const float step = knob_step(p);
	const int dec = (step >= 1) ? 0 : (step >= 0.1f) ? 1 : (step >= 0.01f) ? 2 : 3;
	return QString::number(val, 'f', dec);
}

}

xDisplaySheet::xDisplaySheet(QWidget* parent) : QDialog(parent) {
	lay = new QVBoxLayout(this);
	// the window is as large as its rows need, whenever they change
	lay->setSizeConstraint(QLayout::SetFixedSize);
	QHBoxLayout* row = new QHBoxLayout;
	btnAll = new QPushButton(tr("All settings"));
	btnAll->setCheckable(true);
	btnAll->setToolTip(tr("The finer knobs too"));
	btnDef = new QPushButton(tr("Defaults"));
	btnDef->setToolTip(tr("Every knob back to where this look has it"));
	btnSave = new QPushButton(tr("Save as preset..."));
	btnSave->setToolTip(tr("A look of your own in the Display list"));
	btnDel = new QPushButton(tr("Delete preset"));
	QPushButton* btnClose = new QPushButton(tr("Close"));
	btnAll->setObjectName("dispAll");
	btnDef->setObjectName("dispDefaults");
	btnSave->setObjectName("dispSave");
	btnDel->setObjectName("dispDelete");
	btnClose->setObjectName("dispClose");
	foreach(QPushButton* btn, QList<QPushButton*>() << btnAll << btnDef << btnSave << btnDel << btnClose)
		btn->setAutoDefault(false);
	gpuLab = new QLabel;
	gpuLab->setToolTip(tr("What this look takes of the graphics card, a frame"));
	QFont fnt = gpuLab->font();
	fnt.setItalic(true);
	gpuLab->setFont(fnt);
	lay->addWidget(gpuLab);
	row->addWidget(btnAll);
	row->addWidget(btnDef);
	row->addStretch(1);
	row->addWidget(btnSave);
	row->addWidget(btnDel);
	row->addWidget(btnClose);
	lay->addLayout(row);
	connect(btnAll, &QPushButton::toggled, this, [this](bool on) {
		all = on;
		build();
	});
	connect(btnDef, &QPushButton::clicked, this, &xDisplaySheet::defaults);
	connect(btnSave, &QPushButton::clicked, this, &xDisplaySheet::savePreset);
	connect(btnDel, &QPushButton::clicked, this, &xDisplaySheet::deletePreset);
	connect(btnClose, &QPushButton::clicked, this, &QDialog::close);
	QTimer* tick = new QTimer(this);
	tick->setInterval(500);
	connect(tick, &QTimer::timeout, this, [this]() {
		if (!isVisible()) return;
		const double us = conf.vid.shdGpuUs;
		gpuLab->setText((us < 0) ? QString() : tr("GPU time: %1 ms a frame").arg(us / 1000.0, 0, 'f', 2));
	});
	tick->start();
}

void xDisplaySheet::showEvent(QShowEvent* ev) {
	if (!ev->spontaneous()) {
		conf.vid.shdGpuUs = -1;
		conf.vid.shdTiming++;
		gpuLab->setText(QString());
	}
	QDialog::showEvent(ev);
}

void xDisplaySheet::hideEvent(QHideEvent* ev) {
	if (!ev->spontaneous() && (conf.vid.shdTiming > 0)) conf.vid.shdTiming--;
	look_store_save();
	QDialog::hideEvent(ev);
}

void xDisplaySheet::setLook() {
	const xLook now = look_current();
	if (!(now == look)) lastOn.clear();	// what a stage had is its look's alone
	look = now;
	build();
}

void xDisplaySheet::build() {
	setWindowTitle(tr("Display: %1").arg(look.title));
	// a copy: the file is looked at again as the rows are made, and may have changed
	const xShdPreset* file = look_has_knobs(look) ? look_file(look.file) : nullptr;
	const QVector<xShdParam> params = file ? file->params : QVector<xShdParam>();
	const bool head = !params.isEmpty();
	bool more = false;
	xOptSheet sheet;
	if (head) {
		// A look of many stages stands in two columns of frames, about as tall as each
		// other, so that the window fits a screen: the frames in their order, the first
		// ones on the left. A frame weighs its rows and a row and a half for itself
		QList<int> rowsOf;
		QString group;
		bool first = true;
		for (const xShdParam& p : params) {
			if (p.more && !all) continue;
			const QString name = p.group.isEmpty() ? tr("Parameters") : p.group;
			if (first || (name != group)) rowsOf << 0;
			rowsOf.last()++;
			first = false;
			group = name;
		}
		float whole = 0;
		for (int n : rowsOf) whole += n + 1.5f;
		int left = rowsOf.size();		// frames in the left column; all: one column
		if ((rowsOf.size() > 1) && (whole > 16)) {
			float sum = 0;
			float best = whole;
			for (int i = 0; i < rowsOf.size() - 1; i++) {
				sum += rowsOf[i] + 1.5f;
				if (qAbs(whole - 2 * sum) < best) {
					best = qAbs(whole - 2 * sum);
					left = i + 1;
				}
			}
		}
		const bool columns = (left < rowsOf.size());
		int frame = -1;
		first = true;
		for (const xShdParam& p : params) {
			more = more || p.more;
			if (p.more && !all) continue;
			const QString name = p.group.isEmpty() ? tr("Parameters") : p.group;
			if (first || (name != group)) {
				frame++;
				QGroupBox* box = sheet.group(name, columns ? ((frame < left) ? 0 : 1) : -1);
				// a stage with a switch: off, its pass is not run and its knobs stand idle
				if (!p.groupSwitch.isEmpty()) {
					const QString sw = p.groupSwitch;
					const float on = p.groupOn;
					box->setObjectName("group_" + sw);
					box->setCheckable(true);
					box->setChecked(look_value(look, sw) != 0.0f);
					connect(box, &QGroupBox::toggled, this, [this, sw, on](bool yes) {
						float val = 0;
						// back on: what it had, if that was anything; else the file's value
						const float had = look_value(look, sw);
						if (yes) val = lastOn.value(sw, on);
						else if (had != 0.0f) lastOn[sw] = had;
						moved(sw, val);
						QMetaObject::invokeMethod(this, [this]() {build();}, Qt::QueuedConnection);
					});
				}
			}
			first = false;
			group = name;
			QWidget* wid = knob(p);
			QLabel* lab = sheet.row(p.text, wid);
			lab->setToolTip((p.flat && conf.vid.flatScreen)
				? tr("%1. Flat screen (View > Display) has it 0 unless set here").arg(p.tip) : p.tip);
		}
	} else {
		sheet.group(QString());
		sheet.wide(new QLabel(tr("This look has no settings.")));
	}
	// shown at once: a layout shows it a turn later, and the window would shrink and grow
	setUpdatesEnabled(false);
	if (body) {
		body->hide();
		lay->removeWidget(body);
		body->deleteLater();
	}
	body = sheet.body;
	lay->insertWidget(0, body);
	body->show();
	setUpdatesEnabled(true);
	btnAll->setVisible(more);
	btnAll->setText(all ? tr("Fewer settings") : tr("All settings"));
	btnDef->setEnabled(look_changed(look));
	btnSave->setEnabled(head);
	btnDel->setVisible(look.user);
}

// a slider with its figure, or a list where the values have names
QWidget* xDisplaySheet::knob(const xShdParam& p) {
	const float step = knob_step(p);
	const int count = knob_count(p);
	const float val = look_value(look, p.name);
	const int pos = qBound(0, qRound((val - p.min) / ((step > 0) ? step : 1.0f)), count);
	const QString name = p.name;
	const float min = p.min;
	const float max = p.max;
	// back to the look's own value: there while the knob is turned, its room kept when not
	QToolButton* back = new QToolButton;
	back->setIcon(QIcon(":/images/refresh.png"));
	back->setAutoRaise(true);
	back->setToolTip(tr("Back to this look's value"));
	back->setObjectName("back_" + name);
	QSizePolicy keep = back->sizePolicy();
	keep.setRetainSizeWhenHidden(true);
	back->setSizePolicy(keep);
	back->setVisible(look_turned(look, name));
	if (!p.names.isEmpty()) {
		QComboBox* box = new QComboBox;
		for (int i = 0; i <= count; i++)
			box->addItem((i < p.names.size()) ? p.names[i] : knob_text(p, min + i * step));
		box->setCurrentIndex(pos);
		box->setToolTip(p.tip);
		box->setObjectName("knob_" + name);
		connect(box, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, name, min, max, step, back](int idx) {
			moved(name, qMin(max, min + idx * step));
			back->setVisible(look_turned(look, name));
		});
		connect(back, &QToolButton::clicked, this, [this, name, min, step, count, box, back]() {
			const float own = look_own(look, name);
			box->blockSignals(true);
			box->setCurrentIndex(qBound(0, qRound((own - min) / ((step > 0) ? step : 1.0f)), count));
			box->blockSignals(false);
			moved(name, own);
			back->setVisible(false);
		});
		QWidget* wid = new QWidget;
		QHBoxLayout* row = new QHBoxLayout(wid);
		row->setContentsMargins(0, 0, 0, 0);
		row->addWidget(box);
		row->addStretch(1);
		row->addWidget(back);
		wid->setFocusProxy(box);
		return wid;
	}
	xSlider* sld = new xSlider;
	sld->setOrientation(Qt::Horizontal);
	sld->setRange(0, count);
	sld->setPageStep(qMax(1, count / 10));
	sld->setValue(pos);
	sld->setFixedWidth(KNOB_WIDTH);
	sld->setToolTip(p.tip);
	sld->setObjectName("knob_" + name);
	QLabel* fig = new QLabel(knob_text(p, val));
	fig->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	// as wide as its longest figure, so the sliders stand in one column
	const QFontMetrics fm = fig->fontMetrics();
	fig->setFixedWidth(qMax(fm.horizontalAdvance(knob_text(p, min)), fm.horizontalAdvance(knob_text(p, max))) + 4);
	const xShdParam prm = p;
	connect(sld, &QSlider::valueChanged, this, [this, fig, prm, step, back](int at) {
		const float now = qMin(prm.max, prm.min + at * step);
		fig->setText(knob_text(prm, now));
		moved(prm.name, now);
		back->setVisible(look_turned(look, prm.name));
	});
	connect(back, &QToolButton::clicked, this, [this, sld, fig, prm, step, count, back]() {
		// the look's own value as it is, if it is between two of the slider's steps
		const float own = look_own(look, prm.name);
		sld->blockSignals(true);
		sld->setValue(qBound(0, qRound((own - prm.min) / ((step > 0) ? step : 1.0f)), count));
		sld->blockSignals(false);
		fig->setText(knob_text(prm, own));
		moved(prm.name, own);
		back->setVisible(false);
	});
	QWidget* wid = new QWidget;
	QHBoxLayout* box = new QHBoxLayout(wid);
	box->setContentsMargins(0, 0, 0, 0);
	// what the row has over goes between the two: a short figure does not move its slider
	box->addWidget(sld);
	box->addStretch(1);
	box->addWidget(fig);
	box->addWidget(back);
	wid->setFocusProxy(sld);
	return wid;
}

void xDisplaySheet::moved(const QString& name, float value) {
	look_keep(look, name, value);
	btnDef->setEnabled(look_changed(look));
	emit s_param(name, value);
}

void xDisplaySheet::defaults() {
	lastOn.clear();
	look_reset(look);
	emit s_look();
	build();
}

void xDisplaySheet::savePreset() {
	bool ok = false;
	const QString name = QInputDialog::getText(this, tr("Save as Preset"), tr("Name"), QLineEdit::Normal,
		look.user ? look.title : QString(), &ok).trimmed();
	if (!ok || name.isEmpty()) return;
	const QString taken = look_preset_taken(name);
	if (!taken.isEmpty()
			&& (QMessageBox::question(this, tr("Save as Preset"), tr("Replace the preset \"%1\"?").arg(taken)) != QMessageBox::Yes))
		return;
	QString err;
	if (!look_save_preset(look, name, &err)) {
		QMessageBox::warning(this, tr("Save as Preset"), err);
		return;
	}
	look_store_save();
	emit s_look();
	setLook();
}

void xDisplaySheet::deletePreset() {
	if (!look.user) return;
	if (QMessageBox::question(this, tr("Delete Preset"), tr("Delete the preset \"%1\"?").arg(look.title),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;
	if (!look_delete_preset(look)) return;
	look_store_save();
	emit s_look();
	setLook();
}

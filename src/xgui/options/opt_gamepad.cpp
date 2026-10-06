#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QVBoxLayout>

#include "opt_gamepad.h"

#include "../xgui.h"
#include "../../xcore/xcore.h"

static QLabel* padNote(const QString& txt) {
	QLabel* lab = new QLabel(txt);
	QFont fnt = lab->font();
	fnt.setItalic(true);
	lab->setFont(fnt);
	return lab;
}

// Table model

xPadTableModel::xPadTableModel(xGamepad* gp, QObject* p):QAbstractTableModel(p) {
	gpad = gp;
	update();
}

void xPadTableModel::update() {
	beginResetModel();
	shown.clear();
	for (int i = 0; i < gpad->rowCount(); i++) {
		if (gpad->rowShown(i)) shown.append(i);
	}
	endResetModel();
}

int xPadTableModel::padRow(int r) const {
	return ((r >= 0) && (r < shown.size())) ? shown.at(r) : -1;
}

int xPadTableModel::rowCount(const QModelIndex& idx) const {
	return idx.isValid() ? 0 : shown.size();
}

int xPadTableModel::columnCount(const QModelIndex& idx) const {
	return idx.isValid() ? 0 : 3;
}

QVariant xPadTableModel::headerData(int sec, Qt::Orientation ori, int role) const {
	if ((ori != Qt::Horizontal) || (role != Qt::DisplayRole)) return QVariant();
	switch (sec) {
		case 0: return QString("Spectrum");
		case 1: return QString(gpad->isKeyboard() ? "PC Keyboard" : "Gamepad");
		case 2: return QString("Turbo");
	}
	return QVariant();
}

QVariant xPadTableModel::data(const QModelIndex& idx, int role) const {
	if (!idx.isValid()) return QVariant();
	int row = padRow(idx.row());
	if (row < 0) return QVariant();
	switch (role) {
		case Qt::DisplayRole:
			if (idx.column() == 0) return gpad->rowName(row);
			if (idx.column() == 1) {
				QString ins = xGamepad::inputsName(gpad->liveInputs(row));
				return ins.isEmpty() ? QString("-") : ins;
			}
			break;
		case Qt::ToolTipRole:
			// what "Gamepad up" stands for on this pad
			if (idx.column() == 1) {
				QStringList res;
				foreach(const xJoyMapEntry& e, gpad->liveInputs(row)) {
					if (e.type == JOY_VDIR) res.append(xGamepad::inputsName(gpad->aliasInputs(e.num)));
				}
				if (!res.isEmpty()) return res.join(", ");
			}
			break;
		case Qt::FontRole:
			if ((idx.column() == 0) && (row < PR_JOY)) {
				QFont fnt;
				fnt.setBold(true);
				return fnt;
			}
			break;
		case Qt::CheckStateRole:
			if (idx.column() == 2) return gpad->row(row).rapid ? Qt::Checked : Qt::Unchecked;
			break;
	}
	return QVariant();
}

// Input box

xInputBox::xInputBox(QWidget* p):QLabel(p) {
	setFocusPolicy(Qt::StrongFocus);
	setFrameShape(QFrame::StyledPanel);
	setWordWrap(true);
	setMinimumHeight(fontMetrics().height() * 2 + 8);
	setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
}

// every key but the two a dialog cannot do without
void xInputBox::keyPressEvent(QKeyEvent* ev) {
	if ((ev->key() == Qt::Key_Escape) || (ev->key() == Qt::Key_Tab) || (ev->key() == Qt::Key_Backtab)) {
		QLabel::keyPressEvent(ev);
		return;
	}
	if (!ev->isAutoRepeat() && onKey) onKey(pad_key_id(ev));
	ev->accept();
}

void xInputBox::mousePressEvent(QMouseEvent* ev) {
	setFocus();
	QLabel::mousePressEvent(ev);
}

// Row editor

xPadRowEdit::xPadRowEdit(QWidget* p):QDialog(p) {
	setModal(true);
	gpad = NULL;
	idx = -1;
	fresh = true;

	// left: what the Spectrum gets
	QGroupBox* left = new QGroupBox("Spectrum");
	QGridLayout* lg = new QGridLayout(left);
	labFixed = new QLabel;
	lg->addWidget(labFixed, 0, 0, 1, 3);
	rbKey = new QRadioButton("Key");
	cbKey = new QComboBox;
	const char* zx = pad_zx_keys();
	for (int i = 0; zx[i]; i++) cbKey->addItem(pad_zx_name(zx[i]), int(zx[i]));
	btnPress = new QPushButton("Press a key");
	btnPress->setMinimumWidth(btnPress->sizeHint().width());	// the longer of its two texts
	btnPress->setText("Press");
	btnPress->setCheckable(true);
	btnPress->setToolTip("Press a PC key: the Spectrum key it is on");
	btnPress->installEventFilter(this);
	labMod = new QLabel("with");
	labMod->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	cbMod = new QComboBox;
	cbMod->addItem("-", 0);
	cbMod->addItem(pad_zx_name('C'), int('C'));
	cbMod->addItem(pad_zx_name('S'), int('S'));
	cbMod->setToolTip("Held with the key: Caps Shift and 5 is cursor left");
	rbJoy = new QRadioButton("Kempston");
	cbJoy = new QComboBox;
	cbJoy->addItem("Up", XJ_UP);
	cbJoy->addItem("Down", XJ_DOWN);
	cbJoy->addItem("Left", XJ_LEFT);
	cbJoy->addItem("Right", XJ_RIGHT);
	cbJoy->addItem("Fire", XJ_FIRE);
	cbJoy->addItem("Fire 2", XJ_BUT2);
	cbJoy->addItem("Fire 3", XJ_BUT3);
	cbJoy->addItem("Fire 4", XJ_BUT4);
	rbCut = new QRadioButton("Action");
	cbCut = new QComboBox;
	xShortcut* tab = shortcut_tab();
	for (int i = 0; tab[i].text; i++) {
		if (tab[i].grp & SCG_MAIN) cbCut->addItem(tab[i].text, tab[i].id);
	}
	cbCut->model()->sort(0);
	rbKept = new QRadioButton("As it was");
	labKept = new QLabel;
	lg->addWidget(rbKey, 1, 0);
	lg->addWidget(cbKey, 1, 1);
	lg->addWidget(btnPress, 1, 2);
	lg->addWidget(labMod, 2, 0);
	lg->addWidget(cbMod, 2, 1);
	lg->addWidget(rbJoy, 3, 0);
	lg->addWidget(cbJoy, 3, 1);
	lg->addWidget(rbCut, 4, 0);
	lg->addWidget(cbCut, 4, 1, 1, 2);
	lg->addWidget(rbKept, 5, 0);
	lg->addWidget(labKept, 5, 1, 1, 2);
	lg->setRowStretch(6, 1);
	tgtBox = left;
	connect(cbKey, QOverload<int>::of(&QComboBox::activated), rbKey, [this]() {rbKey->setChecked(true);});
	connect(cbMod, QOverload<int>::of(&QComboBox::activated), rbKey, [this]() {rbKey->setChecked(true);});
	connect(cbJoy, QOverload<int>::of(&QComboBox::activated), rbJoy, [this]() {rbJoy->setChecked(true);});
	connect(cbCut, QOverload<int>::of(&QComboBox::activated), rbCut, [this]() {rbCut->setChecked(true);});
	connect(btnPress, &QPushButton::toggled, this, [this](bool on) {
		btnPress->setText(on ? "Press a key" : "Press");
		if (on) {
			rbKey->setChecked(true);
			btnPress->setFocus();		// the key goes to it
		}
	});

	// right: what presses it
	QGroupBox* right = new QGroupBox;
	right->setObjectName("inputs");
	QGridLayout* rg = new QGridLayout(right);
	inBox = new xInputBox;
	inBox->setMinimumWidth(220);
	rg->addWidget(inBox, 0, 0, 1, 2);
	cbAlias = new QComboBox;
	cbAlias->setToolTip("An input picked from the list: no pad needed");
	QPushButton* btnClear = new QPushButton("Clear");
	btnDefault = new QPushButton("Defaults");
	QHBoxLayout* rb = new QHBoxLayout;
	rb->addWidget(cbAlias);
	rb->addStretch(1);
	rb->addWidget(btnClear);
	rb->addWidget(btnDefault);
	rg->addLayout(rb, 1, 0, 1, 2);
	labHint = padNote(QString());
	labHint->setWordWrap(true);
	rg->addWidget(labHint, 2, 0, 1, 2);
	sldDead = new QSlider(Qt::Horizontal);
	sldDead->setRange(0, 32768);
	sldDead->setToolTip("How far a stick has to move before it counts");
	deadRow = new QWidget;
	QHBoxLayout* db = new QHBoxLayout(deadRow);
	db->setContentsMargins(0, 0, 0, 0);
	db->addWidget(new QLabel("Stick dead zone"));
	db->addWidget(sldDead, 1);
	rg->setRowStretch(3, 1);

	connect(cbAlias, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
		if ((i < 1) || (i > choices.size())) return;
		addInput(choices.at(i - 1));
		cbAlias->setCurrentIndex(0);
		inBox->setFocus();
	});
	connect(btnClear, &QPushButton::clicked, this, [this]() {
		inputs().clear();
		defFlag() = false;
		fresh = false;
		showInputs();
		inBox->setFocus();
	});
	connect(btnDefault, &QPushButton::clicked, this, [this]() {
		defFlag() = true;
		fresh = true;
		showInputs();
		inBox->setFocus();
	});
	connect(sldDead, &QSlider::valueChanged, this, [this](int v) {if (gpad) gpad->setDeadZone(v);});
	inBox->onKey = [this](int id) {
		if (!gpad->isKeyboard()) return;
		xJoyMapEntry e;
		e.type = JOY_KEY;
		e.num = id;
		addInput(e);
	};

	chkTurbo = new QCheckBox("Turbo: repeats while held");
	QDialogButtonBox* bbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	bbox->button(QDialogButtonBox::Ok)->setIcon(QIcon(":/images/ok-apply.png"));		// as Options has them
	bbox->button(QDialogButtonBox::Cancel)->setIcon(QIcon(":/images/cancel.png"));
	connect(bbox, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(bbox, &QDialogButtonBox::rejected, this, &QDialog::reject);
	QHBoxLayout* cols = new QHBoxLayout;
	// at least as wide as with every control in it, whatever a row hides
	left->setMinimumWidth(left->sizeHint().width());
	cols->addWidget(left);
	cols->addWidget(padNote(QString::fromUtf8("\xe2\x86\x90")));		// a left arrow: the right side presses the left
	cols->addWidget(right, 1);
	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->addLayout(cols);
	lay->addWidget(deadRow);
	lay->addWidget(chkTurbo);
	lay->addWidget(bbox);
	// Enter is a key a player may well want to bind
	foreach(QPushButton* b, findChildren<QPushButton*>()) {
		b->setAutoDefault(false);
		b->setDefault(false);
	}
}

// "Press": the next PC key picks the Spectrum key the layout has it on
bool xPadRowEdit::eventFilter(QObject* obj, QEvent* ev) {
	if ((obj == btnPress) && btnPress->isChecked() && (ev->type() == QEvent::KeyPress)) {
		QKeyEvent* kev = (QKeyEvent*)ev;
		if (kev->key() == Qt::Key_Escape) {
			btnPress->setChecked(false);
			return true;
		}
		keyEntry kent = getKeyEntry(pad_key_id(kev));
		if (kent.zxKey[0] && !kent.zxKey[1]) {
			int i = cbKey->findData(int(kent.zxKey[0]));
			if (i >= 0) cbKey->setCurrentIndex(i);
		}
		btnPress->setChecked(false);
		return true;
	}
	return QDialog::eventFilter(obj, ev);
}

QList<xJoyMapEntry>& xPadRowEdit::inputs() {
	return gpad->isKeyboard() ? cur.key : cur.pad;
}

// whether those are the defaults: only a joystick row has any
bool& xPadRowEdit::defFlag() {
	return gpad->isKeyboard() ? cur.keyDef : cur.padDef;
}

// The first input caught replaces what the row had, the next ones add to it.
void xPadRowEdit::addInput(const xJoyMapEntry& in) {
	if (fresh) {
		inputs().clear();
		fresh = false;
	}
	defFlag() = false;
	foreach(const xJoyMapEntry& e, inputs()) {
		if ((e.type == in.type) && (e.num == in.num) && (e.state == in.state)) return;
	}
	inputs().append(in);
	showInputs();
}

void xPadRowEdit::showInputs() {
	QList<xJoyMapEntry> lst;
	if ((idx >= 0) && (idx < PR_JOY) && defFlag()) {
		lst = gpad->defInputs(idx, gpad->isKeyboard());
	} else {
		lst = inputs();
	}
	QString txt = xGamepad::inputsName(lst);
	if (txt.isEmpty()) txt = gpad->isKeyboard() ? "Nothing yet: click here and press a key" : "Nothing yet";
	inBox->setText(txt);
}

// what the left side says, into cur; false if it says nothing
bool xPadRowEdit::takeTarget() {
	xJoyMapEntry t;
	if (rbKept->isChecked()) {
		cur.tgt = kept;
		return !kept.isEmpty();
	}
	if (rbJoy->isChecked()) {
		t.dev = JMAP_JOY;
		t.dir = cbJoy->currentData().toInt();
	} else if (rbCut->isChecked()) {
		t.dev = JMAP_CUT;
		t.dir = cbCut->currentData().toInt();
	} else {
		t.dev = JMAP_ZX;
		t.dir = cbKey->currentData().toInt();
	}
	cur.tgt.clear();
	int mod = cbMod->currentData().toInt();
	if ((t.dev == JMAP_ZX) && mod && (mod != t.dir)) {	// the modifier goes down first
		xJoyMapEntry m = t;
		m.dir = mod;
		cur.tgt.append(m);
	}
	cur.tgt.append(t);
	return true;
}

bool xPadRowEdit::edit(xGamepad* gp, int i) {
	gpad = gp;
	idx = i;
	cur = (i < 0) ? xPadRow() : gp->row(i);
	fresh = (i >= 0);
	bool joy = (i >= 0) && (i < PR_JOY);
	bool custom = joy && (gp->scheme() == GPS_CUSTOM);
	bool own = !joy || custom;
	setWindowTitle(joy ? QString(pad_role_name(i)) : QString((i < 0) ? "New binding" : "Binding"));
	// the left side: the joystick's own and fixed, a key on Custom, anything on an extra row
	QStringList tnames;
	foreach(const xJoyMapEntry& t, gp->rowTargets(i)) tnames.append(xGamepad::getTargetName(t));
	labFixed->setText(QString("%0: %1").arg(gp->rowName(i).section(' ', 0, 0), tnames.join(" + ")));
	labFixed->setVisible(!own);
	foreach(QWidget* w, QList<QWidget*>() << rbKey << cbKey << btnPress << labMod << cbMod) w->setVisible(own);
	foreach(QWidget* w, QList<QWidget*>() << rbJoy << cbJoy << rbCut << cbCut) w->setVisible(own && !custom);
	QList<xJoyMapEntry> tgt = (i < 0) ? QList<xJoyMapEntry>() : gp->rowTargets(i);
	kept.clear();
	rbKey->setChecked(true);
	cbKey->setCurrentIndex(0);
	cbMod->setCurrentIndex(0);
	if (!tgt.isEmpty()) {
		const xJoyMapEntry& t = tgt.first();
		switch (t.dev) {
			case JMAP_ZX:
				// Caps or Symbol Shift and a key after it: the modifier and the key
				if ((tgt.size() > 1) && (tgt.at(1).dev == JMAP_ZX) && ((t.dir == 'C') || (t.dir == 'S'))) {
					cbMod->setCurrentIndex(qMax(0, cbMod->findData(t.dir)));
					cbKey->setCurrentIndex(qMax(0, cbKey->findData(tgt.at(1).dir)));
				} else {
					cbKey->setCurrentIndex(qMax(0, cbKey->findData(t.dir)));
				}
				break;
			case JMAP_JOY:
				rbJoy->setChecked(true);
				cbJoy->setCurrentIndex(qMax(0, cbJoy->findData(t.dir)));
				break;
			case JMAP_CUT:
				rbCut->setChecked(true);
				cbCut->setCurrentIndex(qMax(0, cbCut->findData(t.dir)));
				break;
			default:		// a PC key or the mouse, from an old .pad
				kept = tgt;
				rbKept->setChecked(true);
				break;
		}
	}
	rbKept->setVisible(own && !kept.isEmpty());
	labKept->setVisible(own && !kept.isEmpty());
	labKept->setText(xGamepad::targetsName(kept));
	// the right side: the device in use
	bool keys = gp->isKeyboard();
	findChild<QGroupBox*>("inputs")->setTitle(keys ? "PC Keyboard" : "Gamepad");
	cbAlias->setVisible(!keys);
	cbAlias->clear();
	cbAlias->addItem("Add...");
	choices = xGamepad::inputChoices(gp->asController());
	int lw = 0;
	foreach(const xJoyMapEntry& e, choices) {
		QString nm = xGamepad::getEntryName(e);
		cbAlias->addItem(nm);
		lw = qMax(lw, cbAlias->fontMetrics().horizontalAdvance(nm));
	}
	// the list as wide as its names, not as the box: a scroll bar and margins on top
	cbAlias->view()->setMinimumWidth(lw + cbAlias->style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 24);
	btnDefault->setVisible(joy);
	labHint->setText(keys ? "Press keys here to bind them" : "Press buttons on the pad to bind them");
	deadRow->setVisible(!keys);
	sldDead->blockSignals(true);
	sldDead->setValue(gp->deadZone());
	sldDead->blockSignals(false);
	chkTurbo->setChecked(cur.rapid);
	btnPress->setChecked(false);
	showInputs();
	// a button pressed on the pad: by name where SDL has the layout, never twice
	QMetaObject::Connection con = connect(gp, &xGamepad::inputChanged, this, [this](int type, int num, int state) {
		if (!isActiveWindow() || gpad->isKeyboard() || (state == 0) || !gpad->bindable(type)) return;
		xJoyMapEntry e;
		e.type = type;
		e.num = num;
		e.state = (state < 0) ? -1 : 1;
		addInput(e);
	});
	inBox->setFocus();
	adjustSize();
	bool ok = false;
	if (exec() == QDialog::Accepted) {
		if (!own || takeTarget()) {
			cur.rapid = chkTurbo->isChecked();
			gp->setRow(i, cur);
			ok = true;
		}
	}
	disconnect(con);
	return ok;
}

// One player's panel

xGamepadWidget::xGamepadWidget(xGamepad* gp, QWidget* p):QWidget(p) {
	gpad = gp;
	editor = new xPadRowEdit(this);
	QGridLayout* grid = new QGridLayout(this);
	grid->setColumnStretch(1, 1);
	int row = 0;

	cbDevice = new QComboBox;
	grid->addWidget(new QLabel("Device"), row, 0);
	grid->addWidget(cbDevice, row++, 1);
	// what the last press did, so a pad can be tried out right here
	labTry = padNote("Press a button to try it");
	grid->addWidget(labTry, row++, 1);

	// the joysticks, two columns; Kempston and QAOP each carry a choice of their own
	QWidget* radios = new QWidget;
	QGridLayout* rgrid = new QGridLayout(radios);
	rgrid->setContentsMargins(0, 0, 0, 0);
	grpScheme = new QButtonGroup(this);
	// a radio button, and what goes right after it in its cell
	auto radio = [this, rgrid](int id, const char* text, int r, int c, QWidget* tail = nullptr) {
		QRadioButton* rb = new QRadioButton(text);
		grpScheme->addButton(rb, id);
		rgrid->addWidget(fieldPair(rb, tail, false), r, c);
		return rb;
	};
	radio(GPS_KEMPSTON, "Kempston", 0, 0);
	radio(GPS_SINCLAIR1, pad_scheme_name(GPS_SINCLAIR1), 1, 0);
	radio(GPS_SINCLAIR2, pad_scheme_name(GPS_SINCLAIR2), 2, 0);
	radio(GPS_CURSOR, pad_scheme_name(GPS_CURSOR), 0, 1);
	cbQaop = new QComboBox;
	cbQaop->addItem("Space", GPS_QAOP);
	cbQaop->addItem("M", GPS_QAOPM);
	cbQaop->setToolTip("The key fire presses");
	radio(GPS_QAOP, "QAOP", 1, 1, cbQaop);
	radio(GPS_CUSTOM, "Custom", 2, 1)->setToolTip("Spectrum keys of your own");
	rgrid->setColumnStretch(1, 1);
	grpScheme->button(GPS_KEMPSTON)->setToolTip("Switched on by itself when in use.\nFire 2-4 come with an 8-button one");
	grid->addWidget(new QLabel("Joystick"), row, 0, Qt::AlignTop);
	grid->addWidget(radios, row++, 1);

	model = new xPadTableModel(gp, this);
	table = new QTableView;
	table->setModel(model);
	table->setItemDelegateForColumn(2, new xCheckItem(table));
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setContextMenuPolicy(Qt::CustomContextMenu);
	table->verticalHeader()->hide();
	table->verticalHeader()->setDefaultSectionSize(table->fontMetrics().height() + 4);
	// the eight rows of Kempston 8 and an extra one, with no scrolling
	int rowh = qMax(table->verticalHeader()->defaultSectionSize(), table->verticalHeader()->minimumSectionSize());
	table->setMinimumHeight(rowh * 10 + table->horizontalHeader()->sizeHint().height() + 4);
	table->setMinimumWidth(420);
	QHeaderView* hdr = table->horizontalHeader();
	hdr->setStretchLastSection(false);
	hdr->setHighlightSections(false);
	hdr->setSectionResizeMode(0, QHeaderView::Fixed);	// see nameWidth()
	hdr->setSectionResizeMode(1, QHeaderView::Stretch);
	hdr->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	// the icon at the right edge, as the Machine page's buttons have it
	auto sideButton = [](const char* text, const char* icon) {
		xSideButton* btn = new xSideButton;
		btn->setText(text);
		btn->setIcon(QIcon(icon));
		btn->setAutoDefault(false);		// none of them is what Enter should press
		return btn;
	};
	QPushButton* btnSave = sideButton("Save as...", ":/images/floppy.png");
	btnSave->setToolTip("Keep these bindings in a file, for a game or to share");
	QPushButton* btnLoad = sideButton("Load...", ":/images/fileopen.png");
	QPushButton* btnReset = sideButton("Reset", ":/images/refresh.png");
	btnReset->setToolTip("Back to the joystick's defaults");
	QVBoxLayout* bbox = new QVBoxLayout;
	bbox->addWidget(btnSave);
	bbox->addWidget(btnLoad);
	bbox->addSpacing(12);
	bbox->addWidget(btnReset);
	bbox->addStretch(1);
	QPushButton* btnAdd = sideButton("Add mapping", ":/images/add.png");
	btnAdd->setToolTip("A key, a button or an action of your own");
	QHBoxLayout* abox = new QHBoxLayout;
	abox->addStretch(1);
	abox->addWidget(btnAdd);
	sldTurbo = new QSlider(Qt::Horizontal);
	sldTurbo->setRange(1, 25);
	labTurbo = new QLabel;
	labTurbo->setMinimumWidth(labTurbo->fontMetrics().horizontalAdvance("25 Hz"));
	QHBoxLayout* rbox = new QHBoxLayout;
	rbox->addWidget(new QLabel("Turbo rate"));
	rbox->addWidget(fieldPair(sldTurbo, labTurbo, true), 1);
	QVBoxLayout* tcol = new QVBoxLayout;
	tcol->addWidget(table, 1);
	tcol->addLayout(abox);
	tcol->addLayout(rbox);
	QHBoxLayout* tbox = new QHBoxLayout;
	tbox->addLayout(tcol, 1);
	tbox->addLayout(bbox);
	grid->addLayout(tbox, row++, 0, 1, 2);
	grid->addWidget(padNote("Double-click a row to change it, right-click for more"), row++, 0, 1, 2);
	grid->setRowStretch(row, 1);

	connect(cbDevice, QOverload<int>::of(&QComboBox::activated), this, [this]() {
		setDevFromCombo();
		tell();
	});
	connect(grpScheme, QOverload<QAbstractButton*>::of(&QButtonGroup::buttonClicked), this, [this]() {schemeFromControls();});
	connect(cbQaop, QOverload<int>::of(&QComboBox::activated), this, [this]() {
		grpScheme->button(GPS_QAOP)->setChecked(true);
		schemeFromControls();
	});
	connect(table, &QTableView::doubleClicked, this, [this](const QModelIndex& idx) {
		if (idx.column() == 2) return;		// that one is the turbo box
		editRow(model->padRow(idx.row()));
	});
	connect(btnAdd, &QPushButton::clicked, this, [this]() {editRow(-1);});
	connect(table, &QTableView::clicked, this, [this](const QModelIndex& idx) {
		if (idx.column() == 2) toggleTurbo(model->padRow(idx.row()));
	});
	connect(table, &QTableView::customContextMenuRequested, this, [this](const QPoint& pos) {rowMenu(pos);});
	connect(btnSave, &QPushButton::clicked, this, [this]() {saveAs();});
	connect(btnLoad, &QPushButton::clicked, this, [this]() {load();});
	connect(btnReset, &QPushButton::clicked, this, [this]() {reset();});
	connect(sldTurbo, &QSlider::valueChanged, this, [this](int v) {
		gpad->setTurboRate(v);
		labTurbo->setText(QString("%0 Hz").arg(v));
	});
	connect(gpad, &xGamepad::inputChanged, this, [this](int t, int n, int s) {inputChanged(t, n, s);});
}

void xGamepadWidget::tell() {
	if (changed) changed();
}

// What the name column needs, worked out from the text rather than asked of the
// view: the table on the tab not shown has no rows laid out to ask.
int xGamepadWidget::nameWidth() {
	QFont bold = table->font();
	bold.setBold(true);
	QFontMetrics fmb(bold);
	QFontMetrics fm(table->font());
	int w = table->horizontalHeader()->fontMetrics().horizontalAdvance("Spectrum");
	for (int r = 0; r < model->rowCount(); r++) {
		int i = model->padRow(r);
		QString txt = gpad->rowName(i);
		w = qMax(w, ((i < PR_JOY) ? fmb : fm).horizontalAdvance(txt));
	}
	return w + 20;		// the cell's margins
}

void xGamepadWidget::setNameWidth(int w) {
	table->horizontalHeader()->resizeSection(0, w);
}

void xGamepadWidget::tableChanged() {
	model->update();
	tell();
}

void xGamepadWidget::schemeFromControls() {
	int id = grpScheme->checkedId();
	if (id == GPS_QAOP) id = cbQaop->currentData().toInt();
	gpad->setScheme(id);
	tableChanged();
}

// Every connected pad and the keyboard, and - when the pad this player
// remembers is not among them - that one too, marked as away. A pad asleep
// in a drawer must still be the selected item, or a refresh would throw it away.
void xGamepadWidget::updateList() {
	QList<xPadDev> devs = xGamepad::devList();
	xPadId want = gpad->padId();
	int sel = 0;
	cbDevice->blockSignals(true);			// not a choice of the user's
	cbDevice->clear();
	cbDevice->addItem("None");
	for (int k = GPK_ARROWS; k <= GPK_WASD; k++)
		cbDevice->addItem(pad_kbd_name(k), pad_kbd_key(k));
	if (gpad->keyboard() == GPK_ARROWS) sel = 1;
	if (gpad->keyboard() == GPK_WASD) sel = 2;
	for (int i = 0; i < devs.size(); i++) {
		cbDevice->addItem(devs.at(i).label, devs.at(i).id.toConfig());
		if (!gpad->isKeyboard() && devs.at(i).id.sameAs(want))
			sel = cbDevice->count() - 1;
	}
	if (!sel && !want.isEmpty()) {
		cbDevice->addItem(QString("%0 (not connected)").arg(want.title()), want.toConfig());
		sel = cbDevice->count() - 1;
	}
	cbDevice->setCurrentIndex(sel);
	cbDevice->blockSignals(false);
}

void xGamepadWidget::refresh() {
	updateList();
	int s = gpad->scheme();
	cbQaop->setCurrentIndex((s == GPS_QAOPM) ? 1 : 0);
	s = pad_scheme_kind(s);
	QAbstractButton* rb = grpScheme->button(s);
	if (rb) rb->setChecked(true);
	sldTurbo->blockSignals(true);
	sldTurbo->setValue(gpad->turboRate());
	sldTurbo->blockSignals(false);
	labTurbo->setText(QString("%0 Hz").arg(gpad->turboRate()));
	labTry->setText(gpad->isKeyboard() ? "Press a key to try it" : "Press a button to try it");
	model->update();
}

// Only an explicit None clears the player. Anything else is a device the user
// named, connected or not, and the controller works out which pad each player
// ends up on.
void xGamepadWidget::setDevFromCombo() {
	QString dat = cbDevice->currentData().toString();
	int kbd = pad_kbd_find(dat);
	gpad->setKeyboard(kbd);
	if (kbd == GPK_NONE) {
		if (cbDevice->currentIndex() < 1) {
			gpad->close();
			gpad->setPadId(xPadId());
		} else {
			gpad->setPadId(xPadId::fromConfig(dat));
		}
	}
	conf.gpctrl->rescan();
	refresh();
}

void xGamepadWidget::editRow(int i) {
	if ((i < -1) || (i >= gpad->rowCount())) return;
	if (editor->edit(gpad, i)) tableChanged();
}

void xGamepadWidget::delRow(int i) {
	if (i < 0) return;
	gpad->delRow(i);
	tableChanged();
}

void xGamepadWidget::toggleTurbo(int i) {
	if (i < 0) return;
	xPadRow r = gpad->row(i);
	r.rapid = !r.rapid;
	gpad->setRow(i, r);
	tableChanged();
}

void xGamepadWidget::rowMenu(const QPoint& pos) {
	QModelIndex idx = table->indexAt(pos);
	int i = idx.isValid() ? model->padRow(idx.row()) : -1;
	QMenu menu(this);
	menu.addAction(QIcon(":/images/add.png"), "Add...", this, [this]() {editRow(-1);});
	if (i >= 0) {
		menu.addAction(QIcon(":/images/edit.png"), "Change...", this, [this, i]() {editRow(i);});
		menu.addAction(QIcon((i < PR_JOY) ? ":/images/refresh.png" : ":/images/cancel.png"),
			(i < PR_JOY) ? "Back to defaults" : "Delete", this, [this, i]() {delRow(i);});
		QAction* act = menu.addAction("Turbo", this, [this, i]() {toggleTurbo(i);});
		act->setCheckable(true);
		act->setChecked(gpad->row(i).rapid);
	}
	menu.exec(table->viewport()->mapToGlobal(pos));
}

void xGamepadWidget::saveAs() {
	QString path = QFileDialog::getSaveFileName(this, "Save bindings", QString::fromStdString(conf.path.confDir),
		"Bindings (*.pad)");
	if (path.isEmpty()) return;
	if (!path.endsWith(".pad")) path.append(".pad");
	if (!gpad->saveFile(path.toStdString())) showInfo("Can't write the file");
}

void xGamepadWidget::load() {
	QString path = QFileDialog::getOpenFileName(this, "Load bindings", QString::fromStdString(conf.path.confDir),
		"Bindings (*.pad)");
	if (path.isEmpty()) return;
	if (!gpad->loadFile(path.toStdString())) {
		showInfo("Can't read the file");
		return;
	}
	refresh();
	tell();
}

void xGamepadWidget::reset() {
	if (!areSure("Back to the joystick's defaults? Extra rows go.")) return;
	gpad->resetRows();
	tableChanged();
}

void xGamepadWidget::tryShow(const xJoyMapEntry& ev) {
	QString what = gpad->whatDoes(ev.type, ev.num, ev.state).join(", ");
	labTry->setText(QString("%0: %1").arg(xGamepad::getEntryName(ev), what.isEmpty() ? QString("nothing") : what));
}

// A press and what it does. A pad SDL knows the layout of reports each push
// twice, by name and by number: the name is the one the table binds.
void xGamepadWidget::inputChanged(int type, int num, int state) {
	if (!isVisible() || gpad->isKeyboard() || !gpad->bindable(type)) return;
	if ((state != 0) && gpad->whatDoes(type, num, state).isEmpty()) {
		xJoyMapEntry ev;
		ev.type = type;
		ev.num = num;
		ev.state = state;
		labTry->setText(QString("%0: nothing").arg(xGamepad::getEntryName(ev)));
		return;
	}
	QString held = gpad->heldText();
	if (!held.isEmpty()) labTry->setText(held);
}

void xGamepadWidget::keyTry(int id) {
	if (!gpad->isKeyboard()) return;
	xJoyMapEntry ev;
	ev.type = JOY_KEY;
	ev.num = id;
	ev.state = 1;
	tryShow(ev);
}

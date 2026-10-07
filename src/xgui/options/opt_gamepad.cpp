#include <algorithm>
#include <cctype>
#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QStyleOptionButton>
#include <QTimer>
#include <QVBoxLayout>

#include "opt_gamepad.h"

#include "../xgui.h"
#include "../../xcore/xcore.h"

// a button as wide as the longest of its texts, so changing it moves nothing
class xTextsButton : public QPushButton {
	public:
		xTextsButton(const QStringList& t):QPushButton(t.value(0)) {texts = t;}
		QSize sizeHint() const {
			QSize sz = QPushButton::sizeHint();
			QStyleOptionButton opt;
			initStyleOption(&opt);
			foreach(const QString& t, texts) {
				opt.text = t;
				QSize cont(fontMetrics().horizontalAdvance(t), fontMetrics().height());
				sz.setWidth(qMax(sz.width(), style()->sizeFromContents(QStyle::CT_PushButton, &opt, cont, this).width()));
			}
			return sz;
		}
	private:
		QStringList texts;
};

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
	rbVjoy = new QRadioButton("Joystick");
	rbVjoy->setToolTip("The joystick picked for the player, whichever it is");
	cbVjoy = new QComboBox;
	cbVjoy->addItem("Up", XJ_UP);
	cbVjoy->addItem("Down", XJ_DOWN);
	cbVjoy->addItem("Left", XJ_LEFT);
	cbVjoy->addItem("Right", XJ_RIGHT);
	cbVjoy->addItem("Fire", XJ_FIRE);
	rbKey = new QRadioButton("Key");
	cbKey = new QComboBox;
	// digits and letters in order, then the keys with names, then what Caps
	// Shift makes of a key: the modifier byte above the key's
	const char* zx = pad_zx_keys();
	QList<int> chars;
	for (int i = 0; zx[i]; i++) {
		if (isdigit(zx[i]) || islower(zx[i])) chars.append(zx[i]);	// the named ones are capitals
	}
	std::sort(chars.begin(), chars.end());
	foreach(int c, chars) cbKey->addItem(pad_zx_name(c), c);
	cbKey->insertSeparator(cbKey->count());
	for (const char* c = " ECS"; *c; c++) cbKey->addItem(pad_zx_name(*c), int(*c));
	cbKey->insertSeparator(cbKey->count());
	for (const char* c = pad_zx_ext_keys(); *c; c++)
		cbKey->addItem(pad_zx_ext_name(*c), ('C' << 8) | *c);
	cbKey->setMaxVisibleItems(20);
	int kw = 0;
	for (int i = 0; i < cbKey->count(); i++) kw = qMax(kw, cbKey->fontMetrics().horizontalAdvance(cbKey->itemText(i)));
	cbKey->view()->setMinimumWidth(kw + cbKey->style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 24);
	btnPress = new xTextsButton(QStringList() << "Press" << "Press a key");
	btnPress->setCheckable(true);
	btnPress->setToolTip("Press a PC key: the Spectrum key it is on");
	btnPress->installEventFilter(this);
	labMod = new QLabel("Modifier");
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
	rbMouse = new QRadioButton("Mouse");
	rbMouse->setToolTip("The Kempston mouse: faster the longer it moves");
	cbMouse = new QComboBox;
	cbMouse->addItem("Up", XM_UP);
	cbMouse->addItem("Down", XM_DOWN);
	cbMouse->addItem("Left", XM_LEFT);
	cbMouse->addItem("Right", XM_RIGHT);
	cbMouse->addItem("Left button", XM_LMB);
	cbMouse->addItem("Right button", XM_RMB);
	cbMouse->addItem("Middle button", XM_MMB);
	cbMouse->addItem("Wheel up", XM_WHEELUP);
	cbMouse->addItem("Wheel down", XM_WHEELDN);
	rbCut = new QRadioButton("Action");
	cbCut = new QComboBox;
	xShortcut* tab = shortcut_tab();
	for (int i = 0; tab[i].text; i++) {
		if (tab[i].grp & SCG_MAIN) cbCut->addItem(tab[i].text, tab[i].id);
	}
	cbCut->model()->sort(0);
	rbKept = new QRadioButton("As it was");
	labKept = new QLabel;
	// the player's joystick first: it follows the one picked, the rest are fixed
	lg->addWidget(rbVjoy, 1, 0);
	lg->addWidget(cbVjoy, 1, 1);
	lg->addWidget(rbKey, 2, 0);
	lg->addWidget(cbKey, 2, 1);
	lg->addWidget(btnPress, 2, 2);
	lg->addWidget(labMod, 3, 0);
	lg->addWidget(cbMod, 3, 1);
	lg->addWidget(rbJoy, 4, 0);
	lg->addWidget(cbJoy, 4, 1);
	lg->addWidget(rbMouse, 5, 0);
	lg->addWidget(cbMouse, 5, 1);
	lg->addWidget(rbCut, 6, 0);
	lg->addWidget(cbCut, 6, 1, 1, 2);
	lg->addWidget(rbKept, 7, 0);
	lg->addWidget(labKept, 7, 1, 1, 2);
	lg->setRowStretch(8, 1);
	tgtBox = left;
	foreach(QRadioButton* rb, QList<QRadioButton*>() << rbVjoy << rbKey << rbJoy << rbMouse << rbCut << rbKept)
		connect(rb, &QRadioButton::toggled, this, [this]() {syncTarget();});
	connect(cbKey, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {syncTarget();});
	connect(btnPress, &QPushButton::toggled, this, [this](bool on) {
		btnPress->setText(on ? "Press a key" : "Press");
		if (on) btnPress->setFocus();		// the key goes to it
	});

	// right: what presses it
	QGroupBox* right = new QGroupBox;
	right->setObjectName("inputs");
	QGridLayout* rg = new QGridLayout(right);
	inBox = new xInputBox;
	inBox->setMinimumWidth(220);
	rg->addWidget(inBox, 0, 0, 1, 2);
	btnPick = new xSideButton;
	btnPick->setText("Add from list");
	btnPick->setIcon(QIcon(":/images/add.png"));
	btnPick->setToolTip("An input picked by name: no pad needed");
	pickMenu = new QMenu(btnPick);
	// a menu of its own, not the button's: style sheets draw that arrow over the text
	connect(btnPick, &QPushButton::clicked, this, [this]() {
		pickMenu->exec(btnPick->mapToGlobal(QPoint(0, btnPick->height())));
	});
	QPushButton* btnClear = new QPushButton("Clear");
	btnDefault = new QPushButton("Defaults");
	QHBoxLayout* rb = new QHBoxLayout;
	rb->addWidget(btnPick);
	rb->addStretch(1);
	rb->addWidget(btnClear);
	rb->addWidget(btnDefault);
	rg->addLayout(rb, 1, 0, 1, 2);
	labHint = padNote(QString());
	labHint->setWordWrap(true);
	rg->addWidget(labHint, 2, 0, 1, 2);
	rg->setRowStretch(3, 1);

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

// only the option picked on the left is live, the rest greyed
void xPadRowEdit::syncTarget() {
	foreach(QWidget* w, QList<QWidget*>() << cbKey << btnPress << labMod << cbMod) w->setEnabled(rbKey->isChecked());
	if (cbKey->currentData().toInt() > 0xff) {	// already Caps Shift and a key
		cbMod->setCurrentIndex(0);
		labMod->setEnabled(false);
		cbMod->setEnabled(false);
	}
	cbVjoy->setEnabled(rbVjoy->isChecked());
	cbJoy->setEnabled(rbJoy->isChecked());
	cbMouse->setEnabled(rbMouse->isChecked());
	cbCut->setEnabled(rbCut->isChecked());
	labKept->setEnabled(rbKept->isChecked());
	if (!rbKey->isChecked()) btnPress->setChecked(false);
}

// what the left side says, into cur; false if it says nothing
bool xPadRowEdit::takeTarget() {
	xJoyMapEntry t;
	if (rbKept->isChecked()) {
		cur.tgt = kept;
		return !kept.isEmpty();
	}
	int mod = 0;
	if (rbVjoy->isChecked()) {
		t.dev = JMAP_VJOY;
		t.dir = cbVjoy->currentData().toInt();
	} else if (rbJoy->isChecked()) {
		t.dev = JMAP_JOY;
		t.dir = cbJoy->currentData().toInt();
	} else if (rbMouse->isChecked()) {
		t.dev = JMAP_MOUSE;
		t.dir = cbMouse->currentData().toInt();
	} else if (rbCut->isChecked()) {
		t.dev = JMAP_CUT;
		t.dir = cbCut->currentData().toInt();
	} else {
		int d = cbKey->currentData().toInt();
		t.dev = JMAP_ZX;
		t.dir = d & 0xff;
		mod = (d > 0xff) ? (d >> 8) : cbMod->currentData().toInt();
	}
	cur.tgt.clear();
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
	foreach(QWidget* w, QList<QWidget*>() << rbVjoy << cbVjoy << rbJoy << cbJoy << rbMouse << cbMouse << rbCut << cbCut)
		w->setVisible(own && !custom);
	QList<xJoyMapEntry> tgt = (i < 0) ? QList<xJoyMapEntry>() : gp->rowTargets(i);
	kept.clear();
	cbKey->setCurrentIndex(0);
	cbMod->setCurrentIndex(0);
	// a new row most likely wants another fire, turbo or not
	cbVjoy->setCurrentIndex(cbVjoy->findData(XJ_FIRE));
	if (custom) rbKey->setChecked(true); else rbVjoy->setChecked(true);
	if (!tgt.isEmpty()) {
		const xJoyMapEntry& t = tgt.first();
		switch (t.dev) {
			case JMAP_ZX:
				rbKey->setChecked(true);
				// Caps or Symbol Shift and a key after it: by name if it has one, or the modifier and the key
				if ((tgt.size() > 1) && (tgt.at(1).dev == JMAP_ZX) && ((t.dir == 'C') || (t.dir == 'S'))) {
					int ext = cbKey->findData((t.dir << 8) | tgt.at(1).dir);
					if (ext >= 0) {
						cbKey->setCurrentIndex(ext);
						break;
					}
					cbMod->setCurrentIndex(qMax(0, cbMod->findData(t.dir)));
					cbKey->setCurrentIndex(qMax(0, cbKey->findData(tgt.at(1).dir)));
				} else {
					cbKey->setCurrentIndex(qMax(0, cbKey->findData(t.dir)));
				}
				break;
			case JMAP_VJOY:
				rbVjoy->setChecked(true);
				cbVjoy->setCurrentIndex(qMax(0, cbVjoy->findData(t.dir)));
				break;
			case JMAP_JOY:
				rbJoy->setChecked(true);
				cbJoy->setCurrentIndex(qMax(0, cbJoy->findData(t.dir)));
				break;
			case JMAP_MOUSE:
				rbMouse->setChecked(true);
				cbMouse->setCurrentIndex(qMax(0, cbMouse->findData(t.dir)));
				break;
			case JMAP_CUT:
				rbCut->setChecked(true);
				cbCut->setCurrentIndex(qMax(0, cbCut->findData(t.dir)));
				break;
			default:		// a PC key, from an old .pad
				kept = tgt;
				rbKept->setChecked(true);
				break;
		}
	}
	rbKept->setVisible(own && !kept.isEmpty());
	labKept->setVisible(own && !kept.isEmpty());
	labKept->setText(xGamepad::targetsName(kept));
	syncTarget();
	// the right side: the device in use
	bool keys = gp->isKeyboard();
	findChild<QGroupBox*>("inputs")->setTitle(keys ? "PC Keyboard" : "Gamepad");
	btnPick->setVisible(!keys);
	pickMenu->clear();
	choices = xGamepad::inputChoices(gp->asController());
	for (int n = 0; n < choices.size(); n++) {
		if ((n > 0) && (choices.at(n).type != JOY_VDIR) && (choices.at(n - 1).type == JOY_VDIR))
			pickMenu->addSeparator();		// the aliases, then the inputs one by one
		xJoyMapEntry e = choices.at(n);
		pickMenu->addAction(xGamepad::getEntryName(e), this, [this, e]() {
			addInput(e);
			inBox->setFocus();
		});
	}
	btnDefault->setVisible(joy);
	labHint->setText(keys ? "Press keys here to bind them" : "Press buttons on the pad to bind them");
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
	ensurePolished();		// sized with the style sheet's fonts and margins, before it is placed
	btnPick->setMinimumWidth(btnPick->sizeHint().width());	// it elides rather than push the box wider
	// and with the rows this one hides or shows: in a hidden window nothing tells the layouts
	foreach(QWidget* w, findChildren<QWidget*>()) w->updateGeometry();
	layout()->activate();
	adjustSize();
	// over the middle each time, a row of another kind being another size; once
	// shown, as the last of the sizes only comes through then
	QTimer::singleShot(0, this, [this]() {center_over(this, parentWidget()->window());});
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
	rgrid->setHorizontalSpacing(24);		// the columns read as two, not as one long line
	grpScheme = new QButtonGroup(this);
	// a radio button, and what goes right after it in its cell
	auto radio = [this, rgrid](int id, const char* text, int r, int c, QWidget* tail = nullptr) {
		QRadioButton* rb = new QRadioButton(text);
		grpScheme->addButton(rb, id);
		rgrid->addWidget(fieldPair(rb, tail, false), r, c);
		return rb;
	};
	radio(GPS_KEMPSTON, "Kempston", 0, 0);
	radio(GPS_SINCLAIR1, pad_scheme_name(GPS_SINCLAIR1), 1, 0)->setToolTip("Also Sinclair 1, Interface 2 port 1\nKeys 6-0: left, right, down, up, fire");
	radio(GPS_SINCLAIR2, pad_scheme_name(GPS_SINCLAIR2), 2, 0)->setToolTip("Also Sinclair 2, Interface 2 port 2\nKeys 1-5: left, right, down, up, fire");
	radio(GPS_CURSOR, pad_scheme_name(GPS_CURSOR), 0, 1);
	cbQaop = new QComboBox;
	cbQaop->addItem("Space", GPS_QAOP);
	cbQaop->addItem("M", GPS_QAOPM);
	cbQaop->setToolTip("The key fire presses");
	radio(GPS_QAOP, "QAOP", 1, 1, cbQaop);
	radio(GPS_CUSTOM, "Custom", 2, 1)->setToolTip("Spectrum keys of your own");
	rgrid->setColumnStretch(1, 1);
	grpScheme->button(GPS_KEMPSTON)->setToolTip("Switched on by itself when picked.\nFire 2-4 come with an 8-button one");
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
	abox->addWidget(padNote("Double-click a row to change it, right-click for more"));
	abox->addStretch(1);
	abox->addWidget(btnAdd);
	sldTurbo = new QSlider(Qt::Horizontal);
	sldTurbo->setRange(1, 25);
	labTurbo = new QLabel;
	labTurbo->setMinimumWidth(labTurbo->fontMetrics().horizontalAdvance("25 Hz"));
	sldDead = new QSlider(Qt::Horizontal);
	sldDead->setRange(0, 100);
	sldDead->setToolTip("How far a stick has to move before it counts");
	labDead = new QLabel;
	labDead->setMinimumWidth(labDead->fontMetrics().horizontalAdvance("100%"));
	labDeadName = new QLabel("Stick dead zone");
	QGridLayout* rbox = new QGridLayout;
	rbox->addWidget(new QLabel("Turbo rate"), 0, 0);
	rbox->addWidget(fieldPair(sldTurbo, labTurbo, true), 0, 1);
	rbox->addWidget(labDeadName, 1, 0);
	rbox->addWidget(fieldPair(sldDead, labDead, true), 1, 1);
	rbox->setColumnStretch(1, 1);
	QVBoxLayout* tcol = new QVBoxLayout;
	tcol->addWidget(table, 1);
	tcol->addLayout(abox);
	tcol->addLayout(rbox);
	QHBoxLayout* tbox = new QHBoxLayout;
	tbox->addLayout(tcol, 1);
	tbox->addLayout(bbox);
	grid->addLayout(tbox, row++, 0, 1, 2);
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
	connect(sldDead, &QSlider::valueChanged, this, [this](int v) {
		gpad->setDeadZone(v * 32768 / 100);
		labDead->setText(QString("%0%").arg(v));
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
// Every pad either player has had this session stays in the list, plugged in
// or not: picking the keyboard for a moment must not lose the pad.
static QList<xPadId> padsKnown;

static void padKnow(const xPadId& id) {
	if (id.isEmpty()) return;
	foreach(const xPadId& k, padsKnown) {
		if (k.sameAs(id)) return;
	}
	padsKnown.append(id);
}

void xGamepadWidget::updateList() {
	QList<xPadDev> devs = xGamepad::devList();
	xPadId want = gpad->padId();
	padKnow(conf.gpctrl->gpada->padId());
	padKnow(conf.gpctrl->gpadb->padId());
	int sel = 0;
	cbDevice->blockSignals(true);			// not a choice of the user's
	cbDevice->clear();
	cbDevice->addItem("None");
	for (int k = GPK_ARROWS; k <= GPK_WASD; k++)
		cbDevice->addItem(pad_kbd_name(k), pad_kbd_key(k));
	if (gpad->keyboard() == GPK_ARROWS) sel = 1;
	if (gpad->keyboard() == GPK_WASD) sel = 2;
	for (int i = 0; i < devs.size(); i++) {
		padKnow(devs.at(i).id);
		cbDevice->addItem(devs.at(i).label, devs.at(i).id.toConfig());
		if (!gpad->isKeyboard() && devs.at(i).id.sameAs(want))
			sel = cbDevice->count() - 1;
	}
	foreach(const xPadId& k, padsKnown) {
		bool here = false;
		foreach(const xPadDev& d, devs) {
			if (d.id.sameAs(k)) here = true;
		}
		if (here) continue;
		cbDevice->addItem(QString("%0 (not connected)").arg(k.title()), k.toConfig());
		if (!gpad->isKeyboard() && k.sameAs(want)) sel = cbDevice->count() - 1;
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
	int dz = (gpad->deadZone() * 100 + 16384) / 32768;	// the nearest percent
	sldDead->blockSignals(true);
	sldDead->setValue(dz);
	sldDead->blockSignals(false);
	labDead->setText(QString("%0%").arg(dz));
	// a keyboard has no stick; greyed, not hidden, so the window keeps its size
	labDeadName->setEnabled(!gpad->isKeyboard());
	sldDead->parentWidget()->setEnabled(!gpad->isKeyboard());
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

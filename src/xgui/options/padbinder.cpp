#include <QDebug>

#include <SDL.h>

#include <QLineEdit>

#include "padbinder.h"

// TODO: use xKeyEditor for binding to pc key ?

enum {
	PBMODE_FREE = 0,
	PBMODE_KEY,
	PBMODE_PAD
};

xPadBinder::xPadBinder(QWidget* p):QDialog(p) {
	ui.setupUi(this);
	mode = PBMODE_FREE;

	ui.cbJoyList->addItem("Up", XJ_UP);
	ui.cbJoyList->addItem("Down", XJ_DOWN);
	ui.cbJoyList->addItem("Left", XJ_LEFT);
	ui.cbJoyList->addItem("Right", XJ_RIGHT);
	ui.cbJoyList->addItem("Fire", XJ_FIRE);
	ui.cbJoyList->addItem("Button 2", XJ_BUT2);
	ui.cbJoyList->addItem("Button 3", XJ_BUT3);
	ui.cbJoyList->addItem("Button 4", XJ_BUT4);

	ui.cbMouseList->addItem("Up", XM_UP);
	ui.cbMouseList->addItem("Down", XM_DOWN);
	ui.cbMouseList->addItem("Left", XM_LEFT);
	ui.cbMouseList->addItem("Right", XM_RIGHT);
	ui.cbMouseList->addItem("Left button", XM_LMB);
	ui.cbMouseList->addItem("Mid button", XM_MMB);
	ui.cbMouseList->addItem("Right button", XM_RMB);
	ui.cbMouseList->addItem("Wheel up", XM_WHEELUP);
	ui.cbMouseList->addItem("Wheel down", XM_WHEELDN);

	// connect(&timer, SIGNAL(timeout()), this, SLOT(onTimer()));
	// connect(ui.pbPadBind, SIGNAL(clicked(bool)), this, SLOT(startBindPad()));
	connect(ui.pbKeyBind, SIGNAL(clicked(bool)), this, SLOT(startBindKey()));
	connect(ui.cbJoyList, SIGNAL(currentIndexChanged(int)), this, SLOT(setJoyDir()));
	connect(ui.cbMouseList, SIGNAL(currentIndexChanged(int)), this, SLOT(setMouseDir()));
	connect(ui.pbOk, SIGNAL(clicked(bool)), this, SLOT(okPress()));
	connect(ui.pbRepSlider, SIGNAL(valueChanged(int)), this, SLOT(onRepSlider(int)));

#if USE_SEQ_BIND
	ui.pbKeyBind->setVisible(false);
	connect(ui.seqField, &QKeySequenceEdit::editingFinished, this, &xPadBinder::seqFinished);
#else
	ui.seqField->setEnabled(false);
	ui.seqField->setVisible(false);
#endif
	ui.seqField->installEventFilter(this);
	foreach(QLineEdit* led, ui.seqField->findChildren<QLineEdit*>())
		led->installEventFilter(this);	// the one that takes the click
	ui.cbJoyList->installEventFilter(this);
	ui.cbMouseList->installEventFilter(this);

	// room for the longest value, so the slider does not move as it changes
	ui.pbRepLabel->setMinimumWidth(ui.pbRepLabel->fontMetrics().horizontalAdvance("0.00 sec"));
	layout()->setSizeConstraint(QLayout::SetFixedSize);
}

// TODO: select radiobutton (e->dev)
void xPadBinder::start(xGamepad* gp, xJoyMapEntry e) {
	gpad = gp;
	ent = e;
	setPadButtonText();
#if USE_SEQ_BIND
	ui.seqField->setKeySequence(ent.seq);
#else
	setKeyButtonText();
#endif
	switch(e.dev) {
		case JMAP_KEY:
			ui.rbKey->setChecked(true);
			break;
		case JMAP_JOY:
			ui.rbJoy->setChecked(true);
			ui.cbJoyList->setCurrentIndex(ui.cbJoyList->findData(e.dir));
			break;
		case JMAP_MOUSE:
			ui.rbMouse->setChecked(true);
			ui.cbMouseList->setCurrentIndex(ui.cbMouseList->findData(e.dir));
			break;
	}

	ui.pbRepSlider->setValue(ent.rpt);
	connect(gpad, SIGNAL(inputChanged(int,int,int)), this, SLOT(gpInputChanged(int,int,int)), Qt::UniqueConnection);
	show();
}

void xPadBinder::onRepSlider(int val) {
	QString num = QString::number(val / 50.0);
	ui.pbRepLabel->setText(QString("%0 sec").arg(num));
}

void xPadBinder::setKeyButtonText() {
	switch(ent.dev) {
		case JMAP_KEY:
			ui.rbKey->setChecked(true);
#if !USE_SEQ_BIND
			ui.pbKeyBind->setText(QString("Key %0").arg(getKeyNameById(ent.key)));
#endif
			break;
		case JMAP_JOY:
			ui.pbKeyBind->setText("Push to bind");
			ui.rbJoy->setChecked(true);
			ui.cbJoyList->setCurrentIndex(ui.cbJoyList->findData(ent.dir));
			break;
		case JMAP_MOUSE:
			ui.pbKeyBind->setText("Push to bind");
			ui.rbMouse->setChecked(true);
			ui.cbMouseList->setCurrentIndex(ui.cbMouseList->findData(ent.dir));
			break;
		default:
			ui.pbKeyBind->setText("Push to bind");
			ent.dev = JMAP_NONE;
			break;
	}
	ui.pbOk->setEnabled((ent.type != JOY_NONE) && (ent.dev != JMAP_NONE));
}

void xPadBinder::setPadButtonText() {
	QString nm = xGamepad::getEntryName(ent);
	if (nm.isEmpty()) {
		ent.type = JOY_NONE;
		nm = "Push to bind";
	}
	ui.pbPadBind->setText(nm);
	ui.pbOk->setEnabled((ent.type != JOY_NONE) && (ent.dev != JMAP_NONE));
}

// set kempston bind

void xPadBinder::setJoyDir() {
	ui.rbJoy->setChecked(true);
	ent.dev = JMAP_JOY;
#if !USE_SEQ_BIND
	ent.key = ENDKEY;
#endif
	ent.dir = ui.cbJoyList->itemData(ui.cbJoyList->currentIndex()).toInt();
}

void xPadBinder::setMouseDir() {
	ui.rbMouse->setChecked(true);
	ent.dev = JMAP_MOUSE;
#if !USE_SEQ_BIND
	ent.key = ENDKEY;
#endif
	ent.dir = ui.cbMouseList->itemData(ui.cbMouseList->currentIndex()).toInt();
}

// The choice follows the field the user goes to. Only a click or Tab: the
// focus a window hands out on opening must not change what the entry is.
bool xPadBinder::eventFilter(QObject* obj, QEvent* ev) {
	if (ev->type() == QEvent::FocusIn) {
		Qt::FocusReason why = static_cast<QFocusEvent*>(ev)->reason();
		if ((why == Qt::MouseFocusReason) || (why == Qt::TabFocusReason) || (why == Qt::BacktabFocusReason)) {
			if ((obj == ui.seqField) || (obj->parent() == ui.seqField)) {
				ui.rbKey->setChecked(true);
			} else if (obj == ui.cbJoyList) {
				setJoyDir();
			} else if (obj == ui.cbMouseList) {
				setMouseDir();
			}
		}
	}
	return QDialog::eventFilter(obj, ev);
}

// set key bind

void xPadBinder::startBindKey() {
#if !USE_SEQ_BIND
	mode = PBMODE_KEY;
	ui.pbKeyBind->setText("...scan...");
	ui.rbKey->setChecked(true);
	grabKeyboard();
#endif
}

void xPadBinder::keyPressEvent(QKeyEvent* ev) {
#if !USE_SEQ_BIND
	ev->ignore();
	if (ev->key() == Qt::Key_Escape) {
		releaseKeyboard();
		if (mode == PBMODE_FREE) {
			close();
		} else {
			mode = PBMODE_FREE;
			setPadButtonText();
			setKeyButtonText();
		}
	} else if (mode == PBMODE_KEY) {
		mode = PBMODE_FREE;
		int id = qKey2id(ev->key());
		if (id == ENDKEY) return;
		ent.dev = JMAP_KEY;
		ent.key = id;
		ent.dir = XJ_NONE;
		setKeyButtonText();
		releaseKeyboard();
	}
#endif
}

static struct {
	int src;
	int dst;
} seqTranTab[] = {
	{Qt::Key_Exclam, Qt::SHIFT + Qt::Key_1},
	{Qt::Key_At, Qt::SHIFT + Qt::Key_2},
	{Qt::Key_NumberSign, Qt::SHIFT + Qt::Key_3},
	{Qt::Key_Dollar, Qt::SHIFT + Qt::Key_4},
	{Qt::Key_Percent, Qt::SHIFT + Qt::Key_5},
	{Qt::Key_AsciiCircum, Qt::SHIFT + Qt::Key_6},
	{Qt::Key_Ampersand, Qt::SHIFT + Qt::Key_7},
	{Qt::Key_Asterisk, Qt::SHIFT + Qt::Key_8},
	{Qt::Key_ParenLeft, Qt::SHIFT + Qt::Key_9},
	{Qt::Key_ParenRight, Qt::SHIFT + Qt::Key_0},
	{0, 0}
};

void xPadBinder::seqFinished() {
#if USE_SEQ_BIND
	ent.dev = JMAP_KEY;
	ent.seq = ui.seqField->keySequence();
	ent.dir = XJ_NONE;
	int i,j;
	uint dseq[4] = {0,0,0,0};
	for (i = 0; i < ent.seq.count(); i++) {
		j = 0;
		dseq[i] = ent.seq[i];
		while (seqTranTab[j].src != 0) {
			if (ent.seq[i] == seqTranTab[j].src) {
				dseq[i] = seqTranTab[j].dst;
			}
			j++;
		}
	}
	ent.seq = QKeySequence(dseq[0],dseq[1],dseq[2],dseq[3]);
	ui.seqField->setKeySequence(ent.seq);
#endif
}

// OK pressed

void xPadBinder::okPress() {
	if (ent.type == JOY_NONE) return;
	ent.rpt = ui.pbRepSlider->value();
	if (ui.rbKey->isChecked()) {
		ent.dev = JMAP_KEY;
		ent.dir = XJ_NONE;
#if USE_SEQ_BIND
		if (ent.seq.isEmpty()) return;
#else
		if (ent.key == ENDKEY) return;
#endif
	} else if (ui.rbJoy->isChecked()) {
		ent.dev = JMAP_JOY;
		ent.dir = ui.cbJoyList->itemData(ui.cbJoyList->currentIndex()).toInt();
#if !USE_SEQ_BIND
		ent.key = ENDKEY;
#endif
		if (ent.dir == XJ_NONE) return;
	}
//	disconnect(gpad, this);
	close();
	emit bindReady(ent);
}

void xPadBinder::reject() {
//	disconnect(gpad, this);
	hide();
}

// scan gamepad

// Bind to what the pad reports. On a pad SDL knows the layout of that is the
// named button, not the raw number, so the map keeps working on the next
// machine and the next platform.
void xPadBinder::gpInputChanged(int type, int num, int state) {
	if (!isActiveWindow()) return;
	if ((xGamepad*)sender() != gpad) return;
	if (state == 0) return;			// a release is not a binding
	if (type == JOY_HAT) return;		// a hat also arrives as four buttons, bind those
	if (gpad->isController() && ((type == JOY_BUTTON) || (type == JOY_AXIS)))
		return;				// the same push in raw form: ignore it
	ent.type = type;
	ent.num = num;
	ent.state = (state < 0) ? -1 : 1;
	mode = PBMODE_FREE;
	setPadButtonText();
}


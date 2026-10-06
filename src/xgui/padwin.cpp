#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include "padwin.h"
#include "../xcore/xcore.h"

xPadWin::xPadWin(QWidget* p):QDialog(p) {
	setWindowTitle("Gamepads");
	setProperty("xCenterOnce", true);
	setWindowIcon(QIcon(":/images/gamepad.png"));
	QTabWidget* tabs = new QTabWidget;
	xGamepad* pad[2] = {conf.gpctrl->gpada, conf.gpctrl->gpadb};
	for (int i = 0; i < 2; i++) {
		pan[i] = new xGamepadWidget(pad[i]);
		pan[i]->changed = [this]() {sameWidths();};
		tabs->addTab(pan[i], QIcon(":/images/gamepad.png"), QString("Player %0").arg(i + 1));
	}
	// a pad plugged in or pulled out while the window is up
	connect(conf.gpctrl, &xGamepadController::devicesChanged, this, [this]() {
		if (!isVisible()) return;
		pan[0]->updateList();
		pan[1]->updateList();
	});
	QPushButton* btnClose = new QPushButton(QIcon(":/images/cancel.png"), "Close");
	btnClose->setAutoDefault(false);
	connect(btnClose, &QPushButton::clicked, this, &QDialog::close);
	QHBoxLayout* bottom = new QHBoxLayout;
	bottom->addStretch(1);
	bottom->addWidget(btnClose);
	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->addWidget(tabs, 1);
	lay->addLayout(bottom);
	lay->setSizeConstraint(QLayout::SetFixedSize);
}

// One name column for both players, or it jumps when the tab is switched.
void xPadWin::sameWidths() {
	int w = qMax(pan[0]->nameWidth(), pan[1]->nameWidth());
	pan[0]->setNameWidth(w);
	pan[1]->setNameWidth(w);
}

void xPadWin::refreshAll() {
	pan[0]->refresh();
	pan[1]->refresh();
	sameWidths();
}

void xPadWin::sync() {
	if (isVisible()) refreshAll();
}

void xPadWin::showWindow() {
	refreshAll();
	show();
	raise();
	activateWindow();
}

// over a modal window - Options - the window has to be modal too, or it is
// shown and cannot be used
void xPadWin::execOver() {
	if (isVisible()) hide();
	refreshAll();
	exec();
}

// A key pressed anywhere in the window shows on the try line of a player on
// the keyboard. It is only looked at: the focused control still gets it. The
// focused one is the first to see it, as it goes up through the parents.
bool xPadWin::eventFilter(QObject* obj, QEvent* ev) {
	if ((ev->type() == QEvent::KeyPress) && (obj == QApplication::focusWidget())
			&& (((QWidget*)obj)->window() == this)) {
		QKeyEvent* kev = (QKeyEvent*)ev;
		if (!kev->isAutoRepeat()) {
			int id = pad_key_id(kev);
			pan[0]->keyTry(id);
			pan[1]->keyTry(id);
		}
	}
	return QDialog::eventFilter(obj, ev);
}

void xPadWin::showEvent(QShowEvent* ev) {
	qApp->installEventFilter(this);
	QDialog::showEvent(ev);
}

void xPadWin::hideEvent(QHideEvent* ev) {
	qApp->removeEventFilter(this);
	saveConfig();
	QDialog::hideEvent(ev);
}

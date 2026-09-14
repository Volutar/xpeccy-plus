// the screen panel, detached

#include <QApplication>
#include <QCloseEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QVBoxLayout>

#include "scrwin.h"
#include "xgui.h"

xScrWin::xScrWin(QWidget* p):QDialog(p) {
	setWindowTitle("Screen");
	setWindowIcon(QIcon(":/images/rulers.png"));
	setSizeGripEnabled(true);	// the picture is the point: let it be resized
	lastfrm = 0;
	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	panel = new xZXScrPanel;
	lay->addWidget(panel);
	updateStyle();
	connect(panel, &xZXScrPanel::s_detach, this, &xScrWin::s_detach);
}

// the debugger hands its own font out child by child; this window is not one
// of its children, so it takes the font here
void xScrWin::updateStyle() {
	foreach (QWidget* wid, findChildren<QWidget*>())
		wid->setFont(conf.dbg.font);
	setFont(conf.dbg.font);
	panel->reload();
}

void xScrWin::setDetached(bool on) {
	if (on) {
		// Put it back where it was left, before show(): the move and resize
		// events that follow would otherwise record the place it is being taken
		// away from. Geometry, not move/resize: geometry() and setGeometry()
		// are the same rectangle - the one inside the frame - so a window saved
		// and restored lands where it was, where move() would walk it down the
		// screen by the height of its own title bar every time.
		if ((conf.dbg.scrpos.x() >= 0) && (conf.dbg.scrpos.y() >= 0)) {
			setGeometry(QRect(conf.dbg.scrpos, conf.dbg.scrsiz));
		} else {
			resize(conf.dbg.scrsiz);
		}
		show();
		raise();
		activateWindow();
		panel->reload();
	} else {
		hide();
	}
}

// One picture per emulated frame: the frame signal comes from the emulation
// itself, so the window keeps the machine's own time instead of a timer's.
// Fast mode makes ten times as many frames and the screen has no more to say
// for each, so there it takes every tenth.
void xScrWin::upd() {
	if (!isVisible()) return;
	if (conf.emu.fast && ((conf.vid.fcount - lastfrm) < 10)) return;
	lastfrm = conf.vid.fcount;
	panel->draw();
}

// where the window is left is where it comes back, the way the debugger
// remembers its own place
void xScrWin::moveEvent(QMoveEvent* ev) {
	QDialog::moveEvent(ev);
	if (!isVisible()) return;
	conf.dbg.scrpos = geometry().topLeft();
}

void xScrWin::resizeEvent(QResizeEvent* ev) {
	QDialog::resizeEvent(ev);
	if (!isVisible()) return;
	conf.dbg.scrsiz = geometry().size();
}

// closing it is another way of saying "put it back in the debugger"
void xScrWin::closeEvent(QCloseEvent* ev) {
	emit s_detach(false);
	ev->accept();
}

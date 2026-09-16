// the sound chip panel, detached

#include <QApplication>
#include <QCloseEvent>
#include <QMenu>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QVBoxLayout>

#include "sndwin.h"
#include "xgui.h"

xSndWin::xSndWin(QWidget* p):QDialog(p) {
	setWindowTitle("Sound chips");
	setWindowIcon(QIcon(":/images/note.png"));
	setSizeGripEnabled(true);
	lastfrm = 0;
	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->setContentsMargins(5, 5, 5, 5);
	panel = new xSndPanel;
	lay->addWidget(panel);
	updateStyle();
	connect(panel, &xSndPanel::s_detach, this, &xSndWin::s_detach);
}

// the debugger hands its own font out child by child; this window is not one
// of its children, so it takes the font here
void xSndWin::updateStyle() {
	QFont uiFont = QApplication::font();
	foreach (QWidget* wid, findChildren<QWidget*>())
		wid->setFont(qobject_cast<QMenu*>(wid) ? uiFont : conf.dbg.font);
	setFont(conf.dbg.font);
	panel->reload();
}

void xSndWin::setDetached(bool on) {
	if (on) {
		// geometry(), not move/resize: the same rectangle both ways, where
		// pos()/move() would walk the window down the screen by the height
		// of its own title bar on every restore
		if ((conf.dbg.sndpos.x() >= 0) && (conf.dbg.sndpos.y() >= 0)) {
			setGeometry(QRect(conf.dbg.sndpos, conf.dbg.sndsiz));
		} else {
			resize(conf.dbg.sndsiz);
		}
		show();
		raise();
		activateWindow();
		panel->reload();
	} else {
		hide();
	}
}

// One refresh per emulated frame, so the readout keeps the machine's own time
// instead of a timer's. Fast mode makes ten times as many frames and no more to
// read out of each, so there it takes every tenth.
void xSndWin::upd() {
	if (!isVisible() || isMinimized()) return;
	if (conf.emu.fast && ((conf.vid.fcount - lastfrm) < XSCR_FAST_EVERY)) return;
	lastfrm = conf.vid.fcount;
	panel->draw();
}

// where the window is left is where it comes back
void xSndWin::moveEvent(QMoveEvent* ev) {
	QDialog::moveEvent(ev);
	if (!isVisible()) return;
	conf.dbg.sndpos = geometry().topLeft();
}

void xSndWin::resizeEvent(QResizeEvent* ev) {
	QDialog::resizeEvent(ev);
	if (!isVisible()) return;
	conf.dbg.sndsiz = geometry().size();
}

// Escape on a dialog is reject(), which hides the window on its own without a
// close event: the debugger would go on thinking the panel is detached and the
// next toggle would do nothing at all. Send it the same road as the X button.
void xSndWin::reject() {
	close();
}

// closing it is another way of saying "put it back in the debugger"
void xSndWin::closeEvent(QCloseEvent* ev) {
	emit s_detach(false);
	ev->accept();
}

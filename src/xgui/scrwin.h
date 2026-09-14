#pragma once

#include <QDialog>

#include "debuga/dbg_zxscr.h"

// The screen panel in a window of its own. It belongs to the main window, not
// to the debugger: a window whose parent is hidden is hidden with it, and this
// one has to outlive the debugger and keep up with the machine while it runs.
class xScrWin : public QDialog {
	Q_OBJECT
	public:
		xScrWin(QWidget* = nullptr);
	signals:
		void s_detach(bool);
	public slots:
		void upd();			// the 20 ms tick
		void setDetached(bool);
		void updateStyle();
	protected:
		void closeEvent(QCloseEvent*);
		void moveEvent(QMoveEvent*);
		void resizeEvent(QResizeEvent*);
	private:
		xZXScrPanel* panel;
		int lastfrm;		// frame the picture was taken from
};

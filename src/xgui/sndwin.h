#pragma once

#include <QDialog>

#include "debuga/dbg_sndchip.h"

// The sound chip panel in a window of its own. It belongs to the main window,
// not to the debugger: deBUGa hides itself the moment the machine is let go, so
// a panel that has to show what the chips are doing while it plays cannot live
// there alone.
class xSndWin : public QDialog {
	Q_OBJECT
	public:
		xSndWin(QWidget* = nullptr);
	signals:
		void s_detach(bool);
	public slots:
		void upd();			// one refresh per emulated frame
		void setDetached(bool);
		void updateStyle();
	protected:
		void reject();
		void closeEvent(QCloseEvent*);
		void moveEvent(QMoveEvent*);
		void resizeEvent(QResizeEvent*);
	private:
		xSndPanel* panel;
		int lastfrm;		// frame the readout was taken at
};

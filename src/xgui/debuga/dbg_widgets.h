#pragma once

#include <QDockWidget>

#include "../classes.h"
#include "../../xcore/xcore.h"
#include "../../xgui/xgui.h"

#include "dbg_brkpoints.h"
#include "dbg_disasm.h"
#include "dbg_diskdump.h"
#include "dbg_dump.h"
#include "dbg_finder.h"
#include "dbg_memfill.h"
#include "dbg_sprscan.h"
#include "dbg_rdump.h"
#include "dbg_palette.h"
#include "dbg_heat.h"

void drawHBar(QLabel*, int, int);

// cia


// cmos
#include "dbg_cmos_dump.h"


// fdc

#include "ui_form_fdd.h"

class xFDDWidget : public xDockWidget {
	Q_OBJECT
	public:
		xFDDWidget(QString, QString, QWidget* = nullptr);
	public slots:
		void draw();
	private:
		Ui::FDDWidget ui;
};
// tape

#include "ui_form_tape.h"

class xTapeWidget : public xDockWidget {
	Q_OBJECT
	public:
		xTapeWidget(QString, QString, QWidget* = nullptr);
	public slots:
		void draw();
	private:
		Ui::TapeWidget ui;
};


// zxscr: dbg_zxscr.h

// ps/2


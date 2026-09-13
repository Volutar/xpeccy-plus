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
void drawVBar(QLabel*, int, int);

// ay

#include "ui_form_ay.h"

class xAYWidget : public xDockWidget {
	Q_OBJECT
	public:
		xAYWidget(QString, QString, QWidget* = nullptr);
	public slots:
		void draw();
	private:
		Ui::AYWidget ui;
		fmChan fmView[3];	// what the FM page shows, filled per refresh
	private slots:
		void offChan(int);
};

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


// zxscr

#include "ui_form_zxscreen.h"

class xZXScrWidget : public xDockWidget {
	Q_OBJECT
	public:
		xZXScrWidget(QString, QString, QWidget* = nullptr);
		void setAddress(int, int);
	public slots:
		void draw();
		void setZoom(int);
	private:
		Ui::ZXScrWidget ui;
		QImage scrImg;
};

// ps/2


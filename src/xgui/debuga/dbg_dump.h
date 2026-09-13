#pragma once

#include <QTableView>
#include <QAbstractTableModel>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QHeaderView>
#include <QDockWidget>
#include <QMenu>

#include "../../xcore/xcore.h"
#include "../../xgui/xgui.h"

// Dump layout, shared with the disk dump so both read the same way: cells of a
// fixed width, lined up from the left, with a gap every DUMP_GROUP bytes.
#define DUMP_GROUP	4
#define DUMP_GAP	10
#define DUMP_MAXBYTES	16
#define DUMP_TEXTCOL	(DUMP_MAXBYTES + 1)	// column 0 is the address
#define DUMP_COLS	(DUMP_MAXBYTES + 2)

// One byte cell, the same in every dump so they read alike. Cells are aligned
// right: a wider cell then puts its extra space on the left, which is what
// makes the gap between groups land exactly on the boundary.
int dump_cell_width(const QFontMetrics&);

enum {
	XCP_1251 = 1,
	XCP_866,
	XCP_KOI8R
};

enum {
	XVIEW_NONE = 0,
	XVIEW_CPU,
	XVIEW_RAM,
	XVIEW_ROM,
};

class xDumpModel : public xTableModel {
	Q_OBJECT
	public:
		xDumpModel(QObject* = NULL);
		int codePage;
		unsigned int dmpadr;
		unsigned int maxadr;
		int dmpsize;
		void setMode(int, int, int, int);
		int rowCount(const QModelIndex& = QModelIndex()) const;
		int columnCount(const QModelIndex& = QModelIndex()) const;
		Qt::ItemFlags flags(const QModelIndex&) const;
		QVariant data(const QModelIndex&, int) const;
		bool setData(const QModelIndex&, const QVariant&, int);
	signals:
		void s_adrch(int);
		void s_datach();
	private:
		int mode;
		int page;
		int pgbase;
		int pgsize;
		int mrd(int) const;
		void mwr(int, unsigned char);
};

class xDumpTable:public QTableView {
	Q_OBJECT
	public:
		xDumpTable(QWidget* = NULL);
		int rows();
		void setCodePage(int);
		void setMode(int, int, int, int);
		void update();
//		int getAdr();
		unsigned int limit();
		void setRowBytes(int);		// 0 = as many groups as fit, else a fixed count
		int mode;
	public slots:
		void setAdr(int);
		int getCurrentAdr();
		void setLimit(unsigned int);
	private slots:
		void curAdrChanged();
	signals:
		void s_datach();
		void s_blockch();
		void s_adrch(int);
		void s_curadrch(int);
	private:
		int pagesize;
		int pagenum;
		int rowBytes;			// what setRowBytes was given
		void layoutColumns();
		void gotoReg(const char*);
		xDumpModel* model;
		int markAdr;
		int row_count;
		void keyPressEvent(QKeyEvent*);
		void mousePressEvent(QMouseEvent*);
		void mouseMoveEvent(QMouseEvent*);
		void mouseReleaseEvent(QMouseEvent*);
		void wheelEvent(QWheelEvent*);
		void resizeEvent(QResizeEvent*);
};

#include "ui_form_dump.h"

class xDumpWidget : public xDockWidget {
	Q_OBJECT
	public:
		xDumpWidget(QString, QString, QWidget* = nullptr);
		void setLimit(int);
	signals:
		void s_adrch(int);
		void s_curadrch(int);
		void s_datach();
		void s_blockch();
		void s_brkrq(int, int, int);
	public slots:
		void draw();
		void setAdr(int);
	private:
		Ui::MemDump ui;
		QMenu* cellMenu;
		int ramBase;
		int romBase;
		QItemDelegate* xid_addr;
		QItemDelegate* xid_byte;
		QItemDelegate* xid_none;
	private slots:
		void adr_changed(int);
		void modeChanged();
		void cp_changed();
		void bytes_changed();
		void refill();
		void customMenu();
		void customMenuAction(QAction*);
};

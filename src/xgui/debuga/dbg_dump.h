#pragma once

#include <QTableView>
#include <QAbstractTableModel>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QHeaderView>
#include <QDockWidget>
#include <QMenu>
#include <QComboBox>
#include <QTimer>
#include <QItemDelegate>

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
	XCP_437 = 0,	// the IBM PC's own: the Latin one of the 80s and 90s
	XCP_1251,
	XCP_866,
	XCP_KOI8R
};

// a dump row's bytes as text in a code page (XCP_*), a dot for a control code
QString getDumpString(QByteArray bts, int cp);
// the code pages, as a dump's list offers them
void dump_codepage_items(QComboBox* box);
// a page as a file name says it: RAM5, ROM2, DIV3, DIVROM
QString dump_page_token(int area, int page);

enum {
	XVIEW_NONE = 0,
	XVIEW_CPU,
	XVIEW_RAM,
	XVIEW_ROM,
	XVIEW_DIV,	// divMMC's RAM, by 8K bank (the dump only)
};

// the text column's selected characters, a bit each from the row's first byte
#define DUMP_SEL_ROLE	(Qt::UserRole + 1)

class xDumpModel : public xTableModel {
	Q_OBJECT
	public:
		xDumpModel(QObject* = NULL);
		int codePage;
		unsigned int dmpadr;
		unsigned int maxadr;
		int dmpsize;
		// a page view's own selection (the CPU view's: blockStart/blockEnd)
		int selMode;
		int selPage;
		int selStart;			// -1: none
		int selEnd;
		bool selRange(int* start, int* end) const;	// the selection of the view on show
		QByteArray bytes(int start, int end) const;
		int shownAdr(int adr) const;	// as the address column writes it
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
		QString cellTip(int adr, const QString& note) const;
		bool isSel(int adr) const;
};

// the text column, a character a cell: the selection and a click find their byte
class xDumpText : public QItemDelegate {
	public:
		xDumpText(QObject* par = nullptr) : QItemDelegate(par) {}
		int charAt(const QRect& cell, int x, int count) const;
		QWidget* createEditor(QWidget*, const QStyleOptionViewItem&, const QModelIndex&) const override {return nullptr;}
	protected:
		void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
	private:
		mutable int cw = 0;		// as last painted
		mutable int margin = 3;
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
		bool selection(int* start, int* end);
		QByteArray selBytes();
		QList<QByteArray> selRows();
		int shownAdr(int adr);
		void dropSelection();		// a page view's too, no signal
	public slots:
		void setAdr(int);
		int getCurrentAdr();
		void setLimit(unsigned int);
	private slots:
		void curAdrChanged();
		void dragStep();
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
		xDumpText* text;
		int anchor;			// a page view's selection starts here (the CPU view's: blockAnchor), -1: the caret
		bool dragging;
		QPoint dragPos;
		int dragDir;			// past the top -1, the bottom 1
		QTimer* dragTimer;
		int& anchorRef();
		void dragStop();
		void hideEvent(QHideEvent*) override;
		int byteAt(const QPoint&);
		int selTo(int pos, int anc);
		void focusOutEvent(QFocusEvent*) override;
		void putCaret(int);
		void selSet(int, int);
		void selClear();
		void dragTo();
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
		int viewArea(int* page);	// the area (CELL_*) and page on show, CELL_NONE: the CPU's
		bool selection(int* start, int* end);	// on the view on show, as the Save dump dialog counts
		void dropSelection();
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
		QAction* actCopyHex;
		QAction* actCopyDB;
		QAction* actCopyText;
		QAction* actSaveBin;
		QString binName(int start, int end);
		int ramBase;
		int romBase;
		QItemDelegate* xid_addr;
		QItemDelegate* xid_byte;
	private slots:
		void adr_changed(int);
		void modeChanged();
		void cp_changed();
		void bytes_changed();
		void refill();
		void customMenu(const QPoint&);
		void brkAction();
		void copyHex();
		void copyDB();
		void copyText();
		void saveBin();
};

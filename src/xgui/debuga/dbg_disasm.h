#pragma once

#include <QColor>
#include <QPair>
#include <QTableView>
#include <QStyledItemDelegate>
#include <QAbstractTableModel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QFocusEvent>

#include "../../libxpeccy/spectrum.h"
#include "../../xcore/xcore.h"
#include "../classes.h"
#include "../xgui.h"

// the command column carries the label name for the syntax delegate

#define X_LabelRole	(Qt::UserRole + 1)
// how much of a local label (global.local) is drawn shaded: the label of a
// label row, the label in the command
#define X_PrefixRole	(Qt::UserRole + 2)

// memory cell type (bits 4..7)

typedef struct {
	unsigned ispc:1;		// address is PC
	unsigned issel:1;		// address inside selection
	unsigned isbrk:1;		// breakpoint here
	unsigned islab:1;		// address have label
	unsigned iscom:1;		// address have comment
	unsigned isequ:1;		// this is equ
	unsigned issep:1;		// row is the empty block separator
	int adr;			// command addr (bus/cpu)
	int oadr;			// word operand like nn : jp nn; ld hl,(nn)
	int flag;			// address cell flags
	int oflag;			// opcode flag
	int fmt;			// the form of its numbers (CPU_NUM_*): the debugger's, # for the trace log
	QString aname;			// label/segment/address
	QString bytes;			// all bytes inside command
	QString command;		// command with replaced addr->label
	QString label;			// label put into the command, if any
	int lpre = 0;			// shaded part of the row's own label (label rows)
	int opre = 0;			// ...and of the label in the command: in the same block only
	int lrom = -1;			// the label in the command is a built-in one (ROM, system variable): its address
	QString info;			// memory argument if any
	QString icon;			// icon path if any
} dasmData;

class xDisasmModel : public xTableModel {
	Q_OBJECT
	public:
		xDisasmModel(QObject* = NULL);
		int rowCount(const QModelIndex& = QModelIndex()) const;
		int columnCount(const QModelIndex& = QModelIndex()) const;
		Qt::ItemFlags flags(const QModelIndex&) const;
		QVariant data(const QModelIndex&, int) const;
		bool setData(const QModelIndex&, const QVariant&, int);
		// void update();
		QList<dasmData> dasm;
		int asmadr;			// full memory address
	signals:
		void rqRefill();
		void s_comenter();
		void s_adrch(int, int);
	public slots:
		int update_lst();
	private:
		int fill();
		QColor dim_color(const QColor&, const QColor&) const;
};

class xDisasmTable : public QTableView {
	Q_OBJECT
	Q_PROPERTY(QColor pcbgr MEMBER pc_bgr NOTIFY colChanged)
	Q_PROPERTY(QColor pctxt MEMBER pc_txt NOTIFY colChanged)
	Q_PROPERTY(QColor blkbgr MEMBER blk_bgr NOTIFY colChanged)
	Q_PROPERTY(QColor blktxt MEMBER blk_txt NOTIFY colChanged)
	Q_PROPERTY(QColor brktxt MEMBER brk_txt NOTIFY colChanged)
	public:
		xDisasmTable(QWidget* = NULL);
		QVariant getData(int, int, int);
		int rows();
		void setMode(int, int);
		int getMode(int);
		int getAdr();
	signals:
		void rqRefill();
		void rqRefillAll();
		void s_adrch(int);
		void colChanged();
	public slots:
		int updContent();
		void update();
		void t_update(int, int);
		void setAdr(int, int = 0);
		void setAdrX(int);
		void rowDown();
	private slots:
		void updColors();
	private:
		int markAdr;
		xDisasmModel* model;
		// one entry per undoable jump: the viewport anchor (asmadr) to
		// restore the scroll position, and the cursor's own row address -
		// not the same thing, the cursor can be anywhere in the listing
		QList<QPair<int, int>> history;
		QColor pc_bgr,pc_txt,blk_bgr,blk_txt,brk_txt;
		int storedAddress[5];

		void scrolUp(Qt::KeyboardModifiers = Qt::NoModifier);
		int scrolDn(Qt::KeyboardModifiers = Qt::NoModifier);
		bool rowEnabled(int);
		void putCursor(int, int);
		void cursorDown();
		void cursorUp();

		void copyToCbrd();
		void jumpMarked(int, bool);
		void pushHistory(int);
		int rowForAdr(int);
		void selectAdr(int, int);

		void keyPressEvent(QKeyEvent*);
		void mousePressEvent(QMouseEvent*);
		void mouseReleaseEvent(QMouseEvent*);
		void mouseMoveEvent(QMouseEvent*);
		bool viewportEvent(QEvent*) override;
		void wheelEvent(QWheelEvent*);
		void resizeEvent(QResizeEvent*);
		void focusOutEvent(QFocusEvent*);
		void focusInEvent(QFocusEvent*);
};

// the label column: the global part of a local label (global.local) on its
// own row is drawn shaded

class xDasmLabel : public xItemDelegate {
	public:
		xDasmLabel() : xItemDelegate(XTYPE_LABEL) {}
	protected:
		void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const override;
		void drawDisplay(QPainter*, const QStyleOptionViewItem&, const QRect&, const QString&) const override;
	private:
		mutable int pre = 0;		// of the cell being painted
		mutable QColor bgr;
};

// the bytes column: a data row's bytes cut to what fits, with an ellipsis -
// the command beside them has them all. Edited in full

class xDasmBytes : public xItemDelegate {
	public:
		xDasmBytes() : xItemDelegate(XTYPE_DUMP) {}
	protected:
		void drawDisplay(QPainter*, const QStyleOptionViewItem&, const QRect&, const QString&) const override;
};

// paints the command column token by token: constants take their own
// colour, a label goes bold, everything else keeps the row colour

class xDasmSyntax : public QStyledItemDelegate {
	public:
		xDasmSyntax(QObject* = NULL);
		QWidget* createEditor(QWidget*, const QStyleOptionViewItem&, const QModelIndex&) const;
		bool helpEvent(QHelpEvent*, QAbstractItemView*, const QStyleOptionViewItem&, const QModelIndex&) override;
	protected:
		void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const;
};

QList<dasmData> getDisasm(Computer*, int&);
int dasmSome(Computer*, int adr, dasmData&, int fmt = -1);	// fmt -1: the debugger's form
QString dasm_export(const dasmData&);
bool is_block_end(const dasmData&);
bool dasm_ends_block(const QList<dasmData>&);
bool dasm_has_separator(const QList<dasmData>&);

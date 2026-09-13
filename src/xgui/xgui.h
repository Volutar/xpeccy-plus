#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileSystemModel>
#include <QItemDelegate>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QWheelEvent>

#include "classes.h"

#if QT_VERSION < QT_VERSION_CHECK(6,0,0)
	#include <QRegExpValidator>
#else
	#include <QRegularExpressionValidator>
	typedef QRegularExpressionValidator QRegExpValidator;
#endif

// common

void shitHappens(const char*);
bool areSure(const char*);
int askYNC(const char*);
void showInfo(const char*);

int getRFIData(QComboBox*);
void setRFIndex(QComboBox*, QVariant, int = 0);
int comboFitWidth(QComboBox*);

// subclasses

#define	XHS_BGR	1		// change background if value changed
#define	XHS_DEC	(1<<1)		// hex/dec switch enabled
#define XHS_FILL (1<<2)		// leading zeros
#define XHS_UPD (1<<3)		// force update text, reset after using
#define XHS_AUTOW (1<<4)	// keep width as narrow as the value needs
#define XHS_SPLIT (1<<5)	// light the bytes that changed, not the whole value
#define XHS_LIT (1<<6)		// keep the field lit whatever the value does

class xHexSpin : public QLineEdit {
	Q_OBJECT
	public:
		xHexSpin(QWidget* = NULL);
		void setValue(int);
		int getValue();
		void setXFlag(int);
		void setBase(int);
		void updatePal();
		void refitWidth();
		void setSplit(bool);
		void setLit(bool);
		int getMax();
	signals:
		void valueChanged(int);
	public slots:
		void setMin(int);
		void setMax(int);
	private slots:
		void onChange(int);
		void onTextChange(QString);
	private:
		int chgMask;		// bytes changed by the last setValue, bit 0 = the lowest
		int hsflag;
		int base;
		int value;
		int min;
		int max;
		int len;
		QString vtxt;
		QRegExpValidator vldtr;
		void updateMask();
		int splitBytes();
		bool wholeFieldLit();
	protected:
		void keyPressEvent(QKeyEvent*);
		void wheelEvent(QWheelEvent*);
		void paintEvent(QPaintEvent*);
};

// A style sheet takes the whole slider over, tick marks included: Qt draws none
// once QSlider::groove or ::handle is styled, so every theme but the system one
// loses them. Draw them here in that case.
class xSlider : public QSlider {
	public:
		xSlider(QWidget* p = NULL);
	protected:
		void paintEvent(QPaintEvent*);
};

class xLabel : public QLabel {
	Q_OBJECT
	public:
		xLabel(QWidget* p = NULL);
		int id;
	signals:
		void clicked(QMouseEvent*);
	protected:
		void mousePressEvent(QMouseEvent*);
};

class xTreeBox : public QComboBox {
	Q_OBJECT
	public:
		xTreeBox(QWidget* p = NULL);
		void setDir(QString);
		void setCurrentFile(QString);
		QString currentFile();
	private:
		void showPopup();
		void hidePopup();
		QTreeView* tree;
		QFileSystemModel* mod;
};

enum {
	XTYPE_NONE = -1,
	XTYPE_ADR = 0,
	XTYPE_LABEL,
	XTYPE_DUMP,
	XTYPE_BYTE,
};

class xItemDelegate : public QItemDelegate {
	public:
		xItemDelegate(int);
		int type;
		// QWidget* createEditor (QWidget*, const QStyleOptionViewItem&, const QModelIndex&) const;
	private:
		QRegExpValidator vld;
		QWidget* createEditor(QWidget*, const QStyleOptionViewItem&, const QModelIndex&) const;
};

// a check column: an item view puts its check indicator at the left edge of the
// cell and draws it with the plain style, because the interface style sheets
// only ever name QCheckBox::indicator. Draw it centered under the header and
// through a real QCheckBox, so whatever the style says about checkboxes reaches
// these too.

class xCheckItem : public QStyledItemDelegate {
	public:
		xCheckItem(QObject* par = nullptr) : QStyledItemDelegate(par) {}
		void paint(QPainter*, const QStyleOptionViewItem&, const QModelIndex&) const;
		QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const;
	private:
		QSize boxSize(const QWidget*) const;
		QCheckBox tmpl;		// never shown, only asked how the style paints a checkbox
};

// tape player

#include "options/opt_tapecat.h"
#include "ui_tapewin.h"
#include "../libxpeccy/tape.h"

enum {
	TW_STATE = 0,
	TW_REWIND,
	TW_BREAK
};

enum {
	TWS_PLAY = 1,		// as a person pressing the button
	TWS_AUTOPLAY,		// as a loader asking for the tape
	TWS_REC,
	TWS_STOP,
	TWS_OPEN,
	TWS_REWIND
};

class TapeWin : public QDialog {
	Q_OBJECT
	public:
		TapeWin(QWidget*);
	public slots:
		void updProgress(Tape*);
		void updList(Tape*);
		void upd(Tape*);
		void show();
	private:
		Ui::TapeWin ui;
		int state;
	private slots:
		void doPlay();
		void doRec();
		void doStop();
		void doLoad();
		void doRewind();
		void doDClick(QModelIndex);
		void doClick(QModelIndex);
		void setSpeed(int);
};

// rzx player

#include "ui_rzxplayer.h"
#include "../libxpeccy/spectrum.h"

enum {
	RWS_PLAY = 1,
	RWS_STOP,
	RWS_PAUSE,
	RWS_OPEN
};

class RZXWin : public QDialog {
	Q_OBJECT
	public:
		RZXWin(QWidget*);
		void setProgress(int,int);
	public slots:
		void startPlay();
		void stop();
		void upd(Computer*);
	signals:
		void stateChanged(int);
	private:
		Ui::rzxPlayer ui;
		int state;
	private slots:
		void playPause();
		void open();
};

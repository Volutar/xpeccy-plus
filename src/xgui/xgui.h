#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileSystemModel>
#include <QItemDelegate>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QGroupBox>
#include <QGridLayout>
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
#define XHS_BLANK (1<<7)	// may hold nothing at all, shown as dashes

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
		void setBlank();	// show no value at all, until one is set or typed
		bool isBlank() const;	// holding nothing, the way setBlank() left it
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
		bool isblank;		// holding nothing, with XHS_BLANK
		int hsflag;
		int base;
		int value;
		int min;
		int max;
		int len;
		QString vtxt;
		QRegExpValidator vldtr;
		void updateMask();
		void unblank();
		void startEdit();
		int splitBytes();
		bool wholeFieldLit();
	protected:
		void changeEvent(QEvent*);
		void keyPressEvent(QKeyEvent*);
		void focusInEvent(QFocusEvent*);
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

// A push button with its icon at the right edge and its text from the left,
// cut short to fit; Qt only centers the two together.
class xSideButton : public QPushButton {
	public:
		xSideButton(QWidget* p = NULL);
		QSize sizeHint() const;
	protected:
		void paintEvent(QPaintEvent*);
};

// A group box with an icon in front of its title. The title is padded with
// spaces to make room, since no style draws an icon there.
class xIconGroup : public QGroupBox {
	public:
		xIconGroup(const QString&, const QString&, QWidget* p = NULL);
		xIconGroup(QWidget* p = NULL);
		void setIcon(const QString&);
	protected:
		void paintEvent(QPaintEvent*);
		void changeEvent(QEvent*);
	private:
		QIcon icon;
		QString text;
		void padTitle();
};

// A pop-up laid out like Advanced settings: fields with a name first, then a
// line, then check boxes with a name and, in italics, what they do.
class xSheetColumns;

class xOptSheet {
	public:
		QWidget* body;
		xOptSheet();
		void group(const QString&, int side = -1, const QString& icon = QString());
		void field(const QString&, QWidget*, const QString& = QString());
		void row(const QString&, QWidget*);
		void wide(QWidget*);
		void line();
		void check(QCheckBox*, const QString&, const QString&, bool global = false);
	private:
		QVBoxLayout* box;
		QWidget* rows;
		QGridLayout* grid;
		QHBoxLayout* pair;	// the row of frames side by side, while one is being filled
		QVBoxLayout* half[2];
		int nameSet;		// the column the frame being filled is in: 1 lines its names up apart
		xSheetColumns* cols;
		QLabel* text(const QString&);
};

// a control and what goes after it, as one field of a sheet
QWidget* fieldPair(QWidget*, QWidget*, bool);

// a window over the middle of another, and the first time a window is shown,
// over the one it belongs to (see center_over in classes.cpp); a window with
// the xCenterOnce property gets the second from xApp on its first show
void center_over(QWidget*, QWidget*);
void center_once(QWidget*);

void help_window(QWidget* parent, QDialog** win, const QString& res, const QString& title);
void label_complete(QLineEdit*);
bool lab_char(QChar);

// offers to get FFmpeg and does: true when there is one to use afterwards
bool ffmpeg_get(QWidget* parent);

// a text that shortens in the middle to the width it gets, whole in its tooltip
class xElideLabel : public QLabel {
	public:
		xElideLabel(QWidget* p = NULL);
		void setFull(const QString&);
		QSize sizeHint() const;
		QSize minimumSizeHint() const;
	protected:
		void paintEvent(QPaintEvent*);
	private:
		QString full;
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
	private:
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

// What the tape player window is being told to do. All of these are a person
// pressing a button: the emulation thread owns the tape and moves it there,
// and the window follows on its own refresh.
enum {
	TWS_PLAY = 1,
	TWS_REC,
	TWS_STOP,		// beats the automatics until the tape is started by hand
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
	signals:
		void wannaOpen();
	private:
		Ui::TapeWin ui;
		int state;
		QByteArray tapeRaw;	// the path as the tape holds it, to notice a swap
		int tapeChanged = -1;	// tape->changed as the title shows it
	private slots:
		void doPlay();
		void doRec();
		void doStop();
		void doLoad();
		void doSave();
		void doExport();
		void doRewind();
		void doEject();
		void doDClick(QModelIndex);
		void doClick(QModelIndex);
		void setSpeed(int);
		void setOptions();
		void doBlkUp();
		void doBlkDn();
		void doBlkDel();
		void doToDisk(QAction*);
};

// rzx player

#include "ui_rzxplayer.h"
#include "../libxpeccy/spectrum.h"
#include "../libxpeccy/filetypes/filetypes.h"

enum {
	RWS_PLAY = 1,
	RWS_STOP,
	RWS_PAUSE,
	RWS_OPEN
};

enum {
	RZC_START = 0,
	RZC_LEN,
	RZC_KIND,
	RZC_INFO,
	RZC_COUNT
};

// the blocks of a recording, one row each
typedef struct {
	QStringList cells;
	int frame;			// where a double click goes, -1 nowhere
	int frames;
	int blocks;			// input blocks in the row
	bool input;
} xRzxRow;

class xRzxModel : public xTableModel {
	public:
		xRzxModel(QObject* p = NULL);
		void fill(const rzxInfo*, double fps);
		int setCurrent(int frame);
		int frameAt(int row) const;
		// what fill() found, for the window's summary
		QString creator;
		QString machine;
		int snapStart;
		int snapInside;
		int snapEnd;
	private:
		QList<xRzxRow> rows;
		QVariant data(const QModelIndex&, int) const;
		QVariant headerData(int, Qt::Orientation, int) const;
};

class RZXWin : public QDialog {
	Q_OBJECT
	public:
		RZXWin(QWidget*);
	public slots:
		void startPlay();
		void stop();
		void upd(Computer*);
	signals:
		void stateChanged(int);
		void seekTo(int);
	protected:
		bool eventFilter(QObject*, QEvent*);
	private:
		Ui::rzxPlayer ui;
		xRzxModel* model;
		int state;
		double fps;
		void fillInfo();
		void setProgress(int,int);
	private slots:
		void playPause();
		void open();
		void doDClick(QModelIndex);
};

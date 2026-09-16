#pragma once

#include "dbg_widgets.h"
#include "ui_form_psg.h"
#include "ui_form_fm.h"

#include <vector>

#include <QCheckBox>
#include <QLabel>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>

// A machine may carry up to four sound chips and each YM2203 has an FM half as
// well as a PSG one, so the panel is a row of tabs - PSG1, FM1, PSG2, FM2 ...
// built from the live machine. The pages are the same whichever chip they show,
// so there is one of each and the tab only says which chip to point it at.

// what a tab carries, and how to read it back
#define SND_TAB_ID(chip, fm)	(((chip) << 1) | ((fm) ? 1 : 0))
#define SND_TAB_CHIP(id)	((id) >> 1)
#define SND_TAB_FM(id)		((id) & 1)

// A level, as a bar with its own figure on it. The figure is drawn twice, once
// over the bar and once over the ground, each in a colour that can be read
// there: one colour over two backgrounds is what makes the tape player's
// progress figure disappear into the bar in half the styles.
class xLevelCell : public QWidget {
	Q_OBJECT
	public:
		xLevelCell(QWidget* = nullptr);
		void setLevel(int val, int max, bool fall);	// val < 0 for nothing at all
		void setFigure(int);		// what the figure says, -1 for a dash
		void setDigits(int);		// how wide the figure is written
		QSize minimumSizeHint() const;
	protected:
		void paintEvent(QPaintEvent*);
	private:
		int lev;			// the bar: the peak, on its way down
		int fig;			// the figure, which is not always the same thing
		int top;
		int digits;
};

// Two ramps of an envelope, drawn from what the core actually does with that
// form - so every form is the same length and a ramp the same slope. No readout
// of the form number: the shape is the readout.
class xAYEnvView : public QWidget {
	Q_OBJECT
	public:
		xAYEnvView(QWidget* = nullptr);
		void setForm(int);
		QSize minimumSizeHint() const;
	protected:
		void paintEvent(QPaintEvent*);
	private:
		int form;
};

class xPSGPage : public QWidget {
	Q_OBJECT
	public:
		xPSGPage(QWidget* = nullptr);
		void setChip(aymChip*);
		void draw();
	private:
		Ui::PSGPage ui;
		aymChip* chip;
		bool hold;			// draw() is moving the fields
		xHexSpin* regs[16];		// the minidump
		xHexSpin* per[5];		// period per row: A B C N E
		xHexSpin* vol[3];		// volume per channel
		xLevelCell* lev[3];		// what the channel is putting out, bar and figure
		QCheckBox* mute[3];
		xAYEnvView* envView;
		xHexSpin* addSpin(QWidget* host, int max);
		void reg_edited(int reg, int val);
		void per_edited(int row, int val);
		void vol_edited(int chan, int val);
		void mute_toggled(int chan, bool on);
		void blank();
};

// The mixer's own capture, newest at the right edge: a pixel column is the lowest
// and the highest sample in it with the average drawn over them, on a fixed scale.
// How much time the box holds follows the machine - see WAVE_SECS_* in the source.
class xWaveView : public QWidget {
	Q_OBJECT
	public:
		xWaveView(QWidget* = nullptr);
		void refresh();			// take a new window of the capture
		QString info() const;		// the peak and the timebase, for a label
		QSize minimumSizeHint() const;
	protected:
		void paintEvent(QPaintEvent*);
		void changeEvent(QEvent*);
	private:
		std::vector<short> buf;		// all held across refreshes, not built per frame
		std::vector<int> cmin;
		std::vector<int> cmax;
		std::vector<int> cavg;
		int peak;			// the loudest in the window, for the label
		bool held;
		int ruler() const;	// width of the scale down the left edge
		mutable int rulw;	// what that measured, until the font changes
		void sample();
};

// The operator table's columns, in the order they are shown. A column with a
// register base is editable, the rest are readouts.
enum {
	FMC_DT = 0, FMC_MUL, FMC_ST, FMC_TL, FMC_RS, FMC_AR,
	FMC_DR, FMC_SL, FMC_SR, FMC_RR, FMC_EG, FMC_BKFQ, FMC_LEV, FMC_OUT,
	FMC_COUNT
};

class xFMPage : public QWidget {
	Q_OBJECT
	public:
		xFMPage(QWidget* = nullptr);
		void setChip(aymChip*);
		void draw();
	private slots:
		void offChan(int);
		void chan_changed(int);
	protected:
		void changeEvent(QEvent*);
	private:
		Ui::FMPage ui;
		aymChip* chip;
		bool hold;			// draw() is moving the fields
		QTabBar* chanTabs;
		xHexSpin* opEdit[4][FMC_COUNT];	// null where the column is a readout
		QLabel* opLab[4][FMC_COUNT];	// and the other way round
		xLevelCell* opLev[4];		// the Lev column, which is neither
		fmChan fmView[3];		// the core's FM state, copied out per refresh
		void fitHeaders();	// the readouts keep their widest width
		void op_edited(int op, int col, int val);
		int channel() const;
		void blank();
};

// The panel: the tab row, the page it selects, and the beeper level and the
// scope under them.
// Lives in the debugger dock and, when detached, in a window of its own - one
// instance each, sharing their state through conf.dbg.
class xSndPanel : public QWidget {
	Q_OBJECT
	public:
		xSndPanel(QWidget* = nullptr);
		void reload();			// take the settings back from conf
	signals:
		void s_detach(bool);
	public slots:
		void draw();
	private slots:
		void tab_changed(int);
	private:
		QTabBar* tabs;
		QStackedWidget* stack;
		QToolButton* tbDetach;
		QLabel* labBeep;
		QLabel* labWave;		// the scope's peak and timebase, off the picture
		int lastBeep;			// what the beeper bar was last drawn at
		xWaveView* wave;
		xPSGPage* psg;
		xFMPage* fm;
		bool hold;			// reload() or build_tabs() is moving the controls
		int layout_sig;			// the machine the tab row was built for
		aymChip* chipAt(int) const;
		int machine_sig() const;
		void build_tabs();
		void show_tab(int id);
};

class xSndWidget : public xDockWidget {
	Q_OBJECT
	public:
		xSndWidget(QString, QString, QWidget* = nullptr);
		void setDetached(bool);
	signals:
		void s_detach(bool);
	public slots:
		void draw();
	private:
		xSndPanel* panel;
		QWidget* gone;			// stands in while the window has the panel
};

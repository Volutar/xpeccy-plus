#pragma once

#include "dbg_widgets.h"
#include "ui_form_zxscreen.h"

#include <QImage>
#include <QWidget>

// A ZX screen is 256x192 dots; the panel frames it with a strip of border so
// the border colour is visible too
#define XSCR_W		256
#define XSCR_H		192
#define XSCR_BRD	10
#define XSCR_TILEW	(XSCR_W + 2 * XSCR_BRD)
#define XSCR_TILEH	(XSCR_H + 2 * XSCR_BRD)
#define XSCR_GAP	6	// between the two screens of the Both view

// XSCR_* - the mode, zoom and page constants - are in xcore.h: the config
// reader needs them too

// What the readout says when the cursor is not on a screen

#define XSCR_NODOT	"#----"
#define XSCR_NOBIT	".-"
// the screen address carries a .bit the attribute one does not, so the
// attribute line is padded to put the two addresses in one column
#define XSCR_ATRPAD	"  "
#define XSCR_NOXY	"-,-"

// The picture. Painted rather than laid out, so it can fill the panel, hold
// two screens at once and name the dot under the cursor.
class xZXScrView : public QWidget {
	Q_OBJECT
	public:
		xZXScrView(QWidget* = nullptr);
		void setView(int mode, int zoom, int flags, int page, int shift);
		void redraw();			// re-read the machine, then repaint
		void markAdr(int adr);		// mark the dot an address holds, -1 for none
		QSize minimumSizeHint() const;
	signals:
		// where the cursor is and what holds that dot; x is -1 for nowhere
		void s_dot(int x, int y, int pix, int atr);
	protected:
		void paintEvent(QPaintEvent*);
		void mousePressEvent(QMouseEvent*);
		void mouseMoveEvent(QMouseEvent*);
		void leaveEvent(QEvent*);
		void contextMenuEvent(QContextMenuEvent*);
	private:
		// where the picture stands and how big it is, worked out in one go:
		// the Both view picks its own orientation from the panel's shape
		struct xScrGeom {
			QRect tile[2];		// picture area of each screen
			int head;		// caption height
			int count;
			double scale;		// pixels per dot, for the hit test and the mark
		};
		int mode;
		int zoom;
		int flags;
		int cpage;			// page for XSCR_CUSTOM
		int cshift;			// offset for XSCR_CUSTOM
		int markSlot;			// the screen the mark is on, -1 for no mark
		int curSlot;			// the screen curx/cury are on
		int curx;			// the dot the readout is on, -1 for none
		int cury;
		int pixadr;			// and the two addresses that hold it
		int atradr;
		QImage img[2];
		int page[2];			// page each image was read from
		xScrGeom geom() const;
		double fitScale(bool horiz) const;
		int pageFor(int slot) const;
		QString tileName(int slot) const;
		bool dotAt(const QPoint&, int* slot, int* x, int* y, int* pix, int* atr) const;
		void paintMark(QPainter&, const QRect&, double scale) const;
		void trackDot(const QPoint&);
		void clearDot();
		int baseFor(int slot) const;
		void copyAdr(int) const;
};

// The panel: the picture plus its controls. Lives in the debugger dock and,
// when detached, in a window of its own - one instance each, sharing their
// state through conf.dbg.
class xZXScrPanel : public QWidget {
	Q_OBJECT
	public:
		xZXScrPanel(QWidget* = nullptr);
		void reload();			// take the settings back from conf
	signals:
		void s_detach(bool);
	public slots:
		void draw();
		void show_dot(int x, int y, int pix, int atr);
	private slots:
		void mode_changed();
		void opts_changed();
		void custom_changed();
		void find_changed();
	private:
		Ui::ZXScrWidget ui;
		xZXScrView* view;
		bool hold;			// reload() is moving the controls
		QButtonGroup* grp;		// the mode buttons, each carrying its XSCR_*
		QFont fitFont;			// the font the field widths were measured in
		void fit_fields();
		void apply_mode(int);
};

class xZXScrWidget : public xDockWidget {
	Q_OBJECT
	public:
		xZXScrWidget(QString, QString, QWidget* = nullptr);
		void showDot(int x, int y, int pix, int atr);
		void setDetached(bool);
	signals:
		void s_detach(bool);
	public slots:
		void draw();
	private:
		xZXScrPanel* panel;
		QWidget* gone;			// stands in while the window has the panel
};

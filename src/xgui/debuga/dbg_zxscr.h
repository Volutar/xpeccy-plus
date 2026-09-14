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

// The picture. Painted rather than laid out, so it can fill the panel, hold
// two screens at once and answer a click with the address of the dot.
class xZXScrView : public QWidget {
	Q_OBJECT
	public:
		xZXScrView(QWidget* = nullptr);
		void setView(int mode, int zoom, int flags, int page, int shift);
		void redraw();			// re-read the machine, then repaint
		QSize minimumSizeHint() const;
	signals:
		void s_adr(int, int);		// pixel and attribute address of the dot
	protected:
		void paintEvent(QPaintEvent*);
		void mousePressEvent(QMouseEvent*);
		void contextMenuEvent(QContextMenuEvent*);
	private:
		// where the picture stands and how big it is, worked out in one go:
		// the Both view picks its own orientation from the panel's shape
		struct xScrGeom {
			QRect tile[2];		// picture area of each screen
			int head;		// caption height, 0 when there is one screen
			int count;
		};
		int mode;
		int zoom;
		int flags;
		int cpage;			// page for XSCR_CUSTOM
		int cshift;			// offset for XSCR_CUSTOM
		int pixadr;			// what the last click landed on
		int atradr;
		QImage img[2];
		int page[2];			// page each image was read from
		xScrGeom geom() const;
		double fitScale(bool horiz) const;
		int pageFor(int slot) const;
		QString tileName(int slot) const;
		bool adrAt(const QPoint&, int* pix, int* atr) const;
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
		void setAddress(int, int);
	signals:
		void s_detach(bool);
	public slots:
		void draw();
	private slots:
		void mode_changed();
		void opts_changed();
		void custom_changed();
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
		void setAddress(int, int);
		void setDetached(bool);
	signals:
		void s_detach(bool);
	public slots:
		void draw();
	private:
		xZXScrPanel* panel;
		QWidget* gone;			// stands in while the window has the panel
};

// ZX screen panel

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QMenu>
#include <QPainter>
#include <QPushButton>

#include "dbg_zxscr.h"

// The two pages every 128K machine draws from have names of their own; any
// other page is only ever a number
static QString scr_page_name(int page) {
	switch (page) {
		case XSCR_PAGE_MAIN: return QString("Main (5)");
		case XSCR_PAGE_SHADOW: return QString("Shadow (7)");
	}
	return QString("Page %0").arg(gethexbyte(page));
}

// Page 7 needs a core that can address past 64K, and a machine that has the
// ram for it. The core has to be asked first: a machine's own size is not
// enough, a ZX 48K is built with 128K of ram allocated behind it.
static bool scr_has_shadow(Computer* comp) {
	if (comp->hw->mask && !(comp->hw->mask & ~(MEM_128K - 1))) return false;
	return (comp->mem->ramSize >= (XSCR_PAGE_SHADOW + 1) * MEM_16K);
}

// What a picture is headed with: the page it came from, and in Custom the
// fact that you picked it yourself
QString xZXScrView::tileName(int slot) const {
	if (mode == XSCR_CUSTOM)
		return QString("Custom (%0)").arg(gethexbyte(page[slot]));
	return scr_page_name(page[slot]);
}

// VIEW

xZXScrView::xZXScrView(QWidget* p):QWidget(p) {
	mode = XSCR_AUTO;
	zoom = XSCR_FIT;
	flags = 0;
	cpage = XSCR_PAGE_MAIN;
	cshift = 0;
	pixadr = -1;
	atradr = -1;
	for (int i = 0; i < 2; i++) {
		img[i] = QImage(XSCR_W, XSCR_H, QImage::Format_RGB888);
		img[i].fill(Qt::black);
		page[i] = XSCR_PAGE_MAIN;
	}
	markSlot = -1;
	curSlot = -1;
	curx = -1;
	cury = -1;
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setContextMenuPolicy(Qt::DefaultContextMenu);
	setMouseTracking(true);		// the readout follows the cursor, not the button
	setFocusPolicy(Qt::ClickFocus);	// so the address fields can be let go of
}

QSize xZXScrView::minimumSizeHint() const {
	return QSize(XSCR_TILEW / 2, XSCR_TILEH / 2);
}

// Everything the panel decides, in one go: the repaint comes from redraw(),
// which follows on every call site anyway
void xZXScrView::setView(int m, int z, int f, int pg, int sh) {
	mode = m;
	zoom = (z < XSCR_FIT) ? XSCR_FIT : (z > XSCR_ZOOMMAX) ? XSCR_ZOOMMAX : z;
	flags = f;
	cpage = pg;
	cshift = sh;
}

int xZXScrView::pageFor(int slot) const {
	Computer* comp = conf.zx;
	switch (mode) {
		case XSCR_MAIN: return XSCR_PAGE_MAIN;
		case XSCR_SHADOW: return XSCR_PAGE_SHADOW;
		case XSCR_BOTH: return slot ? XSCR_PAGE_SHADOW : XSCR_PAGE_MAIN;
		case XSCR_CUSTOM: return cpage;
	}
	return comp->vid->vidPage;
}

// A running machine is read from the copy taken at the last frame boundary,
// so the picture is never caught halfway through being rewritten. A stopped
// one makes no frames and so has no copy - and a byte changed by hand in the
// debugger has to show at once - so that is read as it stands.
void xZXScrView::redraw() {
	Computer* comp = conf.zx;
	int cnt = (mode == XSCR_BOTH) ? 2 : 1;
	int shift = (mode == XSCR_CUSTOM) ? cshift : 0;
	bool running = !conf.emu.pause;
	for (int i = 0; i < cnt; i++) {
		// Auto asks for whichever screen is on air rather than the page it saw
		// last time, so a program flipping between them still gets a copy
		int want = (mode == XSCR_AUTO) ? VSCR_ONAIR : pageFor(i);
		const unsigned char* src = NULL;
		if (running) {
			src = vid_scr_snap_get(i, want, shift);
			vid_scr_want(i, want, shift);		// for the next frame
		}
		page[i] = src ? vid_scr_snap_page(i) : pageFor(i);
		vid_get_screen(comp->vid, img[i].bits(), page[i], shift, flags, src);
	}
	update();
}

// how much of a dot one pixel is worth, for a given screen count and direction
double xZXScrView::fitScale(bool horiz) const {
	int count = (mode == XSCR_BOTH) ? 2 : 1;
	int head = fontMetrics().height() + 2;
	double w, h;
	if (horiz) {
		w = (width() - (count - 1) * XSCR_GAP) / (double)(count * XSCR_TILEW);
		h = (height() - head) / (double)XSCR_TILEH;
	} else {
		w = width() / (double)XSCR_TILEW;
		h = (height() - count * head - (count - 1) * XSCR_GAP) / (double)(count * XSCR_TILEH);
	}
	double s = (w < h) ? w : h;
	return (s < 0.05) ? 0.05 : s;
}

xZXScrView::xScrGeom xZXScrView::geom() const {
	xScrGeom g;
	g.count = (mode == XSCR_BOTH) ? 2 : 1;
	g.head = fontMetrics().height() + 2;
	// the Both view takes whichever arrangement leaves the picture bigger, so
	// a tall dock stacks the screens and a wide one puts them side by side
	double sh = fitScale(true);
	double sv = fitScale(false);
	bool horiz = (sh >= sv);
	double s = horiz ? sh : sv;
	// a fixed zoom is exactly that; Fit snaps to whole pixels per dot as soon
	// as there is room for one, and only scales down below that
	if (zoom != XSCR_FIT) {
		s = zoom;
	} else if (s >= 1.0) {
		s = (int)s;
	}
	g.scale = s;
	int tw = (int)(XSCR_TILEW * s);
	int th = (int)(XSCR_TILEH * s);
	int allw = horiz ? (g.count * tw + (g.count - 1) * XSCR_GAP) : tw;
	int allh = horiz ? (th + g.head) : (g.count * (th + g.head) + (g.count - 1) * XSCR_GAP);
	int x0 = (width() - allw) / 2;
	int y0 = (height() - allh) / 2;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	for (int i = 0; i < 2; i++) {
		if (horiz) {
			g.tile[i] = QRect(x0 + i * (tw + XSCR_GAP), y0 + g.head, tw, th);
		} else {
			g.tile[i] = QRect(x0, y0 + i * (th + g.head + XSCR_GAP) + g.head, tw, th);
		}
	}
	return g;
}

void xZXScrView::paintEvent(QPaintEvent*) {
	Computer* comp = conf.zx;
	QPainter pnt(this);
	pnt.fillRect(rect(), palette().color(QPalette::Window));
	// the border is one colour for the whole machine, so both screens get it
	xColor bcol = vid_brd_col(comp->vid);
	xScrGeom g = geom();
	QColor hbg = conf.pal.value("dbg.header.bg");
	QColor htx = conf.pal.value("dbg.header.txt");
	for (int i = 0; i < g.count; i++) {
		QRect t = g.tile[i];
		if ((t.width() < 4) || (t.height() < 4)) continue;
		pnt.fillRect(t, QColor(bcol.r, bcol.g, bcol.b));
		int bw = t.width() * XSCR_BRD / XSCR_TILEW;
		int bh = t.height() * XSCR_BRD / XSCR_TILEH;
		pnt.drawImage(t.adjusted(bw, bh, -bw, -bh), img[i]);
		// the caption names the screen and says whether it is the one on
		// air: the live one gets the dock titles' own colours, the other
		// the plain ones of the style
		QRect head(t.left(), t.top() - g.head, t.width(), g.head);
		bool live = (page[i] == comp->vid->vidPage);
		pnt.fillRect(head, live ? hbg : palette().color(QPalette::Mid));
		pnt.setPen(live ? htx : palette().color(QPalette::WindowText));
		pnt.drawText(head, Qt::AlignCenter, tileName(i));
		if (markSlot == i) paintMark(pnt, t, g.scale);
	}
}

// where this screen's addresses are counted from
int xZXScrView::baseFor(int slot) const {
	int base = vid_scr_base(page[slot]);
	if (mode == XSCR_CUSTOM) base += cshift;
	return base;
}

// The marked cell, in two tones so it shows on any colour, with the eight dots
// the raster address names picked out inside it.
void xZXScrView::paintMark(QPainter& pnt, const QRect& t, double s) const {
	int cw = (int)(8 * s);
	if (cw < 4) cw = 4;
	int bx = t.left() + (int)((XSCR_BRD + (curx & ~7)) * s);
	int by = t.top() + (int)((XSCR_BRD + (cury & ~7)) * s);
	int rh = (int)s;
	if (rh < 1) rh = 1;
	pnt.fillRect(QRect(bx, t.top() + (int)((XSCR_BRD + cury) * s), cw, rh), QColor(255, 255, 255, 110));
	QRect cell(bx, by, cw, cw);
	pnt.setPen(Qt::black);
	pnt.drawRect(cell.adjusted(-1, -1, 0, 0));
	pnt.setPen(Qt::white);
	pnt.drawRect(cell.adjusted(0, 0, -1, -1));
}

// the dot under a point: where it is on the screen and what holds it. The
// border strip around the picture is not a dot, and neither is the ground
// between two screens.
bool xZXScrView::dotAt(const QPoint& p, int* slot, int* dx, int* dy, int* pix, int* atr) const {
	xScrGeom g = geom();
	for (int i = 0; i < g.count; i++) {
		QRect t = g.tile[i];
		if (!t.contains(p)) continue;
		if (t.width() < 1) return false;
		int x = (int)((p.x() - t.left()) / g.scale) - XSCR_BRD;
		int y = (int)((p.y() - t.top()) / g.scale) - XSCR_BRD;
		if ((x < 0) || (x >= XSCR_W) || (y < 0) || (y >= XSCR_H)) return false;
		vid_scr_adr(baseFor(i), x, y, pix, atr);
		*slot = i;
		*dx = x;
		*dy = y;
		return true;
	}
	return false;
}

void xZXScrView::clearDot() {
	curSlot = -1;
	curx = -1;
	cury = -1;
	pixadr = -1;
	atradr = -1;
}

void xZXScrView::trackDot(const QPoint& p) {
	if (!dotAt(p, &curSlot, &curx, &cury, &pixadr, &atradr))
		clearDot();
	emit s_dot(curx, cury, pixadr, atradr);
}

// A click holds the readout on one dot and marks it on the picture, so the
// numbers can be read, copied or screenshotted without the mouse having to
// stay still. The next click lets it go again.
void xZXScrView::mousePressEvent(QMouseEvent* ev) {
	if (ev->button() != Qt::LeftButton) return;
	bool held = (markSlot >= 0);
	markSlot = -1;			// let go, so the readout can move again
	trackDot(ev->pos());
	if (!held && (curx >= 0)) markSlot = curSlot;
	update();
}

// Where an address lands on the picture: the raster ones name a byte of eight
// dots, the attribute ones a whole cell. Both mark the cell that holds them.
void xZXScrView::markAdr(int adr) {
	markSlot = -1;
	clearDot();
	if (adr >= 0) {
		xScrGeom g = geom();
		for (int i = 0; i < g.count; i++) {
			if (!vid_scr_dot((adr - baseFor(i)) & 0xffff, &curx, &cury)) continue;
			vid_scr_adr(baseFor(i), curx, cury, &pixadr, &atradr);
			markSlot = i;
			curSlot = i;
			break;
		}
		if (markSlot < 0) clearDot();	// an address on no screen shown
	}
	emit s_dot(curx, cury, pixadr, atradr);
	update();
}

void xZXScrView::setFocusFrom(QList<QWidget*> lst) {
	focusFrom = lst;
}

// An address field holds on to the caret, and a field with the caret in it is
// left alone - so without this the readout would stay dead after one edit,
// with no obvious way of letting go. Coming back to the picture is that way.
// Only those fields, though: the rest of the panel keeps what it is given.
void xZXScrView::takeFocusBack() {
	if (hasFocus()) return;
	QWidget* foc = QApplication::focusWidget();
	if (foc && focusFrom.contains(foc))
		setFocus(Qt::MouseFocusReason);
}

void xZXScrView::mouseMoveEvent(QMouseEvent* ev) {
	takeFocusBack();
	if (markSlot >= 0) return;		// held on the marked dot
	trackDot(ev->pos());
}

void xZXScrView::leaveEvent(QEvent*) {
	if (markSlot >= 0) return;		// held on the marked dot
	clearDot();
	emit s_dot(curx, cury, pixadr, atradr);
}

void xZXScrView::copyAdr(int adr) const {
	QApplication::clipboard()->setText(gethexword(adr));
}

// The readout moves with the cursor, so it cannot be selected and copied.
// This is where a value is taken from instead, for the dot the menu was
// opened over.
void xZXScrView::contextMenuEvent(QContextMenuEvent* ev) {
	if (markSlot < 0) trackDot(ev->pos());
	QMenu menu(this);
	QAction* apix = menu.addAction(QString("Copy screen address"));
	QAction* aatr = menu.addAction(QString("Copy attribute address"));
	apix->setEnabled(pixadr >= 0);
	aatr->setEnabled(atradr >= 0);
	QAction* act = menu.exec(ev->globalPos());
	if (act == apix) copyAdr(pixadr);
	if (act == aatr) copyAdr(atradr);
}

// PANEL

xZXScrPanel::xZXScrPanel(QWidget* p):QWidget(p) {
	ui.setupUi(this);
	hold = false;
	// not even "nowhere" yet, so the first draw always shows
	for (int i = 0; i < 4; i++) lastdot[i] = -2;

	view = new xZXScrView;
	view->setToolTip("Click to hold the readout. Right click copies");
	view->setFocusFrom(QList<QWidget*>() << ui.leScr << ui.leAtr);
	ui.layScrBody->insertWidget(0, view, 10);

	ui.cbScrZoom->addItem("Zoom Fit", XSCR_FIT);
	for (int z = 1; z <= XSCR_ZOOMMAX; z++)
		ui.cbScrZoom->addItem(QString("Zoom x%0").arg(z), z);

	// QToolButton only groups siblings it was told about, so the row needs a
	// group of its own to stay exclusive
	grp = new QButtonGroup(this);
	grp->addButton(ui.tbScrAuto, XSCR_AUTO);
	grp->addButton(ui.tbScrMain, XSCR_MAIN);
	grp->addButton(ui.tbScrShadow, XSCR_SHADOW);
	grp->addButton(ui.tbScrBoth, XSCR_BOTH);
	grp->addButton(ui.tbScrCustom, XSCR_CUSTOM);
	grp->setExclusive(true);

	// The two Custom inputs keep the default flags - no XHS_BGR, since a value
	// moving is not news here the way it is in the register panel these borrow
	// their look from. Their widths are pinned in fit_fields().
	ui.leScrAdr->setMax(0x3fff);
	// these two show dashes while nothing is pointed at, so they say as much
	// as the rows above them do
	ui.leScr->setXFlag(XHS_DEC | XHS_BLANK | XHS_AUTOW);
	ui.leAtr->setXFlag(XHS_DEC | XHS_BLANK | XHS_AUTOW);
	ui.leScrPage->setMax(0xff);
	// a short value lines up with the long ones instead of floating in its box
	ui.leScrPage->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	ui.leScrAdr->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

	foreach (QAbstractButton* btn, grp->buttons())
		connect(btn, &QAbstractButton::clicked, this, &xZXScrPanel::mode_changed);
	connect(ui.cbScrZoom, SIGNAL(currentIndexChanged(int)), this, SLOT(opts_changed()));
	connect(ui.cbScrPix, &QCheckBox::toggled, this, &xZXScrPanel::opts_changed);
	connect(ui.cbScrAtr, &QCheckBox::toggled, this, &xZXScrPanel::opts_changed);
	connect(ui.cbScrFlash, &QCheckBox::toggled, this, &xZXScrPanel::opts_changed);
	connect(ui.cbScrGrid, &QCheckBox::toggled, this, &xZXScrPanel::opts_changed);
	connect(ui.leScrPage, SIGNAL(valueChanged(int)), this, SLOT(custom_changed()));
	connect(ui.leScrAdr, SIGNAL(valueChanged(int)), this, SLOT(custom_changed()));
	connect(ui.leScr, SIGNAL(valueChanged(int)), this, SLOT(adr_changed(int)));
	connect(ui.leAtr, SIGNAL(valueChanged(int)), this, SLOT(adr_changed(int)));
	connect(view, &xZXScrView::s_dot, this, &xZXScrPanel::show_dot);
	show_dot(-1, -1, -1, -1);		// nothing pointed at yet
	connect(ui.tbScrDetach, &QToolButton::clicked, this, &xZXScrPanel::s_detach);

	reload();
}

// the group was handed every button's XSCR_* id, so the mode table is written
// once and read back through it
void xZXScrPanel::apply_mode(int m) {
	QAbstractButton* btn = grp->button(m);
	if (!btn) btn = ui.tbScrAuto;
	btn->setChecked(true);
}

// the settings live in conf, so the dock and the window always agree
void xZXScrPanel::reload() {
	hold = true;
	apply_mode(conf.dbg.scrmode);
	ui.cbScrZoom->setCurrentIndex(ui.cbScrZoom->findData(conf.dbg.scrzoom));
	ui.cbScrPix->setChecked(conf.dbg.scrnopix);
	ui.cbScrAtr->setChecked(conf.dbg.scrnoatr);
	ui.cbScrFlash->setChecked(conf.dbg.scrnoflash);
	ui.cbScrGrid->setChecked(conf.dbg.scrgrid);
	ui.leScrPage->setValue(conf.dbg.scrpage);
	ui.leScrAdr->setValue(conf.dbg.scrofs);
	ui.tbScrDetach->setChecked(conf.dbg.scrdetach);
	hold = false;
	draw();
}

void xZXScrPanel::mode_changed() {
	if (hold) return;
	conf.dbg.scrmode = grp->checkedId();
	draw();
}

void xZXScrPanel::opts_changed() {
	if (hold) return;
	conf.dbg.scrzoom = getRFIData(ui.cbScrZoom);
	conf.dbg.scrnopix = ui.cbScrPix->isChecked() ? 1 : 0;
	conf.dbg.scrnoatr = ui.cbScrAtr->isChecked() ? 1 : 0;
	conf.dbg.scrnoflash = ui.cbScrFlash->isChecked() ? 1 : 0;
	conf.dbg.scrgrid = ui.cbScrGrid->isChecked() ? 1 : 0;
	draw();
}

// an address typed into either field marks the dot it names
void xZXScrPanel::adr_changed(int adr) {
	view->markAdr(adr);
}

void xZXScrPanel::custom_changed() {
	if (hold) return;
	conf.dbg.scrpage = ui.leScrPage->getValue();
	conf.dbg.scrofs = ui.leScrAdr->getValue();
	draw();
}

// The address fields are inputs as well as readouts, so off the picture they
// keep what they hold - the mark on screen says where that is. Only XY, which
// is nothing but a readout, goes to dashes.
// An address the panel puts there is not the user typing it, so it must not
// come back as an edit - and a field the caret is in belongs to the typist
// and is left alone. -1 is no address at all.
static bool show_adr(xHexSpin* fld, int adr) {
	if (fld->hasFocus()) return false;
	QSignalBlocker hush(fld);
	if (adr < 0) {
		fld->setBlank();
	} else {
		fld->setValue(adr);
	}
	return true;
}

void xZXScrPanel::show_dot(int x, int y, int pix, int atr) {
	// nothing to redo for what is already on show. The addresses are in the
	// comparison too: a program flipping screens moves them under a still cursor
	int dot[4] = {x, y, pix, atr};
	if (!memcmp(dot, lastdot, sizeof(dot))) return;
	memcpy(lastdot, dot, sizeof(dot));
	bool gotpix = show_adr(ui.leScr, pix);
	bool gotatr = show_adr(ui.leAtr, atr);
	// a field left to the typist shows something else, so this dot is not on
	// show after all and must not be remembered as such
	if (!gotpix || !gotatr) lastdot[0] = -2;
	if (x < 0) {
		// nothing is pointed at and nothing is marked, so there is no dot
		ui.labScrBit->setText(XSCR_NOBIT);
		ui.labScrXY->setText(XSCR_NOXY);
		ui.labScrChar->setText(XSCR_NOXY);
		return;
	}
	ui.labScrBit->setText(QString(".%0").arg(vid_scr_bit(x)));
	// in dots, and in character cells under it: both are asked for often enough,
	// and on one line they made the column twice as wide as anything else in it
	ui.labScrXY->setText(QString("%0,%1").arg(x).arg(y));
	ui.labScrChar->setText(QString("%0,%1").arg(x >> 3).arg(y >> 3));
}

// The four value fields are held at four digits, which is what an address
// takes, so a page or an offset does not sit in a box of its own size - and
// so nothing in the column moves when a value gets shorter.
void xZXScrPanel::fit_fields() {
	if (ui.labScrXY->font() == fitFont) return;	// nothing but the font moves these
	fitFont = ui.labScrXY->font();
	QFontMetrics fm(fitFont);
	// the readout changes with every mouse move, so its column is held at the
	// widest thing it can ever say
	int wide = fm.horizontalAdvance(QString("255,191")) + 4;
	ui.labScrXY->setMinimumWidth(wide);
	ui.labScrChar->setMinimumWidth(wide);
	// the address fields size themselves from the digits they hold; the page,
	// which counts to 0xff, is padded to four so the column does not step in
	ui.leScr->refitWidth();
	ui.leAtr->refitWidth();
	int few = fm.horizontalAdvance(QString(4, '0')) + 10;
	ui.leScrPage->setFixedWidth(few);
	ui.leScrAdr->setFixedWidth(few);
	// wide enough for its longest entry, and free to fill the column beyond that
	ui.cbScrZoom->setMinimumWidth(comboFitWidth(ui.cbScrZoom));
}

void xZXScrPanel::draw() {
	Computer* comp = conf.zx;
	bool shadow = scr_has_shadow(comp);
	ui.tbScrShadow->setEnabled(shadow);
	ui.tbScrBoth->setEnabled(shadow);
	if (!shadow && ((conf.dbg.scrmode == XSCR_SHADOW) || (conf.dbg.scrmode == XSCR_BOTH))) {
		conf.dbg.scrmode = XSCR_AUTO;
		apply_mode(XSCR_AUTO);		// setChecked emits no clicked(), so no loop
	}
	bool custom = (conf.dbg.scrmode == XSCR_CUSTOM);
	ui.labScrPageCap->setVisible(custom);
	ui.leScrPage->setVisible(custom);
	ui.labScrOfsCap->setVisible(custom);
	ui.leScrAdr->setVisible(custom);
	ui.lineScrTwo->setVisible(custom);
	// ULA+ spends the flash bit on the palette group, so there is no flash
	ui.cbScrFlash->setEnabled(!comp->vid->ula->active);

	// the controls all write conf as they are touched, so that is the one
	// place the view is fed from - and a panel that is hidden cannot answer
	// with widgets left behind
	int flags = 0;
	if (conf.dbg.scrnopix) flags |= VSCR_NOPIX;
	if (conf.dbg.scrnoatr) flags |= VSCR_MONO;
	if (conf.dbg.scrnoflash) flags |= VSCR_NOFLASH;
	if (conf.dbg.scrgrid) flags |= VSCR_GRID;

	view->setView(conf.dbg.scrmode, conf.dbg.scrzoom, flags,
		conf.dbg.scrpage, conf.dbg.scrofs);
	view->redraw();

	fit_fields();
}

// DOCK

xZXScrWidget::xZXScrWidget(QString i, QString t, QWidget* p):xDockWidget(i,t,p) {
	QWidget* wid = new QWidget;
	setWidget(wid);
	setObjectName("ZXSCRWIDGET");

	QVBoxLayout* lay = new QVBoxLayout(wid);
	lay->setContentsMargins(0, 0, 0, 0);
	panel = new xZXScrPanel;
	lay->addWidget(panel);

	// what the dock holds while the window has the picture: without it the
	// tab would still be there and empty, with nothing saying why
	gone = new QWidget;
	QVBoxLayout* glay = new QVBoxLayout(gone);
	QLabel* lab = new QLabel("The screen is in a window of its own.");
	lab->setAlignment(Qt::AlignCenter);
	lab->setWordWrap(true);
	QPushButton* btn = new QPushButton("Bring it back");
	glay->addStretch(1);
	glay->addWidget(lab);
	glay->addWidget(btn, 0, Qt::AlignCenter);
	glay->addStretch(1);
	lay->addWidget(gone);
	gone->hide();

	connect(panel, &xZXScrPanel::s_detach, this, &xZXScrWidget::s_detach);
	connect(btn, &QPushButton::clicked, this, [this](){emit s_detach(false);});
}

void xZXScrWidget::setDetached(bool on) {
	panel->setVisible(!on);
	gone->setVisible(on);
	if (!on) panel->reload();
}

void xZXScrWidget::showDot(int x, int y, int pix, int atr) {
	panel->show_dot(x, y, pix, atr);
}

void xZXScrWidget::draw() {
	if (panel->isVisible())
		panel->draw();
}

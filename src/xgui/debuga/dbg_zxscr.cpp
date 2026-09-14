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
QString scr_page_name(int page) {
	switch (page) {
		case XSCR_PAGE_MAIN: return QString("Main (5)");
		case XSCR_PAGE_SHADOW: return QString("Shadow (7)");
	}
	return QString("Page %0").arg(page);
}

// Page 7 needs a core that can address past 64K, and a machine that has the
// ram for it. The core has to be asked first: a machine's own size is not
// enough, a ZX 48K is built with 128K of ram allocated behind it.
static bool scr_has_shadow(Computer* comp) {
	if (comp->hw->mask && !(comp->hw->mask & ~(MEM_128K - 1))) return false;
	return (comp->mem->ramSize >= (XSCR_PAGE_SHADOW + 1) * MEM_16K);
}

// A page is seen by the cpu at #4000 only when it is page 5. Everything else
// has to be paged into the top window first, so that is the base its addresses
// are counted from.
static int scr_page_base(int page) {
	return (page == XSCR_PAGE_MAIN) ? 0x4000 : 0xc000;
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
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setContextMenuPolicy(Qt::DefaultContextMenu);
}

QSize xZXScrView::minimumSizeHint() const {
	return QSize(XSCR_TILEW / 2, XSCR_TILEH / 2);
}

void xZXScrView::setMode(int m) {
	mode = m;
	update();
}

void xZXScrView::setCustom(int pg, int sh) {
	cpage = pg;
	cshift = sh;
	update();
}

void xZXScrView::setFlags(int f) {
	flags = f;
	update();
}

void xZXScrView::setZoom(int z) {
	if (z < XSCR_FIT) z = XSCR_FIT;
	if (z > XSCR_ZOOMMAX) z = XSCR_ZOOMMAX;
	zoom = z;
	update();
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

void xZXScrView::redraw() {
	Computer* comp = conf.zx;
	int cnt = (mode == XSCR_BOTH) ? 2 : 1;
	int shift = (mode == XSCR_CUSTOM) ? cshift : 0;
	for (int i = 0; i < cnt; i++) {
		page[i] = pageFor(i);
		vid_get_screen(comp->vid, img[i].bits(), page[i], shift, flags);
	}
	update();
}

// how much of a dot one pixel is worth, for a given screen count and direction
double xZXScrView::fitScale(int count, bool horiz) const {
	int head = (count > 1) ? (fontMetrics().height() + 2) : 0;
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
	g.head = (g.count > 1) ? (fontMetrics().height() + 2) : 0;
	// the Both view takes whichever arrangement leaves the picture bigger, so
	// a tall dock stacks the screens and a wide one puts them side by side
	bool horiz = (fitScale(g.count, true) >= fitScale(g.count, false));
	double s = fitScale(g.count, horiz);
	// a fixed zoom is exactly that; Fit snaps to whole pixels per dot as soon
	// as there is room for one, and only scales down below that
	if (zoom != XSCR_FIT) {
		s = zoom;
	} else if (s >= 1.0) {
		s = (int)s;
	}
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
	int bidx = comp->vid->nextbrd & 0x0f;
	if (comp->vid->ula->active) bidx |= 8;
	xColor bcol = vid_get_col(comp->vid, bidx);
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
		if (g.head < 1) continue;
		// the caption names the screen and says whether it is the one on
		// air: the live one gets the dock titles' own colours, the other
		// the plain ones of the style
		QRect head(t.left(), t.top() - g.head, t.width(), g.head);
		bool live = (page[i] == comp->vid->vidPage);
		pnt.fillRect(head, live ? hbg : palette().color(QPalette::Mid));
		pnt.setPen(live ? htx : palette().color(QPalette::WindowText));
		pnt.drawText(head, Qt::AlignCenter, scr_page_name(page[i]));
	}
}

// the dot under a point, as the cpu would address it
bool xZXScrView::adrAt(const QPoint& p, int* pix, int* atr) const {
	xScrGeom g = geom();
	for (int i = 0; i < g.count; i++) {
		QRect t = g.tile[i];
		if (!t.contains(p)) continue;
		if (t.width() < 1) return false;
		double s = t.width() / (double)XSCR_TILEW;
		int x = (int)((p.x() - t.left()) / s) - XSCR_BRD;
		int y = (int)((p.y() - t.top()) / s) - XSCR_BRD;
		if ((x < 0) || (x >= XSCR_W) || (y < 0) || (y >= XSCR_H)) return false;
		int base = scr_page_base(page[i]);
		if (mode == XSCR_CUSTOM) base += cshift;
		*pix = (base + (((y & 0xc0) << 5) | ((y & 0x38) << 2) | ((y & 7) << 8) | ((x & 0xf8) >> 3))) & 0xffff;
		*atr = (base + 0x1800 + (((y & 0xf8) << 2) | ((x & 0xf8) >> 3))) & 0xffff;
		return true;
	}
	return false;
}

void xZXScrView::mousePressEvent(QMouseEvent* ev) {
	int pix, atr;
	if ((ev->button() == Qt::LeftButton) && adrAt(ev->pos(), &pix, &atr)) {
		pixadr = pix;
		atradr = atr;
		emit s_adr(pix, atr);
	}
}

void xZXScrView::copyAdr(int adr) const {
	QApplication::clipboard()->setText(gethexword(adr));
}

void xZXScrView::contextMenuEvent(QContextMenuEvent* ev) {
	int pix, atr;
	// the menu works on the dot it was opened over, so the fields follow the
	// right button the same way they follow the left one
	if (adrAt(ev->pos(), &pix, &atr)) {
		pixadr = pix;
		atradr = atr;
		emit s_adr(pix, atr);
	}
	QMenu menu(this);
	QAction* apix = menu.addAction(QString("Copy pixel address"));
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

	view = new xZXScrView;
	view->setToolTip("Click a dot to read its pixel and attribute address, right click to copy either");
	ui.layScrBody->insertWidget(0, view, 10);

	ui.cbScrZoom->addItem("Zoom Fit", XSCR_FIT);
	for (int z = 1; z <= XSCR_ZOOMMAX; z++)
		ui.cbScrZoom->addItem(QString("Zoom x%0").arg(z), z);

	// QToolButton only groups siblings it was told about, so the row needs a
	// group of its own to stay exclusive
	QButtonGroup* grp = new QButtonGroup(this);
	grp->addButton(ui.tbScrAuto, XSCR_AUTO);
	grp->addButton(ui.tbScrMain, XSCR_MAIN);
	grp->addButton(ui.tbScrShadow, XSCR_SHADOW);
	grp->addButton(ui.tbScrBoth, XSCR_BOTH);
	grp->addButton(ui.tbScrCustom, XSCR_CUSTOM);
	grp->setExclusive(true);

	// The fields keep the default flags - no XHS_BGR, since a value moving is
	// not news here the way it is in the register panel these borrow their look
	// from. Their widths are pinned together in fit_fields() rather than by
	// XHS_AUTOW, so all four line up whatever each one holds.
	ui.leScr->setMax(0xffff);
	ui.leAtr->setMax(0xffff);
	ui.leScrAdr->setMax(0x3fff);
	ui.leScrPage->setBase(10);		// a page reads as a number, as it does in Shown
	ui.leScrPage->setMax(0xff);

	foreach (QAbstractButton* btn, grp->buttons())
		connect(btn, &QAbstractButton::clicked, this, &xZXScrPanel::mode_changed);
	connect(ui.cbScrZoom, SIGNAL(currentIndexChanged(int)), this, SLOT(opts_changed()));
	connect(ui.cbScrPix, &QCheckBox::toggled, this, &xZXScrPanel::opts_changed);
	connect(ui.cbScrAtr, &QCheckBox::toggled, this, &xZXScrPanel::opts_changed);
	connect(ui.cbScrFlash, &QCheckBox::toggled, this, &xZXScrPanel::opts_changed);
	connect(ui.cbScrGrid, &QCheckBox::toggled, this, &xZXScrPanel::opts_changed);
	connect(ui.leScrPage, SIGNAL(valueChanged(int)), this, SLOT(custom_changed()));
	connect(ui.leScrAdr, SIGNAL(valueChanged(int)), this, SLOT(custom_changed()));
	connect(view, &xZXScrView::s_adr, this, &xZXScrPanel::show_adr);
	connect(ui.tbScrDetach, &QToolButton::clicked, this, &xZXScrPanel::s_detach);

	reload();
}

int xZXScrPanel::mode() const {
	if (ui.tbScrMain->isChecked()) return XSCR_MAIN;
	if (ui.tbScrShadow->isChecked()) return XSCR_SHADOW;
	if (ui.tbScrBoth->isChecked()) return XSCR_BOTH;
	if (ui.tbScrCustom->isChecked()) return XSCR_CUSTOM;
	return XSCR_AUTO;
}

void xZXScrPanel::apply_mode(int m) {
	switch (m) {
		case XSCR_MAIN: ui.tbScrMain->setChecked(true); break;
		case XSCR_SHADOW: ui.tbScrShadow->setChecked(true); break;
		case XSCR_BOTH: ui.tbScrBoth->setChecked(true); break;
		case XSCR_CUSTOM: ui.tbScrCustom->setChecked(true); break;
		default: ui.tbScrAuto->setChecked(true); break;
	}
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
	conf.dbg.scrmode = mode();
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

void xZXScrPanel::custom_changed() {
	if (hold) return;
	conf.dbg.scrpage = ui.leScrPage->getValue();
	conf.dbg.scrofs = ui.leScrAdr->getValue();
	draw();
}

void xZXScrPanel::show_adr(int pix, int atr) {
	setAddress(pix, atr);
}

void xZXScrPanel::setAddress(int adr, int atr) {
	ui.leScr->setValue(adr);
	ui.leAtr->setValue(atr);
}

// Everything in the right column whose width must not follow its contents.
// The name of the screen on air changes as the machine flips between them, and
// a label that follows it drags the column, and with it the picture, about once
// a frame; so it is held at the widest name it can ever carry. The four value
// fields are held at four digits, which is what an address takes, so a page or
// an offset does not sit in a box of its own size.
void xZXScrPanel::fit_fields() {
	QFontMetrics fm = ui.labScrCur->fontMetrics();
	int wid = fm.horizontalAdvance(scr_page_name(XSCR_PAGE_MAIN));
	int one = fm.horizontalAdvance(scr_page_name(XSCR_PAGE_SHADOW));
	if (one > wid) wid = one;
	one = fm.horizontalAdvance(scr_page_name(255));	// the longest Page N
	if (one > wid) wid = one;
	wid += 6;		// slack: the metrics are not the whole of what a style draws
	if (ui.labScrCur->minimumWidth() != wid)
		ui.labScrCur->setMinimumWidth(wid);
	// the same slack xHexSpin's own XHS_AUTOW leaves for the frame and cursor
	int few = fm.horizontalAdvance(QString(4, '0')) + 10;
	if (ui.leScr->width() == few) return;
	ui.leScr->setFixedWidth(few);
	ui.leAtr->setFixedWidth(few);
	ui.leScrPage->setFixedWidth(few);
	ui.leScrAdr->setFixedWidth(few);
	// wide enough for its longest entry, and free to fill the column beyond that
	ui.cbScrZoom->setMinimumWidth(comboFitWidth(ui.cbScrZoom));
}

void xZXScrPanel::draw() {
	Computer* comp = conf.zx;
	int m = mode();
	bool shadow = scr_has_shadow(comp);
	ui.tbScrShadow->setEnabled(shadow);
	ui.tbScrBoth->setEnabled(shadow);
	if (!shadow && ((m == XSCR_SHADOW) || (m == XSCR_BOTH))) {
		hold = true;
		apply_mode(XSCR_AUTO);
		hold = false;
		m = XSCR_AUTO;
		conf.dbg.scrmode = m;
	}
	bool custom = (m == XSCR_CUSTOM);
	ui.labScrPageCap->setVisible(custom);
	ui.leScrPage->setVisible(custom);
	ui.labScrOfsCap->setVisible(custom);
	ui.leScrAdr->setVisible(custom);
	ui.lineScrTwo->setVisible(custom);
	// ULA+ spends the flash bit on the palette group, so there is no flash
	ui.cbScrFlash->setEnabled(!comp->vid->ula->active);

	int flags = 0;
	if (ui.cbScrPix->isChecked()) flags |= VSCR_NOPIX;
	if (ui.cbScrAtr->isChecked()) flags |= VSCR_MONO;
	if (ui.cbScrFlash->isChecked()) flags |= VSCR_NOFLASH;
	if (ui.cbScrGrid->isChecked()) flags |= VSCR_GRID;

	view->setMode(m);
	view->setZoom(getRFIData(ui.cbScrZoom));
	view->setFlags(flags);
	view->setCustom(ui.leScrPage->getValue(), ui.leScrAdr->getValue());
	view->redraw();

	fit_fields();
	ui.labScrCur->setText(scr_page_name(comp->vid->vidPage));
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

void xZXScrWidget::setAddress(int adr, int atr) {
	panel->setAddress(adr, atr);
}

void xZXScrWidget::draw() {
	if (panel->isVisible())
		panel->draw();
}

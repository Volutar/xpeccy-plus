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
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setContextMenuPolicy(Qt::DefaultContextMenu);
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
		int base = vid_scr_base(page[i]);
		if (mode == XSCR_CUSTOM) base += cshift;
		vid_scr_adr(base, x, y, pix, atr);
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
	grp = new QButtonGroup(this);
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
	ui.leScrPage->setMax(0xff);
	// a short value lines up with the long ones instead of floating in its box
	ui.leScr->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	ui.leAtr->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
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
	connect(view, &xZXScrView::s_adr, this, &xZXScrPanel::setAddress);
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

void xZXScrPanel::custom_changed() {
	if (hold) return;
	conf.dbg.scrpage = ui.leScrPage->getValue();
	conf.dbg.scrofs = ui.leScrAdr->getValue();
	draw();
}

void xZXScrPanel::setAddress(int adr, int atr) {
	ui.leScr->setValue(adr);
	ui.leAtr->setValue(atr);
}

// The four value fields are held at four digits, which is what an address
// takes, so a page or an offset does not sit in a box of its own size - and
// so nothing in the column moves when a value gets shorter.
void xZXScrPanel::fit_fields() {
	if (ui.leScr->font() == fitFont) return;	// nothing but the font moves these
	fitFont = ui.leScr->font();
	// the same slack xHexSpin's own XHS_AUTOW leaves for the frame and cursor
	int few = QFontMetrics(fitFont).horizontalAdvance(QString(4, '0')) + 10;
	ui.leScr->setFixedWidth(few);
	ui.leAtr->setFixedWidth(few);
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

void xZXScrWidget::setAddress(int adr, int atr) {
	panel->setAddress(adr, atr);
}

void xZXScrWidget::draw() {
	if (panel->isVisible())
		panel->draw();
}

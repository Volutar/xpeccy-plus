#include <QActionGroup>
#include <QCursor>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QSplitter>
#include <QHeaderView>
#include <QTreeWidgetItemIterator>
#include <QPushButton>
#include <QVBoxLayout>

#include "dbg_evo.h"
#include "../xgui.h"
#include "../../libxpeccy/xreplay.h"

// HELPERS

static QString hex3(int v) {return gethexword(v & 0x1ff).right(3);}

// a 22-bit byte address as the DMA registers set it: page and offset in it
static QString ts_adr(int a) {
	return gethexbyte(a >> 14) + ":" + gethexword(a & 0x3fff);
}

static QString plural(int n, const char* one, const char* many) {
	return QString("%0 %1").arg(n).arg((n == 1) ? one : many);
}

// the Means column in plain words, or in the names the docs and tsconfig.asm give the fields
bool evo_field_names() {return conf.dbg.evonames;}
static bool fieldNames() {return conf.dbg.evonames;}

QStringList evo_bit_list(int v, const evoBit* t) {
	bool nm = fieldNames();
	QStringList s;
	for (; t->mask; t++) {
		const char* w = nm ? t->field : t->words;
		if ((v & t->mask) && w) s << w;
	}
	return s;
}

// parts of a description, in words with commas, by field names with spaces
QString evo_join(const QStringList& s, const char* none) {
	bool nm = fieldNames();
	if (s.isEmpty()) return nm ? "-" : none;
	return s.join(nm ? " " : ", ");
}

QString evo_bits(int v, const evoBit* t, const char* none) {
	return evo_join(evo_bit_list(v, t), none);
}

// the CPU clocks both boards have
static const char* const evoClock[3] = {"3.5", "7", "14"};

// a color the picture shows; index 0 is left transparent over the checker
static QRgb evo_rgb(Video* vid, int idx) {
	uint32_t c = vid->pal[idx & 0xff];
	return qRgb(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff);
}

#define EVO_CHECK	8	// a checker square, in screen pixels whatever the zoom

// the window's color and a faint tint of it; made again only when that color changes
QBrush evo_checker(const QWidget* w) {
	// an image, not a brush: a brush may hold a pixmap, which must not outlive the app
	static QImage img;
	QColor bg = w->palette().color(QPalette::Window);
	if (img.isNull() || (img.pixel(EVO_CHECK, 0) != bg.rgb())) {
		img = QImage(EVO_CHECK * 2, EVO_CHECK * 2, QImage::Format_RGB32);
		img.fill(bg);
		QPainter pnt(&img);
		QColor alt = bg_tint(bg);
		pnt.fillRect(0, 0, EVO_CHECK, EVO_CHECK, alt);
		pnt.fillRect(EVO_CHECK, EVO_CHECK, EVO_CHECK, EVO_CHECK, alt);
	}
	return QBrush(img);
}

// the panel's tables: read-only, rows alternating as the dumps and the listing have them
void evo_tree_setup(QTreeWidget* t, const QStringList& head) {
	t->setColumnCount(head.size());
	t->setHeaderLabels(head);
	t->setRootIsDecorated(false);
	t->setUniformRowHeights(true);
	t->setSelectionMode(QAbstractItemView::NoSelection);
	t->setFocusPolicy(Qt::NoFocus);
	t->setAlternatingRowColors(true);
	t->header()->setStretchLastSection(true);
	t->header()->setSectionsMovable(false);
}

// A column as wide as the widest of its samples, and never narrower than it has
// been: a value that grows for a step and shrinks back must not move the table.
// A font change (the debugger hands its own out after the panel is made) starts
// the widths over.
void evo_fit_column(QTreeWidget* view, int col, QStringList samples) {
	QString fkey = QString("fitFont%0").arg(col);
	if (view->property(fkey.toUtf8().constData()).toString() != view->font().toString()) {
		view->setProperty(fkey.toUtf8().constData(), view->font().toString());
		view->setColumnWidth(col, 0);
	}
	QTreeWidgetItemIterator it(view);
	for (; *it; ++it)
		if (!(*it)->isFirstColumnSpanned()) samples << (*it)->text(col);
	QFontMetrics fm(view->font());
	// a header section has padding of its own round the text
	int w = fm.horizontalAdvance(view->headerItem()->text(col)) + 3 * fm.averageCharWidth();
	foreach (QString smp, samples)
		w = qMax(w, fm.horizontalAdvance(smp) + fm.averageCharWidth());
	if (w > view->columnWidth(col))
		view->setColumnWidth(col, w);
}

// the right click on the panel's descriptions: in words or by field names
static void evo_style_menu(const std::function<void()>& done) {
	QMenu mnu;
	QAction* words = mnu.addAction("In words");
	QAction* names = mnu.addAction("Field names");
	words->setCheckable(true);
	names->setCheckable(true);
	QActionGroup* grp = new QActionGroup(&mnu);
	grp->addAction(words);
	grp->addAction(names);
	words->setChecked(!conf.dbg.evonames);
	names->setChecked(conf.dbg.evonames);
	QAction* got = mnu.exec(QCursor::pos());
	if (!got) return;
	conf.dbg.evonames = (got == names) ? 1 : 0;
	if (done) done();
}

// LIST

xEvoList::xEvoList(QStringList head, QWidget* p):QTreeWidget(p) {
	grown = false;
	named = true;
	evo_tree_setup(this, head);
	setItemsExpandable(false);
	setIndentation(0);
	header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	header()->setToolTip("Right click: in words or by field names");
	setContextMenuPolicy(Qt::CustomContextMenu);
	header()->setContextMenuPolicy(Qt::CustomContextMenu);
	auto menu = [this](const QPoint&) {evo_style_menu(onStyle);};
	connect(this, &QWidget::customContextMenuRequested, this, menu);
	connect(header(), &QWidget::customContextMenuRequested, this, menu);
}

// group rows are colored as the debugger's panel headers are
static void evo_header_colors(QTreeWidgetItem* it, const QFont& base) {
	QFont fnt = base;
	fnt.setBold(true);
	it->setFont(0, fnt);
	QColor bg = conf.pal.value("dbg.header.bg");
	QColor fg = conf.pal.value("dbg.header.txt");
	for (int i = 0; i < it->columnCount(); i++) {
		if (bg.isValid()) it->setBackground(i, bg);
		if (fg.isValid()) it->setForeground(i, fg);
	}
}

QTreeWidgetItem* xEvoList::group(const QString& name) {
	QTreeWidgetItem* it = groups.value(name, nullptr);
	if (it) return it;
	it = new QTreeWidgetItem(this, QStringList(name));
	it->setFirstColumnSpanned(true);
	it->setExpanded(true);
	groups[name] = it;
	grown = true;
	return it;
}

void xEvoList::wordsOnly() {
	named = false;
	setContextMenuPolicy(Qt::NoContextMenu);
	header()->setContextMenuPolicy(Qt::NoContextMenu);
	header()->setToolTip(QString());
}

void xEvoList::begin() {
	chgBg = conf.pal.value(DBG_PAL_CHG_BG);
	chgFg = conf.pal.value(DBG_PAL_CHG_TXT);
	// the switch is made on any of the lists, the header follows it on each
	QString last = conf.dbg.evonames ? "Fields" : "Means";
	if (named && (columnCount() > 3) && (headerItem()->text(3) != last))
		headerItem()->setText(3, last);
}

void xEvoList::put(const QString& grp, const QString& name, const QStringList& cols) {
	QString key = grp + "/" + name;
	seen.insert(key);
	QTreeWidgetItem* it = rows.value(key, nullptr);
	bool fresh = !it;
	if (fresh) {
		it = new QTreeWidgetItem(group(grp));
		it->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
		rows[key] = it;
		grown = true;
	}
	// a value that moved since the last look is lit, as the registers are
	for (int i = 0; i < cols.size(); i++) {
		bool moved = !fresh && (it->text(i) != cols[i]);
		if (moved) it->setText(i, cols[i]);
		else if (fresh) it->setText(i, cols[i]);
		if (i != 2) continue;
		if (moved) {
			if (chgBg.isValid()) it->setBackground(i, chgBg);
			if (chgFg.isValid()) it->setForeground(i, chgFg);
		} else if (!fresh) {
			it->setData(i, Qt::BackgroundRole, QVariant());
			it->setData(i, Qt::ForegroundRole, QVariant());
		}
	}
}

bool xEvoList::finish() {
	foreach (QString key, rows.keys()) {
		if (seen.contains(key)) continue;
		delete rows.take(key);
	}
	foreach (QString key, groups.keys()) {
		if (groups[key]->childCount()) continue;
		delete groups.take(key);
	}
	seen.clear();
	bool res = grown;
	if (grown) restyle();
	grown = false;
	return res;
}

void xEvoList::restyle() {
	foreach (QTreeWidgetItem* it, groups)
		evo_header_colors(it, font());
	for (int i = 0; i < columnCount() - 1; i++)
		evo_fit_column(this, i, (i == 2) ? QStringList("000 > 000") : QStringList());
}

void xEvoList::changeEvent(QEvent* ev) {
	QTreeWidget::changeEvent(ev);
	// the debugger's font and colors come after the panel is made, and with a style change
	QEvent::Type t = ev->type();
	if ((t == QEvent::FontChange) || (t == QEvent::StyleChange) || (t == QEvent::PaletteChange)) restyle();
}

// exactly as tall as its rows, for a list that shares the page with another
void xEvoList::fitHeight() {
	int rows = 0;
	int rh = 0;
	foreach (QTreeWidgetItem* g, groups) {
		rh = rowHeight(indexFromItem(g));
		rows += 1 + g->childCount();
	}
	setFixedHeight(header()->sizeHint().height() + rows * rh + 2 * frameWidth());
}

// THE LAYER SWITCHES

static QString layersNote;

QString evo_layers_note() {
	return layersNote;
}

// A snapshot a frame is a copy of the machine every frame: not taken until the
// debugger has been shown on TSConf once, and kept on from then
void evo_arm_replay() {
	if (conf.zx && (conf.zx->hw->id == HW_TSLAB) && !xreplay_on) xreplay_arm(1);
}

// The machine's own lock keeps the emulation out while the mask changes. A
// running machine draws the next line with it; a stopped one is drawn again.
int evo_set_layers(int hide) {
	Computer* comp = conf.zx;
	if (!comp || (comp->hw->id != HW_TSLAB)) return 0;
	int res = 1;
	layersNote.clear();
	emu_lock();
	comp->vid->tsconf.hide = hide & 0x3f;
	if (conf.emu.pause) {
		res = xreplay_redraw(comp);
		xlog(XLG_GUI, XLL_INFO, "layers %.2X: %s", hide & 0x3f, xreplay_why());
		if (res) {
			frame_publish();
		} else {
			layersNote = QString("Shown from the next frame: %0.").arg(xreplay_why());
		}
	} else {
		tsl_recompose(comp->vid);
	}
	emu_unlock();
	return res;
}

// DMA

// DMACtrl's device pair, W/R:DDEV: in words, and as tsconfig.asm names it (DMA_ dropped)
static const struct {
	const char* words;
	const char* field;
} tsDmaDev[16] = {
	{nullptr, nullptr}, {"RAM > RAM", "RAM"}, {"SPI > RAM", "SPI_RAM"}, {"IDE > RAM", "IDE_RAM"},
	{"fill > RAM", "FILL"}, {nullptr, nullptr}, {"add-blit > RAM", nullptr}, {nullptr, nullptr},
	{nullptr, nullptr}, {"blit > RAM", "BLT"}, {"RAM > SPI", "RAM_SPI"}, {"RAM > IDE", "RAM_IDE"},
	{"RAM > CRAM", "RAM_CRAM"}, {"RAM > SFILE", "RAM_SFILE"}, {nullptr, nullptr}, {nullptr, nullptr}
};

static QString ts_dev(int ctrl, bool nm) {
	int dev = ((ctrl & 0x80) >> 4) | (ctrl & 7);
	const char* s = nm ? tsDmaDev[dev].field : tsDmaDev[dev].words;
	if (s) return s;
	return nm ? QString("%0:%1").arg((ctrl >> 7) & 1).arg(ctrl & 7) : "?";
}

static const evoBit dmaCtrlBits[] = {
	{0x20, "S_ALGN", "source aligned"},
	{0x10, "D_ALGN", "destination aligned"},
	{0x08, "A_SZ", "to 512"},
	{0x40, "OPT", "saturate"},
	{0, nullptr, nullptr}
};

static void dma_regs(xEvoList* l, const tsRegs& r) {
	const QString g("DMA");
	bool nm = fieldNames();
	l->put(g, "src", {"1A..1CAF", "DMASAddr", ts_adr(r.dmasrc), ""});
	l->put(g, "dst", {"1D..1FAF", "DMADAddr", ts_adr(r.dmadst), ""});
	l->put(g, "len", {"26AF", "DMALen", gethexbyte(r.dmalen), plural(r.dmalen + 1, "word a burst", "words a burst")});
	l->put(g, "num", {"28AF", "DMANum", gethexbyte(r.dmanum), plural(r.dmanum + 1, "burst", "bursts")});
	QString ctl = nm ? "-" : "nothing started yet";
	if (r.dmactrl >= 0) {
		QStringList f(nm ? "DDEV=" + ts_dev(r.dmactrl, true) : ts_dev(r.dmactrl, false));
		ctl = evo_join(f + evo_bit_list(r.dmactrl, dmaCtrlBits), "");
	}
	l->put(g, "ctrl", {"27AF", "DMACtrl", (r.dmactrl < 0) ? "-" : gethexbyte(r.dmactrl), ctl});
	l->put(g, "status", {"27AF", "DMAStatus", r.dmaact ? "80" : "00", r.dmaact ? (nm ? "DMA_ACT" : "running") : (nm ? "-" : "idle")});
	// where a running transfer is: the address registers are put back only at its end
	bool run = (r.dmaCurSrc >= 0);
	l->put(g, "nsrc", {"", "Source now", run ? ts_adr(r.dmaCurSrc) : "-", ""});
	l->put(g, "ndst", {"", "Dest now", run ? ts_adr(r.dmaCurDst) : "-", ""});
	l->put(g, "prog", {"", "Done", run ? QString("%0/%1").arg(r.dmaBurst).arg(r.dmaBursts) : "-",
		run ? QString("burst %0 of %1, word %2 of %3").arg(r.dmaBurst).arg(r.dmaBursts).arg(r.dmaWord).arg(r.dmaWords) : ""});
}

// REGISTERS PAGE

static const char* layNames[6] = {"S0", "T0", "S1", "T1", "S2", "Bitmap"};
static const char* layTips[6] = {
	"Sprites under tile layer 0", "Tile layer 0", "Sprites between the tile layers",
	"Tile layer 1", "Sprites over both tile layers", "The bitmap screen"
};

xTsRegsPage::xTsRegsPage(QWidget* p):xEvoPage(HW_TSLAB, p) {
	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	QHBoxLayout* row = new QHBoxLayout;
	row->addWidget(new QLabel("Show"));
	for (int i = 0; i < 6; i++) {
		this->lay[i] = new QCheckBox(layNames[i]);
		this->lay[i]->setToolTip(layTips[i]);
		this->lay[i]->setChecked(true);
		row->addWidget(this->lay[i]);
		connect(this->lay[i], &QCheckBox::toggled, this, &xTsRegsPage::layer_toggled);
	}
	row->addStretch(1);
	lay->addLayout(row);
	labNote = new QLabel;
	labNote->setWordWrap(true);
	labNote->hide();
	lay->addWidget(labNote);
	list = new xEvoList({"Port", "Register", "Value", "Means"});
	list->onStyle = [this]() {if (conf.zx) draw(conf.zx);};
	lay->addWidget(list, 1);
}

void xTsRegsPage::layer_toggled() {
	int hide = 0;
	for (int i = 0; i < 6; i++)
		if (!lay[i]->isChecked()) hide |= 1 << i;
	evo_set_layers(hide);
	emit s_layers();
}

static const evoBit vconfBits[] = {
	{0x20, "NOGFX", "no bitmap"},
	{0x10, "NOTSU", "no tiles or sprites"},
	{0x08, "GFXOVR", "bitmap over them"},
	{0x04, "FT_EN", "VDAC2"},
	{0, nullptr, nullptr}
};

static QString vconf_means(int v) {
	static const char* modes[4] = {"ZX", "16c", "256c", "text"};
	static const char* res[4] = {"256x192", "320x200", "320x240", "360x288"};
	QStringList s;
	if (fieldNames()) s << QString("VM=%0").arg(modes[v & 3]) << QString("RRES=%0").arg(res[(v >> 6) & 3]);
	else s << QString("%0 %1").arg(modes[v & 3]).arg(res[(v >> 6) & 3]);
	return evo_join(s + evo_bit_list(v, vconfBits), "");
}

static const evoBit tsconfBits[] = {
	{0x80, "S_EN", "sprites"},
	{0x20, "T0_EN", "tiles 0"},
	{0x04, "T0Z_EN", "tile 0 drawn on 0"},
	{0x40, "T1_EN", "tiles 1"},
	{0x08, "T1Z_EN", "tile 0 drawn on 1"},
	{0x01, "TS_EXT", "over 360x288"},
	{0, nullptr, nullptr}
};

static const evoBit memconfBits[] = {
	{0x08, "W0_RAM", "window 0 RAM"},
	{0x02, "W0_WE", "writable"},
	{0x04, "!W0_MAP", "Page0 as is"},
	{0x01, "ROM128", "ROM 48 when mapped"},
	{0, nullptr, nullptr}
};

static QString memconf_means(int v) {
	static const char* lck[4] = {"512K", "128K", "auto", "1024K"};
	static const char* lckf[4] = {"512", "128", "AUTO", "1024"};
	QStringList s(fieldNames() ? QString("LCK=%0").arg(lckf[(v >> 6) & 3]) : QString("7FFD %0").arg(lck[(v >> 6) & 3]));
	return evo_join(s + evo_bit_list(v, memconfBits), "");
}

static const evoBit cacheBits[] = {
	{1, "EN_0000", "#0000"}, {2, "EN_4000", "#4000"}, {4, "EN_8000", "#8000"}, {8, "EN_C000", "#C000"},
	{0, nullptr, nullptr}
};

static const evoBit fddBits[] = {
	{1, "VIRT_A", "A virtual"}, {2, "VIRT_B", "B virtual"}, {4, "VIRT_C", "C virtual"}, {8, "VIRT_D", "D virtual"},
	{0x80, "OPEN_VG", "ports open outside DOS"},
	{0, nullptr, nullptr}
};

static const evoBit intBits[] = {
	{1, "FRAME", "frame"}, {2, "LINE", "line"}, {4, "DMA", "DMA"},
	{0, nullptr, nullptr}
};

// a latched register: as it is, and what it becomes at the next line start
static QString pending(int now, int next, QString (*fmt)(int)) {
	QString s = fmt(now);
	if (next != now) s += " > " + fmt(next);
	return s;
}

void xTsRegsPage::draw(Computer* comp) {
	tsRegs r;
	ts_get_regs(comp, &r);
	// the switches follow the machine: another panel or a reset may have moved them
	for (int i = 0; i < 6; i++) {
		QSignalBlocker hold(lay[i]);
		lay[i]->setChecked(!(comp->vid->tsconf.hide & (1 << i)));
	}
	QString note = evo_layers_note();
	labNote->setText(note);
	labNote->setVisible(!note.isEmpty());

	bool nm = fieldNames();
	list->begin();
	QString g = "Video";
	list->put(g, "vconf", {"00AF", "VConfig", pending(r.vconf, r.latVconf, gethexbyte), vconf_means(r.latVconf)});
	list->put(g, "vpage", {"01AF", "VPage", pending(r.vpage, r.latVpage, gethexbyte), ""});
	list->put(g, "gx", {"02/03AF", "GXOffs", pending(r.gxoffs, r.latGx, hex3), QString::number(r.latGx)});
	list->put(g, "gy", {"04/05AF", "GYOffs", pending(r.gyoffs, r.latGy, hex3), QString::number(r.latGy)});
	int pal = r.latPal;
	list->put(g, "pal", {"07AF", "PalSel", pending(r.palsel, r.latPal, gethexbyte),
		QString(nm ? "GPAL=%0 T0PAL=%1 T1PAL=%2" : "bitmap %0, tiles 0 %1, tiles 1 %2").arg(pal & 15).arg((pal >> 4) & 3).arg((pal >> 6) & 3)});
	list->put(g, "brd", {"0FAF", "Border", gethexbyte(r.border), ""});

	g = "Tiles and sprites";
	list->put(g, "tsconf", {"06AF", "TSConfig", gethexbyte(r.tsconf), evo_bits(r.tsconf, tsconfBits, "all off")});
	list->put(g, "tmp", {"16AF", "TMPage", gethexbyte(r.tmpage), "tile maps"});
	list->put(g, "t0g", {"17AF", "T0GPage", pending(r.t0gpage, r.latT0g, gethexbyte), "tiles 0 graphics"});
	list->put(g, "t1g", {"18AF", "T1GPage", pending(r.t1gpage, r.latT1g, gethexbyte), "tiles 1 graphics"});
	list->put(g, "sg", {"19AF", "SGPage", gethexbyte(r.sgpage), "sprite graphics"});
	list->put(g, "t0x", {"40/41AF", "T0XOffs", pending(r.t0xoffs, r.latT0x, hex3), QString::number(r.latT0x)});
	list->put(g, "t0y", {"42/43AF", "T0YOffs", hex3(r.t0yoffs), QString::number(r.t0yoffs)});
	list->put(g, "t1x", {"44/45AF", "T1XOffs", pending(r.t1xoffs, r.latT1x, hex3), QString::number(r.latT1x)});
	list->put(g, "t1y", {"46/47AF", "T1YOffs", hex3(r.t1yoffs), QString::number(r.t1yoffs)});

	g = "Memory";
	for (int i = 0; i < 4; i++) {
		QString means;
		if (i == 0) {
			MemPage* pg = &comp->mem->map[0];
			means = QString("%0 page %1 in window 0").arg((pg->type == MEM_RAM) ? "RAM" : "ROM").arg(gethexbyte(pg->num >> 6));
		}
		list->put(g, QString("page%0").arg(i), {gethexbyte(0x10 + i) + "AF", QString("Page%0").arg(i), gethexbyte(r.page[i]), means});
	}
	list->put(g, "memconf", {"21AF", "MemConfig", gethexbyte(r.memconf), memconf_means(r.memconf)});
	// the three parts of the window (zmaps.v), written to and never read from
	auto fmpart = [&r](int ofs) {return "#" + gethexword((r.fmaddr << 12) | ofs);};
	list->put(g, "fm", {"15AF", "FMAddr", gethexbyte(r.fmaddr | (r.fmen ? 0x10 : 0)), nm ? QString("FM_EN=%0 FM_ADDR=%1").arg(r.fmen).arg(fmpart(0))
		: (r.fmen ? QString("CRAM %0, SFILE %1, regs %2").arg(fmpart(0), fmpart(0x200), fmpart(0x400)) : "off")});
	list->put(g, "cache", {"2BAF", "CacheConfig", gethexbyte(r.cacheconf), evo_bits(r.cacheconf, cacheBits, "off")});
	list->put(g, "fdd", {"29AF", "FDDVirt", gethexbyte(r.fddvirt), evo_bits(r.fddvirt, fddBits, "none virtual")});

	g = "System";
	list->put(g, "sys", {"20AF", "SysConfig", "-", nm ? QString("ZCLK=%0").arg(evoClock[r.clock])
		: QString("%0 MHz, cache %1").arg(evoClock[r.clock]).arg(r.cacheconf ? "on" : "off")});
	list->put(g, "intmask", {"2AAF", "INTMask", gethexbyte(r.intmask), evo_bits(r.intmask, intBits, "none")});
	list->put(g, "hsint", {"22AF", "HSINT", gethexbyte(r.hsint), QString("dot %0 of the line").arg(r.hsint * 2)});
	list->put(g, "vsint", {"23/24AF", "VSINT", hex3(r.vsint), r.vsinc ? QString("line %0, then %1 lines on").arg(r.vsint).arg(r.vsinc)
		: QString("line %0").arg(r.vsint)});

	dma_regs(list, r);
	list->finish();
}

// SPRITES PAGE

#define SPR_PIC		192	// the picked sprite is drawn this big at most

xTsSprPage::xTsSprPage(QWidget* p):xEvoPage(HW_TSLAB, p) {
	sel = -1;
	fitted = false;
	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	cbActive = new QCheckBox("Active only");
	cbActive->setChecked(true);
	lay->addWidget(cbActive);
	list = new QTreeWidget;
	evo_tree_setup(list, {"#", "Layer", "X", "Y", "Size", "Tile", "Pal", "Flags"});
	list->setSelectionMode(QAbstractItemView::SingleSelection);
	list->setFocusPolicy(Qt::StrongFocus);
	// the list over the picked sprite, the line between them movable
	QSplitter* split = new QSplitter(Qt::Vertical);
	split->setChildrenCollapsible(false);
	split->addWidget(list);
	QWidget* bot = new QWidget;
	QHBoxLayout* blay = new QHBoxLayout(bot);
	blay->setContentsMargins(0, 0, 0, 0);
	labPic = new QLabel;
	labPic->setMinimumSize(64, 64);
	labPic->setMaximumWidth(SPR_PIC);
	labPic->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	labPic->setAlignment(Qt::AlignCenter);
	labInfo = new QLabel;
	labInfo->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	labInfo->setWordWrap(true);
	blay->addWidget(labPic);
	blay->addWidget(labInfo, 1);
	split->addWidget(bot);
	split->setStretchFactor(0, 2);
	split->setStretchFactor(1, 1);
	lay->addWidget(split, 1);
	connect(cbActive, &QCheckBox::toggled, this, [this]() {if (conf.zx) draw(conf.zx);});
	connect(list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
		sel = it ? it->data(0, Qt::UserRole).toInt() : -1;
		draw_pick();
	});
}

void xTsSprPage::changeEvent(QEvent* ev) {
	xEvoPage::changeEvent(ev);
	if (ev->type() == QEvent::FontChange) fitted = false;
}

static const char* sprLayer[4] = {"-", "S0", "S1", "S2"};

void xTsSprPage::draw(Computer* comp) {
	Video* vid = comp->vid;
	if (list->topLevelItemCount() == 0) {
		for (int i = 0; i < TSL_SPRITES; i++) {
			QTreeWidgetItem* it = new QTreeWidgetItem(list);
			it->setData(0, Qt::UserRole, i);
			for (int c = 0; c < 7; c++)
				it->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
		}
	}
	signed char layer[TSL_SPRITES];
	tsl_sprite_layers(vid, layer);
	QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
	for (int i = 0; i < TSL_SPRITES; i++) {
		tsSprite s;
		tsl_get_sprite(vid, i, &s);
		QTreeWidgetItem* it = list->topLevelItem(i);
		QStringList fl;
		if (s.act) fl << "ACT";
		if (s.xf) fl << "XF";
		if (s.yf) fl << "YF";
		if (s.leap) fl << "LEAP";
		QStringList cols = {QString::number(i), sprLayer[layer[i] + 1], QString::number(s.x), QString::number(s.y),
			QString("%0x%1").arg(s.w).arg(s.h), QString::number(s.tile), QString::number(s.pal), fl.join(" ")};
		bool drawn = s.act && (layer[i] >= 0);
		for (int c = 0; c < cols.size(); c++) {
			it->setText(c, cols[c]);
			if (drawn) it->setData(c, Qt::ForegroundRole, QVariant());
			else it->setForeground(c, dim);
		}
		it->setHidden(cbActive->isChecked() && !s.act);
	}
	if (!fitted) {
		static const QStringList widest[7] = {{"84"}, {"Layer", "S0"}, {"511"}, {"511"}, {"64x64"}, {"4095"}, {"Pal", "15"}};
		for (int i = 0; i < 7; i++)
			evo_fit_column(list, i, widest[i]);
		fitted = true;
	}
	draw_pick();
}

// the picked sprite as the TSU draws it, through the colors as they are now;
// a transparent dot shows as a checker
void xTsSprPage::draw_pick() {
	Computer* comp = conf.zx;
	if (!comp || (comp->hw->id != HW_TSLAB) || (sel < 0)) {
		labPic->clear();
		labInfo->setText(sel < 0 ? "Pick a sprite to see it" : "");
		return;
	}
	Video* vid = comp->vid;
	tsSprite s;
	tsl_get_sprite(vid, sel, &s);
	static unsigned char dots[64 * 64];
	tsl_sprite_dots(vid, sel, dots);
	QImage img(s.w, s.h, QImage::Format_ARGB32_Premultiplied);
	for (int y = 0; y < s.h; y++) {
		QRgb* line = (QRgb*)img.scanLine(y);
		for (int x = 0; x < s.w; x++) {
			int d = dots[y * s.w + x];
			line[x] = d ? evo_rgb(vid, d) : 0;
		}
	}
	int zoom = qMax(1, qMin(qMin(labPic->width(), SPR_PIC) / s.w, qMin(labPic->height(), SPR_PIC) / s.h));
	QPixmap pic(s.w * zoom, s.h * zoom);
	QPainter pnt(&pic);
	pnt.fillRect(pic.rect(), evo_checker(labPic));
	pnt.drawImage(pic.rect(), img);
	pnt.end();
	labPic->setPixmap(pic);
	int sg = vid->tsconf.SGPage;
	labInfo->setText(QString("Sprite %0, %1x%2 at %3,%4\nTile %5: row %6, column %7 of page %8\nPalette %9 (colors %10..%11)")
		.arg(sel).arg(s.w).arg(s.h).arg(s.x).arg(s.y)
		.arg(s.tile).arg(s.tile >> 6).arg(s.tile & 63).arg(gethexbyte(sg))
		.arg(s.pal).arg(gethexbyte(s.pal << 4)).arg(gethexbyte((s.pal << 4) | 15)));
}

// FIT VIEW

xEvoFitView::xEvoFitView(QWidget* p):QWidget(p) {
	fitW = 128;
	setMinimumSize(64, 64);
	setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Expanding);
}

void xEvoFitView::setFitWidth(int w) {
	w = qMax(w, minimumWidth());
	if (w == fitW) return;
	fitW = w;
	updateGeometry();
}

QSize xEvoFitView::sizeHint() const {
	return QSize(fitW, minimumHeight());
}

// INFO PAGE

xEvoInfoPage::xEvoInfoPage(int hw, int rows, QWidget* p):xEvoPage(hw, p) {
	shown = -1;
	pic = nullptr;
	lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	row = new QHBoxLayout;
	infoBox = new QWidget;
	QVBoxLayout* ilay = new QVBoxLayout(infoBox);
	ilay->setContentsMargins(0, 0, 0, 0);
	QGridLayout* grid = new QGridLayout;
	grid->setContentsMargins(0, 0, 0, 0);
	for (int i = 0; i < rows; i++) {
		names << new QLabel;
		vals << new QLabel;
		names[i]->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		vals[i]->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		grid->addWidget(names[i], i, 0);
		grid->addWidget(vals[i], i, 1);
	}
	grid->setColumnStretch(1, 1);
	ilay->addLayout(grid);
	ilay->addStretch(1);
	row->addWidget(infoBox);
	row->addStretch(1);
	lay->addLayout(row, 1);
}

// the page's picture, at the left of the column
void xEvoInfoPage::setPicture(xEvoFitView* v) {
	pic = v;
	row->insertWidget(0, pic);
}

// the rows past nl hide; the column as wide as the widest of all and widest can be
void xEvoInfoPage::setNames(const QStringList& nl, const QStringList& all, const QStringList& widest) {
	QFontMetrics fm(font());
	int nw = 0;
	int vw = 0;
	foreach (QString n, all) nw = qMax(nw, fm.horizontalAdvance(n));
	foreach (QString v, widest) vw = qMax(vw, fm.horizontalAdvance(v));
	infoBox->setFixedWidth(nw + vw + 2 * fm.averageCharWidth());
	for (int k = 0; k < names.size(); k++) {
		names[k]->setVisible(k < nl.size());
		vals[k]->setVisible(k < nl.size());
		names[k]->setText(nl.value(k));
	}
	fit();
}

void xEvoInfoPage::setValues(const QStringList& vl) {
	for (int k = 0; k < vals.size(); k++)
		vals[k]->setText(vl.value(k, "-"));
}

// The layout has placed the picture when the page's resizeEvent runs (QLayout::widgetEvent
// comes first), so its height is the new one here and one fit a resize is enough.
void xEvoInfoPage::fit() {
	if (!pic) return;
	pic->setFitWidth(fitWidth(width() - infoBox->width() - row->spacing(), pic->height()));
}

void xEvoInfoPage::resizeEvent(QResizeEvent* ev) {
	xEvoPage::resizeEvent(ev);
	fit();
}

void xEvoInfoPage::changeEvent(QEvent* ev) {
	xEvoPage::changeEvent(ev);
	if ((ev->type() == QEvent::FontChange) && (shown >= 0)) setup(shown);
}

// MAP VIEW

xTsMapView::xTsMapView(QWidget* p):xEvoFitView(p) {
	setMouseTracking(true);
	setMinimumSize(128, 128);
}

// as big as fits, square, in whole steps when it is bigger than 1:1
int xTsMapView::sideFor(int w, int h) const {
	int side = qMin(w, h);
	if (side >= img.width()) side = (side / img.width()) * img.width();
	return side;
}

// at the top left, so the values beside it stay close
QRect xTsMapView::target() const {
	if (img.isNull()) return QRect();
	int side = sideFor(width(), height());
	return QRect(0, 0, side, side);
}

void xTsMapView::paintEvent(QPaintEvent*) {
	QPainter pnt(this);
	QRect t = target();
	if (t.isEmpty()) return;
	pnt.fillRect(t, evo_checker(this));
	pnt.drawImage(t, img);
	double k = (double)t.width() / img.width();
	QColor col = conf.pal.value("dbg.header.bg");
	if (!col.isValid()) col = Qt::yellow;
	pnt.setPen(QPen(col, 2));
	pnt.setBrush(Qt::NoBrush);
	foreach (QRect r, marks)
		pnt.drawRect(QRectF(t.x() + r.x() * k, t.y() + r.y() * k, r.width() * k, r.height() * k));
}

void xTsMapView::mouseMoveEvent(QMouseEvent* ev) {
	QRect t = target();
	if (t.isEmpty() || !t.contains(ev->pos())) {
		if (onHover) onHover(-1, -1);
		return;
	}
	double k = (double)img.width() / t.width();
	if (onHover) onHover((int)((ev->pos().x() - t.x()) * k), (int)((ev->pos().y() - t.y()) * k));
}

void xTsMapView::leaveEvent(QEvent*) {
	if (onHover) onHover(-1, -1);
}

// TILES PAGE

enum {TV_MAP0 = 0, TV_MAP1, TV_GFX};

// the rows of the value column, for a map and for a graphics page
static const QStringList tileMapNames = {"TMPage", "Cell", "Address", "Entry", "Tile", "Row", "Column", "Colors", "Flip", "Hole"};
static const QStringList tileGfxNames = {"Page", "Tile", "Row", "Column", "Address", "Line step"};

// page:address, and a color range, as the column shows them
static QString tile_adr(int a) {
	return QString("%0:%1").arg(gethexbyte(a >> 14), gethexword(a & 0x3fff));
}

static QString tile_colors(int pal) {
	return QString("%0..%1").arg(gethexbyte(pal), gethexbyte(pal | 15));
}

xTsTilePage::xTsTilePage(QWidget* p):xEvoInfoPage(HW_TSLAB, qMax(tileMapNames.size(), tileGfxNames.size()), p) {
	hx = hy = -1;
	view = new xTsMapView;
	setPicture(view);
	QHBoxLayout* top = new QHBoxLayout;
	cbView = new QComboBox;
	cbView->addItem("Map, layer 0", TV_MAP0);
	cbView->addItem("Map, layer 1", TV_MAP1);
	cbView->addItem("Graphics", TV_GFX);
	cbView->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	top->addWidget(cbView);
	top->addWidget(new QLabel("Page"));
	sbPage = new QSpinBox;
	sbPage->setDisplayIntegerBase(16);
	sbPage->setRange(0, 255);
	sbPage->setSingleStep(8);		// a graphics page is 8 pages long: 512x512 dots
	top->addWidget(sbPage);
	auto preset = [top](const char* name, const char* tip) {
		QToolButton* tb = new QToolButton;
		tb->setText(name);
		tb->setToolTip(tip);
		top->addWidget(tb);
		return tb;
	};
	tbT0G = preset("T0G", "The page T0GPage points at");
	tbT1G = preset("T1G", "The page T1GPage points at");
	tbSG = preset("SG", "The page SGPage points at");
	top->addWidget(new QLabel("Palette"));
	sbPal = new QSpinBox;
	sbPal->setRange(0, 15);
	top->addWidget(sbPal);
	top->addStretch(1);
	lay->insertLayout(0, top);
	view->img = QImage(512, 512, QImage::Format_ARGB32_Premultiplied);
	QLabel* legend = new QLabel("Checker: color 0, or tile 0 without Z_EN.\n"
		"Outlined: the part of the map the frame shows.");
	legend->setWordWrap(true);		// a narrow window still can be: the text never changes
	lay->addWidget(legend);
	view->onHover = [this](int x, int y) {
		hx = x;
		hy = y;
		hover_info();
	};
	auto redraw = [this]() {if (conf.zx && (conf.zx->hw->id == HW_TSLAB)) draw(conf.zx);};
	connect(cbView, QOverload<int>::of(&QComboBox::currentIndexChanged), this, redraw);
	connect(sbPage, QOverload<int>::of(&QSpinBox::valueChanged), this, redraw);
	connect(sbPal, QOverload<int>::of(&QSpinBox::valueChanged), this, redraw);
	auto pick = [this](int which) {
		if (!conf.zx) return;
		Video* vid = conf.zx->vid;
		sbPage->setValue(which == 0 ? vid->tsconf.T0GPage : (which == 1) ? vid->tsconf.T1GPage : vid->tsconf.SGPage);
		cbView->setCurrentIndex(TV_GFX);
	};
	connect(tbT0G, &QToolButton::clicked, this, [pick]() {pick(0);});
	connect(tbT1G, &QToolButton::clicked, this, [pick]() {pick(1);});
	connect(tbSG, &QToolButton::clicked, this, [pick]() {pick(2);});
	setup(cbView->currentData().toInt());
}

// the names of the view picked; the column as wide as both views need, so it stays put
void xTsTilePage::setup(int mode) {
	shown = mode;
	setNames((mode == TV_GFX) ? tileGfxNames : tileMapNames, tileMapNames + tileGfxNames,
		{tile_adr(0x3ffffe), "63, 63", "4095", tile_colors(0xf0), "XF YF", "yes"});
}

int xTsTilePage::fitWidth(int w, int h) {
	return view->sideFor(w, h);
}

void xTsTilePage::draw(Computer* comp) {
	Video* vid = comp->vid;
	int mode = cbView->currentData().toInt();
	bool gfx = (mode == TV_GFX);
	if (mode != shown) setup(mode);
	sbPage->setEnabled(gfx);
	sbPal->setEnabled(gfx);
	// the colors once, then each dot a lookup; 0 is a hole, left transparent
	QRgb pal[256];
	for (int i = 1; i < 256; i++)
		pal[i] = evo_rgb(vid, i);
	unsigned char dots[64];
	view->marks.clear();
	for (int ty = 0; ty < 64; ty++) {
		QRgb* line[8];
		for (int y = 0; y < 8; y++)
			line[y] = (QRgb*)view->img.scanLine((ty << 3) + y);
		for (int tx = 0; tx < 64; tx++) {
			if (gfx) {
				tsl_tile_dots(vid, sbPage->value(), (ty << 6) | tx, sbPal->value() << 4, dots);
			} else {
				tsl_map_dots(vid, mode, tsl_map_entry(vid, mode, ty, tx), dots);
			}
			for (int y = 0; y < 8; y++) {
				QRgb* dst = line[y] + (tx << 3);
				const unsigned char* src = dots + (y << 3);
				for (int x = 0; x < 8; x++)
					dst[x] = src[x] ? pal[src[x]] : 0;
			}
		}
	}
	if (!gfx) {
		// the window the offsets show, in up to four pieces where it wraps round
		int ox = (mode ? vid->tsconf.T1XOffset : vid->tsconf.T0XOffset) & 0x1ff;
		int oy = (mode ? vid->tsconf.T1YOffset : vid->tsconf.T0YOffset) & 0x1ff;
		int w = vid->tsconf.tsSize.x;
		int h = vid->tsconf.tsSize.y;
		for (int sy = 0; sy < 2; sy++) {
			for (int sx = 0; sx < 2; sx++) {
				QRect r = QRect(ox - sx * 512, oy - sy * 512, w, h).intersected(QRect(0, 0, 512, 512));
				if (!r.isEmpty()) view->marks << r;
			}
		}
	}
	view->update();
	hover_info();
}

// the cell or tile under the pointer; "-" off the picture, the page's own register stays
void xTsTilePage::hover_info() {
	Computer* comp = conf.zx;
	if (!comp || (comp->hw->id != HW_TSLAB)) return;
	Video* vid = comp->vid;
	int mode = cbView->currentData().toInt();
	bool in = (hx >= 0) && (hy >= 0);
	int tx = hx >> 3;
	int ty = hy >> 3;
	QStringList vl;
	if (mode == TV_GFX) {
		vl << gethexbyte(sbPage->value());
		if (in) {
			int tile = (ty << 6) | tx;
			int a = tsl_gfx_adr(sbPage->value(), tile, 0);
			vl << QString::number(tile) << QString::number(ty) << QString::number(tx)
				<< tile_adr(a) << "100";
		}
	} else {
		vl << gethexbyte(vid->tsconf.TMPage);
		if (in) {
			tsMapCell c;
			tsl_map_cell(vid, mode, ty, tx, &c);
			QStringList fl;
			if (c.entry & 0x4000) fl << "XF";
			if (c.entry & 0x8000) fl << "YF";
			vl << QString("%0, %1").arg(tx).arg(ty)
				<< tile_adr(c.adr)
				<< gethexword(c.entry) << QString::number(c.entry & 0xfff)
				<< QString::number((c.entry & 0xfff) >> 6) << QString::number(c.entry & 63)
				<< tile_colors(c.pal)
				<< (fl.isEmpty() ? "-" : fl.join(" ")) << (c.hole ? "yes" : "no");
		}
	}
	setValues(vl);
}

// BASECONF PORTS PAGE

xEvoPortsPage::xEvoPortsPage(QWidget* p):xEvoPage(HW_PENTEVO, p) {
	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	list = new xEvoList({"Port", "Register", "Value", "Means"});
	list->onStyle = [this]() {if (conf.zx) draw(conf.zx);};
	lay->addWidget(list, 1);
}

static const evoBit p7ffdBits[] = {
	{0x08, "SCR", nullptr}, {0x10, "ROM", nullptr}, {0x20, "LOCK", nullptr},
	{0, nullptr, nullptr}
};

static const evoBit eff7Bits[] = {
	{0x01, "P16C", "16 colors"},
	{0x04, "BLOCK1M", "7FFD as 128K"},
	{0x08, "RAM0_0", "RAM 0 at #0000"},
	{0x10, "TURBOOFF", "turbo off"},
	{0x20, "HWMC", "multicolor"},
	{0x80, "GLUCLOCK", "clock chip ports"},
	{0, nullptr, nullptr}
};

static const evoBit bfBits[] = {
	{0x01, "SHADOW_EN", "shadow ports"},
	{0x02, "ROMRW_EN", "ROM writable"},
	{0x04, "FNTW_EN", "font writable"},
	{0x08, "SET_NMI", "NMI"},
	{0x10, "BRK_ENA", "trap on"},
	{0x20, "PAL444_ENA", "4-4-4 palette"},
	{0, nullptr, nullptr}
};

void xEvoPortsPage::draw(Computer* comp) {
	evoRegs r;
	evo_get_regs(comp, &r);
	bool nm = fieldNames();
	list->begin();

	QString g = "Ports";
	list->put(g, "7ffd", {"7FFD", "7FFD", gethexbyte(r.p7ffd), nm
		? QString("PG=%0 %1").arg(r.p7ffd & 7).arg(evo_bit_list(r.p7ffd, p7ffdBits).join(" "))
		: QString("RAM page %0 at #C000, screen %1, ROM %2%3").arg(r.p7ffd & 7).arg((r.p7ffd & 8) ? 7 : 5)
			.arg((r.p7ffd >> 4) & 1).arg((r.p7ffd & 0x20) ? ", locked" : "")});
	list->put(g, "eff7", {"EFF7", "EFF7", gethexbyte(r.peff7), evo_bits(r.peff7, eff7Bits, "-")});
	static const char* atmModes[8] = {"EGA", "?", "hardware multicolor", "ZX", "?", "?", "text", "Evo text"};
	bool pager = r.p77 & 0x20;
	list->put(g, "77", {"xx77", "77", gethexbyte(r.p77), nm
		? QString("RG=%0%1 PEN=%2 CPM_N=%3 PEN2=%4").arg(r.p77 & 7).arg((r.p77 & 8) ? " TURBO" : "")
			.arg(pager ? 0 : 1).arg((r.p77 >> 6) & 1).arg((r.p77 & 0x80) ? 0 : 1)
		: QString("mode %0 (%1)%2, pager %3%4, palette %5").arg(r.p77 & 7).arg(atmModes[r.p77 & 7])
			.arg((r.p77 & 8) ? ", turbo" : "").arg(pager ? "on" : "off").arg((r.p77 & 0x40) ? "" : ", DOS held on")
			.arg((r.p77 & 0x80) ? "closed" : "open")});
	list->put(g, "bf", {"xxBF", "BF", gethexbyte(r.pbf), evo_bits(r.pbf, bfBits, "-")});

	g = "Windows";
	for (int i = 0; i < 4; i++) {
		MemPage* pg = &comp->mem->map[i << 6];
		list->put(g, QString("w%0").arg(i), {"", "#" + gethexword(i << 14), gethexbyte(pg->num >> 6),
			QString("%0 page %1").arg((pg->type == MEM_RAM) ? "RAM" : "ROM").arg(gethexbyte(pg->num >> 6))});
	}

	g = "Pager";
	for (int i = 0; i < 8; i++) {
		int f = r.pgflag[i];
		QStringList m;
		m << ((f & 0x40) ? "RAM" : "ROM");
		if (f & 0x80) m << (nm ? "MIX" : (f & 0x40) ? "7FFD bits mixed in" : "DOS bit mixed in");
		if (f & 0x20) m << (nm ? "WP" : "write protected");
		bool live = ((i & 4) == r.pgset);
		list->put(g, QString("p%0").arg(i), {QString("%0FF7").arg((i & 3) << 2, 1, 16).toUpper(),
			QString("W%0 ROM=%1%2").arg(i & 3).arg(i >> 2).arg(live ? " *" : ""), gethexbyte(r.pg[i]), m.join(nm ? " " : ", ")});
	}

	g = "System";
	QStringList st;
	if (r.dos) st << "DOS";
	if (r.vdos) st << (nm ? "VDOS" : "TR-DOS emulation");
	if (r.vnmi) st << (nm ? "VNMI" : "in NMI");
	list->put(g, "state", {"", "State", "", st.isEmpty() ? "-" : st.join(", ")});
	list->put(g, "clock", {"", "Clock", "", QString("%0 MHz").arg(evoClock[r.clock])});
	list->put(g, "trap", {"00/01BD", "Trap", gethexword(r.trap), (r.pbf & 0x10) ? "on" : "off"});
	list->put(g, "brd", {"", "Border", QString::number(r.border, 16).toUpper(), "palette cell"});
	QStringList vd;
	for (int i = 0; i < 4; i++)
		if (r.virt & (1 << i)) vd << QString(QChar('A' + i));
	list->put(g, "virt", {"13BD", "Virtual", QString::number(r.virt, 16).toUpper(), vd.isEmpty() ? "-" : vd.join(" ")});
	list->put(g, "ext", {"2F..8F", "2F 4F 6F 8F", "", QString("%0 %1 %2 %3").arg(gethexbyte(r.ext[0])).arg(gethexbyte(r.ext[1]))
		.arg(gethexbyte(r.ext[2])).arg(gethexbyte(r.ext[3]))});
	list->finish();
}

// PALETTE VIEW

#define PAL_SIDE	16	// cells are as big as on a grid this many a side
#define PAL_CELL	32	// and no bigger
// The cell size takes room for the names and PAL_TAGS tag columns whether a machine has
// them or not, so both machines get the same cells in the same dock.
#define PAL_TAGS	4

xEvoPalView::xEvoPalView(QWidget* p):xEvoFitView(p) {
	cols = rows = side = 0;
	border = pick = -1;
	setMouseTracking(true);
}

int xEvoPalView::tagColumn() const {
	QFontMetrics fm(font());
	return fm.horizontalAdvance("T0") + fm.averageCharWidth() / 2 + 1;
}

QRect xEvoPalView::gridIn(int w, int h) const {
	if (!cols || !rows) return QRect();
	QFontMetrics fm(font());
	int lw = fm.horizontalAdvance("00") + fm.averageCharWidth();
	int tw = fm.averageCharWidth() + tagColumn() * PAL_TAGS;
	int top = colNames.isEmpty() ? 0 : fm.height();
	int c = qMin(qMin((w - lw - tw) / qMax(cols, PAL_SIDE), (h - top) / qMax(rows, PAL_SIDE)), PAL_CELL);
	if (c < 4) return QRect();
	return QRect(lw, top, c * cols, c * rows);
}

// the cells as big as the page's last fit made them: the view is narrowed to them then,
// and the tag room it takes without tags would make them smaller still
QRect xEvoPalView::grid() const {
	if (!side || !cols || !rows) return gridIn(width(), height());
	QFontMetrics fm(font());
	return QRect(fm.horizontalAdvance("00") + fm.averageCharWidth(), colNames.isEmpty() ? 0 : fm.height(), side * cols, side * rows);
}

// as wide as the grid, its names and tags need in w x h, so what is beside it stays close
int xEvoPalView::widthFor(int w, int h) {
	QRect g = gridIn(w, h);
	side = g.isEmpty() ? 0 : g.width() / cols;
	if (g.isEmpty()) return w;
	int tw = tagNames.isEmpty() ? 0 : QFontMetrics(font()).averageCharWidth() + tagColumn() * tagNames.size();
	return qMin(w, g.right() + 1 + tw);
}

int xEvoPalView::at(QPoint pos) const {
	QRect g = grid();
	if (g.isEmpty() || !g.contains(pos)) return -1;
	int c = g.width() / cols;
	int idx = ((pos.y() - g.y()) / c) * cols + (pos.x() - g.x()) / c;
	return (idx < cells.size()) ? idx : -1;
}

void xEvoPalView::paintEvent(QPaintEvent*) {
	QPainter pnt(this);
	QRect g = grid();
	if (g.isEmpty()) return;
	int c = g.width() / cols;
	auto cell = [&](int i) {return QRect(g.x() + (i % cols) * c, g.y() + (i / cols) * c, c, c);};
	QFontMetrics fm(font());
	int gap = fm.averageCharWidth();
	int tc = tagColumn();
	int tx = g.right() + 1 + gap;
	pnt.setPen(palette().color(QPalette::WindowText));
	for (int i = 0; i < colNames.size(); i++)
		pnt.drawText(QRect(g.x() + i * c, 0, c, g.y()), Qt::AlignCenter, colNames[i]);
	for (int t = 0; t < tagNames.size(); t++)
		pnt.drawText(QRect(tx + t * tc, 0, tc, g.y()), Qt::AlignCenter, tagNames[t]);
	// a name every other row when the rows are too low for each
	int every = (c < fm.height()) ? 2 : 1;
	for (int r = 0; r < rows; r++) {
		int y = g.y() + r * c;
		if ((r < rowNames.size()) && !(r % every))
			pnt.drawText(QRect(0, y - c, g.x() - gap, 3 * c), Qt::AlignRight | Qt::AlignVCenter, rowNames[r]);
		for (int t = 0; t < tagNames.size(); t++)
			if (rowTags.value(r) & (1 << t))
				pnt.drawText(QRect(tx + t * tc, y, tc, c), Qt::AlignCenter, tagNames[t]);
	}
	for (int i = 0; i < cells.size(); i++)
		pnt.fillRect(cell(i).adjusted(0, 0, -1, -1), QColor(cells[i]));
	pnt.setBrush(Qt::NoBrush);
	if ((border >= 0) && (border < cells.size())) {
		QColor col = conf.pal.value("dbg.header.bg");
		if (!col.isValid()) col = Qt::yellow;
		pnt.setPen(QPen(col, 2, Qt::DashLine));
		pnt.drawRect(cell(border).adjusted(1, 1, -2, -2));
	}
	if ((pick >= 0) && (pick < cells.size())) {
		QRect rc = cell(pick).adjusted(0, 0, -1, -1);
		pnt.setPen(QPen(Qt::white, 1));
		pnt.drawRect(rc.adjusted(0, 0, -1, -1));
		pnt.setPen(QPen(Qt::black, 1));
		pnt.drawRect(rc.adjusted(1, 1, -2, -2));
	}
}

void xEvoPalView::mouseMoveEvent(QMouseEvent* ev) {
	if (onHover) onHover(at(ev->pos()));
}

void xEvoPalView::mousePressEvent(QMouseEvent* ev) {
	if (ev->button() != Qt::LeftButton) return;
	int idx = at(ev->pos());
	if ((idx >= 0) && onPick) onPick(idx);
}

void xEvoPalView::leaveEvent(QEvent*) {
	if (onHover) onHover(-1);
}

// PALETTE PAGE

// the grid, and beside it what the cell under the pointer or picked is
xEvoPalPage::xEvoPalPage(QWidget* p):xEvoInfoPage(-1, 9, p) {		// 9: the TSConf names, the longer list
	hov = -1;
	view = new xEvoPalView;
	setPicture(view);
	labLegend = new QLabel;
	labLegend->setWordWrap(true);
	lay->addWidget(labLegend);
	view->onHover = [this](int idx) {
		hov = idx;
		info();
	};
	view->onPick = [this](int idx) {
		view->pick = idx;
		view->update();
		info();
	};
}

// the tag columns on TSConf, a bit each in rowTags
static const QStringList palTags = {"B", "T0", "T1", "S"};
enum {PAL_B = 1, PAL_T0 = 2, PAL_T1 = 4, PAL_S = 8};

// the names, the legend and the room for the values: once a machine, and again with a font
void xEvoPalPage::setup(int hw) {
	shown = hw;
	bool ts = (hw == HW_TSLAB);
	QStringList nl;
	if (ts) {
		nl << "Index" << "CRAM" << "Word" << "R" << "G" << "B" << "VDAC_MODE" << "RGB" << "Used by";
	} else {
		nl << "Index" << "#FF" << "R" << "G" << "B" << "RGB" << "Used by";
	}
	view->rowNames.clear();
	view->colNames.clear();
	for (int i = 0; i < 16; i++)
		view->colNames << QString::number(i, 16).toUpper();
	if (ts) {
		view->cols = view->rows = 16;
		for (int i = 0; i < 16; i++)
			view->rowNames << gethexbyte(i << 4);
		view->tagNames = palTags;
	} else {
		view->cols = 16;
		view->rows = 1;
		view->rowNames << "0";
		view->tagNames.clear();
	}
	view->pick = -1;
	labLegend->setText(ts ? "B, T0, T1, S: the rows the bitmap, tile layers and sprites use now. Dashed: the border. "
		"With VDAC_MODE 0 (PWM) a level is full from 24." : "Dashed: the border.");
	setNames(nl, nl, {"#FFFFFF", "border"});		// after the grid's sizes: it fits
}

void xEvoPalPage::draw(Computer* comp) {
	Video* vid = comp->vid;
	int hw = comp->hw->id;
	if (hw != shown) setup(hw);
	view->cells.clear();
	view->rowTags.clear();
	if (hw == HW_TSLAB) {
		tsRegs r;
		ts_get_regs(comp, &r);
		for (int i = 0; i < 256; i++)
			view->cells << evo_rgb(vid, i);
		// the rows the layers take their colors from now
		int use[16] = {0};
		int mode = r.vconf & 3;
		int gpal = r.palsel & 15;
		if (!(r.vconf & 0x20)) {			// NOGFX
			if (mode == 2) {
				for (int i = 0; i < 16; i++) use[i] |= PAL_B;	// 256 colors: all of them
			} else {
				use[gpal] |= PAL_B;
			}
		}
		for (int k = 0; k < 4; k++) {
			if (r.tsconf & 0x20) use[(((r.palsel >> 4) & 3) << 2) | k] |= PAL_T0;
			if (r.tsconf & 0x40) use[(((r.palsel >> 6) & 3) << 2) | k] |= PAL_T1;
		}
		if (r.tsconf & 0x80) {
			signed char layer[TSL_SPRITES];
			tsl_sprite_layers(vid, layer);
			for (int i = 0; i < TSL_SPRITES; i++) {
				tsSprite s;
				tsl_get_sprite(vid, i, &s);
				if (s.act && (layer[i] >= 0)) use[s.pal & 15] |= PAL_S;
			}
		}
		// text mode puts out the low nibble only, under GPAL: tiles and sprites as well
		if (mode == 3) {
			int all = 0;
			for (int i = 0; i < 16; i++) {
				all |= use[i];
				use[i] = 0;
			}
			use[gpal] = all;
		}
		for (int i = 0; i < 16; i++)
			view->rowTags << use[i];
		view->border = (mode == 3) ? ((gpal << 4) | (r.border & 15)) : (r.border & 0xff);
	} else if (hw == HW_PENTEVO) {
		for (int i = 0; i < 16; i++)
			view->cells << evo_rgb(vid, i);
		view->border = vid->nextbrd & 15;
	}
	view->update();
	info();
}

int xEvoPalPage::fitWidth(int w, int h) {
	return view->widthFor(w, h);
}

// the cell under the pointer or picked, by the names the docs give the fields
void xEvoPalPage::info() {
	Computer* comp = conf.zx;
	if (!comp) return;
	QStringList vl;
	int i = (hov >= 0) ? hov : view->pick;
	if ((i >= 0) && (i < view->cells.size())) {
		QRgb c = evo_rgb(comp->vid, i);
		QString rgb = "#" + gethexbyte(qRed(c)) + gethexbyte(qGreen(c)) + gethexbyte(qBlue(c));
		QStringList use;
		for (int t = 0; t < view->tagNames.size(); t++)
			if (view->rowTags.value(i / view->cols) & (1 << t)) use << view->tagNames[t];
		if (i == view->border) use << "border";
		if (comp->hw->id == HW_TSLAB) {
			unsigned char* cram = comp->vid->tsconf.cram;
			int w = cram[i << 1] | (cram[(i << 1) + 1] << 8);
			vl << gethexbyte(i) << hex3(i << 1) << gethexword(w)
				<< QString::number((w >> 10) & 31) << QString::number((w >> 5) & 31) << QString::number(w & 31)
				<< QString::number((w >> 15) & 1);
		} else {
			evoRegs r;
			evo_get_regs(comp, &r);
			// the levels are n * 0x11: atm3clev in pentevo.c
			vl << QString::number(i, 16).toUpper() << gethexbyte(r.pal[i])
				<< QString::number(qRed(c) / 17) << QString::number(qGreen(c) / 17) << QString::number(qBlue(c) / 17);
		}
		vl << rgb << (use.isEmpty() ? "-" : use.join("\n"));
	}
	setValues(vl);
}

// DMA PAGE

xTsDmaPage::xTsDmaPage(QWidget* p):xEvoPage(HW_TSLAB, p) {
	logCount = ~0u;
	logFrame = -1;
	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	regs = new xEvoList({"Port", "Register", "Value", "Means"});
	regs->onStyle = [this]() {if (conf.zx) draw(conf.zx);};
	lay->addWidget(regs, 0);
	lay->addWidget(new QLabel("Started this frame and the last"));
	log = new QTreeWidget;
	evo_tree_setup(log, {"Frame", "Line:dot", "Transfer", "From", "To", "Size", "Took"});
	lay->addWidget(log, 1);
}

void xTsDmaPage::draw(Computer* comp) {
	tsRegs r;
	ts_get_regs(comp, &r);
	regs->begin();
	dma_regs(regs, r);
	if (regs->finish()) regs->fitHeight();
	// the list only when it can have changed: a transfer started or ended, or a frame went
	unsigned cnt = comp->tsconf.dmalog.count;
	tsDmaRec* last = cnt ? &comp->tsconf.dmalog.rec[(cnt - 1) % TS_DMALOG] : nullptr;
	if ((cnt == logCount) && (comp->vid->fcnt == logFrame) && !(last && (last->dur < 0))) return;
	logCount = cnt;
	logFrame = comp->vid->fcnt;
	log->clear();
	// T to lines: the line is 448 dots whatever the cpu clock is
	double tPerLine = comp->vid->full.x * (double)comp->vid->nsPerDotFixed / (double)comp->nsPerTickFixed;
	unsigned first = (cnt > TS_DMALOG) ? cnt - TS_DMALOG : 0;
	for (unsigned k = first; k < cnt; k++) {
		tsDmaRec* d = &comp->tsconf.dmalog.rec[k % TS_DMALOG];
		int age = comp->vid->fcnt - d->fcnt;
		if (age > 1) continue;
		QString took = (d->dur < 0) ? "running" : QString("%0 T, %1 lines%2").arg(d->dur)
			.arg(d->dur / tPerLine, 0, 'f', 1).arg(d->cut ? ", cut short" : "");
		QTreeWidgetItem* it = new QTreeWidgetItem(log, {age ? "last" : "this", QString("%0:%1").arg(d->line).arg(d->dot),
			ts_dev(d->ctrl, false), ts_adr(d->src), ts_adr(d->dst), QString("%0 x %1").arg(d->len).arg(d->num), took});
		it->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
	}
	static const QStringList widest[6] = {{"Frame", "last"}, {"Line:dot", "319:447"}, {"add-blit > RAM"},
		{"FF:3FFE"}, {"FF:3FFE"}, {"256 x 256"}};
	for (int i = 0; i < 6; i++)
		evo_fit_column(log, i, widest[i]);
	log->scrollToBottom();
}

// PANEL

// the tab each panel shows, and whether it is in a window of its own
static int& panel_tab(int kind) {
	return (kind == NEXT_PANEL) ? conf.dbg.nxtab : conf.dbg.evotab;
}

static bool panel_detached(int kind) {
	return (kind == NEXT_PANEL) ? conf.dbg.nxdetach : conf.dbg.evodetach;
}

xEvoPanel::xEvoPanel(int kind, QWidget* p):QWidget(p), kind(kind) {
	hw = -1;
	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	QHBoxLayout* head = new QHBoxLayout;
	head->setContentsMargins(0, 0, 0, 0);
	tabs = new QTabBar;
	tabs->setObjectName((kind == NEXT_PANEL) ? "nextTabs" : "evoTabs");
	tabs->setDrawBase(false);
	labHidden = new QLabel("Layers hidden");
	labHidden->setToolTip("Some of the layers are kept out of the picture (Registers tab)");
	labHidden->hide();
	tbDetach = new QToolButton;
	tbDetach->setIcon(QIcon(":/images/display.png"));
	tbDetach->setToolTip("Show in its own window");
	tbDetach->setCheckable(true);
	head->addWidget(tabs);
	head->addStretch(1);
	head->addWidget(labHidden);
	head->addWidget(tbDetach);
	lay->addLayout(head);

	stack = new QStackedWidget;
	xTsRegsPage* tsRegs = nullptr;
	if (kind == NEXT_PANEL) {
		pages = next_panel_pages(&names);
	} else {
		tsRegs = new xTsRegsPage;
		pages << tsRegs << new xTsSprPage << new xTsTilePage << new xEvoPalPage << new xTsDmaPage << new xEvoPortsPage;
		names << "Registers" << "Sprites" << "Tiles" << "Palette" << "DMA" << "Ports";
	}
	foreach (xEvoPage* pg, pages)
		stack->addWidget(pg);
	noMachine = new QLabel("Nothing to show on this machine");
	noMachine->setAlignment(Qt::AlignCenter);
	stack->addWidget(noMachine);
	lay->addWidget(stack, 1);

	connect(tabs, &QTabBar::currentChanged, this, &xEvoPanel::tab_changed);
	connect(tbDetach, &QToolButton::clicked, this, &xEvoPanel::s_detach);
	if (tsRegs) connect(tsRegs, &xTsRegsPage::s_layers, this, [this]() {
		draw();
		emit s_picture();
	});
}

// the tabs each machine has, by their place in pages[]
static const QList<int> tsTabs = {0, 1, 2, 3, 4};
static const QList<int> evoTabs = {5, 3};

void xEvoPanel::build_tabs() {
	QSignalBlocker hold(tabs);
	while (tabs->count() > 0)
		tabs->removeTab(0);
	tabPages.clear();
	hw = conf.zx ? conf.zx->hw->id : -1;
	QList<int> list;
	if (kind == NEXT_PANEL) {
		if (hw == HW_ZXNEXT)
			for (int k = 0; k < pages.size(); k++) list << k;
	} else {
		list = (hw == HW_TSLAB) ? tsTabs : (hw == HW_PENTEVO) ? evoTabs : QList<int>();
	}
	foreach (int k, list) {
		tabPages << pages[k];
		tabs->addTab(names[k]);
	}
	tabs->setVisible(tabs->count() > 0);
	int idx = qBound(0, panel_tab(kind), qMax(0, tabs->count() - 1));
	tabs->setCurrentIndex(idx);
	stack->setCurrentWidget(tabPages.isEmpty() ? (QWidget*)noMachine : tabPages[idx]);
	// a switch to TSConf with the panel up: xm_set() disarmed the snapshots
	if (isVisible()) evo_arm_replay();
}

void xEvoPanel::tab_changed(int idx) {
	if ((idx < 0) || (idx >= tabPages.size())) return;
	panel_tab(kind) = idx;
	stack->setCurrentWidget(tabPages[idx]);
	draw();
}

void xEvoPanel::reload() {
	tbDetach->setChecked(panel_detached(kind));
	build_tabs();
	restyle();
	draw();
}

// the header colors may be new: Options applied
void xEvoPanel::restyle() {
	foreach (QTreeWidget* t, findChildren<QTreeWidget*>()) {
		xEvoList* l = dynamic_cast<xEvoList*>(t);
		if (l) l->restyle();
	}
}

void xEvoPanel::draw() {
	Computer* comp = conf.zx;
	if (!comp) return;
	if (comp->hw->id != hw)
		build_tabs();
	labHidden->setVisible((hw == HW_TSLAB) && (comp->vid->tsconf.hide != 0));
	xEvoPage* pg = qobject_cast<xEvoPage*>(stack->currentWidget());
	if (pg && ((pg->hw < 0) || (pg->hw == hw))) pg->draw(comp);
}

// DOCK

xEvoWidget::xEvoWidget(int kind, QString i, QString t, QWidget* p):xDockWidget(i,t,p) {
	QWidget* wid = new QWidget;
	setWidget(wid);
	// the name a saved dock layout knows it by
	setObjectName((kind == NEXT_PANEL) ? "NEXTWIDGET" : "EVOWIDGET");

	QVBoxLayout* lay = new QVBoxLayout(wid);
	lay->setContentsMargins(5, 5, 5, 5);
	panel = new xEvoPanel(kind);
	lay->addWidget(panel);

	gone = new QWidget;
	QVBoxLayout* glay = new QVBoxLayout(gone);
	QLabel* lab = new QLabel(QString("The %0 panel is in a window of its own.").arg((kind == NEXT_PANEL) ? "ZX Next" : "ZX Evolution"));
	lab->setAlignment(Qt::AlignCenter);
	lab->setWordWrap(true);
	QPushButton* btn = new QPushButton("Bring it back");
	glay->addStretch(1);
	glay->addWidget(lab);
	glay->addWidget(btn, 0, Qt::AlignCenter);
	glay->addStretch(1);
	lay->addWidget(gone);
	gone->hide();

	connect(panel, &xEvoPanel::s_detach, this, &xEvoWidget::s_detach);
	connect(panel, &xEvoPanel::s_picture, this, &xEvoWidget::s_picture);
	connect(btn, &QPushButton::clicked, this, [this](){emit s_detach(false);});
}

void xEvoWidget::setDetached(bool on) {
	panel->setVisible(!on);
	gone->setVisible(on);
	if (!on) panel->reload();
}

void xEvoWidget::draw() {
	if (panel->isVisible())
		panel->draw();
}

void xEvoWidget::restyle() {
	panel->restyle();
}

#include "dbg_dump.h"
#include "../../xcore/xcore.h"
#include "../../libxpeccy/filetypes/filetypes.h"

#include <QDebug>

#include <QTextCodec>
#include <QColor>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QFileInfo>
#include <QPainter>
#include <utility>

#include "filer.h"
#include "xcore/arcpath.h"

extern int blockStart;
extern int blockEnd;
extern int blockAnchor;

// #define SECTSIZE 25

// MODEL

// CP437, the IBM PC's own, from 128 up: Qt has no codec for it
static const unsigned short cp437_hi[128] = {
	0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
	0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
	0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
	0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
	0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
	0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
	0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
	0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0
};

// the Latin one first, then the Cyrillic ones; the names as code pages are written
void dump_codepage_items(QComboBox* box) {
	box->addItem("CP437", XCP_437);
	box->addItem("CP1251", XCP_1251);
	box->addItem("CP866", XCP_866);
	box->addItem("KOI8-R", XCP_KOI8R);
	box->setToolTip("Code page of the text column");
}

QString getDumpString(QByteArray bts, int cp) {
	QString res;
	QTextCodec* codec = NULL;
	if (cp == XCP_437) {
		for (int i = 0; i < bts.size(); i++) {
			unsigned char bte = (unsigned char)bts.at(i);
			if (bte > 127) {
				res.append(QChar(cp437_hi[bte - 128]));
			} else {
				res.append(((bte > 31) && (bte != 127)) ? QChar(bte) : QChar('.'));
			}
		}
		return res;
	}
	switch(cp) {
		case XCP_1251: codec = QTextCodec::codecForName("CP1251"); break;
		case XCP_866: codec = QTextCodec::codecForName("IBM866"); break;
		case XCP_KOI8R: codec = QTextCodec::codecForName("KOI8R"); break;
	}
	unsigned char bte;
	for (int i = 0; i < bts.size(); i++) {
		bte = (unsigned char)bts.at(i);
		if ((bte > 31) && (bte != 127)) {		// 127 is a control code in all of them
			res.append(QChar(bte));
		} else {
			res.append(".");
		}
	}
	if (codec == NULL) return res;
	QByteArray arr = res.toLatin1();
	res = codec->toUnicode(arr);
	return res;
}

QString dump_page_token(int area, int page) {
	char buf[16];
	xCell cell = {area, page, 0, 0};
	return (area == CELL_RAM) ? QString("RAM%0").arg(page) : QString(cell_token(cell, buf));
}

// the area a page view shows: its pages are the ones MEMMAP, labels and
// breakpoints name (8K RAM pages on the Next), CELL_NONE for the CPU view
static int dump_view_area(int mode) {
	switch (mode) {
		case XVIEW_RAM: return CELL_RAM;
		case XVIEW_ROM: return CELL_ROM;
		case XVIEW_DIV: return CELL_DIV;
	}
	return CELL_NONE;
}

// the offset in a page of a view's address
static int dump_page_ofs(Computer* comp, int area, int adr) {
	return adr & (comp_cell_unit(comp, area) - 1);
}

// a byte of a page and its breakpoint flags, NULL: no such page
static unsigned char* dump_cell(Computer* comp, int area, int page, int adr, unsigned char** flag) {
	int type, abs;
	if (!comp_cell_xadr(comp, area, page, dump_page_ofs(comp, area, adr), &type, &abs)) return NULL;
	switch (type) {
		case MEM_RAM:
			abs &= comp->mem->ramMask;
			*flag = comp->brkRamMap + abs;
			return comp->mem->ramData + abs;
		case MEM_ROM:
			abs &= comp->mem->romMask;
			*flag = comp->brkRomMap + abs;
			return comp->mem->romData + abs;
		case MEM_DIV:
			if (!comp->div) return NULL;
			*flag = comp->div->brkMap + abs;
			return comp->div->data + abs;
	}
	return NULL;
}

int xDumpModel::mrd(int adr) const {
	Computer* comp = conf.zx;
	int res = 0xff;
	if (mode == XVIEW_CPU) {
		res = comp_peek(comp, adr % maxadr, MEM_VIEW_READ, NULL);
		res |= getBrk(comp, adr % maxadr) << 8;
	} else {
		unsigned char* flag;
		unsigned char* cell = dump_cell(comp, dump_view_area(mode), page, adr, &flag);
		if (cell) res = *cell | (*flag << 8);
	}
	return res;
}

// A byte's tip: its address as the address column writes it, the label there,
// and the word read on from it - into the next page in a page view
QString xDumpModel::cellTip(int adr, const QString& note) const {
	if (mode == XVIEW_CPU) return dbg_cpu_byte_tip(adr, note);
	Computer* comp = conf.zx;
	int lo = mrd(adr) & 0xff;
	int word = -1;
	QString lab;
	int area = dump_view_area(mode);
	int ofs = dump_page_ofs(comp, area, adr);
	int shown = shownAdr(adr);
	xAdr xa;
	xa.bank = -1;
	xa.adr = shown;		// find_label takes a CPU address label only if this cell is there now
	if (comp_cell_xadr(comp, area, page, ofs, &xa.type, &xa.abs))
		lab = find_label(xa);
	int hi = comp_cell_peek(comp, area, page, ofs + 1);
	if (hi >= 0) word = lo | ((hi & 0xff) << 8);
	return dbg_byte_tip(lo, word, gethexbyte(page).append(":").append(dbg_word(shown)), lab, note);
}

void xDumpModel::mwr(int adr, unsigned char bt) {
	Computer* comp = conf.zx;
	rzx_rec_touch();
	if (mode == XVIEW_CPU) {
		comp_poke(comp, adr, bt, conf.dbg.romwr);
	} else {
		int area = dump_view_area(mode);
		comp_cell_poke(comp, area, page, dump_page_ofs(comp, area, adr), bt, conf.dbg.romwr);	// ROM: with Allow ROM wr only
	}
}

// the selection the view on show has: the CPU view's is the listing's too
bool xDumpModel::selRange(int* start, int* end) const {
	if (mode == XVIEW_CPU) {
		if (blockStart < 0) return false;
		*start = blockStart;
		*end = blockEnd;
		return true;
	}
	if ((selStart < 0) || (selMode != mode) || (selPage != page)) return false;
	*start = selStart;
	*end = selEnd;
	return true;
}

bool xDumpModel::isSel(int adr) const {
	int s, e;
	return selRange(&s, &e) && (adr >= s) && (adr <= e);
}

QByteArray xDumpModel::bytes(int start, int end) const {
	QByteArray res;
	for (int adr = start; adr <= end; adr++)
		res.append((char)(mrd(adr) & 0xff));
	return res;
}

int xDumpModel::shownAdr(int adr) const {
	if (mode == XVIEW_CPU) return adr;
	return (adr % pgsize) + pgbase;
}

xDumpModel::xDumpModel(QObject* par):xTableModel(par) {
	selMode = XVIEW_NONE;
	selPage = 0;
	selStart = -1;
	selEnd = -1;
	codePage = XCP_437;
	mode = XVIEW_CPU;
	page = 0;
	dmpadr = 0;
	dmpsize = 8;
	maxadr = MEM_64K;
	setRows(11);
	setCols(18);
	pgbase = 0;
	pgsize = MEM_64K;
}

void xDumpModel::setMode(int md, int pgn, int pgb, int pgs) {
	mode = md;
	if (pgn >= 0) page = pgn;
	pgbase = (pgb >= 0) ? pgb : 0xc000;
	pgsize = (pgs >= 0) ? pgs : MEM_16K;
	maxadr = pgsize;
	update();
}

int xDumpModel::rowCount(const QModelIndex&) const {
	return row_count;
}

int xDumpModel::columnCount(const QModelIndex&) const {
	return col_count;
}

QVariant xDumpModel::data(const QModelIndex& idx, int role) const {
	QVariant res;
	QString str;
	if (!idx.isValid()) return res;
	int row = idx.row();
	int col = idx.column();
	if (row < 0) return res;
	if (row >= rowCount()) return res;
	if (col < 0) return res;
	if (col >= columnCount()) return res;
	int adr = (dmpadr + (row * dmpsize)) % maxadr;
	int cadr = (adr + col - 1) % maxadr;
	int flg = mrd(cadr) >> 8;
	// TSConf: a cpu write here goes to CRAM, SFILE or a register too
	int fm = ((mode == XVIEW_CPU) && (col > 0) && (col < 17)) ? ts_fmaps_part(conf.zx, cadr) : TS_FM_NONE;
	QByteArray arr;
	QColor clr;
	switch(role) {
#if QT_VERSION < QT_VERSION_CHECK(6,0,0)
		case Qt::BackgroundColorRole:
#else
		case Qt::BackgroundRole:
#endif
			if ((col > 0) && (col < 17)) {
				if (isSel(cadr)) {
					clr = conf.pal["dbg.sel.bg"];
				} else if (fm != TS_FM_NONE) {
					clr = conf.pal["dbg.fmaps.bg"];
				//} else {
				//	clr = conf.pal["dbg.table.bg"];
				}
			//} else {
			//	clr = conf.pal["dbg.table.bg"];
			}
			if (clr.isValid())
				res = clr;
			break;
		case Qt::ForegroundRole:
			if ((col > 0) && (col < 17)) {
				if (flg & 0x0f) {								// breakpoint
					clr = conf.pal["dbg.brk.txt"];
				} else if (isSel(cadr)) {
					clr = conf.pal["dbg.sel.txt"];
				} else if (fm != TS_FM_NONE) {
					clr = conf.pal["dbg.fmaps.txt"];
				//} else {
				//	clr = conf.pal["dbg.table.txt"];
				}
			//} else {
			//	clr = conf.pal["dbg.table.txt"];
			}
			if (clr.isValid())
				res = clr;
			break;
		case Qt::ToolTipRole:
			switch (fm) {
				case TS_FM_CRAM: str = QString("FMAPS: CRAM, color #%0").arg(gethexbyte((cadr & 0x1ff) >> 1)); break;
				case TS_FM_SFILE: str = QString("FMAPS: SFILE, sprite %0").arg((cadr & 0x1ff) / 6); break;
				case TS_FM_REGS: str = QString("FMAPS: port #%0AF").arg(gethexbyte(cadr & 0xff)); break;
			}
			if ((col > 0) && (col < 17) && conf.dbg.numtips) {
				res = cellTip(cadr, str);
			} else if (!str.isEmpty()) {
				res = str;
			}
			break;
		case DUMP_SEL_ROLE:
			if (col == DUMP_TEXTCOL) {
				int bits = 0;
				for (int i = 0; i < dmpsize; i++)
					if (isSel((adr + i) % maxadr)) bits |= 1 << i;
				res = bits;
			}
			break;
		case Qt::TextAlignmentRole:
			if (col == 17) {
				// right: the column runs to the edge of the widget, so the
				// text block ends up there too - see layoutColumns
				res = (int)(Qt::AlignRight | Qt::AlignVCenter);
			} else if (col != 0) {
				// right, so the extra width of a group's first cell shows up
				// as a gap in front of it - see layoutColumns
				res = (int)(Qt::AlignRight | Qt::AlignVCenter);
			} else {
				// column 0
			}
			break;
		case Qt::EditRole:
			if (col == 0) {
				if (mode != XVIEW_CPU)
					adr = dump_page_ofs(conf.zx, dump_view_area(mode), adr);
				res = dbg_word(adr);
			} else if (col == 17) {
				// no edit
			} else {
				res = gethexbyte(mrd(cadr) & 0xff);
			}
			break;
		case Qt::DisplayRole:
			if (col == 0) {
				if ((mode == XVIEW_RAM) || (mode == XVIEW_ROM) || (mode == XVIEW_DIV)) {
					adr %= pgsize;
					adr += pgbase;
					str = gethexbyte(page).append(":");
				}
				str.append(dbg_adr(adr));
				res = str;
			} else if (col == 17) {
				for(int i = 0; i < dmpsize; i++)
					arr.append(mrd((adr + i) % maxadr) & 0xff);
				res = getDumpString(arr, codePage);
			} else {
				res = gethexbyte(mrd(cadr) & 0xff);
			}
			break;
	}
	return res;
}

Qt::ItemFlags xDumpModel::flags(const QModelIndex& idx) const {
	Qt::ItemFlags res = QAbstractItemModel::flags(idx);
	if (!idx.isValid()) return res;
	if (idx.column() < 17)
		res |= Qt::ItemIsEditable;
	return res;
}


bool xDumpModel::setData(const QModelIndex& idx, const QVariant& val, int role) {
	if (!idx.isValid()) return false;
	if (role != Qt::EditRole) return false;
	int row = idx.row();
	int col = idx.column();
	if ((row < 0) || (row >= rowCount())) return false;
	if ((col < 0) || (col >= columnCount())) return false;
	int fadr;
	int adr = (dmpadr + row * dmpsize) % maxadr;
	int nadr;
	unsigned char bt;
	QString str = val.toString();
	if (col == 0) {
		fadr = str_to_adr(conf.zx, str, true);
		if (fadr >= 0) {
			dmpadr = (fadr - row * dmpsize) % maxadr;
			update();
			emit s_adrch(dmpadr);
			// emit rqRefill();
		}
	} else if (col < 17) {
		nadr = (adr + col - 1) % maxadr;
		bt = str.toInt(NULL, 16) & 0xff;
		mwr(nadr, bt);
		updateRow(row);
		emit s_datach();
	}
	return true;
}

// TEXT COLUMN

void xDumpText::paint(QPainter* pnt, const QStyleOptionViewItem& opt, const QModelIndex& idx) const {
	drawBackground(pnt, opt, idx);
	QString txt = idx.data(Qt::DisplayRole).toString();
	int sel = idx.data(DUMP_SEL_ROLE).toInt();
	QFontMetrics fm(opt.font);
	cw = fm.horizontalAdvance("0");
	// where the plain delegate put the text, aligned right
	QStyle* st = opt.widget ? opt.widget->style() : QApplication::style();
	margin = st->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, opt.widget) + 1;
	QPalette::ColorGroup cg = !(opt.state & QStyle::State_Enabled) ? QPalette::Disabled
		: ((opt.state & QStyle::State_Active) ? QPalette::Normal : QPalette::Inactive);
	QColor txc = opt.palette.color(cg, (opt.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text);
	int n = txt.size();
	int x = opt.rect.right() + 1 - margin - n * cw;
	pnt->save();
	pnt->setClipRect(opt.rect);
	pnt->setFont(opt.font);
	for (int i = 0; i < n; i++, x += cw) {
		QRect rc(x, opt.rect.top(), cw, opt.rect.height());
		if (sel & (1 << i)) {
			pnt->fillRect(rc, conf.pal["dbg.sel.bg"]);
			pnt->setPen(conf.pal["dbg.sel.txt"]);
		} else {
			pnt->setPen(txc);
		}
		pnt->drawText(rc, Qt::AlignCenter, QString(txt.at(i)));
	}
	pnt->restore();
}

// the character under x, of count in the cell as painted; past the ends the nearest
int xDumpText::charAt(const QRect& cell, int x, int count) const {
	int w = (cw > 0) ? cw : 8;
	int left = cell.right() + 1 - margin - count * w;
	if (x < left) return 0;
	int i = (x - left) / w;
	return (i < count) ? i : (count - 1);
}

// TABLE

xDumpTable::xDumpTable(QWidget* p):QTableView(p) {
	anchor = -1;
	dragging = false;
	dragDir = 0;
	pagenum = 0;
	pagesize = MEM_64K;
	mode = XVIEW_CPU;
	rowBytes = 0;			// auto
	model = new xDumpModel();
	setModel(model);
	text = new xDumpText(this);
	setItemDelegateForColumn(DUMP_TEXTCOL, text);
	// a drag past the top or the bottom scrolls a row a tick
	dragTimer = new QTimer(this);
	dragTimer->setInterval(60);
	connect(dragTimer, &QTimer::timeout, this, &xDumpTable::dragStep);
	dbg_cell_tips(this);		// a byte's tip over the byte, as the listing has it over a number
	// no stretching: the columns are sized in layoutColumns and stay put
	horizontalHeader()->setStretchLastSection(false);

	connect(selectionModel(), SIGNAL(currentChanged(QModelIndex,QModelIndex)), this, SLOT(curAdrChanged()));

	connect(model, SIGNAL(s_datach()), this, SIGNAL(s_datach()));
	connect(model, SIGNAL(s_adrch(int)), this, SIGNAL(s_adrch(int)));
	connect(this, SIGNAL(s_adrch(int)), this, SLOT(curAdrChanged()));
}

void xDumpTable::setMode(int md, int pgn, int pgb, int pgs) {
	if ((md != mode) || (pgn != pagenum)) anchor = -1;	// another view's byte
	mode = md;
	pagenum = pgn;
	pagesize = pgs;
	model->setMode(md, pgn, pgb, pgs);
}

int xDumpTable::rows() {
	return model->rowCount();
}

void xDumpTable::setCodePage(int cp) {
	model->codePage = cp;
	model->updateColumn(17);
}

void xDumpTable::update() {
	model->update();
	QTableView::update();
	dbg_cell_tip_follow(this);
}

void xDumpTable::setLimit(unsigned int lim) {
	if (model->maxadr != lim) {
		model->dmpadr %= lim;
		model->maxadr = lim;
		update();
	}
}

unsigned int xDumpTable::limit() {
	return model->maxadr;
}

void xDumpTable::setAdr(int adr) {
	if (model->dmpadr != (unsigned int)adr) {
		model->dmpadr = adr % model->maxadr;
		emit s_adrch(model->dmpadr);
		update();
	}
}

void xDumpTable::curAdrChanged() {
	int adr = getCurrentAdr();
	emit s_curadrch(adr);
}

int xDumpTable::getCurrentAdr() {
	int adr;
	int col;
	QModelIndex idx;
	idx = currentIndex();
	if (!idx.isValid()) return model->dmpadr % model->maxadr;	// no cursor yet: the top
	col = idx.column();
	adr = model->dmpadr + (idx.row() * model->dmpsize);
	if ((col > 0) && (col <= model->dmpsize)) {
		adr += idx.column() - 1;
	}
	adr %= model->maxadr;
	return adr;
}

int dump_cell_width(const QFontMetrics& fm) {
// horizontalAdvance() since 5.11, before - width()
	return fm.horizontalAdvance("00") + 8;
}

// Auto adds bytes a whole group at a time - 4, 8, 12, 16 - never a half group.
// The constants live in dbg_dump.h, the disk dump lines up by the same numbers.
void xDumpTable::layoutColumns() {
	// measure the debugger font, not font(): a style sheet can leave the widget
	// carrying the interface font while it draws in the monospace one
	QFontMetrics fm(conf.dbg.font);
	int w0;			// address column
	int cw = dump_cell_width(fm);
	int tw;			// one character of the text column
	w0 = fm.horizontalAdvance("0000:0000") + 10;
	tw = fm.horizontalAdvance("0");
	// same reason as the disk dump: the form's minimum would override our width
	horizontalHeader()->setMinimumSectionSize(4);

	int n = rowBytes;
	if (n <= 0) {				// auto: fit whole groups
		int avail = viewport()->width() - w0;
		n = 0;
		while (n + DUMP_GROUP <= DUMP_MAXBYTES) {
			int want = (n + DUMP_GROUP) * (cw + tw)		// cells + their text
				+ (n / DUMP_GROUP) * DUMP_GAP;		// gaps already placed
			if (want > avail) break;
			n += DUMP_GROUP;
		}
		if (n < DUMP_GROUP) n = DUMP_GROUP;	// one group always, even if it clips
	}
	model->dmpsize = n;

	setColumnWidth(0, w0);
	for (int c = 1; c <= DUMP_MAXBYTES; c++) {
		if (c > n) {
			hideColumn(c);
			continue;
		}
		showColumn(c);
		// widen the first cell of every group but the first: that is the gap
		int extra = ((c > 1) && (((c - 1) % DUMP_GROUP) == 0)) ? DUMP_GAP : 0;
		setColumnWidth(c, cw + extra);
	}
	// The text column takes everything left over, so the text - aligned right -
	// sits at the edge of the widget. When there is nothing left over it still
	// keeps a gap, so the two blocks never touch.
	int used = w0 + n * cw + ((n / DUMP_GROUP) - 1) * DUMP_GAP;
	int need = n * tw + DUMP_GAP;
	int rest = viewport()->width() - used;
	setColumnWidth(DUMP_TEXTCOL, (rest > need) ? rest : need);
	update();
}

void xDumpTable::setRowBytes(int n) {
	rowBytes = n;
	layoutColumns();
}

void xDumpTable::resizeEvent(QResizeEvent* ev) {
	int h = ev->size().height();
	if (h < 1) return;
	int rh = verticalHeader()->defaultSectionSize();
	int rc = h / rh;
	model->setRows(rc);
	layoutColumns();
}

extern int dbg_get_reg_adr_name(CPU*, const char*);

// go to the address a register pair holds, the way a right click on its name does

void xDumpTable::gotoReg(const char* name) {
	Computer* comp = conf.zx;
	if (!comp) return;
	int adr = dbg_get_reg_adr_name(comp->cpu, name);
	if (adr < 0) return;
	setAdr(adr);
}

void xDumpTable::keyPressEvent(QKeyEvent* ev) {
	QModelIndex idx = currentIndex();
	int key = shortcut_event(SCG_DUMP, ev);
	if ((key < 0) && (shortcut_event(SCG_DEBUGA, ev) >= 0)) {
		ev->ignore();
		return;
	}
	if (key < 0)
		key = ev->key();
	int was = getCurrentAdr();
	bool shift = (ev->modifiers() & Qt::ShiftModifier);
	bool nav = false;		// the caret moves: Shift takes the selection along
	switch(key) {
		case Qt::Key_Up:
		case Qt::Key_Down:
		case Qt::Key_Left:
		case Qt::Key_Right:
		case Qt::Key_PageUp:
		case Qt::Key_PageDown:
			nav = true;
			break;
	}
	int navKey = key;
	if (nav && shift && ((key == Qt::Key_Left) || (key == Qt::Key_Right))) {
		// byte by byte across the rows, never onto the address or the text
		putCaret((was + ((key == Qt::Key_Left) ? -1 : 1) + model->maxadr) % model->maxadr);
		key = 0;
	}
	switch(key) {
		case 0:
			break;
		case Qt::Key_Up:
			if (idx.row() > 0) {
				QTableView::keyPressEvent(ev);
				emit s_adrch(model->dmpadr);
			} else {
				setAdr(model->dmpadr - model->dmpsize);
			}
			break;
		case Qt::Key_Down:
			if (idx.row() < model->rowCount() - 1) {
				QTableView::keyPressEvent(ev);
				emit s_adrch(model->dmpadr);
			} else {
				setAdr(model->dmpadr + model->dmpsize);
			}
			break;
		case Qt::Key_Left:
		case Qt::Key_Right:
			QTableView::keyPressEvent(ev);
			// emit s_adrch(model->dmpadr);
			break;
		case Qt::Key_PageUp:
			setAdr(model->dmpadr - (rows() * model->dmpsize));
			break;
		case Qt::Key_PageDown:
			setAdr(model->dmpadr + (rows() * model->dmpsize));
			break;
		case XCUT_DUMP_REG_PC: gotoReg("PC"); break;
		case XCUT_DUMP_REG_SP: gotoReg("SP"); break;
		case XCUT_DUMP_REG_BC: gotoReg("BC"); break;
		case XCUT_DUMP_REG_DE: gotoReg("DE"); break;
		case XCUT_DUMP_REG_HL: gotoReg("HL"); break;
		case XCUT_DUMP_REG_IX: gotoReg("IX"); break;
		case XCUT_DUMP_REG_IY: gotoReg("IY"); break;
		case XCUT_DUMP_GOTOADR:
			// put the cursor on the address of the first row, so the address can be typed at once
			if (model->rowCount() < 1) break;
			idx = model->index(0, 0);
			setCurrentIndex(idx);
			edit(idx);
			break;
		case Qt::Key_Return:
			if (state() == QAbstractItemView::EditingState) break;
			edit(currentIndex());
			ev->ignore();
			break;
		default:
			QTableView::keyPressEvent(ev);
			break;
	}
	if (nav) {
		int s, e;
		int& anc = anchorRef();
		bool has = model->selRange(&s, &e);
		if (shift) {
			if (!has || (anc < 0)) anc = was;
			// the caret went round past an end: the selection stops there
			int now = getCurrentAdr();
			bool back = (navKey == Qt::Key_Left) || (navKey == Qt::Key_Up) || (navKey == Qt::Key_PageUp);
			if (back && (now > was)) now = 0;
			if (!back && (now < was)) now = model->maxadr - 1;
			selSet(anc, now);
		} else if (has) {
			selClear();
		}
	}
}

// the CPU view starts its selection where the listing does: one selection, one anchor
int& xDumpTable::anchorRef() {
	return (mode == XVIEW_CPU) ? blockAnchor : anchor;
}

// SELECTION, as in a text editor. The CPU view's is the listing's: blockStart/blockEnd

// the byte under a point, the nearest past an edge; past the end of memory it counts on
int xDumpTable::byteAt(const QPoint& pos) {
	int rows = model->rowCount();
	int n = model->dmpsize;
	if ((rows < 1) || (n < 1)) return -1;
	int row = rowAt(pos.y());
	if (row < 0) row = (pos.y() < 0) ? 0 : (rows - 1);
	int col = columnAt(pos.x());
	int ofs;
	if (col < 0) {
		ofs = (pos.x() < 0) ? 0 : (n - 1);
	} else if (col == 0) {
		ofs = 0;
	} else if (col <= n) {
		ofs = col - 1;
	} else {
		ofs = text->charAt(visualRect(model->index(row, DUMP_TEXTCOL)), pos.x(), n);
	}
	return model->dmpadr + row * n + ofs;
}

// the selection's other end: on the anchor's side of the end of memory
int xDumpTable::selTo(int pos, int anc) {
	int m = model->maxadr;
	int last = model->dmpadr + model->rowCount() * model->dmpsize;
	bool ancPast = (last > m) && (anc < last - m) && (anc < (int)model->dmpadr);	// in the rows past the end
	if (pos >= m) return ancPast ? (pos - m) : (m - 1);
	return ancPast ? 0 : pos;
}

// the caret on a byte's cell, the dump scrolled a row when it is just off it
void xDumpTable::putCaret(int adr) {
	int n = model->dmpsize;
	int rel = (adr - (int)model->dmpadr + model->maxadr) % model->maxadr;
	int row = rel / n;
	if (row >= model->rowCount()) {
		if (rel >= (int)model->maxadr - n) {		// the row above the top
			setAdr(model->dmpadr - n);
		} else {
			setAdr(model->dmpadr + n);
		}
		rel = (adr - (int)model->dmpadr + model->maxadr) % model->maxadr;
		row = rel / n;
		if (row >= model->rowCount()) return;
	}
	setCurrentIndex(model->index(row, (rel % n) + 1));
}

void xDumpTable::selSet(int a, int b) {
	if (a > b) std::swap(a, b);
	if (mode == XVIEW_CPU) {
		blockStart = a;
		blockEnd = b;
		emit s_blockch();
	} else {
		model->selMode = mode;
		model->selPage = pagenum;
		model->selStart = a;
		model->selEnd = b;
	}
	update();
}

void xDumpTable::selClear() {
	int s, e;
	if (!model->selRange(&s, &e)) return;
	if (mode == XVIEW_CPU) {
		blockStart = -1;
		blockEnd = -1;
		emit s_blockch();
	} else {
		model->selStart = -1;
	}
	update();
}

bool xDumpTable::selection(int* start, int* end) {
	return model->selRange(start, end);
}

// the selection cut where the dump's rows are cut
QList<QByteArray> xDumpTable::selRows() {
	QList<QByteArray> res;
	int s, e;
	if (!model->selRange(&s, &e)) return res;
	QByteArray all = model->bytes(s, e);
	int n = model->dmpsize;
	QByteArray row;
	for (int i = 0; i < all.size(); i++) {
		int rel = (s + i - (int)model->dmpadr + model->maxadr) % model->maxadr;
		if ((i > 0) && ((rel % n) == 0)) {
			res.append(row);
			row.clear();
		}
		row.append(all.at(i));
	}
	res.append(row);
	return res;
}

QByteArray xDumpTable::selBytes() {
	int s, e;
	if (!model->selRange(&s, &e)) return QByteArray();
	return model->bytes(s, e);
}

int xDumpTable::shownAdr(int adr) {
	return model->shownAdr(adr);
}

void xDumpTable::dropSelection() {
	model->selStart = -1;
	anchor = -1;
}

void xDumpTable::mousePressEvent(QMouseEvent* ev) {
	int was = getCurrentAdr();
	QTableView::mousePressEvent(ev);
	// Ctrl is a plain click: Qt would take the cursor's cell off instead
	if ((ev->modifiers() & Qt::ControlModifier) && indexAt(ev->pos()).isValid())
		selectionModel()->setCurrentIndex(indexAt(ev->pos()), QItemSelectionModel::ClearAndSelect);
	int pos = byteAt(ev->pos());
	if (pos < 0) return;
	int adr = pos % model->maxadr;
	int s, e;
	int& anc = anchorRef();
	switch(ev->button()) {
		case Qt::LeftButton:
			if (ev->modifiers() & Qt::ShiftModifier) {
				if (!model->selRange(&s, &e) || (anc < 0)) anc = was;
				adr = selTo(pos, anc);
				selSet(anc, adr);
			} else {
				selClear();
				anc = adr;
			}
			dragging = true;
			dragPos = ev->pos();
			// a click on an address keeps the cursor there, for Enter to type another
			if ((columnAt(ev->pos().x()) != 0) || (ev->modifiers() & Qt::ShiftModifier))
				putCaret(adr);
			break;
		case X_MidButton:
			selClear();
			anc = -1;
			break;
		case Qt::RightButton:
			// the menu is for the selection when the click is in it
			if (!model->selRange(&s, &e) || (adr < s) || (adr > e)) {
				selClear();
				anc = adr;
			}
			putCaret(adr);
			break;
		default:
			break;
	}
}

void xDumpTable::mouseReleaseEvent(QMouseEvent* ev) {
	QTableView::mouseReleaseEvent(ev);
	if (ev->button() == Qt::LeftButton)
		dragStop();
}

void xDumpTable::hideEvent(QHideEvent* ev) {
	dragStop();
	QTableView::hideEvent(ev);
}

void xDumpTable::focusOutEvent(QFocusEvent* ev) {
	dragStop();		// Alt+Tab in a drag: no autoscroll left running
	QTableView::focusOutEvent(ev);
}

void xDumpTable::mouseMoveEvent(QMouseEvent* ev) {
	if (!dragging || !(ev->buttons() & Qt::LeftButton)) {
		QTableView::mouseMoveEvent(ev);
		return;
	}
	dragPos = ev->pos();
	dragDir = (dragPos.y() < 0) ? -1 : ((dragPos.y() >= viewport()->height()) ? 1 : 0);
	if (!dragDir) {
		dragTimer->stop();
	} else if (!dragTimer->isActive()) {
		dragTimer->start();
	}
	dragTo();
	ev->accept();
}

// the selection from the anchor to the byte under the pointer
void xDumpTable::dragTo() {
	int pos = byteAt(dragPos);
	int anc = anchorRef();
	int s, e;
	if ((pos < 0) || (anc < 0)) return;
	int adr = selTo(pos, anc);
	bool has = model->selRange(&s, &e);
	if ((adr == anc) && !has) return;			// still the byte pressed: a click
	if (has && (s == qMin(anc, adr)) && (e == qMax(anc, adr))) return;	// the same byte: nothing to redo
	selSet(anc, adr);
	putCaret(adr);
}

void xDumpTable::dragStop() {
	dragging = false;
	dragTimer->stop();
}

// the pointer is past the top or the bottom: a row more, up to the start or the end
void xDumpTable::dragStep() {
	int n = model->dmpsize;
	// a release that went elsewhere (a window over this one) ends the drag too
	if (!dragging || !dragDir || !(QApplication::mouseButtons() & Qt::LeftButton)) {
		dragStop();
		return;
	}
	if (dragDir < 0) {
		if (model->dmpadr == 0) return;
		setAdr((model->dmpadr < (unsigned)n) ? 0 : (model->dmpadr - n));
	} else {
		if (model->dmpadr + model->rowCount() * n >= model->maxadr) return;
		setAdr(model->dmpadr + n);
	}
	dragTo();
}

void xDumpTable::wheelEvent(QWheelEvent* ev) {
	if (ev->yDelta < 0) {
		setAdr(model->dmpadr + model->dmpsize);
	} else if (ev->yDelta > 0) {
		setAdr(model->dmpadr - model->dmpsize);
	}
}

// WIDGET

xDumpWidget::xDumpWidget(QString i, QString t, QWidget* p):xDockWidget(i,t,p) {
	QWidget* wid = new QWidget;
	setWidget(wid);
	ui.setupUi(wid);
	setObjectName("MEMDUMPWIDGET");

	ramBase = 0xc000;
	romBase = 0x0000;

	// the units are in the items: everything else on this row says what it is
	// without a caption, and a bare "16" would read like the page size next to it
	ui.cbDumpBytes->addItem("Auto", 0);
	ui.cbDumpBytes->addItem("8/row", 8);
	ui.cbDumpBytes->addItem("12/row", 12);
	ui.cbDumpBytes->addItem("16/row", 16);

	dump_codepage_items(ui.cbCodePage);

	ui.cbDumpView->addItem("CPU", XVIEW_CPU);
	ui.cbDumpView->addItem("RAM", XVIEW_RAM);
	ui.cbDumpView->addItem("ROM", XVIEW_ROM);
	ui.cbDumpView->addItem("DIV", XVIEW_DIV);
	ui.cbDumpView->setItemData(3, "divMMC's RAM by 8K bank", Qt::ToolTipRole);

	ui.leDumpPageBase->setXFlag(XHS_DEC | XHS_ADDR);	// an address: in the debugger's numbers
	ui.leDumpPageBase->setMin(0);
	ui.leDumpPageBase->setMax(0xffff);

	// one menu: the selection's commands, then the breakpoints; more go in here
	actCopyHex = new QAction("Copy as hex", this);
	actCopyHex->setShortcut(QKeySequence::Copy);
	actCopyHex->setShortcutContext(Qt::WidgetShortcut);
	ui.dumpTable->addAction(actCopyHex);		// Ctrl+C while the dump has the focus
	actCopyDB = new QAction("Copy as DB", this);
	actCopyText = new QAction("Copy as text", this);	// the code page goes in when the menu opens
	actSaveBin = new QAction("Save as binary file...", this);
	cellMenu = new QMenu(this);
	cellMenu->addAction(actCopyHex);
	cellMenu->addAction(actCopyDB);
	cellMenu->addAction(actCopyText);
	cellMenu->addAction(actSaveBin);
	cellMenu->addSeparator();
	cellMenu->addAction(ui.actFetch);
	cellMenu->addAction(ui.actRead);
	cellMenu->addAction(ui.actWrite);
	connect(actCopyHex, &QAction::triggered, this, &xDumpWidget::copyHex);
	connect(actCopyDB, &QAction::triggered, this, &xDumpWidget::copyDB);
	connect(actCopyText, &QAction::triggered, this, &xDumpWidget::copyText);
	connect(actSaveBin, &QAction::triggered, this, &xDumpWidget::saveBin);
	// Ctrl+C is this action too: not left greyed by the menu
	connect(cellMenu, &QMenu::aboutToHide, this, [this]() {actCopyHex->setEnabled(true);});
	connect(ui.actFetch, &QAction::triggered, this, &xDumpWidget::brkAction);
	connect(ui.actRead, &QAction::triggered, this, &xDumpWidget::brkAction);
	connect(ui.actWrite, &QAction::triggered, this, &xDumpWidget::brkAction);

	xid_addr = new xItemDelegate(XTYPE_LABEL);
	xid_byte = new xItemDelegate(XTYPE_BYTE);

	ui.dumpTable->setItemDelegate(xid_byte);
	ui.dumpTable->setItemDelegateForColumn(0, xid_addr);	// the text column has its own, the table's

	connect(ui.dumpTable, &xDumpTable::customContextMenuRequested, this, &xDumpWidget::customMenu);

	connect(ui.dumpTable, &xDumpTable::s_adrch, ui.dumpScroll, &QScrollBar::setValue);
	connect(ui.dumpScroll, &QScrollBar::valueChanged, ui.dumpTable, &xDumpTable::setAdr);

	connect(ui.dumpTable, &xDumpTable::s_blockch, this, &xDumpWidget::s_blockch);
	connect(ui.dumpTable, &xDumpTable::s_datach, this, &xDumpWidget::s_datach);
	connect(ui.dumpTable, &xDumpTable::s_adrch, this, &xDumpWidget::s_adrch);
	connect(ui.dumpTable, &xDumpTable::s_curadrch, this, &xDumpWidget::adr_changed);

	connect(ui.cbDumpBytes, SIGNAL(currentIndexChanged(int)), this, SLOT(bytes_changed()));
	connect(ui.cbCodePage, SIGNAL(currentIndexChanged(int)), this, SLOT(cp_changed()));
	connect(ui.cbDumpView, SIGNAL(currentIndexChanged(int)), this, SLOT(modeChanged()));
	connect(ui.sbDumpPage, SIGNAL(valueChanged(int)), this, SLOT(refill()));
	connect(ui.leDumpPageBase, &xHexSpin::valueChanged, this, &xDumpWidget::refill);

	ui.cbDumpView->setCurrentIndex(0);

	connect(this, &QDockWidget::visibilityChanged, this, &xDumpWidget::draw);
}

void xDumpWidget::setAdr(int a) {
	ui.dumpTable->setAdr(a);
}

void xDumpWidget::setLimit(int lim) {
	ui.dumpTable->setLimit(lim);
	ui.dumpScroll->setMaximum(lim - 1);
}

int xDumpWidget::viewArea(int* page) {
	*page = ui.sbDumpPage->value();
	return dump_view_area(getRFIData(ui.cbDumpView));
}

bool xDumpWidget::selection(int* start, int* end) {
	return ui.dumpTable->selection(start, end);
}

void xDumpWidget::dropSelection() {
	ui.dumpTable->dropSelection();
}

void xDumpWidget::customMenu(const QPoint& pos) {
	int s, e;
	bool sel = ui.dumpTable->selection(&s, &e);
	actCopyHex->setEnabled(sel);
	actCopyDB->setEnabled(sel);
	actCopyText->setEnabled(sel);
	actCopyText->setText(QString("Copy as text (%0)").arg(ui.cbCodePage->currentText()));
	actSaveBin->setEnabled(sel);
	int adr = ui.dumpTable->getCurrentAdr();
	int mode = getRFIData(ui.cbDumpView);
	unsigned char flag = 0;
	if (mode == XVIEW_CPU) {
		flag = getBrk(conf.zx, adr);
	} else {
		unsigned char* fp;		// the byte's own flags, not the CPU address's
		if (dump_cell(conf.zx, dump_view_area(mode), ui.sbDumpPage->value(), adr, &fp)) flag = *fp;
	}
	ui.actFetch->setChecked(flag & MEM_BRK_FETCH);
	ui.actRead->setChecked(flag & MEM_BRK_RD);
	ui.actWrite->setChecked(flag & MEM_BRK_WR);
	cellMenu->popup(ui.dumpTable->viewport()->mapToGlobal(pos));
}

// "3E 01 D3 FE", as a hex editor pastes it
void xDumpWidget::copyHex() {
	QByteArray data = ui.dumpTable->selBytes();
	if (data.isEmpty()) return;
	QStringList list;
	for (int i = 0; i < data.size(); i++)
		list.append(gethexbyte((unsigned char)data.at(i)));
	QApplication::clipboard()->setText(list.join(" "));
}

// a byte as an assembler takes it, in the Debugger's form: #3E, $3E, 0FEh, 254
static QString dump_db_byte(int val) {
	QString num = dbg_num_in(val & 0xff, 2, conf.dbg.numfmt);
	if (num.at(0).isLetter()) num.prepend("0");	// FEh would be a name
	return num;
}

// a DB line a dump row
void xDumpWidget::copyDB() {
	QList<QByteArray> rows = ui.dumpTable->selRows();
	if (rows.isEmpty()) return;
	QString txt;
	foreach(QByteArray row, rows) {
		QStringList list;
		for (int i = 0; i < row.size(); i++)
			list.append(dump_db_byte((unsigned char)row.at(i)));
		txt.append("\tDB ").append(list.join(",")).append("\n");
	}
	QApplication::clipboard()->setText(txt);
}

// as the text column shows it, in its code page
void xDumpWidget::copyText() {
	QByteArray data = ui.dumpTable->selBytes();
	if (data.isEmpty()) return;
	QApplication::clipboard()->setText(getDumpString(data, getRFIData(ui.cbCodePage)));
}

// image_C000-C3FF.bin, image_RAM5_C000-C3FF.bin
QString xDumpWidget::binName(int start, int end) {
	QString name = media_image_name();
	if (!name.isEmpty()) name.append("_");
	int page;
	int area = viewArea(&page);
	if (area != CELL_NONE)
		name.append(dump_page_token(area, page)).append("_");
	name.append(QString("%0-%1.bin").arg(gethexword(ui.dumpTable->shownAdr(start)), gethexword(ui.dumpTable->shownAdr(end))));
	return name;
}

void xDumpWidget::saveBin() {
	static QString dir;		// the last one's, the image's before that
	int s, e;
	if (!ui.dumpTable->selection(&s, &e)) return;
	QByteArray data = ui.dumpTable->selBytes();
	if (dir.isEmpty() && !media_current().isEmpty())
		dir = QFileInfo(arc_beside(media_current(), QString())).absolutePath();	// an archive's own folder
	QString path = binName(s, e);
	if (!dir.isEmpty()) path.prepend(dir + "/");
	path = QFileDialog::getSaveFileName(this, "Save as binary file", path, "Binary files (*.bin);;All files (*)", nullptr, QFileDialog::DontUseNativeDialog);
	if (path.isEmpty()) return;
	dir = QFileInfo(path).absolutePath();
	QFile file(path);
	if (!file.open(QFile::WriteOnly) || (file.write(data) != data.size()))
		shitHappens("Can't write the file");
}

void xDumpWidget::brkAction() {
	int bt = 0;
	xAdr xadr;
	int adr = ui.dumpTable->getCurrentAdr();
	if (ui.actFetch->isChecked()) bt |= MEM_BRK_FETCH;
	if (ui.actRead->isChecked()) bt |= MEM_BRK_RD;
	if (ui.actWrite->isChecked()) bt |= MEM_BRK_WR;
	int mode = getRFIData(ui.cbDumpView);
	if (mode == XVIEW_CPU) {
		int access = (bt & MEM_BRK_FETCH) ? MEM_VIEW_FETCH
			: ((bt & MEM_BRK_WR) ? MEM_VIEW_WRITE : MEM_VIEW_READ);
		xadr = comp_get_xadr(conf.zx, adr, access);
	} else {
		// the cell the page names: on the Next a ROM page is in its SRAM too
		int area = dump_view_area(mode);
		if (!comp_cell_xadr(conf.zx, area, ui.sbDumpPage->value(), dump_page_ofs(conf.zx, area, adr), &xadr.type, &xadr.abs)) return;
	}
	switch(xadr.type) {
		case MEM_RAM: emit s_brkrq(BRK_MEMRAM, bt, xadr.abs); break;
		case MEM_ROM: emit s_brkrq(BRK_MEMROM, bt, xadr.abs); break;
		case MEM_DIV: emit s_brkrq(BRK_MEMDIV, bt, xadr.abs % (DIV_EE_SIZE + DIV_RAM_MAX)); break;
		case MEM_EXT: emit s_brkrq(BRK_MEMEXT, bt, xadr.abs); break;
	}
}

void xDumpWidget::draw() {	ui.dumpTable->update();
	refill();
}

void xDumpWidget::adr_changed(int adr) {
	titleWidget->setText(QString("DUMP | %0").arg(dbg_abs_t(adr & 0xffffff)));
//	setWindowTitle(QString::number(adr, 16).right(6).toUpper().rightJustified(6,'0'));
}

void xDumpWidget::cp_changed() {
	int cp = getRFIData(ui.cbCodePage);
	ui.dumpTable->setCodePage(cp);
	ui.dumpTable->update();
}

void xDumpWidget::bytes_changed() {
	ui.dumpTable->setRowBytes(getRFIData(ui.cbDumpBytes));
}

void xDumpWidget::modeChanged() {
	int mode = getRFIData(ui.cbDumpView);
	switch(mode) {
		case XVIEW_CPU: refill(); break;
		case XVIEW_ROM:
		case XVIEW_RAM:
			ui.leDumpPageBase->setValue((mode == XVIEW_ROM) ? romBase : ramBase);
			refill();
			break;
		case XVIEW_DIV:
			ui.leDumpPageBase->setValue(0x2000);	// where its banks are paged in
			refill();
			break;
	}
}

void xDumpWidget::refill() {
	int mode = getRFIData(ui.cbDumpView);
	int area = dump_view_area(mode);
	if (area != CELL_NONE) {
		// as many pages as the machine has now, which a machine change may have changed
		int pages = comp_cell_pages(conf.zx, area);
		QSignalBlocker block(ui.sbDumpPage);
		ui.sbDumpPage->setMaximum((pages > 0) ? (pages - 1) : 0);
	}
	int page = ui.sbDumpPage->value();
	int pbase = ui.leDumpPageBase->getValue();
	// a page view is as long as its page: 16K, 8K for the Next's RAM and divMMC's
	int psize = (area == CELL_NONE) ? MEM_64K : comp_cell_unit(conf.zx, area);
	ui.widDumpPage->setDisabled(mode == XVIEW_CPU);
	ui.dumpTable->setMode(mode, page, pbase, psize);
	ui.dumpScroll->setMaximum(psize-1);
	ui.dumpTable->setLimit(psize);
}

#include "opt_tapecat.h"
#include "xgui/xgui.h"
#include "xcore/xcore.h"

#include <QHeaderView>
#include "../../filer.h"
#include <QIcon>
#include <QPainter>

// model

xTapeCatModel::xTapeCatModel(QObject* p):xTableModel(p) {
	setRows(0);
	setCols(TCC_COUNT);
	rcur = -1;
	inf = NULL;
	icoBrk = QIcon(":/images/stop.png");
	icoDur = QIcon(":/images/clock.png");
}

void xTapeCatModel::fill(Tape* tap) {
	setRows(tap->blkCount);
	if (inf) delete[] inf;
	inf = NULL;
	dur.clear();
	name.clear();
	named.clear();
	info.clear();
	if (row_count > 0) {
		inf = new TapeBlockInfo[row_count];
		tapGetBlocksInfo(tap, inf);
		for (int i = 0; i < row_count; i++) {
			QString nam = headName(i);
			bool own = !nam.isEmpty();
			dur << QString(getTimeString(inf[i].time).c_str());
			named << own;
			name << (own ? nam : blockKind(i));
			info << blockInfo(i);
		}
	}
	setCurrent(tap->block);
	update();
}

// The block the tape stands on is read off the tape, not stored here: it moves
// on a rewind, on a double click and at a block boundary, and only the last of
// those used to refill the list - so the mark sat where the tape no longer was.
// Repainting the two rows is cheap enough to do on every refresh.
// !0 when it moved.
int xTapeCatModel::setCurrent(int row) {
	if (row == rcur) return 0;
	int was = rcur;
	rcur = row;
	if ((was >= 0) && (was < row_count)) updateRow(was);
	if ((row >= 0) && (row < row_count)) updateRow(row);
	return 1;
}

// a name someone gave the file, as opposed to our own word for the block

int xTapeCatModel::isNamed(int row) const {
	return named.value(row, false);
}

// The name a header carries, shown the way the machine would show it: the
// tokens spelled out and the control codes dropped. Empty when the header has
// no name, or nothing that can be seen of one.

QString xTapeCatModel::headName(int row) const {
	if (inf[row].type != TAPE_HEAD) return QString();
	return zx_text(inf[row].name, TAPE_NAME_LEN);
}

// our own word for a block with no name, in lower case and italics, so a name
// is never in doubt

QString xTapeCatModel::blockKind(int row) const {
	if (!inf[row].hasBytes) return QString("custom");	// a signal, not bytes
	if (inf[row].type != TAPE_HEAD) return QString("data");
	switch (inf[row].htype) {				// a header, but no name in it
		case TAPE_HT_PROG: return QString("program");
		case TAPE_HT_NUMARR: return QString("number array");
		case TAPE_HT_CHRARR: return QString("character array");
		case TAPE_HT_CODE: return QString("code");
	}
	return QString("header");
}

// what a header says about the file, in the words BASIC uses for it

QString xTapeCatModel::blockInfo(int row) const {
	QString res;
	int nam = (inf[row].par1 >> 8) & 0x1f;			// arrays: the variable's letter
	QChar var = QLatin1Char(nam ? 'a' + nam - 1 : '?');
	if (inf[row].type == TAPE_HEAD) {
		switch (inf[row].htype) {
			case TAPE_HT_PROG:
				// a program with no autostart keeps a line number over 32767
				res = (inf[row].par1 < 32768) ? QString("PROGRAM LINE %0").arg(inf[row].par1) : QString("PROGRAM");
				break;
			case TAPE_HT_NUMARR: res = QString("DATA %0()").arg(var); break;
			case TAPE_HT_CHRARR: res = QString("DATA %0$()").arg(var); break;
			case TAPE_HT_CODE: res = QString("CODE %0,%1").arg(inf[row].par1).arg(inf[row].dlen); break;
		}
	}
	if (res.isEmpty())
		res = QString::fromLocal8Bit(inf[row].text);
	// the image stops the tape here itself, always or on a 48K
	QString stop = inf[row].stopMark ? "stop the tape" : inf[row].stop48 ? "stop the tape on a 48K" : "";
	if (!stop.isEmpty())
		res += res.isEmpty() ? stop : QString(" (%0)").arg(stop);
	return res;
}

static QVariant tcmName[TCC_COUNT] = {"", "", "Size", "Content", "Info"};
static QVariant tcmTips[TCC_COUNT] = {
	"Stop the tape when it reaches this block",
	"How long the block plays",
	"Bytes in the block",
	"The name in the header, or what kind of block it is",
	"What the header says about the file"
};

QVariant xTapeCatModel::headerData(int sec, Qt::Orientation ori, int role) const {
	QVariant res;
	if (ori != Qt::Horizontal) return res;
	if ((sec < 0) || (sec >= columnCount())) return res;
	switch (role) {
		case Qt::DisplayRole:
			res = tcmName[sec];
			break;
		case Qt::ToolTipRole:
			res = tcmTips[sec];
			break;
		case TCC_IconRole:			// these two are too narrow for a word
			if (sec == TCC_BRK) res = icoBrk;
			if (sec == TCC_DUR) res = icoDur;
			break;
	}
	return res;
}

QVariant xTapeCatModel::data(const QModelIndex& idx, int role) const {
	QVariant res;
	if (!idx.isValid()) return res;
	int row = idx.row();
	int col = idx.column();
	if ((row < 0) || (row >= rowCount())) return res;
	if ((col < 0) || (col >= columnCount())) return res;
	if (inf == NULL) return res;
	switch (role) {
		case Qt::CheckStateRole:
			// an empty cell says nothing about being clickable, so the box is
			// always there
			if (col == TCC_BRK)
				res = inf[row].breakPoint ? Qt::Checked : Qt::Unchecked;
			break;
		case Qt::TextAlignmentRole:
			if ((col == TCC_DUR) || (col == TCC_SIZE))
				res = (int)(Qt::AlignRight | Qt::AlignVCenter);
			else if ((col == TCC_NAME) && !isNamed(row))
				res = (int)(Qt::AlignRight | Qt::AlignVCenter);
			break;
		case Qt::FontRole:
			if (col == TCC_NAME) {			// bold a name, italic our own word
				QFont fnt;
				if (isNamed(row)) {
					fnt.setBold(true);
				} else {
					fnt.setItalic(true);
				}
				res = fnt;
			}
			break;
		case X_BackgroundRole:
			if (row == rcur) res = QColor(Qt::darkGray);
			break;
		case Qt::ForegroundRole:
			if (row == rcur) res = QColor(Qt::white);
			break;
		case Qt::DisplayRole:
			switch(col) {
				case TCC_DUR: res = dur[row];
					break;
				case TCC_SIZE:	if (inf[row].size > 0)
						res = inf[row].size;
					break;
				case TCC_NAME: res = name[row];
					break;
				case TCC_INFO: res = info[row];
					break;
			}
			break;
	}
	return  res;
}

// header

xTapeCatHeader::xTapeCatHeader(QWidget* p):QHeaderView(Qt::Horizontal, p) {
	setSectionsClickable(false);
	setHighlightSections(false);
}

// the section keeps whatever the style draws for it, the icon goes on top in
// the middle of it

void xTapeCatHeader::paintSection(QPainter* pnt, const QRect& rect, int idx) const {
	// the base leaves the painter clipped to nothing, so anything drawn after it
	// would go nowhere
	pnt->save();
	QHeaderView::paintSection(pnt, rect, idx);
	pnt->restore();
	if (!model()) return;
	QVariant var = model()->headerData(idx, orientation(), TCC_IconRole);
	if (!var.isValid()) return;
	int siz = style()->pixelMetric(QStyle::PM_SmallIconSize, NULL, this);
	QRect box = QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, QSize(siz, siz), rect);
	qvariant_cast<QIcon>(var).paint(pnt, box);
}

// an empty section asks for nothing, so make room for the icon it will carry

QSize xTapeCatHeader::sectionSizeFromContents(int idx) const {
	QSize res = QHeaderView::sectionSizeFromContents(idx);
	if (!model()) return res;
	if (!model()->headerData(idx, orientation(), TCC_IconRole).isValid()) return res;
	int siz = style()->pixelMetric(QStyle::PM_SmallIconSize, NULL, this) + 4;
	if (res.width() < siz) res.setWidth(siz);
	if (res.height() < siz) res.setHeight(siz);
	return res;
}

// table

xTapeCatTable::xTapeCatTable(QWidget* p):QTableView(p) {
	model = new xTapeCatModel();
	setModel(model);
	setHorizontalHeader(new xTapeCatHeader(this));
	setItemDelegateForColumn(TCC_BRK, new xCheckItem(this));
	QHeaderView* hdr = horizontalHeader();
	hdr->setStretchLastSection(true);		// Info takes the rest of the width
	hdr->setMinimumSectionSize(TCC_MIN_WIDTH);
	hdr->setSectionResizeMode(TCC_BRK, QHeaderView::ResizeToContents);
	hdr->setSectionResizeMode(TCC_DUR, QHeaderView::ResizeToContents);
	hdr->setSectionResizeMode(TCC_SIZE, QHeaderView::ResizeToContents);
	hdr->setSectionResizeMode(TCC_NAME, QHeaderView::Interactive);
	setColumnWidth(TCC_NAME, TCC_NAME_WIDTH);
}

// the marked block is kept in view
void xTapeCatTable::setCurrent(int row) {
	if (model->setCurrent(row))
		scrollTo(model->index(row, 0), QAbstractItemView::EnsureVisible);
}

// the row the buttons beside the list work on. currentIndex(), not the selection
// model: this runs on the tape window's refresh, and selectedRows() builds a list
// of persistent indexes every time.
int xTapeCatTable::current() {
	int row = currentIndex().row();
	return ((row >= 0) && (row < model->rowCount())) ? row : -1;
}

// The emulation thread walks the block list on every rom load, so it is held
// while the list is changed under it.

void xTapeCatTable::blkMove(Tape* tape, int dir) {
	int row = current();
	int to = row + dir;
	if ((row < 0) || (to < 0) || (to >= tape->blkCount)) return;
	emu_lock();
	tapSwapBlocks(tape, row, to);
	emu_unlock();
	fill(tape);
	selectRow(to);
}

void xTapeCatTable::blkDel(Tape* tape) {
	int row = current();
	if (row < 0) return;
	emu_lock();
	tapDelBlock(tape, row);
	emu_unlock();
	fill(tape);
}

void xTapeCatTable::fill(Tape* tape) {
	model->fill(tape);
	scrollTo(model->index(tape->block, 0), QAbstractItemView::EnsureVisible);
	setEnabled(tape->blkCount > 0);
}

// tape -> disk

// What a header block says the file is. An unreadable type gives ext 0, which
// the caller reads as "no header here after all".
static TRFile tape_head_info(Tape* tape, int blk) {
	TRFile res;
	TapeBlockInfo inf = tapGetBlockInfo(tape, blk);
	unsigned char* dt = (unsigned char*)malloc(inf.size + 2);
	tapGetBlockData(tape, blk, dt, inf.size + 2);
	for (int i = 0; i < 8; i++) res.name[i] = dt[i+2];
	switch (dt[1]) {
		case 0:
			res.ext = 'B';
			res.lst = dt[12]; res.hst = dt[13];
			res.llen = dt[16]; res.hlen = dt[17];
			break;
		case 3:
			res.ext = 'C';
			res.llen = dt[12]; res.hlen = dt[13];
			res.lst = dt[14]; res.hst = dt[15];
			break;
		default:
			res.ext = 0x00;
	}
	res.slen = res.hlen;
	if (res.llen != 0) res.slen++;
	free(dt);
	return res;
}

// The drive has to hold a TR-DOS disk before anything can be written to it.
// Anything destructive is asked about first. !0 when it is ready.
// (SetupWin::newdisk does the same three calls, but it is a member and ends by
// relabelling its own page)
int tape_disk_ready(Computer* comp, int drive) {
	Floppy* flp = comp->dif->flp[drive & 3];
	if (!flp->insert) {
		if (saveChangedDisk(comp, drive & 3) != ERR_OK) return 0;
		flp_insert(flp, NULL);
		flp->changed = 1;
		trd_format(flp);
	} else if (diskGetType(flp) != DISK_TYPE_TRD) {
		if (!areSure("Not TRDOS disk. Format?<br>All data will be lost")) return 0;
		trd_format(flp);
	}
	return 1;
}

// Copies one tape block to the disk as a TR-DOS file: a header block is taken
// with the data block after it, a data block with the header before it if there
// is one, and a block with neither becomes FILE C. !0 when the file is there,
// and *msg is what to tell the user either way.
int tape_blk_to_disk(Tape* tape, int blk, Floppy* flp, QString* msg) {
	TRFile dsc;
	int headBlock = -1;
	int dataBlock = -1;

	if ((blk < 0) || (blk >= (int)tape->blkCount)) {
		*msg = "No block picked";
		return 0;
	}
	if (!tape->blkData[blk].hasBytes) {
		*msg = "This is not a standard block";
		return 0;
	}
	if (tape->blkData[blk].isHeader) {
		if ((int)tape->blkCount == blk + 1) {
			*msg = "Header without data? Hmm...";
			return 0;
		}
		if (!tape->blkData[blk+1].hasBytes) {
			*msg = "Data block is not standard";
			return 0;
		}
		headBlock = blk;
		dataBlock = blk + 1;
	} else {
		dataBlock = blk;
		if ((blk > 0) && tape->blkData[blk-1].isHeader)
			headBlock = blk - 1;
	}
	TapeBlockInfo inf = tapGetBlockInfo(tape, dataBlock);
	if (headBlock < 0) {
		dsc = diskMakeDescriptor("FILE", 'C', 0, inf.size);
	} else {
		dsc = tape_head_info(tape, headBlock);
		if (dsc.ext == 0x00) {
			*msg = "Yes, it happens";
			return 0;
		}
	}
	// diskCreateFile takes whole sectors out of the buffer, so the block is
	// copied into one of that size with the tail zeroed
	int secs = (inf.size + 0xff) >> 8;
	unsigned char* dt = (unsigned char*)malloc(inf.size + 2);	// +mark +crc
	unsigned char* sec = (unsigned char*)calloc(secs ? secs << 8 : 256, 1);
	tapGetBlockData(tape, dataBlock, dt, inf.size + 2);
	memcpy(sec, dt + 1, inf.size);
	int err = diskCreateFile(flp, dsc, sec, inf.size);
	free(dt);
	free(sec);
	switch (err) {
		case ERR_OK: *msg = "File(s) was copied"; return 1;
		case ERR_SIZE: *msg = "Too much data for a TR-DOS file"; break;
		case ERR_MANYFILES: *msg = "Too many files on the disk"; break;
		case ERR_NOSPACE: *msg = "Not enough space on the disk"; break;
		default: *msg = "Yes, it happens"; break;
	}
	return 0;
}

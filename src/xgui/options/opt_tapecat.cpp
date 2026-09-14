#include "opt_tapecat.h"
#include "xgui/xgui.h"
#include "xcore/xcore.h"

#include <QHeaderView>
#include <QIcon>
#include <QPainter>

// model

xTapeCatModel::xTapeCatModel(QObject* p):xTableModel(p) {
	setRows(0);
	setCols(TCC_COUNT);
	inf = NULL;
	icoBrk = QIcon(":/images/stop.png");
	icoDur = QIcon(":/images/clock.png");
}

void xTapeCatModel::fill(Tape* tap) {
	setRows(tap->blkCount);
	rcur = tap->block;
	if (inf) delete[] inf;
	inf = NULL;
	dur.clear();
	name.clear();
	info.clear();
	if (row_count > 0) {
		inf = new TapeBlockInfo[row_count];
		tapGetBlocksInfo(tap, inf);
		for (int i = 0; i < row_count; i++) {
			dur << QString(getTimeString(inf[i].time).c_str());
			name << blockName(i);
			info << blockInfo(i);
		}
	}
	update();
}

// a name someone gave the file, as opposed to our own word for the block

int xTapeCatModel::isNamed(int row) const {
	return (inf[row].type == TAPE_HEAD) && inf[row].name[0];
}

// what the block is: the name a header carries, or the kind of block it is. Our
// own words go in lower case and italics, so a name is never in doubt

QString xTapeCatModel::blockName(int row) const {
	if (isNamed(row)) return QString::fromLocal8Bit(inf[row].name);
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
	if (inf[row].stopMark)					// the image stops the tape here itself
		res += res.isEmpty() ? QString("stop the tape") : QString(" (stop the tape)");
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

// tape player window will reset scroll on update
void xTapeCatTable::fill(Tape* tape) {
	model->fill(tape);
	scrollTo(model->index(tape->block, 0), QAbstractItemView::EnsureVisible);
	setEnabled(tape->blkCount > 0);
}

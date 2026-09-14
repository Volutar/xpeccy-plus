#include "dbg_diskdump.h"
#include "dbg_dump.h"		// dump layout constants: both dumps line up alike
#include "../../xcore/xcore.h"

// dump index is a physical track: (cylinder << 1) | side, the same slot flpNext
// addresses. how far the head steps is the drive's property, not the disk's -
// same limit flpStep uses. note the number is a slot, not a count: a
// single-sided drive tops out at 172, which is cylinder 86 with no side bit,
// and it gets there in steps of two
int flp_max_track(Floppy* flp) {
	return ((flp->trk80 ? 86 : 43) << 1) | (flp->doubleSide ? 1 : 0);
}

xDiskDump::xDiskDump(QWidget*) {
	rowBytes = 0;			// auto
	mod = new xDiskDumpModel();
	setModel(mod);
	setDrive(0);
}

void xDiskDump::setDrive(int d) {
	drv = d;
	mod->setDrive(drv);
}

void xDiskDump::setTrack(int tr) {
	Floppy* flp = conf.zx->dif->flp[drv & 3];
	if (!flp->doubleSide) tr &= ~1;
	if (tr <= flp_max_track(flp))
		mod->setTrack(tr);
}

void xDiskDump::update() {
	setDrive(drv);
}

void xDiskDump::toTarget() {
	scrollTo(mod->index(conf.zx->dif->fdc->flp->pos / mod->rowBytes, 0));
}

// Columns are laid out like the memory dump's: fixed cells from the left, a gap
// after every group, the text column taking the rest. 0 address, then bytes,
// then DUMP_TEXTCOL.
void xDiskDump::layoutColumns() {
	// measure the debugger font, not font(): a style sheet can leave the widget
	// carrying the interface font while it draws in the monospace one
	QFontMetrics fm(conf.dbg.font);
	int w0;
	int cw = dump_cell_width(fm);
	int tw;
// horizontalAdvance() since 5.11, before - width()
#if (QT_VERSION >= QT_VERSION_CHECK(5,11,0))
	w0 = fm.horizontalAdvance("00:0000") + 12;	// see the DisplayRole format
	tw = fm.horizontalAdvance("0");
#else
	w0 = fm.width("00:0000") + 12;
	tw = fm.width("0");
#endif
	// the form asks for a wide minimum section, which would override the cell
	// width the dumps agree on. setupUi applies it after the constructor, so
	// it has to be lowered here
	horizontalHeader()->setMinimumSectionSize(4);
	int n = rowBytes;
	if (n <= 0) {				// auto: fit whole groups
		int avail = viewport()->width() - w0;
		n = 0;
		while (n + DUMP_GROUP <= DUMP_MAXBYTES) {
			int want = (n + DUMP_GROUP) * (cw + tw)
				+ (n / DUMP_GROUP) * DUMP_GAP;
			if (want > avail) break;
			n += DUMP_GROUP;
		}
		if (n < DUMP_GROUP) n = DUMP_GROUP;
	}
	if (mod->rowBytes != n) {
		mod->rowBytes = n;
		mod->recount();
	}

	horizontalHeader()->setStretchLastSection(false);
	setColumnWidth(0, w0);
	for (int c = 1; c <= DUMP_MAXBYTES; c++) {
		if (c > n) {
			hideColumn(c);
			continue;
		}
		showColumn(c);
		int extra = ((c > 1) && (((c - 1) % DUMP_GROUP) == 0)) ? DUMP_GAP : 0;
		setColumnWidth(c, cw + extra);
	}
	// the leftover goes to the text column, so it ends up at the widget edge,
	// and a gap is kept when there is no leftover
	int used = w0 + n * cw + ((n / DUMP_GROUP) - 1) * DUMP_GAP;
	int need = n * tw + DUMP_GAP;
	int rest = viewport()->width() - used;
	setColumnWidth(DUMP_TEXTCOL, (rest > need) ? rest : need);
}

void xDiskDump::setRowBytes(int n) {
	rowBytes = n;
	layoutColumns();
}

void xDiskDump::resizeEvent(QResizeEvent* ev) {
	if (ev->size().height() < 1) return;
	layoutColumns();
}

// model

xDiskDumpModel::xDiskDumpModel(QObject* p):xTableModel(p) {
	drv = 0;
	trk = 0;
	rowBytes = 8;
	setCols(DUMP_COLS);
}

void xDiskDumpModel::setDrive(int dr) {
	drv = dr & 3;
	recount();
}

// Row count follows the track length and the line width. Guarded: this is
// reached from resizeEvent now, which also fires while docks are dragged
// around and for machines that have no disk interface at all.
void xDiskDumpModel::recount() {
	if (!conf.zx || !conf.zx->dif) return;
	Floppy* flp = conf.zx->dif->flp[drv];
	if (!flp) return;
	int new_rcnt = (flp->trklen + rowBytes - 1) / rowBytes;
#if 1
	setRows(new_rcnt);
#else
	if (new_rcnt < rcnt) {
		emit beginRemoveRows(QModelIndex(), new_rcnt, rcnt - new_rcnt);
		rcnt = new_rcnt;
		emit endRemoveRows();
	} else if (new_rcnt > rcnt) {
		emit beginInsertRows(QModelIndex(), rcnt, new_rcnt - rcnt);
		rcnt = new_rcnt;
		emit endInsertRows();
	}
	emit dataChanged(index(0, 0), index(rowCount(), columnCount()));
#endif
}

void xDiskDumpModel::setTrack(int tr) {
	trk = tr;
	update();
}

QVariant xDiskDumpModel::data(const QModelIndex& idx, int role) const {
	QVariant res;
	int row = idx.row();
	int col = idx.column();
	if (row >= rowCount() || (row < 0)) return res;
	if (col >= columnCount() || (col < 0)) return res;
	int adr = row * rowBytes;		// first byte of the line
	int offset = adr + (col - 1);		// the cell's own byte
	unsigned char ch;
	int pos;
	char buf[256];
	Floppy* flp = conf.zx->dif->flp[drv];
	QFont fnt;
	QString cnam;
	QColor pcol;
	switch (role) {
		case X_BackgroundRole:
		case Qt::ForegroundRole:
			// the marked cells carry a text colour of their own: without it a
			// dark style would put light text on a light marker
			if (col == 0) break;
			if (col > rowBytes) break;
			if (offset >= flp->trklen) break;
			if (!flp->insert) break;
			ch = flp->data[trk].field[offset];
			switch(ch & 0x0f) {
				case 1: cnam = "dbg.disk.id"; break;		// id
				case 2:					// data
				case 3: cnam = "dbg.disk.data"; break;
				case 4: cnam = "dbg.disk.crc"; break;		// crc
			}
			if (!cnam.isEmpty()) {
				pcol = conf.pal[cnam + ((role == X_BackgroundRole) ? ".bg" : ".txt")];
				if (pcol.isValid()) res = pcol;
			}
//			if (ch & 0x80) {
//				res = QColor(220,120,120);	// 'broken' A1
//			}
			break;
		case Qt::FontRole:
			if (col == 0) break;
			if (col > rowBytes) break;
			if (trk != ((flp->trk << 1) | (conf.zx->dif->fdc->side ? 1 : 0))) break;
			if (offset != flp->pos) break;
			// from the debugger font, not a default one: that would drop the
			// monospace family and read as a different face, not as bold
			fnt = conf.dbg.font;
			fnt.setBold(true);
			res = fnt;
			break;
		case Qt::TextAlignmentRole:
			// same rule as the memory dump: everything right, so a wider cell
			// shows its extra space in front of the value
			if (col > 0) {
				res = (int)(Qt::AlignRight | Qt::AlignVCenter);
			}
			break;
		case Qt::DisplayRole:
			if (col == 0) {
				sprintf(buf, "%.2X:%.4X", trk, adr);
			} else if (col == DUMP_TEXTCOL) {
				pos = 0;
				while (pos < rowBytes) {
					if (adr < flp->trklen) {
						ch = flp->data[trk].byte[adr];
						if ((ch < 32) || (ch > 127))
							ch = '.';
					} else {
						ch = '.';
					}
					buf[pos++] = ch;
					adr++;
				}
				buf[pos] = 0;
			} else if (offset < flp->trklen) {
				if (flp->insert) {
					sprintf(buf, "%.2X", flp->data[trk].byte[offset]);
				} else {
					strcpy(buf, "FF");
				}
			} else {
				buf[0] = 0;
			}
			res = QString(buf);
			break;
	}
	return res;
}

// widget

xDiskDumpWidget::xDiskDumpWidget(QString i, QString t, QWidget* p):xDockWidget(i,t,p) {
	QWidget* wid = new QWidget;
	setWidget(wid);
	ui.setupUi(wid);
	setObjectName("FDDDUMPWIDGET");
//	ui.tabDiskDump->setColumnWidth(0, 70);
//	ui.tabDiskDump->horizontalHeader()->setStretchLastSection(true);
	connect(ui.cbDrive, SIGNAL(currentIndexChanged(int)), ui.tabDiskDump, SLOT(setDrive(int)));
	connect(ui.cbDrive, SIGNAL(currentIndexChanged(int)), this, SLOT(setDrive(int)));
	connect(ui.sbTrack, SIGNAL(valueChanged(int)), ui.tabDiskDump, SLOT(setTrack(int)));
	connect(ui.tbTarget, SIGNAL(released()),this,SLOT(toTarget()));

	ui.cbDiskBytes->addItem("Auto", 0);		// same wording as the memory dump
	ui.cbDiskBytes->addItem("8/row", 8);
	ui.cbDiskBytes->addItem("12/row", 12);
	ui.cbDiskBytes->addItem("16/row", 16);
	connect(ui.cbDiskBytes, SIGNAL(currentIndexChanged(int)), this, SLOT(bytes_changed()));

	setDrive(0);
}

// the spin box follows the selected drive: a 40 track one stops halfway, a
// single-sided one steps by whole cylinders
void xDiskDumpWidget::setDrive(int d) {
	Floppy* flp = conf.zx->dif->flp[d & 3];
	ui.sbTrack->setMaximum(flp_max_track(flp));
	ui.sbTrack->setSingleStep(flp->doubleSide ? 1 : 2);
}

void xDiskDumpWidget::bytes_changed() {
	ui.tabDiskDump->setRowBytes(getRFIData(ui.cbDiskBytes));
	ui.tabDiskDump->update();
}

void xDiskDumpWidget::toTarget() {
	FDC* fdc = conf.zx->dif->fdc;
	Floppy* flp = fdc->flp;
	ui.cbDrive->setCurrentIndex(flp->id);
	ui.sbTrack->setValue((flp->trk << 1) | !!fdc->side);
	ui.tabDiskDump->toTarget();
}

void xDiskDumpWidget::draw() {
	setDrive(ui.cbDrive->currentIndex());	// geometry changes with the profile
	ui.tabDiskDump->update();
}

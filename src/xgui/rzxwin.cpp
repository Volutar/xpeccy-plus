#include <QHeaderView>

#include "xgui.h"
#include "xcore/xcore.h"
#include "xcore/filemachine.h"
#include "../filer.h"

// the machine a snapshot in the recording was taken on
static QString rzx_machine_name(int hw) {
	const xMachine* mac = NULL;
	if (hw == SNAP_HW_PENT512) return QString("Pentagon 512");
	if (hw >= SNAP_HW_CORE) {			// one of ours: named by its core
		HardWare* core = findHardwareId(hw - SNAP_HW_CORE);
		if (core) mac = xm_find_by_core(core->name);
	} else if (hw != SNAP_HW_UNKNOWN) {
		mac = xm_find(fm_snap_target(hw));
	}
	return mac ? QString::fromStdString(mac->name) : QString("unknown");
}

static QString rzx_time(int frames, double fps) {
	return QString(getTimeString((int)(frames / fps)).c_str());
}

// Fuse packs its four version numbers a byte each into the two words
static QString rzx_creator(const rzxBlock* blk) {
	QString name = QString::fromLatin1(blk->text).trimmed();
	if (blk->major < 0x100) {
		QString ver = QString("%0.%1").arg(blk->major).arg(blk->minor);
		return name.endsWith(ver) ? name : QString("%0 %1").arg(name, ver);	// SPIN names it twice
	}
	QString ver = QString("%0.%1.%2").arg(blk->major >> 8).arg(blk->major & 0xff).arg(blk->minor >> 8);
	if (blk->minor & 0xff) ver += QString(".%0").arg(blk->minor & 0xff);
	return QString("%0 %1").arg(name, ver);
}

// model

xRzxModel::xRzxModel(QObject* p):xTableModel(p) {
	setRows(0);
	setCols(RZC_COUNT);
}

void xRzxModel::fill(const rzxInfo* inf, double fps) {
	rows.clear();
	markRow(-1);
	creator.clear();
	machine = "not in the file";
	snapStart = snapInside = snapEnd = 0;
	int lastin = -1;			// a snapshot after it is where the file ends
	for (int i = 0; i < inf->count; i++)
		if (inf->blk[i].id == 0x80) lastin = i;
	for (int i = 0; i < inf->count; i++) {
		const rzxBlock* blk = &inf->blk[i];
		xRzxRow row = {QStringList(), -1, blk->frames, 1, false};
		for (int c = 0; c < RZC_COUNT; c++) row.cells << QString();
		switch (blk->id) {
			case 0x10:
				row.cells[RZC_KIND] = "creator";
				row.cells[RZC_INFO] = rzx_creator(blk);
				if (creator.isEmpty()) creator = row.cells[RZC_INFO];
				break;
			case 0x20:
			case 0x21:
				row.cells[RZC_KIND] = "signature";
				break;
			case 0x30: {
				row.frame = blk->frame;
				row.cells[RZC_START] = rzx_time(blk->frame, fps);
				row.cells[RZC_KIND] = "snapshot";
				QString res = QString("%0, %1").arg(QString::fromLatin1(blk->ext).trimmed().toUpper(),
					rzx_machine_name(blk->hw));
				if (blk->flags & 1)
					res += QString(", file %0").arg(QString::fromLocal8Bit(blk->text));
				if (snapStart == 0) {
					res = "start: " + res;
					machine = rzx_machine_name(blk->hw);
					snapStart = 1;
				} else if (i > lastin) {
					res = "end: " + res;
					snapEnd++;
				} else {
					snapInside++;
				}
				row.cells[RZC_INFO] = res;
				break;
			}
			case 0x80:
				// blocks in a row are one stretch of play: Fuse starts a new one
				// every 5 s whether the snapshot before it is kept or not
				if (!rows.isEmpty() && rows.last().input) {
					xRzxRow& last = rows.last();
					last.frames += blk->frames;
					last.blocks++;
					last.cells[RZC_LEN] = rzx_time(last.frames, fps);
					last.cells[RZC_INFO] = QString("%0 frames in %1 blocks").arg(last.frames).arg(last.blocks);
					continue;
				}
				row.input = true;
				row.frame = blk->frame;
				row.cells[RZC_START] = rzx_time(blk->frame, fps);
				row.cells[RZC_LEN] = rzx_time(blk->frames, fps);
				row.cells[RZC_KIND] = "input";
				row.cells[RZC_INFO] = QString("%0 frames").arg(blk->frames);
				if (blk->flags & 1) row.cells[RZC_INFO] += ", encrypted";
				break;
			default:
				row.cells[RZC_KIND] = QString("block %0").arg(blk->id, 2, 16, QChar('0')).toUpper();
				row.cells[RZC_INFO] = QString("%0 bytes").arg(blk->size);
				break;
		}
		rows << row;
	}
	if (inf->junk > 0) {
		xRzxRow row = {QStringList(), -1, 0, 0, false};
		for (int c = 0; c < RZC_COUNT; c++) row.cells << QString();
		row.cells[RZC_KIND] = "junk";
		row.cells[RZC_INFO] = QString("%0 bytes after the last block").arg(inf->junk);
		rows << row;
	}
	setRows(rows.size());
	update();
}

// the input block playback is in; !0 when that moved
int xRzxModel::setCurrent(int frame) {
	int row = -1;
	for (int i = 0; i < rows.size(); i++) {
		if (rows[i].input && (frame >= rows[i].frame) && (frame < rows[i].frame + rows[i].frames)) {
			row = i;
			break;
		}
	}
	return markRow(row);
}

int xRzxModel::frameAt(int row) const {
	return ((row >= 0) && (row < rows.size())) ? rows[row].frame : -1;
}

static const char* rzcName[RZC_COUNT] = {"Start", "Length", "Block", "Info"};

QVariant xRzxModel::headerData(int sec, Qt::Orientation ori, int role) const {
	if ((ori != Qt::Horizontal) || (role != Qt::DisplayRole)) return QVariant();
	if ((sec < 0) || (sec >= RZC_COUNT)) return QVariant();
	return QString(rzcName[sec]);
}

QVariant xRzxModel::data(const QModelIndex& idx, int role) const {
	QVariant res;
	if (!idx.isValid()) return res;
	int row = idx.row();
	int col = idx.column();
	if ((row < 0) || (row >= rows.size()) || (col < 0) || (col >= RZC_COUNT)) return res;
	switch (role) {
		case Qt::DisplayRole:
			res = rows[row].cells[col];
			break;
		case Qt::TextAlignmentRole:
			if ((col == RZC_START) || (col == RZC_LEN))
				res = (int)(Qt::AlignRight | Qt::AlignVCenter);
			break;
		case X_BackgroundRole:
		case Qt::ForegroundRole:
			res = markRole(row, role);
			break;
	}
	return res;
}

// window

RZXWin::RZXWin(QWidget *par):QDialog(par) {
	ui.setupUi(this);
	setWindowFlags(Qt::Tool);
	setProperty("xCenterOnce", true);
	state = RWS_STOP;
	fps = 50;
	model = new xRzxModel(this);
	ui.blkList->setModel(model);
	QHeaderView* hdr = ui.blkList->horizontalHeader();
	hdr->setSectionResizeMode(RZC_START, QHeaderView::ResizeToContents);
	hdr->setSectionResizeMode(RZC_LEN, QHeaderView::ResizeToContents);
	hdr->setSectionResizeMode(RZC_KIND, QHeaderView::ResizeToContents);
	ui.ppButton->setEnabled(false);
	ui.stopButton->setEnabled(false);
	connect(ui.ppButton,SIGNAL(released()),this,SLOT(playPause()));
	connect(ui.stopButton,SIGNAL(released()),this,SLOT(stopPressed()));
	connect(ui.openButton,SIGNAL(released()),this,SLOT(open()));
	connect(ui.blkList,SIGNAL(doubleClicked(QModelIndex)),this,SLOT(doDClick(QModelIndex)));
	ui.progress->installEventFilter(this);
	ui.progress->setCursor(Qt::PointingHandCursor);
}

// a click on the bar goes to that point of the recording
bool RZXWin::eventFilter(QObject* obj, QEvent* ev) {
	if ((obj == ui.progress) && (state != RWS_STOP) && (ev->type() == QEvent::MouseButtonPress)) {
		QMouseEvent* mev = static_cast<QMouseEvent*>(ev);
		int wid = ui.progress->width();
		if ((mev->button() == Qt::LeftButton) && (wid > 0)) {
			int x = qBound(0, mev->pos().x(), wid);
			emit seekTo((int)((long long)ui.progress->maximum() * x / wid));
			return true;
		}
	}
	return QDialog::eventFilter(obj, ev);
}

void RZXWin::doDClick(QModelIndex idx) {
	int frame = model->frameAt(idx.row());
	if ((frame >= 0) && (state != RWS_STOP))
		emit seekTo(frame);
}


// What the file says about itself. A frame of the recording is an interrupt,
// so its time is the frame time of the machine it plays on.
void RZXWin::fillInfo() {
	QString path = rzx_current();
	ui.rpath->setText(path);
	fps = comp_fps(conf.zx);
	rzxInfo inf = {};
	if (path.isEmpty() || (rzx_info(path.toLocal8Bit().constData(), &inf, 0) != ERR_OK)) {
		model->fill(&inf, fps);
		ui.labCreator->clear();
		ui.labMachine->clear();
		ui.labLength->clear();
		ui.labSnaps->clear();
		return;
	}
	model->fill(&inf, fps);
	QString creator = model->creator.isEmpty() ? QString("unknown") : model->creator;
	if (inf.flags & 1) creator += ", signed";
	ui.labCreator->setText(creator);
	ui.labMachine->setText(model->machine);
	ui.labLength->setText(QString("%0, %1 frames").arg(rzx_time(inf.frames, fps)).arg(inf.frames));
	QStringList parts;
	if (model->snapStart) parts << "start";
	if (model->snapInside) parts << QString("%0 inside").arg(model->snapInside);
	if (model->snapEnd) parts << "end";
	ui.labSnaps->setText(inf.snaps ? QString("%0: %1").arg(inf.snaps).arg(parts.join(", ")) : QString("none"));
	rzx_info_free(&inf);
}

void RZXWin::startPlay() {
	fillInfo();
	ui.ppButton->setEnabled(true);
	ui.stopButton->setEnabled(true);
	ui.ppButton->setIcon(QIcon(":/images/pause.png"));
	setProgress(0, conf.zx->rzx.fTotal);
	state = RWS_PLAY;
}

void RZXWin::setProgress(int val, int max) {
	ui.progress->setMaximum(max);
	ui.progress->setValue(val);
	ui.labTime->setText(QString("%0 / %1").arg(rzx_time(val, fps), rzx_time(max, fps)));
}

// slots

void RZXWin::upd(Computer* comp) {
	if (comp->rzx.play && isVisible()) {
		setProgress(comp->rzx.fCurrent, comp->rzx.fTotal);
		if (model->setCurrent(comp->rzx.fCurrent) && (model->markedRow() >= 0))
			ui.blkList->scrollTo(model->index(model->markedRow(), 0), QAbstractItemView::EnsureVisible);
	}
}

void RZXWin::playPause() {
	switch(state) {
		case RWS_PLAY:
			state = RWS_PAUSE;
			ui.ppButton->setIcon(QIcon(":/images/play.png"));
			emit stateChanged(RWS_PAUSE);
			break;
		case RWS_PAUSE:
			state = RWS_PLAY;
			ui.ppButton->setIcon(QIcon(":/images/pause.png"));
			emit stateChanged(RWS_PLAY);
			break;
	}
}

void RZXWin::stop() {
	state = RWS_STOP;
	ui.ppButton->setEnabled(false);
	ui.stopButton->setEnabled(false);
	ui.ppButton->setIcon(QIcon(":/images/play.png"));
	setProgress(0, ui.progress->maximum());
	model->setCurrent(-1);
}

void RZXWin::stopPressed() {
	emit stateChanged(RWS_STOP);
}

void RZXWin::open() {
	emit stateChanged(RWS_OPEN);
}

#include <QMenu>

#include "xgui.h"
#include "xcore/xcore.h"
#include "../filer.h"
#include "ui_tapeexp.h"

TapeWin::TapeWin(QWidget *par):QDialog(par) {
	ui.setupUi(this);
	setWindowFlags(Qt::Tool);
	ui.stopBut->setEnabled(false);
	// which drive Copy to disk writes to: the window has no disk page to take
	// the answer from, so the button asks
	QMenu* menu = new QMenu(this);
	for (int i = 0; i < 4; i++)
		menu->addAction(QString("Drive %0").arg(QChar('A' + i)))->setData(i);
	ui.tbToDisk->setMenu(menu);
	connect(menu, SIGNAL(triggered(QAction*)), this, SLOT(doToDisk(QAction*)));
	connect(ui.playBut,SIGNAL(released()),this,SLOT(doPlay()));
	connect(ui.recBut,SIGNAL(released()),this,SLOT(doRec()));
	connect(ui.stopBut,SIGNAL(released()),this,SLOT(doStop()));
	connect(ui.loadBut,SIGNAL(released()),this,SLOT(doLoad()));
	connect(ui.saveBut,SIGNAL(released()),this,SLOT(doSave()));
	connect(ui.expBut,SIGNAL(released()),this,SLOT(doExport()));
	connect(ui.tbRewind,SIGNAL(released()),this,SLOT(doRewind()));
	connect(ui.tbEject,SIGNAL(released()),this,SLOT(doEject()));
	connect(ui.tbBlkUp,SIGNAL(released()),this,SLOT(doBlkUp()));
	connect(ui.tbBlkDn,SIGNAL(released()),this,SLOT(doBlkDn()));
	connect(ui.tbBlkDel,SIGNAL(released()),this,SLOT(doBlkDel()));
	connect(ui.tapeList,SIGNAL(doubleClicked(QModelIndex)), this, SLOT(doDClick(QModelIndex)));
	connect(ui.tapeList,SIGNAL(clicked(QModelIndex)), this, SLOT(doClick(QModelIndex)));
	connect(ui.sldSpeed,SIGNAL(valueChanged(int)),this, SLOT(setSpeed(int)));
	// clicked, not toggled: upd() writes the boxes on every tick and toggled
	// would send each of those back as a change of the user's
	connect(ui.cbAuto,SIGNAL(clicked(bool)),this,SLOT(setOptions()));
	connect(ui.cbFast,SIGNAL(clicked(bool)),this,SLOT(setOptions()));
	connect(ui.cbRewind,SIGNAL(clicked(bool)),this,SLOT(setOptions()));
}

void TapeWin::show() {
	QDialog::show();
	upd(conf.zx->tape);		// takes the speed slider with it
	updList(conf.zx->tape);
}

// on timer
void TapeWin::updProgress(Tape* tape) {
	if (!isVisible()) return;
	// where the tape stands in this block, playing or not: gated on ->on the bar
	// kept whatever it read when the tape stopped, so a rewind left it at 63%
	if (tape->rec || (tape->block >= tape->blkCount)) {
		ui.tapeBar->setValue(0);
	} else {
		ui.tapeBar->setMaximum(tape->blkData[tape->block].sigCount);
		ui.tapeBar->setValue(tape->pos);
	}
	// the bar keeps no text of its own: over a bright chunk the one colour a
	// style sheet gives it is unreadable half the time
	ui.labBarVal->setText(ui.tapeBar->text());
}

void TapeWin::upd(Tape* tape) {
	if (!isVisible()) return;
	ui.sldSpeed->setValue(tape->speed);	// Setup has the same slider
	int got = (tape->blkCount > 0);
	int row = ui.tapeList->current();
	// the tape can be swapped from anywhere - the menu, a drop, the command line -
	// so the name is read here rather than written by this window's own buttons.
	// Compared as bytes: this runs fifty times a second.
	const char* path = tape->path ? tape->path : "";
	if (tapeRaw != path) {
		tapeRaw = path;
		ui.tpath->setText(QString::fromLocal8Bit(path));
	}
	if (tapeChanged != tape->changed) {
		tapeChanged = tape->changed;
		setWindowTitle(tape->changed ? "Tape player - modified" : "Tape player");
	}
	ui.cbAuto->setChecked(conf.tape.autostart);
	ui.cbFast->setChecked(conf.tape.fast);
	ui.cbRewind->setChecked(conf.tape.rewind);
	ui.playBut->setEnabled(got && !tape->on);
	ui.recBut->setEnabled(got && !tape->on);
	ui.stopBut->setEnabled(tape_running(tape));
	ui.tbRewind->setEnabled(got && !tape->on);
	ui.tbEject->setEnabled(got && !tape->on);
	ui.saveBut->setEnabled(got);
	ui.tbToDisk->setEnabled(row >= 0);
	ui.tbBlkUp->setEnabled(row > 0);
	ui.tbBlkDn->setEnabled((row >= 0) && (row < tape->blkCount - 1));
	ui.tbBlkDel->setEnabled(row >= 0);
	ui.tapeList->setCurrent(tape->block);
}

// on block changed
void TapeWin::updList(Tape* tape) {
	if (!isVisible()) return;	// fill() walks every block: not for a hidden list
	int row = ui.tapeList->current();	// a refill drops the selection, and the
	ui.tapeList->fill(tape);		// tape moves on while a block is picked
	if ((row >= 0) && (row < tape->blkCount))
		ui.tapeList->selectRow(row);
}

// slots

void TapeWin::doPlay() {
	Tape* tap = conf.zx->tape;
	tapUserPlay(tap);
	upd(tap);
}

void TapeWin::doStop() {
	Tape* tap = conf.zx->tape;
	tapUserStop(tap);
	upd(tap);
}

void TapeWin::doRec() {
	Tape* tap = conf.zx->tape;
	tapRec(tap);
	upd(tap);
}

void TapeWin::doRewind() {
	Tape* tap = conf.zx->tape;
	tapRewind(tap, 0);
	upd(tap);
}

void TapeWin::doEject() {
	Tape* tap = conf.zx->tape;
	tapEject(tap);
	upd(tap);
	updList(tap);
}

void TapeWin::doLoad() {
	conf.emu.pause |= PR_FILE;
	load_file(conf.zx, nullptr, FG_TAPE, -1);
	upd(conf.zx->tape);
	updList(conf.zx->tape);
	conf.emu.pause &= ~PR_FILE;
}

void TapeWin::doSave() {
	Tape* tap = conf.zx->tape;
	if (tap->blkCount < 1) return;
	conf.emu.pause |= PR_FILE;
	save_file(conf.zx, tap->path, FG_TAPE, -1);
	upd(tap);
	conf.emu.pause &= ~PR_FILE;
}

// The wav is a recording and the only thing on it is the signal, so what a
// loader gets back depends on how it was written: the rate has to carry the
// shortest pulse on the tape, and the tail has to be there at all, or the last
// block ends with the file and the edge that closes it goes with it.
void TapeWin::doExport() {
	Tape* tap = conf.zx->tape;
	if (tap->blkCount < 1) return;
	QDialog dlg(this);
	Ui::TapeExport eui;
	eui.setupUi(&dlg);
	int autorate = wav_export_rate(conf.zx);
	eui.cbRate->addItem(QString("Auto (%0 Hz)").arg(autorate), 0);
	for (const int* r = wav_export_rates(); *r; r++)
		eui.cbRate->addItem(QString("%0 Hz").arg(*r), *r);
	eui.cbBits->addItem("16 bit", 16);
	eui.cbBits->addItem("8 bit", 8);
	setRFIndex(eui.cbRate, conf.tape.exp.rate);
	setRFIndex(eui.cbBits, conf.tape.exp.bits);
	eui.sbLevel->setValue(conf.tape.exp.level);
	eui.sbLead->setValue(conf.tape.exp.lead);
	eui.sbTail->setValue(conf.tape.exp.tail);
	int secs = 0;
	for (int i = 0; i < tap->blkCount; i++)
		secs += tap->blkData[i].time;
	// how big the file comes out: the one number that decides the rate in practice
	auto showSize = [&]() {
		int rate = getRFIData(eui.cbRate);
		if (rate < 1) rate = autorate;
		double mb = (double)secs * rate * getRFIData(eui.cbBits) / 8 / (1024.0 * 1024.0);
		eui.labSize->setText(QString("%0 blocks, %1, about %2 MB")
			.arg(tap->blkCount).arg(getTimeString(secs).c_str()).arg(mb, 0, 'f', 1));
	};
	QObject::connect(eui.cbRate, &QComboBox::currentTextChanged, &dlg, showSize);
	QObject::connect(eui.cbBits, &QComboBox::currentTextChanged, &dlg, showSize);
	showSize();
	if (dlg.exec() != QDialog::Accepted) return;
	conf.tape.exp.rate = getRFIData(eui.cbRate);
	conf.tape.exp.bits = getRFIData(eui.cbBits);
	conf.tape.exp.level = eui.sbLevel->value();
	conf.tape.exp.lead = eui.sbLead->value();
	conf.tape.exp.tail = eui.sbTail->value();
	QString path = file_ask_save("Export tape to WAV", "WAV tape recording (*.wav)", ".wav");
	if (path.isEmpty()) return;
	conf.emu.pause |= PR_FILE;
	int err = saveWAVopt(conf.zx, path.toLocal8Bit().data(), &conf.tape.exp);
	conf.emu.pause &= ~PR_FILE;
	file_errors(err);
}

// the three options are the same ones the Tape page of Options has, and they
// take effect where they are set - this window has no Apply

void TapeWin::setOptions() {
	conf.tape.autostart = ui.cbAuto->isChecked() ? 1 : 0;
	conf.tape.fast = ui.cbFast->isChecked() ? 1 : 0;
	conf.tape.rewind = ui.cbRewind->isChecked() ? 1 : 0;
	tape_apply_options(conf.zx->tape);
}

void TapeWin::doBlkUp() {
	ui.tapeList->blkMove(conf.zx->tape, -1);
}

void TapeWin::doBlkDn() {
	ui.tapeList->blkMove(conf.zx->tape, 1);
}

void TapeWin::doBlkDel() {
	ui.tapeList->blkDel(conf.zx->tape);
}

void TapeWin::doToDisk(QAction* act) {
	Computer* comp = conf.zx;
	int row = ui.tapeList->current();
	if ((row < 0) || !act) return;
	int drv = act->data().toInt() & 3;
	if (!tape_disk_ready(comp, drv)) return;
	QString msg;
	emu_lock();
	int ok = tape_blk_to_disk(comp->tape, row, comp->dif->flp[drv], &msg);
	emu_unlock();
	if (ok) {
		showInfo(msg.toLocal8Bit().constData());
	} else {
		shitHappens(msg.toLocal8Bit().constData());
	}
}

void TapeWin::doDClick(QModelIndex idx) {
	int row = idx.row();
	int col = idx.column();
	if (col == TCC_BRK) return;
	tapRewind(conf.zx->tape, row);
	updList(conf.zx->tape);
}

void TapeWin::doClick(QModelIndex idx) {
	int row = idx.row();
	int col = idx.column();
	if (col != TCC_BRK) return;
	conf.zx->tape->blkData[row].breakPoint ^= 1;
	updList(conf.zx->tape);
}

void TapeWin::setSpeed(int s) {
	if (s < 95) return;
	if (s > 105) return;
	tape_set_speed(conf.zx->tape, s);
	ui.labSpeedVal->setText(QString("%0%").arg(s));
}

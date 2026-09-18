#include "xgui.h"
#include "xcore/xcore.h"
#include "../filer.h"

TapeWin::TapeWin(QWidget *par):QDialog(par) {
	ui.setupUi(this);
	setWindowFlags(Qt::Tool);
	ui.stopBut->setEnabled(false);
	connect(ui.playBut,SIGNAL(released()),this,SLOT(doPlay()));
	connect(ui.recBut,SIGNAL(released()),this,SLOT(doRec()));
	connect(ui.stopBut,SIGNAL(released()),this,SLOT(doStop()));
	connect(ui.loadBut,SIGNAL(released()),this,SLOT(doLoad()));
	connect(ui.tbRewind,SIGNAL(released()),this,SLOT(doRewind()));
	connect(ui.tbEject,SIGNAL(released()),this,SLOT(doEject()));
	connect(ui.tapeList,SIGNAL(doubleClicked(QModelIndex)), this, SLOT(doDClick(QModelIndex)));
	connect(ui.tapeList,SIGNAL(clicked(QModelIndex)), this, SLOT(doClick(QModelIndex)));
	connect(ui.sldSpeed,SIGNAL(valueChanged(int)),this, SLOT(setSpeed(int)));
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
	ui.playBut->setEnabled(got && !tape->on);
	ui.recBut->setEnabled(got && !tape->on);
	ui.stopBut->setEnabled(tape->on);
	ui.tbRewind->setEnabled(got && !tape->on);
	ui.tbEject->setEnabled(got && !tape->on);
	ui.tapeList->setCurrent(tape->block);
}

// on block changed
void TapeWin::updList(Tape* tape) {
	if (!isVisible()) return;	// fill() walks every block: not for a hidden list
	ui.tapeList->fill(tape);
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
	updList(conf.zx->tape);
	// ui.tapeList->fill(conf.zx->tape);
	conf.emu.pause &= ~PR_FILE;
}

void TapeWin::doDClick(QModelIndex idx) {
	int row = idx.row();
	int col = idx.column();
	if (col == TCC_BRK) return;
	tapRewind(conf.zx->tape, row);
	updList(conf.zx->tape);
	//ui.tapeList->fill(conf.zx->tape);
}

void TapeWin::doClick(QModelIndex idx) {
	int row = idx.row();
	int col = idx.column();
	if (col != TCC_BRK) return;
	conf.zx->tape->blkData[row].breakPoint ^= 1;
	updList(conf.zx->tape);
	// ui.tapeList->fill(conf.zx->tape);
}

void TapeWin::setSpeed(int s) {
	if (s < 95) return;
	if (s > 105) return;
	conf.zx->tape->speed = s;
	ui.labSpeedVal->setText(QString("%0%").arg(s));
}

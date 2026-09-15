// The sound chip panel: PSG and FM of every chip the machine carries.

#include "dbg_sndchip.h"
#include "../../xcore/sound.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPainter>
#include <QLinearGradient>
#include <QStyleOption>
#include <QPushButton>
#include <math.h>
#include <vector>
#include <QVBoxLayout>

// The beeper and the tape panel still use this one: a plain level, not a meter,
// so the xLevelCell below with its loud-is-red ramp would say the wrong thing.
void drawHBar(QLabel* lab, int lev, int max) {
	if (lev > max) lev = max;
	if (lev < 0) lev = 0;
	QPixmap pxm(100, 10);
	QPainter pnt;
	pxm.fill(Qt::black);
	pnt.begin(&pxm);
	pnt.fillRect(0, 0, pxm.width() * lev / max, pxm.height(), Qt::green);
	pnt.setPen(Qt::red);
	pnt.drawLine(pxm.width() / 2, 0, pxm.width() / 2, pxm.height());
	pnt.end();
	lab->setPixmap(pxm);
}

// LEVEL CELL

// A meter is as wide as this many characters of the debugger font, whatever it
// is showing: the bar is there to be read as a bar, and a couple of digits wide
// it would only ever be empty, half or full.
#define LEV_CELLS	10

// The colours a level meter has everywhere: green over most of the range, amber
// near the top, red at the end, so the bar says how loud it is without the
// number being read. They are fixed rather than taken from the style - this is a
// meter, and those three are what a meter means - and each of them is light
// enough to read black digits on.
static const struct {
	double at;
	QRgb col;
} levRamp[] = {
	{0.00, 0x56b256},
	{0.55, 0x56b256},
	{0.78, 0xe8c22e},
	{1.00, 0xe0503f}
};

#define LEV_STOPS	((int)(sizeof(levRamp) / sizeof(levRamp[0])))

// A colour that can be read on `bg`: the one offered when it stands out enough,
// black or white when it does not. Trusting one text colour over two different
// backgrounds is what makes the tape player's progress figure vanish into the
// bar - Gruvbox draws cream on mustard there and nothing can be read at all.
static QColor readableOn(const QColor& bg, const QColor& want) {
	if (qAbs(qGray(bg.rgb()) - qGray(want.rgb())) >= 80) return want;
	return (bg.lightness() < 128) ? QColor(Qt::white) : QColor(Qt::black);
}

// Black digits have to be readable wherever the fill reaches them, so the guard
// is against the darkest colour the ramp can put behind them.
static QColor levInk() {
	QColor worst(levRamp[0].col);
	for (int i = 1; i < LEV_STOPS; i++) {
		QColor c(levRamp[i].col);
		if (qGray(c.rgb()) < qGray(worst.rgb())) worst = c;
	}
	return readableOn(worst, QColor(Qt::black));
}

xLevelCell::xLevelCell(QWidget* p):QWidget(p) {
	lev = -1;
	top = 1;
	digits = 2;
	setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
}

void xLevelCell::setLevel(int val, int max) {
	if ((val == lev) && (max == top)) return;
	lev = val;
	top = (max > 0) ? max : 1;
	update();
}

void xLevelCell::setDigits(int n) {
	digits = n;
	update();
}

QSize xLevelCell::minimumSizeHint() const {
	QFontMetrics fm(font());
	return QSize(fm.horizontalAdvance(QString(LEV_CELLS, '0')), fm.height());
}

void xLevelCell::paintEvent(QPaintEvent*) {
	QPainter pnt(this);
	QStyleOption opt;
	opt.initFrom(this);
	QColor ground = opt.palette.color(QPalette::Window);
	QColor ink = opt.palette.color(QPalette::WindowText);
	QString txt = (lev < 0) ? QString("-") : formbufword(lev).rightJustified(digits, '0');

	int wid = (lev <= 0) ? 0 : (width() * lev / top);
	if (wid > width()) wid = width();
	// the trough, so an empty bar still reads as one. It is the only part that
	// follows the style, and it follows it by being the panel's own ink, faintly
	QColor trough = ink;
	trough.setAlpha(40);
	pnt.fillRect(0, 0, width(), height(), trough);
	if (wid > 0) {
		// the ramp runs across the whole cell, so a colour means a level whatever
		// the bar happens to reach - the way the lamps on a meter are coloured
		QLinearGradient grad(0, 0, width(), 0);
		for (int i = 0; i < LEV_STOPS; i++)
			grad.setColorAt(levRamp[i].at, QColor(levRamp[i].col));
		pnt.fillRect(0, 0, wid, height(), grad);
	}

	// the figure twice, each half in a colour that can be read where it lands
	pnt.setPen(levInk());
	pnt.setClipRect(0, 0, wid, height());
	pnt.drawText(rect(), Qt::AlignCenter, txt);
	pnt.setPen(readableOn(ground, ink));
	pnt.setClipRect(wid, 0, width() - wid, height());
	pnt.drawText(rect(), Qt::AlignCenter, txt);
}

// PSG PAGE

// A frequency as the note nearest to it and how far off it is, in cents.
// A4 is 440 Hz, so the octave numbers are the scientific ones - middle C is C-4.
static QString noteName(double frq) {
	static const char* nam[12] = {"C-", "C#", "D-", "D#", "E-", "F-",
				"F#", "G-", "G#", "A-", "A#", "B-"};
	if (frq < 8.0) return QString("-");		// below C-0, or nothing at all
	double midi = 69.0 + 12.0 * log2(frq / 440.0);
	int n = (int)floor(midi + 0.5);
	int cent = (int)floor((midi - n) * 100.0 + 0.5);
	if ((n < 12) || (n > 127)) return QString("-");
	return QString("%0%1 %2%3").arg(nam[n % 12]).arg(n / 12 - 1)
		.arg((cent < 0) ? "-" : "+").arg(qAbs(cent), 2, 10, QChar('0'));
}

static QString getAYmix(aymChan* ch) {
	QString res = ch->tdis ? "-" : "T";
	res += ch->ndis ? "-" : "N";
	res += ch->een ? "E" : "-";
	return res;
}

// ENVELOPE SHAPE

#define AYENV_STEPS	48	// one period and a half, 32 levels each

xAYEnvView::xAYEnvView(QWidget* p):QWidget(p) {
	form = -1;
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void xAYEnvView::setForm(int f) {
	if (f >= 0) f &= 0x0f;		// -1 means there is no chip to have a form
	if (f == form) return;
	form = f;
	update();
}

QSize xAYEnvView::minimumSizeHint() const {
	int h = fontMetrics().height();
	return QSize(h * 6, h * 3 / 2);
}

void xAYEnvView::paintEvent(QPaintEvent*) {
	if (form < 0) return;
	unsigned char lev[AYENV_STEPS];
	ay_env_shape(form, lev, AYENV_STEPS);
	QPainter pnt(this);
	pnt.setPen(palette().color(QPalette::WindowText));
	// one pixel column per step, the level running bottom to top
	double dx = (double)(width() - 1) / AYENV_STEPS;
	double dy = (double)(height() - 3) / 31;
	QPolygonF line;
	for (int i = 0; i < AYENV_STEPS; i++) {
		double y = 1 + (31 - lev[i]) * dy;
		line << QPointF(i * dx, y) << QPointF((i + 1) * dx, y);
	}
	pnt.setRenderHint(QPainter::Antialiasing, false);
	pnt.drawPolyline(line);
}

// The panel refreshes once per emulated frame, and a field being typed into must
// not be written back under the caret. Focus alone is not enough to stop it:
// a field only clicked on should go on following the machine like the rest of
// the page, or it would quietly freeze at whatever it held when it was clicked.
static void putValue(xHexSpin* xhs, int val) {
	if (xhs->hasFocus() && xhs->isModified()) return;
	// setValue() stops at the value but still restyles the field, and a style
	// sheet is re-applied whether or not it changed - forty of those a frame.
	// A blank field has to be written even at the value it already holds: that is
	// what takes the dashes off when a chip comes back.
	if (!xhs->isBlank() && (xhs->getValue() == val)) return;
	xhs->setValue(val);
}

// A register is written while the emulation thread may be clocking the chip, so
// it goes in under the same lock the machine runs under. Reading is left alone:
// the worst it can give is one inconsistent refresh, and taking the lock fifty
// times a second would stall the gui on the emulation.
static void pokeAY(aymChip* chip, int reg, int val) {
	emu_lock();
	ay_poke_reg(chip, reg, val);
	emu_unlock();
}

static void pokeFM(aymChip* chip, int reg, int val) {
	emu_lock();
	ym2203_poke_reg(chip, reg, val);
	emu_unlock();
}

// THE PAGE

xPSGPage::xPSGPage(QWidget* p):QWidget(p) {
	ui.setupUi(this);
	chip = nullptr;
	hold = false;

	// the minidump: two rows of eight, the row label saying where they start
	// the block keeps to its own width: the fields are narrow, and without this
	// the placeholder grows into the slack and drags the columns away from the label
	ui.wRegs->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
	QGridLayout* rlay = new QGridLayout(ui.wRegs);
	rlay->setContentsMargins(0, 0, 0, 0);
	rlay->setSpacing(2);
	for (int c = 0; c < 8; c++) {
		QLabel* lab = new QLabel(QString::number(c));
		lab->setAlignment(Qt::AlignCenter);
		rlay->addWidget(lab, 0, c + 1);
	}
	for (int r = 0; r < 2; r++) {
		rlay->addWidget(new QLabel(QString("#%0").arg(r * 8, 2, 16, QChar('0')).toUpper()), r + 1, 0);
		for (int c = 0; c < 8; c++) {
			xHexSpin* xhs = addSpin(nullptr, 0xff);
			regs[r * 8 + c] = xhs;
			rlay->addWidget(xhs, r + 1, c + 1);
			int n = r * 8 + c;
			connect(xhs, &xHexSpin::valueChanged, this, [this, n](int v){reg_edited(n, v);});
		}
	}

	// the channel table. A tone period is 12 bits, the noise one 5, the
	// envelope one a full 16
	static const int permax[5] = {0x0fff, 0x0fff, 0x0fff, 0x1f, 0xffff};
	QWidget* perHost[5] = {ui.wPerA, ui.wPerB, ui.wPerC, ui.wPerN, ui.wPerE};
	for (int i = 0; i < 5; i++) {
		per[i] = addSpin(perHost[i], permax[i]);
		connect(per[i], &xHexSpin::valueChanged, this, [this, i](int v){per_edited(i, v);});
	}
	QWidget* volHost[3] = {ui.wVolA, ui.wVolB, ui.wVolC};
	for (int i = 0; i < 3; i++) {
		vol[i] = addSpin(volHost[i], 0x0f);
		connect(vol[i], &xHexSpin::valueChanged, this, [this, i](int v){vol_edited(i, v);});
	}

	// the period, and beside it the note it works out as - one or the other is
	// shown, so the field can still be typed into whenever it is the one up
	QWidget* noteHost[4] = {ui.wPerA, ui.wPerB, ui.wPerC, ui.wPerE};
	for (int i = 0; i < 4; i++) {
		note[i] = new QLabel("-");
		note[i]->setAlignment(Qt::AlignCenter);
		noteHost[i]->layout()->addWidget(note[i]);
	}

	QWidget* levHost[3] = {ui.wLevA, ui.wLevB, ui.wLevC};
	QWidget* muteHost[3] = {ui.wMuteA, ui.wMuteB, ui.wMuteC};
	for (int i = 0; i < 3; i++) {
		lev[i] = new xLevelCell;
		QVBoxLayout* llay = new QVBoxLayout(levHost[i]);
		llay->setContentsMargins(0, 0, 0, 0);
		llay->addWidget(lev[i]);
		mute[i] = new QCheckBox;
		mute[i]->setToolTip("Silence this channel");
		QVBoxLayout* mlay = new QVBoxLayout(muteHost[i]);
		mlay->setContentsMargins(0, 0, 0, 0);
		mlay->addWidget(mute[i], 0, Qt::AlignCenter);
		connect(mute[i], &QCheckBox::toggled, this, [this, i](bool on){mute_toggled(i, on);});
	}

	envView = new xAYEnvView;
	QVBoxLayout* elay = new QVBoxLayout(ui.wEnvForm);
	elay->setContentsMargins(0, 0, 0, 0);
	elay->addWidget(envView, 0, Qt::AlignLeft);
	connect(ui.cbNotes, SIGNAL(stateChanged(int)), this, SLOT(notes_toggled(int)));
	ui.cbNotes->setChecked(conf.dbg.sndnotes);
	show_notes(conf.dbg.sndnotes);
}

// One editable hex field, sized to what it holds. `host`, when given, is a
// placeholder from the form the field takes the place of.
xHexSpin* xPSGPage::addSpin(QWidget* host, int max) {
	xHexSpin* xhs = new xHexSpin;
	xhs->setXFlag(XHS_FILL | XHS_AUTOW | XHS_BLANK);
	xhs->setMax(max);
	xhs->setAlignment(Qt::AlignCenter);
	if (host) {
		QVBoxLayout* lay = new QVBoxLayout(host);
		lay->setContentsMargins(0, 0, 0, 0);
		lay->addWidget(xhs);
	}
	return xhs;
}

void xPSGPage::setChip(aymChip* c) {
	chip = c;
}

// A register goes in the way a port write does, so everything the core derives
// from it is set too - and the whole page is re-read, since one register moves
// several fields.
void xPSGPage::reg_edited(int reg, int val) {
	if (hold || !chip) return;
	pokeAY(chip, reg, val);
	draw();
}

// A tone period is a register pair, the noise one a single register. The low
// byte goes first and the pair is briefly wrong, which the chip cannot hear: the
// period is only read when the counter comes round.
void xPSGPage::per_edited(int row, int val) {
	if (hold || !chip) return;
	switch (row) {
		case 3:					// noise
			pokeAY(chip, 6, val & 0x1f);
			break;
		case 4:					// envelope, a full word
			pokeAY(chip, 11, val & 0xff);
			pokeAY(chip, 12, (val >> 8) & 0xff);
			break;
		default:				// a tone channel, twelve bits
			pokeAY(chip, row * 2, val & 0xff);
			pokeAY(chip, row * 2 + 1, (val >> 8) & 0x0f);
			break;
	}
	draw();
}

// The envelope bit of the volume register is not on show here - it is the E of
// the mixer column - so keep whatever it holds.
void xPSGPage::vol_edited(int chan, int val) {
	if (hold || !chip) return;
	pokeAY(chip, 8 + chan, (chip->reg[8 + chan] & 0x10) | (val & 0x0f));
	draw();
}

// Muting is a bit in the channel the mixer reads while the machine runs, so it
// goes in under the same lock a register does.
void xPSGPage::mute_toggled(int chan, bool on) {
	if (hold || !chip) return;
	aymChan* ch[3] = {&chip->chanA, &chip->chanB, &chip->chanC};
	emu_lock();
	ch[chan]->mute = on ? 1 : 0;
	emu_unlock();
}

void xPSGPage::notes_toggled(int st) {
	if (hold) return;
	conf.dbg.sndnotes = (st == Qt::Checked) ? 1 : 0;
	show_notes(conf.dbg.sndnotes);
	draw();
}

// The period column is either the field or the note, never both: two numbers for
// one thing in a column this narrow reads as clutter.
void xPSGPage::show_notes(bool on) {
	for (int i = 0; i < 3; i++) per[i]->setVisible(!on);
	per[4]->setVisible(!on);
	for (int i = 0; i < 4; i++) note[i]->setVisible(on);
}

void xPSGPage::blank() {
	hold = true;
	for (int i = 0; i < 16; i++) regs[i]->setBlank();
	for (int i = 0; i < 5; i++) per[i]->setBlank();
	for (int i = 0; i < 3; i++) vol[i]->setBlank();
	hold = false;
	QLabel* labs[6] = {ui.labMixA, ui.labMixB, ui.labMixC,
			ui.labStateA, ui.labStateB, ui.labStateC};
	for (int i = 0; i < 6; i++) labs[i]->setText("-");
	for (int i = 0; i < 3; i++) lev[i]->setLevel(-1, 31);
	for (int i = 0; i < 4; i++) note[i]->setText("-");
	ui.labStateN->setText("-");
	ui.labVolE->setText("-");
	envView->setForm(-1);
}

void xPSGPage::draw() {
	if (!chip) {
		blank();
		return;
	}
	hold = true;
	for (int i = 0; i < 16; i++)
		putValue(regs[i], chip->reg[i]);
	for (int i = 0; i < 3; i++) {
		putValue(per[i], ((chip->reg[i * 2 + 1] << 8) | chip->reg[i * 2]) & 0x0fff);
		putValue(vol[i], chip->reg[8 + i] & 0x0f);
	}
	putValue(per[3], chip->reg[6] & 0x1f);
	putValue(per[4], (chip->reg[12] << 8) | chip->reg[11]);
	aymChan* chan[3] = {&chip->chanA, &chip->chanB, &chip->chanC};
	QLabel* mix[3] = {ui.labMixA, ui.labMixB, ui.labMixC};
	QLabel* state[3] = {ui.labStateA, ui.labStateB, ui.labStateC};
	for (int i = 0; i < 3; i++) {
		mix[i]->setText(getAYmix(chan[i]));
		lev[i]->setLevel(ay_chan_lev(chip, chan[i]), 31);
		state[i]->setText(chan[i]->lev ? "1" : "0");
		mute[i]->setChecked(chan[i]->mute);
		if (conf.dbg.sndnotes)
			note[i]->setText(noteName(ay_chan_freq(chip, chan[i])));
	}
	if (conf.dbg.sndnotes)			// hidden otherwise, and a log() a channel
		note[3]->setText(noteName(ay_env_freq(chip)));
	hold = false;
	ui.labStateN->setText(chip->chanN.lev ? "1" : "0");
	ui.labVolE->setText(gethexbyte(chip->chanE.vol & 0x1f));
	envView->setForm(chip->eForm);
}

// FM PAGE

static struct {
	int id;
	QString str;
} stNameTab[] = {
	{OPST_OFF, "OFF"},
	{OPST_ATK, "ATK"},
	{OPST_DEC, "DEC"},
	{OPST_SUS, "SUS"},
	{OPST_REL, "REL"},
	{-1, "?"}
};

static QString getOpStatusName(int id) {
	int i = 0;
	while ((stNameTab[i].id != -1) && (stNameTab[i].id != id))
		i++;
	return stNameTab[i].str;
}

// The columns, in the order the table shows them. `reg` is the register the
// field lives in, offset by the operator's own rofs; -1 marks a readout. The
// names are the ones the TFM manual uses, so DR rather than the DL of the
// sketch this table came from.
static const struct {
	const char* name;
	int reg;
	int shift;
	int mask;
} fmColTab[FMC_COUNT] = {
	{"DT",    0x30, 4, 0x07},
	{"MUL",   0x30, 0, 0x0f},
	{"Key",     -1, 0, 0},
	{"St",      -1, 0, 0},
	{"TL",    0x40, 0, 0x7f},
	{"RS",    0x50, 6, 0x03},
	{"AR",    0x50, 0, 0x1f},
	{"DR",    0x60, 0, 0x1f},
	{"SL",    0x80, 4, 0x0f},
	{"SR",    0x70, 0, 0x1f},
	{"RR",    0x80, 0, 0x0f},
	{"EG",    0x90, 0, 0x0f},
	{"Bk/Fq",   -1, 0, 0},
	{"Lev",     -1, 0, 0},
	{"Out",     -1, 0, 0}
};

// Which operators reach the output, per algorithm, bit 0 being operator 1.
// Operator 4 always does; the rest are s_algorithm_ops in ymfm_fm.ipp.
static const int fmCarrier[8] = {0x8, 0x8, 0x8, 0x8, 0xa, 0xe, 0xe, 0xf};

xFMPage::xFMPage(QWidget* p):QWidget(p) {
	ui.setupUi(this);
	chip = nullptr;
	hold = false;

	chanTabs = new QTabBar;
	chanTabs->setDrawBase(false);
	chanTabs->addTab("A");
	chanTabs->addTab("B");
	chanTabs->addTab("C");
	QVBoxLayout* clay = new QVBoxLayout(ui.wChanTabs);
	clay->setContentsMargins(0, 0, 0, 0);
	clay->addWidget(chanTabs);

	QGridLayout* lay = new QGridLayout(ui.wOpTable);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setHorizontalSpacing(6);
	lay->setVerticalSpacing(2);
	for (int c = 0; c < FMC_COUNT; c++) {
		QLabel* lab = new QLabel(fmColTab[c].name);
		lab->setAlignment(Qt::AlignCenter);
		lay->addWidget(lab, 0, c + 1);
	}
	for (int o = 0; o < 4; o++) {
		lay->addWidget(new QLabel(QString("Op%0").arg(o + 1)), o + 1, 0);
		for (int c = 0; c < FMC_COUNT; c++) {
			opEdit[o][c] = nullptr;
			opLab[o][c] = nullptr;
			if (c == FMC_LEV) {
				opLev[o] = new xLevelCell;
				opLev[o]->setDigits(3);
				lay->addWidget(opLev[o], o + 1, c + 1);
			} else if (fmColTab[c].reg < 0) {
				QLabel* lab = new QLabel("-");
				lab->setAlignment(Qt::AlignCenter);
				opLab[o][c] = lab;
				lay->addWidget(lab, o + 1, c + 1);
			} else {
				xHexSpin* xhs = new xHexSpin;
				xhs->setXFlag(XHS_FILL | XHS_AUTOW | XHS_BLANK);
				xhs->setMax(fmColTab[c].mask);
				xhs->setAlignment(Qt::AlignCenter);
				opEdit[o][c] = xhs;
				lay->addWidget(xhs, o + 1, c + 1);
				connect(xhs, &xHexSpin::valueChanged, this, [this, o, c](int v){op_edited(o, c, v);});
			}
		}
	}
	lay->setColumnStretch(FMC_COUNT + 1, 1);

	connect(chanTabs, &QTabBar::currentChanged, this, &xFMPage::chan_changed);
	connect(ui.fmChanOff, SIGNAL(stateChanged(int)), this, SLOT(offChan(int)));
}

void xFMPage::setChip(aymChip* c) {
	chip = c;
}

int xFMPage::channel() const {
	int c = chanTabs->currentIndex();
	return (c < 0) ? 0 : (c % 3);
}

void xFMPage::chan_changed(int) {
	if (hold) return;
	draw();
}

void xFMPage::offChan(int st) {
	if (hold || !chip) return;
	emu_lock();
	chip->fm_off[channel()] = (st == Qt::Checked);
	emu_unlock();
}

// A cell holds one field of one register: the rest of that register has to come
// back untouched, and the write goes the way a port write does.
void xFMPage::op_edited(int op, int col, int val) {
	if (hold || !chip) return;
	// where an operator keeps its registers is in the view, so take a fresh
	// one: this can be the first thing that happens to the page
	ym2203_fm_view(chip, fmView);
	int reg = fmColTab[col].reg + fmView[channel()].op[op].rofs;
	int m = fmColTab[col].mask << fmColTab[col].shift;
	pokeFM(chip, reg, (chip->reg[reg] & ~m) | ((val << fmColTab[col].shift) & m));
	draw();
}

void xFMPage::blank() {
	hold = true;
	for (int o = 0; o < 4; o++) {
		opLev[o]->setLevel(-1, 0x3ff);
		for (int c = 0; c < FMC_COUNT; c++) {
			if (opEdit[o][c]) opEdit[o][c]->setBlank();
			if (opLab[o][c]) opLab[o][c]->setText("-");
		}
	}
	hold = false;
	ui.labTimerA->setText("Timer A -");
	ui.labTimerB->setText("Timer B -");
	ui.labCh3->setText("Ch3 -");
	ui.labAlg->setText("Alg -");
	ui.labFb->setText("Fb -");
	ui.labBkFq->setText("Bk/Fq -");
	ui.labChanOut->setText("Out -");
}

void xFMPage::draw() {
	if (!chip || (chip->type != SND_YM2203)) {
		blank();
		return;
	}
	// the state lives in the core, so ask for a copy of it first
	ym2203_fm_view(chip, fmView);
	int c = channel();
	fmChan* ch = &fmView[c];

	// Timers and the mode register. Timer A is 10 bits across two registers,
	// timer B one; bits 0 and 1 of the mode register are what starts them.
	int mode = chip->reg[0x27];
	ui.labTimerA->setText(QString("Timer A %0 %1")
		.arg(gethexword(((chip->reg[0x24] << 2) | (chip->reg[0x25] & 3)) & 0x3ff))
		.arg((mode & 1) ? "on" : "off"));
	ui.labTimerB->setText(QString("Timer B %0 %1")
		.arg(gethexbyte(chip->reg[0x26]))
		.arg((mode & 2) ? "on" : "off"));
	static const char* ch3mode[4] = {"off", "special", "CSM", "?"};
	ui.labCh3->setText(QString("Ch3 %0").arg(ch3mode[(mode >> 6) & 3]));

	ui.labAlg->setText(QString("Alg %0").arg(ch->algo));
	ui.labFb->setText(QString("Fb %0").arg((chip->reg[0xb0 + c] >> 3) & 7));
	ui.labBkFq->setText(QString("Bk/Fq %0:%1")
		.arg((chip->reg[0xa4 + c] >> 3) & 7)
		.arg(gethexword(((chip->reg[0xa4 + c] & 7) << 8) | chip->reg[0xa0 + c])));
	ui.labChanOut->setText(QString("Out %0").arg(ch->out));

	hold = true;
	ui.fmChanOff->setChecked(chip->fm_off[c]);
	int carrier = fmCarrier[ch->algo & 7];
	for (int o = 0; o < 4; o++) {
		fmOper* op = &ch->op[o];
		for (int col = 0; col < FMC_COUNT; col++) {
			if (!opEdit[o][col]) continue;
			int reg = chip->reg[fmColTab[col].reg + op->rofs];
			putValue(opEdit[o][col], (reg >> fmColTab[col].shift) & fmColTab[col].mask);
		}
		opLab[o][FMC_KEY]->setText(op->key ? "1" : "0");
		opLab[o][FMC_ST]->setText(getOpStatusName(op->eg.state));
		opLab[o][FMC_BKFQ]->setText(QString("%0:%1").arg(op->pg.block).arg(gethexword(op->pg.freq)));
		// the core keeps an attenuation, 0 loudest; the column says Lev, so
		// turn it round - loud is a big number, the way Unreal shows it
		opLev[o]->setLevel(0x3ff - (op->eg.att & 0x3ff), 0x3ff);
		opLab[o][FMC_OUT]->setText((carrier & (1 << o)) ? "*" : "");
	}
	hold = false;
}

// WAVE

xWaveView::xWaveView(QWidget* p):QWidget(p) {
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QSize xWaveView::minimumSizeHint() const {
	int h = fontMetrics().height();
	return QSize(h * 8, h * 3);
}

void xWaveView::paintEvent(QPaintEvent*) {
	int n = width();
	if ((n < 2) || (height() < 4)) return;
	buf.resize(n);			// kept across paints: this runs once a frame
	n = snd_scope(buf.data(), n);
	if (n < 2) return;
	// Fitted to what is in the window, not to what the device takes: at full
	// scale an ordinary AY tune is a flat line, and an AY never swings below
	// zero at all, so half the box would always be empty. The span has a floor
	// so silence is not magnified into noise, the zero line is drawn where zero
	// really falls, and the peak is written in the corner: the scale is never a
	// secret.
	int lo = buf[0].left;
	int hi = lo;
	for (int i = 0; i < n; i++) {
		int v = (buf[i].left + buf[i].right) / 2;
		buf[i].left = v;		// the mix, kept for the second pass
		if (v < lo) lo = v;
		if (v > hi) hi = v;
	}
	int peak = (-lo > hi) ? -lo : hi;
	if ((hi - lo) < 0x400) {
		int mid = (hi + lo) / 2;
		lo = mid - 0x200;
		hi = mid + 0x200;
	}
	int pad = (hi - lo) / 16;
	lo -= pad;
	hi += pad;
	int span = hi - lo;
	int last = height() - 1;
	QPainter pnt(this);
	QColor txt = palette().color(QPalette::WindowText);
	QColor line = txt;
	line.setAlpha(80);
	if ((lo <= 0) && (hi >= 0)) {
		int y = last - (0 - lo) * last / span;
		pnt.setPen(line);
		pnt.drawLine(0, y, width() - 1, y);
	}
	QPolygon trace;
	trace.reserve(n);
	for (int i = 0; i < n; i++)
		trace << QPoint(i, last - (buf[i].left - lo) * last / span);
	pnt.setPen(txt);
	pnt.drawPolyline(trace);
	pnt.setPen(line);
	pnt.drawText(rect().adjusted(0, 1, -2, 0), Qt::AlignRight | Qt::AlignTop, gethexword(peak));
}

// PANEL

xSndPanel::xSndPanel(QWidget* p):QWidget(p) {
	hold = false;
	layout_sig = -1;
	lastBeep = -1;

	QVBoxLayout* lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);

	QHBoxLayout* head = new QHBoxLayout;
	head->setContentsMargins(0, 0, 0, 0);
	tabs = new QTabBar;
	tabs->setDrawBase(false);
	tbDetach = new QToolButton;
	tbDetach->setIcon(QIcon(":/images/display.png"));
	tbDetach->setToolTip("Show in its own window");
	tbDetach->setCheckable(true);
	head->addWidget(tabs);
	head->addStretch(1);
	head->addWidget(tbDetach);
	lay->addLayout(head);

	stack = new QStackedWidget;
	psg = new xPSGPage;
	fm = new xFMPage;
	stack->addWidget(psg);
	stack->addWidget(fm);
	lay->addWidget(stack, 0);

	QHBoxLayout* foot = new QHBoxLayout;
	foot->addWidget(new QLabel("Wave"));
	foot->addStretch(1);
	foot->addWidget(new QLabel("Beeper"));
	labBeep = new QLabel;
	foot->addWidget(labBeep);
	lay->addLayout(foot);

	wave = new xWaveView;
	lay->addWidget(wave, 1);

	connect(tabs, &QTabBar::currentChanged, this, &xSndPanel::tab_changed);
	connect(tbDetach, &QToolButton::clicked, this, &xSndPanel::s_detach);
}

aymChip* xSndPanel::chipAt(int idx) const {
	return ts_chip(conf.zx->ts, idx);
}

// What the tab row depends on: how many chips there are and which of them have
// an FM half. Two bits per chip is enough to notice any change.
int xSndPanel::machine_sig() const {
	int sig = 0;
	for (int i = 0; i < 4; i++) {
		aymChip* chp = chipAt(i);
		sig |= ((chp->type == SND_NONE) ? 0 : (chp->type == SND_YM2203) ? 2 : 1) << (i * 2);
	}
	return sig;
}

void xSndPanel::build_tabs() {
	hold = true;
	while (tabs->count() > 0)
		tabs->removeTab(0);
	for (int i = 0; i < 4; i++) {
		aymChip* chp = chipAt(i);
		if (chp->type == SND_NONE) continue;
		int tab = tabs->addTab(QString("PSG%0").arg(i + 1));
		tabs->setTabData(tab, SND_TAB_ID(i, 0));
		if (chp->type != SND_YM2203) continue;
		tab = tabs->addTab(QString("FM%0").arg(i + 1));
		tabs->setTabData(tab, SND_TAB_ID(i, 1));
	}
	layout_sig = machine_sig();
	hold = false;
	show_tab(SND_TAB_ID(conf.dbg.sndchip, conf.dbg.sndfm));
}

// Point the pages at the chip a tab names and bring the right one up. A tab the
// machine no longer has falls back to the first one there is.
void xSndPanel::show_tab(int id) {
	int idx = -1;
	for (int i = 0; i < tabs->count(); i++) {
		if (tabs->tabData(i).toInt() == id) {
			idx = i;
			break;
		}
	}
	if ((idx < 0) && (tabs->count() > 0)) {
		idx = 0;
		id = tabs->tabData(0).toInt();
	}
	if (idx < 0) {			// no sound chip at all: leave the setting be
		psg->setChip(nullptr);
		fm->setChip(nullptr);
		return;
	}
	conf.dbg.sndchip = SND_TAB_CHIP(id);
	conf.dbg.sndfm = SND_TAB_FM(id);
	aymChip* chp = chipAt(conf.dbg.sndchip);
	psg->setChip(chp);
	fm->setChip(chp);
	stack->setCurrentWidget(conf.dbg.sndfm ? (QWidget*)fm : (QWidget*)psg);
	bool was = hold;
	hold = true;
	tabs->setCurrentIndex(idx);
	hold = was;
}

void xSndPanel::tab_changed(int idx) {
	if (hold || (idx < 0)) return;
	show_tab(tabs->tabData(idx).toInt());
	draw();
}

void xSndPanel::reload() {
	tbDetach->setChecked(conf.dbg.snddetach);
	build_tabs();
	draw();				// so it does not come up blank
}

void xSndPanel::draw() {
	// the machine may have been switched since the last refresh
	if (machine_sig() != layout_sig)
		build_tabs();
	if (conf.dbg.sndfm) {
		fm->draw();
	} else {
		psg->draw();
	}
	// setPixmap() has no "same again" shortcut the way setText() does, and it
	// invalidates the layout all the way up, so only draw it when it moved
	if (conf.zx->beep->val != lastBeep) {
		lastBeep = conf.zx->beep->val;
		drawHBar(labBeep, lastBeep, 256);
	}
	wave->update();
}

// DOCK

xSndWidget::xSndWidget(QString i, QString t, QWidget* p):xDockWidget(i,t,p) {
	QWidget* wid = new QWidget;
	setWidget(wid);
	setObjectName("AYWIDGET");	// the name a saved dock layout knows it by

	QVBoxLayout* lay = new QVBoxLayout(wid);
	lay->setContentsMargins(5, 5, 5, 5);
	panel = new xSndPanel;
	lay->addWidget(panel);

	// what the dock holds while the window has the panel: without it the
	// tab would still be there and empty, with nothing saying why
	gone = new QWidget;
	QVBoxLayout* glay = new QVBoxLayout(gone);
	QLabel* lab = new QLabel("The sound chips are in a window of their own.");
	lab->setAlignment(Qt::AlignCenter);
	lab->setWordWrap(true);
	QPushButton* btn = new QPushButton("Bring them back");
	glay->addStretch(1);
	glay->addWidget(lab);
	glay->addWidget(btn, 0, Qt::AlignCenter);
	glay->addStretch(1);
	lay->addWidget(gone);
	gone->hide();

	connect(panel, &xSndPanel::s_detach, this, &xSndWidget::s_detach);
	connect(btn, &QPushButton::clicked, this, [this](){emit s_detach(false);});
}

void xSndWidget::setDetached(bool on) {
	panel->setVisible(!on);
	gone->setVisible(on);
	if (!on) panel->reload();
}

void xSndWidget::draw() {
	if (panel->isVisible())
		panel->draw();
}

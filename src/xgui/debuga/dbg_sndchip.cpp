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

// How many refreshes a full bar takes to fall to nothing. A fifth of a second
// at fifty a second: slow enough not to flicker, quick enough to follow a note.
#define LEV_FALL	10

// The row labels down the left of both tables get the same width, so the two
// pages line up with each other when the tab row swaps them. Wide enough for
// the longest of them, which is Op1 on the FM side.
#define SND_LABEL_CELLS	4

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
	fig = -1;
	top = 1;
	digits = 2;
	setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
}

// A meter rises at once and falls slowly, the way every meter does. What it is
// given on a running machine is already the loudest the chip saw since the last
// refresh (ay_chan_peak), so this is not there to catch the square wave - it is
// what carries a note across the refreshes between its attack and its decay. A
// machine standing still wants neither: the figure beside the bar is the exact
// value and the bar has to agree with it.
void xLevelCell::setLevel(int val, int max, bool fall) {
	int step = (max > 0) ? max : 1;
	top = step;
	step /= LEV_FALL;
	if (step < 1) step = 1;
	int want;
	if (!fall || (val >= lev)) {
		want = val;		// rises at once, and -1 for no chip lands here too
	} else {
		want = lev - step;
		if (want < val) want = val;
	}
	if (want == lev) return;
	lev = want;
	update();
}

void xLevelCell::setFigure(int val) {
	if (val == fig) return;
	fig = val;
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
	// nothing at all where there is no figure to show: on a running machine the
	// bar is the whole readout, and a dash sitting in it only breaks the picture
	QString txt;
	if (fig >= 0)
		txt = formbufword(fig).rightJustified(digits, '0');

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

// Whether the machine is standing still. Half the panel reads differently when it
// is - a value that is a coin toss on a running machine is worth showing here -
// so it is one answer, not one per widget.
static bool machineHeld() {
	return conf.zx->flgDBG || conf.emu.pause;
}

// How wide the row labels are, in both tables.
static int labelWidth(const QWidget* w) {
	QFontMetrics fm(w->font());
	return fm.horizontalAdvance(QString(SND_LABEL_CELLS, '0'));
}

// PSG PAGE

static QString getAYmix(aymChan* ch) {
	QString res = ch->tdis ? "-" : "T";
	res += ch->ndis ? "-" : "N";
	res += ch->een ? "E" : "-";
	return res;
}

// ENVELOPE SHAPE

// Always two ramps of it, whatever the form: a ramp is then the same slope on every
// shape, which is what tells them apart. That is one round of the shapes that go up
// and down, two of the ones that saw, and a ramp and its tail for the ones that run
// once and stop - the eight figures the datasheet draws.
#define AYENV_STEPS	64		// two ramps of 32 levels

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
	return QSize(h * 6, h * 2);
}

void xAYEnvView::paintEvent(QPaintEvent*) {
	if (form < 0) return;
	unsigned char lev[AYENV_STEPS];
	int steps = AYENV_STEPS;
	ay_env_shape(form, lev, steps);
	QPainter pnt(this);
	pnt.setPen(palette().color(QPalette::WindowText));
	// A point per step, joined - not a stair per step. The shape is straight lines
	// either way, and squaring off all 32 levels of a ramp only turns it into a
	// staircase a few pixels tall.
	double dx = (double)(width() - 1) / (steps - 1);
	double dy = (double)(height() - 3) / 31;
	QPolygonF line;
	for (int i = 0; i < steps; i++)
		line << QPointF(i * dx, 1 + (31 - lev[i]) * dy);
	pnt.setRenderHint(QPainter::Antialiasing, true);
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
	ui.gridPsg->setColumnMinimumWidth(0, labelWidth(this));
	// the row spacing the FM table has: five rows at the style's own pitch left
	// the page loose beside it, and every pixel of it comes off the oscillogram
	ui.gridPsg->setHorizontalSpacing(6);
	ui.gridPsg->setVerticalSpacing(2);
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

	// The bar and the figure are one cell and one number: the figure is what the
	// channel is putting out at this instant, the bar is the peak of it, which is
	// how loud the channel is. Beside the volume they looked like two answers to
	// one question, and the volume register is not on the same scale anyway.
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
}

// One editable hex field, sized to what it holds. `host`, when given, is a
// placeholder from the form the field takes the place of.
xHexSpin* xPSGPage::addSpin(QWidget* host, int max) {
	xHexSpin* xhs = new xHexSpin;
	xhs->setXFlag(XHS_FILL | XHS_AUTOW | XHS_BLANK);
	xhs->setMax(max);
	xhs->setAlignment(Qt::AlignCenter);
	if (host) {
		QHBoxLayout* lay = new QHBoxLayout(host);
		lay->setContentsMargins(0, 0, 0, 0);
		lay->addWidget(xhs);
		// the noise row has no note beside it, and one item on its own would
		// sit in the middle of the cell instead of under the column
		lay->addStretch(1);
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

void xPSGPage::blank() {
	hold = true;
	for (int i = 0; i < 16; i++) regs[i]->setBlank();
	for (int i = 0; i < 5; i++) per[i]->setBlank();
	for (int i = 0; i < 3; i++) vol[i]->setBlank();
	hold = false;
	QLabel* labs[6] = {ui.labMixA, ui.labMixB, ui.labMixC,
			ui.labStateA, ui.labStateB, ui.labStateC};
	for (int i = 0; i < 6; i++) labs[i]->setText("-");
	for (int i = 0; i < 3; i++) {
		lev[i]->setLevel(-1, 31, false);
		lev[i]->setFigure(-1);
	}
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
	// The tone and noise generators free-run at a rate nothing on this page can
	// sample: a tone counter left as a reset leaves it flips at over 100 kHz, and
	// even a musical note is thousands of times a refresh. Read while the machine
	// runs the bit is a coin toss, so it is only shown when the machine is held -
	// stepping, which is the one place the bit means anything.
	bool held = machineHeld();
	for (int i = 0; i < 3; i++) {
		mix[i]->setText(getAYmix(chan[i]));
		state[i]->setText(!held ? "-" : (chan[i]->lev ? "1" : "0"));
		// what the chip is being handed at this instant: the volume with the tone
		// bit, the noise bit and the envelope applied the way the mixer has them.
		// The figure is only readable on a held machine; the bar is its peak, which
		// is readable either way.
		int out = ay_chan_lev(chip, chan[i]);
		// the bar takes the peak the chip itself kept since the last refresh:
		// asking once a frame lands on one half of the square wave or the other
		// and the bar drops to nothing under a note that is still playing
		lev[i]->setLevel(held ? out : ay_chan_peak(chan[i]), 31, !held);
		lev[i]->setFigure(held ? out : -1);
		mute[i]->setChecked(chan[i]->mute);
	}
	hold = false;
	ui.labStateN->setText(!held ? "-" : (chip->chanN.lev ? "1" : "0"));
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
	{"",        -1, 0, 0}	// the carrier mark: the star says it
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
	// 1 2 3, not A B C: the mode register calls the third one Ch3, and the
	// data sheet numbers them
	chanTabs->addTab("1");
	chanTabs->addTab("2");
	chanTabs->addTab("3");
	QVBoxLayout* clay = new QVBoxLayout(ui.wChanTabs);
	clay->setContentsMargins(0, 0, 0, 0);
	clay->addWidget(chanTabs);

	QGridLayout* lay = new QGridLayout(ui.wOpTable);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setHorizontalSpacing(3);
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
	lay->setColumnMinimumWidth(0, labelWidth(this));
	lay->setColumnStretch(FMC_COUNT + 1, 1);

	fitHeaders();
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

// Every readout above the table is as wide as its widest value from the start.
// They are laid out side by side, so one of them growing by a character - Out
// going negative, Ch3 turning to "special" - pushes the row out and takes the
// window with it, which it then keeps for the rest of the session.
void xFMPage::fitHeaders() {
	QFontMetrics fm(font());
	struct { QLabel* lab; const char* wide; } tab[] = {
		{ui.labTimerA, "Timer A FFFF off"},
		{ui.labTimerB, "Timer B FF off"},
		{ui.labCh3, "Ch3 special"},
		{ui.labAlg, "Alg 7"},
		{ui.labFb, "Fb 7"},
		{ui.labBkFq, "Bk/Fq 7:FFFF"},
		{ui.labChanOut, "Out -32768"}
	};
	for (int i = 0; i < (int)(sizeof(tab) / sizeof(tab[0])); i++)
		tab[i].lab->setMinimumWidth(fm.horizontalAdvance(tab[i].wide));
}

void xFMPage::changeEvent(QEvent* ev) {
	if (ev->type() == QEvent::FontChange) fitHeaders();
	QWidget::changeEvent(ev);
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
		opLev[o]->setLevel(-1, 0x3ff, false);
		opLev[o]->setFigure(-1);
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
		opLab[o][FMC_ST]->setText(getOpStatusName(op->eg.state));
		opLab[o][FMC_BKFQ]->setText(QString("%0:%1").arg(op->pg.block).arg(gethexword(op->pg.freq)));
		// the core keeps an attenuation, 0 loudest; the column says Lev, so
		// turn it round - loud is a big number, the way Unreal shows it
		int elev = 0x3ff - (op->eg.att & 0x3ff);
		opLev[o]->setLevel(elev, 0x3ff, false);	// already steady, no need to hold it
		opLev[o]->setFigure(elev);
		opLab[o][FMC_OUT]->setText((carrier & (1 << o)) ? "*" : "");
	}
	hold = false;
}

// WAVE

// How much time the box holds. Running: two frames, so a tune moves across it.
// Held: two milliseconds, fine enough to count the edges of a beeper routine.
#define WAVE_SECS_RUN	0.04
#define WAVE_SECS_HELD	0.002

// How much the box covers either way from the middle, and it never changes: a scale
// that follows the sound rescales the picture under it, so a passage getting louder
// walks up and down the box instead of simply growing.
//
// Drawn on a curve rather than straight, because the chips cover a range no straight
// scale shows at once: at a scale a loud beeper fits in, a quiet tune is a flicker
// along the middle. The knee says how much the quiet end is lifted - loud still
// reads as loud, and the order of two levels never changes.
//
// It is the top of the fitted range on purpose, so turning Fit on only ever
// zooms in on quiet material and never widens the box.
#define WAVE_FULL	0x4000
#define WAVE_KNEE	32.0

// What the box is worth either way from the middle. Fitted to the wave it steps
// by octaves and only shrinks once the wave is well inside, so it holds still
// while the music stays within a couple of steps - and the scale down the side
// says which one it is on. Unfitted it never moves at all.
#define WAVE_SCALE_MIN	0x0100
#define WAVE_SCALE_MAX	0x8000

xWaveView::xWaveView(QWidget* p):QWidget(p) {
	peak = 0;
	scale = WAVE_SCALE_MIN;
	rulw = -1;
	held = false;
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

// The scale is marked at the top of the box and five halvings of it, in the same
// numbers as the peak in the label, so one can be read against the other.
#define WAVE_MARKS	6


// Asked for three times a frame, and it shapes text: measure it once and again
// when the font changes under it.
int xWaveView::ruler() const {
	if (rulw < 0)
		rulw = fontMetrics().horizontalAdvance(QStringLiteral("0000")) + 6;
	return rulw;
}

void xWaveView::changeEvent(QEvent* ev) {
	if (ev->type() == QEvent::FontChange) rulw = -1;
	QWidget::changeEvent(ev);
}

QSize xWaveView::minimumSizeHint() const {
	int h = fontMetrics().height();
	return QSize(h * 8 + ruler(), h * 3);
}

// Reading the capture and painting it are split: the window is taken once per
// refresh, which is once per emulated frame, and an expose only redraws what was
// taken. It also lets the panel put the peak in a label of its own - drawn in the
// corner of the picture it sat right where a quiet machine puts its line.
void xWaveView::sample() {
	int cols = width() - ruler();
	peak = 0;
	if (cols < 2) return;		// narrower than its own scale
	cmin.assign(cols, 0);
	cmax.assign(cols, 0);
	cavg.assign(cols, 0);
	// Two timebases, and which one is in use follows the machine rather than a
	// switch: running, the box holds a couple of frames, which is what music looks
	// like; held, it holds a couple of milliseconds, which is where the edges of a
	// beeper or a digital routine can be counted.
	held = machineHeld();
	int want = (int)(snd_scope_rate() * (held ? WAVE_SECS_HELD : WAVE_SECS_RUN));
	if (want < cols) want = cols;
	buf.resize(want);
	want = snd_scope(buf.data(), want);
	if (want < 2) return;
	// the lowest and highest sample in each pixel column, the way any audio editor
	// draws a wave. One sample per column instead lands each column on its own phase
	// of the waveform, and a steady square then draws as a picket with a slow wobble
	// that looks like the level moving.
	int lo = buf[0];
	int hi = lo;
	for (int c = 0; c < cols; c++) {
		int a = (int)((long long)c * want / cols);
		int b = (int)((long long)(c + 1) * want / cols);
		if (b <= a) b = a + 1;
		if (b > want) b = want;
		int mn = buf[a];
		int mx = mn;
		long long sum = 0;
		for (int i = a; i < b; i++) {
			if (buf[i] < mn) mn = buf[i];
			if (buf[i] > mx) mx = buf[i];
			sum += buf[i];
		}
		cmin[c] = mn;
		cmax[c] = mx;
		// The average of the column is the decimation proper, and for a beeper it
		// is the whole point: a digi is pulse width, so the carrier fills the box
		// from top to bottom in every column and only the average has the tune in
		// it. Min and max stay as the outline, which is where the peaks are.
		cavg[c] = (int)(sum / (b - a));
		if (mn < lo) lo = mn;
		if (mx > hi) hi = mx;
	}
	peak = qMax(-lo, hi);
	// the fitted scale doubles until the wave is in and halves once it is well
	// inside, so it holds still while the music stays within a couple of steps
	while ((scale < peak) && (scale < WAVE_SCALE_MAX)) scale *= 2;
	while ((scale > WAVE_SCALE_MIN) && (peak < scale / 4)) scale /= 2;
	// The column average is only a box filter, and a box leaks: a tone above what
	// the box can show - a beeper carrier at 40ms, say - comes back as a slow ripple
	// along the middle. Two smoothing passes over the averages take that down to a
	// few per cent. They are measured in pixels, not in time, so the same pass suits
	// both timebases; all it costs is a corner rounded over a pixel or two.
	for (int pass = 0; pass < 2; pass++) {
		int prev = cavg[0];
		for (int c = 1; c < cols - 1; c++) {
			int cur = cavg[c];
			cavg[c] = (prev + cur * 2 + cavg[c + 1]) / 4;
			prev = cur;
		}
	}
	// Each column is drawn as one stroke from its lowest sample to its highest,
	// and two strokes that do not overlap leave a gap between them: wherever the
	// wave moves further between columns than the stroke is tall, the line comes
	// apart into dashes. Stretch each column to meet the one before it - against
	// that column's own range, not the stretched one, so it cannot run away.
	int pmin = cmin[0];
	int pmax = cmax[0];
	for (int c = 1; c < cols; c++) {
		int mn = cmin[c];
		int mx = cmax[c];
		if (mn > pmax) cmin[c] = pmax;
		else if (mx < pmin) cmax[c] = pmin;
		pmin = mn;
		pmax = mx;
	}
}

void xWaveView::refresh() {
	sample();
	update();
}

QString xWaveView::info() const {
	double secs = held ? WAVE_SECS_HELD : WAVE_SECS_RUN;
	return QString("%0 %1ms").arg(gethexword(peak)).arg(qRound(secs * 1000));
}

void xWaveView::paintEvent(QPaintEvent*) {
	int cols = width() - ruler();
	if ((cols < 2) || (height() < 4)) return;
	if ((int)cmin.size() != cols) sample();		// resized since the last refresh
	// Zero is the middle of the box and stays there, and so does the scale: what
	// moves is the wave.
	int last = height() - 1;
	int mid = last / 2;
	int full = conf.dbg.sndfit ? scale : WAVE_FULL;
	bool curve = conf.dbg.sndlog;
	double kmul = WAVE_KNEE / (double)full;	// the curve, worked out once
	double kdiv = 1.0 / log1p(WAVE_KNEE);

	QPainter pnt(this);
	// Its own black box with a green trace, the way the tape diagram and the beeper
	// bar beside it are drawn: a scope is a scope whatever the rest of the window
	// is wearing, and a signal has to read the same in every style.
	pnt.fillRect(rect(), Qt::black);
	int x0 = ruler();
	auto ypos = [=](int v) {
		double a = qMin((v < 0) ? -(double)v : (double)v, (double)full);
		double t = curve ? log1p(a * kmul) * kdiv : a / full;
		int y = mid - (int)((v < 0 ? -t : t) * mid);
		return toLimits(y, 0, last);
	};

	// The scale: what the box is worth is no longer written on the wave, since the
	// wave no longer stretches to fill it - and the curve it is drawn on has to be
	// read off something. A mark either side of zero for each level, the figure on
	// the upper one.
	QColor grid(0x30, 0x30, 0x30);
	int step = fontMetrics().height();
	int shown = -step;
	for (int i = 0; i < WAVE_MARKS; i++) {
		int mark = full >> i;
		int y = ypos(mark);
		int y2 = ypos(-mark);
		pnt.setPen(grid);
		pnt.drawLine(x0 - 3, y, cols + x0 - 1, y);
		pnt.drawLine(x0 - 3, y2, cols + x0 - 1, y2);
		if ((y - shown) < step) continue;	// no room for this one
		shown = y;
		pnt.setPen(QColor(0x60, 0x60, 0x60));
		// the top mark sits on the edge: keep its figure inside the box
		int ty = toLimits(y - step / 2, 0, height() - step);
		pnt.drawText(QRect(0, ty, x0 - 5, step),
			Qt::AlignRight | Qt::AlignVCenter, gethexword(mark));
	}
	pnt.setPen(QColor(0x80, 0x20, 0x20));	// zero, always halfway up
	pnt.drawLine(x0 - 3, mid, cols + x0 - 1, mid);
	// the outline first, dim, then the average over it. One stroke per column,
	// but handed over in one call - the pen does not change along the way, and a
	// drawLine() each is several hundred trips through the painter per frame.
	QVector<QLine> bars;
	bars.reserve(cols);
	QPolygon trace;
	trace.reserve(cols);
	for (int c = 0; c < cols; c++) {
		bars << QLine(x0 + c, ypos(cmax[c]), x0 + c, ypos(cmin[c]));
		trace << QPoint(x0 + c, ypos(cavg[c]));
	}
	pnt.setPen(QColor(0x18, 0x60, 0x18));
	pnt.drawLines(bars);
	pnt.setPen(QColor(0x30, 0xd0, 0x30));
	pnt.drawPolyline(trace);
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
	labWave = new QLabel;
	foot->addWidget(labWave);
	foot->addSpacing(12);
	// how the box is drawn, beside the readout that says what it is showing
	cbFit = new QCheckBox("Fit");
	cbFit->setToolTip("Centre the trace and scale the box to it.\n"
					"Off shows the level as it is, on one fixed scale.");
	cbFit->setChecked(conf.dbg.sndfit);
	foot->addWidget(cbFit);
	cbLog = new QCheckBox("Log");
	cbLog->setToolTip("Draw the wave on a curve, so a quiet tune is\n"
					"still something to see beside a loud one.");
	cbLog->setChecked(conf.dbg.sndlog);
	foot->addWidget(cbLog);
	connect(cbFit, &QCheckBox::toggled, this, [this](bool on){
		conf.dbg.sndfit = on ? 1 : 0;
		wave->update();
	});
	connect(cbLog, &QCheckBox::toggled, this, [this](bool on){
		conf.dbg.sndlog = on ? 1 : 0;
		wave->update();
	});
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
	wave->refresh();
	labWave->setText(wave->info());
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

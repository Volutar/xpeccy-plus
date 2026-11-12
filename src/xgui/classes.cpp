#include "xgui.h"
#include "../xcore/xcore.h"

#include <QGuiApplication>
#include <QScreen>
#include <QPalette>
#include <QFile>
#include <QTextBrowser>
#include <QPainter>
#include <QStylePainter>
#include <QStyleOption>
#include <QHBoxLayout>
#include <QFrame>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStyleOptionComboBox>
#include <QApplication>
#include <QSet>
#include <QTabBar>
#include <QFontMetrics>
#include <QDebug>
#include <QToolTip>
#include <QCursor>
#include <QTimer>
#include <QContextMenuEvent>
#include <QCompleter>
#include <QStringListModel>
#include <QAbstractItemView>
#include <QHelpEvent>
#include <QPointer>
#include <QActionGroup>
#include <QMenu>
#include <climits>

QString gethexword(int);
QString gethexbyte(uchar);

// A combo box asks for much more width than its text needs, and its hint is
// cached until a style or font change. This is what it really needs: the
// widest item, the arrow and whatever frame the style draws around them.
int comboFitWidth(QComboBox* box) {
	QStyleOptionComboBox opt;
	opt.initFrom(box);
	opt.subControls = QStyle::SC_All;
	opt.rect = QRect(QPoint(0, 0), box->sizeHint());
	int field = box->style()->subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, box).width();
	QFontMetrics fm(box->font());
	int text = 0;
	for (int i = 0; i < box->count(); i++)
		text = qMax(text, fm.horizontalAdvance(box->itemText(i)));
	return opt.rect.width() - field + text + 4;	// 4: a pixel of air either side
}

// a faint tint of a background, the way item views alternate their rows: lighter
// on a dark one, darker on a light one
QColor bg_tint(const QColor& bg) {
	return (bg.lightness() < 128) ? bg.lighter(130) : bg.darker(107);
}

void setRegExp(QRegExpValidator& v, QString s) {
#if QT_VERSION < QT_VERSION_CHECK(6,0,0)
	v.setRegExp(QRegExp(s));
#else
	v.setRegularExpression(QRegularExpression(s));
#endif
}

xHexSpin::xHexSpin(QWidget* p):QLineEdit(p) {
	setMinimumWidth(60);
	setAutoFillBackground(true);
	setUpdatesEnabled(true);
	//vldtr.setRegExp(QRegExp(""));
	setRegExp(vldtr, "");
	min = 0x0000;
	max = 0xffff;
	value = 0x0000;
	hsflag = XHS_DEC;
	chgMask = 0;
	isblank = false;
	len = 6;
	hexLen = 4;
	dlen = 4;
	vtxt = "0000";
	// setValidator(&vldtr);
	setBase(16);
	setText(vtxt);
	connect(this, SIGNAL(textChanged(QString)), SLOT(onTextChange(QString)));
	// a decimal field emptied for typing and left so shows its value again
	connect(this, &QLineEdit::editingFinished, this, [this]() {
		if ((base == 10) && !isblank) {
			hsflag |= XHS_UPD;
			onChange(value);
		}
	});
}

int xHexSpin::getValue() {
	return value;
}

// digits of a value in a base: dividing, so a max near the top of int does not overflow
static int num_digits(int val, int base) {
	int len = 1;
	unsigned mx = (unsigned)val / base;
	while (mx) {
		mx /= base;
		len++;
	}
	return len;
}

// true when the width of the text moved: the text then needs redraw()
bool xHexSpin::updateMask() {
	int was = dlen;
	if (base != 10) base = 16;
	len = num_digits(max, base);
	hexLen = num_digits(max, 16);
	setRegExp(vldtr, QString((base == 10) ? "[0-9]{%0}" : "[A-Fa-f0-9]{%0}").arg(len));
	// Hex has a mask, which puts the cursor in overwrite mode; the part after
	// the semicolon is what an empty position shows, for a field that may be
	// empty. Decimal has none: its numbers are not padded with zeros, so it is
	// typed as text. Qt cuts or pads the text to a new mask and reports it as
	// textChanged: that text is not a value, so nobody hears it
	bool blk = blockSignals(true);
	if (base == 10) {
		setInputMask(QString());
		setPlaceholderText((hsflag & XHS_BLANK) ? QString(len, '-') : QString());
	} else {
		setPlaceholderText(QString());
		setInputMask((hsflag & XHS_BLANK) ? (QString(len, 'h') + ";-") : QString(len, 'h'));
	}
	blockSignals(blk);
	dlen = decPairs() ? (hexLen / 2) * 5 - 2 : len;
	refitWidth();
	return dlen != was;
}

// a decimal field lit byte by byte shows each byte as a number of its own:
// "  1  255" for 511, two spaces between - paintEvent draws the strip there
bool xHexSpin::decPairs() {
	return (base == 10) && (hsflag & XHS_SPLIT) && (hexLen >= 4) && !(hexLen & 1);
}

QString xHexSpin::numText(int val) {
	if (base != 10)
		return QString::number(val, base).toUpper().rightJustified(len, '0');
	if (decPairs()) {
		QStringList lst;
		for (int i = hexLen / 2 - 1; i >= 0; i--)
			lst.append(QString::number(((unsigned)val >> (i * 8)) & 0xff).rightJustified(3, ' '));
		return lst.join("  ");
	}
	QString res = QString::number(val);
	// a register stands to the right of its whole field, as wide as it is
	if (hsflag & XHS_NUMS) return res.rightJustified(regChars(), ' ');
	return (hsflag & XHS_FILL) ? res.rightJustified(len, ' ') : res;
}

// The text again from the value, after the mask or the base moved under it.
// Not a new value: no valueChanged, and the changed bytes stay lit.
void xHexSpin::redraw() {
	if (isblank) return;
	hsflag |= XHS_UPD;
	onChange(value);
	updatePal();		// whole field or byte by byte depends on the width
	update();
}

// XHS_AUTOW: 'len' digits + a bit for the frame and cursor.
// call it after a font change too, not only when the value length changes.
// A register (XHS_NUMS) is as wide as a pair, or as one byte of it, whatever
// it holds: PC lines up with HL edge to edge, IM with one half of a pair
int xHexSpin::regChars() {
	if (!(hsflag & XHS_NUMS)) return dlen;
	bool dec = (base == 10);
	if (max <= 0xff) return dec ? 3 : 2;
	if (max <= 0xffff) return !dec ? 4 : conf.dbg.regsplit ? 8 : 5;
	return dlen;
}

void xHexSpin::refitWidth() {
	if (!(hsflag & XHS_AUTOW)) return;
	int chars = regChars();
	// a register's decimal text is padded to the width: Split pairs moves it
	if ((hsflag & XHS_NUMS) && (base == 10) && !isblank && (text() != numText(value))) {
		hsflag |= XHS_UPD;
		onChange(value);
	}
	QFontMetrics fm(font());
	setFixedWidth(fm.horizontalAdvance(QString(chars, '0') + sgnPre + sgnPost) + 10);
}

void xHexSpin::setBase(int b) {
	base = b;
	updateSign();
	updateMask();
	redraw();			// other digits even at the same width
}

// XHS_ADDR / XHS_NUMS: the base the debugger shows; an address's sign is a
// painted margin (Backspace in front of the digits: decimal, # or $: hex)
void xHexSpin::numFmt() {
	if (!(hsflag & (XHS_ADDR | XHS_NUMS))) return;
	int b = (conf.dbg.numfmt == CPU_NUM_DEC) ? 10 : 16;
	int key = conf.dbg.numfmt * 16 + conf.dbg.hexfmt;
	if ((b == base) && (key == fmtSeen)) return;		// base and sign as they are
	fmtSeen = key;
	setBase(b);
}

void xHexSpin::updateSign() {
	sgnPre.clear();
	sgnPost.clear();
	if ((hsflag & XHS_ADDR) && (base == 16))
		dbg_sign(dbg_hexfmt(), &sgnPre, &sgnPost);
	QFontMetrics fm(font());
	setTextMargins(fm.horizontalAdvance(sgnPre), 0, fm.horizontalAdvance(sgnPost), 0);
}

void xHexSpin::showEvent(QShowEvent* ev) {
	numFmt();
	QLineEdit::showEvent(ev);
}

void xHexSpin::setXFlag(int xf) {
	hsflag = xf;
	if (updateMask()) redraw();	// XHS_BLANK decides what an empty position shows
}

// A field that may hold nothing shows the mask's blank character until it is
// given a value or the caret lands in it. Without XHS_BLANK there is no such
// state and this does nothing.
void xHexSpin::setBlank() {
	if (!(hsflag & XHS_BLANK) || isblank) return;
	isblank = true;
	setText(QString());
}

bool xHexSpin::isBlank() const {
	return isblank;
}

// there is something to show again: the text has to be forced out, since the
// value it is built from never moved
void xHexSpin::unblank() {
	if (!isblank) return;
	isblank = false;
	hsflag |= XHS_UPD;
}

// typing into an empty field starts from zero, so the overwrite cursor has
// something to overwrite
void xHexSpin::startEdit() {
	if (!isblank) return;
	unblank();
	onChange(value);
}

// A field that sizes itself to what it holds has to be re-measured whenever
// the font moves under it - a style sheet hands every widget a font of its own,
// and without this the last digit is clipped by however much the new font is
// wider than the one the width was worked out in.
void xHexSpin::changeEvent(QEvent* ev) {
	QLineEdit::changeEvent(ev);
	if (ev->type() == QEvent::FontChange) {
		updateSign();		// its margin is measured in the font too
		refitWidth();
	}
}

void xHexSpin::focusInEvent(QFocusEvent* ev) {
	startEdit();
	QLineEdit::focusInEvent(ev);
}

// The one flag that is switched while the field is alive, so it takes a setter
// of its own rather than having the caller restate the whole set.
void xHexSpin::setSplit(bool on) {
	int flg = on ? (hsflag | XHS_SPLIT) : (hsflag & ~XHS_SPLIT);
	if (flg == hsflag) return;
	hsflag = flg;
	if (base == 10) {
		updateMask();		// one number or one a byte: another text, another width
		redraw();
	}
	updatePal();		// the whole-field colour follows the setting too
}

// A field the caller keeps lit until it says otherwise, for a value marked for
// a reason of its own rather than because it just moved. It always lights
// whole: the per-byte highlight of XHS_SPLIT has nothing to say about it.
void xHexSpin::setLit(bool on) {
	int flg = on ? (hsflag | XHS_LIT) : (hsflag & ~XHS_LIT);
	if (flg == hsflag) return;
	hsflag = flg;
	updatePal();
}

// How many bytes the field shows separately, 0 = it does not. Hex lines digits
// up with bytes, decimal writes a number a byte (decPairs); only a centered
// field has its text where paintEvent works it out to be - the two margins
// cancel out there.
int xHexSpin::splitBytes() {
	if (!(hsflag & XHS_SPLIT)) return 0;
	if ((hexLen < 4) || (hexLen & 1)) return 0;
	if ((base != 16) && !decPairs()) return 0;
	if (!(alignment() & Qt::AlignHCenter)) return 0;
	return hexLen / 2;
}

// Something changed and it is the whole field that says so - either every byte
// moved, or this field does not show them apart. The style sheet draws that
// case; paintEvent draws the rest.
bool xHexSpin::wholeFieldLit() {
	if (hsflag & XHS_LIT) return true;
	int bytes = splitBytes();
	return chgMask && (!bytes || (chgMask == ((1 << bytes) - 1)));
}

int minMaxCorrect(int val, int min, int max) {
	if (val < min) return min;
	if (val > max) return max;
	return val;
}

void xHexSpin::setMin(int v) {
	min = v;
//	updateMask();		// no need if max is not updated
	if (value < min) setValue(min);
}

// a value that no longer fits moves first, at the old width, so it is drawn
// and announced the way any new value is
void xHexSpin::setMax(int v) {
	max = v;
	if (value > max) setValue(max);
	if (updateMask()) redraw();
}

int xHexSpin::getMax() {
	return max;
}

extern QString getStyleString(QString, QString, int = 0, int = 100);

void xHexSpin::updatePal() {
#if 1
	QString str;
	QWidget* p = parentWidget();
	// a field lit byte by byte keeps its normal colours here - paintEvent puts
	// the changed ones on top of them
	if (wholeFieldLit()) {
		str = getStyleString(DBG_PAL_CHG_BG, DBG_PAL_CHG_TXT);
		// scope the rule to the field: a bare property list would leak
		// into child widgets, e.g. the standard context menu
		if (!str.isEmpty()) str = QString("xHexSpin{%0}").arg(str);
	} else {
		str = p ? p->styleSheet() : ""; // getStyleString("dbg.input.bg", "dbg.input.txt");
	}
	// only what moved: either one repolishes the field, and this runs on every value.
	// The font after the sheet: the first sheet after the window is shown hands the
	// field the application font
	if (styleSheet() != str) setStyleSheet(str);
	if (p && (font() != p->font())) setFont(p->font());
#else
	QPalette pal;
	if (chgMask) {
		pal.setColor(QPalette::Base, conf.pal[DBG_PAL_CHG_BG].isValid() ? conf.pal[DBG_PAL_CHG_BG] : pal.toolTipBase().color());
		pal.setColor(QPalette::Text, conf.pal[DBG_PAL_CHG_TXT].isValid() ? conf.pal[DBG_PAL_CHG_TXT] : pal.toolTipText().color());
	} else {
		pal.setColor(QPalette::Base, conf.pal["dbg.input.bg"].isValid() ? conf.pal["dbg.input.bg"] : pal.base().color());
		pal.setColor(QPalette::Text, conf.pal["dbg.input.txt"].isValid() ? conf.pal["dbg.input.txt"] : pal.text().color());
	}
	setPalette(pal);
#endif
}

void xHexSpin::setValue(int nval) {
	nval = minMaxCorrect(nval, min, max);
	unblank();
	int oldMask = chgMask;
	if ((value == nval) && !(hsflag & XHS_UPD)) {
		chgMask = 0;
	} else {
		unsigned diff = value ^ nval;
		if (!diff) diff = ~0u;		// forced update: light the whole value
		value = nval;
		chgMask = 0;
		if (hsflag & XHS_BGR) {
			for (int i = 0; i < 4; i++) {
				if (diff & (0xffu << (i * 8))) chgMask |= 1 << i;
			}
		}
		emit valueChanged(nval);
		onChange(value);
	}
	// A field lit byte by byte keeps the style sheet it had, so updatePal()
	// below schedules no repaint of its own and the bytes painted last time
	// would stay on screen. Ask for one whenever the mask moved.
	if (chgMask != oldMask) update();
	updatePal();
}

// The whole field lit means the whole value changed; one byte of it means only
// that byte did. The base class has drawn the text in its normal colours by
// now, so the changed bytes go on top of it, background and digits both.
void xHexSpin::paintEvent(QPaintEvent* ev) {
	QLineEdit::paintEvent(ev);
	if (!isblank && (!sgnPre.isEmpty() || !sgnPost.isEmpty())) {		// dashes are no number
		// the sign in the margins setTextMargins keeps, by QLineEdit's text rect
		QFontMetrics fm = fontMetrics();
		QMargins mg = textMargins();
		QStyleOptionFrame opt;
		initStyleOption(&opt);
		QRect cr = style()->subElementRect(QStyle::SE_LineEditContents, &opt, this)
			.adjusted(2 + mg.left(), 0, -2 - mg.right(), 0);
		int tw = fm.horizontalAdvance(text());
		int tx = cr.left();
		if (alignment() & Qt::AlignRight) tx = cr.right() + 1 - tw;
		else if (alignment() & Qt::AlignHCenter) tx = cr.left() + (cr.width() - tw) / 2;
		QPainter pnt(this);
		pnt.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::Text));
		int y = cr.top() + (cr.height() + fm.ascent() - fm.descent()) / 2;
		if (!sgnPre.isEmpty()) pnt.drawText(tx - fm.horizontalAdvance(sgnPre), y, sgnPre);
		if (!sgnPost.isEmpty()) pnt.drawText(tx + tw, y, sgnPost);
	}
	int bytes = splitBytes();
	if (!bytes) return;
	QString txt = text();
	// a byte is two hex digits, or three decimal places with two spaces between
	bool dec = (base == 10);
	int cw = dec ? 3 : 2;
	int sp = dec ? 2 : 0;
	if (txt.size() != bytes * (cw + sp) - sp) return;		// mid-edit: nothing lines up
	QFontMetrics fm = fontMetrics();
	QRect cr = contentsRect();
	int tw = fm.horizontalAdvance(txt);
	int x0 = cr.x() + (cr.width() - tw) / 2;
	// each decimal byte gets the field's own frame and padding, as wide as IM
	int padL = x0;
	int padR = width() - (x0 + tw);
	int spw = sp ? fm.horizontalAdvance(QString(sp, ' ')) : 0;
	// each byte, from the left: where its digits are, and the part of the
	// field that is its own
	QVector<int> tx(bytes), tdw(bytes), from(bytes), to(bytes);
	int x = x0;
	for (int k = 0; k < bytes; k++) {
		tx[k] = x;
		tdw[k] = fm.horizontalAdvance(txt.mid(k * (cw + sp), cw));
		x += tdw[k] + spw;
	}
	from[0] = 0;
	to[bytes - 1] = width();
	for (int k = 0; k < bytes - 1; k++) {
		int a = tx[k] + tdw[k];		// hex: the bytes meet where the digits do
		int b = a;
		if (dec) {
			a += padR;
			b = tx[k + 1] - padL;
			if (b <= a) a = b = (tx[k] + tdw[k] + tx[k + 1]) / 2;
		}
		to[k] = a;
		from[k + 1] = b;
	}
	QPainter pnt(this);
	if (dec) {
		// the strip between two decimal bytes, in the panel's own colour
		QWidget* par = parentWidget();
		QColor gap = par ? par->palette().color(par->backgroundRole()) : palette().color(QPalette::Window);
		for (int k = 0; k < bytes - 1; k++) {
			int w = qMax(from[k + 1] - to[k], 1);
			pnt.fillRect(QRect(to[k], 0, w, height()), gap);
		}
	}
	if (!chgMask || wholeFieldLit()) return;
	QColor bg = conf.pal.value(DBG_PAL_CHG_BG);
	QColor fg = conf.pal.value(DBG_PAL_CHG_TXT);
	if (!bg.isValid() && !fg.isValid()) return;
	if (fg.isValid()) pnt.setPen(fg);
	// a changed byte lights all of its part of the field, inside the frame's
	// line. Byte 0 is the last one
	for (int k = 0; k < bytes; k++) {
		if (!(chgMask & (1 << (bytes - 1 - k)))) continue;
		int l = qMax(from[k], 1);
		int r = qMin(to[k], width() - 1);
		if (bg.isValid()) pnt.fillRect(QRect(l, 1, r - l, height() - 2), bg);
		pnt.drawText(QRect(tx[k], cr.y(), tdw[k], cr.height()), Qt::AlignCenter, txt.mid(k * (cw + sp), cw));
	}
}

void xHexSpin::onChange(int val) {
	int pos = cursorPosition();
	QString old = text();
	QString res = numText(val);
	if ((old != res) || (hsflag & XHS_UPD)) {
		hsflag &= ~XHS_UPD;
		setText(res);
		// decimal stands to the right: the caret keeps its place from the end
		setCursorPosition((base == 10) ? qMax(0, res.size() - (old.size() - pos)) : pos);
	}
}

// Decimal as typed: one number, or one a byte with decPairs. A field emptied,
// or one byte short, waits for the rest; anything but digits is refused
void xHexSpin::decTextChange(const QString& txt) {
	QStringList parts = txt.split(' ', X_SkipEmptyParts);
	int bytes = decPairs() ? (hexLen / 2) : 1;
	bool good = (parts.size() <= bytes);
	foreach(QString prt, parts) {
		foreach(QChar chr, prt)
			if (!chr.isDigit() || (chr.unicode() > '9')) good = false;
	}
	if (!good) {
		// back to the value's own text, once: the last good one may not read back
		int pos = cursorPosition();
		fixing = true;
		setText(numText(value));
		fixing = false;
		setCursorPosition(pos - 1);
		return;
	}
	if (parts.size() < bytes) return;
	qint64 nval = 0;
	foreach(QString prt, parts) {
		qint64 v = (prt.size() > 12) ? INT_MAX : prt.toLongLong();
		nval = (bytes > 1) ? ((nval << 8) | qMin(v, (qint64)0xff)) : v;
	}
	vtxt = txt;
	int xval = minMaxCorrect((int)qMin(nval, (qint64)INT_MAX), min, max);
	if (value != xval)
		setValue(xval);
	else
		onChange(value);
}

void xHexSpin::onTextChange(QString txt) {
	if (isblank || fixing) return;		// the dashes are not a number to read
	if (base == 10) {
		decTextChange(txt);
		return;
	}
	if (txt.size() < len) {
		txt = txt.leftJustified(len, '0');
	} else {
		txt = txt.left(len);
	}
	int pos = 0;
	if (vldtr.validate(txt, pos) == QValidator::Acceptable) {
		vtxt = txt;
		int nval = txt.toInt(NULL, base);
		int xval = minMaxCorrect(nval, min, max);
		if (value != xval)
			setValue(xval);
		else
			onChange(value);
	} else {
		pos = cursorPosition();		// Qt moves cursor at end of field after setText
		fixing = true;
		setText(vtxt);
		fixing = false;
		setCursorPosition(pos-1);
	}
}

void xHexSpin::keyPressEvent(QKeyEvent* ev) {
	QString txt;
	int pos;
	startEdit();			// the caret arriving is not the only way in
	if (isReadOnly()) {
		QLineEdit::keyPressEvent(ev);
	} else {
		switch(ev->key()) {
			case Qt::Key_Up:
				setValue(minMaxCorrect(value + 1, min, max));
				break;
			case Qt::Key_Down:
				setValue(minMaxCorrect(value - 1, min, max));
				break;
			case Qt::Key_PageUp:
				setValue(minMaxCorrect(value + 0x100, min, max));
				break;
			case Qt::Key_PageDown:
				setValue(minMaxCorrect(value - 0x100, min, max));
				break;
			case Qt::Key_Insert:
				pos = cursorPosition();
				txt = vtxt;
				if (inputMask().isEmpty()) {
					updateMask();	// the field's own mask, blank character and all
				} else {
					setInputMask(QString());
				}
				setText(txt);
				setCursorPosition(pos);
				break;
			case Qt::Key_X:
				if (hsflag & XHS_DEC)
					setBase((base == 10) ? 16 : 10);
				break;
			case Qt::Key_Backspace:
			case Qt::Key_Delete:
				// on the sign: off it goes, the number is decimal
				if ((hsflag & XHS_ADDR) && (base == 16) && !hasSelectedText()
						&& (((ev->key() == Qt::Key_Backspace) && !sgnPre.isEmpty() && (cursorPosition() == 0))
						|| ((ev->key() == Qt::Key_Delete) && !sgnPost.isEmpty() && (cursorPosition() >= len)))) {
					setBase(10);
				} else {
					QLineEdit::keyPressEvent(ev);
				}
				break;
			case Qt::Key_NumberSign:
			case Qt::Key_Dollar:
				if ((hsflag & XHS_ADDR) && (base == 10)) {
					setBase(16);
				} else {
					QLineEdit::keyPressEvent(ev);
				}
				break;
			default:
				QLineEdit::keyPressEvent(ev);
				break;
		}
	}
}

void xHexSpin::wheelEvent(QWheelEvent* ev) {
	if (isReadOnly()) return;
	if (ev->yDelta > 0) {
		setValue(minMaxCorrect(value + 1, min, max));
	} else if (ev->yDelta < 0) {
		setValue(minMaxCorrect(value - 1, min, max));
	}
	ev->accept();
}

// xPathBox

xPathBox::xPathBox(QWidget* p):QComboBox(p) {
	// as wide as the layout makes it, whatever the path: a long one is cut short
	setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	setMinimumContentsLength(16);
	addItem(QString());
}

void xPathBox::setPath(const QString& path) {
	setItemText(0, path);
}

// the menu in place of the box's own list, under it and at least as wide
void xPathBox::showPopup() {
	QMenu menu(this);
	if (fill) fill(&menu);
	bool any = false;
	for (QAction* act : menu.actions())
		if (!act->isSeparator()) any = true;
	if (!any) menu.addAction(empty)->setEnabled(false);
	menu.setMinimumWidth(width());
	menu.exec(mapToGlobal(QPoint(0, height())));
}

// xSlider

xSlider::xSlider(QWidget* p):QSlider(p) {}

// The style sheet themes all draw the groove and the handle themselves, and a
// styled slider gets no tick marks from Qt at all - they only show under the
// system style. Draw them here when a sheet is on, over the same travel the
// handle has, so they stay under it.
void xSlider::paintEvent(QPaintEvent* ev) {
	QSlider::paintEvent(ev);
	if (tickPosition() == QSlider::NoTicks) return;
	if (qApp->styleSheet().isEmpty()) return;	// the style drew its own
	int step = tickInterval() ? tickInterval() : pageStep();
	if ((step < 1) || (maximum() <= minimum())) return;
	QStyleOptionSlider opt;
	initStyleOption(&opt);
	int hnd = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this).width();
	if (hnd < 1) hnd = 10;
	int span = width() - hnd;
	QPainter pnt(this);
	QColor col = palette().color(QPalette::WindowText);
	col.setAlpha(0x80);
	pnt.setPen(col);
	for (int val = minimum(); val <= maximum(); val += step) {
		int x = QStyle::sliderPositionFromValue(minimum(), maximum(), val, span) + hnd / 2;
		pnt.drawLine(x, height() - 4, x, height() - 1);
	}
}

// xSideButton

QActionGroup* xMenuGroup(QMenu* menu, const char* name) {
	QActionGroup* grp = menu->findChild<QActionGroup*>(name, Qt::FindDirectChildrenOnly);
	if (!grp) {
		grp = new QActionGroup(menu);
		grp->setObjectName(name);
	}
	return grp;
}

// A picture for each scale, its pixels the screen's: images/web-mark-<px>.png,
// white with the shape in alpha. A scale with none takes the next bigger one
// scaled down.
void xWebMark(QPainter* pnt, const QPoint& at, const QColor& col) {
	static const int sizes[] = {10, 13, 15, 18, 20, 23, 25, 30};
	qreal dpr = pnt->device()->devicePixelRatioF();
	int px = qRound(WEBMARK * dpr);
	int src = sizes[sizeof(sizes) / sizeof(sizes[0]) - 1];
	for (int s : sizes) {
		if (s >= px) {
			src = s;
			break;
		}
	}
	QImage mask(QString(":/images/web-mark-%0.png").arg(src));
	if (mask.isNull()) return;
	if (src != px)
		mask = mask.scaled(px, px, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	QImage img(px, px, QImage::Format_ARGB32_Premultiplied);
	img.fill(col);
	QPainter ip(&img);
	ip.setCompositionMode(QPainter::CompositionMode_DestinationIn);
	ip.drawImage(0, 0, mask);
	ip.end();
	img.setDevicePixelRatio(dpr);
	// on whole pixels of the screen, at 125 or 150 % too
	pnt->drawImage(QPointF(qRound(at.x() * dpr) / dpr, qRound(at.y() * dpr) / dpr), img);
}

xSideButton::xSideButton(QWidget* p):QPushButton(p) {}

#define SIDE_GAP	6	// between the text and the icon at the edge

// What paintEvent lays out - the text, its gaps and the icon - put through the
// style for its padding: the style's own hint has the icon beside the text.
QSize xSideButton::sizeHint() const {
	QSize sz = QPushButton::sizeHint();
	if (icon().isNull() && !web) return sz;
	QStyleOptionButton opt;
	initStyleOption(&opt);
	opt.text.clear();
	opt.icon = QIcon();
	QSize cont(fontMetrics().horizontalAdvance(text()) + SIDE_GAP / 2 + SIDE_GAP + (web ? WEBMARK : iconSize().width()),
		fontMetrics().height());
	int w = style()->sizeFromContents(QStyle::CT_PushButton, &opt, cont, this).width();
	return QSize(qMax(sz.width(), w), sz.height());
}

void xSideButton::paintEvent(QPaintEvent*) {
	QStylePainter pnt(this);
	QStyleOptionButton opt;
	initStyleOption(&opt);
	QString txt = opt.text;
	QIcon ico = opt.icon;
	opt.text.clear();
	opt.icon = QIcon();
	pnt.drawControl(QStyle::CE_PushButton, opt);
	QRect box = style()->subElementRect(QStyle::SE_PushButtonContents, &opt, this);
	int gap = SIDE_GAP;
	if (web) {
		QPoint at(box.right() - WEBMARK + 1, box.top() + (box.height() - WEBMARK) / 2);
		xWebMark(&pnt, at, palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::ButtonText));
		box.setRight(at.x() - gap);
	} else if (!ico.isNull()) {
		QSize isz = iconSize();
		QRect irc(box.right() - isz.width() + 1, box.top() + (box.height() - isz.height()) / 2, isz.width(), isz.height());
		ico.paint(&pnt, irc, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);
		box.setRight(irc.left() - gap);
	}
	box.setLeft(box.left() + gap / 2);
	txt = fontMetrics().elidedText(txt, Qt::ElideRight, box.width());
	pnt.drawItemText(box, Qt::AlignLeft | Qt::AlignVCenter, palette(), isEnabled(), txt, QPalette::ButtonText);
}

// xIconGroup

#define	GROUP_ICON	16

xIconGroup::xIconGroup(const QString& ico, const QString& txt, QWidget* p):QGroupBox(txt, p) {
	setIcon(ico);
}

// promoted from a .ui: the title is set there, the icon afterwards
xIconGroup::xIconGroup(QWidget* p):QGroupBox(p) {}

void xIconGroup::setIcon(const QString& path) {
	if (icon.isNull()) text = title();
	icon = QIcon(path);
	padTitle();
}

void xIconGroup::padTitle() {
	if (icon.isNull()) return;
	int spc = qMax(1, fontMetrics().horizontalAdvance(' '));
	setTitle(QString((GROUP_ICON + 4 + spc - 1) / spc, ' ') + text);
}

void xIconGroup::changeEvent(QEvent* ev) {
	QGroupBox::changeEvent(ev);
	if ((ev->type() == QEvent::FontChange) || (ev->type() == QEvent::StyleChange)) padTitle();
}

void xIconGroup::paintEvent(QPaintEvent* ev) {
	QGroupBox::paintEvent(ev);
	QStyleOptionGroupBox opt;
	initStyleOption(&opt);
	QRect lab = style()->subControlRect(QStyle::CC_GroupBox, &opt, QStyle::SC_GroupBoxLabel, this);
	QRect irc(lab.left(), lab.top() + (lab.height() - GROUP_ICON) / 2, GROUP_ICON, GROUP_ICON);
	QPainter pnt(this);
	icon.paint(&pnt, irc, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);
}

// A pop-up laid out like Advanced settings: fields with a name first, then a
// line, then check boxes with a name and, in italics, what they do. A bigger
// one puts them in frames, one row to a field, as Advanced settings does.

#define	SHEET_TEXT	300		// the description column; keeps the window near 520
#define	SHEET_GAP	16		// from a name to its description, as in Advanced settings
#define	SHEET_SIDE_MIN	300		// the right column of a row of frames, which takes a wider window
#define	SHEET_CTRL	200		// a list's width, as in Advanced settings

// Every frame of a sheet is a grid of its own, so the columns are lined up
// across them here, once the style has given the labels their font.
class xSheetColumns : public QObject {
	public:
		QList<QGridLayout*> grids;
		QList<QLabel*> names[2];	// the right column's frames line up among themselves
		QList<QCheckBox*> checks;
		QHash<QGridLayout*, QList<QWidget*> > sized;	// side by side: the frame's own width
		xSheetColumns(QWidget* body) : QObject(body) {
			body->installEventFilter(this);
		}
	protected:
		bool eventFilter(QObject*, QEvent* ev) {
			if (ev->type() != QEvent::Show) return false;
			for (int i = 0; i < 2; i++) {
				int wid = 0;
				foreach(QLabel* lab, names[i]) {
					lab->setMinimumWidth(0);
					wid = qMax(wid, lab->sizeHint().width());
				}
				foreach(QLabel* lab, names[i]) lab->setMinimumWidth(wid);
			}
			// Every name starts where a check box's text does, in every frame, with
			// or without a check box in it. The grids have no spacing of their own:
			// a grid leaves it out after an empty column, and the names moved about.
			int off = 0;
			if (!checks.isEmpty()) {
				QCheckBox* cb = checks.first();
				QStyleOptionButton opt;
				opt.initFrom(cb);
				opt.text = "x";
				off = cb->style()->subElementRect(QStyle::SE_CheckBoxContents, &opt, cb).left();
			}
			foreach(QGridLayout* grd, grids) grd->setColumnMinimumWidth(0, off);
			// a frame that shares its row is as wide as its longest value needs
			foreach(QList<QWidget*> list, sized) {
				int cw = 0;
				foreach(QWidget* wid, list) {
					wid->setMinimumWidth(0);
					wid->setMaximumWidth(QWIDGETSIZE_MAX);
					cw = qMax(cw, wid->sizeHint().width());
				}
				foreach(QWidget* wid, list) wid->setFixedWidth(cw);
			}
			return false;
		}
};

static QGridLayout* sheet_grid(QWidget* wid, bool side = false) {
	QGridLayout* grd = new QGridLayout(wid);
	grd->setHorizontalSpacing(0);		// see xSheetColumns
	grd->setColumnMinimumWidth(2, side ? 0 : SHEET_TEXT);
	grd->setColumnStretch(2, 1);
	return grd;
}

class xFollowEnabled : public QObject {
	public:
		xFollowEnabled(QWidget* src, QList<QWidget*> dst) : QObject(src) {
			list = dst;
			src->installEventFilter(this);
			follow(src);
		}
	protected:
		bool eventFilter(QObject* obj, QEvent* ev) {
			if (ev->type() == QEvent::EnabledChange) follow((QWidget*)obj);
			return false;
		}
	private:
		QList<QWidget*> list;
		void follow(QWidget* src) {
			foreach(QWidget* w, list) w->setEnabled(src->isEnabled());
		}
};

// the rows stay packed at the top: spare height goes under them, so a row that
// hides moves only what is below it
xOptSheet::xOptSheet() {
	body = new QWidget;
	box = new QVBoxLayout(body);
	box->setContentsMargins(0, 0, 0, 0);
	rows = new QWidget;
	box->addWidget(rows);
	box->addStretch(1);
	grid = sheet_grid(rows);
	grid->setContentsMargins(0, 0, 0, 0);
	cols = new xSheetColumns(body);
	cols->grids.append(grid);
	pair = NULL;
	nameSet = 0;
}

// A frame of its own; what follows goes into it. side 0 and 1 put it in the
// left or right column of a row of frames, -1 across the sheet under them.
void xOptSheet::group(const QString& title, int side, const QString& icon) {
	if (grid->count() == 0) rows->hide();		// nothing before the first frame
	QGroupBox* frm = icon.isEmpty() ? new QGroupBox(title) : new xIconGroup(icon, title);
	if (side < 0) {
		pair = NULL;
		box->insertWidget(box->count() - 1, frm);
	} else {
		if (!pair) {
			pair = new QHBoxLayout;
			box->insertLayout(box->count() - 1, pair);
			// the left column keeps its width, the right one takes what the window gives
			for (int i = 0; i < 2; i++) {
				half[i] = new QVBoxLayout;
				half[i]->addStretch(1);		// a shorter column keeps its frames at the top
				pair->addLayout(half[i], i);
			}
		}
		half[side & 1]->insertWidget(half[side & 1]->count() - 1, frm);
		if (side & 1) frm->setMinimumWidth(SHEET_SIDE_MIN);
	}
	nameSet = (side >= 0) ? (side & 1) : 0;
	grid = sheet_grid(frm, pair != NULL);
	cols->grids.append(grid);
}

// one line: the name and the control, at a list's width
void xOptSheet::row(const QString& name, QWidget* wid) {
	int row = grid->rowCount();
	QLabel* nam = new QLabel(name);
	nam->setContentsMargins(0, 0, SHEET_GAP, 0);
	grid->addWidget(nam, row, 1);
	cols->names[nameSet].append(nam);
	// a line of text takes the width, anything else keeps the lists' own
	bool line = qobject_cast<QLineEdit*>(wid) != NULL;
	grid->addWidget(fieldPair(wid, NULL, line), row, 2);
	// after fieldPair, which sets its own
	if (pair) {
		cols->sized[grid].append(wid);
	} else if (!line) {
		wid->setMinimumWidth(SHEET_CTRL);
	}
	new xFollowEnabled(wid, QList<QWidget*>() << nam);
}

// across the whole frame, under the rows
void xOptSheet::wide(QWidget* wid) {
	grid->addWidget(wid, grid->rowCount(), 0, 1, 3);
	QCheckBox* cb = qobject_cast<QCheckBox*>(wid);
	if (cb) cols->checks.append(cb);
}

void xOptSheet::field(const QString& name, QWidget* wid, const QString& desc) {
	int row = grid->rowCount();
	// as tall as its field, so the two read as one line at the top
	QLabel* nam = new QLabel(name);
	nam->setContentsMargins(0, 0, SHEET_GAP, 0);
	// a field of several lines keeps its name on the first one
	if (wid->sizeHint().height() < 2 * nam->sizeHint().height())
		nam->setMinimumHeight(wid->sizeHint().height());
	grid->addWidget(nam, row, 1, Qt::AlignTop);
	cols->names[nameSet].append(nam);
	// a list keeps the width of the others on the page, as in Advanced settings
	Qt::Alignment al = Qt::AlignTop;
	if (qobject_cast<QComboBox*>(wid)) {
		wid->setMinimumWidth(SHEET_CTRL);
		al |= Qt::AlignLeft;
	}
	grid->addWidget(wid, row, 2, al);
	wid->setToolTip(QString());
	if (!desc.isEmpty()) grid->addWidget(text(desc), row + 1, 2, Qt::AlignTop);
}

void xOptSheet::line() {
	QFrame* frm = new QFrame;
	frm->setFrameShape(QFrame::HLine);
	frm->setFrameShadow(QFrame::Sunken);
	grid->addWidget(frm, grid->rowCount(), 0, 1, 3);
}

// every option here is the machine's own unless it says otherwise
void xOptSheet::check(QCheckBox* cb, const QString& name, const QString& desc, bool global) {
	int row = grid->rowCount();
	cb->setText(QString());
	cb->setToolTip(QString());
	xLabel* nam = new xLabel;
	nam->setText(name);
	if (global) nam->setToolTip(QObject::tr("Applies to all machines"));
	QObject::connect(nam, &xLabel::clicked, cb, [cb]() {if (cb->isEnabled()) cb->click();});
	QLabel* dsc = text(desc);
	nam->setContentsMargins(0, 0, SHEET_GAP, 0);
	// a description of two lines hangs from the top, next to its name
	grid->addWidget(cb, row, 0, Qt::AlignTop);
	grid->addWidget(nam, row, 1, Qt::AlignTop);
	grid->addWidget(dsc, row, 2, Qt::AlignTop);
	new xFollowEnabled(cb, QList<QWidget*>() << nam << dsc);
	cols->names[nameSet].append(nam);
	cols->checks.append(cb);
}

QLabel* xOptSheet::text(const QString& str) {
	QLabel* lab = new QLabel(str);
	QFont fnt = lab->font();
	fnt.setItalic(true);
	lab->setFont(fnt);
	lab->setWordWrap(true);
	return lab;
}

// a control and what goes after it - the figure a slider sets, a unit - as one
// field; a slider takes the width, anything else keeps its own
QWidget* fieldPair(QWidget* main, QWidget* tail, bool wide) {
	QWidget* wid = new QWidget;
	QHBoxLayout* box = new QHBoxLayout(wid);
	box->setContentsMargins(0, 0, 0, 0);
	// a list or a slider as wide as the lists above it, what follows in one column
	if (!wide && (qobject_cast<QComboBox*>(main) || qobject_cast<QSlider*>(main))) main->setMinimumWidth(200);
	box->addWidget(main, wide ? 1 : 0);
	if (tail) box->addWidget(tail);
	if (!wide) box->addStretch(1);
	return wid;
}

// Over the middle of a window, frames and all: Qt places a dialog before it
// knows the invisible borders Windows 11 draws around one, and it lands a few
// pixels off to the left. Kept on the screen it lands on. Call it shown.
void center_over(QWidget* dlg, QWidget* win) {
	if (!dlg || !win) return;
	QRect fr = dlg->frameGeometry();
	QPoint at = win->frameGeometry().center() - QPoint(fr.width() / 2, fr.height() / 2);
	if (QScreen* scr = QGuiApplication::screenAt(win->frameGeometry().center())) {
		QRect av = scr->availableGeometry();
		at.setX(qBound(av.left(), at.x(), qMax(av.left(), av.right() + 1 - fr.width())));
		at.setY(qBound(av.top(), at.y(), qMax(av.top(), av.bottom() + 1 - fr.height())));
	}
	dlg->move(at + (dlg->pos() - fr.topLeft()));
}

// the first time only: after that the window stays where the user put it
void center_once(QWidget* dlg) {
	if (!dlg || dlg->property("xCentered").toBool() || !dlg->parentWidget()) return;
	dlg->setProperty("xCentered", true);
	center_over(dlg, dlg->parentWidget()->window());
}

// A page of help from the resources, in a window of its own: made on the first
// call into *win, shown again after that
void help_window(QWidget* parent, QDialog** win, const QString& res, const QString& title) {
	if (!*win) {
		QFile file(res);
		QString txt = "no help text in this build";
		if (file.open(QFile::ReadOnly)) {
			txt = QString::fromUtf8(file.readAll());
			file.close();
		}
		*win = new QDialog(parent);
		(*win)->setWindowTitle(title);
		(*win)->resize(620, 640);
		QTextBrowser* brw = new QTextBrowser(*win);
		brw->document()->setDefaultStyleSheet("td {padding-right:16px;} p {margin:2px 0px;} li {margin:2px 0px;}");
		brw->setHtml(txt);
		QVBoxLayout* lay = new QVBoxLayout(*win);
		lay->setContentsMargins(4, 4, 4, 4);
		lay->addWidget(brw);
	}
	(*win)->show();
	(*win)->raise();
}

// xElideLabel

xElideLabel::xElideLabel(QWidget* p):QLabel(p) {}

void xElideLabel::setFull(const QString& str) {
	full = str;
	setToolTip(str);
	update();
}

// it takes the width it is given, and asks for none
QSize xElideLabel::sizeHint() const {
	return QSize(0, QLabel::sizeHint().height());
}

QSize xElideLabel::minimumSizeHint() const {
	return sizeHint();
}

void xElideLabel::paintEvent(QPaintEvent*) {
	QPainter pnt(this);
	QString txt = fontMetrics().elidedText(full, Qt::ElideMiddle, contentsRect().width());
	style()->drawItemText(&pnt, contentsRect(), alignment(), palette(), isEnabled(), txt, foregroundRole());
}

// xLabel

xLabel::xLabel(QWidget* p):QLabel(p) {}

void xLabel::mousePressEvent(QMouseEvent* ev) {
	emit clicked(ev);
}

// xTreeBox

xTreeBox::xTreeBox(QWidget *p):QComboBox(p) {
	tree = new QTreeView;
	mod = new QFileSystemModel;
	setModel(mod);
	setView(tree);
	mod->setNameFilters(QStringList() << "*.rom" << "*.bin");
	mod->setReadOnly(true);
	mod->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
	mod->setNameFilterDisables(false);
	tree->setEditTriggers(QTableView::NoEditTriggers);
	tree->setSelectionBehavior(QAbstractItemView::SelectRows);
	tree->setSelectionMode(QAbstractItemView::SingleSelection);
	tree->header()->setVisible(false);
}

void xTreeBox::setDir(QString dir) {
	QModelIndex idx = mod->setRootPath(dir);
	setRootModelIndex(idx);
}

void xTreeBox::showPopup() {
	for (int i = 1; i < mod->columnCount(); i++) {
		tree->hideColumn(i);
	}
	QComboBox::showPopup();
}

void xTreeBox::hidePopup() {
	QModelIndex idx = tree->selectionModel()->currentIndex();
	QFileInfo inf = mod->fileInfo(idx);
	if (inf.isDir()) return;
	QComboBox::hidePopup();
}

void xTreeBox::setCurrentFile(QString path) {
	path.prepend(SLSH);
	path.prepend(mod->rootPath());
	QModelIndex idx = mod->index(path, 0);
	if (!idx.isValid()) return;
	QModelIndex x = rootModelIndex();
	setRootModelIndex(idx.parent());
	setModelColumn(0);
	setCurrentIndex(idx.row());
	setRootModelIndex(x);
	tree->setCurrentIndex(idx);
}

QString xTreeBox::currentFile() {
	QModelIndex idx = tree->selectionModel()->currentIndex();
	QFileInfo inf = mod->fileInfo(idx);
	return mod->rootDirectory().relativeFilePath(inf.filePath());
}

// base table model

xTableModel::xTableModel(QObject* p):QAbstractTableModel(p) {
	row_count = 0;
	col_count = 0;
	row_mark = -1;
}

int xTableModel::markRow(int row) {
	if (row == row_mark) return 0;
	int was = row_mark;
	row_mark = row;
	if ((was >= 0) && (was < row_count)) updateRow(was);
	if ((row >= 0) && (row < row_count)) updateRow(row);
	return 1;
}

QVariant xTableModel::markRole(int row, int role) const {
	if (row != row_mark) return QVariant();
	if (role == X_BackgroundRole) return QColor(Qt::darkGray);
	if (role == Qt::ForegroundRole) return QColor(Qt::white);
	return QVariant();
}

QModelIndex xTableModel::index(int row, int col, const QModelIndex& p) const {
	return createIndex(row, col);
}

void xTableModel::update() {
	emit dataChanged(index(0,0),index(rowCount() - 1, columnCount() - 1));
}

void xTableModel::updateRow(int row) {
	emit dataChanged(index(row, 0), index(row, columnCount() - 1));
}

void xTableModel::updateColumn(int col) {
	emit dataChanged(index(0, col), index(rowCount() - 1, col));
}

void xTableModel::updateCell(int row, int col) {
	emit dataChanged(index(row, col), index(row, col));
}

void xTableModel::setRows(int r) {
	if (r < row_count) {
		emit beginRemoveRows(QModelIndex(), r, row_count-1);
		row_count = r;
		emit endRemoveRows();
	} else if (r > row_count) {
		emit beginInsertRows(QModelIndex(), row_count, r-1);
		row_count = r;
		emit endInsertRows();
	}
	update();
}

void xTableModel::setCols(int c) {
	if (c < col_count) {
		emit beginRemoveColumns(QModelIndex(), c, col_count-1);
		col_count = c;
		emit endRemoveColumns();
	} else if (c > col_count) {
		emit beginInsertColumns(QModelIndex(), col_count, c-1);
		col_count = c;
		emit endInsertColumns();
	}
	update();
}

int xTableModel::columnCount(const QModelIndex&) const {
	return col_count;
}

int xTableModel::rowCount(const QModelIndex&) const {
	return row_count;
}

// item delegate

xItemDelegate::xItemDelegate(int t) {
	type = t;
}

QWidget* xItemDelegate::createEditor(QWidget* par, const QStyleOptionViewItem&, const QModelIndex&) const {
	if (type == XTYPE_NONE) return NULL;
	QLineEdit* edt = new QLineEdit(par);
	QRegExpValidator* vld;
	switch (type) {
		case XTYPE_ADR: edt->setInputMask("hhhh"); break;
		case XTYPE_BYTE: edt->setInputMask("hh"); break;
		case XTYPE_LABEL:
			label_complete(edt);
			dbg_num_field(edt);
			break;
		case XTYPE_DUMP:
			// as many bytes as the row has: a mask would pad the field with blanks to its length
			vld = new QRegExpValidator(edt);
			setRegExp(*vld, "[0-9A-Fa-f]*");
			edt->setValidator(vld);
			edt->setMaxLength(32);
			break;
	}
	return edt;
}

// The debugger's address fields. In a hex form typing keeps the sign (select
// all, type C000: #C000); Backspace or Delete on it takes it off, decimal then.
// A label or a letter past F goes in without it. In decimal nothing is added.

// a table, so the values line up whatever the font makes of the names; the head
// over it, not a row of it, where a long line would widen the names' column
static QString tip_head(const QString& head) {
	QString res = head.isEmpty() ? QString() : QString("<div>%0</div>").arg(head);
	return res + "<table cellspacing=0 cellpadding=0>";
}

static QString tip_row(const QString& name, const QString& val) {
	return QString("<tr><td>%0&nbsp;</td><td>%1</td></tr>").arg(name, val);
}

static QString tip_bold(const QString& txt) {
	return QString("<b>%0</b>").arg(txt.toHtmlEscaped());
}

QString dbg_num_tip(int val, bool adr, const QString& name, bool label) {
	val &= 0xffff;
	bool word = adr || (val > 0xff);
	QString bin = QString::number(val, 2).rightJustified(word ? 16 : 8, '0');
	if (word) bin.insert(8, ' ');
	QString lab;
	if (word && label && conf.zx)
		lab = name.isEmpty() ? find_label(comp_get_xadr(conf.zx, val, MEM_VIEW_READ), nullptr, LAB_SYSVAR | LAB_FULL) : name;
	QString res = tip_head(lab.isEmpty() ? QString() : tip_bold(lab));
	res.append(tip_row("HEX:", tip_bold(dbg_hex(val, word ? 4 : 2))));
	res.append(tip_row("DEC:", tip_bold(QString::number(val))));
	res.append(tip_row("BIN:", tip_bold(bin)));
	res.append("</table>");
	return res;
}

QString dbg_byte_tip(int val, int word, const QString& where, const QString& label, const QString& note) {
	val &= 0xff;
	QString head;
	if (!note.isEmpty()) head = note.toHtmlEscaped() + "<br>";
	head.append(tip_bold(where));
	if (!label.isEmpty()) head.append("&nbsp;&nbsp;" + tip_bold(label));
	QString hex = tip_bold(dbg_hex(val, 2));
	QString dec = tip_bold(QString::number(val));
	if (word >= 0) {
		word &= 0xffff;
		hex.append(QString(" (WORD: %0)").arg(tip_bold(dbg_hex(word, 4))));
		dec.append(QString(" (WORD: %0)").arg(tip_bold(QString::number(word))));
	}
	QString res = tip_head(head);
	res.append(tip_row("HEX:", hex));
	res.append(tip_row("DEC:", dec));
	res.append(tip_row("BIN:", tip_bold(QString::number(val, 2).rightJustified(8, '0'))));
	res.append("</table>");
	return res;
}

QString dbg_cpu_byte_tip(int adr, const QString& note) {
	Computer* comp = conf.zx;
	adr &= 0xffff;
	int lo = comp_peek(comp, adr, MEM_VIEW_READ, NULL) & 0xff;
	int hi = comp_peek(comp, (adr + 1) & 0xffff, MEM_VIEW_READ, NULL) & 0xff;
	return dbg_byte_tip(lo, lo | (hi << 8), dbg_word(adr), find_label(comp_get_xadr(comp, adr, MEM_VIEW_READ), nullptr, LAB_SYSVAR | LAB_FULL), note);
}

QString dbg_buf_tip(const unsigned char* buf, int ofs, int len, const QString& where) {
	return dbg_byte_tip(buf[ofs], (ofs + 1 < len) ? buf[ofs] | (buf[ofs + 1] << 8) : -1, where);
}

void reg_field_setup(xHexSpin* fld, int size, int flag) {
	int mx;
	switch (size) {
		case REG_BIT: mx = 1; break;
		case REG_2: mx = 2; break;
		case REG_BYTE: mx = 0xff; break;
		case REG_24: mx = 0xffffff; break;
		case REG_32: mx = 0x7fffffff; break;	// the field and the register value are int
		default: mx = 0xffff; break;
	}
	fld->setMax(mx);
	fld->numFmt();
	// a register with a type of its own (PC, SP, F, a segment) is one number;
	// the rest are bytes that move on their own
	fld->setSplit(conf.dbg.regsplit && !(flag & REG_TYPE_M));
	fld->refitWidth();
}

QString dbg_reg_tip(const QString& name) {
	if (name == "IM")
		return "Interrupt mode, set by IM 0, IM 1 or IM 2.\n"
			"Mode 1 calls #0038, mode 2 the address in\n"
			"the table at I * 256 + the byte on the bus";
	if (name == "IFF1")
		return "Interrupts enabled: EI sets it, DI clears it,\n"
			"and so does taking an interrupt. While it is\n"
			"off, the CPU does not answer INT";
	if (name == "IFF2")
		return "A copy of IFF1, kept while an NMI runs:\n"
			"RETN puts it back into IFF1. LD A,I and\n"
			"LD A,R show it in the P/V flag";
	return QString();
}

// Qt puts a tip off by the size of the mouse cursor from where it is given:
// it is moved once shown
void dbg_show_tip(QWidget* wid, const QRect& at, const QString& text, bool above) {
	if (text.isEmpty()) {
		QToolTip::hideText();
		return;
	}
	QRect r(wid->mapToGlobal(at.topLeft()), at.size());
	if (above) {
		QToolTip::showText(QCursor::pos(), text, wid, at);
	} else {
		QToolTip::showText(r.bottomLeft(), text, wid, QRect(), 3600000);
	}
	QScreen* sc = QGuiApplication::screenAt(r.center());
	QRect scr = (sc ? sc : QGuiApplication::primaryScreen())->availableGeometry();
	foreach(QWidget* tip, QApplication::topLevelWidgets()) {
		if (!tip->isVisible() || !tip->inherits("QTipLabel")) continue;
		int over = r.top() - tip->height() - 2;
		int under = r.bottom() + 3;
		bool up = above ? (over >= scr.top()) : (under + tip->height() > scr.bottom());
		QPoint pos(r.left(), up ? over : under);
		if (pos.x() + tip->width() > scr.right())
			pos.setX(scr.right() - tip->width());
		if (pos.x() < scr.left())
			pos.setX(scr.left());
		tip->move(pos);
	}
}

// The cell a table's tip is up over, for dbg_cell_tip_follow: the text tells it
// is still the tip shown, not one of another widget's since
static QPointer<QAbstractItemView> cellTipView;
static int cellTipRow = -1;
static int cellTipCol = -1;
static QString cellTipText;

bool dbg_cell_tip(QAbstractItemView* view, QEvent* ev) {
	if (ev->type() != QEvent::ToolTip) return false;
	QModelIndex idx = view->indexAt(static_cast<QHelpEvent*>(ev)->pos());
	QString txt = idx.isValid() ? idx.data(Qt::ToolTipRole).toString() : QString();
	dbg_show_tip(view->viewport(), view->visualRect(idx), txt, true);
	cellTipView = view;
	cellTipRow = idx.row();
	cellTipCol = idx.column();
	cellTipText = txt;
	return true;
}

// a table's viewport hands its tooltip events to dbg_cell_tip
class xCellTipFilter : public QObject {
	public:
		xCellTipFilter(QAbstractItemView* p) : QObject(p) {}
	protected:
		bool eventFilter(QObject* obj, QEvent* ev) override {
			QAbstractItemView* view = static_cast<QAbstractItemView*>(parent());
			return (obj == view->viewport()) && dbg_cell_tip(view, ev);
		}
};

void dbg_cell_tips(QAbstractItemView* view) {
	view->viewport()->installEventFilter(new xCellTipFilter(view));
}

// one ToolTipRole a refresh, and only while the tip is up
void dbg_cell_tip_follow(QAbstractItemView* view) {
	if ((cellTipView != view) || cellTipText.isEmpty()) return;
	if (!QToolTip::isVisible() || (QToolTip::text() != cellTipText)) {
		cellTipView = nullptr;
		return;
	}
	QModelIndex idx = view->model()->index(cellTipRow, cellTipCol);
	QString txt = idx.isValid() ? idx.data(Qt::ToolTipRole).toString() : QString();
	if (txt == cellTipText) return;
	dbg_show_tip(view->viewport(), view->visualRect(idx), txt, true);
	cellTipText = txt;
}

// A typed address's tip: a label, a number by the one rule, or a label behind
// the sign the typing kept. Nothing for anything else (.HL, page:offset, half a name)
static QString field_tip(const QString& str) {
	int val;
	xAdr lab;
	QString name;
	if (!dbg_read_adr(str, &val, &lab, &name) || (val < 0)) return QString();
	return dbg_num_tip(val, true, name);
}

class xNumSign : public QObject {
	public:
		xNumSign(QLineEdit* p);
		~xNumSign();
	protected:
		bool eventFilter(QObject*, QEvent*) override;
	private:
		QLineEdit* edt;
		bool bare = false;		// the sign was taken off by hand: no new one
		bool tip = false;		// the tip is this field's
		bool pasting = false;		// a paste is on its way: Ctrl+V, Shift+Ins, Paste, middle button, a drop
		void signPaste();
		void edited(const QString&);
		void showTip();
		void hideTip();
};

xNumSign::xNumSign(QLineEdit* p):QObject(p) {
	edt = p;
	connect(edt, &QLineEdit::textEdited, this, &xNumSign::edited);
}

// A paste that leaves bare hex digits gets the sign as typing would: 8000
// pasted over the lot is #8000, not decimal 8000. Only a paste - typed keys
// have the event filter - and not a label's name (CAFE). Put in once every
// slot has heard the paste, so they all hear the text with the sign next
void xNumSign::edited(const QString&) {
	if (pasting) {
		pasting = false;
		QTimer::singleShot(0, this, &xNumSign::signPaste);
	}
	showTip();
}

void xNumSign::signPaste() {
	int fmt = conf.dbg.numfmt;
	QString txt = edt->text();
	bool digits = !txt.isEmpty();
	for (QChar chr : txt)
		if (!is_hex_char(chr)) digits = false;
	if (!digits || bare || (fmt == CPU_NUM_DEC) || edt->property(DBG_NUM_OFF).toBool()) return;
	if (find_label(txt).type >= 0) return;
	QString pre, post;
	dbg_sign(fmt, &pre, &post);
	int pos = edt->cursorPosition();
	edt->selectAll();
	edt->insert(pre + txt + post);		// undoable, as the paste itself
	edt->setCursorPosition(pos + pre.size());
}

xNumSign::~xNumSign() {
	hideTip();
}

// the tip under the field, clear of the text being typed
void xNumSign::showTip() {
	QString txt;
	if (!edt->hasFocus() || edt->property(DBG_NUM_OFF).toBool()) {
		// a field of plain hex (a port's mask) has nothing to say
	} else if (edt->property(DBG_NUM_PORT).toBool()) {
		bool ok;
		int val = dbg_num_parse(edt->text(), &ok);
		if (ok && (val >= 0)) txt = dbg_num_tip(val, false, QString(), false);
	} else {
		txt = field_tip(edt->text());
	}
	if (txt.isEmpty()) {
		hideTip();
		return;
	}
	dbg_show_tip(edt, edt->rect(), txt, false);
	tip = true;
}

void xNumSign::hideTip() {
	if (!tip) return;
	tip = false;
	QToolTip::hideText();
}

bool xNumSign::eventFilter(QObject* obj, QEvent* ev) {
	if (ev->type() == QEvent::FocusIn) bare = false;
	if (ev->type() == QEvent::FocusOut) hideTip();
	if (ev->type() == QEvent::FocusIn) pasting = false;
	if (ev->type() == QEvent::MouseButtonPress)		// X11's middle button pastes the selection
		pasting = (static_cast<QMouseEvent*>(ev)->button() == Qt::MiddleButton);
	if (ev->type() == QEvent::Drop) pasting = true;
	if ((ev->type() == QEvent::ContextMenu) && qobject_cast<QLineEdit*>(obj)) {
		// the field's own menu: after its Paste, and no other action, the sign
		QMenu* menu = static_cast<QLineEdit*>(obj)->createStandardContextMenu();
		QAction* got = menu->exec(static_cast<QContextMenuEvent*>(ev)->globalPos());
		if (got && (got->objectName() == "edit-paste"))
			QTimer::singleShot(0, this, &xNumSign::signPaste);
		delete menu;
		return true;
	}
	if (ev->type() != QEvent::KeyPress) return false;
	pasting = static_cast<QKeyEvent*>(ev)->matches(QKeySequence::Paste);
	QLineEdit* edt = qobject_cast<QLineEdit*>(obj);
	int fmt = conf.dbg.numfmt;
	if (!edt || (fmt == CPU_NUM_DEC) || edt->property(DBG_NUM_OFF).toBool()) return false;
	QKeyEvent* kev = static_cast<QKeyEvent*>(ev);
	QString txt = edt->text();
	// the digits: lo..hi, the text without its sign
	int lo = 0;
	int hi = txt.size();
	if (txt.startsWith('#') || txt.startsWith('$')) {
		lo = 1;
	} else if ((txt.size() > 1) && txt.endsWith('h', Qt::CaseInsensitive)) {
		hi--;
		for (int i = 0; i < hi; i++)
			if (!is_hex_char(txt.at(i))) hi = txt.size();	// MATCH is a name, not a number
	}
	int start = edt->hasSelectedText() ? edt->selectionStart() : edt->cursorPosition();
	int end = edt->hasSelectedText() ? start + edt->selectedText().size() : start;
	int key = kev->key();
	bool del = (key == Qt::Key_Backspace) || (key == Qt::Key_Delete);
	QString chr = kev->text();
	bool digit = (chr.size() == 1) && is_hex_char(chr.at(0));
	bool hasSign = (lo > 0) || (hi < txt.size());
	bool all = (start == 0) && (end == txt.size());		// the key takes the lot
	if (!digit && hasSign && !all && (chr.size() == 1) && lab_char(chr.at(0))) {
		// a name after all: the sign goes, the caret and selection stay on their text
		int at = (lo > 0) ? 0 : hi;
		int len = end - start;
		edt->setSelection(at, 1);
		edt->del();
		if (at < start) start--;
		if (len > 0) {
			edt->setSelection(start, qMin(len, edt->text().size() - start));
		} else {
			edt->setCursorPosition(qMin(start, edt->text().size()));
		}
		return false;
	}
	if (!del && !digit) return false;
	if (del && all) return false;			// select all and Delete: an empty field
	if (!hasSign) {
		// a field that is all typed over starts with the sign
		if (bare || !digit || (start > 0) || (end < txt.size())) return false;
		QString pre, post;
		dbg_sign(fmt, &pre, &post);
		edt->del();
		edt->insert(pre + post);
		edt->setCursorPosition(pre.size());
		return false;			// the key itself goes in after it
	}
	if (edt->hasSelectedText()) {
		// a selection takes the digits in it, never the sign - unless the sign is all of it
		int s = qMax(start, lo);
		int e = qMin(end, hi);
		if (e > s) {
			edt->setSelection(s, e - s);
		} else if (digit) {
			edt->setCursorPosition(qBound(lo, start, hi));
		} else {
			bare = true;			// the sign alone selected: off it goes
		}
	} else if (digit) {
		edt->setCursorPosition(qBound(lo, start, hi));
	} else if (key == Qt::Key_Backspace) {
		bare = ((lo > 0) && (start == lo)) || ((hi < txt.size()) && (start == txt.size()));
	} else {
		bare = ((lo > 0) && (start == 0)) || ((hi < txt.size()) && (start == hi));
	}
	return false;
}

void dbg_num_field(QLineEdit* edt) {
	edt->installEventFilter(new xNumSign(edt));
}

// label completion

// a character of a label name: an assembler's local labels have a dot in them
bool lab_char(QChar chr) {
	return chr.isLetterOrNumber() || (chr == QChar('_')) || (chr == QChar('.'));
}

// registers and conditions: no labels offered for them
static bool asm_operand_word(const QString& word) {
	static const QStringList words = {"AF", "BC", "DE", "HL", "SP", "IX", "IY", "IXH", "IXL",
		"IYH", "IYL", "HX", "LX", "HY", "LY", "XH", "XL", "YH", "YL", "NZ", "NC", "PO", "PE"};
	return words.contains(word.toUpper());
}

// The labels are offered as a name is typed. It is the word under the caret that
// is completed, not the field, so it works inside an instruction or an expression.
// asm_line: in the operands only, not for the command (RET is not RETURN)
void label_complete(QLineEdit* edt, bool asm_line) {
	QCompleter* cmp = new QCompleter(edt);
	QStringListModel* mod = new QStringListModel(cmp);
	cmp->setModel(mod);
	cmp->setWidget(edt);
	cmp->setCaseSensitivity(Qt::CaseInsensitive);
	cmp->setFilterMode(Qt::MatchContains);	// a big set from an assembler is searched, not browsed
	// the word around the caret, and what of it is before the caret
	auto word = [edt, asm_line](int* from, int* to) {
		QString txt = edt->text();
		int pos = edt->cursorPosition();
		int s = pos;
		int e = pos;
		while ((s > 0) && lab_char(txt.at(s - 1))) s--;
		while ((e < txt.size()) && lab_char(txt.at(e))) e++;
		*from = s;
		*to = e;
		// a number is not a name: #C000, $C000, 0x1F, 12
		if ((s > 0) && ((txt.at(s - 1) == QChar('#')) || (txt.at(s - 1) == QChar('$')))) return QString();
		if ((s < pos) && txt.at(s).isDigit()) return QString();
		if (asm_line) {
			int first = 0;
			while ((first < txt.size()) && txt.at(first).isSpace()) first++;
			if (s <= first) return QString();
			if (asm_operand_word(txt.mid(s, pos - s))) return QString();
		}
		return txt.mid(s, pos - s);
	};
	QObject::connect(edt, &QLineEdit::textEdited, cmp, [cmp, mod, word]() {
		int s, e;
		QString pre = word(&s, &e);
		if (pre.size() < 2) {
			cmp->popup()->hide();
			return;
		}
		// the user's set and the built-in names, again when a label was added or another set picked
		if (cmp->property("labGen").toUInt() != labels_gen()) {
			QStringList names = conf.curlabset ? conf.curlabset->list.keys() : QStringList();
			names += labels_builtin_names();
			names.removeDuplicates();
			mod->setStringList(names);
			cmp->setProperty("labGen", labels_gen());
		}
		cmp->setCompletionPrefix(pre);
		if (cmp->completionCount() < 1) {
			cmp->popup()->hide();
			return;
		}
		cmp->complete();
		// Enter takes the name typed in full, else the first that starts with it, else
		// the first: START is START, not ARC_START before it
		QAbstractItemModel* cm = cmp->completionModel();
		int pick = -1;
		int same = -1;
		for (int i = 0; i < cm->rowCount(); i++) {
			QString nm = cm->index(i, 0).data().toString();
			if (nm == pre) {		// labels are told apart by case: start is not START
				same = i;
				break;
			}
			if ((same < 0) && (nm.compare(pre, Qt::CaseInsensitive) == 0)) same = i;
			if ((pick < 0) && nm.startsWith(pre, Qt::CaseInsensitive)) pick = i;
		}
		if (same >= 0) pick = same;
		cmp->popup()->setCurrentIndex(cm->index(qMax(pick, 0), 0));
	});
	QObject::connect(cmp, QOverload<const QString&>::of(&QCompleter::activated), edt, [edt, word](const QString& lab) {
		int s, e;
		word(&s, &e);
		QString txt = edt->text();
		edt->setText(txt.left(s) + lab + txt.mid(e));
		edt->setCursorPosition(s + lab.size());
	});
}

// dock widget

xDockWidget::xDockWidget(QString icopath, QString txt, QWidget* p):QDockWidget(p) {
	if (icopath.isEmpty()) {
		title = txt;
	} else {
		icon = QIcon(icopath);
	}
	setContextMenuPolicy(Qt::PreventContextMenu);
	titleWidget = new QLabel;
	titleWidget->setAlignment(Qt::AlignCenter);
	titleWidget->setText(txt);
	setTitleBarWidget(titleWidget);

	setFeatures(QDockWidget::DockWidgetMovable/* | QDockWidget::DockWidgetFloatable*/);
	connect(this, &xDockWidget::visibilityChanged, this, &xDockWidget::draw);
	connect(this, &xDockWidget::dockLocationChanged, this, &xDockWidget::moved);
}

// redraw all tabs icon/title
void xDockWidget::moved() {
	QWidget* par = parentWidget();
	if (!par) return;
	// tabData carries a raw pointer Qt put there, and a non-null one proves
	// nothing: it may name a plain QDockWidget (the edge anchors are not
	// xDockWidget) or one that has already gone. Reading icon/title off such a
	// pointer walks past the end of the object. Only touch the ones still
	// alive under this window.
	QSet<quintptr> live;
	foreach (xDockWidget* d, par->findChildren<xDockWidget*>())
		live.insert(reinterpret_cast<quintptr>(d));
	foreach (QTabBar* tb, par->findChildren<QTabBar*>()) {
		for (int i = 0; i < tb->count(); i++) {
			quintptr ptr = qvariant_cast<quintptr>(tb->tabData(i));
			if (!live.contains(ptr)) continue;
			xDockWidget* dw = reinterpret_cast<xDockWidget*>(ptr);
			tb->setTabIcon(i, dw->icon);
			dw->setWindowTitle(dw->title);
		}
	}
}

// xCheckItem

// the option the view hands over already carries the row's selection, hover and
// palette, and these columns have nothing else in them, so there is no call for
// QStyledItemDelegate::initStyleOption() and the model lookups it does

// ask the style about tmpl, not about the view: a check box is what the style
// sheets have a rule for

QSize xCheckItem::boxSize(const QWidget* wid) const {
	QStyleOptionButton bop;
	QStyle* stl = wid ? wid->style() : QApplication::style();
	return QSize(stl->pixelMetric(QStyle::PM_IndicatorWidth, &bop, &tmpl),
			stl->pixelMetric(QStyle::PM_IndicatorHeight, &bop, &tmpl));
}

void xCheckItem::paint(QPainter* pnt, const QStyleOptionViewItem& opt, const QModelIndex& idx) const {
	QStyleOptionViewItem iop(opt);
	iop.state &= ~QStyle::State_HasFocus;		// most of the cell is empty, a focus rect there says nothing
	QVariant bgr = idx.data(X_BackgroundRole);	// a row the model paints itself keeps its colour here too
	if (bgr.isValid())
		iop.backgroundBrush = qvariant_cast<QBrush>(bgr);
	const QWidget* wid = iop.widget;
	QStyle* stl = wid ? wid->style() : QApplication::style();
	stl->drawControl(QStyle::CE_ItemViewItem, &iop, pnt, wid);
	QVariant chk = idx.data(Qt::CheckStateRole);
	if (!chk.isValid()) return;			// this cell has no check box
	QStyleOptionButton bop;
	bop.state = QStyle::State_Enabled;
	if (iop.state & QStyle::State_MouseOver) bop.state |= QStyle::State_MouseOver;
	bop.state |= (chk.toInt() == Qt::Unchecked) ? QStyle::State_Off : QStyle::State_On;
	QSize box = boxSize(wid);
	bop.rect = QRect(opt.rect.x() + (opt.rect.width() - box.width()) / 2,
			opt.rect.y() + (opt.rect.height() - box.height()) / 2,
			box.width(), box.height());
	stl->drawPrimitive(QStyle::PE_IndicatorCheckBox, &bop, pnt, &tmpl);
}

// the default hint counts a text margin the cell has no text for

QSize xCheckItem::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const {
	QSize box = boxSize(opt.widget);
	return QSize(box.width() + 6, box.height() + 2);
}

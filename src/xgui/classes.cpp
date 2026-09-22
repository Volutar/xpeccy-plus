#include "xgui.h"
#include "../xcore/xcore.h"

#include <QPalette>
#include <QPainter>
#include <QStylePainter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStyleOptionComboBox>
#include <QApplication>
#include <QSet>
#include <QTabBar>
#include <QFontMetrics>
#include <QDebug>

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
	vtxt = "0000";
	// setValidator(&vldtr);
	setBase(16);
	setText(vtxt);
	connect(this, SIGNAL(textChanged(QString)), SLOT(onTextChange(QString)));
}

int xHexSpin::getValue() {
	return value;
}

void xHexSpin::updateMask() {
	unsigned mx;
	QString rxp;
	switch(base) {
		case 10:
			rxp = "[0-9]";
			//setStyleSheet("border:1px solid black;");
			break;
		default:
			base = 16;
			rxp = "[A-Fa-f0-9]";
			//setStyleSheet("border:1px solid green;");
			break;
	}
	// REG_32 gives max = 0xffffffff, which is -1 as int. count digits on the
	// unsigned value and divide, so it neither stops at once nor overflows
	len = 1;
	mx = (unsigned)max / base;
	while (mx) {
		mx /= base;
		len++;
	}
	rxp.append(QString("{%0}").arg(len));	// 'len' times this char
	// the mask is what puts the cursor in overwrite mode; the part after the
	// semicolon is what an empty position shows, for a field that may be empty
	setInputMask((hsflag & XHS_BLANK) ? (QString(len, 'h') + ";-") : QString(len, 'h'));
	setRegExp(vldtr, rxp);
	refitWidth();
}

// XHS_AUTOW: 'len' digits + a bit for the frame and cursor.
// call it after a font change too, not only when the value length changes
void xHexSpin::refitWidth() {
	if (!(hsflag & XHS_AUTOW)) return;
	QFontMetrics fm(font());
	setFixedWidth(fm.horizontalAdvance(QString(len, '0')) + 10);
}

void xHexSpin::setBase(int b) {
	int tmp = value;
	base = b;
	updateMask();
	hsflag |= XHS_UPD;			// update even if value doesn't changed
	setValue(tmp);
}

void xHexSpin::setXFlag(int xf) {
	hsflag = xf;
	updateMask();			// XHS_BLANK decides what an empty position shows
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
	if (ev->type() == QEvent::FontChange) refitWidth();
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

// How many bytes the field shows separately, 0 = it does not. Only hex lines
// digits up with bytes, and only a centered field has its text where paintEvent
// works it out to be - the two margins cancel out there.
int xHexSpin::splitBytes() {
	if (!(hsflag & XHS_SPLIT)) return 0;
	if ((base != 16) || (len < 4) || (len & 1)) return 0;
	if (!(alignment() & Qt::AlignHCenter)) return 0;
	return len / 2;
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

void xHexSpin::setMax(int v) {
	max = v;
	updateMask();
	if (value > max) setValue(max);
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
	if (p) setFont(p->font());
	setStyleSheet(str);
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
	int bytes = splitBytes();
	if (!chgMask || !bytes || wholeFieldLit()) return;
	QColor bg = conf.pal.value(DBG_PAL_CHG_BG);
	QColor fg = conf.pal.value(DBG_PAL_CHG_TXT);
	if (!bg.isValid() && !fg.isValid()) return;
	QString txt = text();
	if (txt.size() != bytes * 2) return;		// mid-edit: nothing lines up
	// inside the frame, so a lit byte does not paint over the field's border
	QRect cr = contentsRect();
	QFontMetrics fm = fontMetrics();
	int x = cr.x() + (cr.width() - fm.horizontalAdvance(txt)) / 2;
	QPainter pnt(this);
	if (fg.isValid()) pnt.setPen(fg);
	// left to right, so each byte's width is measured once and carries the
	// next one's position with it. Byte 0 is the last two digits
	for (int i = bytes - 1; i >= 0; i--) {
		QString dig = txt.mid((bytes - 1 - i) * 2, 2);
		int w = fm.horizontalAdvance(dig);
		if (chgMask & (1 << i)) {
			QRect r(x, cr.y(), w, cr.height());
			if (bg.isValid()) pnt.fillRect(r, bg);
			pnt.drawText(r, Qt::AlignCenter, dig);
		}
		x += w;
	}
}

void xHexSpin::onChange(int val) {
	int pos = cursorPosition();
	QString res = QString::number(val, base).toUpper();
	res = res.rightJustified(len, '0');
	if ((text() != res) || (hsflag & XHS_UPD)) {
		hsflag &= ~XHS_UPD;
		setText(res);
		setCursorPosition(pos);
	}
}

void xHexSpin::onTextChange(QString txt) {
	if (isblank) return;		// the dashes are not a number to read
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
		setText(vtxt);
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

xSideButton::xSideButton(QWidget* p):QPushButton(p) {}

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
	int gap = 6;
	if (!ico.isNull()) {
		QSize isz = iconSize();
		QRect irc(box.right() - isz.width() + 1, box.top() + (box.height() - isz.height()) / 2, isz.width(), isz.height());
		ico.paint(&pnt, irc, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);
		box.setRight(irc.left() - gap);
	}
	box.setLeft(box.left() + gap / 2);
	txt = fontMetrics().elidedText(txt, Qt::ElideRight, box.width());
	pnt.drawItemText(box, Qt::AlignLeft | Qt::AlignVCenter, palette(), isEnabled(), txt, QPalette::ButtonText);
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
	QLineEdit* edt = new QLineEdit(par);
	QString pat("[0-9A-Fa-f\\s]");
	int rpt = 0;
	switch (type) {
		case XTYPE_NONE: delete(edt); edt = NULL; break;
		case XTYPE_ADR: rpt = 4; break;
		case XTYPE_LABEL: break;
		case XTYPE_DUMP: rpt = 12; break;		// 6 bytes max
		case XTYPE_BYTE: rpt = 2; break;
	}
	if (edt && (rpt > 0)) {
		edt->setInputMask(QString(rpt,'h'));
		edt->setMaxLength(rpt);
#if QT_VERSION < QT_VERSION_CHECK(6,0,0)
		edt->setValidator(new QRegExpValidator(QRegExp(QString("%0+").arg(pat))));
#else
		edt->setValidator(new QRegularExpressionValidator(QRegularExpression(QString("%0+").arg(pat))));
#endif
	}
	return edt;
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

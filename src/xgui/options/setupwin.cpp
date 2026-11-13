#include <QThread>
#include <QTimer>
#include <QStandardItemModel>
#include <QInputDialog>
#include <QColorDialog>
#include <QFontDialog>
#include <QFileDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QToolButton>
#include <QRadioButton>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QMessageBox>
#include <QVector3D>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QStylePainter>
#include <QStyleOptionComboBox>
#include <QLineEdit>
#include <QIntValidator>
#include <QTextEdit>
#include <QAbstractTextDocumentLayout>
#include <QFontDatabase>
#include <QDateTime>
#include <QGuiApplication>
#include <QScreen>
#include <QDesktopServices>
#include <QUrl>
#include <QRegularExpression>
#include <QPointer>
#include <QSharedPointer>
#include <QShowEvent>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QAbstractSlider>
#include <QDebug>
#include <functional>
#include <stdlib.h>

#include <SDL.h>

#include "filer.h"
#include "setupwin.h"
#include "xcore/vidrec.h"
#include "xgui/xgui.h"
#include "xgui/favorites.h"
#include "xcore/gamepad.h"
#include "xcore/xcore.h"
#include "xcore/vscalers.h"
#include "xcore/sound.h"
#include "xcore/log.h"
#include "xcore/vfilters.h"
#include "xcore/vfat_scan.h"
#include "libxpeccy/spectrum.h"
#include "xcore/rewind.h"
#include "libxpeccy/filetypes/filetypes.h"
#include "libxpeccy/input/input.h"
#include "xcore/rzxrecord.h"
#include "xcore/serial.h"

// the turbo step lists the box offers before a machine of its own adds one
#define CPU_TURBO_ROWS	3

void setRFIndex(QComboBox* box, QVariant data, int defidx) {
	int idx = box->findData(data);
	if (idx < 0) idx = defidx;
	box->setCurrentIndex(idx);
}

int getRFIData(QComboBox* box) {
	bool isok = false;
	int res = box->itemData(box->currentIndex()).toInt(&isok);
	return isok ? res : -1;
}

QString getRFSData(QComboBox* box) {
	return box->itemData(box->currentIndex()).toString();
}

// a stereo separation slider, 0-100, and its label kept in step with it
static xSlider* sep_slider(QLabel** lab) {
	xSlider* sld = new xSlider;
	sld->setRange(0, 100);
	sld->setPageStep(5);
	sld->setTickInterval(25);
	sld->setOrientation(Qt::Horizontal);
	sld->setTickPosition(QSlider::TicksBelow);
	QLabel* l = new QLabel(QString("%0%").arg(sld->value()));
	l->setMinimumWidth(32);
	l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	QObject::connect(sld, &QSlider::valueChanged, l, [l](int v) {l->setText(QString("%0%").arg(v));});
	*lab = l;
	return sld;
}

// a ROM list on its file: one missing, or of the user's own from elsewhere, goes in
static void rom_box_pick(QComboBox* box, const QString& cur) {
	if (!cur.isEmpty() && (box->findData(cur) < 0)) box->insertItem(1, cur, cur);
	box->setCurrentIndex(qMax(0, box->findData(cur)));
}

std::string getRFText(QComboBox* box) {
	QString res = "";
	if (box->currentIndex() >= 0) res = box->currentText();
	return std::string(res.toLocal8Bit().data());
}

// the machines, parted by family the way the emulator's own menu parts them

void fill_machine_list(QComboBox* box) {
	box->clear();
	std::string family;
	foreach(const xMachine& mac, xm_list()) {
		if (!family.empty() && (mac.family != family))
			box->insertSeparator(9999);
		family = mac.family;
		box->addItem(xm_list_name(mac), QString::fromLocal8Bit(mac.id.c_str()));
	}
	box->setMaxVisibleItems(box->count());
}

void fill_layout_list(QComboBox* box, QString txt = QString()) {
	if (txt.isEmpty())
		txt = box->currentText();
	box->clear();
	foreach(xLayout ly, conf.layList) {
		box->addItem(QString::fromLocal8Bit(ly.name.c_str()));
	}
	box->setCurrentIndex(box->findText(txt));
}

void fill_shader_list(QComboBox* box) {
	QStringList lst = xres_list("shaders", QStringList() << "*.txt");
	box->clear();
	box->addItem("None", 0);
	if (conf.vid.shd_support) {
		foreach(QString nam, lst) {
			box->addItem(nam, 1);
		}
		box->setCurrentIndex(box->findText(conf.vid.shader.c_str()));
		if (box->currentIndex() < 0)
			box->setCurrentIndex(0);
	}
}

/*
void fill_palette_list(QComboBox* box) {
	QDir dir(conf.path.palDir.c_str());
	QFileInfoList lst = dir.entryInfoList(QStringList() << "*.txt", QDir::Files, QDir::Name);
	QFileInfo inf;
	box->clear();
	box->addItem("default");					// empty data (no filename = default pal)
	foreach(inf, lst) {
		box->addItem(inf.fileName(), inf.fileName());		// need data=text, cuz setRFIndex using data, not text
	}
	setRFIndex(box, conf.palette.c_str());
	if (box->currentIndex() < 0)
		box->setCurrentIndex(0);
}
*/

void fillComboBox(QComboBox* box, const char* kind, QStringList filt, QString def = "", QString sel = "") {
	box->clear();
	if (!def.isEmpty()) box->addItem(def);
	foreach(QString nam, xres_list(kind, filt)) {
		box->addItem(nam, nam);
	}
	setRFIndex(box, sel, 0);
}

// Indicators page: one checkbox carries the indicator, the icon as it is drawn
// on screen and the name. The style sets the gap after the check itself, and
// QCommonStyle puts the name 4px past iconSize - so pad the picture on the left
// and make iconSize wider than it to get the same air on both sides of it.

#define	LED_ICON_SIZE	16
#define	LED_ICON_GAP	10
#define	LED_ICON_TEXT	4	// QCommonStyle's own icon-to-text gap, which has no metric to ask for

static void spaceLedIcon(QCheckBox* box) {
	QPixmap src = box->icon().pixmap(QSize(LED_ICON_SIZE, LED_ICON_SIZE));
	if (src.isNull()) return;
	int pad = LED_ICON_GAP - box->style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, NULL, box);
	if (pad < 0) pad = 0;
	qreal dpr = src.devicePixelRatio();
	QPixmap pix(qRound((LED_ICON_SIZE + pad) * dpr), qRound(LED_ICON_SIZE * dpr));
	pix.setDevicePixelRatio(dpr);
	pix.fill(Qt::transparent);
	QPainter pnt(&pix);
	pnt.drawPixmap(pad, 0, src);
	pnt.end();
	box->setIcon(QIcon(pix));
	box->setIconSize(QSize(LED_ICON_SIZE + pad + LED_ICON_GAP - LED_ICON_TEXT, LED_ICON_SIZE));
}

// OBJECT


// Two-part list items: "value - detail". The value is drawn bold and the
// detail at reduced alpha, so no colour is hardcoded and every style sheet
// keeps working. Experimental, wired to the PSG boxes only. Items carrying an
// icon fall back to the plain painting - none of them has a detail part.

static const QString detail_sep = " - ";

static void draw_two_part(QPainter* pnt, QRect rc, const QFont& fnt, QColor col, const QString& txt, int cut) {
	QFont bld = fnt;
	bld.setBold(true);
	QString head = txt.left(cut);
	pnt->setFont(bld);
	pnt->setPen(col);
	pnt->drawText(rc, Qt::AlignVCenter | Qt::AlignLeft, head);
	QFontMetrics fmb(bld);
	rc.setLeft(rc.left() + fmb.horizontalAdvance(head) + fmb.horizontalAdvance(" "));
	col.setAlphaF(0.55);
	pnt->setFont(fnt);
	pnt->setPen(col);
	pnt->drawText(rc, Qt::AlignVCenter | Qt::AlignLeft, txt.mid(cut + detail_sep.size()));
}

// the popup

class xTwoPartDelegate : public QStyledItemDelegate {
	public:
		xTwoPartDelegate(QObject* par = NULL) : QStyledItemDelegate(par) {}

		void paint(QPainter* pnt, const QStyleOptionViewItem& op, const QModelIndex& idx) const {
			QStyleOptionViewItem opt = op;
			initStyleOption(&opt, idx);
			int cut = opt.text.indexOf(detail_sep);
			if ((cut < 0) || !opt.icon.isNull()) {
				QStyledItemDelegate::paint(pnt, op, idx);
				return;
			}
			QString txt = opt.text;
			const QWidget* wid = opt.widget;
			QStyle* sty = wid ? wid->style() : QApplication::style();
			opt.text.clear();
			sty->drawControl(QStyle::CE_ItemViewItem, &opt, pnt, wid);
			// the native style paints a light selection and keeps the normal
			// text colour on it, a style sheet sets its own pair
			int sel = (opt.state & QStyle::State_Selected) && !qApp->styleSheet().isEmpty();
			draw_two_part(pnt, opt.rect.adjusted(4, 0, -4, 0), opt.font,
				opt.palette.color(sel ? QPalette::HighlightedText : QPalette::Text), txt, cut);
		}

		QSize sizeHint(const QStyleOptionViewItem& op, const QModelIndex& idx) const {
			QSize sz = QStyledItemDelegate::sizeHint(op, idx);
			sz.rwidth() += 8;			// the bold half is a bit wider
			return sz;
		}
};

// The closed box is painted by the style, not by the delegate, so it needs a
// pass of its own. An editable combo keeps its text in a child QLineEdit and
// that one paints itself - hence the two cases. A filter does it without
// having to promote the widget in the .ui files.

class xTwoPartPainter : public QObject {
	public:
		xTwoPartPainter(QComboBox* box) : QObject(box), cbox(box) {
			QWidget* wid = box->isEditable() ? (QWidget*)box->lineEdit() : (QWidget*)box;
			if (wid) wid->installEventFilter(this);
			if (!box->isEditable()) return;
			// an editable box shows the plain text while it has the focus, so
			// let it go as soon as the value is picked or typed in
			connect(box, QOverload<int>::of(&QComboBox::activated), this, [this](int){
				cbox->lineEdit()->clearFocus();
			});
			connect(box->lineEdit(), &QLineEdit::returnPressed, this, [this](){
				cbox->lineEdit()->clearFocus();
			});
		}
	protected:
		bool eventFilter(QObject* obj, QEvent* ev) {
			if (ev->type() != QEvent::Paint) return false;
			QLineEdit* led = cbox->lineEdit();
			if (led && (obj == led)) return paintEdit(led);
			if (obj == cbox) return paintBox();
			return false;
		}
	private:
		QComboBox* cbox;

		int textCut(const QString& txt) {
			int cut = txt.indexOf(detail_sep);
			if (!cbox->itemIcon(cbox->currentIndex()).isNull()) cut = -1;
			return cut;
		}

		bool paintBox() {
			QString txt = cbox->currentText();
			int cut = textCut(txt);
			if (cut < 0) return false;
			QStylePainter pnt(cbox);
			QStyleOptionComboBox opt;
			opt.initFrom(cbox);
			opt.rect = cbox->rect();
			opt.subControls = QStyle::SC_All;
			opt.frame = cbox->hasFrame();
			if (cbox->view() && cbox->view()->isVisible())
				opt.state |= QStyle::State_On;
			pnt.drawComplexControl(QStyle::CC_ComboBox, opt);
			QRect rc = cbox->style()->subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, cbox);
			draw_two_part(&pnt, rc.adjusted(2, 0, -2, 0), cbox->font(), textColor(cbox), txt, cut);
			return true;
		}

		bool paintEdit(QLineEdit* led) {
			if (led->hasFocus()) return false;	// typing: a plain edit again
			QString txt = led->text();
			int cut = textCut(txt);
			if (cut < 0) return false;
			QPainter pnt(led);
			pnt.fillRect(led->rect(), led->palette().brush(led->backgroundRole()));
			draw_two_part(&pnt, led->rect().adjusted(2, 0, -2, 0), led->font(), textColor(led), txt, cut);
			return true;
		}

		QColor textColor(QWidget* wid) {
			return wid->palette().color(wid->isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::Text);
		}
};


// what the PSG row offers: how many chips, and whether they are the FM ones
enum {PSG_NONE = 0, PSG_ONE, PSG_TS, PSG_TSFM, PSG_NEXT};

// the clock presets, as an AY's
static const struct {double frq; const char* name;} psgFrqTab[] = {
	{1.773447, "ZX 128/+2/+3"},
	{1.75, "ZX 48/ZX-clones"},
};

void opt_fill_psg_boxes(QComboBox* cbcount, QComboBox* cbtype, QComboBox* cbfrq, QComboBox* cbstereo) {
	cbcount->clear();
	cbcount->addItem(QIcon(":/images/cancel.png"),"None",PSG_NONE);
	cbcount->addItem(QString::fromUtf8("×1 - AY/YM"),PSG_ONE);
	cbcount->addItem(QString::fromUtf8("×2 - TurboSound"),PSG_TS);
	cbcount->addItem(QString::fromUtf8("×2 - TurboSound FM"),PSG_TSFM);
	cbcount->addItem(QString::fromUtf8("×3 - ZX Next"),PSG_NEXT);
	cbtype->clear();
	cbtype->addItem(QIcon(":/images/MicrochipLogo.png"),find_chip_type(SND_AY)->name,SND_AY);
	cbtype->addItem(QIcon(":/images/YamahaLogo.png"),find_chip_type(SND_YM)->name,SND_YM);
	cbtype->addItem(QIcon(":/images/YamahaLogo.png"),find_chip_type(SND_YM2203)->name,SND_YM2203);
	cbfrq->clear();
	for (size_t i = 0; i <= sizeof(psgFrqTab) / sizeof(psgFrqTab[0]); i++)
		cbfrq->addItem(QString());	// Auto and the presets, filled in by chapsg()
	cbstereo->clear();
	cbstereo->addItem("Mono",AY_MONO);
	cbstereo->addItem("ABC",AY_ABC);
	cbstereo->addItem("ACB",AY_ACB);
	cbstereo->addItem("BAC",AY_BAC);
	cbstereo->addItem("BCA",AY_BCA);
	cbstereo->addItem("CAB",AY_CAB);
	cbstereo->addItem("CBA",AY_CBA);
}

// frequency items are "<mhz> - machines", so cut the comment off

double opt_get_psg_frq(QComboBox* box) {
	double frq = box->currentText().section(' ', 0, 0).toDouble();
	if ((frq < 0.1) || (frq > 10.0)) frq = 0.0;	// 0 : chip default
	return frq;
}

void opt_set_psg_frq(QComboBox* box, double frq) {
	for (int i = 0; i < box->count(); i++) {
		if (box->itemText(i).section(' ', 0, 0).toDouble() == frq) {
			box->setCurrentIndex(i);
			return;
		}
	}
	box->setCurrentText(QString::number(frq, 'g', 7));
}

// The machine's devices, made here rather than in the .ui: they live in the
// device pop-ups on the Machine page, which are built in code.
void SetupWin::makeDevWidgets() {
	cbTapeAuto = new QCheckBox;
	cbTapeRewind = new QCheckBox;
	cbTapeSet = new QCheckBox;
	cbTapeFast = new QCheckBox;
	cbTapeFlash = new QCheckBox;
	cbTapeEdge = new QCheckBox;
	bdtbox = new QCheckBox;
	cbAddBoot = new QCheckBox;
	a80box = new QCheckBox;
	b80box = new QCheckBox;
	c80box = new QCheckBox;
	d80box = new QCheckBox;
	adsbox = new QCheckBox;
	bdsbox = new QCheckBox;
	cdsbox = new QCheckBox;
	ddsbox = new QCheckBox;
	gsrbox = new QCheckBox;
	ratWheel = new QCheckBox;
	cbSwapButtons = new QCheckBox;
	diskTypeBox = new QComboBox;
	cbFlpInterleave = new QComboBox;
	hiface = new QComboBox;
	hm_type = new QComboBox;
	hs_type = new QComboBox;
	sdrvBox = new QComboBox;
	cbScanTab = new QComboBox;
	cbCpuTurbo = new QComboBox;
	cbPsgCount = new QComboBox;
	cbPsgType = new QComboBox;
	cbPsgFrq = new QComboBox;
	cbPsgStereo = new QComboBox;
	foreach(QComboBox* box, QList<QComboBox*>() << cbPsgCount << cbPsgType << cbPsgFrq) {
		box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		box->setMinimumContentsLength(8);
	}
	cbPsgFrq->setEditable(true);
	cbPsgFrq->setInsertPolicy(QComboBox::NoInsert);
	cbPsgStereo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	sldTapeSpeed = new xSlider;
	sldTapeSpeed->setRange(95, 105);
	sldTapeSpeed->setPageStep(1);
	sldTapeSpeed->setValue(100);
	sldTapeSpeed->setTickInterval(1);
	sldPsgSep = sep_slider(&labPsgSep);
	sldSensitivity = new xSlider;
	sldSensitivity->setRange(100, 1900);
	sldSensitivity->setSingleStep(10);
	sldSensitivity->setPageStep(100);
	sldSensitivity->setValue(1000);
	sldSensitivity->setTickInterval(100);
	sldGsSep = sep_slider(&labGsSep);
	foreach(QSlider* sld, QList<QSlider*>() << sldTapeSpeed << sldSensitivity) {
		sld->setOrientation(Qt::Horizontal);
		sld->setTickPosition(QSlider::TicksBelow);
	}
	labTapeSpeedVal = new QLabel("100%");
	labTapeSpeedVal->setMinimumWidth(34);
	labPsgMhz = new QLabel("MHz");
}

// Source, Screenshots' and Recording's: the machine's frame or the window's picture
static QComboBox* src_box(const QString& tip) {
	QComboBox* box = new QComboBox;
	box->addItem(SetupWin::tr("RAW frame"), VREC_SRC_PICTURE);
	box->addItem(SetupWin::tr("Screen"), VREC_SRC_SCREEN);
	box->setToolTip(tip);
	// no GL view: nothing to read the window back from
	if (!conf.vid.gl) box->setItemData(1, 0, Qt::UserRole - 1);
	return box;
}

static QComboBox* scale_box(int most, const QString& tip) {
	QComboBox* box = new QComboBox;
	for (int n = 1; n <= most; n++)
		box->addItem(QString("Scale x%0").arg(n), n);
	box->setToolTip(tip);
	return box;
}

// A file name template's keys, a rich tooltip that is a list. nobr: Qt wraps a
// rich tooltip at its own width, and a key is one line.
static QString name_tip(const QString& more) {
	return SetupWin::tr("<nobr><b>%d</b> date, 20260929</nobr><br><nobr><b>%t</b> time, 153012</nobr><br>"
		"<nobr><b>%image</b> the image in use, else the machine</nobr><br><nobr><b>%machine</b> the machine</nobr>") + more;
}

// the main window's, where the shots and the video are taken
static qreal main_dpr(QWidget* dlg) {
	QWidget* w = dlg->parentWidget() ? dlg->parentWidget()->window() : dlg;
	return w->devicePixelRatioF();
}

// how many of the window's pixels from pos, len long, are inside a room of size,
// at r pixels a point, rounded as the picture is drawn there
static int shown_span(int pos, int len, int size, qreal r) {
	int a = int(pos * r + 0.5);
	int b = a + int(len * r + 0.5);
	return qMin(b, int(size * r + 0.5)) - qMax(a, 0);
}

SetupWin::SetupWin(QWidget* par):QDialog(par) {
	setModal(true);
	ui.setupUi(this);
	machColMin = ui.machButtons->maximumWidth();
	appColMin = ui.appButtons->maximumWidth();
	makeDevWidgets();
	// a .ui iconset holds a single pixmap, which the title bar would have
	// to downscale; the application icon carries every drawn
	// size instead. It has to be set explicitly: an unset icon is inherited
	// from the parent window, which wears the pause icon while this dialog
	// is open.
	setWindowIcon(QGuiApplication::windowIcon());

	spaceLedIcon(ui.cbKeysLed);
	spaceLedIcon(ui.cbJoyLed);
	spaceLedIcon(ui.cbMouseLed);
	spaceLedIcon(ui.cbTapeLed);
	spaceLedIcon(ui.cbDiskLed);
	spaceLedIcon(ui.cbFpsLed);
	spaceLedIcon(ui.cbHaltLed);
	spaceLedIcon(ui.cbClockLed);
	spaceLedIcon(ui.cbMessage);
	spaceLedIcon(ui.cbSnowNote);
	spaceLedIcon(ui.cbFadeLed);
	// the notice is the window's, for every machine: one place for it
	QWidget* snowRow[] = {ui.cbSnow, ui.nam_cbSnow, ui.lab_cbSnow};
	for (QWidget* w : snowRow)
		w->setToolTip("The notice when a game snows:\nOptions > Appearance > Indicators");

	rseditor = new xRomsetEditor(this);
	rseditor->setModal(true);
	rsmodel = new xRomsetModel();
	ui.tvRomset->setModel(rsmodel);

	layeditor = new QDialog(this);
	layUi.setupUi(layeditor);
	layeditor->setModal(true);

	kedit = new xKeyEditor(this);

	int i;
	fill_machine_list(ui.machbox);

	for (i = 1; i <= 6; i++)
		ui.cbScale->addItem(QString("Scale x%0").arg(i), i);

	resTarget = RES_128;
	resGroup = new QButtonGroup(this);
	connect(resGroup, QOverload<QAbstractButton*>::of(&QButtonGroup::buttonClicked), this,
		[this](QAbstractButton* btn) {resTarget = resGroup->id(btn);});

	ui.tvRomset->setColumnWidth(0,50);
	ui.tvRomset->setColumnWidth(1,200);
	ui.tvRomset->setColumnWidth(2,70);
	ui.tvRomset->setColumnWidth(3,70);
	ui.tvRomset->setColumnWidth(4,70);
// video
	std::map<std::string,int>::iterator it;
	for (it = shotFormat.begin(); it != shotFormat.end(); it++) {
		// the key is the config value, the label is the format name as it is written
		QString key = QString::fromStdString(it->first);
		ui.ssfbox->addItem((key == "hobeta") ? QString("Hobeta") : key.toUpper(), key);
	}
	cbShotSrc = src_box(tr("RAW frame: the machine's picture, no shader\nScreen: the picture as the window shows it, shader and all"));
	cbShotScale = scale_box(SHOT_SCALE_MAX, tr("x1: a pixel for each dot of the machine;\n"
		"a hi-res mode keeps all its dots, lines doubled"));
	ui.horizontalLayout_3->insertWidget(0, cbShotSrc);
	ui.horizontalLayout_3->insertWidget(1, cbShotScale);
	labShotSize = new QLabel;
	labShotSize->setAlignment(Qt::AlignCenter);
	ui.ssColumn->insertWidget(1, labShotSize);
	// rows spread over a taller group would leave the column's behind
	ui.horizontalLayout_22->setAlignment(ui.gridLayout_3, Qt::AlignTop);
	foreach(QComboBox* box, QList<QComboBox*>() << cbShotSrc << cbShotScale << ui.ssfbox)
		connect(box, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SetupWin::showShotSize);
	connect(ui.ssNoBord, &QCheckBox::toggled, this, &SetupWin::showShotSize);
	connect(ui.bszsld, &QSlider::valueChanged, this, &SetupWin::showShotSize);
	connect(ui.cbScale, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SetupWin::showShotSize);
	connect(ui.cbFullscreen, &QCheckBox::toggled, this, &SetupWin::showShotSize);
	connect(ui.cbKeepRatio, &QCheckBox::toggled, this, &SetupWin::showShotSize);
	connect(ui.cbSideFill, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SetupWin::showShotSize);
	leShotName = new QLineEdit;
	leShotName->setPlaceholderText(SHOT_NAME_DEF);
	leShotName->setToolTip(name_tip(tr("<br><nobr><b>%n</b> number, 001 on</nobr>")));
	ui.gridLayout_3->addWidget(new QLabel(tr("Name")), 2, 0);
	ui.gridLayout_3->addWidget(leShotName, 2, 1);
	ui.cbContPattern->addItem("No contention", CONT_NONE);
	ui.cbContPattern->addItem("Ferranti (48K, 128K, +2)", CONT_PATA);
	ui.cbContPattern->addItem("Amstrad (+2A, +3)", CONT_PATB);
	ui.cbFloatBus->addItem("None", FBUS_NONE);
	ui.cbFloatBus->addItem("Ferranti ULA", FBUS_ULA);
	ui.cbFloatBus->addItem("Amstrad gate array", FBUS_ASIC);
	ui.cbFloatBus->addItem("Port #FF (clones)", FBUS_ATTR);
	ui.cbEarBack->addItem("Issue 3", EAR_ISSUE3);
	ui.cbEarBack->addItem("Issue 2", EAR_ISSUE2);
	ui.cbEarBack->addItem("Nothing", EAR_NONE);
	{
		static const char* names[] = {"black", "stretched edges", "mirror"};
		for (int i = 0; i < SFILL_ITEMS; i++) {
			const int item = sfillItems[i];
			QString nam = names[item & ~SFILL_BORDER];
			if ((i > 0) && !(item & SFILL_BORDER) && (sfillItems[i - 1] & SFILL_BORDER))
				ui.cbSideFill->insertSeparator(ui.cbSideFill->count());
			if (item & SFILL_BORDER) {
				nam.prepend("Border + ");
			} else {
				nam[0] = nam[0].toUpper();
			}
			ui.cbSideFill->addItem(nam, item);
		}
	}
	makeFillPopup();
	ui.bszsld->setMaximum(VID_BRD_OVERSCAN);	// one tick per border size
	ui.sldSpeed->setMaximum(XSPD_MAX);		// one tick per step of the overclock scale
	ui.cbCpuFrq->addItem("3.5 MHz");
	ui.cbCpuFrq->addItem("3.5469 MHz");
	cbCpuTurbo->addItem(QString::fromUtf8("×1 (no turbo)"), "1");
	cbCpuTurbo->addItem(QString::fromUtf8("×1, ×2"), "1,2");
	cbCpuTurbo->addItem(QString::fromUtf8("×1, ×2, ×4"), "1,2,4");

//	ui.cbScanlines->setVisible(false);
	fill_shader_list(ui.cbShader);
	fillPalettes();
	paleditor = new xPalEditor(this);
	paleditor->setModal(true);
	ui.cbNoflicMode->addItem("2-frames (fullscreen)", AF_2C_FULL);
	ui.cbNoflicMode->addItem("2-frames (adaptive)", AF_2C_ADAPTIVE);
	ui.cbNoflicMode->addItem("3-frames (fullscreen)", AF_3C_FULL);
	ui.cbNoflicMode->addItem("2-/3-frames (adaptive)", AF_3C_ADAPTIVE);
// emulation
	ui.cbRunAhead->addItem("Off", 0);
	ui.cbRunAhead->addItem("1", 1);
	ui.cbRunAhead->addItem("2", 2);
	for (int i = 0; xtime_slow_steps[i]; i++)
		ui.cbSlowmo->addItem(QString("1/%0").arg(xtime_slow_steps[i]), xtime_slow_steps[i]);
	for (int i = 0; xtime_ffwd_steps[i]; i++)
		ui.cbFfwd->addItem(QString::fromUtf8("\u00d7%0").arg(xtime_ffwd_steps[i]), xtime_ffwd_steps[i]);
	for (int f : {15, 30, 50, 60})
		ui.cbFastFps->addItem(QString::number(f), f);
	ui.cbFastFps->setValidator(new QIntValidator(1, 100, ui.cbFastFps));
	ui.cbFastFps->setInsertPolicy(QComboBox::NoInsert);
	ui.cbFastPic->addItem("Raster", FPIC_RASTER);
	ui.cbFastPic->setItemData(0, "The normal picture: drawn line by line,\nas at normal speed", Qt::ToolTipRole);
	ui.cbFastPic->addItem("Memory dump", FPIC_WHOLE);
	ui.cbFastPic->setItemData(1, "A simplified picture: video memory\nat a frame end, no multicolor", Qt::ToolTipRole);
	// A few percent of speed is not worth a choice nobody can make: the list is hidden,
	// fast.picture in the config still sets it and Apply keeps what it holds
	ui.cbFastPic->setVisible(false);
	for (QComboBox* box : {ui.cbSlowmoKey, ui.cbFfwdKey, ui.cbFastKey}) {
		box->addItem("Toggle", XHOLD_TOGGLE);
		box->addItem("Hold", XHOLD_HOLD);
		box->addItem("Tap or hold", XHOLD_HYBRID);
	}
	// the rewind's own settings mean nothing while it is off
	for (QWidget* w : {(QWidget*)ui.sbRewStep, (QWidget*)ui.labRewStep, (QWidget*)ui.sbRewSecs, (QWidget*)ui.labRewSecs})
		connect(ui.cbRewind, &QCheckBox::toggled, w, &QWidget::setEnabled);
	// the port means nothing while DeZog's server is off
	connect(ui.cbDezog, &QCheckBox::toggled, ui.sbDezogPort, &QWidget::setEnabled);
// sound
	i = 0;
	while (sndTab[i].name) {
		ui.outbox->addItem(QString::fromLocal8Bit(sndTab[i].name));
		i++;
	}
	ui.ratbox->addItem("Auto",0);		// the device's own rate, filled in by start()
	for (i = 0; sndRateTab[i]; i++) {
		ui.ratbox->addItem(QString::number(sndRateTab[i]), sndRateTab[i]);
	}
	opt_fill_psg_boxes(cbPsgCount, cbPsgType, cbPsgFrq, cbPsgStereo);
	cbPsgCount->setItemDelegate(new xTwoPartDelegate(cbPsgCount));
	cbPsgFrq->setItemDelegate(new xTwoPartDelegate(cbPsgFrq));
	new xTwoPartPainter(cbPsgCount);
	new xTwoPartPainter(cbPsgFrq);
	sdrvBox->addItem("None",SDRV_NONE);
	sdrvBox->addItem("Covox only",SDRV_COVOX);
	sdrvBox->addItem("Soundrive 1.05 mode 1",SDRV_105_1);
	sdrvBox->addItem("Soundrive 1.05 mode 2",SDRV_105_2);
// flp
	diskTypeBox->addItem("None",DIF_NONE);
	diskTypeBox->addItem("Beta Disk (WD1793)",DIF_BDI);
	diskTypeBox->addItem("+3 (uPD765)",DIF_P3DOS);
	ui.cbAutorun->addItem("Run opened media", AR_RUN);
	ui.cbAutorun->addItem("Mount opened media", AR_MOUNT);
	ui.cbAutorun->addItem("Ask: run or mount", AR_ASK);
	// the order flp_format_trk_buf() lays the 16 sectors out in, for each value
	cbFlpInterleave->addItem("1, 9, 2, 10, 3… (TR-DOS)", 8);
	cbFlpInterleave->addItem("1, 2, 3, 4, 5… (in a row)", 1);
	cbFlpInterleave->addItem("1, 3, 5, 7, 9…", 2);
	cbFlpInterleave->addItem("1, 4, 7, 10, 13…", 3);
	cbFlpInterleave->addItem("1, 5, 9, 13, 2…", 4);
	cbFlpInterleave->addItem("1, 6, 11, 16, 2…", 5);
	cbFlpInterleave->addItem("1, 7, 13, 2, 8…", 6);
	cbFlpInterleave->addItem("1, 8, 15, 2, 9…", 7);
// hdd
	hiface->addItem("None",IDE_NONE);
	hiface->addItem("NemoIDE",IDE_NEMO);
	hiface->addItem("NemoIDE A8",IDE_NEMOA8);
	hiface->addItem("NemoIDE (ZX Evo)",IDE_NEMO_EVO);
	hiface->addItem("SMUC",IDE_SMUC);
	hiface->addItem("ATM Turbo",IDE_ATM);
	hiface->addItem("Profi",IDE_PROFI);
	hm_type->addItem(QIcon(":/images/cancel.png"),"Not connected",IDE_NONE);
	hm_type->addItem(QIcon(":/images/hdd.png"),"HDD (ATA)",IDE_ATA);
	hs_type->addItem(QIcon(":/images/cancel.png"),"Not connected",IDE_NONE);
	hs_type->addItem(QIcon(":/images/hdd.png"),"HDD (ATA)",IDE_ATA);
// input
	// the pads have a window of their own, live; this only says what is in it
	{
		xIconGroup* grp = new xIconGroup(":/images/gamepad.png", tr("Gamepads"));
		QGridLayout* grd = new QGridLayout(grp);
		grd->setColumnStretch(1, 1);
		for (int i = 0; i < 2; i++) {
			padSum[i] = new QLabel;
			grd->addWidget(new QLabel(tr("Player %0").arg(i + 1)), i, 0);
			grd->addWidget(padSum[i], i, 1);
		}
		cbPadFiles = new QCheckBox(tr("Game bindings"));
		QLabel* lab = new QLabel(tr("Player 1 takes a .pad named after the image, until the next one"));
		QFont fnt = lab->font();
		fnt.setItalic(true);
		lab->setFont(fnt);
		QPushButton* btn = new QPushButton(tr("Gamepads..."));
		connect(btn, &QPushButton::clicked, this, [this]() {
			emit s_padwin();	// returns when the window is closed
			padSummary();
		});
		grd->addWidget(btn, 2, 0, 1, 2, Qt::AlignLeft);
		// a row of its own, so the players' column keeps its width
		QHBoxLayout* gbox = new QHBoxLayout;
		gbox->setSpacing(12);
		gbox->addWidget(cbPadFiles);
		gbox->addWidget(lab, 1);
		grd->addLayout(gbox, 3, 0, 1, 2);
		ui.verticalLayout_2->addWidget(grp);
		ui.verticalLayout_2->addStretch(1);
	}
	// Keymap and Typing: short lists, and what the pick does beside each
	// (connected before they are filled, so the first fill writes the notes too)
	{
		QLabel* kmNote = new QLabel;
		QLabel* tyNote = new QLabel;
		for (QLabel* lab : {kmNote, tyNote}) {
			QFont fnt = lab->font();
			fnt.setItalic(true);
			lab->setFont(fnt);
		}
		ui.gridLayout_21->addWidget(kmNote, 1, 2);
		ui.gridLayout_21->addWidget(tyNote, 2, 2);
		ui.gridLayout_21->setColumnStretch(1, 0);
		ui.gridLayout_21->setColumnStretch(2, 1);
		ui.gridLayout_21->setHorizontalSpacing(12);
		ui.keyMapBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
		ui.typingBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
		connect(ui.keyMapBox, QOverload<int>::of(&QComboBox::currentIndexChanged), kmNote, [this, kmNote]() {
			QString nam = ui.keyMapBox->currentData().toString();
			kmNote->setText(nam.isEmpty() ? tr("Both Shifts are Caps Shift, both Ctrls Symbol Shift")
				: (nam == KMAP_48K) ? tr("Caps Shift on the left, Symbol Shift on the right")
				: tr("Your own keymap"));
		});
		connect(ui.typingBox, QOverload<int>::of(&QComboBox::currentIndexChanged), tyNote, [this, tyNote]() {
			tyNote->setText(getRFIData(ui.typingBox) ? tr("Symbol keys as on the PC: Shift+; is :, Shift+/ is ?")
				: tr("Shift is Caps Shift with every key, as on the Spectrum"));
		});
		ui.typingBox->addItem("PC Symbols", 1);
		ui.typingBox->addItem("Key Positions", 0);
	}
	cbScanTab->addItem("As the machine has it", 0);
	cbScanTab->addItem("Scanset 1 (XT)", KBD_XT);
	cbScanTab->addItem("Scanset 2 (AT)", KBD_AT);
	cbScanTab->addItem("Scanset 3 (PS/2)", KBD_PS2);
// all
	connect(ui.okbut,SIGNAL(released()),this,SLOT(okay()));
	connect(ui.apbut,SIGNAL(released()),this,SLOT(apply()));
	connect(ui.cnbut,SIGNAL(released()),this,SLOT(reject()));
// machine
	connect(ui.machbox,SIGNAL(currentIndexChanged(int)),this,SLOT(setmszbox(int)));
	connect(ui.tvRomset,SIGNAL(doubleClicked(QModelIndex)),this,SLOT(editRom()));
	// a rom set is the machine's own now, so there is none to add or delete
	connect(rseditor,SIGNAL(complete(xRomFile)),this,SLOT(setRom(xRomFile)));
	connect(ui.tbAddRom,SIGNAL(released()),this,SLOT(addRom()));
	connect(ui.tbEditRom,SIGNAL(released()),this,SLOT(editRom()));
	connect(ui.tbDelRom,SIGNAL(released()),this,SLOT(delRom()));
	connect(ui.tbPreset,SIGNAL(released()),this,SLOT(romPreset()));
	connect(ui.pbResetMachine,SIGNAL(released()),this,SLOT(resetMachine()));
	connect(ui.pbSaveMachine,SIGNAL(released()),this,SLOT(saveMachine()));
	connect(ui.pbDelMachine,SIGNAL(released()),this,SLOT(delMachine()));
	connect(ui.pbCfgExport,SIGNAL(released()),this,SLOT(cfgExport()));
	connect(ui.pbCfgImport,SIGNAL(released()),this,SLOT(cfgImport()));
	connect(ui.pbCfgReset,SIGNAL(released()),this,SLOT(cfgReset()));

	// The settings that define the machine rather than how it is used live in
	// a window of their own. It is the same widgets, moved out of the page -
	// so everything that reads and writes them stays as it is.
	advWin = popOut(ui.advBox, "Machine: Advanced Settings");
	connect(ui.pbAdvMachine, &QPushButton::released, advWin, [this]() {
		advWin->adjustSize();
		advWin->show();
		advWin->raise();
	});

	// the page keeps one button for the ROM set; the slots and the reset
	// target are in this window
	romSetWin = popOut(ui.romBox, "Machine: ROM");
	connect(ui.pbRomSet, &QPushButton::released, romSetWin, [this]() {romSetWin->adjustSize(); romSetWin->show(); romSetWin->raise();});

	// same for the set file by file: the slots say which file is in which
	// one, this window has the offsets and sizes behind it
	romWin = popOut(ui.romAdvBox, "Machine: ROM Files");
	romWin->resize(620, 340);
	QPushButton* pbExpert = romSetWin->findChild<QDialogButtonBox*>()->addButton(tr("Expert settings"), QDialogButtonBox::ActionRole);
	pbExpert->setIcon(QIcon(":/images/settings.png"));
	pbExpert->setToolTip(tr("Where each file starts, how much of it is read, and where it lands"));
	connect(pbExpert, SIGNAL(released()), this, SLOT(showRomFiles()));
	buildStart();
	buildDevices();
	buildRecording();
	buildFolders();
	buildPause();
	buildSaves();
	ui.vidViewGBox->setIcon(":/images/grp-picture.png");
	ui.antiflickerGBox->setIcon(":/images/grp-antiflicker.png");
	ui.groupBox->setIcon(":/images/grp-screenshot.png");
	ui.gbRecording->setIcon(":/images/grp-record.png");
	ui.gbCpu->setIcon(":/images/grp-cpu.png");
	ui.gbUla->setIcon(":/images/grp-ula.png");
	ui.gbBoard->setIcon(":/images/grp-board.png");
// media
	// the file types sit on the page itself, there is room for them now
	ftbox = new xFileTypesBox;
	QGroupBox* ftgrp = new QGroupBox(tr("File types"));
	QVBoxLayout* ftlay = new QVBoxLayout(ftgrp);
	QLabel* fthint = new QLabel(tr("Which machine a snapshot, a tape or a disk is opened on"));
	QFont fthf = fthint->font();
	fthf.setItalic(true);
	fthint->setFont(fthf);
	ftlay->addWidget(fthint);
	ftlay->addWidget(ftbox, 1);
	QPushButton* ftdef = new QPushButton(tr("Restore defaults"));
	ftdef->setToolTip("Every format back to Auto");
	connect(ftdef, &QPushButton::released, ftbox, [this]() {ftbox->defaults();});
	QHBoxLayout* ftbtn = new QHBoxLayout;
	ftbtn->addStretch(1);
	ftbtn->addWidget(ftdef);
	ftlay->addLayout(ftbtn);
	ui.verticalLayout_29->insertWidget(1, ftgrp, 1);
// video
	connect(ui.pathtb,SIGNAL(released()),this,SLOT(selsspath()));
	connect(ui.bszsld,SIGNAL(valueChanged(int)),this,SLOT(chabsz()));
	connect(ui.sldSpeed,SIGNAL(valueChanged(int)),this,SLOT(chaspd()));
	connect(ui.sldNoflic,SIGNAL(valueChanged(int)),this,SLOT(chaflc()));
	connect(sldPsgSep,SIGNAL(valueChanged(int)),this,SLOT(chapsg()));
	connect(ui.sldSndLatency,SIGNAL(valueChanged(int)),this,SLOT(chasndlat()));
	connect(cbPsgCount,SIGNAL(currentIndexChanged(int)),this,SLOT(chapsg()));
	connect(ui.cbCpuFrq,SIGNAL(currentTextChanged(QString)),this,SLOT(chapsg()));
	connect(ui.cbSnow,SIGNAL(toggled(bool)),this,SLOT(chasnow()));
	connect(cbPsgStereo,SIGNAL(currentIndexChanged(int)),this,SLOT(chapsg()));

	connect(ui.layEdit,SIGNAL(released()),this,SLOT(edLayout()));
	connect(ui.layAdd,SIGNAL(released()),this,SLOT(addNewLayout()));
	connect(ui.layDel,SIGNAL(released()),this,SLOT(delLayout()));

	connect(ui.tbPalEdit,SIGNAL(released()),this,SLOT(paledit()));
	connect(ui.tbPalRestore,SIGNAL(released()),this,SLOT(palRestore()));
	connect(ui.cbPalPreset,SIGNAL(currentIndexChanged(int)),this,SLOT(palButtons()));

	for (int i = 0; i < XLL_COUNT; i++)
		ui.cbxLogLevel->addItem(QString(xlog_level_name(i)).toLower(), i);
	connect(ui.tbLogDir,SIGNAL(released()),this,SLOT(selLogDir()));
	connect(ui.tbLogOpen,SIGNAL(released()),this,SLOT(openLogDir()));
	connect(paleditor, SIGNAL(ready()), this, SLOT(palstore()));

	connect(layUi.layName,SIGNAL(textChanged(QString)),this,SLOT(layNameCheck(QString)));
	connect(layUi.lineBox,SIGNAL(valueChanged(int)),this,SLOT(layEditorChanged()));
	connect(layUi.rowsBox,SIGNAL(valueChanged(int)),this,SLOT(layEditorChanged()));
	connect(layUi.brdLBox,SIGNAL(valueChanged(int)),this,SLOT(layEditorChanged()));
	connect(layUi.brdUBox,SIGNAL(valueChanged(int)),this,SLOT(layEditorChanged()));
	connect(layUi.hsyncBox,SIGNAL(valueChanged(int)),this,SLOT(layEditorChanged()));
	connect(layUi.vsyncBox,SIGNAL(valueChanged(int)),this,SLOT(layEditorChanged()));
	connect(layUi.intLenBox,SIGNAL(valueChanged(int)),this,SLOT(layEditorChanged()));
	connect(layUi.intPosBox,SIGNAL(valueChanged(int)),this,SLOT(layEditorChanged()));
	connect(layUi.intRowBox,SIGNAL(valueChanged(int)),this,SLOT(layEditorChanged()));
	connect(layUi.sbScrH,SIGNAL(valueChanged(int)),this,SLOT(layEditorChanged()));
	connect(layUi.sbScrW,SIGNAL(valueChanged(int)),this,SLOT(layEditorChanged()));
	connect(layUi.okButton,SIGNAL(released()),this,SLOT(layEditorOK()));
	connect(layUi.cnButton,SIGNAL(released()),layeditor,SLOT(hide()));
// sound
	connect(ui.sldMasterVol,SIGNAL(valueChanged(int)),ui.sbMasterVol,SLOT(setValue(int)));
	connect(ui.sldBeepVol,SIGNAL(valueChanged(int)),ui.sbBeepVol,SLOT(setValue(int)));
	connect(ui.sldTapeVol,SIGNAL(valueChanged(int)),ui.sbTapeVol,SLOT(setValue(int)));
	connect(ui.sldAYVol,SIGNAL(valueChanged(int)),ui.sbAYVol,SLOT(setValue(int)));
	connect(ui.sldGSVol,SIGNAL(valueChanged(int)),ui.sbGSVol,SLOT(setValue(int)));
	connect(ui.sldSdrvVol,SIGNAL(valueChanged(int)),ui.sbSdrvVol,SLOT(setValue(int)));
	connect(ui.sldSAAVol,SIGNAL(valueChanged(int)),ui.sbSAAVol,SLOT(setValue(int)));

	connect(ui.sbMasterVol,SIGNAL(valueChanged(int)),ui.sldMasterVol,SLOT(setValue(int)));
	connect(ui.sbBeepVol,SIGNAL(valueChanged(int)),ui.sldBeepVol,SLOT(setValue(int)));
	connect(ui.sbTapeVol,SIGNAL(valueChanged(int)),ui.sldTapeVol,SLOT(setValue(int)));
	connect(ui.sbAYVol,SIGNAL(valueChanged(int)),ui.sldAYVol,SLOT(setValue(int)));
	connect(ui.sbGSVol,SIGNAL(valueChanged(int)),ui.sldGSVol,SLOT(setValue(int)));
	connect(ui.sbSdrvVol,SIGNAL(valueChanged(int)),ui.sldSdrvVol,SLOT(setValue(int)));
	connect(ui.sbSAAVol,SIGNAL(valueChanged(int)),ui.sldSAAVol,SLOT(setValue(int)));
// tape
	// flash loading and edge detection only refine fast loading
	connect(cbTapeFast, &QCheckBox::toggled, cbTapeFlash, &QWidget::setEnabled);
	connect(cbTapeFast, &QCheckBox::toggled, cbTapeEdge, &QWidget::setEnabled);
	connect(cbTapeRewind, &QCheckBox::toggled, cbTapeSet, &QWidget::setEnabled);
	connect(sldTapeSpeed, &QSlider::valueChanged, this, [this](int v){
		labTapeSpeedVal->setText(QString("%0%").arg(v));
	});
//tools
	connect(ui.pbFavorites, &QPushButton::clicked, this, [this]() {fav_manage(this);});
// debuga
	portwid = new xPortWatch;		// the same editor the debugger opens itself
	ui.layDbgPorts->addWidget(portwid);
	connect(ui.tbDbgFont,SIGNAL(released()),this,SLOT(selectDbgFont()));
	ui.cbDbgNumbers->addItem("#FFFF", CPU_NUM_HASH);
	ui.cbDbgNumbers->addItem("$FFFF", CPU_NUM_DOLLAR);
	ui.cbDbgNumbers->addItem("FFFFh", CPU_NUM_H);
	ui.cbDbgNumbers->addItem("65535", CPU_NUM_DEC);
	// the arrows step by 2, a typed-in odd value snaps once the field is left
	connect(ui.sbDbgStackOfs, &QAbstractSpinBox::editingFinished, this, [this](){
		ui.sbDbgStackOfs->setValue(ui.sbDbgStackOfs->value() & ~1);
	});
// palette
	QToolButton* tbarr[] = {
		ui.tbDbgChaBG, ui.tbDbgChaFG, ui.tbDbgHeadBG, ui.tbDbgHeadFG,
		/*ui.tbDbgTxtCol, ui.tbDbgWinCol, ui.tbDbgInputBG, ui.tbDbgInputFG,
		ui.tbDbgTableBG, ui.tbDbgTableFG,*/ ui.tbDbgPcBG, ui.tbDbgPcFG,
		ui.tbDbgSelBG, ui.tbDbgSelFG,
		ui.tbDbgBrkFG,
		ui.tbDbgDskIdBG, ui.tbDbgDskIdFG,
		ui.tbDbgDskDataBG, ui.tbDbgDskDataFG,
		ui.tbDbgDskCrcBG, ui.tbDbgDskCrcFG,
		ui.tbDbgFmBG, ui.tbDbgFmFG,
		ui.tbDbgAsmConstFG,
		NULL
	};
	i = 0;
	while (tbarr[i] != NULL) {
		connect(tbarr[i], SIGNAL(released()), this, SLOT(selectColor()));
		connect(tbarr[i], SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(triggerColor()));
		if (tbarr[i]->toolTip().isEmpty())
			tbarr[i]->setToolTip(tr("Right click: back to the style color"));
		i++;
	}
// profiles manager
	buildSidebar();
	ui.verticalLayout_40->insertWidget(0, ui.tvHotkeys->controls());
	// a row's name and what it says go to its control: a click on them toggles a box
	label_rows(this);
}

// The page in front, as tables show a selection, with a bar in the style's link color.
// Every icon gets the same square, or a narrow one pulls its name to the left.
class xPageItem : public QStyledItemDelegate {
	public:
		xPageItem(QObject* p) : QStyledItemDelegate(p) {}
		QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
			return QSize(QStyledItemDelegate::sizeHint(opt, idx).width(), 32);
		}
		// the selection in the theme's own colors: the native style frames it in its blue
		void paint(QPainter* pnt, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
			QStyleOptionViewItem o(opt);
			initStyleOption(&o, idx);
			bool sel = o.state & QStyle::State_Selected;
			o.state &= ~(QStyle::State_Selected | QStyle::State_HasFocus | QStyle::State_MouseOver);
			if (sel) {
				pnt->fillRect(o.rect, o.palette.color(QPalette::Highlight));
				o.palette.setColor(QPalette::Text, o.palette.color(QPalette::HighlightedText));
				pnt->fillRect(QRect(o.rect.left(), o.rect.top(), 3, o.rect.height()), o.palette.color(QPalette::Link));
			}
			const QWidget* wid = o.widget;
			(wid ? wid->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &o, pnt, wid);
		}
	protected:
		void initStyleOption(QStyleOptionViewItem* opt, const QModelIndex& idx) const override {
			QStyledItemDelegate::initStyleOption(opt, idx);
			opt->decorationSize = opt->widget ? static_cast<const QAbstractItemView*>(opt->widget)->iconSize() : opt->decorationSize;
		}
};

// The pages are a list down the left side: one level, and room for as many as there are.
void SetupWin::buildSidebar() {
	QListWidget* list = ui.pageList;
	list->setItemDelegate(new xPageItem(list));
	// as wide as the longest name: the list measures nothing until it is shown
	int wid = 0;
	for (int i = 0; i < list->count(); i++)
		wid = qMax(wid, list->fontMetrics().horizontalAdvance(list->item(i)->text()));
	list->setFixedWidth(wid + list->iconSize().width() + 36);
	// a page starts level with the list
	for (int i = 0; i < ui.pages->count(); i++) {
		if (QLayout* lay = ui.pages->widget(i)->layout()) {
			QMargins mrg = lay->contentsMargins();
			mrg.setTop(0);
			lay->setContentsMargins(mrg);
		}
	}
	connect(list, &QListWidget::currentRowChanged, ui.pages, &QStackedWidget::setCurrentIndex);
	list->setCurrentRow(0);
}

void SetupWin::okay() {
	apply();
	reject();
}

void setToolButtonColor(QToolButton* tb, QString nm, QString dc) {
	QColor col = conf.pal[nm];
	if (!col.isValid()) col = dc;
	if (!col.isValid()) col = QColor(0,0,0,0);	// transparent
	QPixmap pxm(16,16);
	pxm.fill(col);
	tb->setIcon(QIcon(pxm));
	tb->setProperty("colorName", nm);
	tb->setProperty("defaultColor", dc);
}

void SetupWin::fillDbgPalette() {
	static const char* names[] = {
		"dbg.header.bg", "dbg.header.txt",
		"dbg.changed.bg", "dbg.changed.txt",
		"dbg.pc.bg", "dbg.pc.txt",
		"dbg.sel.bg", "dbg.sel.txt",
		"dbg.brk.txt",
		"dbg.disk.id.bg", "dbg.disk.id.txt",
		"dbg.disk.data.bg", "dbg.disk.data.txt",
		"dbg.disk.crc.bg", "dbg.disk.crc.txt",
		"dbg.fmaps.bg", "dbg.fmaps.txt",
		"dbg.asm.const.txt",
		NULL
	};
	QToolButton* tb[] = {
		ui.tbDbgHeadBG, ui.tbDbgHeadFG,
		ui.tbDbgChaBG, ui.tbDbgChaFG,
		ui.tbDbgPcBG, ui.tbDbgPcFG,
		ui.tbDbgSelBG, ui.tbDbgSelFG,
		ui.tbDbgBrkFG,
		ui.tbDbgDskIdBG, ui.tbDbgDskIdFG,
		ui.tbDbgDskDataBG, ui.tbDbgDskDataFG,
		ui.tbDbgDskCrcBG, ui.tbDbgDskCrcFG,
		ui.tbDbgFmBG, ui.tbDbgFmFG,
		ui.tbDbgAsmConstFG
	};
	// the default a right click puts back is the same one the emulator starts
	// with, and the same one "System" restores
	for (int i = 0; names[i]; i++)
		setToolButtonColor(tb[i], names[i], dbgPaletteDefault(names[i]));
}

// What a grid's column really needs: the widest cell in it, taking a widget
// pinned to a fixed width at that width rather than at the hint it asks for.
static int gridColWidth(QGridLayout* grid, int col) {
	int wid = 0;
	for (int i = 0; i < grid->count(); i++) {
		int row, cl, rspan, cspan;
		grid->getItemPosition(i, &row, &cl, &rspan, &cspan);
		QWidget* w = grid->itemAt(i)->widget();
		if (w && (cl == col) && (cspan == 1))
			wid = qMax(wid, qMin(w->sizeHint().width(), w->maximumWidth()));
	}
	return wid;
}

// the Hotkeys page, its search ready: the list of keys a user asks for
void SetupWin::startHotkeys() {
	start();
	ui.pageList->setCurrentRow(ui.pages->indexOf(ui.tab_13));
	ui.tvHotkeys->focusFilter();
}

// the key read from the hotkeys as they are, so another preset reads right
static void fullscreenTip(QCheckBox* box) {
	QString key = shortcut_key(XCUT_FULLSCR);
	box->setToolTip(key.isEmpty() ? QString("Fill the screen with the picture")
		: QString("Fill the screen with the picture. %0 does the same").arg(key));
}

void SetupWin::start() {
	fillPages();
	bool shown = isVisible();
	show();
	if (!shown) center_over(this, parentWidget() ? parentWidget()->window() : nullptr);
}

// --bench-apply: Ok pressed on the pages as opening Options fills them, nothing shown.
// The bench runs on no sound output and leaves no files behind.
void SetupWin::benchApply() {
	inBench = true;
	fillPages();
	okay();
	inBench = false;
}

// the pages, from the machine and the settings as they stand
void SetupWin::fillPages() {
	Computer* comp = conf.zx;
	fillLogPage();
	QStringList ports = serial_ports();
	for (int i = 0; i < 2; i++) {
		uartBox[i]->clear();
		uartBox[i]->addItem(tr("Not connected"), QString());
		if (i == 0) uartBox[i]->addItem(tr("ESP-01 (emulated)"), QString(SERIAL_ESP));
		foreach(QString p, ports) uartBox[i]->addItem(p, p);
		QString cur = serial_name(i);
		int k = uartBox[i]->findData(cur);
		if (k >= 0) {
			uartBox[i]->setCurrentIndex(k);
		} else {
			uartBox[i]->addItem(cur, cur);		// a pipe, or a port unplugged since
			uartBox[i]->setCurrentIndex(uartBox[i]->count() - 1);
		}
	}
	cbEspLan->setChecked(conf.next.espLan);
	ui.tvHotkeys->load();
// machine
	fill_machine_list(ui.machbox);
	fill_machine_list(cbStart);
	// the first rows: the last one, Ask, the last session; then the machines (MS_THIS)
	cbStart->insertItem(0, tr("The last one chosen"), -MS_LAST - 1);
	cbStart->insertItem(1, tr("Ask"), -MS_ASK - 1);
	cbStart->insertItem(2, tr("Resume last session"), -MS_RESUME - 1);
	cbStart->setItemData(0, tr("The last one you picked. One a file switched to is not kept"), Qt::ToolTipRole);
	cbStart->setItemData(2, tr("Where you left off: the machine, the game\nand its media as they were on exit"), Qt::ToolTipRole);
	cbStart->insertSeparator(3);
	cbStart->setMaxVisibleItems(cbStart->count());
	if (conf.macStart == MS_THIS) {
		setRFIndex(cbStart, QString::fromLocal8Bit(conf.macStartId.c_str()), 1);	// gone: it asks
	} else {
		cbStart->setCurrentIndex((conf.macStart == MS_ASK) ? 1 : (conf.macStart == MS_RESUME) ? 2 : 0);
	}
	updateMachineButtons();
	roms = conf.roms;
	resTarget = comp->resbank;
	rsmodel->fill(&roms);
	fillRomSlots();
	ui.machbox->setCurrentIndex(ui.machbox->findData(QString::fromLocal8Bit(conf.macId.c_str())));
	setmszbox(ui.machbox->currentIndex());
	// the machine's own size: one a snapshot runs it on lasts the session only
	ui.mszbox->setCurrentIndex(ui.mszbox->findData(xm_own_ram()));
	if (ui.mszbox->currentIndex() < 0) ui.mszbox->setCurrentIndex(ui.mszbox->count() - 1);
	ui.sldSpeed->setMaximum(xspeed_max());	// a board already on turbo has less room
	ui.sldSpeed->setValue(xspeed_get());
	ui.sldSpeed->setEnabled(!comp->rzx.play);	// an rzx is tied to the frame it was taken at
	chaspd();
	ui.cbCpuFrq->setEditText(QString("%0 MHz").arg(comp->cpuFrq, 0, 'g', 6));
	// a list none of the rows has - an edited machine file - gets a row of its
	// own rather than being quietly turned into one of the three
	QString steps = QString::fromStdString(xm_turbo_str(comp));
	while (cbCpuTurbo->count() > CPU_TURBO_ROWS)
		cbCpuTurbo->removeItem(CPU_TURBO_ROWS);
	if (cbCpuTurbo->findData(steps) < 0)
		cbCpuTurbo->addItem(steps, steps);
	setRFIndex(cbCpuTurbo, steps);
	ui.scrpwait->setChecked(comp->flgEM1);
// emulation
	ui.cbLowLat->setChecked(conf.vid.lowLatency);
	setRFIndex(ui.cbRunAhead, conf.emu.runahead, 0);
	setRFIndex(ui.cbSlowmo, conf.emu.slowDiv, 1);
	setRFIndex(ui.cbFfwd, conf.emu.ffMul, 1);
	setRFIndex(ui.cbSlowmoKey, conf.emu.slowHold, XHOLD_HYBRID);
	setRFIndex(ui.cbFfwdKey, conf.emu.ffHold, XHOLD_HYBRID);
	setRFIndex(ui.cbFastKey, conf.emu.fastHold, XHOLD_HYBRID);
	ui.cbFastFps->setEditText(QString::number(conf.emu.fastFps));
	setRFIndex(ui.cbFastPic, conf.emu.fastPic, FPIC_RASTER);
	setRFIndex(cbFolderWrite, (int)conf.fold.rw);
	setRFIndex(cbFolderTrash, (int)conf.fold.trash);
	fillSaves();
	ui.cbRewind->setChecked(conf.emu.rewind.on);
	for (QWidget* w : {(QWidget*)ui.sbRewStep, (QWidget*)ui.labRewStep, (QWidget*)ui.sbRewSecs, (QWidget*)ui.labRewSecs})
		w->setEnabled(conf.emu.rewind.on);
	ui.sbRewStep->setValue(conf.emu.rewind.step);
	ui.sbRewSecs->setValue(conf.emu.rewind.secs);
	fillComboBox(ui.cbStyleSheet, "styles", QStringList() << "*.qss", "System", conf.style.c_str());
	for (int i = 1; i < ui.cbStyleSheet->count(); i++)
		ui.cbStyleSheet->setItemText(i, QFileInfo(ui.cbStyleSheet->itemText(i)).completeBaseName());
	// Input lag, Rewind, Interface and Indicators are grids of their own, and
	// columns line up between them only while all are given the same widths.
	// Measure them here, not in the .ui: a style or a font would outgrow a
	// number set there.
	int ctlw = qMax(comboFitWidth(ui.cbRunAhead), qMax(comboFitWidth(ui.cbSlowmo), comboFitWidth(ui.cbFfwd)));
	ctlw = qMax(ctlw, comboFitWidth(ui.cbFastFps));
	ctlw = qMax(ctlw, comboFitWidth(cbFolderWrite));
	for (QComboBox* box : {ui.cbRunAhead, ui.cbSlowmo, ui.cbFfwd, ui.cbFastFps, cbFolderWrite})
		box->setFixedWidth(ctlw);
	// the keys' column: the three key modes alike, the picture's mode and the deleted
	// files as wide as their own text, so neither widens the keys
	int keyw = comboFitWidth(ui.cbFastKey);
	for (QComboBox* box : {ui.cbSlowmoKey, ui.cbFfwdKey, ui.cbFastKey})
		box->setFixedWidth(keyw);
	for (QComboBox* box : {ui.cbFastPic, cbFolderTrash})
		box->setFixedWidth(comboFitWidth(box));
	ui.cbStyleSheet->setFixedWidth(comboFitWidth(ui.cbStyleSheet));
	QGridLayout* emugrid[6] = {ui.gridLayout_lat, ui.gridLayout_rew, gridPause, gridFolders, ui.gridLayout_look, ui.gridLayout_23};
	for (int col = 0; col < 2; col++) {
		int wid = 0;
		for (QGridLayout* g : emugrid)
			wid = qMax(wid, gridColWidth(g, col));
		for (QGridLayout* g : emugrid)
			g->setColumnMinimumWidth(col, wid);
	}
	ui.cbStyleSheet->setFixedWidth(ui.gridLayout_look->columnMinimumWidth(1));	// as wide as the names under it
	fitWidths();
	// Saves: the numbers alike, as wide as the widest of them asks; the columns
	// as Emulation's, so the names stand in one place on both pages
	int savw = 0;
	for (QSpinBox* sb : {sbQuickKeep, sbExitKeep, sbTimedMin, sbTimedKeep, sbCleanDays})
		savw = qMax(savw, sb->sizeHint().width());
	for (QSpinBox* sb : {sbQuickKeep, sbExitKeep, sbTimedMin, sbTimedKeep, sbCleanDays})
		sb->setFixedWidth(savw);
	for (QGridLayout* g : gridSaves) {
		g->setColumnMinimumWidth(0, qMax(savw, ui.gridLayout_lat->columnMinimumWidth(0)));
		g->setColumnMinimumWidth(1, ui.gridLayout_lat->columnMinimumWidth(1));
	}
// video
	ui.cbFullscreen->setChecked(conf.vid.fullScreen);
	fullscreenTip(ui.cbFullscreen);
	ui.cbKeepRatio->setChecked(conf.vid.keepRatio);
	fillToUi();
	fillTake();
	ui.cbVSync->setChecked(conf.vid.vsync);
	ui.cbFrameSkip->setChecked(conf.vid.frameSkip);
#if !defined(__WIN32) && !defined(__APPLE__)
	// the swaps are the tick here, and only a GL context made with them waiting has them
	ui.cbVSync->setEnabled(conf.vid.gl);
	ui.cbVSync->setToolTip(conf.vid.gl ? "No tearing, steady motion; takes a restart" : "Needs OpenGL");
#endif
	setRFIndex(ui.cbScale, conf.vid.scale, 1);	// x2 if the file says something odd
	ui.sldNoflic->setValue(noflic); chaflc();
	ui.cbNoflicMode->setCurrentIndex(noflicMode);
	ui.cbNoflicAhead->setChecked(noflicAhead);
	// the Next's frames cost too much to run more of them than the machine needs
	bool next = (conf.zx->hw->id == HW_ZXNEXT);
	ui.cbNoflicAhead->setEnabled(!next);
	ui.cbNoflicAhead->setToolTip(next ? tr("Not on the ZX Spectrum Next: its frames cost too much to run ahead")
		: tr("Adaptive modes: frames run ahead to catch a flicker as it starts"));
	ui.sbNoflicGamma->setValue(noflicGamma);
	ui.grayscale->setChecked(greyScale);
//	ui.cbScanlines->setChecked(scanlines);
	ui.border4T->setChecked(comp->vid->brdstep & 0x06);
	ui.contMem->setChecked(comp->flgCNTM);
	ui.contIO->setChecked(comp->flgCNTI);
	setRFIndex(ui.cbContPattern, comp->vid->ula->conttype);
	setRFIndex(ui.cbEarBack, comp->earback);
	setRFIndex(ui.cbFloatBus, comp->fbus);
	ui.cbEarlyTiming->setChecked(comp->vid->ula->early);
	ui.cbSnow->setChecked(xm_snow_user());
	ui.cbSnowCrash->setChecked(comp->flgSNOWX);
	ui.cbBeepMic->setChecked(comp->beep->mic);
	chasnow();
	ui.bszsld->setValue(conf.vid.border);
	// the setting is global and stays as it was; only this machine ignores it
	ui.bszsld->setEnabled(comp->vid->brdmin < VID_BRD_OVERSCAN);
	ui.bszsld->setToolTip(ui.bszsld->isEnabled() ? QString() : tr("This machine always shows its whole frame"));
	chabsz();
	ui.pathle->setText(QString::fromStdString(conf.scrShot.dir));
	ui.ssfbox->setCurrentIndex(ui.ssfbox->findData(QString::fromStdString(conf.scrShot.format)));
	ui.scntbox->setValue(conf.scrShot.count);
	ui.sintbox->setValue(conf.scrShot.interval);
	ui.ssNoBord->setChecked(conf.scrShot.noBorder);
	setRFIndex(cbShotSrc, conf.vid.gl ? conf.scrShot.source : VREC_SRC_PICTURE);
	setRFIndex(cbShotScale, conf.scrShot.scale);
	showShotSize();
	leShotName->setText(QString::fromStdString(conf.scrShot.name));
	fillRecording();
	ui.geombox->clear();
	foreach(xLayout lay, conf.layList) {
		ui.geombox->addItem(QString::fromLocal8Bit(lay.name.c_str()));
	}
	ui.geombox->setCurrentIndex(ui.geombox->findText(QString::fromLocal8Bit(conf.layName.c_str())));
	ui.ulaPlus->setChecked(comp->vid->ula->enabled);
	ui.cbDDp->setChecked(comp->flgDDP);
	fill_shader_list(ui.cbShader);
	palPending.clear();		// what a Cancel left behind
	fillPalettes();
// sound
	gsBox->setCurrentIndex(xm_gs_of(comp));
	gsrbox->setChecked(comp->gs->reset);
	sldGsSep->setValue(comp->gs->sep);
	ngsPick = QString::fromUtf8(conf.ngsFile.c_str());
	rom_box_pick(ngsRomBox, ngsPick);
	gsRomStack->setCurrentIndex((xm_gs_of(comp) == MAC_GS_NEOGS) ? 1 : 0);
	divBox->setCurrentIndex(comp->div->model ? 1 : 0);
	divRamBox->setCurrentIndex((comp->div->pages > 16) ? 1 : 0);
	divWrBox->setChecked(comp->div->eeWrite);
	fillDivFirmware();

	sdrvBox->setCurrentIndex(sdrvBox->findData(comp->sdrv->type));

	saaBox->setCurrentIndex(comp->saa->enabled ? 1 : 0);
	clockBox->setCurrentIndex(comp->flgGLUK ? MAC_CLOCK_GLUK : MAC_CLOCK_NONE);

	ui.senbox->setChecked(conf.snd.enabled);
	ui.dcbox->setChecked(conf.snd.vol.dc);
	ui.outbox->setCurrentIndex(ui.outbox->findText(QString::fromLocal8Bit(sndOutput->name)));
	// Auto keeps conf.snd.rate as the rate actually in use, so the box says
	// which one that turned out to be rather than leaving the user guessing.
	// It is also where a rate that is no longer offered falls back to.
	int autoIdx = ui.ratbox->findData(0);
	ui.ratbox->setItemText(autoIdx,
		conf.snd.rateauto ? QString("Auto (%1)").arg(conf.snd.rate) : QString("Auto"));
	setRFIndex(ui.ratbox, conf.snd.rateauto ? 0 : conf.snd.rate, autoIdx);
	ui.sldSndLatency->setRange(SND_LATENCY_MIN, SND_LATENCY_MAX);	// the block size sets the floor, keep the two together
	ui.sldSndLatency->setValue(conf.snd.latency);
	ui.chkSndLatAuto->setChecked(conf.snd.latauto);
	ui.chkSndFilter->setChecked(conf.snd.filter);
	chasndlat();

	ui.sbMasterVol->setValue(conf.snd.vol.master);
	ui.sbBeepVol->setValue(conf.snd.vol.beep);
	ui.sbTapeVol->setValue(conf.snd.vol.tape);
	ui.sbAYVol->setValue(conf.snd.vol.ay);
	ui.sbGSVol->setValue(conf.snd.vol.gs);
	ui.sbSdrvVol->setValue(conf.snd.vol.sdrv);
	ui.sbSAAVol->setValue(conf.snd.vol.saa);

	int psg = (comp->ts->type == TS_ZXNEXT) ? PSG_NEXT : (comp->ts->type == TS_NEDOPC) ? PSG_TS : PSG_ONE;
	if (comp->ts->chipA->type == SND_YM2203) psg = PSG_TSFM;
	if (comp->ts->chipA->type == SND_NONE) psg = PSG_NONE;
	setRFIndex(cbPsgCount, psg);
	setRFIndex(cbPsgType, (comp->ts->chipA->type == SND_NONE) ? SND_AY : comp->ts->chipA->type);
	setRFIndex(cbPsgStereo, comp->ts->chipA->stereo);
	sldPsgSep->setValue(comp->ts->chipA->sep);
	chapsg();				// the presets at this chip's clock first
	opt_set_psg_frq(cbPsgFrq, comp->ts->frq * psgFrqMul);	// 0 picks Auto
// input
	buildkeylist();
	setRFIndex(cbScanTab, comp->keyb->pcmode);
	setRFIndex(ui.typingBox, conf.kbdTyping ? 1 : 0);
	mouseBox->setCurrentIndex(comp->mouse->enable ? 1 : 0);
	nxMouseOn->setChecked(comp->mouse->enable);
	ratWheel->setChecked(comp->mouse->hasWheel);
	cbSwapButtons->setChecked(comp->mouse->swapButtons);
	sldSensitivity->setValue(comp->mouse->sensitivity * 1000.0f);
	joyBox->setCurrentIndex((comp->joy->type != XJ_KEMPSTON) ? MAC_JOY_NONE : comp->joy->extbuttons ? MAC_JOY_KEMPSTON8 : MAC_JOY_KEMPSTON);
	padSummary();
	cbPadFiles->setChecked(conf.gpctrl->gameOn);
// flp
	diskTypeBox->setCurrentIndex(diskTypeBox->findData(comp->dif->cfg));
	bdtbox->setChecked(fdcFlag & FDC_FAST);
	ui.mempaths->setChecked(conf.storePaths);
	setRFIndex(ui.cbAutorun, conf.autorun);
	ftbox->fill();
	int fitted = 0;
	while ((fitted < 4) && comp->dif->flp[fitted]->fitted) fitted++;
	setRFIndex(drvCountBox, qMax(1, fitted));
	cbAddBoot->setChecked(conf.boot);
	setRFIndex(cbFlpInterleave, flp_get_interleave());
	Floppy* flp = comp->dif->flp[0];
		a80box->setChecked(flp->trk80);
		adsbox->setChecked(flp->doubleSide);
	flp = comp->dif->flp[1];
		b80box->setChecked(flp->trk80);
		bdsbox->setChecked(flp->doubleSide);
	flp = comp->dif->flp[2];
		c80box->setChecked(flp->trk80);
		cdsbox->setChecked(flp->doubleSide);
	flp = comp->dif->flp[3];
		d80box->setChecked(flp->trk80);
		ddsbox->setChecked(flp->doubleSide);
// hdd
	hiface->setCurrentIndex(hiface->findData(comp->ide->cfg));

	hm_type->setCurrentIndex(hm_type->findData(comp->ide->master->type));

	hs_type->setCurrentIndex(hm_type->findData(comp->ide->slave->type));
// tape
	cbTapeAuto->setChecked(conf.tape.autostart);
	cbTapeFast->setChecked(conf.tape.fast);
	cbTapeFlash->setChecked(conf.tape.flash);
	cbTapeEdge->setChecked(conf.tape.edge);
	cbTapeFlash->setEnabled(conf.tape.fast);
	cbTapeEdge->setEnabled(conf.tape.fast);
	cbTapeRewind->setChecked(conf.tape.rewind);
	cbTapeSet->setChecked(conf.tape.setNext);
	cbTapeSet->setEnabled(conf.tape.rewind);
	sldTapeSpeed->setValue(comp->tape->speed);	// the readout follows in the slot
	showDevRows();
// tools
	ui.sbPort->setValue(conf.port);
	ui.cbDezog->setChecked(conf.dezog.on);
	ui.sbDezogPort->setValue(conf.dezog.port);
	ui.sbDezogPort->setEnabled(conf.dezog.on);
	ui.cbUpdCheck->setCurrentIndex(upd_period());
	ui.cbUpdChannel->setCurrentIndex(upd_channel());
	ui.cbConfexit->setChecked(conf.confexit);
	cbPauseInactive->setChecked(conf.pauseInactive);
	cbPauseMenu->setChecked(conf.pauseMenu);
	cbPauseResume->setChecked(conf.pauseResume);
// leds
	ui.cbMouseLed->setChecked(conf.led.mouse);
	ui.cbJoyLed->setChecked(conf.led.joy);
	ui.cbKeysLed->setChecked(conf.led.keys);
	ui.cbTapeLed->setChecked(conf.led.tape);
	ui.cbDiskLed->setChecked(conf.led.disk);
	ui.cbMessage->setChecked(conf.led.message);
	ui.cbSnowNote->setChecked(conf.led.snowNotice);
	ui.cbFadeLed->setChecked(conf.led.fade);
	ui.cbFpsLed->setChecked(conf.led.fps);
	ui.cbHaltLed->setChecked(conf.led.halt);
	ui.cbClockLed->setChecked(conf.led.clock);
// debuga
	ui.sbDbSize->setValue(conf.dbg.dbsize);
	ui.sbDwSize->setValue(conf.dbg.dwsize);
	ui.sbTextSize->setValue(conf.dbg.dmsize);
	ui.sbDbgStackOfs->setValue(conf.dbg.stackofs);
	ui.cbDbgNumbers->setCurrentIndex(ui.cbDbgNumbers->findData(conf.dbg.numfmt));
	ui.cbDbgNumTips->setChecked(conf.dbg.numtips);
	ui.cbDbgMemmap->setChecked(conf.dbg.showmmap);
	ui.cbDbgPorts->setChecked(conf.dbg.showports);
	ui.cbDbgSignals->setChecked(conf.dbg.showsig);
	ui.cbDbgFrame->setChecked(conf.dbg.showfrm);
	ui.cbDbgRay->setChecked(conf.dbg.showray);
	ui.cbImgLabels->setChecked(conf.dbg.imglabels);
	ui.cbRomLabels->setChecked(conf.dbg.romlabels);
	ui.cbSysVars->setChecked(conf.dbg.sysvars);
	portwid->setPorts(getWatchPorts(conf.zx));
	dbgfnt = conf.dbg.font;
	ui.leDbgFont->setText(QString("%0, %1 pt").arg(dbgfnt.family()).arg(dbgfnt.pointSize()));
	ui.leDbgFont->setFont(dbgfnt);
// palette
	fillDbgPalette();
	noteShown();
}

// what Apply sets only if it moves, as the page shows it now
void SetupWin::noteShown() {
	shown.cpu = xcpu_frq_parse(ui.cbCpuFrq->currentText(), conf.zx->cpuFrq);
	shown.psg = opt_get_psg_frq(cbPsgFrq) / psgFrqMul;
	shown.mem = getRFIData(ui.mszbox);
}

// widths that follow the font and style: on fill and after a style is applied
void SetupWin::fitWidths() {
	int wid = machColMin;
	for (xSideButton* btn : {ui.pbAdvMachine, ui.pbResetMachine, ui.pbSaveMachine, ui.pbDelMachine, btnRecAdv})
		wid = qMax(wid, btn->sizeHint().width());
	for (QWidget* col : {ui.machButtons, ui.picRight, ui.afRight, ui.ssRight, boxRecAdv})
		col->setFixedWidth(wid);
	// Screenshots' column is a layout of its own: rows as high and as far apart as
	// the grid's keep it level with the grid
	int rowH = 0;
	for (QWidget* w : {(QWidget*)ui.ssfbox, (QWidget*)ui.pathle, (QWidget*)ui.pathtb, (QWidget*)cbShotSrc, (QWidget*)ui.scntbox})
		rowH = qMax(rowH, w->sizeHint().height());
	ui.ssNoBord->setFixedHeight(rowH);
	labShotSize->setFixedHeight(rowH);
	int gap = ui.gridLayout_3->verticalSpacing();
	ui.ssColumn->setSpacing((gap < 0) ? ui.gridLayout_3->spacing() : gap);
	wid = appColMin;
	for (xSideButton* btn : {ui.pbFavorites, ui.pbCfgExport, ui.pbCfgImport, ui.pbCfgReset})
		wid = qMax(wid, btn->sizeHint().width());
	ui.appButtons->setFixedWidth(wid);
	wid = 0;
	for (int m = conf.zx->vid->brdmin; m <= VID_BRD_OVERSCAN; m++)
		wid = qMax(wid, ui.bszlab->fontMetrics().horizontalAdvance(borderText(m)));
	ui.bszlab->setMinimumWidth(wid);
}

void SetupWin::apply() {
	ui.tvHotkeys->commit();
	x_input_bump();			// the machine changed from outside: frames run ahead go
	// the rewind history holds the machine as it was: it goes only if the
	// machine or the history's own settings change
	std::string macWas = xm_signature();
	int rwStep = conf.emu.rewind.step;
	int rwSecs = conf.emu.rewind.secs;
	// R/O stops the folders in use from changing at once; R/W is taken at the next read
	int foldRw = getRFIData(cbFolderWrite) ? 1 : 0;
	if (conf.fold.rw && !foldRw) {
		emu_lock();
		fold_read_only(conf.zx, "made read-only in Options");
		emu_unlock();
	}
	conf.fold.rw = foldRw;
	conf.fold.trash = getRFIData(cbFolderTrash) ? 1 : 0;
	applySaves();
	int row = cbStart->currentIndex();
	int ms = (row == 0) ? MS_LAST : (row == 1) ? MS_ASK : (row == 2) ? MS_RESUME : MS_THIS;
	// unset shows as Last, and stays unset - the first start still asks - until changed
	if ((conf.macStart != MS_UNSET) || (ms != MS_LAST)) conf.macStart = ms;
	if (ms == MS_THIS) conf.macStartId = getRFSData(cbStart).toLocal8Bit().data();
	// another machine is not this page with different values in it: it has its
	// own, so load it and show them rather than writing these over it. What the
	// pages hold for the emulator itself still goes in, onto the new machine.
	std::string mid = std::string(getRFSData(ui.machbox).toLocal8Bit().data());
	bool another = !mid.empty() && (mid != conf.macId);
	// the NeoGS flash is every machine's, so it goes in before a switch builds the
	// new machine; a NeoGS reads it when it is the card and the file is new to it
	conf.ngsFile = std::string(ngsPick.toUtf8().data());
	if (another) {
		xm_choose(mid);
	} else {
		applyMachine();
	}
	applyGlobal();
	if (another) {
		// the window as it was, or as big as the new machine's page needs
		QSize was = size();
		fillPages();		// the new machine's own values on the pages
		QTimer::singleShot(0, this, [this, was]() {resize(was.expandedTo(minimumSizeHint()));});
		emit s_prf_changed();
	} else {
		// the machine carries what the page put in it: into its own file, so it is
		// still there after a switch away and back
		if (!inBench) xm_save_over();
		if ((xm_signature() != macWas) || (conf.emu.rewind.step != rwStep) || (conf.emu.rewind.secs != rwSecs))
			rewind_clear();
		// The machine changed under a recording: it ends with the frames before
		// the change, which is all the log holds of them
		if (xm_signature() != macWas) {
			emu_lock();
			rzxr_stop(conf.zx);
			emu_unlock();
		}
		updateMachineButtons();
		// the mark on the machine may have just appeared or gone
		int midx = ui.machbox->findData(QString::fromLocal8Bit(conf.macId.c_str()));
		const xMachine* cmac = xm_find(conf.macId);
		if (cmac && (midx >= 0)) ui.machbox->setItemText(midx, xm_list_name(*cmac));
	}

	fullscreenTip(ui.cbFullscreen);		// the Hotkeys page may have moved it
	emit s_apply();
	fitWidths();		// a style just applied pads the buttons its own way

	if (!inBench) {
		layouts_save();
		saveConfig();
	}
}

// What the pages hold for this machine. What it set itself stays: the clocks, the
// layout and the sound chips are set only when the page has them otherwise.
void SetupWin::applyMachine() {
	Computer* comp = conf.zx;
	// the Next sets these itself: what the hidden rows show is not put back on it
	bool own = (comp->hw->id == HW_ZXNEXT);
// machine
	emu_lock();		// roms and memory size are rebuilt below
	xm_set_roms(roms);
	comp->resbank = resTarget;
	int msz = getRFIData(ui.mszbox);
	double frq = own ? comp->cpuFrq : xcpu_frq_parse(ui.cbCpuFrq->currentText(), comp->cpuFrq);
	if (msz != shown.mem) xm_set_ram(msz);
	if ((msz != shown.mem) || (frq != shown.cpu)) compSetBaseFrq(comp, frq);
	if (!own) xm_turbo_set(comp, getRFSData(cbCpuTurbo).toStdString());
	comp->flgGLUK = hw_takes_gluk(comp->hw) && (clockBox->currentIndex() == MAC_CLOCK_GLUK);
	if (ui.sldSpeed->value() != xspeed_get())
		xspeed_set(ui.sldSpeed->value());
	if (!own) comp->flgEM1 = ui.scrpwait->isChecked();
	if (comp->hw->id == HW_ZX48) comp->mem->ramMask = MEM_128K - 1;		// TODO: find a better way
	// NextZXOS finds the memory at power-on, by a test of its own: off and on again
	if ((msz != shown.mem) && own) xm_power_cycle();
	emu_unlock();
// video
	if (!own) {
		comp->vid->brdstep = ui.border4T->isChecked() ? 7 : 1;
		comp_set_cont(comp, ui.contMem->isChecked());
		comp->flgCNTI = ui.contIO->isChecked() ? 1 : 0;
		comp_set_cont_pattern(comp, getRFIData(ui.cbContPattern));
		comp->earback = getRFIData(ui.cbEarBack);
		comp->fbus = getRFIData(ui.cbFloatBus);
		comp->vid->ula->early = ui.cbEarlyTiming->isChecked();
		xm_snow_set(comp, ui.cbSnow->isChecked() ? 1 : 0);
		comp->flgSNOWX = ui.cbSnowCrash->isChecked() ? 1 : 0;
		// the MIC level in the beeper or out of it, at once; left alone, the levels stay
		// as the machine set them
		int mic = ui.cbBeepMic->isChecked() ? 1 : 0;
		if (comp->beep->mic != mic) {
			comp->beep->mic = mic;
			bc_out(comp->beep, comp->beep->lev, comp->tape->levRec);
		}
		// The ula type also picks the screen drawer. The reset above ran before this
		// line and saw the old type, so it has to be redone here - but only while a
		// plain zx screen is up: a machine sitting in one of its own modes keeps it
		// and gets the drawer at its next reset.
		if ((comp->vid->vmode == VID_NORMAL) || (comp->vid->vmode == VID_ULA_SCR))
			zx_set_vmode(comp);
		comp->vid->ula->enabled = ui.ulaPlus->isChecked() ? 1 : 0;
		comp->flgDDP = ui.cbDDp->isChecked() ? 1 : 0;
		// another layout than the machine has, or this one edited - in the editor at
		// any time, Cancel or not (the border size goes on top of it in applyGlobal())
		std::string lay = getRFText(ui.geombox);
		xLayout* nlay = findLayout(lay);
		if (nlay && ((lay != conf.layName) || memcmp(&nlay->lay, &conf.layGeom, sizeof(vLayout))))
			xm_set_layout(lay);
	}
// sound
	if (!own) {
		int psg = getRFIData(cbPsgCount);
		int chips = (psg == PSG_NEXT) ? 3 : ((psg == PSG_TS) || (psg == PSG_TSFM)) ? 2 : (psg == PSG_ONE) ? 1 : 0;
		int chtype = (psg == PSG_TSFM) ? SND_YM2203 : getRFIData(cbPsgType);
		if ((psg != PSG_TSFM) && (chtype == SND_YM2203)) chtype = SND_YM;
		int chstereo = getRFIData(cbPsgStereo);
		double psgFrq = opt_get_psg_frq(cbPsgFrq) / psgFrqMul;	// 0: Auto
		int tsType = (chips > 2) ? TS_ZXNEXT : (chips > 1) ? TS_NEDOPC : TS_NONE;
		bool chipsNew = (comp->ts->type != tsType);
		bool stereoNew = false;		// the chips' tick goes on: only the sound thread's copy is told
		aymChip* chip[3] = {comp->ts->chipA, comp->ts->chipB, comp->ts->chipC};
		for (int i = 0; i < 3; i++) {			// one setting for all the chips
			int type = (i < chips) ? chtype : SND_NONE;
			if (chip[i]->type != type) {
				chip_set_type(chip[i], type);
				chipsNew = true;
			}
			if (chip[i]->stereo != chstereo) {
				chip[i]->stereo = chstereo;
				stereoNew = true;
			}
		}
		// a chip of another type takes its clock here too (a YM2203 runs at twice the AY's)
		if (chipsNew || (psgFrq != shown.psg)) {
			ts_set_frq(comp->ts, psgFrq, comp->cpuFrq);
			comp->ts->type = tsType;
			comp->sndSeed = 1;		// the chips the sound thread mixes take all of it
		} else if (stereoNew) {
			comp->sndSeed = 1;
		}
		xm_set_gs(comp, gsBox->currentIndex());
		comp->sdrv->type = getRFIData(sdrvBox);
		comp->saa->enabled = saaBox->currentIndex() ? 1 : 0;
	}
// input
	comp->keyb->pcmode = getRFIData(cbScanTab);
	comp->mouse->enable = own ? (nxMouseOn->isChecked() ? 1 : 0) : mouseBox->currentIndex();
	if (!own) {
		comp->mouse->hasWheel = ratWheel->isChecked() ? 1 : 0;
		comp->joy->type = (joyBox->currentIndex() != MAC_JOY_NONE) ? XJ_KEMPSTON : XJ_NONE;
		comp->joy->extbuttons = ((joyBox->currentIndex() == MAC_JOY_KEMPSTON8) && hw_kempston8(comp->hw)) ? 1 : 0;
	}
// flp
	if (!own) {
		difSetHW(comp->dif, getRFIData(diskTypeBox));
		// a drive taken off the cable takes its disk with it: a changed one is
		// saved first, or the drive stays
		int fit = getRFIData(drvCountBox);
		if (getRFIData(diskTypeBox) == DIF_P3DOS) fit = qMin(fit, 2);
		for (int i = 3; i >= fit; i--) {
			Floppy* dflp = comp->dif->flp[i];
			if (dflp->fitted && dflp->insert && dflp->changed && (saveChangedDisk(comp, i) != ERR_OK))
				fit = i + 1;
		}
		difSetDrives(comp->dif, fit);
	}
// hdd
	if (!own) ide_set_type(comp->ide, getRFIData(hiface));
	// what is mounted is the Drives menu's, a folder read again only with the power off
// divmmc: a board put on, off or given other RAM powers the machine off and on
	int divModel = (divBox->currentIndex() && hw_takes_divmmc(comp->hw)) ? DIV_MMC : DIV_NONE;
	int divPages = divRamBox->currentIndex() ? DIV_PAGES_MAX : 16;
	bool divPower = (divModel != comp->div->model) || (divModel && (divPages != comp->div->pages));
	if (divPower)
		div_set_model(comp->div, divModel, divPages);
	comp_board_storage(comp);
	comp->div->eeWrite = divWrBox->isChecked() ? 1 : 0;
	div_map(comp->div);
	// a chip written outside the machine goes back in with the power off too
	if (applyDivFirmware() && divModel) divPower = true;
	fillDivFirmware();
// debuga
	setWatchPorts(comp, portwid->getPorts());
	if (divPower) {
		emu_lock();
		xm_power_cycle();
		emu_unlock();
	}
	fillNextGroup();	// a power cycle above set the nextregs back
	noteShown();		// for the next Apply with the window still open
}

// The emulator's own settings, and the user's that live on the machine (mouse,
// PSG separation, GS reset, drives' 80/DS, HDD types, tape speed): these go
// onto whichever machine is in now.
void SetupWin::applyGlobal() {
	Computer* comp = conf.zx;
	for (int i = 0; i < 2; i++) {
		QString txt = uartBox[i]->currentText().trimmed();
		int k = uartBox[i]->findText(txt);
		QString port = (k >= 0) ? uartBox[i]->itemData(k).toString() : txt;
		if (port != serial_name(i)) serial_set(i, port, true);		// else a --uart-* stays
	}
	conf.next.espLan = cbEspLan->isChecked() ? 1 : 0;
// emulation
	conf.vid.lowLatency = ui.cbLowLat->isChecked() ? 1 : 0;
	conf.emu.runahead = getRFIData(ui.cbRunAhead);
	conf.emu.slowDiv = getRFIData(ui.cbSlowmo);
	conf.emu.ffMul = getRFIData(ui.cbFfwd);
	conf.emu.slowHold = getRFIData(ui.cbSlowmoKey);
	conf.emu.ffHold = getRFIData(ui.cbFfwdKey);
	conf.emu.fastHold = getRFIData(ui.cbFastKey);
	conf.emu.fastFps = fast_fps_valid(ui.cbFastFps->currentText().toInt());
	conf.emu.fastPic = getRFIData(ui.cbFastPic);
	conf.emu.rewind.on = ui.cbRewind->isChecked() ? 1 : 0;
	conf.emu.rewind.step = ui.sbRewStep->value();
	conf.emu.rewind.secs = ui.sbRewSecs->value();
// video
	conf.vid.fullScreen = ui.cbFullscreen->isChecked() ? 1 : 0;
	conf.vid.keepRatio = ui.cbKeepRatio->isChecked() ? 1 : 0;
	fillSet(getRFIData(ui.cbSideFill));
	fillTake();		// the pop-up's values are in conf already: now they stay
	conf.vid.vsync = ui.cbVSync->isChecked() ? 1 : 0;
	conf.vid.frameSkip = ui.cbFrameSkip->isChecked() ? 1 : 0;
	conf.vid.scale = getRFIData(ui.cbScale);
	noflic = ui.sldNoflic->value();
	noflicMode = ui.cbNoflicMode->currentIndex();
	noflicAhead = ui.cbNoflicAhead->isChecked() ? 1 : 0;
	noflicGamma = ui.sbNoflicGamma->value();
	vid_set_grey(ui.grayscale->isChecked() ? 1 : 0);
//	scanlines = ui.cbScanlines->isChecked() ? 1 : 0;
	// a screen dump breakpoint names its file from the emulation thread
	emu_lock();
	conf.scrShot.dir = ui.pathle->text().toStdString();		// UTF-8, as shot_dir() reads it
	conf.scrShot.format = getRFSData(ui.ssfbox).toStdString();
	conf.scrShot.count = ui.scntbox->value();
	conf.scrShot.interval = ui.sintbox->value();
	conf.scrShot.noBorder = ui.ssNoBord->isChecked() ? 1 : 0;
	// greyed with no GL view, Screen is kept for a run that has one
	if (conf.vid.gl) conf.scrShot.source = getRFIData(cbShotSrc);
	conf.scrShot.scale = getRFIData(cbShotScale);
	conf.scrShot.name = leShotName->text().trimmed().toStdString();
	emu_unlock();
	applyRecording();
	vid_set_border_mode(ui.bszsld->value());
	// with no shaders the list holds None only: the chosen one stays for a run that has them
	if (!conf.vid.shd_support) {
	} else if (getRFIData(ui.cbShader) == 0) {
		conf.vid.shader.clear();
	} else {
		conf.vid.shader = std::string(ui.cbShader->currentText().toLocal8Bit().data());
	}
	std::string pal = getRFSData(ui.cbPalPreset).toStdString();		// empty: the built-in one
	bool palNew = (pal != conf.palette) || !palPending.isEmpty();
	conf.palette = pal;
	// the edited palettes go into the user's folder now, over a shipped one of the same name
	for (auto it = palPending.cbegin(); it != palPending.cend(); ++it) {
		if (saveColors(it.key().toStdString(), it.value()) != ERR_OK)
			shitHappens("Could not write the palette file");
	}
	palPending.clear();
	if (palNew) loadPalette();
	palButtons();
// sound
	conf.snd.enabled = ui.senbox->isChecked() ? 1 : 0;
	conf.snd.vol.dc = ui.dcbox->isChecked() ? 1 : 0;

	conf.snd.vol.master = ui.sbMasterVol->value();
	conf.snd.vol.beep = ui.sbBeepVol->value();
	conf.snd.vol.tape = ui.sbTapeVol->value();
	conf.snd.vol.ay = ui.sbAYVol->value();
	conf.snd.vol.gs = ui.sbGSVol->value();
	conf.snd.vol.sdrv = ui.sbSdrvVol->value();
	conf.snd.vol.saa = ui.sbSAAVol->value();

	std::string nname = getRFText(ui.outbox);
	// 0 is the Auto item. The rate it finds goes into conf.snd.rate like any
	// other, so everything downstream keeps reading one setting.
	int rate = getRFIData(ui.ratbox);
	int rateauto = (rate == 0) ? 1 : 0;
	if (rateauto) rate = conf.snd.rate;		// re-read from the device on open
	// the auto mode writes its findings back into the same setting, so the
	// slider shows what the emulator settled on and is still the way to nudge
	// it by hand
	conf.snd.latauto = ui.chkSndLatAuto->isChecked() ? 1 : 0;
	conf.snd.filter = ui.chkSndFilter->isChecked() ? 1 : 0;
	int latency = ui.sldSndLatency->value();
	// reopen on a changed latency too: the pacer would creep to the new target
	// over tens of seconds, refilling the ring gets there at once
	if (!inBench && ((rate != conf.snd.rate) || (rateauto != conf.snd.rateauto) ||
		(latency != conf.snd.latency) || (nname != sndGetName()))) {
		conf.snd.rate = rate;
		conf.snd.rateauto = rateauto;
		conf.snd.latency = latency;
		setOutput(nname.c_str());
	}

	int chsep = sldPsgSep->value();
	aymChip* chip[3] = {comp->ts->chipA, comp->ts->chipB, comp->ts->chipC};
	for (int i = 0; i < 3; i++) {
		if (chip[i]->sep == chsep) continue;
		chip[i]->sep = chsep;
		comp->sndSeed = 1;
	}
	comp->gs->reset = gsrbox->isChecked() ? 1 : 0;
	comp->gs->sep = sldGsSep->value();
// input
	comp->mouse->swapButtons = cbSwapButtons->isChecked() ? 1 : 0;
	comp->mouse->sensitivity = sldSensitivity->value() * 0.001f;
	conf.kmapName = ui.keyMapBox->currentData().toString().toUtf8().constData();	// none: the built-in one
	conf.kbdTyping = getRFIData(ui.typingBox) ? 1 : 0;
	loadKeys();
	conf.gpctrl->gameOn = cbPadFiles->isChecked();
	if (!conf.gpctrl->gameOn) conf.gpctrl->gameRevert();	// own bindings back now
// flp
	setFlagBit(bdtbox->isChecked(),&fdcFlag,FDC_FAST);
	conf.boot = cbAddBoot->isChecked() ? 1 : 0;
	conf.storePaths = ui.mempaths->isChecked() ? 1 : 0;
	conf.autorun = getRFIData(ui.cbAutorun);
	ftbox->apply();
	flp_set_interleave(getRFIData(cbFlpInterleave));

	Floppy* flp = comp->dif->flp[0];
	flp->trk80 = a80box->isChecked() ? 1 : 0;
	flp->doubleSide = adsbox->isChecked() ? 1 : 0;

	flp = comp->dif->flp[1];
	flp->trk80 = b80box->isChecked() ? 1 : 0;
	flp->doubleSide = bdsbox->isChecked() ? 1 : 0;

	flp = comp->dif->flp[2];
	flp->trk80 = c80box->isChecked() ? 1 : 0;
	flp->doubleSide = cdsbox->isChecked() ? 1 : 0;

	flp = comp->dif->flp[3];
	flp->trk80 = d80box->isChecked() ? 1 : 0;
	flp->doubleSide = ddsbox->isChecked() ? 1 : 0;

// hdd
	comp->ide->master->type = getRFIData(hm_type);
	comp->ide->slave->type = getRFIData(hs_type);
// tape
	conf.tape.autostart = cbTapeAuto->isChecked() ? 1 : 0;
	conf.tape.fast = cbTapeFast->isChecked() ? 1 : 0;
	conf.tape.flash = cbTapeFlash->isChecked() ? 1 : 0;
	conf.tape.edge = cbTapeEdge->isChecked() ? 1 : 0;
	conf.tape.rewind = cbTapeRewind->isChecked() ? 1 : 0;
	conf.tape.setNext = cbTapeSet->isChecked() ? 1 : 0;
	tape_set_speed(comp->tape, sldTapeSpeed->value());
	tape_apply_options(comp->tape);
// tools
	conf.port = ui.sbPort->value() & 0xffff;
	conf.dezog.on = ui.cbDezog->isChecked() ? 1 : 0;
	conf.dezog.port = ui.sbDezogPort->value() & 0xffff;
	// a build's default stays unset until changed, so a later build may change it
	if (ui.cbUpdCheck->currentIndex() != upd_period()) conf.upd.check = ui.cbUpdCheck->currentIndex();
	if (ui.cbUpdChannel->currentIndex() != upd_channel()) conf.upd.channel = ui.cbUpdChannel->currentIndex();
	conf.confexit = ui.cbConfexit->isChecked() ? 1 : 0;
	conf.pauseInactive = cbPauseInactive->isChecked() ? 1 : 0;
	conf.pauseMenu = cbPauseMenu->isChecked() ? 1 : 0;
	conf.pauseResume = cbPauseResume->isChecked() ? 1 : 0;
// leds
	conf.led.mouse = ui.cbMouseLed->isChecked() ? 1 : 0;
	conf.led.joy = ui.cbJoyLed->isChecked() ? 1 : 0;
	conf.led.keys = ui.cbKeysLed->isChecked() ? 1 : 0;
	conf.led.tape = ui.cbTapeLed->isChecked() ? 1 : 0;
	conf.led.disk = ui.cbDiskLed->isChecked() ? 1 : 0;
	conf.led.message = ui.cbMessage->isChecked() ? 1 : 0;
	conf.led.snowNotice = ui.cbSnowNote->isChecked() ? 1 : 0;
	conf.led.fade = ui.cbFadeLed->isChecked() ? 1 : 0;
	conf.led.fps = ui.cbFpsLed->isChecked() ? 1 : 0;
	conf.led.halt = ui.cbHaltLed->isChecked() ? 1 : 0;
	conf.led.clock = ui.cbClockLed->isChecked() ? 1 : 0;
// debuga
	conf.dbg.dbsize = ui.sbDbSize->value();
	conf.dbg.dwsize = ui.sbDwSize->value();
	conf.dbg.dmsize = ui.sbTextSize->value();
	// the step is 2, but a typed-in value can still land odd
	conf.dbg.stackofs = ui.sbDbgStackOfs->value() & ~1;
	dbg_set_numfmt(ui.cbDbgNumbers->currentData().toInt());
	conf.dbg.numtips = ui.cbDbgNumTips->isChecked() ? 1 : 0;
	conf.dbg.font = dbgfnt;
	conf.dbg.showports = ui.cbDbgPorts->isChecked() ? 1 : 0;
	conf.dbg.showsig = ui.cbDbgSignals->isChecked() ? 1 : 0;
	conf.dbg.showfrm = ui.cbDbgFrame->isChecked() ? 1 : 0;
	conf.dbg.showray = ui.cbDbgRay->isChecked() ? 1 : 0;
	conf.dbg.imglabels = ui.cbImgLabels->isChecked() ? 1 : 0;
	if (!conf.dbg.imglabels) labels_image(QString());	// the set from before is back now
	if (conf.dbg.romlabels != (ui.cbRomLabels->isChecked() ? 1 : 0)) {
		conf.dbg.romlabels = ui.cbRomLabels->isChecked() ? 1 : 0;
		labels_touch();
	}
	if (conf.dbg.sysvars != (ui.cbSysVars->isChecked() ? 1 : 0)) {
		conf.dbg.sysvars = ui.cbSysVars->isChecked() ? 1 : 0;
		labels_touch();
	}
	QString name = getRFSData(ui.cbStyleSheet);
	std::string style = name.isEmpty() ? std::string() : name.toStdString();
	if (style != conf.style) {
		conf.style = style;
		// the debugger colours follow the style: the built-in ones first, then
		// whatever the new style ships next to it - "System" ends up with the
		// defaults, and a .pal that names only a few colours leaves no leftovers
		// from the style before. Only on a change: anything edited by hand
		// afterwards is the user's and stays
		dbgPaletteDefaults();
		loadStylePalette(conf.style);
		fillDbgPalette();
		ui.leDbgFont->setFont(dbgfnt);		// the new style sheet resets it
	}
	applyLogPage();
}

// LOG

// The page sets the log going and picks one level for the lot. Per-group levels
// are a bug-hunting tool and stay where the bug hunter already is: --log-groups
// and the groups line in config.conf.
void SetupWin::fillLogPage() {
	ui.cbLogEnable->setChecked(conf.log.enabled);
	ui.cbLogConsole->setChecked(conf.log.console);
	setRFIndex(ui.cbxLogLevel, conf.log.level);
	ui.leLogDir->setText(QString::fromStdString(conf.log.dir));
	ui.leLogFile->setText(log_file());
}

void SetupWin::applyLogPage() {
	// the page has spoken, so whatever the command line asked for stops
	// standing on top of it - and a group set apart stays where it was put
	log_args_clear();
	conf.log.enabled = ui.cbLogEnable->isChecked() ? 1 : 0;
	conf.log.console = ui.cbLogConsole->isChecked() ? 1 : 0;
	conf.log.level = getRFIData(ui.cbxLogLevel);
	conf.log.dir = ui.leLogDir->text().toStdString();
	log_apply();
	ui.leLogFile->setText(log_file());
}

void SetupWin::selLogDir() {
	QString dir = QFileDialog::getExistingDirectory(this, "Where logs/ goes", ui.leLogDir->text());
	if (!dir.isEmpty()) ui.leLogDir->setText(dir);
}

void SetupWin::openLogDir() {
	QString dir = log_dir();
	if (dir.isEmpty()) return;
	QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void SetupWin::reject() {
	fillPut();		// shown while it was set, and not applied
	hide();
	emit closed();
}

// LAYOUTS

void SetupWin::layNameCheck(QString nam) {
	layUi.okButton->setEnabled(!layUi.layName->text().isEmpty());
/*
	for (int i = 0; i < conf.layList.size(); i++) {
		if ((QString(conf.layList[i].name.c_str()) == nam) && (eidx != i)) {
			layUi.okButton->setEnabled(false);
		}
	}
*/
}

void SetupWin::editLayout() {
	layUi.lineBox->setValue(nlay.lay.full.x);
	layUi.rowsBox->setValue(nlay.lay.full.y);
	layUi.hsyncBox->setValue(nlay.lay.blank.x);
	layUi.vsyncBox->setValue(nlay.lay.blank.y);
	layUi.brdLBox->setValue(nlay.lay.bord.x);
	layUi.brdUBox->setValue(nlay.lay.bord.y);
	layUi.intRowBox->setValue(nlay.lay.intpos.y);
	layUi.intPosBox->setValue(nlay.lay.intpos.x);
	layUi.intLenBox->setValue(nlay.lay.intSize);
	layUi.sbScrW->setValue(nlay.lay.scr.x);
	layUi.sbScrH->setValue(nlay.lay.scr.y);
	layUi.okButton->setDisabled(eidx == 0);
	layUi.layWidget->setDisabled(eidx == 0);
	layUi.layName->setText(nlay.name.c_str());
	layeditor->show();
	layeditor->setFixedSize(layeditor->minimumSize());
}

void SetupWin::edLayout() {
	eidx = ui.geombox->currentIndex();
	nlay = conf.layList[eidx];
	editLayout();
}

void SetupWin::delLayout() {
	int eidx = ui.geombox->currentIndex();
	if (eidx < 0) return;
	const xLayout* shp = layout_shipped(conf.layList[eidx].name);
	if (shp) {
		if (!areSure("Put this layout back the way it ships?")) return;
		conf.layList[eidx] = *shp;
		return;
	}
	if (areSure("Do you really want to delete this layout?")) {
		conf.layList.erase(conf.layList.begin() + eidx);
		ui.geombox->removeItem(eidx);
	}
}

void SetupWin::addNewLayout() {
	eidx = -1;
	nlay = conf.layList[0];
	nlay.name = "";
	editLayout();
}

void SetupWin::layEditorChanged() {
	layUi.showField->setFixedSize(layUi.lineBox->value(),layUi.rowsBox->value());
	QPixmap pix(layUi.lineBox->value(),layUi.rowsBox->value());
	QPainter pnt;
	pnt.begin(&pix);
	pnt.fillRect(0,0,pix.width(),pix.height(),Qt::black);
	// visible screen = full - blank
	pnt.fillRect(layUi.hsyncBox->value(),layUi.vsyncBox->value(),
			layUi.lineBox->value() - layUi.hsyncBox->value(),
			layUi.rowsBox->value() - layUi.vsyncBox->value(),
			Qt::blue);
	// main screen area
	pnt.fillRect(layUi.brdLBox->value()+layUi.hsyncBox->value(), layUi.brdUBox->value()+layUi.vsyncBox->value(),
			layUi.sbScrW->value(),layUi.sbScrH->value(),
			Qt::gray);
	// INT signal
	pnt.setPen(Qt::red);
	pnt.drawLine(layUi.intPosBox->value(),
			layUi.intRowBox->value(),
			layUi.intPosBox->value() + layUi.intLenBox->value(),
			layUi.intRowBox->value());
	pnt.end();
	layUi.showField->setPixmap(pix);
	layeditor->setFixedSize(layeditor->minimumSize());
}

void SetupWin::layEditorOK() {
	QString nm = layUi.layName->text();
	std::string name = std::string(nm.toLocal8Bit().data());
	xLayout* exlay = findLayout(name);
	vLayout vlay;
	int ok = 1;
	vlay.full.x = layUi.lineBox->value();
	vlay.full.y = layUi.rowsBox->value();
	vlay.bord.x = layUi.brdLBox->value();
	vlay.bord.y = layUi.brdUBox->value();
	vlay.blank.x = layUi.hsyncBox->value();
	vlay.blank.y = layUi.vsyncBox->value();
	vlay.intpos.x = layUi.intPosBox->value();
	vlay.intpos.y = layUi.intRowBox->value();
	vlay.intSize = layUi.intLenBox->value();
	vlay.scr.x = layUi.sbScrW->value();
	vlay.scr.y = layUi.sbScrH->value();
	if (eidx < 0) {						// new layout
		if (exlay == NULL) {				// new name
			addLayout(name, vlay);
			fill_layout_list(ui.geombox, nm);
		} else {					// existing name
			ok = areSure("Replace existing layout?");
			if (ok) exlay->lay = vlay;
			fill_layout_list(ui.geombox, nm);
		}
	} else {
		std::string onm = conf.layList[eidx].name;
		if (onm != name) {				// name changed
			if (exlay == NULL) {			// no existing layout with new name
				conf.layList[eidx].name = name;
				conf.layList[eidx].lay = vlay;
				if (conf.layName == onm) conf.layName = name;
				fill_layout_list(ui.geombox, nm);
			} else {
				ok = areSure("Replace existing layout?");
				if (ok) {
					if (conf.layName == onm) conf.layName = name;
					exlay->lay = vlay;		// replace new-name layout
					rmLayout(onm);
					fill_layout_list(ui.geombox, nm);
				}
			}
		} else {					// name doesn't changed, replace old layout
			conf.layList[eidx].lay = vlay;
		}
	}
	if (ok) layeditor->hide();
}

// A box taken out of the page, in a window of its own with OK and Cancel, which
// wear the icons the main dialog's do. OK keeps what the box shows for the main
// dialog's Apply; Cancel (Esc, the close box) puts back what it showed when it
// opened. The window only edits the page, so either way nothing reaches the
// machine before Apply.
//
// Modal, so the page it came from cannot be closed out from under it.

// what a sheet's fields hold, to put back on Cancel
struct xSheetState {
	QList<QPair<QPointer<QWidget>, QVariant> > vals;
	xRomset roms;		// the ROM pickers write the page's set as they go
	int resTarget;
};

class xSheetWin : public QDialog {
	public:
		xSheetWin(QWidget* par) : QDialog(par) {}
		std::function<void()> onOpen;
		std::function<void()> onCancel;
	protected:
		void showEvent(QShowEvent* ev) override {
			if (!ev->spontaneous() && onOpen) onOpen();
			QDialog::showEvent(ev);
		}
	public:
		void reject() override {
			if (isVisible() && onCancel) onCancel();
			QDialog::reject();
		}
};

static QVariant sheet_value(QWidget* w) {
	if (QComboBox* cb = qobject_cast<QComboBox*>(w)) {
		QVariantList v;
		v << cb->currentIndex() << cb->currentData() << cb->currentText();
		return v;
	}
	if (QAbstractButton* ab = qobject_cast<QAbstractButton*>(w))
		return ab->isCheckable() ? QVariant(ab->isChecked()) : QVariant();
	if (QAbstractSlider* sl = qobject_cast<QAbstractSlider*>(w)) return sl->value();
	if (QSpinBox* sb = qobject_cast<QSpinBox*>(w)) return sb->value();
	if (QDoubleSpinBox* ds = qobject_cast<QDoubleSpinBox*>(w)) return ds->value();
	// an editable combo's own line is the combo's
	if (QLineEdit* le = qobject_cast<QLineEdit*>(w))
		return qobject_cast<QComboBox*>(le->parentWidget()) ? QVariant() : QVariant(le->text());
	return QVariant();
}

static void sheet_put(QWidget* w, const QVariant& val) {
	if (QComboBox* cb = qobject_cast<QComboBox*>(w)) {
		QVariantList v = val.toList();
		int idx = v[0].toInt();
		// the list may have been filled again: the item goes by its data then
		if ((idx >= cb->count()) || (cb->itemData(idx) != v[1]) || (cb->itemText(idx) != v[2])) {
			int alt = v[1].isValid() ? cb->findData(v[1]) : cb->findText(v[2].toString());
			if (alt >= 0) idx = alt;
		}
		if (idx < cb->count()) cb->setCurrentIndex(idx);
		if (cb->isEditable()) cb->setEditText(v[2].toString());
	} else if (QAbstractButton* ab = qobject_cast<QAbstractButton*>(w)) {
		ab->setChecked(val.toBool());
	} else if (QAbstractSlider* sl = qobject_cast<QAbstractSlider*>(w)) {
		sl->setValue(val.toInt());
	} else if (QSpinBox* sb = qobject_cast<QSpinBox*>(w)) {
		sb->setValue(val.toInt());
	} else if (QDoubleSpinBox* ds = qobject_cast<QDoubleSpinBox*>(w)) {
		ds->setValue(val.toDouble());
	} else if (QLineEdit* le = qobject_cast<QLineEdit*>(w)) {
		le->setText(val.toString());
	}
}

QDialog* SetupWin::popOut(QWidget* box, const char* title) {
	xSheetWin* win = new xSheetWin(this);
	win->setModal(true);
	win->setWindowTitle(title);
	QVBoxLayout* lay = new QVBoxLayout(win);
	lay->addWidget(box);
	QDialogButtonBox* bbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, win);
	bbox->button(QDialogButtonBox::Ok)->setIcon(QIcon(":/images/ok-apply.png"));
	bbox->button(QDialogButtonBox::Cancel)->setIcon(QIcon(":/images/cancel.png"));
	lay->addWidget(bbox);
	connect(bbox, SIGNAL(accepted()), win, SLOT(accept()));
	connect(bbox, SIGNAL(rejected()), win, SLOT(reject()));
	QSharedPointer<xSheetState> st(new xSheetState);
	win->onOpen = [this, box, st]() {
		st->vals.clear();
		foreach(QWidget* w, box->findChildren<QWidget*>()) {
			QVariant v = sheet_value(w);
			if (v.isValid()) st->vals << qMakePair(QPointer<QWidget>(w), v);
		}
		st->roms = roms;
		st->resTarget = resTarget;
	};
	win->onCancel = [this, st]() {
		roms = st->roms;
		resTarget = st->resTarget;
		rsmodel->fill(&roms);
		fillRomSlots();		// the slot rows are made from the set: new ones, already right
		for (int i = 0; i < st->vals.size(); i++) {
			if (st->vals[i].first) sheet_put(st->vals[i].first, st->vals[i].second);
		}
	};
	return win;
}

#define	RSLOT_GS	-1
#define	RSLOT_FONT	-2
#define	RSLOT_NGS	-3	// the NeoGS flash: not the romset's, every machine's

#define	TRDOS_ROM	"trdos504t.rom"		// what a Beta Disk gets when its slot is empty


// THE MACHINE'S DEVICES
//
// One row per device on the Machine page: its name, the choice, and a button
// for the rest. The widgets are the ones the other pages had, moved here, so
// what reads and writes them stays as it is.

static QComboBox* devCombo(QStringList items) {
	QComboBox* box = new QComboBox;
	box->addItems(items);
	return box;
}

// Which machine a start takes is the application's business, not the machine's
void SetupWin::buildStart() {
	cbStart = new QComboBox;
	QLabel* lab = new QLabel(tr("Machine at start"));
	lab->setToolTip(tr("Which machine the emulator starts with"));
	ui.gridLayout_6->addWidget(lab, 0, 0);
	ui.gridLayout_6->addWidget(cbStart, 0, 1);
}

// the gear that pops a sheet out; greyed with nothing to pop
QToolButton* SetupWin::sheetButton(QWidget* body, const char* title) {
	QToolButton* btn = new QToolButton;
	btn->setIcon(QIcon(":/images/settings.png"));
	if (body) {
		QDialog* win = popOut(body, title);
		btn->setToolTip(tr("More settings"));
		connect(btn, &QToolButton::released, win, [win]() {win->adjustSize(); win->show(); win->raise();});
	} else {
		btn->setEnabled(false);
	}
	return btn;
}

QToolButton* SetupWin::devRow(QGridLayout* grid, const QString& name, QWidget* choice, QWidget* body, const char* title) {
	int row = grid->rowCount();
	QLabel* lab = new QLabel(name);
	QToolButton* btn = sheetButton(body, title);
	grid->addWidget(lab, row, 0);
	// the widths it had on its old page mean nothing here
	choice->setMinimumWidth(0);
	choice->setMaximumWidth(QWIDGETSIZE_MAX);
	grid->addWidget(choice, row, 1);
	grid->addWidget(btn, row, 2);
	return btn;
}

// a group in the page's grid: Storage and Sound in row 0, as tall as each other,
// Input and Board in row 1, at the top
static QGridLayout* devGroup(QGridLayout* page, int row, int col, const char* icon, const QString& title) {
	xIconGroup* box = new xIconGroup(icon, title);
	QGridLayout* grid = new QGridLayout(box);
	grid->setColumnStretch(1, 1);
	if (row) page->addWidget(box, row, col, Qt::AlignTop);
	else page->addWidget(box, row, col);
	page->setColumnStretch(col, 1);
	page->setRowStretch(2, 1);
	return grid;
}

// once the rows are in: one name width a column, so the choices line up from group
// to group; the row 0 groups' rows at their top
static void finishGroups(QGridLayout* page) {
	for (int c = 0; c < 2; c++) {
		QList<QWidget*> labs;		// a label, or a check box as the Next's Mouse
		for (int r = 0; r < 2; r++) {
			QLayoutItem* box = page->itemAtPosition(r, c);
			QGridLayout* g = (box && box->widget()) ? qobject_cast<QGridLayout*>(box->widget()->layout()) : NULL;
			if (!g) continue;
			for (int row = 0; row < g->rowCount(); row++) {
				QLayoutItem* itm = g->itemAtPosition(row, 0);
				QWidget* w = itm ? itm->widget() : NULL;
				if (qobject_cast<QLabel*>(w) || qobject_cast<QCheckBox*>(w)) labs << w;
			}
			if (r == 0) g->setRowStretch(g->rowCount(), 1);
		}
		int wid = 0;
		foreach(QWidget* lab, labs) wid = qMax(wid, lab->sizeHint().width());
		foreach(QWidget* lab, labs) lab->setMinimumWidth(wid);
	}
}

void SetupWin::buildDevices() {
	QWidget* area = new QWidget;
	QGridLayout* cols = new QGridLayout(area);	// Storage | Sound over Input | Board (devGroup)
	cols->setContentsMargins(0, 0, 0, 0);
	QToolButton* btn;

	QGridLayout* grid = devGroup(cols, 0, 0, ":/images/fdd_disk.png", tr("Storage"));
	tapeSum = new QLabel;
	xOptSheet tape;
	tape.field(tr("Playback speed"), fieldPair(sldTapeSpeed, labTapeSpeedVal, true),
		tr("Per cent of normal. A few images made on machines with an unusual clock load only when it is nudged either way"));
	tape.line();
	tape.check(cbTapeAuto, tr("Auto play / stop"), tr("Start the tape when a loader asks for it, stop it between blocks"), true);
	tape.check(cbTapeRewind, tr("Rewind at end"), tr("Play or the next load starts the tape again. Off: it stops at the end"), true);
	tape.check(cbTapeSet, tr("Next tape of a set"), tr("With rewind at end: a load goes on to the next tape of the set (Side B). Off: the same tape starts again, as a levels tape wants"), true);
	tape.check(cbTapeFast, tr("Fast loading"), tr("Full speed, sound off and picture held while a loader reads the tape"), true);
	tape.check(cbTapeFlash, tr("Flash loading"), tr("With fast loading: ROM blocks go straight to the machine"), true);
	tape.check(cbTapeEdge, tr("Edge detection"), tr("With fast loading: a loader waiting for a pulse gets it at once. Faster, not exact"), true);
	QToolButton* tapeGear = devRow(grid, tr("Tape"), tapeSum, tape.body, "Machine: Tape");
	foreach(QCheckBox* cb, QList<QCheckBox*>() << cbTapeAuto << cbTapeRewind << cbTapeFast << cbTapeFlash << cbTapeEdge)
		connect(cb, &QCheckBox::toggled, this, &SetupWin::fillDevSummary);

	xOptSheet disk;
	disk.check(bdtbox, tr("Fast disk access"), tr("No head-seek and rotation delays"), true);
	disk.check(cbAddBoot, tr("Add boot loader"), tr("Write a boot file into TR-DOS images that have none"), true);
	disk.line();
	dosRomBox = new QComboBox;
	dosRomBtn = new QToolButton;
	dosRomBtn->setIcon(QIcon(":/images/fileopen.png"));
	dosRomBtn->setToolTip(tr("Pick a ROM file"));
	connect(dosRomBox, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
		romSlotPick(hw_reset_bank(conf.zx->hw->id, RES_DOS), dosRomBox->itemData(idx).toString());
		fillRomSlots();
	});
	connect(dosRomBtn, &QToolButton::released, this, [this]() {
		romSlotFile(dosRomBox, hw_reset_bank(conf.zx->hw->id, RES_DOS));
		fillRomSlots();
	});
	disk.field(tr("TR-DOS ROM"), fieldPair(dosRomBox, dosRomBtn, false), tr("The same slot as TR-DOS in the ROM window"));
	// a Beta Disk put on a machine with nothing in that slot would not boot
	connect(diskTypeBox, QOverload<int>::of(&QComboBox::activated), this, [this]() {
		int bank = hw_reset_bank(conf.zx->hw->id, RES_DOS);
		if ((getRFIData(diskTypeBox) == DIF_BDI) && (bank >= 0) && romSlotFileName(bank).isEmpty()) {
			romSlotPick(bank, TRDOS_ROM);
			fillRomSlots();
		}
		fillDosRom();
	});
	disk.field(tr("Interleave"), cbFlpInterleave,
		tr("Sector order on each track of a TRD or SCL image, set when it is opened. TR-DOS formats 1, 9, 2, 10..."));
	// how many drives are on the cable, and what each is; the disk in one is
	// the Drives menu's
	drvCountBox = new QComboBox;
	drvCountBox->addItem(QString::fromUtf8("×1 - A"), 1);
	drvCountBox->addItem(QString::fromUtf8("×2 - A, B"), 2);
	drvCountBox->addItem(QString::fromUtf8("×3 - A, B, C"), 3);
	drvCountBox->addItem(QString::fromUtf8("×4 - A, B, C, D"), 4);
	drvCountBox->setItemDelegate(new xTwoPartDelegate(drvCountBox));
	new xTwoPartPainter(drvCountBox);
	connect(drvCountBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {showDriveRows();});
	disk.field(tr("Number of drives"), drvCountBox, tr("A drive left out is not there at all: software finds it missing"));
	QWidget* drvs = new QWidget;
	QGridLayout* dgrid = new QGridLayout(drvs);
	dgrid->setContentsMargins(0, 0, 0, 0);
	dgrid->addWidget(new QLabel(tr("80 cylinders")), 0, 1);
	dgrid->addWidget(new QLabel(tr("Double side")), 0, 2);
	QCheckBox* cyl[4] = {a80box, b80box, c80box, d80box};
	QCheckBox* dsd[4] = {adsbox, bdsbox, cdsbox, ddsbox};
	for (int i = 0; i < 4; i++) {
		QLabel* lab = new QLabel;
		lab->setPixmap(QIcon(QString(":/images/fdd_disk_%0.png").arg(QChar('A' + i))).pixmap(16, 16));
		lab->setToolTip(QString("Drive %0").arg(QChar('A' + i)));
		cyl[i]->setText(QString());
		dsd[i]->setText(QString());
		dgrid->addWidget(lab, i + 1, 0);
		dgrid->addWidget(cyl[i], i + 1, 1, Qt::AlignHCenter);
		dgrid->addWidget(dsd[i], i + 1, 2, Qt::AlignHCenter);
		// a +3 has two drives
		drvRow[i] << lab << cyl[i] << dsd[i];
	}
	dgrid->setColumnStretch(3, 1);
	disk.field(tr("Drives"), drvs);
	btn = devRow(grid, tr("Floppy disk"), diskTypeBox, disk.body, "Machine: Floppy Disk");
	diskRow << grid->itemAtPosition(grid->rowCount() - 1, 0)->widget() << diskTypeBox << btn;

	// the images and what is in them are the Drives menu's
	xOptSheet hdd;
	hdd.field(tr("Master"), hm_type);
	hdd.field(tr("Slave"), hs_type);
	btn = devRow(grid, tr("Hard disk"), hiface, hdd.body, "Machine: Hard Disk");
	hddRow << grid->itemAtPosition(grid->rowCount() - 1, 0)->widget() << hiface << btn;

	sdSum = new QLabel;
	btn = devRow(grid, tr("SD card"), sdSum, NULL, NULL);
	sdRow << grid->itemAtPosition(grid->rowCount() - 1, 0)->widget() << sdSum << btn;

	// a divMMC on the edge connector; its cards are the Drives menu's
	divBox = devCombo(QStringList() << tr("None") << tr("divMMC"));
	divBox->setObjectName("divBox");
	xOptSheet dv;
	divRamBox = devCombo(QStringList() << tr("128K") << tr("512K"));
	divRamBox->setObjectName("divRamBox");
	dv.field(tr("RAM"), divRamBox);
	divWrBox = new QCheckBox;
	divWrBox->setObjectName("divWrBox");
	dv.check(divWrBox, tr("EEPROM writable"), tr("For flashing; the firmware does not start"));
	dv.line();
	divFwBox = new QComboBox;
	divFwBox->setObjectName("divFwBox");
	QToolButton* fwBtn = new QToolButton;
	fwBtn->setIcon(QIcon(":/images/fileopen.png"));
	fwBtn->setToolTip(tr("Pick a firmware file"));
	connect(fwBtn, &QToolButton::released, this, &SetupWin::divFirmwareFile);
	dv.field(tr("Firmware"), fieldPair(divFwBox, fwBtn, false), tr("What the EEPROM holds. Apply writes it"));
	btn = devRow(grid, tr("divMMC"), divBox, dv.body, "Machine: divMMC");
	btn->setObjectName("divGear");
	divRow << grid->itemAtPosition(grid->rowCount() - 1, 0)->widget() << divBox << btn;
	QString divTip = tr("Floppy and hard disk controllers are\nunavailable while divMMC is enabled");
	divRow[0]->setToolTip(divTip);
	divBox->setToolTip(divTip);
	qobject_cast<QLabel*>(divRow[0])->setText(tr("divMMC (!)"));	// measured so (finishGroups)
	connect(divBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {showBoardStorage();});

	slotSum = new QLabel;
	btn = devRow(grid, tr("Cartridge"), slotSum, NULL, NULL);
	slotRow << grid->itemAtPosition(grid->rowCount() - 1, 0)->widget() << slotSum << btn;

	grid = devGroup(cols, 1, 0, ":/images/joystick.png", tr("Input"));
	joyBox = devCombo(QStringList() << tr("None") << tr("Kempston 5-bit") << tr("Kempston 8-bit"));
	devRow(grid, tr("Joystick"), joyBox, NULL, NULL);
	mouseBox = devCombo(QStringList() << tr("None") << tr("Kempston mouse"));
	xOptSheet mouse;
	mouse.field(tr("Sensitivity"), fieldPair(sldSensitivity, NULL, true),
		tr("How fast the pointer moves. In the middle it keeps up with the PC one"));
	mouse.line();
	mouse.check(ratWheel, tr("Wheel"), tr("The wheel is read too, as on the extended Kempston mouse"));
	mouse.check(cbSwapButtons, tr("Swap buttons"), tr("The left and right buttons trade places"));
	QToolButton* mouseGear = devRow(grid, tr("Mouse"), mouseBox, mouse.body, "Machine: Mouse");
	btn = devRow(grid, tr("PC keyboard"), cbScanTab, NULL, NULL);
	kbdRow << grid->itemAtPosition(grid->rowCount() - 1, 0)->widget() << cbScanTab << btn;

	grid = devGroup(cols, 0, 1, ":/images/speaker.png", tr("Sound"));
	xOptSheet psg;
	psg.field(tr("Chip"), cbPsgType);
	psg.field(tr("Clock"), fieldPair(cbPsgFrq, labPsgMhz, false),
		tr("Auto: follows the CPU clock"));
	psg.field(tr("Stereo"), cbPsgStereo);
	psg.field(tr("Separation"), fieldPair(sldPsgSep, labPsgSep, false),
		tr("100%: the channels kept apart, 0%: mono"));
	devRow(grid, tr("PSG"), cbPsgCount, psg.body, "Machine: PSG");
	devRow(grid, tr("DAC"), sdrvBox, NULL, NULL);
	gsBox = devCombo(QStringList() << tr("None") << tr("Original") << tr("NeoGS"));	// MAC_GS_*
	xOptSheet gs;
	// a card's ROM: the list and the folder button, a pick going to its slot
	auto romPair = [this](QComboBox*& box, int slot) -> QWidget* {
		box = new QComboBox;
		QToolButton* btn = new QToolButton;
		btn->setIcon(QIcon(":/images/fileopen.png"));
		btn->setToolTip(tr("Pick a ROM file"));
		QComboBox* b = box;
		connect(b, QOverload<int>::of(&QComboBox::activated), this, [this, b, slot](int idx) {
			romSlotPick(slot, b->itemData(idx).toString());
		});
		connect(btn, &QToolButton::released, this, [this, b, slot]() {romSlotFile(b, slot);});
		return fieldPair(b, btn, false);
	};
	// the ROM row holds the card's own file: the original one's in the romset,
	// the NeoGS flash one for every machine
	gsRomStack = new QStackedWidget;
	gsRomStack->addWidget(romPair(gsRomBox, RSLOT_GS));
	gsRomStack->addWidget(romPair(ngsRomBox, RSLOT_NGS));
	connect(gsBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
		gsRomStack->setCurrentIndex((idx == MAC_GS_NEOGS) ? 1 : 0);
	});
	gs.field(tr("ROM"), gsRomStack);
	gs.field(tr("Separation"), fieldPair(sldGsSep, labGsSep, false),
		tr("100%: the sides as the card has them, 0%: mono"));
	gs.line();
	gs.check(gsrbox, tr("Reset"), tr("The card is reset with the machine"));
	devRow(grid, tr("General Sound"), gsBox, gs.body, "Machine: General Sound");
	saaBox = devCombo(QStringList() << tr("Off") << tr("On"));
	devRow(grid, tr("SAA1099"), saaBox, NULL, NULL);

	grid = devGroup(cols, 1, 1, ":/images/grp-board.png", tr("Board"));
	devRow(grid, tr("Turbo"), cbCpuTurbo, NULL, NULL);
	clockBox = devCombo(QStringList() << tr("None") << tr("Gluk"));	// MAC_CLOCK_*
	btn = devRow(grid, tr("Clock"), clockBox, NULL, NULL);
	clockRow << grid->itemAtPosition(grid->rowCount() - 1, 0)->widget() << clockBox << btn;
	clockRow[0]->setToolTip(tr("Mr Gluk's clock on ports #EFF7, #DFF7, #BFF7.\nGluk Reset Service shows and sets it"));
	finishGroups(cols);
	qobject_cast<QLabel*>(divRow[0])->setText(tr("divMMC"));

	// THE NEXT'S OWN: NextZXOS's settings in its menu's words, this computer's for the Next among them
	nxArea = new QWidget;
	QVBoxLayout* nv = new QVBoxLayout(nxArea);
	nv->setContentsMargins(0, 0, 0, 0);
	QHBoxLayout* head = new QHBoxLayout;
	QLabel* logo = new QLabel;
	logo->setPixmap(QIcon(":/images/next.png").pixmap(24, 24));
	QLabel* title = new QLabel(tr("ZX Spectrum Next"));
	QFont tf = title->font();
	tf.setBold(true);
	title->setFont(tf);
	nxCore = new QLabel;
	QLabel* whose = new QLabel(tr("(the gray ones are set by NextZXOS)"));
	whose->setToolTip(tr("NextZXOS sets these itself: in its NMI menu,\nSettings. What is changed there takes\neffect when the menu closes"));
	QFrame* rule = new QFrame;
	rule->setFrameShape(QFrame::HLine);
	rule->setFrameShadow(QFrame::Sunken);
	head->addWidget(logo);
	head->addWidget(title);
	head->addWidget(nxCore);
	head->addWidget(whose);
	head->addWidget(rule, 1);
	nv->addLayout(head);
	QGridLayout* ncols = new QGridLayout;		// as the usual groups (devGroup)
	nv->addLayout(ncols);
	// NextZXOS's values grayed: a field (as wide as its column's widest, lineUp) or a check box
	auto field = [this](const char* name, const QStringList& vals, const QString& tip) {
		QLineEdit* f = new QLineEdit;
		f->setEnabled(false);
		f->setFocusPolicy(Qt::NoFocus);
		int w = 0;
		foreach(const QString& v, vals) w = qMax(w, f->fontMetrics().horizontalAdvance(v));
		f->setFixedWidth(w + 12);		// the frame and the text margins
		f->setToolTip(tip);
		nxText[name] = f;
		return f;
	};
	auto lineUp = [](QGridLayout* g) {
		for (int c = 1; c < 4; c++) {
			QList<QLineEdit*> fs;
			for (int r = 0; r < g->rowCount(); r++) {
				QLayoutItem* itm = g->itemAtPosition(r, c);
				QLineEdit* f = itm ? qobject_cast<QLineEdit*>(itm->widget()) : NULL;
				if (f) fs << f;
			}
			int w = 0;
			foreach(QLineEdit* f, fs) w = qMax(w, f->minimumWidth());
			foreach(QLineEdit* f, fs) f->setFixedWidth(w);
		}
	};
	// a few check boxes in one cell
	auto pack = [](const QList<QWidget*>& ws) {
		QWidget* box = new QWidget;
		QHBoxLayout* hl = new QHBoxLayout(box);
		hl->setContentsMargins(0, 0, 0, 0);
		foreach(QWidget* w, ws) hl->addWidget(w);
		hl->addStretch(1);
		return box;
	};
	auto flag = [this](const char* name, const QString& text, const QString& tip) {
		QCheckBox* c = new QCheckBox(text);
		c->setEnabled(false);
		c->setToolTip(tip);
		nxFlag[name] = c;
		return c;
	};
	// a group's grid: the name, up to three values, room, a gear
	auto nxGroup = [ncols](int row, int col, const char* icon, const QString& title) {
		QGridLayout* g = devGroup(ncols, row, col, icon, title);
		g->setColumnStretch(1, 0);
		g->setColumnStretch(4, 1);
		g->setHorizontalSpacing(4);		// pairs of values in a row: a little closer
		return g;
	};
	// a row of such a grid; `span`: the last widget takes the room too; with no
	// name the row starts in the name's column
	auto line = [](QGridLayout* g, const QString& name, const QList<QWidget*>& ws, QToolButton* gearBtn, bool span) {
		int row = g->rowCount();
		int col = name.isEmpty() ? 0 : 1;
		if (col) g->addWidget(new QLabel(name), row, 0);
		for (int i = 0; i < ws.size(); i++) {
			bool last = (i == ws.size() - 1);
			g->addWidget(ws[i], row, col + i, 1, (last && span) ? 5 - col - i : 1);
		}
		if (gearBtn) g->addWidget(gearBtn, row, 5);
	};
	// a gear that opens a sheet of the usual groups
	auto gear = [this](QToolButton* sheet) {
		QToolButton* btn = sheetButton(NULL, NULL);
		btn->setEnabled(true);
		btn->setToolTip(tr("More settings"));
		connect(btn, &QToolButton::released, sheet, &QToolButton::click);
		return btn;
	};
	QString joyTip = tr("Sinclair 2, Kempston 1, Cursor, Sinclair 1,\nKempston 2, MD 1, MD 2 or Keyjoy");
	QStringList joys = QStringList() << tr("L: Kempston 2") << tr("R: Kempston 2");

	grid = nxGroup(0, 0, ":/images/fdd_disk.png", tr("Storage"));
	nxSdSum = new xElideLabel;
	line(grid, tr("SD card"), QList<QWidget*>() << nxSdSum, sheetButton(NULL, NULL), true);
	line(grid, "NextZXOS", QList<QWidget*>() << field("NextZXOS", QStringList() << "v2.09.99" << tr("not started"),
		tr("The NextZXOS the machine runs, from its card")), NULL, false);
	nxTapeSum = new QLabel;
	line(grid, tr("Tape"), QList<QWidget*>() << nxTapeSum, gear(tapeGear), true);

	grid = nxGroup(1, 0, ":/images/joystick.png", tr("Input"));
	line(grid, tr("Joystick"), QList<QWidget*>() << field("Joystick L", joys, tr("The left joystick:\n") + joyTip)
		<< field("Joystick R", joys, tr("The right joystick:\n") + joyTip), NULL, false);
	line(grid, tr("Mouse"), QList<QWidget*>() << field("Mouse DPI", QStringList() << tr("Standard DPI") << tr("Medium DPI"),
			tr("Low, Standard, Medium or High DPI"))
		<< field("Mouse Btn", QStringList() << tr("Right/Left"), tr("The buttons: Left/Right, or swapped,\nRight/Left")), gear(mouseGear), false);
	// the row's name is this computer's switch: the mouse in use, DPI and buttons NextZXOS's
	nxMouseOn = new QCheckBox(tr("Mouse"));
	nxMouseOn->setToolTip(tr("This computer's mouse as the Next's: a click\nin the window takes the pointer"));
	QLayoutItem* mlab = grid->itemAtPosition(grid->rowCount() - 1, 0);
	delete mlab->widget();
	grid->addWidget(nxMouseOn, grid->rowCount() - 1, 0);
	line(grid, tr("Keyboard"), QList<QWidget*>() << field("Keyboard", QStringList() << tr("Issue 3"),
		tr("Issue 3 or Issue 2: what bit 6 of #FE\nreads with no tape playing")), NULL, false);
	lineUp(grid);

	// by device: the PSG, its chips' mono, the DACs, the beeper
	grid = nxGroup(0, 1, ":/images/speaker.png", tr("Sound"));
	line(grid, tr("PSG"), QList<QWidget*>()
		<< field("Audiochip", QStringList() << QString::fromUtf8("×3 ZXN-8950"),
			tr("YM, AY, ZXN-8950 or Off. x3: TurboSound,\nall three play; x1: the one selected only"))
		<< field("Stereo", QStringList() << "ACB", tr("ABC or ACB: the voices from left to right")), NULL, false);
	QString monoTip = tr("The chip's three voices on both sides");
	line(grid, tr("AY mono"), QList<QWidget*>() << pack(QList<QWidget*>() << flag("AY0 Mono", "0", monoTip)
		<< flag("AY1 Mono", "1", monoTip) << flag("AY2 Mono", "2", monoTip)), NULL, true);
	line(grid, tr("DAC"), QList<QWidget*>() << flag("Covox", tr("Covox"), tr("The DACs: Covox, Soundrive and SpecDrum\nports")), NULL, true);
	line(grid, tr("Beeper"), QList<QWidget*>()
		<< field("BEEPer", QStringList() << tr("Speaker only") << tr("All outputs"), tr("All outputs, or the internal speaker only"))
		<< flag("Int. speaker", tr("Int. speaker"), tr("The board's own speaker")), NULL, false);
	lineUp(grid);

	grid = nxGroup(1, 1, ":/images/grp-board.png", tr("Board"));
	line(grid, tr("Timing"), QList<QWidget*>()
		<< field("CPU speed", QStringList() << "3.5 MHz" << "28 MHz", tr("CPU speed: 3.5, 7, 14 or 28 MHz,\nas programs ask"))
		<< field("Frequency", QStringList() << "50 Hz", tr("Frames: 50 or 60 Hz"))
		<< field("Timings", QStringList() << "Next (+3)" << "Pentagon", tr("48K, 128K, Next (+3) or Pentagon: that\nmachine's frame and contention")), NULL, false);
	line(grid, QString(), QList<QWidget*>() << pack(QList<QWidget*>()
		<< flag("Contention", tr("Contention"), tr("The ULA holds the CPU off the screen\nbanks, at 3.5 MHz"))
		<< flag("Timex RD", tr("Timex RD"), tr("Port #FF reads back the Timex screen\nmode"))
		<< flag("ULAplus", tr("ULAplus"), tr("64 colors, when the program asks for them"))), NULL, true);
	cbEspLan = new QCheckBox;
	xOptSheet esp;
	esp.check(cbEspLan, tr("Server on the network"),
		tr("The emulated module's server (AT+CIPSERVER) and a UDP link's own port take clients from other computers too. Off: the server only from this one, at 127.0.0.1; a UDP link hears only whom it sent to"), true);
	for (int i = 0; i < 2; i++) {
		uartBox[i] = new QComboBox;
		uartBox[i]->setObjectName(i ? "uartPi" : "uartEsp");
		uartBox[i]->setEditable(true);
		uartBox[i]->setInsertPolicy(QComboBox::NoInsert);
		uartBox[i]->setToolTip(i ? tr("The serial port of this computer the Pi UART is wired to")
			: tr("The Wi-Fi module: emulated, or a real one on a serial port of this computer"));
		uartBox[i]->setMinimumWidth(0);
		line(grid, i ? tr("Pi UART") : tr("ESP UART"), QList<QWidget*>() << uartBox[i],
			i ? sheetButton(NULL, NULL) : sheetButton(esp.body, "Machine: ESP UART"), true);
	}
	finishGroups(ncols);
	QWidget* page = new QWidget;
	QVBoxLayout* pv = new QVBoxLayout(page);
	pv->setContentsMargins(0, 0, 0, 0);
	pv->addWidget(area);
	pv->addWidget(nxArea);
	devArea = area;
	ui.verticalLayout_39->insertWidget(1, page);
	tidySoundPage();
}

// What the Next has set, in NextZXOS's words, as the machine has it now (Options
// stops the machine while it is open).
void SetupWin::fillNextGroup() {
	Computer* comp = conf.zx;
	if (comp->hw->id != HW_ZXNEXT) return;
	emu_lock();		// Options pauses the machine, but does not wait for it
	auto rd = [comp](int r) {return next_nr_peek(comp, r);};
	// the menu calls the +3 timing the Next's own
	static const char* const tim[4] = {"48K", "128K", "Next (+3)", "Pentagon"};
	static const char* const dpi[4] = {QT_TR_NOOP("Low"), QT_TR_NOOP("Standard"), QT_TR_NOOP("Medium"), QT_TR_NOOP("High")};
	static const char* const chip[4] = {"YM", "AY", "ZXN-8950", QT_TR_NOOP("Off")};
	static const char* const joy[8] = {"Sinclair 2", "Kempston 1", "Cursor", "Sinclair 1", "Kempston 2", "MD 1", "MD 2",
		QT_TR_NOOP("Keyjoy")};
	static const char* const mhz[4] = {"3.5", "7", "14", "28"};
	int r05 = rd(0x05), r06 = rd(0x06), r08 = rd(0x08), r09 = rd(0x09), r0a = rd(0x0a);
	QMap<QString, QString> v;
	QMap<QString, bool> f;
	v["Joystick L"] = tr("L: ") + tr(joy[(((r05 >> 3) & 1) << 2) | ((r05 >> 6) & 3)]);
	v["Joystick R"] = tr("R: ") + tr(joy[(((r05 >> 1) & 1) << 2) | ((r05 >> 4) & 3)]);
	v["Keyboard"] = (r08 & 0x01) ? tr("Issue 2") : tr("Issue 3");
	v["Mouse DPI"] = tr("%0 DPI").arg(tr(dpi[r0a & 3]));
	v["Mouse Btn"] = (r0a & 0x08) ? tr("Right/Left") : tr("Left/Right");
	v["Stereo"] = (r08 & 0x20) ? "ACB" : "ABC";
	v["BEEPer"] = (r06 & 0x40) ? tr("Speaker only") : tr("All outputs");
	v["Audiochip"] = QString::fromUtf8((r08 & 0x02) ? "×3 " : "×1 ") + tr(chip[r06 & 3]);
	v["CPU speed"] = QString("%0 MHz").arg(mhz[(rd(0x07) >> 4) & 3]);
	v["Frequency"] = (r05 & 0x04) ? "60 Hz" : "50 Hz";		// the one in use: a read merges it in
	v["Timings"] = tim[comp->next->timing & 3];
	char os[16];
	next_os_version(comp, os, sizeof(os));
	v["NextZXOS"] = os[0] ? QString(os) : tr("not started");
	int core = rd(0x01);
	nxCore->setText(tr("core %0.%1.%2").arg(core >> 4).arg(core & 15, 2, 10, QChar('0')).arg(rd(0x0e), 2, 10, QChar('0')));
	f["Int. speaker"] = r08 & 0x10;
	f["Covox"] = r08 & 0x08;
	f["AY0 Mono"] = r09 & 0x20;
	f["AY1 Mono"] = r09 & 0x40;
	f["AY2 Mono"] = r09 & 0x80;
	f["Contention"] = !(r08 & 0x40);
	f["Timex RD"] = r08 & 0x04;
	f["ULAplus"] = rd(0x85) & 0x01;
	emu_unlock();
	for (auto it = nxText.begin(); it != nxText.end(); ++it) it.value()->setText(v.value(it.key()));
	for (auto it = nxFlag.begin(); it != nxFlag.end(); ++it) it.value()->setChecked(f.value(it.key()));
}

// The volumes, named the way the devices are, with the master set apart.

void SetupWin::tidySoundPage() {
	QGridLayout* grid = ui.gridLayout_14;
	while (grid->count() > 0) {
		QLayoutItem* itm = grid->takeAt(0);
		if (itm->widget()) itm->widget()->hide();
		delete itm;
	}
	struct {
		QLabel* lab;
		QString name;
		QSlider* sld;
		QSpinBox* spin;
	} vols[] = {
		{ui.label_14, tr("Master"), ui.sldMasterVol, ui.sbMasterVol},
		{ui.label_9, tr("Beeper"), ui.sldBeepVol, ui.sbBeepVol},
		{ui.label_10, tr("Tape"), ui.sldTapeVol, ui.sbTapeVol},
		{ui.label_11, tr("PSG"), ui.sldAYVol, ui.sbAYVol},
		{ui.label_30, tr("DAC"), ui.sldSdrvVol, ui.sbSdrvVol},
		{new QLabel, tr("General Sound"), ui.sldGSVol, ui.sbGSVol},
		{new QLabel, tr("SAA1099"), ui.sldSAAVol, ui.sbSAAVol}
	};
	int row = 0;
	for (size_t i = 0; i < sizeof(vols) / sizeof(vols[0]); i++) {
		vols[i].lab->setText(vols[i].name);
		vols[i].lab->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		grid->addWidget(vols[i].lab, row, 0);
		grid->addWidget(vols[i].sld, row, 1);
		grid->addWidget(vols[i].spin, row, 2);
		vols[i].lab->show();
		vols[i].sld->show();
		vols[i].spin->show();
		row++;
		if (i == 0) {
			QFrame* frm = new QFrame;
			frm->setFrameShape(QFrame::HLine);
			frm->setFrameShadow(QFrame::Sunken);
			grid->addWidget(frm, row++, 0, 1, 3);
		}
	}
	grid->setColumnStretch(1, 1);
	// the gaps that stood above GS reset and under the output rows
	foreach(QLayout* lay, QList<QLayout*>() << ui.verticalLayout_22 << ui.gridLayout_5) {
		for (int i = lay->count() - 1; i >= 0; i--) {
			if (lay->itemAt(i)->spacerItem())
				delete lay->takeAt(i);
		}
	}
	// the groups keep their height, the page's spare room goes under them
	ui.verticalLayout_43->addStretch(1);
}

// what the rows with no choice of their own show
void SetupWin::fillDevSummary() {
	QStringList tape;
	if (cbTapeFast->isChecked()) {
		tape << tr("fast");
		if (cbTapeFlash->isChecked()) tape << tr("flash");
		if (cbTapeEdge->isChecked()) tape << tr("edge");
	}
	if (cbTapeAuto->isChecked()) tape << tr("auto");
	tapeSum->setText(tape.isEmpty() ? tr("plain") : tape.join(", "));
	nxTapeSum->setText(tapeSum->text());
	Computer* comp = conf.zx;
	QString sd = comp->sdc->image ? QString::fromLocal8Bit(comp->sdc->image) : QString();
	QString slot = comp->slot->path ? QString::fromLocal8Bit(comp->slot->path) : QString();
	sdSum->setText(sd.isEmpty() ? tr("(no card)") : QFileInfo(sd).fileName());
	nxSdSum->setFull(sd.isEmpty() ? tr("(no card)") : QDir::toNativeSeparators(sd));
	slotSum->setText(slot.isEmpty() ? tr("(empty)") : QFileInfo(slot).fileName());
}

// THE divMMC's FIRMWARE
//
// The combo shows what the EEPROM holds, told by its bytes: a bundled esxDOS
// release, erased, or something else - flashed in the machine or loaded from a
// file. A choice is written by Apply, so opening and closing Options never
// touches the chip.

#define	DIVFW_ESX	"@esx:"		// and the release
#define	DIVFW_ERASED	"@erased"
#define	DIVFW_CHIP	"@chip"		// keep what the chip has

// what the EEPROM would hold after a choice; empty if it can't be read
static QByteArray div_fw_bytes(const QString& choice) {
	QString path = choice;
	if (choice == DIVFW_ERASED) return QByteArray(DIV_EE_SIZE, (char)0xff);
	if (choice.startsWith(DIVFW_ESX))
		path = QString::fromLocal8Bit(div_esx_rom(choice.mid(strlen(DIVFW_ESX)).toStdString()).c_str());
	QFile file(path);
	if (!file.open(QFile::ReadOnly)) return QByteArray();
	QByteArray res = file.read(DIV_EE_SIZE);
	if (res.isEmpty()) return res;
	res.append(QByteArray(DIV_EE_SIZE - res.size(), (char)0xff));	// a shorter file leaves the rest erased
	return res;
}

static QByteArray div_chip_bytes() {
	return QByteArray((const char*)conf.zx->div->data, DIV_EE_SIZE);
}

// the esxDOS banner in an image ("v0.8.8-DivMMC"), to tell one custom firmware from another
static QString div_fw_banner(const QByteArray& img) {
	QRegularExpressionMatch m = QRegularExpression("v\\d+\\.\\d+\\.\\d+-Div\\w+").match(QString::fromLatin1(img));
	return m.hasMatch() ? m.captured(0) : QString();
}

void SetupWin::fillDivFirmware() {
	QByteArray chip = div_chip_bytes();
	divFwBox->clear();
	for (int i = 0; div_esx_releases[i]; i++)
		divFwBox->addItem(QString("esxDOS %0").arg(div_esx_releases[i]), QString(DIVFW_ESX) + div_esx_releases[i]);
	divFwBox->addItem(tr("(erased)"), DIVFW_ERASED);
	std::string rel = div_esx_release_of(conf.zx->div->data);
	if (!rel.empty()) {
		divFwBox->setCurrentIndex(divFwBox->findData(QString(DIVFW_ESX) + rel.c_str()));
	} else if (chip == div_fw_bytes(DIVFW_ERASED)) {
		divFwBox->setCurrentIndex(divFwBox->findData(DIVFW_ERASED));
	} else {
		QString ban = div_fw_banner(chip);
		divFwBox->addItem(ban.isEmpty() ? tr("Custom") : tr("Custom: %0").arg(ban), DIVFW_CHIP);
		divFwBox->setItemData(divFwBox->count() - 1, tr("What the EEPROM holds now"), Qt::ToolTipRole);
		divFwBox->setCurrentIndex(divFwBox->count() - 1);
	}
}

void SetupWin::divFirmwareFile() {
	QString path = QFileDialog::getOpenFileName(this, tr("Firmware"), "", tr("Images (*.bin *.rom);;All files (*)"), NULL, QFileDialog::DontUseNativeDialog);
	if (path.isEmpty()) return;
	if (div_fw_bytes(path).isEmpty()) {
		shitHappens("Can't read that file");
		return;
	}
	int idx = divFwBox->findData(path);
	if (idx < 0) {
		divFwBox->addItem(tr("Custom: %0").arg(QFileInfo(path).fileName()), path);
		idx = divFwBox->count() - 1;
		divFwBox->setItemData(idx, path, Qt::ToolTipRole);
	}
	divFwBox->setCurrentIndex(idx);
}

// The choice into the chip, if it changes what the chip holds. A firmware that is
// only in the chip is not replaced without asking. Returns true if it wrote.
bool SetupWin::applyDivFirmware() {
	QString choice = divFwBox->currentData().toString();
	if (choice == DIVFW_CHIP) return false;
	QByteArray img = div_fw_bytes(choice);
	QByteArray chip = div_chip_bytes();
	if (img.isEmpty() || (img == chip)) return false;
	bool known = !div_esx_release_of(conf.zx->div->data).empty() || (chip == div_fw_bytes(DIVFW_ERASED));
	if (!known && !areSure("The firmware in the EEPROM will be lost. Replace it?")) return false;
	DivMMC* div = conf.zx->div;
	emu_lock();
	memcpy(div->data, img.constData(), DIV_EE_SIZE);
	div->eeDirty = 1;
	div_map(div);
	emu_unlock();
	return true;
}

// the rows of the drives that are fitted; a +3 has two at most
void SetupWin::showDriveRows() {
	int max = (getRFIData(diskTypeBox) == DIF_P3DOS) ? 2 : 4;
	QStandardItemModel* cnts = qobject_cast<QStandardItemModel*>(drvCountBox->model());
	for (int i = 0; i < 4; i++) {
		if (cnts) cnts->item(i)->setEnabled(i < max);
		foreach(QWidget* w, drvRow[i]) w->setVisible(i < qMin(max, getRFIData(drvCountBox)));
	}
}

// rows only some cores have
void SetupWin::showDevRows() {
	int hw = conf.zx->hw->id;
	bool sd = hw_has_sd(conf.zx->hw);
	bool slot = (hw == HW_ZX48) || (hw == HW_ALF);
	// only these read PC scancodes
	bool kbd = (hw == HW_ATM2) || (hw == HW_PENTEVO) || (hw == HW_TSLAB);
	foreach(QWidget* w, sdRow) w->setVisible(sd);
	foreach(QWidget* w, divRow) w->setVisible(hw_takes_divmmc(conf.zx->hw));
	foreach(QWidget* w, slotRow) w->setVisible(slot);
	foreach(QWidget* w, kbdRow) w->setVisible(kbd);
	foreach(QWidget* w, clockRow) w->setVisible(hw_takes_gluk(conf.zx->hw));
	showDriveRows();
	showBoardStorage();
	// the Next sets itself up, by its nextregs: its own groups in place of the usual
	bool next = (hw == HW_ZXNEXT);
	// the one going away first: both shown for a moment would stretch the window
	(next ? devArea : nxArea)->hide();
	(next ? nxArea : devArea)->show();
	ratWheel->setEnabled(!next);		// always read on the Next
	ui.gbCpu->setEnabled(!next);
	ui.gbUla->setEnabled(!next);
	ui.gbBoard->setEnabled(!next);
	// the Next sets these itself, by its NextZXOS settings
	ui.pbAdvMachine->setEnabled(!next);
	if (next) advWin->hide();
	fillNextGroup();
	QString fixed = tr("Built into this board");
	// ALF reads its two joysticks its own way, on #1F and #FE
	joyBox->setEnabled(hw != HW_ALF);
	joyBox->setToolTip((hw == HW_ALF) ? fixed : QString());
	QStandardItemModel* joys = qobject_cast<QStandardItemModel*>(joyBox->model());
	if (joys) {
		bool k8 = hw_kempston8(conf.zx->hw);
		joys->item(MAC_JOY_KEMPSTON8)->setEnabled(k8);
		joys->item(MAC_JOY_KEMPSTON8)->setToolTip(k8 ? QString() : tr("This board's joystick port has no Fire 2 to Fire 4"));
	}
	// the +2A and +3 never page TR-DOS in
	QStandardItemModel* difs = qobject_cast<QStandardItemModel*>(diskTypeBox->model());
	int bdi = diskTypeBox->findData(DIF_BDI);
	if (difs && (bdi >= 0)) difs->item(bdi)->setEnabled((hw != HW_PLUS2A) && (hw != HW_PLUS3));
	fillDosRom();
	fillDevSummary();
}

// The Floppy disk and Hard disk rows: a controller on the board is not swapped out, and
// with the divMMC set on both are out of the machine - they keep what they say,
// for when it goes. Follows the divMMC row before Apply.
void SetupWin::showBoardStorage() {
	const xMachine* mac = xm_find(conf.macId);
	int bi = mac ? mac->builtin : 0;
	bool board = divBox->currentIndex() && hw_takes_divmmc(conf.zx->hw);
	QString fixed = tr("Built into this board");
	QString out = tr("Unavailable while divMMC is enabled");
	foreach(QWidget* w, diskRow) w->setEnabled(!board);
	foreach(QWidget* w, hddRow) w->setEnabled(!board);
	diskTypeBox->setEnabled(!board && !(bi & MAC_BI_DISK));
	diskTypeBox->setToolTip(board ? out : (bi & MAC_BI_DISK) ? fixed : QString());
	hiface->setEnabled(!board && !(bi & MAC_BI_IDE));
	hiface->setToolTip(board ? out : (bi & MAC_BI_IDE) ? fixed : QString());
	clockBox->setEnabled(!(bi & MAC_BI_CLOCK));
	clockBox->setToolTip((bi & MAC_BI_CLOCK) ? fixed : QString());
	// the board on takes the disk controllers out (comp_board_storage): its name marks it
	QLabel* divName = qobject_cast<QLabel*>(divRow.value(0));
	if (divName) divName->setText(board ? tr("divMMC (!)") : tr("divMMC"));
}

// THE WHOLE CONFIGURATION, IN AND OUT
//
// One text file with the settings, the layouts and the machines of your own.
// Importing one or going back to the defaults reads the configuration again
// under the running machine, so the page has to be filled from scratch after.

#define	CFG_FILTER	"Xpeccy+ settings (*.conf);;All files (*)"
#define	CFG_NAME	"xpeccy-settings.conf"

void SetupWin::cfgExport() {
	QString path = QFileDialog::getSaveFileName(this, tr("Export settings"),
		QString::fromLocal8Bit(conf.path.confDir.c_str()) + SLASH CFG_NAME,
		tr(CFG_FILTER));
	if (path.isEmpty()) return;
	apply();				// what is written is what the page shows
	if (!xconf_export(path))
		shitHappens("Could not write the settings file");
}

void SetupWin::cfgLoaded() {
	start();
	emit s_apply();
	fitWidths();		// the style of the settings just read
	emit s_prf_changed();
}

void SetupWin::cfgImport() {
	QString path = QFileDialog::getOpenFileName(this, tr("Import settings"),
		QString::fromLocal8Bit(conf.path.confDir.c_str()) + SLASH CFG_NAME,
		tr(CFG_FILTER));
	if (path.isEmpty()) return;
	if (!areSure("Take the settings out of this file?<br>"
		"What you have now is replaced, this machine included.")) return;
	if (!xconf_import(path)) {
		shitHappens("Could not read the settings file");
		return;
	}
	cfgLoaded();
}

void SetupWin::cfgReset() {
	if (!areSure("Back to the settings the emulator ships with?<br>"
		"Your own machines are left where they are.")) return;
	if (!xconf_reset()) {
		shitHappens("Could not read the settings");
		return;
	}
	cfgLoaded();
}

// A machine of the user's own: this one, under a name of its own, inheriting
// the machine it was made from. What was changed on that machine is already its
// own file, so it is left as it is.

// what the two buttons may do with the machine that is up

void SetupWin::updateMachineButtons() {
	ui.pbDelMachine->setEnabled(xm_is_users(conf.macId));
	ui.pbResetMachine->setEnabled(xm_is_changed(conf.macId));
}

void SetupWin::saveMachine() {
	const xMachine* mac = xm_find(conf.macId);
	if (!mac) return;
	bool ok = false;
	QString name = QInputDialog::getText(this, "Save as new machine",
		"Name for the new machine:", QLineEdit::Normal,
		QString::fromLocal8Bit(mac->name.c_str()) + " (mine)", &ok);
	name = name.trimmed();
	if (!ok || name.isEmpty()) return;
	std::string nam = std::string(name.toLocal8Bit().data());
	if (!xm_name_free(nam)) {
		shitHappens("A machine is already called that.<br>"
			"Give this one a name of its own.");
		return;
	}
	// what is saved is what the page shows, so the page goes in first
	std::string was = conf.macId;
	apply();
	if (conf.macId != was) return;		// that was a machine switch, not a save
	std::string id = xm_free_id(nam);
	if (!xm_save_as(id, nam)) {
		shitHappens("Could not write the machine file");
		return;
	}
	xm_choose(id);
	start();
	emit s_prf_changed();
}

void SetupWin::delMachine() {
	if (!xm_is_users(conf.macId)) {
		shitHappens("This machine ships with the emulator, so there is nothing to delete.<br>"
			"Restore machine drops what you changed on it.");
		return;
	}
	if (!areSure("Delete this machine?")) return;
	const xMachine* mac = xm_find(conf.macId);
	std::string id = conf.macId;
	std::string back = mac ? mac->parent : std::string();
	if (!xm_delete(id)) {
		shitHappens("Could not delete the machine file");
		return;
	}
	// a machine of the user's is gone for good: on to the one it was made from
	id = back;
	if (!xm_find(id)) id = xm_list().isEmpty() ? std::string() : xm_list().first().id;
	conf.macId.clear();		// nothing to save into a machine that is gone
	if (!id.empty()) xm_set(id);
	start();
	emit s_prf_changed();
}

void SetupWin::resetMachine() {
	if (!areSure(xm_is_users(conf.macId)
		? "Drop everything you changed on this machine, back to the one it was made from?"
		: "Take this machine as it ships, dropping everything you changed on it?")) return;
	xm_reset_over();
	start();
	emit s_prf_changed();
}

void SetupWin::romPreset() {
	const xMachine* mac = xm_find(conf.macId);
	if (!mac) return;
	roms = mac->roms;
	rsmodel->fill(&roms);
	fillRomSlots();
}

// THE SET AS THE MACHINE WEARS IT
//
// One row per slot, with the files to put in it. Where a file starts, how much
// of it is read and where it lands are in the window behind Expert settings.


static QStringList rom_files() {
	QDir dir(QString::fromLocal8Bit(conf.path.romDir.c_str()));
	QStringList res;
	QDirIterator it(dir.absolutePath(), QStringList() << "*.rom" << "*.bin",
			QDir::Files, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		it.next();
		res << dir.relativeFilePath(it.filePath());
	}
	res.sort(Qt::CaseInsensitive);
	return res;
}

void SetupWin::showRomFiles() {
	romWin->show();
	romWin->raise();
}

void SetupWin::fillRomSlots() {
	QLayoutItem* itm;
	while ((itm = ui.gridRomSlots->takeAt(0)) != NULL) {
		delete itm->widget();
		delete itm;
	}
	const xMachine* mac = xm_find(conf.macId);
	if (!mac) return;
	QStringList files = rom_files();
	QVector<int> from = xm_rom_cover(roms, mac->romBanks);
	int hw = conf.zx->hw->id;
	bool resets = false;
	int row = 0;
	for (int i = 0; i < mac->romBanks; i++) {
		xRomRole role = hw_rom_role(hw, i);
		QString name = QString("ROM %0").arg(i);
		if (role.name) name += QString(" (%0)").arg(role.name);
		QRadioButton* rb = NULL;
		if (role.res >= 0) {
			rb = new QRadioButton;
			rb->setToolTip(tr("Boot from this ROM"));
			rb->setEnabled(!romSlotFileName(i).isEmpty() || (from[i] >= 0));
			rb->setChecked(role.res == resTarget);
			resGroup->addButton(rb, role.res);
			ui.gridRomSlots->addWidget(rb, row, 0);
			resets = true;
		}
		addRomSlot(row, name, i, files, true, from[i], rb);
		row++;
	}
	// only a machine with a text mode draws from a font rom
	addRomSlot(row++, "Font", RSLOT_FONT, files, !mac->roms.fntFile.empty(), -1);
	// spare height under the rows, so they do not spread out
	ui.gridRomSlots->addItem(new QSpacerItem(20, 0, QSizePolicy::Minimum,
		QSizePolicy::Expanding), row, 0);
	ui.labResetHint->setVisible(resets);
	fillGsRom(files);
	fillDosRom();
	fillRomSummary();
}

// the page's one button: the files in the banks, in bank order

void SetupWin::fillRomSummary() {
	QStringList names;
	const xMachine* mac = xm_find(conf.macId);
	int banks = mac ? mac->romBanks : 4;
	for (int i = 0; i < banks; i++) {
		QString nam = romSlotFileName(i);
		if (!nam.isEmpty())
			names.append(QFileInfo(nam).completeBaseName());
	}
	QString txt = names.isEmpty() ? tr("(none)") : names.join(", ");
	ui.pbRomSet->setText(txt);		// the button cuts it short itself
	ui.pbRomSet->setToolTip(names.isEmpty() ? tr("The ROM files this machine runs") : names.join("\n"));
}

// the TR-DOS ROM, which the Beta Disk's window picks; on a machine whose
// firmware carries it, or with no Beta Disk, there is nothing to pick

void SetupWin::fillDosRom() {
	int bank = hw_reset_bank(conf.zx->hw->id, RES_DOS);
	bool bdi = (getRFIData(diskTypeBox) == DIF_BDI);
	dosRomBox->clear();
	if (bank < 0) {
		dosRomBox->addItem(tr("(in the firmware)"), QString());
	} else {
		dosRomBox->addItem(tr("(empty)"), QString());
		foreach(QString f, rom_files()) dosRomBox->addItem(f, f);
		QString cur = romSlotFileName(bank);
		if (!cur.isEmpty() && (dosRomBox->findData(cur) < 0)) dosRomBox->insertItem(1, cur, cur);
		dosRomBox->setCurrentIndex(qMax(0, dosRomBox->findData(cur)));
	}
	dosRomBox->setEnabled(bdi && (bank >= 0));
	dosRomBtn->setEnabled(bdi && (bank >= 0));
}

// the General Sound's ROM and the NeoGS flash, which its own window picks

void SetupWin::fillGsRom(const QStringList& files) {
	foreach(QComboBox* box, QList<QComboBox*>() << gsRomBox << ngsRomBox) {
		box->clear();
		box->addItem(tr("(empty)"), QString());
		foreach(QString f, files) box->addItem(f, f);
	}
	rom_box_pick(gsRomBox, romSlotFileName(RSLOT_GS));
	rom_box_pick(ngsRomBox, ngsPick);
}

// the file in a slot, or an empty string when there is none

QString SetupWin::romSlotFileName(int slot) {
	std::string res;
	if (slot == RSLOT_GS) {
		res = roms.gsFile;
	} else if (slot == RSLOT_NGS) {
		return ngsPick;
	} else if (slot == RSLOT_FONT) {
		res = roms.fntFile;
	} else {
		foreach(xRomFile rf, roms.roms) {
			if (rf.roffset == slot * 16) res = rf.name;
		}
	}
	return QString::fromUtf8(res.c_str());		// rom names are UTF-8
}

// boot: the slot's radio, which its name picks while it can be picked; else the name is its list's
void SetupWin::addRomSlot(int row, QString name, int slot, const QStringList& files, bool has, int from, QRadioButton* boot) {
	QLabel* lab = new QLabel(name);
	lab->setEnabled(has);
	QComboBox* box = new QComboBox;
	box->setMinimumWidth(200);
	box->setMaximumWidth(200);
	QString none = has ? tr("(empty)") : tr("(not fitted)");
	if (from >= 0) none = tr("(from ROM %0)").arg(from);
	box->addItem(none, QString());
	foreach(QString f, files) {
		box->addItem(f, f);
	}
	QString cur = romSlotFileName(slot);
	// a file that is missing, or one of the user's own from elsewhere
	if (!cur.isEmpty() && (box->findData(cur) < 0)) box->insertItem(1, cur, cur);
	box->setCurrentIndex(qMax(0, box->findData(cur)));
	box->setEnabled(has);
	connect(box, QOverload<int>::of(&QComboBox::activated), this, [=](int idx){
		romSlotPick(slot, box->itemData(idx).toString());
	});
	QToolButton* btn = new QToolButton;
	btn->setIcon(QIcon(":/images/fileopen.png"));
	btn->setToolTip(tr("Pick a ROM file"));
	btn->setEnabled(has);
	connect(btn, &QToolButton::released, this, [=](){ romSlotFile(box, slot); });
	label_for(lab, (boot && boot->isEnabled()) ? (QWidget*)boot : (QWidget*)box);
	ui.gridRomSlots->addWidget(lab, row, 1);
	ui.gridRomSlots->addWidget(box, row, 2);
	ui.gridRomSlots->addWidget(btn, row, 3);
}

// a file from anywhere, which is kept as the path it is

void SetupWin::romSlotFile(QComboBox* box, int slot) {
	QString dir = QString::fromLocal8Bit(conf.path.romDir.c_str());
	QString file = QFileDialog::getOpenFileName(this, tr("ROM file"), dir,
		tr("ROM images (*.rom *.bin);;All files (*)"));
	if (file.isEmpty()) return;
	QFile test(file);
	if (!test.open(QFile::ReadOnly)) {
		shitHappens(QString("Can't read that file:<br>%0")
			.arg(QDir::toNativeSeparators(file).toHtmlEscaped()).toUtf8().constData());
		return;
	}
	test.close();
	QString rel = QDir(dir).relativeFilePath(file);
	if (!rel.startsWith("..")) file = rel;	// under the rom directory
	if (box->findData(file) < 0) box->insertItem(1, file, file);
	box->setCurrentIndex(box->findData(file));
	romSlotPick(slot, file);
}

void SetupWin::romSlotPick(int slot, const QString& file) {
	std::string name = std::string(file.toUtf8().data());
	if (slot == RSLOT_GS) {
		roms.gsFile = name;
	} else if (slot == RSLOT_NGS) {
		ngsPick = file;
		return;			// not the romset's
	} else if (slot == RSLOT_FONT) {
		roms.fntFile = name;
	} else {
		xm_rom_set_file(roms, slot, name);
	}
	rsmodel->fill(&roms);
	fillRomSummary();
}

void SetupWin::addRom() {
	xRomFile f;
	f.name[0] = 0;
	f.foffset = 0;
	f.fsize = 0;
	f.roffset = 0;
	eidx = -1;
	rseditor->edit(f);
}

void SetupWin::delRom() {
	QModelIndexList qmil = ui.tvRomset->selectionModel()->selectedRows();
	int row = (qmil.size() > 0) ? qmil.first().row() : -1;
	if (row < 0) return;
	int sz = roms.roms.size();
	if (row < sz) {
		roms.roms.erase(roms.roms.begin() + row);
	} else if (row == sz) {
		roms.gsFile.clear();
	} else if (row == sz+1) {
		roms.fntFile.clear();
	}
	rsmodel->fill(&roms);
	fillRomSlots();
}

void SetupWin::editRom() {
	QModelIndexList qmil = ui.tvRomset->selectionModel()->selectedRows();
	int row = (qmil.size() > 0) ? qmil.first().row() : -1;
	if (row < 0) return;
	xRomFile f;
	f.foffset = 0;
	f.fsize = 0;
	f.roffset = 0;
	int sz = roms.roms.size();
	if (row < sz) {
		f = roms.roms[row];
	} else if (row == sz) {
		f.name = roms.gsFile;
	} else if (row == sz+1) {
		f.name = roms.fntFile;
	}
	eidx = row;
	rseditor->edit(f);
}

void SetupWin::setRom(xRomFile f) {
	int sz = roms.roms.size();
	if (eidx < 0) {
		roms.roms.push_back(f);
	} else if (eidx < sz) {
		roms.roms[eidx] = f;
	} else if (eidx == sz) {
		roms.gsFile = f.name;
	} else if (eidx == sz+1) {
		roms.fntFile = f.name;
	}
	rsmodel->fill(&roms);
	fillRomSlots();
}

// lists

void SetupWin::buildkeylist() {
	ui.keyMapBox->clear();
	for (const auto& km : keymap_list())
		ui.keyMapBox->addItem(km.first, km.second);
	setRFIndex(ui.keyMapBox, QString::fromUtf8(conf.kmapName.c_str()), 0);
}

// the core's mask bit, and the size it stands for (768K has no bit of its own)
struct xMemName {
	int mask;
	int size;
	const char* name;
};

static xMemName memNameTab[] = {
	{MEM_16K, MEM_16K, "16 KB"},
	{MEM_32K, MEM_32K, "32 KB"},
	{MEM_64K, MEM_64K, "64 KB"},
	{MEM_128K, MEM_128K, "128 KB"},
	{MEM_256K, MEM_256K, "256 KB"},
	{MEM_512K, MEM_512K, "512 KB"},
	{MEM_768K, MEM_512K + MEM_256K, "768 KB"},
	{MEM_1M, MEM_1M, "1024 KB"},
	{MEM_2M, MEM_2M, "2048 KB"},
	{MEM_4M, MEM_4M, "4096 KB"},
	{-1, 0, ""}
};

void SetupWin::setmszbox(int idx) {
	// the size that machine comes up with, not the one the last machine had:
	// switching machines loads the new one, it does not carry this page over
	int t = 0;
	int size = xm_ram_size(std::string(ui.machbox->itemData(idx).toString().toLocal8Bit().data()), &t);
	if (!t) return;
	ui.mszbox->clear();
	idx = 0;
	while (memNameTab[idx].mask > 0) {
		// a core whose 64K is 48K of RAM, the rest ROM: shown as what it has
		bool is48 = (memNameTab[idx].mask == MEM_64K) && (t & MEM_48K);
		if (t & memNameTab[idx].mask)
			ui.mszbox->addItem(is48 ? "48 KB" : memNameTab[idx].name, memNameTab[idx].size);
		idx++;
	}
	ui.mszbox->setCurrentIndex(ui.mszbox->findData(size));
}

// hobeta header crc = ((105 + 257 * std::accumulate(data, data + 15, 0u)) & 0xffff))

// video

// the label beside the slider says what the mode gives on this machine - the
// fixed sizes are the same everywhere, overscan is not, and a machine whose own
// screen modes need more border shows that much whatever the slider says
void SetupWin::chabsz() {
	Computer* comp = conf.zx;
	int mode = ui.bszsld->value();
	if (mode < comp->vid->brdmin) mode = comp->vid->brdmin;
	ui.bszlab->setText(borderText(mode));
}

// a border size's name and the frame it gives on this machine
QString SetupWin::borderText(int mode) {
	vCoord sz = vid_crop_size(conf.zx->vid, mode);
	return QString("%0 (%1×%2)").arg(brd_mode_name(mode)).arg(sz.x).arg(sz.y);
}

// the label beside the overclock slider says what the cpu ends up on - the board's
// own turbo is not in this number, the clock indicator is where the whole truth is
void SetupWin::chaspd() {
	Computer* comp = conf.zx;
	int pos = ui.sldSpeed->value();
	ui.labSpeed->setText(xspeed_name(pos) + QString(" (%0 MHz)").arg(comp->cpuFrq * xspeed_mult(pos), 0, 'g', 6));
}

void SetupWin::chaflc() {
	int val = ui.sldNoflic->value() * 2;
	ui.labNoflic->setText(val == 0 ? "0% (off)" : QString("%0%").arg(val));
}

void SetupWin::chapsg() {
	int psg = getRFIData(cbPsgCount);
	int on = (psg != PSG_NONE);
	int fm = (psg == PSG_TSFM);
	int split = on && (getRFIData(cbPsgStereo) != AY_MONO);
	// TurboSound FM is two YM2203 and nothing else; the others never are
	QStandardItemModel* types = qobject_cast<QStandardItemModel*>(cbPsgType->model());
	int fmrow = cbPsgType->findData(SND_YM2203);
	if (types && (fmrow >= 0)) types->item(fmrow)->setEnabled(fm);
	if (fm) setRFIndex(cbPsgType, SND_YM2203);
	else if (getRFIData(cbPsgType) == SND_YM2203) setRFIndex(cbPsgType, SND_YM);
	cbPsgType->setEnabled(on && !fm);
	cbPsgFrq->setEnabled(on);
	cbPsgStereo->setEnabled(on);
	// the clock is shown as the chip's own, and set as an AY's
	int mul = chip_frq_mul(getRFIData(cbPsgType));
	double ay = opt_get_psg_frq(cbPsgFrq) / psgFrqMul;	// 0: Auto
	bool moved = (mul != psgFrqMul);
	psgFrqMul = mul;
	double base = xcpu_frq_parse(ui.cbCpuFrq->currentText(), conf.zx->cpuFrq);
	QStringList txt(QString("Auto - %0").arg(base / 2 * mul, 0, 'g', 7));
	for (const auto& p : psgFrqTab)
		txt << QString("%0 - %1").arg(p.frq * mul, 0, 'g', 7).arg(p.name);
	for (int i = 0; i < txt.size(); i++) {
		if (cbPsgFrq->itemText(i) == txt[i]) continue;
		cbPsgFrq->setItemText(i, txt[i]);
		moved = true;
	}
	if (moved)		// renaming the current item wipes a figure typed in
		opt_set_psg_frq(cbPsgFrq, ay * mul);
	sldPsgSep->setEnabled(split);
	labPsgSep->setEnabled(split);
}

// the crash is a property of the snow, not a setting of its own: it says nothing
// while the snow is off, so it greys out with it and keeps what it was set to
void SetupWin::chasnow() {
	bool on = ui.cbSnow->isChecked();
	ui.cbSnowCrash->setEnabled(on);
	ui.nam_cbSnowCrash->setEnabled(on);
	ui.lab_cbSnowCrash->setEnabled(on);
}

void SetupWin::chasndlat() {
	ui.labSndLatency->setText(QString("%0 ms").arg(ui.sldSndLatency->value()));
}

void SetupWin::selsspath() {
	QString fpath = QFileDialog::getExistingDirectory(this,"Screenshots folder",QString::fromStdString(conf.scrShot.dir),QFileDialog::ShowDirsOnly);
	if (fpath!="") ui.pathle->setText(fpath);
}

// the item data keeps the file name
void SetupWin::fillPalettes() {
	ui.cbPalPreset->blockSignals(true);
	fillComboBox(ui.cbPalPreset, "palettes", QStringList() << "*.txt" << "*.pal", PAL_DEFAULT_NAME, conf.palette.c_str());
	for (int i = 1; i < ui.cbPalPreset->count(); i++)
		ui.cbPalPreset->setItemText(i, palette_title(ui.cbPalPreset->itemText(i)));
	ui.cbPalPreset->blockSignals(false);
	palButtons();
}

// Edit is for a palette in a file; Restore for the user's own copy of one that
// ships, saved or still waiting for Apply
void SetupWin::palButtons() {
	QString nam = getRFSData(ui.cbPalPreset);
	bool ships = !nam.isEmpty() && QFile::exists(xres_root("palettes") + "/" + nam);
	bool own = palPending.contains(nam) || QFile::exists(palette_own_file(nam));
	ui.tbPalEdit->setEnabled(!nam.isEmpty());
	ui.tbPalRestore->setVisible(ships && own);
}

void SetupWin::paledit() {
	QString str = getRFSData(ui.cbPalPreset);
	if (str.isEmpty()) return;			// the built-in one is no file
	editpal = palPending.contains(str) ? palPending.value(str) : loadColors(str.toStdString());
	while (editpal.size() < 16) editpal.append(Qt::black);
	paleditor->edit(&editpal);
}

void SetupWin::palchoosecol(QPoint p) {
}

// held until Apply, so Cancel drops it
void SetupWin::palstore() {
	palPending[getRFSData(ui.cbPalPreset)] = editpal.mid(0, 16);	// the editor is modal: still the one picked
	palButtons();
}

void SetupWin::palRestore() {
	QString nam = getRFSData(ui.cbPalPreset);
	if (nam.isEmpty() || !areSure("Drop your changes to this palette and take it as it ships?")) return;
	palPending.remove(nam);
	QFile::remove(palette_own_file(nam));
	if (nam == QString::fromStdString(conf.palette)) loadPalette();
	palButtons();
}

// what each player's pad is and what it stands for, as the window has it
void SetupWin::padSummary() {
	xGamepad* pad[2] = {conf.gpctrl->gpada, conf.gpctrl->gpadb};
	for (int i = 0; i < 2; i++) {
		QString dev = pad[i]->padId().title();
		if (pad[i]->isKeyboard()) {
			dev = pad_kbd_name(pad[i]->keyboard());
		} else if (dev.isEmpty()) {
			padSum[i]->setText(tr("None"));
			continue;
		} else if (!pad[i]->isOpened()) {
			dev += tr(" (not connected)");
		}
		padSum[i]->setText(QString("%0 - %1").arg(dev, pad_scheme_name(pad[i]->scheme())));
	}
}

void SetupWin::selectDbgFont() {
	bool ok;
	dbgfnt = QFontDialog::getFont(&ok, dbgfnt, this, "Select font", QFontDialog::DontUseNativeDialog);
	ui.leDbgFont->setText(QString("%0, %1 pt").arg(dbgfnt.family()).arg(dbgfnt.pointSize()));
	ui.leDbgFont->setFont(dbgfnt);
}

// debuga palette

void SetupWin::selectColor() {
	QToolButton* obj = (QToolButton*)sender();
	QString cn = obj->property("colorName").toString();
	QString dn = obj->property("defaultColor").toString();
	if (cn.isEmpty()) return;
	QColor col = QColorDialog::getColor(conf.pal[cn], this, "Select color", QColorDialog::DontUseNativeDialog);
	if (!col.isValid()) return;
	conf.pal[cn] = col;
	setToolButtonColor(obj, cn, dn);
}

void SetupWin::triggerColor() {
	QToolButton* obj = (QToolButton*)sender();
	QString cn = obj->property("colorName").toString();
	QString dn = obj->property("defaultColor").toString();
	if (cn.isEmpty()) return;
	if (dn.isEmpty()) {
		conf.pal.remove(cn);
	} else {
		QColor col(dn);
		if (col.isValid())
			conf.pal[cn] = col;
	}
	setToolButtonColor(obj, cn, dn);
}

// VIDEO RECORDING
//
// The group on the Video page holds what changes from one video to the next;
// how FFmpeg encodes it is in a pop-up of its own. The tooltips name the FFmpeg
// options a control sets, and the pop-up ends with the command lines in full.

static const int recBitrates[] = {128, 192, 256, 320, 0};

static QToolButton* rec_button(const char* icon, const QString& tip) {
	QToolButton* btn = new QToolButton;
	btn->setIcon(QIcon(icon));
	btn->setToolTip(tip);
	return btn;
}

// Folder as a disk, under Speed and rewind: whether a folder mounted as a card or a disk
// takes the machine's writes, and where a file the machine deletes there goes. Laid out
// as Speed and rewind is: a short list first, a mode in the keys' column
void SetupWin::buildFolders() {
	QGroupBox* box = new QGroupBox(tr("Folder as a disk"));
	gridFolders = new QGridLayout(box);
	gridFolders->setHorizontalSpacing(12);
	gridFolders->setVerticalSpacing(8);
	gridFolders->setColumnStretch(2, 1);
	cbFolderWrite = new QComboBox;
	cbFolderWrite->addItem(tr("R/O"), 0);
	cbFolderWrite->setItemData(0, tr("Read-only: the folder stays as it is, from Apply on"), Qt::ToolTipRole);
	cbFolderWrite->addItem(tr("R/W"), 1);
	cbFolderWrite->setItemData(1, tr("Read/write: the folder's files follow what\nthe machine writes, from the next Hard Reset"), Qt::ToolTipRole);
	cbFolderTrash = new QComboBox;
	cbFolderTrash->addItem(tr("Move to trash"), 1);
	cbFolderTrash->addItem(tr("Delete permanently"), 0);
	auto label = [](QComboBox* cb, const QString& name, const QString& tip) {
		QLabel* lab = new QLabel(name);
		lab->setBuddy(cb);
		cb->setToolTip(tip);
		lab->setToolTip(tip);
		return lab;
	};
	QLabel* note = new QLabel(tr("Whether a folder in a card slot or a drive takes what the machine saves"));
	QFont font = note->font();
	font.setItalic(true);
	note->setFont(font);
	gridFolders->addWidget(cbFolderWrite, 0, 0);
	gridFolders->addWidget(label(cbFolderWrite, tr("Folders"),
		tr("R/O: read-only, the folder stays as it is,\nfrom Apply on\nR/W: read/write, the folder's files follow\nwhat the machine writes, from the next Hard Reset")), 0, 1);
	gridFolders->addWidget(note, 0, 2);
	gridFolders->addWidget(label(cbFolderTrash, tr("Deleted files"),
		tr("Where a file the machine deletes in such\na folder goes. A file the trash will not\ntake stays in the folder")), 1, 1);
	gridFolders->addWidget(cbFolderTrash, 1, 2, Qt::AlignLeft);
	ui.verticalLayout_16->insertWidget(ui.verticalLayout_16->indexOf(ui.gbRewind) + 1, box);
}

// when the machine stops and goes on by itself: after Speed and rewind
void SetupWin::buildPause() {
	// the rows of the page: the switch, its name, what it does in italics
	QGroupBox* box = new QGroupBox(tr("Pause"));
	gridPause = new QGridLayout(box);
	gridPause->setHorizontalSpacing(12);
	gridPause->setVerticalSpacing(8);
	gridPause->setColumnStretch(2, 1);
	int r = 0;
	auto row = [this, &r](const QString& name, const QString& note, const QString& tip) {
		QCheckBox* cb = new QCheckBox;
		QLabel* lab = new QLabel(name);
		lab->setBuddy(cb);
		QLabel* what = new QLabel(note);
		QFont font = what->font();
		font.setItalic(true);
		what->setFont(font);
		for (QWidget* w : {(QWidget*)cb, (QWidget*)lab, (QWidget*)what})
			w->setToolTip(tip);
		gridPause->addWidget(cb, r, 0);
		gridPause->addWidget(lab, r, 1);
		gridPause->addWidget(what, r++, 2);
		return cb;
	};
	cbPauseInactive = row(tr("Pause when inactive"), tr("While another program has the focus"),
		tr("Stop the machine while another\nprogram has the focus"));
	cbPauseMenu = row(tr("Pause in menus"), tr("While a menu of this window is open"),
		tr("Stop the machine while a menu\nof this window is open"));
	cbPauseResume = row(tr("Resume by input"), tr("A key, a click or a pad ends your pause"),
		tr("Pause ends on a key, a click in the picture,\nthe on-screen keyboard or a pad"));
	ui.verticalLayout_16->insertWidget(ui.verticalLayout_16->indexOf(ui.gbRewind) + 1, box);
}

// The Saves page, after Emulation: where quick saves and autosaves go and how
// many of them stay. Its groups are laid out as Emulation's: a control, its
// name, and a line saying what it does.
static QLabel* savesHint(const QString& text) {
	QLabel* lab = new QLabel(text);
	QFont font = lab->font();
	font.setItalic(true);
	lab->setFont(font);
	return lab;
}

static QGridLayout* savesGroup(QVBoxLayout* page, const char* icon, const QString& title) {
	xIconGroup* box = new xIconGroup(icon, title);
	QGridLayout* grid = new QGridLayout(box);
	grid->setHorizontalSpacing(12);
	grid->setVerticalSpacing(8);
	grid->setColumnStretch(2, 1);
	page->addWidget(box);
	return grid;
}

// a row: the control, its name and what it does; the tip on all three
static void savesRow(QGridLayout* grid, QWidget* ctl, const QString& name, const QString& hint, const QString& tip) {
	int row = grid->rowCount();
	QLabel* lab = new QLabel(name);
	lab->setBuddy(ctl);
	ctl->setToolTip(tip);
	lab->setToolTip(tip);
	grid->addWidget(ctl, row, 0);
	grid->addWidget(lab, row, 1);
	if (!hint.isEmpty()) grid->addWidget(savesHint(hint), row, 2);
}

void SetupWin::buildSaves() {
	QWidget* page = new QWidget;
	QVBoxLayout* lay = new QVBoxLayout(page);

	QGridLayout* grid = gridSaves[3] = savesGroup(lay, ":/images/save-as.png", tr("Snapshots"));
	cbSnapEmbed = new QCheckBox;
	savesRow(grid, cbSnapEmbed, tr("Embed media"), tr("Off: only links to the files"),
		tr("The tape and the disks go into a snapshot\nsaved with File > Save, as they were;\nuntil set here, the first one asks"));
	connect(cbSnapEmbed, &QCheckBox::toggled, this, [this]() { snapTouched = true; });
	cbSnapLoad = new QCheckBox;
	savesRow(grid, cbSnapLoad, tr("Load media"), tr("Off: the tape and drives stay as they are"),
		tr("Opening a snapshot puts its tape and disks in;\na quick load always does"));

	grid = gridSaves[0] = savesGroup(lay, ":/images/floppy.png", tr("Quick saves"));
	cbSavePerGame = new QCheckBox;
	savesRow(grid, cbSavePerGame, tr("Saves per game"), tr("Each game keeps its own saves"),
		tr("On: a folder for each game opened\nOff: one for each machine"));
	sbQuickKeep = new QSpinBox;
	sbQuickKeep->setRange(1, 99);
	savesRow(grid, sbQuickKeep, tr("Quick saves kept"), tr("The oldest goes when a new one is past it"),
		tr("Quick saves each folder keeps"));
	cbSaveEmbed = new QCheckBox;
	savesRow(grid, cbSaveEmbed, tr("Embed media"), tr("Off: only links to the files"),
		tr("The tape and the disks go into a quick save\nor an autosave, as they were;\nit loads with the files gone"));

	grid = gridSaves[1] = savesGroup(lay, ":/images/grp-auto.png", tr("Autosaves"));
	cbExitSave = new QCheckBox;
	savesRow(grid, cbExitSave, tr("Autosave on exit"), tr("File > Resume Last Session goes back to it"),
		tr("The machine saved as the emulator closes"));
	sbExitKeep = new QSpinBox;
	sbExitKeep->setRange(1, 99);
	savesRow(grid, sbExitKeep, tr("Exit autosaves kept"), tr("In the game's folder, or the machine's"),
		tr("Exit autosaves each folder keeps"));
	sbTimedMin = new QSpinBox;
	sbTimedMin->setRange(0, 60);
	sbTimedMin->setSpecialValueText(tr("Off"));
	savesRow(grid, sbTimedMin, tr("Autosave every"), tr("Minutes, counted while the machine runs"),
		tr("An autosave every so many minutes;\na pause or the Debugger holds the count"));
	sbTimedKeep = new QSpinBox;
	sbTimedKeep->setRange(1, 99);
	savesRow(grid, sbTimedKeep, tr("Timed autosaves kept"), tr("A ring of their own, beside the exit ones"),
		tr("Timed autosaves each folder keeps"));

	grid = gridSaves[2] = savesGroup(lay, ":/images/fileopen.png", tr("Folder and clean-up"));
	leSaveDir = new QLineEdit;
	// empty: the config folder's, named in words as the log's folder is
	leSaveDir->setPlaceholderText(tr("saves, in the config folder"));
	leSaveDir->setToolTip(QDir::toNativeSeparators(QDir(QString::fromLocal8Bit(conf.path.confDir.c_str())).filePath("saves")));
	QToolButton* pick = new QToolButton;
	pick->setIcon(QIcon(":/images/fileopen.png"));
	pick->setToolTip(tr("Pick the folder"));
	connect(pick, &QToolButton::clicked, this, [this]() {
		QString dir = QFileDialog::getExistingDirectory(this, tr("Saves folder"),
			leSaveDir->text().isEmpty() ? leSaveDir->toolTip() : leSaveDir->text());
		if (!dir.isEmpty()) leSaveDir->setText(QDir::toNativeSeparators(dir));
	});
	{
		int row = grid->rowCount();
		QLabel* lab = new QLabel(tr("Saves folder"));
		lab->setBuddy(leSaveDir);
		lab->setToolTip(tr("Where the saves go. Empty: the config folder"));
		QHBoxLayout* dirRow = new QHBoxLayout;
		dirRow->addWidget(leSaveDir, 1);
		dirRow->addWidget(pick);
		grid->addWidget(lab, row, 1);
		grid->addLayout(dirRow, row, 2);
	}
	sbCleanDays = new QSpinBox;
	sbCleanDays->setRange(0, 999);
	sbCleanDays->setSpecialValueText(tr("Off"));
	savesRow(grid, sbCleanDays, tr("Delete saves older than"), tr("Days, at start, in every game"),
		tr("Old saves deleted at each start;\nFile > Save History does it by hand"));
	cbKeepOne = new QCheckBox;
	savesRow(grid, cbKeepOne, tr("Keep at least one save"), tr("A game's newest save stays, however old"),
		tr("Here and in File > Save History"));

	lay->addStretch(1);
	int at = ui.pages->indexOf(ui.tabEmulation) + 1;
	ui.pages->insertWidget(at, page);
	QListWidgetItem* item = new QListWidgetItem(QIcon(":/images/floppy.png"), tr("Saves"));
	ui.pageList->insertItem(at, item);
}

void SetupWin::fillSaves() {
	cbSavePerGame->setChecked(conf.saves.perGame);
	sbQuickKeep->setValue(conf.saves.quickKeep);
	cbSaveEmbed->setChecked(conf.saves.carry);
	cbSnapEmbed->setChecked(conf.saves.snapMedia != SNAP_MEDIA_LINKS);
	snapTouched = false;
	cbSnapLoad->setChecked(conf.saves.snapLoad);
	cbExitSave->setChecked(conf.saves.exitOn);
	sbExitKeep->setValue(conf.saves.exitKeep);
	sbTimedMin->setValue(conf.saves.timedOn ? conf.saves.timedMin : 0);
	sbTimedKeep->setValue(conf.saves.timedKeep);
	sbCleanDays->setValue(conf.saves.cleanOn ? conf.saves.cleanDays : 0);
	cbKeepOne->setChecked(conf.saves.keepOne);
	leSaveDir->setText(QDir::toNativeSeparators(conf.saves.dir));
}

void SetupWin::applySaves() {
	conf.saves.perGame = cbSavePerGame->isChecked() ? 1 : 0;
	conf.saves.quickKeep = sbQuickKeep->value();
	conf.saves.carry = cbSaveEmbed->isChecked() ? 1 : 0;
	// left as it was, the first snapshot still asks
	if (snapTouched) conf.saves.snapMedia = cbSnapEmbed->isChecked() ? SNAP_MEDIA_CARRY : SNAP_MEDIA_LINKS;
	conf.saves.snapLoad = cbSnapLoad->isChecked() ? 1 : 0;
	conf.saves.exitOn = cbExitSave->isChecked() ? 1 : 0;
	conf.saves.exitKeep = sbExitKeep->value();
	// off keeps the number it had, for when it is on again
	conf.saves.timedOn = (sbTimedMin->value() > 0) ? 1 : 0;
	if (conf.saves.timedOn) conf.saves.timedMin = sbTimedMin->value();
	conf.saves.timedKeep = sbTimedKeep->value();
	conf.saves.cleanOn = (sbCleanDays->value() > 0) ? 1 : 0;
	if (conf.saves.cleanOn) conf.saves.cleanDays = sbCleanDays->value();
	conf.saves.keepOne = cbKeepOne->isChecked() ? 1 : 0;
	conf.saves.dir = QDir::fromNativeSeparators(leSaveDir->text().trimmed());
}

void SetupWin::buildRecording() {
	QGridLayout* grid = new QGridLayout;
	ui.verticalLayout_rec->addLayout(grid);

	// the picture: three lists and the button after them
	cbRecSrc = src_box(tr("RAW frame: the machine's picture, no shader or indicators\nScreen: the window as shown"));
	cbRecScale = scale_box(VREC_SCALE_MAX, "-vf scale=W:H:flags=neighbor");
	cbRecFps = new QComboBox;
	cbRecFps->addItem(tr("Real FPS"), VREC_FPS_MACHINE);
	cbRecFps->addItem(tr("50 FPS"), VREC_FPS_50);
	cbRecFps->setToolTip(tr("-framerate: the machine's own, or 50 with the sound fitted to it"));
	foreach(QComboBox* box, QList<QComboBox*>() << cbRecSrc << cbRecScale << cbRecFps) {
		connect(box, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SetupWin::showRecSize);
	}
	connect(cbRecSrc, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SetupWin::recEnables);
	labRecSrc = new QLabel(tr("Source"));
	grid->addWidget(labRecSrc, 0, 0);
	grid->addWidget(cbRecSrc, 0, 1);
	grid->addWidget(cbRecScale, 0, 2);
	grid->addWidget(cbRecFps, 0, 3);

	// where to and where from, ending under the last list
	leRecDir = new QLineEdit;
	leRecDir->setToolTip(tr("Folder videos are saved in"));
	QToolButton* btn = rec_button(":/images/fileopen.png", tr("Pick the folder"));
	connect(btn, &QToolButton::released, this, [this]() {
		QString dir = leRecDir->text();
		if (dir.isEmpty()) dir = leRecDir->placeholderText();
		dir = QFileDialog::getExistingDirectory(this, tr("Videos folder"), dir, QFileDialog::ShowDirsOnly);
		if (!dir.isEmpty()) leRecDir->setText(QDir::toNativeSeparators(dir));
	});
	labRecOut = new QLabel(tr("Output"));
	grid->addWidget(labRecOut, 1, 0);
	grid->addWidget(fieldPair(leRecDir, btn, true), 1, 1, 1, 3);

	leRecFfmpeg = new QLineEdit;
	btn = rec_button(":/images/fileopen.png", tr("Pick the program"));
	connect(btn, &QToolButton::released, this, [this]() {
		QString path = QFileDialog::getOpenFileName(this, tr("FFmpeg"), QFileInfo(leRecFfmpeg->text()).path(),
#ifdef _WIN32
			"ffmpeg.exe (ffmpeg.exe);;Programs (*.exe)"
#else
			QString()
#endif
		);
		if (path.isEmpty()) return;
		leRecFfmpeg->setText(QDir::toNativeSeparators(path));
		showFfmpeg();
	});
	connect(leRecFfmpeg, &QLineEdit::editingFinished, this, &SetupWin::showFfmpeg);
	// shown while there is no FFmpeg to run
	btnRecGet = rec_button(":/images/arrow-down.png", tr("Get FFmpeg"));
	connect(btnRecGet, &QToolButton::released, this, [this]() {
		if (!ffmpeg_get(this)) return;
		leRecFfmpeg->clear();		// as the settings now are: Auto, which finds it
		showFfmpeg();
	});
	QWidget* ffBtns = new QWidget;
	QHBoxLayout* ffLay = new QHBoxLayout(ffBtns);
	ffLay->setContentsMargins(0, 0, 0, 0);
	ffLay->addWidget(btnRecGet);
	ffLay->addWidget(btn);
	labRecFfm = new QLabel(tr("FFmpeg"));
	grid->addWidget(labRecFfm, 2, 0);
	grid->addWidget(fieldPair(leRecFfmpeg, ffBtns, true), 2, 1, 1, 3);

	// what the video comes out as, after where it goes
	labRecSize = new QLabel;
	labRecSize->setToolTip(tr("The video's frame and rate, with the border and scale set on this page"));
	grid->addWidget(labRecSize, 1, 4, Qt::AlignCenter);
	labRecFmt = new QLabel;
	grid->addWidget(labRecFmt, 2, 4, Qt::AlignCenter);
	// the lists share the width up to the button, which keeps to the right edge
	for (int i = 1; i <= 3; i++)
		grid->setColumnStretch(i, 1);
	// the page's own settings move it too
	connect(ui.bszsld, &QSlider::valueChanged, this, &SetupWin::showRecSize);
	connect(ui.cbScale, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SetupWin::showRecSize);
	connect(ui.cbFullscreen, &QCheckBox::toggled, this, &SetupWin::showRecSize);

	// the rest, in a pop-up laid out as Machine: advanced settings
	cbRecCodec = new QComboBox;
	// the list is filled by showFfmpeg(), which knows what the program can do
	cbRecCodec->setToolTip(tr("-c:v x264, x265, SVT-AV1, NVENC, AMF, QSV or FFV1; AMF, QSV and AV1 write 4:2:0"));
	cbRecChroma = new QComboBox;
	cbRecChroma->addItem(tr("Auto"), VREC_CH_AUTO);
	cbRecChroma->addItem("4:2:0", VREC_CH_420);
	cbRecChroma->addItem("4:4:4", VREC_CH_444);
	cbRecChroma->setToolTip(tr("Auto: 4:2:0, and 4:4:4 at an odd scale or for the screen"));
	cbRecBox = new QComboBox;
	cbRecBox->addItem("MP4", VREC_MP4);
	cbRecBox->addItem("MKV", VREC_MKV);
	sbRecCrf = new QSpinBox;
	sbRecCrf->setRange(0, 51);
	sbRecCrf->setToolTip(tr("-crf, NVENC -qp: lower is better, 18 looks lossless"));
	cbRecPreset = new QComboBox;
	for (int i = 0; vrecPresets[i]; i++)
		cbRecPreset->addItem(vrecPresets[i], vrecPresets[i]);
	cbRecPreset->setToolTip(tr("-preset, AMF -quality: slower packs smaller,\nbut can hold the machine up"));
	cbRecAbr = new QComboBox;
	for (int i = 0; recBitrates[i]; i++)
		cbRecAbr->addItem(QString("%0 kbps").arg(recBitrates[i]), recBitrates[i]);
	cbRecAbr->setToolTip("-b:a");
	cbRecAcodec = new QComboBox;
	cbRecAcodec->addItem(tr("Auto"), VREC_AUDIO_AUTO);
	cbRecAcodec->addItem("AAC", VREC_AAC);
	cbRecAcodec->addItem("Opus", VREC_OPUS);
	cbRecAcodec->setToolTip(tr("-c:a; Auto is AAC in MP4, Opus in MKV"));
	leRecExtra = new QLineEdit;
	leRecExtra->setToolTip(tr("Added to the encoder's options, as typed"));
	// empty, each shows what it would stand in for
	leRecVOver = new QLineEdit;
	leRecVOver->setToolTip(tr("In place of the picture's options, from -vf on"));
	leRecSOver = new QLineEdit;
	leRecSOver->setToolTip(tr("In place of the sound's options when it is put in"));
	cbRec60 = new QCheckBox;
	cbRecPitch = new QCheckBox;
	leRecName = new QLineEdit;
	leRecName->setPlaceholderText(VREC_NAME_DEF);
	leRecName->setToolTip(name_tip(QString()));
	teRecCmd = new QTextEdit;
	teRecCmd->setAcceptRichText(false);
	teRecCmd->setReadOnly(true);
	teRecCmd->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	// a part to a line, broken again only where it does not fit, and the box
	// as tall as what is in it
	teRecCmd->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	teRecCmd->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	teRecCmd->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	// a QTextEdit, not a plain one: that one lays out only what is in view, so
	// the lines it counts under the edge are short of the wrapped ones
	connect(teRecCmd->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this](const QSizeF& sz) {
		teRecCmd->setFixedHeight(int(sz.height() + 0.5) + 2 * teRecCmd->frameWidth());
	});
	xOptSheet adv;
	adv.group(tr("Video"), 0, ":/images/video.png");
	adv.row(tr("Container"), cbRecBox);
	adv.row(tr("Codec"), cbRecCodec);
	adv.row(tr("Preset"), cbRecPreset);
	adv.row(tr("Quality (CRF)"), sbRecCrf);
	adv.row(tr("Color"), cbRecChroma);
	// one piece, so the frame is no wider than its lists
	cbRec60->setText(tr("Blend up to 60 fps"));
	adv.wide(cbRec60);
	adv.group(tr("Sound"), 1, ":/images/speaker.png");
	QWidget* sndBox = new QWidget;
	QHBoxLayout* sndLay = new QHBoxLayout(sndBox);
	sndLay->setContentsMargins(0, 0, 0, 0);
	sndLay->addWidget(cbRecAcodec);
	sndLay->addWidget(cbRecAbr);
	adv.row(tr("Codec"), sndBox);
	cbRecPitch->setText(tr("Keep pitch at 50 FPS"));
	adv.wide(cbRecPitch);
	// the frame takes the column's width from Sound, and neither the template
	// nor what it gives asks for more: a longer name must not move the frames
	adv.group(tr("File name template"), 1, ":/images/grp-filename.png");
	leRecName->setMinimumWidth(0);
	leRecName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	adv.wide(leRecName);
	labRecName = new xElideLabel;
	QFont nfnt = labRecName->font();
	nfnt.setItalic(true);
	labRecName->setFont(nfnt);
	labRecName->setAlignment(Qt::AlignCenter);
	adv.wide(labRecName);
	// Across the sheet, the start and the stop side by side, each condition
	// right after its switch. Code in ram serves a plain 48K/128K; a firmware
	// that runs from ram, as Scorpion's does, needs the program's own address.
	adv.group(tr("Auto recording"), -1, ":/images/grp-auto.png");
	cbRecStart = new QCheckBox(tr("Autostart at"));
	cbRecStart->setToolTip(tr("Armed once a reset; switched on here, from the next one"));
	cbRecStartAt = new QComboBox;
	cbRecStartAt->setToolTip(tr("Command line: --video-autostart ram | ==#6000"));
	leRecStartAt = new QLineEdit;
	cbRecStop = new QCheckBox(tr("Autostop at"));
	cbRecStop->setToolTip(tr("Ends any recording, one started by hand too"));
	cbRecStopAt = new QComboBox;
	cbRecStopAt->setToolTip(tr("Command line: --video-autostop reset | ==#0000"));
	leRecStopAt = new QLineEdit;
	QToolButton* hlp = new QToolButton;
	hlp->setText("?");
	hlp->setToolTip(tr("Command line"));
	// a child of the pop-up, which is modal: one of the setup window would be
	// shut out until the pop-up closes
	connect(hlp, &QToolButton::clicked, this, [this, hlp]() {
		help_window(hlp->window(), &recHelpWin, ":/res/help/rec-cli.html", "Recording: Command Line");
	});
	QWidget* autoRow = new QWidget;
	QHBoxLayout* autoLay = new QHBoxLayout(autoRow);
	autoLay->setContentsMargins(0, 0, 0, 0);
	autoLay->addWidget(recCondBox(cbRecStart, cbRecStartAt, leRecStartAt, tr("Code in RAM")), 1);
	autoLay->addWidget(recCondBox(cbRecStop, cbRecStopAt, leRecStopAt, tr("Reset")), 1);
	autoLay->addWidget(hlp);
	adv.wide(autoRow);
	adv.group("FFmpeg", -1, ":/images/grp-ffmpeg.png");
	adv.row(tr("Extra options"), leRecExtra);
	adv.row(tr("Video override"), leRecVOver);
	adv.row(tr("Sound override"), leRecSOver);
	adv.wide(teRecCmd);
	// the checkboxes lose their tooltips to the sheet, their names keep one
	cbRec60->setToolTip("framerate=fps=60");
	cbRecPitch->setToolTip(tr("-af atempo: sped up in time, not in pitch"));
	connect(cbRecCodec, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
		bool lossy = getRFIData(cbRecCodec) != VREC_FFV1;
		// FFV1 goes into MKV only, and the choice made for the others is kept
		if (!lossy && cbRecBox->isEnabled()) recBoxKeep = getRFIData(cbRecBox);
		setRFIndex(cbRecBox, lossy ? recBoxKeep : VREC_MKV);
		recEnables();
	});
	foreach(QComboBox* box, QList<QComboBox*>() << cbRecBox << cbRecCodec << cbRecPreset << cbRecAcodec << cbRecAbr << cbRecChroma)
		connect(box, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SetupWin::showRecCmd);
	connect(cbRecChroma, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SetupWin::recEnables);
	connect(sbRecCrf, QOverload<int>::of(&QSpinBox::valueChanged), this, &SetupWin::showRecCmd);
	connect(leRecExtra, &QLineEdit::textChanged, this, &SetupWin::showRecCmd);
	foreach(QLineEdit* led, QList<QLineEdit*>() << leRecVOver << leRecSOver) {
		connect(led, &QLineEdit::textChanged, this, &SetupWin::recEnables);
		connect(led, &QLineEdit::textChanged, this, &SetupWin::showRecCmd);
	}
	connect(cbRecPitch, &QCheckBox::toggled, this, &SetupWin::showRecCmd);
	connect(cbRecFps, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SetupWin::recEnables);
	connect(leRecName, &QLineEdit::textChanged, this, &SetupWin::showRecCmd);
	connect(cbRec60, &QCheckBox::toggled, this, &SetupWin::showRecSize);
	QDialog* win = popOut(adv.body, "Recording: Advanced Settings");
	xSideButton* adb = new xSideButton;
	adb->setIcon(QIcon(":/images/settings.png"));
	adb->setText(tr("Advanced settings"));
	adb->setToolTip(tr("Codec, quality, sound, and the FFmpeg command"));
	// as wide as Machine: advanced settings, the other pop-up of this size
	connect(adb, &QPushButton::released, win, [this, win]() {
		win->adjustSize();
		int wid = advWin->sizeHint().width();
		if (win->width() < wid) win->resize(wid, win->height());
		win->show();
		win->raise();
		// the command is wrapped to its box only once shown, and may take a line more
		QTimer::singleShot(0, win, [win]() {win->adjustSize();});
	});
	// as wide as the Machine page's buttons: a box of their width (fitWidths), which
	// the style sheet's min-width on a push button cannot override
	QWidget* adbox = new QWidget;
	btnRecAdv = adb;
	boxRecAdv = adbox;
	QVBoxLayout* adlay = new QVBoxLayout(adbox);
	adlay->setContentsMargins(0, 0, 0, 0);
	adlay->addWidget(adb);
	grid->addWidget(adbox, 0, 4);
}

// The side fill's settings, in the order of the pop-up's rows
static int* fill_val(int i) {
	int* vals[5] = {&conf.vid.fillBlur, &conf.vid.fillDim, &conf.vid.fillFade, &conf.vid.fillEdge, &conf.vid.fillStretch};
	return vals[i];
}

// The side fill's strength: a name and a slider each, the ranges being short and
// fixed. Every move goes to conf and the window at once, so it can be judged.
void SetupWin::makeFillPopup() {
	static const struct {const char* name; const char* tip; int min; int max; int step; int tick; int def;} rows[5] = {
		{"Blur", "How soft the fill is, from crisp to a wide glow", SFILL_BLUR_MIN, SFILL_BLUR_MAX, 1, 1, SFILL_BLUR_DEF},
		{"Brightness", "How bright the fill is beside the picture", SFILL_DIM_MIN, 100, 5, 10, SFILL_DIM_DEF},
		{"Fade", "How much darker it gets towards the screen's edge", 0, 100, 5, 10, SFILL_FADE_DEF},
		{"Edge", "How gently the picture runs into the fill", 0, SFILL_EDGE_MAX, 1, 4, SFILL_EDGE_DEF},
		{"Stretch", "Mirror: the reflection drawn out towards the screen's edge", 0, 100, 5, 10, SFILL_STRETCH_DEF},
	};
	fillPop = new QFrame(this, Qt::Popup);
	fillPop->setFrameShape(QFrame::StyledPanel);
	fillPop->setAutoFillBackground(true);
	QGridLayout* lay = new QGridLayout(fillPop);
	for (int i = 0; i < 5; i++) {
		QLabel* lab = new QLabel(tr(rows[i].name));
		lab->setToolTip(tr(rows[i].tip));
		xSlider* sld = new xSlider;
		sld->setOrientation(Qt::Horizontal);
		sld->setRange(rows[i].min, rows[i].max);
		sld->setSingleStep(rows[i].step);
		sld->setPageStep(rows[i].tick);
		sld->setTickPosition(QSlider::TicksBelow);
		sld->setTickInterval(rows[i].tick);
		sld->setMinimumWidth(200);
		sld->setToolTip(lab->toolTip());
		lay->addWidget(lab, i, 0);
		lay->addWidget(sld, i, 1);
		sldFill[i] = sld;
		connect(sld, &QSlider::valueChanged, this, [this, i](int val) {
			*fill_val(i) = val;
			emit s_fill();
		});
	}
	QPushButton* def = new QPushButton(tr("Defaults"));
	def->setObjectName("fillDefaults");
	def->setToolTip(tr("Every slider back to where it starts"));
	lay->addWidget(def, 5, 1, Qt::AlignRight);
	connect(def, &QPushButton::released, this, [this]() {
		for (int i = 0; i < 5; i++)
			sldFill[i]->setValue(rows[i].def);
	});
	connect(ui.tbSideFill, &QToolButton::released, this, [this]() {
		fillPop->adjustSize();
		fillPop->move(ui.tbSideFill->mapToGlobal(QPoint(0, ui.tbSideFill->height())));
		fillPop->show();
	});
	connect(ui.cbSideFill, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
		fillSet(getRFIData(ui.cbSideFill));
		ui.tbSideFill->setEnabled(conf.vid.gl && (conf.vid.sideFill != SFILL_BLACK));
		sldFill[4]->setEnabled(conf.vid.sideFill == SFILL_MIRROR);
		emit s_fill();
	});
}

// The plain picture has black only: the border first or not is taken, the fill
// stays for a run with OpenGL.
void SetupWin::fillSet(int item) {
	if (conf.vid.gl) {
		sfill_set(item);
	} else {
		conf.vid.fillBorder = (item & SFILL_BORDER) ? 1 : 0;
	}
}

void SetupWin::fillToUi() {
	QStandardItemModel* mdl = qobject_cast<QStandardItemModel*>(ui.cbSideFill->model());
	for (int i = 0; mdl && (i < ui.cbSideFill->count()); i++) {
		QVariant dat = ui.cbSideFill->itemData(i);
		if (dat.isValid() && ((dat.toInt() & ~SFILL_BORDER) != SFILL_BLACK))
			mdl->item(i)->setEnabled(conf.vid.gl);
	}
	ui.cbSideFill->blockSignals(true);
	setRFIndex(ui.cbSideFill, conf.vid.gl ? sfill_item() : (sfill_item() & SFILL_BORDER));
	ui.cbSideFill->blockSignals(false);
	ui.tbSideFill->setEnabled(conf.vid.gl && (conf.vid.sideFill != SFILL_BLACK));
	sldFill[4]->setEnabled(conf.vid.sideFill == SFILL_MIRROR);
	for (int i = 0; i < 5; i++) {
		sldFill[i]->blockSignals(true);
		sldFill[i]->setValue(*fill_val(i));
		sldFill[i]->blockSignals(false);
	}
}

void SetupWin::fillTake() {
	fillWas[0] = conf.vid.sideFill;
	for (int i = 0; i < 5; i++)
		fillWas[i + 1] = *fill_val(i);
	fillWas[6] = conf.vid.fillBorder;
}

void SetupWin::fillPut() {
	bool same = (conf.vid.sideFill == fillWas[0]) && (conf.vid.fillBorder == unsigned(fillWas[6]));
	for (int i = 0; i < 5; i++)
		same = same && (*fill_val(i) == fillWas[i + 1]);
	if (same) return;
	conf.vid.sideFill = fillWas[0];
	conf.vid.fillBorder = fillWas[6] ? 1 : 0;
	for (int i = 0; i < 5; i++)
		*fill_val(i) = fillWas[i + 1];
	emit s_fill();
}

// the first column of the Video page's groups, one width for all of them
void SetupWin::alignVideoLabels() {
	QList<QLabel*> col = QList<QLabel*>() << ui.label_3 << ui.labPalPreset << ui.labShader << ui.labSideFill
		<< ui.label_12 << ui.antiflickerModeLabel << ui.label_13 << ui.label_5
		<< labRecSrc << labRecOut << labRecFfm;
	// Picture sets its column in the .ui, a label may be wider in a bigger font
	int wid = ui.gridLayout_2->columnMinimumWidth(0);
	foreach(QLabel* lab, col) {
		lab->setMinimumWidth(0);
		lab->ensurePolished();
		wid = qMax(wid, lab->sizeHint().width());
	}
	foreach(QLabel* lab, col)
		lab->setMinimumWidth(wid);
}

// where the picture goes in the main window with this page's settings
vidFit SetupWin::pageFit() {
	int fill = getRFIData(ui.cbSideFill);
	return vid_fit(conf.zx->vid, ui.bszsld->value(), vid_room(ui.cbFullscreen->isChecked()), getRFIData(ui.cbScale),
		ui.cbKeepRatio->isChecked(), (fill & SFILL_BORDER) ? 1 : 0, conf.zx->hw->xscale);
}

// the pictures handed to FFmpeg and the video made of them, as the page is set now
void SetupWin::recSizes(QSize* in, QSize* out) {
	vCoord sze = vid_crop_size(conf.zx->vid, ui.bszsld->value());
	if (getRFIData(cbRecSrc) == VREC_SRC_SCREEN) {
		vidFit f = pageFit();
		qreal r = main_dpr(this);
		*in = QSize(int(f.roomW * r + 0.5) & ~1, int(f.roomH * r + 0.5) & ~1);
		*out = *in;
	} else {
		int n = getRFIData(cbRecScale);
		*in = QSize(sze.x * 2, sze.y);
		*out = QSize((sze.x * n) & ~1, (sze.y * n) & ~1);
	}
}

void SetupWin::showRecSize() {
	QSize in;
	QSize out;
	recSizes(&in, &out);
	QString fps = "50";
	if (cbRec60->isChecked()) {
		fps = "60";
	} else if (getRFIData(cbRecFps) == VREC_FPS_MACHINE) {
		fps = QString::number(1e9 / conf.zx->vid->nsPerFrame, 'f', 2);
	}
	labRecSize->setText(QString("%0%1%2 @ %3 fps").arg(out.width()).arg(QChar(0xd7)).arg(out.height()).arg(fps));
	showRecCmd();
}

// What a screenshot comes out as with this page's settings. SCR and Hobeta are
// the screen's memory, and the rest of the line has nothing to say about them.
void SetupWin::showShotSize() {
	QString fmt = getRFSData(ui.ssfbox);
	bool mem = (fmt == "scr") || (fmt == "hobeta");
	bool screen = (getRFIData(cbShotSrc) == VREC_SRC_SCREEN);
	cbShotSrc->setEnabled(!mem);
	cbShotScale->setEnabled(!mem && !screen);
	ui.ssNoBord->setEnabled(!mem);
	if (mem) {
		labShotSize->setText(QString("%0, %1 bytes").arg(ui.ssfbox->currentText()).arg(SCR_SIZE + ((fmt == "scr") ? 0 : HOB_HEAD_SIZE)));
		labShotSize->setToolTip(tr("The screen's memory"));
		return;
	}
	Video* vid = conf.zx->vid;
	bool nob = ui.ssNoBord->isChecked();
	QSize out;
	if (!screen) {
		vCoord sze = vid_crop_size(vid, ui.bszsld->value());
		QSize pic(nob ? vid->scrn.x : sze.x, nob ? vid->scrn.y : sze.y);
		out = pic * getRFIData(cbShotScale);
		labShotSize->setToolTip(tr("A hi-res picture comes out twice as big,\nwith all its dots"));
	} else {
		// as MainWin::grabPicture() reads it
		vidFit f = pageFit();
		qreal r = main_dpr(this);
		int fill = getRFIData(ui.cbSideFill);
		// a fill drawn beside the picture, as MainWin::fillWanted() has it
		bool beside = conf.vid.gl && ((fill & ~SFILL_BORDER) != SFILL_BLACK)
			&& ((f.x > 0) || (f.y > 0) || (f.x + f.w < f.roomW) || (f.y + f.h < f.roomH));
		if (nob) {
			double pw = int(f.w * r + 0.5);
			double ph = int(f.h * r + 0.5);
			out = QSize(int(vid->scrn.x * pw / f.crop.x + 0.5), int(vid->scrn.y * ph / f.crop.y + 0.5));
		} else if (beside) {
			out = QSize(int(f.roomW * r + 0.5), int(f.roomH * r + 0.5));
		} else {
			out = QSize(shown_span(f.x, f.w, f.roomW, r), shown_span(f.y, f.h, f.roomH, r));
		}
		labShotSize->setToolTip(tr("As big as the picture in the window,\nwith the side fill beside it"));
	}
	labShotSize->setText(QString("%0, %1%2%3").arg(ui.ssfbox->currentText()).arg(out.width()).arg(QChar(0xd7)).arg(out.height()));
}

// a control is live only while it changes what is recorded: an override stands
// in for what the ones it covers make, and the pitch is kept or not only at 50 FPS
void SetupWin::recEnables() {
	int codec = getRFIData(cbRecCodec);
	int chroma = getRFIData(cbRecChroma);
	bool lossy = codec != VREC_FFV1;
	bool video = leRecVOver->text().trimmed().isEmpty();
	bool sound = leRecSOver->text().trimmed().isEmpty();
	cbRecScale->setEnabled((getRFIData(cbRecSrc) == VREC_SRC_PICTURE) && video);
	// 4:2:0, the codec's or the one asked for: an odd scale puts two dots'
	// colours in one, so those are off and one that was picked goes up to the next
	bool even = vrec_420_only(codec, chroma);
	QStandardItemModel* sm = qobject_cast<QStandardItemModel*>(cbRecScale->model());
	for (int i = 0; sm && (i < sm->rowCount()); i++)
		sm->item(i)->setEnabled(!even || !(cbRecScale->itemData(i).toInt() & 1));
	int sc = getRFIData(cbRecScale);
	int fit = vrec_scale_for(codec, chroma, sc);
	if (fit != sc) setRFIndex(cbRecScale, fit);
	// the cards, AV1 and FFV1 have one format of their own
	cbRecChroma->setEnabled(lossy && video && !vrec_420_only(codec, VREC_CH_AUTO));
	cbRecBox->setEnabled(lossy);
	sbRecCrf->setEnabled(lossy && video);
	cbRecPreset->setEnabled(lossy && video);
	cbRec60->setEnabled(video);
	cbRecAcodec->setEnabled(lossy && sound);
	cbRecAbr->setEnabled(lossy && sound);
	cbRecPitch->setEnabled((getRFIData(cbRecFps) == VREC_FPS_50) && sound);
	cbRecStartAt->setEnabled(cbRecStart->isChecked());
	leRecStartAt->setEnabled(cbRecStart->isChecked() && (getRFIData(cbRecStartAt) != VREC_AT_OWN));
	cbRecStopAt->setEnabled(cbRecStop->isChecked());
	leRecStopAt->setEnabled(cbRecStop->isChecked() && (getRFIData(cbRecStopAt) != VREC_AT_OWN));
}

// a switch and its condition: its own case (code in ram, a reset) or the pc
// against an address
QWidget* SetupWin::recCondBox(QCheckBox* cb, QComboBox* at, QLineEdit* adr, const QString& own) {
	at->addItem(own, VREC_AT_OWN);
	at->addItem("PC ==", VREC_AT_EQ);
	at->addItem("PC >=", VREC_AT_GE);
	at->addItem("PC <=", VREC_AT_LE);
	adr->setPlaceholderText("#0000");
	adr->setFixedWidth(adr->fontMetrics().horizontalAdvance("#00000") + 12);
	QWidget* box = new QWidget;
	QHBoxLayout* lay = new QHBoxLayout(box);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->addWidget(cb);
	lay->addWidget(at);
	lay->addWidget(adr);
	lay->addStretch(1);
	connect(cb, &QCheckBox::toggled, this, &SetupWin::recEnables);
	connect(at, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SetupWin::recEnables);
	return box;
}

// an address that does not read keeps the one before
static void rec_cond_read(QComboBox* at, QLineEdit* adr, int* op, int* val) {
	*op = getRFIData(at);
	int o;
	int a;
	if ((*op != VREC_AT_OWN) && vrec_auto_parse(adr->text(), &o, &a)) *val = a;
}

void SetupWin::showFfmpeg() {
	QString prog = leRecFfmpeg->text().trimmed();
	if (prog.isEmpty()) {
		// Auto: what it finds goes straight into the field
		prog = vrec_ffmpeg_auto();
		if (!prog.isEmpty()) leRecFfmpeg->setText(QDir::toNativeSeparators(prog));
	}
	askFfmpeg(QDir::cleanPath(prog));
}

// what the program can do: from the settings while they are about this very
// file, else asked; one running already sees the program changed and asks again
void SetupWin::askFfmpeg(const QString& prog) {
	recProbeFor = prog;
	vrecProbe res;
	if (prog.isEmpty() || vrec_probe_known(prog, &res)) {
		showProbe(res);
	} else if (!recProbing) {
		probeFfmpeg();
	}
}

// On the way out: a probe still running is told to stop after the FFmpeg run
// it is in, and waited for, or it would outlive the log it writes to
void SetupWin::stopProbe() {
	recProbeStop = true;
	if (recProbeThr) recProbeThr->wait(25000);
}

// Soon after the start, so Options finds the answer ready: once for each new
// FFmpeg, and the emulation, a thread of its own, goes on meanwhile
void SetupWin::prewarmFfmpeg() {
	QString prog = QDir::cleanPath(vrec_ffmpeg());
	if (!prog.isEmpty()) askFfmpeg(prog);
}

// Asking FFmpeg what it can do runs it a dozen times, so it is done on a thread
// of its own and Options opens meanwhile, with the codecs that need no asking
void SetupWin::probeFfmpeg() {
	QString prog = recProbeFor;
	recProbing = true;
	leRecFfmpeg->setToolTip(tr("Checking what it can do"));
	btnRecGet->hide();
	fillCodecs(0);
	showRecCmd();
	vrecProbe* res = new vrecProbe;
	xRecord rec = conf.rec;		// a copy: the thread must not read what the GUI writes
	std::atomic<bool>* stop = &recProbeStop;
	QThread* thr = QThread::create([prog, rec, res, stop]() { *res = vrec_probe(prog, rec, stop); });
	recProbeThr = thr;
	connect(thr, &QThread::finished, this, [this, thr, prog, res]() {
		thr->deleteLater();
		recProbeThr = NULL;
		vrecProbe got = *res;
		delete res;
		recProbing = false;
		if (recProbeStop) return;		// cut short, so not an answer
		if (!got.version.isEmpty()) vrec_probe_keep(prog, got);
		if (prog != recProbeFor) {
			showFfmpeg();		// the field was changed meanwhile
		} else {
			showProbe(got);
			showRecCmd();
		}
	});
	thr->start();
}

void SetupWin::showProbe(const vrecProbe& res) {
	btnRecGet->setVisible(res.version.isEmpty());
	if (!res.version.isEmpty()) {
		leRecFfmpeg->setToolTip("FFmpeg " + vrec_ffmpeg_release(res.version));
	} else if (recProbeFor.isEmpty()) {
		leRecFfmpeg->setPlaceholderText(tr("Not found: pick ffmpeg"));
		leRecFfmpeg->setToolTip(tr("Not in the config folder, not on PATH"));
	} else {
		leRecFfmpeg->setToolTip(tr("This does not run as FFmpeg"));
	}
	fillCodecs(res.works);
}

// The codecs, the cards' only where this program runs them here. The pick
// stays; while nobody knows yet, it stays listed as well.
void SetupWin::fillCodecs(unsigned works) {
	int cur = cbRecCodec->count() ? getRFIData(cbRecCodec) : conf.rec.codec;
	cbRecCodec->clear();
	cbRecCodec->addItem("H.264", VREC_H264);
	cbRecCodec->addItem("H.265", VREC_H265);
	static const struct {int id; const char* name;} probed[] = {
		{VREC_AV1, "AV1"},
		{VREC_H264_NVENC, "NVENC H.264"}, {VREC_H265_NVENC, "NVENC H.265"}, {VREC_AV1_NVENC, "NVENC AV1"},
		{VREC_H264_AMF, "AMF H.264"}, {VREC_H265_AMF, "AMF H.265"}, {VREC_AV1_AMF, "AMF AV1"},
		{VREC_H264_QSV, "QSV H.264"}, {VREC_H265_QSV, "QSV H.265"}, {VREC_AV1_QSV, "QSV AV1"},
	};
	for (const auto& enc : probed)
		if ((works & (1u << enc.id)) || (recProbing && (enc.id == cur))) cbRecCodec->addItem(enc.name, enc.id);
	cbRecCodec->addItem(tr("FFV1, lossless"), VREC_FFV1);
	int idx = cbRecCodec->findData(cur);
	cbRecCodec->setCurrentIndex(idx < 0 ? 0 : idx);
}

// the settings as the controls have them, for the command shown
xRecord SetupWin::recFromUi() {
	xRecord rec = conf.rec;
	rec.ffmpeg = std::string(leRecFfmpeg->text().trimmed().toLocal8Bit().data());
	rec.dir = std::string(leRecDir->text().trimmed().toLocal8Bit().data());
	rec.source = getRFIData(cbRecSrc);
	rec.scale = getRFIData(cbRecScale);
	rec.fps = getRFIData(cbRecFps);
	rec.container = getRFIData(cbRecBox);
	rec.codec = getRFIData(cbRecCodec);
	rec.chroma = getRFIData(cbRecChroma);
	if (rec.codec == VREC_FFV1) rec.container = recBoxKeep;	// the list says MKV, the choice stays
	rec.crf = sbRecCrf->value();
	rec.preset = getRFSData(cbRecPreset).toStdString();
	rec.acodec = getRFIData(cbRecAcodec);
	rec.abitrate = getRFIData(cbRecAbr);
	rec.extra = std::string(leRecExtra->text().trimmed().toLocal8Bit().data());
	rec.videoOver = std::string(leRecVOver->text().trimmed().toLocal8Bit().data());
	rec.soundOver = std::string(leRecSOver->text().trimmed().toLocal8Bit().data());
	rec.fps60 = cbRec60->isChecked() ? 1 : 0;
	rec.keepPitch = cbRecPitch->isChecked() ? 1 : 0;
	rec.autoStart = cbRecStart->isChecked() ? 1 : 0;
	rec.autoStop = cbRecStop->isChecked() ? 1 : 0;
	rec_cond_read(cbRecStartAt, leRecStartAt, &rec.autoStartOp, &rec.autoStartAdr);
	rec_cond_read(cbRecStopAt, leRecStopAt, &rec.autoStopOp, &rec.autoStopAdr);
	rec.name = std::string(leRecName->text().trimmed().toLocal8Bit().data());
	return rec;
}

void SetupWin::showRecCmd() {
	if (recFilling) return;
	xRecord rec = recFromUi();
	QSize in;
	QSize out;
	recSizes(&in, &out);
	// what the file will be called, with the image in use now; the file names
	// alone, and none of the options that only keep FFmpeg quiet
	QString name = vrec_file_name(leRecName->text(), media_image_name(), QDateTime::currentDateTime());
	vrecCmd cmd = vrec_command(rec, in.width(), in.height(), conf.zx->vid->nsPerFrame, conf.snd.rate, name, false);
	labRecName->setFull(cmd.out);
	// the colour resolution the video is written in, under its size
	if (recProbing) {
		labRecFmt->setText(tr("checking FFmpeg..."));
		labRecFmt->setToolTip(tr("Which encoders it runs here, asked once for each program"));
	} else if (!rec.videoOver.empty()) {
		labRecFmt->setText(tr("format: custom"));
		labRecFmt->setToolTip(tr("Video override sets it"));
	} else switch (vrec_chroma(rec)) {
		case VREC_RGB:
			labRecFmt->setText(tr("format RGB"));
			labRecFmt->setToolTip(tr("FFV1 keeps every pixel as it is"));
			break;
		case VREC_444:
			labRecFmt->setText(tr("format 4:4:4 (*)"));
			labRecFmt->setToolTip((rec.chroma == VREC_CH_444) ? tr("Some players and editors cannot open it")
				: tr("An odd scale or a shader needs every pixel's own colour.\nSome players and editors cannot open it"));
			break;
		default:
			labRecFmt->setText(tr("format 4:2:0"));
			labRecFmt->setToolTip(tr("Plays everywhere"));
			break;
	}
	leRecVOver->setPlaceholderText(vrec_args_line(cmd.video));
	leRecSOver->setPlaceholderText(vrec_args_line(cmd.sound));
	teRecCmd->setPlainText(vrec_command_line(cmd.enc, true) + "\n\n" + vrec_command_line(cmd.mux, true));
}

void SetupWin::fillRecording() {
	recFilling = true;
	leRecFfmpeg->setText(QString::fromLocal8Bit(conf.rec.ffmpeg.c_str()));
	leRecDir->setText(QString::fromLocal8Bit(conf.rec.dir.c_str()));
	leRecDir->setPlaceholderText(QDir::toNativeSeparators(vrec_dir_auto()));
	showFfmpeg();
	setRFIndex(cbRecSrc, conf.rec.source);
	setRFIndex(cbRecScale, conf.rec.scale);
	setRFIndex(cbRecFps, conf.rec.fps);
	setRFIndex(cbRecBox, conf.rec.container);
	recBoxKeep = conf.rec.container;
	setRFIndex(cbRecCodec, conf.rec.codec);
	setRFIndex(cbRecChroma, conf.rec.chroma);
	sbRecCrf->setValue(conf.rec.crf);
	int idx = cbRecPreset->findData(QString::fromStdString(conf.rec.preset));
	cbRecPreset->setCurrentIndex(idx < 0 ? 2 : idx);
	setRFIndex(cbRecAcodec, conf.rec.acodec);
	// the codec and the bitrate as wide as the wider, measured here as the
	// Emulation page's lists are: a style or a font would outgrow a width set once
	int sndw = qMax(comboFitWidth(cbRecAcodec), comboFitWidth(cbRecAbr));
	cbRecAcodec->setFixedWidth(sndw);
	cbRecAbr->setFixedWidth(sndw);
	idx = cbRecAbr->findData(conf.rec.abitrate);
	cbRecAbr->setCurrentIndex(idx < 0 ? 1 : idx);
	leRecExtra->setText(QString::fromLocal8Bit(conf.rec.extra.c_str()));
	leRecVOver->setText(QString::fromLocal8Bit(conf.rec.videoOver.c_str()));
	leRecSOver->setText(QString::fromLocal8Bit(conf.rec.soundOver.c_str()));
	cbRec60->setChecked(conf.rec.fps60);
	cbRecPitch->setChecked(conf.rec.keepPitch);
	cbRecStart->setChecked(conf.rec.autoStart);
	setRFIndex(cbRecStartAt, conf.rec.autoStartOp);
	leRecStartAt->setText("#" + gethexword(conf.rec.autoStartAdr));
	cbRecStop->setChecked(conf.rec.autoStop);
	setRFIndex(cbRecStopAt, conf.rec.autoStopOp);
	leRecStopAt->setText("#" + gethexword(conf.rec.autoStopAdr));
	leRecName->setText(QString::fromLocal8Bit(conf.rec.name.c_str()));
	recFilling = false;
	recEnables();
	showRecSize();
	alignVideoLabels();
}

void SetupWin::applyRecording() {
	conf.rec = recFromUi();
	vrec_auto_settings();
}

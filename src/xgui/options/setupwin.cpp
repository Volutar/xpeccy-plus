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
#include <QGuiApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QDebug>
#include <stdlib.h>

#include <SDL.h>

#include "filer.h"
#include "setupwin.h"
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
#include "libxpeccy/filetypes/filetypes.h"
#include "libxpeccy/input/input.h"

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
	box->addItem("none", 0);
#if defined(USEOPENGL)
	if (conf.vid.shd_support) {
		foreach(QString nam, lst) {
			box->addItem(nam, 1);
		}
		box->setCurrentIndex(box->findText(conf.vid.shader.c_str()));
		if (box->currentIndex() < 0)
			box->setCurrentIndex(0);
	}
#else
	box->setCurrentIndex(0);
#endif
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

void opt_fill_psg_boxes(QComboBox* cbcount, QComboBox* cbtype, QComboBox* cbfrq, QComboBox* cbstereo) {
	cbcount->clear();
	cbcount->addItem(QIcon(":/images/cancel.png"),"None",PSG_NONE);
	cbcount->addItem(QString::fromUtf8("×1 - AY/YM"),PSG_ONE);
	cbcount->addItem(QString::fromUtf8("×2 - TurboSound"),PSG_TS);
	cbcount->addItem(QString::fromUtf8("×2 - TurboSound FM"),PSG_TSFM);
	cbcount->addItem(QString::fromUtf8("×3 - ZX Next"),PSG_NEXT);
	cbtype->clear();
	cbtype->addItem(QIcon(":/images/MicrochipLogo.png"),"AY-3-8910",SND_AY);
	cbtype->addItem(QIcon(":/images/YamahaLogo.png"),"Yamaha 2149",SND_YM);
	cbtype->addItem(QIcon(":/images/YamahaLogo.png"),"Yamaha 2203",SND_YM2203);
	cbfrq->clear();
	cbfrq->addItem("Auto");			// its figure is filled in by chapsg()
	cbfrq->addItem(QString::fromUtf8("1.773447 - ZX 128/+2/+3"));
	cbfrq->addItem(QString::fromUtf8("1.75 - ZX 48/ZX-clones"));
	cbfrq->addItem(QString::fromUtf8("3.5 - YM2203"));
	cbstereo->clear();
	cbstereo->addItem("Mono",AY_MONO);
	cbstereo->addItem("ABC",AY_ABC);
	cbstereo->addItem("ACB",AY_ACB);
	cbstereo->addItem("BAC",AY_BAC);
	cbstereo->addItem("BCA",AY_BCA);
	cbstereo->addItem("CAB",AY_CAB);
	cbstereo->addItem("CBA",AY_CBA);
}

// frequency items are "<mhz> (machines)", so cut the comment off

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
	sldPsgSep = new xSlider;
	sldPsgSep->setRange(0, 100);
	sldPsgSep->setPageStep(5);
	sldPsgSep->setTickInterval(25);
	sldPsgSep->setMinimumWidth(45);
	sldSensitivity = new xSlider;
	sldSensitivity->setRange(100, 1900);
	sldSensitivity->setSingleStep(10);
	sldSensitivity->setPageStep(100);
	sldSensitivity->setValue(1000);
	sldSensitivity->setTickInterval(100);
	foreach(QSlider* sld, QList<QSlider*>() << sldTapeSpeed << sldPsgSep << sldSensitivity) {
		sld->setOrientation(Qt::Horizontal);
		sld->setTickPosition(QSlider::TicksBelow);
	}
	labTapeSpeedVal = new QLabel("100%");
	labTapeSpeedVal->setMinimumWidth(34);
	labPsgMhz = new QLabel("MHz");
	labPsgSep = new QLabel("75%");
	labPsgSep->setMinimumWidth(32);
	labPsgSep->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
}

SetupWin::SetupWin(QWidget* par):QDialog(par) {
	setModal(true);
	ui.setupUi(this);
	makeDevWidgets();
	// a .ui iconset holds a single pixmap, which the title bar and the tab
	// bar would have to downscale; the application icon carries every drawn
	// size instead. It has to be set explicitly: an unset icon is inherited
	// from the parent window, which wears the pause icon while this dialog
	// is open.
	setWindowIcon(QGuiApplication::windowIcon());
	ui.tabz->setTabIcon(ui.tabz->indexOf(ui.tab_4), QGuiApplication::windowIcon());

	spaceLedIcon(ui.cbKeysLed);
	spaceLedIcon(ui.cbJoyLed);
	spaceLedIcon(ui.cbMouseLed);
	spaceLedIcon(ui.cbTapeLed);
	spaceLedIcon(ui.cbDiskLed);
	spaceLedIcon(ui.cbFpsLed);
	spaceLedIcon(ui.cbHaltLed);
	spaceLedIcon(ui.cbClockLed);
	spaceLedIcon(ui.cbMessage);

	rseditor = new xRomsetEditor(this);
	rseditor->setModal(true);
	rsmodel = new xRomsetModel();
	ui.tvRomset->setModel(rsmodel);

	layeditor = new QDialog(this);
	layUi.setupUi(layeditor);
	layeditor->setModal(true);

	padial = new xPadBinder(this);
	gpwid_a = new xGamepadWidget(conf.gpctrl->gpada);
	gpwid_b = new xGamepadWidget(conf.gpctrl->gpadb);
	connect(gpwid_a, &xGamepadWidget::s_edit_entry, padial, &xPadBinder::start);
	connect(gpwid_b, &xGamepadWidget::s_edit_entry, padial, &xPadBinder::start);
	connect(padial, &xPadBinder::bindReady, this, &SetupWin::bindAccept);

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
	ui.bszsld->setMaximum(VID_BRD_OVERSCAN);	// one tick per border size
	ui.sldSpeed->setMaximum(XSPD_MAX);		// one tick per step of the speed scale
	ui.cbCpuFrq->addItem("3.5 MHz");
	ui.cbCpuFrq->addItem("3.5469 MHz");
	cbCpuTurbo->addItem(QString::fromUtf8("×1 (no turbo)"), "1");
	cbCpuTurbo->addItem(QString::fromUtf8("×1, ×2"), "1,2");
	cbCpuTurbo->addItem(QString::fromUtf8("×1, ×2, ×4"), "1,2,4");

#if defined(USEOPENGL)
//	ui.cbScanlines->setVisible(false);
	fill_shader_list(ui.cbShader);
#else
	ui.labShader->setVisible(false);
	ui.cbShader->setVisible(false);
#endif
	//fill_palette_list(ui.cbPalPreset);
	fillComboBox(ui.cbPalPreset, "palettes", QStringList() << "*.txt" << "*.pal", PAL_DEFAULT_NAME, conf.palette.c_str());
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
	diskTypeBox->addItem("Beta Disk (VG93)",DIF_BDI);
	diskTypeBox->addItem("+3 DOS (uPD765)",DIF_P3DOS);
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
	hiface->addItem("Nemo",IDE_NEMO);
	hiface->addItem("Nemo A8",IDE_NEMOA8);
	hiface->addItem("Nemo Evo",IDE_NEMO_EVO);
	hiface->addItem("SMUC",IDE_SMUC);
	hiface->addItem("ATM",IDE_ATM);
	hiface->addItem("Profi",IDE_PROFI);
	hm_type->addItem(QIcon(":/images/cancel.png"),"Not connected",IDE_NONE);
	hm_type->addItem(QIcon(":/images/hdd.png"),"HDD (ATA)",IDE_ATA);
	hs_type->addItem(QIcon(":/images/cancel.png"),"Not connected",IDE_NONE);
	hs_type->addItem(QIcon(":/images/hdd.png"),"HDD (ATA)",IDE_ATA);
// input
//	padModel = new xPadMapModel();
//	ui.tvPadTable->setModel(padModel);
//	ui.tvPadTable->addAction(ui.actAddBinding);
//	ui.tvPadTable->addAction(ui.actEditBinding);
//	ui.tvPadTable->addAction(ui.actDelBinding);
	ui.tabsGamepad->addTab(gpwid_a, "Gamepad A");
	ui.tabsGamepad->addTab(gpwid_b, "Gamepad B");
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
	connect(ui.pbAdvanced,SIGNAL(released()),this,SLOT(showAdvanced()));
	connect(ui.pbSaveMachine,SIGNAL(released()),this,SLOT(saveMachine()));
	connect(ui.pbDelMachine,SIGNAL(released()),this,SLOT(delMachine()));
	connect(ui.pbCfgExport,SIGNAL(released()),this,SLOT(cfgExport()));
	connect(ui.pbCfgImport,SIGNAL(released()),this,SLOT(cfgImport()));
	connect(ui.pbCfgReset,SIGNAL(released()),this,SLOT(cfgReset()));

	// The settings that define the machine rather than how it is used live in
	// a window of their own. It is the same widgets, moved out of the page -
	// so everything that reads and writes them stays as it is.
	advWin = popOut(ui.advBox, "Machine: advanced settings");

	// the page keeps one button for the ROM set; the slots and the reset
	// target are in this window
	romSetWin = popOut(ui.romBox, "Machine: ROM");
	connect(ui.pbRomSet, &QPushButton::released, romSetWin, [this]() {romSetWin->adjustSize(); romSetWin->show(); romSetWin->raise();});

	// same for the set file by file: the slots say which file is in which
	// one, this window has the offsets and sizes behind it
	romWin = popOut(ui.romAdvBox, "Machine: ROM files");
	romWin->resize(620, 340);
	QPushButton* pbExpert = romSetWin->findChild<QDialogButtonBox*>()->addButton(tr("Expert settings"), QDialogButtonBox::ActionRole);
	pbExpert->setIcon(QIcon(":/images/settings.png"));
	pbExpert->setToolTip(tr("Where each file starts, how much of it is read, and where it lands"));
	connect(pbExpert, SIGNAL(released()), this, SLOT(showRomFiles()));
	buildDevices();
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
	connect(sldTapeSpeed, &QSlider::valueChanged, this, [this](int v){
		labTapeSpeedVal->setText(QString("%0%").arg(v));
	});
// input
//	connect(ui.tbPadNew, SIGNAL(released()),this,SLOT(newPadMap()));
//	connect(ui.tbPadDelete,SIGNAL(released()),this,SLOT(delPadMap()));
//	connect(ui.cbPadMap, SIGNAL(currentIndexChanged(int)),this,SLOT(chaPadMap(int)));
//	connect(ui.tbAddBind,SIGNAL(clicked(bool)),this, SLOT(addBinding()));
//	connect(ui.tbEditBind,SIGNAL(clicked(bool)),this,SLOT(editBinding()));
//	connect(ui.tbDelBind,SIGNAL(clicked(bool)),this,SLOT(delBinding()));
//	connect(ui.actAddBinding,SIGNAL(triggered()),this,SLOT(addBinding()));
//	connect(ui.actEditBinding,SIGNAL(triggered()),this,SLOT(editBinding()));
//	connect(ui.tvPadTable,SIGNAL(doubleClicked(QModelIndex)),this,SLOT(editBinding()));
//	connect(ui.actDelBinding,SIGNAL(triggered()),this,SLOT(delBinding()));
//	connect(padial, SIGNAL(bindReady(xJoyMapEntry)), this, SLOT(bindAccept(xJoyMapEntry)));
//	connect(ui.cbGamepad, SIGNAL(currentIndexChanged(int)),this,SLOT(setCurrentGamepad(int)));
//tools
	connect(ui.pbFavorites, &QPushButton::clicked, this, [this]() {fav_manage(this);});
// debuga
	portwid = new xPortWatch;		// the same editor the debugger opens itself
	ui.layDbgPorts->addWidget(portwid);

	connect(ui.tbDbgFont,SIGNAL(released()),this,SLOT(selectDbgFont()));
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
		ui.tbDbgAsmConstFG,
		NULL
	};
	i = 0;
	while (tbarr[i] != NULL) {
		connect(tbarr[i], SIGNAL(released()), this, SLOT(selectColor()));
		connect(tbarr[i], SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(triggerColor()));
		i++;
	}
// profiles manager
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
		ui.tbDbgAsmConstFG
	};
	// the default a right click puts back is the same one the emulator starts
	// with, and the same one "System" restores
	for (int i = 0; names[i]; i++)
		setToolButtonColor(tb[i], names[i], dbgPaletteDefault(names[i]));
}

void SetupWin::setPadName() {
//	ui.lePadName->setText(conf.joy.gpad->name());
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

void SetupWin::start() {
	Computer* comp = conf.zx;
	fillLogPage();
// machine
	int idx;
	fill_machine_list(ui.machbox);
	updateMachineButtons();
	roms = conf.roms;
	resTarget = comp->resbank;
	rsmodel->fill(&roms);
	fillRomSlots();
	ui.machbox->setCurrentIndex(ui.machbox->findData(QString::fromLocal8Bit(conf.macId.c_str())));
	setmszbox(ui.machbox->currentIndex());
	ui.mszbox->setCurrentIndex(ui.mszbox->findData(comp->mem->ramSize));
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
	// Input lag and Indicators are grids of their own, and columns line up
	// between two grids only while both are given the same widths. Measure them
	// here, not in the .ui: a style or a font would outgrow a number set there.
	ui.cbRunAhead->setFixedWidth(comboFitWidth(ui.cbRunAhead));
	QGridLayout* emugrid[2] = {ui.gridLayout_lat, ui.gridLayout_23};
	for (int col = 0; col < 2; col++) {
		int wid = qMax(gridColWidth(emugrid[0], col), gridColWidth(emugrid[1], col));
		emugrid[0]->setColumnMinimumWidth(col, wid);
		emugrid[1]->setColumnMinimumWidth(col, wid);
	}
// video
	ui.cbFullscreen->setChecked(conf.vid.fullScreen);
	ui.cbKeepRatio->setChecked(conf.vid.keepRatio);
	setRFIndex(ui.cbScale, conf.vid.scale, 1);	// x2 if the file says something odd
	ui.sldNoflic->setValue(noflic); chaflc();
	ui.cbNoflicMode->setCurrentIndex(noflicMode);
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
	ui.cbSnow->setChecked(comp->flgSNOW);
	ui.cbSnowCrash->setChecked(comp->flgSNOWX);
	chasnow();
	ui.bszsld->setValue(conf.vid.border);
	chabsz();
	ui.pathle->setText(QString::fromLocal8Bit(conf.scrShot.dir.c_str()));
	ui.ssfbox->setCurrentIndex(ui.ssfbox->findData(QString::fromStdString(conf.scrShot.format)));
	ui.scntbox->setValue(conf.scrShot.count);
	ui.sintbox->setValue(conf.scrShot.interval);
	ui.ssNoLeds->setChecked(conf.scrShot.noLeds);
	ui.ssNoBord->setChecked(conf.scrShot.noBorder);
	ui.geombox->clear();
	foreach(xLayout lay, conf.layList) {
		ui.geombox->addItem(QString::fromLocal8Bit(lay.name.c_str()));
	}
	ui.geombox->setCurrentIndex(ui.geombox->findText(QString::fromLocal8Bit(conf.layName.c_str())));
	ui.ulaPlus->setChecked(comp->vid->ula->enabled);
	ui.cbDDp->setChecked(comp->flgDDP);
	fill_shader_list(ui.cbShader);
	//fill_palette_list(ui.cbPalPreset);
	fillComboBox(ui.cbPalPreset, "palettes", QStringList() << "*.txt" << "*.pal", PAL_DEFAULT_NAME, conf.palette.c_str());
// sound
	gsBox->setCurrentIndex(comp->gs->enable ? 1 : 0);
	gsrbox->setChecked(comp->gs->reset);

	sdrvBox->setCurrentIndex(sdrvBox->findData(comp->sdrv->type));

	ui.cbSAA->setChecked(comp->saa->enabled);

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
	if (comp->ts->frqAuto)
		cbPsgFrq->setCurrentIndex(0);
	else
		opt_set_psg_frq(cbPsgFrq, comp->ts->chipA->frq);
	sldPsgSep->setValue(comp->ts->chipA->sep);
	chapsg();
// input
	buildkeylist();
	setRFIndex(cbScanTab, comp->keyb->pcmode);
	idx = ui.keyMapBox->findText(QString(conf.kmapName.c_str()));
	if (idx < 1) idx = 0;
	ui.keyMapBox->setCurrentIndex(idx);
	mouseBox->setCurrentIndex(comp->mouse->enable ? 1 : 0);
	ratWheel->setChecked(comp->mouse->hasWheel);
	cbSwapButtons->setChecked(comp->mouse->swapButtons);
	sldSensitivity->setValue(comp->mouse->sensitivity * 1000.0f);
	joyBox->setCurrentIndex((comp->joy->type != XJ_KEMPSTON) ? 0 : comp->joy->extbuttons ? 2 : 1);
	gpwid_a->update(conf.jmapNameA);
	gpwid_b->update(conf.jmapNameB);
//	ui.sldDeadZone->setValue(conf.joy.gpad->deadZone());
//	ui.cbGamepad->blockSignals(true);
//	fillRFBox(ui.cbGamepad, conf.joy.gpad->getList());
//	setRFIndex(ui.cbGamepad, conf.joy.gpad->name()); // curName);
//	ui.cbGamepad->blockSignals(false);
//	padModel->update();
//	buildpadlist();
//	setRFIndex(ui.cbPadMap, conf.jmapNameA.c_str());
// flp
	diskTypeBox->setCurrentIndex(diskTypeBox->findData(comp->dif->type));
	bdtbox->setChecked(fdcFlag & FDC_FAST);
	ui.mempaths->setChecked(conf.storePaths);
	ui.cbAutorun->setChecked(conf.autorun);
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
	hiface->setCurrentIndex(hiface->findData(comp->ide->type));

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
	sldTapeSpeed->setValue(comp->tape->speed);	// the readout follows in the slot
	showDevRows();
// tools
	ui.sbPort->setValue(conf.port);
	ui.cbConfexit->setChecked(conf.confexit);
// leds
	ui.cbMouseLed->setChecked(conf.led.mouse);
	ui.cbJoyLed->setChecked(conf.led.joy);
	ui.cbKeysLed->setChecked(conf.led.keys);
	ui.cbTapeLed->setChecked(conf.led.tape);
	ui.cbDiskLed->setChecked(conf.led.disk);
	ui.cbMessage->setChecked(conf.led.message);
	ui.cbFpsLed->setChecked(conf.led.fps);
	ui.cbHaltLed->setChecked(conf.led.halt);
	ui.cbClockLed->setChecked(conf.led.clock);
// debuga
	ui.sbDbSize->setValue(conf.dbg.dbsize);
	ui.sbDwSize->setValue(conf.dbg.dwsize);
	ui.sbTextSize->setValue(conf.dbg.dmsize);
	ui.sbDbgStackOfs->setValue(conf.dbg.stackofs);
	ui.cbDbgMemmap->setChecked(conf.dbg.showmmap);
	ui.cbDbgPorts->setChecked(conf.dbg.showports);
	ui.cbDbgSignals->setChecked(conf.dbg.showsig);
	ui.cbDbgFrame->setChecked(conf.dbg.showfrm);
	ui.cbDbgRay->setChecked(conf.dbg.showray);
	portwid->setPorts(getWatchPorts(conf.zx));
	dbgfnt = conf.dbg.font;
	ui.leDbgFont->setText(QString("%0, %1 pt").arg(dbgfnt.family()).arg(dbgfnt.pointSize()));
	ui.leDbgFont->setFont(dbgfnt);
// palette
	fillDbgPalette();
	fillComboBox(ui.cbStyleSheet, "styles", QStringList() << "*.qss", "System", conf.style.c_str());

	show();
}

void SetupWin::apply() {
	Computer* comp = conf.zx;
// machine
	// another machine is not this page with different values in it: it has its
	// own, so load it and show them rather than writing these over it
	std::string mid = std::string(getRFSData(ui.machbox).toLocal8Bit().data());
	if (!mid.empty() && (mid != conf.macId)) {
		xm_set(mid);
		start();
		emit s_prf_changed();
		return;
	}
	emu_lock();		// roms and memory size are rebuilt below
	xm_set_roms(roms);
	comp->resbank = resTarget;
	memSetSize(comp->mem, getRFIData(ui.mszbox), -1);
	compSetBaseFrq(comp, xcpu_frq_parse(ui.cbCpuFrq->currentText(), comp->cpuFrq));
	xm_turbo_set(comp, getRFSData(cbCpuTurbo).toStdString());
	xspeed_set(ui.sldSpeed->value());
	comp->flgEM1 = ui.scrpwait->isChecked();
	if (comp->hw->id == HW_ZX48) comp->mem->ramMask = MEM_128K - 1;		// TODO: find a better way
	emu_unlock();
// emulation
	conf.vid.lowLatency = ui.cbLowLat->isChecked() ? 1 : 0;
	conf.emu.runahead = getRFIData(ui.cbRunAhead);
// video
	conf.vid.fullScreen = ui.cbFullscreen->isChecked() ? 1 : 0;
	conf.vid.keepRatio = ui.cbKeepRatio->isChecked() ? 1 : 0;
	conf.vid.scale = getRFIData(ui.cbScale);
	noflic = ui.sldNoflic->value();
	noflicMode = ui.cbNoflicMode->currentIndex();
	noflicGamma = ui.sbNoflicGamma->value();
	vid_set_grey(ui.grayscale->isChecked() ? 1 : 0);
//	scanlines = ui.cbScanlines->isChecked() ? 1 : 0;
	conf.scrShot.dir = std::string(ui.pathle->text().toLocal8Bit().data());
	conf.scrShot.format = getRFSData(ui.ssfbox).toStdString();
	conf.scrShot.count = ui.scntbox->value();
	conf.scrShot.interval = ui.sintbox->value();
	conf.scrShot.noLeds = ui.ssNoLeds->isChecked() ? 1 : 0;
	conf.scrShot.noBorder = ui.ssNoBord->isChecked() ? 1 : 0;
	vid_set_border_mode(ui.bszsld->value());
	comp->vid->brdstep = ui.border4T->isChecked() ? 7 : 1;
	comp_set_cont(comp, ui.contMem->isChecked());
	comp->flgCNTI = ui.contIO->isChecked() ? 1 : 0;
	comp->vid->ula->conttype = getRFIData(ui.cbContPattern);
	comp->earback = getRFIData(ui.cbEarBack);
	comp->fbus = getRFIData(ui.cbFloatBus);
	comp->vid->ula->early = ui.cbEarlyTiming->isChecked();
	comp_set_snow(comp, ui.cbSnow->isChecked() ? 1 : 0);
	comp->flgSNOWX = ui.cbSnowCrash->isChecked() ? 1 : 0;
	// The ula type also picks the screen drawer. The reset above ran before this
	// line and saw the old type, so it has to be redone here - but only while a
	// plain zx screen is up: a machine sitting in one of its own modes keeps it
	// and gets the drawer at its next reset.
	if ((comp->vid->vmode == VID_NORMAL) || (comp->vid->vmode == VID_ULA_SCR))
		zx_set_vmode(comp);
	comp->vid->ula->enabled = ui.ulaPlus->isChecked() ? 1 : 0;
	comp->flgDDP = ui.cbDDp->isChecked() ? 1 : 0;
	xm_set_layout(getRFText(ui.geombox));
	if (getRFIData(ui.cbShader) == 0) {
		conf.vid.shader.clear();
	} else {
		conf.vid.shader = std::string(ui.cbShader->currentText().toLocal8Bit().data());
	}
	QString str = getRFSData(ui.cbPalPreset);
	if (str.isEmpty()) {
		//conf.vid.palette.clear();
		conf.palette.clear();
	} else {
		//conf.vid.palette = std::string(ui.cbPalPreset->currentText().toLocal8Bit().data());
		conf.palette = str.toStdString();
	}
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
	if ((rate != conf.snd.rate) || (rateauto != conf.snd.rateauto) ||
		(latency != conf.snd.latency) || (nname != sndGetName())) {
		conf.snd.rate = rate;
		conf.snd.rateauto = rateauto;
		conf.snd.latency = latency;
		setOutput(nname.c_str());
	}

	int psg = getRFIData(cbPsgCount);
	int chips = (psg == PSG_NEXT) ? 3 : ((psg == PSG_TS) || (psg == PSG_TSFM)) ? 2 : (psg == PSG_ONE) ? 1 : 0;
	int chtype = (psg == PSG_TSFM) ? SND_YM2203 : getRFIData(cbPsgType);
	if ((psg != PSG_TSFM) && (chtype == SND_YM2203)) chtype = SND_YM;
	int chstereo = getRFIData(cbPsgStereo);
	int chsep = sldPsgSep->value();
	aymChip* chip[3] = {comp->ts->chipA, comp->ts->chipB, comp->ts->chipC};
	for (int i = 0; i < 3; i++) {			// one setting for all the chips
		chip_set_type(chip[i], (i < chips) ? chtype : SND_NONE);
		chip[i]->stereo = chstereo;
		chip[i]->sep = chsep;
	}
	ts_set_frq(comp->ts, opt_get_psg_frq(cbPsgFrq), comp->cpuFrq);	// 0: Auto
	comp->ts->type = (chips > 2) ? TS_ZXNEXT : (chips > 1) ? TS_NEDOPC : TS_NONE;

	comp->gs->enable = gsBox->currentIndex();
	comp->gs->reset = gsrbox->isChecked() ? 1 : 0;

	comp->sdrv->type = getRFIData(sdrvBox);

	comp->saa->enabled = ui.cbSAA->isChecked() ? 1 : 0;
// input
	comp->keyb->pcmode = getRFIData(cbScanTab);
	comp->mouse->enable = mouseBox->currentIndex();
	comp->mouse->hasWheel = ratWheel->isChecked() ? 1 : 0;
	comp->mouse->swapButtons = cbSwapButtons->isChecked() ? 1 : 0;
	comp->mouse->sensitivity = sldSensitivity->value() * 0.001f;
	comp->joy->type = joyBox->currentIndex() ? XJ_KEMPSTON : XJ_NONE;
	comp->joy->extbuttons = (joyBox->currentIndex() == 2) ? 1 : 0;
	gpwid_a->apply();
	gpwid_b->apply();
/*
	conf.joy.gpad->setDeadZone(ui.sldDeadZone->value());
	if (ui.cbGamepad->currentIndex() < 1) {
		conf.joy.gpad->close();
	} else {
		conf.joy.gpad->open(getRFSData(ui.cbGamepad));
	}
*/
	std::string kmname = getRFText(ui.keyMapBox);
	if (kmname == "none") kmname = "default";
	conf.kmapName = kmname;
	loadKeys();
// flp
	difSetHW(comp->dif, getRFIData(diskTypeBox));
	setFlagBit(bdtbox->isChecked(),&fdcFlag,FDC_FAST);
	conf.boot = cbAddBoot->isChecked() ? 1 : 0;
	conf.storePaths = ui.mempaths->isChecked() ? 1 : 0;
	conf.autorun = ui.cbAutorun->isChecked() ? 1 : 0;
	ftbox->apply();
	flp_set_interleave(getRFIData(cbFlpInterleave));
	// a drive taken off the cable takes its disk with it: a changed one is
	// saved first, or the drive stays
	int fit = getRFIData(drvCountBox);
	if (getRFIData(diskTypeBox) == DIF_P3DOS) fit = qMin(fit, 2);
	for (int i = 3; i >= fit; i--) {
		Floppy* dflp = comp->dif->flp[i];
		if (dflp->fitted && dflp->insert && dflp->changed && (saveChangedDisk(comp, i) == ERR_CANCEL))
			fit = i + 1;
	}
	difSetDrives(comp->dif, fit);

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
	//comp->ide->type = getRFIData(hiface);
	ide_set_type(comp->ide, getRFIData(hiface));

	comp->ide->master->type = getRFIData(hm_type);
	comp->ide->slave->type = getRFIData(hs_type);
	// what is mounted is the Drives menu's; a folder is read again on Apply
	ide_remount(comp->ide);
// others
	sdc_remount(comp->sdc);
// tape
	conf.tape.autostart = cbTapeAuto->isChecked() ? 1 : 0;
	conf.tape.fast = cbTapeFast->isChecked() ? 1 : 0;
	conf.tape.flash = cbTapeFlash->isChecked() ? 1 : 0;
	conf.tape.edge = cbTapeEdge->isChecked() ? 1 : 0;
	conf.tape.rewind = cbTapeRewind->isChecked() ? 1 : 0;
	comp->tape->speed = sldTapeSpeed->value();
	tape_apply_options(comp->tape);
// input
	conf.jmapNameA = gpwid_a->getMapName();
	conf.jmapNameB = gpwid_b->getMapName();
//	conf.joy.gpad->loadMap(conf.jmapNameA);
// tools
	conf.port = ui.sbPort->value() & 0xffff;
	conf.confexit = ui.cbConfexit->isChecked() ? 1 : 0;
// leds
	conf.led.mouse = ui.cbMouseLed->isChecked() ? 1 : 0;
	conf.led.joy = ui.cbJoyLed->isChecked() ? 1 : 0;
	conf.led.keys = ui.cbKeysLed->isChecked() ? 1 : 0;
	conf.led.tape = ui.cbTapeLed->isChecked() ? 1 : 0;
	conf.led.disk = ui.cbDiskLed->isChecked() ? 1 : 0;
	conf.led.message = ui.cbMessage->isChecked() ? 1 : 0;
	conf.led.fps = ui.cbFpsLed->isChecked() ? 1 : 0;
	conf.led.halt = ui.cbHaltLed->isChecked() ? 1 : 0;
	conf.led.clock = ui.cbClockLed->isChecked() ? 1 : 0;
// debuga
	conf.dbg.dbsize = ui.sbDbSize->value();
	conf.dbg.dwsize = ui.sbDwSize->value();
	conf.dbg.dmsize = ui.sbTextSize->value();
	// the step is 2, but a typed-in value can still land odd
	conf.dbg.stackofs = ui.sbDbgStackOfs->value() & ~1;
	conf.dbg.font = dbgfnt;
	conf.dbg.showports = ui.cbDbgPorts->isChecked() ? 1 : 0;
	conf.dbg.showsig = ui.cbDbgSignals->isChecked() ? 1 : 0;
	conf.dbg.showfrm = ui.cbDbgFrame->isChecked() ? 1 : 0;
	conf.dbg.showray = ui.cbDbgRay->isChecked() ? 1 : 0;
	setWatchPorts(conf.zx, portwid->getPorts());
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
	// the machine carries what the page put in it: into its own file, so it is
	// still there after a switch away and back
	xm_save_over();
	updateMachineButtons();
	// the mark on the machine may have just appeared or gone
	int midx = ui.machbox->findData(QString::fromLocal8Bit(conf.macId.c_str()));
	const xMachine* cmac = xm_find(conf.macId);
	if (cmac && (midx >= 0)) ui.machbox->setItemText(midx, xm_list_name(*cmac));

	emit s_apply();

	layouts_save();
	saveConfig();
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

// A box taken out of the page, in a window with one button to put it away.
// The button wears the cross the main dialog's Cancel wears, so the three
// windows read as one family.
//
// Modal, so the page it came from cannot be closed out from under it.

QDialog* SetupWin::popOut(QWidget* box, const char* title) {
	QDialog* win = new QDialog(this);
	win->setModal(true);
	win->setWindowTitle(title);
	QVBoxLayout* lay = new QVBoxLayout(win);
	lay->addWidget(box);
	QDialogButtonBox* bbox = new QDialogButtonBox(QDialogButtonBox::Close, win);
	bbox->button(QDialogButtonBox::Close)->setIcon(QIcon(":/images/cancel.png"));
	lay->addWidget(bbox);
	connect(bbox, SIGNAL(rejected()), win, SLOT(hide()));
	return win;
}

#define	RSLOT_GS	-1
#define	RSLOT_FONT	-2
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

QToolButton* SetupWin::devRow(QGridLayout* grid, const QString& name, QWidget* choice, QWidget* body, const char* title) {
	int row = grid->rowCount();
	QLabel* lab = new QLabel(name);
	QToolButton* btn = new QToolButton;
	btn->setIcon(QIcon(":/images/settings.png"));
	if (body) {
		QDialog* win = popOut(body, title);
		btn->setToolTip(tr("More settings"));
		connect(btn, &QToolButton::released, win, [win]() {win->adjustSize(); win->show(); win->raise();});
	} else {
		btn->setEnabled(false);
	}
	grid->addWidget(lab, row, 0);
	// the widths it had on its old page mean nothing here
	choice->setMinimumWidth(0);
	choice->setMaximumWidth(QWIDGETSIZE_MAX);
	grid->addWidget(choice, row, 1);
	grid->addWidget(btn, row, 2);
	return btn;
}

static QGridLayout* devGroup(QVBoxLayout* col, const char* icon, const QString& title) {
	xIconGroup* box = new xIconGroup(QIcon(icon), title);
	QGridLayout* grid = new QGridLayout(box);
	grid->setColumnStretch(1, 1);
	col->addWidget(box);
	return grid;
}

void SetupWin::buildDevices() {
	QWidget* area = new QWidget;
	QHBoxLayout* cols = new QHBoxLayout(area);
	cols->setContentsMargins(0, 0, 0, 0);
	QVBoxLayout* left = new QVBoxLayout;
	QVBoxLayout* right = new QVBoxLayout;
	cols->addLayout(left, 1);
	cols->addLayout(right, 1);
	QToolButton* btn;

	QGridLayout* grid = devGroup(left, ":/images/floppy.png", tr("Storage"));
	tapeSum = new QLabel;
	xOptSheet tape;
	tape.field(tr("Playback speed"), fieldPair(sldTapeSpeed, labTapeSpeedVal, true),
		tr("Per cent of normal. A few images made on machines with an unusual clock load only when it is nudged either way"));
	tape.line();
	tape.check(cbTapeAuto, tr("Auto play / stop"), tr("Start the tape when a loader asks for it, stop it between blocks"), true);
	tape.check(cbTapeRewind, tr("Rewind at end"), tr("Play or the next load starts the tape again. Off: it stops at the end"), true);
	tape.check(cbTapeFast, tr("Fast loading"), tr("Full speed, sound off and picture held while a loader reads the tape"), true);
	tape.check(cbTapeFlash, tr("Flash loading"), tr("With fast loading: ROM blocks go straight to the machine"), true);
	tape.check(cbTapeEdge, tr("Edge detection"), tr("With fast loading: a loader waiting for a pulse gets it at once. Faster, not exact"), true);
	devRow(grid, tr("Tape"), tapeSum, tape.body, "Machine: tape");
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
	devRow(grid, tr("Disk"), diskTypeBox, disk.body, "Machine: disk");

	// the images and what is in them are the Drives menu's
	xOptSheet hdd;
	hdd.field(tr("Master"), hm_type);
	hdd.field(tr("Slave"), hs_type);
	devRow(grid, tr("Hard disk"), hiface, hdd.body, "Machine: hard disk");

	sdSum = new QLabel;
	btn = devRow(grid, tr("SD card"), sdSum, NULL, NULL);
	sdRow << grid->itemAtPosition(grid->rowCount() - 1, 0)->widget() << sdSum << btn;

	slotSum = new QLabel;
	btn = devRow(grid, tr("Cartridge"), slotSum, NULL, NULL);
	slotRow << grid->itemAtPosition(grid->rowCount() - 1, 0)->widget() << slotSum << btn;

	grid = devGroup(left, ":/images/joystick.png", tr("Input"));
	joyBox = devCombo(QStringList() << tr("None") << tr("Kempston 5-bit") << tr("Kempston 8-bit"));
	devRow(grid, tr("Joystick"), joyBox, NULL, NULL);
	// the gamepads are bound to the Kempston, so say when there is none
	joyHint = new QLabel(tr("No Kempston on this machine: bindings to it do nothing"));
	QFont hfnt = joyHint->font();
	hfnt.setItalic(true);
	joyHint->setFont(hfnt);
	ui.verticalLayout_2->insertWidget(1, joyHint);
	connect(joyBox, QOverload<int>::of(&QComboBox::currentIndexChanged), joyHint, [this](int idx) {joyHint->setVisible(idx == 0);});
	mouseBox = devCombo(QStringList() << tr("None") << tr("Kempston mouse"));
	xOptSheet mouse;
	mouse.field(tr("Sensitivity"), fieldPair(sldSensitivity, NULL, true),
		tr("How fast the pointer moves. In the middle it keeps up with the PC one"));
	mouse.line();
	mouse.check(ratWheel, tr("Wheel"), tr("The wheel is read too, as on the extended Kempston mouse"));
	mouse.check(cbSwapButtons, tr("Swap buttons"), tr("The left and right buttons trade places"));
	devRow(grid, tr("Mouse"), mouseBox, mouse.body, "Machine: mouse");
	btn = devRow(grid, tr("PC keyboard"), cbScanTab, NULL, NULL);
	kbdRow << grid->itemAtPosition(grid->rowCount() - 1, 0)->widget() << cbScanTab << btn;
	left->addStretch(1);

	grid = devGroup(right, ":/images/speaker.png", tr("Sound"));
	xOptSheet psg;
	psg.field(tr("Chip"), cbPsgType);
	psg.field(tr("Clock"), fieldPair(cbPsgFrq, labPsgMhz, false),
		tr("Auto: half the CPU clock, and 3.5 MHz for TurboSound FM"));
	psg.field(tr("Stereo"), cbPsgStereo);
	psg.field(tr("Separation"), fieldPair(sldPsgSep, labPsgSep, true),
		tr("100%: the channels kept apart, 0%: mono"));
	devRow(grid, tr("PSG"), cbPsgCount, psg.body, "Machine: PSG");
	devRow(grid, tr("DAC"), sdrvBox, NULL, NULL);
	gsBox = devCombo(QStringList() << tr("Off") << tr("On"));
	xOptSheet gs;
	gsRomBox = new QComboBox;
	QToolButton* gsRomBtn = new QToolButton;
	gsRomBtn->setIcon(QIcon(":/images/fileopen.png"));
	gsRomBtn->setToolTip(tr("Pick a ROM file"));
	connect(gsRomBox, QOverload<int>::of(&QComboBox::activated), this, [this](int idx) {
		romSlotPick(RSLOT_GS, gsRomBox->itemData(idx).toString());
	});
	connect(gsRomBtn, &QToolButton::released, this, [this]() {romSlotFile(gsRomBox, RSLOT_GS);});
	gs.field(tr("ROM"), fieldPair(gsRomBox, gsRomBtn, false));
	gs.line();
	gs.check(gsrbox, tr("Reset"), tr("The card is reset with the machine"));
	devRow(grid, tr("General Sound"), gsBox, gs.body, "Machine: General Sound");

	grid = devGroup(right, ":/images/clock.png", tr("Board"));
	devRow(grid, tr("Turbo"), cbCpuTurbo, NULL, NULL);
	right->addStretch(1);

	// one label width per column, so the choices line up from group to group
	foreach(QVBoxLayout* col, QList<QVBoxLayout*>() << left << right) {
		QList<QLabel*> labs;
		for (int i = 0; i < col->count(); i++) {
			QWidget* box = col->itemAt(i)->widget();
			if (!box) continue;
			QGridLayout* g = qobject_cast<QGridLayout*>(box->layout());
			for (int row = 0; g && (row < g->rowCount()); row++) {
				QLayoutItem* itm = g->itemAtPosition(row, 0);
				QLabel* lab = itm ? qobject_cast<QLabel*>(itm->widget()) : NULL;
				if (lab) labs << lab;
			}
		}
		int wid = 0;
		foreach(QLabel* lab, labs) wid = qMax(wid, lab->sizeHint().width());
		foreach(QLabel* lab, labs) lab->setMinimumWidth(wid);
	}
	ui.verticalLayout_39->insertWidget(1, area);
	tidySoundPage();
}

// The volumes, named the way the devices are, with the master set apart.
// SAM Coupe's chip stays hidden: no machine here has one.

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
		{new QLabel, tr("General Sound"), ui.sldGSVol, ui.sbGSVol}
	};
	int row = 0;
	for (int i = 0; i < 6; i++) {
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
	Computer* comp = conf.zx;
	QString sd = comp->sdc->image ? QString::fromLocal8Bit(comp->sdc->image) : QString();
	QString slot = comp->slot->path ? QString::fromLocal8Bit(comp->slot->path) : QString();
	sdSum->setText(sd.isEmpty() ? tr("(no card)") : QFileInfo(sd).fileName());
	slotSum->setText(slot.isEmpty() ? tr("(empty)") : QFileInfo(slot).fileName());
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
	bool sd = (hw == HW_PENTEVO) || (hw == HW_TSLAB);
	bool slot = (hw == HW_ZX48) || (hw == HW_ALF);
	// only these read PC scancodes
	bool kbd = (hw == HW_ATM2) || (hw == HW_PENTEVO) || (hw == HW_TSLAB);
	foreach(QWidget* w, sdRow) w->setVisible(sd);
	foreach(QWidget* w, slotRow) w->setVisible(slot);
	foreach(QWidget* w, kbdRow) w->setVisible(kbd);
	showDriveRows();
	// a controller on the board is not swapped out
	const xMachine* mac = xm_find(conf.macId);
	int bi = mac ? mac->builtin : 0;
	QString fixed = tr("Built into this board");
	diskTypeBox->setEnabled(!(bi & MAC_BI_DISK));
	diskTypeBox->setToolTip((bi & MAC_BI_DISK) ? fixed : QString());
	hiface->setEnabled(!(bi & MAC_BI_IDE));
	hiface->setToolTip((bi & MAC_BI_IDE) ? fixed : QString());
	// ALF reads its two joysticks its own way, on #1F and #FE
	joyBox->setEnabled(hw != HW_ALF);
	joyHint->setVisible(joyBox->currentIndex() == 0);
	joyBox->setToolTip((hw == HW_ALF) ? fixed : QString());
	// the +2A and +3 never page TR-DOS in
	QStandardItemModel* difs = qobject_cast<QStandardItemModel*>(diskTypeBox->model());
	int bdi = diskTypeBox->findData(DIF_BDI);
	if (difs && (bdi >= 0)) difs->item(bdi)->setEnabled((hw != HW_PLUS2A) && (hw != HW_PLUS3));
	fillDosRom();
	fillDevSummary();
}

void SetupWin::showAdvanced() {
	advWin->show();
	advWin->raise();
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
	xm_set(id);
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
	// gone for good, or back as it ships: either way the list is rebuilt
	if (!xm_find(id)) id = back;
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
		addRomSlot(row, name, i, files, true, from[i]);
		if (role.res >= 0) {
			QRadioButton* rb = new QRadioButton;
			rb->setToolTip(tr("Boot from this ROM"));
			rb->setEnabled(!romSlotFileName(i).isEmpty() || (from[i] >= 0));
			rb->setChecked(role.res == resTarget);
			resGroup->addButton(rb, role.res);
			ui.gridRomSlots->addWidget(rb, row, 0);
			resets = true;
		}
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

// the General Sound's ROM, which its own window picks

void SetupWin::fillGsRom(const QStringList& files) {
	gsRomBox->clear();
	gsRomBox->addItem(tr("(empty)"), QString());
	foreach(QString f, files) gsRomBox->addItem(f, f);
	QString cur = romSlotFileName(RSLOT_GS);
	if (!cur.isEmpty() && (gsRomBox->findData(cur) < 0)) gsRomBox->insertItem(1, cur, cur);
	gsRomBox->setCurrentIndex(qMax(0, gsRomBox->findData(cur)));
}

// the file in a slot, or an empty string when there is none

QString SetupWin::romSlotFileName(int slot) {
	std::string res;
	if (slot == RSLOT_GS) {
		res = roms.gsFile;
	} else if (slot == RSLOT_FONT) {
		res = roms.fntFile;
	} else {
		foreach(xRomFile rf, roms.roms) {
			if (rf.roffset == slot * 16) res = rf.name;
		}
	}
	return QString::fromLocal8Bit(res.c_str());
}

void SetupWin::addRomSlot(int row, QString name, int slot, const QStringList& files, bool has, int from) {
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
	QString rel = QDir(dir).relativeFilePath(file);
	if (!rel.startsWith("..")) file = rel;	// under the rom directory
	if (box->findData(file) < 0) box->insertItem(1, file, file);
	box->setCurrentIndex(box->findData(file));
	romSlotPick(slot, file);
}

void SetupWin::romSlotPick(int slot, const QString& file) {
	std::string name = std::string(file.toLocal8Bit().data());
	if (slot == RSLOT_GS) {
		roms.gsFile = name;
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

void SetupWin::buildpadlist() {
//	QDir dir(conf.path.confDir.c_str());
//	QStringList lst = dir.entryList(QStringList() << "*.pad",QDir::Files,QDir::Name);
//	fillRFBox(ui.cbPadMap, lst);
}

void SetupWin::buildkeylist() {
	fillComboBox(ui.keyMapBox, "keymaps", QStringList() << "*.map", "none",
		QString::fromLocal8Bit(conf.kmapName.c_str()));
}

struct xMemName {
	int mask;
	const char* name;
};

static xMemName memNameTab[] = {
	{MEM_16K, "16 KB"},
	{MEM_32K, "32 KB"},
	{MEM_64K, "64 KB"},
	{MEM_128K, "128 KB"},
	{MEM_256K, "256 KB"},
	{MEM_512K, "512 KB"},
	{MEM_1M, "1024 KB"},
	{MEM_2M, "2 MB"},
	{MEM_4M, "4 MB"},
	{MEM_8M, "8 MB"},
	{MEM_16M, "16 MB"},
	{-1, ""}
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
		if (t & memNameTab[idx].mask)
			ui.mszbox->addItem(memNameTab[idx].name, memNameTab[idx].mask);
		idx++;
	}
	ui.mszbox->setCurrentIndex(ui.mszbox->findData(size));
}

// hobeta header crc = ((105 + 257 * std::accumulate(data, data + 15, 0u)) & 0xffff))

// video

// the label beside the slider says what the mode gives on this machine - the
// fixed sizes are the same everywhere, overscan is not
void SetupWin::chabsz() {
	Computer* comp = conf.zx;
	int mode = ui.bszsld->value();
	vCoord sze = vid_crop_size(comp->vid, mode);
	ui.bszlab->setText(QString("%0 (%1×%2)").arg(brd_mode_name(mode)).arg(sze.x).arg(sze.y));
}

// the label beside the speed slider says which of the two things it is doing
// and what the cpu ends up on - the board's own turbo is not in this number,
// the clock indicator is where the whole truth is
void SetupWin::chaspd() {
	Computer* comp = conf.zx;
	int pos = ui.sldSpeed->value();
	QString txt = xspeed_name(pos);
	if (pos >= XSPD_CENTER)
		txt += QString(" (%0 MHz)").arg(comp->cpuFrq * xspeed_mult(pos), 0, 'g', 6);
	else
		txt += QString(" (%0 fps)").arg(1e9 / comp->vid->nsPerFrame * xspeed_mult(pos), 0, 'f', 1);
	ui.labSpeed->setText(txt);
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
	labPsgSep->setText(QString("%0%").arg(sldPsgSep->value()));
	// TurboSound FM is two YM2203 and nothing else; the others never are
	QStandardItemModel* types = qobject_cast<QStandardItemModel*>(cbPsgType->model());
	int fmrow = cbPsgType->findData(SND_YM2203);
	if (types && (fmrow >= 0)) types->item(fmrow)->setEnabled(fm);
	if (fm) setRFIndex(cbPsgType, SND_YM2203);
	else if (getRFIData(cbPsgType) == SND_YM2203) setRFIndex(cbPsgType, SND_YM);
	cbPsgType->setEnabled(on && !fm);
	cbPsgFrq->setEnabled(on);
	cbPsgStereo->setEnabled(on);
	// what Auto comes to on this machine, as the core works it out
	double base = xcpu_frq_parse(ui.cbCpuFrq->currentText(), conf.zx->cpuFrq);
	cbPsgFrq->setItemText(0, QString("Auto - %0").arg(fm ? 3.5 : base / 2, 0, 'g', 7));
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
	QString fpath = QFileDialog::getExistingDirectory(this,"Screenshots folder",QString::fromLocal8Bit(conf.scrShot.dir.c_str()),QFileDialog::ShowDirsOnly);
	if (fpath!="") ui.pathle->setText(fpath);
}

void SetupWin::paledit() {
	QString str = getRFSData(ui.cbPalPreset);
	if (str.isEmpty()) return;			// is default
	editpal = loadColors(str.toStdString());	// load colors from file
	while (editpal.size() < 16) editpal.append(Qt::black);
	paleditor->edit(&editpal);
}

void SetupWin::palchoosecol(QPoint p) {
}

void SetupWin::palstore() {
	int i;
	xColor xcol;
	bool upd = !!vid_zx_palette(conf.zx->vid);
	for (i = 0; (i < editpal.size()) && (i < 16); i++) {
		qDebug() << editpal[i];
		xcol.r = editpal[i].red();
		xcol.g = editpal[i].green();
		xcol.b = editpal[i].blue();
		vid_set_bcol(conf.zx->vid, i, xcol);
		if (upd) vid_set_col(conf.zx->vid, i, xcol);
	}
	// save palette to file, cuz Settings OK will reload palette from file
	saveColors(getRFSData(ui.cbPalPreset).toStdString(), editpal);
}

// input

void SetupWin::setCurrentGamepad(int idx) {
//	if (idx > 0) {			// 0 is 'none'
//		conf.joy.gpad->open(idx-1);
//	} else {
//		conf.joy.gpad->close();
//	}
}

void SetupWin::newPadMap() {
//	QString nam = QInputDialog::getText(this,"Enter...","New gamepad map name");
//	if (nam.isEmpty()) return;
//	nam.append(".pad");
//	std::string name = nam.toStdString();
//	if (padCreate(name)) {
//		ui.cbPadMap->addItem(nam, nam);
//		ui.cbPadMap->setCurrentIndex(ui.cbPadMap->count() - 1);
//	} else {
//		showInfo("Map with that name already exists");
//	}
}

void SetupWin::delPadMap() {
//	if (ui.cbPadMap->currentIndex() == 0) return;
//	QString name = getRFSData(ui.cbPadMap);
//	if (name.isEmpty()) return;
//	if (!areSure("Delete this map?")) return;
//	padDelete(name.toStdString());
//	ui.cbPadMap->removeItem(ui.cbPadMap->currentIndex());
//	ui.cbPadMap->setCurrentIndex(0);
}

void SetupWin::chaPadMap(int idx) {
//	idx--;
//	if (idx < 0) {
//		conf.joy.gpad->mapClear();
//	} else {
//		conf.joy.gpad->loadMap(getRFSData(ui.cbPadMap).toStdString());
//	}
//	padModel->update();
}

void SetupWin::addBinding() {
//	if (getRFSData(ui.cbPadMap).isEmpty()) return;
//	bindidx = -1;
//	xJoyMapEntry jent;
//	jent.dev = JOY_NONE;
//	jent.dev = JMAP_JOY;
#if USE_SEQ_BIND
//	jent.seq = QKeySequence();
#else
//	jent.key = ENDKEY;
#endif
//	jent.dir = XJ_NONE;
//	jent.rpt = 0;
//	padial->start(jent);
}

void SetupWin::editBinding() {
//	bindidx = ui.tvPadTable->currentIndex().row();
//	if (bindidx < 0) return;
//	padial->start(conf.joy.gpad->mapItem(bindidx));
}

void SetupWin::bindAccept(xJoyMapEntry ent) {
	((xGamepadWidget*)(ui.tabsGamepad->currentWidget()))->entryReady(ent);		// TODO: construct something more elegant

//	if (ent.type == JOY_NONE) return;
//	if (ent.dev == JMAP_NONE) return;
//	if ((bindidx < 0) || (bindidx >= (int)conf.joy.gpad->map.size())) {
//		conf.joy.gpad->setItem(-1, ent);
//	} else {
//		conf.joy.gpad->setItem(bindidx, ent);
//	}
//	conf.joy.gpad->saveMap(getRFSData(ui.cbPadMap).toStdString());
//	padModel->update();
}

//extern bool qmidx_greater(const QModelIndex, const QModelIndex);

void SetupWin::delBinding() {
//	QModelIndexList lst = ui.tvPadTable->selectionModel()->selectedRows();
//	if (!lst.isEmpty()) {
//		std::sort(lst.begin(), lst.end(), qmidx_greater);
//		if (areSure("Delete this binding(s)?")) {
//			int row;
//			foreach(QModelIndex idx, lst) {
//				row = idx.row();
//				conf.joy.gpad->delItem(row); // map.erase(conf.joy.gpad->map.begin() + row);
//			}
//			padModel->update();
//			conf.joy.gpad->saveMap(getRFSData(ui.cbPadMap).toStdString());
//		}
//	}
/*
		int row = ui.tvPadTable->currentIndex().row();
		if (row < 0) return;
		if (!areSure("Delete this binding?")) return;
		conf.joy.map.erase(conf.joy.map.begin() + row);
		padModel->update();
		padSaveConfig(getRFSData(ui.cbPadMap).toStdString());
*/
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

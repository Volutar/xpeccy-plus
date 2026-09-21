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


void opt_fill_psg_boxes(QComboBox* cbcount, QComboBox* cbtype, QComboBox* cbfrq, QComboBox* cbstereo) {
	cbcount->clear();
	cbcount->addItem(QIcon(":/images/cancel.png"),"None",0);
	cbcount->addItem(QString::fromUtf8("×1 - AY/YM"),1);
	cbcount->addItem(QString::fromUtf8("×2 - TurboSound NedoPC"),2);
	cbcount->addItem(QString::fromUtf8("×3 - TurboSound ZX Next"),3);
	cbtype->clear();
	cbtype->addItem(QIcon(":/images/MicrochipLogo.png"),"AY-3-8910",SND_AY);
	cbtype->addItem(QIcon(":/images/YamahaLogo.png"),"Yamaha 2149",SND_YM);
	cbtype->addItem(QIcon(":/images/YamahaLogo.png"),"Yamaha 2203",SND_YM2203);
	cbfrq->clear();
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

SetupWin::SetupWin(QWidget* par):QDialog(par) {
	setModal(true);
	ui.setupUi(this);
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

	ui.resbox->addItem("BASIC 48",RES_48);
	ui.resbox->addItem("BASIC 128",RES_128);
	ui.resbox->addItem("DOS",RES_DOS);
	ui.resbox->addItem("SHADOW",RES_SHADOW);

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
	ui.cbCpuTurbo->addItem(QString::fromUtf8("×1 (no turbo)"), "1");
	ui.cbCpuTurbo->addItem(QString::fromUtf8("×1, ×2"), "1,2");
	ui.cbCpuTurbo->addItem(QString::fromUtf8("×1, ×2, ×4"), "1,2,4");

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
	opt_fill_psg_boxes(ui.cbPsgCount, ui.cbPsgType, ui.cbPsgFrq, ui.cbPsgStereo);
	ui.cbPsgCount->setItemDelegate(new xTwoPartDelegate(ui.cbPsgCount));
	ui.cbPsgFrq->setItemDelegate(new xTwoPartDelegate(ui.cbPsgFrq));
	new xTwoPartPainter(ui.cbPsgCount);
	new xTwoPartPainter(ui.cbPsgFrq);
	ui.sdrvBox->addItem("None",SDRV_NONE);
	ui.sdrvBox->addItem("Covox only",SDRV_COVOX);
	ui.sdrvBox->addItem("Soundrive 1.05 mode 1",SDRV_105_1);
	ui.sdrvBox->addItem("Soundrive 1.05 mode 2",SDRV_105_2);
// flp
	ui.disklist->horizontalHeader()->setVisible(true);
	ui.diskTypeBox->addItem("None",DIF_NONE);
	ui.diskTypeBox->addItem("Beta Disk (VG93)",DIF_BDI);
	ui.diskTypeBox->addItem("+3 DOS (uPD765)",DIF_P3DOS);
	ui.disklist->addAction(ui.actCopyToTape);
	ui.disklist->addAction(ui.actSaveHobeta);
	ui.disklist->addAction(ui.actSaveRaw);
	// the order flp_format_trk_buf() lays the 16 sectors out in, for each value
	ui.cbFlpInterleave->addItem("1, 9, 2, 10, 3… (TR-DOS)", 8);
	ui.cbFlpInterleave->addItem("1, 2, 3, 4, 5… (in a row)", 1);
	ui.cbFlpInterleave->addItem("1, 3, 5, 7, 9…", 2);
	ui.cbFlpInterleave->addItem("1, 4, 7, 10, 13…", 3);
	ui.cbFlpInterleave->addItem("1, 5, 9, 13, 2…", 4);
	ui.cbFlpInterleave->addItem("1, 6, 11, 16, 2…", 5);
	ui.cbFlpInterleave->addItem("1, 7, 13, 2, 8…", 6);
	ui.cbFlpInterleave->addItem("1, 8, 15, 2, 9…", 7);
// tape
	ui.tapelist->addAction(ui.actCopyToDisk);
// hdd
	ui.hiface->addItem("None",IDE_NONE);
	ui.hiface->addItem("Nemo",IDE_NEMO);
	ui.hiface->addItem("Nemo A8",IDE_NEMOA8);
	ui.hiface->addItem("Nemo Evo",IDE_NEMO_EVO);
	ui.hiface->addItem("SMUC",IDE_SMUC);
	ui.hiface->addItem("ATM",IDE_ATM);
	ui.hiface->addItem("Profi",IDE_PROFI);
	ui.hm_type->addItem(QIcon(":/images/cancel.png"),"Not connected",IDE_NONE);
	ui.hm_type->addItem(QIcon(":/images/hdd.png"),"HDD (ATA)",IDE_ATA);
	ui.hs_type->addItem(QIcon(":/images/cancel.png"),"Not connected",IDE_NONE);
	ui.hs_type->addItem(QIcon(":/images/hdd.png"),"HDD (ATA)",IDE_ATA);
// input
//	padModel = new xPadMapModel();
//	ui.tvPadTable->setModel(padModel);
//	ui.tvPadTable->addAction(ui.actAddBinding);
//	ui.tvPadTable->addAction(ui.actEditBinding);
//	ui.tvPadTable->addAction(ui.actDelBinding);
	ui.tabsGamepad->addTab(gpwid_a, "Gamepad A");
	ui.tabsGamepad->addTab(gpwid_b, "Gamepad B");
	ui.cbScanTab->addItem("As the machine has it", 0);
	ui.cbScanTab->addItem("Scanset 1 (XT)", KBD_XT);
	ui.cbScanTab->addItem("Scanset 2 (AT)", KBD_AT);
	ui.cbScanTab->addItem("Scanset 3 (PS/2)", KBD_PS2);
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

	// same for the set file by file: the page shows which file is in which
	// slot, this window has the offsets and sizes behind it
	romWin = popOut(ui.romAdvBox, "Machine: ROM files");
	romWin->resize(620, 340);
	connect(ui.pbRomAdvanced, SIGNAL(released()), this, SLOT(showRomFiles()));
// media
	ftbox = new xFileTypesBox;
	ftWin = popOut(ftbox, "Media: file types");
	ftWin->resize(520, ftWin->sizeHint().height());
	connect(ui.pbFileTypes, &QPushButton::released, ftWin, [this]() {ftWin->show(); ftWin->raise();});
	QPushButton* ftdef = ftWin->findChild<QDialogButtonBox*>()->addButton(QDialogButtonBox::RestoreDefaults);
	ftdef->setToolTip("Every format back to Auto");
	connect(ftdef, &QPushButton::released, ftbox, [this]() {ftbox->defaults();});
// video
	connect(ui.pathtb,SIGNAL(released()),this,SLOT(selsspath()));
	connect(ui.bszsld,SIGNAL(valueChanged(int)),this,SLOT(chabsz()));
	connect(ui.sldSpeed,SIGNAL(valueChanged(int)),this,SLOT(chaspd()));
	connect(ui.sldNoflic,SIGNAL(valueChanged(int)),this,SLOT(chaflc()));
	connect(ui.sldPsgSep,SIGNAL(valueChanged(int)),this,SLOT(chapsg()));
	connect(ui.sldSndLatency,SIGNAL(valueChanged(int)),this,SLOT(chasndlat()));
	connect(ui.cbPsgCount,SIGNAL(currentIndexChanged(int)),this,SLOT(chapsg()));
	connect(ui.cbSnow,SIGNAL(toggled(bool)),this,SLOT(chasnow()));
	connect(ui.cbPsgStereo,SIGNAL(currentIndexChanged(int)),this,SLOT(chapsg()));

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

// dos
	connect(ui.newatb,SIGNAL(released()),this,SLOT(newa()));
	connect(ui.newbtb,SIGNAL(released()),this,SLOT(newb()));
	connect(ui.newctb,SIGNAL(released()),this,SLOT(newc()));
	connect(ui.newdtb,SIGNAL(released()),this,SLOT(newd()));

	connect(ui.loadatb,SIGNAL(released()),this,SLOT(loada()));
	connect(ui.loadbtb,SIGNAL(released()),this,SLOT(loadb()));
	connect(ui.loadctb,SIGNAL(released()),this,SLOT(loadc()));
	connect(ui.loaddtb,SIGNAL(released()),this,SLOT(loadd()));

	connect(ui.saveatb,SIGNAL(released()),this,SLOT(savea()));
	connect(ui.savebtb,SIGNAL(released()),this,SLOT(saveb()));
	connect(ui.savectb,SIGNAL(released()),this,SLOT(savec()));
	connect(ui.savedtb,SIGNAL(released()),this,SLOT(saved()));

	connect(ui.remoatb,SIGNAL(released()),this,SLOT(ejcta()));
	connect(ui.remobtb,SIGNAL(released()),this,SLOT(ejctb()));
	connect(ui.remoctb,SIGNAL(released()),this,SLOT(ejctc()));
	connect(ui.remodtb,SIGNAL(released()),this,SLOT(ejctd()));

	connect(ui.disktabs,SIGNAL(currentChanged(int)),this,SLOT(fillDiskCat()));
	connect(ui.actCopyToTape,SIGNAL(triggered()),this,SLOT(copyToTape()));
	connect(ui.actSaveHobeta,SIGNAL(triggered()),this,SLOT(diskToHobeta()));
	connect(ui.actSaveRaw,SIGNAL(triggered()),this,SLOT(diskToRaw()));
	connect(ui.tbToTape,SIGNAL(released()),this,SLOT(copyToTape()));
	connect(ui.tbToHobeta,SIGNAL(released()),this,SLOT(diskToHobeta()));
	connect(ui.tbToRaw,SIGNAL(released()),this,SLOT(diskToRaw()));
// tape
	connect(ui.tapelist,SIGNAL(doubleClicked(QModelIndex)),this,SLOT(chablock(QModelIndex)));
	connect(ui.tapelist,SIGNAL(clicked(QModelIndex)),this,SLOT(tlistclick(QModelIndex)));
	connect(ui.tloadtb,SIGNAL(released()),this,SLOT(loatape()));
	connect(ui.tsavetb,SIGNAL(released()),this,SLOT(savtape()));
	connect(ui.tremotb,SIGNAL(released()),this,SLOT(ejctape()));
	connect(ui.blkuptb,SIGNAL(released()),this,SLOT(tblkup()));
	connect(ui.blkdntb,SIGNAL(released()),this,SLOT(tblkdn()));
	connect(ui.blkrmtb,SIGNAL(released()),this,SLOT(tblkrm()));
	connect(ui.actCopyToDisk,SIGNAL(triggered()),this,SLOT(copyToDisk()));
	connect(ui.tbToDisk,SIGNAL(released()),this,SLOT(copyToDisk()));
	connect(ui.sldTapeSpeed, &QSlider::valueChanged, this, [this](int v){
		ui.labTapeSpeedVal->setText(QString("%0%").arg(v));
	});
// hdd
	connect(ui.hm_pathtb,SIGNAL(released()),this,SLOT(hddMasterImg()));
	connect(ui.hs_pathtb,SIGNAL(released()),this,SLOT(hddSlaveImg()));
	connect(ui.hm_pathdir,SIGNAL(released()),this,SLOT(hddMasterDir()));
	connect(ui.hs_pathdir,SIGNAL(released()),this,SLOT(hddSlaveDir()));
// external
	connect(ui.tbSDCimg,SIGNAL(released()),this,SLOT(selSDCimg()));
	connect(ui.tbSDCdir,SIGNAL(released()),this,SLOT(selSDCdir()));
	connect(ui.sdPath,SIGNAL(textChanged(QString)),this,SLOT(sdcPathChanged()));
	connect(ui.tbsdcfree,SIGNAL(released()),ui.sdPath,SLOT(clear()));
	connect(ui.cSlotOpen,SIGNAL(released()),this,SLOT(openSlot()));
	connect(ui.cSlotEject,SIGNAL(released()),this,SLOT(ejectSlot()));
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
	rsmodel->fill(&roms);
	fillRomSlots();
	ui.machbox->setCurrentIndex(ui.machbox->findData(QString::fromLocal8Bit(conf.macId.c_str())));
	ui.resbox->setCurrentIndex(ui.resbox->findData(comp->resbank));
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
	while (ui.cbCpuTurbo->count() > CPU_TURBO_ROWS)
		ui.cbCpuTurbo->removeItem(CPU_TURBO_ROWS);
	if (ui.cbCpuTurbo->findData(steps) < 0)
		ui.cbCpuTurbo->addItem(steps, steps);
	setRFIndex(ui.cbCpuTurbo, steps);
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
	ui.cbGS->setChecked(comp->gs->enable);
	ui.gsrbox->setChecked(comp->gs->reset);

	ui.sdrvBox->setCurrentIndex(ui.sdrvBox->findData(comp->sdrv->type));

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

	int chips = (comp->ts->type == TS_ZXNEXT) ? 3 : (comp->ts->type == TS_NEDOPC) ? 2 : 1;
	if (comp->ts->chipA->type == SND_NONE) chips = 0;
	setRFIndex(ui.cbPsgCount, chips);
	setRFIndex(ui.cbPsgType, (comp->ts->chipA->type == SND_NONE) ? SND_AY : comp->ts->chipA->type);
	setRFIndex(ui.cbPsgStereo, comp->ts->chipA->stereo);
	opt_set_psg_frq(ui.cbPsgFrq, comp->ts->chipA->frq);
	ui.sldPsgSep->setValue(comp->ts->chipA->sep);
	chapsg();
// input
	buildkeylist();
	setRFIndex(ui.cbScanTab, comp->keyb->pcmode);
	idx = ui.keyMapBox->findText(QString(conf.kmapName.c_str()));
	if (idx < 1) idx = 0;
	ui.keyMapBox->setCurrentIndex(idx);
	ui.ratEnable->setChecked(comp->mouse->enable);
	ui.ratWheel->setChecked(comp->mouse->hasWheel);
	ui.cbSwapButtons->setChecked(comp->mouse->swapButtons);
	ui.sldSensitivity->setValue(comp->mouse->sensitivity * 1000.0f);
	ui.cbKbuttons->setChecked(comp->joy->extbuttons);
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
	ui.diskTypeBox->setCurrentIndex(ui.diskTypeBox->findData(comp->dif->type));
	ui.bdtbox->setChecked(fdcFlag & FDC_FAST);
	ui.mempaths->setChecked(conf.storePaths);
	ui.cbAutorun->setChecked(conf.autorun);
	ftbox->fill();
	ui.cbAddBoot->setChecked(conf.boot);
	setRFIndex(ui.cbFlpInterleave, flp_get_interleave());
	Floppy* flp = comp->dif->flp[0];
	ui.apathle->setText(QString::fromLocal8Bit(flp->path));
		ui.a80box->setChecked(flp->trk80);
		ui.adsbox->setChecked(flp->doubleSide);
		ui.awpbox->setChecked(flp->protect);
	flp = comp->dif->flp[1];
	ui.bpathle->setText(QString::fromLocal8Bit(flp->path));
		ui.b80box->setChecked(flp->trk80);
		ui.bdsbox->setChecked(flp->doubleSide);
		ui.bwpbox->setChecked(flp->protect);
	flp = comp->dif->flp[2];
	ui.cpathle->setText(QString::fromLocal8Bit(flp->path));
		ui.c80box->setChecked(flp->trk80);
		ui.cdsbox->setChecked(flp->doubleSide);
		ui.cwpbox->setChecked(flp->protect);
	flp = comp->dif->flp[3];
	ui.dpathle->setText(QString::fromLocal8Bit(flp->path));
		ui.d80box->setChecked(flp->trk80);
		ui.ddsbox->setChecked(flp->doubleSide);
		ui.dwpbox->setChecked(flp->protect);
	fillDiskCat();
// hdd
	ui.hiface->setCurrentIndex(ui.hiface->findData(comp->ide->type));

	ui.hm_type->setCurrentIndex(ui.hm_type->findData(comp->ide->master->type));
	ATAPassport pass = ideGetPassport(comp->ide,IDE_MASTER);
	ui.hm_path->setText(QString::fromLocal8Bit(comp->ide->master->image));
	ui.hm_islba->setChecked(comp->ide->master->hasLBA);
	ui.hm_gsec->setValue(pass.spt);
	ui.hm_ghd->setValue(pass.hds);
	ui.hm_gcyl->setValue(pass.cyls);
	ui.hm_glba->setValue(comp->ide->master->maxlba);
	ui.hm_capacity->setValue(comp->ide->master->maxlba >> 11);

	ui.hs_type->setCurrentIndex(ui.hm_type->findData(comp->ide->slave->type));
	pass = ideGetPassport(comp->ide,IDE_SLAVE);
	ui.hs_path->setText(QString::fromLocal8Bit(comp->ide->slave->image));
	ui.hs_islba->setChecked(comp->ide->slave->hasLBA);
	ui.hs_gsec->setValue(pass.spt);
	ui.hs_ghd->setValue(pass.hds);
	ui.hs_gcyl->setValue(pass.cyls);
	ui.hs_glba->setValue(comp->ide->slave->maxlba);
	ui.hs_capacity->setValue(comp->ide->slave->maxlba >> 11);
// external
	ui.sdPath->setText(QString::fromLocal8Bit(comp->sdc->image));
	ui.sdlock->setChecked(comp->sdc->lock);
	sdcPathChanged();

	ui.cSlotName->setText(comp->slot->path);
// tape
	ui.cbTapeAuto->setChecked(conf.tape.autostart);
	ui.cbTapeFast->setChecked(conf.tape.fast);
	ui.cbTapeRewind->setChecked(conf.tape.rewind);
	ui.sldTapeSpeed->setValue(comp->tape->speed);	// the readout follows in the slot
	ui.tpathle->setText(QString::fromLocal8Bit(comp->tape->path));
	buildtapelist();
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
	comp->resbank = getRFIData(ui.resbox);
	memSetSize(comp->mem, getRFIData(ui.mszbox), -1);
	compSetBaseFrq(comp, xcpu_frq_parse(ui.cbCpuFrq->currentText(), comp->cpuFrq));
	xm_turbo_set(comp, getRFSData(ui.cbCpuTurbo).toStdString());
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

	int chips = getRFIData(ui.cbPsgCount);
	int chtype = getRFIData(ui.cbPsgType);
	double chfrq = opt_get_psg_frq(ui.cbPsgFrq);
	int chstereo = getRFIData(ui.cbPsgStereo);
	int chsep = ui.sldPsgSep->value();
	aymChip* chip[3] = {comp->ts->chipA, comp->ts->chipB, comp->ts->chipC};
	for (int i = 0; i < 3; i++) {			// one setting for all the chips
		chip[i]->frq = chfrq;
		chip_set_type(chip[i], (i < chips) ? chtype : SND_NONE);
		chip[i]->stereo = chstereo;
		chip[i]->sep = chsep;
	}
	comp->ts->type = (chips > 2) ? TS_ZXNEXT : (chips > 1) ? TS_NEDOPC : TS_NONE;

	comp->gs->enable = ui.cbGS->isChecked() ? 1 : 0;
	comp->gs->reset = ui.gsrbox->isChecked() ? 1 : 0;

	comp->sdrv->type = getRFIData(ui.sdrvBox);

	comp->saa->enabled = ui.cbSAA->isChecked() ? 1 : 0;
// input
	comp->keyb->pcmode = getRFIData(ui.cbScanTab);
	comp->mouse->enable = ui.ratEnable->isChecked() ? 1 : 0;
	comp->mouse->hasWheel = ui.ratWheel->isChecked() ? 1 : 0;
	comp->mouse->swapButtons = ui.cbSwapButtons->isChecked() ? 1 : 0;
	comp->mouse->sensitivity = ui.sldSensitivity->value() * 0.001f;
	comp->joy->extbuttons = ui.cbKbuttons->isChecked() ? 1 : 0;
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
	difSetHW(comp->dif, getRFIData(ui.diskTypeBox));
	setFlagBit(ui.bdtbox->isChecked(),&fdcFlag,FDC_FAST);
	conf.boot = ui.cbAddBoot->isChecked() ? 1 : 0;
	conf.storePaths = ui.mempaths->isChecked() ? 1 : 0;
	conf.autorun = ui.cbAutorun->isChecked() ? 1 : 0;
	ftbox->apply();
	flp_set_interleave(getRFIData(ui.cbFlpInterleave));

	Floppy* flp = comp->dif->flp[0];
	flp->trk80 = ui.a80box->isChecked() ? 1 : 0;
	flp->doubleSide = ui.adsbox->isChecked() ? 1 : 0;
	flp->protect = ui.awpbox->isChecked() ? 1 : 0;

	flp = comp->dif->flp[1];
	flp->trk80 = ui.b80box->isChecked() ? 1 : 0;
	flp->doubleSide = ui.bdsbox->isChecked() ? 1 : 0;
	flp->protect = ui.bwpbox->isChecked() ? 1 : 0;

	flp = comp->dif->flp[2];
	flp->trk80 = ui.c80box->isChecked() ? 1 : 0;
	flp->doubleSide = ui.cdsbox->isChecked() ? 1 : 0;
	flp->protect = ui.cwpbox->isChecked() ? 1 : 0;

	flp = comp->dif->flp[3];
	flp->trk80 = ui.d80box->isChecked() ? 1 : 0;
	flp->doubleSide = ui.ddsbox->isChecked() ? 1 : 0;
	flp->protect = ui.dwpbox->isChecked() ? 1 : 0;

// hdd
	//comp->ide->type = getRFIData(ui.hiface);
	ide_set_type(comp->ide, getRFIData(ui.hiface));

	comp->ide->master->type = getRFIData(ui.hm_type);
	ide_mount(comp->ide, IDE_MASTER, ui.hm_path->text());
	comp->ide->master->hasLBA = ui.hm_islba->isChecked() ? 1 : 0;

	comp->ide->slave->type = getRFIData(ui.hs_type);
	ide_mount(comp->ide, IDE_SLAVE, ui.hs_path->text());
	comp->ide->slave->hasLBA = ui.hs_islba->isChecked() ? 1 : 0;
// others
	sdc_mount(comp->sdc, ui.sdPath->text());
	sdcSetLock(comp->sdc, ui.sdlock->isChecked() ? 1 : 0);
// tape
	conf.tape.autostart = ui.cbTapeAuto->isChecked() ? 1 : 0;
	conf.tape.fast = ui.cbTapeFast->isChecked() ? 1 : 0;
	conf.tape.rewind = ui.cbTapeRewind->isChecked() ? 1 : 0;
	comp->tape->speed = ui.sldTapeSpeed->value();
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
// of it is read and where it lands are in the window behind Advanced.

#define	RSLOT_GS	-1
#define	RSLOT_FONT	-2

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

// how far a rom file reaches, in 16K banks

static int rom_file_banks(const xRomFile& rf) {
	int size = rf.fsize * 1024;
	if (size <= 0) {
		QFileInfo inf(QString::fromLocal8Bit(xm_rom_path(rf.name).c_str()));
		size = inf.size() - rf.foffset * 1024;
	}
	return (size + MEM_16K - 1) / MEM_16K;
}

// a bank with no file of its own may still be covered by a big one in a bank
// before it: for each bank, the slot it comes from, or -1

static QVector<int> rom_slot_cover(const xRomset& rs, int banks) {
	QVector<int> res(banks, -1);
	foreach(xRomFile rf, rs.roms) {
		int first = rf.roffset / 16;
		int last = first + rom_file_banks(rf);
		for (int i = first + 1; (i < last) && (i < banks); i++) {
			if (res[i] < 0) res[i] = first;
		}
	}
	return res;
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
	QVector<int> from = rom_slot_cover(roms, mac->romBanks);
	int row = 0;
	for (int i = 0; i < mac->romBanks; i++)
		addRomSlot(row++, QString("ROM %0").arg(i), i, files, true, from[i]);
	addRomSlot(row++, "GS", RSLOT_GS, files, conf.zx->gs->enable, -1);
	// only a machine with a text mode draws from a font rom
	addRomSlot(row++, "Font", RSLOT_FONT, files, !mac->roms.fntFile.empty(), -1);
	// spare height under the rows, so they do not spread out
	ui.gridRomSlots->addItem(new QSpacerItem(20, 0, QSizePolicy::Minimum,
		QSizePolicy::Expanding), row, 0);
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
	lab->setMinimumWidth(60);
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
	ui.gridRomSlots->addWidget(lab, row, 0);
	ui.gridRomSlots->addWidget(box, row, 1);
	ui.gridRomSlots->addWidget(btn, row, 2);
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

void SetupWin::buildtapelist() {
	ui.tapelist->fill(conf.zx->tape);
}

void SetupWin::copyToTape() {
	int dsk = ui.disktabs->currentIndex();
	QModelIndexList idx = ui.disklist->selectionModel()->selectedRows();
	if (idx.size() == 0) return;
	Computer* comp = conf.zx;
	TRFile cat[128];
	diskGetTRCatalog(comp->dif->flp[dsk],cat);
	int row;
	unsigned char* buf = new unsigned char[0xffff];
	unsigned short line,start,len;
	char name[10];
	int savedFiles = 0;
	for (int i=0; i<idx.size(); i++) {
		row = idx[i].row();
		if (diskGetSectorsData(comp->dif->flp[dsk],cat[row].trk, cat[row].sec+1, buf, cat[row].slen)) {
			if (cat[row].slen == (cat[row].hlen + ((cat[row].llen == 0) ? 0 : 1))) {
				start = ((cat[row].hst << 8) + cat[row].lst) & 0xffff;
				len = ((cat[row].hlen << 8) + cat[row].llen) & 0xffff;
				line = (cat[row].ext == 'B') ? (buf[start] + (buf[start+1] << 8)) & 0xffff : 0x8000;
				memset(name,0x20,10);
				memcpy(name,(char*)cat[row].name,8);
				tapAddFile(comp->tape,name,(cat[row].ext == 'B') ? 0 : 3, start, len, line, buf,true);
				savedFiles++;
			} else {
				shitHappens("File seems to be joined, skip");
			}
		} else {
			shitHappens("Can't get file data, skip");
		}
	}
	buildtapelist();
	std::string msg = std::string(int2str(savedFiles)) + " of " + int2str(idx.size()) + " files copied";
	showInfo(msg.c_str());
}

// hobeta header crc = ((105 + 257 * std::accumulate(data, data + 15, 0u)) & 0xffff))

void SetupWin::diskToHobeta() {
	QModelIndexList idx = ui.disklist->selectionModel()->selectedRows();
	if (idx.size() == 0) return;
	QString dir = QFileDialog::getExistingDirectory(this,"Save file(s) to...","",QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly);
	if (dir == "") return;
	Computer* comp = conf.zx;
	std::string sdir = std::string(dir.toLocal8Bit().data()) + SLASH;
	Floppy* flp = comp->dif->flp[ui.disktabs->currentIndex()];		// selected floppy
	int savedFiles = 0;
	for (int i=0; i<idx.size(); i++) {
		if (saveHobetaFile(flp,idx[i].row(),sdir.c_str()) == ERR_OK) savedFiles++;
	}
	std::string msg = std::string(int2str(savedFiles)) + " of " + int2str(idx.size()) + " files saved";
	showInfo(msg.c_str());
}

void SetupWin::diskToRaw() {
	QModelIndexList idx = ui.disklist->selectionModel()->selectedRows();
	if (idx.size() == 0) return;
	QString dir = QFileDialog::getExistingDirectory(this,"Save file(s) to...","",QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly);
	if (dir == "") return;
	Computer* comp = conf.zx;
	std::string sdir = std::string(dir.toLocal8Bit().data()) + SLASH;
	Floppy* flp = comp->dif->flp[ui.disktabs->currentIndex()];
	int savedFiles = 0;
	for (int i=0; i<idx.size(); i++) {
		if (saveRawFile(flp,idx[i].row(),sdir.c_str()) == ERR_OK) savedFiles++;
	}
	std::string msg = std::string(int2str(savedFiles)) + " of " + int2str(idx.size()) + " files saved";
	showInfo(msg.c_str());
}

void SetupWin::copyToDisk() {
	QModelIndexList idl = ui.tapelist->selectionModel()->selectedRows();
	if (idl.size() < 1) return;
	int dsk = ui.disktabs->currentIndex();
	if (dsk < 0) dsk = 0;
	if (dsk > 3) dsk = 3;
	Computer* comp = conf.zx;
	if (!tape_disk_ready(comp, dsk)) return;
	QString msg;
	int ok = tape_blk_to_disk(comp->tape, idl.first().row(), comp->dif->flp[dsk], &msg);
	updatedisknams();
	fillDiskCat();
	if (ok) {
		showInfo(msg.toLocal8Bit().constData());
	} else {
		shitHappens(msg.toLocal8Bit().constData());
	}
}

void SetupWin::fillDiskCat() {
	int dsk = ui.disktabs->currentIndex();
	Computer* comp = conf.zx;
	Floppy* flp = comp->dif->flp[dsk];
	TRFile ct[128];
	QList<TRFile> cat;
	int catSize = 0;
	ui.disklist->setEnabled(flp->insert);
	if (flp->insert && (diskGetType(flp) == DISK_TYPE_TRD)) {
		catSize = diskGetTRCatalog(flp, ct);
		for(int i = 0; i < catSize; i++) {
			if (ct[i].name[0] > 0x1f)
				cat.append(ct[i]);
		}
	}
	ui.disklist->setCatalog(cat);
}

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
	int chips = getRFIData(ui.cbPsgCount);
	int split = (chips > 0) && (getRFIData(ui.cbPsgStereo) != AY_MONO);
	ui.labPsgSep->setText(QString("%0%").arg(ui.sldPsgSep->value()));
	ui.cbPsgType->setEnabled(chips > 0);
	ui.cbPsgFrq->setEnabled(chips > 0);
	ui.cbPsgStereo->setEnabled(chips > 0);
	ui.sldPsgSep->setEnabled(split);
	ui.labPsgSep->setEnabled(split);
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

// disk

void SetupWin::newdisk(int idx, int ask) {
	Computer* comp = conf.zx;
	Floppy *flp = comp->dif->flp[idx];
	if (saveChangedDisk(comp,idx & 3) != ERR_OK) return;
	flp_insert(flp, NULL);
	// diskClear(flp);
	flp->changed = 1;
	if (ask && areSure("Format for TRDOS?")) {
		trd_format(flp);
	}
	updatedisknams();
}

void SetupWin::newa() {newdisk(0,1);}
void SetupWin::newb() {newdisk(1,1);}
void SetupWin::newc() {newdisk(2,1);}
void SetupWin::newd() {newdisk(3,1);}

void SetupWin::loada() {load_file(conf.zx, NULL, FH_DRIVE_A, 0); updatedisknams();}
void SetupWin::loadb() {load_file(conf.zx, NULL, FH_DRIVE_B, 1); updatedisknams();}
void SetupWin::loadc() {load_file(conf.zx, NULL, FH_DRIVE_C, 2); updatedisknams();}
void SetupWin::loadd() {load_file(conf.zx, NULL, FH_DRIVE_D, 3); updatedisknams();}

void SetupWin::savea() {Computer* comp = conf.zx; Floppy* flp = comp->dif->flp[0]; if (flp->insert) save_file(comp, flp->path, FG_DISK_A, 0); updatedisknams();}
void SetupWin::saveb() {Computer* comp = conf.zx; Floppy* flp = comp->dif->flp[1]; if (flp->insert) save_file(comp, flp->path, FG_DISK_B, 1); updatedisknams();}
void SetupWin::savec() {Computer* comp = conf.zx; Floppy* flp = comp->dif->flp[2]; if (flp->insert) save_file(comp, flp->path, FG_DISK_C, 2); updatedisknams();}
void SetupWin::saved() {Computer* comp = conf.zx; Floppy* flp = comp->dif->flp[3]; if (flp->insert) save_file(comp, flp->path, FG_DISK_D, 3); updatedisknams();}

void SetupWin::ejcta() {Computer* comp = conf.zx; saveChangedDisk(comp,0); flp_eject(comp->dif->flp[0]); updatedisknams();}
void SetupWin::ejctb() {Computer* comp = conf.zx; saveChangedDisk(comp,1); flp_eject(comp->dif->flp[1]); updatedisknams();}
void SetupWin::ejctc() {Computer* comp = conf.zx; saveChangedDisk(comp,2); flp_eject(comp->dif->flp[2]); updatedisknams();}
void SetupWin::ejctd() {Computer* comp = conf.zx; saveChangedDisk(comp,3); flp_eject(comp->dif->flp[3]); updatedisknams();}

void SetupWin::updatedisknams() {
	Computer* comp = conf.zx;
	ui.apathle->setText(QString::fromLocal8Bit(comp->dif->flp[0]->path));
	ui.bpathle->setText(QString::fromLocal8Bit(comp->dif->flp[1]->path));
	ui.cpathle->setText(QString::fromLocal8Bit(comp->dif->flp[2]->path));
	ui.dpathle->setText(QString::fromLocal8Bit(comp->dif->flp[3]->path));
	fillDiskCat();
}

// tape

void SetupWin::loatape() {
	Computer* comp = conf.zx;
	load_file(comp, NULL, FG_TAPE, -1);
	ui.tpathle->setText(QString::fromLocal8Bit(comp->tape->path));
	buildtapelist();
}

void SetupWin::savtape() {
	Computer* comp = conf.zx;
	if (comp->tape->blkCount != 0) {
		save_file(comp, comp->tape->path, FG_TAPE, -1);
	}
}

void SetupWin::ejctape() {
	Computer* comp = conf.zx;
	tapEject(comp->tape);
	ui.tpathle->setText(QString::fromLocal8Bit(comp->tape->path));
	buildtapelist();
}

void SetupWin::tblkup() {
	ui.tapelist->blkMove(conf.zx->tape, -1);
}

void SetupWin::tblkdn() {
	ui.tapelist->blkMove(conf.zx->tape, 1);
}

void SetupWin::tblkrm() {
	ui.tapelist->blkDel(conf.zx->tape);
}

void SetupWin::chablock(QModelIndex idx) {
	Computer* comp = conf.zx;
	int row = idx.row();
	tapRewind(comp->tape,row);
	buildtapelist();
//	ui.tapelist->selectRow(row);
}

void SetupWin::tlistclick(QModelIndex idx) {
	int row = idx.row();
	int col = idx.column();
	Computer* comp = conf.zx;
	if ((row < 0) || (row >= comp->tape->blkCount)) return;
	if (col != TCC_BRK) return;
	comp->tape->blkData[row].breakPoint ^= 1;
	buildtapelist();
//	ui.tapelist->selectRow(row);
}

// hdd

// show what the device reports after it was given an image or a folder
void SetupWin::hddShowGeom(int wut) {
	ATADev* dev = (wut == IDE_MASTER) ? conf.zx->ide->master : conf.zx->ide->slave;
	QSpinBox* cyl = (wut == IDE_MASTER) ? ui.hm_gcyl : ui.hs_gcyl;
	QSpinBox* sec = (wut == IDE_MASTER) ? ui.hm_gsec : ui.hs_gsec;
	QSpinBox* hds = (wut == IDE_MASTER) ? ui.hm_ghd : ui.hs_ghd;
	QSpinBox* lba = (wut == IDE_MASTER) ? ui.hm_glba : ui.hs_glba;
	QSpinBox* cap = (wut == IDE_MASTER) ? ui.hm_capacity : ui.hs_capacity;
	cyl->setValue(dev->pass.cyls);
	sec->setValue(dev->pass.spt);
	hds->setValue(dev->pass.hds);
	lba->setValue(dev->maxlba);
	cap->setValue(dev->maxlba >> 11);		// 512 (sector) -> 1024*1024 (Mb)
}

void SetupWin::hddMasterImg() {
	QString path = QFileDialog::getOpenFileName(this,"Image for master HDD","","All files (*)",NULL,QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite);
	if (path.isEmpty()) return;
	ui.hm_path->setText(path);
	ide_mount(conf.zx->ide, IDE_MASTER, path);
	hddShowGeom(IDE_MASTER);
}

// a host folder is served as a read only disk
void SetupWin::hddMasterDir() {
	QString path = QFileDialog::getExistingDirectory(this,"Folder to serve as master HDD","",QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly);
	if (path.isEmpty()) return;
	ui.hm_path->setText(path);
	ide_mount(conf.zx->ide, IDE_MASTER, path);
	hddShowGeom(IDE_MASTER);
}

void SetupWin::hddSlaveDir() {
	QString path = QFileDialog::getExistingDirectory(this,"Folder to serve as slave HDD","",QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly);
	if (path.isEmpty()) return;
	ui.hs_path->setText(path);
	ide_mount(conf.zx->ide, IDE_SLAVE, path);
	hddShowGeom(IDE_SLAVE);
}

void SetupWin::hddSlaveImg() {
	QString path = QFileDialog::getOpenFileName(this,"Image for slave HDD","","All files (*)",NULL,QFileDialog::DontUseNativeDialog | QFileDialog::DontConfirmOverwrite);
	if (path.isEmpty()) return;
	ui.hs_path->setText(path);
	ide_mount(conf.zx->ide, IDE_SLAVE, path);
	hddShowGeom(IDE_SLAVE);
}

/*
void SetupWin::hddcap() {
	int sz;
	if (ui.hs_islba->isChecked()) {
		sz = (ui.hs_glba->value() >> 9);
	} else {
		sz = ((ui.hs_gsec->value() * (ui.hs_ghd->value() + 1) * (ui.hs_gcyl->value() + 1)) >> 11);
	}
	ui.hs_capacity->setValue(sz);
}
*/

// external

void SetupWin::selSDCimg() {
	QString fnam = QFileDialog::getOpenFileName(this,"Image for SD card","","All files (*.*)",nullptr,QFileDialog::DontUseNativeDialog);
	if (!fnam.isEmpty()) ui.sdPath->setText(fnam);
}

// a host folder is served as a read only card, so the lock box follows the path
void SetupWin::selSDCdir() {
	QString fnam = QFileDialog::getExistingDirectory(this,"Folder to serve as SD card","",QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly);
	if (!fnam.isEmpty()) ui.sdPath->setText(fnam);
}

void SetupWin::sdcPathChanged() {
	bool dir = !ui.sdPath->text().isEmpty() && QFileInfo(ui.sdPath->text()).isDir();
	if (dir) ui.sdlock->setChecked(true);
	ui.sdlock->setEnabled(!dir);
}

void SetupWin::openSlot() {
	Computer* comp = conf.zx;
//	if (fnam.isEmpty()) return;
//	ui.cSlotName->setText(fnam);
//	loadFile(comp, fnam.toLocal8Bit().data(), FT_SLOT_A, 0);
	if (load_file(comp, NULL, FH_SLOTS, 0) == ERR_OK) {
		ui.cSlotName->setText(comp->slot->path);
	}
}

int testSlotOn(Computer*);

void SetupWin::ejectSlot() {
	Computer* comp = conf.zx;
	sltEject(comp->slot);
	ui.cSlotName->clear();
	if (testSlotOn(comp))
		compReset(comp,RES_DEFAULT);
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

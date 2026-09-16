#include "xcore/xcore.h"
#include "xcore/sound.h"

#include <stdio.h>

#include <QIcon>
#include <QDebug>
#include <QBuffer>
#include <QPainter>
#include <QStyleOptionButton>
#include <QShowEvent>
#include <QTabBar>
#include <QVector>
#include <QFileDialog>
#include <QTemporaryFile>
#include <QTextCodec>

#include "debuger.h"
#include "dbg_sprscan.h"
#include "filer.h"
#include "../xgui.h"
#include "../portwatch.h"

// debuga.layout format: bump whenever the set of docks changes, so an older
// file is refused instead of scattering panels it knows nothing about.
// 1 = cpu, disasm, misc and stack became docks (the MISCTOOLBAR is gone)
// 2 = empty anchor strips at the left and right edges
// 3 = the memory map panel is gone, its controls sit in MEMMAP
#define DBG_LAYOUT_VERSION	3

// The cpu panel's geometry. One register takes three grid columns - name, value
// and a gap after it - so layout column k starts on grid column k * RCOL_STEP
#define RCOL_STEP	3
#define RCOL_GAP	10
// the flags grid: between its cells, and either side of it when it stands as a
// column of its own. HEAD_SPLIT parts the two halves of the panel and of the
// title bar over it, so the divider in the header lands on the same pixel
#define FLAG_SPACING	2
#define FLAG_MARGIN	4
#define HEAD_SPLIT	2

static QDockWidget* make_edge_anchor(const char*);

int blockStart = -1;
int blockEnd = -1;

int tmpcnt = 0;

int getRFIData(QComboBox*);
void setRFIndex(QComboBox*, QVariant, int);
int dasmSome(Computer*, int, dasmData&);

// trace type
enum {
	DBG_TRACE_ALL = 0x100,
	DBG_TRACE_INT,
	DBG_TRACE_KEY,
	DBG_TRACE_HERE,
	DBG_TRACE_LOG
};

enum {
	NES_SCR_OFF = 0x00,
	NES_SCR_0,
	NES_SCR_1,
	NES_SCR_2,
	NES_SCR_3,
	NES_SCR_ALL,
	NES_SCR_TILES
};

enum {
	NES_TILE_0000 = 0x0000,
	NES_TILE_1000 = 0x1000
};

typedef struct {
	QLabel* name;
	QLineEdit* edit;
} dbgRegPlace;

// #define SETCOLOR(_n, _r) col = conf.pal[_n]; if (col.isValid()) pal.setColor(_r, col)

#define FLG_ALTCOLOR	1
#define FLG_LIGHTER	(1<<1)

QString getStyleString(QString bgcn, QString txcn, int f = 0, int g = 100) {
	QString str;
	QColor col = conf.pal[bgcn];
	if (col.isValid()) {
		if (f & FLG_LIGHTER) {
			str.append(QString("background-color:%0;").arg(col.lighter(g).name()));
		} else {
			str.append(QString("background-color:%0;").arg(col.name()));
		}
		if (f & FLG_ALTCOLOR) {
			str.append(QString("alternate-background-color:%0;").arg(col.lighter(g).name()));
		}
	}
	col = conf.pal[txcn];
	if (col.isValid()) str.append(QString("color:%0;").arg(col.name()));
	return str;
}

// gap from whatever's above (native titlebar, another dock or a tab bar), which
// would otherwise touch it with no border - dbg.header.bg colours both alike
#define	HEADER_GAP	2
static const QString headerGap = QString("margin-top:%0px;").arg(HEADER_GAP);

// A header that answers the right button says so with a dot in its corner:
// nothing else on the panel shows a menu is there. It is a child of the header,
// so it hides and dies with it, and it lets the click through to the header
// underneath. dbg.header.txt keeps it visible whatever the style does.

#define	MENU_DOT_SIZE	4
#define	MENU_DOT_EDGE	3	// from the left
#define	MENU_DOT_TOP	2	// from the top of the coloured plate

class xMenuDot : public QWidget {
	public:
		xMenuDot(QWidget* par, int top):QWidget(par) {
			setAttribute(Qt::WA_TransparentForMouseEvents);
			setFixedSize(MENU_DOT_SIZE, MENU_DOT_SIZE);
			move(MENU_DOT_EDGE, top);
			show();
		}
	protected:
		void paintEvent(QPaintEvent*) {
			QColor col = conf.pal["dbg.header.txt"];
			if (!col.isValid()) col = palette().color(QPalette::WindowText);
			QPainter pnt(this);
			pnt.setRenderHint(QPainter::Antialiasing);
			pnt.setPen(Qt::NoPen);
			pnt.setBrush(col);
			pnt.drawEllipse(rect());
		}
};

// a dock title sits below the gap its style sheet leaves, a panel label doesn't
static void markMenuHeader(QWidget* wid, int gap = 0) {
	new xMenuDot(wid, MENU_DOT_TOP + gap);
}

void DebugWin::updateStyle() {
	QString str;
#if 0
	QString bcstr;
	bcstr = getStyleString("dbg.window", "dbg.text");
	if (!bcstr.isEmpty()) {
		str.append("QWidget {").append(bcstr).append("}");		// main color (window + all widgets by default), must be 1st
		bcstr = getStyleString("dbg.window", "dbg.text", FLG_LIGHTER, 80);
		str.append("QTabBar::tab {").append(bcstr).append("}");		// by some reason, shape is disappearing
		bcstr = getStyleString("dbg.window", "dbg.text", FLG_LIGHTER, 120);
		str.append("QTabBar::tab:selected {").append(bcstr).append("}");
	}
	bcstr = getStyleString("dbg.table.bg", "dbg.table.txt", FLG_ALTCOLOR, 80);
	if (!bcstr.isEmpty()) {
		// qDebug() << bcstr;
		str.append("QTableView {").append(bcstr).append("}");
	}
	bcstr = getStyleString("dbg.input.bg", "dbg.input.txt");
	if (!bcstr.isEmpty()) {
		str.append("QLineEdit {").append(bcstr).append("}");
		str.append("QComboBox {").append(bcstr).append("}");
	}
	setStyleSheet(str);
#endif
// headers
	str = getStyleString("dbg.header.bg", "dbg.header.txt");
	ui_cpu.labHeadFlags->setStyleSheet(str);
	ui_misc.labHeadRay->setStyleSheet(str);
	ui_misc.labHeadFrame->setStyleSheet(str);
	ui_misc.labHeadSignal->setStyleSheet(str);
	ui_misc.labPorts->setStyleSheet(str);
	setMiscBlocks();
	QString headStr = str + headerGap;
	foreach(xDockWidget* dw, dockWidgets) {
		if (dw == wid_cpu_dock) {
			// its bar is a pair of labels: paint those, not the box holding
			// them, so the gap between the two shows through as a divider
			dw->titleBarWidget()->setStyleSheet(QStringLiteral("background:transparent;"));
			cpuTitleName->setStyleSheet(headStr);
			cpuTitleFlags->setStyleSheet(headStr);
		} else {
			dw->titleBarWidget()->setStyleSheet(headStr);
		}
	}
	styleTabBars();

	setFont(conf.dbg.font);
	// A style sheet gives every widget a font of its own, which stops the
	// propagation from this window: the panels would fall back to the
	// interface font. Hand the font over one by one.
	// Menus keep the interface font: the debugger one is a monospace picked for
	// the listings and it reads as a different program in a popup
	QFont uiFont = QApplication::font();
	foreach(QWidget* wid, findChildren<QWidget*>()) {
		wid->setFont(qobject_cast<QMenu*>(wid) ? uiFont : conf.dbg.font);
	}
	foreach(xHexSpin* xhs, dbgRegEdit) {
		xhs->updatePal();	// takes the new font from the parent
	}
	fitMMapBoxes();
	curCpuCore = nullptr;		// font changed: re-measure the register columns
	fillStack();			// the font sets the row height, and the offset may be new
	fillDisasm();
	wid_dump->draw();
	//ui.dumpTable->update();
	wid_disk_dump->draw();
}

// The boxes come out wide at startup and only shrank once Options re-applied
// the style, since a combo box caches its hint. Cut them down to what they
// need, the way fitFlagBoxes() does for the flags.
void DebugWin::fitMMapBoxes() {
	int wid = comboFitWidth(mmapType[0]);
	for (int i = 0; i < 4; i++)
		mmapType[i]->setFixedWidth(wid);
}

// A dock area's tab bar, once panels are tabified onto it. It needs the same
// gap under the titlebar as a dock header does - on ::tab, not on the bar
// itself, or the dock area's layout ignores it - and, when the tabs carry
// nothing but icons, a width. A style sheet makes Qt count the icon twice,
// once for the tab and once for the text that is not there, and it leaves the
// spare room on the right, which is what pushes the icon off centre. Pinning
// the content box to the icon takes that room back.
// The room around the icon comes from the same metric the native bar uses, so
// a styled tab keeps the size of a System one instead of shrinking to the
// style's own padding. Asking the tab bar for it would give 0: a style sheet
// with a box on ::tab zeroes that metric.
// A bar mixing icons with titled tabs is left alone: a width would squash the
// text, and a style sheet has no per-tab selector to tell them apart.
// Under "System" nothing is set at all: any style sheet on a QTabBar drops it
// out of native painting.
void DebugWin::styleTabBars() {
	bool styled = !conf.style.empty();
	int pad = styled ? style()->pixelMetric(QStyle::PM_TabBarTabHSpace, nullptr, this) / 2 : 0;
	// direct children only: a panel's own tabs belong to its QTabWidget, not here
	foreach (QTabBar* tb, findChildren<QTabBar*>(QString(), Qt::FindDirectChildrenOnly)) {
		QString str;
		if (styled) {
			QString rules = headerGap;
			// an icon-only dock has no tab text (see xDockWidget)
			bool icons = tb->count() > 0;
			for (int i = 0; i < tb->count(); i++) {
				if (!tb->tabText(i).isEmpty()) { icons = false; break; }
			}
			if (icons) {
				rules.append(QString("width:%0px;").arg(tb->iconSize().width()));
				if (pad > 0) rules.append(QString("padding-left:%0px;padding-right:%0px;").arg(pad));
			}
			str = QStringLiteral("QTabBar::tab{") + rules + QLatin1Char('}');
		}
		// setStyleSheet repolishes the widget even when nothing changed, and a
		// dock being dropped fires this for every bar
		if (tb->styleSheet() != str) tb->setStyleSheet(str);
	}
}

void DebugWin::save_mem_map() {
	Computer* comp = conf.zx;
	for (int i = 0; i < 256; i++) {
		mem_map[i] = comp->mem->map[i];
	}
	for (int i = 0; i < 4; i++) {
		mmapForced[i] = -1;
	}
}

void DebugWin::rest_mem_map() {
	Computer* comp = conf.zx;
	for (int i = 0; i < 256; i++) {
		comp->mem->map[i] = mem_map[i];
	}
	for (int i = 0; i < 4; i++) {
		mmapForced[i] = -1;
	}
	fillAll();
}

void DebugWin::start() {
	if (isVisible()) {
		activateWindow();
		return;
	}
	blockStart = -1;
	blockEnd = -1;
	save_mem_map();
	Computer* comp = conf.zx;
	if (!comp->vid->tail)
		vid_dark_tail(comp->vid);

	this->move(conf.dbg.pos);
	comp->vid->debug = 1;
	comp->flgDBG = 1;
	comp->flgBRK = 0;
	comp->cpu->flgRetBRK = 0;

	brk_clear_tmp(comp);		// clear temp breakpoints

	// Once, on the first open: Options re-applies the style on every change, and
	// doing it per open rebuilds the register grid for nothing. Before show() -
	// it re-fonts every widget, which would otherwise lay the window out twice.
	if (!styleApplied) {
		updateStyle();
		styleApplied = 1;
	}
	show();
// fillall redrawing all vivisble widgets
	if (!fillAll()) {
		ui_asm.dasmTable->setAdr(cpu_get_pc(comp->cpu));
	}
	if (memViewer->vis) {
		memViewer->move(memViewer->winPos);
		memViewer->show();
		memViewer->fillImage();
	}
	wid_brk->moved();		// to redraw all icons
	activateWindow();
	ui_asm.dasmTable->setFocus();
}

void DebugWin::stop() {
	Computer* comp = conf.zx;
	if (!ui_asm.cbAccT->isChecked())
		tCount = comp->tickCount;	// before compExec to add current opcode T
	// one opcode, and the scope has to hear it too or the wave has a notch in it
	// at every resume
	if (comp->flgDBG) snd_scope_step(comp, compExec(comp));
	comp->flgDBG = 0;		// back to normal work, turn breakpoints on
	comp->vid->debug = 0;
	comp->flgMAP = ui_asm.actMaping->isChecked() ? 1 : 0;
	stopTrace();

	memViewer->vis = memViewer->isVisible() ? 1 : 0;
	memViewer->winPos = memViewer->pos();

	foreach(xDockWidget* dw, dockWidgets) {
		dw->setFloating(false);
	}

	memViewer->hide();
	hide();
	emit closed();
}

void DebugWin::resetTCount() {
	Computer* comp = conf.zx;
	if (ui_asm.cbAccT->isChecked()) {
		tCount = comp->tickCount;
		ui_asm.labTcount->setText(QString("%0 / %1").arg(comp->tickCount - tCount).arg(comp->frmtCount));
	}
}

void DebugWin::onPrfChange() {
	if (!conf.zx) return;
	save_mem_map();

	setMiscBlocks();

	unsigned int lim = MEM_64K;
	wid_dump->setLimit(lim);
	ui_asm.dasmScroll->setMaximum(lim - 1);

	dui.leStart->setMax(lim - 1);
	//dui.leEnd->setMax(lim - 1);		// TODO:why segfault
	dui.leLen->setMax(lim);

	// ui.tabDiskDump->setDrive(ui.cbDrive->currentIndex());
	wid_disk_dump->draw();

	wid_brk->moved();
	styleTabBars();

	fillAll();
}

// void DebugWin::reject() {stop();}
void DebugWin::closeEvent(QCloseEvent*) {stop();}

DebugWin::DebugWin(QWidget* par):QMainWindow(par) {
	int i;

	curCpuCore = nullptr;
	regCols = 0;
	regPairW = 0;
	cpuWideDock = 0;
	reformPending = 0;
	fitPending = 0;
	winShown = 0;
	styleApplied = 0;
	reformWait = 0;

	// AllowNestedDocks is off by default, and without it a left/right area can
	// only stack docks vertically: nothing can be dropped beside anything.
	// AnimatedDocks is on by default and makes dragging panels feel sluggish
	setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks);
	setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
	setWindowTitle("Xpeccy+ Debugger");
	setWindowIcon(QIcon(":/images/bug.png"));

	wid_cpu = new QWidget;
	QWidget* wid_dasm = new QWidget;
	ui_cpu.setupUi(wid_cpu);
	ui_asm.setupUi(wid_dasm);

	// No central widget: with one, nothing can ever be docked above or below the
	// disassembler, and the boundaries around it carry no splitter handle. As a
	// dock among docks it can be arranged - and resized - like everything else
	wid_cpu->installEventFilter(this);	// reflows to 1 or 2 columns, see reFormCPU
	wid_cpu_dock = new xDockWidget("", "CPU");
	wid_cpu_dock->setObjectName("CPU");
	wid_cpu_dock->setWidget(wid_cpu);
	// The title bar of this one carries two names in the wide layout - "CPU"
	// over the registers and "Flags" over the column beside them - so it is a
	// pair of labels instead of the single one xDockWidget makes. The right one
	// is hidden in the other layouts and the left one spans the bar as before.
	QWidget* cpuTitle = new QWidget;
	QHBoxLayout* cpuTitleBox = new QHBoxLayout(cpuTitle);
	cpuTitleBox->setContentsMargins(2, 0, 0, 0);	// same left margin as the panel
	cpuTitleBox->setSpacing(HEAD_SPLIT);		// the gap is the divider between them
	cpuTitleName = new QLabel("CPU");
	cpuTitleName->setAlignment(Qt::AlignCenter);
	cpuTitleFlags = new QLabel("Flags");
	cpuTitleFlags->setAlignment(Qt::AlignCenter);
	cpuTitleFlags->setVisible(false);
	cpuTitleBox->addWidget(cpuTitleName);
	cpuTitleBox->addWidget(cpuTitleFlags);
	wid_cpu_dock->setTitleBarWidget(cpuTitle);

	wid_dasm_dock = new xDockWidget("", "Disasm");
	wid_dasm_dock->setObjectName("DISASM");
	wid_dasm_dock->setWidget(wid_dasm);

	wid_dump = new xDumpWidget("","DUMP");
	wid_rdump = new xRDumpWidget("","REG-DUMP");
	wid_disk_dump = new xDiskDumpWidget(":/images/floppy.png","FDD");
	wid_cmos_dump = new xCmosDumpWidget("","CMOS");
	wid_zxscr = new xZXScrWidget(":/images/rulers.png","Screen");
	connect(wid_zxscr, &xZXScrWidget::s_detach, this, &DebugWin::scrSetDetached);
	wid_ay = new xSndWidget(":/images/note.png","Sound Chip");
	connect(wid_ay, &xSndWidget::s_detach, this, &DebugWin::sndSetDetached);
	wid_tape = new xTapeWidget(":/images/tape.png","Tape");
	wid_fdd = new xFDDWidget(":/images/floppy.png","FDC");
	wid_brk = new xBreakWidget(":/images/stop.png","Breakpoints");
	wid_heat = new xHeatWidget(":/images/memory.png","Heat map");
	wid_pal = new xPalWidget(":/images/palette.png", "Palette");

	dockWidgets << wid_dump << wid_rdump << wid_disk_dump << wid_cmos_dump;
	dockWidgets << wid_brk << wid_zxscr << wid_ay << wid_tape;
	dockWidgets << wid_fdd << wid_heat << wid_pal;

	// misc used to be one monolithic MISCTOOLBAR pinned to the window edge.
	// two docks instead, so they can be dragged and split like the rest
	QWidget* wmisc = new QWidget;
	ui_misc.setupUi(wmisc);
	wid_misc = new xDockWidget("", "MEMMAP");
	wid_misc->setObjectName("MISC");
	wid_misc->setWidget(wmisc);

	wid_stack_view = new xStackView;
	wid_stack_view->installEventFilter(this);	// refills to the height, see fillStack
	wid_stack = new xDockWidget("", "STACK");
	wid_stack->setObjectName("STACK");
	wid_stack->setWidget(wid_stack_view);

	wid_anchor_l = make_edge_anchor("ANCHOR_L");
	wid_anchor_r = make_edge_anchor("ANCHOR_R");

	// deliberately not in dockWidgets: they carry no content, so nothing should
	// style them, hide them per hardware, or redraw them
	dockWidgets << wid_misc << wid_stack << wid_cpu_dock << wid_dasm_dock;

	setDefaultLayout();

	setContextMenuPolicy(Qt::PreventContextMenu);

	dumpwin = new QDialog(this);
	labswin = new xLabeList(this);
// create registers group
	xLabel* lab;
	xHexSpin* xhs;
	QLabel* qlb;
	QCheckBox* qcb;
	for(i = 0; i < 32; i++) {		// set max registers here
		// parent them at once: reFormCPU() shows a register before putting it into
		// the layout, and a parentless visible widget is a window of its own
		lab = new xLabel(wid_cpu);
		lab->id = i;
		lab->setVisible(false);
		lab->setProperty("isbit", false);
		lab->setContentsMargins(0, 0, 4, 0);	// don't let it touch the value

		xhs = new xHexSpin(wid_cpu);
		xhs->setXFlag(XHS_BGR | XHS_DEC | XHS_FILL | XHS_AUTOW);
		xhs->setVisible(false);
		xhs->setFrame(false);
		xhs->setMinimumHeight(21);
		xhs->setAlignment(Qt::AlignCenter);
		qcb = new QCheckBox(wid_cpu);
		qcb->setVisible(false);
		dbgRegLabs.append(lab);
		dbgRegEdit.append(xhs);
		dbgRegBits.append(qcb);
		// widgets are put into the layout by reFormCPU(), it knows the cpu register set
		connect(lab, &xLabel::clicked, this, &DebugWin::regClick);
		connect(xhs, &xHexSpin::textChanged, this, &DebugWin::setCPU);
		connect(qcb, SIGNAL(toggled(bool)), this, SLOT(setCPU()));
	}
// create flags group (for cpu->f, 16 bit max)
	flagrp = new QButtonGroup;
	flagrp->setExclusive(false);
	for(i = 0; i < 16; i++) {
		qlb = new QLabel(wid_cpu);
		qcb = new QCheckBox(wid_cpu);
		qlb->setVisible(false);
		qcb->setVisible(false);
		dbgFlagLabs.append(qlb);
		dbgFlagBox.append(qcb);
		flagrp->addButton(qcb);
		// placed by reFormFlags(), it follows the register columns
	}
	connect(flagrp, SIGNAL(buttonClicked(QAbstractButton*)), this, SLOT(setFlags()));
// register layout menu: three shapes and the automatic choice between the
// first two. With three of them, guessing which one the width will produce is
// no longer something a user can do, so the panel is asked instead. It hangs on
// the dock's title bar: inside the panel the right button already means
// something - it points the dump at a register, see regClick()
	static const struct {const char* name; int lay;} regLayTab[] = {
		{"Auto", DBG_REGS_AUTO},
		{"1 column", DBG_REGS_1COL},
		{"2 columns", DBG_REGS_2COL},
		{"Wide", DBG_REGS_WIDE},
		{NULL, 0}
	};
	regMenu = new QMenu(this);
	regMenu->addAction("Registers layout")->setEnabled(false);
	regMenu->addSeparator();
	for (i = 0; regLayTab[i].name; i++) {
		QAction* act = regMenu->addAction(regLayTab[i].name);
		act->setCheckable(true);
		act->setData(regLayTab[i].lay);
	}
	regMenu->addSeparator();
	regSplitAct = regMenu->addAction("Split pairs");
	regSplitAct->setCheckable(true);
	connect(regMenu, &QMenu::triggered, this, &DebugWin::setRegLayout);
	wid_cpu_dock->titleBarWidget()->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(wid_cpu_dock->titleBarWidget(), &QWidget::customContextMenuRequested,
		this, &DebugWin::regLayoutMenu);
	markMenuHeader(cpuTitleName, HEADER_GAP);	// the name half heads the bar

	conf.dbg.labels = 1;
	conf.dbg.segment = 0;
	ui_asm.actShowLabels->setChecked(conf.dbg.labels);
	ui_asm.actShowSeg->setChecked(conf.dbg.segment);
	ui_asm.actDimAddr->setChecked(conf.dbg.dimadr);
	ui_asm.actDimOps->setChecked(conf.dbg.dimops);
	ui_asm.actSyntax->setChecked(conf.dbg.synhl);
	ui_asm.actBlockSep->setChecked(conf.dbg.blocksep);

	xid_none = new xItemDelegate(XTYPE_NONE);
	xid_byte = new xItemDelegate(XTYPE_BYTE);
	xid_labl = new xItemDelegate(XTYPE_LABEL);
	xid_dump = new xItemDelegate(XTYPE_DUMP);

// actions data
	ui_asm.actFetch->setData(MEM_BRK_FETCH);
	ui_asm.actRead->setData(MEM_BRK_RD);
	ui_asm.actWrite->setData(MEM_BRK_WR);

	ui_asm.actViewOpcode->setData(DBG_VIEW_EXEC);
	ui_asm.actViewByte->setData(DBG_VIEW_BYTE);
	ui_asm.actViewWord->setData(DBG_VIEW_WORD);
	ui_asm.actViewAddr->setData(DBG_VIEW_ADDR);
	ui_asm.actViewText->setData(DBG_VIEW_TEXT);

	ui_asm.actTrace->setData(DBG_TRACE_ALL);
	ui_asm.actTraceHere->setData(DBG_TRACE_HERE);
	ui_asm.actTraceINT->setData(DBG_TRACE_INT);
	ui_asm.actTraceLog->setData(DBG_TRACE_LOG);

	ui_asm.dasmTable->setFocus();

// disasm table
	ui_asm.dasmTable->setItemDelegateForColumn(0, xid_labl);
	ui_asm.dasmTable->setItemDelegateForColumn(1, xid_dump);
	ui_asm.dasmTable->setItemDelegateForColumn(2, new xDasmSyntax(ui_asm.dasmTable));

// actions
	ui_asm.tbBreak->addAction(ui_asm.actFetch);
	ui_asm.tbBreak->addAction(ui_asm.actRead);
	ui_asm.tbBreak->addAction(ui_asm.actWrite);

	ui_asm.tbLabels->addAction(ui_asm.actShowLabels);
	ui_asm.tbLabels->addAction(ui_asm.actLoadLabels);
	ui_asm.tbLabels->addAction(ui_asm.actSaveLabels);
	ui_asm.tbLabels->addAction(ui_asm.actLabelsList);
//	ui_asm.tbLabels->addAction(ui_asm.actLabManager);

	ui_asm.tbView->addAction(ui_asm.actViewOpcode);
	ui_asm.tbView->addAction(ui_asm.actViewByte);
	ui_asm.tbView->addAction(ui_asm.actViewText);
	ui_asm.tbView->addAction(ui_asm.actViewWord);
	ui_asm.tbView->addAction(ui_asm.actViewAddr);

	ui_asm.tbSaveDasm->addAction(ui_asm.actDisasm);
	ui_asm.tbSaveDasm->addAction(ui_asm.actLoadDump);
	ui_asm.tbSaveDasm->addAction(ui_asm.actSaveDump);
//	ui_asm.tbSaveDasm->addAction(ui_asm.actLoadLabels);
//	ui_asm.tbSaveDasm->addAction(ui_asm.actSaveLabels);
	ui_asm.tbSaveDasm->addAction(ui_asm.actLoadMap);
	ui_asm.tbSaveDasm->addAction(ui_asm.actSaveMap);

	ui_asm.tbTrace->addAction(ui_asm.actTrace);
	ui_asm.tbTrace->addAction(ui_asm.actTraceHere);
	ui_asm.tbTrace->addAction(ui_asm.actTraceINT);
	ui_asm.tbTrace->addAction(ui_asm.actTraceLog);

	ui_asm.tbTool->addAction(ui_asm.actSearch);
	ui_asm.tbTool->addAction(ui_asm.actFill);
	ui_asm.tbTool->addAction(ui_asm.actSprScan);
	ui_asm.tbTool->addAction(ui_asm.actShowKeys);
	ui_asm.tbTool->addAction(ui_asm.actWutcha);
//	ui_asm.tbTool->addAction(ui_asm.actLabelsList);

//	ui_asm.tbDbgOpt->addAction(ui_asm.actShowLabels);
	ui_asm.tbDbgOpt->addAction(ui_asm.actDimAddr);
	ui_asm.tbDbgOpt->addAction(ui_asm.actDimOps);
	ui_asm.tbDbgOpt->addAction(ui_asm.actSyntax);
	ui_asm.tbDbgOpt->addAction(ui_asm.actBlockSep);
	ui_asm.tbDbgOpt->addAction(ui_asm.actShowSeg);
	ui_asm.tbDbgOpt->addAction(ui_asm.actRomWr);
	ui_asm.tbDbgOpt->addAction(ui_asm.actMaping);
	ui_asm.tbDbgOpt->addAction(ui_asm.actMapingClear);
	QAction* actResetLayout = new QAction("Reset panel layout", this);
	ui_asm.tbDbgOpt->addAction(actResetLayout);
	connect(actResetLayout, &QAction::triggered, this, &DebugWin::resetLayout);

// connections
	connect(this, &DebugWin::needStep, this, &DebugWin::doStep);
	connect(ui_asm.cbAccT, &QCheckBox::toggled, this, &DebugWin::resetTCount);
	connect(ui_asm.actMapingClear, &QAction::triggered, this, &DebugWin::mapClear);
	connect(ui_asm.dasmTable, &xDisasmTable::customContextMenuRequested, this, &DebugWin::putBreakPoint);
	//connect(ui_asm.dasmTable, &xDisasmTable::rqRefill, this, &DebugWin::fillDisasm);		// must update internally
	connect(ui_asm.dasmTable, &xDisasmTable::rqRefill, wid_dump, &xDumpWidget::draw);
	connect(ui_asm.dasmTable, &xDisasmTable::rqRefill, wid_zxscr, &xZXScrWidget::draw);
	connect(ui_asm.dasmTable, &xDisasmTable::rqRefill, wid_brk, &xBreakWidget::draw);
	connect(ui_asm.dasmTable, &xDisasmTable::rqRefillAll, this, &DebugWin::fillAll);
	connect(ui_asm.dasmTable, &xDisasmTable::s_adrch, ui_asm.dasmScroll, &QScrollBar::setValue);
	connect(ui_asm.dasmScroll, &QScrollBar::valueChanged, ui_asm.dasmTable, &xDisasmTable::setAdrX);

	connect(wid_dump, &xDumpWidget::s_blockch, this, &DebugWin::fillDisasm);
	connect(wid_dump, &xDumpWidget::s_datach, this, &DebugWin::fillAll);
	connect(wid_dump, &xDumpWidget::s_brkrq, this, &DebugWin::brkRequest);

	connect(wid_brk, &xBreakWidget::rqDisasm, ui_asm.dasmTable, &xDisasmTable::setAdrX);
	connect(wid_brk, &xBreakWidget::updated, this, &DebugWin::fillDisasm);
	connect(wid_brk, &xBreakWidget::updated, wid_dump, &xDumpWidget::draw);

	connect(ui_asm.actSearch, &QAction::triggered, this, &DebugWin::doFind);
	connect(ui_asm.actFill, &QAction::triggered, this, &DebugWin::doFill);
	connect(ui_asm.actSprScan, &QAction::triggered, this, &DebugWin::doMemView);
	connect(ui_asm.actShowKeys, &QAction::triggered, this, &DebugWin::wannaKeys);
	connect(ui_asm.actWutcha, &QAction::triggered, this, &DebugWin::wannaWutch);

	connect(ui_asm.actLabelsList, &QAction::triggered, labswin, &xLabeList::show);
	connect(labswin, &xLabeList::labSelected, this, &DebugWin::jumpToLabel);
	connect(labswin, &xLabeList::labSetChanged, this, &DebugWin::fillDisasm);

	connect(ui_asm.actShowLabels, &QAction::toggled, this, &DebugWin::setShowLabels);
	connect(ui_asm.actDimAddr, &QAction::toggled, this, &DebugWin::fillDisasm);
	connect(ui_asm.actDimOps, &QAction::toggled, this, &DebugWin::fillDisasm);
	connect(ui_asm.actSyntax, &QAction::toggled, this, &DebugWin::fillDisasm);
	connect(ui_asm.actBlockSep, &QAction::toggled, this, &DebugWin::fillDisasm);
	connect(ui_asm.actShowSeg, &QAction::toggled, this, &DebugWin::setShowSegment);
	connect(ui_asm.actRomWr, &QAction::toggled, this, &DebugWin::setRomWriteable);

	connect(ui_asm.tbView, &QToolButton::triggered, this, &DebugWin::chaCellProperty);
	connect(ui_asm.tbBreak, SIGNAL(triggered(QAction*)),this,SLOT(chaCellProperty(QAction*)));
	connect(ui_asm.tbTrace, SIGNAL(triggered(QAction*)),this,SLOT(doTrace(QAction*)));

	connect(ui_asm.actLoadDump, SIGNAL(triggered(bool)),this,SLOT(doOpenDump()));
	connect(ui_asm.actSaveDump, SIGNAL(triggered(bool)),dumpwin,SLOT(show()));
	connect(ui_asm.actLoadLabels, SIGNAL(triggered(bool)),this,SLOT(dbgLLab()));
	connect(ui_asm.actSaveLabels, SIGNAL(triggered(bool)),this,SLOT(dbgSLab()));
	connect(ui_asm.actLoadMap, SIGNAL(triggered(bool)),this,SLOT(loadMap()));
	connect(ui_asm.actSaveMap, SIGNAL(triggered(bool)),this,SLOT(saveMap()));
	connect(ui_asm.actDisasm, SIGNAL(triggered(bool)),this,SLOT(saveDasm()));
	connect(ui_asm.tbRefresh, SIGNAL(released()), this, SLOT(reload()));

	connect(wid_heat, &xHeatWidget::s_adr, ui_asm.dasmTable, &xDisasmTable::setAdrX);

// registers
//	connect(ui.flagGroup,SIGNAL(buttonClicked(int)),this,SLOT(setFlags()));

	block = 0;
	tCount = 0;
	trace = 0;
// subwindows
	dui.setupUi(dumpwin);
	dui.tbSave->addAction(dui.aSaveBin);
	dui.tbSave->addAction(dui.aSaveHobeta);
	dui.tbSave->addAction(dui.aSaveToA);
	dui.tbSave->addAction(dui.aSaveToB);
	dui.tbSave->addAction(dui.aSaveToC);
	dui.tbSave->addAction(dui.aSaveToD);
	dui.leStart->setMin(0);
	dui.leStart->setMax(0xffffff);
	dui.leEnd->setMin(0);
	dui.leEnd->setMax(0xffffff);
	dui.leLen->setMin(1);
	dui.leLen->setMax(0x1000000);

	connect(dui.aSaveBin,SIGNAL(triggered()),this,SLOT(saveDumpBin()));
	connect(dui.aSaveHobeta,SIGNAL(triggered()),this,SLOT(saveDumpHobeta()));
	connect(dui.aSaveToA,SIGNAL(triggered()),this,SLOT(saveDumpToA()));
	connect(dui.aSaveToB,SIGNAL(triggered()),this,SLOT(saveDumpToB()));
	connect(dui.aSaveToC,SIGNAL(triggered()),this,SLOT(saveDumpToC()));
	connect(dui.aSaveToD,SIGNAL(triggered()),this,SLOT(saveDumpToD()));
	connect(dui.leStart,SIGNAL(valueChanged(int)),this,SLOT(dmpLimChanged()));
	connect(dui.leEnd,SIGNAL(valueChanged(int)),this,SLOT(dmpLimChanged()));
	connect(dui.leLen,SIGNAL(valueChanged(int)),this,SLOT(dmpLenChanged()));

	openDumpDialog = new QDialog(this);
	oui.setupUi(openDumpDialog);
	connect(oui.tbFile,SIGNAL(clicked()),this,SLOT(chDumpFile()));
	connect(oui.leStart,SIGNAL(textChanged(QString)),this,SLOT(dmpStartOpen()));
	connect(oui.butOk,SIGNAL(clicked()), this, SLOT(loadDump()));

	memViewer = new MemViewer(this);

	memFinder = new xMemFinder(this);
	connect(memFinder, SIGNAL(patFound(int)), this, SLOT(onFound(int)));

	memFiller = new xMemFiller(this);
	connect(memFiller, SIGNAL(rqRefill()),this,SLOT(fillNotCPU()));
	connect(memFiller, &xMemFiller::rqRefill, this, &DebugWin::fillDisasm);

// right click on a block of the misc panel. PORTS also gets it row by row, as
// the rows are made (setPortRow); FRAME goes back to 0, so the next stop is
// counted from here - breakpoint conditions read the same number

	setHeaderMenu(ui_misc.labPorts, ":/images/bars.png", "Watched ports...", [this](){
		editWatchPorts();
	});
	// MEMMAP: the four 16K banks
	mmapType[0] = ui_misc.cbPG0;	mmapPage[0] = ui_misc.numPG0;
	mmapType[1] = ui_misc.cbPG1;	mmapPage[1] = ui_misc.numPG1;
	mmapType[2] = ui_misc.cbPG2;	mmapPage[2] = ui_misc.numPG2;
	mmapType[3] = ui_misc.cbPG3;	mmapPage[3] = ui_misc.numPG3;
	QWidgetList mmapMenu;
	mmapMenu << ui_misc.widMMap;
	for (i = 0; i < 4; i++) {
		mmapForced[i] = -1;
		mmapType[i]->addItem("ROM", MEM_ROM);
		mmapType[i]->addItem("RAM", MEM_RAM);
		// this column is narrow and the stack panel under it lives with whatever
		// width is set here - fitMMapBoxes() pins it to the text and the arrow
		// no XHS_BGR: this field is lit by the forced mark, not by the value moving
		mmapPage[i]->setXFlag(XHS_FILL | XHS_AUTOW);
		mmapPage[i]->setMax(0xff);
		QString tip = QString("%0..%1").arg(i * 0x4000, 4, 16, QChar('0'))
			.arg(i * 0x4000 + 0x3fff, 4, 16, QChar('0')).toUpper();
		mmapType[i]->setToolTip(tip);
		mmapPage[i]->setToolTip(tip);
		connect(mmapType[i], QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, i](int){mmapEdit(i);});
		connect(mmapPage[i], &xHexSpin::valueChanged, this, [this, i](int){mmapEdit(i);});
		mmapMenu << mmapType[i] << mmapPage[i];
	}
	// anywhere in the block answers the right button, the way FRAME does, and
	// so does the MEMMAP title over it - the one of the two that wears the dot
	foreach (QWidget* wid, mmapMenu) {
		setLabelMenu(wid, ":/images/refresh.png", "Restore mapping", [this](){rest_mem_map();});
	}
	setHeaderMenu(wid_misc->titleBarWidget(), ":/images/refresh.png", "Restore mapping",
		[this](){rest_mem_map();}, HEADER_GAP);
	// the rows keep their own width and sit centered, like the labels around
	// them: stretched to the dock they would drag the whole column wider
	ui_misc.verticalLayout->setAlignment(ui_misc.widMMap, Qt::AlignHCenter);

	setHeaderMenu(ui_misc.labHeadFrame, ":/images/refresh.png", "Reset counter", [this](){
		conf.zx->frmCount = 0;
		fillNotCPU();
	});
	setLabelMenu(ui_misc.labFrame, ":/images/refresh.png", "Reset counter", [this](){
		conf.zx->frmCount = 0;
		fillNotCPU();
	});

// context menu
	cellMenu = new QMenu(this);
	QMenu* bpMenu = new QMenu("Breakpoints");
	bpMenu->setIcon(QIcon(":/images/stop.png"));
	cellMenu->addMenu(bpMenu);
	bpMenu->addAction(ui_asm.actFetch);
	bpMenu->addAction(ui_asm.actRead);
	bpMenu->addAction(ui_asm.actWrite);
	QMenu* viewMenu = new QMenu("View");
	viewMenu->setIcon(QIcon(":/images/bars.png"));
	cellMenu->addMenu(viewMenu);
	viewMenu->addAction(ui_asm.actViewOpcode);
	viewMenu->addAction(ui_asm.actViewByte);
	viewMenu->addAction(ui_asm.actViewText);
	viewMenu->addAction(ui_asm.actViewWord);
	viewMenu->addAction(ui_asm.actViewAddr);
	cellMenu->addSeparator();
	cellMenu->addAction(ui_asm.actLabelsList);
	cellMenu->addAction(ui_asm.actTraceHere);
	cellMenu->addAction(ui_asm.actShowLabels);
	// NOTE: actions already connected to slots by main menu. no need to double it here

	resize(minimumSize());

	// A dock moving can change what sits under the cpu panel, and which tabs
	// share a bar. Queued: the geometry is not settled yet while the drop is
	// being handled, and the new tab bar may not exist yet either
	foreach (xDockWidget* dw, dockWidgets) {
		connect(dw, &QDockWidget::dockLocationChanged,
			this, &DebugWin::updateCpuDockWidth, Qt::QueuedConnection);
		connect(dw, &QDockWidget::dockLocationChanged,
			this, &DebugWin::styleTabBars, Qt::QueuedConnection);
	}

	// A saved layout only makes sense for the set of panels it was written for.
	// restoreState refuses a state whose version differs, and leaves docks it
	// finds no entry for wherever they happen to be - which is how an old file
	// scatters the new panels. Refused or damaged: fall back to the default.
	QString path = conf.path.confDir.c_str();
	path.append("/debuga.layout");
	QFile file(path);
	if (file.open(QFile::ReadOnly)) {
		QByteArray state = file.readAll();
		file.close();
		// resetLayout, not setDefaultLayout: a refused restore can leave the
		// window stretched, and only the former puts the size back too
		if (restoreState(state, DBG_LAYOUT_VERSION)) {
			dockLayout = state;		// see hideEvent
		} else {
			resetLayout();
		}
	}
}

DebugWin::~DebugWin() {
	// while the window is hidden the layout no longer holds the arrangement the
	// user left (see hideEvent), so take the kept one
	QByteArray state = isVisible() ? saveState(DBG_LAYOUT_VERSION) : dockLayout;
	if (state.isEmpty()) state = saveState(DBG_LAYOUT_VERSION);
	QString path = conf.path.confDir.c_str();
	path.append("/debuga.layout");
	QFile file(path);
	if (file.open(QFile::WriteOnly)) {
		file.write(state);
		file.close();
	}
	dumpwin->deleteLater();
	openDumpDialog->deleteLater();
	memViewer->deleteLater();
	memFiller->deleteLater();
	memFinder->deleteLater();
}

void DebugWin::setShowLabels(bool f) {
	conf.dbg.labels = !!f;
	fillDisasm();
}

void DebugWin::setShowSegment(bool f) {
	conf.dbg.segment = !!f;
	fillDisasm();
	wid_dump->draw();
	//fillDump();
}

void DebugWin::setRomWriteable(bool f) {
	conf.dbg.romwr = !!f;
}

/*
void DebugWin::setDumpCP() {
	int cp = getRFIData(ui.cbCodePage);
	ui.dumpTable->setCodePage(cp);
	fillDump();
}

void DebugWin::chDumpView() {
	int mode,page,pbase,psize;
	Computer* comp = conf.zx;
	mode = getRFIData(ui.cbDumpView);
	page = ui.sbDumpPage->value();
	pbase = ui.leDumpPageBase->getValue();
	if (mode == XVIEW_CPU) {
		psize = MEM_64K;
	} else {
		psize = getRFIData(ui.cbDumpPageSize);
	}
	ui.widDumpPage->setDisabled(mode == XVIEW_CPU);
	ui.dumpTable->setMode(mode, page, pbase, psize);
	ui.dumpScroll->setMaximum(psize-1);
	ui.dumpTable->setLimit(psize);
}
*/

static QFile logfile;

void DebugWin::doStep() {
	Computer* comp = conf.zx;
	if (!ui_asm.cbAccT->isChecked())
		tCount = comp->tickCount;
	// the scope is fed from the emulation thread, which is not running while we
	// hold the machine: carry it across this step ourselves
	snd_scope_step(comp, compExec(comp));
	if (!fillAll()) {
		ui_asm.dasmTable->setAdr(cpu_get_pc(comp->cpu));
		//fillDisasm();
	}
}

void DebugWin::doTraceHere() {
	doTrace(ui_asm.actTraceHere);
}

void DebugWin::doTrace(QAction* act) {
	if (trace) return;

	traceType = act->data().toInt();

	if (traceType == DBG_TRACE_LOG) {
		QString path = QFileDialog::getSaveFileName(this, "Log file",QString(),QString(),nullptr,QFileDialog::DontUseNativeDialog);
		if (path.isEmpty()) return;
		logfile.setFileName(path);
		if (!logfile.open(QFile::WriteOnly)) return;
		logfile.write("addr|command");
		xRegBunch regs = cpuGetRegs(conf.zx->cpu);
		int i = 0;
		while (regs.regs[i].id != REG_EOT) {
			if (regs.regs[i].id != REG_EMPTY) {
				logfile.write("|");
				logfile.write(regs.regs[i].name);
			}
			i++;
		}
		logfile.write("\n");
	}

	trace = 1;
	traceAdr = getAdr();
	ui_asm.tbTrace->setEnabled(false);
	QApplication::postEvent(this, new QEvent((QEvent::Type)DBG_EVENT_STEP));
}

void DebugWin::stopTrace() {
	trace = 0;
	ui_asm.tbTrace->setEnabled(true);
	if (logfile.isOpen()) logfile.close();
}

void DebugWin::reload() {
	Computer* comp = conf.zx;
	if (media_reload(comp) & RELOAD_SNAPSHOT)
		ui_asm.dasmTable->setAdr(cpu_get_pc(comp->cpu));
	fillAll();
}

void DebugWin::keyPressEvent(QKeyEvent* ev) {
	if (trace && !ev->isAutoRepeat()) {
		stopTrace();
		return;
	}
	int i;
	int key = shortcut_check(SCG_DEBUGA, QKeySequence(ev->key() | xNativeMods(ev->modifiers())));
	if (key < 0)
		key = ev->key() | xNativeMods(ev->modifiers());
	unsigned char* ptr;
	int len;
	dasmData drow;
	QModelIndex idx;
	Computer* comp = conf.zx;
	int pc = cpu_get_pc(comp->cpu);
	switch (key) {
		case XCUT_OPTIONS:
			emit wannaOptions();
			break;
		case XCUT_LOAD:
			load_file(comp, NULL, FG_ALL, -1);
			ui_asm.dasmTable->setAdr(pc);
			//fillAll();
			activateWindow();
			break;
		case XCUT_SAVE:
			save_file(comp, NULL, FG_ALL, -1);
			activateWindow();
			break;
		case XCUT_STEPIN:
			if (!ev->isAutoRepeat()) {
				doStep();
			} else if (!trace) {
				doTrace(ui_asm.actTrace);
			}
			break;
		case XCUT_STEPOVER:
			len = dasmSome(comp, pc, drow);
			if (drow.oflag & OF_SKIPABLE) {
				ptr = getBrkPtr(comp, pc + len);
				*ptr |= MEM_BRK_TFETCH;
				stop();
			} else {
				doStep();
			}
			break;
		case XCUT_STEPOUT:
			comp->cpu->flgRetBRK = 1;
			comp->cpu->regCallCnt = 0;
			stop();
			break;
		case XCUT_FASTSTEP:
			for (i = 10; i > 0; i--)
				doStep();
			break;
		case XCUT_TMPBRK:
			if (!ui_asm.dasmTable->hasFocus()) break;
			idx = ui_asm.dasmTable->currentIndex();
			i = ui_asm.dasmTable->getData(idx.row(), 0, Qt::UserRole).toInt();
			ptr = getBrkPtr(comp, i);
			stop();
			*ptr |= MEM_BRK_TFETCH;
			break;
		case XCUT_RESET:
			rzxStop(comp);
			compReset(comp, RES_DEFAULT);
			if (!fillAll()) {
				ui_asm.dasmTable->setAdr(pc);
				//fillDisasm();
			}
			break;
		case XCUT_TRACE:
			doTrace(ui_asm.actTrace);
			break;
		case XCUT_LABELS:
			ui_asm.actShowLabels->setChecked(!conf.dbg.labels);
			break;
		case XCUT_LABLIST:
			labswin->show();
			break;
		case XCUT_DBG_RELOAD:
			reload();
			break;
		case XCUT_KEYBOARD:
			emit wannaKeys();
			break;
		case XCUT_SNDWIN:
			sndToggle();
			break;
		case XCUT_OPEN_DUMP:
			doOpenDump();
			break;
		case XCUT_SAVE_DUMP:
			//doSaveDump();
			dumpwin->show();
			break;
		case XCUT_OPEN_XMAP:
			loadMap();
			break;
		case XCUT_SAVE_XMAP:
			saveMap();
			break;
		case XCUT_FINDER:
			doFind();
			break;
		case XCUT_DEBUG:
			if (!ev->isAutoRepeat())
				stop();
			break;
	}
}

void DebugWin::keyReleaseEvent(QKeyEvent* ev) {
	QKeySequence seq(ev->key() | xNativeMods(ev->modifiers()));
	if (!ev->isAutoRepeat() && (shortcut_match(SCG_DEBUGA, XCUT_STEPIN, seq) != QKeySequence::NoMatch)) {
		stopTrace();
	}
}

static xRegBunch traceregs;
static QString tracestr;
static dasmData tracemnm;
extern int dasmrd(int, void*);

void DebugWin::customEvent(QEvent* ev) {
	Computer* comp = conf.zx;
	int pcadr = cpu_get_pc(comp->cpu);
	switch(ev->type()) {
		case DBG_EVENT_STEP:
			if ((traceType == DBG_TRACE_LOG) && logfile.isOpen()) {
				dasmSome(comp, pcadr, tracemnm);
				tracestr = "\"";			// to avoid numbers conversion, like 3e4->3000
				tracestr.append(gethexword(pcadr));
				tracestr.append("\"|");
				doStep();
				traceregs = cpuGetRegs(comp->cpu);
				tracestr.append(tracemnm.command);
				int i = 0;
				while (traceregs.regs[i].id != REG_EOT) {
					if (traceregs.regs[i].id != REG_EMPTY) {			// mustn't be visible
						tracestr.append("|\"");
						// tracestr.append(traceregs.regs[i].name).append(":");
						switch(traceregs.regs[i].size) {
							case REG_BIT: tracestr.append(traceregs.regs[i].value ? "1" : "0"); break;
							case REG_BYTE: tracestr.append(gethexbyte(traceregs.regs[i].value)); break;
							case REG_WORD: tracestr.append(gethexword(traceregs.regs[i].value)); break;
							case REG_24: tracestr.append(gethex6(traceregs.regs[i].value)); break;
							case REG_32: tracestr.append(gethexint(traceregs.regs[i].value)); break;
						}
						tracestr.append("\"");
					}
					i++;
				}
				tracestr.append("\n");
				logfile.write(tracestr.toUtf8());
			} else {
				doStep();
			}
			switch(traceType) {
				case DBG_TRACE_INT:
					if (comp->cpu->intrq & comp->cpu->inten)
						stopTrace();
					break;
				case DBG_TRACE_HERE:
					if (cpu_get_pc(comp->cpu) == traceAdr)
						stopTrace();
					break;
			}
			if (trace) {
				QApplication::postEvent(this, new QEvent((QEvent::Type)DBG_EVENT_STEP));
			}
			break;
		default:
			break;
	}
}

void DebugWin::moveEvent(QMoveEvent* ev) {
	if (!isVisible()) return;
	conf.dbg.pos = ev->pos();
}

void DebugWin::resizeEvent(QResizeEvent* ev) {
	if (!isVisible()) return;
	conf.dbg.siz = ev->size();
}

void setSignal(QLabel* lab, int on) {
	QFont fnt = lab->font();
	fnt.setBold(on);
	lab->setFont(fnt);
}

/*
void DebugWin::fillTabs() {
}
*/

void DebugWin::fillNotCPU() {
	Computer* comp = conf.zx;
	ui_asm.labTcount->setText(QString("%0 / %1").arg(comp->tickCount - tCount).arg(comp->frmtCount));

	fillMem();
	foreach(xDockWidget* dw, dockWidgets) {
		if (dw->isVisible())
			dw->draw();
	}
	// a stopped machine makes no frames, and the detached windows are fed by
	// those: while the debugger has the machine, it feeds them itself
	emit s_scr_upd();
	emit s_snd_upd();

	setSignal(ui_misc.labDOS, comp->flgDOS);
	setSignal(ui_misc.labROM, comp->flgROM);
	setSignal(ui_misc.labCPM, comp->flgCPM);
	setSignal(ui_misc.labINT, comp->cpu->intrq & comp->cpu->inten);

	ui_misc.labRX->setNum(comp->vid->ray.x);
	setSignal(ui_misc.labRX, comp->vid->hblank);
	ui_misc.labRY->setNum(comp->vid->ray.y);
	setSignal(ui_misc.labRY, comp->vid->vblank);

	ui_misc.labFrame->setNum(comp->frmCount);

	if (memViewer->isVisible())
		memViewer->fillImage();
	fillStack();
	fillPorts();
}

bool DebugWin::fillAll() {
	fillCPU();
	fillNotCPU();
	return fillDisasm();
}


// The dock and the detached window are two views of one panel, so the switch
// between them goes through here: conf holds which it is, and both sides are
// told, whichever of them asked.
void DebugWin::scrSetDetached(bool on) {
	conf.dbg.scrdetach = on ? 1 : 0;
	wid_zxscr->setDetached(on);
	emit wannaScrWin(on);
}

void DebugWin::scrToggle() {
	scrSetDetached(!conf.dbg.scrdetach);
}

void DebugWin::sndSetDetached(bool on) {
	conf.dbg.snddetach = on ? 1 : 0;
	wid_ay->setDetached(on);
	emit wannaSndWin(on);
}

void DebugWin::sndToggle() {
	sndSetDetached(!conf.dbg.snddetach);
}

void DebugWin::showScrDot(int x, int y, int adr, int atr) {
	wid_zxscr->showDot(x, y, adr, atr);
}

// ...

// NOTE: called from start()
void DebugWin::chLayout() {
}

// the same address by register name, for the dump hotkeys. -1 = this cpu
// has no such register

int dbg_get_reg_adr_name(CPU* cpu, const char* name) {
	xRegBunch bunch = cpuGetRegs(cpu);
	for (int i = 0; (i < 32) && (bunch.regs[i].id != REG_EOT); i++) {
		if (!strcmp(bunch.regs[i].name, name))
			return bunch.regs[i].value;
	}
	return -1;
}

void DebugWin::regClick(QMouseEvent* ev) {
	xLabel* lab = qobject_cast<xLabel*>(sender());
	int id = lab->id;
	if (id < 0) return;
	Computer* comp = conf.zx;
	xRegBunch bunch = cpuGetRegs(comp->cpu);
	xRegister reg = bunch.regs[id];
	int adr = reg.value;
	//qDebug() << adr;
	switch (ev->button()) {
		case Qt::RightButton:
			//ui.dumpTable->setAdr(adr);
			wid_dump->setAdr(adr);
			break;
		case Qt::LeftButton:
			ui_asm.dasmTable->setAdr(adr, 1);
			break;
		default:
			break;
	}
}

// fdc

/*
void DebugWin::fillFDC() {
	if (ui.tabsPanel->currentWidget() != ui.fdcTab) return;
	Computer* comp = conf.zx;
	ui.fdcBusyL->setText(comp->dif->fdc->idle ? "0" : "1");
	ui.fdcComL->setText(comp->dif->fdc->idle ? "--" : gethexbyte(comp->dif->fdc->com));
	ui.fdcIrqL->setText(comp->dif->fdc->irq ? "1" : "0");
	ui.fdcDrqL->setText(comp->dif->fdc->drq ? "1" : "0");
	ui.fdcTrkL->setText(gethexbyte(comp->dif->fdc->trk));
	ui.fdcSecL->setText(gethexbyte(comp->dif->fdc->sec));
	ui.fdcHeadL->setText(comp->dif->fdc->side ? "1" : "0");
	ui.fdcDataL->setText(gethexbyte(comp->dif->fdc->data));
	ui.fdcStateL->setText(gethexbyte(comp->dif->fdc->state));
	ui.fdcSr0L->setText(gethexbyte(comp->dif->fdc->sr0));
	ui.fdcSr1L->setText(gethexbyte(comp->dif->fdc->sr1));
	ui.fdcSr2L->setText(gethexbyte(comp->dif->fdc->sr2));
	ui.flpCRC->setText(gethexword(comp->dif->fdc->crc));
	ui.flpInt->setText(comp->dif->fdc->intr ? "1" : "0");
	ui.flpDma->setText(comp->dif->fdc->dma ? "1" : "0");
	ui.flpIntEn->setText(comp->dif->fdc->inten ? "1" : "0");

	ui.flpCurL->setText(QString(QChar(('A' + comp->dif->fdc->flp->id) & 0xff)));
	ui.flpRdyL->setText((comp->dif->fdc->flp->insert && comp->dif->fdc->flp->door) ? "1" : "0");
	ui.flpTrkL->setText(gethexbyte(comp->dif->fdc->flp->trk));
	ui.flpPosL->setText(gethexword(comp->dif->fdc->flp->pos));
	ui.flpIdxL->setText(comp->dif->fdc->flp->index ? "1" : "0");
	ui.flpDataL->setText(comp->dif->fdc->flp->insert ? gethexbyte(flpRd(comp->dif->fdc->flp, comp->dif->fdc->side)): "--"); comp->dif->fdc->flp->rd = 0;
	ui.flpMotL->setText(comp->dif->fdc->flp->motor ? "1" : "0");
}
*/
// CPU

void DebugWin::fillFlags(const char* fnam) {
	if (fnam == NULL)
		fnam = cpuGetRegs(conf.zx->cpu).flags;
	int flgcnt = strlen(fnam);
	QString allflags = QString(fnam).rightJustified(16, '-');
	int f = cpu_get_flag(conf.zx->cpu);
	for (int i = 0; i < 16; i++) {
		if (i < flgcnt) {
			dbgFlagBox[i]->setVisible(true);
			dbgFlagLabs[i]->setVisible(true);
			dbgFlagLabs[i]->setText(allflags.at(15 - i));
			dbgFlagBox[i]->setChecked(f & (1 << i));
		} else {
			dbgFlagBox[i]->setVisible(false);
			dbgFlagLabs[i]->setVisible(false);
		}
	}
}


// A check box is wider than the mark it draws: the style keeps room for a label
// that is not there, so centering the box in its cell leaves the mark off to one
// side of the name above it. Cut the box down to the mark and the padding in
// front of it - then centering the box centers the mark.
void DebugWin::fitFlagBoxes() {
	QStyleOptionButton opt;
	opt.initFrom(dbgFlagBox[0]);
	opt.rect = QRect(QPoint(0, 0), dbgFlagBox[0]->sizeHint());
	QRect ir = dbgFlagBox[0]->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &opt, dbgFlagBox[0]);
	// 2 more, one either side: a mark drawn to the very edge of its box loses a
	// pixel to the neighbouring cell. Even, so the mark stays in the middle
	int w = ir.left() * 2 + ir.width() + 2;
	if (w < 3) return;
	foreach(QCheckBox* box, dbgFlagBox)
		box->setFixedWidth(w);
}

// Flags in a grid of their own: 8 boxes per row under two register columns, 4
// under one and in the wide layout, where they stand as a column beside the
// registers. width is the row the flags have to fill, 0 for that wide layout -
// there they are centered in whatever room is left for them instead.
void DebugWin::reFormFlags(int width) {
	int per = (regCols == 2) ? 8 : 4;
	ui_cpu.flagsGrid = new QGridLayout;
	ui_cpu.flagsGrid->setSpacing(FLAG_SPACING);
	// standing as a column of its own, the block gets an empty column either
	// side to center it, and a margin so the marks keep clear of the neighbours
	int ofs = (width > 0) ? 0 : 1;
	ui_cpu.flagsGrid->setContentsMargins(ofs * FLAG_MARGIN, FLAG_SPACING, ofs * FLAG_MARGIN, 0);
	for (int i = 0; i < 16; i++) {
		int p = 15 - i;				// flag 15 is the leftmost one
		ui_cpu.flagsGrid->addWidget(dbgFlagLabs[i], (p / per) * 2, p % per + ofs, Qt::AlignCenter);
		ui_cpu.flagsGrid->addWidget(dbgFlagBox[i], (p / per) * 2 + 1, p % per + ofs, Qt::AlignCenter);
	}
	if (width > 0) {
		// The row is exactly as wide as the registers above it, and no wider:
		// spreading it over the whole panel instead would put the split between
		// two register columns off the middle of the row as soon as the dock is
		// wider than the registers need
		int cells = width - FLAG_SPACING * (per - 1);
		// hand the remainder out a pixel at a time: dropping it would leave the
		// row short and its middle no longer under the register column split
		for (int i = 0; i < per; i++)
			ui_cpu.flagsGrid->setColumnMinimumWidth(i, cells / per + ((i < cells % per) ? 1 : 0));
		ui_cpu.flagsGrid->setColumnStretch(per, 1);	// slack goes after them
	} else {
		ui_cpu.flagsGrid->setColumnStretch(0, 1);
		ui_cpu.flagsGrid->setColumnStretch(per + 1, 1);
	}
}

// put one register into the grid, at the grid column a layout column starts on
void DebugWin::placeReg(xRegBunch* b, int i, int row, int col) {
	// A register with a type of its own - the counter, the stack pointer, the
	// flags, a segment - is one number. The rest are bytes that move on their
	// own, so a changed one can light just the byte that moved.
	dbgRegEdit[i]->setSplit(conf.dbg.regsplit && !(b->regs[i].flag & REG_TYPE_M));
	ui_cpu.formRegs->addWidget(dbgRegLabs[i], row, col);
	dbgRegLabs[i]->setVisible(true);
	if (b->regs[i].size == REG_BIT) {
		dbgRegLabs[i]->setProperty("isbit", true);
		ui_cpu.formRegs->addWidget(dbgRegBits[i], row, col + 1);
		dbgRegBits[i]->setCheckable(!(b->regs[i].flag & REG_RO));
		dbgRegBits[i]->setVisible(true);
		dbgRegEdit[i]->setVisible(false);
	} else {
		dbgRegLabs[i]->setProperty("isbit", false);
		ui_cpu.formRegs->addWidget(dbgRegEdit[i], row, col + 1);
		dbgRegEdit[i]->setReadOnly(b->regs[i].flag & REG_RO);
		dbgRegEdit[i]->setVisible(true);
		dbgRegBits[i]->setVisible(false);
	}
}

// index of a register in the bunch, -1 if it is not there
static int find_bunch_reg(xRegBunch* b, int cnt, int id) {
	for (int i = 0; i < cnt; i++) {
		if (b->regs[i].id == id) return i;
	}
	return -1;
}

// the width a layout asks for: its register columns with a gap between them,
// and the flags - under the registers, or beside them in the wide layout.
// Needs regPairW
int DebugWin::regsLayoutWidth(int cols) {
	int w = regPairW * cols + RCOL_GAP * ((cols > 1) ? (cols - 1) : 1);
	int box = dbgFlagBox[0]->minimumWidth();
	if (cols > 2) {
		// the flags are the column after the registers, 4 in a row with a
		// little room either side. This width is what the whole dock column
		// has to fit, so keep it tight
		w += HEAD_SPLIT + box * 4 + FLAG_SPACING * 3 + FLAG_MARGIN * 2;
	} else if (cols > 1) {
		int flgw = box * 8 + FLAG_SPACING * 7;		// 8 in a row underneath
		if (flgw > w) w = flgw;
	}
	// one column: 4 flags in a row are never wider than a name and its value
	return w;
}

// The one writer of the panel's width limits. Two policies share them - the
// automatic mode follows the dock arrangement, a chosen layout asks for what it
// needs - so whatever changes an input calls this instead of picking a policy.
void DebugWin::applyCpuWidthLimits() {
	if (!regPairW) return;			// nothing measured yet
	if (conf.dbg.reglayout == DBG_REGS_AUTO) {
		// never wider than two columns: dragging further would only add empty space
		wid_cpu->setMaximumWidth(regsLayoutWidth(2));
		wid_cpu->setMinimumWidth(regsLayoutWidth(cpuWideDock ? 2 : 1));
	} else {
		// no maximum: it clamps the whole dock column, and this panel is meant
		// to be able to sit above the disassembler
		wid_cpu->setMaximumWidth(QWIDGETSIZE_MAX);
		wid_cpu->setMinimumWidth(regsLayoutWidth(conf.dbg.reglayout));
	}
}

void DebugWin::reFormCPU(xRegBunch* b) {
	// Dock sizes from a restored layout only arrive on the first show, and the
	// column count follows the panel width. Building earlier builds one column,
	// which is tall enough to push the docks below it out of place for good.
	if (!winShown) {
		reformWait = 1;
		return;
	}
	int i;
	int cnt = 0;			// visible registers (bunch holds no REG_EMPTY ones)
	int labw = 0;
	int fldw = 0;
	// 1st pass: set names and sizes, measure the widest name and value
	while ((cnt < dbgRegLabs.size()) && (b->regs[cnt].id != REG_EOT)) {
		dbgRegLabs[cnt]->setText(b->regs[cnt].name);
		dbgRegLabs[cnt]->setProperty("regid", b->regs[cnt].id);
		int mx;
		switch(b->regs[cnt].size) {
			case REG_2: mx = 2; break;
			case REG_BYTE: mx = 0xff; break;
			case REG_24: mx = 0xffffff; break;
			case REG_32: mx = 0xffffffff; break;
			default: mx = 0xffff; break;
		}
		// setMax resets the input mask and the text with it, so don't
		// touch it on a simple reflow: it would drop the 'changed' color
		if (dbgRegEdit[cnt]->getMax() != mx)
			dbgRegEdit[cnt]->setMax(mx);
		if (dbgRegLabs[cnt]->sizeHint().width() > labw) labw = dbgRegLabs[cnt]->sizeHint().width();
		if (dbgRegEdit[cnt]->minimumWidth() > fldw) fldw = dbgRegEdit[cnt]->minimumWidth();
		cnt++;
	}
	regPairW = labw + fldw;
	fitFlagBoxes();			// the widths below are measured off them
	// the automatic mode reads the width it is given; only apply the limits
	// afterwards. Recomputing the mode from them instead would resize the panel,
	// which lands back in this function through eventFilter
	regCols = conf.dbg.reglayout;
	if (regCols == DBG_REGS_AUTO)
		regCols = (wid_cpu->width() >= regsLayoutWidth(2)) ? 2 : 1;
	applyCpuWidthLimits();

	// width the registers and the flags under them share, so the two line up
	int blockW = (regCols > 2) ? 0 : regsLayoutWidth(regCols);

	// 2nd pass: (re)build the layout. Empty the panel first - the registers,
	// flags and the flags header stay alive, wid_cpu owns them, only the
	// layouts and spacers that held them go
	QLayoutItem* item;
	while ((item = ui_cpu.verticalLayout->takeAt(0)) != nullptr)
		delete item;
	ui_cpu.formRegs = new QGridLayout;
	ui_cpu.formRegs->setHorizontalSpacing(0);
	ui_cpu.formRegs->setVerticalSpacing(2);
	for (i = 0; i < regCols; i++)
		ui_cpu.formRegs->setColumnMinimumWidth(i * RCOL_STEP + 2, RCOL_GAP);
	if ((regCols == 2) && (blockW > regPairW * 2 + RCOL_GAP)) {
		// the flags row is the wider of the two: widen the gap between the
		// register columns by the same amount, or the split between them would
		// no longer sit over the middle of the row
		ui_cpu.formRegs->setColumnMinimumWidth(2, blockW - regPairW * 2);
	}
	ui_cpu.formRegs->setColumnStretch(regCols * RCOL_STEP - 1, 1);	// packed to the left
	// one column per register group, in the order the groups are numbered, but
	// only where the table names them and there is room for that many columns
	bool bygroup = false;
	for (i = 0; (i < cnt) && !bygroup; i++)
		bygroup = (regCols > 2) && (b->regs[i].group > 0);
	if (bygroup) {
		QVector<int> nrow(regCols, 0);
		for (i = 0; i < cnt; i++) {
			int c = b->regs[i].group - 1;
			if ((c < 0) || (c >= regCols)) c = regCols - 1;	// the table forgot this one
			placeReg(b, i, nrow[c], c * RCOL_STEP);
			nrow[c]++;
		}
	} else {
		// rows of the 1 and 2 column layouts: a register and the one the table
		// pairs it with. For a cpu whose table names no groups, the wide layout
		// is these rows cut in half, the lower half set beside the upper
		QVector<char> done(dbgRegLabs.size(), 0);
		QList<QPair<int, int> > rows;
		for (i = 0; i < cnt; i++) {
			if (done[i]) continue;		// already taken as someone's pair
			int p = -1;
			done[i] = 1;
			if ((regCols > 1) && b->regs[i].pair) {
				int j = find_bunch_reg(b, cnt, b->regs[i].pair);
				if ((j >= 0) && !done[j]) {
					p = j;
					done[j] = 1;
				}
			}
			rows.append(qMakePair(i, p));
		}
		int half = (regCols > 2) ? ((rows.size() + 1) / 2) : rows.size();
		if (half < 1) half = 1;
		for (int r = 0; r < rows.size(); r++) {
			int blk = (r / half) * 2;	// which pair of columns this row goes to
			placeReg(b, rows.at(r).first, r % half, blk * RCOL_STEP);
			if (rows.at(r).second >= 0)
				placeReg(b, rows.at(r).second, r % half, (blk + 1) * RCOL_STEP);
		}
	}
	for (i = cnt; i < dbgRegLabs.size(); i++) {	// rest of the widgets is unused
		dbgRegLabs[i]->setVisible(false);
		dbgRegBits[i]->setVisible(false);
		dbgRegEdit[i]->setVisible(false);
	}
	reFormFlags(blockW);
	if (regCols > 2) {
		// wide: registers on the left, the flags as one more column beside
		// them. Their name is in the dock's title bar, over that column, so the
		// flags start on the same line the registers do
		ui_cpu.labHeadFlags->setVisible(false);
		// The header splits where the registers end, and the flags take all the
		// room after it. Any extra width the dock has then shows up as air
		// around the flags instead of a hole between the two blocks
		cpuTitleName->setFixedWidth(ui_cpu.formRegs->sizeHint().width());
		cpuTitleFlags->setVisible(true);
		QVBoxLayout* fbox = new QVBoxLayout;
		fbox->setContentsMargins(0, 0, 0, 0);
		fbox->setSpacing(2);
		fbox->addLayout(ui_cpu.flagsGrid);
		fbox->addStretch(1);
		QHBoxLayout* hbox = new QHBoxLayout;
		hbox->setContentsMargins(0, 0, 0, 0);
		hbox->setSpacing(HEAD_SPLIT);	// splits where the title bar does
		hbox->addLayout(ui_cpu.formRegs, 0);
		hbox->addLayout(fbox, 1);
		ui_cpu.verticalLayout->addLayout(hbox);
		// take the minimum off the built layout: the estimate the width was set
		// from earlier measures every register against the widest one, and the
		// slack it leaves would go to the flags and widen the whole dock column
		wid_cpu->setMinimumWidth(ui_cpu.verticalLayout->minimumSize().width());
	} else {
		cpuTitleFlags->setVisible(false);
		cpuTitleName->setMinimumWidth(0);
		cpuTitleName->setMaximumWidth(QWIDGETSIZE_MAX);
		ui_cpu.labHeadFlags->setVisible(true);
		ui_cpu.verticalLayout->addLayout(ui_cpu.formRegs);
		ui_cpu.verticalLayout->addSpacing(8);
		ui_cpu.verticalLayout->addWidget(ui_cpu.labHeadFlags);
		ui_cpu.verticalLayout->addLayout(ui_cpu.flagsGrid);
	}
	ui_cpu.verticalLayout->addStretch(1);	// keep everything packed to the top
	// queued: the panel has to be laid out in its new shape before the dock can
	// be measured, and resizeDocks() must not run inside a layout pass. One per
	// turn - a rebuild on the way in and a core change can both land in it
	if (!fitPending) {
		fitPending = 1;
		QTimer::singleShot(0, this, &DebugWin::fitCpuDock);
	}
}

// The panel is a different shape after a layout change: hand the dock what the
// new one asks for and let the neighbours take back the rest, so no separator
// has to be dragged by hand. Width only when the layout was chosen - in the
// automatic mode the width is what picks the layout in the first place.
void DebugWin::fitCpuDock() {
	fitPending = 0;
	if (!winShown || !regPairW) return;
	QSize sh = wid_cpu_dock->sizeHint();
	if (conf.dbg.reglayout != DBG_REGS_AUTO) {
		int w = sh.width();
		if (w < wid_cpu->minimumWidth()) w = wid_cpu->minimumWidth();
		resizeDocks(QList<QDockWidget*>() << wid_cpu_dock, QList<int>() << w, Qt::Horizontal);
	}
	fitDockHeights(wid_cpu_dock, sh.height(), docksBelowCpu());
}

// Qt hands every dock in a column an equal share, which parks a short panel in
// the middle of empty space. Give the top one the height it asks for and let
// the ones under it swallow the rest.
void DebugWin::fitDockHeights(QDockWidget* top, int hgt, const QList<QDockWidget*>& rest) {
	if (rest.isEmpty()) return;
	QList<QDockWidget*> col;
	QList<int> hgts;
	col << top;
	hgts << hgt;
	foreach (QDockWidget* dw, rest) {
		col << dw;
		hgts << 10000;		// relative, resizeDocks normalises them
	}
	resizeDocks(col, hgts, Qt::Vertical);
}

// Reset from the menu: put the panels back and take the window back to the size
// that arrangement asks for. Without the resize the window keeps whatever the
// dragged-apart layout had stretched it to.
void DebugWin::resetLayout() {
	setDefaultLayout();
	dockLayout.clear();		// nothing to put back over it, see showEvent
	QTimer::singleShot(0, this, [this]() {
		// same size the config hands out on a first run, never below what fits
		resize(QSize(960, 720).expandedTo(minimumSizeHint()));
	});
}

// A narrow strip of nothing, pinned to a window edge. QMainWindow works out
// drop zones from the docks already there, so once panels cover the whole
// window there is nowhere to aim for "outside everything" - a new outermost
// column. An anchor is that target: drop beside it and the column appears.
// Fixed, featureless and title-less, so it cannot be dragged or resized away.
static QDockWidget* make_edge_anchor(const char* name) {
	QDockWidget* dock = new QDockWidget;
	dock->setObjectName(name);
	// empty: no title bar at all. It needs the layout: a bare QWidget answers
	// sizeHint() with (-1,-1), and QDockWidgetLayout takes the title height
	// straight from it, so the dock ends up with a minimum size of (4,-1)
	QWidget* bar = new QWidget;
	QHBoxLayout* barBox = new QHBoxLayout(bar);
	barBox->setContentsMargins(0, 0, 0, 0);
	dock->setTitleBarWidget(bar);
	dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
	QWidget* pad = new QWidget;
	pad->setFixedWidth(4);
	dock->setWidget(pad);
	return dock;
}

// The shipped arrangement: cpu on the left, [ dump / brk ] [ misc / stack ] on
// the right. Also the way back when the docks have been dragged into a corner
// an empty area gives no drop zone to return to.
void DebugWin::setDefaultLayout() {
	foreach (xDockWidget* dw, dockWidgets) {
		// a dock reports itself floating until it is added to a main window, and
		// un-floating one that has no main window yet crashes. parentWidget()
		// tells the two apart: null before the first addDockWidget
		if (dw->parentWidget() && dw->isFloating()) dw->setFloating(false);
	}
	// anchors first and outermost, then everything else splits in between them
	addDockWidget(Qt::LeftDockWidgetArea, wid_anchor_l);
	splitDockWidget(wid_anchor_l, wid_anchor_r, Qt::Horizontal);
	splitDockWidget(wid_anchor_l, wid_cpu_dock, Qt::Horizontal);
	splitDockWidget(wid_cpu_dock, wid_dasm_dock, Qt::Horizontal);
	splitDockWidget(wid_dasm_dock, wid_dump, Qt::Horizontal);
	splitDockWidget(wid_dump, wid_misc, Qt::Horizontal);
	splitDockWidget(wid_dump, wid_brk, Qt::Vertical);
	splitDockWidget(wid_misc, wid_stack, Qt::Vertical);

	tabifyDockWidget(wid_dump, wid_rdump);
	tabifyDockWidget(wid_dump, wid_disk_dump);
	tabifyDockWidget(wid_dump, wid_cmos_dump);
	tabifyDockWidget(wid_brk, wid_zxscr);
	tabifyDockWidget(wid_brk, wid_ay);
	tabifyDockWidget(wid_brk, wid_tape);
	tabifyDockWidget(wid_brk, wid_fdd);
	tabifyDockWidget(wid_brk, wid_heat);
	tabifyDockWidget(wid_brk, wid_pal);
	// A panel has no close button, so the only way one can be hidden is a
	// debuga.layout that was written while it was - which a build with more
	// machines in it could do. Reset is the way back. Here, not in the loop
	// above: a dock has no parent until addDockWidget, and showing one then
	// would put it on screen as a window of its own.
	foreach (xDockWidget* dw, dockWidgets) {
		dw->setVisible(true);
	}
	wid_dump->raise();
	wid_brk->raise();

	// MEMMAP gets exactly its content and STACK under it swallows the rest, so
	// the two sit against each other instead of being spread apart
	fitDockHeights(wid_misc, wid_misc->widget()->sizeHint().height(),
		QList<QDockWidget*>() << wid_stack);

	QList<QDockWidget*> row;
	QList<int> wdt;
	row << wid_cpu_dock << wid_dasm_dock << wid_dump << wid_misc;
	wdt << 1 << 5 << 5 << 1;		// relative, resizeDocks normalises them
	resizeDocks(row, wdt, Qt::Horizontal);

	// Nothing sits under the cpu panel in this arrangement, so the narrow mode
	// is known: set it outright. Measuring instead would race the layout - the
	// geometry still describes the old arrangement for another cycle or two,
	// and this runs on paths where no dock changes area, so nothing would come
	// along later to correct the guess.
	// Nothing sits under the cpu panel in this arrangement, so the narrow mode
	// is known: set it here, while still inside the call that rearranges the
	// docks. Deferring it to the event loop lowers the minimum only after the
	// layout pass has already handed the panel a two column width, and a
	// smaller minimum does not shrink a widget that is already wider.
	cpuWideDock = 0;
	// only the automatic policy has an input here: a chosen layout took its
	// width off the panel it built, which is tighter than the estimate
	// applyCpuWidthLimits() would put back
	if (conf.dbg.reglayout == DBG_REGS_AUTO)
		applyCpuWidthLimits();
}

// only what sits under the cpu panel counts: a dock merely standing beside it
// (the disassembler, say) is in another column and shares nothing with it
QList<QDockWidget*> DebugWin::docksBelowCpu() {
	QList<QDockWidget*> res;
	QRect cr = wid_cpu_dock->geometry();
	foreach (xDockWidget* dw, dockWidgets) {
		if ((dw == wid_cpu_dock) || dw->isHidden() || dw->isFloating()) continue;
		QRect r = dw->geometry();
		if (r.top() < cr.bottom()) continue;			// not below
		if ((r.left() >= cr.right()) || (cr.left() >= r.right())) continue;	// not in this column
		res << dw;
	}
	return res;
}

// Two panels sitting side by side under the cpu dock is the cue for the wide
// (two column) register layout: demand the width that needs, so the row below
// can split in half. A single panel under it keeps the column narrow.
// Only the automatic mode listens to this - a chosen layout sets its own width.
void DebugWin::updateCpuDockWidth() {
	if (!regPairW) return;
	if (conf.dbg.reglayout != DBG_REGS_AUTO) return;
	QList<QDockWidget*> below = docksBelowCpu();
	bool wide = false;
	for (int i = 0; !wide && (i < below.size()); i++) {
		for (int j = i + 1; j < below.size(); j++) {
			QRect a = below.at(i)->geometry();
			QRect b = below.at(j)->geometry();
			// overlapping vertical bands = the two are next to each other
			if ((a.top() < b.bottom()) && (b.top() < a.bottom())) {
				wide = true;
				break;
			}
		}
	}
	if (wide == !!cpuWideDock) return;
	cpuWideDock = wide ? 1 : 0;
	applyCpuWidthLimits();
}

// the dock sizes are in place now, so the cpu panel can be built at the
// width it is really going to have
void DebugWin::showEvent(QShowEvent* ev) {
	QMainWindow::showEvent(ev);
	winShown = 1;
	// the arrangement Qt is about to undo (see hideEvent) goes back in. The
	// state also carries which panels were up, and the machine may have changed
	// while the window was away: the list for this one wins.
	if (!dockLayout.isEmpty()) {
		restoreState(dockLayout, DBG_LAYOUT_VERSION);
		styleTabBars();
	}
	if (reformWait) {
		reformWait = 0;
		rebuildCpuPanel();
	}
}

// Qt lays the docks out again on every show, and that pass gives each panel
// its size hint - the stack one has none worth the name, so it comes back a
// single line while the panel over it takes the rest. Keep the arrangement
// here and put it back on the next show.
void DebugWin::hideEvent(QHideEvent* ev) {
	if (winShown)
		dockLayout = saveState(DBG_LAYOUT_VERSION);
	QMainWindow::hideEvent(ev);
}

// values are kept, no fillCPU after it: that would clear the 'changed' color
void DebugWin::rebuildCpuPanel() {
	if (!conf.zx) return;
	xRegBunch bunch = cpuGetRegs(conf.zx->cpu);
	reFormCPU(&bunch);
}

// a panel resized: the cpu one reflows its columns, the stack one refills to
// however many lines fit now
bool DebugWin::eventFilter(QObject* obj, QEvent* ev) {
	if ((obj == wid_stack_view) && (ev->type() == QEvent::Resize) && isVisible())
		fillStack();		// only repaints the panel: safe from inside a resize
	// isVisible: while the window is hidden the panel is squeezed to its minimum
	if ((obj == wid_cpu) && (ev->type() == QEvent::Resize) && isVisible()
			&& (conf.dbg.reglayout == DBG_REGS_AUTO)
			&& regPairW && conf.zx) {
		int cols = (wid_cpu->width() >= regsLayoutWidth(2)) ? 2 : 1;
		// Never rebuild from inside the resize event. reFormCPU() deletes and
		// re-adds layout items, and a separator drag delivers a stream of
		// resizes while Qt is walking the dock layout - mutating it underneath
		// leaves that walk holding freed items
		if ((cols != regCols) && !reformPending) {
			reformPending = 1;
			QTimer::singleShot(0, this, &DebugWin::reformCpuLater);
		}
	}
	return QMainWindow::eventFilter(obj, ev);
}

void DebugWin::regLayoutMenu(const QPoint& pos) {
	foreach (QAction* act, regMenu->actions()) {
		if (act == regSplitAct) {
			act->setChecked(conf.dbg.regsplit);
		} else {
			act->setChecked(act->isCheckable() && (act->data().toInt() == conf.dbg.reglayout));
		}
	}
	regMenu->popup(wid_cpu_dock->titleBarWidget()->mapToGlobal(pos));
}

void DebugWin::setRegLayout(QAction* act) {
	if (act == regSplitAct) {
		conf.dbg.regsplit = act->isChecked() ? 1 : 0;
		rebuildCpuPanel();	// placeReg() hands the setting to every field
		return;
	}
	int lay = act->data().toInt();
	if (lay == conf.dbg.reglayout) return;
	conf.dbg.reglayout = lay;
	rebuildCpuPanel();
}

void DebugWin::reformCpuLater() {
	reformPending = 0;
	if (!regPairW) return;
	if (((wid_cpu->width() >= regsLayoutWidth(2)) ? 2 : 1) == regCols) return;	// settled meanwhile
	rebuildCpuPanel();
}

void DebugWin::fillCPU() {
	block = 1;
//	Computer* comp = conf.zx;
	CPU* cpu = conf.zx->cpu;
	xRegBunch bunch = cpuGetRegs(cpu);
#if 1
	if (cpu->core != curCpuCore) {
		curCpuCore = cpu->core;
		reFormCPU(&bunch);
	}
	int c = 0;
	int r = 0;
	while (bunch.regs[c].id != REG_EOT) {
		if (bunch.regs[c].id != REG_EMPTY) {
			if (bunch.regs[c].size == REG_BIT) {
				dbgRegBits[r]->setChecked(bunch.regs[c].value);
			} else {
				dbgRegEdit[r]->setValue(bunch.regs[c].value);
			}
			r++;
		}
		c++;
	}

#else
	xRegister* rp;
	int i;
	int t;
	bool f;
	for (i = 0; i < dbgRegLabs.size(); i++) {
		rp = &bunch.regs[i];
		switch (rp->id) {
			case REG_EMPTY:
			case REG_NONE:
				dbgRegLabs[i]->setVisible(false);
				dbgRegEdit[i]->setVisible(false);
				dbgRegBits[i]->setVisible(false);
				break;
			default:
				dbgRegLabs[i]->setText(rp->name);
				dbgRegLabs[i]->setProperty("regid", rp->id);
				t = rp->type;
				switch (t) {
					case REG_2: dbgRegEdit[i]->setMax(2); break;
					case REG_BYTE: dbgRegEdit[i]->setMax(0xff); break;
					case REG_24: dbgRegEdit[i]->setMax(0xffffff); break;
					case REG_32: dbgRegEdit[i]->setMax(0xffffffff); break;
					default: dbgRegEdit[i]->setMax(0xffff); break;
				}
				f = !!(rp->flag & REG_RO);
				dbgRegLabs[i]->setVisible(true);
				// NOTE: setWidget return a error if a cell is occuped
				// NOTE: takeRow exists from Qt5.8
				if (t == REG_BIT) {
					if (!dbgRegLabs[i]->property("isbit").toBool()) {
						ui_cpu.formRegs->takeRow(i);
						ui_cpu.formRegs->insertRow(i, dbgRegLabs[i], dbgRegBits[i]);
						dbgRegLabs[i]->setProperty("isbit", true);
					}
					dbgRegBits[i]->setCheckable(!f);		// if read only
					dbgRegBits[i]->setChecked(rp->value);
					dbgRegBits[i]->setVisible(true);
					dbgRegEdit[i]->setVisible(false);
				} else {
					if (dbgRegLabs[i]->property("isbit").toBool()) {
						ui_cpu.formRegs->takeRow(i);
						ui_cpu.formRegs->insertRow(i, dbgRegLabs[i], dbgRegEdit[i]);
						dbgRegLabs[i]->setProperty("isbit", false);
					}
					dbgRegEdit[i]->setValue(rp->value);
					dbgRegEdit[i]->setReadOnly(f);
					dbgRegEdit[i]->setVisible(true);
					dbgRegBits[i]->setVisible(false);
				}
				break;
		}
	}
#endif
	fillFlags(bunch.flags);
	fillStack();
	block = 0;
}

// called only from connection. checkboxes to cpu->f
void DebugWin::setFlags() {
	if (block) return;
	unsigned short f = 0;
	for (int i = 0; i < 16; i++) {
		if (dbgFlagBox[i]->isVisible() && dbgFlagBox[i]->isChecked())
			f |= (1 << i);
	}
	cpu_set_flag(conf.zx->cpu, f);
	fillCPU();
}

void DebugWin::setCPU() {
	if (block) return;
	Computer* comp = conf.zx;
	CPU* cpu = comp->cpu;
	int i = 0;
	xRegBunch bunch;
	foreach(xLabel* xlb, dbgRegLabs) {
		if (xlb->isVisible()) {
			bunch.regs[i].id = xlb->property("regid").toInt();
			if (xlb->property("isbit").toBool()) {
				bunch.regs[i].value = dbgRegBits[i]->isChecked() ? 1 : 0;
			} else {
				bunch.regs[i].value = dbgRegEdit[i]->getValue();
			}
			i++;
		} else {
			bunch.regs[i].id = REG_EOT;
		}
	}
	cpuSetRegs(cpu, bunch);
	fillFlags(NULL);
	fillStack();
	fillDisasm();
}

// memory map section

// A bank the user forced is marked the way a changed register is. The type
// beside it goes bold instead: a style sheet on a combo box makes Qt draw the
// whole widget through the sheet, and its arrow changes shape with it.
void DebugWin::setMMapMark(int idx, bool on) {
	mmapPage[idx]->setLit(on);
	QFont fnt = mmapType[idx]->font();
	if (fnt.bold() == on) return;
	fnt.setBold(on);
	mmapType[idx]->setFont(fnt);
}

void DebugWin::fillMem() {
	// each field costs a style pass, so fill them only while they are up - the
	// same rule the docks are refreshed by
	if (!ui_misc.widMMap->isVisible()) return;
	Computer* comp = conf.zx;
	MemPage pg;
	int page;
	block = 1;
	for (int i = 0; i < 4; i++) {
		pg = comp->mem->map[i << 6];
		page = pg.num >> 6;
		// the machine pages whenever it likes and says nothing: a bank stays
		// marked as forced only while the value put there is still in the map
		if (mmapForced[i] != ((pg.type << 16) | page)) mmapForced[i] = -1;
		setRFIndex(mmapType[i], pg.type);
		mmapPage[i]->setValue(page);
		setMMapMark(i, mmapForced[i] >= 0);
	}
	block = 0;
}

// Forcing a bank in: this writes into the map behind the machine's back, so it
// holds only until the machine pages that bank itself. Restore puts back the
// whole map as it was when the debugger opened.
void DebugWin::mmapEdit(int bank) {
	if (block) return;
	Computer* comp = conf.zx;
	int type = getRFIData(mmapType[bank]);
	int page = mmapPage[bank]->getValue();
	memSetBank(comp->mem, bank << 6, type, page, MEM_16K, NULL, NULL, NULL);
	mmapForced[bank] = (type << 16) | page;
	wid_dump->draw();
	ui_asm.dasmTable->updContent();
	fillMem();
}

void DebugWin::loadMap() {
	QString path = QFileDialog::getOpenFileName(this, "Open the universe", "", "Xpeccy memory map (*.xmap)",nullptr,QFileDialog::DontUseNativeDialog);
	if (path.isEmpty()) return;
	load_xmap(path);
	xmap_path = path;
	brkInstallAll();
	fillAll();
}

void DebugWin::saveMap() {
	QString path = QFileDialog::getSaveFileName(this, "Save the universe", xmap_path, "Xpeccy memory map (*.xmap)",nullptr,QFileDialog::DontUseNativeDialog);
	if (path.isEmpty()) return;
	if (!path.endsWith(".xmap",Qt::CaseInsensitive))
		path.append(".xmap");
	save_xmap(path);
	xmap_path = path;
}

// labels

void DebugWin::dbgLLab() {
	if (!loadLabels(NULL)) {
		shitHappens("Can't open file");
	}
	fillDisasm();
}
void DebugWin::dbgSLab() {saveLabels(NULL);}

void DebugWin::jumpToLabel(QString lab) {
	xAdr xadr = find_label(lab);
	if (xadr.type >= 0) {
		int cadr = memFindAdr(conf.zx->mem, xadr.type, xadr.abs);
		if (cadr >= 0) {
			ui_asm.dasmTable->setAdr(cadr, 1);
		}
	}
}

// disasm table

int rdbyte(int adr, void* ptr) {
	Computer* comp = (Computer*)ptr;
	int res = -1;
	MemPage* pg = mem_get_page(comp->mem, adr);	// = &comp->mem->map[(adr >> 8) & 0xff];
	int fadr = mem_get_phys_adr(comp->mem, adr);	// = pg->num << 8) | (adr & 0xff);
	switch (pg->type) {
		case MEM_RAM: res = comp->mem->ramData[fadr & comp->mem->ramMask]; break;
		case MEM_ROM: res = comp->mem->romData[fadr & comp->mem->romMask]; break;
		// through the map: the page the machine has in the window is
		// the one the bank's own reader knows how to find
		case MEM_SLOT: res = memRd(comp->mem, adr & 0xffff); break;
	}
	return res;
}

int DebugWin::fillDisasm() {
	conf.dbg.dimadr = ui_asm.actDimAddr->isChecked() ? 1 : 0;
	conf.dbg.dimops = ui_asm.actDimOps->isChecked() ? 1 : 0;
	conf.dbg.synhl = ui_asm.actSyntax->isChecked() ? 1 : 0;
	conf.dbg.blocksep = ui_asm.actBlockSep->isChecked() ? 1 : 0;
	conf.dbg.labels = ui_asm.actShowLabels->isChecked() ? 1 : 0;
	return ui_asm.dasmTable->updContent();
}

void DebugWin::saveDasm() {
	QString path = QFileDialog::getSaveFileName(this, "Save disasm",QString(),QString(),nullptr,QFileDialog::DontUseNativeDialog);
	if (path.isEmpty()) return;
	QFile file(path);
	dasmData drow;
	QList<dasmData> list;
	Computer* comp = conf.zx;
	if (file.open(QFile::WriteOnly)) {
		QTextStream strm(&file);
		int adr = (blockStart < 0) ? 0 : (blockStart & comp->mem->busmask);
		int end = (blockEnd < 0) ? comp->mem->busmask : (blockEnd & comp->mem->busmask);
		int work = 1;
		strm << "; Created by Xpeccy+ Debugger\n\n";
		strm << "\tORG 0x" << gethexword(adr) << "\n\n";
		while ((adr <= end) && work) {
			list = getDisasm(comp, adr);
			foreach (drow, list) {
				if (adr > comp->mem->busmask)
					work = 0;		// address overfill (FFFF+)
				if (drow.isequ) {
					strm << drow.aname << ":";
					strm << drow.command;
				} else if (drow.islab) {
					if (drow.iscom) {
						strm << drow.aname;
					} else {
						strm << drow.aname << ":";
					}
				} else {
					strm << "\t" << drow.command;
				}
				strm << "\n";
			}
			if (dasm_has_separator(list))
				strm << "\n";
		}
		file.close();
	} else {
		shitHappens("Can't write to file");
	}
}

// memory dump

/*
void DebugWin::fillDump() {
	block = 1;
	ui.dumpTable->update();
	fillStack();
	dumpChadr(ui.dumpTable->getAdr());
	block = 0;
}

void DebugWin::dumpChadr(int adr) {
	ui.dumpScroll->setValue(adr);
	QModelIndex idx = ui.dumpTable->selectionModel()->currentIndex();
	int col = idx.column();
	adr += idx.row() << 3;
	if ((col > 0) && (col < 9)) {
		 adr += (col - 1);
	}
	if (ui.dumpTable->mode != XVIEW_CPU) {
		adr &= 0x3fff;
	} else {
		adr %= ui.dumpTable->limit();
	}
	ui.tabsDump->setTabText(0, QString::number(adr, 16).right(6).toUpper().rightJustified(6,'0'));
}
*/

// maping

void DebugWin::mapClear() {
	if (!areSure("Clear memory mapping?")) return;
	Computer* comp = conf.zx;
	int adr;
	for (adr = 0; adr < 0x400000; adr++) {
		comp->brkRamMap[adr] &= 0x0f;
		if (adr < 0x80000) comp->brkRomMap[adr] &= 0x0f;
		if (comp->slot->data && (adr <= comp->slot->memMask))
			comp->slot->brkMap[adr] &= 0x0f;
	}
	fillDisasm();
}

void DebugWin::mapAuto() {

}

// stack

// the offset column: signed hex padded to two digits so the labels are all one width
static QString stack_offset_label(int ofs) {
	return QString("%0%1:").arg(QLatin1Char(ofs < 0 ? '-' : '+'))
		.arg(qAbs(ofs), 2, 16, QLatin1Char('0')).toUpper();
}

// the +0 row names itself by its own address rather than a zero offset
static QString stack_anchor_label(int sp) {
	return QString("%0:").arg(gethexword(sp));
}

void DebugWin::fillStack() {
	if (!conf.zx) return;
	Computer* comp = conf.zx;
	int sp = cpu_get_sp(comp->cpu);
	int adr = sp;
	int ofs = conf.dbg.stackofs;		// kept even where it is set, see DBG_STACK_OFS
	int cnt = wid_stack_view->rowsFit();
	QList<xStackRow> rows;
	xStackRow row;
	for (int i = 0; i < cnt; i++) {
		row.anchor = !ofs;
		row.name = ofs ? stack_offset_label(ofs) : stack_anchor_label(sp);
		row.value = gethexbyte(rdbyte(adr + ofs + 1, comp));
		row.value.append(gethexbyte(rdbyte(adr + ofs, comp)));
		rows << row;
		ofs += 2;
	}
	wid_stack_view->setRows(rows);
}

// ports

// a label of the misc panel with one action behind the right click

// the same, on a header: the dot says the menu is there

void DebugWin::setHeaderMenu(QWidget* wid, QString icon, QString text, std::function<void()> action, int gap) {
	setLabelMenu(wid, icon, text, action);
	markMenuHeader(wid, gap);
}

void DebugWin::setLabelMenu(QWidget* wid, QString icon, QString text, std::function<void()> action) {
	wid->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(wid, &QWidget::customContextMenuRequested, this, [this, wid, icon, text, action](QPoint pos){
		QMenu mnu(this);
		QAction* act = mnu.addAction(QIcon(icon), text);
		if (mnu.exec(wid->mapToGlobal(pos)) == act)
			action();
	});
}

// the ports this profile watches, edited from the panel itself

void DebugWin::editWatchPorts() {
	Computer* comp = conf.zx;
	xPortWatchDialog dlg(this);
	dlg.wid->setPorts(getWatchPorts(comp));
	if (dlg.exec() != QDialog::Accepted) return;
	setWatchPorts(comp, dlg.wid->getPorts());
	fillPorts();
}

// which blocks of the misc panel the user wants to see (Options - deBUGa).
// PORTS is left to fillPorts: it is the only one that can be empty by itself

void DebugWin::setMiscBlocks() {
	ui_misc.widMMap->setVisible(conf.dbg.showmmap);
	ui_misc.labHeadSignal->setVisible(conf.dbg.showsig);
	ui_misc.labDOS->setVisible(conf.dbg.showsig);
	ui_misc.labROM->setVisible(conf.dbg.showsig);
	ui_misc.labCPM->setVisible(conf.dbg.showsig);
	ui_misc.labINT->setVisible(conf.dbg.showsig);
	ui_misc.labHeadFrame->setVisible(conf.dbg.showfrm);
	ui_misc.labFrame->setVisible(conf.dbg.showfrm);
	ui_misc.labHeadRay->setVisible(conf.dbg.showray);
	ui_misc.labRX->setVisible(conf.dbg.showray);
	ui_misc.labRY->setVisible(conf.dbg.showray);
	fillPorts();
}

// one row of the PORTS block, adding it when the panel is short of rows

void DebugWin::setPortRow(int idx, QString nam, QString val) {
	while (idx >= ui_misc.formPort->rowCount()) {
		QLabel* lnam = new QLabel;
		QLabel* lval = new QLabel;
		lnam->setFont(conf.dbg.font);		// added after the font was handed out
		lval->setFont(conf.dbg.font);
		lnam->setToolTip("Last value on the port");
		lval->setToolTip("Last value on the port");
		setLabelMenu(lnam, ":/images/bars.png", "Watched ports...", [this](){ editWatchPorts(); });
		setLabelMenu(lval, ":/images/bars.png", "Watched ports...", [this](){ editWatchPorts(); });
		ui_misc.formPort->addRow(lnam, lval);
	}
	QLabel* wid = (QLabel*)(ui_misc.formPort->itemAt(idx, QFormLayout::LabelRole)->widget());
	wid->setVisible(true);
	wid->setText(nam);
	wid = (QLabel*)(ui_misc.formPort->itemAt(idx, QFormLayout::FieldRole)->widget());
	wid->setVisible(true);
	wid->setText(val);
}

// one list, the machine's own ports among the rest (comp_pwatch_sync), and one
// value per port: its register when the machine keeps one, the bus otherwise

void DebugWin::fillPorts() {
	Computer* comp = conf.zx;
	int i = 0;
	int n;
	if (conf.dbg.showports) {
		for (n = 0; n < comp->pwcount; n++) {
			if (!comp->pwatch[n].on) continue;
			int val = comp_pwatch_val(comp, n);
			setPortRow(i, getPortString(comp->pwatch[n].port, comp->pwatch[n].mask),
				(val < 0) ? QString("--") : gethexbyte(val));
			i++;
		}
	}
	ui_misc.labPorts->setVisible(i > 0);
	int cnt = ui_misc.formPort->rowCount();
	while (i < cnt) {
		((QLabel*)(ui_misc.formPort->itemAt(i, QFormLayout::LabelRole)->widget()))->setVisible(false);
		((QLabel*)(ui_misc.formPort->itemAt(i, QFormLayout::FieldRole)->widget()))->setVisible(false);
		//ui_misc.formPort->takeAt(i); // removeRow(i);
		i++;
	}
}

// breakpoint

int DebugWin::getAdr() {
	int adr;
//	int col;
	QModelIndex idx;
	Computer* comp = conf.zx;

//	if (ui.dumpTable->hasFocus()) {
//		idx = ui.dumpTable->currentIndex();
//		col = idx.column();
//		adr = (ui.dumpTable->getAdr() + (idx.row() << 3));
//		if ((col > 0) && (col < 9)) {
//			adr += idx.column() - 1;
//		}
//	} else {
		idx = ui_asm.dasmTable->currentIndex();
		adr = ui_asm.dasmTable->getData(idx.row(), 0, Qt::UserRole).toInt();
//	}

	adr &= comp->mem->busmask;
	return adr;
}

void DebugWin::brkRequest(int t, int m, int a) {
	int bgn, end, fadr;
	if ((a < blockStart) || (a > blockEnd)) {	// pointer outside block : process 1 cell
		bgn = a;
		end = a;
	} else {								// pointer inside block : process all block
		bgn = blockStart;
		end = blockEnd;
	}
	if (end < bgn) {
		fadr = bgn;
		bgn = end;
		end = fadr;
	}
	brkSet(t, m, bgn, end);
	fillDisasm();
	wid_dump->draw();
	wid_brk->draw();
}

void DebugWin::putBreakPoint() {
	int adr = getAdr();
	if (adr < 0) return;
	doBreakPoint(adr);
	cellMenu->move(QCursor::pos());
	cellMenu->show();
}

void DebugWin::doBreakPoint(unsigned short adr) {
//	bpAdr = adr;
	unsigned char flag = getBrk(conf.zx, adr);
	ui_asm.actFetch->setChecked(flag & MEM_BRK_FETCH);
	ui_asm.actRead->setChecked(flag & MEM_BRK_RD);
	ui_asm.actWrite->setChecked(flag & MEM_BRK_WR);
}

// TODO: breakpoints on block
void DebugWin::chaCellProperty(QAction* act) {
	int data = act->data().toInt();		// flag to change. b4..7 = type, b0..3 = brk
	int adr = getAdr();
	int bgn, end;
	unsigned char bt;
	int fadr;
	unsigned char* ptr;
	if ((adr < blockStart) || (adr > blockEnd)) {	// pointer outside block : process 1 cell
		bgn = adr;
		end = adr;
	} else {								// pointer inside block : process all block
		bgn = blockStart;
		end = blockEnd;
	}
	if (end < bgn) {
		fadr = bgn;
		bgn = end;
		end = fadr;
	}
//	int proc = 1;
	bt = 0;
	xAdr xadr;
	xAdr xend;
	Computer* comp = conf.zx;
	if (ui_asm.actFetch->isChecked()) bt |= MEM_BRK_FETCH;
	if (ui_asm.actRead->isChecked()) bt |= MEM_BRK_RD;
	if (ui_asm.actWrite->isChecked()) bt |= MEM_BRK_WR;
	adr = bgn;
	if (data & MEM_BRK_ANY) {				// if set breakpoint
		if (ui_asm.dasmTable->hasFocus()) {			// from disasm table
			xadr = mem_get_xadr(comp->mem, bgn);
			xend = mem_get_xadr(comp->mem, end);
			if (xadr.type == MEM_ROM) {
				bt |= MEM_BRK_ROM;
			} else if (xadr.type == MEM_RAM) {
				bt |= MEM_BRK_RAM;
			} else {
				bt |= MEM_BRK_SLT;
			}
			brkSet(BRK_MEMCELL, bt, xadr.abs, xend.abs);
//		} else if (ui.dumpTable->hasFocus()) {		// from dump table
//			int fadr = getRFIData(ui.cbDumpView);
//			switch(fadr) {	// XVIEW_RAM/ROM/CPU
//				case XVIEW_CPU:
//					xadr = mem_get_xadr(comp->mem, bgn);
//					xend = mem_get_xadr(comp->mem, end);
//					switch (xadr.type) {
//						case MEM_ROM: bt |= MEM_BRK_ROM; break;
//						case MEM_RAM: bt |= MEM_BRK_RAM; break;
//						default: bt |= MEM_BRK_SLT; break;
//					}
//					brkSet(BRK_MEMCELL, bt, xadr.abs, xend.abs);
//					break;
//				case XVIEW_ROM:
//				case XVIEW_RAM: bt |= (fadr == XVIEW_ROM) ? MEM_BRK_ROM : MEM_BRK_RAM;
//					brkSet(BRK_MEMCELL, bt, bgn, end);
//					break;
//			}
		}
	} else {						// change cell type
		while (adr <= end) {
			ptr = getBrkPtr(comp, adr);
			*ptr &= 0x0f;
			if ((data & 0xf0) == DBG_VIEW_TEXT) {
				bt = rdbyte(adr, comp);
				if ((bt < 32) || (bt > 127)) {
					*ptr |= DBG_VIEW_BYTE;
				} else {
					*ptr |= DBG_VIEW_TEXT;
				}
			} else {
				*ptr |= (data & 0xf0);
			}
			adr++;
		}
	}
	wid_brk->draw();
	//ui.bpList->update();
	fillDisasm();
	wid_dump->draw();
	//fillDump();
//	fillBrkTable();
}

// memDump

//void DebugWin::doSaveDump() {
//	dumpwin->show();
//}

void DebugWin::dmpLimChanged() {
	int start = dui.leStart->getValue();
	int end = dui.leEnd->getValue();
	if (end < start) end = start;
	int len = end - start + 1;
	start = dui.leEnd->cursorPosition();
	dui.leEnd->setValue(end);
	dui.leLen->setValue(len);
	dui.leEnd->setCursorPosition(start);
}

void DebugWin::dmpLenChanged() {
	int start = dui.leStart->getValue();
	int len = dui.leLen->getValue();
	int end = dui.leEnd->getMax();
	if (start + len >= end) {
		len = end - start;
		dui.leLen->setValue(len);
	}
	end = start + len - 1;
	start = dui.leLen->cursorPosition();
	dui.leEnd->setValue(end);
	dui.leLen->setCursorPosition(start);
}

QByteArray DebugWin::getDumpData() {
	//int bank = dui.leBank->text().toInt(NULL,16);
	int adr = dui.leStart->getValue();
	int len = dui.leLen->getValue();
	QByteArray res;
	while (len > 0) {
		//if (adr < 0xc000) {
		res.append(rdbyte(adr, conf.zx));
		//} else {
		//	res.append(comp->mem->ramData[(bank << 14) | (adr & 0x3fff)]);
		//}
		adr++;
		len--;
	}
	return res;
}

void DebugWin::saveDumpBin() {
	QByteArray data = getDumpData();
	if (data.size() == 0) return;
	QString path = QFileDialog::getSaveFileName(this,"Save memory dump",QString(),QString(),nullptr,QFileDialog::DontUseNativeDialog);
	if (path.isEmpty()) return;
	QFile file(path);
	if (file.open(QFile::WriteOnly)) file.write(data);
	dumpwin->hide();
}

void DebugWin::saveDumpHobeta() {
	QByteArray data = getDumpData();
	if (data.size() == 0) return;
	QString path = QFileDialog::getSaveFileName(this,"Save memory dump as hobeta","","Hobeta files (*.$C)",nullptr,QFileDialog::DontUseNativeDialog);
	if (path.isEmpty()) return;
	TRFile dsc;
	QString name = dui.leStart->text();
	// name.append(".").append(dui.leBank->text());
	std::string nms = name.toStdString();
	nms.resize(8,' ');
	memcpy(dsc.name,nms.c_str(),8);
	dsc.ext = 'C';
	int start = dui.leStart->getValue();
	int len = data.size();
	dsc.hst = (start >> 8) & 0xff;
	dsc.lst = start & 0xff;
	dsc.hlen = (len >> 8) & 0xff;
	dsc.llen = len & 0xff;
	dsc.slen = dsc.hlen + ((len & 0xff) ? 1 : 0);
	saveHobeta(dsc, data.data(), path.toStdString().c_str());
	dumpwin->hide();
}

void DebugWin::saveDumpToA() {saveDumpToDisk(0);}
void DebugWin::saveDumpToB() {saveDumpToDisk(1);}
void DebugWin::saveDumpToC() {saveDumpToDisk(2);}
void DebugWin::saveDumpToD() {saveDumpToDisk(3);}

void DebugWin::saveDumpToDisk(int idx) {
	QByteArray data = getDumpData();
	if (data.size() == 0) return;
	if (data.size() > 0xff00) return;
	int start = dui.leStart->getValue();
	int len = dui.leLen->getValue();
	QString name = dui.leStart->text();
	// name.append(".").append(dui.leBank->text());
	Floppy* flp = conf.zx->dif->flp[idx & 3];
	if (!flp->insert) {
		flp_insert(flp, NULL);
		trd_format(flp);
	}
	TRFile dsc = diskMakeDescriptor(name.toStdString().c_str(), 'C', start, len);
	if (diskCreateFile(flp, dsc, (unsigned char*)data.data(), data.size()) == ERR_OK)
		dumpwin->hide();

}

// memfinder

void DebugWin::doFind() {
	Computer* comp = conf.zx;
	memFinder->mem = comp->mem;
	if (memFinder->adr < 0)
		memFinder->adr = (ui_asm.dasmTable->getAdr() + 1) & comp->mem->busmask;
	memFinder->show();
}

void DebugWin::onFound(int adr) {
	ui_asm.dasmTable->setAdr(adr);
	wid_dump->setAdr(adr);
	// ui.dumpTable->setAdr(adr);
}

// memfiller

void DebugWin::doFill() {
	Computer* comp = conf.zx;
	memFiller->start(comp->mem, blockStart, blockEnd);
}

// spr scanner

void DebugWin::doMemView() {
	Computer* comp = conf.zx;
	memViewer->mem = comp->mem;
	memViewer->ui.sbPage->setValue(comp->mem->map[0xc0].num >> 6);
	memViewer->fillImage();
	memViewer->show();
}

// open dump

void dbg_mem_wr(Computer* comp, int adr, unsigned char bt) {
	MemPage* pg = mem_get_page(comp->mem, adr);	// = &comp->mem->map[(adr >> 8) & 0xff];
	int fadr = mem_get_phys_adr(comp->mem, adr);	// = pg->num << 8) | (adr & 0xff);
	switch (pg->type) {
		case MEM_RAM:
			comp->mem->ramData[fadr & comp->mem->ramMask] = bt;
			break;
		case MEM_ROM:
			if (conf.dbg.romwr)
				comp->mem->romData[fadr & comp->mem->romMask] = bt;
			break;
	}
}

int loadDUMP(Computer* comp, const char* name, int adr) {
	FILE* file = fopen(name, "rb");
	if (!file) return ERR_CANT_OPEN;
	int bt;
	while (adr < 0x10000) {
		bt = fgetc(file);
		if (feof(file)) break;
		dbg_mem_wr(comp, adr & 0xffff, bt & 0xff);
		adr++;
	}
	return ERR_OK;
}

void DebugWin::doOpenDump() {
	dumpPath.clear();
	oui.laPath->clear();
	oui.leStart->setText("C000");
	openDumpDialog->show();
}

void DebugWin::chDumpFile() {
	QString path = QFileDialog::getOpenFileName(this,"Open dump",QString(),QString(),nullptr,QFileDialog::DontUseNativeDialog);
	if (path.isEmpty()) return;
	QFileInfo inf(path);
	if ((inf.size() == 0) || (inf.size() > 0xff00)) {
		shitHappens("File is too long");
	} else {
		dumpPath = path;
		oui.laPath->setText(path);
		oui.leLen->setValue(inf.size() & 0xffff);
		dmpStartOpen();
	}
}

void DebugWin::dmpStartOpen() {
	int start = oui.leStart->getValue();
	int len = oui.leLen->getValue();
	int pos = oui.leStart->cursorPosition();
	int end = oui.leEnd->getMax();
	if (start + len > end) {
		start = end - len + 1;
	} else {
		end = start + len - 1;
	}
	oui.leStart->setValue(start);
	oui.leEnd->setValue(end);
	oui.leStart->setCursorPosition(pos);
}

void DebugWin::loadDump() {
	if (dumpPath.isEmpty()) return;
	int res = loadDUMP(conf.zx, dumpPath.toLocal8Bit().data(),oui.leStart->text().toInt(NULL,16));
	fillAll();
	if (res == ERR_OK) {
		openDumpDialog->hide();
	} else {
		shitHappens("Can't open file");
	}
}

// ps/2 widget (tmp here)


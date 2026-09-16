#pragma once

#include <QDialog>
#include <QMainWindow>
#include <QLineEdit>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QKeyEvent>
#include <QTimer>
#include <QItemDelegate>
#include <QMenu>
#include <QButtonGroup>
#include <functional>
#include <QTableWidget>
#if QT_VERSION >= QT_VERSION_CHECK(5,4,0)
#include <QRegularExpressionValidator>
#else
#include <QRegExpValidator>
#endif

#include "../labelist.h"
#include "libxpeccy/spectrum.h"
#include "dbg_widgets.h"
#include "dbg_stack.h"
#include "dbg_zxscr.h"
#include "dbg_sndchip.h"

#include "ui_dumpdial.h"
#include "ui_openDump.h"
// #include "ui_debuger.h"

#include "ui_form_cpu.h"
#include "ui_form_disasm.h"
#include "ui_form_misc.h"

enum {
	DMP_MEM = 1,
	DMP_REG
};

enum {
	DBG_EVENT_STEP = QEvent::User
};

typedef struct {
	QIcon icon;
	QString name;
	QWidget* wid;
} tabDSC;

class DebugWin : public QMainWindow {
	Q_OBJECT
	public:
		DebugWin(QWidget* = NULL);
		~DebugWin();

//		void reject();
		void stop();

	signals:
		void closed();
		void wannaKeys();
		void wannaWutch();
		void wannaScrWin(bool);
		void s_scr_upd();
		void wannaSndWin(bool);
		void s_snd_upd();
		void wannaOptions();
		void needStep();
	public slots:
		void start();
		void onPrfChange();
		void showScrDot(int, int, int, int);
		void scrSetDetached(bool);
		void scrToggle();
		void sndSetDetached(bool);
		void sndToggle();
		void updateStyle();
	private:
		unsigned block:1;
		cpuCore* curCpuCore;
		QWidget* wid_cpu;
		xDockWidget* wid_cpu_dock;
		int regCols;		// register columns in use now (1, 2 or 4)
		int regPairW;		// width one 'name + value' pair needs
		unsigned cpuWideDock:1;	// two panels sit side by side under the cpu dock
		unsigned winShown:1;	// window has been shown: the dock sizes are real
		unsigned styleApplied:1;	// the style sheet has been put on this window
		unsigned reformWait:1;	// a build was asked for before the first show
		unsigned reformPending:1;	// a reflow is already queued
		unsigned fitPending:1;		// so is a dock refit
		QByteArray dockLayout;	// the panel arrangement, kept over a hide/show
		// tracer
		unsigned trace:1;
		int traceType;
		int traceAdr;
		long tCount;

		QImage scrImg;
		QMap<int, QList<tabDSC> > tablist;

		xDockWidget* wid_dasm_dock;
		QString xmap_path;
		// widgets
		Ui::CPUWidget ui_cpu;
		Ui::DisasmWidget ui_asm;
		Ui::FormDbgMisc ui_misc;
		xDockWidget* wid_misc;		// memmap + ports + signals + ray
		xDockWidget* wid_stack;
		xStackView* wid_stack_view;	// its content, see fillStack
		QDockWidget* wid_anchor_l;	// empty strips at the window edges: something
		QDockWidget* wid_anchor_r;	// to drop a panel next to, see make_edge_anchor
		xDumpWidget* wid_dump;
		xRDumpWidget* wid_rdump;
		xDiskDumpWidget* wid_disk_dump;
		xCmosDumpWidget* wid_cmos_dump;
		xZXScrWidget* wid_zxscr;
		xSndWidget* wid_ay;
		xTapeWidget* wid_tape;
		xFDDWidget* wid_fdd;
		xBreakWidget* wid_brk;
		xPalWidget* wid_pal;
		xHeatWidget* wid_heat;
		QList<xDockWidget*> dockWidgets;

		// MEMMAP: the four 16K banks, editable on ZX and read-only labels elsewhere
		MemPage mem_map[256];		// the map as it was on entry, for Restore
		QComboBox* mmapType[4];
		xHexSpin* mmapPage[4];
		int mmapForced[4];		// (type << 16) | page the user forced, -1 = none

		QList<xLabel*> dbgRegLabs;
		QList<xHexSpin*> dbgRegEdit;
		QList<QCheckBox*> dbgRegBits;

		QList<QLabel*> dbgFlagLabs;
		QList<QCheckBox*> dbgFlagBox;
		QButtonGroup* flagrp;

		QDialog* dumpwin;
		Ui::DumpDial dui;
		QByteArray getDumpData();

		QDialog* openDumpDialog;
		Ui::oDumpDial oui;
		QString dumpPath;

		xMemFiller* memFiller;
		xMemFinder* memFinder;
		MemViewer* memViewer;
		xBrkManager* brkManager;
		xLabeList* labswin;

		QMenu* labMenu;
		QMenu* labSetMenu;
		QMenu* regMenu;		// right click on the cpu header: pick a layout
		QAction* regSplitAct;	// its one switch: light changed bytes on their own
		QLabel* cpuTitleName;	// the cpu dock's title bar is two labels: the
		QLabel* cpuTitleFlags;	// second one heads the flags column, wide layout

		QMenu* cellMenu;
		void doBreakPoint(unsigned short);
		int getAdr();

		xItemDelegate* xid_none;
		xItemDelegate* xid_byte;
		xItemDelegate* xid_labl;
		xItemDelegate* xid_dump;

		void fillCPU();
		void fillFlags(const char*);
		void fillMem();
		void setMMapMark(int, bool);
		void fillStack();
		void fillPorts();
		void setPortRow(int, QString, QString);
		void setMiscBlocks();
		void editWatchPorts();
		void setLabelMenu(QWidget*, QString, QString, std::function<void()>);
		void setHeaderMenu(QWidget*, QString, QString, std::function<void()>, int = 0);
		void reFormCPU(xRegBunch*);
		void reFormFlags(int);
		void placeReg(xRegBunch*, int, int, int);
		int regsLayoutWidth(int);
		void applyCpuWidthLimits();
		void fitFlagBoxes();
		void rebuildCpuPanel();
		QList<QDockWidget*> docksBelowCpu();
		void fitDockHeights(QDockWidget*, int, const QList<QDockWidget*>&);
		bool eventFilter(QObject*, QEvent*);

		void chLayout();

	private slots:
		void reformCpuLater();
		void fitCpuDock();
		void regLayoutMenu(const QPoint&);
		void setRegLayout(QAction*);
		void resetLayout();
		void setDefaultLayout();
		void updateCpuDockWidth();
		void styleTabBars();
		void fitMMapBoxes();
		void setShowLabels(bool);
		void setShowSegment(bool);
		void setRomWriteable(bool);
		void resetTCount();

		bool fillAll();
		void fillNotCPU();
		void doStep();

		void saveDasm();
		void mmapEdit(int);
		void save_mem_map();
		void rest_mem_map();

		void dbgLLab();
		void dbgSLab();
		void jumpToLabel(QString);

		void saveMap();
		void loadMap();
		void mapClear();
		void mapAuto();

		int fillDisasm();
		void regClick(QMouseEvent*);
		void reload();

		void setCPU();
		void setFlags();

		void brkRequest(int, int, int);
		void putBreakPoint();
		void chaCellProperty(QAction*);

		void doMemView();
		void doFill();

		void doFind();
		void onFound(int);

		void doTrace(QAction*);
		void doTraceHere();
		void stopTrace();

		void doOpenDump();
//		void doSaveDump();
		void loadDump();
		void chDumpFile();
		void dmpStartOpen();
		void dmpLimChanged();
		void dmpLenChanged();
		void saveDumpBin();
		void saveDumpHobeta();
		void saveDumpToDisk(int);
		void saveDumpToA();
		void saveDumpToB();
		void saveDumpToC();
		void saveDumpToD();
	protected:
		void keyPressEvent(QKeyEvent*);
		void keyReleaseEvent(QKeyEvent*);
		void showEvent(QShowEvent*);
		void hideEvent(QHideEvent*);
		void resizeEvent(QResizeEvent*);
		void moveEvent(QMoveEvent*);
		void closeEvent(QCloseEvent*);
		void customEvent(QEvent*);
};

int loadDUMP(Computer*, const char*, int);

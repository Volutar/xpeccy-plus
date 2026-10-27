#pragma once

#include <QDialog>
#include <QKeyEvent>
#include <QShortcut>
#include <QComboBox>
#include <QButtonGroup>
#include <QGridLayout>
#include <QFrame>
#include <QToolButton>
#include <QLabel>
#include <QModelIndex>
#include <QKeySequence>

#include "../../xcore/xcore.h"
#include "../../xcore/vidrec.h"
#include "../portwatch.h"
#include "opt_romset.h"
#include "opt_diskcat.h"
#include "opt_tapecat.h"
#include "opt_hotkeytab.h"
#include "opt_paledit.h"
#include "opt_filetypes.h"

#include "ui_rsedit.h"
#include "ui_setupwin.h"
#include "ui_layedit.h"

class QTextEdit;
class QThread;

class SetupWin : public QDialog {
	Q_OBJECT
	public:
		SetupWin(QWidget*);
		void prewarmFfmpeg();
		void stopProbe();
	public slots:
		void start();
		void startHotkeys();
	private:
		QDialog* popOut(QWidget*, const char*);
		void cfgLoaded();
		void fillRomSlots();
		void fillRomSummary();
		void makeDevWidgets();
		void buildSidebar();
		void buildDevices();
		QToolButton* devRow(QGridLayout*, const QString&, QWidget*, QWidget*, const char*);
		QToolButton* sheetButton(QWidget*, const char*);
		void buildStart();
		void fillDevSummary();
		void fillDivFirmware();
		void divFirmwareFile();
		bool applyDivFirmware();
		void showDevRows();
		void showBoardStorage();
		void showDriveRows();
		void tidySoundPage();
		void fillGsRom(const QStringList&);
		void fillDosRom();
		void addRomSlot(int, QString, int, const QStringList&, bool, int);
		QString romSlotFileName(int);
		void romSlotPick(int, const QString&);
		void romSlotFile(QComboBox*, int);
		void fillDbgPalette();
		void fillLogPage();
		void applyLogPage();
		void applyMachine();
		void applyGlobal();
		// video recording
		void buildRecording();
		void buildFolders();
		void fillRecording();
		void applyRecording();
		void showFfmpeg();
		void askFfmpeg(const QString& prog);
		void probeFfmpeg();
		void showProbe(const vrecProbe&);
		void fillCodecs(unsigned works);
		void showRecCmd();
		void recEnables();
		QWidget* recCondBox(QCheckBox*, QComboBox*, QLineEdit*, const QString&);
		void showRecSize();
		void recSizes(QSize*, QSize*);
		void alignVideoLabels();
		xRecord recFromUi();
		// the side fill's strength, in a pop-up under the gear beside its list; the
		// window shows each move at once, and Cancel puts back what was there
		QFrame* fillPop;
		xSlider* sldFill[4];		// blur, brightness, fade, edge
		int fillWas[5];			// sideFill, then the four, as applied
		void makeFillPopup();
		void fillToUi();
		void fillTake();
		void fillPut();

		Ui::SetupWin ui;
		Ui::LayEditor layUi;

		xRomsetEditor* rseditor;
		QDialog* advWin;		// the machine-defining settings
		QDialog* romSetWin;		// the slots and where a reset starts
		QDialog* romWin;		// the set, file by file
		xFileTypesBox* ftbox;
		xRomsetModel* rsmodel;
		xRomset roms;			// the set the page edits, until Apply
		int resTarget;			// where a reset starts, until Apply
		QButtonGroup* resGroup;
		QComboBox* cbStart;		// the machine a start takes: Last, Ask, then the machines
		QComboBox* cbFolderWrite;	// Folder as a disk: read only or read and write
		QComboBox* cbFolderTrash;	// ...and where a file deleted there goes
		QGridLayout* gridFolders;

		// the devices' own controls, see makeDevWidgets()
		QCheckBox *cbTapeAuto, *cbTapeRewind, *cbTapeFast, *cbTapeFlash, *cbTapeEdge, *bdtbox, *cbAddBoot, *a80box, *b80box, *c80box, *d80box, *adsbox, *bdsbox, *cdsbox, *ddsbox, *gsrbox, *ratWheel, *cbSwapButtons;
		QComboBox *diskTypeBox, *cbFlpInterleave, *hiface, *hm_type, *hs_type, *sdrvBox, *cbScanTab, *cbCpuTurbo, *cbPsgCount, *cbPsgType, *cbPsgFrq, *cbPsgStereo;
		xSlider *sldTapeSpeed, *sldPsgSep, *sldSensitivity;
		QLabel *labTapeSpeedVal, *labPsgMhz, *labPsgSep;
		int psgFrqMul = 1;	// the chip's clock against the AY's the setting holds
		// the machine's devices, on the Machine page
		QComboBox* gsBox;
		QComboBox* saaBox;
		QComboBox* gsRomBox;
		QComboBox* dosRomBox;
		QToolButton* dosRomBtn;
		QComboBox* joyBox;
		QComboBox* mouseBox;
		QLabel* tapeSum;
		QLabel* sdSum;
		QLabel* slotSum;
		QList<QWidget*> sdRow;
		QComboBox* divBox;		// a divMMC: the board, its RAM, its jumper
		QComboBox* divRamBox;
		QCheckBox* divWrBox;
		QComboBox* divFwBox;		// what its EEPROM holds, and what Apply writes there
		QList<QWidget*> divRow;
		QList<QWidget*> diskRow;	// Disk and Hard disk: off while the divMMC is on
		QList<QWidget*> hddRow;
		QLabel* divNote;		// says so under the divMMC row
		QList<QWidget*> slotRow;
		QList<QWidget*> kbdRow;
		QComboBox* uartBox[2];		// the Next's UARTs: a serial port of the host each
		QCheckBox* cbEspLan;		// the emulated ESP's server reachable from the network
		QList<QWidget*> uartRow;
		QList<QWidget*> drvRow[4];
		QComboBox* drvCountBox;
		// video recording
		QLineEdit *leRecFfmpeg, *leRecDir, *leRecExtra, *leRecVOver, *leRecSOver;
		QComboBox *cbRecSrc, *cbRecScale, *cbRecFps, *cbRecBox, *cbRecCodec, *cbRecPreset, *cbRecAcodec, *cbRecAbr, *cbRecChroma;
		QSpinBox* sbRecCrf;
		QCheckBox *cbRec60, *cbRecPitch, *cbRecStart, *cbRecStop;
		QComboBox *cbRecStartAt, *cbRecStopAt;
		QLineEdit *leRecStartAt, *leRecStopAt;
		QDialog* recHelpWin = NULL;
		QLineEdit* leRecName;
		QTextEdit* teRecCmd;
		QLabel *labRecSrc, *labRecOut, *labRecFfm, *labRecSize, *labRecFmt;
		QToolButton* btnRecGet;
		xElideLabel* labRecName;
		int recBoxKeep = 0;	// the container picked for a codec that has a choice
		bool recFilling = false;	// the controls are being set, not changed
		bool recProbing = false;	// FFmpeg is being asked what it can do
		QString recProbeFor;		// ...and this is the one the field names
		QThread* recProbeThr = NULL;
		std::atomic<bool> recProbeStop{false};

		QDialog* layeditor;
		xKeyEditor* kedit;
		xPalEditor* paleditor;

		xPortWatch* portwid;
		QLabel* padSum[2];
		QCheckBox* cbPadFiles;

		QList<QColor> editpal;
		QMap<QString, QList<QColor> > palPending;	// edited, written on Apply
		void fillPalettes();

		int eidx;

		xLayout nlay;
		void editLayout();

		QFont dbgfnt;

		int bindidx;
		void buildkeylist();

	signals:
		void closed();
		void s_apply();
		void s_prf_changed();
		void s_padwin();		// open the gamepads window
		void s_fill();			// a side fill setting moved: the window shows it
	private slots:
		void reject();
		void apply();
		void okay();
		void setmszbox(int);
		void selsspath();
		void chabsz();
		void chaspd();
		void chaflc();
		void chapsg();
		void chasnow();
		void chasndlat();

		void paledit();
		void palchoosecol(QPoint);
		void palstore();
		void palButtons();
		void palRestore();

		void selLogDir();
		void openLogDir();



		void addRom();
		void editRom();
		void delRom();
		void setRom(xRomFile);
		void romPreset();
		void resetMachine();
		void showRomFiles();
		void updateMachineButtons();
		void saveMachine();
		void cfgExport();
		void cfgImport();
		void cfgReset();
		void delMachine();

		void padSummary();



		void edLayout();
		void addNewLayout();
		void delLayout();
		void layEditorChanged();
		void layEditorOK();
		void layNameCheck(QString);

		void selectColor();
		void selectDbgFont();
		void triggerColor();
};

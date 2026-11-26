#pragma once

#include <QMap>
#include <QDialog>
#include <QKeyEvent>
#include <QShortcut>
#include <QComboBox>
#include <QButtonGroup>
#include <QGridLayout>
#include <QFrame>
#include <QToolButton>
#include <QStackedWidget>
#include <QLabel>
#include <QModelIndex>
#include <QKeySequence>

#include "../../xcore/xcore.h"
#include "../../xcore/vidrec.h"
#include "../../xcore/vscalers.h"
#include "../portwatch.h"
#include "../displaysheet.h"
#include "opt_romset.h"
#include "opt_diskcat.h"
#include "opt_tapecat.h"
#include "opt_hotkeytab.h"
#include "opt_paledit.h"
#include "opt_filetypes.h"

#include "ui_rsedit.h"
#include "ui_setupwin.h"
#include "ui_layedit.h"

class QRadioButton;
class QPushButton;
class xElideLabel;
class QTextEdit;
class QThread;

class SetupWin : public QDialog {
	Q_OBJECT
	public:
		SetupWin(QWidget*);
		void prewarmFfmpeg();
		void stopProbe();
		void benchApply();
	public slots:
		void start();
		void startHotkeys();
	private:
		bool inBench = false;		// benchApply(): no files written, the sound output left alone
		struct {			// the machine's page as filled: applyMachine() sets only what moved
			double cpu = 0, psg = 0;
			int mem = 0;
		} shown;
		void fillPages();
		void noteShown();
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
		void makeSoundGroup();
		void soundPageOn(bool);
		void sndOwnShow();		// the own picks' texts, rows and the Sound page's notices, as the lists stand
		void fillNextGroup();
		void showDriveRows();
		void tidySoundPage();
		void fillGsRom(const QStringList&);
		void fillDosRom();
		void addRomSlot(int, QString, int, const QStringList&, bool, int, QRadioButton* = NULL);
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
		void buildPause();
		void buildSaves();
		void fillSaves();
		void applySaves();
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
		void showShotSize();
		vidFit pageFit();
		int fitInt();
		int fitKeep();
		void alignVideoLabels();
		xRecord recFromUi();
		// the side fill's strength, in a pop-up under the gear beside its list; the
		// window shows each move at once, and Cancel puts back what was there
		QFrame* fillPop;
		xSlider* sldFill[5];		// blur, brightness, fade, edge, stretch
		QLabel* fillOff;		// Blur's Off, Stretched Edges' alone
		void fillModeToUi();
		int fillWas[7];			// sideFill, the five, fillBorder, as applied
		void makeFillPopup();
		void fillToUi();
		void fillSet(int item);		// a Side fill item into conf
		void fillTake();
		void fillPut();
		// the Display look: picked and tuned live, as the side fill is
		xDisplaySheet* dispSheet;
		std::string lookWas[2];		// the shader and its preset, as applied
		xLookStore lookKept;		// ...and the knobs turned
		void makeDisplay();
		void lookToUi();
		void lookTake();
		void lookPut();

		Ui::SetupWin ui;
		Ui::LayEditor layUi;

		xRomsetEditor* rseditor;
		QDialog* advWin;		// the machine-defining settings
		QDialog* romSetWin;		// the slots and where a reset starts
		QDialog* ledWin;		// Appearance's indicators
		QDialog* romWin;		// the set, file by file
		xFileTypesBox* ftbox;
		xRomsetModel* rsmodel;
		xRomset roms;			// the set the page edits, until Apply
		int resTarget;			// where a reset starts, until Apply
		bool memArmed = false;		// a change of Memory is the user's, not the page filling
		QButtonGroup* resGroup;
		QComboBox* cbStart;		// the machine a start takes: Last, Ask, then the machines
		QComboBox* cbFolderWrite;	// Folder as a disk: read only or read and write
		QComboBox* cbFolderTrash;	// ...and where a file deleted there goes
		QGridLayout* gridFolders;
		QGridLayout* gridPause;
		QCheckBox* cbPauseInactive;	// the Pause group
		QCheckBox* cbPauseMenu;
		QCheckBox* cbPauseResume;
		// the Saves page
		QCheckBox* cbSavePerGame;
		QSpinBox* sbQuickKeep;
		QCheckBox* cbSaveEmbed;
		QCheckBox* cbExitSave;
		QSpinBox* sbExitKeep;
		QSpinBox* sbTimedMin;		// 0: off
		QSpinBox* sbTimedKeep;
		QSpinBox* sbCleanDays;		// 0: never
		QCheckBox* cbKeepOne;
		QLineEdit* leSaveDir;
		QCheckBox* cbSnapEmbed;
		QCheckBox* cbSnapLoad;
		bool snapTouched = false;	// the box clicked: an answer, no more asking
		QGridLayout* gridSaves[4];	// their names lined up with Emulation's

		// the devices' own controls, see makeDevWidgets()
		QCheckBox *cbTapeAuto, *cbTapeRewind, *cbTapeSet, *cbTapeFast, *cbTapeFlash, *cbTapeEdge, *bdtbox, *cbAddBoot, *a80box, *b80box, *c80box, *d80box, *adsbox, *bdsbox, *cdsbox, *ddsbox, *gsrbox, *ratWheel, *cbSwapButtons;
		QComboBox *diskTypeBox, *cbFlpInterleave, *hiface, *hm_type, *hs_type, *sdrvBox, *cbScanTab, *cbCpuTurbo, *cbPsgCount, *cbPsgType, *cbPsgFrq;
		xSlider *sldTapeSpeed, *sldSensitivity;
		QLabel *labTapeSpeedVal, *labPsgMhz;
		// the machine's own sound picks over the Sound page's: the PSG sheet's Stereo and
		// Separation, the Machine page's Board filter, each with its button back to the Sound page
		QComboBox *cbOwnLay = nullptr, *cbOwnBoard = nullptr;
		QToolButton *btnOwnLay = nullptr, *btnOwnBoard = nullptr;
		xSlider* sldOwnSep = nullptr;
		QLabel *labOwnSep = nullptr, *ownSepName = nullptr, *ownLayNote = nullptr;
		QWidget* ownSepPair = nullptr;
		int ownLayShown = SND_GLOBAL;	// the Stereo item sndOwnShow() saw last
		int ownSepKept = SND_GLOBAL;	// its separation while the Stereo is off an order
		xSndPicks ownPicks();		// the picks as the lists stand
		// the other machines with picks of their own, as their files have them (sndOthersRead),
		// with their spec: psg.stereo and beeper
		struct xSndOwnRow {std::string id; QString name; xSndPicks snd; int specLay; int specBoard;};
		QList<xSndOwnRow> sndOthers;
		void sndOthersRead();
		// the Sound page's Stereo and Filters groups (makeSoundGroup)
		QComboBox *cbBoard, *cbPsgLay;
		QCheckBox *chkLowCut, *chkHighCut;	// the Sound page's Filters
		QCheckBox* chkSndOn;	// Emulate sound, over the page's groups
		xSlider* sldSep;
		QLabel* labSep;
		// at the foot of each (0 Stereo, 1 Filters): how many machines override it, and
		// Apply to all machines
		QWidget* ownRow[2] = {nullptr, nullptr};	// made after the machine's lists, which call on them
		QLabel* ownLine[2];
		QPushButton* btnAllGlobal[2];
		QStringList sndOwnTips[2];	// each of them and what it has of its own
		void sndAllGlobal(int grp);	// Apply to all machines, asked first
		int psgFrqMul = 1;	// the chip's clock against the AY's the setting holds
		// the machine's devices, on the Machine page
		QComboBox* gsBox;
		QComboBox* saaBox;
		QComboBox* clockBox;		// the Gluk clock (MAC_CLOCK_*), where the board takes one
		QList<QWidget*> clockRow;
		// the Robik's board: its shadow RAM, BRIGHT and cell edges (rows of its own)
		QComboBox* shadowBox;
		QComboBox* brightBox;
		QComboBox* edgesBox;
		QList<QWidget*> robikRows;
		QComboBox* gsRomBox;
		QComboBox* ngsRomBox;		// the NeoGS flash, in the ROM row while the card is one
		QStackedWidget* gsRomStack;
		QString ngsPick;		// the flash file picked, put on the card by Apply
		QComboBox* dosRomBox;
		QToolButton* dosRomBtn;
		QComboBox* joyBox;
		QComboBox* mouseBox;
		QLabel* tapeSum;
		QLabel* sdSum;
		QLabel* slotSum;
		QList<QWidget*> sdRow;
		QComboBox* divBox;		// the SD card board (SD_BOARD_*); a divMMC's RAM, jumper below
		int sdBoard();			// what divBox says, of what the machine takes
		QComboBox* divRamBox;
		QCheckBox* divWrBox;
		QComboBox* divFwBox;		// what its EEPROM holds, and what Apply writes there
		QList<QWidget*> divRow;
		QList<QWidget*> diskRow;	// Floppy disk and Hard disk: off while the divMMC is on
		QList<QWidget*> hddRow;
		QList<QWidget*> slotRow;
		QList<QWidget*> kbdRow;
		QComboBox* uartBox[2];		// the Next's UARTs: a serial port of the host each
		QCheckBox* cbEspLan;		// the emulated ESP's server reachable from the network
		QWidget* devArea;		// the device groups of every machine but the Next
		QWidget* nxArea;		// the Next's own: what NextZXOS set, and this computer's for it
		QMap<QString, QLineEdit*> nxText;	// NextZXOS's settings, by the menu's name: a value
		QMap<QString, QCheckBox*> nxFlag;	// and on or off
		xElideLabel* nxSdSum;		// the card's whole path, shortened in the middle
		QLabel* nxCore;			// the core's version, in the heading
		QLabel* nxTapeSum;
		QCheckBox* nxMouseOn;		// the Next's mouse row: this computer's mouse in use, or not
		QList<QWidget*> drvRow[4];
		QComboBox* drvCountBox;
		// screenshots
		QString shdErrLog;		// the last shader load's errors, as the button beside the list shows them
		void shaderError(const QString& log);
		void shaderErrorLog();
		QComboBox* cbShotSrc;
		QComboBox* cbShotScale;
		QLineEdit* leShotName;
		QLabel* labShotSize;
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
		xSideButton* btnRecAdv;
		QWidget* boxRecAdv;
		int machColMin;		// the button columns as the .ui has them, the least they get
		int appColMin;
		void fitWidths();
		QString borderText(int);
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
		void s_look();			// another Display look, or its knobs as a whole
		void s_lookParam(const QString& name, float value);	// one knob moved
		void s_palette();		// the colors changed: a paused picture is drawn again
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

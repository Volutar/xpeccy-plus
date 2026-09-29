#pragma once

#include <QDialog>
#include <QKeyEvent>
#include <QShortcut>
#include <QComboBox>
#include <QButtonGroup>
#include <QGridLayout>
#include <QToolButton>
#include <QLabel>
#include <QModelIndex>
#include <QKeySequence>

#include "../../xcore/xcore.h"
#include "../../xcore/vidrec.h"
#include "../portwatch.h"
#include "padbinder.h"
#include "opt_romset.h"
#include "opt_diskcat.h"
#include "opt_tapecat.h"
#include "opt_hotkeytab.h"
#include "opt_paledit.h"
#include "opt_gamepad.h"
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
		void setPadName();
	private:
		QDialog* popOut(QWidget*, const char*);
		void cfgLoaded();
		void fillRomSlots();
		void fillRomSummary();
		void makeDevWidgets();
		void buildDevices();
		QToolButton* devRow(QGridLayout*, const QString&, QWidget*, QWidget*, const char*);
		void fillDevSummary();
		void showDevRows();
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
		// video recording
		void buildRecording();
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
		QLabel* joyHint;
		QComboBox* mouseBox;
		QLabel* tapeSum;
		QLabel* sdSum;
		QLabel* slotSum;
		QList<QWidget*> sdRow;
		QList<QWidget*> slotRow;
		QList<QWidget*> kbdRow;
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
//		xPadMapModel* padModel;
		xKeyEditor* kedit;
		xPalEditor* paleditor;

		xPadBinder* padial;
		xPortWatch* portwid;
		xGamepadWidget* gpwid_a;
		xGamepadWidget* gpwid_b;

		QList<QColor> editpal;

		int eidx;

		xLayout nlay;
		void editLayout();

		QFont dbgfnt;

		int bindidx;
		void buildkeylist();
		void buildpadlist();

	signals:
		void closed();
		void s_apply();
		void s_prf_changed();
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

		void selLogDir();
		void openLogDir();



		void addRom();
		void editRom();
		void delRom();
		void setRom(xRomFile);
		void romPreset();
		void resetMachine();
		void showAdvanced();
		void showRomFiles();
		void updateMachineButtons();
		void saveMachine();
		void cfgExport();
		void cfgImport();
		void cfgReset();
		void delMachine();

		void newPadMap();
		void delPadMap();
		void chaPadMap(int);
		void addBinding();
		void editBinding();
		void delBinding();
		void bindAccept(xJoyMapEntry);
		void setCurrentGamepad(int);



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

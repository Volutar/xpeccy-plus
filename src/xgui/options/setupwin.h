#pragma once

#include <QDialog>
#include <QKeyEvent>
#include <QShortcut>
#include <QComboBox>
#include <QModelIndex>
#include <QKeySequence>

#include "../../xcore/xcore.h"
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

class SetupWin : public QDialog {
	Q_OBJECT
	public:
		SetupWin(QWidget*);
	public slots:
		void start();
		void setPadName();
	private:
		QDialog* popOut(QWidget*, const char*);
		void cfgLoaded();
		void fillRomSlots();
		void addRomSlot(int, QString, int, const QStringList&, bool, int);
		QString romSlotFileName(int);
		void romSlotPick(int, const QString&);
		void romSlotFile(QComboBox*, int);
		void fillDbgPalette();
		void fillLogPage();
		void applyLogPage();

		Ui::SetupWin ui;
		Ui::LayEditor layUi;

		xRomsetEditor* rseditor;
		QDialog* advWin;		// the machine-defining settings
		QDialog* romWin;		// the set, file by file
		QDialog* ftWin;			// the machine each file type is opened on
		xFileTypesBox* ftbox;
		xRomsetModel* rsmodel;
		xRomset roms;			// the set the page edits, until Apply

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
		void buildtapelist();
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
		void updatedisknams();
		void loada(); void loadb(); void loadc(); void loadd();
		void savea(); void saveb(); void savec(); void saved();
		void ejcta(); void ejctb(); void ejctc(); void ejctd();
		void newa(); void newb(); void newc(); void newd(); void newdisk(int, int=0);
		void loatape(); void savtape(); void ejctape();
		void tblkup(); void tblkdn(); void tblkrm();
		void hddShowGeom(int);
		void hddMasterImg(); void hddSlaveImg();
		void hddMasterDir(); void hddSlaveDir();
		void chablock(QModelIndex);
		void tlistclick(QModelIndex);

		void paledit();
		void palchoosecol(QPoint);
		void palstore();

		void selLogDir();
		void openLogDir();

		void selSDCimg();
		void selSDCdir();
		void sdcPathChanged();

		void openSlot();
		void ejectSlot();

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

		void fillDiskCat();
		void copyToTape();
		void copyToDisk();
		void diskToHobeta();
		void diskToRaw();


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

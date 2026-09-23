#pragma once

#include <functional>
#include <QDialog>
#include <QTabBar>
#include <QToolButton>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QTimer>

#include "options/opt_diskcat.h"

// Disk manager: what is in a drive, what is on the disk, and taking files off
// it. Only a TR-DOS disk has a catalog to show. While it is up it watches the
// drives, so a file written by the machine or copied in from the tape shows.

enum {
	DW_OPEN = 0,
	DW_NEW,
	DW_SAVE,
	DW_SAVE_AS,
	DW_EJECT
};

class xDiskWin : public QDialog {
	public:
		xDiskWin(QWidget* = NULL);
		void showWindow();
		void refresh();
		std::function<void()> tapeChanged;	// files went onto the tape
		std::function<void(int, int)> diskOp;	// DW_* on a drive, done by the main window
	private:
		QTabBar* tabs;
		QLineEdit* path;
		QToolButton* btnOpen;
		QToolButton* btnNew;
		QToolButton* btnSave;
		QToolButton* btnSaveAs;
		QToolButton* btnEject;
		QCheckBox* protect;
		xDiskCatTable* list;
		QLabel* note;
		QToolButton* toTape;
		QToolButton* toHobeta;
		QToolButton* toRaw;
		QLabel* head;
		QTimer* watch;
		QList<int> rows;		// catalog index of each row: deleted files are not shown
		QList<TRFile> files;		// what the rows show, for finding the one under the head
		int sides = 2;			// of the disk shown, as its system sector says
		QList<QByteArray> seen;		// each drive as the window last showed it
		int drive();
		QList<int> picked();
		QByteArray driveState(int);
		QString driveName(int);
		void watchDrives();
		void showHead();
		void fill();
		void fillDrive(Floppy*);
		void doOp(int);
		void setColumns();
		void pickedChanged();
		void copyToTape();
		void saveFiles(bool hobeta);
};

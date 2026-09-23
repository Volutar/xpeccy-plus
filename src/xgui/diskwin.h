#pragma once

#include <functional>
#include <QDialog>
#include <QTabBar>
#include <QToolButton>
#include <QLabel>

#include "options/opt_diskcat.h"

// Disk manager: what is on the disk in a drive, and taking files off it. Only a
// TR-DOS disk has a catalog to show.

class xDiskWin : public QDialog {
	public:
		xDiskWin(QWidget* = NULL);
		void showDrive(int);
		void refresh();
		std::function<void()> tapeChanged;	// files went onto the tape
	private:
		QTabBar* tabs;
		xDiskCatTable* list;
		QLabel* note;
		QToolButton* toTape;
		QToolButton* toHobeta;
		QToolButton* toRaw;
		QList<int> rows;		// catalog index of each row: deleted files are not shown
		int drive();
		QList<int> picked();
		void fill();
		void setColumns();
		void pickedChanged();
		void copyToTape();
		void saveFiles(bool hobeta);
};

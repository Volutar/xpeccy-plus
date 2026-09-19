#pragma once

#include <QAbstractTableModel>
#include <QHeaderView>
#include <QIcon>
#include <QStringList>
#include <QTableView>

#include "libxpeccy/spectrum.h"
#include "libxpeccy/tape.h"
#include "../classes.h"

// columns of the tape catalog, shared by the tape player and the Tape map page
enum {
	TCC_BRK = 0,		// stop the tape here
	TCC_DUR,		// how long the block plays
	TCC_SIZE,		// bytes in it
	TCC_NAME,		// the header's name, or what kind of block it is
	TCC_INFO,		// what the header says about the file
	TCC_COUNT
};

// the header icon does not go through Qt::DecorationRole: an item view draws
// that one at the left edge of the section, and these two columns are only wide
// enough for their content

#define	TCC_IconRole	(Qt::UserRole + 1)

#define	TCC_MIN_WIDTH	8	// let a column be as narrow as its content
#define	TCC_NAME_WIDTH	110

class xTapeCatModel : public xTableModel {
	Q_OBJECT
	public:
		xTapeCatModel(QObject* p = NULL);
		void fill(Tape*);
		int setCurrent(int);
	private:
		int rcur;
		TapeBlockInfo* inf;
		QStringList dur;		// what the columns say, built once per fill: the
		QStringList name;		// view asks for these again on every repaint, and
		QStringList info;		// for every row it measures a column against
		QIcon icoBrk;
		QIcon icoDur;
		int isNamed(int) const;
		QString blockName(int) const;
		QString blockInfo(int) const;
		QVariant data(const QModelIndex&, int) const;
		QVariant headerData(int, Qt::Orientation, int) const;
};

class xTapeCatHeader : public QHeaderView {
	Q_OBJECT
	public:
		xTapeCatHeader(QWidget* = NULL);
	protected:
		void paintSection(QPainter*, const QRect&, int) const;
		QSize sectionSizeFromContents(int) const;
};

class xTapeCatTable : public QTableView {
	Q_OBJECT
	public:
		xTapeCatTable(QWidget* = NULL);
		void fill(Tape*);
		void setCurrent(int);
		int current();			// the row the buttons work on, -1 for none
		void blkMove(Tape*, int dir);
		void blkDel(Tape*);
	private:
		xTapeCatModel* model;
};

// A tape block as a TR-DOS file, shared by the tape player and the Tape map
// page of Options.
int tape_disk_ready(Computer*, int drive);
int tape_blk_to_disk(Tape*, int blk, Floppy*, QString* msg);

#pragma once

#include <QDialog>
#include <QTableView>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QKeySequence>
#include <QKeyEvent>
#include <QVector>

#include "../xgui.h"
#include "../../xcore/xcore.h"

class xKeyEditor : public QDialog {
	Q_OBJECT
	public:
		xKeyEditor(QWidget* p = NULL);
		void edit(int, int, const QString&, const QKeySequence&);
	signals:
		void s_done(int, int, QKeySequence);
	private:
		int foo;
		int slot;
		QLabel lab;
		QPushButton but;
		QPushButton clr;
		QKeySequence kseq;
		void keyPressEvent(QKeyEvent*);
		void keyReleaseEvent(QKeyEvent*);
	private slots:
		void okay();
		void clear();
		void reject();
};

class xHotkeyModel : public xTableModel {
	Q_OBJECT
	public:
		xHotkeyModel(const xHotkeySet* set, QObject* p = nullptr);
		int rowCount(const QModelIndex& idx = QModelIndex()) const;
		int columnCount(const QModelIndex& idx = QModelIndex()) const;
		QVariant data(const QModelIndex& idx, int role) const;
		QVariant headerData(int sec, Qt::Orientation ori, int role) const;
		Qt::ItemFlags flags(const QModelIndex& idx) const;
		int cut(int row) const;			// the shortcut on a row, -1 on a heading
		QString rowText(int row) const;
	private:
		struct xRow {int tab; QString text;};	// a heading has no shortcut
		QVector<xRow> list;
		const xHotkeySet* set;
};

// The keys are edited on a copy, which Apply makes the live set.
class xHotkeyTable : public QTableView {
	Q_OBJECT
	public:
		xHotkeyTable(QWidget* p = nullptr);
		QWidget* controls();		// the preset, the filter and the reset, for the page to place
		void load();
		void commit();
		void focusFilter();
	private slots:
		void dbl_click(QModelIndex);
		void set_seq(int, int, QKeySequence);
		void presetChanged(int);
		void filterChanged(const QString&);
		void resetCustom();
		void rowMenu(const QPoint&);
	private:
		xHotkeySet work;
		xHotkeyModel* model;
		xKeyEditor* edt;
		QComboBox* preset;
		QLineEdit* filter;
		QPushButton* reset;
		QPushButton* findKey;
		void keyPressEvent(QKeyEvent*);
		void editSlot(int row, int slot);
		bool toCustom();
		void putKeys(int id, const QKeySequence* keys);
		void updateControls();
};

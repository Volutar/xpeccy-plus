#pragma once

#include <QDialog>
#include <QTableView>
#include <QLabel>
#include <QPushButton>
#include <QKeySequence>
#include <QKeyEvent>
#include <QVector>

#include "../xgui.h"

class xKeyEditor : public QDialog {
	Q_OBJECT
	public:
		xKeyEditor(QWidget* p = NULL);
		void edit(int);
	signals:
		void s_done(int, QKeySequence);
	private:
		int foo;
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
		xHotkeyModel(QObject* p = nullptr);
		int rowCount(const QModelIndex& idx = QModelIndex()) const;
		int columnCount(const QModelIndex& idx = QModelIndex()) const;
		QVariant data(const QModelIndex& idx, int role) const;
		Qt::ItemFlags flags(const QModelIndex& idx) const;
		int cut(int row) const;			// the shortcut on a row, -1 on a heading
		int rowOf(int id) const;
	private:
		struct xRow {int tab; QString text;};	// a heading has no shortcut
		QVector<xRow> list;
};

class xHotkeyTable : public QTableView {
	Q_OBJECT
	public:
		xHotkeyTable(QWidget* p = nullptr);
	public slots:
		void set_seq(int, QKeySequence);
		void dbl_click(QModelIndex);
	private:
		xHotkeyModel* model;
		xKeyEditor* edt;
};

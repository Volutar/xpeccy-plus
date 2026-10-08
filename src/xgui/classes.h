#pragma once

#include <QAbstractTableModel>
#include <QDockWidget>
#include <QLabel>
#include <QProxyStyle>

class xTableModel : public QAbstractTableModel {
	Q_OBJECT
	public:
		xTableModel(QObject* = NULL);
		QModelIndex index(int, int, const QModelIndex& = QModelIndex()) const;
		void setRows(int);
		void setCols(int);
		int rowCount(const QModelIndex& = QModelIndex()) const;
		int columnCount(const QModelIndex& = QModelIndex()) const;
		// the row a player stands on, drawn inverted; !0 when it moved
		int markRow(int);
		int markedRow() const {return row_mark;}
	public slots:
		void update();
		void updateRow(int);
		void updateColumn(int);
		void updateCell(int, int);
	protected:
		int row_count;
		int col_count;
		int row_mark;
		QVariant markRole(int row, int role) const;
};

class xDockWidget : public QDockWidget {
	Q_OBJECT
	public:
		xDockWidget(QString = "", QString = "", QWidget* = nullptr);
	private:
		QIcon icon;
		QString title;
	protected:
		QLabel* titleWidget;
	public slots:
		virtual void draw() {}
		void moved();
};

#pragma once

#include <QWidget>

class QTableWidget;

// Options - Media - File types: which machine each format is opened on (see
// xcore/filemachine.h). fill() reads the settings and apply() writes them, so
// the table goes with the dialog's own OK and Cancel.
class xFileTypesBox : public QWidget {
	public:
		xFileTypesBox(QWidget* = nullptr);
		void fill();
		void apply();
		void defaults();		// every row back to its first choice
	protected:
		bool eventFilter(QObject*, QEvent*) override;
	private:
		QTableWidget* table;
};

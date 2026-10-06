#pragma once

#include <functional>
#include <QAbstractTableModel>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QTableView>

#include "../../xcore/gamepad.h"

// A player's table: what the Spectrum gets, what presses it, turbo. The rows
// a joystick does not have are left out.
class xPadTableModel : public QAbstractTableModel {
	public:
		xPadTableModel(xGamepad*, QObject* = nullptr);
		void update();
		int padRow(int) const;		// the player's row a table row shows, -1 for none
		int rowCount(const QModelIndex& = QModelIndex()) const;
		int columnCount(const QModelIndex& = QModelIndex()) const;
		QVariant data(const QModelIndex&, int) const;
		QVariant headerData(int, Qt::Orientation, int) const;
	private:
		xGamepad* gpad;
		QList<int> shown;
};

// the inputs of a row, and where a key pressed to add one goes
class xInputBox : public QLabel {
	public:
		xInputBox(QWidget* = nullptr);
		std::function<void(int)> onKey;		// XKEY_*
	protected:
		void keyPressEvent(QKeyEvent*);
		void mousePressEvent(QMouseEvent*);
};

// One row: what it presses on the left, what presses it on the right.
class xPadRowEdit : public QDialog {
	public:
		xPadRowEdit(QWidget* = nullptr);
		bool edit(xGamepad*, int);		// -1 for a new extra row; true if it was changed
	protected:
		bool eventFilter(QObject*, QEvent*);
	private:
		xGamepad* gpad;
		int idx;
		xPadRow cur;
		bool fresh;				// the next input caught replaces the list
		QLabel* labFixed;			// a joystick row's target, from the joystick
		QWidget* tgtBox;
		QRadioButton* rbKey;
		QRadioButton* rbJoy;
		QRadioButton* rbCut;
		QRadioButton* rbKept;
		QComboBox* cbKey;
		QComboBox* cbMod;			// Caps or Symbol Shift held with the key
		QLabel* labMod;
		QPushButton* btnPress;
		QComboBox* cbJoy;
		QComboBox* cbCut;
		QLabel* labKept;
		QList<xJoyMapEntry> kept;		// a target the editor has no control for, as it was
		xInputBox* inBox;
		QPushButton* btnPick;			// "Add from list", every input by name
		QList<xJoyMapEntry> choices;		// what its menu offers
		QPushButton* btnDefault;
		QLabel* labHint;
		QCheckBox* chkTurbo;
		QList<xJoyMapEntry>& inputs();
		bool& defFlag();
		void addInput(const xJoyMapEntry&);
		void showInputs();
		bool takeTarget();
		void syncTarget();
};

// One player: which device, which joystick, and the table.
class xGamepadWidget : public QWidget {
	public:
		xGamepadWidget(xGamepad*, QWidget* = nullptr);
		void refresh();
		void updateList();
		void keyTry(int);			// a key pressed in the window, for the try line
		int nameWidth();			// the name column, as wide as its longest
		void setNameWidth(int);
		std::function<void()> changed;		// the player's input now does something else
	private:
		xGamepad* gpad;
		xPadTableModel* model;
		QComboBox* cbDevice;
		QLabel* labTry;
		QButtonGroup* grpScheme;
		QComboBox* cbQaop;
		QTableView* table;
		QSlider* sldTurbo;
		QLabel* labTurbo;
		QLabel* labDeadName;
		QSlider* sldDead;
		QLabel* labDead;
		xPadRowEdit* editor;
		void setDevFromCombo();
		void schemeFromControls();
		void inputChanged(int, int, int);
		void tryShow(const xJoyMapEntry&);
		void editRow(int);
		void delRow(int);
		void toggleTurbo(int);
		void rowMenu(const QPoint&);
		void saveAs();
		void load();
		void reset();
		void tableChanged();
		void tell();
};

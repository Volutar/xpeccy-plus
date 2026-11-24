#pragma once

// The knobs of the chosen Display look: a frame a group of the shader's parameters,
// built from the shader's file. Every move is shown at once and kept with the look.

#include "displaylook.h"

#include <QDialog>

class QPushButton;
class QVBoxLayout;

class xDisplaySheet : public QDialog {
	Q_OBJECT
	public:
		xDisplaySheet(QWidget* parent = nullptr);
		void setLook();			// the chosen look's knobs, where they stand
	signals:
		void s_param(const QString& name, float value);	// a knob moved
		void s_look();			// another look is chosen, or every knob is back
	protected:
		void hideEvent(QHideEvent*) override;
	private:
		xLook look;
		bool all = false;		// All settings
		QVBoxLayout* lay;
		QWidget* body = nullptr;
		QPushButton* btnAll;
		QPushButton* btnDef;
		QPushButton* btnSave;
		QPushButton* btnDel;
		void build();
		QWidget* knob(const xShdParam&);
		void moved(const QString& name, float value);
		void defaults();
		void savePreset();
		void deletePreset();
};

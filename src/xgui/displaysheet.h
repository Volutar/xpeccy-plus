#pragma once

// The knobs of the chosen Display look: a frame a group of the shader's parameters,
// built from the shader's file. Every move is shown at once and kept with the look.

#include "displaylook.h"

#include <QDialog>
#include <QHash>

class QLabel;
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
		void s_flat();			// a curvature knob moved turned Flat screen off: the chain and the switches follow
	protected:
		void showEvent(QShowEvent*) override;
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
		QLabel* gpuLab;			// what the look costs, while the window is up
		QHash<QString, float> lastOn;	// a switched-off group's value, to come back to
		QString rows;			// the knobs build() made, as shown() has them
		void build();
		void refresh();			// the knobs made set to where they stand, nothing made anew
		QString shown() const;		// the knobs build() would make now
		QVector<xShdParam> params() const;
		bool param(const QString& name, xShdParam* out) const;
		QWidget* knob(const xShdParam&);
		void moved(const QString& name, float value);
		void defaults();
		void savePreset();
		void deletePreset();
};

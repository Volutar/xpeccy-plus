#pragma once

#include <QDialog>

#include "options/opt_gamepad.h"

// Gamepads: each player's device, the joystick it stands for and its table.
// Live like the other tool windows - there is nothing to apply - and saved
// when it closes.
class xPadWin : public QDialog {
	public:
		xPadWin(QWidget* = NULL);
		void showWindow();
		void execOver();
		void sync();				// the tables again, if they are up
		std::function<void()> gameEdited;	// player 1's table changed by hand
	protected:
		bool eventFilter(QObject*, QEvent*);
		void showEvent(QShowEvent*);
		void hideEvent(QHideEvent*);
	private:
		xGamepadWidget* pan[2];
		void refreshAll();
		void sameWidths();
};

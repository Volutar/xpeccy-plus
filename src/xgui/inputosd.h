#pragma once

#include <QImage>
#include <QPainter>
#include <QRect>

#include "../libxpeccy/input/inview.h"

// what lights up: the 40 keys, the joystick's 8 bits, the mouse's 3 buttons,
// its wheel and its movement
enum {LIT_KEY = 0, LIT_JOY = 40, LIT_BTN = 48, LIT_WHEEL = 51, LIT_MOVE, LIT_COUNT};

// The player's input drawn over the picture: the ZX keyboard, the kempston
// joystick and the kempston mouse. Painted, not a picture, so it scales with
// the window and stays see-through.
class xInputOsd {
	public:
		xInputOsd();
		void paint(QPainter&, const QRect& pic, const InState&, qreal dpr);
	private:
		// when each thing was last seen down, ms; a tap is held lit a while
		qint64 litAt[LIT_COUNT];
		int wheelDir;
		unsigned char wheelWas;
		double moveX;
		double moveY;
		unsigned char mxWas;
		unsigned char myWas;
		int mouseSeen;
		qint64 now;

		// The outlines and labels change only with the layout, so they are an
		// image of their own; the lit fills go under them, and only when one
		// of them moved is anything drawn again
		struct Layout {
			double u, dx, dy;
			int w, h, keys, joy, mouse, ext, joyLive, mouseLive;
			qreal dpr;
		} drawnFor;
		struct Lit {
			float lit[LIT_COUNT];
			int wheelDir;
			double moveX, moveY;
		} drawnLit;
		QImage baseImg;
		QImage osdImg;		// what goes on the window, whole

		double glow(qint64 at);
		void stamp(const InState&);
		void paintBlocks(QPainter&, const Layout&, const Lit*);
		void paintKeys(QPainter&, QPointF org, double u, const Lit*);
		void paintJoy(QPainter&, QPointF org, double u, const Lit*, bool ext);
		void paintMouse(QPainter&, QPointF org, double u, const Lit*);
};

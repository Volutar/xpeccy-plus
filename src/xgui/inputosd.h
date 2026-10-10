#pragma once

#include <QPainter>
#include <QRect>

#include "../libxpeccy/input/inview.h"

// The player's input drawn over the picture: the ZX keyboard, the kempston
// joystick and the kempston mouse. Painted, not a picture, so it scales with
// the window and stays see-through.
class xInputOsd {
	public:
		xInputOsd();
		void paint(QPainter&, const QRect& pic, const InState&, bool reads);
	private:
		// when each thing was last seen down, ms; a tap is held lit a while
		qint64 keyAt[8][5];
		qint64 joyAt[8];
		qint64 btnAt[3];
		qint64 wheelAt;
		int wheelDir;
		unsigned char wheelWas;
		qint64 moveAt;
		double moveX;
		double moveY;
		unsigned char mxWas;
		unsigned char myWas;
		int mouseSeen;
		qint64 now;

		double glow(qint64 at);
		void drawKey(QPainter&, const QRectF&, const char*, double lit, double unit);
		void drawButton(QPainter&, const QRectF&, const char*, double lit, double unit);
		void paintKeys(QPainter&, QPointF org, double u, const InState&);
		void paintJoy(QPainter&, QPointF org, double u, const InState&, bool ext);
		void paintMouse(QPainter&, QPointF org, double u, const InState&);
};

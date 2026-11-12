#pragma once

#include <QWidget>
#include <QImage>
#include <QPixmap>
#include <QElapsedTimer>
#include <QPointer>
#include <QMessageBox>
#include <QUrl>
#include <functional>
#include <vector>

class QPainter;

// Over the picture while the snow notice is up: the picture shaded, snow
// falling on it, a snowflake sign blinking in the corner. It ticks at the
// screen's rate (60 Hz at most) only while shown; hidden, it costs nothing.
// After the notice the window draws the sign alone, with drawSign().
class xSnowFall : public QWidget {
	public:
		xSnowFall(QWidget* parent);
		void start();
		void stop();
		std::function<QPoint()> signAt;	// the sign's top left, asked at every paint
		void drawSign(QPainter&, QPoint at);	// blinking by the wall clock
	protected:
		void paintEvent(QPaintEvent*) override;
		void timerEvent(QTimerEvent*) override;
		bool eventFilter(QObject*, QEvent*) override;
	private:
		struct Flake {
			float x, y;		// logical pixels
			float vy;		// fall, px/s
			float amp, freq, phase;	// its own sway
			float drift;		// how much of the wind it takes
			int layer;		// 0 far .. 2 near
			int kind;		// sprite of that layer
			float rot, spin;	// a crystal's turn, degrees
		};
		std::vector<Flake> flakes;
		std::vector<QPixmap> sprites[3];
		QImage sign[2];
		qreal spriteDpr;
		int tid;
		QElapsedTimer clock;
		qint64 lastNs;
		double t;
		void seed();
		void spawn(Flake&, bool top);
		void makeSprites();
		void step(double dt);
		double wind() const;
};

// The notice itself: a message and the choice, not modal - the game runs on
// under it. off() is called for "Turn Off for Now"; never() when it ends
// with "Don't show this again" ticked, however it ends; more is the manual.
QMessageBox* snow_notice_box(QWidget* parent, const QUrl& more, std::function<void()> off, std::function<void()> never);

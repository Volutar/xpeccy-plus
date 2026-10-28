#pragma once

#include <QWidget>
#include <QDialog>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPixmap>
#include <QColor>
#include <QResizeEvent>
#include <QShowEvent>
#include <QMoveEvent>

#include <QTimer>
#include <QElapsedTimer>
#include <QFileSystemWatcher>

#include "libxpeccy/input/input.h"
#include "xgui/vkbdskin.h"

class QMenu;

#if defined(__APPLE__)
// vkeyboard_mac.mm: let the window keep its shape by itself
void vkbd_set_aspect(QWidget*, int, int);
#endif

class keyWindow : public QDialog {
	Q_OBJECT
	public:
		keyWindow(QWidget* = NULL);
	signals:
		void s_key_press(QKeyEvent*);
		void s_key_release(QKeyEvent*);
	public slots:
		void switcher();
		void upd(Keyboard*);
		void watch(Keyboard*);	// repaint if the matrix moved with no event to say so
		void rall(Keyboard*);
		void hostKey(QByteArray, bool);	// a PC key pressed or let go, as the Spectrum keys it presses
		void snap();		// sit under the emulator window
		void fillMenu(QMenu*);	// its settings, added to that menu
	private slots:
		void redraw();		// the picture at the window's size, now
		void loadSkin();
		void pollHover();	// the key under the pointer, by where the pointer is
		void follow();		// a PC key's keys going down or up in the matrix, closely
	private:
		// a PC key held, or let go of while its keys are still in the matrix
		struct HostKey {
			QByteArray zx;		// sorted, so CS+0 and 0+CS are one
			bool down;
			qint64 upAt;		// ms of followClock when let go of
		};
		Keyboard* kb;
		keyEntry xent;
		VkSkin skin;
		QString skinWant;	// the skin asked for when it was loaded: by hand or by the machine
		QString skinShown;	// the one shown: another if that one has no layout
		QSizeF cnv;		// the layout's canvas, in its own units
		QPixmap pic;		// the picture as last drawn
		QPixmap litPic;		// a pad light's: every key lit, for over the pressed ones
		double picScale;	// device pixels per canvas unit it was drawn at
		QTimer drawTimer;	// a size being dragged is drawn when it settles
		QTimer hoverTimer;	// after a drag of the frame: pollHover() until Qt passes moves again
		QFileSystemWatcher watcher;	// a layout of the user's own, edited
		unsigned dock:1;	// glued under the emulator window
		int hover;		// the key under the pointer, -1 for none
		int held;		// the key the left button holds down, -1 for none
		QByteArray heldZx;	// what it pressed, to let go of the same
		QVector<bool> latched;	// keys a click put down for good
		QVector<int> firstCopy;	// of a key the skin has twice (the shifts), the left one
		QByteArray shownSig;	// matrixSig() of the last paint
		QList<HostKey> host;	// the PC keys that went to the machine and are not all up yet
		QTimer followTimer;	// a short close look at the matrix after a PC key
		QElapsedTimer followClock;
		qint64 followEnd;	// ms of followClock to look until
		int keyAt(QPointF);
		QByteArray keysDown();
		QByteArray matrixSig();
		void hostTidy(const QByteArray&);
		QVector<double> lights(const QByteArray&);
		void pressKeys(const QByteArray&, bool);
		void latchKey(int, const QByteArray&);
		void placeUndocked();
		double scale();
		double devScale();
		QPointF picOrigin();
		double storedZoom();
		int higFor(int);
		int widFor(int);
		QPointF imgPos(QPoint);
		void setZoom(double);
		void setDock(bool);
		void showMenu(QPoint);
		void paintEvent(QPaintEvent*);
		void mousePressEvent(QMouseEvent*);
		void mouseReleaseEvent(QMouseEvent*);
		void mouseMoveEvent(QMouseEvent*);
		void leaveEvent(QEvent*);
		void keyPressEvent(QKeyEvent*);
		void keyReleaseEvent(QKeyEvent*);
		void resizeEvent(QResizeEvent*);
		void moveEvent(QMoveEvent*);
		void showEvent(QShowEvent*);
#if defined(_WIN32)
	#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
		bool nativeEvent(const QByteArray&, void*, qintptr*);
	#else
		bool nativeEvent(const QByteArray&, void*, long*);
	#endif
#endif
};

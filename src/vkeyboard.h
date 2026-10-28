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
		void snap();		// sit under the emulator window
		void fillMenu(QMenu*);	// its settings, added to that menu
	private slots:
		void redraw();		// the picture at the window's size, now
		void loadSkin();
		void pollHover();	// the key under the pointer, by where the pointer is
	private:
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
		QVector<bool> latched;	// combination keys the right button put down
		QByteArray shownSig;	// matrixSig() of the last paint
		int keyAt(QPointF);
		QByteArray keysDown();
		QByteArray matrixSig();
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

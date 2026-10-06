#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QStatusBar>
#include <QToolBar>
#include <QTimer>
#include <QWidget>
#include <QString>

#ifdef USENETWORK
#include <QTcpServer>
#include <QTcpSocket>
#endif

#include <SDL.h>

#include "xcore/xcore.h"
#include "xgui/xgui.h"
#include "xgui/diskwin.h"
#include "xgui/padwin.h"
#include "libxpeccy/spectrum.h"
#include "watcher.h"
#include "vkeyboard.h"
#include "ethread.h"

// The two keys that let the mouse and the keyboard go, pressed together and let
// go with nothing between them: Ctrl and the second one. On a Mac Qt names Cmd
// Control and Ctrl Meta.
#ifdef __APPLE__
#define XREL_KEY2 Qt::Key_Meta
#define XREL_MODS (Qt::ControlModifier | Qt::MetaModifier)
#define XREL_KEYS "Ctrl+Cmd"
#else
#define XREL_KEY2 Qt::Key_Alt
#define XREL_MODS (Qt::ControlModifier | Qt::AltModifier)
#define XREL_KEYS "Ctrl+Alt"
#endif

class xSatFilter;

#if USE_QT_GAMEPAD
#include <QGamepad>
#include <QGamepadManager>
#endif

// for windows
#define STICKY_KEY 1

inline qreal widgetDpr(const QWidget* w) {
#if QT_VERSION >= QT_VERSION_CHECK(5,6,0)
	return w->devicePixelRatioF();
#elif QT_VERSION >= QT_VERSION_CHECK(5,0,0)
	return w->devicePixelRatio();
#else
	return 1.0;
#endif
}

enum {
	led_kbd = 0,
	led_joy,
	led_mouse,
	led_tap_red,
	led_tap_yellow,
	led_disk_green,
	led_disk_red,
	led_wav,
	leds_count
};

// what the machine's speed is doing, shown in the top right corner
enum {
	osd_none = 0,
	osd_fast,
	osd_rewind,
	osd_pause,
	osd_ffwd2,	// x2, then x4 and x8 after it
	osd_slow2 = osd_ffwd2 + 3,	// 1/2, then 1/4 and 1/8
	osd_rec = osd_slow2 + 3,	// recording, taking turns with the others
	osd_rec_off,	// its other phase, with no speed mode to take turns with
	osd_count
};

typedef struct {
	int showTime;	// in 1/50 sec
	int x;
	int y;
	QString imgName;
} xLed;

// QOpenGLWidget since Qt5.4

#define BLOCKGL 0
#define USELEGACYGL 0
#define ISLEGACYGL ((QT_VERSION < QT_VERSION_CHECK(5,4,0)) || (USELEGACYGL && (QT_VERSION < QT_VERSION_CHECK(6,0,0))))

#ifdef USEOPENGL
	#include <QtOpenGL>

	#if ISLEGACYGL
		class MainWin : public QGLWidget {
	#else
		#include <QOpenGLWidget>
		#include <QOpenGLBuffer>
		#include <QOpenGLVertexArrayObject>
		class MainWin : public QOpenGLWidget, protected QOpenGLFunctions {
	#endif
#else
	class MainWin : public QWidget {
#endif
	Q_OBJECT
	public:
		MainWin(QMainWindow*);
		~MainWin();
//		Computer* comp;
		void checkState();
		void addSatellite(QWidget*);
		int hotkeyOf(QKeyEvent*);
		void hotkeysNote();
		void loadLabels(const char*);
		void fillUserMenu();
		void openMedia(const QString& path, int id, int drv, int run);
		void fsOverlay(QWidget*);
		void setMachine(const std::string&);
		void resetMachine(int);
		void resetTo(int);
		void fillDrivesMenu();
		void driveOp(std::function<void()>);
		void diskOp(int, int);
		void diskNew(int);
		void diskSave(int, bool);
		void diskEject(int);
		void hddOpen(int, bool);
		void hddProps(int);
		void sdcOpen(bool);
		void setTurbo(int);
		void addFavorite(const QString& path);
		bool recStart(const QString& file = QString());
	signals:
		void s_options();
		void s_hotkeys();
		void s_debug();
		void s_debug_off();
		// void s_prf_change(xProfile*);
		void s_scradr(int, int, int, int);

		void s_tape_show();
		void s_tape_progress(Tape*);
		void s_tape_upd(Tape*);
		void s_tape_blk(Tape*);

		void s_step();

		void s_rzx_start();
		void s_rzx_stop();
		void s_rzx_upd(Computer*);
		void s_rzx_show();
		void s_watch_upd(Computer*);
		void s_watch_show();
		void s_scr_show();
		void s_snd_show();
		void s_keywin_rall(Keyboard*);
		void s_keywin_upd(Keyboard*);
		void s_keywin_shide();
		void s_keywin_close();
		void s_keywin_snap();		// the window moved or changed size
		void s_emulwin_close();
	public slots:
		void d_frame();
		void doOptions();
		void padWinShow();
		void padWinModal();
		void doDebug();
		void updateWindow();
		void pause(bool, int);
		void tapStateChanged(int,int);
		void onPrfChange();
		void kPress(QKeyEvent*);
		void kRelease(QKeyEvent*);
		void loadShader();
	private slots:
		void updateSatellites();
		void menuHide();
		void optResize();
		void optApply();
		void dbgReturn();
		void rzxStateChanged(int);
		void profileSelected(QAction*);
		void shdSelected(QAction*);
		void keySelected(QAction*);
		void palSelected(QAction*);
		void reset(QAction*);
		void gpInputChanged(int, int, int);
		void connected();
		void disconnected();
		void socketRead();

		void debugAction();
		void frame_timer();
	private:
		unsigned grabMice:1;
		long long mouseReadAt = 0;	// when a program last read the mouse, host ns
		bool relArmed = false;		// both release keys are down, nothing else since
		xSatFilter* satFilter = nullptr;	// passes the tool windows' hotkeys on to this one
		void releaseChord(QKeyEvent*, bool);
		void fast_key(bool);
		QPoint warpAt;		// where the last recentering aimed
		unsigned char warpTtl;	// events it stays a candidate for
		unsigned char warpFail;	// recenterings the host undid in a row
		QPoint mouseLast;	// where the pointer was at the previous move
		double mouseRemX = 0.0;	// what the division by the zoom left over
		double mouseRemY = 0.0;
		unsigned block:1;
		unsigned hasPicture:1;	// the emulation has handed over a frame
		int upSwaps;		// bufSwaps at the last upload the timer made, -1: upload
		unsigned refit:1;	// geometry changed: re-read the frame before painting it
		unsigned paintOwed:1;	// presentFrame asked, or resizeGL left an empty framebuffer

		std::string shdLoaded;	// the shader the program is linked with now
		int mediaSrc;		// where the image in use is: a drive, the tape, or a snapshot
		int mediaSeen;		// the drive motors (bits 0..3) and the tape playing, as last seen
		QByteArray mediaRaw[5];	// what each drive and the tape held, as last seen
		void watchMedia();
		double hwMulSeen = 1.0;	// the board's turbo as last reported
		std::string macSeen;	// ...on this machine
		void watchClock();
		void watchPads();
		void fillPadMenu();
		int padK8 = -1;		// the machine's Kempston had 8 buttons, as last seen
		void showMedia(const QString&, int src);
		std::string wantedShader();

		int timid;
		int secid;
		int cmsid;
		QImage leds[leds_count];
		QImage osdImg[osd_count];

		QTimer frm_tmr;
		int frm_ns;

		int scrCounter;
		int scrInterval;

		QImage alphabet;
		void drawText(QPainter*, int, int, const char*);

#ifdef USENETWORK
		QTcpServer srv;
		QList<QTcpSocket*> clients;
#endif

		int msgTimer;
		QString msg;
		void setMessage(QString, double = 2.0);

		bool saveChanged();
		void updateHead();
		void screenShot();
		void videoRec();
		void grabScreen();
		void drawPicture();
		QRect modeSlot();
		int speedOsd();
		int recOsd();
		void drawIcons(QPainter&);
		void presentFrame();
		void uploadFrame();
		void uploadOffPaint();
		void renderFrame();

#if USE_QT_GAMEPAD
		QGamepadManager* gpadmgr;
#endif
		void mapJoystick(xGamepad*, Computer*, int, int, int);
		void mapPress(Computer*, xJoyMapEntry);
		void mapRelease(Computer*, xJoyMapEntry);
#ifdef USENETWORK
		void openServer();
		void closeServer();
#endif
		QMenu* userMenu;
		QMenu* bookmarkMenu;
		QMenu* profileMenu;
		QMenu* resMenu;
		QMenu* dskMenu;		// the floppies; its root opens the Disk manager
		QMenu* cartMenu;
		QMenu* sdcMenu;
		QMenu* hddMenu;
		xDiskWin* diskWin;
		xPadWin* padWin;
		QMenu* turboMenu;
		QMenu* shdMenu;
		QMenu* keyMenu;
		QMenu* padMenu;		// the window, then each player's joystick
		QMenu* palMenu;
		QMenu* dbgMenu;
		QAction* pckAct;

		void initUserMenu();
		// the menu bar of the window around this one
		QMainWindow* frame;
		QMenu* fileMenu;
		QMenu* viewMenu;
		QAction* recAct;
		QAction* wavAct;
		QAction* fullAct;
		QAction* ratioAct;
		QList<QAction*> sizeActs;
		typedef struct {QAction* act; QString name; int id;} xCutAct;
		QList<xCutAct> cutActs;		// menu items that are hotkeys, to show the key
		QHash<int, QAction*> cutById;
		QAction* cutAct(const QString& name, int id, const QString& icon = QString());
		QAction* cutAction(QMenu*, const QString& name, int id, const QString& icon = QString());
		QMenu* sizeMenu;
		QMenu* recentMenu;
		void fillRecent();
		// the toolbar and the status bar (emw_bars.cpp)
		QToolBar* toolBar;
		QToolBar* fsTool = nullptr;	// the same buttons over the picture in fullscreen, under fsBar
		QStatusBar* statusBar;
		QLabel* sbMachine;
		QLabel* sbClock;
		QLabel* sbTape;
		QLabel* sbDisk[4];	// "A:" and so on
		QLabel* sbDiskIcon[4];
		QWidget* sbDiskBox[4] = {nullptr, nullptr, nullptr, nullptr};
		QLabel* sbTapeIcon;
		QWidget* sbTapeBox = nullptr;
		QPixmap sbPix[7];	// SB_*
		int sbTapeShown = -1;
		int sbDiskShown[4] = {-1, -1, -1, -1};
		int flpSeen[4] = {0, 0, 0, 0};	// bit 0 read, bit 1 written since the status bar last looked
		QMenu* flpMenu[4] = {nullptr, nullptr, nullptr, nullptr};	// each drive's own, from the Drives menu
		void tapeMenu(const QPoint&);
		QLabel* sbFps;
		QAction* tbShowAct;
		QAction* sbShowAct;
		QAction* pauseAct;
		QAction* fastAct;
		QAction* tapeAct;
		QAction* tapeRecAct;
		QAction* mouseAct;
		QAction* ffAct;
		QAction* slowAct;
		QAction* rewAct;
		QAction* diskAct;
		QAction* muteAct;
		QAction* watchAct;
		QMenu* helpMenu;
		typedef struct {QString id; QString group; QAction* act; int kind; QMenu* list;} xTbItem;
		QList<xTbItem> tbCatalog;	// everything a button can be
		QStringList tbList;		// what the bar holds, ids and separators
		const xTbItem* tbFind(const QString&);
		void initBars();
		void initMachineMenus();
		void tbBuild();
		void tbFill(QToolBar*);
		void tbApply();
		void tbMove(int, int);
		void tbMenu(const QPoint&, int);
		void showBars();
		void syncActions();
		QMenu* volumeMenu();
		class xVolBox* volPop;
		QTimer volTimer;
		void volumeChanged();
		void updateStatus();
		void cutTexts();
		QString cutKey(int);
		void initMenuBar();
		// the menu over the picture in fullscreen, while the pointer is at the top
		QMenuBar* fsBar;
		QTimer fsShow;
		int fsY;		// where the pointer was last seen over the picture
		unsigned fsTall:1;	// a line taller than the screen, while a menu is up
		bool fsCompose(bool);
		bool fpsOsd();
		bool fsHeld();
		void popupUserMenu(const QPoint&);
		void placeWindow();
		void fsReveal(int y);
		void fsHide();
		void showAbout();
		void favManage();
		void calcCoords(QMouseEvent*);
		QPoint winCenter();		// the middle of the window, on the screen
		void winCursorTo(QPoint);
		void mouseGrabOn();
		void mouseGrabOff();
		void mouseRecenter(int fresh = 0);
		void dropAsk(QString);
		int askRun();

		void xkey_press(int, bool cmd = false);
		void xkey_release(int);
		void xcut_release(int);
		int mapHotkey(const QKeySequence&, int, Qt::Key*, Qt::KeyboardModifier*);
		bool mapIsHotkey(const xJoyMapEntry&);
		void mapReplayHeld(xGamepad*);
		bool padKey(QKeyEvent*, bool);
		bool padLive();
		void mapOut(Computer*, const xJoyMapEntry&);
		long long padTurboNs = 0;	// when turbo was last stepped
		void mapZxKey(Computer*, int, bool);
		void mapKeySeq(const xJoyMapEntry&, bool);

		void closeEvent(QCloseEvent*);
		bool eventFilter(QObject*, QEvent*) override;
		bool focusNextPrevChild(bool) override {return false;}	// Tab is a key of the machine
		void dragEnterEvent(QDragEnterEvent*);
		void dropEvent(QDropEvent*);
		void paintEvent(QPaintEvent*);
		void keyPressEvent(QKeyEvent*);
		void keyReleaseEvent(QKeyEvent*);
		void mousePressEvent(QMouseEvent*);
		void contextMenuEvent(QContextMenuEvent*);
		void mouseReleaseEvent(QMouseEvent*);
		void mouseMoveEvent(QMouseEvent*);
		void wheelEvent(QWheelEvent*);
		void focusOutEvent(QFocusEvent*);
		void focusInEvent(QFocusEvent*);
		void timerEvent(QTimerEvent*);
#if defined(USEOPENGL) && !BLOCKGL
		unsigned curtex:2;
		GLuint texids[4];
		GLuint curtxid;
		QList<GLuint> queue;
		void initializeGL();
		void resizeGL(int,int);
		void paintGL();
		void cleanupGL();
#if ISLEGACYGL
		QGLContext* cont;
		QGLShaderProgram prg;
		QGLShader* vtx_shd;
		QGLShader* frg_shd;
#else
		QOpenGLShaderProgram prg;
		QOpenGLShader* vtx_shd;
		QOpenGLShader* frg_shd;
		QOpenGLVertexArrayObject vao;
		QOpenGLBuffer vbo;
#endif
#endif
};

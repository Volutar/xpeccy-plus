#pragma once

#include <QLabel>
#include <QProgressBar>
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
#include "xgui/cheatwin.h"
#include "libxpeccy/spectrum.h"
#include "watcher.h"
#include "vkeyboard.h"
#include "xgui/inputosd.h"
#include "ethread.h"

// the Reset menu's power cycle, beside the RES_* starts
#define RES_POWER	-1

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
	led_disk_green,
	led_disk_red,
	led_wav,
	leds_count
};

// The lights of the tape, the drives and the inputs, in the status bar and over the picture.
// Each READ right after its IDLE.
enum {SB_TAPE_EMPTY = 0, SB_TAPE_IN, SB_TAPE_RD, SB_TAPE_WR, SB_TAPE_RD_HI, SB_TAPE_WR_HI,
	SB_DISK_EMPTY, SB_DISK_IN, SB_DISK_RD, SB_DISK_WR, SB_DISK_OUT,
	SB_CARD_EMPTY, SB_CARD_IN, SB_CARD_RD, SB_CARD_WR,
	SB_JOY_IDLE, SB_JOY_READ, SB_MOUSE_IDLE, SB_MOUSE_READ, SB_COUNT};

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
	osd_rzx,	// the same for an RZX recording
	osd_rzx_off,
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
	#if !ISLEGACYGL
		#include <QOpenGLWidget>
		#include <QOpenGLBuffer>
		#include <QOpenGLVertexArrayObject>
	#endif
#endif

class MainWin;

#if defined(USEOPENGL) && !BLOCKGL
// Whether the picture can go through OpenGL here; why not, when it cannot
bool gl_probe(QString* why);

// The picture through OpenGL: a child that covers MainWin and only draws.
// Keys and the mouse go past it to MainWin. Made only when gl_probe() says
// yes - with no context a GL widget takes the whole window down.
#if ISLEGACYGL
class xGLView : public QGLWidget {
#else
class xGLView : public QOpenGLWidget {
#endif
	public:
		xGLView(MainWin*);
	protected:
		void initializeGL() override;
		void resizeGL(int, int) override;
		void paintGL() override {}
		void paintEvent(QPaintEvent*) override;
	private:
		MainWin* mw;
};
#endif

#if defined(USEOPENGL) && !BLOCKGL && !ISLEGACYGL
class MainWin : public QWidget, protected QOpenGLFunctions {
#else
class MainWin : public QWidget {
#endif
	Q_OBJECT
	friend class xGLView;
	public:
		MainWin(QMainWindow*);
		~MainWin();
//		Computer* comp;
		void rzxWatch();		// the RZX player told of a recording opened or closed
		void addSatellite(QWidget*, bool allKeys = false);
		int hotkeyOf(QKeyEvent*);
		void hotkeysNote();
		void setMessage(QString, double = 2.0, bool hold = false);
		void fillUserMenu();
		void openMedia(const QString& path, int id, int drv, int run, bool pinned = false);
		void padGame(const QString&);		// an image opened: player 1 takes its .pad, or its own table back
		bool openCheats(const QString&);	// a .pok, into the Cheats window
		void fsOverlay(QWidget*);
		void setMachine(const std::string&);
		void chooseMachine(const std::string&);		// the user's pick, see xm_choose
		void askStartMachine();
		void resetMachine(int);
		void resetTo(int);
		void hardReset();
		void divToggle(bool ask = true);	// the divMMC in or out, with the power off
		void addDivCards(QMenu*);
		bool drivesBack();		// asks to switch the divMMC off when it has the drives out
		void folderAsk();		// the first write to a folder asks before the folder changes
		void nextCard(bool asked = false);		// the Next's own card: made from System/Next, or one the user has
		void folderNews();		// says once when a folder could not take what the machine wrote
		void esxCheck();		// says when card 1 and the EEPROM are different esxDOS releases
		QString esxSeen;		// the power cycle and card it looked at last
		void fillDrivesMenu();
		void driveOp(std::function<void()>);
		void diskOp(int, int);
		void diskNew(int);
		void diskSave(int, bool);
		void diskEject(int);
		void tapeEject();
		void hddOpen(int, bool);
		void hddProps(int);
		void sdcOpen(int, bool);
		void sdcNewEsx(int);
		void fillCardMenu(QMenu*, int);
		void fillDivMenu();
		void setTurbo(int);
		void addFavorite(const QString& path);
		bool recStart(const QString& file = QString());
	signals:
		void s_options();
		void s_hotkeys();
		void s_debug();
		void s_debug_off();
		void s_machine();		// setMachine() switched to another one
		// void s_prf_change(xProfile*);
		void s_scradr(int, int, int, int);

		void s_tape_show();
		void s_tape_progress(Tape*);
		void s_tape_upd(Tape*);
		void s_tape_blk(Tape*);

		void s_step();

		void s_rzx_rec(bool);		// recording started or stopped
		void s_rzx_start();		// a recording opened, the one rzx_playing names
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
		void s_keywin_menu(QMenu*);	// its settings wanted in that menu
		void s_emulwin_close();
	public slots:
		void d_frame();
		void doOptions();
		void padWinShow();
		void padWinModal();
		void cheatWinShow();
		void doDebug();
		void updateWindow();
		void pause(bool, int);
		void tapStateChanged(int,int);
		void onPrfChange();
		void kPress(QKeyEvent*);
		void kRelease(QKeyEvent*);
		void loadShader();
		bool reloadMedia();
	private slots:
		void updateScreen(QScreen*);
		void vsyncProbe();
		void vsyncUpdate();
		void updateSatellites();
		void menuHide();
		void optResize();
		void optApply();
		void dbgReturn();
		void rzxStateChanged(int);
		void rzxSeek(int);
		void rzxReplay(QString);
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
		QMetaObject::Connection screenGeometryConnection;
		QMetaObject::Connection screenRateConnection;
		int vsyncShown = 0;		// the rate x100 the VSync note last gave
		QPoint fsHome;		// where the window goes back to from fullscreen, taken on the way in
		xSatFilter* satFilter = nullptr;	// passes the tool windows' hotkeys on to this one
		unsigned rzxSeen = 0;			// the recording the player was last told of
		void releaseChord(QKeyEvent*, bool);
		void fast_key(bool);
		void fast_toggle();
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
		int mediaSrc;		// where the image in use is: a drive, the tape, a divMMC card, or a snapshot
		int mediaSeen;		// the drive motors (bits 0..3), the tape playing, the cards read, as last seen
		QByteArray mediaRaw[7];	// what each drive, the tape and the cards held, as last seen
		int cardReads[2];	// the divMMC cards' read counts as last seen
		void watchMedia();
		double hwMulSeen = 1.0;	// the board's turbo as last reported
		std::string macSeen;	// ...on this machine
		void watchClock();
		void watchPads();
		void fillPadMenu();
		bool padGameEdited();
		int padK8 = -1;		// the machine's Kempston had 8 buttons, as last seen
		void showMedia(const QString&, int src);
		std::string wantedShader();

		int timid;
		int secid;
		int cmsid;
		QImage leds[leds_count];
		QImage osdImg[osd_count];
		xInputOsd inputOsd;
		struct xIosdItem {QAction* act; int* field; int val;};	// val < 0: a switch
		std::vector<xIosdItem> iosdItems;
		void initIosdMenu(QMenu*);

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
		bool msgHeld = false;	// msg stays its time out, see setMessage
		QString msgNext;	// what came meanwhile, shown after it
		int msgNextTimer = 0;

		bool saveChanged(bool tape);
		void updateHead();
		void screenShot();
		void videoRec();
		void rzxRec(bool finalize = false);
		void rzxContinue();
		void rzxContStep();
		void rzxFinalizeFile();
		int rzxContStage = 0;		// Open and Continue: 1 waits for the playback, 2 for the run to its end
		void grabScreen();
		void drawPicture();
		QRect modeSlot();
		int speedOsd();
		int recOsd();
		void drawIcons(QPainter&);
		void drawInputOsd(QPainter&);
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
		QMenu* drvMenu;		// the same items under Media's Floppy Drives
		QMenu* cartMenu;
		QMenu* sdcMenu;
		QMenu* hddMenu;
		xDiskWin* diskWin;
		xPadWin* padWin;
		xCheatWin* cheatWin;
		QMenu* turboMenu;
		QMenu* shdMenu;
		QMenu* keyMenu;
		QMenu* padMenu;		// the window, then each player's joystick
		QMenu* palMenu;
		QMenu* dbgMenu;		// the toolbar's Debugger button: a click opens it, its list the panels
		QMenu* toolsMenu;	// the right-click menu's Tools, the menu bar's list
		QAction* pckAct;

		void initUserMenu();
		// the menu bar of the window around this one
		QMainWindow* frame;
		QMenu* fileMenu;
		QMenu* viewMenu;
		QAction* recAct;
		QAction* rzxRecAct;
		QAction* wavAct;
		QAction* fullAct;
		QAction* ratioAct;
		QList<QAction*> sizeActs;
		typedef struct {QAction* act; QString name; int id;} xCutAct;
		QList<xCutAct> cutActs;		// menu items that are hotkeys, to show the key
		QHash<int, QAction*> cutById;
		QAction* cutAct(const QString& name, int id, const QString& icon = QString());
		QAction* cutAction(QMenu*, const QString& name, int id, const QString& icon = QString());
		QMenu* keywinMenu(QMenu*);
		QMenu* sizeMenu;
		QMenu* brdMenu;
		void fillBorderMenu();
		QMenu* recentMenu;
		void fillRecent();
		// the toolbar and the status bar (emw_bars.cpp)
		QToolBar* toolBar;
		QToolBar* fsTool = nullptr;	// the same buttons over the picture in fullscreen, under fsBar
		QStatusBar* statusBar;
		QLabel* sbMachine = nullptr;
		QLabel* sbClock;
		QLabel* sbTape;
		QProgressBar* sbTapeBar;	// where the tape stands in this block
		QLabel* sbDisk[4];	// "A:" and so on
		QLabel* sbDiskIcon[4];
		QWidget* sbDiskBox[4] = {nullptr, nullptr, nullptr, nullptr};
		QLabel* sbCard;		// "SD" and the divMMC's light
		QLabel* sbCardIcon;
		QWidget* sbCardBox = nullptr;
		int sbCardShown = -1;
		int cardLight = SB_CARD_IN;
		int cardLook[2][2] = {{0, 0}, {0, 0}};	// the cards' reads and writes at the last look
		QLabel* sbTapeIcon;
		QWidget* sbTapeBox = nullptr;
		QLabel* sbJoyIcon;
		QLabel* sbMouseIcon;
		QWidget* sbInputBox = nullptr;
		QPixmap sbPix[SB_COUNT];
		int sbTapeShown = -1;
		int sbDiskShown[4] = {-1, -1, -1, -1};
		int sbJoyShown = -1;
		int sbMouseShown = -1;
		int flpSeen[4] = {0, 0, 0, 0};	// bit 0 read, bit 1 written, bit 2 the head stepped, since the last look
		int flpTrk[4] = {0, 0, 0, 0};	// the track the head was on the frame before
		int tapeLight = SB_TAPE_EMPTY;	// SB_* as the last look found them
		int diskLight[4] = {SB_DISK_EMPTY, SB_DISK_EMPTY, SB_DISK_EMPTY, SB_DISK_EMPTY};
		bool diskDark[4] = {false, false, false, false};	// put out for a step at the last look
		int lightPhase = 0;
		void lightLook();
		bool joySeen = false;		// read since the status bar last looked
		bool mouseSeen = false;
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
		int slowShown = 0;		// the slow speed its icon shows
		int ffShown = 0;		// the fast forward speed its icon shows
		int tbHeld = 0;			// the time key a toolbar button holds down
		bool tbDown = false;		// a time button is down: its click is not a switch
		QAction* rewAct;
		QAction* diskAct;
		QAction* divAct;		// the divMMC on or off: hotkey and toolbar
		QAction* divItem;		// the same in the menus while it is off
		QMenu* divMenu;			// while it is on: its cards, and the switch
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
		void tbSize(QToolBar*, int);
		void tbAuto();
		QTimer tbRestyled;		// a style or a font changed the buttons' widths
		void tbApply();
		void tbMove(int, int);
		void tbMenu(const QPoint&, int);
		void showBars();
		void syncActions();
		void speedIcons();
		void tbLetGo();
		QMenu* volumeMenu();
		class xVolBox* volPop;
		QTimer volTimer;
		void volumeChanged();
		void updateStatus();
		void cutTexts();
		void initMenuBar();
		// the menu over the picture in fullscreen, while the pointer is at the top
		QMenuBar* fsBar;
		QTimer fsShow;
		int fsY;		// where the pointer was last seen over the picture
		unsigned fsTall:1;	// a line taller than the screen, while a menu is up
		bool fsCompose(bool);
		bool fpsOsd();
		bool fsHeld();
		bool fsCovers();
		void popupUserMenu(const QPoint&);
		void placeWindow();
		void fsReveal(int y);
		void fsHide();
		void showAbout();
		void openManual();
		void favManage();
		void calcCoords(QMouseEvent*);
		QPoint winCenter();		// the middle of the window, on the screen
		void winCursorTo(QPoint);
		void mouseGrabOn();
		void mouseGrabOff();
		void mouseRecenter(int fresh = 0);
		void dropAsk(QString);
		int askRun();
		int askRunKeep(const QString&);

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
		void machineKey(Computer*, keyEntry*, bool);
		void mapKeySeq(const xJoyMapEntry&, bool);

		void closeEvent(QCloseEvent*) override;
		bool eventFilter(QObject*, QEvent*) override;
		bool focusNextPrevChild(bool) override {return false;}	// Tab is a key of the machine
		void dragEnterEvent(QDragEnterEvent*) override;
		void dropEvent(QDropEvent*) override;
		void paintEvent(QPaintEvent*) override;
		void keyPressEvent(QKeyEvent*) override;
		void keyReleaseEvent(QKeyEvent*) override;
		void mousePressEvent(QMouseEvent*) override;
		void contextMenuEvent(QContextMenuEvent*) override;
		void mouseReleaseEvent(QMouseEvent*) override;
		void mouseMoveEvent(QMouseEvent*) override;
		void wheelEvent(QWheelEvent*) override;
		void focusOutEvent(QFocusEvent*) override;
		void focusInEvent(QFocusEvent*) override;
		void timerEvent(QTimerEvent*) override;
		void resizeEvent(QResizeEvent*) override;
		void paintPlain(QPainter&);
#if defined(USEOPENGL) && !BLOCKGL
		xGLView* glw = nullptr;		// the picture through OpenGL; none: drawn here, plain
		unsigned curtex:2;
		GLuint texids[4];
		GLuint curtxid;
		QList<GLuint> queue;
		void initializeGL();
		void resizeGL(int,int);
		void paintGLView();
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

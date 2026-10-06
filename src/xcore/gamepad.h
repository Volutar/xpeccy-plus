#pragma once

#include <cstdio>
#include <string>
#include <QMap>
#include <QObject>
#include <QKeySequence>
#include <QStringList>
#include <SDL_joystick.h>
#if HAVESDL2
#include <SDL_gamecontroller.h>
#endif

// joystick

// use QKeySequence to bind gamepad to pc keyboard, instead of single key
#define USE_SEQ_BIND 1

// What a map entry is bound to on the pad. The first three are raw SDL
// joystick numbers, which differ from pad to pad and from one platform to
// the next; the last two are SDL's normalized layout (A/B/X/Y, d-pad,
// sticks, triggers) and mean the same thing everywhere.
enum {
	JOY_NONE = 0,
	JOY_AXIS,
	JOY_BUTTON,
	JOY_HAT,
	JOY_CBUTTON,		// SDL_CONTROLLER_BUTTON_*
	JOY_CAXIS,		// SDL_CONTROLLER_AXIS_*
	JOY_KEY,		// a host key, num is its XKEY_*
	JOY_VDIR		// "Gamepad up": a direction on whatever the pad has for it, num is PR_*
};

enum {
	JMAP_NONE = 0,
	JMAP_KEY,
	JMAP_JOY,
	JMAP_JOYB,
	JMAP_MOUSE,
	JMAP_CUT,		// an emulator action, dir is its XCUT_*
	JMAP_ZX			// a Spectrum key, dir is its char in the keymap's zxKey terms
};

typedef struct {
	int type = JOY_NONE;	// axis/button
	int num = 0;		// number of axis/button
	int state = 0;		// -x/+x for axis, 0/x for button
	int dev = JMAP_NONE;	// device for action JMAP_*
#if USE_SEQ_BIND
	QKeySequence seq;	// key sequence to activate
#else
	int key;		// key XKEY_* for keyboard
#endif
	int dir = 0;		// XJ_* for kempston
	int rps = 0;		// repeat state (0:released, !0:pressed)
	int rpt = 0;		// repeat period (0 = no repeat)
	int cnt = 0;		// repeat counter
} xJoyMapEntry;

// The joystick a player's joystick rows stand for. Changing it changes what
// they press, never what presses them.
enum {
	GPS_KEMPSTON = 0,	// Fire 2..4 too when the machine's Kempston has them
	GPS_SINCLAIR1,		// Interface 2, keys 6-0
	GPS_SINCLAIR2,		// Interface 2, keys 1-5
	GPS_CURSOR,		// keys 5-8 and 0
	GPS_QAOP,		// fire on Space
	GPS_QAOPM,		// fire on M
	GPS_CUSTOM,		// Spectrum keys of the user's own
	GPS_COUNT
};

// the host keyboard as a player's device, in the two layouts it comes in
enum {
	GPK_NONE = 0,
	GPK_ARROWS,		// arrows and Ctrl
	GPK_WASD		// WASD and Space
};

// the joystick rows, in this order; extra rows follow them
enum {
	PR_UP = 0,
	PR_DOWN,
	PR_LEFT,
	PR_RIGHT,
	PR_FIRE,
	PR_FIRE2,		// an 8-button Kempston only
	PR_FIRE3,
	PR_FIRE4,
	PR_JOY
};

const char* pad_scheme_key(int);		// config word
const char* pad_scheme_name(int);		// what the gui says
const char* pad_kbd_key(int);			// GPK_* as config.conf has it, "" for none
int pad_kbd_find(const QString&);		// GPK_NONE if the word is not one
const char* pad_kbd_name(int);			// what the gui says
QString pad_key_name(int);			// a host key, XKEY_*, as the gui says it
const char* pad_role_name(int);			// Up, Down...
QString pad_zx_name(int);			// a Spectrum key, JMAP_ZX's dir
const char* pad_zx_keys();			// all forty of them, in the order of the keyboard

// One row of a player's table: what the Spectrum gets, and what presses it.
// A joystick row takes its target from the joystick, and its inputs are the
// defaults until the user picks others; an extra row has both of its own.
class xPadRow {
	public:
		QList<xJoyMapEntry> tgt;	// an extra row's targets; a joystick row's on Custom
		QList<xJoyMapEntry> pad;	// the pad inputs
		QList<xJoyMapEntry> key;	// the host keys
		bool padDef = true;		// a joystick row on its default pad inputs
		bool keyDef = true;		// ...and keys
		bool rapid = false;		// turbo: repeats while held, at the player's rate (turbo is a macro in fdc.h)
};

enum {
	GPBACKEND_NONE = 0,
	GPBACKEND_SDL,
	GPBACKEND_QT
};

// How a remembered pad is told apart from another one of the same model.
// Every pad of a model shares one guid, so this is what picks between two
// of them - best first, and which one was available is recorded in the
// config so the next run does not silently change its mind.
enum {
	GPD_NONE = 0,		// guid alone
	GPD_ORD,		// nth device with this guid, in SDL's order
	GPD_PATH,		// os device path: holds while the pad stays in one port
	GPD_SERIAL		// device serial: holds for good, but few pads report one
};

// What is remembered about a pad so it can be found again. guid is SDL's
// device guid - the same key gamecontrollerdb.txt is indexed by.
class xPadId {
	public:
		QString guid;
		QString disc;		// serial or path, per dtype
		QString name;		// for display only
		int dtype = GPD_NONE;
		int ord = 0;		// 1-based, for GPD_ORD

		bool isEmpty() const;
		bool sameAs(const xPadId&) const;
		QString toConfig() const;
		QString title() const;		// name for the gui, with the twin marker
		QString keyName() const;	// which key tells this one from its twins
		static xPadId fromConfig(QString);
};

// A device SDL can see right now. index is only good until the device list
// changes, which is why nothing outside a rescan holds on to it.
class xPadDev {
	public:
		xPadId id;
		QString label;		// how the gui names it, one of a kind in the list
		int index = -1;
		int player = -1;	// host's player number, -1 when it will not say
		bool ctrl = false;	// SDL knows a controller layout for it
};

class xGamepad : public QObject {
	Q_OBJECT
	public:
		xGamepad(QObject* = nullptr);
		~xGamepad();
		void openDev(const xPadDev&);
		void close();
		int isOpened();
		int getId();
		int isController();
		void setDeadZone(int);
		int deadZone();
		QString name();			// name of the open pad, empty if there is none
		xPadId padId();			// what is remembered, open or not
		void setPadId(const xPadId&);
		static QString getButtonName(int);
		static QString getEntryName(const xJoyMapEntry&);
		static QString getTargetName(const xJoyMapEntry&);
		static QList<xPadDev> devList();
		void update();

		// the player plays from the host keyboard instead of a pad
		bool isKeyboard();
		int keyboard();			// GPK_*
		void setKeyboard(int);
		bool isLive();			// a pad open, or the keyboard
		bool bindable(int);		// a JOY_* worth binding: a hat comes as buttons, a known pad by name
		int slot = 0;			// 0 or 1: which player, for the log

		// what the player's inputs do now
		int mapSize();
		xJoyMapEntry mapItem(int);
		QStringList whatDoes(int, int, int);	// targets of one input, for the gui
		bool drivesKempston();
		bool bindsKey(int);
		QList<xJoyMapEntry> dropHeld();		// let go of everything held, and say what was
		QList<xJoyMapEntry> takeChanges();	// presses (rps) and releases a rebuild owes the machine

		// the table
		int scheme();
		void setScheme(int);
		int turboRate();		// presses a second
		void setTurboRate(int);
		int rowCount();
		bool rowShown(int);		// Fire 2..4 only where the Kempston has them
		void rebuild();			// again: the machine's Kempston changed
		xPadRow row(int);
		void setRow(int, const xPadRow&);	// -1 adds an extra row
		void delRow(int);
		void resetRows();			// the joystick's defaults, no extra rows
		QList<xJoyMapEntry> rowInputs(int);	// what presses it, with the device in use
		QList<xJoyMapEntry> rowTargets(int);	// what it presses
		QList<xJoyMapEntry> defInputs(int, bool keys);
		QList<xJoyMapEntry> aliasInputs(int);	// what "Gamepad up" is on this pad
		QString rowName(int);
		static QString targetsName(const QList<xJoyMapEntry>&);
		static QString inputsName(const QList<xJoyMapEntry>&);

		// config.conf: "<prefix>.<name> = <value>"; a .pad file is the same
		// lines with no prefix
		void saveConf(FILE*, const char*);
		bool loadConf(const std::string&, const std::string&);
		bool saveFile(const std::string&);
		bool loadFile(const std::string&);
		void importMap(std::string);		// a .pad from before the table

		QList<xJoyMapEntry> scanMap(int, int, int);
		QList<xJoyMapEntry> repTick();
	signals:
		// type is JOY_*, num the button/axis/hat number, state its value
		void inputChanged(int, int, int);
	private:
		int id;
		int dead;
		xPadId pid;
		int kbd;				// GPK_*
		bool ctrl;				// bound by controller names, see padIsCtrl()
		int scm;				// GPS_*
		int trate;				// turbo, presses a second
		QList<xPadRow> rows;			// PR_JOY joystick rows, then the extras
		QList<xJoyMapEntry> map;		// what is in effect, see rebuild()
		QList<xJoyMapEntry> changes;		// see takeChanges()
		QMap<int, QMap<int, int> > jState;	// last value handed out, per type and number
		QMap<int, int> hatPrev;			// last hat value scanMap acted on
		SDL_Joystick* sjptr;
#if HAVESDL2
		SDL_GameController* scptr;
#endif
		void emitChanged(int, int, int);
		bool asController();
};

class xGamepadController : public QObject {
	Q_OBJECT
	public:
		xGamepadController(QObject* = nullptr);
		void rescan();
		xGamepad* gpada;
		xGamepad* gpadb;
		QStringList seen;		// guids of the pads met so far, see newPad
	signals:
		void devicesChanged();
		void newPad(int);		// a model never met before took slot 0/1
	protected:
		void timerEvent(QTimerEvent*);
	private:
		void meetNew(const QList<xPadDev>&, const int*);
};

// gamecontrollerdb.txt, if the user dropped one in the config dir
void padLoadControllerDb();

// the key a key event is about, as the keymap counts keys (XKEY_*)
class QKeyEvent;
int pad_key_id(QKeyEvent*);

#pragma once

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
	JOY_CAXIS		// SDL_CONTROLLER_AXIS_*
};

enum {
	JMAP_NONE = 0,
	JMAP_KEY,
	JMAP_JOY,
	JMAP_JOYB,
	JMAP_MOUSE
};

typedef struct {
	int type;		// axis/button
	int num;		// number of axis/button
	int state;		// -x/+x for axis, 0/x for button
	int dev;		// device for action JMAP_*
#if USE_SEQ_BIND
	QKeySequence seq;	// key sequence to activate
#else
	int key;		// key XKEY_* for keyboard
#endif
	int dir;		// XJ_* for kempston
	int rps;		// repeat state (0:released, !0:pressed)
	int rpt;		// repeat period (0 = no repeat)
	int cnt;		// repeat counter
} xJoyMapEntry;

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
		static QList<xPadDev> devList();
		void update();

		int mapSize();
		void mapClear();
		xJoyMapEntry mapItem(int);
		void setItem(int, xJoyMapEntry);
		void delItem(int);

		void loadMap(std::string);
		void saveMap(std::string);
		QList<xJoyMapEntry> scanMap(int, int, int);
		QList<xJoyMapEntry> repTick();
	signals:
		// type is JOY_*, num the button/axis/hat number, state its value
		void inputChanged(int, int, int);
	private:
		int id;
		int dead;
		xPadId pid;
		QList<xJoyMapEntry> map;			// gamepad map for gamepad
		QMap<int, QMap<int, int> > jState;	// last value handed out, per type and number
		QMap<int, int> hatPrev;			// last hat value scanMap acted on
		SDL_Joystick* sjptr;
#if HAVESDL2
		SDL_GameController* scptr;
#endif
		void emitChanged(int, int, int);
};

class xGamepadController : public QObject {
	Q_OBJECT
	public:
		xGamepadController(QObject* = nullptr);
		void rescan();
		xGamepad* gpada;
		xGamepad* gpadb;
	protected:
		void timerEvent(QTimerEvent*);
};

// gamecontrollerdb.txt, if the user dropped one in the config dir
void padLoadControllerDb();

// operations with gamepad map files
int padExists(std::string);
int padCreate(std::string);
void padDelete(std::string);

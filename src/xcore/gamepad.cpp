#include <QDebug>

#if USE_QT_GAMEPAD
#include <QGamepadManager>
#endif

#include <SDL_events.h>
#include <SDL_joystick.h>
#include <SDL_version.h>

#include <stdio.h>

#include "xcore.h"
#include "gamepad.h"

#define VIRTKEYBASE 12

// SDL_JoystickGetSerial arrived in 2.0.14, SDL_JoystickPathForIndex in 2.24.
// Linux is built against the distro's SDL, which is often older than the one
// pinned for Windows, so both are optional and the code falls back a step.
#if HAVESDL2 && SDL_VERSION_ATLEAST(2,0,14)
 #define HAVE_PAD_SERIAL 1
#else
 #define HAVE_PAD_SERIAL 0
#endif
#if HAVESDL2 && SDL_VERSION_ATLEAST(2,24,0)
 #define HAVE_PAD_PATH 1
#else
 #define HAVE_PAD_PATH 0
#endif
#if HAVESDL2 && SDL_VERSION_ATLEAST(2,0,9)
 #define HAVE_PAD_PLAYER 1
#else
 #define HAVE_PAD_PLAYER 0
#endif

typedef struct {
	char ch;
	int val;
} xCharDir;

const xCharDir kjoyChars[] = {
	{'U', XJ_UP},
	{'D', XJ_DOWN},
	{'L', XJ_LEFT},
	{'R', XJ_RIGHT},
	{'F', XJ_FIRE},
	{'2', XJ_BUT2},
	{'3', XJ_BUT3},
	{'4', XJ_BUT4},
	{'A', XJ_FIRE},	// the same four under the letters a pad wears
	{'B', XJ_BUT2},
	{'S', XJ_BUT3},
	{'O', XJ_BUT4},
	{'-', XJ_NONE}
};

const xCharDir kmouChars[] = {
	{'U', XM_UP},
	{'D', XM_DOWN},
	{'L', XM_LEFT},
	{'R', XM_RIGHT},
	{'[', XM_LMB},
	{'|', XM_MMB},
	{']', XM_RMB},
	{'^', XM_WHEELUP},
	{'v', XM_WHEELDN},
	{'-', XM_NONE}
};

const xCharDir hatChars[] = {
	{'U', SDL_HAT_UP},
	{'D', SDL_HAT_DOWN},
	{'L', SDL_HAT_LEFT},
	{'R', SDL_HAT_RIGHT},
	{'-', 0}
};

const xCharDir pabhChars[] = {
	{'A', JOY_AXIS},
	{'B', JOY_BUTTON},
	{'H', JOY_HAT},
	{'C', JOY_CBUTTON},
	{'X', JOY_CAXIS},
	{'-', JOY_NONE}
};

const xCharDir devChars[] = {
	{'K', JMAP_KEY},
	{'J', JMAP_JOY},
	{'B', JMAP_JOYB},
	{'M', JMAP_MOUSE},
	{'-', JMAP_NONE}
};

char padGetChar(int val, const xCharDir* tab) {
	int idx = 0;
	while ((tab[idx].val > 0) && (tab[idx].val != val))
		idx++;
	return tab[idx].ch;
}

int padGetId(char ch, const xCharDir* tab) {
	int idx = 0;
	while ((tab[idx].val > 0) && (tab[idx].ch != ch))
		idx++;
	return tab[idx].val;
}

// The pad half of a map line: A0+ A1- B3 H0U Ca Cdpup Xlefty- Xlefttrigger+.
// A..H are raw SDL numbers, C and X are SDL's controller names. Returns 0 if
// the line says nothing usable.
static int padParseSource(const char* str, xJoyMapEntry* jent) {
	int idx = 1;
	int num = 0;
	char nam[32];
	int len;
	if (!str || !str[0]) return 0;
	jent->type = padGetId(str[0], pabhChars);
	jent->state = 0;
	switch (jent->type) {
		case JOY_CBUTTON:
		case JOY_CAXIS:
			len = 0;
			while (str[idx] && (str[idx] != '+') && (str[idx] != '-') && (len < (int)sizeof(nam) - 1))
				nam[len++] = str[idx++];
			nam[len] = 0;
#if HAVESDL2
			if (jent->type == JOY_CBUTTON) {
				jent->num = SDL_GameControllerGetButtonFromString(nam);
			} else {
				jent->num = SDL_GameControllerGetAxisFromString(nam);
				jent->state = (str[idx] == '-') ? -1 : +1;
			}
#else
			jent->num = -1;
#endif
			if (jent->num < 0) return 0;
			break;
		case JOY_AXIS:		// A0+ A0-
		case JOY_BUTTON:
		case JOY_HAT:
			while ((str[idx] >= '0') && (str[idx] <= '9')) {
				num = num * 10 + str[idx] - '0';
				idx++;
			}
			jent->num = num;
			if (jent->type == JOY_AXIS) {
				jent->state = (str[idx] == '-') ? -1 : +1;
			} else if (jent->type == JOY_HAT) {		// HU HD HR HL
				jent->type = JOY_BUTTON;		// convert hat->button for xGamepad
				switch (str[idx]) {
					case 'U': jent->num = VIRTKEYBASE; break;
					case 'D': jent->num = VIRTKEYBASE+1; break;
					case 'L': jent->num = VIRTKEYBASE+2; break;
					case 'R': jent->num = VIRTKEYBASE+3; break;
					default: jent->type = JOY_HAT; jent->state = padGetId(str[idx], hatChars); break;
				}
			}
			break;
		default:
			return 0;
	}
	return 1;
}

// the same the other way round, for saveMap
static void padWriteSource(FILE* file, const xJoyMapEntry& jent) {
	switch (jent.type) {
#if HAVESDL2
		case JOY_CBUTTON:
			fprintf(file, "C%s", SDL_GameControllerGetStringForButton((SDL_GameControllerButton)jent.num));
			break;
		case JOY_CAXIS:
			fprintf(file, "X%s%c", SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)jent.num),
				(jent.state < 0) ? '-' : '+');
			break;
#endif
		case JOY_AXIS:
			fprintf(file, "A%i%c", jent.num, (jent.state < 0) ? '-' : '+');
			break;
		case JOY_HAT:
			fprintf(file, "H%i%c", jent.num, padGetChar(jent.state, hatChars));
			break;
		default:
			fprintf(file, "%c%i", padGetChar(jent.type, pabhChars), jent.num);
			break;
	}
}

int xGamepad::mapSize() {
	return map.size();
}

void xGamepad::mapClear() {
	map.clear();
}

xJoyMapEntry xGamepad::mapItem(int i) {
	return map[i];
}

void xGamepad::setItem(int i, xJoyMapEntry xjm) {
	if ((i < 0) || (i >= map.size())) {
		map.append(xjm);
	} else {
		map[i] = xjm;
	}
}

void xGamepad::delItem(int i) {
	if (i < 0) return;
	if (i >= map.size()) return;
	map.erase(map.begin() + i);
}

void xGamepad::loadMap(std::string mapname) {
	if (mapname.empty()) return;
	xJoyMapEntry jent;
	FILE* file;
	char buf[1024];
	char* ptr;

	std::string path = conf.path.confDir + SLASH + mapname;
	file = fopen(path.c_str(), "rb");
	if (file) {
		map.clear();
		while(!feof(file)) {
			memset(buf, 0x00, 1024);
			fgets(buf, 1023, file);
			ptr = strtok(buf, ":\n");
			if (ptr && !padParseSource(ptr, &jent)) {
				xlog(XLG_INPUT, XLL_WARN, "map '%s': can't read '%s'", mapname.c_str(), ptr);
			} else if (ptr) {
				ptr = strtok(NULL, ":\n");
				if (ptr) {		// there was 1st :
					jent.dev = padGetId(ptr[0], devChars);
					switch (jent.dev) {
						case JMAP_KEY:		// KUP, KLEFT, KQ, KA
#if USE_SEQ_BIND
							jent.seq = QKeySequence::fromString(QString(&ptr[1]));
							if (jent.seq.isEmpty())
								jent.dev = JMAP_NONE;
#else
							jent.key = getKeyIdByName(&ptr[1]);
							if (jent.key == ENDKEY)
								jent.dev = JMAP_NONE;
#endif
							break;
						case JMAP_JOY:		// JU, JD, JL, JR, JF, J2, J3, J4
						case JMAP_JOYB:
							jent.dir = padGetId(ptr[1], kjoyChars);
							break;
						case JMAP_MOUSE:	// MD, ML, M[ M| M] M^ Mv
							jent.dir = padGetId(ptr[1], kmouChars);
							break;
						default:
							jent.dev = JMAP_NONE;	// ignore it
							break;
					}
					jent.rpt = 0;
					jent.cnt = 0;
					ptr = strtok(NULL, ":\n");
					if (ptr)
						jent.rpt = atoi(ptr);
					if (jent.dev != JMAP_NONE)
						map.push_back(jent);
				}
			}
		}
		fclose(file);
		xlog(XLG_INPUT, XLL_DEBUG, "map '%s': %i bindings", mapname.c_str(), (int)map.size());
	}
}

void xGamepad::saveMap(std::string mapname) {
	if (mapname.empty()) return;
	std::string path = conf.path.confDir + SLASH + mapname;
	FILE* file;
	file = fopen(path.c_str(), "wb");
	if (file) {
		foreach(xJoyMapEntry jent, map) {
			padWriteSource(file, jent);
			fprintf(file, ":%c", padGetChar(jent.dev, devChars));
			switch(jent.dev) {
				case JMAP_KEY:
#if USE_SEQ_BIND
					fprintf(file, "%s", jent.seq.toString().toUtf8().data());
#else
					fprintf(file, "%s", getKeyNameById(jent.key));
#endif
					break;
				case JMAP_JOY:
				case JMAP_JOYB:
					fputc(padGetChar(jent.dir, kjoyChars), file);
					break;
				case JMAP_MOUSE:
					fputc(padGetChar(jent.dir, kmouChars), file);
					break;
				default:
					fprintf(file, "?");
			}
			if (jent.rpt > 0)
				fprintf(file, ":%i", jent.rpt);
			fputc('\n', file);
		}
		fclose(file);
	}
}

int padExists(std::string name) {
	std::string path = conf.path.confDir + SLASH + name;
	FILE* file = fopen(path.c_str(), "rb");
	if (!file) return 0;
	fclose(file);
	return 1;
}

int padCreate(std::string name) {
	if (padExists(name)) return 0;
	std::string path = conf.path.confDir + SLASH + name;
	FILE* file = fopen(path.c_str(), "wb");
	if (!file) return 0;
	fclose(file);
	return 1;
}

void padDelete(std::string name) {
	std::string path = conf.path.confDir + SLASH + name;
	remove(path.c_str());
}

// gamecontrollerdb.txt: SDL carries a big layout database of its own, this is
// for the pads it does not know yet, or knows wrong. Optional - most setups
// never need one.
void padLoadControllerDb() {
#if HAVESDL2
	std::string path = conf.path.confDir + SLASH + "gamecontrollerdb.txt";
	FILE* file = fopen(path.c_str(), "rb");
	if (!file) return;
	fclose(file);
	int n = SDL_GameControllerAddMappingsFromFile(path.c_str());
	if (n < 0) {
		xlog(XLG_INPUT, XLL_WARN, "gamecontrollerdb.txt: %s", SDL_GetError());
	} else {
		xlog(XLG_INPUT, XLL_INFO, "gamecontrollerdb.txt: %i mappings", n);
	}
#endif
}

// xPadId

bool xPadId::isEmpty() const {
	return guid.isEmpty() && name.isEmpty();
}

// Same remembered pad? An old config has a name and no guid, so the name is
// the fallback and only then.
bool xPadId::sameAs(const xPadId& o) const {
	if (guid.isEmpty() || o.guid.isEmpty())
		return !name.isEmpty() && (name == o.name);
	if (guid != o.guid) return false;
	if (dtype != o.dtype) return false;
	switch (dtype) {
		case GPD_SERIAL:
		case GPD_PATH: return disc == o.disc;
		case GPD_ORD: return ord == o.ord;
	}
	return true;
}

// "<guid>|<kind>:<disc>|<name>". The name is last and free-form; '|' cannot
// turn up in a guid, and no platform puts one in a device path.
QString xPadId::toConfig() const {
	if (isEmpty()) return QString();
	QString key;
	switch (dtype) {
		case GPD_SERIAL: key = QString("S:%0").arg(disc); break;
		case GPD_PATH: key = QString("P:%0").arg(disc); break;
		case GPD_ORD: key = QString("N:%0").arg(ord); break;
		default: key = "-"; break;
	}
	return QString("%0|%1|%2").arg(guid).arg(key).arg(name);
}

xPadId xPadId::fromConfig(QString str) {
	xPadId res;
	str = str.trimmed();
	if (str.isEmpty()) return res;
	QStringList part = str.split('|');
	if (part.size() < 3) {		// a config written before pads had a guid
		res.name = str;
		return res;
	}
	res.guid = part.at(0);
	res.name = part.mid(2).join('|');
	QString key = part.at(1);
	if (key.startsWith("S:")) {
		res.dtype = GPD_SERIAL;
		res.disc = key.mid(2);
	} else if (key.startsWith("P:")) {
		res.dtype = GPD_PATH;
		res.disc = key.mid(2);
	} else if (key.startsWith("N:")) {
		res.dtype = GPD_ORD;
		res.ord = key.mid(2).toInt();
	}
	return res;
}

// which key is holding this one apart from its twins, for the log
QString xPadId::keyName() const {
	switch (dtype) {
		case GPD_SERIAL: return QString("serial %0").arg(disc);
		case GPD_PATH: return QString("path %0").arg(disc);
		case GPD_ORD: return QString("order (#%0)").arg(ord);
	}
	return QString("guid alone");
}

QString xPadId::title() const {
	if (name.isEmpty()) return QString();
	if (ord > 1)		// one of several of the same model
		return QString("%0 #%1").arg(name).arg(ord);
	return name;
}

// xGamepad
xGamepad::xGamepad(QObject* p):QObject(p) {
	id = -1;
	dead = 8192;
	sjptr = NULL;
#if HAVESDL2
	scptr = NULL;
#endif
}

xGamepad::~xGamepad() {
	close();
}

// Every connected pad, with what it takes to recognise it again.
QList<xPadDev> xGamepad::devList() {
	QList<xPadDev> res;
	int cnt = SDL_NumJoysticks();
	int i;
	for (i = 0; i < cnt; i++) {
		xPadDev dev;
		dev.index = i;
#if HAVESDL2
		char gstr[64];
		SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), gstr, sizeof(gstr));
		dev.id.guid = QString(gstr);
		const char* nm = SDL_JoystickNameForIndex(i);
		dev.ctrl = SDL_IsGameController(i) ? true : false;
#else
		const char* nm = SDL_JoystickName(i);
#endif
		dev.id.name = (nm && nm[0]) ? QString(nm) : QString("Gamepad %0").arg(i + 1);
#if HAVE_PAD_PLAYER
		dev.player = SDL_JoystickGetDevicePlayerIndex(i);
#endif
		res.append(dev);
	}
	// Every device gets its key, not just the ones with a twin in sight. A
	// pad's identity must not depend on what else is plugged in: when its
	// twin is unplugged, the one left behind still has to answer to the key
	// it was remembered by.
	for (i = 0; i < res.size(); i++) {
		int ord = 0;
		for (int j = 0; j <= i; j++) {
			if (res[j].id.guid == res[i].id.guid) ord++;
		}
		res[i].id.ord = ord;
		res[i].id.dtype = GPD_ORD;
#if HAVE_PAD_SERIAL
		SDL_Joystick* jp = SDL_JoystickOpen(res[i].index);	// refcounted, ours to close
		if (jp) {
			const char* ser = SDL_JoystickGetSerial(jp);
			if (ser && ser[0]) {
				res[i].id.dtype = GPD_SERIAL;
				res[i].id.disc = QString(ser);
			}
			SDL_JoystickClose(jp);
		}
#endif
#if HAVE_PAD_PATH
		if (res[i].id.dtype != GPD_SERIAL) {	// a serial is better: it outlives a change of port
			const char* pth = SDL_JoystickPathForIndex(res[i].index);
			if (pth && pth[0]) {
				res[i].id.dtype = GPD_PATH;
				res[i].id.disc = QString(pth);
			}
		}
#endif
	}
	// The gui has to name them apart, and two pads of one model read the
	// same. Say which player each one is - a 360 pad lights that number on
	// its ring, so it names the pad in your hands - and only fall back to
	// counting them when the host will not say. This is for the eye alone:
	// the player number moves as pads come and go, so nothing is matched by
	// it.
	for (i = 0; i < res.size(); i++) {
		int same = 0;
		int pos = 0;
		for (int j = 0; j < res.size(); j++) {
			if (res[j].id.name != res[i].id.name) continue;
			same++;
			if (j <= i) pos++;
		}
		res[i].label = res[i].id.name;
		if (same < 2) continue;
		if (res[i].player >= 0) {
			res[i].label += QString(" (player %0)").arg(res[i].player + 1);
		} else {
			res[i].label += QString(" #%0").arg(pos);
		}
	}
	return res;
}

void xGamepad::openDev(const xPadDev& dev) {
	close();
	if (dev.index < 0) return;
#if HAVESDL2
	if (dev.ctrl) {
		scptr = SDL_GameControllerOpen(dev.index);
		if (scptr) sjptr = SDL_GameControllerGetJoystick(scptr);
	}
#endif
	if (!sjptr) sjptr = SDL_JoystickOpen(dev.index);
	if (!sjptr) {
		xlog(XLG_INPUT, XLL_WARN, "can't open pad '%s': %s",
			dev.id.name.toUtf8().data(), SDL_GetError());
		return;
	}
	id = SDL_JoystickInstanceID(sjptr);
	pid = dev.id;
	xlog(XLG_INPUT, XLL_INFO, "pad open: %s [%s] %s, told apart by %s",
		dev.label.toUtf8().data(), pid.guid.toUtf8().data(),
		isController() ? "as controller" : "raw", pid.keyName().toUtf8().data());
}

void xGamepad::close() {
	if (id < 0) return;
#if HAVESDL2
	if (scptr) {
		SDL_GameControllerClose(scptr);		// takes the joystick with it
		scptr = NULL;
		sjptr = NULL;
	}
#endif
	if (sjptr) SDL_JoystickClose(sjptr);
	sjptr = NULL;
	id = -1;
	jState.clear();		// a pad that comes back starts from nothing held
	hatPrev.clear();
}

int xGamepad::isOpened() {
	return !(id < 0);
}

int xGamepad::getId() {
	return id;
}

int xGamepad::isController() {
#if HAVESDL2
	return scptr ? 1 : 0;
#else
	return 0;
#endif
}

QString xGamepad::name() {
	return isOpened() ? pid.name : QString();
}

xPadId xGamepad::padId() {
	return pid;
}

void xGamepad::setPadId(const xPadId& np) {
	pid = np;
}

int sign(int v) {
	if (v < 0) return -1;
	if (v > 0) return 1;
	return 0;
}

// update() only reports changes, so this no longer filters them itself; all
// it still needs of the old value is which hat bits moved.
QList<xJoyMapEntry> xGamepad::scanMap(int type, int num, int st) {
	QList<xJoyMapEntry> presslist;
	int state;
	int hst;
	if (type == JOY_HAT) {
		state = st;
		hst = hatPrev[num] ^ st;		// changed only
		hatPrev[num] = st;
	} else {
		state = sign(st);
		hst = 0;
	}
	for (int i = 0; i < map.size(); i++) {
		xJoyMapEntry& xjm = map[i];
		if ((type == xjm.type) && (num == xjm.num)) {
			if ((state == 0) && (type != JOY_HAT)) {
				xjm.cnt = 0;
				xjm.rps = 0;
				presslist.append(xjm);
			} else {
				switch(type) {
					case JOY_AXIS:
					case JOY_CAXIS:
						if (sign(state) == sign(xjm.state)) {
							xjm.state = st;
							xjm.cnt = xjm.rpt;
							xjm.rps = 1;
						} else {
							xjm.cnt = 0;
							xjm.rps = 0;
						}
						presslist.append(xjm);
						break;
					case JOY_HAT:
						if (hst & xjm.state) {			// state changed
							if (state & xjm.state) {	// pressed
								xjm.cnt = xjm.rpt;
								xjm.rps = 1;
							} else {			// released
								xjm.cnt = 0;
								xjm.rps = 0;
							}
							presslist.append(xjm);
						}
						break;
					case JOY_BUTTON:
					case JOY_CBUTTON:
						xjm.cnt = xjm.rpt;
						xjm.rps = 1;
						presslist.append(xjm);
						break;
				}
			}
		}
	}
	return presslist;
}

QList<xJoyMapEntry> xGamepad::repTick() {
	QList<xJoyMapEntry> presslist;
	for (int i = 0; i < map.size(); i++) {
		xJoyMapEntry& xjm = map[i];
		if (xjm.cnt > 0) {
			xjm.cnt--;
			if (xjm.cnt == 0) {
				xjm.cnt = xjm.rpt;
				xjm.rps = !xjm.rps;
				presslist.append(xjm);
			}
		}
	}
	return presslist;
}

// Report a value only when it moved. Keeping that here rather than in
// scanMap means it stays right whether or not anyone is listening.
void xGamepad::emitChanged(int type, int num, int state) {
	if (jState[type][num] == state) return;
	jState[type][num] = state;
	emit inputChanged(type, num, state);
}

// Read the pad and report what moved. A pad SDL knows the layout of is read
// both ways: by controller name, and by raw number for maps written before
// there was a controller layout to name.
void xGamepad::update() {
	int n, state;
	if (id < 0) return;
#if HAVESDL2
	if (scptr) {
		for (n = 0; n < SDL_CONTROLLER_BUTTON_MAX; n++)
			emitChanged(JOY_CBUTTON, n, SDL_GameControllerGetButton(scptr, (SDL_GameControllerButton)n));
		for (n = 0; n < SDL_CONTROLLER_AXIS_MAX; n++) {
			state = SDL_GameControllerGetAxis(scptr, (SDL_GameControllerAxis)n);
			if (abs(state) < dead) state = 0;
			emitChanged(JOY_CAXIS, n, sign(state));
		}
	}
#endif
	int h = SDL_JoystickNumHats(sjptr);
	n = SDL_JoystickNumButtons(sjptr);
	// clamp if HAT is present: skip virtual D-Pad buttons (>=VIRTKEYBASE)
	if (h > 0 && n > VIRTKEYBASE) n = VIRTKEYBASE;
	while (n > 0) {
		n--;
		emitChanged(JOY_BUTTON, n, SDL_JoystickGetButton(sjptr, n));
	}
	n = SDL_JoystickNumAxes(sjptr);
	while (n > 0) {
		n--;
		state = SDL_JoystickGetAxis(sjptr, n);
		if (abs(state) < dead) state = 0;
		emitChanged(JOY_AXIS, n, sign(state));
	}
	while (h > 0) {
		h--;
		state = SDL_JoystickGetHat(sjptr, h);
		emitChanged(JOY_BUTTON, VIRTKEYBASE + h * 4, !!(state & SDL_HAT_UP));
		emitChanged(JOY_BUTTON, VIRTKEYBASE + 1 + h * 4, !!(state & SDL_HAT_DOWN));
		emitChanged(JOY_BUTTON, VIRTKEYBASE + 2 + h * 4, !!(state & SDL_HAT_LEFT));
		emitChanged(JOY_BUTTON, VIRTKEYBASE + 3 + h * 4, !!(state & SDL_HAT_RIGHT));
		emitChanged(JOY_HAT, h, state);
	}
}

// Forget what is held, so the next update() reports the pad from scratch.
// The window uses it on the way back from a pause: a direction held across
// one would otherwise stay dead until it is let go.
void xGamepad::resync() {
	jState.clear();
	hatPrev.clear();
}

// The order the four virtual buttons of a hat are laid out in, from
// VIRTKEYBASE up. Both ways of naming a hat direction read from here.
static const char* hatDirName[4] = {"up", "down", "left", "right"};

static int hatDirIdx(int state) {
	switch (state) {
		case SDL_HAT_UP: return 0;
		case SDL_HAT_DOWN: return 1;
		case SDL_HAT_LEFT: return 2;
		case SDL_HAT_RIGHT: return 3;
	}
	return -1;
}

QString xGamepad::getButtonName(int n) {
	if (n < VIRTKEYBASE)
		return QString("Button %0").arg(n);
	n -= VIRTKEYBASE;
	return QString("Hat %0 %1").arg(n >> 2).arg(hatDirName[n & 3]);
}

// How a binding reads in the gui - the map table and the bind dialog both
// say it this way.
QString xGamepad::getEntryName(const xJoyMapEntry& jent) {
	switch (jent.type) {
#if HAVESDL2
		case JOY_CBUTTON:
			return QString(SDL_GameControllerGetStringForButton((SDL_GameControllerButton)jent.num));
		case JOY_CAXIS:
			return QString("%0 %1").arg(SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)jent.num))
				.arg((jent.state < 0) ? "-" : "+");
#endif
		case JOY_BUTTON:
			return getButtonName(jent.num);
		case JOY_AXIS:
			return QString("Axis %0 %1").arg(jent.num).arg((jent.state < 0) ? "-" : "+");
		case JOY_HAT: {
			int d = hatDirIdx(jent.state);
			return QString("Hat %0 %1").arg(jent.num).arg((d < 0) ? "??" : hatDirName[d]);
		}
	}
	return QString();
}

void xGamepad::setDeadZone(int v) {
	if (v < 0) return;
	if (v > 32768) return;
	dead = v;
}

int xGamepad::deadZone() {return dead;}

// controller

// Pad poll period. The pads are read on the gui thread, so this is also the
// delay a press can sit for before the emulation sees it. Polling at all is
// the stopgap: see the TODO on update().
#define GP_POLL_MS	2

xGamepadController::xGamepadController(QObject* p):QObject(p) {
	gpada = new xGamepad;
	gpadb = new xGamepad;
	startTimer(GP_POLL_MS, Qt::PreciseTimer);
}

// How well a connected device answers to what a slot remembers. 0 is no.
static int padMatch(const xPadId& want, const xPadDev& dev) {
	if (want.isEmpty()) return 0;
	if (want.guid.isEmpty())			// config from before pads had a guid
		return (want.name == dev.id.name) ? 1 : 0;
	if (want.guid != dev.id.guid) return 0;
	if (want.sameAs(dev.id)) return 4;				// this very unit
	if ((want.ord > 0) && (want.ord == dev.id.ord)) return 3;	// same place in the list
	return 1;			// the right model, with no telling which one
}

// Hand out the pads, best match first, whichever slot it belongs to. Taking
// the slots in order instead would let a slot whose own pad has been
// unplugged grab its twin, out from under the slot that twin belongs to.
void xGamepadController::rescan() {
	QList<xPadDev> devs = xGamepad::devList();
	xGamepad* slot[2] = {gpada, gpadb};
	int pick[2] = {-1, -1};
	bool done[2] = {false, false};
	int n;
	for (n = 0; n < 2; n++) {
		int best = 0;
		int bslot = -1;
		int bdev = -1;
		for (int s = 0; s < 2; s++) {
			if (done[s]) continue;
			for (int i = 0; i < devs.size(); i++) {
				if ((i == pick[0]) || (i == pick[1])) continue;
				int sc = padMatch(slot[s]->padId(), devs.at(i));
				if (sc > best) {
					best = sc;
					bslot = s;
					bdev = i;
				}
			}
		}
		if (bslot < 0) break;
		done[bslot] = true;
		if (best == 1) {
			// A weak match knows the model and nothing more. Take it only
			// when there is nothing to choose between - one pad moved to
			// another port is worth finding - but with two of a model left
			// over, picking one is a coin toss that half the time puts them
			// the wrong way round. Better to leave the slot empty and say so.
			int cand = 0;
			for (int i = 0; i < devs.size(); i++) {
				if ((i == pick[0]) || (i == pick[1])) continue;
				if (padMatch(slot[bslot]->padId(), devs.at(i)) == 1) cand++;
			}
			if (cand > 1) continue;
		}
		pick[bslot] = bdev;
	}
	for (n = 0; n < 2; n++) {
		if (pick[n] < 0) {
			// Say so when pads are connected and none of them is the one
			// this slot wants. Switching SDL's joystick driver changes both
			// the guid and the path, so a slot can go quiet with the very
			// same pad plugged in, and nothing else would explain it.
			if (!slot[n]->padId().isEmpty() && !devs.isEmpty())
				xlog(XLG_INPUT, XLL_INFO, "pad %c: no sure match for '%s' among the %i connected",
					n ? 'B' : 'A', slot[n]->padId().name.toUtf8().data(), (int)devs.size());
			slot[n]->close();		// keeps what it remembers
		} else if (!slot[n]->isOpened() || !slot[n]->padId().sameAs(devs.at(pick[n]).id)) {
			// openDev takes the device's own key, which may be sharper than
			// what was remembered - a twin turning up tells them apart
			slot[n]->openDev(devs.at(pick[n]));
		}
	}
}

void xGamepadController::timerEvent(QTimerEvent* e) {
#ifdef HAVESDL2
	// Pump the events first: SDL fills the pad state from the pump, so reading
	// it before would hand out the values of the previous tick.
	SDL_Event ev;
	bool changed = false;
	while (SDL_PollEvent(&ev)) {
		switch (ev.type) {
			case SDL_JOYDEVICEREMOVED:
				if ((ev.jdevice.which == gpada->getId()) && gpada->isOpened()) gpada->close();
				if ((ev.jdevice.which == gpadb->getId()) && gpadb->isOpened()) gpadb->close();
				changed = true;
				break;
			case SDL_JOYDEVICEADDED:
				changed = true;
				break;
		}
	}
	// A pad going or coming shuffles SDL's device indices, so both slots are
	// worked out again from what is there now.
	if (changed) rescan();
#endif
	gpada->update();
	gpadb->update();
}

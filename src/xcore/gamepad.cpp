#include <QDebug>
#include <QKeyEvent>
#include <QFileInfo>
#include <QUrl>

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

// the four directions as files and the gui spell them, in PR_* order
static const char* dirName[4] = {"up", "down", "left", "right"};

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
	{'G', JOY_VDIR},
	{'-', JOY_NONE}
};

const xCharDir devChars[] = {
	{'K', JMAP_KEY},
	{'J', JMAP_JOY},
	{'B', JMAP_JOYB},
	{'M', JMAP_MOUSE},
	{'A', JMAP_CUT},
	{'Z', JMAP_ZX},
	{'-', JMAP_NONE}
};

int sign(int v) {
	if (v < 0) return -1;
	if (v > 0) return 1;
	return 0;
}

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
		case JOY_VDIR: {		// Gup Gdown Gleft Gright
			jent->num = -1;
			for (int i = 0; i < 4; i++) {
				if (!strcmp(str + 1, dirName[i])) jent->num = i;
			}
			if (jent->num < 0) return 0;
			break;
		}
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

// The pad half the other way round, as config.conf keeps it
static QString padSourceStr(const xJoyMapEntry& jent) {
	switch (jent.type) {
#if HAVESDL2
		case JOY_CBUTTON:
			return QString("C%0").arg(SDL_GameControllerGetStringForButton((SDL_GameControllerButton)jent.num));
		case JOY_CAXIS:
			return QString("X%0%1").arg(SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)jent.num))
				.arg(QChar((jent.state < 0) ? '-' : '+'));
#endif
		case JOY_AXIS:
			return QString("A%0%1").arg(jent.num).arg(QChar((jent.state < 0) ? '-' : '+'));
		case JOY_HAT:
			return QString("H%0%1").arg(jent.num).arg(QChar(padGetChar(jent.state, hatChars)));
		case JOY_VDIR:
			return QString("G%0").arg(dirName[jent.num & 3]);
	}
	return QString("%0%1").arg(QChar(padGetChar(jent.type, pabhChars))).arg(jent.num);
}

// What an entry presses: Z<Spectrum key> J<dir> B<dir> M<dir> A<action>, or
// K<PC key sequence> as an old .pad has it. The argument is percent-encoded
// in config.conf, where ';' and ' ' part the fields; an old .pad has it bare.
static int padParseTarget(const char* str, xJoyMapEntry* jent, bool enc) {
	jent->dev = str[0] ? padGetId(str[0], devChars) : JMAP_NONE;
	QString arg = str[0] ? QString::fromUtf8(str + 1) : QString();
	if (enc) arg = QUrl::fromPercentEncoding(arg.toUtf8());
	switch (jent->dev) {
		case JMAP_KEY:		// KUP, KLEFT, KQ, KA
			jent->seq = QKeySequence::fromString(arg);
			if (jent->seq.isEmpty())
				jent->dev = JMAP_NONE;
			break;
		case JMAP_ZX:		// Zq, Z1, ZC (Caps Shift), ZS (Symbol Shift), ZE (Enter), Z%20
			if ((arg.size() == 1) && strchr(pad_zx_keys(), arg.at(0).toLatin1())) {
				jent->dir = arg.at(0).toLatin1();
			} else {
				jent->dev = JMAP_NONE;
			}
			break;
		case JMAP_JOY:		// JU, JD, JL, JR, JF, J2, J3, J4
		case JMAP_JOYB:
			jent->dir = padGetId(str[1], kjoyChars);
			break;
		case JMAP_MOUSE:	// MD, ML, M[ M| M] M^ Mv
			jent->dir = padGetId(str[1], kmouChars);
			break;
		case JMAP_CUT: {	// Akey.rewind
			xShortcut* cut = find_shortcut_name(arg.toUtf8().data());
			if (cut) {
				jent->dir = cut->id;
			} else {
				jent->dev = JMAP_NONE;
			}
			break;
		}
		default:
			jent->dev = JMAP_NONE;	// ignore it
			break;
	}
	return (jent->dev != JMAP_NONE) ? 1 : 0;
}

static QString padTargetStr(const xJoyMapEntry& jent) {
	QString arg;
	xShortcut* cut;
	switch (jent.dev) {
		case JMAP_KEY:
			arg = jent.seq.toString();
			break;
		case JMAP_ZX:
			arg = QChar(jent.dir);
			break;
		case JMAP_JOY:
		case JMAP_JOYB:
			arg = QChar(padGetChar(jent.dir, kjoyChars));
			break;
		case JMAP_MOUSE:
			arg = QChar(padGetChar(jent.dir, kmouChars));
			break;
		case JMAP_CUT:
			cut = find_shortcut_id(jent.dir);
			if (!cut) return QString();
			arg = QString(cut->name);
			break;
		default:
			return QString();
	}
	return QChar(padGetChar(jent.dev, devChars)) + QString::fromLatin1(QUrl::toPercentEncoding(arg, "+"));
}

// One line of an old .pad file, "<pad>:<target>[:<repeat>]". Returns 0 for a
// line that binds nothing.
static int padParseLine(char* buf, xJoyMapEntry* jent, const char* mapname) {
	char* ptr = strtok(buf, ":\r\n");
	if (!ptr) return 0;
	if (!padParseSource(ptr, jent)) {
		xlog(XLG_INPUT, XLL_WARN, "map '%s': can't read '%s'", mapname, ptr);
		return 0;
	}
	ptr = strtok(NULL, ":\r\n");
	if (!ptr) return 0;
	if (!padParseTarget(ptr, jent, false)) return 0;
	jent->rpt = 0;
	jent->rps = 0;
	jent->cnt = 0;
	ptr = strtok(NULL, ":\r\n");
	if (ptr)
		jent->rpt = atoi(ptr);
	return 1;
}

static xJoyMapEntry padTgt(const char* str) {
	xJoyMapEntry e;
	padParseTarget(str, &e, false);
	return e;
}

static xJoyMapEntry padSrc(const char* str) {
	xJoyMapEntry e;
	padParseSource(str, &e);
	return e;
}

static xJoyMapEntry padAlias(int r) {
	xJoyMapEntry e;
	e.type = JOY_VDIR;
	e.num = r;
	return e;
}

// The Spectrum key an old .pad's PC key means, as the stock layout has it:
// a digit, a letter, Space, Enter. 0 when it is not one of those.
static int padZxOfSeq(const QKeySequence& seq) {
	if (seq.count() != 1) return 0;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	int c = seq[0].toCombined();
#else
	int c = seq[0];
#endif
	if (c & Qt::KeyboardModifierMask) return 0;
	int k = c & ~Qt::KeyboardModifierMask;
	if ((k >= Qt::Key_0) && (k <= Qt::Key_9)) return '0' + (k - Qt::Key_0);
	if ((k >= Qt::Key_A) && (k <= Qt::Key_Z)) return 'a' + (k - Qt::Key_A);
	if (k == Qt::Key_Space) return ' ';
	if ((k == Qt::Key_Return) || (k == Qt::Key_Enter)) return 'E';
	return 0;
}

static bool padSameTarget(const xJoyMapEntry& a, const xJoyMapEntry& b) {
	int za = (a.dev == JMAP_ZX) ? a.dir : (a.dev == JMAP_KEY) ? padZxOfSeq(a.seq) : 0;
	int zb = (b.dev == JMAP_ZX) ? b.dir : (b.dev == JMAP_KEY) ? padZxOfSeq(b.seq) : 0;
	if (za || zb) return za == zb;
	if (a.dev != b.dev) return false;
	return (a.dev == JMAP_KEY) ? (a.seq == b.seq) : (a.dir == b.dir);
}

// Schemes

static const struct {
	const char* key;
	const char* name;
	const char* role[PR_JOY];	// what each joystick row presses
} schTab[GPS_COUNT] = {
	{"kempston", "Kempston", {"JU", "JD", "JL", "JR", "JF", "J2", "J3", "J4"}},
	{"sinclair1", "Sinclair / Interface II (6-0)", {"Z9", "Z8", "Z6", "Z7", "Z0"}},
	{"sinclair2", "Sinclair port 2 (left) (1-5)", {"Z4", "Z3", "Z1", "Z2", "Z5"}},
	{"cursor", "Cursor/Protek/AGF", {"Z7", "Z6", "Z5", "Z8", "Z0"}},
	{"qaop", "QAOP + Space", {"Zq", "Za", "Zo", "Zp", "Z "}},
	{"qaopm", "QAOP + M", {"Zq", "Za", "Zo", "Zp", "Zm"}},
	{"custom", "Custom", {NULL}}
};

// the keys Custom starts from, until the user picks others
static const char* customDef[PR_FIRE + 1] = {"Zq", "Za", "Zo", "Zp", "Z "};

static const char* roleName[PR_JOY] = {"Up", "Down", "Left", "Right", "Fire", "Fire 2", "Fire 3", "Fire 4"};
static const char* roleKey[PR_JOY] = {"up", "down", "left", "right", "fire", "fire2", "fire3", "fire4"};

// "Gamepad up" and the rest: the d-pad and the left stick. A pad SDL knows
// the layout of is bound by name, any other by number, as its first hat and
// axes.
static const char* aliasCtrl[4][2] = {
	{"Cdpup", "Xlefty-"}, {"Cdpdown", "Xlefty+"}, {"Cdpleft", "Xleftx-"}, {"Cdpright", "Xleftx+"}
};
static const char* aliasRaw[4][2] = {
	{"H0U", "A1-"}, {"H0D", "A1+"}, {"H0L", "A0-"}, {"H0R", "A0+"}
};
// The four face buttons. Fire takes the ones no other row has, and Kempston 8
// gives the last three to Fire 2..4.
static const char* faceCtrl[4] = {"Ca", "Cb", "Cx", "Cy"};
static const char* faceRaw[4] = {"B0", "B1", "B2", "B3"};
// and on the keyboard, the two layouts it comes in: the arrows and Ctrl, or WASD and Space
static const char* defKeys[2][PR_FIRE + 1][2] = {
	{{"UP", NULL}, {"DOWN", NULL}, {"LEFT", NULL}, {"RIGHT", NULL}, {"LC", "RC"}},
	{{"W", NULL}, {"S", NULL}, {"A", NULL}, {"D", NULL}, {"SPC", NULL}}
};

const char* pad_scheme_key(int s) {
	return ((s >= 0) && (s < GPS_COUNT)) ? schTab[s].key : schTab[GPS_KEMPSTON].key;
}

const char* pad_scheme_name(int s) {
	return ((s >= 0) && (s < GPS_COUNT)) ? schTab[s].name : schTab[GPS_KEMPSTON].name;
}

static int padSchemeFind(const std::string& key) {
	for (int i = 0; i < GPS_COUNT; i++) {
		if (key == schTab[i].key) return i;
	}
	return -1;
}

static const char* kbdKey[3] = {"", "keyboard", "keyboard.wasd"};
static const char* kbdName[3] = {"None", "Keyboard: arrows + Ctrl", "Keyboard: WASD + Space"};

const char* pad_kbd_key(int k) {
	return ((k >= 0) && (k < 3)) ? kbdKey[k] : kbdKey[GPK_NONE];
}

int pad_kbd_find(const QString& key) {
	for (int k = GPK_ARROWS; k < 3; k++) {
		if (key == kbdKey[k]) return k;
	}
	return GPK_NONE;
}

const char* pad_kbd_name(int k) {
	return ((k >= 0) && (k < 3)) ? kbdName[k] : kbdName[GPK_NONE];
}

const char* pad_role_name(int r) {
	return ((r >= 0) && (r < PR_JOY)) ? roleName[r] : "";
}

// in the keymap's zxKey terms: digits, small letters, Enter, Caps, Symbol, Space
const char* pad_zx_keys() {
	return "1234567890qwertyuiopasdfghjklEC" "zxcvbnmS ";
}

QString pad_zx_name(int c) {
	switch (c) {
		case 'E': return QString("Enter");
		case 'C': return QString("Caps Shift");
		case 'S': return QString("Symbol Shift");
		case ' ': return QString("Space");
	}
	return QString(QChar(c).toUpper());
}

// the names the keymap gives keys are short; these are the ones worth spelling out
static const struct {
	const char* map;
	const char* gui;
} keyNameTab[] = {
	{"UP", "Up arrow"}, {"DOWN", "Down arrow"}, {"LEFT", "Left arrow"}, {"RIGHT", "Right arrow"},
	{"LC", "Left Ctrl"}, {"RC", "Right Ctrl"}, {"LS", "Left Shift"}, {"RS", "Right Shift"},
	{"LA", "Left Alt"}, {"RA", "Right Alt"}, {"SPC", "Space"}, {"ENT", "Enter"}, {"BSP", "Backspace"},
	{"CAPS", "Caps Lock"}, {"TAB", "Tab"}, {"ESC", "Esc"}, {"PGUP", "Page Up"}, {"PGDN", "Page Down"},
	{"DEL", "Delete"}, {"INS", "Insert"}, {"HOME", "Home"}, {"END", "End"}, {"NENT", "Num Enter"},
	{"NLOCK", "Num Lock"}, {"NSLASH", "Num /"}, {"NMUL", "Num *"}, {"NMINUS", "Num -"}, {"NPLUS", "Num +"},
	{"NDOT", "Num ."}, {NULL, NULL}
};

QString pad_key_name(int id) {
	const char* nm = getKeyNameById(id);
	if (!nm || !nm[0]) return QString("Key #%0").arg(id);
	for (int i = 0; keyNameTab[i].map; i++) {
		if (!strcmp(nm, keyNameTab[i].map)) return QString(keyNameTab[i].gui);
	}
	if ((nm[0] == 'N') && (nm[1] >= '0') && (nm[1] <= '9') && !nm[2])
		return QString("Num %0").arg(QChar(nm[1]));
	return QString(nm);
}

int pad_key_id(QKeyEvent* ev) {
#if defined(__linux) || defined(__BSD) || defined(__WIN32)
	return ev->nativeScanCode();		// the keymap counts keys by scan code here
#else
	return qKey2id(ev->key(), ev->modifiers());		// as the layout takes it
#endif
}

// The table

void xGamepad::resetRows() {
	rows.clear();
	for (int i = 0; i < PR_JOY; i++)
		rows.append(xPadRow());
	rebuild();
}

int xGamepad::rowCount() {return rows.size();}

// The extra fire buttons follow the machine: an 8-button Kempston has them.
bool xGamepad::rowShown(int i) {
	if ((i < PR_FIRE2) || (i >= PR_JOY)) return true;
	if (scm != GPS_KEMPSTON) return false;
	return conf.zx && (conf.zx->joy->type == XJ_KEMPSTON) && conf.zx->joy->extbuttons;
}

xPadRow xGamepad::row(int i) {
	return ((i >= 0) && (i < rows.size())) ? rows[i] : xPadRow();
}

void xGamepad::setRow(int i, const xPadRow& r) {
	if ((i < 0) || (i >= rows.size())) {
		rows.append(r);
	} else {
		rows[i] = r;
	}
	rebuild();
}

// an extra row goes; a joystick row goes back to its defaults
void xGamepad::delRow(int i) {
	if ((i < 0) || (i >= rows.size())) return;
	if (i < PR_JOY) {
		QList<xJoyMapEntry> tgt = rows[i].tgt;
		rows[i] = xPadRow();
		rows[i].tgt = tgt;
	} else {
		rows.removeAt(i);
	}
	rebuild();
}

int xGamepad::scheme() {return scm;}

void xGamepad::setScheme(int s) {
	if ((s < 0) || (s >= GPS_COUNT)) s = GPS_KEMPSTON;
	scm = s;
	rebuild();
}

int xGamepad::turboRate() {return trate;}

// Only the rate of what repeats moves, so a fire held down keeps going.
void xGamepad::setTurboRate(int r) {
	trate = qBound(1, r, 25);
	for (int i = 0; i < map.size(); i++) {
		if (map[i].rpt > 0) map[i].rpt = trate;
	}
}

// an old .pad's repeat period, in 20 ms ticks, as a rate
static int padRateOfRpt(int rpt) {
	return qBound(1, qRound(25.0 / qMax(1, rpt)), 25);
}

bool xGamepad::isKeyboard() {return kbd != GPK_NONE;}

int xGamepad::keyboard() {return kbd;}

void xGamepad::setKeyboard(int k) {
	if (k != GPK_NONE) {
		close();
		pid = xPadId();
		ctrl = false;
	}
	kbd = k;
	rebuild();
}

bool xGamepad::isLive() {
	return isKeyboard() || isOpened();
}

bool xGamepad::inUse() {
	return isKeyboard() || !pid.isEmpty();
}

// A pad not plugged in yet is shown by the names its layout would give, if SDL
// knows the model - the table should not change its words when it turns up.
// Worked out where the pad changes and kept: the table asks it all the time.
static bool padIsCtrl(const xPadId& pid) {
#if HAVESDL2
	if (pid.guid.isEmpty()) return false;
	char* mp = SDL_GameControllerMappingForGUID(SDL_JoystickGetGUIDFromString(pid.guid.toUtf8().data()));
	if (!mp) return false;
	SDL_free(mp);
	return true;
#else
	return false;
#endif
}

bool xGamepad::asController() {
	return ctrl;
}

bool xGamepad::bindable(int type) {
	if (type == JOY_HAT) return false;
	return !(isController() && ((type == JOY_BUTTON) || (type == JOY_AXIS)));
}

QList<xJoyMapEntry> xGamepad::aliasInputs(int r) {
	QList<xJoyMapEntry> res;
	if ((r < 0) || (r > 3)) return res;
	for (int k = 0; k < 2; k++)
		res.append(padSrc(asController() ? aliasCtrl[r][k] : aliasRaw[r][k]));
	return res;
}

// What presses a joystick row out of the box; liveInputs() takes from it what another row has.
QList<xJoyMapEntry> xGamepad::defInputs(int i, bool keys) {
	QList<xJoyMapEntry> res;
	if ((i < 0) || (i >= PR_JOY)) return res;
	if (keys) {
		if (i > PR_FIRE) return res;
		for (int k = 0; k < 2; k++) {
			const char* nm = defKeys[(kbd == GPK_WASD) ? 1 : 0][i][k];
			if (!nm) continue;
			xJoyMapEntry e;
			e.type = JOY_KEY;
			e.num = getKeyIdByName(nm);
			if (e.num != ENDKEY) res.append(e);
		}
		return res;
	}
	const char** face = asController() ? faceCtrl : faceRaw;
	if (i < PR_FIRE) {
		res.append(padAlias(i));
	} else if (i > PR_FIRE) {
		res.append(padSrc(face[i - PR_FIRE]));
	} else {
		// every face button, but those Fire 2..4 have on their own defaults
		for (int k = 0; k < 4; k++) {
			if ((k > 0) && rowShown(PR_FIRE + k) && rowOnDefaults(PR_FIRE + k)) continue;
			res.append(padSrc(face[k]));
		}
	}
	return res;
}

bool xGamepad::rowOnDefaults(int i) {
	if ((i < 0) || (i >= PR_JOY)) return false;		// an extra row has no defaults
	return isKeyboard() ? rows[i].keyDef : rows[i].padDef;
}

QList<xJoyMapEntry> xGamepad::flatInputs(const QList<xJoyMapEntry>& lst) {
	QList<xJoyMapEntry> res;
	foreach(const xJoyMapEntry& e, lst) {
		if (e.type == JOY_VDIR) res.append(aliasInputs(e.num)); else res.append(e);
	}
	return res;
}

QList<xJoyMapEntry> xGamepad::rowInputs(int i) {
	if ((i < 0) || (i >= rows.size())) return QList<xJoyMapEntry>();
	if (rowOnDefaults(i)) return defInputs(i, isKeyboard());
	return isKeyboard() ? rows[i].key : rows[i].pad;
}

// one input: the same button, or the same way along an axis or a hat
static bool padSameInput(const xJoyMapEntry& a, const xJoyMapEntry& b) {
	if ((a.type != b.type) || (a.num != b.num)) return false;
	return (a.type == JOY_HAT) ? (a.state == b.state) : (sign(a.state) == sign(b.state));
}

static bool padHasInput(const QList<xJoyMapEntry>& lst, const xJoyMapEntry& e) {
	foreach(const xJoyMapEntry& x, lst) {
		if (padSameInput(x, e)) return true;
	}
	return false;
}

// A row's inputs as they work: a row on its defaults gives up whatever another
// row binds by hand, the members of an alias included - so Left stick up bound
// on its own row stops pressing Up as well. An alias untouched stays itself.
QList<xJoyMapEntry> xGamepad::liveInputs(int i) {
	QList<xJoyMapEntry> ins = rowInputs(i);
	if (!rowOnDefaults(i)) return ins;
	QList<xJoyMapEntry> fixed;
	for (int r = 0; r < rows.size(); r++) {
		if ((r != i) && rowShown(r) && !rowOnDefaults(r)) fixed.append(flatInputs(rowInputs(r)));
	}
	QList<xJoyMapEntry> res;
	foreach(const xJoyMapEntry& e, ins) {
		if (e.type != JOY_VDIR) {
			if (!padHasInput(fixed, e)) res.append(e);
			continue;
		}
		QList<xJoyMapEntry> al = aliasInputs(e.num);
		QList<xJoyMapEntry> keep;
		foreach(const xJoyMapEntry& a, al) {
			if (!padHasInput(fixed, a)) keep.append(a);
		}
		if (keep.size() == al.size()) res.append(e); else res.append(keep);
	}
	return res;
}

// What can be picked from a list, the aliases first: by name for a pad SDL
// knows the layout of, by number for any other.
QList<xJoyMapEntry> xGamepad::inputChoices(bool ctrl) {
	static const char* ctrlMore[] = {"Cleftshoulder", "Crightshoulder", "Xlefttrigger+", "Xrighttrigger+",
		"Xrighty-", "Xrighty+", "Xrightx-", "Xrightx+", "Cleftstick", "Crightstick", "Cback", "Cstart", NULL};
	static const char* rawMore[] = {"B4", "B5", "B6", "B7", "B8", "B9", "B10", "B11",
		"A3-", "A3+", "A2-", "A2+", NULL};
	QList<xJoyMapEntry> res;
	for (int r = 0; r < 4; r++) res.append(padAlias(r));
	for (int k = 0; k < 4; k++) res.append(padSrc(ctrl ? faceCtrl[k] : faceRaw[k]));
	for (int k = 0; k < 2; k++) {		// the d-pad, then the left stick
		for (int r = 0; r < 4; r++) res.append(padSrc(ctrl ? aliasCtrl[r][k] : aliasRaw[r][k]));
	}
	for (const char** s = ctrl ? ctrlMore : rawMore; *s; s++) {
		xJoyMapEntry e = padSrc(*s);
		if (e.type != JOY_NONE) res.append(e);
	}
	return res;
}

QList<xJoyMapEntry> xGamepad::rowTargets(int i) {
	QList<xJoyMapEntry> res;
	if ((i < 0) || (i >= rows.size())) return res;
	if (i >= PR_JOY) return rows[i].tgt;
	if (scm == GPS_CUSTOM) {
		res = rows[i].tgt;
		if (res.isEmpty() && (i <= PR_FIRE)) res.append(padTgt(customDef[i]));
		return res;
	}
	if (schTab[scm].role[i]) res.append(padTgt(schTab[scm].role[i]));
	return res;
}

// a Spectrum key the way a row names it: bare
static QString padTargetShort(const xJoyMapEntry& e) {
	if (e.dev == JMAP_KEY) return e.seq.toString(QKeySequence::NativeText);
	return xGamepad::getTargetName(e);
}

QString xGamepad::targetsName(const QList<xJoyMapEntry>& lst) {
	QStringList res;
	foreach(const xJoyMapEntry& e, lst) res.append(padTargetShort(e));
	return res.join(" + ");
}

QString xGamepad::inputsName(const QList<xJoyMapEntry>& lst) {
	QStringList res;
	foreach(const xJoyMapEntry& e, lst) res.append(getEntryName(e));
	return res.join(", ");
}

// "Up" on the Kempston, "Up (Q)" where it is a key; an extra row by what it presses
QString xGamepad::rowName(int i) {
	if ((i < 0) || (i >= rows.size())) return QString();
	if (i >= PR_JOY) return targetsName(rows[i].tgt);
	if (scm == GPS_KEMPSTON) return QString(roleName[i]);
	return QString("%0 (%1)").arg(roleName[i], targetsName(rowTargets(i)));
}

// The same binding: the same way on the same input, pressing the same thing.
static bool padSameBinding(const xJoyMapEntry& a, const xJoyMapEntry& b) {
	return padSameInput(a, b) && padSameTarget(a, b);
}

// Every input of every shown row, times every target of it.
void xGamepad::rebuild() {
	QList<xJoyMapEntry> old;
	old.swap(map);
	for (int i = 0; i < rows.size(); i++) {
		if (!rowShown(i)) continue;
		QList<xJoyMapEntry> tgt = rowTargets(i);
		foreach(const xJoyMapEntry& in, flatInputs(liveInputs(i))) {
			foreach(xJoyMapEntry e, tgt) {
				e.type = in.type;
				e.num = in.num;
				e.state = in.state;
				e.rpt = rows[i].rapid ? trate : 0;
				e.rps = 0;
				e.cnt = 0;
				map.append(e);
			}
		}
	}
	// A binding held through the change stays held, mid-repeat too. One that
	// went while down, or that lost its turbo in the off phase, goes into
	// changes for the machine to be told (takeChanges()), or its key sticks.
	foreach(const xJoyMapEntry& o, old) {
		if (!o.held) continue;
		int k = 0;
		while ((k < map.size()) && !padSameBinding(o, map[k])) k++;
		if (k >= map.size()) {
			if (o.rps) {
				changes.append(o);
				changes.last().rps = 0;
			}
			continue;
		}
		xJoyMapEntry& e = map[k];
		e.state = o.state;
		e.held = 1;
		if (e.rpt > 0) {
			e.rps = o.rps;
			e.cnt = o.cnt;
		} else {
			e.rps = 1;
			e.cnt = 0;
			if (!o.rps) changes.append(e);
		}
	}
}

QList<xJoyMapEntry> xGamepad::takeChanges() {
	QList<xJoyMapEntry> res;
	res.swap(changes);
	return res;
}

int xGamepad::mapSize() {
	return map.size();
}

xJoyMapEntry xGamepad::mapItem(int i) {
	return map[i];
}

// What one input does now, for the gui: a press only.
QStringList xGamepad::whatDoes(int type, int num, int state) {
	QStringList res;
	if (state == 0) return res;
	foreach(const xJoyMapEntry& e, map) {
		if ((e.type != type) || (e.num != num)) continue;
		if (((type == JOY_AXIS) || (type == JOY_CAXIS)) && (sign(e.state) != sign(state))) continue;
		if ((type == JOY_HAT) && !(e.state & state)) continue;
		res.append(getTargetName(e));
	}
	return res;
}

// "Left stick up + Left stick right: Kempston up, Kempston right"; empty when
// nothing bound is held. A stick pushed on a slant is two inputs at once.
QString xGamepad::heldText() {
	QStringList ins;
	QStringList outs;
	foreach(const xJoyMapEntry& e, map) {
		if (!e.held) continue;
		QString in = getEntryName(e);
		QString out = getTargetName(e);
		if (!ins.contains(in)) ins.append(in);
		if (!outs.contains(out)) outs.append(out);
	}
	if (ins.isEmpty()) return QString();
	return QString("%0: %1").arg(ins.join(" + "), outs.join(", "));
}

bool xGamepad::drivesKempston() {
	if (!isLive()) return false;
	foreach(const xJoyMapEntry& e, map) {
		if (e.dev == JMAP_JOY) return true;
	}
	return false;
}

bool xGamepad::bindsKey(int id) {
	if (!isKeyboard()) return false;
	foreach(const xJoyMapEntry& e, map) {
		if ((e.type == JOY_KEY) && (e.num == id)) return true;
	}
	return false;
}

QList<xJoyMapEntry> xGamepad::dropHeld() {
	QList<xJoyMapEntry> res;
	for (int i = 0; i < map.size(); i++) {
		if (map[i].rps) res.append(map[i]);
		map[i].rps = 0;
		map[i].held = 0;
		map[i].cnt = 0;
	}
	return res;
}

// config.conf and .pad files

static QString padInputsStr(const QList<xJoyMapEntry>& lst, bool def) {
	if (def) return QString("*");
	QStringList res;
	foreach(const xJoyMapEntry& e, lst) {
		if (e.type == JOY_KEY) {
			res.append(QString::fromLatin1(QUrl::toPercentEncoding(QString(getKeyNameById(e.num)))));
		} else {
			res.append(padSourceStr(e));
		}
	}
	return res.join(' ');
}

static QList<xJoyMapEntry> padParseInputs(const QString& str, bool keys, bool* def) {
	QList<xJoyMapEntry> res;
	QString f = str.trimmed();
	*def = (f == "*");
	if (*def) return res;
	foreach(QString tok, f.split(' ', X_SkipEmptyParts)) {
		xJoyMapEntry e;
		if (keys) {
			e.type = JOY_KEY;
			e.num = getKeyIdByName(QUrl::fromPercentEncoding(tok.toUtf8()).toUtf8().data());
			if (e.num != ENDKEY) res.append(e);
		} else if (padParseSource(tok.toUtf8().data(), &e)) {
			res.append(e);
		}
	}
	return res;
}

static QString padTargetsStr(const QList<xJoyMapEntry>& lst) {
	QStringList res;
	foreach(const xJoyMapEntry& e, lst) res.append(padTargetStr(e));
	return res.join(' ');
}

static QList<xJoyMapEntry> padParseTargets(const QString& str) {
	QList<xJoyMapEntry> res;
	foreach(QString tok, str.trimmed().split(' ', X_SkipEmptyParts)) {
		xJoyMapEntry e;
		if (padParseTarget(tok.toUtf8().data(), &e, true)) res.append(e);
	}
	return res;
}

// "joystick = qaop", "turbo = 10", a line per joystick row off its defaults
// ("fire = <pad>;<keys>;<turbo>", * for the defaults), the keys of Custom,
// and a line per extra row ("extra = <targets>;<pad>;<keys>;<turbo>"). In
// config.conf each name has the player's prefix.
void xGamepad::saveConf(FILE* file, const char* pfx) {
	QString p = (pfx && pfx[0]) ? QString("%0.").arg(pfx) : QString();
	QByteArray px = p.toUtf8();
	fprintf(file, "%sjoystick = %s\n", px.data(), pad_scheme_key(scm));
	fprintf(file, "%sturbo = %i\n", px.data(), trate);
	QStringList keys;
	bool own = false;
	for (int i = 0; i < PR_JOY; i++) {
		const xPadRow& r = rows[i];
		if (i <= PR_FIRE) keys.append(padTargetsStr(r.tgt));
		if (!r.tgt.isEmpty()) own = true;
		if (r.padDef && r.keyDef && !r.rapid) continue;
		fprintf(file, "%s%s = %s;%s;%i\n", px.data(), roleKey[i], padInputsStr(r.pad, r.padDef).toUtf8().data(),
			padInputsStr(r.key, r.keyDef).toUtf8().data(), r.rapid ? 1 : 0);
	}
	if (own) fprintf(file, "%scustom = %s\n", px.data(), keys.join(';').toUtf8().data());
	for (int i = PR_JOY; i < rows.size(); i++) {
		const xPadRow& r = rows[i];
		fprintf(file, "%sextra = %s;%s;%s;%i\n", px.data(), padTargetsStr(r.tgt).toUtf8().data(),
			padInputsStr(r.pad, false).toUtf8().data(), padInputsStr(r.key, false).toUtf8().data(), r.rapid ? 1 : 0);
	}
}

bool xGamepad::loadConf(const std::string& name, const std::string& val) {
	QStringList fld = QString::fromUtf8(val.c_str()).split(';');
	while (fld.size() < 4) fld.append(QString());
	if (name == "joystick") {
		int s = padSchemeFind(val);
		scm = (s < 0) ? GPS_KEMPSTON : s;
	} else if (name == "turbo") {
		trate = qBound(1, atoi(val.c_str()), 25);
	} else if (name == "custom") {
		for (int i = 0; i <= PR_FIRE; i++)
			rows[i].tgt = padParseTargets(fld.at(i));
	} else if (name == "extra") {
		xPadRow r;
		bool def;
		r.tgt = padParseTargets(fld.at(0));
		if (r.tgt.isEmpty()) return true;
		r.pad = padParseInputs(fld.at(1), false, &def);
		r.key = padParseInputs(fld.at(2), true, &def);
		r.rapid = fld.at(3).toInt() > 0;
		rows.append(r);
	} else {
		int i = 0;
		while ((i < PR_JOY) && (name != roleKey[i])) i++;
		if (i >= PR_JOY) return false;
		rows[i].pad = padParseInputs(fld.at(0), false, &rows[i].padDef);
		rows[i].key = padParseInputs(fld.at(1), true, &rows[i].keyDef);
		rows[i].rapid = fld.at(2).toInt() > 0;
	}
	rebuild();
	return true;
}

bool xGamepad::saveFile(const std::string& path) {
	FILE* file = fopen(path.c_str(), "wb");
	if (!file) return false;
	saveConf(file, "");
	fclose(file);
	return true;
}

// A table saved by Save as, or a .pad from before the table: the first has a
// "joystick =" line.
bool xGamepad::loadFile(const std::string& path) {
	FILE* file = fopen(path.c_str(), "rb");
	if (!file) return false;
	QList<std::pair<std::string, std::string> > lines;
	char buf[1024];
	bool table = false;
	while (fgets(buf, sizeof(buf), file)) {
		std::pair<std::string, std::string> spl = splitline(buf);
		if (spl.first.empty() || spl.second.empty()) continue;
		if (spl.first == "joystick") table = true;
		lines.append(spl);
	}
	fclose(file);
	if (!table) {
		importMap(path);
		return true;
	}
	scm = GPS_KEMPSTON;
	resetRows();
	for (int i = 0; i < lines.size(); i++)
		loadConf(lines[i].first, lines[i].second);
	return true;
}

// the joystick row of a scheme a target is, -1 for none
static int padRoleOf(int s, const xJoyMapEntry& e) {
	for (int r = 0; r < PR_JOY; r++) {
		const char* t = schTab[s].role[r];
		if (t && padSameTarget(padTgt(t), e)) return r;
	}
	return -1;
}

// A .pad from before the table, by its path. The joystick it covers most of
// is the scheme and the bindings to it its rows; the rest are extra rows, one
// per target. The kempston.pad that shipped is the defaults: it is told by
// what it binds to, since one the gui ever saved has its hat written as
// buttons 12-15.
void xGamepad::importMap(std::string path) {
	scm = GPS_KEMPSTON;
	resetRows();
	if (path.empty()) return;
	QList<xJoyMapEntry> ents;
	FILE* fh = fopen(path.c_str(), "rb");
	if (!fh) return;
	char buf[1024];
	xJoyMapEntry jent;
	while (fgets(buf, sizeof(buf), fh)) {
		if (padParseLine(buf, &jent, path.c_str())) ents.append(jent);
	}
	fclose(fh);
	bool joy = true;
	foreach(const xJoyMapEntry& e, ents) {
		if (e.dev != JMAP_JOY) joy = false;
	}
	QString name = QFileInfo(QString::fromStdString(path)).fileName();
	if (ents.isEmpty() || ((name == "kempston.pad") && joy)) return;
	// a scheme is only taken for a map that covers most of its rows: one key
	// in common - the A of WASD is QAOP's down - would read as nonsense
	int best = 2;
	int found = -1;
	for (int s = 0; s < GPS_CUSTOM; s++) {
		int roles = 0;
		for (int r = 0; r < PR_JOY; r++) {
			foreach(const xJoyMapEntry& e, ents) {
				if (padRoleOf(s, e) == r) {
					roles++;
					break;
				}
			}
		}
		if (roles > best) {
			best = roles;
			found = s;
		}
	}
	scm = (found < 0) ? GPS_KEMPSTON : found;
	for (int r = 0; r < PR_JOY; r++)
		rows[r].padDef = false;
	foreach(xJoyMapEntry e, ents) {
		xJoyMapEntry src;
		src.type = e.type;
		src.num = e.num;
		src.state = e.state;
		int k = (found < 0) ? -1 : padRoleOf(scm, e);
		if (k < 0) {
			// a PC key the stock layout has on the Spectrum is that Spectrum key now
			int zx = (e.dev == JMAP_KEY) ? padZxOfSeq(e.seq) : 0;
			if (zx) {
				e.dev = JMAP_ZX;
				e.dir = zx;
			}
			k = PR_JOY;
			while ((k < rows.size()) && !padSameTarget(rows[k].tgt.first(), e)) k++;
			if (k >= rows.size()) {
				rows.append(xPadRow());
				rows[k].tgt.append(e);
			}
		}
		rows[k].pad.append(src);
		if (e.rpt > 0) {
			rows[k].rapid = true;
			trate = padRateOfRpt(e.rpt);
		}
	}
	rebuild();
	xlog(XLG_INPUT, XLL_INFO, "pad %c: map '%s' brought in as %s and %i extra row(s)",
		slot ? 'B' : 'A', path.c_str(), pad_scheme_key(scm), (int)rows.size() - PR_JOY);
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
	kbd = GPK_NONE;
	ctrl = false;
	trate = 10;
	scm = GPS_KEMPSTON;
	sjptr = NULL;
#if HAVESDL2
	scptr = NULL;
#endif
	resetRows();
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
	ctrl = isController();
	rebuild();		// a scheme binds a controller by name, a raw pad by number
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
	ctrl = padIsCtrl(pid);
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
	if (!isOpened()) ctrl = padIsCtrl(pid);
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
				xjm.held = 0;
				presslist.append(xjm);
			} else {
				switch(type) {
					case JOY_AXIS:
					case JOY_CAXIS:
						if (sign(state) == sign(xjm.state)) {
							xjm.state = st;
							xjm.rps = 1;
						} else {
							xjm.rps = 0;
						}
						xjm.held = xjm.rps;
						xjm.cnt = 0;
						presslist.append(xjm);
						break;
					case JOY_HAT:
						if (hst & xjm.state) {			// state changed
							xjm.rps = (state & xjm.state) ? 1 : 0;	// pressed or released
							xjm.held = xjm.rps;
							xjm.cnt = 0;
							presslist.append(xjm);
						}
						break;
					case JOY_BUTTON:
					case JOY_CBUTTON:
					case JOY_KEY:
						xjm.cnt = 0;
						xjm.rps = 1;
						xjm.held = 1;
						presslist.append(xjm);
						break;
				}
			}
		}
	}
	return presslist;
}

// Turbo. The phase runs on in thousandths of a flip by the time that went,
// so the rate comes out right on average whatever it is.
QList<xJoyMapEntry> xGamepad::repTick(int ms) {
	QList<xJoyMapEntry> presslist;
	for (int i = 0; i < map.size(); i++) {
		xJoyMapEntry& xjm = map[i];
		if (!xjm.held || (xjm.rpt <= 0)) continue;
		xjm.cnt += 2 * xjm.rpt * ms;		// two flips a press
		if (xjm.cnt < 1000) continue;
		xjm.cnt -= 1000;
		xjm.rps = !xjm.rps;
		presslist.append(xjm);
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

// The four virtual buttons of a hat are laid out from VIRTKEYBASE up in
// dirName's order; both ways of naming a hat direction read from there.
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
	return QString("Hat %0 %1").arg(n >> 2).arg(dirName[n & 3]);
}

#if HAVESDL2

// SDL's own names are config keys ("righttrigger"); these are what the gui says.
// Looked up by that key, so a button an older SDL lacks costs nothing.
static const struct {
	const char* sdl;
	const char* name;		// button, or axis at +
	const char* neg;		// axis at -
} padNameTab[] = {
	{"a", "Button A", NULL},		// a bare letter reads as a key
	{"b", "Button B", NULL},
	{"x", "Button X", NULL},
	{"y", "Button Y", NULL},
	{"back", "Back", NULL},
	{"guide", "Guide", NULL},
	{"start", "Start", NULL},
	{"leftstick", "Left stick click", NULL},
	{"rightstick", "Right stick click", NULL},
	{"leftshoulder", "Left bumper", NULL},
	{"rightshoulder", "Right bumper", NULL},
	{"dpup", "D-pad up", NULL},
	{"dpdown", "D-pad down", NULL},
	{"dpleft", "D-pad left", NULL},
	{"dpright", "D-pad right", NULL},
	{"misc1", "Share", NULL},
	{"paddle1", "Paddle 1", NULL},
	{"paddle2", "Paddle 2", NULL},
	{"paddle3", "Paddle 3", NULL},
	{"paddle4", "Paddle 4", NULL},
	{"touchpad", "Touchpad", NULL},
	{"leftx", "Left stick right", "Left stick left"},
	{"lefty", "Left stick down", "Left stick up"},
	{"rightx", "Right stick right", "Right stick left"},
	{"righty", "Right stick down", "Right stick up"},
	{"lefttrigger", "Left trigger", NULL},
	{"righttrigger", "Right trigger", NULL},
	{NULL, NULL, NULL}
};

static QString padNiceName(const char* sdl, int state) {
	if (sdl == NULL) return QString();
	for (int i = 0; padNameTab[i].sdl; i++) {
		if (!strcmp(sdl, padNameTab[i].sdl))
			return QString((state < 0 && padNameTab[i].neg) ? padNameTab[i].neg : padNameTab[i].name);
	}
	return QString(sdl);
}

#endif

// How a binding reads in the gui - the map table and the bind dialog both
// say it this way.
QString xGamepad::getEntryName(const xJoyMapEntry& jent) {
	switch (jent.type) {
#if HAVESDL2
		case JOY_CBUTTON:
			return padNiceName(SDL_GameControllerGetStringForButton((SDL_GameControllerButton)jent.num), 1);
		case JOY_CAXIS:
			return padNiceName(SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)jent.num), jent.state);
#endif
		case JOY_BUTTON:
			return getButtonName(jent.num);
		case JOY_KEY:
			return pad_key_name(jent.num);
		case JOY_VDIR: {
			static const char* dir[4] = {"Gamepad up", "Gamepad down", "Gamepad left", "Gamepad right"};
			return QString(dir[jent.num & 3]);
		}
		case JOY_AXIS:
			return QString("Axis %0 %1").arg(jent.num).arg((jent.state < 0) ? "-" : "+");
		case JOY_HAT: {
			int d = hatDirIdx(jent.state);
			return QString("Hat %0 %1").arg(jent.num).arg((d < 0) ? "??" : dirName[d]);
		}
	}
	return QString();
}

// What a binding drives, as the map table and the window say it.
QString xGamepad::getTargetName(const xJoyMapEntry& jent) {
	static const char* joyDir[] = {"up", "down", "left", "right", "fire", "fire 2", "fire 3", "fire 4"};
	static const int joyBit[] = {XJ_UP, XJ_DOWN, XJ_LEFT, XJ_RIGHT, XJ_FIRE, XJ_BUT2, XJ_BUT3, XJ_BUT4};
	static const char* mouDir[] = {"up", "down", "left", "right", "left button", "middle button",
		"right button", "wheel up", "wheel down"};
	static const int mouBit[] = {XM_UP, XM_DOWN, XM_LEFT, XM_RIGHT, XM_LMB, XM_MMB, XM_RMB, XM_WHEELUP, XM_WHEELDN};
	QString dir = "??";
	unsigned i;
	switch (jent.dev) {
		case JMAP_KEY:
#if USE_SEQ_BIND
			return QString("PC key %0").arg(jent.seq.toString(QKeySequence::NativeText));
#else
			return QString("PC key %0").arg(getKeyNameById(jent.key));
#endif
		case JMAP_ZX:
			return pad_zx_name(jent.dir);
		case JMAP_JOY:
		case JMAP_JOYB:
			for (i = 0; i < sizeof(joyBit) / sizeof(int); i++)
				if (jent.dir == joyBit[i]) dir = joyDir[i];
			return QString((jent.dev == JMAP_JOY) ? "Kempston %0" : "Joystick 2 %0").arg(dir);
		case JMAP_MOUSE:
			for (i = 0; i < sizeof(mouBit) / sizeof(int); i++)
				if (jent.dir == mouBit[i]) dir = mouDir[i];
			return QString("Mouse %0").arg(dir);
		case JMAP_CUT: {
			xShortcut* cut = find_shortcut_id(jent.dir);
			return cut ? QString(cut->text) : QString("??");
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
	gpadb->slot = 1;
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
	bool done[2] = {gpada->isKeyboard(), gpadb->isKeyboard()};	// on the keyboard: no pad
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
		if (slot[n]->isKeyboard()) continue;
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
	meetNew(devs, pick);
}

// both QAOPs are one joystick, the key fire presses aside
int pad_scheme_kind(int s) {
	return (s == GPS_QAOPM) ? GPS_QAOP : s;
}

// A model never met before goes to the first slot with no pad of its own, so
// plugging one in is all it takes. Once per model: a pad the user took out of
// a slot is not pushed back in the next time it is plugged.
void xGamepadController::meetNew(const QList<xPadDev>& devs, const int* pick) {
	xGamepad* slot[2] = {gpada, gpadb};
	for (int i = 0; i < devs.size(); i++) {
		const xPadId& id = devs.at(i).id;
		QString key = id.guid.isEmpty() ? id.name : id.guid;
		if (seen.contains(key)) continue;
		seen.append(key);
		if ((i == pick[0]) || (i == pick[1])) continue;
		for (int s = 0; s < 2; s++) {
			if (slot[s]->inUse()) continue;
			slot[s]->openDev(devs.at(i));
			xlog(XLG_INPUT, XLL_INFO, "pad %c: new pad '%s' taken", s ? 'B' : 'A', id.name.toUtf8().data());
			emit newPad(s);
			break;
		}
	}
}

void xGamepadController::timerEvent(QTimerEvent* e) {
#ifdef HAVESDL2
	// Pump the events first: SDL fills the pad state from the pump, so reading
	// it before would hand out the values of the previous tick.
	SDL_Event ev;
	bool changed = false;
	// a slot waiting for a pad of its own: a pad never met before goes to one
	// with none, and newPad says that
	bool waits[2] = {gpada->inUse() && !gpada->isOpened(), gpadb->inUse() && !gpadb->isOpened()};
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
	if (changed) {
		rescan();
		emit devicesChanged();
		if (waits[0] && gpada->isOpened()) emit padOn(0);
		if (waits[1] && gpadb->isOpened()) emit padOn(1);
	}
#endif
	gpada->update();
	gpadb->update();
}

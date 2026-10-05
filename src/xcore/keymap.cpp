#include <fstream>

#include <QFile>
#include <QKeyEvent>

#include <string.h>
#include <math.h>

#include "xcore.h"

// KEYMAPS

static keyEntry keyMap[256];			// current keymap (init at start from keyMapInit[]

// profiextkeys: the Profi xt keyboard, used instead of zxkeys when the host keyboard is grabbed;
// a letter with bit 7 set is that letter plus EXT (F1-F10, Home, End, PgUp, PgDn, Ins, Del)
//	{"keyname",xkeyid,"zxkeys","profiextkeys",{cpmcode,rowscan}(atm),ps2code(set3),atcode(set2),xtcode(set1),joybind[,"profiextkeys with Shift"]}
static keyEntry keyMapInit[] = {
	{"1",XKEY_1,{'1',0},{0,0},{'1',0x31},0x16,0x16,0x02,0},
	{"2",XKEY_2,{'2',0},{0,0},{'2',0x32},0x1e,0x1e,0x03,0},
	{"3",XKEY_3,{'3',0},{0,0},{'3',0x33},0x26,0x26,0x04,0},
	{"4",XKEY_4,{'4',0},{0,0},{'4',0x34},0x25,0x25,0x05,0},
	{"5",XKEY_5,{'5',0},{0,0},{'5',0x35},0x2e,0x2e,0x06,0},
	{"6",XKEY_6,{'6',0},{0,0},{'6',0x45},0x36,0x36,0x07,0,{'S','h'}},
	{"7",XKEY_7,{'7',0},{0,0},{'7',0x44},0x3d,0x3d,0x08,0,{'S','6'}},
	{"8",XKEY_8,{'8',0},{0,0},{'8',0x43},0x3e,0x3e,0x09,0,{'S','b'}},
	{"9",XKEY_9,{'9',0},{0,0},{'9',0x42},0x46,0x46,0x0a,0,{'S','8'}},
	{"0",XKEY_0,{'0',0},{0,0},{'0',0x41},0x45,0x45,0x0b,0,{'S','9'}},
	{"Q",XKEY_Q,{'q',0},{0,0},{'Q',0x21},0x15,0x15,0x10,0},
	{"W",XKEY_W,{'w',0},{0,0},{'W',0x22},0x1d,0x1d,0x11,0},
	{"E",XKEY_E,{'e',0},{0,0},{'E',0x23},0x24,0x24,0x12,0},
	{"R",XKEY_R,{'r',0},{0,0},{'R',0x24},0x2d,0x2d,0x13,0},
	{"T",XKEY_T,{'t',0},{0,0},{'T',0x25},0x2c,0x2c,0x14,0},
	{"Y",XKEY_Y,{'y',0},{0,0},{'Y',0x55},0x35,0x35,0x15,0},
	{"U",XKEY_U,{'u',0},{0,0},{'U',0x54},0x3c,0x3c,0x16,0},
	{"I",XKEY_I,{'i',0},{0,0},{'I',0x53},0x43,0x43,0x17,0},
	{"O",XKEY_O,{'o',0},{0,0},{'O',0x52},0x44,0x44,0x18,0},
	{"P",XKEY_P,{'p',0},{0,0},{'P',0x51},0x4d,0x4d,0x19,0},
	{"A",XKEY_A,{'a',0},{0,0},{'A',0x11},0x1c,0x1c,0x1e,0},
	{"S",XKEY_S,{'s',0},{0,0},{'S',0x12},0x1b,0x1b,0x1f,0},
	{"D",XKEY_D,{'d',0},{0,0},{'D',0x13},0x23,0x23,0x20,0},
	{"F",XKEY_F,{'f',0},{0,0},{'F',0x14},0x2b,0x2b,0x21,0},
	{"G",XKEY_G,{'g',0},{0,0},{'G',0x15},0x34,0x34,0x22,0},
	{"H",XKEY_H,{'h',0},{0,0},{'H',0x65},0x33,0x33,0x23,0},
	{"J",XKEY_J,{'j',0},{0,0},{'J',0x64},0x3b,0x3b,0x24,0},
	{"K",XKEY_K,{'k',0},{0,0},{'K',0x63},0x42,0x42,0x25,0},
	{"L",XKEY_L,{'l',0},{0,0},{'L',0x62},0x4b,0x4b,0x26,0},
	{"ENT",XKEY_ENTER,{'E',0},{0,0},{0x0d,0x61},0x5a,0x5a,0x1c,0},
	{"LS",XKEY_LSHIFT,{'C',0},{'S',0},{0,0x08},0x12,0x12,0x2a,0},
	{"Z",XKEY_Z,{'z',0},{0,0},{'Z',0x02},0x1a,0x1a,0x2c,0},
	{"X",XKEY_X,{'x',0},{0,0},{'X',0x03},0x22,0x22,0x2d,0},
	{"C",XKEY_C,{'c',0},{0,0},{'C',0x04},0x21,0x21,0x2e,0},
	{"V",XKEY_V,{'v',0},{0,0},{'V',0x05},0x2a,0x2a,0x2f,0},
	{"B",XKEY_B,{'b',0},{0,0},{'B',0x75},0x32,0x32,0x30,0},
	{"N",XKEY_N,{'n',0},{0,0},{'N',0x74},0x31,0x31,0x31,0},
	{"M",XKEY_M,{'m',0},{0,0},{'M',0x73},0x3a,0x3a,0x32,0},
	{"LC",XKEY_LCTRL,{'S',0},{'C',0},{0,0x80},0x11,0x14,0x1d,0},		// TODO
	{"SPC",XKEY_SPACE,{' ',0},{0,0},{0x20,0x71},0x29,0x29,0x39,0},

	{"RS",XKEY_RSHIFT,{0,0},{'S',0},{0,0},0x59,0x59,0x36,0},
	{"RC",XKEY_RCTRL,{0,0},{'C',0},{0,0},0x58,0x14e0,0x1de0,0},

	{"LEFT",XKEY_LEFT,{'C','5'},{'C','5'},{0x72,0x3d},0x61,0x6be0,0x4be0,0},
	{"RIGHT",XKEY_RIGHT,{'C','8'},{'C','8'},{0x73,0x4b},0x6a,0x74e0,0x4de0,0},
	{"DOWN",XKEY_DOWN,{'C','6'},{'C','6'},{0x71,0x4d},0x60,0x72e0,0x50e0,0},
	{"UP",XKEY_UP,{'C','7'},{'C','7'},{0x70,0x4c},0x63,0x75e0,0x48e0,0},

	{"BSP",XKEY_BSP,{'C','0'},{'C','0'},{0x08,0x49},0x66,0x66,0x0e,0},
	{"CAPS",XKEY_CAPS,{'C','2'},{'C','S'},{0,0x3a},0x14,0x58,0x3a,0},
	{"TAB",XKEY_TAB,{'C',' '},{'C','i'},{0x09,0x3b},0x0d,0x0d,0x0f,0},
	{"[",XKEY_LBRACK,{'S','8'},{'S','y'},{'[',0xd5},0x54,0x54,0x1a,0},
	{"]",XKEY_RBRACK,{'S','9'},{'S','u'},{']',0xd4},0x5b,0x5b,0x1b,0},
	{"`",XKEY_TILDA,{'C','S'},{'S','x'},{0x60,0x91},0x0e,0x0e,0x29,0},
	{"\\",XKEY_SLASH,{'S','C'},{'S','d'},{0x2f,0x92},0x5c,0x5d,0x2b,0},		// TODO

	{"PGUP",XKEY_PGUP,{'C','3'},{'m'|0x80,0},{0x75,0x49},0x6f,0x7de0,0x49e0,0},
	{"PGDN",XKEY_PGDN,{'C','4'},{'n'|0x80,0},{0x74,0x51},0x6d,0x7ae0,0x51e0,0},

	{"DEL",XKEY_DEL,{'C','9'},{'p'|0x80,0},{0x79,0x49},0x64,0x71e0,0x53e0,0},
	{"INS",XKEY_INS,{0,0},{'o'|0x80,0},{0x78,0x84},0x67,0x70e0,0x52e0,0},
	{"HOME",XKEY_HOME,{'S','q'},{'k'|0x80,0},{0x76,0},0x6e,0x6ce0,0x47e0,0},
	{"END",XKEY_END,{'S','e'},{'l'|0x80,0},{0x77,0},0x65,0x69e0,0x4fe0,0},

	{";",XKEY_DOTCOM,{'S','o'},{'S','o'},{0x3b,0xd2},0x4c,0x4c,0x27,0,{'S','z'}},
	{"\"",XKEY_APOS,{'S','p'},{'S','7'},{0x27,0xd1},0x52,0x52,0x28,0,{'S','p'}},		// '
	{"-",XKEY_MINUS,{'S','j'},{'S','j'},{0x2d,0xe4},0x4e,0x4e,0x0c,0,{'S','0'}},
	{"+",XKEY_EQUAL,{'S','k'},{'S','l'},{0x3d,0xe2},0x44,0x44,0x0d,0,{'S','k'}},		// NOTE
	{",",XKEY_COMMA,{'S','n'},{'S','n'},{0x2c,0xf4},0x41,0x41,0x33,0,{'S','r'}},
	{".",XKEY_PERIOD,{'S','m'},{'S','m'},{0x2e,0xf3},0x49,0x49,0x34,0,{'S','t'}},
	{"/",XKEY_BSLASH,{'S','c'},{'S','v'},{0x5c,0x85},0x4a,0x4a,0x35,0,{'S','c'}},

	{"ESC",XKEY_ESC,{'C',' '},{'C','1'},{0x1b,0x39},0x08,0x76,0x01,0},	// NOTE: pc98xx esc code is 00
	{"F1",XKEY_F1,{0,0},{'a'|0x80,0},{0x61,0xb1},0x07,0x05,0x3b,0},
	{"F2",XKEY_F2,{0,0},{'b'|0x80,0},{0x62,0xb2},0x0f,0x06,0x3c,0},
	{"F3",XKEY_F3,{0,0},{'c'|0x80,0},{0x63,0xb3},0x17,0x04,0x3d,0},
	{"F4",XKEY_F4,{0,0},{'d'|0x80,0},{0x64,0xb4},0x1f,0x0C,0x3e,0},
	{"F5",XKEY_F5,{0,0},{'e'|0x80,0},{0x65,0xb5},0x27,0x03,0x3f,0},
	{"F6",XKEY_F6,{0,0},{'f'|0x80,0},{0x66,0xc5},0x2f,0x0B,0x40,0},
	{"F7",XKEY_F7,{0,0},{'g'|0x80,0},{0x67,0xc4},0x37,0x83,0x41,0},
	{"F8",XKEY_F8,{0,0},{'h'|0x80,0},{0x68,0xc3},0x3f,0x0A,0x42,0},
	{"F9",XKEY_F9,{0,0},{'i'|0x80,0},{0x69,0xc2},0x47,0x01,0x43,0},
	{"F10",XKEY_F10,{0,0},{'j'|0x80,0},{0x6a,0xc1},0x4f,0x09,0x44,0},
	{"F11",XKEY_F11,{0,0},{'S','q'},{0x6b,0},0x56,0x78,0x57,0},

	{"LA",XKEY_LALT,{0,0},{'S','E'},{0,0},0x19,0x11,0x38,0},
	{"RA",XKEY_RALT,{0,0},{'S',' '},{0,0},0x39,0x11e0,0x38e0,0},		// nec: kana key

	{"NLOCK", XKEY_NUMLCK,{0,0},{0,0},{0,0},0x76,0x77,0x45,0},
	{"NSLASH", XKEY_NSLASH,{0,0},{0,0},{0,0},0x4a,0x4ae0,0x35e0,0},
	{"NMUL", XKEY_NMUL,{0,0},{0,0},{0,0},0x7e,0x7c,0x37,0},
	{"NMINUS", XKEY_NMINUS,{0,0},{0,0},{0,0},0x4e,0x7b,0x4a,0},
	{"NPLUS", XKEY_NPLUS,{0,0},{0,0},{0,0},0x7c,0x79,0x4e,0},
	{"NENT", XKEY_NENTER,{0,0},{0,0},{0,0},0x79,0x5ae0,0x1ce0,0},
	{"NDOT", XKEY_NDOT,{0,0},{0,0},{0,0},0x71,0x71,0x53,0},
	{"N0", XKEY_N0,{0,0},{0,0},{0,0},0x70,0x70,0x52,0},
	{"N1", XKEY_N1,{0,0},{0,0},{0,0},0x69,0x69,0x4f,0},
	{"N2", XKEY_N2,{0,0},{0,0},{0,0},0x72,0x72,0x50,0},
	{"N3", XKEY_N3,{0,0},{0,0},{0,0},0x7a,0x7a,0x51,0},
	{"N4", XKEY_N4,{0,0},{0,0},{0,0},0x6b,0x6b,0x4b,0},
	{"N5", XKEY_N5,{0,0},{0,0},{0,0},0x73,0x73,0x4c,0},
	{"N6", XKEY_N6,{0,0},{0,0},{0,0},0x74,0x74,0x4d,0},
	{"N7", XKEY_N7,{0,0},{0,0},{0,0},0x6c,0x6c,0x47,0},
	{"N8", XKEY_N8,{0,0},{0,0},{0,0},0x75,0x75,0x48,0},
	{"N9", XKEY_N9,{0,0},{0,0},{0,0},0x7d,0x7d,0x49,0},

	{"",ENDKEY,{0,0},{0,0},{0,0},0,0,0,0}
};

keyEntry getKeyEntry(int qkey) {
	int idx = 0;
	while ((keyMap[idx].key != ENDKEY) && (keyMap[idx].key != qkey)) {
		idx++;
	}
	return keyMap[idx];
}

int getKeyIdByName(const char* name) {
	int idx = 0;
	while ((keyMap[idx].key != ENDKEY) && strcmp(name, keyMap[idx].name)) {
		idx++;
	}
	return keyMap[idx].key;
}

const char* getKeyNameById(int id) {
	int idx = 0;
	while ((keyMap[idx].key != ENDKEY) && (keyMap[idx].key != id)) {
		idx++;
	}
	return keyMap[idx].name;
}

void setKey(const char* kname, const char* kstr) {
	int idx = 0;
	int pos,kpos;
	while (keyMap[idx].key != ENDKEY) {
		if (!strcmp(kname, keyMap[idx].name)) {
			memset(keyMap[idx].zxKey, 0, KEYSEQ_MAXLEN);
			pos = 0;
			kpos = 0;
			while (pos < KEYSEQ_MAXLEN-1) {
				switch(kstr[kpos]) {
					case 0x00:
						pos = KEYSEQ_MAXLEN;
						break;
					case 'J':
						kpos++;
						switch (kstr[kpos]) {
							case 'U': keyMap[idx].joyMask = XJ_UP; break;
							case 'D': keyMap[idx].joyMask = XJ_DOWN; break;
							case 'R': keyMap[idx].joyMask = XJ_RIGHT; break;
							case 'L': keyMap[idx].joyMask = XJ_LEFT; break;
							case 'F': keyMap[idx].joyMask = XJ_FIRE; break;
							case '2': keyMap[idx].joyMask = XJ_BUT2; break;
							case '3': keyMap[idx].joyMask = XJ_BUT3; break;
							case '4': keyMap[idx].joyMask = XJ_BUT4; break;
						}
						kpos++;
						if (kstr[kpos] == '*') {
							keyMap[idx].joyMask |= XJ_JOYB;
							kpos++;
						}
						break;
					default:
						keyMap[idx].zxKey[pos] = kstr[kpos];
						pos++;
						kpos++;
						break;
				}
			}
//			strncpy((char*)keyMap[idx].zxKey, kstr, KEYSEQ_MAXLEN - 1);
//			printf("%s -> %c %c\n", keyMap[idx].name, key1, key2);
		}
		idx++;
	}
}

void initKeyMap() {
//	printf("init keys\n");
	int idx = -1;
	do {
		idx++;
		keyMap[idx] = keyMapInit[idx];
	} while (keyMapInit[idx].key != ENDKEY);
}

static bool keys_have_joy() {
	for (int i = 0; keyMap[i].key != ENDKEY; i++) {
		if (keyMap[i].joyMask) return true;
	}
	return false;
}

// A machine with no keyboard - the ALF console - has nothing to type on, so the
// keys that would send ZX keys drive its two joysticks instead. Laid over
// whatever layout is loaded, and only when that layout binds no joystick at all,
// so a layout that says where the joystick lives always wins. The layout the
// user picked is never changed: switching to such a machine and back must not
// lose it, which is why this is not "switch to the console's own layout".
static void keys_joy_only() {
	setKey("LEFT", "JL");	setKey("RIGHT", "JR");
	setKey("UP", "JU");	setKey("DOWN", "JD");
	setKey("SPC", "JF");	setKey("ENT", "JF");
	setKey("A", "JL*");	setKey("D", "JR*");
	setKey("W", "JU*");	setKey("S", "JD*");
	setKey("LS", "JF*");	setKey("LC", "JF*");
}

static void load_key_file() {
	if (conf.kmapName.empty() || (conf.kmapName == "default")) return;
	QFile file(xres_path("keymaps", QString::fromLocal8Bit(conf.kmapName.c_str())));
	if (!file.open(QFile::ReadOnly)) {
		xlog(XLG_INPUT, XLL_WARN, "can't open the keyboard layout, using the default one");
		return;
	}
	std::string line;
	std::vector<std::string> vec;
	char keys[8];
	int rlen;
	unsigned int i;
	while (!file.atEnd()) {
		QByteArray ba = file.readLine();
		while (ba.endsWith('\n') || ba.endsWith('\r'))		// a CRLF checkout, see CLAUDE.md
			ba.chop(1);
		line = std::string(ba.data());
		vec = splitstr(line,"\t");
		memset(keys, 0, 8);
		rlen = 0;
		if (vec.size() > 0) {
			for(i = 1; (rlen < KEYSEQ_MAXLEN) && (i < vec.size()); i++) {
				rlen += vec[i].size();
				if (rlen < KEYSEQ_MAXLEN)
					strcat(keys, vec[i].c_str());
			}
			setKey(vec[0].c_str(), keys);
		}
	}
}

void loadKeys() {
	initKeyMap();
	load_key_file();
	if (conf.zx && conf.zx->hw && !conf.zx->hw->keyp && !keys_have_joy())
		keys_joy_only();
}

// key translation qt->xkey

struct keyTrans {
	int keyLat;		// Qt::Key_Q : qt keycode @ QWERTY layout
	int keyRus;		// 0x419 : qt keycode @ russian 'JZUKEN' layout
	int keyId;		// internal key id = XKEY_*
};

static keyTrans ktTab[] = {
	{Qt::Key_0, Qt::Key_0, XKEY_0},
	{Qt::Key_1, Qt::Key_1, XKEY_1},
	{Qt::Key_2, Qt::Key_2, XKEY_2},
	{Qt::Key_3, Qt::Key_3, XKEY_3},
	{Qt::Key_4, Qt::Key_4, XKEY_4},
	{Qt::Key_5, Qt::Key_5, XKEY_5},
	{Qt::Key_6, Qt::Key_6, XKEY_6},
	{Qt::Key_7, Qt::Key_7, XKEY_7},
	{Qt::Key_8, Qt::Key_8, XKEY_8},
	{Qt::Key_9, Qt::Key_9, XKEY_9},
	{Qt::Key_Minus, Qt::Key_Minus, XKEY_MINUS},
	{Qt::Key_Plus, Qt::Key_Plus, XKEY_EQUAL},
	{Qt::Key_Equal, Qt::Key_Equal, XKEY_EQUAL},
	{Qt::Key_Backspace, Qt::Key_Backspace, XKEY_BSP},
	{Qt::Key_QuoteLeft, 1025, XKEY_TILDA},		// Ё

	{Qt::Key_Exclam, Qt::Key_Exclam, XKEY_1},		// !
	{Qt::Key_At, Qt::Key_QuoteDbl, XKEY_2},			// @ (")
	{Qt::Key_NumberSign, 8470, XKEY_3},			// # (№)
	{Qt::Key_Dollar, Qt::Key_Dollar, XKEY_4},		// $
	{Qt::Key_Percent, Qt::Key_Percent, XKEY_5},		// %
	{Qt::Key_AsciiCircum, Qt::Key_AsciiCircum, XKEY_6},	// ^
	{Qt::Key_Ampersand, Qt::Key_Ampersand, XKEY_7},		// &
	{Qt::Key_Asterisk, Qt::Key_Asterisk, XKEY_8},		// *
	{Qt::Key_ParenLeft, Qt::Key_ParenLeft, XKEY_9},		// (
	{Qt::Key_ParenRight, Qt::Key_ParenRight, XKEY_0},	// )

	{Qt::Key_Tab, Qt::Key_Tab, XKEY_TAB},
	{Qt::Key_Q, 1049, XKEY_Q},
	{Qt::Key_W, 1062, XKEY_W},
	{Qt::Key_E, 1059, XKEY_E},
	{Qt::Key_R, 1050, XKEY_R},
	{Qt::Key_T, 1045, XKEY_T},
	{Qt::Key_Y, 1053, XKEY_Y},
	{Qt::Key_U, 1043, XKEY_U},
	{Qt::Key_I, 1064, XKEY_I},
	{Qt::Key_O, 1065, XKEY_O},
	{Qt::Key_P, 1047, XKEY_P},
	{Qt::Key_BracketLeft, 1061, XKEY_LBRACK},		// [
	{Qt::Key_BracketRight, 1066, XKEY_RBRACK},		// ]
//	{Qt::Key_BraceLeft, 1061, XKEY_LBRACE},			// { == Shift + [
//	{Qt::Key_BraceRight, 1066, XKEY_LBRACE},		// } == Shift + ]
	{Qt::Key_Backslash, Qt::Key_Backslash, XKEY_SLASH},	// |

	{Qt::Key_CapsLock, Qt::Key_CapsLock, XKEY_CAPS},
	{Qt::Key_A, 1060, XKEY_A},
	{Qt::Key_S, 1067, XKEY_S},
	{Qt::Key_D, 1042, XKEY_D},
	{Qt::Key_F, 1040, XKEY_F},
	{Qt::Key_G, 1055, XKEY_G},
	{Qt::Key_H, 1056, XKEY_H},
	{Qt::Key_J, 1054, XKEY_J},
	{Qt::Key_K, 1051, XKEY_K},
	{Qt::Key_L, 1044, XKEY_L},
	{Qt::Key_Semicolon, 1046, XKEY_DOTCOM},
	{Qt::Key_Apostrophe, 1069, XKEY_APOS},
	{Qt::Key_Return, Qt::Key_Enter, XKEY_ENTER},

	{Qt::Key_Shift, Qt::Key_Shift, XKEY_LSHIFT},
	{Qt::Key_Z, 1071, XKEY_Z},
	{Qt::Key_X, 1063, XKEY_X},
	{Qt::Key_C, 1057, XKEY_C},
	{Qt::Key_V, 1052, XKEY_V},
	{Qt::Key_B, 1048, XKEY_B},
	{Qt::Key_N, 1058, XKEY_N},
	{Qt::Key_M, 1068, XKEY_M},
	{Qt::Key_Comma, 0x411, XKEY_COMMA},			// ,
	{Qt::Key_Period, 0x42e, XKEY_PERIOD},			// .
	{Qt::Key_Slash, Qt::Key_Slash, XKEY_BSLASH},		// ?
	{Qt::Key_Apostrophe, 0x44d, XKEY_APOS},			// '

#ifdef __APPLE__
	{Qt::Key_Meta, Qt::Key_Meta, XKEY_LCTRL},
#else
	{Qt::Key_Control, Qt::Key_Control, XKEY_LCTRL},
#endif
	{Qt::Key_Alt, Qt::Key_Alt, XKEY_LALT},
	{Qt::Key_Space, Qt::Key_Space, XKEY_SPACE},

	{Qt::Key_Escape, Qt::Key_Escape, XKEY_ESC},
	{Qt::Key_F1, Qt::Key_F1, XKEY_F1},
	{Qt::Key_F2, Qt::Key_F2, XKEY_F2},
	{Qt::Key_F3, Qt::Key_F3, XKEY_F3},
	{Qt::Key_F4, Qt::Key_F4, XKEY_F4},
	{Qt::Key_F5, Qt::Key_F5, XKEY_F5},
	{Qt::Key_F6, Qt::Key_F6, XKEY_F6},
	{Qt::Key_F7, Qt::Key_F7, XKEY_F7},
	{Qt::Key_F8, Qt::Key_F8, XKEY_F8},
	{Qt::Key_F9, Qt::Key_F9, XKEY_F9},
	{Qt::Key_F10, Qt::Key_F10, XKEY_F10},
	{Qt::Key_F11, Qt::Key_F11, XKEY_F11},
	{Qt::Key_F12, Qt::Key_F12, XKEY_F12},

	{Qt::Key_Up, Qt::Key_Up, XKEY_UP},
	{Qt::Key_Down, Qt::Key_Down, XKEY_DOWN},
	{Qt::Key_Left, Qt::Key_Left, XKEY_LEFT},
	{Qt::Key_Right, Qt::Key_Right, XKEY_RIGHT},

	{Qt::Key_Home, Qt::Key_Home, XKEY_HOME},
	{Qt::Key_End, Qt::Key_End, XKEY_END},
	{Qt::Key_Insert, Qt::Key_Insert, XKEY_INS},
	{Qt::Key_Delete, Qt::Key_Delete, XKEY_DEL},
	{Qt::Key_PageUp, Qt::Key_PageUp, XKEY_PGUP},
	{Qt::Key_PageDown, Qt::Key_PageDown, XKEY_PGDN},

	{Qt::Key_SysReq, Qt::Key_SysReq, XKEY_SYSRQ},
	{Qt::Key_Pause, Qt::Key_Pause, XKEY_PAUSE},
	{Qt::Key_ScrollLock, Qt::Key_ScrollLock, XKEY_SCRLCK},
	{Qt::Key_NumLock, Qt::Key_NumLock, XKEY_NUMLCK},

	// TODO: complete this table
	{Qt::Key_unknown, Qt::Key_unknown, ENDKEY}
};

static keyTrans numPadTab[] = {
#ifdef __APPLE__
// why the f-ck arrow keys have numpad modifier in macosx?
	{Qt::Key_Left, Qt::Key_Left, XKEY_LEFT},
	{Qt::Key_Right, Qt::Key_Right, XKEY_RIGHT},
	{Qt::Key_Up, Qt::Key_Up, XKEY_UP},
	{Qt::Key_Down, Qt::Key_Down, XKEY_DOWN},
#endif
	{Qt::Key_0, Qt::Key_Insert, XKEY_N0},
	{Qt::Key_1, Qt::Key_End, XKEY_N1},
	{Qt::Key_2, Qt::Key_Down, XKEY_N2},
	{Qt::Key_3, Qt::Key_PageDown, XKEY_N3},
	{Qt::Key_4, Qt::Key_Left, XKEY_N4},
	{Qt::Key_5, Qt::Key_5, XKEY_N5},
	{Qt::Key_6, Qt::Key_Right, XKEY_N6},
	{Qt::Key_7, Qt::Key_Home, XKEY_N7},
	{Qt::Key_8, Qt::Key_Up, XKEY_N8},
	{Qt::Key_9, Qt::Key_PageUp, XKEY_N9},
	{Qt::Key_Period, Qt::Key_Delete, XKEY_NDOT},
	{Qt::Key_Slash, Qt::Key_Slash, XKEY_NSLASH},
	{Qt::Key_Asterisk, Qt::Key_Asterisk, XKEY_NMUL},
	{Qt::Key_Minus, Qt::Key_Minus, XKEY_NMINUS},
	{Qt::Key_Plus, Qt::Key_Plus, XKEY_NPLUS},
	{Qt::Key_Enter, Qt::Key_Return, XKEY_NENTER},
	{Qt::Key_unknown, Qt::Key_unknown, ENDKEY}
};

int qKey2id(int qkey, Qt::KeyboardModifiers mod) {
	int idx = 0;
	keyTrans* tab = (mod & Qt::KeypadModifier) ? numPadTab : ktTab;
	while ((tab[idx].keyLat != qkey) && (tab[idx].keyRus != qkey) && (tab[idx].keyLat != Qt::Key_unknown)) {
		idx++;
	}
	return tab[idx].keyId;
}

int key2qid(int key) {
	int idx = 0;
	while ((ktTab[idx].keyId != key) && (ktTab[idx].keyLat != Qt::Key_unknown)) {
		idx++;
	}
	return ktTab[idx].keyLat;
}

// shortcuts

// what each action is; its keys come from the preset or Custom (hotkeys_set)
static xShortcut short_tab[] = {
	{SCG_MAIN | SCG_DEBUGA, XCUT_OPTIONS, "key.options", "Options", {}},
	{SCG_MAIN, XCUT_HOTKEYS, "key.hotkeys", "Hotkeys list", {}},
	{SCG_MAIN | SCG_DEBUGA, XCUT_DEBUG, "key.debuger", "Debugger", {}},
	{SCG_MAIN, XCUT_PAUSE, "key.pause", "Pause", {}},
	{SCG_MAIN, XCUT_FAST, "key.fast", "Fast mode", {}},
	{SCG_MAIN, XCUT_REWIND, "key.rewind", "Rewind (hold)", {}},
	{SCG_MAIN, XCUT_FFWD, "key.ffwd", "Fast forward", {}},
	{SCG_MAIN, XCUT_SLOWMO, "key.slowmo", "Slow motion", {}},
	{SCG_MAIN | SCG_DEBUGA, XCUT_SAVE, "key.save", "Save", {}},
	{SCG_MAIN | SCG_DEBUGA, XCUT_LOAD, "key.load", "Open", {}},
	{SCG_MAIN, XCUT_FASTSAVE, "key.fastsave", "Fast saving", {}},
	{SCG_MAIN, XCUT_MOUSE, "key.mouse.grab", "Grab mouse", {}},
	{SCG_MAIN, XCUT_GRABKBD, "key.keyboard.grab","Grab keyboard", {}},
	{SCG_MAIN | SCG_DEBUGA, XCUT_KEYBOARD, "key.keywin", "Show virtual keyboard", {}},
	{SCG_MAIN, XCUT_TAPWIN, "key.tapewin", "Show tape player", {}},
	{SCG_MAIN, XCUT_RZXWIN, "key.rzxwin", "Show RZX player", {}},
	{SCG_MAIN | SCG_DEBUGA, XCUT_SCRWIN, "key.scrwin", "Show screen window", {}},
	{SCG_MAIN | SCG_DEBUGA, XCUT_SNDWIN, "key.sndwin", "Show sound chip window", {}},
	{SCG_MAIN, XCUT_MUTE, "key.mute", "Mute", {}},
	{SCG_MAIN, XCUT_TAPLAY, "key.tape.play", "Tape play", {}},
	{SCG_MAIN, XCUT_TAPREC, "key.tape.rec", "Tape rec", {}},
	{SCG_MAIN, XCUT_SCRSHOT, "key.scrshot", "Screenshot", {}},
	{SCG_MAIN, XCUT_COMBOSHOT, "key.scrshot.combo", "Screenshot combo", {}},
	{SCG_MAIN, XCUT_SIZEX1, "key.size.x1", "Size x1", {}},
	{SCG_MAIN, XCUT_SIZEX2, "key.size.x2", "Size x2", {}},
	{SCG_MAIN, XCUT_SIZEX3, "key.size.x3", "Size x3", {}},
	{SCG_MAIN, XCUT_SIZEX4, "key.size.x4", "Size x4", {}},
	{SCG_MAIN, XCUT_SIZEX5, "key.size.x5", "Size x5", {}},
	{SCG_MAIN, XCUT_SIZEX6, "key.size.x6", "Size x6", {}},
	{SCG_MAIN, XCUT_FULLSCR, "key.fullscreen", "Fullscreen", {}},
	{SCG_MAIN, XCUT_RATIO, "key.ratio", "Keep aspect ratio", {}},
	{SCG_MAIN, XCUT_NOFLICK, "key.noflick", "Noflick", {}},
	{SCG_MAIN, XCUT_NMI, "key.nmi", "NMI", {}},
	{SCG_MAIN | SCG_DEBUGA, XCUT_RESET, "key.reset", "Reset", {}},
	{SCG_MAIN, XCUT_RES_48, "key.reset.48", "Reset to 48K", {}},
	{SCG_MAIN, XCUT_RES_128, "key.reset.128", "Reset to 128K", {}},
	{SCG_MAIN, XCUT_RES_DOS, "key.reset.dos", "Reset to DOS", {}},
	{SCG_MAIN, XCUT_RES_SERVICE, "key.reset.service", "Reset to Service", {}},
	{SCG_MAIN, XCUT_TURBO, "key.turbo", "Switch turbo", {}},
	{SCG_MAIN, XCUT_SPEED_UP, "key.speed.up", "Speed up", {}},
	{SCG_MAIN, XCUT_SPEED_DOWN, "key.speed.down", "Speed down", {}},
	{SCG_MAIN, XCUT_VIDREC, "key.video.rec", "Start/stop video recording", {}},
	{SCG_MAIN, XCUT_WAV_OUT, "key.write.wav", "Start/stop WAV output", {}},
	{SCG_MAIN, XCUT_RELOAD_SHD, "key.reload.shader", "Reload shader", {}},
	{SCG_MAIN, XCUT_RELOAD, "key.reload", "Reload snapshot and labels", {}},
	{SCG_MAIN, XCUT_FAVORITE, "key.favorite.add", "Add to Favorites", {}},
	{SCG_MAIN, XCUT_QUICKSAVE, "key.quick.save", "Quick save", {}},
	{SCG_MAIN, XCUT_QUICKLOAD, "key.quick.load", "Quick load", {}},
	{SCG_MAIN, XCUT_QUICKUNDO, "key.quick.undo", "Undo quick load", {}},
	{SCG_MAIN, XCUT_TAPE_START, "key.tape.start", "Tape to start", {}},

	{SCG_DEBUGA, XCUT_STEPIN, "key.dbg.stepin", "Debugger: Step in", {}},
	{SCG_DEBUGA, XCUT_STEPOVER, "key.dbg.stepover", "Debugger: Step over", {}},
	{SCG_DEBUGA, XCUT_STEPOUT, "key.dbg.stepout", "Debugger: Step out", {}},
	{SCG_DEBUGA, XCUT_FASTSTEP, "key.dbg.faststep", "Debugger: Fast step", {}},
	{SCG_DEBUGA, XCUT_TMPBRK, "key.dbg.runtohere", "Debugger: Stop here", {}},
	{SCG_DEBUGA, XCUT_TRACE, "key.dbg.trace", "Debugger: Trace", {}},
	{SCG_DEBUGA, XCUT_OPEN_DUMP, "key.dbg.opendump", "Debugger: Load dump", {}},
	{SCG_DEBUGA, XCUT_SAVE_DUMP, "key.dbg.savedump", "Debugger: Save dump", {}},
	{SCG_DEBUGA, XCUT_OPEN_XMAP, "key.dbg.openxmap", "Debugger: Load xmap", {}},
	{SCG_DEBUGA, XCUT_SAVE_XMAP, "key.dbg.savexmap", "Debugger: Save xmap", {}},
	{SCG_DEBUGA, XCUT_FINDER, "key.dbg.finder", "Debugger: Find pattern", {}},
	{SCG_DEBUGA, XCUT_LABELS, "key.dbg.labels", "Debugger: Switch labels", {}},
	{SCG_DEBUGA, XCUT_LABLIST, "key.dbg.lablist", "Debugger: Show labels list", {}},
	{SCG_DEBUGA, XCUT_DBG_RELOAD, "key.dbg.reload", "Debugger: Reload snapshot and labels", {}},
	{SCG_DEBUGA, XCUT_DBG_CLOSE, "key.dbg.close", "Debugger: Close", {}},

	{SCG_DISASM, XCUT_TOPC, "key.disasm.topc", "Disasm: Jump to PC", {}},
	{SCG_DISASM, XCUT_SETPC, "key.disasm.setpc", "Disasm: Set PC", {}},
	{SCG_DISASM, XCUT_SETBRK, "key.disasm.setbrk", "Disasm: Breakpoint", {}},
	{SCG_DISASM, XCUT_SETBRK_RD, "key.disasm.setbrk.rd", "Disasm: Breakpoint on read", {}},
	{SCG_DISASM, XCUT_SETBRK_WR, "key.disasm.setbrk.wr", "Disasm: Breakpoint on write", {}},
	{SCG_DISASM, XCUT_SETBRK_ADR, "key.disasm.setbrk.adr", "Disasm: Breakpoint on CPU address", {}},
	{SCG_DISASM, XCUT_MARK1, "key.disasm.mark.1", "Disasm: Bookmark 1", {}},
	{SCG_DISASM, XCUT_MARK2, "key.disasm.mark.2", "Disasm: Bookmark 2", {}},
	{SCG_DISASM, XCUT_MARK3, "key.disasm.mark.3", "Disasm: Bookmark 3", {}},
	{SCG_DISASM, XCUT_MARK4, "key.disasm.mark.4", "Disasm: Bookmark 4", {}},
	{SCG_DISASM, XCUT_MARK5, "key.disasm.mark.5", "Disasm: Bookmark 5", {}},
	{SCG_DISASM, XCUT_GOMARK1, "key.disasm.gomark.1", "Disasm: Go to bookmark 1", {}},
	{SCG_DISASM, XCUT_GOMARK2, "key.disasm.gomark.2", "Disasm: Go to bookmark 2", {}},
	{SCG_DISASM, XCUT_GOMARK3, "key.disasm.gomark.3", "Disasm: Go to bookmark 3", {}},
	{SCG_DISASM, XCUT_GOMARK4, "key.disasm.gomark.4", "Disasm: Go to bookmark 4", {}},
	{SCG_DISASM, XCUT_GOMARK5, "key.disasm.gomark.5", "Disasm: Go to bookmark 5", {}},
	{SCG_DISASM, XCUT_JUMPTO, "key.disasm.jump", "Disasm: Jump to operand", {}},
	{SCG_DISASM, XCUT_RETFROM, "key.disasm.ret", "Disasm: Return", {}},
	{SCG_DISASM, XCUT_GOTOADR, "key.disasm.goto", "Disasm: Go to address", {}},

	{SCG_DUMP, XCUT_DUMP_GOTOADR, "key.dump.goto", "Dump: Go to address", {}},
	{SCG_DUMP, XCUT_DUMP_REG_PC, "key.dump.goto.pc", "Dump: Go to (PC)", {}},
	{SCG_DUMP, XCUT_DUMP_REG_SP, "key.dump.goto.sp", "Dump: Go to (SP)", {}},
	{SCG_DUMP, XCUT_DUMP_REG_BC, "key.dump.goto.bc", "Dump: Go to (BC)", {}},
	{SCG_DUMP, XCUT_DUMP_REG_DE, "key.dump.goto.de", "Dump: Go to (DE)", {}},
	{SCG_DUMP, XCUT_DUMP_REG_HL, "key.dump.goto.hl", "Dump: Go to (HL)", {}},
	{SCG_DUMP, XCUT_DUMP_REG_IX, "key.dump.goto.ix", "Dump: Go to (IX)", {}},
	{SCG_DUMP, XCUT_DUMP_REG_IY, "key.dump.goto.iy", "Dump: Go to (IY)", {}},

	{0, -1, NULL, NULL, {}}
};

// presets: an action not listed has no key

typedef struct {
	int id;
	QKeySequence key[2];
} xCutDef;

// Qt::AltModifier, not Qt::ALT, beside Qt::KeypadModifier: mixing the two
// modifier enums leaves no exact operator| match and clang calls it ambiguous.

// The machine keeps every key it has; the emulator takes the F-keys, the block
// over the arrows and one modifier that is no ZX key: Alt here, Cmd on a Mac.
// Ctrl is Symbol Shift, so only what opens a dialog sits on it.
#ifdef __APPLE__
#define HK_MOD Qt::META
#define HK_MODM Qt::MetaModifier
#define HK_APP Qt::META
#else
#define HK_MOD Qt::ALT
#define HK_MODM Qt::AltModifier
#define HK_APP Qt::CTRL
#endif

// the debugger's breakpoints, bookmarks and Esc, alike in both presets.
// Alt+Space is the window menu on Windows, so a read is Shift+Alt+Space
#define DEBUGGER_KEYS \
	{XCUT_SETBRK, {QKeySequence(Qt::Key_Space)}}, \
	{XCUT_SETBRK_RD, {QKeySequence(Qt::SHIFT | Qt::ALT | Qt::Key_Space)}}, \
	{XCUT_SETBRK_WR, {QKeySequence(HK_APP | Qt::Key_Space)}}, \
	{XCUT_SETBRK_ADR, {QKeySequence(Qt::SHIFT | Qt::Key_Space)}}, \
	{XCUT_DBG_CLOSE, {QKeySequence(Qt::Key_Escape)}}, \
	{XCUT_MARK1, {QKeySequence(HK_APP | Qt::Key_1)}}, {XCUT_MARK2, {QKeySequence(HK_APP | Qt::Key_2)}}, \
	{XCUT_MARK3, {QKeySequence(HK_APP | Qt::Key_3)}}, {XCUT_MARK4, {QKeySequence(HK_APP | Qt::Key_4)}}, \
	{XCUT_MARK5, {QKeySequence(HK_APP | Qt::Key_5)}}, \
	{XCUT_GOMARK1, {QKeySequence(Qt::ALT | Qt::Key_1)}}, {XCUT_GOMARK2, {QKeySequence(Qt::ALT | Qt::Key_2)}}, \
	{XCUT_GOMARK3, {QKeySequence(Qt::ALT | Qt::Key_3)}}, {XCUT_GOMARK4, {QKeySequence(Qt::ALT | Qt::Key_4)}}, \
	{XCUT_GOMARK5, {QKeySequence(Qt::ALT | Qt::Key_5)}},

// the keys as they have always been, after UnrealSpeccy
static const xCutDef cutClassic[] = {
#ifdef __APPLE__
	{XCUT_OPTIONS, {QKeySequence(Qt::META | Qt::Key_Comma)}},
#else
	{XCUT_OPTIONS, {QKeySequence(Qt::Key_F1)}},
#endif
	{XCUT_DEBUG, {QKeySequence(Qt::Key_Escape)}},
	{XCUT_PAUSE, {QKeySequence(Qt::Key_Pause)}},
	{XCUT_FAST, {QKeySequence(Qt::Key_Insert)}},
	{XCUT_REWIND, {QKeySequence(Qt::Key_Delete)}},
	{XCUT_FFWD, {QKeySequence(Qt::Key_Home)}},
	{XCUT_SLOWMO, {QKeySequence(Qt::Key_End)}},
	{XCUT_SAVE, {QKeySequence(Qt::Key_F2)}},
	{XCUT_LOAD, {QKeySequence(Qt::Key_F3)}},
	{XCUT_FASTSAVE, {QKeySequence(Qt::Key_F9)}},
	{XCUT_MOUSE, {QKeySequence(Qt::ALT | Qt::Key_M)}},
	{XCUT_GRABKBD, {QKeySequence(Qt::ALT | Qt::Key_G)}},
	{XCUT_KEYBOARD, {QKeySequence(Qt::ALT | Qt::Key_K)}},
	{XCUT_SCRWIN, {QKeySequence(Qt::ALT | Qt::Key_S)}},
	{XCUT_SNDWIN, {QKeySequence(Qt::ALT | Qt::Key_A)}},
	{XCUT_TAPLAY, {QKeySequence(Qt::Key_F4)}},
	{XCUT_TAPREC, {QKeySequence(Qt::Key_F5)}},
	{XCUT_SCRSHOT, {QKeySequence(Qt::Key_F7)}},
	{XCUT_COMBOSHOT, {QKeySequence(Qt::ALT | Qt::Key_F7)}},
	{XCUT_SIZEX1, {QKeySequence(Qt::ALT | Qt::Key_1)}},
	{XCUT_SIZEX2, {QKeySequence(Qt::ALT | Qt::Key_2)}},
	{XCUT_SIZEX3, {QKeySequence(Qt::ALT | Qt::Key_3)}},
	{XCUT_SIZEX4, {QKeySequence(Qt::ALT | Qt::Key_4)}},
	{XCUT_SIZEX5, {QKeySequence(Qt::ALT | Qt::Key_5)}},
	{XCUT_SIZEX6, {QKeySequence(Qt::ALT | Qt::Key_6)}},
	{XCUT_FULLSCR, {QKeySequence(Qt::ALT | Qt::Key_Return)}},
	{XCUT_RATIO, {QKeySequence(Qt::ALT | Qt::Key_R)}},
	{XCUT_NOFLICK, {QKeySequence(Qt::ALT | Qt::Key_N)}},
	{XCUT_NMI, {QKeySequence(Qt::Key_F10)}},
	{XCUT_RESET, {QKeySequence(Qt::Key_F12)}},
	{XCUT_TURBO, {QKeySequence(Qt::ALT | Qt::Key_T)}},
	{XCUT_SPEED_UP, {QKeySequence(Qt::AltModifier | Qt::KeypadModifier | Qt::Key_Plus)}},
	{XCUT_SPEED_DOWN, {QKeySequence(Qt::AltModifier | Qt::KeypadModifier | Qt::Key_Minus)}},
	{XCUT_VIDREC, {QKeySequence(Qt::CTRL | Qt::Key_F7)}},

	{XCUT_STEPIN, {QKeySequence(Qt::Key_F7)}},
	{XCUT_STEPOVER, {QKeySequence(Qt::Key_F8)}},
	{XCUT_STEPOUT, {QKeySequence(Qt::Key_F6)}},
	{XCUT_FASTSTEP, {QKeySequence(Qt::ALT | Qt::Key_F7)}},
	{XCUT_TMPBRK, {QKeySequence(Qt::Key_F9)}},
	{XCUT_TRACE, {QKeySequence(Qt::CTRL | Qt::Key_T)}},
	{XCUT_OPEN_DUMP, {QKeySequence(Qt::CTRL | Qt::Key_O)}},
	{XCUT_SAVE_DUMP, {QKeySequence(Qt::CTRL | Qt::Key_S)}},
	{XCUT_FINDER, {QKeySequence(Qt::CTRL | Qt::Key_F)}},
	{XCUT_LABELS, {QKeySequence(Qt::CTRL | Qt::Key_L)}},

	{XCUT_TOPC, {QKeySequence(Qt::Key_Home)}},
	{XCUT_SETPC, {QKeySequence(Qt::Key_End)}},
	DEBUGGER_KEYS
	{XCUT_JUMPTO, {QKeySequence(Qt::Key_F4)}},
	{XCUT_RETFROM, {QKeySequence(Qt::Key_F5)}},
	{XCUT_GOTOADR, {QKeySequence(Qt::Key_G)}},

	{XCUT_DUMP_GOTOADR, {QKeySequence(Qt::Key_G)}},
	{XCUT_DUMP_REG_PC, {QKeySequence(Qt::CTRL | Qt::Key_P)}},
	{XCUT_DUMP_REG_SP, {QKeySequence(Qt::CTRL | Qt::Key_S)}},
	{XCUT_DUMP_REG_BC, {QKeySequence(Qt::CTRL | Qt::Key_B)}},
	{XCUT_DUMP_REG_DE, {QKeySequence(Qt::CTRL | Qt::Key_D)}},
	{XCUT_DUMP_REG_HL, {QKeySequence(Qt::CTRL | Qt::Key_H)}},
	{XCUT_DUMP_REG_IX, {QKeySequence(Qt::CTRL | Qt::Key_X)}},
	{XCUT_DUMP_REG_IY, {QKeySequence(Qt::CTRL | Qt::Key_Y)}},
	{-1, {}}
};

static const xCutDef cutModern[] = {
	{XCUT_OPTIONS, {QKeySequence(HK_APP | Qt::Key_Comma)}},
	{XCUT_SAVE, {QKeySequence(Qt::Key_F2), QKeySequence(HK_APP | Qt::Key_S)}},
	{XCUT_LOAD, {QKeySequence(Qt::Key_F3), QKeySequence(HK_APP | Qt::Key_O)}},
	{XCUT_TAPLAY, {QKeySequence(Qt::Key_F4)}},
	{XCUT_TAPE_START, {QKeySequence(Qt::SHIFT | Qt::Key_F4)}},
	{XCUT_QUICKSAVE, {QKeySequence(Qt::Key_F5)}},
	{XCUT_TAPWIN, {QKeySequence(Qt::Key_F6)}},
	{XCUT_SCRSHOT, {QKeySequence(Qt::Key_F7)}},
	{XCUT_VIDREC, {QKeySequence(Qt::SHIFT | Qt::Key_F7)}},
	{XCUT_QUICKLOAD, {QKeySequence(Qt::Key_F9)}},
	{XCUT_QUICKUNDO, {QKeySequence(Qt::SHIFT | Qt::Key_F9)}},
#ifdef __APPLE__
	{XCUT_DEBUG, {QKeySequence(Qt::Key_F10)}},
#else
	{XCUT_DEBUG, {QKeySequence(Qt::Key_F10), QKeySequence(Qt::CTRL | Qt::Key_Cancel)}},
#endif
	{XCUT_NMI, {QKeySequence(Qt::SHIFT | Qt::Key_F10)}},
#ifdef __APPLE__
	{XCUT_FULLSCR, {QKeySequence(Qt::CTRL | Qt::META | Qt::Key_F), QKeySequence(Qt::META | Qt::Key_Return)}},
#else
	{XCUT_FULLSCR, {QKeySequence(Qt::ALT | Qt::Key_Return), QKeySequence(Qt::Key_F11)}},
#endif
	{XCUT_RESET, {QKeySequence(Qt::Key_F12), QKeySequence(HK_MOD | Qt::Key_R)}},
	{XCUT_RES_DOS, {QKeySequence(Qt::ALT | Qt::Key_F12)}},

	{XCUT_PAUSE, {QKeySequence(Qt::Key_Pause), QKeySequence(HK_MOD | Qt::Key_P)}},
	{XCUT_REWIND, {QKeySequence(Qt::Key_Delete), QKeySequence(HK_MOD | Qt::Key_Left)}},
	{XCUT_FFWD, {QKeySequence(Qt::Key_Home), QKeySequence(HK_MOD | Qt::Key_Right)}},
	{XCUT_SLOWMO, {QKeySequence(Qt::Key_End), QKeySequence(HK_MOD | Qt::Key_Down)}},
	{XCUT_FAST, {QKeySequence(Qt::Key_Insert), QKeySequence(HK_MOD | Qt::Key_Up)}},
	{XCUT_SPEED_UP, {QKeySequence(HK_MODM | Qt::KeypadModifier | Qt::Key_Plus), QKeySequence(HK_MOD | Qt::Key_Equal)}},
	{XCUT_SPEED_DOWN, {QKeySequence(HK_MODM | Qt::KeypadModifier | Qt::Key_Minus), QKeySequence(HK_MOD | Qt::Key_Minus)}},
	{XCUT_TURBO, {QKeySequence(HK_MOD | Qt::Key_T)}},
	{XCUT_MUTE, {QKeySequence(HK_MOD | Qt::Key_U)}},

	{XCUT_SIZEX1, {QKeySequence(HK_MOD | Qt::Key_1)}},
	{XCUT_SIZEX2, {QKeySequence(HK_MOD | Qt::Key_2)}},
	{XCUT_SIZEX3, {QKeySequence(HK_MOD | Qt::Key_3)}},
	{XCUT_SIZEX4, {QKeySequence(HK_MOD | Qt::Key_4)}},
	{XCUT_SIZEX5, {QKeySequence(HK_MOD | Qt::Key_5)}},
	{XCUT_SIZEX6, {QKeySequence(HK_MOD | Qt::Key_6)}},
	{XCUT_KEYBOARD, {QKeySequence(HK_MOD | Qt::Key_K)}},
	{XCUT_NOFLICK, {QKeySequence(HK_MOD | Qt::Key_N)}},
	{XCUT_MOUSE, {QKeySequence(Qt::ALT | Qt::Key_M)}},		// Cmd+M minimizes on a Mac
	{XCUT_GRABKBD, {QKeySequence(Qt::Key_ScrollLock), QKeySequence(HK_MOD | Qt::Key_G)}},

	// the debugger keeps the Borland steps it has always had
	{XCUT_STEPIN, {QKeySequence(Qt::Key_F7)}},
	{XCUT_STEPOVER, {QKeySequence(Qt::Key_F8)}},
	{XCUT_STEPOUT, {QKeySequence(Qt::SHIFT | Qt::Key_F8)}},
	{XCUT_FASTSTEP, {QKeySequence(Qt::ALT | Qt::Key_F7)}},
	{XCUT_TMPBRK, {QKeySequence(Qt::Key_F4)}},
	{XCUT_TRACE, {QKeySequence(HK_APP | Qt::Key_T)}},
	{XCUT_OPEN_DUMP, {QKeySequence(HK_APP | Qt::Key_O)}},
	{XCUT_SAVE_DUMP, {QKeySequence(HK_APP | Qt::SHIFT | Qt::Key_S)}},
	{XCUT_FINDER, {QKeySequence(HK_APP | Qt::Key_F)}},
	{XCUT_LABELS, {QKeySequence(HK_APP | Qt::Key_L)}},

	{XCUT_TOPC, {QKeySequence(Qt::Key_Home)}},
	{XCUT_SETPC, {QKeySequence(Qt::Key_End)}},
	DEBUGGER_KEYS
	{XCUT_JUMPTO, {QKeySequence(Qt::ALT | Qt::Key_Right)}},
	{XCUT_RETFROM, {QKeySequence(Qt::ALT | Qt::Key_Left)}},
	{XCUT_GOTOADR, {QKeySequence(Qt::Key_G), QKeySequence(HK_APP | Qt::Key_G)}},

	{XCUT_DUMP_GOTOADR, {QKeySequence(Qt::Key_G), QKeySequence(HK_APP | Qt::Key_G)}},
	{XCUT_DUMP_REG_PC, {QKeySequence(Qt::CTRL | Qt::Key_P)}},
	{XCUT_DUMP_REG_SP, {QKeySequence(Qt::CTRL | Qt::Key_S)}},
	{XCUT_DUMP_REG_BC, {QKeySequence(Qt::CTRL | Qt::Key_B)}},
	{XCUT_DUMP_REG_DE, {QKeySequence(Qt::CTRL | Qt::Key_D)}},
	{XCUT_DUMP_REG_HL, {QKeySequence(Qt::CTRL | Qt::Key_H)}},
	{XCUT_DUMP_REG_IX, {QKeySequence(Qt::CTRL | Qt::Key_X)}},
	{XCUT_DUMP_REG_IY, {QKeySequence(Qt::CTRL | Qt::Key_Y)}},
	{-1, {}}
};

static const char* presetNames[HKP_COUNT] = {"modern", "classic", "custom"};

const char* hotkeys_preset_name(int p) {
	return ((p >= 0) && (p < HKP_COUNT)) ? presetNames[p] : presetNames[HKP_MODERN];
}

int hotkeys_preset_id(const char* name) {
	for (int p = 0; p < HKP_COUNT; p++)
		if (!strcmp(name, presetNames[p])) return p;
	return -1;
}

void hotkeys_default(int preset, int id, QKeySequence* out) {
	out[0] = out[1] = QKeySequence();
	const xCutDef* tab = (preset == HKP_CLASSIC) ? cutClassic : cutModern;
	for (int i = 0; tab[i].id >= 0; i++) {
		if (tab[i].id == id) {
			out[0] = tab[i].key[0];
			out[1] = tab[i].key[1];
			return;
		}
	}
}

// the keys an action has in a set: Custom's own, else its preset's
void hotkeys_resolve(const xHotkeySet& set, int id, QKeySequence* out) {
	if (set.preset == HKP_CUSTOM) {
		auto it = set.over.find(id);
		if (it != set.over.end()) {
			out[0] = it->second.first;
			out[1] = it->second.second;
			return;
		}
		hotkeys_default(set.base, id, out);
	} else {
		hotkeys_default(set.preset, id, out);
	}
}

static xHotkeySet hkSet;

const xHotkeySet& hotkeys_get() {
	return hkSet;
}

void hotkeys_set(const xHotkeySet& set) {
	hkSet = set;
	for (int i = 0; short_tab[i].id >= 0; i++)
		hotkeys_resolve(hkSet, short_tab[i].id, short_tab[i].seq);
}

void shortcut_init() {
	hotkeys_set(xHotkeySet());
}

// [KEYS] in the config: the preset, Custom's base and Custom's own keys. A config
// from before the presets has every key and no preset: what it changed from the
// old defaults becomes Custom over Modern, and the rest moves to Modern.

static bool hkSawPreset;
static bool hkMigrated;
static std::map<std::string, std::string> hkLines;

void hotkeys_load_begin() {
	hkSawPreset = false;
	hkMigrated = false;
	hkLines.clear();
	shortcut_init();
}

void hotkeys_load_line(const std::string& name, const std::string& val) {
	if (name == "preset") {
		int p = hotkeys_preset_id(val.c_str());
		if (p >= 0) hkSet.preset = p;
		hkSawPreset = true;
	} else if (name == "custom.base") {
		int p = hotkeys_preset_id(val.c_str());
		if ((p >= 0) && (p != HKP_CUSTOM)) hkSet.base = p;
	} else {
		hkLines[name] = val;
	}
}

// the other actions that answer to a key where this one works
QList<int> hotkeys_holders(const xHotkeySet& set, int id, const QKeySequence& seq) {
	QList<int> res;
	xShortcut* me = find_shortcut_id(id);
	if (!me || seq.isEmpty()) return res;
	QKeySequence keys[2];
	for (int i = 0; short_tab[i].id >= 0; i++) {
		if ((short_tab[i].id == id) || !(short_tab[i].grp & me->grp)) continue;
		hotkeys_resolve(set, short_tab[i].id, keys);
		if ((keys[0] == seq) || (keys[1] == seq)) res.append(short_tab[i].id);
	}
	return res;
}

// an action's keys in Custom, which keeps only what differs from its base
void hotkeys_put(xHotkeySet& set, int id, const QKeySequence* keys) {
	QKeySequence base[2];
	hotkeys_default(set.base, id, base);
	if ((keys[0] == base[0]) && (keys[1] == base[1])) {
		set.over.erase(id);
	} else {
		set.over[id] = {keys[0], keys[1]};
	}
}

// a key given to an action in Custom, taken from whichever other had it there
void hotkeys_assign(xHotkeySet& set, int id, int slot, const QKeySequence& seq) {
	QKeySequence keys[2];
	for (int other : hotkeys_holders(set, id, seq)) {
		hotkeys_resolve(set, other, keys);
		for (int s = 0; s < 2; s++)
			if (keys[s] == seq) keys[s] = QKeySequence();
		hotkeys_put(set, other, keys);
	}
	hotkeys_resolve(set, id, keys);
	if (keys[slot ^ 1] == seq) keys[slot ^ 1] = QKeySequence();	// not twice on one action
	keys[slot] = seq;
	hotkeys_put(set, id, keys);
}

void hotkeys_load_end() {
	xHotkeySet set = hkSet;
	set.over.clear();
	if (hkSawPreset) {
		for (auto& ln : hkLines) {
			xShortcut* cut = find_shortcut_name(ln.first.c_str());
			if (!cut) continue;
			QList<QKeySequence> lst = QKeySequence::listFromString(QString::fromStdString(ln.second));
			set.over[cut->id] = {lst.value(0), lst.value(1)};
		}
	} else if (!hkLines.empty()) {
		// the user's keys go over Modern and win, the ones left as they were move on
		hkMigrated = true;
		set.preset = HKP_CUSTOM;
		set.base = HKP_MODERN;
		QKeySequence def[2];
		for (auto& ln : hkLines) {
			xShortcut* cut = find_shortcut_name(ln.first.c_str());
			if (!cut) continue;
			QKeySequence seq(QString::fromStdString(ln.second));
			hotkeys_default(HKP_CLASSIC, cut->id, def);
			if (seq != def[0]) hotkeys_assign(set, cut->id, 0, seq);
		}
		if (set.over.empty()) set.preset = HKP_MODERN;
	}
	hotkeys_set(set);
	hkLines.clear();
}

// a config from before the presets was read this run
bool hotkeys_migrated() {
	return hkMigrated;
}

xShortcut* find_shortcut_id(int id) {
	int i = 0;
	while ((short_tab[i].id != id) && (short_tab[i].id >= 0))
		i++;
	return (short_tab[i].id < 0) ? NULL : &short_tab[i];
}

xShortcut* find_shortcut_name(const char* name) {
	int i = 0;
	while ((short_tab[i].id >= 0) && (strcmp(name, short_tab[i].name)))
		i++;
	return (short_tab[i].id < 0) ? NULL : &short_tab[i];
}

static int shortcut_check(int grp, QKeySequence seq) {
	if (seq.isEmpty()) return -1;
	for (int i = 0; short_tab[i].id >= 0; i++) {
		if (!(short_tab[i].grp & grp)) continue;
		if ((short_tab[i].seq[0] == seq) || (short_tab[i].seq[1] == seq))
			return short_tab[i].id;
	}
	return -1;
}

// The key a hotkey is matched by. A key the layout gave a letter of its own to
// (Cyrillic and the like) is taken by where it is, so Alt+M is Alt+M in any
// layout; anything Qt names in Latin-1 or as a special key is left as it is.
int hotkey_key(QKeyEvent* ev) {
	int key = ev->key();
	if ((key < 0x100) || (key >= 0x01000000)) return key;
#if defined(__APPLE__)
	int lat = key2qid(qKey2id(key));
#else
	int lat = key2qid(ev->nativeScanCode());
#endif
	return (lat == Qt::Key_unknown) ? key : lat;
}

// The action a key press means in a group, -1 for none. Only the exact combination
// counts, so Shift+F2 is not F2. A key off the keypad is also tried without it:
// Ins/Home/End come from there with NumLock off, and a Mac marks every arrow so.
int shortcut_find(int grp, int key, Qt::KeyboardModifiers mod) {
	mod = xNativeMods(mod);
	int id = shortcut_check(grp, QKeySequence(key | mod));
	if ((id < 0) && (mod & Qt::KeypadModifier))
		id = shortcut_check(grp, QKeySequence(key | (mod & ~Qt::KeypadModifier)));
	return id;
}

int shortcut_event(int grp, QKeyEvent* ev) {
	return shortcut_find(grp, hotkey_key(ev), ev->modifiers());
}

// the hotkey a key means in the main window, -1 for none; a grabbed keyboard
// goes to the machine whole, but for the key that lets it go
int hotkey_for(int key, Qt::KeyboardModifiers mod, bool kgrab) {
	int id = shortcut_find(SCG_MAIN, key, mod);
	return (kgrab && (id != XCUT_GRABKBD)) ? -1 : id;
}

xShortcut* shortcut_tab() {
	return short_tab;
}

// Qt swaps Ctrl and Cmd on macOS by default (Cmd is reported as Qt::ControlModifier,
// physical Ctrl as Qt::MetaModifier), so a raw ev->modifiers() from a QKeyEvent matches
// the wrong physical key against this table's Ctrl/Meta bindings, and displays the wrong
// name for whatever was actually pressed. Swapping the two bits back here - only where
// this hotkey table reads modifiers, not globally - fixes both without touching Qt's
// native shortcut handling or any other Ctrl-modified feature elsewhere in the app.
Qt::KeyboardModifiers xNativeMods(Qt::KeyboardModifiers mod) {
#ifdef __APPLE__
	Qt::KeyboardModifiers res = mod & ~(Qt::ControlModifier | Qt::MetaModifier);
	if (mod & Qt::ControlModifier) res |= Qt::MetaModifier;
	if (mod & Qt::MetaModifier) res |= Qt::ControlModifier;
	return res;
#else
	return mod;
#endif
}

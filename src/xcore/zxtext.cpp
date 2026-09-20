// Text in the ZX Spectrum's own character set, turned into something a Qt
// widget can show. Codes 0x00..0x1f are control codes, six of them carrying one
// argument byte and two carrying two; 0x80..0x8f are the 2x2 block graphics;
// 0x90..0xa4 are the characters the user defines (on a 128K the last two of
// those are the SPECTRUM and PLAY tokens instead); 0xa5..0xff are the BASIC
// tokens. Three of the ascii codes are not the ascii character.
//
// This is for showing text, never for keeping it: a name goes on to a disk as
// the bytes it is.

#include <QString>

#include "xcore.h"

// 0x80..0x8f, one bit per quadrant: 1 top right, 2 top left, 4 bottom right,
// 8 bottom left
static const ushort zxBlockTab[16] = {
	0x0020, 0x259d, 0x2598, 0x2580,
	0x2597, 0x2590, 0x259a, 0x259c,
	0x2596, 0x259e, 0x258c, 0x259b,
	0x2584, 0x259f, 0x2599, 0x2588
};

// 0xa5..0xff
static const char* zxTokenTab[91] = {
	"RND", "INKEY$", "PI", "FN", "POINT", "SCREEN$", "ATTR", "AT",
	"TAB", "VAL$", "CODE", "VAL", "LEN", "SIN", "COS", "TAN",
	"ASN", "ACS", "ATN", "LN", "EXP", "INT", "SQR", "SGN",
	"ABS", "PEEK", "IN", "USR", "STR$", "CHR$", "NOT", "BIN",
	"OR", "AND", "<=", ">=", "<>", "LINE", "THEN", "TO",
	"STEP", "DEF FN", "CAT", "FORMAT", "MOVE", "ERASE", "OPEN #", "CLOSE #",
	"MERGE", "VERIFY", "BEEP", "CIRCLE", "INK", "PAPER", "FLASH", "BRIGHT",
	"INVERSE", "OVER", "OUT", "LPRINT", "LLIST", "STOP", "READ", "DATA",
	"RESTORE", "NEW", "BORDER", "CONTINUE", "DIM", "REM", "FOR", "GO TO",
	"GO SUB", "INPUT", "LOAD", "LIST", "LET", "PAUSE", "NEXT", "POKE",
	"PRINT", "PLOT", "RUN", "SAVE", "RANDOMIZE", "IF", "CLS", "DRAW",
	"CLEAR", "RETURN", "COPY"
};

// how many bytes a control code takes with it

static int zx_ctl_args(unsigned char ch) {
	switch (ch) {
		case 0x10: case 0x11:		// INK, PAPER
		case 0x12: case 0x13:		// FLASH, BRIGHT
		case 0x14: case 0x15:		// INVERSE, OVER
			return 1;
		case 0x16: case 0x17:		// AT, TAB
			return 2;
	}
	return 0;
}

// Control codes are dropped along with their arguments - they place and colour
// the text rather than being part of it, and a name that keeps them is a name
// that reads as empty. The rest is shown the way the rom prints it, a token
// with a space either side of it.

QString zx_text(const unsigned char* src, int len) {
	QString res;
	int i = 0;
	while (i < len) {
		unsigned char ch = src[i++];
		if (ch < 0x20) {
			i += zx_ctl_args(ch);
		} else if (ch < 0x80) {
			switch (ch) {
				case 0x5e: res.append(QChar(0x2191)); break;	// up arrow
				case 0x60: res.append(QChar(0x00a3)); break;	// pound
				case 0x7f: res.append(QChar(0x00a9)); break;	// copyright
				default: res.append(QChar(ch)); break;
			}
		} else if (ch < 0x90) {
			res.append(QChar(zxBlockTab[ch & 0x0f]));
		} else if (ch < 0xa5) {
			res.append(QChar(0x25a9));			// a character only that machine knows
		} else {
			if (!res.isEmpty() && !res.endsWith(QChar(' ')))
				res.append(QChar(' '));
			res.append(QString(zxTokenTab[ch - 0xa5]));
			res.append(QChar(' '));
		}
	}
	return res.trimmed();
}

#pragma once

// Xpeccy+'s text format for what the Debugger knows about memory: labels, data
// with its statement lengths, comments, ROM lists, breakpoints, sessions. One
// record per line, every line standing alone (docs/debugger-format.md). This
// part only reads a line into a record and writes the pieces of one back; what a
// record means for the machine in use is labels.cpp's and breakpoints.cpp's.

#include <QList>
#include <QString>
#include <QStringList>

// where a record is
enum {
	DT_PLACE_NONE = 0,
	DT_PLACE_PAGE,		// 05:1A00 - a RAM page (decimal) and the offset in it
	DT_PLACE_CPU,		// :8000 - a CPU address
	DT_PLACE_AREA,		// ROM0:0038, DIV3:0010 - a page of a named area of the machine
	DT_PLACE_ROM,		// BASIC48:0556 - an offset in a ROM a rom record names
	DT_PLACE_NAME,		// START - where that label is
	DT_PLACE_PORT		// io:FE/00FF - a port and its mask
};

typedef struct {
	int kind;		// DT_PLACE_*
	int page;		// PAGE: the RAM page; AREA: the area's page
	int area;		// AREA: CELL_*
	int ofs;		// the offset, CPU address or port
	int end;		// a range's last offset, -1: one cell
	int mask;		// PORT: the bits of the port that count
	bool hexPage;		// PAGE: written in hex, as Xpeccy+ did before 2026.8.2
	QString name;		// ROM: the ROM's name; NAME: the label
} dtPlace;

enum {
	DT_REC_NONE = 0,	// a blank line or a comment line
	DT_REC_LABEL,		// WHERE NAME [SIZE]
	DT_REC_DATA,		// db/dw/da/dm/dc WHERE LEN|N*K, code WHERE LEN
	DT_REC_COMMENT,		// comment WHERE text
	DT_REC_ROM,		// rom NAME MD5 [skip X...]
	DT_REC_BRK,		// brk [WHERE] words
	DT_REC_MACHINE,		// machine NAME
	DT_REC_OPEN,		// open PATH
	DT_REC_UNKNOWN,		// a keyword this version does not know: skipped
	DT_REC_BAD		// a known record it cannot read
};

// a word of a brk record; a quoted one ("A==#FF") is one word, spaces and all
typedef struct {
	QString text;
	bool quoted;
} dtWord;

typedef struct {
	int type;		// DT_REC_*
	dtPlace at;
	QString name;		// LABEL: the name; ROM: the ROM's name; MACHINE: the machine
	int size;		// LABEL: its size in bytes, 0: none given
	int view;		// DATA: DBG_VIEW_*
	int unit;		// DATA: bytes an element takes, 1 or 2
	int len;		// DATA: elements a statement has
	int count;		// DATA: statements, one after another
	bool dc;		// DATA: text whose strings end with bit 7 (dc)
	QString text;		// COMMENT: its text, line breaks in it; OPEN: the path
	QString md5;		// ROM: of its 16K bank, lower case
	QStringList skipNames;	// ROM: labels this one has not
	QList<dtPlace> skipPlaces;	// ...and data that starts at these offsets
	QList<dtWord> words;	// BRK: everything after brk, the place included
	QString keyword;	// UNKNOWN: the word that starts the line
	QString error;		// BAD: what is wrong with it
} dtRecord;

void dt_place_clear(dtPlace*);
// one place; withName: a bare word is a label's name (not on a label line)
bool dt_place_parse(const QString&, dtPlace*, bool withName = true);
QString dt_place_text(const dtPlace&);
// a name may be a ROM's: a letter, then letters, digits, _; not an area's token
bool dt_rom_name_ok(const QString&);
// one line into a record; false for DT_REC_BAD (the record says why)
bool dt_parse_line(const QString&, dtRecord*);
// a comment's text as a record keeps it: \n for a line break, \\ for a backslash
QString dt_escape(const QString&);
QString dt_unescape(const QString&);
// a brk word as written: quoted when it has to be
QString dt_word_text(const QString&, bool quote);

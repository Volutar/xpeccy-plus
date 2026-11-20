#include "dbgtext.h"
#include "../libxpeccy/spectrum.h"

// The reading side of the format (docs/debugger-format.md). A line is a label
// when its first word has a colon in it - a place - and a keyword record when it
// has not, so a label called db or CAFE is never taken for anything else.

void dt_place_clear(dtPlace* p) {
	p->kind = DT_PLACE_NONE;
	p->page = 0;
	p->area = 0;
	p->ofs = 0;
	p->end = -1;
	p->mask = 0xffff;
	p->hexPage = false;
	p->name.clear();
}

static bool dt_all(const QString& str, bool hex) {
	if (str.isEmpty()) return false;
	for (QChar ch : str) {
		ushort c = ch.unicode();
		bool dig = (c >= '0') && (c <= '9');
		bool hx = ((c >= 'a') && (c <= 'f')) || ((c >= 'A') && (c <= 'F'));
		if (!dig && !(hex && hx)) return false;
	}
	return true;
}

// plain hex, as LABELSLIST writes its offsets; 0x is let through
static bool dt_hex(QString str, int* val) {
	if (str.startsWith("0x", Qt::CaseInsensitive)) str = str.mid(2);
	if (!dt_all(str, true) || (str.size() > 8)) return false;
	bool ok;
	*val = (int)str.toUInt(&ok, 16);
	return ok;
}

// a length or a count: decimal; #, $ or 0x make it hex
static bool dt_num(const QString& str, int* val) {
	bool ok = false;
	if (str.startsWith('#') || str.startsWith('$')) {
		*val = str.mid(1).toInt(&ok, 16);
		return ok && dt_all(str.mid(1), true);
	}
	if (str.startsWith("0x", Qt::CaseInsensitive)) {
		*val = str.mid(2).toInt(&ok, 16);
		return ok && dt_all(str.mid(2), true);
	}
	*val = str.toInt(&ok, 10);
	return ok && dt_all(str, false);
}

static bool dt_name_char(QChar ch, bool first) {
	ushort c = ch.unicode();
	if (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || (c == '_')) return true;
	return !first && (c >= '0') && (c <= '9');
}

bool dt_rom_name_ok(const QString& str) {
	if (str.isEmpty() || !dt_name_char(str.at(0), true)) return false;
	for (QChar ch : str)
		if (!dt_name_char(ch, false)) return false;
	if (str.compare("io", Qt::CaseInsensitive) == 0) return false;
	if (dt_all(str, true)) return false;		// it would read as a page
	int area, page;
	return !cell_token_parse(str.toLatin1().constData(), &area, &page);
}

bool dt_place_parse(const QString& str, dtPlace* p, bool withName) {
	dt_place_clear(p);
	int colon = str.indexOf(':');
	if (colon < 0) {
		// a label's name: whatever a label line could have given it
		// (a number alone is no name: 9000 is a CPU address that lost its colon)
		if (!withName || str.isEmpty() || str.contains(';') || str.at(0).isDigit()) return false;
		p->kind = DT_PLACE_NAME;
		p->name = str;
		return true;
	}
	QString left = str.left(colon);
	QString right = str.mid(colon + 1);
	int area, page;
	if (left.compare("io", Qt::CaseInsensitive) == 0) {
		// io:FE/00FF - no mask is all 16 bits
		p->kind = DT_PLACE_PORT;
		int slash = right.indexOf('/');
		if (slash >= 0) {
			if (!dt_hex(right.mid(slash + 1), &p->mask)) return false;
			right = right.left(slash);
		}
		return dt_hex(right, &p->ofs) && (p->ofs <= 0xffff) && (p->mask <= 0xffff);
	}
	if (left.isEmpty() || (left.compare("FF", Qt::CaseInsensitive) == 0)) {
		p->kind = DT_PLACE_CPU;		// FF: a CPU address in LABELSLIST of old
	} else if (dt_all(left, false)) {
		p->kind = DT_PLACE_PAGE;
		p->page = left.toInt();
	} else if (cell_token_parse(left.toLatin1().constData(), &area, &page)) {
		p->kind = DT_PLACE_AREA;
		p->area = area;
		p->page = page;
	} else if (dt_all(left, true)) {
		p->kind = DT_PLACE_PAGE;		// a page with hex digits: Xpeccy+'s before 2026.8.2
		p->page = left.toInt(nullptr, 16);
		p->hexPage = true;
	} else if (dt_rom_name_ok(left)) {
		p->kind = DT_PLACE_ROM;
		p->name = left;
	} else {
		return false;
	}
	int dash = right.indexOf('-');
	if (dash >= 0) {
		if (!dt_hex(right.mid(dash + 1), &p->end)) return false;
		right = right.left(dash);
	}
	if (!dt_hex(right, &p->ofs)) return false;
	if ((p->end >= 0) && (p->end < p->ofs)) return false;
	return true;
}

static QString dt_hex4(int val) {
	return QString("%1").arg(val, 4, 16, QChar('0')).toUpper();
}

QString dt_place_text(const dtPlace& p) {
	QString str;
	switch (p.kind) {
		case DT_PLACE_PAGE: str = QString("%1:%2").arg(p.page, 2, 10, QChar('0')).arg(dt_hex4(p.ofs)); break;
		case DT_PLACE_CPU: str = QString(":%1").arg(dt_hex4(p.ofs)); break;
		case DT_PLACE_AREA: {
			xCell cell = {p.area, p.page, p.ofs, 0};
			char buf[16];
			str = QString("%1:%2").arg(cell_token(cell, buf), dt_hex4(p.ofs));
			break;
		}
		case DT_PLACE_ROM: str = QString("%1:%2").arg(p.name, dt_hex4(p.ofs)); break;
		case DT_PLACE_NAME: return p.name;
		case DT_PLACE_PORT:
			str = QString("io:%1").arg(dt_hex4(p.ofs));
			if (p.mask != 0xffff) str.append("/").append(dt_hex4(p.mask));
			return str;
		default: return QString();
	}
	if (p.end >= 0) str.append("-").append(dt_hex4(p.end));
	return str;
}

QString dt_escape(const QString& str) {
	QString res;
	for (QChar ch : str) {
		if (ch == '\\') res.append("\\\\");
		else if (ch == '\n') res.append("\\n");
		else if (ch != '\r') res.append(ch);
	}
	return res;
}

QString dt_unescape(const QString& str) {
	QString res;
	for (int i = 0; i < str.size(); i++) {
		QChar ch = str.at(i);
		if ((ch == '\\') && (i + 1 < str.size())) {
			QChar nx = str.at(i + 1);
			if (nx == 'n') { res.append('\n'); i++; continue; }
			if (nx == '\\') { res.append('\\'); i++; continue; }
		}
		res.append(ch);
	}
	return res;
}

QString dt_word_text(const QString& str, bool quote) {
	if (!quote) return str;
	QString res("\"");
	for (QChar ch : str) {
		if ((ch == '"') || (ch == '\\')) res.append('\\');
		res.append(ch);
	}
	return res.append('"');
}

// the words of a line: blanks between them, a quoted one whole (\" and \\ in it)
static bool dt_words(const QString& line, int from, QList<dtWord>* out) {
	int i = from;
	int n = line.size();
	while (i < n) {
		while ((i < n) && line.at(i).isSpace()) i++;
		if (i >= n) break;
		dtWord wd;
		wd.quoted = (line.at(i) == '"');
		if (wd.quoted) {
			i++;
			bool closed = false;
			while (i < n) {
				QChar ch = line.at(i++);
				if ((ch == '\\') && (i < n)) {
					wd.text.append(line.at(i++));
				} else if (ch == '"') {
					closed = true;
					break;
				} else {
					wd.text.append(ch);
				}
			}
			if (!closed) return false;
		} else {
			while ((i < n) && !line.at(i).isSpace()) wd.text.append(line.at(i++));
		}
		out->append(wd);
	}
	return true;
}

// the first blank-separated word from pos on; pos moves past it
static QString dt_take(const QString& line, int* pos) {
	int i = *pos;
	int n = line.size();
	while ((i < n) && line.at(i).isSpace()) i++;
	int start = i;
	while ((i < n) && !line.at(i).isSpace()) i++;
	*pos = i;
	return line.mid(start, i - start);
}

// what is left of the line after pos, the blanks before it skipped
static QString dt_rest(const QString& line, int pos) {
	int n = line.size();
	while ((pos < n) && line.at(pos).isSpace()) pos++;
	return line.mid(pos);
}

static bool dt_bad(dtRecord* rec, const QString& why) {
	rec->type = DT_REC_BAD;
	rec->error = why;
	return false;
}

static bool dt_data_kind(const QString& kw, dtRecord* rec) {
	rec->unit = 1;
	rec->dc = false;
	if (kw == "db") rec->view = DBG_VIEW_BYTE;
	else if (kw == "dw") { rec->view = DBG_VIEW_WORD; rec->unit = 2; }
	else if (kw == "da") { rec->view = DBG_VIEW_ADDR; rec->unit = 2; }
	else if (kw == "dm") rec->view = DBG_VIEW_TEXT;
	else if (kw == "dc") { rec->view = DBG_VIEW_TEXT; rec->dc = true; }
	else if (kw == "code") rec->view = DBG_VIEW_EXEC;
	else return false;
	return true;
}

bool dt_parse_line(const QString& raw, dtRecord* rec) {
	rec->type = DT_REC_NONE;
	dt_place_clear(&rec->at);
	rec->name.clear();
	rec->size = 0;
	rec->view = 0;
	rec->unit = 1;
	rec->len = 0;
	rec->count = 1;
	rec->dc = false;
	rec->text.clear();
	rec->md5.clear();
	rec->skipNames.clear();
	rec->skipPlaces.clear();
	rec->words.clear();
	rec->keyword.clear();
	rec->error.clear();
	QString line = raw;
	while (!line.isEmpty() && ((line.endsWith('\n')) || line.endsWith('\r'))) line.chop(1);
	QString trim = line.trimmed();
	if (trim.isEmpty() || trim.startsWith(';')) return true;
	int pos = 0;
	QString first = dt_take(line, &pos);
	if (first.contains(':')) {
		// a label: LABELSLIST's line, a size after it if a system variable's
		if (!dt_place_parse(first, &rec->at, false) || (rec->at.kind == DT_PLACE_PORT))
			return dt_bad(rec, "not a place: " + first);
		rec->type = DT_REC_LABEL;
		rec->name = dt_take(line, &pos);
		if (rec->name.isEmpty()) return dt_bad(rec, "a place with no name");
		QString sz = dt_take(line, &pos);
		if (!sz.isEmpty() && !sz.startsWith(';') && (!dt_num(sz, &rec->size) || (rec->size < 0)))
			return dt_bad(rec, "not a size: " + sz);
		return true;
	}
	QString kw = first.toLower();
	if (dt_data_kind(kw, rec)) {
		rec->type = DT_REC_DATA;
		QString where = dt_take(line, &pos);
		QString len = dt_take(line, &pos);
		if (!dt_place_parse(where, &rec->at) || (rec->at.kind == DT_PLACE_PORT) || (rec->at.end >= 0))
			return dt_bad(rec, "not a place: " + where);
		int star = len.indexOf('*');
		if (star >= 0) {
			if (rec->view == DBG_VIEW_EXEC) return dt_bad(rec, "code has no statements to count");
			if (!dt_num(len.left(star), &rec->len) || !dt_num(len.mid(star + 1), &rec->count))
				return dt_bad(rec, "not a length: " + len);
		} else if (!dt_num(len, &rec->len)) {
			return dt_bad(rec, "not a length: " + len);
		}
		if ((rec->len < 1) || (rec->count < 1) || ((long long)rec->len * rec->count * rec->unit > 0x1000000))
			return dt_bad(rec, "a length out of range: " + len);
		return true;
	}
	if (kw == "comment") {
		rec->type = DT_REC_COMMENT;
		QString where = dt_take(line, &pos);
		if (!dt_place_parse(where, &rec->at) || (rec->at.kind == DT_PLACE_PORT) || (rec->at.end >= 0))
			return dt_bad(rec, "not a place: " + where);
		rec->text = dt_unescape(dt_rest(line, pos));
		return true;
	}
	if (kw == "rom") {
		rec->type = DT_REC_ROM;
		rec->name = dt_take(line, &pos);
		rec->md5 = dt_take(line, &pos).toLower();
		if (!dt_rom_name_ok(rec->name)) return dt_bad(rec, "not a ROM's name: " + rec->name);
		if ((rec->md5.size() != 32) || !dt_all(rec->md5, true)) return dt_bad(rec, "not an MD5: " + rec->md5);
		QString wd = dt_take(line, &pos);
		if (wd.isEmpty()) return true;
		if (wd.toLower() != "skip") return dt_bad(rec, "skip expected: " + wd);
		for (wd = dt_take(line, &pos); !wd.isEmpty(); wd = dt_take(line, &pos)) {
			dtPlace pl;
			if (wd.contains(':')) {
				if (!dt_place_parse(wd, &pl, false) || (pl.kind != DT_PLACE_ROM) || (pl.name != rec->name) || (pl.end >= 0))
					return dt_bad(rec, "not a place in this ROM: " + wd);
				rec->skipPlaces.append(pl);
			} else {
				rec->skipNames.append(wd);
			}
		}
		return true;
	}
	if (kw == "brk") {
		rec->type = DT_REC_BRK;
		if (!dt_words(line, pos, &rec->words)) return dt_bad(rec, "a quote not closed");
		return true;
	}
	if (kw == "machine") {
		rec->type = DT_REC_MACHINE;
		rec->name = dt_rest(line, pos).trimmed();
		if (rec->name.isEmpty()) return dt_bad(rec, "no machine named");
		return true;
	}
	if (kw == "open") {
		rec->type = DT_REC_OPEN;
		rec->text = dt_rest(line, pos).trimmed();
		if (rec->text.isEmpty()) return dt_bad(rec, "no file named");
		return true;
	}
	rec->type = DT_REC_UNKNOWN;
	rec->keyword = first;
	return true;
}

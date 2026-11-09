#include "dbg_disasm.h"
#include "dbg_dump.h"
#include "../../xcore/xcore.h"
#include "../../xcore/xexpr.h"
#include "../../libxpeccy/cpu/Z80/z80.h"
#include "../../libxpeccy/filetypes/filetypes.h"

#include <QDebug>
#include <QFont>
#include <QPainter>
#include <QClipboard>
#include <QHeaderView>
#include <QVector>
#include <QApplication>
#include <QFontMetrics>
#include <QRegularExpression>
#include <QToolTip>
#include <QHelpEvent>

extern int blockStart;
extern int blockEnd;
extern int blockAnchor;

static int mode = XVIEW_CPU;
static int page = 0;

static xAdr dasm_xadr(Computer* comp, int adr, int access = MEM_VIEW_PC) {
	if (mode == XVIEW_CPU) return comp_get_xadr(comp, adr, access);
	xAdr xadr;
	xadr.type = (mode == XVIEW_ROM) ? MEM_ROM : MEM_RAM;
	xadr.bank = page << 6;
	xadr.adr = adr & 0x3fff;
	xadr.abs = (page << 14) | xadr.adr;
	return xadr;
}

static int text_width(const QFontMetrics& fm, const QString& str) {
#if QT_VERSION >= QT_VERSION_CHECK(5,11,0)
	return fm.horizontalAdvance(str);
#else
	return fm.width(str);
#endif
}

// a return or a jump ends a logical block, conditional ones too. The list
// covers every cpu core here: z80/i8080/gb, 6502, x86 and 1801vm1

bool is_block_end(const dasmData& drow) {
	static const char* ends[] = {
		"RET","RETI","RETN","RETF","RTS","RTI","IRET","RETURN",
		"JP","JR","JMP","DJNZ","BR","BRA",
		"BCC","BCS","BEQ","BNE","BMI","BPL","BVC","BVS",
		"BGE","BLT","BGT","BLE","BHI","BLS",
		NULL
	};
	if (drow.islab) return false;
	switch (drow.flag & 0xf0) {
		case DBG_VIEW_CODE:
		case DBG_VIEW_EXEC: break;
		default: return false;
	}
	int pos = drow.command.indexOf(QChar(0x20));
	QString mnem = (pos < 0) ? drow.command : drow.command.left(pos);
	for (int i = 0; ends[i]; i++)
		if (mnem == ends[i]) return true;
	return false;
}

bool dasm_ends_block(const QList<dasmData>& list) {
	dasmData drow;
	foreach (drow, list)
		if (is_block_end(drow)) return true;
	return false;
}

// a block end is followed by a blank row, if those are turned on. the one
// place that says so: the listing is built on it and navigation counts on it
bool dasm_has_separator(const QList<dasmData>& list) {
	return conf.dbg.blocksep && dasm_ends_block(list);
}

static dasmData make_separator(int adr) {
	dasmData drow = {};
	drow.issep = 1;
	drow.adr = adr;			// the row below it: page down still lands right
	drow.oadr = -1;
	return drow;
}

// MODEL

xDisasmModel::xDisasmModel(QObject* p):xTableModel(p) {
//	cptr = NULL;
	asmadr = 0;
	row_count = 25;
}

int xDisasmModel::columnCount(const QModelIndex&) const {
	return 4;
}

int xDisasmModel::rowCount(const QModelIndex&) const {
	return row_count;
}

// half way between two colours: dimmed columns, the shaded part of a label
static QColor color_mix(const QColor& one, const QColor& two) {
	return QColor((one.red() + two.red()) / 2, (one.green() + two.green()) / 2, (one.blue() + two.blue()) / 2);
}

// a row's background as painted: the selection, the model's colour or the view's
static QColor row_ground(const QStyleOptionViewItem& opt, const QModelIndex& idx) {
	if (opt.state & QStyle::State_Selected) return opt.palette.color(QPalette::Highlight);
	QVariant bg = idx.data(X_BackgroundRole);
	return bg.isValid() ? qvariant_cast<QBrush>(bg).color() : opt.palette.color(QPalette::Base);
}

// secondary text: halfway between the row colour and the row background,
// so it follows the current style instead of a hardcoded gray, and stays
// readable on the pc/selection rows

QColor xDisasmModel::dim_color(const QColor& fgr, const QColor& rbg) const {
	QWidget* wid = qobject_cast<QWidget*>(parent());
	QColor bgr = rbg;
	if (!bgr.isValid())
		bgr = wid ? wid->palette().color(QPalette::Base) : QColor(Qt::white);
	QColor txt = fgr;
	if (!txt.isValid())
		txt = wid ? wid->palette().color(QPalette::Text) : QColor(Qt::black);
	return color_mix(txt, bgr);
}

QVariant xDisasmModel::data(const QModelIndex& idx, int role) const {
	QVariant res;
//	if (!cptr) return res;
	if (!idx.isValid()) return res;
	int row = idx.row();
	int col = idx.column();
	if (row < 0) return res;
	if (col < 0) return res;
	if (row >= rowCount()) return res;
	if (col >= columnCount()) return res;
	if (row >= dasm.size()) return res;
	QFont font;
	QColor clr;
	QPixmap pxm;
	QPixmap icn;
	QPainter pnt;
	switch(role) {
		case Qt::ToolTipRole:
			// the address as a number, on its own row and on a label's
			if ((col == 0) && !dasm[row].iscom && !dasm[row].issep && conf.dbg.numtips)
				res = dbg_num_tip(dasm[row].adr, true);
			break;
		case Qt::EditRole:
			switch(col) {
				// a decimal address is padded with spaces for the column, not for typing
				case 0: res = dasm[row].iscom ? dasm[row].aname : QString(dasm[row].aname).remove(' '); break;
				case 1: res = dasm[row].bytes; break;
				case 2: res = dasm[row].command; break;
			}
			break;
		case Qt::FontRole:
			if ((col == 0) && dasm[row].islab && !dasm[row].iscom) {
				// start from the debugger font: a default QFont would drop the
				// monospace family, which reads as "not bold" rather than bold
				font = conf.dbg.font;
				font.setBold(true);
				res = font;
			}
			break;
		case Qt::ForegroundRole:
			if (dasm[row].isbrk) {
				clr = conf.pal["dbg.brk.txt"];
			} else if ((col == 0) && dasm[row].islab && dasm[row].iscom) {
				clr = QColor(Qt::gray);
			} else if (dasm[row].ispc && !dasm[row].islab) {
				clr = conf.pal["dbg.pc.txt"];
			} else if (dasm[row].issel) {
				clr = conf.pal["dbg.sel.txt"];
			} else if ((col == 0) && dasm[row].islab) {
				clr = conf.pal["dbg.disk.id.txt"];
			//} else {
			//	clr = conf.pal["dbg.table.txt"];
			}
			// address and opcodes are secondary, breakpoints stay loud
			if (!dasm[row].isbrk &&
				(((col == 0) && !dasm[row].islab && conf.dbg.dimadr) || ((col == 1) && conf.dbg.dimops))) {
				QColor bgr;
				if (dasm[row].ispc && !dasm[row].islab) {
					bgr = conf.pal["dbg.pc.bg"];
				} else if (dasm[row].issel) {
					bgr = conf.pal["dbg.sel.bg"];
				}
				clr = dim_color(clr, bgr);
			}
			if (clr.isValid())
				res = clr;
			break;
		case X_BackgroundRole:
			if (dasm[row].ispc && !dasm[row].islab) {
				clr = conf.pal["dbg.pc.bg"];
			} else if (dasm[row].issel) {
				clr = conf.pal["dbg.sel.bg"];
			} else if ((col == 0) && dasm[row].islab && !dasm[row].iscom) {
				// Shares the disk id colours: that pair is drawn for a tinted
				// run of text on the listing background, it is only ever seen
				// in the track dump, and the user can retune it in the palette
				// editor. Comments keep the plain background.
				clr = conf.pal["dbg.disk.id.bg"];
			//} else {
			//	clr = conf.pal["dbg.table.bg"];
			}
			if (clr.isValid())
				res = clr;
			break;
		case Qt::DecorationRole:
			if ((col == 3) && !dasm[row].icon.isEmpty()) {
				pxm = QPixmap(45,15);
				pxm.fill(Qt::transparent);
				icn.load(dasm[row].icon);
				pnt.begin(&pxm);
				pnt.drawPixmap(pxm.width() - icn.width(),0,icn.scaled(16,16));
				pnt.end();
				res = pxm;
			}
			break;
		case Qt::TextAlignmentRole:
			if (col == 3) res = Qt::AlignLeft;
			break;
		case Qt::DisplayRole:
			switch(col) {
				case 0: res = dasm[row].aname; break;
				case 1: res = dasm[row].bytes; break;
				case 2: res = dasm[row].command; break;
				case 3: res = dasm[row].info; break;
			}
			break;
		case X_LabelRole:
			if (col == 2) res = dasm[row].label;
			break;
		case X_PrefixRole:
			if ((col == 0) && dasm[row].islab && !dasm[row].iscom) res = dasm[row].lpre;
			if (col == 2) res = dasm[row].opre;
			break;
		case Qt::UserRole:
			switch (col) {
				case 0: res = dasm[row].adr; break;
				case 2: res = dasm[row].oadr; break;
			}

			break;
	}
	return res;
}

int dasmrd(int adr, void* ptr) {
	Computer* comp = (Computer*)ptr;
	unsigned char res = 0xff;
	switch (mode) {
		case XVIEW_CPU:
			res = comp_peek(comp, adr, MEM_VIEW_PC, NULL);
			break;
		case XVIEW_RAM:
			res = comp->mem->ramData[((adr & 0x3fff) | (page << 14)) & comp->mem->ramMask];
			break;
		case XVIEW_ROM:
			res = comp->mem->romData[((adr & 0x3fff) | (page << 14)) & comp->mem->romMask];
			break;
	}
	return res;
}

// The listing shows memory as the fetch at PC sees it: TR-DOS (or a divMMC)
// paged in from #3Dxx is there in every row. The row at PC is the instruction
// the CPU will run, with whatever its own fetch pages in (a divMMC trap, the
// Multiface's NMI entry) - stepping runs exactly that row.
static bool dasm_is_pc(Computer* comp, int adr) {
	return (mode == XVIEW_CPU) && ((adr & 0xffff) == cpu_get_pc(comp->cpu));
}

static int dasm_access(Computer* comp, int adr) {
	return dasm_is_pc(comp, adr) ? MEM_VIEW_FETCH : MEM_VIEW_PC;
}

// a character a text row shows: ASCII 32-127
static bool dasm_printable(int bt) {
	return (bt > 31) && (bt < 128);
}

// a row of text, DB "..." or DC "...": its words are not operands
static bool dasm_is_text(const QString& cmd) {
	return cmd.startsWith("DB \"") || cmd.startsWith("DC \"");
}

// a cell's flags where setData() writes them; with no mark of its own, a ROM list's
static unsigned char dasm_flag(Computer* comp, int adr, int access = MEM_VIEW_PC) {
	if (mode != XVIEW_CPU) access = MEM_VIEW_READ;
	unsigned char fl = getBrkView(comp, adr, access);
	if (fl & 0xf0) return fl;
	xAdr xadr = dasm_xadr(comp, adr, access);
	if (xadr.type == MEM_ROM) fl |= labels_rom_view(xadr.abs);
	return fl;
}

static QString dasm_word(int val, int fmt) {
	return dbg_num_in(val & 0xffff, 4, fmt);
}

static QString dasm_byte(int val, int fmt) {
	return dbg_num_in(val & 0xff, 2, fmt);
}

static xMnem dasm_mnem(Computer* comp, int adr, char* buf, int fmt, QByteArray* bytes = NULL) {
	if (!dasm_is_pc(comp, adr)) {
		xMnem mn = cpuDisasmFmt(comp->cpu, adr, buf, dasmrd, comp, fmt);
		if (bytes) {
			bytes->resize(mn.len);
			for (int i = 0; i < mn.len; i++) (*bytes)[i] = dasmrd(adr + i, comp);
		}
		return mn;
	}
	unsigned char code[16];
	xMnem mn = compDisasmFmt(comp, adr, buf, bytes ? code : NULL, sizeof(code), fmt);
	if (bytes) {
		bytes->resize(mn.len);
		if (mn.len > (int)sizeof(code)) {
			compDisasmFmt(comp, adr, buf, (unsigned char*)bytes->data(), bytes->size(), fmt);
		} else {
			memcpy(bytes->data(), code, mn.len);
		}
	}
	return mn;
}

void dasmwr(Computer* comp, int adr, int bt) {
	rzx_rec_touch();
	vid_still_break(comp->vid);
	adr &= comp->mem->busmask;
	switch(mode) {
		case XVIEW_CPU:
			comp_poke(comp, adr, bt, conf.dbg.romwr);
			break;
		case XVIEW_RAM:
			comp->mem->ramData[((adr & 0x3fff) | (page << 14)) & comp->mem->ramMask] = bt & 0xff;
			break;
		case XVIEW_ROM:
			if (conf.dbg.romwr)
				comp->mem->romData[((adr & 0x3fff) | (page << 14)) & comp->mem->romMask] = bt & 0xff;
			break;
	}
}

// a number in a command as a whole word, put in place of: a decimal 1 is not
// the 1 of (IX+1), nor of 10
static void replace_number(QString& cmd, const QString& num, const QString& with) {
	int pos = 0;
	while ((pos = cmd.indexOf(num, pos)) >= 0) {
		QChar before = (pos > 0) ? cmd.at(pos - 1) : QChar(' ');
		int end = pos + num.size();
		QChar after = (end < cmd.size()) ? cmd.at(end) : QChar(' ');
		bool inWord = before.isLetterOrNumber() || (before == '_') || (before == '#') || (before == '$');
		if (!inWord && !after.isLetterOrNumber() && (after != '_')) {
			cmd.replace(pos, num.size(), with);
			pos += with.size();
		} else {
			pos = end;
		}
	}
}

void placeLabel(Computer* comp, dasmData& drow) {
	int shift = 0;
	int work = 1;
	QString lab;
	QString num;
	xMnem mn;
	xAdr xadr;
	bool rom = false;
	while (work && (shift < 64)) {		// as far back as the longest system variable
		xadr = dasm_xadr(comp, drow.oadr - shift);
		lab = find_label(xadr, &rom, LAB_SYSVAR);
		if (lab.isEmpty()) {
			if (drow.oflag & OF_MEMADR) {
				shift++;
			} else {
				work = 0;
			}
		} else {
			// a system variable reaches as far as its size; another label, the
			// instruction it names (or 8 bytes of data)
			int reach = labels_sys_size(xadr);
			if (!reach) {
				mn.len = 8;					// default seek range
				if ((drow.flag & 0xf0) == DBG_VIEW_CODE) {	// for code - size of opcode only
					mn = dasm_mnem(comp, drow.oadr - shift, NULL, drow.fmt);
				}
				reach = qMin(mn.len, 8);
			}
			if (shift < reach) {
				replace_number(drow.command, dasm_word(drow.oadr, drow.fmt),
					shift ? QString("%0 + %1").arg(lab).arg(shift) : lab);
				drow.label = lab;		// the delegate colours it as one word
				if (rom) drow.lrom = (drow.oadr - shift) & 0xffff;
			}
			work = 0;
		}
	}
}

int dasmByte(Computer* comp, int adr, dasmData& drow) {
	int len = 1;
	drow.command = QString("DB %0").arg(dasm_byte(dasmrd(adr, comp), drow.fmt));
	adr++;
	unsigned char fl = dasm_flag(comp, adr);
	xAdr xadr = dasm_xadr(comp, adr);
	while ((len < conf.dbg.dbsize) && (fl == DBG_VIEW_BYTE) && find_label(xadr).isEmpty()) {
		drow.command.append(QString(",%0").arg(dasm_byte(dasmrd(adr, comp), drow.fmt)));
		len++;
		adr++;
		fl = dasm_flag(comp, adr);
		xadr = dasm_xadr(comp, adr);
	}
	return len;
}

int dasmWord(Computer* comp, int adr, dasmData& drow) {
	int len = 2;
	int word = dasmrd(adr, comp);
	word |= (dasmrd(adr + 1, comp) << 8);
	drow.command = QString("DW %0").arg(dasm_word(word, drow.fmt));
	adr += 2;
	unsigned char fl = dasm_flag(comp, adr);
	xAdr xadr = dasm_xadr(comp, adr);
	while ((len < conf.dbg.dwsize * 2) && (fl == DBG_VIEW_WORD) && find_label(xadr).isEmpty()) {
		word = dasmrd(adr, comp);
		word |= (dasmrd(adr + 1, comp) << 8);
		drow.command.append(QString(",%0").arg(dasm_word(word, drow.fmt)));
		adr += 2;
		len += 2;
		fl = dasm_flag(comp, adr);
		xadr = dasm_xadr(comp, adr);
	}
	return len;
}

int dasmAddr(Computer* comp, int adr, dasmData& drow) {
	int len = 2;
	int word = dasmrd(adr, comp);
	word |= (dasmrd(adr + 1, comp) << 8);
	drow.oadr = word;
	drow.command = QString("DW %0").arg(dasm_word(word, drow.fmt));
	placeLabel(comp, drow);
	return len;
}

int dasmText(Computer* comp, int adr, dasmData& drow) {
	int clen = 0;
	drow.command = QString("DB \"");
	unsigned char fl = dasm_flag(comp, adr);
	unsigned char bt = dasmrd(adr, comp);
	while (((fl & 0xf0) == DBG_VIEW_TEXT) && dasm_printable(bt) && (clen < conf.dbg.dmsize)) {
		drow.command.append(QChar(bt));
		clen++;
		bt = dasmrd(adr + clen, comp);
		fl = dasm_flag(comp, adr + clen);
	}
	// a character with bit 7 set ends the text, as the ROMs keep theirs: DC "..."
	// (not #FF: the +3's messages end in it as a byte of its own)
	if (((fl & 0xf0) == DBG_VIEW_TEXT) && (bt & 0x80) && dasm_printable(bt & 0x7f) && (bt != 0xff)) {
		drow.command.append(QChar(bt & 0x7f));
		drow.command.replace(0, 2, "DC");
		clen++;
	}
	if (clen == 0) {		// TODO: non-ascii characters: single db, multiple db untill ascii or wut?
		// shown with the breakpoints on it; nothing written back - getBrk() has
		// the cpu address points in it too, and they are not the cell's
		drow.flag = (dasm_flag(comp, adr) & 0x0f) | DBG_VIEW_BYTE;
		clen = 1;
		drow.command = QString("DB %0").arg(dasm_byte(dasmrd(adr, comp), drow.fmt));
	} else {
		drow.command.append("\"");
	}
	return clen;
}

int dasmCode(Computer* comp, int adr, dasmData& drow) {
	char buf[1024];
	QByteArray bytes;
	xMnem mnm = dasm_mnem(comp, adr, buf, drow.fmt, &bytes);
	for (unsigned char byte : bytes) drow.bytes.append(gethexbyte(byte));
	drow.command = QString(buf).toUpper();
	static const QRegularExpression hexSuffix("(?<![\\w#$])([0-9A-F]+)H(?!\\w)");
	if (drow.fmt == CPU_NUM_H)	// the sign stays small: C9h, not C9H
		drow.command.replace(hexSuffix, "\\1h");
	drow.oadr = mnm.oadr;
	drow.oflag = mnm.flag;
	placeLabel(comp, drow);
	if (drow.ispc) {
		if (mnm.mem) {
			if (mnm.flag & OF_MWORD) {
				drow.info = QString::number(mnm.mop, 16).toUpper().rightJustified(4, '0');
			} else {
				drow.info = QString::number(mnm.mop & 0xff, 16).toUpper().rightJustified(2, '0');
			}
		} else if (mnm.cond && mnm.met && (drow.oadr >= 0)) {
			if (drow.adr < drow.oadr) {
				drow.icon = QString(":/images/arrdn.png");
			} else if (drow.adr > drow.oadr) {
				drow.icon = QString(":/images/arrup.png");
			} else {
				//drow.icon = QString(":/images/redCircle.png");
				drow.icon = QString(":/images/arrleft.png");
			}
		}
	}
	return mnm.len;
}

// A command as an assembler reads it: in the FFFFh form a number that starts
// with a letter gets a 0 in front (C9h is a name to it, 0C9h a number). The
// row's own label and a row of text (DB "...") are left alone
QString dasm_export(const dasmData& drow) {
	if ((drow.fmt != CPU_NUM_H) || ((drow.flag & 0xf0) == DBG_VIEW_TEXT)) return drow.command;
	static const QRegularExpression letterHex("(?<![\\w#$])([A-F][0-9A-F]*h)(?!\\w)");
	QString res = drow.command;
	QList<int> at;
	QRegularExpressionMatchIterator it = letterHex.globalMatch(res);
	while (it.hasNext()) {
		QRegularExpressionMatch m = it.next();
		if (m.captured(1) != drow.label) at.prepend(m.capturedStart(1));
	}
	for (int pos : at) res.insert(pos, '0');		// from the end: earlier places stay
	return res;
}

// adr is bus value
int dasmSome(Computer* comp, int adr, dasmData& drow, int fmt) {
	int clen = 0;
	drow.fmt = (fmt < 0) ? conf.dbg.numfmt : fmt;
	drow.label.clear();
	drow.opre = 0;
	drow.lrom = -1;
	drow.bytes.clear();
	drow.adr = adr;
	drow.flag = dasm_flag(comp, adr, dasm_access(comp, adr));
	drow.oflag = 0;
	drow.oadr = -1;
	switch(drow.flag & 0xf0) {
		case DBG_VIEW_WORD: clen = dasmWord(comp, adr, drow); break;
		case DBG_VIEW_ADDR: clen = dasmAddr(comp, adr, drow); break;
		case DBG_VIEW_TEXT: clen = dasmText(comp, adr, drow); break;
		case DBG_VIEW_CODE:
		case DBG_VIEW_EXEC: clen = dasmCode(comp, adr, drow); break;
		case DBG_VIEW_BYTE: clen = dasmByte(comp, adr, drow); break;
	}
	return clen;
}

// adr must be cpu adr bus (16/24 bits)
QList<dasmData> getDisasm(Computer* comp, int& adr) {
	QList<dasmData> list;
	dasmData drow;
	xAdr xadr;
	int oadr = adr;
	drow.adr = adr;
	drow.fmt = conf.dbg.numfmt;	// the label rows before dasmSome() sets it
	drow.oflag = 0;
	drow.ispc = 0;
	drow.issel = 0;
	drow.islab = 0;
	drow.iscom = 0;
	drow.isequ = 0;
	drow.issep = 0;
	drow.info.clear();
	drow.icon.clear();
	drow.bytes.clear();
	int clen = 0;
//	int wid;
	// 0:adr
	QString lab;
	xadr = comp_get_xadr(comp, cpu_get_pc(comp->cpu), MEM_VIEW_FETCH);
	int abs = xadr.abs;		// remember pc cell
	int pct = xadr.type;
	switch (mode) {
		case XVIEW_RAM:
			xadr.type = MEM_RAM;
			xadr.bank = page << 6;
			xadr.adr = adr & 0x3fff;
			xadr.abs = xadr.adr | (page << 14);
			drow.flag = comp->brkRamMap[xadr.abs & comp->mem->ramMask];
			drow.ispc = ((xadr.type == pct) && (abs == xadr.abs)) ? 1 : 0;
			break;
		case XVIEW_ROM:
			xadr.type = MEM_ROM;
			xadr.bank = page << 6;
			xadr.adr = adr & 0x3fff;
			xadr.abs = xadr.adr | (page << 14);
			drow.flag = comp->brkRomMap[xadr.abs & comp->mem->romMask];
			drow.ispc = ((xadr.type == pct) && (abs == xadr.abs)) ? 1 : 0;
			break;
		default:
			xadr = dasm_xadr(comp, adr, dasm_access(comp, adr));
			drow.flag = getBrkView(comp, xadr.adr, dasm_access(comp, adr));
			drow.ispc = ((xadr.type == pct) && (abs == xadr.abs)) ? 1 : 0;
			drow.issel = ((adr >= blockStart) && (adr <= blockEnd)) ? 1 : 0;
			break;
	}
	drow.isbrk = (drow.flag & MEM_BRK_ANY) ? 1 : 0;

	// comments
	lab = find_comment(xadr);
	if (!lab.isEmpty()) {
		drow.islab = 1;
		drow.iscom = 1;
		drow.aname = lab;
		drow.aname.prepend("; ");
		list.append(drow);
		drow.iscom = 0;
		lab.clear();
	}
	// add label line if any
	if (conf.dbg.labels) {		// if show labels
		lab = find_label(xadr);
	}
	if (!lab.isEmpty()) {		// place label line if it exists
		drow.islab = 1;
		drow.aname = lab;
		drow.lpre = label_local_prefix(lab);
		list.append(drow);
	}
	drow.islab = 0;			// next line is addr
	drow.lpre = 0;
	if (conf.dbg.segment || (mode != XVIEW_CPU)) {
		switch(xadr.type) {
			case MEM_RAM: drow.aname = "RAM:"; break;
			case MEM_ROM: drow.aname = "ROM:"; break;
			case MEM_SLOT: drow.aname = "SLT:"; break;
			case MEM_EXT: drow.aname = "EXT:"; break;
			case MEM_DIV: drow.aname = "DIV:"; break;
			default: drow.aname = "???"; break;
		}
		drow.aname.append(QString("%1:%2").arg(gethexbyte((xadr.bank >> 6) & 0xff)).arg(dbg_adr(xadr.adr & 0x3fff)));
	} else {
		drow.aname = dbg_adr(xadr.adr);
	}
	// 2:command / 3:info
	clen = dasmSome(comp, adr, drow);
	// a local label of the block the row is in: its global part is shaded
	if (!drow.label.isEmpty()) {
		int pre = label_local_prefix(drow.label);
		if (pre && ((labels_ns_len(drow.label) == pre) || (label_block(xadr) == drow.label.left(pre)))) drow.opre = pre;
	}
	// 1:bytes
	if (drow.bytes.isEmpty()) {
		for (int i = 0; i < clen; i++) drow.bytes.append(gethexbyte(dasmrd(adr + i, comp)));
	}
	adr += clen;
	list.append(drow);
// add equ's
	clen = adr - oadr;
	while (clen > 1) {
		oadr++;
		clen--;
		xadr = dasm_xadr(comp, oadr);
		lab = find_label(xadr);
		//lab = findLabel(oadr, -1, -1);
		if (!lab.isEmpty()) {
			drow.islab = 1;
			drow.isequ = 1;
			drow.adr = oadr;
			drow.aname = lab;
			drow.lpre = label_local_prefix(lab);
			drow.command = QString("EQU $-%0").arg(clen);
			list.append(drow);
		}
	}

	return list;
}

int xDisasmModel::fill() {
	dasmData drow;
	QList<dasmData> list;
	int row;
	int adr = asmadr;
	int res = 0;
	dasm.clear();
	for(row = 0; row < rowCount(); row++) {
		adr &= conf.zx->mem->busmask;
		list = getDisasm(conf.zx, adr);
		foreach (drow, list) {
			if (dasm.size() < rowCount()) {
				dasm.append(drow);
				res |= drow.ispc;
			} else {
				row = rowCount();		// it will prevent next iteration
			}
		}
		if (dasm_has_separator(list) && (dasm.size() < rowCount()))
			dasm.append(make_separator(adr));
	}
	return res;
}

int xDisasmModel::update_lst() {
	int res = fill();
	int i;
	Computer* comp = conf.zx;
	int pc = cpu_get_pc(comp->cpu);
	xMnem mnm = dasm_mnem(comp, pc, NULL, conf.dbg.numfmt);
	if (mnm.cond && mnm.met) {
		for (i = 0; i < dasm.size(); i++) {
			if ((dasm[i].adr == mnm.oadr) && (mnm.oadr != pc)) {
				dasm[i].icon = QString(":/images/arrleft.png");
			}
		}
	}
	update();
	return res;
}

Qt::ItemFlags xDisasmModel::flags(const QModelIndex& idx) const {
	Qt::ItemFlags res = QAbstractItemModel::flags(idx);
	if (idx.isValid() && (idx.row() < dasm.size())) {
		if (dasm[idx.row()].issep)
			return Qt::NoItemFlags;
		if ((idx.column() < 3) && !((idx.column() == 2) && dasm[idx.row()].isequ)) {
			res |= Qt::ItemIsEditable;
		}
	}
	return res;
}

int getPrevAdr(Computer* comp, int adr) {
	dasmData drow;
	int i;
	for(i = 16; i > 0; i--) {
		if (dasmSome(comp, adr - i, drow) == i) {
			adr = adr - i;
			break;
		}
	}
	if (i == 0)
		adr--;
	return adr;
}


// convert string val to address (.reg, a number, label)
int asmAddr(Computer* comp, QVariant val, xAdr xadr) {
	QString lab;
	QString str = val.toString();
	int adr;
	// a ROM label's name makes a label of the user's: @name goes to the ROM
	bool romName = !str.startsWith('@') && labels_rom_only(str);
	if (str.startsWith('@')) {			// @label will not create new label, just jump if it exists
		adr = str_to_adr(comp, str.mid(1), true);
	} else if (romName) {
		adr = -1;
	} else {
		adr = str_to_adr(comp, str, true);
	}
	if (adr < 0) {
		if (!romName && (find_label(str).type >= 0)) return -1;
		if ((str.at(0).isLetter() || (str.at(0) == '_')) && !str.contains('$') && !str.contains(' ')) {
			bool rom;
			lab = find_label(xadr, &rom);
			if (!lab.isEmpty() && !rom) {					// remove current label, if the user's
				del_label(lab);
			}
			add_label(xadr, str);						// add new label
		}
	}
	return adr;
}

// first address of a listing that puts 'target' on row 'row', or as close to
// it as the code allows. disassembling backwards is guesswork, so this goes
// forward from every byte in reach, counting rows the way fill() lays them
// out, and keeps the deepest start that leaves the target at or above the row
// asked for. one row lower and the target would be pushed off the listing.
// with 'exact' only starts whose commands land on the target are taken, for
// when the target itself has to be shown (a jump, a typed address). without
// it a start whose last command swallows the target will do, so that paging
// up still moves a whole page where the bytes above do not line up
static int adr_for_row(Computer* comp, int target, int row, int exact) {
	const int cmdmax = 8;		// bytes per row to look back for. generous for the
					// 8-bit cores; where commands run longer the
					// listing just starts nearer the target
	QList<dasmData> list;
	int back = (row + 2) * cmdmax;
	int mask = comp->mem->busmask;
	int start = target;
	int adr, next, len, rows, i;
	if (row < 1) return target;
	QVector<int> dist(back + 1, -1);	// rows from (target - i) to target, -1 = no fit
	dist[0] = 0;
	for (i = 1; i <= back; i++) {
		adr = (target - i) & mask;
		next = adr;
		list = getDisasm(comp, next);		// moves next to the following command
		rows = list.size();
		if (dasm_has_separator(list))
			rows++;
		len = (next - adr) & mask;
		if (len < 1) continue;
		if (len > i) {				// this command swallows the target
			if (exact) continue;
			dist[i] = rows;
		} else if (dist[i - len] >= 0) {
			dist[i] = dist[i - len] + rows;
		} else {
			continue;			// the bytes here do not line up
		}
		if (dist[i] <= row) start = adr;	// deepest start that still fits
	}
	return start;
}

// The assembler reads a number and nothing else: an operand with a label in it is
// worked out here, so label+2, -label and (ix+label) come out as numbers too. The
// brackets and an index register are kept, and so is any operand without a label.
// Empty when an operand cannot be worked out, so nothing is assembled from it.
static bool asm_is_reg(const QString& name) {
	static const QStringList conds = {"nz", "z", "nc", "c", "po", "pe", "p", "m", "af", "hx", "lx", "hy", "ly", "xh", "xl", "yh", "yl"};
	bool err = false;
	cpu_get_reg(conf.zx->cpu, name.toUpper().toLocal8Bit().data(), &err);
	return !err || conds.contains(name.toLower());
}

static bool asm_has_label(const QString& op) {
	int n = op.size();
	for (int i = 0; i < n; ) {
		QChar chr = op.at(i);
		if (chr.isDigit()) {			// a number whole: 0x1F is not a label x1F
			while ((i < n) && lab_char(op.at(i))) i++;
		} else if (lab_char(chr)) {
			int start = i;
			while ((i < n) && lab_char(op.at(i))) i++;
			QString name = op.mid(start, i - start);
			if ((find_label(name).type >= 0) && !asm_is_reg(name)) return true;
		} else {
			i++;
		}
	}
	return false;
}

static QString asm_operand(QString op) {
	op = op.trimmed();
	if (!asm_has_label(op)) return op;
	bool brk = op.startsWith('(') && op.endsWith(')');
	QString in = brk ? op.mid(1, op.size() - 2).trimmed() : op;
	QString idx;
	if (in.startsWith("ix", Qt::CaseInsensitive) || in.startsWith("iy", Qt::CaseInsensitive)) {
		idx = in.left(2);
		in = "0" + in.mid(2);		// the displacement, its sign included
	}
	xExpr exp = xexpr_compile(in.toLocal8Bit().data());
	bool err = false;
	int val = (int)xexpr_eval(exp, conf.zx, &err, 0, 0, XE_CPU_HOME);
	if (err || !xexpr_ok(exp)) return QString();
	QString num = idx.isEmpty() ? QString("0x%0").arg(val & 0xffff, 0, 16)
		: QString("%0%1").arg((val < 0) ? "-" : "+").arg(QString("0x%0").arg(qAbs(val) & 0xff, 0, 16));
	return brk ? QString("(%0%1)").arg(idx).arg(num) : (idx + num);
}

// the full names of the first bare name in an instruction that is in several
// ROMs and none or more than one of them paged in: why it does not assemble
static QStringList asm_ambiguous(const QString& src) {
	int n = src.size();
	for (int i = 0; i < n; ) {
		if (lab_char(src.at(i)) && !src.at(i).isDigit()) {
			int start = i;
			while ((i < n) && lab_char(src.at(i))) i++;
			QStringList amb = labels_ambiguous(src.mid(start, i - start));
			if (!amb.isEmpty()) return amb;
		} else {
			while ((i < n) && lab_char(src.at(i))) i++;
			if ((i < n) && !lab_char(src.at(i))) i++;
		}
	}
	return QStringList();
}

QString asm_labels(const QString& src) {
	QString str = src.trimmed();
	int sp = str.indexOf(' ');
	if (sp < 0) return str;
	QString res = str.left(sp) + " ";
	QStringList ops;
	int depth = 0;
	int from = sp + 1;
	for (int i = from; i <= str.size(); i++) {
		QChar chr = (i < str.size()) ? str.at(i) : QChar(',');
		if (chr == '(') depth++;
		if (chr == ')') depth--;
		if ((chr == ',') && (depth == 0)) {
			QString op = asm_operand(str.mid(from, i - from));
			if (op.isEmpty()) return QString();
			ops.append(op);
			from = i + 1;
		}
	}
	return res + ops.join(",");
}

// the hex the listing shows - #FF, $FF, FFh - as the assembler reads it, 0xFF;
// a label that happens to read as FFh stays a label. A leading zero is taken
// off a decimal number: the assembler would read it as octal
static QString asm_hex_signs(QString str) {
	static const QRegularExpression dollar("\\$(?=[0-9A-Fa-f])");
	static const QRegularExpression zeros("(?<![\\w.])0+(?=[0-9])");
	str.replace(zeros, "");
	str.replace("#", "0x");
	str.replace(dollar, "0x");
	static const QRegularExpression suf("(?<!\\w)([0-9A-Fa-f]+)[hH](?!\\w)");
	QList<QRegularExpressionMatch> found;
	QRegularExpressionMatchIterator it = suf.globalMatch(str);
	while (it.hasNext())
		found.prepend(it.next());		// from the end, so the places hold
	for (const QRegularExpressionMatch& m : found) {
		if (m.captured(0).at(0).isLetter() && (find_label(m.captured(0)).type >= 0)) continue;
		str.replace(m.capturedStart(0), m.capturedLength(0), "0x" + m.captured(1));
	}
	return str;
}

bool xDisasmModel::setData(const QModelIndex& cidx, const QVariant& val, int role) {
	if (!cidx.isValid()) return false;
	if (role != Qt::EditRole) return false;
	int row = cidx.row();
	int col = cidx.column();
	if ((row < 0) || (row >= rowCount())) return false;
	if ((col < 0) || (col >= columnCount())) return false;
	bool flag;
	bool isdc;
	unsigned char cbyte;
	char buf[256];
	unsigned char* ptr;
	int idx = -1;
	int oadr = asmadr;	// disasmAdr;
	int len;
	QString str;
	int adr = dasm[row].adr;
	Computer* comp = conf.zx;
	xAdr xadr = dasm_xadr(comp, adr);
	switch(col) {
		case 0:
			str = val.toString();
			if (str.isEmpty()) {				// empty string = delete label/comment
				if (dasm[row].islab) {			// comment or label
					if (dasm[row].iscom) {		// comment
						del_comment(xadr);
					} else {			// label: the user's, a ROM label stays
						bool rom;
						str = find_label(xadr, &rom);
						if (!str.isEmpty() && !rom) {
							del_label(str);
						}
					}
				}
			} else if (str.startsWith(";")) {		// ; comment
				str.remove(";");
				str = str.trimmed();
				if (str.isEmpty()) {
					del_comment(xadr);
				} else {
					add_comment(xadr, str);
				}
			} else if (str.startsWith(".") || str.contains(":")) {		// .REG || seg:offset
				idx = str_to_adr(comp, str, true);
			} else {							// hex/label
				idx = asmAddr(comp, val, xadr);
			}
			if (idx >= 0) {
				asmadr = adr_for_row(comp, idx, row, 1);
			}
			emit s_adrch(oadr, asmadr);
			break;
		case 1:	// bytes
			str = val.toString();
			while(!str.isEmpty()) {
				cbyte = str.left(2).toInt(&flag,16) & 0xff;
				if (flag)
					dasmwr(comp, adr, cbyte);
				adr++;
				str.remove(0, 2);
			}
			emit rqRefill();
			break;
		case 2:	// command
			ptr = getBrkPtr(comp, adr);
			str = val.toString();
			// dc "text": text whose last character has bit 7 set, as the ROMs end theirs
			isdc = str.startsWith("dc \"", Qt::CaseInsensitive);
			if (!str.startsWith("db \"", Qt::CaseInsensitive) && !isdc)	// #NUM -> 0xNUM if not db "..."
				str = asm_hex_signs(str);
			if (str.startsWith("db ", Qt::CaseInsensitive) || isdc) {	// db
				if ((str.size() > 4) && (str.at(3) == '"') && str.endsWith("\"")) {		// db "text"
					str = str.mid(4, str.size() - 5);
					len = 0;		// the characters it can write
					while ((len < 250) && (len < str.size()) && (str.at(len).unicode() > 31) && (str.at(len).unicode() < 128))
						len++;
					if (isdc && (len < str.size())) len = 0;	// dc: all of it or nothing, its end would be lost
					for (idx = 0; idx < len; idx++) {
						buf[idx] = str.at(idx).unicode();
						// each byte's own flag cell: the next one may be on
						// another page, or (DIV, EXT) have none at all
						ptr = getBrkPtr(comp, (adr + idx) & 0xffff);
						*ptr &= 0x0f;
						*ptr |= DBG_VIEW_TEXT;
					}
					if (isdc && (len > 0)) buf[len - 1] |= 0x80;
				} else if (isdc) {
					len = 0;
				} else {						// db n
					str = str.mid(3);
					idx = str.toInt(&flag, 0);
					if (flag) {
						len = 1;
						buf[0] = idx & 0xff;
						*ptr &= 0x0f;
						*ptr |= DBG_VIEW_BYTE;
					} else {
						len = 0;
					}
				}
			} else if (str.startsWith("dw ", Qt::CaseInsensitive)) {	// word/addr
				str = str.mid(3);
				xadr = label_home(find_label(str));
				if ((xadr.type >= 0) && (xadr.adr >= 0)) {	// check label
					idx = xadr.adr;
					*ptr &= 0x0f;
					*ptr |= DBG_VIEW_ADDR;
					ptr = getBrkPtr(comp, (adr + 1) & 0xffff);
					*ptr &= 0x0f;
					*ptr |= DBG_VIEW_ADDR;
					len = 2;
				} else {
					idx = str.toInt(&flag, 0);
					if (flag) {
						len = 2;
						*ptr &= 0x0f;
						*ptr |= DBG_VIEW_WORD;
						ptr = getBrkPtr(comp, (adr + 1) & 0xffff);
						*ptr &= 0x0f;
						*ptr |= DBG_VIEW_WORD;
					} else {
						len = 0;
						if (!labels_ambiguous(str).isEmpty())		// a name of several ROMs: say which there are
							shitHappens(QString("Which one: %0").arg(labels_ambiguous(str).join(", ")).toLocal8Bit().constData());
					}
				}
				if (len > 0) {
					buf[0] = idx & 0xff;
					buf[1] = (idx >> 8) & 0xff;
				}
			} else {			// code
				len = cpuAsm(comp->cpu, asm_labels(str).toLocal8Bit().data(), buf, adr);
				if (len > 0) {
					for(idx = 0; idx < len; idx++) {
						*ptr &= 0x0f;
						*ptr |= DBG_VIEW_EXEC;
					}
				} else if (!asm_ambiguous(str).isEmpty()) {	// a name of several ROMs: say which there are
					shitHappens(QString("Which one: %0").arg(asm_ambiguous(str).join(", ")).toLocal8Bit().constData());
				}
			}
			idx = 0;
			while (idx < len) {
				dasmwr(comp, adr + idx, buf[idx]);
				idx++;
			}
			emit s_comenter();
			break;
	}
	update_lst();
	return true;
}

// SYNTAX COLOURS

static bool is_word_char(QChar chr) {
	return chr.isLetterOrNumber() || (chr == QChar(0x5f)) || (chr == QChar(0x23)) || (chr == QChar(0x24));
}

// a number as the listing writes it: #FF, $FF, 255, or FFh
static bool is_number_word(const QString& tok) {
	if (tok.isEmpty()) return false;
	QChar chr = tok.at(0);
	if ((chr == QChar(0x23)) || (chr == QChar(0x24)) || chr.isDigit()) return true;
	if ((tok.size() < 2) || !tok.endsWith('h')) return false;	// C9h; most words are not
	for (int i = 0; i < tok.size() - 1; i++)
		if (!is_hex_char(tok.at(i)) || tok.at(i).isLower()) return false;
	return true;
}

// an instruction is typed with the labels offered
QWidget* xDasmSyntax::createEditor(QWidget* par, const QStyleOptionViewItem& opt, const QModelIndex& idx) const {
	QWidget* wid = QStyledItemDelegate::createEditor(par, opt, idx);
	if (QLineEdit* edt = qobject_cast<QLineEdit*>(wid))
		label_complete(edt);
	return wid;
}

xDasmSyntax::xDasmSyntax(QObject* p):QStyledItemDelegate(p) {
}

void xDasmLabel::paint(QPainter* pnt, const QStyleOptionViewItem& opt, const QModelIndex& idx) const {
	pre = idx.data(X_PrefixRole).toInt();
	bgr = row_ground(opt, idx);
	xItemDelegate::paint(pnt, opt, idx);
	pre = 0;
}

// as QItemDelegate lays it out, in two colours
void xDasmLabel::drawDisplay(QPainter* pnt, const QStyleOptionViewItem& opt, const QRect& rect, const QString& text) const {
	if ((pre < 1) || (pre >= text.size())) {
		xItemDelegate::drawDisplay(pnt, opt, rect, text);
		return;
	}
	xItemDelegate::drawDisplay(pnt, opt, rect, QString());		// the selection, if any
	bool sel = opt.state & QStyle::State_Selected;
	QColor txt = opt.palette.color(sel ? QPalette::HighlightedText : QPalette::Text);
	const QWidget* wid = opt.widget;
	QStyle* stl = wid ? wid->style() : QApplication::style();
	int mrg = stl->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, wid) + 1;
	QRect trc = rect.adjusted(mrg, 0, -mrg, 0);
	int flg = Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine;
	QFontMetrics fm(opt.font);
	QString all = fm.elidedText(text, opt.textElideMode, trc.width());
	QString head = all.left(pre);
	pnt->save();
	pnt->setFont(opt.font);
	pnt->setPen(color_mix(txt, bgr));
	pnt->drawText(trc, flg, head);
	pnt->setPen(txt);
	pnt->drawText(trc.adjusted(text_width(fm, head), 0, 0, 0), flg, all.mid(head.size()));
	pnt->restore();
}

void xDasmBytes::drawDisplay(QPainter* pnt, const QStyleOptionViewItem& opt, const QRect& rect, const QString& text) const {
	const QWidget* wid = opt.widget;
	QStyle* stl = wid ? wid->style() : QApplication::style();
	int mrg = stl->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, wid) + 1;
	QFontMetrics fm(opt.font);
	xItemDelegate::drawDisplay(pnt, opt, rect, fm.elidedText(text, Qt::ElideRight, rect.width() - 2 * mrg));
}

void xDasmSyntax::paint(QPainter* pnt, const QStyleOptionViewItem& option, const QModelIndex& idx) const {
	QStyleOptionViewItem opt = option;
	initStyleOption(&opt, idx);
	QString text = opt.text;
	// a db "string" (dc "string") is text, not operands
	if (!conf.dbg.synhl || text.isEmpty() || dasm_is_text(text)) {
		QStyledItemDelegate::paint(pnt, option, idx);
		return;
	}
	QStyle* stl = opt.widget ? opt.widget->style() : QApplication::style();
	opt.text.clear();
	stl->drawControl(QStyle::CE_ItemViewItem, &opt, pnt, opt.widget);
	QRect rect = stl->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
	QColor base = (opt.state & QStyle::State_Selected)
		? opt.palette.color(QPalette::HighlightedText)
		: opt.palette.color(QPalette::Text);
	// the label is taken as it stands: it may hold dots, digits, anything
	// but a space, so picking it out of the text by hand would miss half
	QString lab = idx.data(X_LabelRole).toString();
	int lpos = lab.isEmpty() ? -1 : text.indexOf(lab);
	int lpre = idx.data(X_PrefixRole).toInt();
	if ((lpre < 1) || (lpre >= lab.size())) lpre = 0;
	QColor shade = color_mix(base, row_ground(opt, idx));
	QFont bfnt = opt.font;
	bfnt.setBold(true);
	QFontMetrics fm(opt.font);
	QFontMetrics bfm(bfnt);
	QColor cst = conf.pal.value(DBG_PAL_CONST);
	// hover (the item:hover style rule) and selection (native Highlight)
	// both replace the row background, and the fixed constant colour is
	// not picked to stay readable against either - fall back to bold
	// instead of risking a low-contrast pair
	bool highlighted = opt.state & (QStyle::State_MouseOver | QStyle::State_Selected);
	int x = rect.left();
	int y = rect.top() + (rect.height() + fm.ascent() - fm.descent()) / 2;
	int pos = 0;
	pnt->save();
	while (pos < text.size()) {
		int end = pos;
		bool bold = false;
		QColor clr;
		if (pos == lpos) {			// a label stands out by weight, not by colour
			end = pos + (lpre ? lpre : lab.size());
			bold = true;
			if (lpre) clr = shade;		// the global part of a local label in its own block
		} else if (lpre && (pos == lpos + lpre)) {
			end = lpos + lab.size();
			bold = true;
		} else {
			bool word = is_word_char(text.at(pos));
			while ((end < text.size()) && (end != lpos) && (is_word_char(text.at(end)) == word))
				end++;
			if (word && is_number_word(text.mid(pos, end - pos))) {
				if (highlighted) {
					bold = true;
				} else {
					clr = cst;
				}
			}
		}
		QString tok = text.mid(pos, end - pos);
		pnt->setFont(bold ? bfnt : opt.font);
		pnt->setPen(clr.isValid() ? clr : base);
		pnt->drawText(x, y, tok);
		x += text_width(bold ? bfm : fm, tok);
		pos = end;
	}
	pnt->restore();
}

// The tip of the word under the mouse: a number (an address when it is the
// operand the command goes to or reads), or the label. The words are laid out
// as paint() lays them, bold where it makes them bold, so the one found is the
// one drawn there.
bool xDasmSyntax::helpEvent(QHelpEvent* ev, QAbstractItemView* view, const QStyleOptionViewItem& option, const QModelIndex& idx) {
	if (ev->type() != QEvent::ToolTip)
		return QStyledItemDelegate::helpEvent(ev, view, option, idx);
	if (!conf.dbg.numtips) {
		QToolTip::hideText();
		return true;
	}
	QStyleOptionViewItem opt = option;
	initStyleOption(&opt, idx);
	QString text = opt.text;
	QStyle* stl = opt.widget ? opt.widget->style() : QApplication::style();
	QRect rect = stl->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
	bool syn = conf.dbg.synhl && !dasm_is_text(text);
	QString lab = syn ? idx.data(X_LabelRole).toString() : QString();
	int lpos = lab.isEmpty() ? -1 : text.indexOf(lab);
	QFont bfnt = opt.font;
	bfnt.setBold(true);
	QFontMetrics fm(opt.font);
	QFontMetrics bfm(bfnt);
	bool highlighted = opt.state & (QStyle::State_MouseOver | QStyle::State_Selected);
	int x = rect.left();
	int pos = 0;
	QString tip;
	QRect at;
	while ((pos < text.size()) && tip.isEmpty()) {
		int end = pos;
		bool bold = false;
		bool word = false;
		if (pos == lpos) {
			end = pos + lab.size();
			bold = true;
		} else {
			word = is_word_char(text.at(pos));
			while ((end < text.size()) && (end != lpos) && (is_word_char(text.at(end)) == word))
				end++;
			bold = syn && word && highlighted && is_number_word(text.mid(pos, end - pos));
		}
		QString tok = text.mid(pos, end - pos);
		int w = text_width(bold ? bfm : fm, tok);
		if ((ev->pos().x() >= x) && (ev->pos().x() < x + w)) {
			bool ok = false;
			int val = word ? dbg_num_parse(tok, &ok) : 0;
			if (pos == lpos) {
				xAdr xa = label_home(find_label(lab));
				QString full = find_label(xa, nullptr, LAB_SYSVAR | LAB_FULL);	// BASIC48.LD_BYTES
				if (xa.adr >= 0) tip = dbg_num_tip(xa.adr, true, full.isEmpty() ? lab : full);
			} else if (ok) {
				tip = dbg_num_tip(val, val == idx.data(Qt::UserRole).toInt());
			}
			at = QRect(x, rect.top(), w, rect.height());
		}
		x += w;
		pos = end;
	}
	dbg_show_tip(view->viewport(), at, tip, true);
	return true;
}

// TABLE

xDisasmTable::xDisasmTable(QWidget* p):QTableView(p) {
	for (int i = 0; i < 5; i++) {
		storedAddress[i] = -1;
	}
	dragging = false;
	model = new xDisasmModel(this);
	setModel(model);
	connect(model, SIGNAL(s_comenter()), this, SLOT(rowDown()));
	connect(model, SIGNAL(s_adrch(int, int)), this, SLOT(t_update(int, int)));
	connect(model, SIGNAL(rqRefill()), this, SIGNAL(rqRefill()));		// for updating other widgets
	connect(model, SIGNAL(rqRefill()), this, SLOT(updContent()));		// for updating itself
	connect(this, SIGNAL(colChanged()), this, SLOT(updColors()));
}

void xDisasmTable::resizeEvent(QResizeEvent* ev) {
	int wid = ev->size().width();
	QFontMetrics qfm(font());
	int adw = text_width(qfm, "000:00:00000") + 10;	// RAM:05:#0123: five for the offset in any form
	int btw = text_width(qfm, "0000000000") + 10;
	setColumnWidth(0, adw);
	setColumnWidth(1, btw);
	setColumnWidth(2, wid - adw - btw - 50);
	setColumnWidth(3, 50);

	int h = ev->size().height();
	int rh = verticalHeader()->defaultSectionSize();
	int rc = h / rh;
	// if (h % rh != 0) rc++;
	model->setRows(rc);
	updContent();
}

void xDisasmTable::updColors() {
	conf.pal["dbg.pc.bg"] = pc_bgr;
	conf.pal["dbg.pc.txt"] = pc_txt;
	conf.pal["dbg.sel.bg"] = blk_bgr;
	conf.pal["dbg.sel.txt"] = blk_txt;
	conf.pal["dbg.brk.txt"] = brk_txt;
}

QVariant xDisasmTable::getData(int row, int col, int role) {
	return model->data(model->index(row, col), role);
}

int xDisasmTable::rows() {
	return model->rowCount();
}

void xDisasmTable::setMode(int md, int pg) {
	mode = md;
	page = pg;
	updContent();
}

int xDisasmTable::getAdr() {
	return model->asmadr;
}

void xDisasmTable::setAdr(int adr, int hist) {
	if (hist)
		pushHistory(model->asmadr);
	int oadr = model->asmadr;
	model->asmadr = adr & conf.zx->mem->busmask;
	if (oadr != model->asmadr) {
		updContent();
		emit s_adrch(model->asmadr);
	}
}

// used as slot
void xDisasmTable::setAdrX(int adr) {
	setAdr(adr, 0);
}

int xDisasmTable::updContent() {
	QModelIndex idx;
	int res = model->update_lst();
	clearSpans();
	for (int i = 0; i < model->dasm.size(); i++) {
		if (model->dasm[i].issep) {
			setSpan(i, 0, 1, model->columnCount());
		} else if (model->dasm[i].isequ) {
			setSpan(i, 0, 1, 2);
			setSpan(i, 2, 1, 2);
		} else if (model->dasm[i].islab) {
			setSpan(i, 0, 1, model->columnCount());
		} else if (model->dasm[i].icon.isEmpty() && model->dasm[i].info.isEmpty()) {
			setSpan(i, 2, 1, model->columnCount() - 2);
		}
	}
	// prevent case if current cell is invisible (eated by span)
	idx = currentIndex();
	int r = idx.row();
	if (idx.isValid()) {
		if (model->dasm[r].islab) {
			setCurrentIndex(model->index(r, 0));
		}
	}
	dbg_cell_tip_follow(this);
	return res;
}

void xDisasmTable::update() {
	model->update();
}

void xDisasmTable::t_update(int oadr, int nadr) {
	if (oadr >= 0) {
		pushHistory(oadr);
	}
	updContent();
	if (oadr != nadr)
		emit s_adrch(nadr);
}

void xDisasmTable::rowDown() {
	updContent();
	cursorDown();
	selectionModel()->select(currentIndex(), QItemSelectionModel::ClearAndSelect);
	emit rqRefill();
}

// a row the cursor can stand on: a block separator is not one
bool xDisasmTable::rowEnabled(int row) {
	if ((row < 0) || (row >= model->rowCount())) return false;
	return model->flags(model->index(row, 0)) & Qt::ItemIsEnabled;
}

// the cursor on a row, in the cell a span shows of that column
void xDisasmTable::putCursor(int row, int col) {
	if (row < model->dasm.size()) {
		if (model->dasm[row].islab) {
			col = 0;
		} else if (model->dasm[row].isequ) {
			col = (col < 2) ? 0 : 2;
		} else if ((col > 2) && model->dasm[row].icon.isEmpty() && model->dasm[row].info.isEmpty()) {
			col = 2;
		}
	}
	setCurrentIndex(model->index(row, col));
}

// Down: the next row the cursor can stand on, the listing scrolled when there is
// none below - a block separator on the last row is not the end of the listing
void xDisasmTable::cursorDown() {
	QModelIndex idx = currentIndex();
	int row = idx.isValid() ? idx.row() : 0;
	int col = idx.isValid() ? idx.column() : 0;
	for (int i = 0; i < 8; i++) {
		int r = row + 1;
		while ((r < model->rowCount()) && !rowEnabled(r))
			r++;
		if (r < model->rowCount()) {
			putCursor(r, col);
			return;
		}
		int was = model->asmadr;
		row -= scrolDn();		// the rows the top command took are gone
		if (model->asmadr == was) return;
	}
}

// Up: the same the other way, a separator on the first row included
void xDisasmTable::cursorUp() {
	QModelIndex idx = currentIndex();
	int row = idx.isValid() ? idx.row() : 0;
	int col = idx.isValid() ? idx.column() : 0;
	for (int i = 0; i < 8; i++) {
		int r = row - 1;
		while ((r >= 0) && !rowEnabled(r))
			r--;
		if (r >= 0) {
			putCursor(r, col);
			return;
		}
		int was = model->asmadr;
		scrolUp();
		if (model->asmadr == was) return;
		// the old top row is where the new command's rows end
		int top = rowForAdr(was);
		row = (top < 0) ? 1 : (row + top);
	}
}

void xDisasmTable::copyToCbrd() {
	QList<dasmData> dasm;
	dasmData drow;
	QString str;
	QClipboard* cbrd = QApplication::clipboard();
	int adr = (blockStart < 0) ? model->dasm[currentIndex().row()].adr : blockStart;
	int end = (blockEnd < 0) ? model->dasm[currentIndex().row()].adr : blockEnd;
	int work = 1;
	str = "\tORG 0x" + QString::number(adr, 16).toUpper() + "\n";
	while ((adr <= end) && work) {
		dasm = getDisasm(conf.zx, adr);
		foreach (drow, dasm) {
			if (adr > conf.zx->mem->busmask)
				work = 0;		// address overfill (FFFF+)
			if (drow.isequ) {
				str += drow.aname + ":";
				str += dasm_export(drow);
			} else if (drow.islab) {
				if (drow.iscom) {
					str += drow.aname;
				} else {
					str += drow.aname + ":";
				}
			} else {
				str += "\t" + dasm_export(drow);
			}
			str += "\n";
		}
		if (dasm_has_separator(dasm))
			str += "\n";
	}
	cbrd->setText(str);
}

// records a jump for F5 to undo: 'oldAsmadr' (the viewport anchor before the
// jump) restores the scroll position, and the cursor's own row address (not
// the same thing - the cursor can be anywhere in the visible listing)
// restores which row F5 actually selects
void xDisasmTable::pushHistory(int oldAsmadr) {
	QModelIndex idx = currentIndex();
	int curAdr = idx.isValid() ? model->dasm[idx.row()].adr : oldAsmadr;
	history.append(qMakePair(oldAsmadr, curAdr));
}

// finds the row that holds 'adr', skipping block separators: those carry
// the address of the row below them (see make_separator), so a plain
// row-index match can land on a separator instead of the real instruction
int xDisasmTable::rowForAdr(int adr) {
	for (int row = 0; row < model->dasm.size(); row++) {
		if (!model->dasm[row].issep && (model->dasm[row].adr == adr))
			return row;
	}
	return -1;
}

void xDisasmTable::selectAdr(int adr, int col) {
	int row = rowForAdr(adr);
	if (row >= 0)
		setCurrentIndex(model->index(row, col));
}

// a bookmark: go to it, or put it where the listing is
void xDisasmTable::jumpMarked(int idx, bool go) {
	if ((idx < 0) || (idx > 4)) return;
	if (go) {
		int adr = storedAddress[idx];
		if (adr >= 0) setAdr(adr);
	} else {
		storedAddress[idx] = getAdr();
	}
}

void xDisasmTable::keyPressEvent(QKeyEvent* ev) {
	QModelIndex idx = currentIndex();
	int i;
	int bpt;
	int bpr;
	int adr;
	xAdr xadr;
	Qt::KeyboardModifiers mod = ev->modifiers();
	int key = shortcut_event(SCG_DISASM, ev);
	if ((key < 0) && (shortcut_event(SCG_DEBUGA, ev) >= 0)) {
		ev->ignore();		// the debugger's own key: it goes on to the window
		return;
	}
	if (key < 0)
		key = ev->key();
	Computer* comp = conf.zx;
	int pc = cpu_get_pc(comp->cpu);
	switch (key) {
		case Qt::Key_Up:
			if (mod & Qt::ControlModifier) {
				scrolUp(mod);
			} else {
				cursorUp();
			}
			break;
		case Qt::Key_Down:
			if (mod & Qt::ControlModifier) {
				scrolDn(mod);
			} else {
				cursorDown();
			}
			break;
		case Qt::Key_PageUp:
			// the top row moves to the bottom one, so nothing is stepped over
			model->asmadr = adr_for_row(comp, model->asmadr, rows() - 1, 0);
			updContent();
			emit s_adrch(model->asmadr);
			break;
		case Qt::Key_PageDown:
			// the bottom row becomes the top one. an EQU row is skipped: its
			// address points inside a command, not at the start of one
			i = model->dasm.size() - 1;
			while ((i > 0) && model->dasm[i].isequ)
				i--;
			model->asmadr = model->dasm[i].adr;
			updContent();
			emit s_adrch(model->asmadr);
			break;
		case XCUT_TOPC:
			if (mode != XVIEW_CPU) break;
			if (!comp) break;
			setAdr(pc, 0);
			break;
		case XCUT_SETPC:
			if (mode != XVIEW_CPU) break;
			if (!comp) break;
			i = getData(idx.row(), 0, Qt::UserRole).toInt();
			if ((i >= 0) && (i <= 0xffff)) {
				pc = i;
				cpu_set_pc(comp->cpu, i);
				rzx_rec_touch();
				updContent();
				emit rqRefillAll();
			}
			break;
		case XCUT_MARK1: case XCUT_MARK2: case XCUT_MARK3: case XCUT_MARK4: case XCUT_MARK5:
			jumpMarked(key - XCUT_MARK1, false);
			break;
		case XCUT_GOMARK1: case XCUT_GOMARK2: case XCUT_GOMARK3: case XCUT_GOMARK4: case XCUT_GOMARK5:
			jumpMarked(key - XCUT_GOMARK1, true);
			break;
		case XCUT_SETBRK:
		case XCUT_SETBRK_RD:
		case XCUT_SETBRK_WR:
		case XCUT_SETBRK_ADR:
			adr = getData(idx.row(), 0, Qt::UserRole).toInt();	// bus addr
			if (key == XCUT_SETBRK_ADR) {
				bpr = BRK_CPUADR;
				bpt = 0;
			} else {
				bpr = BRK_MEMCELL;
				// the cell the row shows; a write goes where writes go
				int access = (key == XCUT_SETBRK_WR) ? MEM_VIEW_WRITE : dasm_access(comp, adr);
				xadr = dasm_xadr(comp, adr, access);
				switch(xadr.type) {
					case MEM_RAM: bpt = MEM_BRK_RAM;
						adr = xadr.abs & comp->mem->ramMask;
						break;
					case MEM_ROM: bpt = MEM_BRK_ROM;
						adr = xadr.abs & comp->mem->romMask;
						break;
					case MEM_DIV: bpt = MEM_BRK_DIV;
						adr = xadr.abs % (DIV_EE_SIZE + DIV_RAM_MAX);
						break;
					default: bpt = MEM_BRK_SLT;
						adr = xadr.abs & comp->slot->memMask;
						break;
				}
				// adr = xadr.abs;
			}
			if (key == XCUT_SETBRK_RD) {
				bpt |= MEM_BRK_RD;
			} else if (key == XCUT_SETBRK_WR) {
				bpt |= MEM_BRK_WR;
			} else {
				bpt |= MEM_BRK_FETCH;
			}
			brkXor(bpr, bpt, adr, -1, 1);
			updContent();
			emit rqRefill();
			break;
		case XCUT_JUMPTO:
			if (!idx.isValid()) break;
			adr = model->dasm[idx.row()].oadr;
			if (adr < 0) break;
			model->setData(model->index(idx.row(), 0), QString::number(adr, 16).prepend("0x"), Qt::EditRole);
			updContent();
			selectAdr(adr, idx.column());
			emit s_adrch(model->asmadr);
			break;
		case XCUT_RETFROM: {
			if (history.size() < 1) break;
			QPair<int, int> h = history.takeLast();
			model->asmadr = h.first;
			adr = h.second;
			updContent();
			selectAdr(adr, idx.column());
			emit s_adrch(model->asmadr);
			break;
		}
		case XCUT_GOTOADR:
			// put the cursor on the address of the first row, so the address can be typed at once
			i = 0;
			while ((i < model->dasm.size()) && model->dasm[i].issep)
				i++;
			if (i >= model->dasm.size()) break;
			idx = model->index(i, 0);
			setCurrentIndex(idx);
			edit(idx);
			break;
		case Qt::Key_Return:
			if (state() == QAbstractItemView::EditingState) break;
			edit(currentIndex());
			break;
		default:
			if ((mod & Qt::ControlModifier) && (ev->key() == Qt::Key_C)) {
				copyToCbrd();
			} else {
				QTableView::keyPressEvent(ev);
			}
			break;
	}
}

// the selection over rows, from the first byte of the first to the first byte of
// the last; the DUMP's CPU view shows and sets the same one
void xDisasmTable::blockSet(int a, int b) {
	blockStart = (a < b) ? a : b;
	blockEnd = (a < b) ? b : a;
	updContent();
	emit rqRefill();
}

// As the DUMP: a click puts the cursor and drops the selection, a drag selects
// from the row pressed, Shift+click from there (or the cursor) to the row clicked
void xDisasmTable::mousePressEvent(QMouseEvent* ev) {
	int row = rowAt(ev->pos().y());
	if ((row < 0) || (row >= model->rowCount())) return;
	int adr = getData(row, 0, Qt::UserRole).toInt();
	QModelIndex cur = currentIndex();
	int was = cur.isValid() ? getData(cur.row(), 0, Qt::UserRole).toInt() : adr;
	switch (ev->button()) {
		case Qt::MiddleButton:
			blockStart = -1;
			blockEnd = -1;
			blockAnchor = -1;
			updContent();
			emit rqRefill();
			ev->ignore();
			break;
		case Qt::LeftButton:
			QTableView::mousePressEvent(ev);
			// Ctrl is a plain click: Qt would take the cursor's cell off instead
			if ((ev->modifiers() & Qt::ControlModifier) && indexAt(ev->pos()).isValid())
				selectionModel()->setCurrentIndex(indexAt(ev->pos()), QItemSelectionModel::ClearAndSelect);
			if (mode != XVIEW_CPU) break;
			if (ev->modifiers() & Qt::ShiftModifier) {
				if ((blockStart < 0) || (blockAnchor < 0)) blockAnchor = was;
				blockSet(blockAnchor, adr);
			} else {			// Ctrl too: one way to start a block
				blockAnchor = adr;
				if (blockStart >= 0) {
					blockStart = -1;
					blockEnd = -1;
					updContent();
					emit rqRefill();
				}
			}
			dragging = true;
			break;
		default:
			QTableView::mousePressEvent(ev);
			break;
	}
}

void xDisasmTable::mouseReleaseEvent(QMouseEvent* ev) {
	if (ev->button() == Qt::LeftButton)
		dragging = false;
}

// Qt keeps the current cell selected (and thus filled with the native
// Highlight colour) even once the table loses focus, only greying it out -
// which both clashes with the style and drowns xDasmSyntax::paint's syntax
// colours (it reads QPalette::HighlightedText for the token base colour).
// Drop the selection while unfocused so the row goes back to its normal,
// syntax-highlighted look, and restore it on refocus.

void xDisasmTable::focusOutEvent(QFocusEvent* ev) {
	selectionModel()->clearSelection();
	QTableView::focusOutEvent(ev);
}

void xDisasmTable::focusInEvent(QFocusEvent* ev) {
	QTableView::focusInEvent(ev);
	QModelIndex idx = currentIndex();
	if (idx.isValid())
		selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
}

// the address column's tip over its address, as the command column has it over
// the number pointed at
bool xDisasmTable::viewportEvent(QEvent* ev) {
	if ((ev->type() == QEvent::ToolTip) && (columnAt(static_cast<QHelpEvent*>(ev)->pos().x()) == 0))
		return dbg_cell_tip(this, ev);
	return QTableView::viewportEvent(ev);
}

void xDisasmTable::mouseMoveEvent(QMouseEvent* ev) {
	if (mode != XVIEW_CPU) return;
	int row = rowAt(ev->pos().y());
	if ((row < 0) || (row >= model->rowCount())) return;
	int adr = model->data(model->index(row, 0), Qt::UserRole).toInt();	// item(row,0)->data(Qt::UserRole).toInt();
	if (dragging && (ev->buttons() & Qt::LeftButton) && (blockAnchor >= 0)) {
		// still on the row pressed with nothing selected: a click
		bool same = (blockStart < 0) ? (adr == blockAnchor)
			: ((blockStart == ((adr < blockAnchor) ? adr : blockAnchor)) && (blockEnd == ((adr < blockAnchor) ? blockAnchor : adr)));
		if (!same) blockSet(blockAnchor, adr);
	}
	QTableView::mouseMoveEvent(ev);
}

// the rows the top went up by: those before the new top's own (a separator too);
// Ctrl moves a byte, not a row: 0
int xDisasmTable::scrolDn(Qt::KeyboardModifiers mod) {
	int i = 1;
	int topadr = model->dasm[0].adr;
	int adr;
	int gone = 0;
	dasmData d;
	if (mod & Qt::ControlModifier) {
		adr = model->asmadr + 1;
	} else {
		// TODO: comparing disasmAdr (16 bit) with dasm[i].adr (24 bit)
		while ((i < model->dasm.size()) && (topadr == model->dasm[i].adr))
			i++;
		if (i < model->dasm.size()) {
			adr = model->dasm[i].adr;
			gone = rowForAdr(adr);
			if (gone < 0) gone = i;
		} else {
			adr = model->asmadr + dasmSome(conf.zx, model->asmadr, d);
			gone = model->dasm.size();
			// model->asmadr++;
		}
	}
	setAdr(adr, 0);
//	updContent();
//	emit s_adrch(model->asmadr);
	return gone;
}

void xDisasmTable::scrolUp(Qt::KeyboardModifiers mod) {
	int adr;
	if (mod & Qt::ControlModifier) {
		adr = model->asmadr - 1;
	} else {
		adr = getPrevAdr(conf.zx, model->asmadr);
	}
	setAdr(adr, 0);
//	updContent();
//	emit s_adrch(model->asmadr);
}

void xDisasmTable::wheelEvent(QWheelEvent* ev) {
	if (ev->yDelta < 0) {
		scrolDn(ev->modifiers());
	} else if (ev->yDelta > 0) {
		scrolUp(ev->modifiers());
	}
}

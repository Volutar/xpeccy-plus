#include "dbg_disasm.h"
#include "dbg_dump.h"
#include "../../xcore/xcore.h"
#include "../../libxpeccy/cpu/Z80/z80.h"

#include <QDebug>
#include <QFont>
#include <QPainter>
#include <QClipboard>
#include <QHeaderView>
#include <QVector>
#include <QApplication>
#include <QFontMetrics>

extern int blockStart;
extern int blockEnd;

static int mode = XVIEW_CPU;
static int page = 0;

extern int adr_of_reg(CPU* cpu, bool* flag, QString nam);

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
	return QColor((txt.red() + bgr.red()) / 2, (txt.green() + bgr.green()) / 2, (txt.blue() + bgr.blue()) / 2);
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
		case Qt::EditRole:
			switch(col) {
				case 0: res = dasm[row].aname; break;
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
	int fadr;
	MemPage* pg;
	switch (mode) {
		case XVIEW_CPU:
			pg = mem_get_page(comp->mem, adr);	// = &comp->mem->map[adr >> 8];
			fadr = mem_get_phys_adr(comp->mem, adr);
			switch (pg->type) {
				case MEM_ROM: res = comp->mem->romData[fadr & comp->mem->romMask]; break;
				case MEM_RAM: res = comp->mem->ramData[fadr & comp->mem->ramMask]; break;
				case MEM_SLOT: res = memRd(comp->mem, adr);
					break;
			}
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

void dasmwr(Computer* comp, int adr, int bt) {
	adr &= comp->mem->busmask;
	int fadr;
	MemPage* pg;
	switch(mode) {
		case XVIEW_CPU:
			pg = mem_get_page(comp->mem, adr);		// = &comp->mem->map[(adr >> 8) & 0xff];
			fadr = mem_get_phys_adr(comp->mem, adr);	// pg->num << 8) | (adr & 0xff);
			switch (pg->type) {
				// no writings to slot
				case MEM_ROM:
					if (conf.dbg.romwr)
						comp->mem->romData[fadr & comp->mem->romMask] = bt & 0xff;
					break;
				case MEM_RAM:
					comp->mem->ramData[fadr & comp->mem->ramMask] = bt & 0xff;
					break;
			}
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

void placeLabel(Computer* comp, dasmData& drow) {
	int shift = 0;
	int work = 1;
	QString lab;
	QString num;
	xMnem mn;
	xAdr xadr;
	while (work && (shift < 8)) {
		xadr = mem_get_xadr(comp->mem, drow.oadr - shift);
		lab = find_label(xadr);
		if (lab.isEmpty()) {
			if (drow.oflag & OF_MEMADR) {
				shift++;
			} else {
				work = 0;
			}
		} else {
			mn.len = 8;					// default seek range
			if ((drow.flag & 0xf0) == DBG_VIEW_CODE) {	// for code - size of opcode only
				mn = cpuDisasm(comp->cpu, (drow.oadr - shift), NULL, dasmrd, comp);
			}
			if (shift < mn.len) {
				num = gethexword(drow.oadr).prepend("#").toUpper();
				if (shift == 0) {
					drow.command.replace(num, QString("%0").arg(lab));
				} else {
					drow.command.replace(num, QString("%0 + %1").arg(lab).arg(shift));
				}
				drow.label = lab;		// the delegate colours it as one word
			}
			work = 0;
		}
	}
}

int dasmByte(Computer* comp, int adr, dasmData& drow) {
	int len = 1;
	drow.command = QString("DB #%0").arg(gethexbyte(dasmrd(adr, comp)));
	adr++;
	unsigned char fl = getBrk(comp, adr);
	xAdr xadr = mem_get_xadr(comp->mem, adr);
	while ((len < conf.dbg.dbsize) && (fl == DBG_VIEW_BYTE) && find_label(xadr).isEmpty()) {
		drow.command.append(QString(",#%0").arg(gethexbyte(dasmrd(adr, comp))));
		len++;
		adr++;
		fl = getBrk(comp, adr);
		xadr = mem_get_xadr(comp->mem, adr);
	}
	return len;
}

int dasmWord(Computer* comp, int adr, dasmData& drow) {
	int len = 2;
	int word = dasmrd(adr, comp);
	word |= (dasmrd(adr + 1, comp) << 8);
	drow.command = QString("DW #%0").arg(gethexword(word));
	adr += 2;
	unsigned char fl = getBrk(comp, adr);
	xAdr xadr = mem_get_xadr(comp->mem, adr);
	while ((len < conf.dbg.dwsize * 2) && (fl == DBG_VIEW_WORD) && find_label(xadr).isEmpty()) {
		word = dasmrd(adr, comp);
		word |= (dasmrd(adr + 1, comp) << 8);
		drow.command.append(QString(",#%0").arg(gethexword(word)));
		adr += 2;
		len += 2;
		fl = getBrk(comp, adr);
		xadr = mem_get_xadr(comp->mem, adr);
	}
	return len;
}

int dasmAddr(Computer* comp, int adr, dasmData& drow) {
	int len = 2;
	int word = dasmrd(adr, comp);
	word |= (dasmrd(adr + 1, comp) << 8);
	drow.oadr = word;
	drow.command = QString("DW #%0").arg(gethexword(word));
	placeLabel(comp, drow);
	return len;
}

int dasmText(Computer* comp, int adr, dasmData& drow) {
	int clen = 0;
	drow.command = QString("DB \"");
	unsigned char fl = getBrk(comp, adr);
	unsigned char bt = dasmrd(adr, comp);
	while (((fl & 0xf0) == DBG_VIEW_TEXT) && (bt > 31) && (bt < 128) && (clen < conf.dbg.dmsize)) {
		drow.command.append(QChar(bt));
		clen++;
		bt = dasmrd(adr + clen, comp);
		fl = getBrk(comp, adr + clen);
	}
	if (clen == 0) {		// TODO: non-ascii characters: single db, multiple db untill ascii or wut?
		drow.flag = (getBrk(comp, adr) & 0x0f) | DBG_VIEW_BYTE;
		setBrk(comp, adr, drow.flag);
		clen = 1;
		drow.command = QString("DB #%0").arg(dasmrd(adr, comp));
	} else {
		drow.command.append("\"");
	}
	return clen;
}

int dasmCode(Computer* comp, int adr, dasmData& drow) {
	char buf[1024];
	xMnem mnm = cpuDisasm(comp->cpu, adr, buf, dasmrd, comp);
	drow.command = QString(buf).toUpper();
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

// adr is bus value
int dasmSome(Computer* comp, int adr, dasmData& drow) {
	int clen = 0;
	drow.label.clear();
	drow.adr = adr;
	drow.flag = getBrk(comp, adr);
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
	drow.oflag = 0;
	drow.ispc = 0;
	drow.issel = 0;
	drow.islab = 0;
	drow.iscom = 0;
	drow.isequ = 0;
	drow.issep = 0;
	drow.info.clear();
	drow.icon.clear();
	int clen = 0;
//	int wid;
	// 0:adr
	QString lab;
	xadr = mem_get_xadr(comp->mem, cpu_get_pc(comp->cpu));
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
			xadr = mem_get_xadr(comp->mem, adr);
			drow.flag = getBrk(comp, xadr.adr);
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
		list.append(drow);
	}
	drow.islab = 0;			// next line is addr
	if (conf.dbg.segment || (mode != XVIEW_CPU)) {
		switch(xadr.type) {
			case MEM_RAM: drow.aname = "RAM:"; break;
			case MEM_ROM: drow.aname = "ROM:"; break;
			case MEM_SLOT: drow.aname = "SLT:"; break;
			case MEM_EXT: drow.aname = "EXT:"; break;
			default: drow.aname = "???"; break;
		}
		drow.aname.append(QString("%1:%2").arg(gethexbyte((xadr.bank >> 6) & 0xff)).arg(gethexword(xadr.adr & 0x3fff)));
	} else {
		drow.aname = gethexword(xadr.adr);
	}
	// 2:command / 3:info
	clen = dasmSome(comp, adr, drow);
	// 1:bytes
	drow.bytes.clear();
	while(clen > 0) {
		drow.bytes.append(gethexbyte(dasmrd(adr, comp)));
		adr++;
		clen--;
	}
	list.append(drow);
// add equ's
	clen = adr - oadr;
	while (clen > 1) {
		oadr++;
		clen--;
		xadr = mem_get_xadr(comp->mem, oadr);
		lab = find_label(xadr);
		//lab = findLabel(oadr, -1, -1);
		if (!lab.isEmpty()) {
			drow.islab = 1;
			drow.isequ = 1;
			drow.adr = oadr;
			drow.aname = lab;
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
	xMnem mnm = cpuDisasm(comp->cpu, pc, NULL, dasmrd, conf.zx);
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

int adr_of_reg(CPU* cpu, bool* flag, QString nam) {
	xRegBunch bch = cpuGetRegs(cpu);
	int i = 0;
	int res;
	nam = nam.toUpper();
	while ((bch.regs[i].id != REG_EOT) && (QString(bch.regs[i].name).toUpper() != nam))
		i++;
	if (bch.regs[i].id == REG_EOT) {
		*flag = false;
		res = 0;
	} else {
		*flag = true;
		res = bch.regs[i].value;
	}
	return res;
}

int str_to_adr(Computer* comp, QString str) {
	bool flag = true;
	int adr = -1;
	QStringList lst;
	int off;
	int seg;
	if (str.startsWith(".")) {			// .REG : name of CPU register
		adr = adr_of_reg(comp->cpu, &flag, str.mid(1));
	} else if (str.contains(":")) {			// seg:adr (for real mode)
		lst = str.split(":",X_SkipEmptyParts);
		if (lst.size() == 2) {
			off = lst[1].toInt(&flag,16);
			if (!flag) {
				off = adr_of_reg(comp->cpu, &flag, lst[1]);
			}
			if (flag) {
				seg = lst[0].toInt(&flag,16);
				if (flag) {
					adr = (seg << 4) + off;
				} else {
					seg = adr_of_reg(comp->cpu, &flag, lst[0]);
					if (flag) {
						adr = (seg << 4) + off;
					}
				}
			}
		}
	} else if (str.startsWith("#")) {			// #nnnn : hex adr
		str.replace(0, 1, "0x");
		adr = str.toInt(&flag, 16);
	} else if (str.startsWith("0x")) {			// 0xnnnn : hex adr
		adr = str.toInt(&flag, 16);
	} else {
		xAdr xadr = find_label(str);			// NAME : label name
		if (xadr.type >= 0) {
			adr = xadr.adr;
			flag = true;
		} else {						// other : plain hex
			adr = str.toInt(&flag, 16);
		}
	}
	if (!flag) adr = -1;
	return adr;
}

// convert string val to address (.reg, #HEXA, 0xHEXA, HEXA, label)
int asmAddr(Computer* comp, QVariant val, xAdr xadr) {
	QString lab;
	QString str = val.toString();
	int adr;
	if (str.startsWith('@')) {			// @label will not create new label, just jump if it exists
		adr = str_to_adr(comp, str.mid(1));
	} else {
		adr = str_to_adr(comp, str);
	}
	if (adr < 0) {
		if ((str.at(0).isLetter() || (str.at(0) == '_')) && !str.contains('$') && !str.contains(' ')) {
			lab = find_label(xadr);
			if (!lab.isEmpty()) {						// remove current label
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

bool xDisasmModel::setData(const QModelIndex& cidx, const QVariant& val, int role) {
	if (!cidx.isValid()) return false;
	if (role != Qt::EditRole) return false;
	int row = cidx.row();
	int col = cidx.column();
	if ((row < 0) || (row >= rowCount())) return false;
	if ((col < 0) || (col >= columnCount())) return false;
	bool flag;
	unsigned char cbyte;
	char buf[256];
	unsigned char* ptr;
	int idx = -1;
	int oadr = asmadr;	// disasmAdr;
	int len;
	QString str;
	int adr = dasm[row].adr;
	Computer* comp = conf.zx;
	xAdr xadr = mem_get_xadr(comp->mem, adr);
	switch(col) {
		case 0:
			str = val.toString();
			if (str.isEmpty()) {				// empty string = delete label/comment
				if (dasm[row].islab) {			// comment or label
					if (dasm[row].iscom) {		// comment
						del_comment(xadr);
					} else {			// label
						str = find_label(xadr);
						if (!str.isEmpty()) {
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
				idx = str_to_adr(comp, str);
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
			if (!str.startsWith("db \"", Qt::CaseInsensitive))		// #NUM -> 0xNUM if not db "..."
				str.replace("#", "0x");
			if (str.startsWith("db ", Qt::CaseInsensitive)) {		// db
				if ((str.at(3) == '"') && str.endsWith("\"")) {		// db "text"
					str = str.mid(4, str.size() - 5);
					idx = 0;
					cbyte = str.at(idx).cell();
					while ((idx < 250) && (idx < str.size()) && (cbyte > 31) && (cbyte < 128)) {
						buf[idx] = cbyte;
						*ptr &= 0x0f;
						*ptr |= DBG_VIEW_TEXT;
						ptr++;
						idx++;
						cbyte = str.at(idx).cell();
					}
					len = idx;
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
				xadr = find_label(str);
				if (xadr.type >= 0) {				// check label
					idx = xadr.adr;
					*ptr &= 0x0f;
					*ptr |= DBG_VIEW_ADDR;
					ptr++;
					*ptr &= 0x0f;
					*ptr |= DBG_VIEW_ADDR;
					len = 2;
				} else {
					idx = str.toInt(&flag, 0);
					if (flag) {
						len = 2;
						*ptr &= 0x0f;
						*ptr |= DBG_VIEW_WORD;
						ptr++;
						*ptr &= 0x0f;
						*ptr |= DBG_VIEW_WORD;
					} else {
						len = 0;
					}
				}
				if (len > 0) {
					buf[0] = idx & 0xff;
					buf[1] = (idx >> 8) & 0xff;
				}
			} else {			// code
				// TODO: replace label name
				len = cpuAsm(comp->cpu, str.toLocal8Bit().data(), buf, adr);
				if (len > 0) {
					for(idx = 0; idx < len; idx++) {
						*ptr &= 0x0f;
						*ptr |= DBG_VIEW_EXEC;
					}
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

xDasmSyntax::xDasmSyntax(QObject* p):QStyledItemDelegate(p) {
}

void xDasmSyntax::paint(QPainter* pnt, const QStyleOptionViewItem& option, const QModelIndex& idx) const {
	QStyleOptionViewItem opt = option;
	initStyleOption(&opt, idx);
	QString text = opt.text;
	// a db "string" is text, not operands
	if (!conf.dbg.synhl || text.isEmpty() || text.startsWith("DB \"")) {
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
			end = pos + lab.size();
			bold = true;
		} else {
			bool word = is_word_char(text.at(pos));
			while ((end < text.size()) && (end != lpos) && (is_word_char(text.at(end)) == word))
				end++;
			QChar chr = text.at(pos);
			if (word && ((chr == QChar(0x23)) || (chr == QChar(0x24)) || chr.isDigit())) {
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

// TABLE

xDisasmTable::xDisasmTable(QWidget* p):QTableView(p) {
	for (int i = 0; i < 5; i++) {
		storedAddress[i] = -1;
	}
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
	int adw = text_width(qfm, "000:00:0000") + 10;
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
	QModelIndex idx = currentIndex();
	int row = idx.row();
	int col = idx.column();
	if (row < model->rowCount() - 1) {
		idx = model->index(row + 1, col);
		setCurrentIndex(idx);
		selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
	} else {
		scrolDn();
	}
	emit rqRefill();
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
				str += drow.command;
			} else if (drow.islab) {
				if (drow.iscom) {
					str += drow.aname;
				} else {
					str += drow.aname + ":";
				}
			} else {
				str += "\t" + drow.command;
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

void xDisasmTable::jumpMarked(int idx, Qt::KeyboardModifiers mod) {
	if ((idx < 0) || (idx > 4)) return;
	if (mod & Qt::AltModifier) {
		int adr = storedAddress[idx];
		if (adr >= 0) setAdr(adr);
	} else if (mod & Qt::ControlModifier) {
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
	int key = shortcut_check(SCG_DISASM, QKeySequence(ev->key() | mod));
	if (key < 0)
		key = shortcut_check(SCG_DISASM, QKeySequence(ev->key()));
	if (key < 0)
		key = ev->key();
	Computer* comp = conf.zx;
	int pc = cpu_get_pc(comp->cpu);
	switch (key) {
		case Qt::Key_Up:
			if ((mod & Qt::ControlModifier) || (idx.row() == 0)) {
				scrolUp(mod);
			} else {
				QTableView::keyPressEvent(ev);
			}
			break;
		case Qt::Key_Down:
			if ((mod & Qt::ControlModifier) || (idx.row() == model->rowCount() - 1)) {
				scrolDn(mod);
			} else {
				QTableView::keyPressEvent(ev);
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
				updContent();
				emit rqRefillAll();
			}
			break;
		case XCUT_SAVE:
			ev->ignore();		// send it to debuga
			break;
		case XCUT_SETBRK:
			adr = getData(idx.row(), 0, Qt::UserRole).toInt();	// bus addr
#ifndef __WIN32__
			if (mod & Qt::ShiftModifier) {
#else
			if ((mod & Qt::ShiftModifier) &&
				!(mod & Qt::ControlModifier) &&
				!(mod & Qt::AltModifier)) {
#endif
				bpr = BRK_CPUADR;
				bpt = 0;
			} else {
				bpr = BRK_MEMCELL;
				xadr = mem_get_xadr(comp->mem, adr);
				switch(xadr.type) {
					case MEM_RAM: bpt = MEM_BRK_RAM;
						adr = xadr.abs & comp->mem->ramMask;
						break;
					case MEM_ROM: bpt = MEM_BRK_ROM;
						adr = xadr.abs & comp->mem->romMask;
						break;
					default: bpt = MEM_BRK_SLT;
						adr = xadr.abs & comp->slot->memMask;
						break;
				}
				// adr = xadr.abs;
			}
			// modifiers doesn't work with new hotkeys (hotkey not detected)
			if (mod & Qt::AltModifier) {
				bpt |= MEM_BRK_RD;
			} else if (mod & Qt::ControlModifier) {
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
			if (mod & Qt::ControlModifier) {
				switch(ev->key()) {
					case Qt::Key_C: copyToCbrd(); break;
					case Qt::Key_1: jumpMarked(0, mod); break;
					case Qt::Key_2: jumpMarked(1, mod); break;
					case Qt::Key_3: jumpMarked(2, mod); break;
					case Qt::Key_4: jumpMarked(3, mod); break;
					case Qt::Key_5: jumpMarked(4, mod); break;
					default: QTableView::keyPressEvent(ev); break;
				}
			} else if (mod & Qt::AltModifier) {
				switch(ev->key()) {
					case Qt::Key_1: jumpMarked(0, mod); break;
					case Qt::Key_2: jumpMarked(1, mod); break;
					case Qt::Key_3: jumpMarked(2, mod); break;
					case Qt::Key_4: jumpMarked(3, mod); break;
					case Qt::Key_5: jumpMarked(4, mod); break;
					default: QTableView::keyPressEvent(ev); break;
				}
			} else {
				QTableView::keyPressEvent(ev);
			}
			break;
	}
}

void xDisasmTable::mousePressEvent(QMouseEvent* ev) {
	int row = rowAt(ev->pos().y());
	if ((row < 0) || (row >= model->rowCount())) return;
	int adr = getData(row, 0, Qt::UserRole).toInt();
	switch (ev->button()) {
		case Qt::MiddleButton:
			blockStart = -1;
			blockEnd = -1;
			updContent();
			emit rqRefill();
			ev->ignore();
			break;
		case Qt::LeftButton:
			if (mode != XVIEW_CPU) {
				QTableView::mousePressEvent(ev);
			} else if (ev->modifiers() & Qt::ControlModifier) {
				blockStart = adr;
				if (blockEnd < blockStart) blockEnd = blockStart;
				updContent();
				emit rqRefill();
				ev->ignore();
			} else if (ev->modifiers() & Qt::ShiftModifier) {
				blockEnd = adr;
				if (blockStart > blockEnd) blockStart = blockEnd;
				if (blockStart < 0) blockStart = 0;
				updContent();
				emit rqRefill();
				ev->ignore();
			} else {
				markAdr = adr;
				QTableView::mousePressEvent(ev);
			}
			break;
		default:
			QTableView::mousePressEvent(ev);
			break;
	}
}

void xDisasmTable::mouseReleaseEvent(QMouseEvent* ev) {
	if (ev->button() == Qt::LeftButton)
		markAdr = -1;
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

void xDisasmTable::mouseMoveEvent(QMouseEvent* ev) {
	if (mode != XVIEW_CPU) return;
	int row = rowAt(ev->pos().y());
	if ((row < 0) || (row >= model->rowCount())) return;
	int adr = model->data(model->index(row, 0), Qt::UserRole).toInt();	// item(row,0)->data(Qt::UserRole).toInt();
	if ((ev->modifiers() == Qt::NoModifier) && (ev->buttons() & Qt::LeftButton) && (adr != blockStart) && (adr != blockEnd) && (markAdr >= 0)) {
		if (adr < blockStart) {
			blockStart = adr;
			blockEnd = markAdr;
		} else {
			blockStart = markAdr;
			blockEnd = adr;
		}
		updContent();
		emit rqRefill();
	}
	QTableView::mouseMoveEvent(ev);
}

void xDisasmTable::scrolDn(Qt::KeyboardModifiers mod) {
	int i = 1;
	int topadr = model->dasm[0].adr;
	int adr;
	dasmData d;
	if (mod & Qt::ControlModifier) {
		adr = model->asmadr + 1;
	} else {
		// TODO: comparing disasmAdr (16 bit) with dasm[i].adr (24 bit)
		while ((topadr == model->dasm[i].adr) && (i < model->dasm.size()))
			i++;
		if (i < model->dasm.size()) {
			adr = model->dasm[i].adr;
		} else {
			adr = model->asmadr + dasmSome(conf.zx, model->asmadr, d);
			// model->asmadr++;
		}
	}
	setAdr(adr, 0);
//	updContent();
//	emit s_adrch(model->asmadr);
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

#include <QFile>
#include <QStringList>

#include "xcore.h"

typedef struct {
	int idx;
	xBrkPoint* ptr;
} xbpIndex;

xbpIndex brkFind(xBrkPoint* brk, int flag = 0) {
	xbpIndex res;
	res.ptr = NULL;
	res.idx = -1;
	xBrkPoint* tbrk;
	std::vector<xBrkPoint>* list = (flag & BRKF_SYSTEM) ? &conf.brk.list_sys : &conf.brk.list;
	int i;
	int max = list->size();
	for (i = 0; (i < max) && !res.ptr; i++) {
		tbrk = &list->at(i);
		if ((tbrk->type == brk->type) && (tbrk->adr == brk->adr)) {
			if (brk->type == BRK_IOPORT) {
				if (tbrk->mask == brk->mask) {
					res.ptr = tbrk;
					res.idx = i;
				}
			} else if (brk->type == BRK_COND) {
				if (tbrk->cond == brk->cond) {		// global conditions differ by text only
					res.ptr = tbrk;
					res.idx = i;
				}
			} else if (brk->type == BRK_CPUADR) {
				if (tbrk->eadr == brk->eadr) {
					res.ptr = tbrk;
					res.idx = i;
				}
			} else {
				res.ptr = tbrk;
				res.idx = i;
			}
		}
//		if (res.ptr) break;
	}
	return res;
}

xBrkPoint* brk_find(int t, int adr) {
	xBrkPoint* ptr = NULL;
	if (t == BRK_IRQ) {
		for (auto it = conf.brk.list.begin(); it != conf.brk.list.end(); it++) {
			if (!it->off && (it->type == t)) {
				ptr = &(*it);
			}
		}
	} else {
		std::map<int, xBrkPoint*>* map = NULL;
		if (conf.brk.map.count(t) > 0) {
			map = &conf.brk.map[t];
		}
		if (map) {
			std::map<int, xBrkPoint*>::iterator it;
			it = map->find(adr);
			if (it != map->end()) {
				ptr = map->at(adr);
			}
		}
	}
	return ptr;
}

// create breakpoint
// type		BRK_MEMCELL	memory cell
//		BRK_CPUADR	cpu address
//		BRK_IOPORT	io address
//		BRK_IRQ		interrupt
// flag		MEM_BRK_ROM | MEM_BRK_RAM | MEM_BRK_SLT | 0 : memory type for BRK_MEMCELL
//		BRK_FETCH || BRK_RD || BRK_WR		: break conditions
// adr		0000..FFFF for BRK_CPUADR
//		full memory address for BRK_MEMCELL
// mask		address mask for BRK_IOADR
//		end address for BRK_MEMCELL/BRK_CPUADR (-1 if single address)
xBrkPoint brkCreate(int type, int flag, int adr, int mask, int act = BRK_ACT_DBG) {
	xBrkPoint brk;
	int adrmask = -1;
	if (type == BRK_MEMCELL) {
		switch(flag  & MEM_BRK_TMASK) {
			case MEM_BRK_ROM: brk.type = BRK_MEMROM; adrmask = conf.zx->mem->romMask; break;
			case MEM_BRK_RAM: brk.type = BRK_MEMRAM; adrmask = conf.zx->mem->ramMask; break;
			case MEM_BRK_SLT: brk.type = BRK_MEMSLT; adrmask = conf.zx->slot->memMask; break;
			default: brk.type = BRK_MEMEXT; break;
		}
		adr &= adrmask;
		if (mask > 0) mask &= adrmask;
		brk.adr = adr;
		if (mask < 0) {
			brk.eadr = brk.adr;
		} else if (mask > adr) {
			brk.eadr = mask;
		} else {
			brk.adr = mask;
			brk.eadr = adr;
		}
		mask = -1;
	} else if (type == BRK_CPUADR) {
		brk.type = BRK_CPUADR;
		adr &= 0xffff;		// mem->busmask
		if (mask > 0) mask &= 0xffff;
		brk.adr = adr;
		if (mask > adr) {
			brk.eadr = mask;
		} else if (mask >= 0) {
			brk.adr = mask;
			brk.eadr = adr;
		} else {
			brk.eadr = brk.adr;
		}
		mask = -1;
	} else {
		brk.type = type;
		brk.adr = adr;
		brk.eadr = brk.adr;
	}
	brk.off = 0;
	brk.fetch = !!(flag & MEM_BRK_FETCH);
	brk.read = !!(flag & MEM_BRK_RD);
	brk.write = !!(flag & MEM_BRK_WR);
	brk.temp = 0;
	brk.mask = mask;
	brk.hits = 0;
	brk.count = 0;
	brk.action = act;
	brk.last = 0;
	brk.fired = 0;
	brk.onchg = 0;
	brk.log = 0;
	return brk;
}

// (re)compile the condition of a breakpoint

void brk_set_cond(xBrkPoint* brk, const char* cond) {
	brk->cond = cond ? cond : "";
	brk->script = xexpr_compile(brk->cond.c_str());
	brk->last = 0;
}

// true = breakpoint may fire. a broken condition breaks too, so that a typo
// doesn't silently disable the breakpoint

int brk_cond_true(xBrkPoint* brk, Computer* comp) {
	bool err = false;
	unsigned res;
	if (brk->cond.empty()) return 1;
	if (!xexpr_ok(brk->script)) return 1;
	res = xexpr_eval(brk->script, comp, &err, brk->hits);
	if (err) return 1;
	return res ? 1 : 0;
}

// global conditions: fire on false->true edge only, else a condition like
// 'A==5' would stop on every instruction while A stays 5

static int cond_count = 0;

int brk_cond_count() {
	return cond_count;
}

// marks every condition that fired and returns how many there are: two of them
// can come true on the same instruction, and both are entitled to their action.
// a condition fires while it is true, the way it reads and the way an address
// breakpoint treats its own condition; 'on change' asks for the edge instead

int brk_check_cond(Computer* comp) {
	int res = 0;
	int trg;
	for (auto it = conf.brk.list.begin(); it != conf.brk.list.end(); it++) {
		it->fired = 0;
		if (it->type != BRK_COND) continue;
		if (it->off) {
			it->last = 0;
			continue;
		}
		trg = brk_cond_true(&(*it), comp);
		if (trg && !(it->onchg && it->last)) {
			it->hits++;
			it->fired = 1;
			res++;
		}
		it->last = trg ? 1 : 0;
	}
	return res;
}

bool brk_compare(xBrkPoint& bp1, xBrkPoint& bp2) {return (bp1.adr < bp2.adr);}
void brkSort() {std::sort(conf.brk.list.begin(), conf.brk.list.end(), brk_compare);}

void brkAdd(xBrkPoint brk, int flag) {
	xBrkPoint* bp = brkFind(&brk, flag & BRKF_SYSTEM).ptr;
	if (bp) {
		bp->fetch = brk.fetch;
		bp->read = brk.read;
		bp->write = brk.write;
		bp->action = brk.action;
		bp->log = brk.log;
		bp->cond = brk.cond;
		bp->script = brk.script;
	} else if (flag & BRKF_SYSTEM) {
		conf.brk.list_sys.push_back(brk);
	} else {
		conf.brk.list.push_back(brk);
	}
	brkSort();
	brkInstallAll();
}

void brkSet(int type, int flag, int adr, int mask) {
	xBrkPoint brk = brkCreate(type, flag, adr, mask);
	brkAdd(brk);
}

void brkXor(int type, int flag, int adr, int mask, int del) {
	xBrkPoint brk = brkCreate(type, flag, adr, mask);
	xbpIndex idx = brkFind(&brk);
	if (idx.ptr) {
		idx.ptr->fetch ^= brk.fetch;
		idx.ptr->read ^= brk.read;
		idx.ptr->write ^= brk.write;
		brk = *idx.ptr;
		if (del && !brk.fetch && !brk.read && !brk.write && !brk.temp) {
			conf.brk.list.erase(conf.brk.list.begin() + idx.idx);
		}
	} else {
		conf.brk.list.push_back(brk);
	}
	brkSort();
	brkInstallAll();
	// brkInstall(brk, del);		// delete if inactive
}

void brkDelete(xBrkPoint dbrk) {
	int idx = brkFind(&dbrk).idx;
	if (idx < 0) return;
	if (idx >= (int)conf.brk.list.size()) return;
	conf.brk.list.erase(conf.brk.list.begin() + idx);
	brkSort();
	brkInstallAll();
}

void brk_clear_tmp(Computer* comp) {
	int i;
	for (i = 0; i < MEM_4M; i++) {
		comp->brkRamMap[i] &= ~MEM_BRK_TFETCH;
	}
	for (i = 0; i < MEM_512K; i++) {
		comp->brkRomMap[i] &= ~MEM_BRK_TFETCH;
	}
	for (i = 0; i < MEM_64K; i++) {
		comp->brkAdrMap[i] &= ~MEM_BRK_TFETCH;
	}
}

void clearMap(unsigned char* ptr, int siz) {
	while (siz > 0) {
		*ptr &= 0xf0;
		ptr++;
		siz--;
	}
}

void brkInstall(xBrkPoint* brk, int del) {
	if (del) {
		brkDelete(*brk);
	} else {
		unsigned char* ptr = NULL;
		Computer* comp = conf.zx;
		unsigned char msk = 0;
		std::map<int, xBrkPoint*>* map = NULL;
		int cnt = 1;
		int adr = -1;
		if (!brk->off) {
			if (brk->temp) msk |= MEM_BRK_TFETCH;
			if (brk->fetch) msk |= MEM_BRK_FETCH;
			if (brk->read) msk |= MEM_BRK_RD;
			if (brk->write) msk |= MEM_BRK_WR;
		}
		if (brk->type != BRK_IRQ)
			map = &conf.brk.map[brk->type];
		switch(brk->type) {
			case BRK_IOPORT:
				for (adr = 0; adr < 0x10000; adr++) {
					if ((adr & brk->mask) == (brk->adr & brk->mask)) {
						comp->brkIOMap[adr] = 0;
						if (!brk->off) {
							if (brk->read) comp->brkIOMap[adr] |= MEM_BRK_RD;
							if (brk->write) comp->brkIOMap[adr] |= MEM_BRK_WR;
						}
					}
					if (comp->brkIOMap[adr]) {
						conf.brk.map[BRK_IOPORT][adr] = brk;
						map = NULL;
					}
				}
				break;
			case BRK_CPUADR:
				ptr = comp->brkAdrMap + (brk->adr & 0xffff);
				cnt = brk->eadr - brk->adr + 1;
				break;
			case BRK_MEMRAM:
				ptr = comp->brkRamMap + (brk->adr & comp->mem->ramMask);
				cnt = brk->eadr - brk->adr + 1;
				break;
			case BRK_MEMROM:
				ptr = comp->brkRomMap + (brk->adr & comp->mem->romMask);
				cnt = brk->eadr - brk->adr + 1;
				break;
			case BRK_MEMSLT:
				if (!comp->slot->brkMap) break;
				ptr = comp->slot->brkMap + (brk->adr & comp->slot->memMask);
				cnt = brk->eadr - brk->adr + 1;
				break;
			case BRK_IRQ:
				comp->flgIBRK = !brk->off;
				ptr = NULL;
				break;
		}
		if (ptr) {
			if (msk & 0x0f) comp->flgBRKMEM = 1;
			adr = brk->adr;
			while (cnt > 0) {
				*ptr &= 0xf0;
				*ptr |= (msk & 0x0f);
				ptr++;
				if (map) (*map)[adr++] = brk;
				cnt--;
			}
		}
	}
}

void brkInstallList(std::vector<xBrkPoint>* list) {
	std::vector<xBrkPoint>::iterator it;
	for (it = list->begin(); it != list->end(); it++) {
		brkInstall(&(*it), 0);
	}
}

void brkInstallAll() {
	Computer* comp = conf.zx;
	int conds = 0;
	int logs = 0;
	cond_count = 0;
	for (auto it = conf.brk.list.begin(); it != conf.brk.list.end(); it++) {
		it->script = xexpr_compile(it->cond.c_str());	// cpu/labels may have changed
		if (!it->cond.empty()) conds++;
		if (it->log && xlog_on(XLG_BRK, XLL_INFO)) logs++;
		if ((it->type == BRK_COND) && !it->off) cond_count++;
	}
	// the flag is what makes the core record the last mem/io event: a
	// condition reads it, and a logged hit prints it
	comp->flgCOND = (conds || logs) ? 1 : 0;
	comp_brk_newstep(comp);		// conditions start from a known beam position
	memset(comp->brkAdrMap, 0x00, MEM_64K);
	memset(comp->brkIOMap, 0x00, MEM_64K);
	clearMap(comp->brkRamMap, MEM_4M);
	clearMap(comp->brkRomMap, MEM_512K);
	if (comp->slot->brkMap)
		clearMap(comp->slot->brkMap, comp->slot->memMask + 1);
	comp->flgBRKMEM = 0;		// brkInstall() sets it again for each point it puts in the maps
	conf.brk.map.clear();
	comp->flgIBRK = 0;
#if 1
	brkInstallList(&conf.brk.list);
	brkInstallList(&conf.brk.list_sys);
#else
	for (auto it = conf.brk.list.begin(); it != conf.brk.list.end(); it++) {
		brkInstall(&(*it), 0);
	}
	for (auto it = conf.brk.list_sys.begin(); it != conf.brk.list_sys.end(); it++) {
		brkInstall(&(*it), 0);
	}
#endif
}

// ------------------------------------------------------------------ logging

// A hit as one line of the event log: what fired and on what, then every
// register the cpu shows, the flags and the machine state around them. One hit
// is one line, so the log keeps its columns and a trace greps by group.
//
// The registers are taken from the cpu's own table, so this is not Z80 code:
// a core showing another set logs that set.

static QString brk_log_page(const char* nm, int abs) {
	return QString("%0:%1:%2").arg(nm).arg(gethexbyte(abs >> 14)).arg(gethexword(abs & 0x3fff));
}

// a cpu address in the page:offset form, for whatever the machine has paged
// in at the time

static QString brk_log_cell(Computer* comp, int cadr) {
	xAdr xa = mem_get_xadr(comp->mem, cadr);
	const char* nm;
	switch (xa.type) {
		case MEM_RAM: nm = "RAM"; break;
		case MEM_ROM: nm = "ROM"; break;
		case MEM_SLOT: nm = "SLT"; break;
		default: nm = "EXT"; break;
	}
	return brk_log_page(nm, xa.abs);
}

// the breakpoint's own form of the address first, then the other one: a cell
// says where the cpu saw it, a cpu address says which page it landed in

static QString brk_log_place(xBrkPoint* brk, Computer* comp) {
	int adr = comp->brka;
	int cadr = comp->brkev.adr;
	QString own;
	switch (brk->type) {
		case BRK_IOPORT: return QString("IO:%0").arg(gethexword(adr));
		case BRK_IRQ: return QString("IRQ");
		case BRK_COND: return QString("COND");
		case BRK_CPUADR:
			return QString("CPU:%0 %1").arg(gethexword(adr)).arg(brk_log_cell(comp, adr));
		case BRK_MEMRAM: own = brk_log_page("RAM", adr); break;
		case BRK_MEMROM: own = brk_log_page("ROM", adr); break;
		case BRK_MEMSLT: own = brk_log_page("SLT", adr); break;
		case BRK_MEMEXT: own = QString("EXT:%0").arg(gethex6(adr)); break;
		default: return QString("BRK");
	}
	if (cadr >= 0) own.append(QString(" CPU:%0").arg(gethexword(cadr)));
	return own;
}

// What the machine was doing when it was caught; an IRQ or a global condition
// is not an access and says nothing here. It is the last mem/io event of the
// instruction, the same one a condition reads - the breakpoint is raised
// before the access is made and the instruction then runs on to its end. The
// cpu address comes with it, so a byte belonging to another access of the same
// instruction is not read as this one's; an instruction byte is latched as no
// event at all and gets no value.

static QString brk_log_access(xBrkPoint* brk, Computer* comp) {
	if (brk->type == BRK_IOPORT) {
		if (comp->brkev.in >= 0) return QString("IN=%0").arg(gethexbyte(comp->brkev.val));
		if (comp->brkev.out >= 0) return QString("OUT=%0").arg(gethexbyte(comp->brkev.val));
		return (comp->brkev.kind == MEM_BRK_WR) ? QString("OUT") : QString("IN");
	}
	if (comp->brkev.kind == MEM_BRK_FETCH) return QString("F");
	if (comp->brkev.kind == MEM_BRK_RD) {
		if (comp->brkev.rd < 0) return QString("RD");
		return QString("RD %0=%1").arg(gethexword(comp->brkev.rd)).arg(gethexbyte(comp->brkev.mdt));
	}
	if (comp->brkev.kind == MEM_BRK_WR) {
		if (comp->brkev.wr < 0) return QString("WR");
		return QString("WR %0=%1").arg(gethexword(comp->brkev.wr)).arg(gethexbyte(comp->brkev.mdt));
	}
	return QString();
}

// PC is the instruction the breakpoint belongs to, not the one after it:
// a write breakpoint is looked at once the instruction has run to its end,
// and pc has moved on by then

static QString brk_log_regs(Computer* comp, xRegBunch* bunch) {
	QString res;
	for (int i = 0; (i < 32) && (bunch->regs[i].id != REG_EOT); i++) {
		xRegister* reg = &bunch->regs[i];
		QString val;
		if ((reg->flag & REG_TYPE_M) == REG_PC) reg->value = comp->brkpc;
		switch (reg->size) {
			case REG_BIT: val = QString::number(reg->value & 1); break;
			case REG_2: val = QString::number(reg->value & 3); break;
			case REG_BYTE: val = gethexbyte(reg->value); break;
			case REG_24: val = gethex6(reg->value); break;
			case REG_32: val = gethexint(reg->value); break;
			default: val = gethexword(reg->value); break;
		}
		res.append(QString(" %0=%1").arg(reg->name).arg(val));
	}
	return res;
}

// the flag register spelled out, in the letters the cpu's own table names
// them by

static QString brk_log_flags(Computer* comp, const char* fnames) {
	if (!fnames) return QString();
	int val = cpu_get_flag(comp->cpu);
	QString names(fnames);
	QString res;
	for (int i = 0; i < names.size(); i++)
		res.append((val & (1 << (names.size() - i - 1))) ? names.at(i) : QChar('-'));
	return res;
}

// where the machine stands: the rom page it runs and the ram page at the top
// window, which is what tells two hits at the same address apart, plus the
// signals the debugger's side panel shows

static QString brk_log_state(Computer* comp) {
	MemPage* pg0 = mem_get_page(comp->mem, 0x0000);
	MemPage* pg3 = mem_get_page(comp->mem, 0xc000);
	QString res;
	res.append(QString(" ROMPG=%0").arg((pg0->type == MEM_ROM) ? QString::number(pg0->num >> 6) : QString("-")));
	res.append(QString(" RAMPG=%0").arg((pg3->type == MEM_RAM) ? QString::number(pg3->num >> 6) : QString("-")));
	res.append(QString(" DOS=%0 ROM=%1 CPM=%2").arg(comp->flgDOS).arg(comp->flgROM).arg(comp->flgCPM));
	res.append(QString(" INT=%0").arg((comp->cpu->intrq & comp->cpu->inten) ? 1 : 0));
	return res;
}

void brk_log_hit(xBrkPoint* brk, Computer* comp) {
	if (!brk->log) return;
	if (!xlog_on(XLG_BRK, XLL_INFO)) return;
	QString line = brk_log_place(brk, comp);
	QString acc = brk_log_access(brk, comp);
	if (!acc.isEmpty()) line.append(QString(" %0").arg(acc));
	line.append(QString(" n=%0/%1").arg(brk->count).arg(brk->hits));
	if (!brk->cond.empty()) line.append(QString(" cond=%0").arg(brk->cond.c_str()));
	xRegBunch bunch = cpuGetRegs(comp->cpu);
	line.append(brk_log_regs(comp, &bunch));
	QString flg = brk_log_flags(comp, bunch.flags);
	if (!flg.isEmpty()) line.append(QString(" F=%0").arg(flg));
	line.append(brk_log_state(comp));
	xlog(XLG_BRK, XLL_INFO, "%s", line.toUtf8().constData());
}

// breakpoint list files

static bool parseRange(QString str, xBrkPoint* p) {
	bool r;
	int badr;
	int eadr;
	int tadr;
	QStringList splt;
	splt = str.split('-', X_SkipEmptyParts);
	badr = splt.first().toInt(&r, 16);
	if (r) {
		if (splt.size() > 1) {
			eadr = splt[1].toInt(&r, 16);
			if (eadr < badr) {
				tadr = badr;
				badr = eadr;
				eadr = tadr;
			}
		} else {
			eadr = badr;
		}
		if (r) {
			p->adr = badr;
			p->eadr = eadr;
		}
	}
	return r;
}


// a number the way unreal writes it: 0x forces hex, # and $ too, everything
// else is decimal (that is what unreal's own sscanf("%i") does)

static int unreal_num(QString str, bool* ok) {
	str = str.trimmed();
	if (str.startsWith("#") || str.startsWith("$"))
		return str.mid(1).toInt(ok, 16);
	return str.toInt(ok, 0);
}

// one line of unreal's bpx.ini: x0=0x80A6 (exec), r0=/w0= (read/write), the
// address may be a range: x0=0x8000-0x8FFF. the digit is the cpu index - unreal
// has a second z80 in the General Sound, we don't, so only 0 is taken.
// returns 1 if the line belongs to that format (even when it is skipped)

static int brk_add_unreal(QString line) {
	QString str = line.trimmed();
	int adr, eadr;
	bool ok = false;
	if (str.size() < 3) return 0;
	QChar tp = str.at(0).toLower();
	if ((tp != QChar('x')) && (tp != QChar('r')) && (tp != QChar('w'))) return 0;
	str.remove(0, 1);
	if (str.at(0).isDigit()) {			// cpu index
		if (str.at(0) != QChar('0')) return 1;	// another cpu: not for us
		str.remove(0, 1);
	}
	if (!str.startsWith("=")) return 0;
	str.remove(0, 1);
	QStringList prt = str.split('-', X_SkipEmptyParts);
	if (prt.isEmpty()) return 0;
	adr = unreal_num(prt.first(), &ok) & 0xffff;
	if (!ok) return 0;
	eadr = adr;
	if (prt.size() > 1) {
		eadr = unreal_num(prt.at(1), &ok) & 0xffff;
		if (!ok) return 0;
		if (eadr < adr) return 0;
	}
	// unreal keeps read, write and exec in separate lines: merge them
	for (auto it = conf.brk.list.begin(); it != conf.brk.list.end(); it++) {
		if ((it->type != BRK_CPUADR) || (it->adr != adr) || (it->eadr != eadr)) continue;
		if (tp == QChar('x')) it->fetch = 1;
		else if (tp == QChar('r')) it->read = 1;
		else it->write = 1;
		return 1;
	}
	int flag = MEM_BRK_WR;
	if (tp == QChar('x')) flag = MEM_BRK_FETCH;
	else if (tp == QChar('r')) flag = MEM_BRK_RD;
	xBrkPoint brk = brkCreate(BRK_CPUADR, flag, adr, (eadr > adr) ? eadr : -1);
	brk_set_cond(&brk, "");
	conf.brk.list.push_back(brk);
	return 1;
}

// load / save the breakpoint list. one breakpoint per line:
// type:arg1:arg2:flags:action:condition - the condition comes last, so it can
// hold colons of its own. lines in unreal's bpx.ini format are taken too

int brk_load_list(const char* fpath) {
	QString fnam(fpath);
	QFile file(fnam);
	QString line;
	QStringList splt;
	QStringList list;
	int off;
	bool b0,b1;
	if (!file.open(QFile::ReadOnly)) return 0;
	conf.brk.list.clear();
	while(!file.atEnd()) {
		line = QString(file.readLine());
		if (line.trimmed().isEmpty()) continue;
		if (brk_add_unreal(line)) continue;	// unreal's bpx.ini line
		if (!line.startsWith(";")) {
			xBrkPoint brk{};		// every field zeroed, not left from the line before
			b0 = true;
			b1 = true;
			list = line.trimmed().split(":", X_KeepEmptyParts);
			while(list.size() < 6)
				list.append(QString());
			// the condition is the last field: keep its own colons, if any
			QString cond = QStringList(list.mid(5)).join(":").trimmed();
			brk.fetch = list.at(3).contains("F") ? 1 : 0;
			brk.read = list.at(3).contains("R") ? 1 : 0;
			brk.write = list.at(3).contains("W") ? 1 : 0;
			brk.off = list.at(3).contains("0") ? 1 : 0;
			brk.onchg = list.at(3).contains("E") ? 1 : 0;
			brk.log = list.at(3).contains("L") ? 1 : 0;
			if (list.at(0) == "IO") {
				brk.type = BRK_IOPORT;
				brk.adr = list.at(1).toInt(&b0, 16) & 0xffff;
				brk.mask = list.at(2).toInt(&b1, 16) & 0xffff;
			} else if (list.at(0) == "CPU") {
				brk.type = BRK_CPUADR;
				b0 = parseRange(list.at(1), &brk);
				brk.adr &= 0xffff;
				brk.eadr &= 0xffff;
//					if (list.at(1).contains("-")) {		// 1234-ABCD
//						splt = list.at(1).split(QLatin1Char('-'), X_SkipEmptyParts);
//						list[1] = splt.first();
//						list[2] = splt.last();
//					}
//					brk.adr = list.at(1).toInt(&b0, 16) & 0xffff;
//					if (list.at(2).isEmpty()) {
//						brk.eadr = brk.adr;
//					} else {
//						brk.eadr = list.at(2).toInt(&b1, 16) & 0xffff;
//						if (brk.eadr < brk.adr)
//							brk.eadr = brk.adr;
//					}
			} else if (list.at(0) == "ROM") {
				brk.type = BRK_MEMROM;
				b0 = parseRange(list.at(2), &brk);
				off = (list.at(1).toInt(&b0, 16) & 0xff) << 14;
				brk.adr = (brk.adr & 0x3fff) | off;
				brk.eadr = (brk.eadr & 0x3fff) | off;
//					brk.adr = (list.at(1).toInt(&b0, 16) & 0xff) << 14;
//					brk.adr |= (list.at(2).toInt(&b1, 16) & 0x3fff);
//					brk.eadr = brk.adr;
			} else if (list.at(0) == "RAM") {
				brk.type = BRK_MEMRAM;
				b0 = parseRange(list.at(2), &brk);
				off = (list.at(1).toInt(&b0, 16) & 0xff) << 14;
				brk.adr = (brk.adr & 0x3fff) | off;
				brk.eadr = (brk.eadr & 0x3fff) | off;
//					brk.adr = (list.at(1).toInt(&b0, 16) & 0xff) << 14;
//					brk.adr |= (list.at(2).toInt(&b1, 16) & 0x3fff);
//					brk.eadr = brk.adr;
			} else if (list.at(0) == "SLT") {
				brk.type = BRK_MEMSLT;
				b0 = parseRange(list.at(2), &brk);
				off = (list.at(1).toInt(&b0, 16) & 0xff) << 14;
				brk.adr = (brk.adr & 0x3fff) | off;
				brk.eadr = (brk.eadr & 0x3fff) | off;
//					brk.adr = (list.at(1).toInt(&b0, 16) & 0xff) << 14;
//					brk.adr |= (list.at(2).toInt(&b1, 16) & 0x3fff);
//					brk.eadr = brk.adr;
			} else if (list.at(0) == "IRQ") {
				brk.type = BRK_IRQ;
			} else if (list.at(0) == "COND") {
				brk.type = BRK_COND;
				brk.adr = 0;
				brk.eadr = 0;
			} else {
				b0 = false;
			}
			if (list.at(4) == "SCR") {
				brk.action = BRK_ACT_SCR;
			} else if (list.at(4) == "CNT") {
				brk.action = BRK_ACT_COUNT;
			} else {
				brk.action = BRK_ACT_DBG;
			}
			if (b0 && b1) {
				brk.hits = 0;
				brk.count = 0;
				brk.temp = 0;
				brk.last = 0;
				brk_set_cond(&brk, cond.toLocal8Bit().data());
				conf.brk.list.push_back(brk);
			}
		}
	}
	file.close();
	brkInstallAll();
	return 1;
}

int brk_save_list(const char* fpath) {
	xBrkPoint brk;
	QString fnam(fpath);
	QFile file(fnam);
	QString nm,ar1,ar2,flag,act;
	if (!file.open(QFile::WriteOnly)) return 0;
	file.write("; Xpeccy+ Debugger breakpoints list\n");
	foreach(brk, conf.brk.list) {
		switch(brk.type) {
			case BRK_IOPORT:
				nm = "IO";
				ar1 = gethexword(brk.adr & 0xffff);
				ar2 = gethexword(brk.mask & 0xffff);
				break;
			case BRK_CPUADR:
				nm = "CPU";
				ar1 = gethexword(brk.adr & 0xffff);
				ar2.clear();
				if (brk.eadr > brk.adr) {
					ar1.append("-");
					ar1.append(gethexword(brk.eadr & 0xffff));
				}
				break;
			case BRK_MEMRAM:
				nm = "RAM";
				ar1 = gethexbyte((brk.adr >> 14) & 0xff);	// 16K page
				ar2 = gethexword(brk.adr & 0x3fff);		// adr in page
				if (brk.eadr > brk.adr) {
					ar2.append("-");
					ar2.append(gethexword(brk.eadr & 0x3fff));
				}
				break;
			case BRK_MEMROM:
				nm = "ROM";
				ar1 = gethexbyte((brk.adr >> 14) & 0xff);
				ar2 = gethexword(brk.adr & 0x3fff);
				if (brk.eadr > brk.adr) {
					ar2.append("-");
					ar2.append(gethexword(brk.eadr & 0x3fff));
				}
				break;
			case BRK_MEMSLT:
				nm = "SLT";
				ar1 = gethexbyte((brk.adr >> 14) & 0xff);
				ar2 = gethexword(brk.adr & 0x3fff);
				if (brk.eadr > brk.adr) {
					ar2.append("-");
					ar2.append(gethexword(brk.eadr & 0x3fff));
				}
				break;
			case BRK_IRQ:
				nm = "IRQ";
				ar1.clear();
				ar2.clear();
				break;
			case BRK_COND:
				nm = "COND";
				ar1.clear();
				ar2.clear();
				break;
			default:
				nm.clear();
				break;
		}
		switch(brk.action) {
			case BRK_ACT_DBG: act = "DBG"; break;
			case BRK_ACT_SCR: act = "SCR"; break;
			case BRK_ACT_COUNT: act = "CNT"; break;
			default: act.clear(); break;
		}
		if (!nm.isEmpty()) {
			flag.clear();
			if (brk.fetch) flag.append("F");
			if (brk.read) flag.append("R");
			if (brk.write) flag.append("W");
			if (brk.off) flag.append("0");
			if (brk.onchg) flag.append("E");
			if (brk.log) flag.append("L");
			file.write(QString("%0:%1:%2:%3:%4:%5\n").arg(nm).arg(ar1).arg(ar2).arg(flag).arg(act).arg(brk.cond.c_str()).toUtf8());
		}
	}
	file.close();
	return 1;
}

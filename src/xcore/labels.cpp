#include "xcore.h"
#include "arcpath.h"
#include "dbgtext.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDialog>

#include <QDebug>

// labels

xLabelSet* createLabelSet(QString name) {
	xLabelSet* set = new xLabelSet;
	set->name = name;
	set->list.clear();
	conf.labsets.append(set);
	return set;
}

// return pointer to existing labelset for given name, or nullptr if doesn't exist
xLabelSet* findLabelSet(QString name) {
	xLabelSet* ptr = nullptr;
	xLabelSet* res = nullptr;
	foreach (ptr, conf.labsets) {
		if (ptr->name == name) {
			res = ptr;
		}
	}
	return res;
}

// bumped on every change to the labels in effect: what a compiled expression
// remembers of a label is good while this stays the same
static unsigned labGen = 1;

unsigned labels_gen() {
	return labGen;
}

void labels_touch() {
	labGen++;
}

// Built-in labels: the lists in config/rom-labels (docs/debugger-format.md, the
// sources in its README.md) name the routines and data of known ROMs, found by
// each 16K bank's MD5, and the system variables of the systems in the machine -
// each name in its system's namespace, BASIC48.LD_BYTES.

struct romLabData {
	int start;
	int end;
	unsigned char view;	// DBG_VIEW_*
	int step;		// bytes a statement takes, 0: one statement for all of it
	QString key;		// "@START": how a rom record's skip drops it
};

struct romLab {
	int ofs;		// in the bank; the CPU address for a system variable
	QString full;		// BASIC48.LD_BYTES
	QString bare;		// LD_BYTES
	int size;		// a system variable's, 0: as far as the next one
	int ram;		// a system variable's RAM bank
};

// what one file says about one ROM name: the banks it is, the names and data in them
struct romLabList {
	QString mem;					// its rom name, BASIC48 of BASIC48:0556
	QList<romLab> labs;
	QList<romLabData> data;
	QSet<QString> spaces;				// the namespaces of its names
	QHash<QByteArray, QSet<QString> > banks;	// MD5 (hex) of a bank -> names and @data it has not
};

// a file's system variables: there while a ROM with names of their namespace is
struct sysLabList {
	QList<romLab> labs;
};

struct sysVar {
	QString name;		// full
	int adr;		// the CPU address it is at
	int size;		// up to the next variable
};

static QList<romLabList> romLists;
static QList<sysLabList> sysLists;
static QSet<QString> romSpaces;			// every namespace of the lists
static QHash<int, QString> romByAbs;		// ROM cell -> full name, for the machine in use
static QMap<QString, QList<int> > romByName;	// full name -> its cells, lowest first
static QHash<QString, QStringList> romShort;	// short name -> the full names it may stand for
static QByteArray romView;			// ROM cell -> the data mark a list gives it
static QHash<int, sysVar> sysByAbs;		// RAM cell -> system variable
static QMap<QString, xAdr> sysByName;		// full name -> its cell
static QHash<QString, QStringList> sysShort;
static QList<QByteArray> romBanks;		// the MD5s they were found by
static int romCustomSeen = 0;			// the machine's romCustom at that time
static bool labFull = false;			// names in full, namespace and all

// a name's namespace as the lists give it: BASIC48 of BASIC48.LD_BYTES
static QString rom_space(const QString& full) {
	int dot = full.indexOf('.');
	return (dot > 0) ? full.left(dot) : QString();
}

// a skip key for data starting at an offset, as a rom record's skip says it
static QString rom_data_key(int ofs) {
	return QString("@%1").arg(ofs, 4, 16, QChar('0')).toUpper();
}

// One list file: what it says of each ROM name it uses, and its system variables.
// Lines it cannot use are counted, and logged once for the file
static void rom_list_file(const QString& path) {
	QFile file(path);
	if (!file.open(QFile::ReadOnly)) return;
	QHash<QString, int> byName;		// a ROM name -> its list in romLists
	sysLabList sys;
	int skipped = 0;
	int num = 0;
	auto list_of = [&](const QString& mem) -> romLabList& {
		if (!byName.contains(mem)) {
			byName[mem] = romLists.size();
			romLabList lst;
			lst.mem = mem;
			romLists.append(lst);
		}
		return romLists[byName[mem]];
	};
	dtRecord rec;
	while (!file.atEnd()) {
		num++;
		QString line = QString::fromUtf8(file.readLine());
		if (!dt_parse_line(line, &rec)) {
			xlog(XLG_FILE, XLL_WARN, "ROM labels: %s, line %i: %s", path.toLocal8Bit().constData(), num,
				rec.error.toLocal8Bit().constData());
			continue;
		}
		switch (rec.type) {
			case DT_REC_NONE:
				break;
			case DT_REC_ROM: {
				QSet<QString> drop;
				foreach(const QString& nm, rec.skipNames) drop.insert(nm);
				foreach(const dtPlace& pl, rec.skipPlaces) drop.insert(rom_data_key(pl.ofs));
				list_of(rec.name).banks[rec.md5.toLatin1()] = drop;
				break;
			}
			case DT_REC_LABEL: {
				QString space = rom_space(rec.name);
				romLab lab = {rec.at.ofs, rec.name, space.isEmpty() ? rec.name : rec.name.mid(space.size() + 1),
					rec.size, -1};
				if (!space.isEmpty()) romSpaces.insert(space);
				if ((rec.at.kind == DT_PLACE_ROM) && (rec.at.ofs < MEM_16K)) {
					romLabList& lst = list_of(rec.at.name);
					lst.labs.append(lab);
					if (!space.isEmpty()) lst.spaces.insert(space);
				} else if ((rec.at.kind == DT_PLACE_PAGE) && !space.isEmpty()) {
					// a system variable: its CPU address where a 128K has that bank
					int ofs = rec.at.ofs & 0x3fff;
					lab.ram = rec.at.page;
					lab.ofs = ((lab.ram == 5) ? 0x4000 : (lab.ram == 2) ? 0x8000 : 0xc000) | ofs;
					sys.labs.append(lab);
				} else {
					skipped++;
				}
				break;
			}
			case DT_REC_DATA:
				if ((rec.at.kind == DT_PLACE_ROM) && (rec.view != DBG_VIEW_EXEC)) {
					romLabData dat;
					int bytes = rec.len * rec.count * rec.unit;
					dat.start = rec.at.ofs;
					dat.end = rec.at.ofs + bytes - 1;
					dat.view = rec.view;
					// one statement each: the count's; dc's strings end themselves
					dat.step = ((rec.count > 1) && !rec.dc) ? rec.len * rec.unit : 0;
					dat.key = rom_data_key(rec.at.ofs);
					if (dat.end < MEM_16K) list_of(rec.at.name).data.append(dat);
					else skipped++;
				} else {
					skipped++;
				}
				break;
			default:
				skipped++;
				break;
		}
	}
	if (!sys.labs.isEmpty()) sysLists.append(sys);
	if (skipped)
		xlog(XLG_FILE, XLL_WARN, "ROM labels: %s: %i lines not for a ROM list, skipped", path.toLocal8Bit().constData(), skipped);
}

// The lists are files in config/rom-labels of the installed program, each under
// the license of its source (config/rom-labels/README.md)
static void rom_lists_read() {
	static bool done = false;
	if (done) return;
	done = true;
	QDir dir(xconf_install_dir() + "/rom-labels");
	if (!dir.exists()) xlog(XLG_FILE, XLL_WARN, "ROM labels: no %s", dir.path().toLocal8Bit().constData());
	foreach(QString fn, dir.entryList(QStringList("*.labels"), QDir::Files, QDir::Name))
		rom_list_file(dir.filePath(fn));
}

// the length of a name's namespace, BASIC48 of BASIC48.LD_BYTES; 0 if it has none
int labels_ns_len(const QString& nm) {
	int dot = nm.indexOf('.');
	return ((dot > 0) && romSpaces.contains(nm.left(dot))) ? dot : 0;
}

static QString short_name(const QString& full) {
	int len = labels_ns_len(full);
	return len ? full.mid(len + 1) : full;
}

static void short_add(QHash<QString, QStringList>& map, const QString& bare, const QString& full) {
	if (bare == full) return;
	QStringList& lst = map[bare];
	if (!lst.contains(full)) lst.append(full);
}

// The machine's ROM, as loaded: called under emu_lock whenever it may have
// changed. The same banks as last time change nothing
void labels_rom_scan(Computer* comp) {
	QList<QByteArray> banks;
	if (comp) {
		int size = comp->mem->romMask + 1;
		for (int bank = 0; bank + MEM_16K <= size; bank += MEM_16K)
			banks.append(QCryptographicHash::hash(QByteArray::fromRawData((const char*)comp->mem->romData + bank, MEM_16K),
				QCryptographicHash::Md5).toHex());
	}
	if (comp) romCustomSeen = comp->romCustom;
	if (banks == romBanks) return;
	romBanks = banks;
	romByAbs.clear();
	romByName.clear();
	romShort.clear();
	sysByAbs.clear();
	sysByName.clear();
	sysShort.clear();
	romView = QByteArray(banks.size() * MEM_16K, 0);
	labGen++;
	rom_lists_read();
	QSet<QString> fitted;		// the namespaces of the banks found
	for (int b = 0; b < banks.size(); b++) {
		foreach(const romLabList& lst, romLists) {
			QHash<QByteArray, QSet<QString> >::const_iterator it = lst.banks.constFind(banks.at(b));
			if (it == lst.banks.constEnd()) continue;
			fitted.unite(lst.spaces);
			foreach(const romLab& lab, lst.labs) {
				if (it->contains(lab.bare) || it->contains(lab.full)) continue;
				int abs = b * MEM_16K + lab.ofs;
				romByAbs[abs] = lab.full;
				QList<int>& cells = romByName[lab.full];
				if (!cells.contains(abs)) cells.append(abs);	// two lists of a bank may share names
				short_add(romShort, lab.bare, lab.full);
			}
			foreach(const romLabData& dat, lst.data) {
				if (it->contains(dat.key)) continue;
				char* cell = romView.data() + b * MEM_16K;
				for (int ofs = dat.start; ofs <= dat.end; ofs++) {
					bool first = dat.step ? (((ofs - dat.start) % dat.step) == 0) : (ofs == dat.start);
					cell[ofs] = (char)(dat.view | (first ? DBG_STMT : 0));
				}
			}
		}
	}
	// the system variables of the systems in the machine: cells of their bank, as
	// the 128K numbers its banks - not the Next, whose RAM cells go another way
	bool next = comp && (comp->hw->id == HW_ZXNEXT);
	foreach(const sysLabList& lst, sysLists) {
		if (next) break;
		for (int i = 0; i < lst.labs.size(); i++) {
			const romLab& lab = lst.labs.at(i);
			if (!fitted.contains(rom_space(lab.full))) continue;
			xAdr xadr;
			xadr.type = MEM_RAM;
			xadr.bank = lab.ram;
			xadr.adr = lab.ofs;
			xadr.abs = (lab.ram << 14) | (xadr.adr & 0x3fff);
			// as far as the next one (the lists go up); the last, a word
			int size = lab.size ? lab.size : (i + 1 < lst.labs.size()) ? lst.labs.at(i + 1).ofs - lab.ofs : 2;
			sysByAbs[xadr.abs] = {lab.full, lab.ofs, qBound(1, size, 64)};
			sysByName[lab.full] = xadr;
			short_add(sysShort, lab.bare, lab.full);
		}
	}
	if (!romByAbs.isEmpty())
		xlog(XLG_FILE, XLL_INFO, "ROM labels: %i, system variables: %i", (int)romByAbs.size(), (int)sysByAbs.size());
}

// A ROM a snapshot put in since the last scan (an RZX loads its snapshots as it
// plays): scan again. A byte written into the ROM is no reason to drop a bank's names
void labels_rom_check(Computer* comp) {
	if (comp && (comp->romCustom != romCustomSeen)) labels_rom_scan(comp);
}

static int rom_cpu_adr(int abs);

static bool user_has(const QString& nm) {
	return conf.curlabset && conf.curlabset->list.contains(nm);
}

// the first of a ROM name's cells the CPU sees now, -1 if none; adr: where
static int rom_paged_cell(const QList<int>& cells, int* adr = nullptr) {
	foreach(int abs, cells) {
		int at = rom_cpu_adr(abs);
		if (at >= 0) {
			if (adr) *adr = at;
			return abs;
		}
	}
	return -1;
}

// The full name a built-in name stands for: itself, or the one full name a short
// one has among the ROM labels and system variables in effect - of several, the
// one whose ROM is paged in now (a system variable always is). Empty when none
// or still more than one; amb then gets the names it could be
static QString builtin_full(const QString& nm, QStringList* amb = nullptr) {
	if (conf.dbg.romlabels && romByName.contains(nm)) return nm;
	if (conf.dbg.sysvars && sysByName.contains(nm)) return nm;
	QStringList cands;
	if (conf.dbg.romlabels) cands += romShort.value(nm);
	if (conf.dbg.sysvars) cands += sysShort.value(nm);
	if (cands.size() == 1) return cands.first();
	if (cands.size() > 1) {
		QStringList now;
		foreach(const QString& full, cands)
			if (!romByName.contains(full) || (rom_paged_cell(romByName[full]) >= 0)) now.append(full);
		if (now.size() == 1) return now.first();
	}
	if (amb) *amb = cands;
	return QString();
}

// every full name a bare one could stand for, whatever is paged in; none for a
// label of the user's
QStringList labels_candidates(const QString& nm) {
	QStringList res;
	if (user_has(nm)) return res;
	if (conf.dbg.romlabels) res += romShort.value(nm);
	if (conf.dbg.sysvars) res += sysShort.value(nm);
	return res;
}

// a short name more than one system has: what it stands for follows the paging
bool labels_shared(const QString& nm) {
	return labels_candidates(nm).size() > 1;
}

// A built-in name as the listing shows it: short, unless the short one would mean
// something else when typed now (a label of the user's, another system's name)
static QString builtin_shown(const QString& full) {
	if (labFull) return full;
	QString sh = short_name(full);
	if ((sh == full) || user_has(sh) || (builtin_full(sh) != full)) return full;
	return sh;
}

// the full names a bare name typed could mean, when it is more than one
QStringList labels_ambiguous(const QString& nm) {
	QStringList amb;
	if (!user_has(nm) && builtin_full(nm, &amb).isEmpty() && (amb.size() > 1)) return amb;
	return QStringList();
}

// built-in names in full while on: Disasm to file
void labels_full(bool on) {
	labFull = on;
}

// a system variable's size at that cell, 0 if none is there: how far "VAR + n" reaches
int labels_sys_size(xAdr xadr) {
	if ((xadr.type != MEM_RAM) || !conf.dbg.sysvars) return 0;
	QHash<int, sysVar>::const_iterator it = sysByAbs.constFind(xadr.abs);
	if ((it == sysByAbs.constEnd()) || ((xadr.adr >= 0) && ((xadr.adr & 0xffff) != it->adr))) return 0;
	return it->size;
}

// every cell of a ROM label's name, one a bank it is in
QList<int> labels_rom_cells(const QString& nm) {
	return conf.dbg.romlabels ? romByName.value(builtin_full(nm)) : QList<int>();
}

QStringList labels_rom_names() {
	return conf.dbg.romlabels ? romByName.keys() : QStringList();
}

// the built-in names to complete: ROM labels and system variables in full, and
// their short names
QStringList labels_builtin_names() {
	QStringList res = labels_rom_names();
	QStringList shorts;
	if (conf.dbg.romlabels) shorts += romShort.keys();
	if (conf.dbg.sysvars) {
		res += sysByName.keys();
		shorts += sysShort.keys();
	}
	return res + shorts;		// a short name of several systems too: it follows the paging
}

bool labels_rom_only(const QString& nm) {
	return !builtin_full(nm).isEmpty() && !user_has(nm);
}

// the data mark a ROM list gives a ROM cell, DBG_VIEW_CODE if none
unsigned char labels_rom_view(int abs) {
	if (!conf.dbg.romlabels || !conf.zx || romView.isEmpty()) return DBG_VIEW_CODE;
	abs &= conf.zx->mem->romMask;
	return (abs < romView.size()) ? (unsigned char)romView.at(abs) : DBG_VIEW_CODE;
}

// Where the CPU sees ROM cell abs now, -1 if nowhere. A page number past the
// ROM wraps, as the memory reads it: the Evo pages its 48 BASIC as #3C of 32
static int rom_cpu_adr(int abs) {
	Computer* comp = conf.zx;
	if (!comp) return -1;
	if (comp->hw->memView) return comp_find_xadr(comp, MEM_ROM, abs, MEM_VIEW_READ);
	Memory* mem = comp->mem;
	int base = abs & mem->romMask & ~mem->pgmask;
	for (int i = 0; i < 256; i++) {
		if ((mem->map[i].type == MEM_ROM) && (((mem->map[i].num << mem->pgshift) & mem->romMask) == base))
			return (i << mem->pgshift) | (abs & mem->pgmask);
	}
	return -1;
}

// Which ROM pages the CPU's four 16K windows hold now, as one number: a name of
// several systems looked up under one key stays right while the key stays the
// same. Another window counts by its type alone, so a RAM page switch is none
unsigned long long labels_paging_key() {
	Computer* comp = conf.zx;
	if (!comp || comp->hw->memView) return 0;
	Memory* mem = comp->mem;
	unsigned long long key = 0;
	for (int w = 0; w < 4; w++) {
		int i = (w << 14) >> mem->pgshift;
		int num = (mem->map[i].type == MEM_ROM) ? (mem->map[i].num & 0x0fff) : 0;
		key = (key << 16) | ((unsigned long long)(mem->map[i].type & 0x0f) << 12) | num;
	}
	return key;
}

// a ROM cell as the lists name it: wrapped the same way
static int rom_abs(int abs) {
	return conf.zx ? (abs & conf.zx->mem->romMask) : abs;
}

// the cell of a built-in name: a ROM label's, the one paged in now if the name
// is in more banks; a system variable's
xAdr labels_rom_cell(const QString& name) {
	xAdr xadr = {-1, -1, -1, -1};
	QString nm = builtin_full(name);
	if (nm.isEmpty()) return xadr;
	if (!conf.dbg.romlabels || !romByName.contains(nm)) return sysByName.value(nm, xadr);
	const QList<int>& cells = romByName[nm];
	xadr.type = MEM_ROM;
	xadr.abs = rom_paged_cell(cells, &xadr.adr);
	if (xadr.abs < 0) xadr.abs = cells.first();
	xadr.bank = xadr.abs >> 14;
	return xadr;
}

// create conf.labmap from current labset
void map_labels() {
	xLabelSet* set = conf.curlabset;
	labGen++;
	conf.labmap.clear();
	if (set) {
		xAdr xadr;
		QString nm;
		foreach(nm, set->list.keys()) {
			xadr = set->list[nm];
			conf.labmap[xadr.type][xadr.abs] = nm;
		}
	}
}

int delLabelSet(QString name) {
	int res = 0;
	xLabelSet* cur = conf.curlabset;
	xLabelSet* ptr;
	for (int i = conf.labsets.size() - 1; i >= 0; i--) {
		ptr = conf.labsets.at(i);
		if (ptr->name == name) {
			conf.labsets.removeAt(i);
			if (cur == ptr) {		// delete current?
				conf.curlabset = conf.labsets.size() ? conf.labsets.at(0) : nullptr;
			}
			delete(ptr);
			res++;
		}
	}
	map_labels();
	return res;
}

// returns pointer to existing labels set for given name, or add new labelset and return its pointer
xLabelSet* newLabelSet(QString name) {
	xLabelSet* res = findLabelSet(name);
	if (!res) {
		res = createLabelSet(name);
	}
	return res;
}

void setLabelSet(xLabelSet* set) {
	conf.curlabset = set;
	map_labels();			// recreate labmap
}

xLabelSet* setLabelSet(QString path) {
	xLabelSet* set = findLabelSet(path);
	if (set) {
		setLabelSet(set);
	}
	return set;
}

void clear_labels() {
	xLabelSet* set = conf.curlabset;
	if (set) {
		set->list.clear();
		conf.labmap.clear();
	}
	labGen++;
}

void clear_all_labels() {
	while(!conf.labsets.isEmpty()) {
		free(conf.labsets.first());
		conf.labsets.takeFirst();
	}
	labGen++;
}

void del_label(QString name) {
	xLabelSet* set = conf.curlabset;
	if (set) {
		if (set->list.contains(name)) {
			xAdr xadr = set->list[name];
			set->list.remove(name);
			conf.labmap[xadr.type].remove(xadr.abs);
		}
	}
	labGen++;
}

void add_label(xAdr xadr, QString name, xLabelSet* set) {
	if (!set) {
		set = conf.curlabset;
	}
	if (!set) {				// if no current labset
		set = newLabelSet("noname");	// new internal labset
		setLabelSet(set);
	}
	if (set) {
		if (set->list.contains(name))
			del_label(name);
		if (xadr.type == MEM_ROM) {		// a page past the ROM is the one it wraps to
			xadr.abs = rom_abs(xadr.abs);
			xadr.bank = xadr.abs >> 14;
		}
		set->list[name] = xadr;
		conf.labmap[xadr.type][xadr.abs] = name;
	}
	labGen++;
}

static QString find_label_user(xAdr xadr) {
	QString lab;
	xLabelSet* set = conf.curlabset;
	if (xadr.type == MEM_ROM) xadr.abs = rom_abs(xadr.abs);
	if (set) {
		if (conf.labmap.contains(xadr.type)) {
			if (conf.labmap[xadr.type].contains(xadr.abs)) {
				lab = conf.labmap[xadr.type][xadr.abs];
			}
		}
		// a label at a CPU address names whatever cell is there: a page view passes
		// an offset in its adr, and the cell at that offset is not that CPU address
		if (lab.isEmpty() && (xadr.adr >= 0) && conf.labmap.contains(MEM_IO) && conf.labmap[MEM_IO].contains(xadr.adr)) {
			xAdr now = conf.zx ? comp_get_xadr(conf.zx, xadr.adr, MEM_VIEW_READ) : xadr;
			if ((now.type == xadr.type) && (now.abs == xadr.abs)) lab = conf.labmap[MEM_IO].value(xadr.adr);
		}
	}
	return lab;
}

// The user's labels first, then the ROM's; with LAB_SYSVAR a system variable's
// too (operands and tips, never a row of its own). rom: the name is a built-in one
QString find_label(xAdr xadr, bool* rom, int how) {
	QString lab = find_label_user(xadr);
	bool builtin = false;
	if (lab.isEmpty() && (xadr.type == MEM_ROM) && conf.dbg.romlabels) {
		lab = romByAbs.value(rom_abs(xadr.abs));
		builtin = !lab.isEmpty();
	} else if (lab.isEmpty() && (xadr.type == MEM_RAM) && (how & LAB_SYSVAR) && conf.dbg.sysvars) {
		// only where the system has it: bank 5 seen at C000 too is not its variables
		if (labels_sys_size(xadr)) lab = sysByAbs.value(xadr.abs).name;
		builtin = !lab.isEmpty();
	}
	if (builtin && !(how & LAB_FULL)) lab = builtin_shown(lab);
	if (rom) *rom = builtin;
	return lab;
}

// A label in ROM whose bank is paged out: where that ROM runs, 0000-3FFF, for a
// typed address and the assembler (CALL TRDOS_ENTRY_EXECUTE_COMMAND_IN_C from BASIC). Breakpoints
// and conditions go by label_cpu(), only where it is now
xAdr label_home(xAdr xadr) {
	if ((xadr.type == MEM_ROM) && (xadr.adr < 0)) xadr.adr = xadr.abs & 0x3fff;
	return xadr;
}

// the cell a label names, its CPU address as it was when the label was read
xAdr find_label_cell(QString nm) {
	xAdr xadr = {-1, -1, -1, -1};
	xLabelSet* set = conf.curlabset;
	if (set) xadr = set->list.value(nm, xadr);
	if (xadr.type < 0) xadr = labels_rom_cell(nm);
	return xadr;
}

// that cell's CPU address now: on the Next RAM, and ROM everywhere, follow the
// paging, -1 when the cell is not paged in
xAdr label_cpu(xAdr xadr) {
	if (conf.zx && (xadr.type == MEM_ROM))
		xadr.adr = rom_cpu_adr(xadr.abs);
	else if (conf.zx && (conf.zx->hw->id == HW_ZXNEXT) && (xadr.type == MEM_RAM))
		xadr.adr = comp_find_xadr(conf.zx, xadr.type, xadr.abs, MEM_VIEW_READ);
	// a ROM label's name in other banks too (the Evo's copies of the 48 BASIC): the
	// one paged in now, unless the cell is a label of the user's
	if (conf.zx && (xadr.type == MEM_ROM) && (xadr.adr < 0) && conf.dbg.romlabels) {
		QHash<int, QString>::const_iterator ra = romByAbs.constFind(xadr.abs);
		if ((ra != romByAbs.constEnd()) && !(conf.labmap.contains(MEM_ROM) && conf.labmap[MEM_ROM].contains(xadr.abs))) {
			int adr = -1;
			int abs = rom_paged_cell(romByName.value(ra.value()), &adr);
			if (abs >= 0) {
				xadr.abs = abs;
				xadr.adr = adr;
				xadr.bank = abs >> 14;
			}
		}
	}
	return xadr;
}

xAdr find_label(QString nm) {
	return label_cpu(find_label_cell(nm));
}

// A local label, SjASMPlus's "global.local": the length of the part before the
// last dot when a label of that name is loaded, else 0
int label_local_prefix(const QString& nm) {
	int ns = labels_ns_len(nm);		// a namespace, not a label of the user's
	if (ns) return ns;
	int pos = nm.lastIndexOf('.');
	if ((pos < 1) || (pos == nm.size() - 1) || !conf.curlabset) return 0;
	return conf.curlabset->list.contains(nm.left(pos)) ? pos : 0;
}

// The block a cell is in: the nearest label at or before it, in its 16K bank,
// that is not a local one. Empty when there is none near
QString label_block(xAdr xadr) {
	if (!conf.labmap.contains(xadr.type)) return QString();
	const QMap<int, QString>& map = conf.labmap[xadr.type];
	QMap<int, QString>::const_iterator it = map.upperBound(xadr.abs);
	int low = xadr.abs & ~0x3fff;
	for (int i = 0; (i < 256) && (it != map.constBegin()); i++) {
		it--;
		if (it.key() < low) break;
		if (!label_local_prefix(it.value())) return it.value();
	}
	return QString();
}

// A LABELSLIST page the way SjASMPlus writes it: decimal (a page with a hex
// digit in it, from Xpeccy+ before 2026.8.2, still reads - dt_place_parse)
static QString labels_page_str(int page) {
	return QString("%1").arg(page, 2, 10, QChar('0'));
}

// A label at RAM page:offset, in the pages the machine in use has: the Next's
// 8K pages (in its SRAM image, after the ROM and divMMC areas), 16K banks elsewhere
static void labels_add_ram(bool isNext, int page, int ofs, const QString& name) {
	xAdr xadr;
	xadr.type = MEM_RAM;
	if (isNext) {
		if ((page < 0) || (page >= 0xe0)) return;
		xadr.abs = 0x40000 + (page << 13) + (ofs & 0x1fff);
		xadr.bank = xadr.abs >> 14;
		xadr.adr = comp_find_xadr(conf.zx, xadr.type, xadr.abs, MEM_VIEW_READ);
	} else {
		xadr.bank = page;
		xadr.abs = (page << 14) | (ofs & 0x3fff);
		switch (page) {		// where a 128K sees it
			case 0x05: xadr.adr = 0x4000 | (ofs & 0x3fff); break;
			case 0x02: xadr.adr = 0x8000 | (ofs & 0x3fff); break;
			default: xadr.adr = 0xc000 | (ofs & 0x3fff); break;
		}
	}
	add_label(xadr, name);
}

// A label at a CPU address: the Next keeps it there whatever is paged in, other
// machines take it into the 128K's bank behind it
static void labels_add_cpu(bool isNext, int adr, const QString& name) {
	if (isNext) {
		xAdr xadr;
		xadr.type = MEM_IO;
		xadr.abs = adr;
		xadr.adr = adr;
		xadr.bank = -1;
		add_label(xadr, name);
		return;
	}
	static const int bank[4] = {0, 5, 2, 0};
	labels_add_ram(false, bank[(adr >> 14) & 3], adr, name);
}

static bool labels_hex(const QString& str, int len, int* val) {
	bool ok;
	*val = str.toInt(&ok, 16);
	return ok && (str.size() == len);
}

// A SjASMPlus CSPECTMAP line: "logical linear type name", the linear address over
// the device's RAM - page * page size + offset, so it needs no page size to place.
// Only labels (type 00) and ROM / no-device ones (03, a CPU address) are kept;
// EQU, DEFL and STRUCT values are not places. false: not a map line.
static bool labels_map_line(bool isNext, const QString& line) {
	QStringList arr = line.simplified().split(' ');
	int logical, linear, type;
	if ((arr.size() < 4) || !labels_hex(arr.at(0), 8, &logical) || !labels_hex(arr.at(1), 8, &linear)
			|| !labels_hex(arr.at(2), 2, &type))
		return false;
	if (type == 0) {
		if (isNext) labels_add_ram(true, linear >> 13, linear & 0x1fff, arr.at(3));
		else labels_add_ram(false, linear >> 14, linear & 0x3fff, arr.at(3));
	} else if (type == 3) {
		labels_add_cpu(isNext, logical & 0xffff, arr.at(3));
	}
	return true;
}

// ROM names a file of the user's gave its own rom records: name -> the MD5s
static QHash<QString, QSet<QByteArray> > userRoms;

// the banks of the machine's ROM a ROM name of the lists (or the user's) is
static QList<int> rom_name_banks(const QString& mem) {
	QList<int> res;
	for (int b = 0; b < romBanks.size(); b++) {
		bool hit = userRoms.value(mem).contains(romBanks.at(b));
		for (int i = 0; !hit && (i < romLists.size()); i++)
			hit = (romLists.at(i).mem == mem) && romLists.at(i).banks.contains(romBanks.at(b));
		if (hit) res.append(b);
	}
	return res;
}

static xAdr cell_xadr(int type, int abs) {
	xAdr xadr;
	xadr.type = type;
	xadr.abs = abs;
	xadr.bank = abs >> 14;
	xadr.adr = conf.zx ? comp_find_xadr(conf.zx, type, abs, MEM_VIEW_READ) : -1;
	return xadr;
}

// The cells a place of the text format stands for in the machine in use: one, or
// one in each bank a ROM name is in. A CPU address is the cell the CPU sees there
// now; a label's name, its cell. Empty: none
QList<xAdr> labels_place_cells(const dtPlace& p) {
	QList<xAdr> res;
	Computer* comp = conf.zx;
	if (!comp) return res;
	int type, abs;
	switch (p.kind) {
		case DT_PLACE_CPU:
			res.append(comp_get_xadr(comp, p.ofs & 0xffff, MEM_VIEW_READ));
			break;
		case DT_PLACE_PAGE:
			if (comp_cell_xadr(comp, CELL_RAM, p.page, p.ofs, &type, &abs)) res.append(cell_xadr(type, abs));
			break;
		case DT_PLACE_AREA:
			if (comp_cell_xadr(comp, p.area, p.page, p.ofs, &type, &abs)) res.append(cell_xadr(type, abs));
			break;
		case DT_PLACE_ROM:
			if (p.ofs < MEM_16K) {
				foreach(int b, rom_name_banks(p.name)) res.append(cell_xadr(MEM_ROM, b * MEM_16K + p.ofs));
			}
			break;
		case DT_PLACE_NAME: {
			xAdr xadr = find_label_cell(p.name);
			if (xadr.type == MEM_IO) xadr = comp_get_xadr(comp, xadr.abs & 0xffff, MEM_VIEW_READ);
			if (xadr.type >= 0) res.append(xadr);
			break;
		}
	}
	return res;
}

// ...and back: a cell as the text format writes it - RAM as its page in the
// machine's own pages, other memory as its area's token, a CPU label's address.
// DT_PLACE_NONE: a cell with no name (the SRAM behind the Next's banks 5 and 7)
dtPlace labels_cell_place(int type, int abs) {
	dtPlace p;
	dt_place_clear(&p);
	if (type == MEM_IO) {
		p.kind = DT_PLACE_CPU;
		p.ofs = abs & 0xffff;
		return p;
	}
	if (!conf.zx) return p;
	xCell cell = comp_cell(conf.zx, type, abs);
	if (cell.area == CELL_NONE) return p;
	p.kind = (cell.area == CELL_RAM) ? DT_PLACE_PAGE : DT_PLACE_AREA;
	p.area = cell.area;
	p.page = cell.page;
	p.ofs = cell.ofs;
	return p;
}

// a cell's flag byte (breakpoint bits, view, statement start); NULL if it has none
static unsigned char* cell_flag(Computer* comp, int type, int abs) {
	switch (type) {
		case MEM_RAM: return comp->brkRamMap + (abs & comp->mem->ramMask);
		case MEM_ROM: return comp->brkRomMap + (abs & comp->mem->romMask);
		case MEM_SLOT: return comp->slot->brkMap ? comp->slot->brkMap + (abs & comp->slot->memMask) : NULL;
		case MEM_DIV: return comp->div ? comp->div->brkMap + (abs % (DIV_EE_SIZE + DIV_RAM_MAX)) : NULL;
	}
	return NULL;
}

// A data record onto the cells: their view, and where each statement starts. A
// CPU address goes cell by cell as the CPU sees them now; other places run on in
// their memory. false: no cell there
static bool labels_mark(const dtRecord& rec) {
	Computer* comp = conf.zx;
	QList<xAdr> cells = labels_place_cells(rec.at);
	if (cells.isEmpty()) return false;
	int step = rec.len * rec.unit;
	int bytes = step * rec.count;
	foreach(const xAdr& start, cells) {
		for (int i = 0; i < bytes; i++) {
			xAdr xadr = start;
			if (rec.at.kind == DT_PLACE_CPU) xadr = comp_get_xadr(comp, (rec.at.ofs + i) & 0xffff, MEM_VIEW_READ);
			else xadr.abs += i;
			unsigned char* fl = cell_flag(comp, xadr.type, xadr.abs);
			if (!fl) continue;
			// dc's strings end themselves: the record starts one statement
			bool first = (i == 0) || (!rec.dc && (rec.view != DBG_VIEW_EXEC) && ((i % step) == 0));
			*fl = (*fl & 0x0f) | rec.view | (first ? DBG_STMT : 0);
		}
	}
	return true;
}

// A label line of a user's file, placed as LABELSLIST's always was: a page in the
// machine's own pages, a CPU address in the 128K's bank behind it (the Next keeps
// it at its address)
static bool labels_text_label(const dtRecord& rec, bool isNext, int* wide) {
	const dtPlace& p = rec.at;
	switch (p.kind) {
		case DT_PLACE_CPU:
			if (p.ofs > 0xffff) return false;
			labels_add_cpu(isNext, p.ofs, rec.name);
			return true;
		case DT_PLACE_PAGE:
			if (p.ofs > 0xffff) return false;
			if (p.ofs > 0x1fff) (*wide)++;
			labels_add_ram(isNext, p.page, p.ofs, rec.name);
			return true;
		default: {
			QList<xAdr> cells = labels_place_cells(p);
			if (cells.isEmpty()) return false;
			add_label(cells.first(), rec.name);
			return true;
		}
	}
}

// One file of labels into the set named after it, which is then the one in
// effect: the Debugger's text format (docs/debugger-format.md), of which
// SjASMPlus's LABELSLIST is a part, or a CSPECTMAP. Its pages are taken as the
// machine in use has them. Data, comments and breakpoints are taken after the
// labels, so a name in them may be a label of the file. 0: it could not be
// opened, and nothing changed.
static int labels_read(const QString& path) {
	QFile file(path);
	if (!file.open(QFile::ReadOnly)) return 0;
	bool isNext = conf.zx && (conf.zx->hw->id == HW_ZXNEXT);
	xLabelSet* set = newLabelSet(QFileInfo(path).fileName());
	set->list.clear();		// before it is made current, or its old map is built for nothing
	setLabelSet(set);
	QByteArray pathb = path.toLocal8Bit();
	int wide = 0;		// offsets past 8K: a 16K-page file
	int unknown = 0;
	int bad = 0;
	int num = 0;
	QList<QPair<int, dtRecord> > later;
	dtRecord rec;
	while(!file.atEnd()) {
		QString line = QString::fromUtf8(file.readLine());
		num++;
		if (labels_map_line(isNext, line)) continue;
		bool ok = dt_parse_line(line, &rec);
		if (ok && (rec.type == DT_REC_LABEL)) ok = labels_text_label(rec, isNext, &wide);
		if (!ok) {
			if (bad++ < 10) xlog(XLG_FILE, XLL_WARN, "labels: %s, line %i: %s", pathb.constData(), num,
				(rec.error.isEmpty() ? line.trimmed() : rec.error).toLocal8Bit().constData());
			continue;
		}
		switch (rec.type) {
			case DT_REC_ROM:
				userRoms[rec.name].insert(rec.md5.toLatin1());
				break;
			case DT_REC_DATA:
			case DT_REC_COMMENT:
			case DT_REC_BRK:
				later.append(qMakePair(num, rec));
				break;
			case DT_REC_UNKNOWN:
				unknown++;
				break;
		}
	}
	int brks = 0;
	for (const auto& it : later) {
		const dtRecord& r = it.second;
		bool ok = true;
		if (r.type == DT_REC_DATA) {
			ok = labels_mark(r);
		} else if (r.type == DT_REC_COMMENT) {
			QList<xAdr> cells = labels_place_cells(r.at);
			foreach(const xAdr& cell, cells) add_comment(cell, r.text);
			ok = !cells.isEmpty();
		} else if (brk_text_record(r, pathb.constData(), it.first, &bad)) {
			brks++;
		}
		if (!ok && (bad++ < 10))
			xlog(XLG_FILE, XLL_WARN, "labels: %s, line %i: no such place here", pathb.constData(), it.first);
	}
	if (brks) brkInstallAll();
	xlog(XLG_FILE, XLL_INFO, "labels from %s", pathb.constData());
	if (unknown) xlog(XLG_FILE, XLL_INFO, "labels: %s: %i lines of a newer kind skipped", pathb.constData(), unknown);
	if (isNext && wide)
		xlog(XLG_FILE, XLL_WARN, "labels: %i offsets past 8K, the file has 16K pages - read as the Next's 8K ones", wide);
	return 1;
}

// A file the user named, on the command line or in the debugger: the one
// Reload reads again. NULL asks for it.
int loadLabels(const char* fn) {
	QString path(fn);
	if (path.isEmpty())
		path = QFileDialog::getOpenFileName(NULL, "Load SJASM labels",QString(),QString(),nullptr,QFileDialog::DontUseNativeDialog);
	if (path.isEmpty()) return 1;
	if (!labels_read(path)) return 0;
	conf.labpath = path;
	return 1;
}

bool labels_named_on() {
	return !conf.labpath.isEmpty() && conf.curlabset
		&& (conf.curlabset->name == QFileInfo(conf.labpath).fileName());
}

// the labels an image came with last, and the set in effect before them
static QString imgSet;
static QString imgBefore;

void labels_image(const QString& image) {
	if (!imgSet.isEmpty()) {
		// a set picked by hand since then stays
		if (conf.curlabset && (conf.curlabset->name == imgSet))
			setLabelSet(findLabelSet(imgBefore));
		imgSet.clear();
	}
	if (!conf.dbg.imglabels || image.isEmpty()) return;
	QString path = arc_beside(image, ".labels");		// an archive's: beside the archive
	if (!QFileInfo(path).isFile()) return;
	QString before = conf.curlabset ? conf.curlabset->name : QString();
	if (!labels_read(path)) return;
	imgBefore = before;
	imgSet = conf.curlabset->name;
}

// One label as a LABELSLIST line: RAM in the machine's pages, the page in decimal
// as SjASMPlus writes it; any other memory as its area's token (ROM0:0123,
// DIV3:0010), which only Xpeccy+ reads back; a CPU address as :ADDR. Empty: a
// cell with no name (the SRAM behind the Next's banks 5 and 7)
static QString labels_line(const QString& name, xAdr xadr) {
	if (xadr.type == MEM_IO) return QString(":%0 %1\n").arg(gethexword(xadr.abs), name);
	if (!conf.zx) return QString();
	xCell cell = comp_cell(conf.zx, xadr.type, xadr.abs);
	if (cell.area == CELL_RAM)
		return QString("%0:%1 %2\n").arg(labels_page_str(cell.page), gethexword(cell.ofs), name);
	if (cell.area == CELL_NONE) return QString();
	char buf[16];
	return QString("%0:%1 %2\n").arg(cell_token(cell, buf), gethexword(cell.ofs), name);
}

int saveLabels(const char* fn) {
	int res = 1;
	QStringList keys;
	QString key;
	QString line;
	QFile file;
	QString path(fn);
	if (path.isEmpty())
		path = QFileDialog::getSaveFileName(NULL, "Save labels",QString(),QString(),nullptr,QFileDialog::DontUseNativeDialog);
	if (path.isEmpty()) {
		res = 0;
	} else {
		file.setFileName(path);
		if (file.open(QFile::WriteOnly)) {
			xLabelSet* set = conf.curlabset;
			if (set) {
				keys = set->list.keys();
				int lost = 0;
				foreach(key, keys) {
					line = labels_line(key, set->list.value(key));
					if (line.isEmpty()) {
						lost++;
					} else {
						file.write(line.toUtf8());
					}
				}
				if (lost) xlog(XLG_FILE, XLL_WARN, "labels: %i not saved, in memory with no name to write", lost);
			}
			file.close();
		} else {
			res = 0;
		}
	}
	return res;
}

// comments

void add_comment(xAdr xadr, QString str) {
	conf.commap[xadr.type][xadr.abs] = str;
}

void del_comment(xAdr xadr) {
	if (conf.commap.contains(xadr.type)) {
		conf.commap[xadr.type].remove(xadr.abs);
	}
}

QString find_comment(xAdr xadr) {
	QString str;
	if (conf.commap.contains(xadr.type)) {
		if (conf.commap[xadr.type].contains(xadr.abs)) {
			str = conf.commap[xadr.type][xadr.abs];
		}
	}
	return str;
}

void clear_comments() {
	conf.commap.clear();
}

// sessions: everything the Debugger knows of the machine's memory in one file of
// the text format (docs/debugger-format.md), the machine and its images if asked

// a map's marks as data records: a statement runs from its first cell to the
// next one that starts a statement or has another view; statements of one
// length one after another make one N*K record
static void session_marks(QByteArray& out, int type, const unsigned char* map, int size, const unsigned char* bytes) {
	int i = 0;
	while (i < size) {
		int view = map[i] & DBG_VIEW_MASK;
		if (!view) {
			i++;
			continue;
		}
		auto stmt_end = [&](int from) {
			int j = from + 1;
			while ((j < size) && ((map[j] & DBG_VIEW_MASK) == view) && !(map[j] & DBG_STMT)) j++;
			return j;
		};
		int start = i;
		i = stmt_end(start);
		int len = i - start;
		int count = 1;
		// the next statements of the same length join it (code's are one each)
		while ((view != DBG_VIEW_EXEC) && (i < size) && ((map[i] & DBG_VIEW_MASK) == view)) {
			int next = stmt_end(i);
			if (next - i != len) break;
			count++;
			i = next;
		}
		dtPlace p = labels_cell_place(type, start);
		if (p.kind == DT_PLACE_NONE) continue;
		int unit = ((view == DBG_VIEW_WORD) || (view == DBG_VIEW_ADDR)) ? 2 : 1;
		if (len / unit < 1) continue;		// a word mark on one byte: not a word
		const char* kw = "db";
		switch (view) {
			case DBG_VIEW_WORD: kw = "dw"; break;
			case DBG_VIEW_ADDR: kw = "da"; break;
			case DBG_VIEW_EXEC: kw = "code"; break;
			case DBG_VIEW_TEXT:
				// one string that ends with bit 7 reads best as dc; the cells are the same
				kw = ((count == 1) && bytes && (bytes[start + len - 1] & 0x80)) ? "dc" : "dm";
				break;
		}
		QString line = QString("%1 %2 %3").arg(kw, dt_place_text(p)).arg(len / unit);
		if (count > 1) line.append(QString("*%1").arg(count));
		out.append(line.append("\n").toUtf8());
	}
}

// a path a session keeps: beside or under the session, relative to it
static QString session_path_text(const QString& session, const char* raw) {
	QString path = QFileInfo(QString::fromLocal8Bit(raw)).absoluteFilePath();
	QString rel = QFileInfo(session).absoluteDir().relativeFilePath(path);
	return rel.startsWith("..") ? QDir::toNativeSeparators(path) : QDir::toNativeSeparators(rel);
}

int session_save(const QString& path, bool machine, bool images) {
	Computer* comp = conf.zx;
	if (!comp) return 0;
	QFile file(path);
	if (!file.open(QFile::WriteOnly)) return 0;
	QByteArray out("; Xpeccy+ Debugger session (docs/debugger-format.md)\n");
	if (machine && !conf.macId.empty()) out.append(QString("machine %1\n").arg(QString::fromLocal8Bit(conf.macId.c_str())).toUtf8());
	if (images) {
		if (comp->tape->path && comp->tape->blkCount)
			out.append(QString("open %1\n").arg(session_path_text(path, comp->tape->path)).toUtf8());
		for (int i = 0; i < 4; i++) {
			Floppy* flp = comp->dif->flp[i];
			if (flp->insert && flp->path)
				out.append(QString("open %1: %2\n").arg(QChar('A' + i)).arg(session_path_text(path, flp->path)).toUtf8());
		}
		if (comp->slot->path)
			out.append(QString("open %1\n").arg(session_path_text(path, comp->slot->path)).toUtf8());
	}
	int lost = 0;
	if (conf.curlabset) {
		foreach(const QString& key, conf.curlabset->list.keys()) {
			QString line = labels_line(key, conf.curlabset->list.value(key));
			if (line.isEmpty()) lost++;
			else out.append(line.toUtf8());
		}
	}
	session_marks(out, MEM_RAM, comp->brkRamMap, comp->mem->ramMask + 1, comp->mem->ramData);
	session_marks(out, MEM_ROM, comp->brkRomMap, comp->mem->romMask + 1, comp->mem->romData);
	if (comp->slot->brkMap) session_marks(out, MEM_SLOT, comp->slot->brkMap, comp->slot->memMask + 1, NULL);
	if (comp->div) session_marks(out, MEM_DIV, comp->div->brkMap, DIV_EE_SIZE + DIV_RAM_MAX, NULL);
	for (auto tp = conf.commap.constBegin(); tp != conf.commap.constEnd(); tp++) {
		for (auto it = tp->constBegin(); it != tp->constEnd(); it++) {
			dtPlace p = labels_cell_place(tp.key(), it.key());
			if (p.kind == DT_PLACE_NONE) {
				lost++;
				continue;
			}
			out.append(QString("comment %1 %2\n").arg(dt_place_text(p), dt_escape(it.value())).toUtf8());
		}
	}
	for (const xBrkPoint& brk : conf.brk.list) {
		QString line = brk_text_line(brk);
		if (line.isEmpty()) lost++;
		else out.append(line.append("\n").toUtf8());
	}
	if (lost) xlog(XLG_FILE, XLL_WARN, "session: %i not saved, in memory with no name to write", lost);
	bool ok = (file.write(out) == out.size());
	file.close();
	return ok ? 1 : 0;
}

// The machine and the images a session names, for the window that puts them in:
// media gets "DRIVE<tab>PATH", the drive -1 for the deck or the slot (where the
// file says), the path made whole against the session's folder
void session_media(const QString& path, QString* machine, QStringList* media) {
	machine->clear();
	media->clear();
	QFile file(path);
	if (!file.open(QFile::ReadOnly)) return;
	QDir dir = QFileInfo(path).absoluteDir();
	dtRecord rec;
	while (!file.atEnd()) {
		if (!dt_parse_line(QString::fromUtf8(file.readLine()), &rec)) continue;
		if (rec.type == DT_REC_MACHINE) {
			*machine = rec.name;
		} else if (rec.type == DT_REC_OPEN) {
			QString img = rec.text;
			int drv = -1;
			// A: to D: and a blank: a drive (C:\ is a path)
			if ((img.size() > 3) && (img.at(1) == ':') && img.at(2).isSpace()) {
				drv = img.at(0).toUpper().unicode() - 'A';
				if ((drv < 0) || (drv > 3)) continue;
				img = img.mid(3).trimmed();
			}
			media->append(QString("%1\t%2").arg(drv).arg(QDir::cleanPath(dir.absoluteFilePath(img))));
		}
	}
}

// A session in place of what the Debugger had: its marks, comments and
// breakpoints go, the session's come; its labels are the set named after it
int session_load(const QString& path) {
	Computer* comp = conf.zx;
	if (!comp || !QFileInfo(path).isFile()) return 0;
	{
		EmuLock lock;
		auto unmark = [](unsigned char* map, int size) {
			for (int i = 0; i < size; i++) map[i] &= 0x0f;
		};
		unmark(comp->brkRamMap, comp->mem->ramMask + 1);
		unmark(comp->brkRomMap, comp->mem->romMask + 1);
		if (comp->slot->brkMap) unmark(comp->slot->brkMap, comp->slot->memMask + 1);
		if (comp->div) unmark(comp->div->brkMap, DIV_EE_SIZE + DIV_RAM_MAX);
		clear_comments();
		conf.brk.list.clear();
	}
	int res = labels_read(path);
	brkInstallAll();
	return res;
}

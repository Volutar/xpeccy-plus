#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QTextStream>

#include "xcore.h"
#include "../filer.h"
#include "../xgui/xgui.h"
#include "autostart.h"
#include "vfat_scan.h"
#include "vscalers.h"

#include <fstream>
#include <algorithm>

// Machines: what a machine is, and the one that is running.
//
// A definition says what a machine is - one file per machine, named by its id.
// The built-in ones ship in the binary's resources; machines/ in the config
// directory holds the user's. A user file whose id is a built-in one is a patch
// *over* it, not a replacement: the machine keeps taking the fixes an update
// brings and carries what the user changed on top. A user file with an id of
// its own is a machine of its own, inheriting the one it was made from. Either
// way that file is the only place the user's settings live - there is nothing
// about a machine in config.conf. The last part of this file brings a
// pre-machines profile, and a pre-patch config, across.
// See docs/machine-format.md for the format.

#define	MAC_DIR		"machines"
#define	MAC_SUFFIX	".conf"

// Every machine as the user has it, and - built on demand, since only the one
// that is running is ever asked for - the same machine without its patch, which
// is what a patch is written against. A machine of the user's own has no
// built-in half, so its "stock" is the machine it inherits.
static QList<xMachine> macList;
static QMap<QString, xMachine> macStock;

static QString xm_user_path(const std::string& id) {
	return xres_dir(MAC_DIR) + SLASH + QString::fromLocal8Bit(id.c_str()) + MAC_SUFFIX;
}

// one key of one file, kept as read so inherit can be resolved before any of
// it is applied

typedef struct {
	std::string sect;
	std::string name;
	std::string val;
} xMacLine;

typedef struct {
	QList<xMacLine> lines;
} xMacFile;

// Both stay loaded for the life of the process: which ids are in them is what
// tells a machine that ships from one of the user's own, and xm_stock() builds
// from them long after the list is up.
static QMap<QString, xMacFile> macSrc;		// what ships
static QMap<QString, xMacFile> macUsr;		// what the user wrote

// value vocabularies

typedef struct {
	const char* name;
	int val;
} xMacWord;

static xMacWord psgTypeTab[] = {
	{"none", SND_NONE}, {"ay", SND_AY}, {"ym", SND_YM}, {"ym2203", SND_YM2203}, {NULL, 0}
};

static xMacWord stereoTab[] = {
	{"mono", AY_MONO}, {"abc", AY_ABC}, {"acb", AY_ACB}, {"bac", AY_BAC},
	{"bca", AY_BCA}, {"cab", AY_CAB}, {"cba", AY_CBA}, {NULL, 0}
};

static xMacWord sdrvTab[] = {
	{"none", SDRV_NONE}, {"covox", SDRV_COVOX},
	{"soundrive1", SDRV_105_1}, {"soundrive2", SDRV_105_2}, {NULL, 0}
};

static xMacWord diskTab[] = {
	{"none", DIF_NONE}, {"trdos", DIF_BDI}, {"plus3", DIF_P3DOS}, {NULL, 0}
};

static xMacWord ideTab[] = {
	{"none", IDE_NONE}, {"nemo", IDE_NEMO}, {"nemo-a8", IDE_NEMOA8},
	{"nemo-evo", IDE_NEMO_EVO}, {"smuc", IDE_SMUC}, {"atm", IDE_ATM},
	{"profi", IDE_PROFI}, {NULL, 0}
};

// what a machine's PC keyboard talks; "none" leaves the core's own type alone
static xMacWord scanTab[] = {
	{"none", 0}, {"xt", KBD_XT}, {"at", KBD_AT}, {"ps2", KBD_PS2}, {NULL, 0}
};

static xMacWord resetTab[] = {
	{"basic128", RES_128}, {"basic48", RES_48},
	{"dos", RES_DOS}, {"shadow", RES_SHADOW}, {NULL, 0}
};

// how much of the last out #FE the machine hears back on the ear input
static xMacWord earTab[] = {
	{"none", EAR_NONE}, {"2", EAR_ISSUE2}, {"3", EAR_ISSUE3}, {NULL, 0}
};

// what a port nothing answers reads back
static xMacWord fbusTab[] = {
	{"none", FBUS_NONE}, {"ula", FBUS_ULA},
	{"asic", FBUS_ASIC}, {"attr", FBUS_ATTR}, {NULL, 0}
};

static int mac_word(xMacWord* tab, const std::string& val, int def, const char* id) {
	for (int i = 0; tab[i].name; i++) {
		if (!strcmp(tab[i].name, val.c_str())) return tab[i].val;
	}
	xlog(XLG_CONF, XLL_WARN, "machine %s: unknown value '%s'", id, val.c_str());
	return def;
}

// file

static QList<xMacLine> mac_read(const QString& path) {
	QList<xMacLine> res;
	QFile file(path);		// QFile, not ifstream: the built-in ones are resources
	if (!file.open(QFile::ReadOnly | QFile::Text)) {
		xlog(XLG_CONF, XLL_ERROR, "can't read machine %s", path.toLocal8Bit().data());
		return res;
	}
	QTextStream stream(&file);
	std::string sect;
	xMacLine ln;
	while (!stream.atEnd()) {
		QString line = stream.readLine().section('#', 0, 0).section(';', 0, 0).trimmed();
		if (line.isEmpty()) continue;
		if (line.startsWith('[')) {
			sect = line.mid(1, line.indexOf(']') - 1).trimmed().toLower().toLocal8Bit().data();
			continue;
		}
		if (!line.contains('=')) continue;
		std::pair<std::string,std::string> spl = splitline(line.toLocal8Bit().data());
		ln.sect = sect;
		ln.name = spl.first;
		ln.val = spl.second;
		res << ln;
	}
	return res;
}

// rom

// file[:foffset[:fsize]], both in KB, 0 size = as far as the file reaches

// an empty file name empties the bank, which is how a child or the user says
// "there is nothing here" over a set that has something

static void mac_rom_add(QList<xRomFile>& roms, const std::string& val, int bank) {
	std::vector<std::string> part = splitstr(val, ":");
	if (part.empty() || part[0].empty()) {
		for (int i = 0; i < roms.size(); i++) {
			if (roms[i].roffset == bank * 16) {
				roms.removeAt(i);
				return;
			}
		}
		return;
	}
	xRomFile rom;
	rom.name = part[0];
	rom.foffset = (part.size() > 1) ? atoi(part[1].c_str()) : 0;
	rom.fsize = (part.size() > 2) ? atoi(part[2].c_str()) : 0;
	rom.roffset = bank * 16;
	for (int i = 0; i < roms.size(); i++) {
		if (roms[i].roffset == rom.roffset) {
			roms[i] = rom;		// a child names the banks it changes
			return;
		}
	}
	roms << rom;
}

// where a key of the flat block goes in a definition file: its section,
// and the name it has there

static const struct {
	const char* key;
	const char* sect;
} macSectTab[] = {
	{"hw", "machine"}, {"cpu", "machine"}, {"cpu.frq", "machine"},
	{"memory", "machine"}, {"ram.cold", "machine"}, {"ram.noise", "machine"},
	{"reset", "machine"}, {"contio", "machine"}, {"issue", "machine"},
	{"contmem", "machine"}, {"scrp.wait", "machine"},
	{"geometry", "video"}, {"contPattern", "video"}, {"earlyTiming", "video"},
	{"4t-border", "video"}, {"ULAplus", "video"}, {"DDpal", "video"},
	{"snow", "video"}, {"snow.crash", "video"}, {"floatbus", "video"},
	{"psg.count", "sound"}, {"psg.type", "sound"}, {"psg.frq", "sound"},
	{"psg.stereo", "sound"}, {"gs", "sound"},
	{"saa", "sound"}, {"soundrive", "sound"},
	{"disk", "storage"}, {"ide", "storage"},
	{"mouse", "input"}, {"mouse.wheel", "input"}, {"joy.buttons", "input"},
	{"kbd.scantab", "input"},
	{NULL, NULL}
};

static QString mac_key_place(const QString& key, QString* sect) {
	if (key.startsWith("rom")) {		// rom0..rom3, rom.gs, rom.font
		*sect = "rom";
		return key.startsWith("rom.") ? key.mid(4) : key;
	}
	*sect = "machine";
	for (int i = 0; macSectTab[i].key; i++) {
		if (key == macSectTab[i].key) *sect = macSectTab[i].sect;
	}
	return key;
}

// one line of a machine file, and the same thing out of a flat key: the table
// above is the only place that says where a key lives

static xMacLine mac_line(const char* sect, const char* name, const std::string& val) {
	xMacLine ln;
	ln.sect = sect;
	ln.name = name;
	ln.val = val;
	return ln;
}

static xMacLine mac_line_of_key(const QString& key, const std::string& val) {
	QString sect;
	QString nam = mac_key_place(key, &sect);	// fills sect, so not inline
	return mac_line(sect.toLocal8Bit().data(), nam.toLocal8Bit().data(), val);
}

static void mac_line_set(QList<xMacLine>& lines, const xMacLine& ln) {
	for (int i = 0; i < lines.size(); i++) {
		if ((lines[i].sect == ln.sect) && (lines[i].name == ln.name)) {
			lines[i] = ln;
			return;
		}
	}
	lines << ln;
}

static void mac_defaults(xMachine& mac) {
	mac.memory = 128;
	mac.cpu = "Z80";
	mac.cpufrq = 3500000;
	mac.resbank = RES_128;
	mac.earback = EAR_ISSUE3;
	mac.contio = 0;
	mac.contmem = 0;
	mac.scrpwait = 0;
	mac.contPattern = 0;
	mac.early = 0;
	mac.brd4t = 0;
	mac.snow = 0;
	mac.snowcrash = 0;
	mac.floatbus = FBUS_NONE;
	mac.ramCold.clear();
	mac.ramNoise = 0;
	mac.psgCount = 1;
	mac.psgType = SND_AY;
	mac.psgFrq = 0;
	mac.psgStereo = AY_MONO;
	mac.soundrive = SDRV_NONE;
	mac.disk = DIF_NONE;
	mac.ide = IDE_NONE;
	mac.mouse = 0;
	mac.mouseWheel = 0;
	mac.joyButtons = 0;
	mac.scantab = 0;
	mac.gs = 0;
	mac.saa = 0;
	mac.ulaplus = 0;
	mac.ddpal = 0;
	mac.romBanks = 4;
}

static void mac_apply(xMachine& mac, const QList<xMacLine>& lines) {
	const char* id = mac.id.c_str();
	xArg arg;
	foreach(const xMacLine& ln, lines) {
		const std::string& nam = ln.name;
		const std::string& val = ln.val;
		arg.s = val.c_str();
		arg.b = str2bool(val) ? 1 : 0;
		arg.i = strtol(arg.s, NULL, 0);
		arg.d = strtod(arg.s, NULL);
		if (ln.sect == "machine") {
			if (nam == "name") mac.name = val;
			else if (nam == "family") mac.family = val;
			else if (nam == "hw") mac.hw = val;
			else if (nam == "cpu") mac.cpu = val;
			else if (nam == "memory") mac.memory = arg.i;
			else if (nam == "ram.cold") mac.ramCold = val;
			else if (nam == "ram.noise") mac.ramNoise = toLimits(arg.i, 0, 1000);
			else if (nam == "cpu.frq") mac.cpufrq = arg.i;
			else if (nam == "reset") mac.resbank = mac_word(resetTab, val, RES_128, id);
			else if (nam == "issue") mac.earback = mac_word(earTab, val, EAR_ISSUE3, id);
			else if (nam == "contio") mac.contio = arg.b;
			else if (nam == "contmem") mac.contmem = arg.b;
			else if (nam == "scrp.wait") mac.scrpwait = arg.b;
			else if (nam != "inherit")	// mac_build's, and it is done with
				xlog(XLG_CONF, XLL_WARN, "machine %s: unknown setting '%s'", id, nam.c_str());
		} else if (ln.sect == "video") {
			if (nam == "geometry") mac.geometry = val;
			else if (nam == "contPattern") mac.contPattern = arg.i;
			else if (nam == "earlyTiming") mac.early = arg.b;
			else if (nam == "4t-border") mac.brd4t = arg.b;
			else if (nam == "snow") mac.snow = arg.b;
			else if (nam == "snow.crash") mac.snowcrash = arg.b;
			else if (nam == "floatbus") mac.floatbus = mac_word(fbusTab, val, FBUS_NONE, id);
			else if (nam == "ULAplus") mac.ulaplus = arg.b;
			else if (nam == "DDpal") mac.ddpal = arg.b;
		} else if (ln.sect == "sound") {
			if (nam == "psg.count") mac.psgCount = toLimits(arg.i, 0, 3);
			else if (nam == "psg.type") mac.psgType = mac_word(psgTypeTab, val, SND_AY, id);
			else if (nam == "psg.frq") mac.psgFrq = arg.d;
			else if (nam == "psg.stereo") mac.psgStereo = mac_word(stereoTab, val, AY_MONO, id);
			else if (nam == "soundrive") mac.soundrive = mac_word(sdrvTab, val, SDRV_NONE, id);
			else if (nam == "gs") mac.gs = arg.b;
			else if (nam == "saa") mac.saa = arg.b;
		} else if (ln.sect == "storage") {
			if (nam == "disk") mac.disk = mac_word(diskTab, val, DIF_NONE, id);
			else if (nam == "ide") mac.ide = mac_word(ideTab, val, IDE_NONE, id);
		} else if (ln.sect == "input") {
			if (nam == "mouse") mac.mouse = arg.b;
			else if (nam == "mouse.wheel") mac.mouseWheel = arg.b;
			else if (nam == "joy.buttons") mac.joyButtons = arg.b;
			else if (nam == "kbd.scantab") mac.scantab = mac_word(scanTab, val, 0, id);
		} else if (ln.sect == "rom") {
			if (nam == "banks") mac.romBanks = toLimits(arg.i, 1, 4);
			else if (nam == "gs") mac.roms.gsFile = val;
			else if (nam == "font") mac.roms.fntFile = val;
			else if ((nam.compare(0, 3, "rom") == 0) && isdigit(nam[3]))
				mac_rom_add(mac.roms.roms, val, atoi(nam.c_str() + 3));
			else
				xlog(XLG_CONF, XLL_WARN, "machine %s: unknown rom key '%s'", id, nam.c_str());
		}
	}
}

static bool mac_known(const QString& id) {
	return macSrc.contains(id) || macUsr.contains(id);
}

// A machine is what it inherits, then what ships under its id, then the user's
// patch on top. With `withUser` off the last step is left out, and that is the
// machine a patch is written against - for a machine of the user's own, whose
// id ships nothing, simply the machine it inherits.
//
// What is inherited is always the parent *as it ships*. Inheritance is how the
// definitions are written - a +2 is a 128K with a couple of lines changed - and
// it is not something the user asked for, so changing the 128K must not move
// the +2 with it. For the same reason a machine of the user's own is a snapshot:
// it is written out against the stock parent, carrying what the parent was
// patched with as its own, and stays put when that patch changes later.

static xMachine mac_build(const QString& id, int depth, bool withUser) {
	xMachine mac;
	const QList<xMacLine>& lines = macSrc.value(id).lines;
	const QList<xMacLine>& mine = macUsr.value(id).lines;
	QString parent;
	foreach(const xMacLine& ln, lines) {
		if ((ln.sect == "machine") && (ln.name == "inherit"))
			parent = QString::fromLocal8Bit(ln.val.c_str());
	}
	foreach(const xMacLine& ln, mine) {		// the user's file has the last word
		if ((ln.sect == "machine") && (ln.name == "inherit"))
			parent = QString::fromLocal8Bit(ln.val.c_str());
	}
	if (parent.isEmpty()) {
		mac_defaults(mac);
	} else if (!mac_known(parent) || (depth > 8)) {
		xlog(XLG_CONF, XLL_ERROR, "machine %s: can't inherit %s",
			id.toLocal8Bit().data(), parent.toLocal8Bit().data());
		mac_defaults(mac);
	} else {
		mac = mac_build(parent, depth + 1, false);	// roms and all
	}
	mac.id = id.toLocal8Bit().data();
	mac.parent = parent.toLocal8Bit().data();
	mac_apply(mac, lines);
	if (withUser) mac_apply(mac, mine);
	if (mac.name.empty()) mac.name = mac.id;
	return mac;
}

// load

static void mac_scan_dir(const QString& dir, QMap<QString, xMacFile>& dst) {
	xMacFile mf;
	foreach(QString name, QDir(dir).entryList(
			QStringList() << ("*" MAC_SUFFIX), QDir::Files, QDir::Name)) {
		mf.lines = mac_read(dir + "/" + name);
		dst[QFileInfo(name).completeBaseName()] = mf;
	}
}

static void mac_scan() {
	mac_scan_dir(xres_root(MAC_DIR), macSrc);
	mac_scan_dir(xres_dir(MAC_DIR), macUsr);
}

// the machine list reads in the order the cores are in, which phase 1 put in
// lineage order - the Sinclair machines, then the clones that came from them

extern "C" tabHwItem tabHwPtr[];

static int mac_core_order(const std::string& hw) {
	for (int i = 0; tabHwPtr[i].id != HW_NULL; i++) {
		if (tabHwPtr[i].core && (hw == tabHwPtr[i].core->name)) return i;
	}
	return 9999;
}

static bool mac_before(const xMachine& a, const xMachine& b) {
	int oa = mac_core_order(a.hw);
	int ob = mac_core_order(b.hw);
	return (oa != ob) ? (oa < ob) : (a.id < b.id);
}

void xm_load_all() {
	macList.clear();
	macStock.clear();
	macSrc.clear();
	macUsr.clear();
	mac_scan();
	QStringList ids = macSrc.keys();
	foreach(QString id, macUsr.keys()) {
		if (!ids.contains(id)) ids << id;
	}
	foreach(QString id, ids) {
		macList << mac_build(id, 0, true);
	}
	std::sort(macList.begin(), macList.end(), mac_before);
	xlog(XLG_CONF, XLL_INFO, "%i machines, %i of them the user's",
		(int)macList.size(), (int)macUsr.size());
}

// The machine as it ships, which is what a patch of the user's is written
// against; for a machine of their own it is the one it inherits. Built when
// asked and kept, since the answer only changes when the list is read again.

const xMachine* xm_stock(std::string id) {
	QString qid = QString::fromLocal8Bit(id.c_str());
	if (!mac_known(qid)) return NULL;
	if (!macStock.contains(qid)) macStock[qid] = mac_build(qid, 0, false);
	return &macStock[qid];
}

bool xm_ships(const std::string& id) {
	return macSrc.contains(QString::fromLocal8Bit(id.c_str()));
}

const QList<xMachine>& xm_list() {
	return macList;
}

const xMachine* xm_find(std::string id) {
	for (int i = 0; i < macList.size(); i++) {
		if (macList[i].id == id) return &macList[i];
	}
	return NULL;
}

// the first machine built on that core. More than one machine can share a
// core - a 48K with a Beta Disk is still a ZX48 - so this answers "what does
// this core usually come as", not "which machine is running".

const xMachine* xm_find_by_core(std::string hw) {
	for (int i = 0; i < macList.size(); i++) {
		if (macList[i].hw == hw) return &macList[i];
	}
	return NULL;
}


#define	PS_NONE		0
#define	PS_MACHINE	1
#define	PS_ROMSET	2
#define	PS_VIDEO	3
#define	PS_SOUND	4
#define	PS_INPUT	5
#define	PS_TAPE		6
#define	PS_DISK		7
#define	PS_IDE		8
#define	PS_SDC		9
#define	PS_SLOT		10
#define	PS_DEBUGA	11

// ------------------------------------------------------------- the machine

// One machine per process, built from its definition with the user's patch
// already in it - so a fix shipped in an update reaches a machine the user has
// been tweaking.
//
// A config written before the patch files kept the same settings in
// [MACHINE.<id>] blocks of config.conf. They are read into this map and written
// out as machine files once, by xm_over_migrate().

typedef QList<QPair<std::string, std::string> > xMacOver;
static QMap<QString, xMacOver> macOver;

// A machine is only worth writing back once it has been built. Until then
// conf.macId is no more than what the config file asked for, while the Computer
// still carries the dummy hardware - and writing that out as "what the user
// changed" would wreck the machine's file. Reading the configuration again
// (import, reset to defaults) puts it back to that state on purpose.

static bool macLive = false;

void xm_drop_running() {
	macLive = false;
}

void xm_over_add(const std::string& id, const std::string& nam, const std::string& val) {
	macOver[QString::fromLocal8Bit(id.c_str())] << qMakePair(nam, val);
}

// what a machine keeps for itself, in nvram/<id>.*

static std::string mac_nv_path(const char* ext) {
	return conf.path.nvDir + SLASH + conf.macId + ext;
}

static void mac_nv_read(const std::string& path, unsigned char* dst, int size, const char* seed) {
	FILE* file = fopen(path.c_str(), "rb");
	if (!file && seed) {			// first run of this machine: give it its own
		copyFile(seed, path.c_str());
		file = fopen(path.c_str(), "rb");
	}
	if (!file) return;
	if (fread(dst, size, 1, file) != 1)
		xlog(XLG_CONF, XLL_WARN, "short read from %s", path.c_str());
	fclose(file);
}

static void mac_nv_write(const std::string& path, unsigned char* src, int size) {
	FILE* file = fopen(path.c_str(), "wb");
	if (!file) return;
	fwrite(src, size, 1, file);
	fclose(file);
}

static void xm_load_nvram() {
	std::string seed = std::string(":/res/nvram/") + conf.macId + ".cmos";
	bool have = QFile::exists(QString::fromLocal8Bit(seed.c_str()));
	mac_nv_read(mac_nv_path(".cmos"), conf.zx->cmos.data, 256, have ? seed.c_str() : NULL);
	mac_nv_read(mac_nv_path(".nvram"), conf.zx->ide->smuc.nv->mem, 256, NULL);
}

void xm_save_nvram() {
	if (conf.macId.empty()) return;
	mac_nv_write(mac_nv_path(".cmos"), conf.zx->cmos.data, 256);
	if (conf.zx->ide->type == IDE_SMUC)
		mac_nv_write(mac_nv_path(".nvram"), conf.zx->ide->smuc.nv->mem, 256);
}

// romset

// a rom the user picked can be anywhere; the ones a machine names are
// relative to the rom directory

std::string xm_rom_path(const std::string& name) {
	if (name.empty()) return name;
	if (QDir::isAbsolutePath(QString::fromLocal8Bit(name.c_str()))) return name;
	return conf.path.romDir + SLASH + name;
}

static void mac_load_rom(Computer* comp, const QList<xRomFile>& roms, const std::string& gsf, const std::string& fntf, bool withfnt) {
	std::string fpath;
	int romsz = MEM_256;
	int fsze;
	FILE* file;
	memset(comp->mem->romData, 0xff, MEM_512K);
	foreach(xRomFile xrf, roms) {
		int foff = xrf.foffset * 1024;
		int roff = xrf.roffset * 1024;
		fpath = xm_rom_path(xrf.name);
		file = fopen(fpath.c_str(), "rb");
		if (!file) {
			xlog(XLG_CONF, XLL_ERROR, "can't load rom file '%s'", fpath.c_str());
			continue;
		}
		if (xrf.fsize <= 0) {			// no size given: as far as the file reaches
			fseek(file, 0, SEEK_END);
			fsze = ftell(file);
			rewind(file);
		} else {
			fsze = xrf.fsize * 1024;
		}
		if (roff + fsze > romsz) {
			romsz = toPower(toLimits(roff + fsze, MEM_256, MEM_512K));
		}
		if (roff + fsze > romsz)
			fsze = romsz - roff;
		if ((foff >= 0) && (roff >= 0) && (roff < MEM_512K) && (fsze > 0)) {
			fseek(file, foff, SEEK_SET);
			if (fread(comp->mem->romData + roff, fsze, 1, file) != 1)
				xlog(XLG_CONF, XLL_WARN, "short read from '%s'", fpath.c_str());
		}
		fclose(file);
	}
	// a machine whose roms are all missing still needs a rom space to page
	if (romsz < MEM_16K) romsz = MEM_16K;
	memSetSize(comp->mem, -1, romsz);
	comp_heat_sync(comp);
	if (gsf.empty()) {
		memset((char*)comp->gs->mem->romData, 0xff, MEM_32K);
	} else {
		fpath = xm_rom_path(gsf);
		file = fopen(fpath.c_str(), "rb");
		if (file) {
			if (fread(comp->gs->mem->romData, MEM_32K, 1, file) != 1)
				xlog(XLG_CONF, XLL_WARN, "short read from '%s'", fpath.c_str());
			fclose(file);
		} else {
			xlog(XLG_CONF, XLL_ERROR, "can't load gs rom '%s'", fpath.c_str());
			memset((char*)comp->gs->mem->romData, 0xff, MEM_32K);
		}
	}
	if (withfnt) {				// else leave the font the machine put there
		if (fntf.empty()) {
			vid_fnt_del(comp->vid);
		} else {
			fpath = xm_rom_path(fntf);
			vid_fnt_load(comp->vid, fpath.c_str());
		}
	}
}

// one bank of a set, by the same rule the definitions use: no name empties it

void xm_rom_set_file(xRomset& rs, int bank, const std::string& name) {
	mac_rom_add(rs.roms, name, bank);
}

// what conf.roms says, into the machine
//
// The text mode font is not rom: it is ram the machine fills itself - ZX Evo
// through b2 of #BF, and its service rom does so at every reset - and the file
// is only what that ram holds at power on. So it is loaded when the machine is
// set up, and when the user picks a different file, but not on an Apply that
// left it alone, which would otherwise wipe the font under a running program.

void xm_set_roms(const xRomset& rs, bool poweron) {
	if (!conf.zx) return;
	emu_lock();				// rom data is rewritten under the running machine
	bool withfnt = poweron || (rs.fntFile != conf.roms.fntFile);
	conf.roms = rs;
	Computer* comp = conf.zx;
	tsSetRomSize(comp->ts, 0);
	mac_load_rom(comp, rs.roms, rs.gsFile, rs.fntFile, withfnt);
	emu_unlock();
}

// What differs from the machine as it ships, as the lines of its file. The key
// is the flat name the vocabulary tables use; mac_line_of_key puts it in its
// section.

static void mac_put(QList<xMacLine>& out, const char* nam, const std::string& val, const std::string& def) {
	if (val != def) out << mac_line_of_key(nam, val);
}

static void mac_put(QList<xMacLine>& out, const char* nam, int val, int def) {
	if (val != def) out << mac_line_of_key(nam, std::string(QString::number(val).toLatin1().data()));
}

static void mac_put(QList<xMacLine>& out, const char* nam, double val, double def) {
	if (fabs(val - def) > 1e-6)
		out << mac_line_of_key(nam, std::string(QString::number(val, 'g', 8).toLatin1().data()));
}

static void mac_put_yn(QList<xMacLine>& out, const char* nam, int val, int def) {
	if (!val != !def) out << mac_line_of_key(nam, std::string(YESNO(val)));
}

// the files that are the user's own, as keys of the machine's block

static void mac_put_roms(QList<xMacLine>& out, const xMachine* base) {
	xRomset def = base->roms;
	foreach(xRomFile rf, conf.roms.roms) {
		int i = 0;
		while ((i < def.roms.size()) && (def.roms[i].roffset != rf.roffset)) i++;
		bool same = (i < def.roms.size()) && (def.roms[i].name == rf.name)
			&& (def.roms[i].foffset == rf.foffset) && (def.roms[i].fsize == rf.fsize);
		if (same) continue;
		QString val = QString::fromLocal8Bit(rf.name.c_str());
		if (rf.foffset || rf.fsize)
			val += QString(":%1:%2").arg(rf.foffset).arg(rf.fsize);
		out << mac_line_of_key(QString("rom%1").arg(rf.roffset / 16),
			std::string(val.toLocal8Bit().data()));
	}
	foreach(xRomFile rf, def.roms) {		// a bank the user emptied
		int i = 0;
		while ((i < conf.roms.roms.size()) && (conf.roms.roms[i].roffset != rf.roffset)) i++;
		if (i >= conf.roms.roms.size())
			out << mac_line_of_key(QString("rom%1").arg(rf.roffset / 16), std::string());
	}
	// "rom." tells these from the sound chip's own gs key
	mac_put(out, "rom.gs", conf.roms.gsFile, def.gsFile);
	mac_put(out, "rom.font", conf.roms.fntFile, def.fntFile);
}

// layout

bool xm_set_layout(std::string nm) {
	xLayout* lay = findLayout(nm);
	if (lay == NULL) return false;
	conf.layName = nm;
	comp_set_layout(conf.zx, &lay->lay);
	vid_set_border(conf.zx->vid, conf.vid.border);
	if ((conf.zx->vid->res.x > 0) && (conf.zx->vid->res.y > 0))
		vid_set_resolution(conf.zx->vid, conf.zx->vid->res.x, conf.zx->vid->res.y);
	return true;
}

// core

// Core names as they were spelled before the machine list was cleaned up. Only
// a config written by an older build carries one; a definition names the new
// name. The other half of the compatibility story, old profile name -> machine
// id, is the migration in config.cpp.

static const struct {
	const char* oldName;
	const char* newName;
} hwAliasTab[] = {
	{"ZX48K",	"ZX48"},
	{"Spectrum +2",	"Plus2A"},	// that core has always been a +2A
	{"Spectrum +3",	"Plus3"},
	{"PentEvo",	"Baseconf"},
	{"TSLab",	"TSConf"},	// TS-Labs is the group, not the machine
	{NULL, NULL}
};

int xm_set_hardware(std::string nm) {
	for (int i = 0; hwAliasTab[i].oldName; i++) {
		if (nm == hwAliasTab[i].oldName) {
			nm = hwAliasTab[i].newName;
			break;
		}
	}
	return compSetHardware(conf.zx, nm.empty() ? NULL : nm.c_str());
}

// the machine's own settings: first its definition, then the user's own block.
// Both go through here, so what a key means is written once.

static int mac_ram_size(int kb, int mask) {
	int sz = kb ? kb : 64;
	sz = toLimits(toPower(sz << 10), MEM_256, MEM_4M);
	if ((mask != 0) && (~mask & sz)) {	// the core has no such size
		sz = MEM_4M;
		while (!(mask & sz) && sz)
			sz >>= 1;
	}
	return sz;
}

static int mac_psg_count(Computer* comp) {
	if (comp->ts->chipA->type == SND_NONE) return 0;
	return (comp->ts->type == TS_ZXNEXT) ? 3 : (comp->ts->type == TS_NEDOPC) ? 2 : 1;
}

static void mac_set_psg(Computer* comp, int count, int type, double frq, int stereo) {
	aymChip* psg[3] = {comp->ts->chipA, comp->ts->chipB, comp->ts->chipC};
	for (int i = 0; i < 3; i++) {
		psg[i]->frq = frq;		// 0: chip_set_type puts the chip's own clock in
		psg[i]->stereo = stereo;
		chip_set_type(psg[i], (i < count) ? type : SND_NONE);
	}
	comp->ts->type = (count > 2) ? TS_ZXNEXT : (count > 1) ? TS_NEDOPC : TS_NONE;
}

static void mac_set_cpu(Computer* comp, const std::string& val) {
	std::pair<std::string,std::string> spl = splitline(val, '@');	// NAME@LIBRARY
	if (spl.second.empty()) {
		cpu_set_type(comp->cpu, spl.first.c_str(), NULL, NULL);
	} else {
		std::string dir = conf.path.plgDir + SLASH + "cpu";
		cpu_set_type(comp->cpu, spl.first.c_str(), dir.c_str(), spl.second.c_str());
	}
}

// What the memory holds when the machine is switched on. Real ram comes up with
// a pattern in it and software sees it: on a ZX Evo the service rom's screen
// comes up striped, and switching video modes leaves specks of the old contents
// behind. `ram.cold` is that pattern, hex bytes repeated over the whole of ram;
// no key at all leaves memory as it was, which is what every machine did before.
// Written when the machine is set up, not on reset - a reset does not clear the
// ram of real hardware either.
//
// Groups are separated by spaces and a group may carry `*N` to repeat it, since
// these patterns are runs of one byte - `ff*8 00*8` is eight ff then eight 00.
// `ram.noise` is how many bytes in a thousand come up wrong in that pattern:
// what a board shows depends on its own ram, so it is a number per machine.
static void mac_cold_ram(Computer* comp, const std::string& pat, int noise) {
	QByteArray bytes;
	foreach(QString grp, QString::fromLatin1(pat.c_str()).split(' ', X_SkipEmptyParts)) {
		int rep = 1;
		int pos = grp.indexOf('*');
		if (pos >= 0) {
			rep = grp.mid(pos + 1).toInt();
			grp = grp.left(pos);
		}
		bytes.append(QByteArray::fromHex(grp.toLatin1()).repeated(rep));
	}
	if (bytes.isEmpty()) return;
	mem_cold_fill(comp->mem, (const unsigned char*)bytes.constData(), bytes.size(), noise);
}

static void mac_from_def(const xMachine* mac) {
	Computer* comp = conf.zx;
	xm_set_hardware(mac->hw);
	mac_set_cpu(comp, mac->cpu);
	compSetBaseFrq(comp, mac->cpufrq / 1e6);
	memSetSize(comp->mem, mac_ram_size(mac->memory, comp->hw->mask), -1);
	mac_cold_ram(comp, mac->ramCold, mac->ramNoise);
	comp->resbank = mac->resbank;
	comp->earback = mac->earback;
	comp->fbus = mac->floatbus;
	comp->flgCNTI = mac->contio;
	comp->flgCNTM = mac->contmem;
	comp->flgEM1 = mac->scrpwait;
	comp->flgDDP = mac->ddpal;
	comp->vid->ula->conttype = mac->contPattern;
	comp->vid->ula->early = mac->early;
	comp->vid->ula->enabled = mac->ulaplus;
	comp->vid->brdstep = mac->brd4t ? 7 : 1;
	comp_set_snow(comp, mac->snow);
	comp->flgSNOWX = mac->snowcrash;
	mac_set_psg(comp, mac->psgCount, mac->psgType, mac->psgFrq, mac->psgStereo);
	comp->gs->enable = mac->gs;
	comp->saa->enabled = mac->saa;
	comp->sdrv->type = mac->soundrive;
	difSetHW(comp->dif, mac->disk);
	ide_set_type(comp->ide, mac->ide);
	comp->mouse->enable = mac->mouse;
	comp->mouse->hasWheel = mac->mouseWheel;
	comp->joy->extbuttons = mac->joyButtons;
	comp->keyb->pcmode = mac->scantab;
	conf.layName = mac->geometry;
}

// the RAM size a machine comes up with, before it is loaded: the value it has
// with the user's patch in it, fitted to what the core can page. mask, when
// asked for, takes every size that core has - the sizes that one is picked
// from, so both come off the same lookup

int xm_ram_size(std::string id, int* mask) {
	if (mask) *mask = 0;
	const xMachine* mac = xm_find(id);
	if (!mac) return 0;
	HardWare* hw = findHardware(mac->hw.c_str());
	if (!hw) return 0;
	if (mask) *mask = hw->mask;
	return mac_ram_size(mac->memory, hw->mask);
}

bool xm_set(std::string id) {
	// before anything: what the user has done to the machine that is running
	// goes into its own file, so coming back finds it there
	xm_save_over();
	const xMachine* mac = xm_find(id);
	if (!mac) {
		xlog(XLG_CONF, XLL_ERROR, "no such machine: %s", id.c_str());
		return false;
	}
	emu_lock();
	conf.emu.pause |= PR_EXTRA;
	// the start and a machine put back to its defaults are not a change of machine
	bool another = !conf.macId.empty() && (conf.macId != id);
	if (!conf.macId.empty()) {			// what the machine we leave keeps
		xm_save_nvram();
		ideCloseFiles(conf.zx->ide);
		sdcCloseFile(conf.zx->sdc);
	}
	conf.macId = id;
	mac_from_def(mac);
	xm_set_roms(mac->roms, true);
	if (!xm_set_layout(conf.layName)) xm_set_layout(LAY_DEFAULT);
	loadPalette();
	xm_load_nvram();
	comp_kbd_release(conf.zx);
	loadKeys();		// a machine with no keyboard puts the joystick on the keys
	mouseReleaseAll(conf.zx->mouse);
	compReset(conf.zx, RES_DEFAULT);
	// The images were closed above, when the machine we came from let go of
	// them. Open them again for this one: what is mounted is a property of the
	// emulator, not of the machine, and it stays mounted across a switch. A
	// folder served as a disk is re-read here, which is the other half of it.
	ide_remount(conf.zx->ide);
	sdc_remount(conf.zx->sdc);
	if (another) {		// says so in the window, whoever asked for it
		static std::string msg;
		msg = " " + mac->name + " ";
		conf.zx->msg = (char*)msg.c_str();
	}
	conf.emu.pause &= ~PR_EXTRA;
	emu_unlock();
	macLive = true;
	xlog(XLG_CONF, XLL_INFO, "machine: %s (%s)", conf.macId.c_str(), conf.zx->hw->name);
	return true;
}

// what to write back: only what differs from the definition, so a machine the
// user never touched carries nothing and takes every fix an update brings.

static const char* mac_word_name(xMacWord* tab, int val) {
	for (int i = 0; tab[i].name; i++) {
		if (tab[i].val == val) return tab[i].name;
	}
	return "none";
}

// is this named romset the machine's own set under another name?

static bool mac_same_roms(const xRomset* rs, const xRomset* set) {
	if (!rs || !set) return false;
	if ((rs->gsFile != set->gsFile) || (rs->fntFile != set->fntFile)) return false;
	if (rs->roms.size() != set->roms.size()) return false;
	for (int i = 0; i < rs->roms.size(); i++) {
		if ((rs->roms[i].name != set->roms[i].name)
			|| (rs->roms[i].roffset != set->roms[i].roffset)
			|| (rs->roms[i].foffset != set->roms[i].foffset)) return false;
	}
	return true;
}

// everything about the machine in use that differs from what it ships with

static void mac_put_all(QList<xMacLine>& out, const xMachine* mac) {
	if (!mac || !conf.zx) return;
	Computer* comp = conf.zx;
	std::string cpu = comp->cpu->core->name;
	if (comp->cpu->lib) cpu += std::string("@") + comp->cpu->libname;
	mac_put(out, "hw", comp->hw->name, mac->hw);
	mac_put(out, "cpu", cpu, mac->cpu);
	mac_put(out, "cpu.frq", int(comp->cpuFrq * 1e6), mac->cpufrq);
	mac_put(out, "memory", comp->mem->ramSize >> 10, mac->memory);
	mac_put(out, "reset", mac_word_name(resetTab, comp->resbank), mac_word_name(resetTab, mac->resbank));
	mac_put(out, "issue", mac_word_name(earTab, comp->earback), mac_word_name(earTab, mac->earback));
	mac_put_yn(out, "contio", comp->flgCNTI, mac->contio);
	mac_put_yn(out, "contmem", comp->flgCNTM, mac->contmem);
	mac_put_yn(out, "scrp.wait", comp->flgEM1, mac->scrpwait);
	mac_put(out, "geometry", conf.layName, mac->geometry);
	mac_put(out, "contPattern", comp->vid->ula->conttype, mac->contPattern);
	mac_put_yn(out, "earlyTiming", comp->vid->ula->early, mac->early);
	mac_put_yn(out, "4t-border", comp->vid->brdstep & 0x06, mac->brd4t);
	mac_put_yn(out, "snow", comp->flgSNOW, mac->snow);
	mac_put_yn(out, "snow.crash", comp->flgSNOWX, mac->snowcrash);
	mac_put(out, "floatbus", mac_word_name(fbusTab, comp->fbus), mac_word_name(fbusTab, mac->floatbus));
	mac_put_yn(out, "ULAplus", comp->vid->ula->enabled, mac->ulaplus);
	mac_put_yn(out, "DDpal", comp->flgDDP, mac->ddpal);
	mac_put(out, "psg.count", mac_psg_count(comp), mac->psgCount);
	if (mac_psg_count(comp) > 0) {		// with no chips there is nothing to keep
		mac_put(out, "psg.type", mac_word_name(psgTypeTab, comp->ts->chipA->type), mac_word_name(psgTypeTab, mac->psgType));
		// an unnamed clock in the definition is the chip type's own
		mac_put(out, "psg.frq", comp->ts->chipA->frq,
			mac->psgFrq ? mac->psgFrq : find_chip_type(mac->psgType)->frq);
		mac_put(out, "psg.stereo", mac_word_name(stereoTab, comp->ts->chipA->stereo),
			mac_word_name(stereoTab, mac->psgStereo));
	}
	mac_put_yn(out, "gs", comp->gs->enable, mac->gs);
	mac_put_yn(out, "saa", comp->saa->enabled, mac->saa);
	mac_put(out, "soundrive", mac_word_name(sdrvTab, comp->sdrv->type), mac_word_name(sdrvTab, mac->soundrive));
	mac_put(out, "disk", mac_word_name(diskTab, comp->dif->type), mac_word_name(diskTab, mac->disk));
	mac_put(out, "ide", mac_word_name(ideTab, comp->ide->type), mac_word_name(ideTab, mac->ide));
	mac_put_yn(out, "mouse", comp->mouse->enable, mac->mouse);
	mac_put_yn(out, "mouse.wheel", comp->mouse->hasWheel, mac->mouseWheel);
	mac_put_yn(out, "joy.buttons", comp->joy->extbuttons, mac->joyButtons);
	mac_put(out, "kbd.scantab", mac_word_name(scanTab, comp->keyb->pcmode), mac_word_name(scanTab, mac->scantab));
	mac_put_roms(out, mac);
}

// WHAT THE USER CHANGED
//
// It lives in the machine's own file under machines/ in the config directory -
// the keys that differ from the machine as it ships, and nothing else. The file
// is written when the settings are applied and when the machine is switched
// away from, so coming back finds them; a machine with nothing of the user's
// left in it has no file at all.

// What a write did. MACW_SAME is the common case - the settings were applied
// again with nothing changed - and it is worth telling apart, because only a
// file that really moved is worth rebuilding the machine list for.

#define	MACW_FAIL	0
#define	MACW_SAME	1
#define	MACW_WROTE	2

static int mac_write(const std::string& id, const QList<xMacLine>& lines) {
	QStringList out;
	out << (xm_ships(id) ? "# What you changed on a machine that ships. Delete this file to take it as it comes."
		: "# A machine of your own. Delete this file to drop it.");
	QMap<QString, QStringList> part;
	foreach(const xMacLine& ln, lines) {
		part[QString::fromLocal8Bit(ln.sect.c_str())] << QString("%1 = %2")
			.arg(QString::fromLocal8Bit(ln.name.c_str()))
			.arg(QString::fromLocal8Bit(ln.val.c_str()));
	}
	const char* sect[] = {"machine", "video", "sound", "storage", "input", "rom", NULL};
	for (int i = 0; sect[i]; i++) {
		if (!part.contains(sect[i])) continue;
		out << "";
		out << QString("[%1]").arg(sect[i]);
		out << part.value(sect[i]);
	}
	out << "";
	QByteArray txt = out.join("\n").toLocal8Bit();
	QFile file(xm_user_path(id));
	if (file.open(QFile::ReadOnly) && (file.readAll() == txt)) return MACW_SAME;
	file.close();
	QDir().mkpath(xres_dir(MAC_DIR));
	if (!file.open(QFile::WriteOnly)) {
		xlog(XLG_CONF, XLL_ERROR, "can't write %s", xm_user_path(id).toLocal8Bit().data());
		return MACW_FAIL;
	}
	file.write(txt);
	file.close();
	return MACW_WROTE;
}

// the running machine, written back into its own file

void xm_save_over() {
	if (!macLive || !conf.zx) return;
	const xMachine* mac = xm_find(conf.macId);
	const xMachine* base = xm_stock(conf.macId);
	if (!mac || !base) return;
	std::string id = conf.macId;		// xm_load_all invalidates mac and base
	bool own = !xm_ships(id);		// its file is the machine, not a patch
	QList<xMacLine> lines;
	if (own) {
		lines << mac_line("machine", "name", mac->name);
		lines << mac_line("machine", "inherit", mac->parent);
	} else if (mac->name != base->name) {
		lines << mac_line("machine", "name", mac->name);
	}
	mac_put_all(lines, base);
	if (lines.isEmpty()) {			// nothing of the user's left
		if (QFile::remove(xm_user_path(id))) xm_load_all();
		return;
	}
	if (mac_write(id, lines) == MACW_WROTE) xm_load_all();
}

// "Restore machine": what the user changed on it goes. A machine that ships
// comes back as it ships; one of the user's own stays in the list and goes back
// to what it inherits.

void xm_reset_over() {
	const xMachine* mac = xm_find(conf.macId);
	if (!mac) return;
	std::string id = conf.macId;
	QList<xMacLine> lines;
	if (!xm_ships(id)) {			// keep the machine, drop only its settings
		lines << mac_line("machine", "name", mac->name);
		lines << mac_line("machine", "inherit", mac->parent);
	}
	xm_drop_running();			// so xm_set does not save what we are dropping
	if (lines.isEmpty()) {
		QFile::remove(xm_user_path(id));
	} else {
		mac_write(id, lines);
	}
	xm_load_all();
	xm_set(id);
}

// A config from before the machine files kept the same settings in
// [MACHINE.<id>] blocks of config.conf. Read once, they are written out as
// machine files - merged into a file the machine may already have.

void xm_over_migrate() {
	if (macOver.isEmpty()) return;
	int done = 0;
	foreach(QString qid, macOver.keys()) {
		const xMacOver& ov = macOver.value(qid);
		if (ov.isEmpty()) continue;
		std::string id = std::string(qid.toLocal8Bit().data());
		if (!xm_find(id)) {
			xlog(XLG_CONF, XLL_WARN, "settings for machine '%s', which is not here", id.c_str());
			continue;
		}
		QList<xMacLine> lines = macUsr.value(qid).lines;
		foreach(const xMacOver::value_type& kv, ov)
			mac_line_set(lines, mac_line_of_key(QString::fromLocal8Bit(kv.first.c_str()), kv.second));
		if (mac_write(id, lines) == MACW_WROTE) done++;
	}
	macOver.clear();
	if (!done) return;
	xm_load_all();
	xlog(XLG_CONF, XLL_INFO, "settings of %i machines moved out of config.conf into %s",
		done, xres_dir(MAC_DIR).toLocal8Bit().data());
}

// --------------------------------------------------------------- the media

// What is mounted stays with the application, not with the machine: switching
// machines keeps the tape and the disks, and only what the new machine cannot
// take goes quiet.

std::string getDiskString(Floppy* flp) {
	std::string res = "40SW";
	if (flp->trk80) res[0] = '8';
	if (flp->doubleSide) res[2] = 'D';
	if (flp->protect) res[3] = 'R';
	if (flp->path) {
		res += ':';
		res += std::string(flp->path);
	}
	return res;
}

static void mac_set_disk(Floppy* flp, std::string st) {
	if (st.size() < 4) return;
	flp->trk80 = (st.substr(0, 2) == "80") ? 1 : 0;
	flp->doubleSide = (st.substr(2, 1) == "D") ? 1 : 0;
	flp->protect = (st.substr(3, 1) == "R") ? 1 : 0;
	if (flp->path || (st.size() < 5) || !conf.storePaths) return;
	st = st.substr(5);
	if (st.size() > 1)
		flp_insert(flp, st.c_str());		// the image itself is loaded once the machine is up
}

// Settings that live on the machine but belong to the user, not to the model:
// what is mounted, and the preferences the machine happens to hold. They are
// collected while the config file is read and applied once the machine is up,
// so building the machine cannot wipe them.

static QList<QPair<std::string, std::string> > macDefer;

void xm_defer(const std::string& nam, const std::string& val) {
	macDefer << qMakePair(nam, val);
}

static void mac_set_defer_key(const std::string& nam, const std::string& val) {
	Computer* comp = conf.zx;
	xArg arg;
	arg.s = val.c_str();
	arg.b = str2bool(val) ? 1 : 0;
	arg.i = strtol(arg.s, NULL, 0);
	arg.d = strtod(arg.s, NULL);
	if (nam == "tape") {
		if (conf.storePaths) tape_set_path(comp->tape, val.c_str());
	} else if ((nam.size() == 6) && (nam.compare(0, 5, "disk.") == 0)) {
		int drv = nam[5] - 'A';
		if ((drv >= 0) && (drv < 4)) mac_set_disk(comp->dif->flp[drv], val);
	} else if (nam == "hdd.master.type") comp->ide->master->type = arg.i;
	else if (nam == "hdd.master.lba") comp->ide->master->hasLBA = arg.b;
	else if (nam == "hdd.master") ide_mount(comp->ide, IDE_MASTER, QString::fromLocal8Bit(arg.s));
	else if (nam == "hdd.slave.type") comp->ide->slave->type = arg.i;
	else if (nam == "hdd.slave.lba") comp->ide->slave->hasLBA = arg.b;
	else if (nam == "hdd.slave") ide_mount(comp->ide, IDE_SLAVE, QString::fromLocal8Bit(arg.s));
	else if (nam == "sdcard") sdc_mount(comp->sdc, QString::fromLocal8Bit(arg.s));
	else if (nam == "sdcard.lock") sdcSetLock(comp->sdc, arg.b);
	else if (nam == "cartrige") {
		if (conf.storePaths) sltSetPath(comp->slot, arg.s);
	}
	else if (nam == "frq.mul") compSetTurbo(comp, (arg.d < 0.1) ? 0.1 : (arg.d > 8.0) ? 8.0 : arg.d);
	else if (nam == "tape.speed") { if ((arg.i > 94) && (arg.i < 106)) comp->tape->speed = arg.i; }
	else if (nam == "psg.frq") {
		aymChip* psg[3] = {comp->ts->chipA, comp->ts->chipB, comp->ts->chipC};
		for (int i = 0; i < 3; i++) {
			psg[i]->frq = arg.d;
			chip_set_type(psg[i], psg[i]->type);	// the period follows the clock
		}
	}
	else if (nam == "psg.stereo") {
		comp->ts->chipA->stereo = arg.i;
		comp->ts->chipB->stereo = arg.i;
		comp->ts->chipC->stereo = arg.i;
	}
	else if (nam == "psg.separation") {
		comp->ts->chipA->sep = arg.i;
		comp->ts->chipB->sep = arg.i;
		comp->ts->chipC->sep = arg.i;
	}
	else if (nam == "gs.reset") comp->gs->reset = arg.b;
	else if (nam == "gs.stereo") comp->gs->stereo = arg.b ? GS_12_34 : GS_MONO;
	else if (nam == "mouse.wheel") comp->mouse->hasWheel = arg.b;
	else if (nam == "mouse.swapButtons") comp->mouse->swapButtons = arg.b;
	else if (nam == "mouse.sensitivity") comp->mouse->sensitivity = arg.d;
	else if (nam == "kbd.scantab") comp->keyb->pcmode = arg.i;
	else if (nam == "ports") setWatchPorts(comp, QString::fromLocal8Bit(arg.s).split(","));
}

// the images themselves, once the machine is built

void xm_finish_load() {
	foreach(xMacOver::value_type kv, macDefer)
		mac_set_defer_key(kv.first, kv.second);
	macDefer.clear();
	if (!conf.storePaths) return;
	Computer* comp = conf.zx;
	if (comp->tape->path)
		load_file(comp, comp->tape->path, FG_TAPE, 0);
	if (comp->slot->path)
		load_file(comp, comp->slot->path, FH_SLOTS, 0);
	for (int i = 0; i < 4; i++) {
		Floppy* flp = comp->dif->flp[i];
		if (flp->path)
			load_file(comp, flp->path, FG_DISK, flp->id);
	}
	// this is the machine being set up as it was left, so nothing here was
	// opened by the user and nothing here is to be started
	media_autorun_forget();
}

void xm_save_media(FILE* file) {
	Computer* comp = conf.zx;
	fprintf(file, "\n[MEDIA]\n\n");
	fprintf(file, "tape = %s\n", comp->tape->path ? comp->tape->path : "");
	for (int i = 0; i < 4; i++)
		fprintf(file, "disk.%c = %s\n", 'A' + i, getDiskString(comp->dif->flp[i]).c_str());
	fprintf(file, "hdd.master.type = %i\n", comp->ide->master->type);
	fprintf(file, "hdd.master.lba = %s\n", YESNO(comp->ide->master->hasLBA));
	fprintf(file, "hdd.master = %s\n", comp->ide->master->image ? comp->ide->master->image : "");
	fprintf(file, "hdd.slave.type = %i\n", comp->ide->slave->type);
	fprintf(file, "hdd.slave.lba = %s\n", YESNO(comp->ide->slave->hasLBA));
	fprintf(file, "hdd.slave = %s\n", comp->ide->slave->image ? comp->ide->slave->image : "");
	fprintf(file, "sdcard = %s\n", comp->sdc->image ? comp->sdc->image : "");
	fprintf(file, "sdcard.lock = %s\n", YESNO(comp->sdc->lock));
	fprintf(file, "cartrige = %s\n", comp->slot->path ? comp->slot->path : "");
}

// ports the debugger watches, as text: "7FFD" is watched, "-7FFD" keeps its
// place in the list switched off. The editor wants them all; the config file
// takes only what the user decided, since the ports the machine brings itself
// are put back by comp_pwatch_sync() anyway

QStringList getWatchPorts(Computer* comp, int all) {
	QStringList res;
	for (int i = 0; i < comp->pwcount; i++) {
		if (!all && comp->pwatch[i].hw && comp->pwatch[i].on) continue;
		QString str = getPortString(comp->pwatch[i].port, comp->pwatch[i].mask);
		res << (comp->pwatch[i].on ? str : "-" + str);
	}
	return res;
}

// the list is built anew every time: a port that leaves takes its history with
// it, and the emulation reads the same array between two instructions

void setWatchPorts(Computer* comp, QStringList ports) {
	int port, mask;
	emu_lock();
	comp_pwatch_clear(comp);
	foreach(QString str, ports) {
		bool on = !str.startsWith('-');
		if (parsePort(on ? str : str.mid(1), &port, &mask))
			comp_pwatch_add(comp, port, mask, on);
	}
	comp_pwatch_sync(comp);
	emu_unlock();
}

// ----------------------------------------------------------- old profiles

// Read once, on the first run of a build that has no profiles any more. The
// old directories are left exactly where they are - nothing here deletes or
// rewrites them.

static const struct {
	const char* profile;
	const char* id;
} macIdTab[] = {
	{"ZX Spectrum 48K",		"zx48"},
	{"ZX Spectrum 48K + TR-DOS",	"zx48"},
	{"ZX Spectrum 128K",		"zx128"},
	{"ZX Spectrum 128K + TR-DOS",	"zx128"},
	{"ZX Spectrum +2",		"zxplus2"},
	{"ZX Spectrum +2A",		"zxplus2a"},
	{"ZX Spectrum +3",		"zxplus3"},
	{"Pentagon 128",		"pent"},
	{"Pentagon 512",		"pent"},	// 512K rides in as an override
	{"Pentagon 1024 SL",		"pent1024"},
	{"Scorpion ZS 256",		"scorp"},
	{"Profi",			"profi"},
	{"ATM Turbo 2+",		"atm2"},
	{"ZXM-Phoenix",			"phoenix"},
	{"ZX Evo (BaseConf)",		"evo-baseconf"},
	{"ZX Evo (TSConf)",		"evo-tsconf"},
	{NULL, NULL}
};

// what the old file said the machine was, when the name is not one of ours

static std::string mac_id_by_core(const std::string& path) {
	std::ifstream file(path);
	std::string line;
	bool inmac = false;
	while (std::getline(file, line)) {
		std::pair<std::string,std::string> spl = splitline(line);
		if (spl.second.empty() && !spl.first.empty() && (spl.first[0] == '[')) {
			inmac = (spl.first == "[MACHINE]") || (spl.first == "[GENERAL]");
			continue;
		}
		if (!inmac || (spl.first != "current")) continue;
		xm_set_hardware(spl.second);		// resolves an old core name too
		const xMachine* mac = xm_find_by_core(conf.zx->hw->name);
		if (mac) return mac->id;
		break;
	}
	return std::string();
}

static std::string mac_id_for(const std::string& name, const std::string& path) {
	std::string id = xm_id_for_name(name);
	return id.empty() ? mac_id_by_core(path) : id;
}

// a machine id, the name an old profile went by, or a machine's display name

std::string xm_id_for_name(const std::string& name) {
	if (xm_find(name)) return name;
	for (int i = 0; macIdTab[i].profile; i++) {
		if (name == macIdTab[i].profile) return macIdTab[i].id;
	}
	foreach(const xMachine& mac, macList) {
		if (mac.name == name) return mac.id;
	}
	return std::string();
}

// one key of an old profile file, onto the machine that is already up

static std::string oldRomset;

static void mac_set_old_key(int sect, const std::string& nam, const std::string& val) {
	Computer* comp = conf.zx;
	xArg arg;
	arg.s = val.c_str();
	arg.b = str2bool(val) ? 1 : 0;
	arg.i = strtol(arg.s, NULL, 0);
	arg.d = strtod(arg.s, NULL);
	switch (sect) {
		case PS_MACHINE:
			if (nam == "current") xm_set_hardware(val);
			else if (nam == "cpu.type") mac_set_cpu(comp, val);
			else if (nam == "cpu.frq") {
				int frq = arg.i;
				if ((frq > 1) && (frq < 58)) frq *= 5e5;	// the old 2..28 field
				compSetBaseFrq(comp, toLimits(frq, 100000, 28000000) / 1e6);
			}
			else if (nam == "frq.mul") compSetTurbo(comp, (arg.d < 0.1) ? 0.1 : (arg.d > 8.0) ? 8.0 : arg.d);
			else if (nam == "memory") memSetSize(comp->mem, mac_ram_size(arg.i, comp->hw->mask), -1);
			else if (nam == "contmem") comp->flgCNTM = arg.b;
			else if (nam == "contio") comp->flgCNTI = arg.b;
			else if (nam == "scrp.wait") comp->flgEM1 = arg.b;
			else if (nam == "lastdir") conf.lastDir = val;
			break;
		case PS_ROMSET:
			if (nam == "current") oldRomset = val;
			else if (nam == "reset") {
				comp->resbank = RES_48;
				if ((val == "basic128") || (val == "0")) comp->resbank = RES_128;
				if ((val == "basic48") || (val == "1")) comp->resbank = RES_48;
				if ((val == "shadow") || (val == "2")) comp->resbank = RES_SHADOW;
				if ((val == "dos") || (val == "3")) comp->resbank = RES_DOS;
			}
			break;
		case PS_VIDEO:
			if (nam == "geometry") conf.layName = val;
			else if (nam == "4t-border") comp->vid->brdstep = arg.b ? 7 : 1;
			else if (nam == "snow") comp_set_snow(comp, arg.b);
			else if (nam == "snow.crash") comp->flgSNOWX = arg.b;
			else if (nam == "ULAplus") comp->vid->ula->enabled = arg.b;
			else if (nam == "contPattern") comp->vid->ula->conttype = arg.i;
			else if (nam == "earlyTiming") comp->vid->ula->early = arg.b;
			else if (nam == "DDpal") comp->flgDDP = arg.b;
			else if (nam == "palette") conf.palette = val;
			break;
		case PS_SOUND:
			if (nam == "psg.count") mac_set_psg(comp, toLimits(arg.i, 0, 3), comp->ts->chipA->type,
					comp->ts->chipA->frq, comp->ts->chipA->stereo);
			else if (nam == "psg.type") mac_set_psg(comp, mac_psg_count(comp), arg.i,
					comp->ts->chipA->frq, comp->ts->chipA->stereo);
			else if ((nam == "psg.frq") || (nam == "psg.stereo") || (nam == "psg.separation")
				|| (nam == "gs.reset") || (nam == "gs.stereo")) xm_defer(nam, val);
			else if (nam == "gs") comp->gs->enable = arg.b;
			else if (nam == "soundrive_type") comp->sdrv->type = arg.i;
			else if (nam == "saa") comp->saa->enabled = arg.b;
			break;
		case PS_TAPE:
			if (nam == "path") xm_defer("tape", val);
			else if ((nam == "speed") && (arg.i > 94) && (arg.i < 106)) comp->tape->speed = arg.i;
			break;
		case PS_DISK:
			if (nam == "type") difSetHW(comp->dif, arg.i);
			else if ((nam.size() == 1) && (nam[0] >= 'A') && (nam[0] <= 'D'))
				xm_defer(std::string("disk.") + nam, val);
			break;
		case PS_IDE:
			if (nam == "iface") ide_set_type(comp->ide, arg.i);
			else if (nam == "master.type") xm_defer("hdd.master.type", val);
			else if (nam == "master.lba") xm_defer("hdd.master.lba", val);
			else if (nam == "master.image") xm_defer("hdd.master", val);
			else if (nam == "slave.type") xm_defer("hdd.slave.type", val);
			else if (nam == "slave.lba") xm_defer("hdd.slave.lba", val);
			else if (nam == "slave.image") xm_defer("hdd.slave", val);
			break;
		case PS_INPUT:
			if (nam == "mouse") comp->mouse->enable = arg.b;
			else if ((nam == "mouse.wheel") || (nam == "mouse.swapButtons")
				|| (nam == "mouse.sensitivity") || (nam == "kbd.scantab")) xm_defer(nam, val);
			else if (nam == "joy.extbuttons") comp->joy->extbuttons = arg.b;
			else if (nam == "keymap") conf.kmapName = val;
			else if (nam == "gamepad.map") conf.jmapNameA = val;
			else if (nam == "gamepad2.map") conf.jmapNameB = val;
			break;
		case PS_SDC:
			if (nam == "sdcimage") xm_defer("sdcard", val);
			else if (nam == "sdclock") xm_defer("sdcard.lock", val);
			break;
		case PS_SLOT:
			if (nam == "path") xm_defer("cartrige", val);
			break;
		case PS_DEBUGA:
			if (nam == "ports") setWatchPorts(comp, QString::fromLocal8Bit(val.c_str()).split(","));
			break;
	}
}

// only what is really there: an absent file must not become an empty one

static void mac_copy_nv(const std::string& name, const std::string& id, const char* ext) {
	std::string src = conf.path.prfDir + SLASH + name + SLASH + name + ext;
	if (!QFile::exists(QString::fromLocal8Bit(src.c_str()))) return;
	copyFile(src.c_str(), (conf.path.nvDir + SLASH + id + ext).c_str());
}

// An old config named a romset out of its own [ROMSETS] table. Unless it holds
// what the machine ships with anyway, its files become the user's own.

static void mac_migrate_romset(const std::string& id) {
	const xMachine* mac = xm_find(id);
	xRomset* rs = oldRomset.empty() ? NULL : findRomset(oldRomset);
	oldRomset.clear();
	if (!mac || !conf.zx) return;
	xm_set_roms(mac->roms, true);	// what it ships with, before the old set
	if (!rs) return;
	if (mac_same_roms(rs, &mac->roms)) return;
	xRomset user = conf.roms;
	user.gsFile = rs->gsFile;
	user.fntFile = rs->fntFile;
	user.roms = rs->roms;
	xm_set_roms(user);
}

// MACHINES OF THE USER'S OWN
//
// The running machine, written to the config directory as a definition that
// inherits the one it came from - so it carries only what the user changed and
// follows a shipped fix in everything else.

// the machines the user owns, by name, relative to the config directory

QStringList xm_user_files() {
	QStringList res;
	foreach(QString id, macUsr.keys())
		res << MAC_DIR "/" + id + MAC_SUFFIX;
	return res;
}

bool xm_is_user_file(const QString& nam) {
	return nam.startsWith(MAC_DIR "/");
}

// a machine of the user's own making, as against one that ships - which may
// still carry a patch of theirs, and that is xm_is_changed()

bool xm_is_users(const std::string& id) {
	return !xm_ships(id) && (xm_find(id) != NULL);
}

bool xm_is_changed(const std::string& id) {
	return macUsr.contains(QString::fromLocal8Bit(id.c_str()));
}

// How a machine reads in a list. A machine that ships is marked when it carries
// something of the user's - a machine of their own is not, since there is no
// "as it ships" to tell it from, and the mark would be on it for ever.

QString xm_list_name(const xMachine& mac) {
	QString res = QString::fromLocal8Bit(mac.name.c_str());
	if (xm_ships(mac.id) && xm_is_changed(mac.id)) res += " *";
	return res;
}

// a file name out of a name a person typed

static std::string xm_id_of_name(const std::string& name) {
	QString res;
	foreach(QChar c, QString::fromLocal8Bit(name.c_str()).toLower()) {
		if (c.isLetterOrNumber()) res += c;
		else if (!res.isEmpty() && !res.endsWith('-')) res += '-';
	}
	while (res.endsWith('-')) res.chop(1);
	if (res.isEmpty()) res = "machine";
	return std::string(res.toLocal8Bit().data());
}

// ...and one nobody is using. The id is a file name and never shown, so a
// number on the end of it is nothing the user has to care about - the name is
// theirs and has to be free, which is the caller's question to ask.

std::string xm_free_id(const std::string& name) {
	std::string base = xm_id_of_name(name);
	std::string id = base;
	for (int i = 2; xm_find(id); i++)
		id = base + "-" + std::string(QString::number(i).toLatin1().data());
	return id;
}

bool xm_name_free(const std::string& name) {
	QString nm = QString::fromLocal8Bit(name.c_str()).trimmed();
	foreach(const xMachine& mac, macList) {
		if (QString::fromLocal8Bit(mac.name.c_str()).compare(nm, Qt::CaseInsensitive) == 0)
			return false;
	}
	return true;
}

// A machine of the user's own: the one that is running, under a name and an id
// of its own, inheriting the machine it was made from - so it follows that one
// wherever it is not told otherwise. The machine it came from is left alone:
// what was changed on it is already its own file.

bool xm_save_as(const std::string& id, const std::string& name) {
	// against the machine it inherits, which is that machine as it ships
	const xMachine* base = xm_stock(conf.macId);
	if (!base || xm_find(id)) return false;
	QList<xMacLine> lines;
	lines << mac_line("machine", "name", name);
	lines << mac_line("machine", "inherit", conf.macId);
	mac_put_all(lines, base);
	if (mac_write(id, lines) == MACW_FAIL) return false;
	xm_load_all();
	return true;
}

// a machine of the user's own goes for good; what it inherited stays where it
// is. A machine that ships is not deleted at all - "restore" drops its patch.

bool xm_delete(const std::string& id) {
	if (!xm_is_users(id)) return false;
	if (!QFile::remove(xm_user_path(id))) return false;
	xm_load_all();
	return true;
}

// name is the old profile's directory, file its .conf inside it

bool xm_migrate(const std::string& name, const std::string& file) {
	std::string path = conf.path.prfDir + SLASH + name + SLASH + file;
	std::string id = mac_id_for(name, path);
	if (id.empty() || !xm_find(id)) {
		xlog(XLG_CONF, XLL_ERROR, "profile '%s': no machine to migrate it to", name.c_str());
		return false;
	}
	if (!xm_set(id)) return false;
	// the machine's own nvram, before anything reads it
	mac_copy_nv(name, id, ".cmos");
	mac_copy_nv(name, id, ".nvram");
	xm_load_nvram();

	std::ifstream ifile(path);
	if (!ifile.good()) {
		xlog(XLG_CONF, XLL_WARN, "profile '%s': no settings file, taking the machine as it ships", name.c_str());
		return true;
	}
	std::string line;
	int sect = PS_NONE;
	while (std::getline(ifile, line)) {
		size_t pos = line.find_first_of("#;");
		if (pos != std::string::npos) line.erase(pos);
		std::pair<std::string,std::string> spl = splitline(line);
		if (spl.second.empty() && !spl.first.empty() && (spl.first[0] == '[')) {
			if ((spl.first == "[MACHINE]") || (spl.first == "[GENERAL]")) sect = PS_MACHINE;
			else if (spl.first == "[ROMSET]") sect = PS_ROMSET;
			else if (spl.first == "[VIDEO]") sect = PS_VIDEO;
			else if (spl.first == "[SOUND]") sect = PS_SOUND;
			else if (spl.first == "[TAPE]") sect = PS_TAPE;
			else if (spl.first == "[DISK]") sect = PS_DISK;
			else if (spl.first == "[IDE]") sect = PS_IDE;
			else if (spl.first == "[INPUT]") sect = PS_INPUT;
			else if (spl.first == "[SDC]") sect = PS_SDC;
			else if (spl.first == "[SLOT]") sect = PS_SLOT;
			else if (spl.first == "[DEBUGA]") sect = PS_DEBUGA;
			else sect = PS_NONE;
			continue;
		}
		if (spl.first.empty()) continue;
		mac_set_old_key(sect, spl.first, spl.second);
	}
	mac_migrate_romset(id);
	if (!xm_set_layout(conf.layName)) xm_set_layout(LAY_DEFAULT);
	loadPalette();
	xlog(XLG_CONF, XLL_INFO, "profile '%s' migrated to machine '%s'; the old profiles/ is left as it is",
		name.c_str(), id.c_str());
	return true;
}

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
// A definition says what a machine is - one file per machine, named by its id,
// read from the binary's own resources and from machines/ in the config
// directory, where a file of the same id shadows the built-in one. The running
// machine is built from a definition plus whatever the user changed on it, and
// the last part of this file brings a pre-machines profile across.
// See docs/machines-plan.md for the format.

#define	MAC_DIR		"machines"
#define	MAC_SUFFIX	".conf"

static QList<xMachine> macList;

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

static QMap<QString, xMacFile> macSrc;

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
	{"reset", "machine"}, {"contio", "machine"},
	{"contmem", "machine"}, {"scrp.wait", "machine"},
	{"geometry", "video"}, {"contPattern", "video"}, {"earlyTiming", "video"},
	{"4t-border", "video"}, {"ULAplus", "video"}, {"DDpal", "video"},
	{"snow", "video"}, {"snow.crash", "video"},
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

static void mac_defaults(xMachine& mac) {
	mac.memory = 128;
	mac.cpu = "Z80";
	mac.cpufrq = 3500000;
	mac.resbank = RES_128;
	mac.contio = 0;
	mac.contmem = 0;
	mac.scrpwait = 0;
	mac.contPattern = 0;
	mac.early = 0;
	mac.brd4t = 0;
	mac.snow = 0;
	mac.snowcrash = 0;
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
	foreach(xMacLine ln, lines) {
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

static xMachine mac_build(const QString& id, int depth) {
	xMachine mac;
	QList<xMacLine> lines = macSrc.value(id).lines;
	QString parent;
	foreach(xMacLine ln, lines) {
		if ((ln.sect == "machine") && (ln.name == "inherit"))
			parent = QString::fromLocal8Bit(ln.val.c_str());
	}
	if (parent.isEmpty()) {
		mac_defaults(mac);
	} else if (!macSrc.contains(parent) || (depth > 8)) {
		xlog(XLG_CONF, XLL_ERROR, "machine %s: can't inherit %s",
			id.toLocal8Bit().data(), parent.toLocal8Bit().data());
		mac_defaults(mac);
	} else {
		mac = mac_build(parent, depth + 1);	// roms and all
	}
	mac.id = id.toLocal8Bit().data();
	mac.parent = parent.toLocal8Bit().data();
	mac_apply(mac, lines);
	if (mac.name.empty()) mac.name = mac.id;
	return mac;
}

// load

static void mac_scan() {
	xMacFile mf;
	foreach(QString name, xres_list(MAC_DIR, QStringList() << ("*" MAC_SUFFIX))) {
		mf.lines = mac_read(xres_path(MAC_DIR, name));
		macSrc[QFileInfo(name).completeBaseName()] = mf;
	}
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
	macSrc.clear();
	mac_scan();
	foreach(QString id, macSrc.keys()) {
		macList << mac_build(id, 0);
	}
	std::sort(macList.begin(), macList.end(), mac_before);
	macSrc.clear();		// only inherit needs the files, and it is done
	xlog(XLG_CONF, XLL_INFO, "%i machines", (int)macList.size());
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

// One machine per process. It is built from its definition, then whatever the
// user changed is applied on top - so a fix shipped in an update reaches a
// machine the user has been tweaking. What the user changed is kept per
// machine id, which is what makes switching away and back remember it, and
// what keeps a machine we never touch from losing what it had.

typedef QList<QPair<std::string, std::string> > xMacOver;
static QMap<QString, xMacOver> macOver;

void xm_over_clear() {
	macOver.clear();
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

static void mac_put(QStringList& out, const char* nam, const std::string& val, const std::string& def) {
	if (val != def) out << QString("%1 = %2").arg(nam).arg(QString::fromLocal8Bit(val.c_str()));
}

static void mac_put(QStringList& out, const char* nam, int val, int def) {
	if (val != def) out << QString("%1 = %2").arg(nam).arg(val);
}

static void mac_put(QStringList& out, const char* nam, double val, double def) {
	if (fabs(val - def) > 1e-6) out << QString("%1 = %2").arg(nam).arg(val, 0, 'g', 8);
}

static void mac_put_yn(QStringList& out, const char* nam, int val, int def) {
	if (!val != !def) out << QString("%1 = %2").arg(nam).arg(YESNO(val));
}

// the files that are the user's own, as keys of the machine's block

static void mac_put_roms(QStringList& out, const xMachine* base) {
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
		out << QString("rom%1 = %2").arg(rf.roffset / 16).arg(val);
	}
	foreach(xRomFile rf, def.roms) {		// a bank the user emptied
		int i = 0;
		while ((i < conf.roms.roms.size()) && (conf.roms.roms[i].roffset != rf.roffset)) i++;
		if (i >= conf.roms.roms.size())
			out << QString("rom%1 = ").arg(rf.roffset / 16);
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
	vid_set_border(conf.zx->vid, brd_mode_for(conf.zx, conf.vid.border));
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

// the machine as the user has it: the definition with what was changed on it
// laid over, read the same way the definition itself is

xMachine xm_with_over(const xMachine& def) {
	xMachine mac = def;
	QList<xMacLine> lines;
	xMacLine ln;
	foreach(xMacOver::value_type kv, macOver.value(QString::fromLocal8Bit(def.id.c_str()))) {
		QString sect;
		QString nam = mac_key_place(QString::fromLocal8Bit(kv.first.c_str()), &sect);
		ln.sect = std::string(sect.toLocal8Bit().data());
		ln.name = std::string(nam.toLocal8Bit().data());
		ln.val = kv.second;
		lines << ln;
	}
	mac_apply(mac, lines);
	return mac;
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

// the RAM size a machine comes up with, before it is loaded: its own value with
// the user's change over it, fitted to what the core can page. mask, when asked
// for, takes every size that core has - the sizes that one is picked from, so
// both come off the same lookup

int xm_ram_size(std::string id, int* mask) {
	if (mask) *mask = 0;
	const xMachine* mac = xm_find(id);
	if (!mac) return 0;
	xMachine cur = xm_with_over(*mac);
	HardWare* hw = findHardware(cur.hw.c_str());
	if (!hw) return 0;
	if (mask) *mask = hw->mask;
	return mac_ram_size(cur.memory, hw->mask);
}

bool xm_set(std::string id) {
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
	xMachine cur = xm_with_over(*mac);
	mac_from_def(&cur);
	xm_set_roms(cur.roms, true);
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

static void mac_put_all(QStringList& out, const xMachine* mac) {
	if (!mac || !conf.zx) return;
	Computer* comp = conf.zx;
	std::string cpu = comp->cpu->core->name;
	if (comp->cpu->lib) cpu += std::string("@") + comp->cpu->libname;
	mac_put(out, "hw", comp->hw->name, mac->hw);
	mac_put(out, "cpu", cpu, mac->cpu);
	mac_put(out, "cpu.frq", int(comp->cpuFrq * 1e6), mac->cpufrq);
	mac_put(out, "memory", comp->mem->ramSize >> 10, mac->memory);
	mac_put(out, "reset", mac_word_name(resetTab, comp->resbank), mac_word_name(resetTab, mac->resbank));
	mac_put_yn(out, "contio", comp->flgCNTI, mac->contio);
	mac_put_yn(out, "contmem", comp->flgCNTM, mac->contmem);
	mac_put_yn(out, "scrp.wait", comp->flgEM1, mac->scrpwait);
	mac_put(out, "geometry", conf.layName, mac->geometry);
	mac_put(out, "contPattern", comp->vid->ula->conttype, mac->contPattern);
	mac_put_yn(out, "earlyTiming", comp->vid->ula->early, mac->early);
	mac_put_yn(out, "4t-border", comp->vid->brdstep & 0x06, mac->brd4t);
	mac_put_yn(out, "snow", comp->flgSNOW, mac->snow);
	mac_put_yn(out, "snow.crash", comp->flgSNOWX, mac->snowcrash);
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

// the settings that differ from what the machine ships with - the ui marks
// them, and "machine defaults" throws them away


void xm_over_forget(const std::string& id) {
	macOver.remove(QString::fromLocal8Bit(id.c_str()));
}

void xm_reset_over() {
	xm_over_forget(conf.macId);
	std::string id = conf.macId;
	conf.macId.clear();			// so xm_set does not save what we are dropping
	xm_set(id);
}

void xm_save(FILE* file) {
	if (conf.macId.empty() || !conf.zx) return;
	QStringList out;
	mac_put_all(out, xm_find(conf.macId));
	if (!out.isEmpty()) {
		fprintf(file, "\n[MACHINE.%s]\n\n", conf.macId.c_str());
		fprintf(file, "%s\n", out.join("\n").toLocal8Bit().data());
	}

	// and the machines the user set up but is not running now
	foreach(QString id, macOver.keys()) {
		if (id == QString::fromLocal8Bit(conf.macId.c_str())) continue;
		if (macOver.value(id).isEmpty()) continue;
		fprintf(file, "\n[MACHINE.%s]\n\n", id.toLocal8Bit().data());
		foreach(xMacOver::value_type kv, macOver.value(id))
			fprintf(file, "%s = %s\n", kv.first.c_str(), kv.second.c_str());
	}
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
	foreach(QString nam, QDir(xres_dir(MAC_DIR)).entryList(
			QStringList() << ("*" MAC_SUFFIX), QDir::Files, QDir::Name))
		res << MAC_DIR "/" + nam;
	return res;
}

bool xm_is_user_file(const QString& nam) {
	return nam.startsWith(MAC_DIR "/");
}

QString xm_user_path(const std::string& id) {
	return xres_dir(MAC_DIR) + SLASH + QString::fromLocal8Bit(id.c_str()) + MAC_SUFFIX;
}

bool xm_is_users(const std::string& id) {
	return QFile::exists(xm_user_path(id));
}

// a file name out of a name a person typed

std::string xm_id_of_name(const std::string& name) {
	QString res;
	foreach(QChar c, QString::fromLocal8Bit(name.c_str()).toLower()) {
		if (c.isLetterOrNumber()) res += c;
		else if (!res.isEmpty() && !res.endsWith('-')) res += '-';
	}
	while (res.endsWith('-')) res.chop(1);
	if (res.isEmpty()) res = "machine";
	return std::string(res.toLocal8Bit().data());
}

// The id is the caller's: it is what says whether this is another machine or
// one being written over. A machine written over keeps what it inherits, so
// updating the one you are on does not make it inherit itself.

bool xm_save_as(const std::string& id, const std::string& name) {
	const xMachine* mac = xm_find(conf.macId);
	if (!mac) return false;
	std::string parent = (id == mac->id) ? mac->parent : mac->id;
	const xMachine* base = xm_find(parent);
	if (!base) {
		xlog(XLG_CONF, XLL_ERROR, "machine %s inherits nothing to write a diff against",
			id.c_str());
		return false;
	}
	QStringList keys;
	mac_put_all(keys, base);
	QDir().mkpath(xres_dir(MAC_DIR));
	QFile file(xm_user_path(id));
	if (!file.open(QFile::WriteOnly)) {
		xlog(XLG_CONF, XLL_ERROR, "can't write %s", xm_user_path(id).toLocal8Bit().data());
		return false;
	}
	QStringList out;
	out << "# A machine of your own. Delete this file to drop it.";
	out << "";
	out << "[machine]";
	out << QString("name    = %1").arg(QString::fromLocal8Bit(name.c_str()));
	out << QString("inherit = %1").arg(QString::fromLocal8Bit(base->id.c_str()));
	QMap<QString, QStringList> part;
	foreach(QString line, keys) {
		QString where;
		QString key = line.section('=', 0, 0).trimmed();
		QString nam = mac_key_place(key, &where);	// fills where, so not inline
		part[where] << line.replace(0, key.size(), nam);
	}
	const char* sect[] = {"machine", "video", "sound", "storage", "input", "rom", NULL};
	for (int i = 0; sect[i]; i++) {
		if (!part.contains(sect[i])) continue;
		if (strcmp(sect[i], "machine")) {
			out << "";
			out << QString("[%1]").arg(sect[i]);
		}
		out << part.value(sect[i]);
	}
	out << "";
	file.write(out.join("\n").toLocal8Bit());
	file.close();
	xm_load_all();
	return true;
}

// the file goes; a machine that only shadowed a built-in one comes back as it
// ships, and one that was the user's own is gone from the list

bool xm_delete(const std::string& id) {
	if (!xm_is_users(id)) return false;
	if (!QFile::remove(xm_user_path(id))) return false;
	xm_over_forget(id);
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

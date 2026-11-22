#include "filemachine.h"
#include "xcore.h"
#include "autostart.h"
#include "../filer.h"

#include <map>
#include <algorithm>
#include <QFile>

// Which machine a file is opened on: what each format needs, what the user
// wants done about it, and the decision the two make together.

static const xFileMac fm_tab[] = {
	{FL_SNA, "sna", "SNA snapshot", FMN_SNAPSHOT, NULL, snaGetHardware},
	{FL_Z80, "z80", "Z80 snapshot", FMN_SNAPSHOT, NULL, z80GetHardware},
	{FL_SZX, "szx", "SZX snapshot", FMN_EXACT, NULL, szxGetHardware},
	{FL_SPG, "spg", "SPG snapshot", FMN_TSCONF, "evo-tsconf", NULL},
	{FL_NEX, "nex", "NEX program", FMN_NEXT, "zxnext", NULL},
	{FL_SNX, "snx", "SNX snapshot", FMN_NEXT, "zxnext", NULL},
	{FL_RZX, "rzx", "RZX playback", FMN_RZX, NULL, rzxGetHardware},
	{FL_TAP, "tap", "TAP tape", FMN_ANY, NULL, NULL},
	{FL_TZX, "tzx", "TZX tape", FMN_ANY, NULL, NULL},
	{FL_WAV, "wav", "WAV tape", FMN_ANY, NULL, NULL},
	{FL_TRD, "trd", "TRD disk", FMN_TRDOS, "pent", NULL},
	{FL_SCL, "scl", "SCL disk", FMN_TRDOS, "pent", NULL},
	{FL_FDI, "fdi", "FDI disk", FMN_TRDOS, "pent", NULL},
	{FL_TD0, "td0", "TD0 disk", FMN_TRDOS, "pent", NULL},
	{FL_UDI, "udi", "UDI disk", FMN_TRDOS, "pent", NULL},
	{FL_DSK, "dsk", "DSK disk", FMN_PLUS3DOS, "zxplus3", NULL},
	{0, NULL, NULL, 0, NULL, NULL}
};

// the machine that is exactly what a snapshot was taken on
static const struct {
	int hw;
	const char* target;
} fm_snap_tab[] = {
	{SNAP_HW_48K, "zx48"},
	{SNAP_HW_128K, "zx128"},
	{SNAP_HW_PLUS2, "zxplus2"},
	{SNAP_HW_PLUS2A, "zxplus2a"},
	{SNAP_HW_PLUS3, "zxplus3"},
	{SNAP_HW_PENTAGON, "pent"},
	{SNAP_HW_SCORPION, "scorp"},
	{SNAP_HW_PENT512, "pent"},
	{SNAP_HW_PENT1024, "pent1024"},
	{SNAP_HW_UNKNOWN, NULL}
};

std::string fm_snap_target(int snap) {
	snap = SNAP_HW_OF(snap);
	int i = 0;
	while ((fm_snap_tab[i].hw != SNAP_HW_UNKNOWN) && (fm_snap_tab[i].hw != snap))
		i++;
	return fm_snap_tab[i].target ? fm_snap_tab[i].target : "";
}

static const char* fm_need_tab[] = {
	"any machine", "TSConf", "ZX Spectrum Next", "Beta Disk (TR-DOS)", "+3 disk drive", "read from the file",
	"exactly the one it was recorded on", "exactly the one it was taken on"
};

static std::map<std::string, std::string> fm_pref_map;

const xFileMac* fm_rows() {
	return fm_tab;
}

static const xFileMac* fm_row(int ftype) {
	for (int i = 0; fm_tab[i].key; i++) {
		if (fm_tab[i].ftype == ftype) return &fm_tab[i];
	}
	return NULL;
}

const char* fm_need_text(int need) {
	if ((need < FMN_ANY) || (need > FMN_EXACT)) return "";
	return fm_need_tab[need];
}

// a disk has to be started, not just read, so it asks the one table that knows;
// model is the machine's snapModel, which tells a +2 from a 128K
static bool fm_takes(int hwid, int model, int ram, int dif, int need, int snap) {
	switch (need) {
		case FMN_TSCONF: return hwid == HW_TSLAB;
		case FMN_NEXT: return hwid == HW_ZXNEXT;
		case FMN_TRDOS: return autostart_can(hwid, dif, AS_DISK);
		case FMN_PLUS3DOS: return autostart_can(hwid, dif, AS_DISK3);
		case FMN_SNAPSHOT: return snapHwRuns(snap, hwid, model, ram);
		case FMN_RZX:
		case FMN_EXACT: return snapHwIs(snap, hwid);
	}
	return true;
}

// A snapshot names the model it was taken on - but a .sna, whose size only
// says 48K or 128K, and a disk names a family at most
static bool fm_names_model(const xFileMac* row) {
	return row->probe && (row->ftype != FL_SNA);
}

// An SZX, and a .z80 on Auto, stand the machine at a point in its frame, so
// they are loaded on the model they name: a core shared by several models is
// not enough, unless the machine is one of the user's own rather than a model
// of the table.
static bool fm_exact_on(const std::string& id, int snap, const std::string& target) {
	if (target.empty() || (target == id)) return true;
	if (SNAP_HW_OF(snap) >= SNAP_HW_CORE) return !xm_find(target);
	for (int i = 0; fm_snap_tab[i].target; i++) {
		if (id == fm_snap_tab[i].target) return false;
	}
	return true;
}

// the running machine is asked as it runs: what the user changed on it since
// it was set is not in its definition yet
bool fm_runs(const std::string& id, int need, int snap) {
	if ((id == conf.macId) && conf.zx)
		return fm_takes(conf.zx->hw->id, conf.zx->snapModel, conf.zx->mem->ramSize, conf.zx->dif->cfg, need, snap);	// out under a divMMC is still its
	const xMachine* mac = xm_find(id);
	if (!mac) return false;
	HardWare* hw = findHardware(mac->hw.c_str());
	return hw && fm_takes(hw->id, mac->snapModel, mac->memory << 10, mac->disk, need, snap);
}

// Is that machine the model the file names. A .z80's hw 9 is written for every
// Pentagon alike, and the 1024 runs the 128's frame, so it is one of them.
static bool fm_is_model(const std::string& id, int ftype, int snap, const std::string& target) {
	if (fm_runs(id, FMN_EXACT, snap) && fm_exact_on(id, snap, target)) return true;
	return (ftype == FL_Z80) && (SNAP_HW_OF(snap) == SNAP_HW_PENTAGON)
		&& fm_runs(id, FMN_EXACT, SNAP_HW_PENT1024) && fm_exact_on(id, SNAP_HW_PENT1024, "pent1024");
}

void fm_clear() {
	fm_pref_map.clear();
}

std::string fm_pref(const std::string& key) {
	std::map<std::string, std::string>::iterator it = fm_pref_map.find(key);
	return (it == fm_pref_map.end()) ? std::string(FM_AUTO) : it->second;
}

void fm_load(const std::string& key, const std::string& val) {
	int i = 0;
	while (fm_tab[i].key && (key != fm_tab[i].key))
		i++;
	if (!fm_tab[i].key) return;			// no such row
	if (val.empty() || (val == FM_AUTO)) {
		fm_pref_map.erase(key);
	} else {
		fm_pref_map[key] = val;
	}
}

void fm_save(FILE* file) {
	fprintf(file, "\n[FILETYPES]\n\n");
	for (int i = 0; fm_tab[i].key; i++) {
		std::string val = fm_pref(fm_tab[i].key);
		if (val != FM_AUTO)
			fprintf(file, "%s = %s\n", fm_tab[i].key, val.c_str());
	}
}

// a 16K image from the ROM folder, empty if it is not there
static std::vector<unsigned char> fm_rom_image(const char* name) {
	QFile file(QString::fromUtf8(xm_rom_path(name).c_str()));
	if (!file.open(QIODevice::ReadOnly)) return std::vector<unsigned char>();
	QByteArray data = file.read(MEM_16K);
	if (data.size() != MEM_16K) return std::vector<unsigned char>();
	return std::vector<unsigned char>(data.begin(), data.end());
}

// A 128K .sna in its editor names no model, so the editor is told from the file
// against the ones in the ROM folder, then the model. Told, it is that model; one
// that does not tell is SNAP_UNSURE, and the user is asked - the Sinclair editor's
// too, as the 128K and the clones share it but not their menus.
static int fm_sna_guess(const char* path) {
	static const char* names[] = {"128-0.rom", "plus2-0.rom", "plus3-0.rom"};
	std::vector<unsigned char> img[3];
	const unsigned char* eds[3];
	int idx[3];
	int n = 0;
	for (int i = 0; i < 3; i++) {
		img[i] = fm_rom_image(names[i]);
		if (img[i].empty()) continue;
		eds[n] = img[i].data();
		idx[n++] = i;
	}
	int ed = (n > 0) ? sna_editor_of(path, eds, n) : -1;
	// the 128K's #5B00 routines are the +2's: one told apart needs both to compare
	if ((ed >= 0) && (idx[ed] < 2) && (img[0].empty() || img[1].empty())) ed = -1;
	xlog(XLG_FILE, XLL_INFO, "sna in its editor: %s", (ed < 0) ? "no model told" : names[idx[ed]]);
	if (ed < 0) return SNAP_HW_128K | SNAP_UNSURE;
	switch (idx[ed]) {
		case 1: return SNAP_HW_PLUS2 | SNAP_IN_ROM;
		case 2: return (sna_plus3_drive(path) ? SNAP_HW_PLUS3 : SNAP_HW_PLUS2A) | SNAP_IN_ROM;
	}
	return SNAP_HW_128K | SNAP_EDITOR | SNAP_UNSURE;
}

xFileMacPick fm_pick(int ftype, const char* path) {
	xFileMacPick pick;
	const xFileMac* row = fm_row(ftype);
	if (!row) return pick;
	std::string pref = fm_pref(row->key);
	if (pref == FM_KEEP) return pick;
	int snap = SNAP_HW_UNKNOWN;
	std::string target = row->target ? row->target : "";
	if (row->probe) {
		snap = row->probe(path);
		if ((ftype == FL_SNA) && (snap & SNAP_IN_ROM))
			snap = fm_sna_guess(path);
		target = fm_snap_target(snap);
		// a file of ours names the very machine it was taken on
		if ((SNAP_HW_OF(snap) >= SNAP_HW_CORE) && (ftype == FL_SZX)) {
			char id[256];
			szx_machine_of(path, id, sizeof(id));
			target = id;
		}
	}
	// one machine for the whole format, unless it cannot take this very file
	if ((pref != FM_AUTO) && (pref != FM_ASK) && fm_runs(pref, row->need, snap)) {
		if (pref != conf.macId) pick.target = pref;
		return pick;
	}
	// a model only guessed at is never switched to unasked
	pick.unsure = (snap & SNAP_UNSURE) != 0;
	if (pick.unsure) pref = FM_ASK;
	bool exact = (row->need == FMN_EXACT);
	// model: the machine has to be the model the file names, too
	auto fits = [&](const std::string& id, bool model) {
		return fm_runs(id, row->need, snap) && (!model || fm_is_model(id, ftype, snap, target));
	};
	// the machines but the running one that can take the file, in this order
	std::vector<std::string> can;
	auto add = [&](const std::string& id, bool model) {
		if (!id.empty() && (id != conf.macId) && (std::find(can.begin(), can.end(), id) == can.end())
				&& fits(id, model))
			can.push_back(id);
	};
	// Ask, whenever there is a choice: not when the running machine is the model
	// the file names, nor when only one machine can take it. A file that names
	// none has the machine Auto would go back to on top, and Keep for the answer
	// when the running one can take it.
	if (pref == FM_ASK) {
		bool named = fm_names_model(row) && !target.empty();
		if (named && (target == conf.macId)) return pick;
		pick.runs = fits(conf.macId, exact);
		if (!named && fits(conf.macBase, exact)) add(conf.macBase, exact);
		add(target, exact);
		foreach(const xMachine& mac, xm_list())
			add(mac.id, exact);
		if (can.size() + (pick.runs ? 1 : 0) < 2) {
			if (!pick.runs && !can.empty()) pick.target = can.front();
			return pick;
		}
		pick.ask = can;
		pick.keep = !named && pick.runs;
		xlog(XLG_FILE, XLL_INFO, "%s on %s: asking", row->key, conf.macId.c_str());
		return pick;
	}
	// The user's own machine when it can take the file: a machine a file
	// switched to was for that file. Then the one running, if it can, then the
	// one the file names, then every other one that can take it. false: none can.
	auto choose = [&](bool model) {
		if (fits(conf.macBase, model)) {
			if (conf.macBase != conf.macId) {
				pick.target = conf.macBase;
				xlog(XLG_FILE, XLL_INFO, "%s: back from %s to %s", row->key, conf.macId.c_str(), pick.target.c_str());
			}
			return true;
		}
		if (fits(conf.macId, model)) return true;
		can.clear();
		add(target, model);
		foreach(const xMachine& mac, xm_list())
			add(mac.id, model);
		if (can.empty()) return false;
		pick.target = can.front();
		xlog(XLG_FILE, XLL_INFO, "%s does not run on %s: %s", row->key, conf.macId.c_str(), pick.target.c_str());
		return true;
	};
	// A .z80 names its model too, so Auto takes that one, as for an SZX - or any
	// machine that runs it, when none is that model. Ask, and a machine set for
	// the row that cannot run the file, take any that runs it.
	bool z80auto = (ftype == FL_Z80) && (pref == FM_AUTO);
	if (!choose(exact || z80auto) && z80auto) choose(false);
	return pick;
}

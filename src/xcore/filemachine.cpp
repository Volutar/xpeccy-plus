#include "filemachine.h"
#include "xcore.h"
#include "autostart.h"
#include "../filer.h"

#include <map>
#include <algorithm>

// Which machine a file is opened on: what each format needs, what the user
// wants done about it, and the decision the two make together.

static const xFileMac fm_tab[] = {
	{FL_SNA, "sna", "SNA snapshot", FMN_SNAPSHOT, NULL, snaGetHardware},
	{FL_Z80, "z80", "Z80 snapshot", FMN_SNAPSHOT, NULL, z80GetHardware},
	{FL_SZX, "szx", "SZX snapshot", FMN_EXACT, NULL, szxGetHardware},
	{FL_SPG, "spg", "SPG snapshot", FMN_TSCONF, "evo-tsconf", NULL},
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
	int i = 0;
	while ((fm_snap_tab[i].hw != SNAP_HW_UNKNOWN) && (fm_snap_tab[i].hw != snap))
		i++;
	return fm_snap_tab[i].target ? fm_snap_tab[i].target : "";
}

static const char* fm_need_tab[] = {
	"any machine", "TSConf", "Beta Disk (TR-DOS)", "+3 disk drive", "read from the file",
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

// a disk has to be started, not just read, so it asks the one table that knows
static bool fm_takes(int hwid, int dif, int need, int snap) {
	switch (need) {
		case FMN_TSCONF: return hwid == HW_TSLAB;
		case FMN_TRDOS: return autostart_can(hwid, dif, AS_DISK);
		case FMN_PLUS3DOS: return autostart_can(hwid, dif, AS_DISK3);
		case FMN_SNAPSHOT: return snapHwRuns(snap, hwid);
		case FMN_RZX:
		case FMN_EXACT: return snapHwIs(snap, hwid);
	}
	return true;
}

// An SZX stands the machine at a point in its frame, so it is loaded on the
// model it names: a core shared by several models is not enough, unless the
// machine is one of the user's own rather than a model of the table.
static bool fm_exact_on(const std::string& id, int snap, const std::string& target) {
	if (target.empty() || (target == id)) return true;
	if (snap >= SNAP_HW_CORE) return !xm_find(target);
	for (int i = 0; fm_snap_tab[i].target; i++) {
		if (id == fm_snap_tab[i].target) return false;
	}
	return true;
}

// the running machine is asked as it runs: what the user changed on it since
// it was set is not in its definition yet
bool fm_runs(const std::string& id, int need, int snap) {
	if ((id == conf.macId) && conf.zx)
		return fm_takes(conf.zx->hw->id, conf.zx->dif->type, need, snap);
	const xMachine* mac = xm_find(id);
	if (!mac) return false;
	HardWare* hw = findHardware(mac->hw.c_str());
	return hw && fm_takes(hw->id, mac->disk, need, snap);
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

xFileMacPick fm_pick(int ftype, const char* path) {
	xFileMacPick pick;
	const xFileMac* row = fm_row(ftype);
	if (!row) return pick;
	int snap = SNAP_HW_UNKNOWN;
	std::string target = row->target ? row->target : "";
	if (row->probe) {
		snap = row->probe(path);
		target = fm_snap_target(snap);
		// a file of ours names the very machine it was taken on
		if ((snap >= SNAP_HW_CORE) && (ftype == FL_SZX)) {
			char id[256];
			szx_machine_of(path, id, sizeof(id));
			target = id;
		}
	}
	std::string pref = fm_pref(row->key);
	if (pref == FM_KEEP) return pick;
	// one machine for the whole format, unless it cannot take this very file
	if ((pref != FM_AUTO) && (pref != FM_ASK) && fm_runs(pref, row->need, snap)) {
		if (pref != conf.macId) pick.target = pref;
		return pick;
	}
	auto fits = [&](const std::string& id) {
		return fm_runs(id, row->need, snap) && ((row->need != FMN_EXACT) || fm_exact_on(id, snap, target));
	};
	if (fits(conf.macId)) return pick;
	// the user's own machine first, then the one the file names, then every
	// other one that can take it
	std::vector<std::string> can;
	auto add = [&](const std::string& id) {
		if (!id.empty() && (std::find(can.begin(), can.end(), id) == can.end()) && fm_runs(id, row->need, snap))
			can.push_back(id);
	};
	if (fits(conf.macBase)) add(conf.macBase);
	add(target);
	foreach(const xMachine& mac, xm_list())
		add(mac.id);
	if (can.empty()) return pick;
	if (pref == FM_ASK) {
		pick.ask = can;
	} else {
		pick.target = can.front();
	}
	xlog(XLG_FILE, XLL_INFO, "%s does not run on %s: %s", row->key, conf.macId.c_str(),
		pick.target.empty() ? "asking" : pick.target.c_str());
	return pick;
}

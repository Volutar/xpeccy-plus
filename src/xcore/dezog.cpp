// DeZog's remote: the DZRP protocol, 2.2 (DeZog's design/DeZogProtocol.md).
//
// The server has a thread of its own, so a modal dialog or an open menu does not
// stall it. Every command runs there under emu_lock: between the emulation's
// cycles, on a machine standing still. A reply is written before anything that
// command started can stop the machine again, and the stop's NTF_PAUSE comes to
// this thread as a queued call - so a reply always goes out before the
// notification, as the protocol asks.
//
// The stops are DeZog's while it is attached: a breakpoint holds the machine
// with PR_DEZOG and no debugger window (dezog_stop). DeZog's points live in a
// list of their own (brk_ext); its conditions, hit counts and logpoints it works
// out itself, so the points here are plain. Stepping is DeZog's too: a CONTINUE
// with up to two temporary points, one past the instruction and one where it
// jumps to.
//
// The machine goes to DeZog as one of its memory models: the 48K and the ALF as
// ZX48K, the Next as the Next, any other as ZX128K (dz_slot_bank names the rest).

#include <QtGlobal>
#include "dezog.h"
#include "xcore.h"

static int cliOn = 0;
static int cliPort = 0;

void dezog_cli(int port) {
	cliOn = 1;
	cliPort = port;
}

#include <QThread>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <atomic>
#include <map>
#include <string>
#include <vector>
#include "version.h"
#include "../libxpeccy/xlog.h"
#include "../libxpeccy/xstate.h"
#include "../libxpeccy/filetypes/filetypes.h"
#include "../libxpeccy/input/input.h"
#include "sound.h"
#include "../filer.h"
#include <QCryptographicHash>
#include <algorithm>
#include <deque>

enum {
	CMD_INIT = 1,
	CMD_CLOSE = 2,
	CMD_GET_REGISTERS = 3,
	CMD_SET_REGISTER = 4,
	CMD_WRITE_BANK = 5,		// DZRP 2.0 (DeZog 3.7): an 8K bank, the Next's numbering
	CMD_CONTINUE = 6,
	CMD_PAUSE = 7,
	CMD_READ_MEM = 8,		// DZRP 2.0
	CMD_WRITE_MEM = 9,
	CMD_SET_SLOT = 10,
	CMD_GET_NEXTREG = 11,
	CMD_SET_BORDER = 12,		// DZRP 2.0
	CMD_GET_SPRITES_PALETTE = 16,
	CMD_GET_SPRITES_CLIP = 17,
	CMD_GET_SPRITES = 18,
	CMD_GET_SPRITE_PATTERNS = 19,
	CMD_WRITE_PORT = 21,
	CMD_INTERRUPT_ON_OFF = 23,
	CMD_GET_SUPPORTED = 24,
	CMD_READ_BANK_MEM = 25,
	CMD_WRITE_BANK_MEM = 26,
	CMD_SET_NEXTREGS = 27,
	CMD_READ_MEM_BLOCKS = 28,
	CMD_ADD_BREAKPOINT = 40,
	CMD_REMOVE_BREAKPOINT = 41,
	CMD_ADD_WATCHPOINT = 42,
	CMD_REMOVE_WATCHPOINT = 43,
	CMD_READ_STATE = 50,
	CMD_WRITE_STATE = 51
};

enum {NTF_PAUSE = 1};
enum {WHY_NONE = 0, WHY_MANUAL = 1, WHY_POINT = 2, WHY_READ = 3, WHY_WRITE = 4, WHY_OTHER = 255};
enum {DZ_ZX16K = 1, DZ_ZX48K = 2, DZ_ZX128K = 3, DZ_NEXT = 4};

// what is advertised: the 2.0 commands are served, not named
static const int dzCommon[] = {CMD_INIT, CMD_CLOSE, CMD_GET_REGISTERS, CMD_SET_REGISTER, CMD_CONTINUE,
	CMD_PAUSE, CMD_WRITE_MEM, CMD_SET_SLOT, CMD_WRITE_PORT, CMD_INTERRUPT_ON_OFF, CMD_GET_SUPPORTED,
	CMD_READ_BANK_MEM, CMD_WRITE_BANK_MEM, CMD_READ_MEM_BLOCKS, CMD_ADD_BREAKPOINT, CMD_REMOVE_BREAKPOINT,
	CMD_ADD_WATCHPOINT, CMD_REMOVE_WATCHPOINT, CMD_READ_STATE, CMD_WRITE_STATE, 0};
static const int dzNext[] = {CMD_GET_NEXTREG, CMD_GET_SPRITES_PALETTE, CMD_GET_SPRITES_CLIP,
	CMD_GET_SPRITES, CMD_GET_SPRITE_PATTERNS, CMD_SET_NEXTREGS, 0};

static QThread* dzThread = NULL;
static QObject* dzCtx = NULL;		// lives on dzThread; the server and the client are its children
static QTcpServer* dzSrv = NULL;
static QTcpServer* dzSrv6 = NULL;	// the same on ::1, for a client to whom localhost is that
static QTcpSocket* dzCli = NULL;
static QByteArray dzIn;
static std::atomic<int> dzPort(0);	// listening on it, 0: not
static std::atomic<int> dzOn(0);	// a client said INIT: the stops are its own
static int dzModel = DZ_ZX48K;
static int dzLoading = 0;		// INIT up to the first CONTINUE: DeZog may be loading a file
static int dzTmp[2] = {-1, -1};		// CONTINUE's temp points, cpu addresses
static std::map<int, int> dzBrk;	// DZRP id -> point id
static int dzBrkNext = 1;
typedef struct {
	int adr, bank, size, acc;
	int id;
} dzWatch;
static std::vector<dzWatch> dzWat;
// The states READ_STATE handed out, by their hash: WRITE_STATE takes back only one of
// those, as its bytes go over the machine's whole state, pointers and all
static std::deque<QByteArray> dzStates;
#define DZ_STATES	16
#define DZ_READ_MAX	0x40000		// a reply of memory blocks reads no more than this

static int dz_model_of(Computer* comp) {
	switch (comp->hw->id) {
		case HW_ZX48: case HW_ALF: return DZ_ZX48K;
		case HW_ZXNEXT: return DZ_NEXT;
	}
	return DZ_ZX128K;
}

// the machine is still of the model INIT gave DeZog (a profile switch makes another)
static int dz_same(Computer* comp) {
	return comp && (dz_model_of(comp) == dzModel);
}

// a 128K's RAM page as a bank: 8 and 9 are the ROMs, so the pages past 7 move up two
static int dz_ram_bank(int page) {
	if (page < 8) return page;
	return (page + 2 < 0xfe) ? page + 2 : 0xfe;
}

static int dz_bank_ram(int bank) {
	return (bank < 8) ? bank : bank - 2;
}

static int dz_slot_count() {
	switch (dzModel) {
		case DZ_NEXT: return 8;
		case DZ_ZX128K: return 4;
	}
	return 2;
}

static int dz_slot_of(int adr) {
	adr &= 0xffff;
	switch (dzModel) {
		case DZ_NEXT: return adr >> 13;
		case DZ_ZX128K: return adr >> 14;
	}
	return (adr < 0x4000) ? 0 : 1;
}

// The bank DeZog's model names for what the cpu reads in a slot. ROM pages past
// the two of the 128K and anything that is neither RAM nor ROM (an interface's
// own memory) are 0xFE, a bank no model has
static int dz_slot_bank(Computer* comp, int slot) {
	if (!dz_same(comp)) return 0xfe;
	switch (dzModel) {
		case DZ_NEXT:
			return next_mmu_get(comp, slot) & 0xff;
		case DZ_ZX128K: {
			xAdr xa = comp_get_xadr(comp, slot << 14, MEM_VIEW_READ);
			if (xa.type == MEM_RAM) return dz_ram_bank((xa.abs & comp->mem->ramMask) >> 14);
			if ((xa.type == MEM_ROM) && (((xa.abs & comp->mem->romMask) >> 14) < 2))
				return 8 + ((xa.abs & comp->mem->romMask) >> 14);
			return 0xfe;
		}
	}
	return slot ? 1 : 0;
}

// a bank of the model as a cell: its area, page and the offset of adr in it. 0: none
// (the 48K's banks, the Next's ROM: they go by the cpu address)
static int dz_cell(Computer* comp, int bank, int adr, int* area, int* page, int* ofs) {
	if (!dz_same(comp)) return 0;
	switch (dzModel) {
		case DZ_NEXT:
			if (bank >= comp_cell_pages(comp, CELL_RAM)) return 0;	// 0x60 on a 1 MB board
			*area = CELL_RAM;
			*page = bank;
			*ofs = adr & 0x1fff;
			return 1;
		case DZ_ZX128K:
			if (bank >= 0xfe) return 0;
			if ((bank == 8) || (bank == 9)) {
				*area = CELL_ROM;
				*page = bank - 8;
			} else {
				*area = CELL_RAM;
				*page = dz_bank_ram(bank);
			}
			*ofs = adr & 0x3fff;
			return (*page < comp_cell_pages(comp, *area)) ? 1 : 0;
	}
	return 0;
}

// A byte of a bank by the model's numbers, -1: none. The 48K's two banks are what
// the cpu reads there; the Next's ROM (0xFF) is the 16K ROM its slots 0 and 1 show
static int dz_bank_peek(Computer* comp, int bank, int ofs) {
	if (!dz_same(comp)) return -1;
	switch (dzModel) {
		case DZ_NEXT:
			if (bank == 0xff) return (ofs < 0x4000) ? comp_cell_peek(comp, CELL_ROM, next_rom(comp), ofs) : -1;
			if ((bank >= 0xe0) || (ofs >= 0x2000)) return -1;
			return comp_cell_peek(comp, CELL_RAM, bank, ofs);
		case DZ_ZX128K:
			if ((bank >= 0xfe) || (ofs >= 0x4000)) return -1;
			if ((bank == 8) || (bank == 9)) return comp_cell_peek(comp, CELL_ROM, bank - 8, ofs);
			return comp_cell_peek(comp, CELL_RAM, dz_bank_ram(bank), ofs);
	}
	if (bank == 0) return (ofs < 0x4000) ? comp_peek(comp, ofs, MEM_VIEW_READ, NULL) : -1;
	if (bank == 1) return (ofs < 0xc000) ? comp_peek(comp, 0x4000 + ofs, MEM_VIEW_READ, NULL) : -1;
	return -1;
}

static void dz_bank_poke(Computer* comp, int bank, int ofs, int val) {
	if (!dz_same(comp)) return;
	switch (dzModel) {
		case DZ_NEXT:
			if ((bank < 0xe0) && (ofs < 0x2000)) comp_cell_poke(comp, CELL_RAM, bank, ofs, val, 0);
			return;
		case DZ_ZX128K:
			if ((bank != 8) && (bank != 9) && (bank < 0xfe) && (ofs < 0x4000))
				comp_cell_poke(comp, CELL_RAM, dz_bank_ram(bank), ofs, val, 0);
			return;
	}
	if ((bank == 1) && (ofs < 0xc000)) comp_poke(comp, 0x4000 + ofs, val, 0);
}

// An 8K bank by the Next's numbers, which DeZog's loading of .sna and .z80 sends
// whatever the model (two halves of a 16K bank), and so does DZRP 2.0's WRITE_BANK
static void dz_bank8_poke(Computer* comp, int bank8, int ofs, int val) {
	if (!dz_same(comp)) return;
	if (dzModel == DZ_NEXT) {
		if (bank8 < 0xe0) comp_cell_poke(comp, CELL_RAM, bank8, ofs, val, 0);
	} else {
		comp_cell_poke(comp, CELL_RAM, bank8 >> 1, ((bank8 & 1) << 13) + (ofs & 0x1fff), val, 0);
	}
}

// the machine changed from outside: a recording joins, frames run ahead go
static void dz_touched() {
	rzx_rec_touch();
	x_input_bump();
}

// ------------------------------------------------------------------ points

static void dz_tmp_clear(Computer* comp) {
	for (int i = 0; i < 2; i++) {
		if (dzTmp[i] >= 0) comp->brkAdrMap[dzTmp[i]] &= ~MEM_BRK_TFETCH;
		dzTmp[i] = -1;
	}
}

static void dz_tmp_set(Computer* comp, int i, int adr) {
	adr &= 0xffff;
	dzTmp[i] = adr;
	comp->brkAdrMap[adr] |= MEM_BRK_TFETCH;
	comp->flgBRKMEM = 1;
}

// A point of DeZog's: a cell's when the address comes with a bank the model has
// as memory of its own, the cpu address's otherwise. Its id
static int dz_point_add(Computer* comp, int adr, int bank, int len, int flag) {
	xBrkPoint brk;
	int area, page, ofs, type, abs;
	adr &= 0xffff;
	if (len < 1) len = 1;
	if ((bank >= 0) && dz_cell(comp, bank, adr, &area, &page, &ofs)
			&& comp_cell_xadr(comp, area, page, ofs, &type, &abs)
			&& ((type == MEM_RAM) || (type == MEM_ROM))) {
		// a range stops at its bank's end
		int unit = comp_cell_unit(comp, area);
		if (ofs + len > unit) len = unit - ofs;
		flag |= (type == MEM_RAM) ? MEM_BRK_RAM : MEM_BRK_ROM;
		brk = brkCreate(BRK_MEMCELL, flag, abs, (len > 1) ? abs + len - 1 : -1);
	} else {
		if (adr + len > 0x10000) len = 0x10000 - adr;
		brk = brkCreate(BRK_CPUADR, flag, adr, (len > 1) ? adr + len - 1 : -1);
	}
	brk.ext = 1;
	brk_ext.push_back(brk);
	brkInstall(&brk_ext.back(), 0);
	return brk.id;
}

// a DZRP id not in use, 1..65535; 0: none left
static int dz_new_id() {
	for (int i = 0; i < 0xffff; i++) {
		int id = dzBrkNext;
		dzBrkNext = (dzBrkNext >= 0xffff) ? 1 : dzBrkNext + 1;
		if (!dzBrk.count(id)) return id;
	}
	return 0;
}

static void dz_point_del(int id) {
	for (auto it = brk_ext.begin(); it != brk_ext.end(); it++) {
		if (it->id == id) {
			brk_ext.erase(it);
			brkInstallAll();
			return;
		}
	}
}

// the client gone or a new one come: none of its points stay, the machine runs on
static void dz_detach() {
	emu_lock();
	Computer* comp = conf.zx;
	int on = dzOn.exchange(0);
	brk_ext.clear();
	dzBrk.clear();
	dzWat.clear();
	if (comp) {
		dz_tmp_clear(comp);
		brkInstallAll();
	}
	if (on) pause_del(PR_DEZOG);
	emu_unlock();
}

// a span of the cpu's memory into a reply
static void dz_peek_into(QByteArray& r, Computer* comp, int adr, int len) {
	for (int i = 0; i < len; i++)
		r.append((char)comp_peek(comp, (adr + i) & 0xffff, MEM_VIEW_READ, NULL));
}

// ------------------------------------------------------------------ messages

static void dz_put16(QByteArray& b, int v) {
	b.append((char)(v & 0xff));
	b.append((char)((v >> 8) & 0xff));
}

static int dz_get8(const QByteArray& b, int i) {
	return (i < b.size()) ? (unsigned char)b[i] : 0;
}

static int dz_get16(const QByteArray& b, int i) {
	return dz_get8(b, i) | (dz_get8(b, i + 1) << 8);
}

// a body (from the seq byte on) with the length in front
static QByteArray dz_frame(const QByteArray& body) {
	QByteArray msg;
	int len = body.size();
	msg.append((char)(len & 0xff));
	msg.append((char)((len >> 8) & 0xff));
	msg.append((char)((len >> 16) & 0xff));
	msg.append((char)((len >> 24) & 0xff));
	msg.append(body);
	return msg;
}

static void dz_send(const QByteArray& msg) {
	if (!dzCli) return;
	dzCli->write(msg);
	dzCli->flush();
}

static const char* dz_kind(int kind) {
	switch (kind) {
		case MEM_BRK_FETCH: return "fetch";
		case MEM_BRK_RD: return "read";
		case MEM_BRK_WR: return "write";
	}
	return "hit";
}

// the user's own point, as DeZog shows the reason of the stop
static std::string dz_own_text(const xBrkStop& st) {
	char buf[64];
	if (st.why == BRK_WHY_COND) {
		for (const xBrkPoint& brk : conf.brk.list)
			if (brk.id == st.id) return "Xpeccy+ breakpoint: condition " + brk.cond;
		return "Xpeccy+ breakpoint: condition";
	}
	switch (st.type) {
		case BRK_IRQ:
			return "Xpeccy+ breakpoint: interrupt";
		case BRK_IOPORT:
			snprintf(buf, sizeof(buf), "Xpeccy+ breakpoint: %s port #%04X", (st.kind == MEM_BRK_WR) ? "OUT" : "IN", st.adr & 0xffff);
			return buf;
	}
	if (st.adr < 0) return "Xpeccy+ breakpoint";
	snprintf(buf, sizeof(buf), "Xpeccy+ breakpoint: %s #%04X", dz_kind(st.kind), st.adr & 0xffff);
	return buf;
}

// NTF_PAUSE for the stop brk_take_hits left in brk_stops (manual: a pause asked
// for; text: some other reason, said so)
static QByteArray dz_pause_ntf(Computer* comp, int manual, const char* other = NULL) {
	int why = WHY_OTHER;
	int adr = cpu_get_pc(comp->cpu) & 0xffff;
	std::string text;
	if (other) {
		manual = 1;		// no hits to look at
		text = other;
	}
	const xBrkStop* temp = NULL;
	const xBrkStop* fetch = NULL;
	const xBrkStop* watch = NULL;
	const xBrkStop* own = NULL;
	if (!manual) {
		for (const xBrkStop& st : brk_stops) {
			if (st.why == BRK_WHY_TEMP) {
				if (!temp) temp = &st;
			} else if (!st.ext) {
				if (!own) own = &st;
			} else if (st.kind == MEM_BRK_FETCH) {
				if (!fetch) fetch = &st;
			} else if (!watch) {
				watch = &st;
			}
		}
	}
	// a step's end before a point there: DeZog looks for its own points by the address
	if (other) {
		why = WHY_OTHER;
	} else if (manual) {
		why = WHY_MANUAL;
	} else if (temp) {
		why = WHY_NONE;
	} else if (fetch) {
		why = WHY_POINT;
	} else if (watch) {
		why = (watch->kind == MEM_BRK_WR) ? WHY_WRITE : WHY_READ;
		if (watch->adr >= 0) adr = watch->adr & 0xffff;
	} else if (own) {
		text = dz_own_text(*own);
	} else {
		text = "Xpeccy+ stopped";
	}
	QByteArray b;
	b.append((char)0);		// a notification
	b.append((char)NTF_PAUSE);
	b.append((char)why);
	dz_put16(b, adr);
	b.append((char)(dz_slot_bank(comp, dz_slot_of(adr)) + 1));
	b.append(text.c_str());
	b.append((char)0);
	return dz_frame(b);
}

// ------------------------------------------------------------------ commands

static const char* dzRegName[] = {"PC", "SP", "AF", "BC", "DE", "HL", "IX", "IY", "AF'", "BC'", "DE'", "HL'",
	NULL, "IM", "F", "A", "C", "B", "E", "D", "L", "H", "IXL", "IXH", "IYL", "IYH",
	"F'", "A'", "C'", "B'", "E'", "D'", "L'", "H'", "R", "I"};

static int dz_reg(Computer* comp, const char* name) {
	bool ok = false;
	return cpu_get_reg(comp->cpu, name, &ok);
}

static void dz_registers(Computer* comp, QByteArray& r) {
	for (int i = 0; i < 12; i++)		// PC .. HL', in DZRP's order
		dz_put16(r, dz_reg(comp, dzRegName[i]));
	r.append((char)dz_reg(comp, "R"));
	r.append((char)dz_reg(comp, "I"));
	r.append((char)dz_reg(comp, "IM"));
	r.append((char)0);
	int n = dz_slot_count();
	r.append((char)n);
	for (int i = 0; i < n; i++)
		r.append((char)dz_slot_bank(comp, i));
}

// SET_SLOT: the model's slot, or one of the Next's 8K ones, which DeZog's loading
// sends whatever the model. 0: done or nothing to do
static int dz_set_slot(Computer* comp, int slot, int bank) {
	if (!dz_same(comp)) return 1;
	if (dzModel == DZ_NEXT) {
		if (slot > 7) return 1;
		if ((bank == 0xfe) && (slot < 2)) bank = 0xff;		// DeZog 3.7 and older name the ROM so
		next_mmu_set(comp, slot, bank);
		return 0;
	}
	if (dzModel != DZ_ZX128K) return 0;
	if (slot >= 4) {
		// an 8K slot of the Next's: C000 takes the bank its upper half has
		if ((slot != 7) || (bank >= 0xe0)) return 0;
		slot = 3;
		bank >>= 1;
	}
	// the +2A/+3's all-RAM paging has no slot DeZog could set
	if (((comp->hw->id == HW_PLUS2A) || (comp->hw->id == HW_PLUS3)) && (comp->p1FFD & 1)) return 1;
	int p = comp->p7FFD;
	if ((slot == 3) && (bank < 8)) {
		p = (p & ~0xc7) | bank;		// b6-7: more page bits on the Pentagons with more RAM
	} else if ((slot == 0) && ((bank == 8) || (bank == 9))) {
		p = (p & ~0x10) | ((bank - 8) << 4);
	} else {
		return 1;		// a fixed slot, or a bank 7FFD alone does not page
	}
	if (p == comp->p7FFD) return 0;
	if (comp->p7FFD & 0x20) return 1;	// paging locked
	comp->hw->out(comp, 0x7ffd, p);
	return 0;
}

static int dz_supported(int cmd) {
	for (int i = 0; dzCommon[i]; i++)
		if (dzCommon[i] == cmd) return 1;
	if (dzModel == DZ_NEXT) {
		for (int i = 0; dzNext[i]; i++)
			if (dzNext[i] == cmd) return 1;
	}
	return 0;
}

static QByteArray dz_state_hash(const QByteArray& blob) {
	return QCryptographicHash::hash(blob, QCryptographicHash::Sha256);
}

static QByteArray dz_state_save(Computer* comp) {
	QByteArray res;
	xState* st = xstate_create();
	if (st && xstate_save(st, comp)) {
		const unsigned char* data;
		size_t msz = xstate_meta_size();
		std::vector<char> meta(msz);
		size_t sz = xstate_bytes(st, &data, meta.data());
		res.append(meta.data(), (int)msz);
		res.append((const char*)data, (int)sz);
	}
	if (st) xstate_destroy(st);
	if (!res.isEmpty()) {
		dzStates.push_back(dz_state_hash(res));
		if (dzStates.size() > DZ_STATES) dzStates.pop_front();
	}
	return res;
}

// a state READ_STATE gave, taken back if it is this machine's
static void dz_state_load(Computer* comp, const QByteArray& blob) {
	size_t msz = xstate_meta_size();
	if ((size_t)blob.size() < msz) return;
	QByteArray h = dz_state_hash(blob);
	if (std::find(dzStates.begin(), dzStates.end(), h) == dzStates.end()) {
		xlog(XLG_NET, XLL_WARN, "DeZog: a state READ_STATE did not give, not loaded");
		return;
	}
	xState* now = xstate_create();
	xState* st = xstate_create();
	if (now && st && xstate_save(now, comp)) {
		std::vector<char> meta(msz);
		const unsigned char* data;
		size_t sz = xstate_bytes(now, &data, meta.data());
		if (xstate_same_layout(meta.data(), blob.constData()) && ((size_t)blob.size() == msz + sz)) {
			unsigned char* room = xstate_put_begin(st, blob.constData());
			if (room) {
				memcpy(room, blob.constData() + msz, sz);
				if (quick_xstate_load(st, comp)) x_input_bump();
			}
		} else {
			xlog(XLG_NET, XLL_WARN, "DeZog: a state of another machine, not loaded");
		}
	}
	if (now) xstate_destroy(now);
	if (st) xstate_destroy(st);
}

static void dz_command(int seq, int cmd, const QByteArray& in) {
	QByteArray r;
	QByteArray ntf;		// a stop to tell of, after the reply
	r.append((char)seq);
	xlog(XLG_NET, XLL_TRACE, "DeZog: command %i, %i bytes", cmd, (int)in.size());
	if (!dzOn && (cmd != CMD_INIT) && (cmd != CMD_GET_SUPPORTED)) {
		dz_send(dz_frame(r));		// an empty reply: nothing before INIT
		return;
	}
	emu_lock();
	Computer* comp = conf.zx;
	if (dzOn && (cmd != CMD_INIT) && !dz_same(comp)) {
		// the machine changed under the session: DeZog's picture of it is gone
		emu_unlock();
		xlog(XLG_NET, XLL_WARN, "DeZog: another machine now, the session ends");
		dzCli->abort();
		return;
	}
	int next = comp && (comp->hw->id == HW_ZXNEXT) && comp->next;
	switch (cmd) {
		case CMD_INIT: {
			dz_detach();		// a session before it leaves nothing
			dzModel = dz_model_of(comp);
			dzLoading = 1;
			dzOn = 1;
			pause_add(PR_DEZOG);		// DeZog starts from a machine that stands
			r.append((char)0);
			r.append((char)2);
			r.append((char)2);
			r.append((char)0);
			r.append((char)dzModel);
			r.append(XPRODUCT " " XVERSION);
			r.append((char)0);
			xlog(XLG_NET, XLL_INFO, "DeZog: %s attached, DZRP %i.%i.%i, model %i", in.mid(3).constData(),
				dz_get8(in, 0), dz_get8(in, 1), dz_get8(in, 2), dzModel);
			break;
		}
		case CMD_CLOSE:
			break;
		case CMD_GET_REGISTERS:
			dz_registers(comp, r);
			break;
		case CMD_SET_REGISTER: {
			int reg = dz_get8(in, 0);
			if ((reg < (int)(sizeof(dzRegName) / sizeof(dzRegName[0]))) && dzRegName[reg]) {
				cpu_set_reg(comp->cpu, dzRegName[reg], dz_get16(in, 1));
				dz_touched();
			}
			break;
		}
		case CMD_CONTINUE: {
			dzLoading = 0;
			dz_tmp_clear(comp);
			if (dz_get8(in, 0)) dz_tmp_set(comp, 0, dz_get16(in, 1));
			if (dz_get8(in, 3)) dz_tmp_set(comp, 1, dz_get16(in, 4));
			if (conf.emu.pause & PR_DEBUG) {
				// the Debugger window has the machine: it runs nothing behind its back
				pause_add(PR_DEZOG);
				dz_tmp_clear(comp);
				ntf = dz_pause_ntf(comp, 0, "The Xpeccy+ Debugger is open: close it to run");
			} else if (pause_has(PR_DEZOG)) {
				// the instruction it stands on runs first, its fetch not looked at; the
				// scope hears it, as the Debugger's resume makes it
				int tm;
				int res = brk_step_off(comp, &tm);
				snd_scope_step(comp, tm);
				if (res == BRK_GO_DBG) {
					ntf = dz_pause_ntf(comp, 0);
					dz_tmp_clear(comp);
				} else {
					if (res == BRK_GO_CLOSE) comp_brk(comp, BRK_STOP);
					pause_del(PR_DEZOG);
				}
				x_input_bump();
			}
			break;
		}
		case CMD_PAUSE:
			if (!pause_has(PR_DEZOG)) {
				pause_add(PR_DEZOG);
				dz_tmp_clear(comp);
				ntf = dz_pause_ntf(comp, 1);
			}
			break;
		case CMD_WRITE_MEM: {
			int adr = dz_get16(in, 1);
			for (int i = 3; i < in.size(); i++)
				comp_poke(comp, (adr + i - 3) & 0xffff, dz_get8(in, i), 0);
			dz_touched();
			break;
		}
		case CMD_READ_MEM:
			dz_peek_into(r, comp, dz_get16(in, 1), dz_get16(in, 3));
			break;
		case CMD_READ_MEM_BLOCKS:
			for (int i = 4; (i + 3 < in.size()) && (r.size() < DZ_READ_MAX); i += 4)
				dz_peek_into(r, comp, dz_get16(in, i), dz_get16(in, i + 2));
			break;
		case CMD_SET_SLOT:
			r.append((char)dz_set_slot(comp, dz_get8(in, 0), dz_get8(in, 1)));
			dz_touched();
			break;
		case CMD_READ_BANK_MEM: {
			int bank = dz_get8(in, 0);
			int ofs = dz_get16(in, 1);
			int len = dz_get16(in, 3);
			for (int i = 0; i < len; i++) {
				int v = dz_bank_peek(comp, bank, ofs + i);
				r.append((char)((v < 0) ? 0xff : v));
			}
			break;
		}
		case CMD_WRITE_BANK_MEM: {
			int bank = dz_get8(in, 0);
			int ofs = dz_get16(in, 1);
			int len = in.size() - 3;
			// a whole 8K bank from the start before the first CONTINUE: DeZog loading
			// a .sna or a .z80, the Next's numbers whatever the model
			int whole = dzLoading && (dzModel != DZ_NEXT) && (ofs == 0) && (len == 0x2000);
			for (int i = 0; i < len; i++) {
				if (whole) {
					dz_bank8_poke(comp, bank, i, dz_get8(in, i + 3));
				} else {
					dz_bank_poke(comp, bank, ofs + i, dz_get8(in, i + 3));
				}
			}
			dz_touched();
			break;
		}
		case CMD_WRITE_BANK: {
			int bank = dz_get8(in, 0);
			for (int i = 1; i < in.size(); i++)
				dz_bank8_poke(comp, bank, i - 1, dz_get8(in, i));
			dz_touched();
			r.append((char)0);
			r.append((char)0);
			break;
		}
		case CMD_WRITE_PORT:
			comp->hw->out(comp, dz_get16(in, 0), dz_get8(in, 2));
			dz_touched();
			break;
		case CMD_SET_BORDER:
			comp->hw->out(comp, 0xfe, dz_get8(in, 0) & 7);
			dz_touched();
			break;
		case CMD_INTERRUPT_ON_OFF:
			cpu_set_reg(comp->cpu, "IFF1", dz_get8(in, 0) ? 1 : 0);
			cpu_set_reg(comp->cpu, "IFF2", dz_get8(in, 0) ? 1 : 0);
			dz_touched();
			break;
		case CMD_GET_SUPPORTED: {
			QByteArray bits(8, 0);
			for (int c = 1; c < 64; c++)
				if (dz_supported(c)) bits[c >> 3] = bits[c >> 3] | (1 << (c & 7));
			while ((bits.size() > 1) && !bits[bits.size() - 1]) bits.chop(1);
			r.append(bits);
			break;
		}
		case CMD_GET_NEXTREG:
			r.append((char)(next ? next_nr_peek(comp, dz_get8(in, 0)) : 0));
			break;
		case CMD_SET_NEXTREGS:
			if (next) {
				for (int i = 0; i + 1 < in.size(); i += 2)
					next_nr_poke(comp, dz_get8(in, i), dz_get8(in, i + 1));
				dz_touched();
			}
			break;
		case CMD_GET_SPRITES_PALETTE: {
			// NR 43's palette numbers: the sprites' first is 2, their second 6
			int p = dz_get8(in, 0) ? 6 : 2;
			for (int i = 0; i < 256; i++) {
				int c = next ? comp->next->pal[p][i] : 0;
				r.append((char)((c >> 1) & 0xff));
				r.append((char)(c & 1));
			}
			break;
		}
		case CMD_GET_SPRITES_CLIP:
			for (int i = 0; i < 4; i++)
				r.append((char)(next ? comp->next->clip[1][i] : 0));
			r.append((char)(next ? next_nr_peek(comp, 0x15) : 0));
			break;
		case CMD_GET_SPRITES: {
			int idx = dz_get8(in, 0);
			int cnt = dz_get8(in, 1);
			for (int i = 0; i < cnt; i++)
				for (int k = 0; k < 5; k++)
					r.append((char)(next ? comp->next->spr.attr[(idx + i) & 0x7f][k] : 0));
			break;
		}
		case CMD_GET_SPRITE_PATTERNS: {
			int idx = dz_get16(in, 0);
			int cnt = std::min(dz_get16(in, 2), 64);		// all the pattern memory
			for (int i = 0; i < cnt * 256; i++)
				r.append((char)(next ? comp->next->spr.pat[(idx * 256 + i) & 0x3fff] : 0));
			break;
		}
		case CMD_ADD_BREAKPOINT: {
			int dz = dz_new_id();
			if (dz) dzBrk[dz] = dz_point_add(comp, dz_get16(in, 0), dz_get8(in, 2) - 1, 1, MEM_BRK_FETCH);
			dz_put16(r, dz);
			break;
		}
		case CMD_REMOVE_BREAKPOINT: {
			auto it = dzBrk.find(dz_get16(in, 0));
			if (it != dzBrk.end()) {
				dz_point_del(it->second);
				dzBrk.erase(it);
			}
			break;
		}
		case CMD_ADD_WATCHPOINT: {
			dzWatch w;
			w.adr = dz_get16(in, 0);
			w.bank = dz_get8(in, 2) - 1;
			w.size = dz_get16(in, 3);
			w.acc = dz_get8(in, 5);
			int flag = ((w.acc & 1) ? MEM_BRK_RD : 0) | ((w.acc & 2) ? MEM_BRK_WR : 0);
			if (flag) {
				w.id = dz_point_add(comp, w.adr, w.bank, w.size, flag);
				dzWat.push_back(w);
			}
			r.append((char)0);
			break;
		}
		case CMD_REMOVE_WATCHPOINT: {
			int adr = dz_get16(in, 0);
			int bank = dz_get8(in, 2) - 1;
			int size = dz_get16(in, 3);
			int acc = dz_get8(in, 5);
			for (auto it = dzWat.begin(); it != dzWat.end(); it++) {
				if ((it->adr == adr) && (it->bank == bank) && (it->size == size) && (it->acc == acc)) {
					dz_point_del(it->id);
					dzWat.erase(it);
					break;
				}
			}
			break;
		}
		case CMD_READ_STATE:
			r.append(dz_state_save(comp));
			break;
		case CMD_WRITE_STATE:
			dz_state_load(comp, in);
			break;
		default:
			xlog(XLG_NET, XLL_DEBUG, "DeZog: command %i is not served", cmd);
			break;
	}
	emu_unlock();
	dz_send(dz_frame(r));
	if (!ntf.isEmpty()) dz_send(ntf);
	if (cmd == CMD_CLOSE) {
		dz_detach();
		xlog(XLG_NET, XLL_INFO, "DeZog: closed");
	}
}

// ------------------------------------------------------------------ the socket

// A command: the payload's length (4 bytes), seq, the command, the payload
static void dz_read() {
	if (!dzCli) return;
	dzIn.append(dzCli->readAll());
	while (dzCli && (dzIn.size() >= 6)) {
		unsigned int len = (unsigned char)dzIn[0] | ((unsigned char)dzIn[1] << 8)
			| ((unsigned char)dzIn[2] << 16) | ((unsigned int)(unsigned char)dzIn[3] << 24);
		if (len > (64u << 20)) {
			xlog(XLG_NET, XLL_WARN, "DeZog: a message of %u bytes, the client dropped", len);
			dzCli->abort();
			return;
		}
		if ((unsigned int)dzIn.size() < len + 6) break;
		int seq = (unsigned char)dzIn[4];
		int cmd = (unsigned char)dzIn[5];
		QByteArray pl = dzIn.mid(6, (int)len);
		dzIn.remove(0, (int)len + 6);
		dz_command(seq, cmd, pl);
	}
}

static void dz_gone() {
	dz_detach();
	if (dzCli) {
		dzCli->deleteLater();
		dzCli = NULL;
	}
	dzIn.clear();
	xlog(XLG_NET, XLL_INFO, "DeZog: the client went");
}

static void dz_accept(QTcpServer* srv) {
	while (srv->hasPendingConnections()) {
		QTcpSocket* sock = srv->nextPendingConnection();
		if (dzCli) {
			xlog(XLG_NET, XLL_WARN, "DeZog: a second client refused");
			sock->abort();
			sock->deleteLater();
			continue;
		}
		dzCli = sock;
		dzIn.clear();
		sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
		QObject::connect(sock, &QTcpSocket::readyRead, dzCtx, dz_read);
		QObject::connect(sock, &QTcpSocket::disconnected, dzCtx, dz_gone);
		xlog(XLG_NET, XLL_INFO, "DeZog: a client connected");
	}
}

// on the server's thread
static void dz_listen(int port) {
	dzSrv = new QTcpServer(dzCtx);
	if (!dzSrv->listen(QHostAddress::LocalHost, port & 0xffff)) {
		xlog(XLG_NET, XLL_WARN, "DeZog: can't listen on port %i: %s", port, dzSrv->errorString().toLocal8Bit().constData());
		delete dzSrv;
		dzSrv = NULL;
		return;
	}
	QObject::connect(dzSrv, &QTcpServer::newConnection, dzCtx, [] {dz_accept(dzSrv);});
	// a host where localhost is ::1 first: the same port there, if it has IPv6
	dzSrv6 = new QTcpServer(dzCtx);
	if (dzSrv6->listen(QHostAddress::LocalHostIPv6, port & 0xffff)) {
		QObject::connect(dzSrv6, &QTcpServer::newConnection, dzCtx, [] {dz_accept(dzSrv6);});
	} else {
		delete dzSrv6;
		dzSrv6 = NULL;
	}
	dzPort = port;
	xlog(XLG_NET, XLL_INFO, "DeZog: listening on port %i", port);
}

void dezog_close() {
	if (!dzThread) return;
	QMetaObject::invokeMethod(dzCtx, []() {
		if (dzCli) {
			dzCli->disconnect();
			dzCli->abort();
		}
		dz_detach();
		if (dzSrv) dzSrv->close();
		if (dzSrv6) dzSrv6->close();
	}, Qt::BlockingQueuedConnection);
	dzThread->quit();
	dzThread->wait();
	delete dzCtx;		// the server and the client with it
	delete dzThread;
	dzCtx = NULL;
	dzThread = NULL;
	dzSrv = NULL;
	dzSrv6 = NULL;
	dzCli = NULL;
	dzPort = 0;
}

void dezog_apply() {
	int port = (cliOn && cliPort) ? cliPort : conf.dezog.port;
	if (!cliOn && !conf.dezog.on) port = 0;
	if (dzThread && port && (port == dzPort)) return;
	dezog_close();
	if (!port) return;
	dzThread = new QThread;
	dzCtx = new QObject;
	dzCtx->moveToThread(dzThread);
	dzThread->start();
	QMetaObject::invokeMethod(dzCtx, [port]() { dz_listen(port); }, Qt::BlockingQueuedConnection);
}

// A machine switch under a session: DeZog's points and hold go now, before the
// new machine runs, and the client is let go - its picture of the machine is gone
void dezog_end() {
	if (!dzOn.load()) return;
	dz_detach();
	xlog(XLG_NET, XLL_WARN, "DeZog: another machine, the session ends");
	QMetaObject::invokeMethod(dzCtx, [] {if (dzCli) dzCli->abort();}, Qt::QueuedConnection);
}

// the Debugger cleared every temp point: a step of DeZog's under way keeps its own
void dezog_keep_tmp(Computer* comp) {
	EmuLock lock;
	for (int i = 0; i < 2; i++)
		if (dzTmp[i] >= 0) dz_tmp_set(comp, i, dzTmp[i]);
}

int dezog_stop(Computer* comp) {
	if (!dzOn.load()) return 0;
	pause_add(PR_DEZOG);
	QByteArray ntf = dz_pause_ntf(comp, 0);
	dz_tmp_clear(comp);
	QMetaObject::invokeMethod(dzCtx, [ntf]() { dz_send(ntf); }, Qt::QueuedConnection);
	return 1;
}

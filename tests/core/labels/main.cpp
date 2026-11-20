#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

#include "../../../src/xcore/xcore.h"
#include "../../../src/xcore/dbgtext.h"
#include "../../../src/libxpeccy/cpu/Z80/z80.h"

xConfig conf;

QString gethexbyte(int value) { return QString("%1").arg(value & 0xff, 2, 16, QChar('0')).toUpper(); }
QString gethexword(int value) { return QString("%1").arg(value & 0xffff, 4, 16, QChar('0')).toUpper(); }
static QString installDir;		// the ROM lists of the test are in its labels/
QString xconf_install_dir() { return installDir; }
QString arc_beside(const QString& path, const QString& ext) { return path + ext; }	// labels_image: unused here
// what breakpoints.cpp takes from the rest of the program, as far as a list's records need it
QString gethex6(int value) { return QString("%1").arg(value & 0xffffff, 6, 16, QChar('0')).toUpper(); }
QString gethexint(int value) { return QString("%1").arg((unsigned)value, 8, 16, QChar('0')).toUpper(); }
QString dbg_word(int value) { return gethexword(value); }
int str_num(QString str, bool* ok) {
	if (str.startsWith('#') || str.startsWith('$')) return str.mid(1).toInt(ok, 16);
	return str.toInt(ok, 0);
}
void emu_lock() {}
void emu_unlock() {}
FILE* log_open_beside(const char*, QString*) { return nullptr; }
QString shot_path(const QString&) { return QString(); }

static int failures;

static void check(const char* name, bool ok) {
	std::printf("%s: %s\n", name, ok ? "ok" : "FAIL");
	if (!ok) failures++;
}

static xResult eval(const char* expression, int type = 0) {
	return xEval(expression, type);
}

static dtRecord parsed(const char* line) {
	dtRecord rec;
	dt_parse_line(QString::fromUtf8(line), &rec);
	return rec;
}

// the format's lines, read with no machine (docs/debugger-format.md)
static void parser_checks() {
	dtRecord r = parsed("05:1A00 SPRITES\r\n");
	check("text: a LABELSLIST line", r.type == DT_REC_LABEL && r.at.kind == DT_PLACE_PAGE && r.at.page == 5
		&& r.at.ofs == 0x1a00 && r.name == "SPRITES" && r.size == 0);
	r = parsed(":8000 START");
	dtRecord ff = parsed("FF:8000 START");
	dtRecord hx = parsed("0A:0010 old_hex");
	check("text: CPU address, and pages of older files", r.at.kind == DT_PLACE_CPU && r.at.ofs == 0x8000
		&& ff.at.kind == DT_PLACE_CPU && hx.at.kind == DT_PLACE_PAGE && hx.at.page == 10 && hx.at.hexPage);
	r = parsed("ROM0:0038 MY_IM1");
	dtRecord rom = parsed("BASIC48:0556 BASIC48.LD_BYTES");
	dtRecord sz = parsed("05:5BA0 BASIC128.STRIP2 8");
	check("text: an area, a ROM's name, a size", r.at.kind == DT_PLACE_AREA && r.at.area == CELL_ROM && r.at.page == 0
		&& rom.at.kind == DT_PLACE_ROM && rom.at.name == "BASIC48" && rom.at.ofs == 0x556
		&& sz.size == 8 && sz.name == "BASIC128.STRIP2");
	r = parsed("db :9000 7*4");
	dtRecord dw = parsed("dw SPRITES #10");
	dtRecord dc = parsed("dc BASIC48:0095 368");
	dtRecord code = parsed("code :8000 256");
	check("text: data records", r.type == DT_REC_DATA && r.view == DBG_VIEW_BYTE && r.len == 7 && r.count == 4
		&& dw.view == DBG_VIEW_WORD && dw.unit == 2 && dw.len == 16 && dw.at.kind == DT_PLACE_NAME
		&& dc.view == DBG_VIEW_TEXT && dc.dc && dc.len == 368 && code.view == DBG_VIEW_EXEC && code.len == 256);
	check("text: what is not a data record", parsed("code :8000 2*4").type == DT_REC_BAD
		&& parsed("db 9000 8").type == DT_REC_BAD && parsed("db :9000 0").type == DT_REC_BAD
		&& parsed("db :9000-9010 8").type == DT_REC_BAD);
	r = parsed("comment :8003 two\\nlines \\\\ and ; too");
	check("text: a comment's lines", r.type == DT_REC_COMMENT && r.text == "two\nlines \\ and ; too"
		&& dt_escape(r.text) == "two\\nlines \\\\ and ; too");
	r = parsed("rom BASIC48 4C42A2F075212361C3117015B107FF68 skip BASIC48.KEY_INT BASIC48:386E");
	check("text: a rom record", r.type == DT_REC_ROM && r.name == "BASIC48" && r.md5 == "4c42a2f075212361c3117015b107ff68"
		&& r.skipNames == QStringList("BASIC48.KEY_INT") && r.skipPlaces.size() == 1 && r.skipPlaces.at(0).ofs == 0x386e);
	check("text: a ROM's name is no area or page", parsed("rom ROM48 4c42a2f075212361c3117015b107ff68").type == DT_REC_BAD
		&& parsed("rom CAFE 4c42a2f075212361c3117015b107ff68").type == DT_REC_BAD
		&& parsed("rom X 4c42a2f0").type == DT_REC_BAD);
	r = parsed("brk :8000 exec if \"A == #FF\" span=open");
	check("text: a brk record's words", r.type == DT_REC_BRK && r.words.size() == 5 && r.words.at(3).quoted
		&& r.words.at(3).text == "A == #FF" && !r.words.at(0).quoted && r.words.at(0).text == ":8000");
	check("text: blank, comment, unknown", parsed("  ").type == DT_REC_NONE && parsed("; x").type == DT_REC_NONE
		&& parsed("struct ENTITY db 1").type == DT_REC_UNKNOWN && parsed("open  C:\\ZX\\a b.tap ").text == "C:\\ZX\\a b.tap");
	dtPlace pl;
	bool rt = true;
	const char* places[] = {"05:1A00", ":8000", "ROM0:0038", "DIV3:0010", "BASIC48:0556", "io:00FE/00FF", "io:7FFD",
		":5B00-5BFF", "START"};
	for (const char* p : places) rt &= dt_place_parse(p, &pl) && (dt_place_text(pl) == p);
	check("text: places written as read", rt);
}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	if (argc != 2) return 2;
	QDir fixtures(QString::fromLocal8Bit(argv[1]));
	QTemporaryDir temp;
	if (!temp.isValid()) return 2;
	parser_checks();
	Computer* comp = compCreate();
	conf.zx = comp;
	compSetHardware(comp, "ZXNext");
	compSetBaseFrq(comp, 3.5);
	compReset(comp, RES_DEFAULT);
	comp->flgBOOT = 0;
	comp->next->configMode = 0;
	comp->next->nr[0x83] = 0;
	check("load Next labels", loadLabels(fixtures.filePath("next.labels").toLocal8Bit().constData()) == 1);
	xAdr camera = find_label("Camera.WORLD_X");
	xAdr odd = find_label("Debug.calc_char");
	check("8K pages and SRAM base", camera.type == MEM_RAM && camera.abs == 0x48456
		&& odd.abs == 0x4a039 && find_label("LastRAM").abs == 0x1fffff);
	check("pages are decimal", find_label("Page17").abs == 0x40000 + 17 * 0x2000 + 0x123);
	check("adjacent pages stay distinct", find_label("EvenPage").abs == 0x48200
		&& find_label("OddPage").abs == 0x4a200 && find_label("LastByte").abs + 1 == find_label("NextPage").abs);
	check("CPU label lookup", camera.adr == 0x8456 && odd.adr == 0xa039
		&& find_label(comp_get_xadr(comp, 0x8456, MEM_VIEW_READ)) == "Camera.WORLD_X");
	comp->mem->ramData[camera.abs] = 0x42;
	check("CPU expression reads the label", !eval("Camera.WORLD_X").err && eval("Camera.WORLD_X").value == 0x8456
		&& eval("M(Camera.WORLD_X)").value == 0x42);
	xExpr condition = xexpr_compile("PC == Camera.WORLD_X");
	next_mmu_set(comp, 4, 0x20);
	next_mmu_set(comp, 6, 4);
	comp->cpu->regPC = 0xc456;
	check("expressions follow MMU changes", find_label("Camera.WORLD_X").adr == 0xc456
		&& eval("Camera.WORLD_X").value == 0xc456 && xexpr_eval(condition, comp) == 1);
	next_mmu_set(comp, 6, 0x21);
	check("unmapped CPU label reports an error", find_label("Camera.WORLD_X").adr == -1
		&& eval("Camera.WORLD_X").err != 0);
	xResult physical = eval("Camera.WORLD_X", MEM_RAM);
	check("physical RAM expression survives unmapping", !physical.err && physical.value == 0x48456
		&& eval("M(Camera.WORLD_X)", MEM_RAM).value == 0x42);
	check("virtual labels stay at their CPU address", find_label("Virtual").adr == 0x8123
		&& find_label(comp_get_xadr(comp, 0x8123, MEM_VIEW_READ)) == "Virtual");
	comp->next->nr[0x83] = 1;
	comp->next->pe3 = 0x82;
	comp->mem->ramData[0x24123] = 0x5a;
	check("expression memory reads follow overlays", !eval("M(0x2123)").err && eval("M(0x2123)").value == 0x5a);
	next_mmu_set(comp, 0, 4);
	check("hidden MMU pages are not CPU addresses", find_label("Camera.WORLD_X").adr == -1);
	comp->next->nr[0x83] = 0;
	check("exposed MMU page becomes visible", find_label("Camera.WORLD_X").adr == 0x456);
	comp->next->p123b = 4;
	comp->next->nr[0x12] = 0x20;
	check("Layer 2 overlay hides an MMU page", find_label("Camera.WORLD_X").adr == -1);
	comp->next->p123b = 0;
	// labels the debugger puts on ROM and divMMC cells, kept by their area
	xAdr cell = {MEM_RAM, 0, 0x0038, 0x0038};
	add_label(cell, "RomLabel");
	cell.abs = 0x20000 + 3 * 0x2000 + 0x10;
	cell.adr = -1;
	add_label(cell, "DivLabel");
	// a CPU label names the cell at its address, not the same offset of a page
	cell.type = MEM_IO;
	cell.abs = cell.adr = 0x0123;
	add_label(cell, "LowCpu");
	next_mmu_set(comp, 0, 0xff);
	xAdr page = {MEM_RAM, 0, 0x0123, 0x40000 + 0x30 * 0x2000 + 0x123};
	check("CPU labels stay off page views", find_label(page).isEmpty()
		&& find_label(comp_get_xadr(comp, 0x0123, MEM_VIEW_READ)) == "LowCpu");
	QMap<QString, xAdr> before = conf.curlabset->list;
	QString saved = temp.filePath("roundtrip.labels");
	check("save Next labels", saveLabels(saved.toLocal8Bit().constData()) == 1);
	QFile file(saved);
	file.open(QIODevice::ReadOnly);
	QByteArray text = file.readAll();
	file.close();
	check("export keeps Next numbering", text.contains("04:0456 Camera.WORLD_X\n")
		&& text.contains("05:0039 Debug.calc_char\n") && text.contains(":8123 Virtual\n")
		&& text.contains("17:0123 Page17\n") && text.contains("223:1FFF LastRAM\n"));
	check("export names ROM and divMMC cells", text.contains("ROM0:0038 RomLabel\n") && text.contains("DIV3:0010 DivLabel\n")
		&& text.contains(":0123 LowCpu\n"));
	loadLabels(saved.toLocal8Bit().constData());
	bool same = conf.curlabset->list.size() == before.size();
	for (auto it = before.begin(); it != before.end(); ++it) {
		xAdr after = find_label(it.key());
		same &= after.type == it.value().type && after.abs == it.value().abs;
	}
	check("Next labels round trip", same);
	// SjASMPlus's own output (sjasm-next.asm says how it was made)
	check("load SjASMPlus labels", loadLabels(fixtures.filePath("sjasm-next.labels").toLocal8Bit().constData()) == 1);
	check("SjASMPlus pages", find_label("start").abs == 0x40000 + 4 * 0x2000
		&& find_label("lowpage").abs == 0x40000 + 10 * 0x2000
		&& find_label("dot").abs == 0x40000 + 11 * 0x2000
		&& find_label("p17b").abs == 0x40000 + 17 * 0x2000 + 0x123
		&& find_label("last").abs == 0x40000 + 223 * 0x2000);
	check("load SjASMPlus map", loadLabels(fixtures.filePath("sjasm-next.map").toLocal8Bit().constData()) == 1);
	check("map places by linear address", find_label("P17B").abs == 0x40000 + 17 * 0x2000 + 0x123
		&& find_label("DOT").abs == 0x40000 + 11 * 0x2000 && find_label("LAST").abs == 0x40000 + 223 * 0x2000
		&& find_label("START").abs == 0x40000 + 4 * 0x2000);
	check("map leaves EQU out", find_label("CONST").type < 0 && conf.curlabset->list.size() == 6);
	compSetHardware(comp, "ZX128");
	compReset(comp, RES_DEFAULT);
	loadLabels(fixtures.filePath("legacy.labels").toLocal8Bit().constData());
	xAdr legacy = find_label("print_score");
	check("legacy 16K labels stay compatible", legacy.abs == 0x8010 && legacy.adr == 0x8010
		&& find_label("screen_data").abs == 0x14010 && find_label("level_data").abs == 0x1a00);
	check("16K pages: decimal, and hex from older Xpeccy+ files",
		find_label("decimal_bank").abs == ((12 << 14) | 0x20) && find_label("old_hex_bank").abs == ((10 << 14) | 0x10));
	loadLabels(fixtures.filePath("sjasm-128.labels").toLocal8Bit().constData());
	check("SjASMPlus 128K labels", find_label("b7").abs == ((7 << 14) | 0x123) && find_label("b7").adr == 0xc123
		&& find_label("scr").abs == ((5 << 14) | 0x10) && find_label("scr").adr == 0x4010);
	loadLabels(fixtures.filePath("sjasm-128.map").toLocal8Bit().constData());
	check("SjASMPlus 128K map", find_label("B7").abs == ((7 << 14) | 0x123) && find_label("SCR").adr == 0x4010
		&& find_label("CONST").type < 0);
	loadLabels(fixtures.filePath("legacy.labels").toLocal8Bit().constData());
	xAdr rom0 = {MEM_ROM, 0, 0x0038, 0x38};	// the bench machine has no ROM file: one page
	add_label(rom0, "RomLabel");
	QString saved128 = temp.filePath("z128.labels");
	saveLabels(saved128.toLocal8Bit().constData());
	QFile f128(saved128);
	f128.open(QIODevice::ReadOnly);
	QByteArray text128 = f128.readAll();
	f128.close();
	loadLabels(saved128.toLocal8Bit().constData());
	check("16K ROM labels by name", text128.contains("ROM0:0038 RomLabel\n") && text128.contains("02:0010 print_score\n")
		&& find_label("RomLabel").type == MEM_ROM && find_label("RomLabel").abs == 0x38);
	loadLabels(fixtures.filePath("legacy.labels").toLocal8Bit().constData());
	check("legacy label expressions", !eval("print_score").err && eval("print_score").value == 0x8010);
	// ROM lists: namespaces, data ranges, system variables. The bench 128K gets
	// two 16K ROM banks, 0 paged in; the lists name them by the MD5 of their bytes
	comp->mem->romMask = 2 * MEM_16K - 1;
	for (int i = 0; i < 2 * MEM_16K; i++) comp->mem->romData[i] = (unsigned char)(i * 7 + (i >> 8) + (i >> 14) * 3);
	QString md5 = QCryptographicHash::hash(QByteArray((const char*)comp->mem->romData, MEM_16K),
		QCryptographicHash::Md5).toHex();
	QString md5b = QCryptographicHash::hash(QByteArray((const char*)comp->mem->romData + MEM_16K, MEM_16K),
		QCryptographicHash::Md5).toHex();
	QDir(temp.path()).mkdir("rom-labels");
	installDir = temp.path();
	const char* lists[][2] = {
		{"a.labels", "; a list\nrom LA %1\nLA:0010 SYSA.ENTRY\nLA:0020 SYSA.ONLYA\ndb LA:3D00 8\ndm LA:0100 4\n"
			"db LA:3D10 3*2\n"},
		{"b.labels", "rom LB %1 skip LB:0200 LB:3E00\nLB:0018 SYSB.ENTRY\ndw LB:0200 1\ndw LB:0300 1\ndb LB:3E00 2\n"},
		{"b2.labels", "rom LB2 %1\nLB2:0020 SYSA.ONLYA\n"},
		{"c.labels", "05:5C3D SYSA.ERR_SP\n05:5C3F SYSA.LIST_SP\n"},
		{"d.labels", "05:5C00 SYSC.NOT_FITTED\n"},
		{"e.labels", "rom LE %2\nLE:0030 SYSD.ONLYA\nLE:0040 SYSD.LATER\n"}};
	for (auto& lst : lists) {
		QFile out(temp.filePath(QString("rom-labels/") + lst[0]));
		out.open(QIODevice::WriteOnly);
		out.write(QString(lst[1]).replace("%1", md5).replace("%2", md5b).toLatin1());
	}
	conf.dbg.romlabels = 1;
	conf.dbg.sysvars = 1;
	memSetBank(comp->mem, 0x00, MEM_ROM, 0, MEM_16K, NULL, NULL, NULL);
	labels_rom_scan(comp);
	xAdr r10 = comp_get_xadr(comp, 0x10, MEM_VIEW_READ);
	xAdr r20 = comp_get_xadr(comp, 0x20, MEM_VIEW_READ);
	check("ROM names in namespaces", find_label("SYSA.ENTRY").adr == 0x10 && find_label("SYSB.ENTRY").adr == 0x18
		&& find_label("ONLYA").adr == 0x20 && eval("SYSA.ENTRY").value == 0x10);
	check("a short name of two systems is neither",
		find_label("ENTRY").type < 0 && eval("ENTRY").err && labels_ambiguous("ENTRY").size() == 2);
	check("rows show the short name where it is enough", find_label(r20) == "ONLYA" && find_label(r10) == "SYSA.ENTRY"
		&& find_label(r10, nullptr, LAB_FULL) == "SYSA.ENTRY" && label_local_prefix("SYSA.ENTRY") == 4);
	xAdr ram = comp_get_xadr(comp, 0x8000, MEM_VIEW_READ);
	add_label(ram, "ONLYA");
	check("a label of the user's takes the short name", find_label("ONLYA").adr == 0x8000
		&& find_label(r20) == "SYSA.ONLYA" && find_label("SYSA.ONLYA").adr == 0x20);
	del_label("ONLYA");
	check("data ranges, one dropped on that bank", labels_rom_view(0x3D00) == (DBG_VIEW_BYTE | DBG_STMT)
		&& labels_rom_view(0x3D07) == DBG_VIEW_BYTE && labels_rom_view(0x3D08) == DBG_VIEW_CODE
		&& labels_rom_view(0x101) == DBG_VIEW_TEXT && labels_rom_view(0x200) == DBG_VIEW_CODE
		&& labels_rom_view(0x300) == (DBG_VIEW_WORD | DBG_STMT) && labels_rom_view(0x301) == DBG_VIEW_WORD
		&& labels_rom_view(0x3E00) == DBG_VIEW_CODE);
	check("statements of a data range", labels_rom_view(0x3D10) == (DBG_VIEW_BYTE | DBG_STMT)
		&& labels_rom_view(0x3D11) == DBG_VIEW_BYTE && labels_rom_view(0x3D13) == (DBG_VIEW_BYTE | DBG_STMT)
		&& labels_rom_view(0x3D15) == DBG_VIEW_BYTE && labels_rom_view(0x3D16) == DBG_VIEW_CODE);
	check("a name two lists of a bank share is one cell", labels_rom_cells("SYSA.ONLYA").size() == 1);
	// ONLYA is SYSA's in bank 0 and SYSD's in bank 1: the one paged in
	xExpr paged = xexpr_compile("ONLYA");
	check("a short name of several systems: the ROM paged in", labels_shared("ONLYA")
		&& find_label("ONLYA").adr == 0x20 && (xexpr_eval(paged, comp) == 0x20)
		&& find_label("LATER").type == MEM_ROM && find_label("LATER").adr < 0);
	xExpr fullx = xexpr_compile("SYSD.ONLYA");	// a full name's address, cached by the paging
	bool ferr = false;
	xexpr_eval(fullx, comp, &ferr);
	memSetBank(comp->mem, 0x00, MEM_ROM, 1, MEM_16K, NULL, NULL, NULL);
	bool ferr2 = false;
	check("a ROM name's address follows the paging", ferr && (xexpr_eval(fullx, comp, &ferr2) == 0x30) && !ferr2);
	check("...and after the paging changes", find_label("ONLYA").adr == 0x30 && (xexpr_eval(paged, comp) == 0x30)
		&& find_label(comp_get_xadr(comp, 0x30, MEM_VIEW_READ)) == "ONLYA"
		&& find_label(comp_get_xadr(comp, 0x40, MEM_VIEW_READ)) == "LATER");
	// neither paged in (RAM at 0000): compiles, and in a condition it equals no
	// address, the rest of the condition as written
	memSetBank(comp->mem, 0x00, MEM_RAM, 0, MEM_16K, NULL, NULL, NULL);
	comp->cpu->regPC = 0;
	xExpr none = xexpr_compile("PC == ONLYA");
	xExpr ne = xexpr_compile("PC != ONLYA");
	xExpr either = xexpr_compile("PC == 0 || PC == ONLYA");
	bool nerr = false, nout = false;
	unsigned nres = xexpr_eval(none, comp, &nerr, 0, 0, 0, &nout);
	check("...with none paged in: compiled, no address", xexpr_ok(none) && !nerr && nout && (nres == 0)
		&& (labels_candidates("ONLYA").size() == 2) && (labels_ambiguous("ONLYA").size() == 2));
	nerr = nout = false;
	check("...each comparison as written", (xexpr_eval(ne, comp, &nerr, 0, 0, 0, &nout) == 1)
		&& (xexpr_eval(either, comp, &nerr, 0, 0, 0, &nout) == 1) && !nerr && nout);
	nerr = false;
	xexpr_eval(none, comp, &nerr);
	check("...an error where nothing asks for that", nerr);
	// a label of the user's gone: the condition is broken, not false
	add_label(comp_get_xadr(comp, 0x8100, MEM_VIEW_READ), "GONE");
	xExpr gone = xexpr_compile("PC == GONE");
	del_label("GONE");
	nerr = nout = false;
	xexpr_eval(gone, comp, &nerr, 0, 0, 0, &nout);
	check("a label no longer there is an error", xexpr_ok(gone) && nerr && !nout);
	memSetBank(comp->mem, 0x00, MEM_ROM, 0, MEM_16K, NULL, NULL, NULL);
	nerr = nout = false;
	check("...and paged in again", (xexpr_eval(paged, comp, &nerr, 0, 0, 0, &nout) == 0x20) && !nerr && !nout);
	xAdr var = comp_get_xadr(comp, 0x5C3D, MEM_VIEW_READ);
	check("system variables in operands only", find_label(var).isEmpty()
		&& find_label(var, nullptr, LAB_SYSVAR) == "ERR_SP" && find_label(var, nullptr, LAB_SYSVAR | LAB_FULL) == "SYSA.ERR_SP"
		&& eval("ERR_SP").value == 0x5C3D && eval("SYSA.ERR_SP").value == 0x5C3D);
	xAdr high = var;
	high.adr = 0xDC3D;		// bank 5 paged at C000 as well
	check("a system variable only at its own address", find_label(high, nullptr, LAB_SYSVAR).isEmpty()
		&& !labels_sys_size(high));
	check("a system variable's size, to the next one", labels_sys_size(var) == 2
		&& labels_sys_size(comp_get_xadr(comp, 0x5C3F, MEM_VIEW_READ)) == 2);
	check("a system not in the machine names nothing", find_label("NOT_FITTED").type < 0
		&& find_label("SYSC.NOT_FITTED").type < 0);
	// a user's file of the text format: labels, data and its statements, comments,
	// breakpoints, a place in a ROM of the lists
	QString user = temp.filePath("game.labels");
	QFile uf(user);
	uf.open(QIODevice::WriteOnly);
	uf.write(":8000 START\n05:1A00 SPRITES\ncode :8000 4\ndb :9000 7*4\ndw :9100 2\ndc :9200 3\n"
		"comment :8003 jump table\\nstarts here\nLA:0010 MYROM\ndb LA:3D20 2\nstruct LATER db 1\n"
		"brk START exec\nbrk :5B00-5BFF write count\nbrk io:00FE/00FF read\nbrk 05:1A00 read log \"A,DE\"\n"
		"brk LA:0010 if \"A == #FF\" span=open\nbrk irq off\nbrk if \"FRAME == 300\" onchange\n"
		"brk :8100 frobnicate\nbrk NOSUCH\n");
	uf.close();
	conf.brk.list.clear();
	check("load a file of the text format", loadLabels(user.toLocal8Bit().constData()) == 1);
	auto flag = [&](int adr) { return comp->brkRamMap[comp_get_xadr(comp, adr, MEM_VIEW_READ).abs] & 0xf0; };
	check("its labels", find_label("START").adr == 0x8000 && find_label("SPRITES").abs == ((5 << 14) | 0x1a00)
		&& find_label("MYROM").type == MEM_ROM && find_label("MYROM").abs == 0x10);
	check("its data, statement by statement", flag(0x9000) == (DBG_VIEW_BYTE | DBG_STMT) && flag(0x9001) == DBG_VIEW_BYTE
		&& flag(0x9007) == (DBG_VIEW_BYTE | DBG_STMT) && flag(0x901B) == DBG_VIEW_BYTE && flag(0x901C) == 0
		&& flag(0x9100) == (DBG_VIEW_WORD | DBG_STMT) && flag(0x9102) == DBG_VIEW_WORD && flag(0x9104) == 0
		&& flag(0x9200) == (DBG_VIEW_TEXT | DBG_STMT) && flag(0x9202) == DBG_VIEW_TEXT
		&& flag(0x8000) == (DBG_VIEW_EXEC | DBG_STMT) && flag(0x8003) == DBG_VIEW_EXEC && flag(0x8004) == 0
		&& (comp->brkRomMap[0x3D20] & 0xf0) == (DBG_VIEW_BYTE | DBG_STMT));
	check("its comment, two lines", find_comment(comp_get_xadr(comp, 0x8003, MEM_VIEW_READ)) == "jump table\nstarts here");
	QStringList points;
	for (const xBrkPoint& b : conf.brk.list) points.append(brk_text_line(b));
	check("its breakpoints", points == QStringList({"brk :8000 exec", "brk :5B00-5BFF write count",
		"brk io:00FE/00FF read", "brk 05:1A00 read log \"A,DE\"", "brk ROM0:0010 exec if \"A == #FF\" span=open",
		"brk irq off", "brk if \"FRAME == 300\" onchange", "brk :8100 exec"}));
	// a session: all of it saved, then read in place of what the Debugger has
	auto marks = [&]() {
		QByteArray m((const char*)comp->brkRamMap, comp->mem->ramMask + 1);
		m.append((const char*)comp->brkRomMap, comp->mem->romMask + 1);
		for (int i = 0; i < m.size(); i++) m[i] = (char)(m.at(i) & 0xf0);
		return m;
	};
	auto userLabels = [&]() {
		QStringList res;
		for (auto it = conf.curlabset->list.begin(); it != conf.curlabset->list.end(); ++it)
			res.append(QString("%1 %2 %3").arg(it.key()).arg(it.value().type).arg(it.value().abs, 0, 16));
		return res;
	};
	QByteArray marksBefore = marks();
	QStringList labsBefore = userLabels();
	auto commBefore = conf.commap;
	conf.macId = "ZX128";
	QString sess = temp.filePath("game.xdbg");
	check("save a session", session_save(sess, true, true) == 1);
	QFile sf(sess);
	sf.open(QIODevice::ReadOnly);
	QByteArray stext = sf.readAll();
	sf.close();
	QString mac;
	QStringList media;
	session_media(sess, &mac, &media);
	check("a session names its machine", stext.startsWith("; Xpeccy+ Debugger session") && mac == "ZX128" && media.isEmpty()
		&& stext.contains("\ndb :9000 7*4\n") == false && stext.contains("\ndb 02:1000 7*4\n")
		&& stext.contains("\ncomment 02:0003 jump table\\nstarts here\n"));
	add_comment(comp_get_xadr(comp, 0x8100, MEM_VIEW_READ), "stray");
	comp->brkRamMap[comp_get_xadr(comp, 0x8200, MEM_VIEW_READ).abs] |= DBG_VIEW_WORD;
	add_label(comp_get_xadr(comp, 0x8300, MEM_VIEW_READ), "STRAY");
	check("load a session", session_load(sess) == 1);
	QStringList sessPoints;
	for (const xBrkPoint& b : conf.brk.list) sessPoints.append(brk_text_line(b));
	check("a session read back is what was saved", (marks() == marksBefore) && (conf.commap == commBefore)
		&& (sessPoints == points) && (userLabels() == labsBefore) && (conf.curlabset->name == "game.xdbg"));
	// its lines stand alone: sorted, the file still says the same
	QList<QByteArray> slines = stext.split('\n');
	std::sort(slines.begin(), slines.end());
	QString sorted = temp.filePath("sorted.xdbg");
	QFile so(sorted);
	so.open(QIODevice::WriteOnly);
	so.write(slines.join('\n'));
	so.close();
	session_load(sorted);
	QStringList sortedPoints;
	for (const xBrkPoint& b : conf.brk.list) sortedPoints.append(brk_text_line(b));
	sortedPoints.sort();
	QStringList pointsSorted = points;
	pointsSorted.sort();
	check("a session sorted line by line loads the same", (marks() == marksBefore) && (conf.commap == commBefore)
		&& (sortedPoints == pointsSorted) && (userLabels() == labsBefore));
	session_load(sess);		// the points in the file's order again
	QString blist = temp.filePath("points.txt");
	brk_save_list(blist.toLocal8Bit().constData());
	conf.brk.list.clear();
	int bbad = 0;
	brk_load_list(blist.toLocal8Bit().constData(), &bbad);
	QStringList again;
	for (const xBrkPoint& b : conf.brk.list) again.append(brk_text_line(b));
	check("a breakpoint list saved and read back", (again == points) && !bbad);
	QFile old(blist);
	old.open(QIODevice::WriteOnly);
	old.write("; an older list\nCPU:8000::F:DBG::\nRAM:5:0010-001F:W:CNT::\nx0=0x8100\n");
	old.close();
	brk_load_list(blist.toLocal8Bit().constData(), &bbad);
	again.clear();
	for (const xBrkPoint& b : conf.brk.list) again.append(brk_text_line(b));
	check("an older breakpoint list still reads", !bbad
		&& (again == QStringList({"brk :8000 exec", "brk 05:0010-001F write count", "brk :8100 exec"})));
	conf.brk.list.clear();
	conf.dbg.sysvars = 0;
	check("system variables switched off", find_label(var, nullptr, LAB_SYSVAR).isEmpty() && find_label("ERR_SP").type < 0);
	conf.dbg.romlabels = 0;
	check("ROM labels switched off", find_label(r20).isEmpty() && labels_rom_view(0x3D00) == DBG_VIEW_CODE);
	while (!conf.labsets.isEmpty()) delLabelSet(conf.labsets.first()->name);
	compDestroy(comp);
	conf.zx = nullptr;
	return failures ? 1 : 0;
}

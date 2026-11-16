// Headless .pok bench: libxpeccy's reader over the files given, then the pokes going in
// and out of a 16K, a 48K and a 128K. A section each:
//
//	read/<file>	what the reader made of it: the trainers, their pokes, why one is spoiled
//	apply16/48/128	check, apply, undo, the place a page goes to, a rollback's verdict
//
//	pok-bench file.pok...
//	pok-bench -sweep file.pok...	one line a file, for a run over a whole collection

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spectrum.h"
#include "pokes.h"

static const char* base_name(const char* path) {
	const char* p = path;
	for (const char* s = path; *s; s++)
		if ((*s == '/') || (*s == '\\')) p = s + 1;
	return p;
}

static char* read_all(const char* path, size_t* len) {
	FILE* f = fopen(path, "rb");
	if (!f) return NULL;
	size_t cap = 1 << 16, n = 0, got;
	char* buf = malloc(cap);
	while ((got = fread(buf + n, 1, cap - n, f)) > 0) {
		n += got;
		if (n == cap) buf = realloc(buf, cap *= 2);
	}
	fclose(f);
	*len = n;
	return buf;
}

static const char* why_name[] = {"", "line", "range", "key", "open", "empty"};

static int parse_text(const char* text, xPokFile* pf) {
	return pok_parse(text, strlen(text), pf);
}

static void print_file(const char* path) {
	size_t len;
	char* buf = read_all(path, &len);
	printf("== read/%s\n", base_name(path));
	if (!buf) {
		printf("can't open\n");
		return;
	}
	xPokFile pf;
	int err = pok_parse(buf, len, &pf);
	free(buf);
	printf("result %d, %d trainers\n", err, pf.cnt);
	for (int i = 0; i < pf.cnt; i++) {
		xTrainer* tr = &pf.list[i];
		printf("'%s'", tr->name);
		if (tr->why) printf(" bad=%s@%d", why_name[tr->why], tr->line);
		if (tr->ask) printf(" ask");
		printf(":");
		for (int j = 0; j < tr->cnt; j++)
			printf(" %d:%d=%d/%d", tr->pokes[j].bank, tr->pokes[j].adr, tr->pokes[j].val, tr->pokes[j].org);
		printf("\n");
	}
	pok_free(&pf);
}

static void sweep_file(const char* path) {
	size_t len;
	char* buf = read_all(path, &len);
	if (!buf) {
		printf("open-error %s\n", path);
		return;
	}
	xPokFile pf;
	int err = pok_parse(buf, len, &pf);
	free(buf);
	int bad = 0, pokes = 0, ask = 0, known = 0;
	for (int i = 0; i < pf.cnt; i++) {
		bad += pf.list[i].why != 0;
		ask += pf.list[i].ask;
		pokes += pf.list[i].cnt;
		for (int j = 0; j < pf.list[i].cnt; j++)
			known += pf.list[i].pokes[j].org != 0;
	}
	printf("%s err=%d trainers=%d bad=%d pokes=%d known=%d ask=%d\n", path, err, pf.cnt, bad, pokes, known, ask);
	for (int i = 0; i < pf.cnt; i++)
		if (pf.list[i].why)
			printf("  bad '%s' %s@%d\n", pf.list[i].name, why_name[pf.list[i].why], pf.list[i].line);
	pok_free(&pf);
}

static Computer* machine(const char* hw, int ram) {
	Computer* comp = compCreate();
	if (!compSetHardware(comp, hw)) {
		printf("no %s core\n", hw);
		exit(1);
	}
	memSetSize(comp->mem, ram, MEM_32K);
	compReset(comp, RES_DEFAULT);
	memset(comp->mem->ramData, 0, MEM_128K);
	return comp;
}

static int cpu_rd(Computer* comp, int adr) {
	return pok_peek(comp, 8, adr);
}

static void cpu_wr(Computer* comp, int adr, int val) {
	comp->mem->ramData[mem_get_phys_adr(comp->mem, adr) & comp->mem->ramMask] = val;
}

static int page_rd(Computer* comp, int page, int adr) {
	return comp->mem->ramData[(page << 14) | (adr & 0x3fff)];
}

static xTrainer* one(xPokFile* pf, const char* text) {
	if (parse_text(text, pf) != POK_OK) {
		printf("can't read: %s\n", text);
		exit(1);
	}
	return &pf->list[0];
}

static void test_48(void) {
	printf("== apply48\n");
	Computer* comp = machine("ZX48", MEM_64K);
	xPokFile pf;
	xTrainer* tr = one(&pf, "NLives\nZ 8 55877 127 218\n");
	cpu_wr(comp, 55877, 218);
	printf("fits: check=%d", pok_check(comp, tr));
	printf(" apply=%d", pok_apply(comp, tr, 0));
	printf(" byte=%d where=%d", cpu_rd(comp, 55877), pok_where(comp, tr, 0));
	pok_undo(comp, tr, 0);
	printf(" undo byte=%d\n", cpu_rd(comp, 55877));
	cpu_wr(comp, 55877, 17);
	printf("other release: check=%d", pok_check(comp, tr));
	pok_apply(comp, tr, 0);
	pok_undo(comp, tr, 0);
	printf(" forced and undone byte=%d\n", cpu_rd(comp, 55877));
	cpu_wr(comp, 55877, 127);
	printf("already poked: check=%d", pok_check(comp, tr));
	pok_apply(comp, tr, 0);
	pok_undo(comp, tr, 0);
	printf(" undo byte=%d\n", cpu_rd(comp, 55877));
	pok_free(&pf);

	tr = one(&pf, "NAsk\nM 8 30000 0 0\nZ 2 32768 256 0\n");
	printf("no org: check=%d", pok_check(comp, tr));
	pok_apply(comp, tr, 99);
	printf(" 256 as 99: #8000=%d where=%d\n", cpu_rd(comp, 0x8000), pok_where(comp, tr, 99));
	pok_undo(comp, tr, 99);
	pok_free(&pf);

	tr = one(&pf, "NPages\nM 5 16384 1 0\nM 2 32768 2 0\nZ 0 58813 3 0\n");
	printf("pages 5/2/0: check=%d", pok_check(comp, tr));
	pok_apply(comp, tr, 0);
	printf(" #4000=%d #8000=%d 58813=%d\n", cpu_rd(comp, 0x4000), cpu_rd(comp, 0x8000), cpu_rd(comp, 58813));
	pok_undo(comp, tr, 0);
	pok_free(&pf);

	tr = one(&pf, "NPage 7\nM 8 40000 9 0\nZ 7 49152 1 0\n");
	printf("page 7: check=%d apply=%d 40000=%d\n", pok_check(comp, tr), pok_apply(comp, tr, 0), cpu_rd(comp, 40000));
	pok_free(&pf);
	tr = one(&pf, "NRom\nZ 8 100 0 0\n");
	printf("rom: check=%d apply=%d\n", pok_check(comp, tr), pok_apply(comp, tr, 0));
	pok_free(&pf);

	tr = one(&pf, "NTwice\nM 8 50006 1 0\nM 8 50006 2 0\nZ 8 50007 3 0\n");
	cpu_wr(comp, 50006, 77);
	pok_apply(comp, tr, 0);
	printf("twice: 50006=%d where=%d", cpu_rd(comp, 50006), pok_where(comp, tr, 0));
	pok_undo(comp, tr, 0);
	printf(" undo 50006=%d\n", cpu_rd(comp, 50006));

	pok_apply(comp, tr, 0);
	cpu_wr(comp, 50006, 77);
	cpu_wr(comp, 50007, 0);
	printf("rolled back: where=%d", pok_where(comp, tr, 0));
	pok_apply(comp, tr, 0);
	printf(" again where=%d", pok_where(comp, tr, 0));
	cpu_wr(comp, 50007, 5);
	printf(" changed where=%d\n", pok_where(comp, tr, 0));
	pok_free(&pf);

	tr = one(&pf, "NGame\nM 8 50010 1 0\nZ 8 50011 2 0\n");
	cpu_wr(comp, 50010, 30);
	cpu_wr(comp, 50011, 31);
	pok_apply(comp, tr, 0);
	cpu_wr(comp, 50010, 99);		// the game wrote there, or was loaded again
	pok_undo(comp, tr, 0);
	printf("changed since: undo 50010=%d 50011=%d\n", cpu_rd(comp, 50010), cpu_rd(comp, 50011));
	pok_free(&pf);

	xTrainer hand;
	memset(&hand, 0, sizeof(hand));
	for (int i = 0; i < 4; i++) cpu_wr(comp, 40000 + i, 10 + i);
	for (int i = 0; i < 4; i++) pok_poke(comp, &hand, 8, 40000 + i, 200 + i);
	printf("by hand: rom=%d", pok_poke(comp, &hand, 8, 0x100, 1));
	for (int i = 0; i < 4; i++) printf(" %d", cpu_rd(comp, 40000 + i));
	pok_undo(comp, &hand, 0);
	printf(" undo");
	for (int i = 0; i < 4; i++) printf(" %d", cpu_rd(comp, 40000 + i));
	printf("\n");
	pok_trainer_free(&hand);
	compDestroy(comp);
}

static void test_128(void) {
	printf("== apply128\n");
	Computer* comp = machine("ZX128", MEM_128K);
	comp->p7FFD = 7;
	comp->hw->mapMem(comp);
	xPokFile pf;
	xTrainer* tr = one(&pf, "NPage 0\nM 0 58813 201 0\nZ 5 16384 7 0\n");
	printf("page 7 in: check=%d", pok_check(comp, tr));
	printf(" apply=%d", pok_apply(comp, tr, 0));
	printf(" page0=%d page7=%d cpu 58813=%d cpu #4000=%d\n", page_rd(comp, 0, 58813), page_rd(comp, 7, 58813),
		cpu_rd(comp, 58813), cpu_rd(comp, 0x4000));
	comp->p7FFD = 0;
	comp->hw->mapMem(comp);
	printf("page 0 in: cpu 58813=%d where=%d", cpu_rd(comp, 58813), pok_where(comp, tr, 0));
	pok_undo(comp, tr, 0);
	printf(" undo page0=%d\n", page_rd(comp, 0, 58813));
	pok_free(&pf);
	compDestroy(comp);
}

static void test_16(void) {
	printf("== apply16\n");
	Computer* comp = machine("ZX48", MEM_16K);
	xPokFile pf;
	xTrainer* tr = one(&pf, "NPage 2\nZ 2 32768 1 0\n");
	printf("page 2: %d", pok_check(comp, tr));
	pok_free(&pf);
	tr = one(&pf, "NUpper\nZ 8 32768 1 0\n");
	printf(" #8000: %d", pok_check(comp, tr));
	pok_free(&pf);
	tr = one(&pf, "NScreen\nZ 5 16384 1 0\n");
	printf(" page 5: %d\n", pok_check(comp, tr));
	pok_free(&pf);
	compDestroy(comp);
}

// A page on the Next is its own 16K bank, the one #7FFD pages in: its RAM starts past the
// ROMs in the SRAM, in 8K cells of two to a bank
static void test_next(void) {
	printf("== applynext\n");
	Computer* comp = compCreate();
	if (!compSetHardware(comp, "ZXNext")) {
		printf("no ZXNext core\n");
		exit(1);
	}
	compReset(comp, RES_DEFAULT);
	unsigned char* ram = comp->mem->ramData;
	ram[(5 << 14) + 16] = 0x55;		// where a page counted from the SRAM's start would land
	ram[(7 << 14) + 20] = 0x55;
	int ty, ab;
	int b5 = comp_cell_xadr(comp, CELL_RAM, 10, 16, &ty, &ab) ? ab : -1;
	int b7 = comp_cell_xadr(comp, CELL_RAM, 14, 20, &ty, &ab) ? ab : -1;
	if ((b5 < 0) || (b7 < 0)) {
		printf("no RAM cells 10/14\n");
		exit(1);
	}
	ram[b5] = 0;
	ram[b7] = 0;
	comp->hw->out(comp, 0x7ffd, 7);
	xPokFile pf;
	xTrainer* tr = one(&pf, "NPages\nM 5 16400 7 0\nZ 7 49172 9 0\n");
	printf("check=%d", pok_check(comp, tr));
	printf(" apply=%d", pok_apply(comp, tr, 0));
	printf(" bank5=%d bank7=%d cpu #4010=%d cpu #C014=%d sram #14010=%d sram #1C014=%d\n",
		ram[b5], ram[b7], cpu_rd(comp, 0x4010), cpu_rd(comp, 0xc014), ram[(5 << 14) + 16], ram[(7 << 14) + 20]);
	pok_undo(comp, tr, 0);
	printf("undo bank5=%d bank7=%d\n", ram[b5], ram[b7]);
	pok_free(&pf);
	compDestroy(comp);
}

int main(int argc, char** argv) {
	if (argc > 1 && !strcmp(argv[1], "-sweep")) {
		for (int i = 2; i < argc; i++) sweep_file(argv[i]);
		return 0;
	}
	for (int i = 1; i < argc; i++) print_file(argv[i]);
	test_16();
	test_48();
	test_128();
	test_next();
	return 0;
}

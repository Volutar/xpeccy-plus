#include <stdlib.h>
#include <string.h>

#include "pokes.h"
#include "spectrum.h"

// reading

int pok_add_trainer(xPokFile* pf, const char* name, int len) {
	if (pf->cnt == pf->cap) {
		int cap = pf->cap ? pf->cap * 2 : 16;
		xTrainer* nl = (xTrainer*)realloc(pf->list, cap * sizeof(xTrainer));
		if (!nl) return 0;
		pf->list = nl;
		pf->cap = cap;
	}
	xTrainer* tr = &pf->list[pf->cnt++];
	memset(tr, 0, sizeof(xTrainer));
	while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t'))
		len--;
	if (len > POK_NAME_MAX) len = POK_NAME_MAX;
	memcpy(tr->name, name, len);
	tr->name[len] = 0;
	return 1;
}

int pok_add_poke(xTrainer* tr, int bank, int adr, int val, int org) {
	if (tr->cnt == tr->cap) {
		int cap = tr->cap ? tr->cap * 2 : 8;
		xPoke* np = (xPoke*)realloc(tr->pokes, cap * sizeof(xPoke));
		if (!np) return 0;
		tr->pokes = np;
		tr->cap = cap;
	}
	xPoke* pk = &tr->pokes[tr->cnt++];
	pk->bank = bank;
	pk->adr = adr;
	pk->val = val;
	pk->org = org;
	pk->saved = -1;
	if (val > 255) tr->ask = 1;
	return 1;
}

static void pk_spoil(xTrainer* tr, int why, int line) {
	if (tr->why) return;
	tr->why = why;
	tr->line = line;
}

// four decimal numbers apart by spaces or tabs; whatever follows them is left alone
static int pk_numbers(const char* s, const char* e, int* num) {
	int i;
	for (i = 0; i < 4; i++) {
		while (s < e && (*s == ' ' || *s == '\t')) s++;
		if (s == e || *s < '0' || *s > '9') return 0;
		long v = 0;
		while (s < e && *s >= '0' && *s <= '9') {
			if (v < 1000000) v = v * 10 + (*s - '0');
			s++;
		}
		if (s < e && *s != ' ' && *s != '\t') return 0;
		num[i] = (int)v;
	}
	return 1;
}

// Keyed by a line's first char, as the file's own rules have it. A bad line spoils the
// trainer it is in, not the file; no Y at the end is fine; what follows Y is not read.
int pok_parse(const char* buf, size_t len, xPokFile* pf) {
	memset(pf, 0, sizeof(xPokFile));
	if (len > POK_FILE_MAX) return POK_ERR_SIZE;
	const char* p = buf;
	const char* end = buf + len;
	xTrainer* cur = NULL;		// the trainer before its Z
	int line = 0;
	while (p < end) {
		const char* e = p;
		while (e < end && *e != '\r' && *e != '\n') e++;
		line++;
		const char* s = p;
		p = e;
		if (p < end && *p == '\r') p++;
		if (p < end && *p == '\n') p++;
		if (s == e) continue;
		int key = *s & 0xff;
		if (key >= 'a' && key <= 'z') key -= 0x20;
		if (key == 'Y') break;
		if (key == 'N') {
			if (cur) pk_spoil(cur, cur->cnt ? POK_BAD_OPEN : POK_BAD_EMPTY, line - 1);
			if (!pok_add_trainer(pf, s + 1, (int)(e - s - 1))) {pok_free(pf); return POK_ERR_MEM;}
			cur = &pf->list[pf->cnt - 1];
		} else if (key == 'M' || key == 'Z') {
			if (!cur) continue;		// outside a trainer: not a poke of any
			int n[4];
			if (!pk_numbers(s + 1, e, n)) {
				pk_spoil(cur, POK_BAD_LINE, line);
			} else if (n[0] > 255 || n[1] > 0xffff || n[2] > 256 || n[3] > 255) {
				pk_spoil(cur, POK_BAD_RANGE, line);
			} else if (!pok_add_poke(cur, n[0], n[1], n[2], n[3])) {
				pok_free(pf);
				return POK_ERR_MEM;
			}
			if (key == 'Z') cur = NULL;
		} else if (cur) {
			pk_spoil(cur, POK_BAD_KEY, line);
		}
	}
	if (cur) pk_spoil(cur, cur->cnt ? POK_BAD_OPEN : POK_BAD_EMPTY, line);
	int i;
	int good = 0;
	for (i = 0; i < pf->cnt; i++) {
		if (!pf->list[i].why && !pf->list[i].cnt)
			pk_spoil(&pf->list[i], POK_BAD_EMPTY, 0);
		if (!pf->list[i].why) good++;
	}
	if (!good) {
		pok_free(pf);
		return POK_ERR_EMPTY;
	}
	return POK_OK;
}

void pok_trainer_free(xTrainer* tr) {
	free(tr->pokes);
	tr->pokes = NULL;
	tr->cnt = 0;
	tr->cap = 0;
}

void pok_free(xPokFile* pf) {
	int i;
	for (i = 0; i < pf->cnt; i++)
		pok_trainer_free(&pf->list[i]);
	free(pf->list);
	memset(pf, 0, sizeof(xPokFile));
}

// memory

// A 128K page: every one on a machine with 128K or more, else the ones it has paged
// in (5/2/0 on a 48K, at #4000/#8000/#C000).
static int pk_page_ok(Computer* comp, int page) {
	Memory* mem = comp->mem;
	if ((size_t)((page + 1) << 14) > mem_ram_extent(mem)) return 0;
	if (mem->ramSize >= MEM_128K) return 1;
	return memFindAdr(mem, MEM_RAM, page << 14) >= 0;
}

static unsigned char* pk_byte(Computer* comp, int bank, int adr, int* err) {
	Memory* mem = comp->mem;
	if (bank & 8) {
		adr &= 0xffff;
		if (mem_get_page(mem, adr)->type != MEM_RAM) {
			*err = POK_NO_RAM;
			return NULL;
		}
		return &mem->ramData[mem_get_phys_adr(mem, adr) & mem->ramMask];
	}
	bank &= 7;
	if (!pk_page_ok(comp, bank)) {
		*err = POK_NO_PAGE;
		return NULL;
	}
	// the Next's 16K bank of that number, the one #7FFD pages in: its RAM starts past the ROMs
	if (comp->hw && (comp->hw->id == HW_ZXNEXT)) {
		int type, abs;
		if (!comp_cell_xadr(comp, CELL_RAM, (bank << 1) | ((adr >> 13) & 1), adr & 0x1fff, &type, &abs)) {
			*err = POK_NO_PAGE;
			return NULL;
		}
		return &mem->ramData[abs & mem->ramMask];
	}
	return &mem->ramData[((bank << 14) | (adr & 0x3fff)) & mem->ramMask];
}

int pok_peek(Computer* comp, int bank, int adr) {
	int err;
	unsigned char* p = pk_byte(comp, bank, adr, &err);
	return p ? *p : -1;
}

static int pk_places(Computer* comp, const xTrainer* tr) {
	int i, err = POK_FITS;
	for (i = 0; i < tr->cnt; i++) {
		if (!pk_byte(comp, tr->pokes[i].bank, tr->pokes[i].adr, &err))
			return err;
	}
	return POK_FITS;
}

static int pk_val(const xPoke* pk, int val) {
	return (pk->val > 255) ? (val & 0xff) : pk->val;
}

int pok_check(Computer* comp, const xTrainer* tr) {
	int err = pk_places(comp, tr);
	if (err != POK_FITS) return err;
	int i, known = 0;
	for (i = 0; i < tr->cnt; i++) {
		const xPoke* pk = &tr->pokes[i];
		if (!pk->org) continue;
		known++;
		int b = pok_peek(comp, pk->bank, pk->adr);
		if (b != pk->org && (pk->val > 255 || b != pk->val))
			return POK_MISMATCH;
	}
	return known ? POK_FITS : POK_UNCHECKED;
}

// What goes back on undo: the byte found, but org when the game came already poked
// (the value found is the trainer's own, and org says what was there before).
static void pk_write(Computer* comp, xPoke* pk, int val) {
	int err;
	unsigned char* p = pk_byte(comp, pk->bank, pk->adr, &err);
	int v = pk_val(pk, val);
	pk->saved = (*p == v && pk->org) ? pk->org : *p;
	vid_outside_write(comp->vid);	// a still picture goes, the rest drawn first
	*p = v;
	x_input_bump();		// the memory changed from outside: frames run ahead go
}

int pok_apply(Computer* comp, xTrainer* tr, int val) {
	int err = pk_places(comp, tr);
	if (err != POK_FITS) return err;
	int i;
	for (i = 0; i < tr->cnt; i++)
		pk_write(comp, &tr->pokes[i], val);
	return POK_FITS;
}

// backwards, so an address poked twice gets its first saved byte last. Only where the
// poke's value still is: a byte the game changed since, or loaded again, is its own.
void pok_undo(Computer* comp, xTrainer* tr, int val) {
	int i, err;
	vid_outside_write(comp->vid);
	for (i = tr->cnt - 1; i >= 0; i--) {
		xPoke* pk = &tr->pokes[i];
		if (pk->saved < 0) continue;
		unsigned char* p = pk_byte(comp, pk->bank, pk->adr, &err);
		if (p && (*p == pk_val(pk, val))) *p = pk->saved;
		pk->saved = -1;
	}
	x_input_bump();
}

static int pk_same_place(const xPoke* a, const xPoke* b) {
	if ((a->bank & 8) != (b->bank & 8)) return 0;
	if (a->bank & 8) return a->adr == b->adr;
	return ((a->bank & 7) == (b->bank & 7)) && ((a->adr & 0x3fff) == (b->adr & 0x3fff));
}

// Per address: the last poke's value there means on, the first poke's saved byte off.
int pok_where(Computer* comp, const xTrainer* tr, int val) {
	int i, j, on = 1, off = 1;
	for (i = 0; i < tr->cnt && (on || off); i++) {
		const xPoke* pk = &tr->pokes[i];
		int b = pok_peek(comp, pk->bank, pk->adr);
		if (b < 0) return POK_AT_OTHER;
		int last = 1, first = 1;
		for (j = 0; j < tr->cnt; j++) {
			if (j == i || !pk_same_place(pk, &tr->pokes[j])) continue;
			if (j > i) last = 0;
			if (j < i) first = 0;
		}
		if (last && b != pk_val(pk, val)) on = 0;
		if (first && b != pk->saved) off = 0;
	}
	return on ? POK_AT_ON : off ? POK_AT_OFF : POK_AT_OTHER;
}

int pok_poke(Computer* comp, xTrainer* tr, int bank, int adr, int val) {
	int err;
	if (!pk_byte(comp, bank, adr, &err)) return err;
	if (!pok_add_poke(tr, bank, adr, val & 0xff, 0)) return POK_NO_RAM;
	pk_write(comp, &tr->pokes[tr->cnt - 1], 0);
	return POK_FITS;
}

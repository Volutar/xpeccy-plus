// Copies of LD-BYTES in ram, found and checked for the tape trap

#include "tapetrap.h"
#include "../libxpeccy/xlog.h"

int ldcReads = 0;		// tape->portReads at the last look for a copy
static int ldcProbed = -1;	// the pc after an IN already looked at this frame

int tap_byte(Computer* comp, int adr) {
	return memRd(comp->mem, adr & 0xffff) & 0xff;
}

int tap_peek(int adr, void* comp) {
	return tap_byte((Computer*)comp, adr);
}

// LD-BYTES copied into ram, and often touched up on the way: the rom's bytes
// #0556-#05F9 at another address, the calls inside it moved along. A copy is
// the Sinclair rom's, whatever rom the machine runs, hence the table.
static const unsigned char ldcRom[LDC_LEN] = {
	0x14,0x08,0x15,0xF3,0x3E,0x0F,0xD3,0xFE,0x21,0x3F,0x05,0xE5,0xDB,0xFE,0x1F,0xE6,
	0x20,0xF6,0x02,0x4F,0xBF,0xC0,0xCD,0xE7,0x05,0x30,0xFA,0x21,0x15,0x04,0x10,0xFE,
	0x2B,0x7C,0xB5,0x20,0xF9,0xCD,0xE3,0x05,0x30,0xEB,0x06,0x9C,0xCD,0xE3,0x05,0x30,
	0xE4,0x3E,0xC6,0xB8,0x30,0xE0,0x24,0x20,0xF1,0x06,0xC9,0xCD,0xE7,0x05,0x30,0xD5,
	0x78,0xFE,0xD4,0x30,0xF4,0xCD,0xE7,0x05,0xD0,0x79,0xEE,0x03,0x4F,0x26,0x00,0x06,
	0xB0,0x18,0x1F,0x08,0x20,0x07,0x30,0x0F,0xDD,0x75,0x00,0x18,0x0F,0xCB,0x11,0xAD,
	0xC0,0x79,0x1F,0x4F,0x13,0x18,0x07,0xDD,0x7E,0x00,0xAD,0xC0,0xDD,0x23,0x1B,0x08,
	0x06,0xB2,0x2E,0x01,0xCD,0xE3,0x05,0xD0,0x3E,0xCB,0xB8,0xCB,0x15,0x06,0xB0,0xD2,
	0xCA,0x05,0x7C,0xAD,0x67,0x7A,0xB3,0x20,0xCA,0x7C,0xFE,0x01,0xC9,0xCD,0xE7,0x05,
	0xD0,0x3E,0x16,0x3D,0x20,0xFD,0xA7,0x04,0xC8,0x3E,0x7F,0xDB,0xFE,0x1F,0xD0,0xA9,
	0xE6,0x20,0x28,0xF3
};

// The operands of the calls and the jump inside it, which move with the copy
static const int ldcRel[] = {0x0017, 0x0026, 0x002d, 0x003c, 0x0046, 0x0075, 0x0080, 0x008e};

// What a copy may change and still read bytes as the rom does, which is all a
// hand-over asks of it: border colours, the SA/LD-RET it pushes, BREAK, and the
// timing constants. The stripes after an edge (#05FA on) are not compared.
static const int ldcLoose[] = {
	0x055b, 0x055f, 0x0560, 0x0568, 0x05a1,				// colours, SA/LD-RET
	0x056b, 0x05f0, 0x05f4,						// BREAK
	0x0572, 0x0573, 0x0581, 0x0588, 0x0590,				// timings
	0x0598, 0x05a6, 0x05c7, 0x05cf, 0x05d4, 0x05e8
};

// how each byte of a copy is compared
enum {LDC_SAME = 0, LDC_SKIP, LDC_IX};
static unsigned char ldcKind[LDC_LEN];

static void ldc_kinds() {
	static bool done = false;
	if (done) return;
	for (int r : ldcRel) {
		ldcKind[r] = LDC_SKIP;
		ldcKind[r + 1] = LDC_SKIP;
	}
	for (int a : ldcLoose)
		ldcKind[a - LD_ROM_BASE] = LDC_SKIP;
	ldcKind[LDC_INCIX] = LDC_IX;
	done = true;
}

// Is there a copy at base, from byte from up to to? Its INC IX may be a DEC IX,
// for a loader that loads downwards: dir says which.
static int ldc_check(Computer* comp, int base, int* dir, int from = 0, int to = LDC_LEN) {
	ldc_kinds();
	// read as compared: most of what is looked at fails on the first bytes
	int shift = (base - LD_ROM_BASE) & 0xffff;
	for (int i : ldcRel) {
		if ((i < from) || (i + 1 >= to)) continue;
		int w = tap_byte(comp, base + i) | (tap_byte(comp, base + i + 1) << 8);
		int v = ldcRom[i] | (ldcRom[i + 1] << 8);
		if (((w - v) & 0xffff) != shift) return 0;
	}
	for (int i = from; i < to; i++) {
		switch (ldcKind[i]) {
			case LDC_SKIP:
				break;
			case LDC_IX: {
				int x = tap_byte(comp, base + i);
				if ((x != 0x23) && (x != 0x2b)) return 0;
				break;
			}
			default:
				if (tap_byte(comp, base + i) != ldcRom[i]) return 0;
		}
	}
	if ((from <= LDC_INCIX) && (LDC_INCIX < to))
		*dir = (tap_byte(comp, base + LDC_INCIX) == 0x2b) ? -1 : 1;
	return 1;
}

void ldc_forget(Computer* comp) {
	comp->tape->ldBase = -1;
}

void ldc_frame() {
	ldcProbed = -1;
}

// A tape port read from ram: is it the IN of a copy's LD-EDGE-1? The reads
// come every few dozen T, so an address is looked at once a frame.
void ldc_find(Computer* comp) {
	Tape* tap = comp->tape;
	ldcReads = tap->portReads;
	int pc = comp->cpu->regPC;
	if (pc == ldcProbed) return;
	ldcProbed = pc;
	if (mem_get_page(comp->mem, pc)->type != MEM_RAM) return;
	int base = (pc - LDC_IN_NEXT) & 0xffff;
	if (base == tap->ldBase) return;
	// IN A,(#FE) / RRA, which every copy keeps, before the whole of it
	if ((tap_byte(comp, pc - 2) != 0xdb) || (tap_byte(comp, pc - 1) != 0xfe) || (tap_byte(comp, pc) != 0x1f)) return;
	int dir;
	if (!ldc_check(comp, base, &dir)) return;
	tap->ldBase = base;
	tap->ldDir = dir;
	tap->ldBlock = -1;
	xlog(XLG_TAPE, XLL_INFO, "a copy of LD-BYTES at %04X%s", base, (dir < 0) ? ", loading downwards" : "");
}

// The pc is at the copy's LD_START or LD-EDGE-1: is it still there? A loader
// may have put something else over it since. The whole of it is compared where
// a block would be handed over, the edge routine once for each block.
int ldc_check_hook(Computer* comp, int start) {
	Tape* tap = comp->tape;
	int ok;
	if (start) {
		ok = ldc_check(comp, tap->ldBase, &tap->ldDir);
	} else if (tap->ldBlock == tap->block) {
		return 1;
	} else {
		int dir;
		ok = ldc_check(comp, tap->ldBase, &dir, LDC_EDGE1, LDC_LEN);
	}
	if (!ok) {
		xlog(XLG_TAPE, XLL_INFO, "the copy of LD-BYTES at %04X is gone", tap->ldBase);
		ldc_forget(comp);
		return 0;
	}
	tap->ldBlock = tap->block;
	return 1;
}

#pragma once

#include "../libxpeccy/spectrum.h"
#include "../libxpeccy/ldbytes.h"
#include "../libxpeccy/cpu/Z80/z80.h"

// Copies of the rom's LD-BYTES in ram, which flash loading answers for as it
// does the rom's. The copy found is comp->tape->ldBase, -1 while there is none.

// a byte of memory as the cpu sees it
int tap_byte(Computer*, int adr);
// the same, for cpu_peek_word
int tap_peek(int adr, void* comp);

// no copy any more
void ldc_forget(Computer*);
// a new frame: the INs looked at may be looked at again
void ldc_frame();

// After every opcode while flash loading is on. Returns 1 when the pc is at the
// copy's LD_START (*start set) or LD-EDGE-1 and the copy is still there.
extern int ldcReads;
void ldc_find(Computer*);
int ldc_check_hook(Computer*, int start);
static inline int ldc_step(Computer* comp, int* start) {
	Tape* tap = comp->tape;
	if (tap->portReads != ldcReads)		// a read of the tape port: an IN to look at
		ldc_find(comp);
	if (tap->ldBase < 0) return 0;
	int ofs = (comp->cpu->regPC - tap->ldBase) & 0xffff;
	if ((ofs != LDC_START) && (ofs != LDC_EDGE1)) return 0;
	*start = (ofs == LDC_START);
	return ldc_check_hook(comp, *start);
}

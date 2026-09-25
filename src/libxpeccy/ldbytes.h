#pragma once

// Where things are in the rom's LD-BYTES, from its start - the same offsets in
// a copy of it in ram, which the tape trap answers for as it does the rom's
#define LD_ROM_BASE	0x0556
#define LDC_LEN		0xa4	// up to the stripes after an edge, #05FA
#define LDC_RET_OP	0x09	// the SA/LD-RET it pushes, #055F
#define LDC_START	0x16	// LD_START, #056C
#define LDC_INCIX	0x6d	// the second byte of INC IX, #05C3
#define LDC_BITS_RET	0x77	// after LD-8-BITS' call, #05CD
#define LDC_TAIL	0x89	// LD A,H / CP 1 / RET, #05DF
#define LDC_EDGE2	0x8d	// LD-EDGE-2, #05E3
#define LDC_EDGE2_RET	0x90	// after LD-EDGE-2's call, #05E6
#define LDC_EDGE1	0x91	// LD-EDGE-1, #05E7
#define LDC_IN_NEXT	0x9d	// after its IN A,(#FE), #05F3
#define LDC_EDGE_END	0xa3	// the last byte of the edge routines, #05F9

// pc is inside LD-EDGE-1/2 of the LD-BYTES at base
static inline int ld_edge(int pc, int base) {
	return ((unsigned)(((pc - base) & 0xffff) - LDC_EDGE2)) <= (LDC_EDGE_END - LDC_EDGE2);
}

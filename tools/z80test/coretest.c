/* coretest.c: run Xpeccy's Z80 core against fuse's timing tests
   Copyright (c) 2026 Oleksandr Kovalchuk

   Derived from coretest.c of Fuse - the Free Unix Spectrum Emulator,
   Copyright (c) 2003-2017 Philip Kendall, and covered by the same terms.
   The test data in tests/ is Fuse's as well.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see the LICENSE file beside it.

   NOTE: this is a development tool, not part of the emulator. Nothing here
   is linked into xpeccy-plus, which stays under its own MIT license.
*/

// The tests come in pairs: tests.in sets up registers and memory, tests.expected
// lists every bus cycle with the tstate it happened on, then the final state.
// This harness reads the first and prints the second, so a plain diff of our
// output against tests.expected names any instruction with the wrong timing.
//
// fuse's own coretest applies no contention delay, so the MC/PC lines say only
// *when* the ULA was given the chance to look at the bus - which is exactly what
// our IRQ_CPU_CONT / IRQ_CPU_CONTNM hooks report.
//
// Build and run: see run.sh in this folder.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/libxpeccy/cpu/Z80/z80.h"

// The Z80 core reaches for four things that live in cpu.c; pulling that file in
// would drag every other core along - cpuTab[] names the reset, exec and asm
// entry points of all six - so they are answered here instead. Two of them are
// no-ops on paths the tests never reach and cpu_irq is a one-line forwarder, but
// parity is a copy and has to stay identical to the one in cpu.c.

int parity(int val) {
	int p = 1;
	while (val) {
		p ^= val;
		val >>= 1;
	}
	return p & 1;
}

void cpu_irq(CPU* cpu, int id) {cpu->xirq(id, cpu->xptr);}
unsigned short cpu_peek_word(cbdmr mrd, void* data, int adr) {return 0;}
xAsmScan scanAsmTab(const char* src, opCode* tab) {xAsmScan r; memset(&r, 0, sizeof(r)); return r;}

// ----------------------------------------------------------------- state

static unsigned char memory[0x10000];
static unsigned char initial_memory[0x10000];

static CPU* cpu;
static long long base_t;		// tstates before the current instruction

static int now(void) {return (int)(base_t + cpu->t);}

// fuse prints a bus cycle at the tick it ends on. A read hands the byte over one
// tick earlier here (T3 of 3, T3 of 4 on M1) so the ray is where the ULA had it,
// and fuse spends a tick before IORQ drops - hence the +1s at the call sites.
static void ev(int t, const char* type, int adr, int val) {
	if (val < 0)
		printf("%5d %s %04x\n", t, type, adr);
	else
		printf("%5d %s %04x %02x\n", t, type, adr, val);
}

// ------------------------------------------------------------- callbacks

static int cb_mrd(int adr, int m1, void* ptr) {
	adr &= 0xffff;
	ev(now() + (m1 ? 2 : 1), "MR", adr, memory[adr]);
	return memory[adr];
}

static void cb_mwr(int adr, int val, void* ptr) {
	adr &= 0xffff;
	val &= 0xff;
	ev(now() + 1, "MW", adr, val);
	memory[adr] = val;
}

static int cb_ird(int adr, void* ptr) {
	adr &= 0xffff;
	int val = (adr >> 8) & 0xff;		// as fuse's harness does
	ev(now() + 1, "PR", adr, val);
	return val;
}

static void cb_iwr(int adr, int val, void* ptr) {
	ev(now() + 1, "PW", adr & 0xffff, val & 0xff);
}

static int cb_ack(void* ptr) {return 0xff;}

static void cb_irq(int id, void* ptr) {
	switch (id) {
		case IRQ_CPU_CONT:		// T1 of a bus cycle
		case IRQ_CPU_CONTNM:		// one internal tick, address still held
			ev(now(), "MC", cpu->adr & 0xffff, -1);
			break;
	}
}

// ------------------------------------------------------------------- i/o

static void put_state(int end_t) {
	printf("%04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x\n",
		z80_get_af(cpu) & 0xffff, cpu->regBC, cpu->regDE, cpu->regHL,
		cpu->regAFa, cpu->regBCa, cpu->regDEa, cpu->regHLa,
		cpu->regIX, cpu->regIY, cpu->regSP, cpu->regPC, cpu->regWZ);
	printf("%02x %02x %d %d %d %d %d\n",
		cpu->regI & 0xff, z80_get_r(cpu) & 0xff,
		cpu->flgIFF1 ? 1 : 0, cpu->flgIFF2 ? 1 : 0, cpu->regIM,
		cpu->flgHALT ? 1 : 0, end_t);
}

static void put_memory(void) {
	int i = 0;
	while (i < 0x10000) {
		if (memory[i] == initial_memory[i]) {
			i++;
			continue;
		}
		printf("%04x ", i);
		while ((i < 0x10000) && (memory[i] != initial_memory[i]))
			printf("%02x ", memory[i++]);
		printf("-1\n");
	}
}

// one test; 0 at eof
static int run_test(FILE* f) {
	unsigned af, bc, de, hl, afa, bca, dea, hla, ix, iy, sp, pc, wz;
	unsigned ri, rr, iff1, iff2, im;
	int halted, end_t;
	unsigned adr, byte;
	char name[80];
	int i;

	if (fscanf(f, "%79s", name) != 1) return 0;		// eof
	if (fscanf(f, "%x %x %x %x %x %x %x %x %x %x %x %x %x",
			&af, &bc, &de, &hl, &afa, &bca, &dea, &hla,
			&ix, &iy, &sp, &pc, &wz) != 13) {
		fprintf(stderr, "%s: corrupt registers line\n", name);
		return 0;
	}
	if (fscanf(f, "%x %x %u %u %u %d %d",
			&ri, &rr, &iff1, &iff2, &im, &halted, &end_t) != 7) {
		fprintf(stderr, "%s: corrupt state line\n", name);
		return 0;
	}

	for (i = 0; i < 0x10000; i += 4) {
		memory[i] = 0xde; memory[i + 1] = 0xad;
		memory[i + 2] = 0xbe; memory[i + 3] = 0xef;
	}
	// blocks of "<address> <byte>... -1", the last one closed by -1 for an address
	while ((fscanf(f, "%x", &adr) == 1) && (adr < 0x10000)) {
		while ((fscanf(f, "%x", &byte) == 1) && (byte < 0x100))
			memory[adr++ & 0xffff] = byte;
	}
	if (ferror(f)) {
		fprintf(stderr, "%s: corrupt memory setup\n", name);
		return 0;
	}
	memcpy(initial_memory, memory, 0x10000);

	z80_reset(cpu);
	z80_set_af(cpu, af);
	cpu->regBC = bc; cpu->regDE = de; cpu->regHL = hl;
	cpu->regAFa = afa; cpu->regBCa = bca; cpu->regDEa = dea; cpu->regHLa = hla;
	cpu->regIX = ix; cpu->regIY = iy; cpu->regSP = sp; cpu->regPC = pc;
	cpu->regWZ = wz;
	z80_set_ir(cpu, ((ri & 0xff) << 8) | (rr & 0xff));
	cpu->flgIFF1 = !!iff1; cpu->flgIFF2 = !!iff2;
	cpu->regIM = im;
	cpu->flgHALT = !!halted;

	printf("%s\n", name);		// events print as they happen, so the name goes first
	base_t = 0;
	while (base_t < end_t)
		base_t += z80_exec(cpu);

	put_state((int)base_t);
	put_memory();
	printf("\n");
	return 1;
}

int main(int argc, char** argv) {
	const char* path = (argc > 1) ? argv[1] : "tests/tests.in";
	FILE* f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "can't open %s\n", path);
		return 1;
	}
	cpu = (CPU*)calloc(1, sizeof(CPU));
	cpu->mrd = cb_mrd;
	cpu->mwr = cb_mwr;
	cpu->ird = cb_ird;
	cpu->iwr = cb_iwr;
	cpu->xack = cb_ack;
	cpu->xirq = cb_irq;
	// fuse's data has the contention events in it, so ask for them: the core
	// only reports a bus cycle when the machine says it contends one
	cpu->flgCONT = 1;
	while (run_test(f));
	fclose(f);
	free(cpu);
	return 0;
}

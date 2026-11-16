#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

#include "../defines.h"

typedef struct {
	unsigned cond:1;	// condition present
	unsigned met:1;		// condition met
	unsigned mem:1;		// operand mem rd :(nn),(hl),(de) etc
	int flag;
	int len;
	int oadr;		// direct addressation adr; also the ret/reti/retn target, peeked off the stack
	unsigned short mop;	// operand
	const char* mnem;
} xMnem;

// special register id
#define REG_EOT	0			// end of table
#define REG_EMPTY	-1		// don't show in debuga, but is a register
// register size
#define REG_BIT		1
#define REG_2		2		// special, values 0,1,2 (z80 interrupt mode)
#define REG_BYTE	8
#define REG_WORD	16
#define REG_24		24
#define REG_32		32
// register flags (bit 0-7)
#define REG_RO		1	// protect from changes in deBUGa
#define REG_RDMP	(1<<2)	// use register as line addr for regs-dump in deBUGa (new widget)
// what a register is for (xRegDsc.group). Not a flag bit: a register has one
#define REG_GRP_MAIN	1	// the working set (af, bc, a, x, ...)
#define REG_GRP_SHADOW	2	// their shadow copies
#define REG_GRP_PTR	3	// points into memory (pc, sp, ix, ...)
#define REG_GRP_CTRL	4	// interrupt and control state
// register type (bit 8-10 of flag). 0 is 'no type of its own', so a register
// that names none is not mistaken for the one the code is looking for
#define REG_TYPE_M	(7<<8)	// 8 types (to find register)
#define REG_PC		(3<<8)	// execution pointer (pc, ip)
#define REG_SP		(1<<8)	// stack (sp)
#define REG_FLG		(2<<8)	// is flag

typedef struct {
	int id;
	int size;
	int flag;
	const char* name;
	int value;	// register value
	int pair;	// id of register to show in the same line in deBUGa (0 = none)
	int group;	// what the register is for, REG_GRP_* (0 = table says nothing)
} xRegister;

typedef struct {
	char* flags;		// name of flags
	xRegister regs[32];	// registers
} xRegBunch;

typedef struct CPU CPU;

// 'pair' and 'group' are optional: tables that don't set them get 0. deBUGa
// spreads a register set over columns by group; a table that names no groups
// gets that layout folded out of the narrow one instead
typedef struct {
	int id;
	char* name;
	int size;
	int flag;
	int(*get)(CPU*);
	void(*set)(CPU*,int);
	int pair;
	int group;
} xRegDsc;

// memrq rd
typedef int(*cbmr)(int, int, void*);
// memrq wr
typedef void(*cbmw)(int, int, void*);
// iorq rd
typedef int(*cbir)(int, void*);
// iorq wr
typedef void(*cbiw)(int, int, void*);
// iorq int : interrupt vector request
typedef int(*cbiack)(void*);
// memrd external
typedef int(*cbdmr)(int, void*);

// FETCH is also CODE; DATA is a memory operand or a stack read.
enum {
	CPU_DASM_DATA = 0,
	CPU_DASM_CODE = 1,
	CPU_DASM_FETCH = 3
};

#define OF_PREFIX	1
#define OF_SKIPABLE	(1<<1)		// opcode is skipable by f8
#define OF_RELJUMP	(1<<2)
#define OF_MWORD	(1<<4)		// operand is word from memory
#define OF_MEMADR	(1<<5)		// operand contains memory address (nn)

typedef struct opCode opCode;

typedef void(*cbcpu)(CPU*);

struct opCode {
	int flag;
	int t;				// T-states
	cbcpu exec;			// fuction to exec
	opCode *tab;			// next opCode tab (for prefixes)
	const char* mnem;		// mnemonic
};

typedef struct {
	unsigned match:1;
	int idx;
	opCode* op;
	char* ptr;
	char arg[8][256];
} xAsmScan;

enum {
	CPU_NONE = 0,		// dummy
	CPU_Z80,
	CPU_Z80N		// the ZX Spectrum Next's: the Z80 with the Next's opcodes and its own undocumented flags
};

// A Z80-family core's opcode tables: the prefixes go from one to the next
// through opCode.tab, so a core with tables of its own (the Z80N) shares the
// exec loop with the plain Z80
typedef struct {
	opCode* np;
	opCode* cb;
	opCode* ed;
	opCode* dd;
	opCode* fd;
	opCode* ddcb;
	opCode* fdcb;
} opTabSet;

#define flgHALT	flags[62]		// cpu halted, undo on interrput
#define flgNOINT flags[60]		// Z80: don't handle INT after EI
#define flgWAIT	flags[59]		// ALL: WAIT signal (dummy 1T)
#define flgACK	flags[58]		// Z80: acknowledge INT after execution (prevent last-1T INT)
#define flgRetBRK flags[56]
#define flgRFSH	flags[55]		// Z80: report M1 T4 (IRQ_CPU_RFSH); the ULA snow effect needs it
#define flgCONT	flags[54]		// the machine contends the bus: report the start of each cycle
#define flgINTOK flags[53]		// Z80: the last exec took an INT; compExec clears it (the RZX recorder asks)
#define flgNMIOK flags[52]		// Z80: ...and an NMI, the same way
#define flgMSTART flags[51]		// Z80: the next internal tick starts an M-cycle (the Next's DMA may take the bus)

#define regCallCnt regs[63].ih

struct CPU {
	// common part
	int type;			// cpu type id
	unsigned short intrq;		// interrupts request. each bit for each INT type, 1 = requested
	unsigned short inten;		// interrupts enabled mask
	int adr;			// address bus for using from outside
	int busmask;			// mask for address bus
	int t;				// ticks counter
	// external callbacks
	cbmr mrd;			// memory reading
	cbmw mwr;			// memeory writing
	cbir ird;			// i/o reading
	cbiw iwr;			// i/o writing
	cbiack xack;			// interrupt vector acknowledge
	cbirq xirq;			// send signal
	void (*xcont)(void*, int);	// contend the bus cycle about to start (mreq)
	// where xcont has something to do: an address's 256-byte page set in contp
	// (NULL: everywhere; all zeros while a board's own arbiter has nothing to do), and
	// an internal cycle only if contInt
	unsigned char* contp;
	int contInt;
	// the refresh is reported (flgRFSH) where I's page is set in rfshp (NULL: everywhere)
	unsigned char* rfshp;
	// Memory straight, where the machine allows it for the instruction about to
	// run: the bytes of a 256-byte page by A15..A8, NULL for a page that has to
	// go through mrd/mwr - or the whole table NULL. m1p is for opcode fetches.
	unsigned char* const* rdp;
	unsigned char* const* m1p;
	unsigned char* const* wrp;
	// the two signals sent on every instruction, straight to their handlers:
	// xirq(IRQ_CPU_ACK) and xirq(IRQ_CPU_SYNC) unless the machine says otherwise
	cbcpu xend;			// the end of an instruction, where INT is sampled
	cbcpu xsync;			// a write is about to go out
	// an M1 cycle whose byte is thrown away - the fetch at PC a Z80 makes when it
	// takes an NMI: the address only, for a device that watches the fetches
	void (*xm1x)(void*, int);
	void* xptr;			// pointer to external data (almost always Computer*)
	// core: runtime callbacks (depends on type)
	struct cpuCore* core;
	xRegDsc* pcdsc;			// the core's PC, looked up once: the core asks for it on every read
	// opcode
	reg16(com, hcom, lcom);
	opCode* opTab;
	opCode* op;
	const opTabSet* tabs;		// the core's tables (Z80 family), set by its init/reset
	// Z80N: a NEXTREG instruction's register write (a side-band request, no I/O cycle)
	void (*xnextreg)(void*, int, int);
	// Z80: a maskable interrupt is taken, its acknowledge cycle about to run (PC is the
	// return address); NULL for a machine that has nothing to do there
	void (*xinta)(void*);
	// common registers block
	xreg32 regs[64];
	// TODO: common flags (unnamed, must be defined same way as registers). replace cpuFlags with it
	bool flags[64];
	// temp
	unsigned char tmp;
	unsigned char tmpb;
	reg16(tmpw,htw,ltw);
	int tmpi;
};

struct cpuCore {
	int type;				// cpu type
	const char* name;			// printable name
	xRegDsc* rdsctab;			// registers descriptors table
	int adrbus;				// width of address bus (bits)
	void (*init)(CPU*);			// call it when core changed
	void (*reset)(CPU*);			// reset
	int (*exec)(CPU*);			// exec opcode, return T
	xAsmScan (*asmbl)(int,const char*, char*);	// compile mnemonic (adr,src.text,result.buf)
	xMnem (*mnem)(CPU*, int, cbmr, void*);
};
typedef struct cpuCore cpuCore;

CPU* cpuCreate(int,cbmr,cbmw,cbir,cbiw,cbiack,cbirq,void*);
void cpuDestroy(CPU*);
int cpu_set_type(CPU*, const char*);

void cpu_reset(CPU*);
// one opcode or interrupt, T back; called on every instruction
static inline int cpu_exec(CPU* cpu) {
	if (!cpu->core) return 1;
	if (!cpu->core->exec) return 1;
	return cpu->core->exec(cpu);
}

// built-in cores tab
extern cpuCore cpuTab[];

// how a disassembly writes numbers: #FF, $FF, FFh, 255
enum {
	CPU_NUM_HASH = 0,
	CPU_NUM_DOLLAR,
	CPU_NUM_H,
	CPU_NUM_DEC
};

// the plain entries write #FF, as logs and traces have it
xMnem cpuDisasm(CPU*, int, char*, cbdmr, void*);
xMnem cpuDisasmView(CPU*, int, char*, cbmr, void*);
xMnem cpuDisasmFmt(CPU*, int, char*, cbdmr, void*, int fmt);
xMnem cpuDisasmViewFmt(CPU*, int, char*, cbmr, void*, int fmt);
int cpuAsm(CPU*, const char*, char*, unsigned short);
xAsmScan scanAsmTab(const char*, opCode*);
unsigned short cpu_peek_word(cbdmr, void*, int);	// little-endian word at adr, wrapping at 0xffff
int cpu_int_ready(CPU*);		// the next exec takes an interrupt, not an instruction

xRegBunch cpuGetRegs(CPU*);
void cpuSetRegs(CPU*, xRegBunch);
xRegDsc* find_reg_type(CPU*, int);
xRegDsc* find_reg_name(CPU*, const char*);
int cpu_get_regtype(CPU*, int);
int cpu_get_reg(CPU*, const char*, bool*);
int cpu_get_pc(CPU*);
void cpu_set_pc(CPU*, int);
int cpu_get_sp(CPU*);
int cpu_set_reg(CPU*, const char*, int);
int cpu_get_flag(CPU*);
void cpu_set_flag(CPU*, int);
int reg_get_value(CPU*, xRegDsc*);
int reg_set_value(CPU*, xRegDsc*, int);

// 1 when val holds an even number of set bits (the P flag's sense).
// Callers pass masked, non-negative values only.
static inline int parity(int val) {
	return __builtin_parity((unsigned int)val) ^ 1;
}

// never NULL: cpuCreate puts a stub in for a cpu with no machine behind it
static inline void cpu_irq(CPU* cpu, int id) {
	cpu->xirq(id, cpu->xptr);
}

#ifdef __cplusplus
}
#endif

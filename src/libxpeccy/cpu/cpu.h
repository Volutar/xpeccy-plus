#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

#include <setjmp.h>		// to implement try-catch
//#define THROW(__N) longjmp(cpu->jbuf, __N)
//#define THROW_EC(__N,__C) cpu->errcod = __C; longjmp(cpu->jbuf, __N)

#include "../defines.h"

typedef unsigned char xbyte;
typedef unsigned short xword;

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

#define OF_PREFIX	1
#define OF_SKIPABLE	(1<<1)		// opcode is skipable by f8
#define OF_RELJUMP	(1<<2)
#define OF_MBYTE	(1<<3)		// operand is byte from memory
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
	CPU_Z80
};

#define flgTMP flags[63]
#define flgHALT	flags[62]		// cpu halted, undo on interrput
#define flgEXC	flags[61]		// exception occured
#define flgNOINT flags[60]		// Z80: don't handle INT after EI
#define flgWAIT	flags[59]		// ALL: WAIT signal (dummy 1T)
#define flgACK	flags[58]		// Z80: acknowledge INT after execution (prevent last-1T INT)
#define flgRetBRK flags[56]
#define flgRFSH	flags[55]		// Z80: report M1 T4 (IRQ_CPU_RFSH); the ULA snow effect needs it

#define regCallCnt regs[63].ih
#define regExcCode regs[63].l		// exception code if flgEXC

struct CPU {
	// common part
	int type;			// cpu type id
	unsigned short intrq;		// interrupts request. each bit for each INT type, 1 = requested
	unsigned short inten;		// interrupts enabled mask
	int intvec;			// interrupt vector (internal/external)
	int errcod;			// error code (-1 if not present)
	int adr;			// address bus for using from outside
	int busmask;			// mask for address bus
	int t;				// ticks counter
	unsigned short oldpc;		// address of current instruction
	// if cpu is from external lib
	unsigned lib:1;			// cpu core from exernal lib
	char* libname;			// name of lib inside libs folder
	void* libhnd;			// lib handler (dlopen)
	// external callbacks
	cbmr mrd;			// memory reading
	cbmw mwr;			// memeory writing
	cbir ird;			// i/o reading
	cbiw iwr;			// i/o writing
	cbiack xack;			// interrupt vector acknowledge
	cbirq xirq;			// send signal
	void* xptr;			// pointer to external data (almost always Computer*)
	// core: runtime callbacks (depends on type)
	struct cpuCore* core;
	xRegDsc* pcdsc;			// the core's PC, looked up once: the core asks for it on every read
	// opcode
	reg16(com, hcom, lcom);
	opCode* opTab;
	opCode* op;
	// common registers block
	xreg32 regs[64];
	// TODO: common flags (unnamed, must be defined same way as registers). replace cpuFlags with it
	bool flags[64];
	// temp
	unsigned char tmp;
	unsigned char tmpb;
	reg16(tmpw,htw,ltw);
	reg16(twrd,hwr,lwr);
	int tmpi;
//	jmp_buf jbuf;			// for throws
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
	xMnem (*mnem)(CPU*, int, cbdmr, void*);
};
typedef struct cpuCore cpuCore;

CPU* cpuCreate(int,cbmr,cbmw,cbir,cbiw,cbiack,cbirq,void*);
void cpuDestroy(CPU*);
int cpu_set_type(CPU*, const char*, const char*, const char*);

void cpu_reset(CPU*);
int cpu_exec(CPU*);

// built-in cores tab
extern cpuCore cpuTab[];

xMnem cpuDisasm(CPU*, int, char*, cbdmr, void*);
int cpuAsm(CPU*, const char*, char*, unsigned short);
xAsmScan scanAsmTab(const char*, opCode*);
unsigned short cpu_peek_word(cbdmr, void*, int);	// little-endian word at adr, wrapping at 0xffff

xRegBunch cpuGetRegs(CPU*);
xRegister cpuGetReg(CPU*, int);
void cpuSetRegs(CPU*, xRegBunch);
xRegDsc* find_reg_type(CPU*, int);
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

int parity(int);

int cpu_fetch(CPU*, int);
int cpu_mrd(CPU*, int);
void cpu_mwr(CPU*, int, int);
int cpu_ird(CPU*, int);
void cpu_iwr(CPU*, int, int);
void cpu_irq(CPU*, int);

#ifdef __cplusplus
}
#endif

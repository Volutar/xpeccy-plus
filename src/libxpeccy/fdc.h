#pragma once

#include <stdlib.h>
#include "floppy.h"

#define FDC_FAST	1

enum {
	DIF_NONE = 0,
	DIF_BDI,
	DIF_P3DOS,
	DIF_END	= -1
};

#define	BDI_SYS		0xff
#define FDC_COM		0x1f
#define FDC_TRK		0x3f
#define FDC_SEC		0x5f
#define FDC_DATA	0x7f

// uPD765 command bits, and its status bits that a DSK image stores too
#define F_COM_MT	0x80
#define F_COM_MF	0x40
#define F_COM_SK	0x20

#define F_SR0_AT	0x40	// abnormal termination
#define F_SR0_NR	0x08	// not ready
#define F_SR1_EN	0x80	// end of cylinder
#define F_SR1_DE	0x20	// CRC error
#define F_SR1_OR	0x10	// data lost
#define F_SR1_ND	0x04	// no data
#define F_SR1_NW	0x02	// write protect
#define F_SR1_MA	0x01	// missing address mark
#define F_SR2_CM	0x40	// the other data mark than the command reads
#define F_SR2_DD	0x20	// CRC error in the data field
#define F_SR2_WC	0x10	// wrong cylinder
#define F_SR2_BC	0x02	// bad cylinder (C = FF)
#define F_SR2_MD	0x01	// no data mark

// fast disk access, unless the command in progress is to keep real time
#define fdc_turbo(f) ((fdcFlag & FDC_FAST) && !(f)->slow)

// difOut's last argument on the +3: the command comes from a program in RAM, not
// from +3DOS in ROM - a loader of its own, which may count on the drive's timing
#define DIF_FROM_RAM	2

typedef struct {
	unsigned char trk;
	unsigned char head;
	unsigned char sec;
	unsigned char sz;	// 0..3 = 128..1024
	unsigned char type;
	unsigned char flags;	// SEC_* below: what of the sector is on the disk
	int crc;
	unsigned char data[0x1800];
} Sector;

// The bytes after an ID's CRC the WD1793 looks for the data mark in, else
// Record Not Found. The FD179X datasheet says 43 in MFM; the TRD and SCL
// layout (flp_format_sec) puts the mark at 44
#define WD_DAM_WINDOW	52

// Sector.flags: what an image says a sector lacks or has wrong
#define SEC_NO_DATA	1	// an ID field with no data field after it
#define SEC_NO_ID	2	// a data field with no ID field before it
#define SEC_BAD_CRC	4	// the data field's CRC does not match

typedef struct FDC FDC;
typedef void(*fdcCall)(FDC*);

struct FDC {
	const int id;
	unsigned brk:1;		// signal for debug
	unsigned seekend:1;	// uPD765: set at end of seek/recalibrate com
	unsigned irq:1;		// WD1793:irq ; uPD765:exec
	unsigned drq:1;		// 1:data request
	unsigned dir:1;		// drq dir: 0 - cpu->fdc; 1 - fdc->cpu
	unsigned mr:1;		// master reset
	unsigned block:1;
	unsigned side:1;
	unsigned step:1;	// step dir
	unsigned mfm:1;
	unsigned idle:1;
	unsigned crchi:1;
	unsigned hd:1;		// HD mode (trklen 12500, bytedelay 16000)
	unsigned debug:1;
	unsigned slow:1;	// uPD765: this command keeps real time whatever fast access says
	unsigned ram:1;		// uPD765: the byte being written comes from a program in RAM

	unsigned char trk;	// registers
	unsigned char sec;
	unsigned char data;
	unsigned char com;
	unsigned char state;	// status register
	unsigned char tmp;
	unsigned short wdata;
	unsigned short tdata;
	int bytedelay;

	Floppy* flop[4];	// all floppies belongs to disk interface, here is pointers
	Floppy* flp;		// current floppy ptr

	unsigned short crc;	// calculated crc
	unsigned short fcrc;	// crc get from floppy
	unsigned char buf[6];
	int fmode;
	int cnt;
	int wait;		// pause (ns)
	int tns;
	int hold;		// wd1793 turbo: ns left in this command to wait for the cpu to take a byte
	int drdy;		// time (ns) to replace fdc->insert signal by 0 (time between opening and closing a flp gate)

	fdcCall* plan;		// current task
	int pos;		// pos in plan

	int irqn;		// upd765 irq_fdc_int nr
	int irqwn;		// upd765 irq_fdc_wr
	int irqrn;		// upd765 irq_fdc_rd

	cbirq xirq;
	void* xptr;

	unsigned dma:1;		// not implemented yet
	unsigned intr:1;	// uPD765 interrupt pending. reset @ com08
	int hlt;		// head load time (all ns)
	int hut;		// head unload time
	int srt;		// step rate time
	unsigned char comBuf[8]; // uPD765 command args
	int comCnt;		// arg count for command
	int comPos;		// pos in comBuf
	unsigned char resBuf[8]; // uPD765 response
	int resCnt;
	int resPos;
	int xfer;		// uPD765: bytes of the field still to hand to the CPU
	unsigned char sr0,sr1,sr2,sr3;	// status registers
	int scnt;		// uPD765: sectors of a Read Track or a Format so far

	Sector slst[64];
};

void fdc_set_hd(FDC*, int);

typedef struct DiskIF DiskIF;

typedef struct {
	const int id;
	void(*reset)(struct DiskIF*);
	int(*in)(struct DiskIF*,int,int*,int);
	int(*out)(struct DiskIF*,int,int,int);
	void(*sync)(struct DiskIF*,int);
} DiskHW;

struct DiskIF {
	unsigned doors:1;	// a drive's door is still closing: difSync has to run
	unsigned out:1;		// taken out of the machine for now (dif_set_out)
	int type;		// the interface the machine has now: none while out
	int cfg;		// the one it is set to have, out or not
	int sys;		// the last write to the interface's own port (Beta: #FF)
	Floppy* flp[4];
	DiskHW* hw;
	FDC* fdc;
};

extern int fdcFlag;

DiskIF* difCreate(int, cbirq, void*);
void difDestroy(DiskIF*);

void difReset(DiskIF*);
void difSync(DiskIF*, int);
int difOut(DiskIF*, int, int, int);
int difIn(DiskIF*, int, int*, int);

void difSetHW(DiskIF*, int);
// out of the machine and back, as a board that takes its place wants: the
// setting, the drives and their disks stay
void dif_set_out(DiskIF*, int);
void difSetDrives(DiskIF*, int);	// how many drives are on the cable, from A
// the plan in progress as a number a snapshot can keep, -1: one not known
int dif_plan_id(DiskIF*);
void dif_plan_set(DiskIF*, int);
// The uPD765 plans' layout a snapshot keeps with the step: 1 since Read Data and
// Read Track check the data CRC in a step of their own. upd_plan_v0() moves a step
// of layout 0 (2026.9) to where it is now.
#define UPD_PLANS	1
void upd_plan_v0(FDC*);
// The WD1793's, 1 since RESTORE waits its start before #FF goes into the track register
#define VG_PLANS	1
void vg_plan_v0(FDC*);
// A Format in progress as a snapshot keeps it: the IDs it has taken (0 for any other
// command), and one of them put back as the chip holds it
int upd_format_ids(FDC*);
void upd_form_id(FDC*, int i, const unsigned char* chrn);
int dif_quiet(DiskIF*);		// its time may come in any pieces
void dif_align_flps(DiskIF*, FDC*, int, int, int, int);

void add_crc_16(FDC*, unsigned char);
int seekADR(FDC*);	// wd1793.c: wait for an ID mark and read the ID into buf; 2: the index came
void rdFCRC(FDC*);	// wd1793.c: the two CRC bytes off the track into fcrc

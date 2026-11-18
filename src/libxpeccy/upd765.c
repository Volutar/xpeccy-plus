#include <stdio.h>
#include "xlog.h"
#include <string.h>

#include "fdc.h"

#ifdef ISDEBUG
#define DBGOUT(args...) xlog(XLG_DISK, XLL_DEBUG, args)
#else
#define DBGOUT(args...)
#endif

#define UPD_TURBO_BYTE 1000
#define UPD_TURBO_STEP 1

// wait until all args is done
void uwargs(FDC* fdc) {
	if (fdc->comCnt == 0) {
		fdc->irq = 1;		// enter exec phase (idle=0, irq=1)
		fdc->pos++;
	}
	fdc->wait = 1;
}

// do dothing, just spin floppy if motor is on
void unothing(FDC* fdc) {
	if (fdc->flp->motor) {
		flpNext(fdc->flp, fdc->side);
	}
	fdc->wait += fdc->bytedelay; // turbo ? UPD_TURBO_BYTE : fdc->bytedelay;
}

void uidle(FDC* fdc) {
	fdc->idle = 1;
	fdc->drq = 1;
	fdc->dir = 0;
}

void ustp(FDC* fdc) {
	fdc->irq = 0;		// enter result phase
	fdc->pos++;
	if (fdc->resCnt > 0) {
		fdc->drq = 1;
		fdc->dir = 1;
	} else {
		uidle(fdc);
	}
}

void uInt(FDC* fdc) {
	if (!fdc->intr) {
		fdc->xirq(fdc->irqn, fdc->xptr);
		fdc->intr = 1;
	}
}

void uResp(FDC* fdc, int len, int ie) {
	// EXM drops with the result posted, or a loop polling the MSR reads ST0 as data
	fdc->irq = 0;
	fdc->resPos = 0;
	fdc->resCnt = len;
	fdc->dir = 1;
	fdc->drq = !!len;	// 1 if answer size > 0
	if (ie) {uInt(fdc);}
}

void uSetDrive(FDC* fdc) {
	fdc->flp = fdc->flop[fdc->comBuf[0] & 3];
	fdc->trk = fdc->flp->trk;
	fdc->side = (fdc->comBuf[0] & 4) ? 1 : 0;
	fdc->sr0 &= 0xf8;
	fdc->sr0 |= fdc->comBuf[0] & 7;	// hd & drive
}

// The CRC bytes come off the disk while the CPU takes the field's last byte.
// Turbo has no such time of its own: it waits for the CPU as it does for any byte.
static int u_last_taken(FDC* fdc) {
	if (fdc_turbo(fdc) && fdc->drq && !fdc->dma && (fdc->tns <= fdc->bytedelay)) {
		fdc->wait = 1;
		return 0;
	}
	return 1;
}

// The next byte off the track into tmp and the CRC. 0: turbo still waits for
// the CPU to take the last one. A byte the CPU did not take in time sets OR.
int uGetByte(FDC* fdc) {
	if (!fdc->mr && !u_last_taken(fdc)) return 0;
	if (fdc->drq && !fdc->dma && !fdc->mr)
		fdc->sr1 |= F_SR1_OR;
	fdc->tmp = flpRd(fdc->flp, fdc->side);
	flpNext(fdc->flp, fdc->side);
	add_crc_16(fdc, fdc->tmp);
	fdc->tns = 0;
	if (fdc_turbo(fdc)) {
		fdc->wait = 1;
	} else {
		fdc->wait += fdc->bytedelay;
	}
	return 1;
}

static fdcCall utermTab[] = {&ustp, &unothing};

// Parked after a command: unothing alone, for good. It reads nothing an
// instruction changes, so its time may come in any pieces (dif_quiet)
int uParked(FDC* fdc) {
	return (fdc->plan == utermTab) && (fdc->pos == 1);
}

void uTerm(FDC* fdc) {
	fdc->plan = utermTab;
	fdc->pos = 0;
	fdc->wait = 1;
	fdc->mr = 0;
}

// 00000011 [SRT:4.HUT:4, HLT:7.ND:1]
// specify

void uspcf00(FDC* fdc) {
	fdc->srt = (((fdc->comBuf[0] >> 4) & 0x0f) + 1) * 1000;
	fdc->hut = (((fdc->comBuf[0] & 0x0f) + 1) << 4) * 1000;
	fdc->hlt = ((fdc->comBuf[1] & 0xfe) + 2) * 1000;
	fdc->dma = (fdc->comBuf[1] & 1) ? 0 : 1;
	fdc->resCnt = 0;			// no answer
	fdc->pos++;
}

static fdcCall uSpecify[] = {&uwargs, &uspcf00, &uTerm};

typedef struct {
	int mask;
	int val;
	int argCount;
	fdcCall* plan;
} uCom;

// 00000100 [xxxxx.HD.DRV:2] R [ST3]
// drive status

void udrvst00(FDC* fdc) {
	uSetDrive(fdc);
	fdc->sr3 = (fdc->flp->protect ? 0x40 : 0x00) |
		((fdc->flp->insert && fdc->flp->door) ? 0x20 : 0x00) |
		(flp_trk0(fdc->flp) ? 0x10 : 0x00) |
		(fdc->flp->doubleSide ? 0x08 : 0x00) |
		(fdc->comBuf[0] & 7);
	fdc->resBuf[0] = fdc->sr3;
	uResp(fdc, 1, 1);
	fdc->pos++;
}

static fdcCall uDrvStat[] = {&uwargs, &udrvst00, &uTerm};

// 00000111 [xxxxx0.DRV:2]
// recalibrate
// FIXME: too fast when flp is already at track 0

void ucalib00(FDC* fdc) {
	int drv = fdc->comBuf[0] & 3;
	if (fdc->side) fdc->comBuf[0] |= 4;
	uSetDrive(fdc);
	fdc->cnt = 77;			// do 77 step back
	fdc->wait += fdc_turbo(fdc) ? UPD_TURBO_STEP : fdc->srt;
	fdc->state |= (1 << drv);	// set "flp is in seek mode"
	fdc->pos++;
}

void ucalib01(FDC* fdc) {
	if (flp_trk0(fdc->flp)) {
		fdc->sr0 &= 0x0f;
		fdc->sr0 |= 0x20;	// SR0:0010xxxx
		fdc->state &= 0xf0;
		fdc->wait = fdc->bytedelay * 3;		// wait until ending interrupt
		fdc->pos++;
	} else if (fdc->cnt > 0) {
		flpStep(fdc->flp, FLP_BACK);
		fdc->wait += fdc_turbo(fdc) ? UPD_TURBO_STEP : fdc->srt;
		fdc->cnt--;
	} else {
		fdc->sr0 &= 0x0f;
		fdc->sr0 |= 0x70;	// SR0:0111xxxxx
		fdc->state &= 0xf0;
		fdc->wait = fdc->bytedelay * 3;
		fdc->pos++;
	}
}

void ucalib02(FDC* fdc) {
	uInt(fdc);
	fdc->seekend = 1;
	fdc->pos++;
}

static fdcCall uCalib[] = {&uwargs,&ucalib00,&ucalib01,&ucalib02,&uTerm};

// 00001111 [xxxxx.H.DRV:2,NCN]
// seek

void useek00(FDC* fdc) {
	uSetDrive(fdc);
	// fdc->trk = fdc->flp->trk;		// wait, oh shi...
	fdc->wait += fdc_turbo(fdc) ? UPD_TURBO_STEP : fdc->srt;
	fdc->pos++;
	fdc->state |= (1 << fdc->comBuf[0] & 3);	// set "flp is in seek mode"
}

void useek01(FDC* fdc) {
	if (fdc->trk == fdc->comBuf[1]) {
		fdc->sr0 &= 0x0f;
		fdc->sr0 |= 0x20;
		fdc->pos++;
	} else if (fdc->trk < fdc->comBuf[1]) {
		flpStep(fdc->flp, FLP_FORWARD);
		fdc->wait += fdc_turbo(fdc) ? UPD_TURBO_STEP : fdc->srt;
		fdc->trk++;
	} else {
		flpStep(fdc->flp, FLP_BACK);
		fdc->wait += fdc_turbo(fdc) ? UPD_TURBO_STEP : fdc->srt;
		fdc->trk--;
	}
}

void useek02(FDC* fdc) {
	fdc->state &= 0xf0;		// seek mode off
	fdc->sr0 &= 0x1f;		// b7,6=00:success
	fdc->sr0 |= 0x20;		// seek end
	if (fdc->trk != fdc->flp->trk) {
		fdc->sr0 |= F_SR0_AT;	// b7,6=01:abnornal termination
	}
	fdc->wait = fdc->bytedelay * 2;
	fdc->pos++;
}

static fdcCall uSeek[] = {&uwargs,&useek00,&useek01,&useek02,&ucalib02,&uTerm};

// 0.MF.001010 [xxxxx.H.DRV:2] R [ST0,ST1,ST2,C,H,R,N]
// read ID

void urdaRS(FDC* fdc) {
	fdc->resBuf[0] = fdc->sr0;
	fdc->resBuf[1] = fdc->sr1;
	fdc->resBuf[2] = fdc->sr2;
	fdc->resBuf[3] = fdc->buf[0];	// C
	fdc->resBuf[4] = fdc->buf[1];	// H
	fdc->resBuf[5] = fdc->buf[2];	// R
	fdc->resBuf[6] = fdc->buf[3];	// N
	uResp(fdc, 7, 1);
	fdc->pos++;
}

void urda00(FDC* fdc) {
	uSetDrive(fdc);
	if (!(fdc->flp->insert && fdc->flp->door)) {
		fdc->sr0 |= F_SR0_AT | F_SR0_NR;
		urdaRS(fdc);
		uTerm(fdc);
	} else {
		fdc->cnt = 2;
		fdc->pos++;
	}
}

void urda01(FDC* fdc) {
	int res = seekADR(fdc);
	if (res == 2) {
		fdc->cnt--;
		if (fdc->cnt == 0) {		// no ID in two index holes
			fdc->sr1 |= F_SR1_MA | F_SR1_ND;
			fdc->sr0 |= F_SR0_AT;
			fdc->pos++;
		}
	} else if (res == 1) {
		if (fdc->crc != fdc->fcrc) {	// the first ID there is, its CRC wrong
			fdc->sr1 |= F_SR1_DE | F_SR1_ND;
			fdc->sr0 |= F_SR0_AT;
		}
		fdc->pos++;
	}
}

static fdcCall uReadID[] = {&uwargs,&urda00,&urda01,&urdaRS,&uTerm};

// MT.MF.SK.00110 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,DTL] R [SR0,SR1,SR2,C,H,R,N]
// MT.MF.SK.01000 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,DTL] R [SR0,SR1,SR2,C,H,R,N]
// read [deleted] data

// The ID registers as the command left them: C, H and N as given (H flipped by
// MT), R the sector it stopped at
void ureadRS(FDC* fdc) {
	fdc->resBuf[0] = fdc->sr0;
	fdc->resBuf[1] = fdc->sr1;
	fdc->resBuf[2] = fdc->sr2;
	fdc->resBuf[3] = fdc->comBuf[1];
	fdc->resBuf[4] = fdc->comBuf[2];
	fdc->resBuf[5] = fdc->sec;
	fdc->resBuf[6] = fdc->comBuf[4];
	uResp(fdc, 7, 1);
	fdc->pos++;
}

// the ID just read is the one the command names, C, H, R and N alike
static int u_match(FDC* fdc) {
	return (fdc->buf[0] == fdc->comBuf[1]) && (fdc->buf[1] == fdc->comBuf[2])
		&& (fdc->buf[2] == fdc->sec) && (fdc->buf[3] == fdc->comBuf[4]);
}

// An ID turned up: 1 if it is the one named. Another cylinder's sets WC, and BC for FF.
static int u_id_named(FDC* fdc) {
	if (u_match(fdc)) {
		fdc->sr2 &= ~(F_SR2_WC | F_SR2_BC);
		return 1;
	}
	if (fdc->buf[0] != fdc->comBuf[1])
		fdc->sr2 |= (fdc->buf[0] == 0xff) ? (F_SR2_WC | F_SR2_BC) : F_SR2_WC;
	return 0;
}

// The sector is done: the next one, side 1's first at EOT with MT, else 0 and the
// command ends with EN (the +3 has no TC)
static int u_next_sec(FDC* fdc) {
	if (fdc->sec != fdc->comBuf[5]) {
		fdc->sec++;
		return 1;
	}
	if ((fdc->com & F_COM_MT) && !(fdc->comBuf[2] & 1)) {
		fdc->comBuf[2] |= 1;
		fdc->side = 1;
		fdc->sr0 |= 0x04;
		fdc->sec = 1;
		return 1;
	}
	fdc->sr0 |= F_SR0_AT;
	fdc->sr1 |= F_SR1_EN;
	return 0;
}

// wait ADR
void uread01(FDC* fdc) {
	int res = seekADR(fdc);
	if (res == 2) {
		fdc->cnt--;
		if (fdc->cnt < 1) {		// two index holes: the sector is not there
			fdc->sr0 |= F_SR0_AT;
			ureadRS(fdc);
			uTerm(fdc);
		}
	} else if (res == 1) {
		fdc->sr1 &= ~F_SR1_MA;		// the track has ID marks
		if (u_id_named(fdc)) {
			fdc->sr1 &= ~F_SR1_ND;
			if (fdc->crc != fdc->fcrc) {	// the ID's own CRC is wrong
				fdc->sr0 |= F_SR0_AT;
				fdc->sr1 |= F_SR1_DE;
				ureadRS(fdc);
				uTerm(fdc);
			} else {
				fdc->cnt = 52;
				fdc->pos++;
			}
		}
	}
}

// After an ID: its data mark within the gap, either kind. 0: still looking,
// 1: found, tmp holds the mark and the head is on the first data byte, the
// CRC counted that far; 2: none (MA + MD).
static int u_find_dam(FDC* fdc) {
	fdc->tmp = flpRd(fdc->flp, fdc->side);
	flpNext(fdc->flp, fdc->side);
	fdc->wait += fdc_turbo(fdc) ? UPD_TURBO_BYTE : fdc->bytedelay;
	if ((fdc->flp->field == 2) || (fdc->flp->field == 3)) {
		fdc->crc = 0xcdb4;		// A1 A1 A1 counted
		add_crc_16(fdc, fdc->tmp);
		return 1;
	}
	if (--fdc->cnt > 0) return 0;
	fdc->sr0 |= F_SR0_AT;
	fdc->sr1 |= F_SR1_MA;
	fdc->sr2 |= F_SR2_MD;
	return 2;
}

int ureadCHK(FDC* fdc, int rt, int wt) {
	fdc->tmp = flpRd(fdc->flp, fdc->side);
	flpNext(fdc->flp, fdc->side);
	fdc->wait += fdc_turbo(fdc) ? UPD_TURBO_BYTE : fdc->bytedelay;
	if (fdc->flp->field == rt) return 1;
	if ((fdc->flp->field == wt) && (~fdc->com & F_COM_SK)) {	// wrong data field type, skip off: set b4,sr2, read sector & terminate execution
		fdc->sr2 |= F_SR2_CM;
		return 1;
	}
	fdc->cnt--;
	if (fdc->cnt > 0) return 0;	// seek in progress
	fdc->sr0 |= F_SR0_AT;		// error
	fdc->sr1 |= F_SR1_ND;		// no data
	return 2;
}

// the data mark: the other kind than the command reads sets CM, and with SK
// the sector is passed over unread
void uread02(FDC* fdc) {
	int res = u_find_dam(fdc);
	if (res == 2) {
		ureadRS(fdc);
		uTerm(fdc);
	} else if (res == 1) {
		int deleted = (fdc->tmp & 0xfe) == 0xf8;
		if (deleted != ((fdc->com & 0x1f) == 0x0c)) {
			fdc->sr2 |= F_SR2_CM;
			if (fdc->com & F_COM_SK) {
				fdc->pos = 5;		// uread05: on to the next sector
				return;
			}
		}
		fdc->pos++;
		fdc->wait = 1;
	}
}

// The field is read whole, 128 << N bytes off the track whatever its own
// length, then its CRC; N 0 hands over DTL bytes of the 128
void uread03(FDC* fdc) {
	int n = fdc->comBuf[4];
	fdc->cnt = flp_seclen(n);
	fdc->xfer = (n || (fdc->comBuf[7] > 0x80)) ? fdc->cnt : fdc->comBuf[7];
	fdc->drq = 0;
	fdc->dir = 1;
	fdc->tns = 0;
	fdc->wait = fdc->bytedelay;
	fdc->pos++;
}

void uread04(FDC* fdc) {
	if (!uGetByte(fdc)) return;
	if (fdc->sr1 & F_SR1_OR) {		// the CPU let a byte go by: the command ends
		fdc->sr0 |= F_SR0_AT;
		ureadRS(fdc);
		uTerm(fdc);
		return;
	}
	if (!fdc->mr && (fdc->xfer > 0)) {
		fdc->xfer--;
		fdc->data = fdc->tmp;
		fdc->drq = 1;
		if (fdc->dma) {
			fdc->xirq(fdc->irqrn, fdc->xptr);
		} else {
			uInt(fdc);
		}
	}
	fdc->cnt--;
	if (fdc->cnt < 1)
		fdc->pos++;
}

// the data field's CRC: wrong -> DE + DD; ends a read, not a read track
static int u_data_crc(FDC* fdc) {
	unsigned short crc = fdc->crc;
	rdFCRC(fdc);
	fdc->wait += fdc_turbo(fdc) ? 1 : (2 * fdc->bytedelay);
	if (crc == fdc->fcrc) return 1;
	fdc->sr1 |= F_SR1_DE;
	fdc->sr2 |= F_SR2_DD;
	return 0;
}

void uread04c(FDC* fdc) {
	if (!u_last_taken(fdc)) return;
	if (u_data_crc(fdc)) {
		fdc->pos++;
	} else {
		fdc->sr0 |= F_SR0_AT;
		ureadRS(fdc);
		uTerm(fdc);
	}
}

// The sector is done: the next one, or the end. With no TC (the +3 has none)
// a read stops at EOT with EN, or after a sector of the other mark (SK 0).
void uread05(FDC* fdc) {
	if (fdc->mr || ((fdc->sr2 & F_SR2_CM) && !(fdc->com & F_COM_SK)) || !u_next_sec(fdc)) {
		fdc->pos++;
		return;
	}
	fdc->sr1 |= F_SR1_ND;
	fdc->cnt = 2;
	fdc->pos = 0;
}

static fdcCall uReadD01[] = {&uread01,&uread02,&uread03,&uread04,&uread04c,&uread05,&ureadRS,&uTerm};

void uread00(FDC* fdc) {
	uSetDrive(fdc);
// NOTE: multitrack means select hd1 after hd0 is done, not to read hd0 from start
	fdc->sec = fdc->comBuf[3];	// R, sec.num
	if (!(fdc->flp->insert && fdc->flp->door)) {
		fdc->sr0 |= F_SR0_AT | F_SR0_NR;	// not ready
		ureadRS(fdc);		// prepare resp
		uTerm(fdc);		// terminate
	} else {
		fdc->sr1 = F_SR1_MA | F_SR1_ND;	// until an ID mark, and the sector, turn up
		fdc->cnt = 2;
		fdc->wait += fdc_turbo(fdc) ? UPD_TURBO_BYTE : fdc->hlt;	// load head
		fdc->pos++;
	}
}

void uread00a(FDC* fdc) {
	fdc->plan = uReadD01;
	fdc->pos = 0;
}

static fdcCall uRdData[] = {&uwargs,&uread00,&uread00a};

// 0.MF.SK.00010 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,DTL] R [SR0,SR1,SR2,C,H,R,N]
// read track

// From the index hole on, every sector in the order the track holds them, read
// as N says and handed over whole, CRC errors and all; EOT sectors and it ends
// (EN), or the index comes round again first. ND: the named ID was not among them.

void urdtrk00(FDC* fdc) {
	uSetDrive(fdc);
	fdc->sec = fdc->comBuf[3];		// R, sec.num
	if (!(fdc->flp->insert && fdc->flp->door)) {
		fdc->sr0 |= F_SR0_AT | F_SR0_NR;	// not ready
		ureadRS(fdc);		// prepare resp
		uTerm(fdc);		// terminate
	} else {
		fdc->scnt = 0;		// sectors read
		fdc->sr1 = F_SR1_MA | F_SR1_ND;
		fdc->pos++;
		fdc->wait = 0;
	}
}

// wait IDX
void urdtrk01(FDC* fdc) {
	if (seekADR(fdc) == 2)
		fdc->pos++;
}

// find ADR
void urdtrk02(FDC* fdc) {
	int res = seekADR(fdc);
	if (res == 2) {				// round again short of EOT
		fdc->sr0 |= F_SR0_AT;
		ureadRS(fdc);
		uTerm(fdc);
	} else if (res == 1) {
		fdc->sr1 &= ~F_SR1_MA;
		if (fdc->crc != fdc->fcrc) fdc->sr1 |= F_SR1_DE;
		if (u_match(fdc)) fdc->sr1 &= ~F_SR1_ND;	// the named ID was among them
		fdc->cnt = 52;
		fdc->pos++;
	}
}

// find DATA
void urdtrk03(FDC* fdc) {
	int res = u_find_dam(fdc);
	if (res == 2) {
		ureadRS(fdc);
		uTerm(fdc);
	} else if (res == 1) {
		fdc->pos++;
		fdc->wait = 1;
	}
}

// uread03: prepare to data transfer
// uread04: transfer data FDD->FDC->CPU
void urdtrk04(FDC* fdc) {
	if (!u_last_taken(fdc)) return;
	u_data_crc(fdc);		// a CRC error is flagged, the read goes on
	fdc->pos++;
}

// go next sector
void urdtrk05(FDC* fdc) {
	fdc->scnt++;
	if (fdc->mr || (fdc->scnt >= fdc->comBuf[5])) {	// EOT
		if (!fdc->mr) {
			fdc->sr0 |= F_SR0_AT;
			fdc->sr1 |= F_SR1_EN;
		}
		fdc->pos++;
	} else {
		fdc->pos = 3;
	}
}

static fdcCall uRdTrk[] = {&uwargs, &urdtrk00, &urdtrk01, &urdtrk02, &urdtrk03, &uread03, &uread04, &urdtrk04, &urdtrk05, &ureadRS, &uTerm};

// MT.MF.SK.10001 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,STP] R [SR0,SR1,SR2,C,H,R,N]	scan equal
// MT.MF.SK.11001 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,STP] R [SR0,SR1,SR2,C,H,R,N]	scan low or equal
// MT.MF.SK.11101 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,STP] R [SR0,SR1,SR2,C,H,R,N]	scan high or equal

// uread00 : prepare FDC, fdc->sec = R
// uread01 : find ADR for sector C in 2 turns
// find DATA
void uscan00(FDC* fdc) {
	int res = ureadCHK(fdc,2,3);
	if (res == 1) {
		fdc->pos++;
		fdc->wait = 0;
	} else if (res == 2) {
		ureadRS(fdc);
		uTerm(fdc);
	}
}

// prepare compare
void uscan01(FDC* fdc) {
	fdc->sr2 &= 0xf3;
	fdc->sr2 |= 0x08;	// SH=1, SN=0
	fdc->cnt = 0x80 << (fdc->buf[3] & 3);
	fdc->drq = 0;
	fdc->dir = 1;
	fdc->wait = fdc->bytedelay; // turbo ? UPD_TURBO_BYTE : fdc->bytedelay;
	fdc->pos++;
}

// compare whole sector
void uscan02(FDC* fdc) {
	if (!uGetByte(fdc)) return;
	fdc->drq = 1;
	if (fdc->tmp != fdc->data)
		fdc->sr2 &= ~0x08;	// tmp!=data : SH=0 for all
	switch(fdc->com & 0x0c) {
		case 0x00:
			if (fdc->tmp != fdc->data)
				fdc->sr2 |= 0x04;	// SN=1 : not meet
			break;
		case 0x08:
			if (fdc->tmp > fdc->data)
				fdc->sr2 |= 0x04;
			break;
		case 0x0c:
			if (fdc->tmp < fdc->data)
				fdc->sr2 |= 0x04;
			break;
	}
	fdc->cnt--;
	if (fdc->cnt < 1) {
		fdc->pos++;
		fdc->wait = fdc->bytedelay; // turbo ? UPD_TURBO_BYTE : fdc->bytedelay;
	}
}

// check end
void uscan03(FDC* fdc) {
	if ((fdc->sr2 & 0x08) || (fdc->sr2 & 0x04)) {	// scan succes OR deleted sector found
		fdc->pos++;
	} else {
		fdc->sec += fdc->comBuf[7];		// STP
		if (fdc->sec >= fdc->comBuf[5]) {	// check EOT
			fdc->pos++;
		} else {
			fdc->cnt = 2;
			fdc->pos = 2;
		}
	}
}

static fdcCall uScan[] = {&uwargs, &uread00, &uread01, &uscan00, &uscan01, &uscan02, &uscan03, &ureadRS, &uTerm};

// MT.MF.SK.00101 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,DTL] R [SR0,SR1,SR2,C,H,R,N]
// MT.MF.SK.01001 [xxxxx.HD.DRV:2,C,H,R,N,EOT,GPL,DTL] R [SR0,SR1,SR2,C,H,R,N]
// write [deleted] data

void uwrdat00(FDC* fdc) {
	uSetDrive(fdc);
	fdc->sec = fdc->comBuf[3];		// R, sec.num
	if (!(fdc->flp->insert && fdc->flp->door)) {
		fdc->sr0 |= F_SR0_AT | F_SR0_NR;		// not ready
		ureadRS(fdc);
		uTerm(fdc);
	} else if (fdc->flp->protect)	{
		fdc->sr1 |= F_SR1_NW;
		fdc->sr0 |= F_SR0_AT;
		ureadRS(fdc);
		uTerm(fdc);
	} else {
		fdc->cnt = 2;
		fdc->pos++;
		fdc->drq = 1;
		fdc->dir = 0;
	}
}

// wait sector header
void uwrdat01(FDC* fdc) {
	int res = seekADR(fdc);
	if (res == 2) {				// index
		fdc->cnt--;
		if (fdc->cnt < 1) {
			fdc->sr0 |= F_SR0_AT;	// error
			fdc->sr1 |= F_SR1_ND;	// no data
			ureadRS(fdc);
			uTerm(fdc);
		}
	} else if ((res == 1) && u_id_named(fdc)) {
		fdc->cnt = 52;
		fdc->pos++;
	}
}

// wait sector data
void uwrdat02(FDC* fdc) {
	flpNext(fdc->flp, fdc->side);
	if ((fdc->flp->field == 2) || (fdc->flp->field == 3)) {		// data filed (excluding A1 A1 A1 F8(FB))
		flpPrev(fdc->flp, fdc->side);				// to data mark (F8 | FB)
		fdc->crc = 0;						// form crc for A1 A1 A1
		add_crc_16(fdc, 0xa1);
		add_crc_16(fdc, 0xa1);
		add_crc_16(fdc, 0xa1);
		unsigned char mark = (fdc->com & 8) ? 0xf8 : 0xfb;
		flpWr(fdc->flp, fdc->side, mark);
		add_crc_16(fdc, mark);
		flpNext(fdc->flp, fdc->side);
		fdc->wait = fdc->bytedelay;
		// todo : data length
		fdc->cnt = 128 << (fdc->buf[3] & 7);
		// printf("cnt = %i\n", fdc->cnt);
		fdc->tns = 0;
		fdc->pos++;
	} else {
		fdc->wait += fdc_turbo(fdc) ? UPD_TURBO_BYTE : fdc->bytedelay;
		fdc->cnt--;
		if (fdc->cnt < 1) {
			fdc->sr0 |= F_SR0_AT;		// error
			fdc->sr1 |= F_SR1_ND;		// no data
			ureadRS(fdc);
			uTerm(fdc);
		}
	}
}

// write data

void uwrdat03(FDC* fdc) {
	if (fdc->mr) {
		flpWr(fdc->flp, fdc->side, 0x00);
	} else {
		if (fdc->drq) {				// still wait byte from cpu
			fdc->sr1 |= F_SR1_OR;		// overrun. don't terminate command
		}
		flpWr(fdc->flp, fdc->side, fdc->data);
	}
	flpNext(fdc->flp, fdc->side);
	fdc->wait += fdc->bytedelay;
	fdc->cnt--;
	if (fdc->cnt < 1) {
		fdc->pos++;
	} else {
		fdc->drq = 1;		// request next byte
		if (fdc->dma) {
			fdc->xirq(fdc->irqwn, fdc->xptr);
		} else {
			uInt(fdc);
		}
	}
}

// write crc
void uwrdat04(FDC* fdc) {
	flpWr(fdc->flp, fdc->side, (fdc->crc >> 8) & 0xff);
	flpNext(fdc->flp, fdc->side);
	fdc->wait += fdc->bytedelay;
	flpWr(fdc->flp, fdc->side, fdc->crc & 0xff);
	flpNext(fdc->flp, fdc->side);
	fdc->wait += fdc->bytedelay;
	fdc->pos++;
}

// check eot/mt
void uwrdat05(FDC* fdc) {
	if (fdc->mr || !u_next_sec(fdc)) {		// TC, or EOT
		fdc->pos++;
		return;
	}
	fdc->cnt = 2;
	fdc->pos = 2;	// to uwrdat01
	fdc->drq = 1;
	fdc->dir = 0;
}

static fdcCall uWrData[] = {&uwargs,&uwrdat00,&uwrdat01,&uwrdat02,&uwrdat03,&uwrdat04,&uwrdat05,&ureadRS,&uTerm};

// 0.MF.001101 [xxxxx.H.DRV:1, N, SC, GPL, D] R [SR0,SR1,SR2,C,H,R,N]
// 1:N:bytes/sector
// 2:SC:sectors/trk
// 3:GPL:gap3 size
// 4:D:filler byte
// fdc wants C,H,R,N for each sector.
// writing start @ index, stop @ next index

void utrkfrm00(FDC* fdc) {
	uSetDrive(fdc);
	if (!(fdc->flp->insert && fdc->flp->door)) {
		fdc->sr0 |= F_SR0_AT | F_SR0_NR;		// not ready
		ureadRS(fdc);
		uTerm(fdc);
	} else if (fdc->flp->protect) {
		fdc->sr0 |= F_SR0_AT;
		fdc->sr1 |= F_SR1_NW;
		ureadRS(fdc);
		uTerm(fdc);
	} else {
		fdc->pos++;
	}
}

void utrkfrm01(FDC* fdc) {		// wait for index
	flpNext(fdc->flp, fdc->side);
	fdc->wait += fdc->bytedelay; // turbo ? UPD_TURBO_BYTE : fdc->bytedelay;
	if (fdc->flp->index) {
		fdc->cnt = 0;
		fdc->scnt = 0;
		fdc->dir = 0;
		fdc->drq = 1;		// waiting for 1st C
		if (fdc->dma) {
			fdc->xirq(fdc->irqwn, fdc->xptr);
		} else {
			uInt(fdc);
		}
		fdc->pos++;
	}
}

void diskFormTrack(Floppy* flp, int tr, Sector* sdata, int scount);
int diskFormFits(Floppy* flp, Sector* sdata, int scount);

#define U_SLST	((int)(sizeof(((FDC*)0)->slst) / sizeof(((FDC*)0)->slst[0])))

void upd_form_id(FDC* fdc, int i, const unsigned char* chrn) {
	Sector* s = &fdc->slst[i];
	s->trk = chrn[0];
	s->head = chrn[1];
	s->sec = chrn[2];
	s->sz = chrn[3];
	s->crc = -1;			// to be worked out
	s->type = 0xfb;			// a normal data mark
	s->flags = 0;
	memset(s->data, fdc->comBuf[4], 128 << (chrn[3] & 3));	// filled with D, as much as diskFormTrack takes
}

void utrkfrm02(FDC* fdc) {		// recieve cnt (4 * spt) bytes from cpu = sectors header data
	if (!fdc->drq) {		// byte recieved
		fdc->buf[fdc->cnt & 3] = fdc->data;
		fdc->cnt++;
		fdc->drq = 1;			// want next byte
		if (fdc->cnt >= 4) {		// if all 4 bytes is recieved, register new sector
			fdc->cnt = 0;
			// The index coming round ends the command (datasheet): the track
			// holds the sectors that fit, and no more IDs are asked for
			int fit = (fdc->scnt < U_SLST);
			if (fit) {
				upd_form_id(fdc, fdc->scnt, fdc->buf);
				fit = diskFormFits(fdc->flp, fdc->slst, fdc->scnt + 1);
			}
			if (fit) fdc->scnt++;				// next sector
			if (!fit || (fdc->scnt >= fdc->comBuf[2])) {	// sectors/track
				int sc = (fdc->scnt < fdc->comBuf[2]) ? fdc->scnt : fdc->comBuf[2];	// SC 0: none
				if (sc > U_SLST) sc = U_SLST;		// a count a snapshot file put there
				diskFormTrack(fdc->flp, (fdc->trk << 1) | fdc->side, fdc->slst, sc);
				fdc->drq = 0;				// all data completed, no more bytes needed
				fdc->pos++;
			} else {
				fdc->drq = 1;
				if (fdc->dma) {
					fdc->xirq(fdc->irqwn, fdc->xptr);
				} else {
					uInt(fdc);
				}
			}
		} else {
			fdc->drq = 1;		// else request new byte
			if (fdc->dma) {
				fdc->xirq(fdc->irqwn, fdc->xptr);
			} else {
				uInt(fdc);
			}
		}
	}
	fdc->wait += fdc->bytedelay;
}

void utrkfrm03(FDC* fdc) {		// emulate whole track writing
	// wait for next index
	if (flpNext(fdc->flp, fdc->side)) {
		fdc->pos++;
	} else {
		fdc->wait += fdc->bytedelay;
	}
}

static fdcCall uFormat[] = {&uwargs,&utrkfrm00,&utrkfrm01,&utrkfrm02,&utrkfrm03,&ureadRS,&uTerm};

int upd_format_ids(FDC* fdc) {
	if ((fdc->plan != uFormat) || (fdc->pos != 3)) return 0;	// utrkfrm02 alone uses them
	return (fdc->scnt < 0) ? 0 : (fdc->scnt > U_SLST) ? U_SLST : fdc->scnt;
}

// invalid op : R [SR0 = 0x80]

void uinv00(FDC* fdc) {
	fdc->sr0 = 0x80;
	fdc->resBuf[0] = fdc->sr0;
	uResp(fdc, 1, 0);
	fdc->pos++;
}

static fdcCall uInvalid[] = {&uwargs,&uinv00,&uTerm};

// 00001000 R [ST0],[PCN]
// sense interrupt status

void usint00(FDC* fdc) {
	fdc->intr = 0;
	if (!fdc->seekend) {
		fdc->plan = uInvalid;
		fdc->pos = 0;
	} else {
		fdc->state &= 0xf0;		// clear b0..3 of main status register
		fdc->resBuf[0] = fdc->sr0;
		fdc->resBuf[1] = fdc->trk;
		uResp(fdc, 2, 0);		// no int on response/reset interrupt
		fdc->seekend = 0;
		fdc->pos++;
	}
}

static fdcCall uSenseInt[] = {&uwargs,&usint00,&uTerm};

// common

static uCom uComTab[] = {
	{0x1f, 0x06, 8, uRdData},	// xxx00110 read data
	{0x1f, 0x0c, 8, uRdData},	// xxx01100 read deleted data
	{0x3f, 0x05, 8, uWrData},	// xx000101 write data
	{0x3f, 0x09, 8, uWrData},	// xx001001 write deleted data
	{0x1f, 0x02, 8, uRdTrk},	// -xx00010 read track		TODO:check
	{0xbf, 0x0a, 1, uReadID},	// 0x001010 read ID
	{0x3f, 0x0d, 5, uFormat},	// -x001101 format track	ibm send CD for format trk
	{0x1f, 0x11, 8, uScan},		// xxx10001 scan equal		TODO:check scan commands
	{0x1f, 0x19, 8, uScan},		// xxx11001 scan low or equal
	{0x1f, 0x1d, 8, uScan},		// xxx11101 scan high or equal
	{0xff, 0x07, 1, uCalib},	// 00000111 recalibrate
	{0xff, 0x08, 0, uSenseInt},	// 00001000 sense interrupt status
	{0xff, 0x03, 2, uSpecify},	// 00000011 specify
	{0xff, 0x04, 1, uDrvStat},	// 00000100 sense drive status
	{0xff, 0x0f, 2, uSeek},		// 00001111 seek
	{0x00, 0x00, 0, uInvalid}	// -------- invalid op
};

// 1)'sense interrupt status' com must be sent after 'seek' and 'recalibrate'; otherwise, fdc will consider next com as invalid
// 2)issuing 'sense interrupt status' com without interrupt pending is treated as invalid com
// 3)'seek' and 'recalibrate' doesn't have result phase, 'sense interrupt status' must be used to effectively terminate them

void uExec(FDC* fdc, unsigned char val) {
	fdc->com = val;
	int idx = 0;
	while ((uComTab[idx].mask & val) != uComTab[idx].val)
		idx++;
	fdc->comCnt = uComTab[idx].argCount;
	fdc->plan = uComTab[idx].plan;
	fdc->pos = 0;
	fdc->comPos = 0;
	fdc->idle = 0;
	if (fdc->comCnt > 0)
		fdc->drq = 1;	// wait for args
	fdc->dir = 0;		// cpu->fdc
	fdc->wait = 0;
}

void uWrite(FDC* fdc, int adr, unsigned char val) {
	if (!(adr & 1)) return;			// wr data only
	fdc->intr = 0;				// reset interrupt
	if (fdc->idle) {			// 1st byte, command
		fdc->slow = fdc->ram;
		uExec(fdc, val);
		if (val != 0x08) {		// sense interupt status doesn't change status flags
			fdc->sr0 = fdc->flp->id & 3;
			fdc->sr1 = 0x00;
			fdc->sr2 = 0x00;
		}
	} else if (fdc->comCnt > 0) {		// arguments
		fdc->comBuf[fdc->comPos] = val;
		fdc->comPos++;
		fdc->comCnt--;
		if (fdc->comCnt == 0) {
			fdc->irq = 1;		// exec
			fdc->drq = 0;
			fdc->wait = 15000;	// insert pause before execution (otherwise some commands executed too fast)
		}
	} else if (fdc->drq && !fdc->dir) {	// data cpu->fdc
		fdc->data = val;
		fdc->drq = 0;
	} else if (val == 0x04) {
		xlog(XLG_DISK, XLL_DEBUG, "sense drive status");
	}
}

// msr.bit4: set when command is send, reset after last answer byte is readed
unsigned char uRead(FDC* fdc, int adr) {
	unsigned char res = 0xff;
	fdc->intr = 0;					// reset interrupt
	if (adr & 1) {					// 1: data
		if (fdc->drq && fdc->dir) {		// data fdc->cpu
			if (fdc->irq) {			// execution: transfer data
				res = fdc->data;
				fdc->drq = 0;
			} else if (fdc->resCnt > 0) {	// result phase
				fdc->intr = 0;		// reset interrupt @ reading [TODO: on 1st byte of result]
				res = fdc->resBuf[fdc->resPos];
				fdc->resPos++;
				fdc->resCnt--;
				if (fdc->resCnt == 0) {	// result phase completed
					uidle(fdc);
				}
			} else {			// other
				res = 0xff;
			}
		}
	} else {			// RD 0 : main status register
		res = (fdc->state & 0x0f) | (fdc->drq << 7) | (fdc->dir << 6) | (!fdc->idle << 4);
		if (fdc->irq && !fdc->dma)		// only in non-dma mode
			res |= 0x20;
	}
	return res;
}

// Every plan a command can run, so a snapshot can name the one in progress
// (dif_plan_id())
fdcCall* const uPlans[] = {utermTab, uSpecify, uDrvStat, uCalib, uSeek, uReadID, uReadD01,
	uRdData, uRdTrk, uScan, uWrData, uFormat, uInvalid, uSenseInt, NULL};

// Layout 0 had no uread04c, and its Read Track went from the data to the next
// sector at once, counting the sectors in sec
void upd_plan_v0(FDC* fdc) {
	if ((fdc->plan == uReadD01) && (fdc->pos >= 4)) fdc->pos++;
	if (fdc->plan == uRdTrk) {
		if (fdc->pos >= 7) fdc->pos++;
		fdc->scnt = fdc->sec;
		fdc->sec = fdc->comBuf[3];
	}
}

void uReset(FDC* fdc) {
//	fdc->idle = 1;
//	fdc->drq = 1;
//	fdc->dir = 0;	// cpu->fdc
	uidle(fdc);
	fdc->mr = 0;
	fdc->slow = 0;
	fdc->plan = NULL;
	fdc->wait = 0;
	fdc->trk = 0;
	fdc->sec = 0;
	fdc->data = 0;
	fdc->state = 0;
	fdc->srt = 1000;
	fdc->hlt = 0;
	fdc->hut = 0;
	fdc->resCnt = 0;
	fdc->resPos = 0;
	// TODO:check this
	fdc->intr = 0;		// ibm bios want interrupt after reset
	uInt(fdc);
	fdc->sr0 = 0xc0;	// and b7,6=11 in sr0
}

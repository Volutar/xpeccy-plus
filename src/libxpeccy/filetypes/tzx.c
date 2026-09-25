#include <stdlib.h>
#include "filetypes.h"

#ifdef HAVEZLIB
#include <zlib.h>
#endif

#pragma pack (push, 1)

typedef struct {
	char sign[7];
	char eot;
	char major;
	char minor;
} tzxHead;

#pragma pack (pop)

typedef struct {
	unsigned char id;
	void(*callback)(FILE*, Tape*);
} tzxBCall;

//#define NS_TICK TAPCPUNS
//#define NS_TICK 284
// #define NS_TICK tape->t_ns

static int sigLens[] = {PILOTLEN,SYNC1LEN,SYNC2LEN,SIGN0LEN,SIGN1LEN,0,-1};	// 0->SYNC3LEN

// Every pulse opens with a level change, the first one of a block too. A block
// with no pause after it hands its level on to the next, so one built from
// scratch at the low level loses that edge whenever the pulses before it are an
// odd count - and a loader reading the last bit of the block waits for it.
static void tzxAddBlock(Tape* tape) {
	TapeBlock* blk = &tape->tmpBlock;
	if ((tape->blkCount > 0) && (blk->sigCount > 0)) {
		TapeBlock* prv = &tape->blkData[tape->blkCount - 1];
		if (prv->sigCount > 0) {
			int pv = prv->data[prv->sigCount - 1].vol;
			int nv = blk->data[0].vol;
			if (!TAP_VOL_PAUSE(pv) && !TAP_VOL_PAUSE(nv) && (TAP_VOL_LEV(pv) == TAP_VOL_LEV(nv))) {
				for (int i = 0; i < blk->sigCount; i++)
					blk->data[i].vol ^= TAP_VOL_PAUSE(blk->data[i].vol) ? 0xff : 0xe0;
			}
		}
	}
	tap_add_block(tape, *blk);
}

// A data block goes after whatever #12/#13 pulses are pending: they are its own
// pilot and sync (a #11 with no pilot relies on that), so they are kept, not replaced.
static void tzxTakeData(Tape* tape, TapeBlock blk) {
	TapeBlock* tmp = &tape->tmpBlock;
	int pre = tmp->sigCount;
	int i;
	if (pre == 0) {
		blk.breakPoint = tmp->breakPoint;	// a #20 stop before it
		blk.stopMark = tmp->stopMark;
		blk.stop48 = tmp->stop48;
		blkClear(tmp);
		*tmp = blk;
		return;
	}
	for (i = 0; i < blk.sigCount; i++)
		blkAddPulse(tmp, blk.data[i].size, -1);
	tmp->plen = blk.plen;
	tmp->s1len = blk.s1len;
	tmp->s2len = blk.s2len;
	tmp->len0 = blk.len0;
	tmp->len1 = blk.len1;
	tmp->pdur = blk.pdur;
	tmp->hasBytes = blk.hasBytes;
	tmp->isHeader = blk.isHeader;
	tmp->dataPos = pre + blk.dataPos;
	blkClear(&blk);
}

// what is pending becomes a block of its own, closed by a pause of ms
static void tzxCloseBlock(Tape* tape, int ms) {
	blkAddPause(&tape->tmpBlock, ms * 1e6 / TAPTICKNS);
	tzxAddBlock(tape);
	blkClear(&tape->tmpBlock);
}

// #10: <pause:2>,<datalen:2>,{data}
void tzxBlock10(FILE* file, Tape* tape) {
	int pausems = fgetw(file);
	int len = fgetw(file);
	char buf[0x10000];
	fread(buf, len, 1, file);
	tzxTakeData(tape, tapDataToBlock(buf, len, sigLens));
	tzxCloseBlock(tape, pausems);
}

// #11: <pilot:2>,<sync1:2>,<sync2:2>,<bit0:2>,<bit1:2>,<pilotcnt:2>,<usedbits:1>,<pause:2>,<datalen:3>,{data}
void tzxBlock11(FILE* file, Tape* tape) {
	int altLens[7];
	altLens[0] = fgetw(file) * TAPCPUNS / TAPTICKNS;	// pilot
	altLens[1] = fgetw(file) * TAPCPUNS / TAPTICKNS;	// sync1
	altLens[2] = fgetw(file) * TAPCPUNS / TAPTICKNS;	// sync2
	altLens[3] = fgetw(file) * TAPCPUNS / TAPTICKNS;	// 0
	altLens[4] = fgetw(file) * TAPCPUNS / TAPTICKNS;	// 1
	altLens[6] = fgetw(file);			// pilot pulses
	int bits = fgetc(file);				// used bits in last byte
	int pausems = fgetw(file);
	int len = fgett(file);		// freadLen(file, 3);
	char* buf = (char*)malloc(len);
	fread(buf, len-1, 1, file);
	tzxTakeData(tape, tapDataToBlock(buf, len-1, altLens));
	int data = fgetc(file);		// last byte
	if (bits > 8) bits = 8;
	if (bits != 8) tape->isData = 0;
	while (bits > 0) {
		blkAddWave(&tape->tmpBlock, (data & 0x80) ? altLens[4] : altLens[3]);
		bits--;
		data <<= 1;
	}
	tzxCloseBlock(tape, pausems);
	free(buf);
}

// #12: <pulselen:2>,<count:2>
void tzxBlock12(FILE* file, Tape* tape) {
	int len = fgetw(file) * TAPCPUNS / TAPTICKNS;	// Length of one pulse in T-states -> mks
	int count = fgetw(file);		// Number of pulses
	while (count > 0) {
		blkAddPulse(&tape->tmpBlock, len, -1);
		count--;
	}
	tape->isData = 0;
}

// #13: <count:1>,{len:2*count}		pulse seq
void tzxBlock13(FILE* file, Tape* tape) {
	int len;
	int count = fgetc(file);
	while (count > 0) {
		len = fgetw(file) * TAPCPUNS / TAPTICKNS;
		blkAddPulse(&tape->tmpBlock, len, -1);
		count--;
	}
	tape->isData = 0;
}

// #14: <bit0:2>,<bit1:2>,<usedbits:1>,<pause:2>,<len:3>,{data:len}
void tzxBlock14(FILE* file, Tape* tape) {
	int bit0 = fgetw(file) * TAPCPUNS / TAPTICKNS;
	int bit1 = fgetw(file) * TAPCPUNS / TAPTICKNS;
	int data;
	int bits = fgetc(file);			// used bits in last byte
	int pausems = fgetw(file);
	int pause = pausems * 1e6 / TAPTICKNS;	// ms -> ticks
	int len = fgett(file);
	while (len > 1) {
		data = fgetc(file);
		blkAddByte(&tape->tmpBlock, data & 0xff, bit0, bit1);
		len--;
	}
	// last byte
	data = fgetc(file);
	if (bits > 8) bits = 8;
	while (bits > 0) {
		blkAddWave(&tape->tmpBlock, (data & 0x80) ? bit1 : bit0);
		bits--;
		data <<= 1;
	}
	// the pause is on the tape whatever its length; only a long one also
	// parts the blocks in the list
	blkAddPause(&tape->tmpBlock, pause);
	if (pausems > 100) {		// .1 sec will be block separator
		tzxAddBlock(tape);
		blkClear(&tape->tmpBlock);
	}
	tape->isData = 0;
}

// #15,<step:2>,<pause:2>,<last:1>,<len:3>,{data:len}
// direct recording: one bit per sample, 1 is the high level. A change opens the
// next pulse, so the sample it happens on is the first of that one and not the
// last of the one it closes - counting it into neither made every pulse a
// sample short, which a loader reading four samples to a bit cannot survive.
void tzxBlock15(FILE* file, Tape* tape) {
	int size = fgetw(file) * TAPCPUNS / TAPTICKNS;
	int pausems = fgetw(file);
	int pause = pausems * 1e6 / TAPTICKNS;
	int last = fgetc(file);
	if ((last < 1) || (last > 8)) last = 8;		// bits used in the last byte
	int len = (fgett(file) - 1) * 8 + last;		// bits
	int data = 0;
	int cnt;
	int bit;
	int lev = -1;					// level of the pulse being measured
	int memt = 0;
	for (cnt = 0; cnt < len; cnt++) {
		if ((cnt & 7) == 0)
			data = fgetc(file) & 0xff;
		bit = (data & 0x80) ? 1 : 0;
		data <<= 1;
		if (lev < 0) {
			lev = bit;			// the level the recording opens on
		} else if (bit != lev) {
			blkAddPulseLev(&tape->tmpBlock, memt, lev);
			lev = bit;
			memt = 0;
		}
		memt += size;
	}
	if (memt > 0)
		blkAddPulseLev(&tape->tmpBlock, memt, lev);
	blkAddPause(&tape->tmpBlock, pause);
	tape->isData = 0;
}

// #18,<len:4>,<pause:2>,<rate:3>,<compression:1>,<pulses:4>,{data}
// a CSW recording: each byte is a pulse in samples, a 0 byte gives the length in
// the four after it. Compression 2 is that stream deflated. The lengths are
// summed before they are turned into ticks, so the rounding does not add up.
void tzxBlock18(FILE* file, Tape* tape) {
	int len = fgeti(file);
	int pausems = fgetw(file);
	long long rate = fgett(file);
	int comp = fgetc(file);
	fgeti(file);				// pulse count, known from the data
	len -= 10;
	tape->isData = 0;
	if ((len < 1) || (rate < 1)) return;
	unsigned char* buf = (unsigned char*)malloc(len);
	len = fread(buf, 1, len, file);
	unsigned char* rle = buf;
	long rlen = len;
	if (comp == 2) {
		rle = NULL;
		rlen = 0;
#ifdef HAVEZLIB
		z_stream strm;
		long cap = 0;
		memset(&strm, 0, sizeof(strm));
		if (inflateInit(&strm) == Z_OK) {
			strm.next_in = buf;
			strm.avail_in = len;
			int err = Z_OK;
			while (err == Z_OK) {
				if (rlen == cap) {
					cap = cap ? cap * 2 : len * 4 + 256;
					rle = (unsigned char*)realloc(rle, cap);
				}
				strm.next_out = rle + rlen;
				strm.avail_out = cap - rlen;
				err = inflate(&strm, Z_NO_FLUSH);
				rlen = cap - strm.avail_out;
			}
			inflateEnd(&strm);
		}
#endif
	}
	long long samples = 0;
	long long ticks = 0;
	long i = 0;
	while (i < rlen) {
		long n = rle[i++];
		if (n == 0) {
			if (i + 4 > rlen) break;
			n = rle[i] | (rle[i + 1] << 8) | (rle[i + 2] << 16) | ((long)rle[i + 3] << 24);
			i += 4;
		}
		samples += n;
		long long end = samples * 1000000000LL / (rate * TAPTICKNS);
		blkAddPulse(&tape->tmpBlock, (int)(end - ticks), -1);
		ticks = end;
	}
	if (rle != buf) free(rle);
	free(buf);
	tzxCloseBlock(tape, pausems);
}

// #19 symbols: flags, then up to max pulses in T, a 0 ending them early. The
// flags say what happens at the start: an edge (0), none (1), or the level
// forced low (2) or high (3) - no edge if it is there already.
static void tzxSymbol(Tape* tape, unsigned char* def, int npulses) {
	TapeBlock* blk = &tape->tmpBlock;
	int flag = def[0] & 3;
	int i;
	for (i = 0; i < npulses; i++) {
		int len = (def[1 + i * 2] | (def[2 + i * 2] << 8)) * TAPCPUNS / TAPTICKNS;
		if (len == 0) break;
		int cur = (blk->sigCount > 0) ? TAP_VOL_LEV(blk->data[blk->sigCount - 1].vol) : -1;
		int keep = 0;				// no edge: the last pulse goes on
		if (i == 0) {
			if (flag == 1) keep = (cur >= 0);
			else if (flag > 1) keep = (cur == (flag & 1));
		}
		if (keep) {
			blk->data[blk->sigCount - 1].size += len;
		} else if ((i == 0) && (flag > 1)) {
			blkAddPulseLev(blk, len, flag & 1);
		} else {
			blkAddPulse(blk, len, -1);
		}
	}
}

// #19,<len:4>,<pause:2>,<totp:4>,<npp:1>,<asp:1>,<totd:4>,<npd:1>,<asd:1>,
// {pilot symbols},{pilot runs: symbol:1,count:2},{data symbols},{data: bits}
// generalized data: a pilot and sync as runs of symbols, then data as symbols
// of ceil(log2(asd)) bits each, most significant first
void tzxBlock19(FILE* file, Tape* tape) {
	int len = fgeti(file);
	int pausems = fgetw(file);
	long totp = fgeti(file);
	int npp = fgetc(file);
	int asp = fgetc(file);
	long totd = fgeti(file);
	int npd = fgetc(file);
	int asd = fgetc(file);
	if (asp == 0) asp = 256;
	if (asd == 0) asd = 256;
	len -= 14;
	tape->isData = 0;
	if (len < 1) return;
	unsigned char* buf = (unsigned char*)calloc(len + 4, 1);
	len = fread(buf, 1, len, file);
	unsigned char* end = buf + len;
	unsigned char* ptr = buf;
	int psz = 1 + npp * 2;			// one symbol definition
	int dsz = 1 + npd * 2;
	long i;
	if (totp > 0) {
		unsigned char* sym = ptr;
		ptr += asp * psz;
		for (i = 0; (i < totp) && (ptr + 3 <= end); i++, ptr += 3) {
			int s = ptr[0];
			int cnt = ptr[1] | (ptr[2] << 8);
			if (s >= asp) continue;
			while (cnt-- > 0)
				tzxSymbol(tape, sym + s * psz, npp);
		}
	}
	if (totd > 0) {
		unsigned char* sym = ptr;
		ptr += asd * dsz;
		int nb = 0;
		while ((1 << nb) < asd) nb++;
		long bit = 0;
		for (i = 0; i < totd; i++) {
			int s = 0;
			int k;
			for (k = 0; k < nb; k++, bit++) {
				if (ptr + (bit >> 3) >= end) break;
				s = (s << 1) | ((ptr[bit >> 3] >> (7 - (bit & 7))) & 1);
			}
			if (k < nb) break;		// the data runs out
			if (s < asd)
				tzxSymbol(tape, sym + s * dsz, npd);
		}
	}
	free(buf);
	tzxCloseBlock(tape, pausems);
}

// #20,<len:2> : pause or stop tape
void tzxBlock20(FILE* file, Tape* tape) {
	int len = fgetw(file) * 1e6 / TAPTICKNS;
	if (len) {
		blkAddPause(&tape->tmpBlock, len);
	} else {
		tape->tmpBlock.breakPoint = 1;
		tape->tmpBlock.stopMark = 1;
	}
}

// a <len:1>,{text:len} string, and it labels the blocks that follow

static void tzxGetText(FILE* file, Tape* tape) {
	char buf[256];
	int len = fgetc(file);
	if (len < 0) len = 0;			// truncated file
	fread(buf, len, 1, file);
	buf[len] = 0;
	tap_set_text(tape, buf);
}

// #21,<len:1>,{text:len}		group start
void tzxBlock21(FILE* file, Tape* tape) {
	tzxGetText(file, tape);
}

// #22					group end
void tzxBlock22(FILE* file, Tape* tape) {
	tap_set_text(tape, NULL);
}

// #2A,<len:4>				stop the tape on a 48K
void tzxBlock2A(FILE* file, Tape* tape) {
	tape->tmpBlock.stop48 = 1;
}

// #2B,<len:4>,<lev:1>			the level the next pulse is played at
void tzxBlock2B(FILE* file, Tape* tape) {
	fseek(file, 4, SEEK_CUR);
	tape->tmpBlock.vol = fgetc(file) ? 1 : 0;
}

// #30,<len:1>,<text:len>		description
void tzxBlock30(FILE* file, Tape* tape) {
	tzxGetText(file, tape);
}

// Everything else is information or a menu for a person (#28, #31-#35, #40, #5A)
// and is stepped over; the reader knows every block's size, so an unknown one
// costs nothing either.
tzxBCall tzxBlockTab[] = {
	{0x10, tzxBlock10},
	{0x11, tzxBlock11},
	{0x12, tzxBlock12},
	{0x13, tzxBlock13},
	{0x14, tzxBlock14},
	{0x15, tzxBlock15},
	{0x18, tzxBlock18},
	{0x19, tzxBlock19},
	{0x20, tzxBlock20},
	{0x21, tzxBlock21},
	{0x22, tzxBlock22},
	{0x2a, tzxBlock2A},
	{0x2b, tzxBlock2B},
	{0x30, tzxBlock30},

	{0xff, NULL}
};

static long tzxRead(FILE* file, long pos, int n) {
	long res = 0;
	int i;
	fseek(file, pos, SEEK_SET);
	for (i = 0; i < n; i++)
		res |= (long)(fgetc(file) & 0xff) << (i * 8);
	return res;
}

// The size of a block after its id, from the fields its header gives. A block
// this list does not name starts with its length, as the spec asks of new ones.
static long tzxBodySize(FILE* file, long pos, int id) {
	switch (id) {
		case 0x10: return 0x04 + tzxRead(file, pos + 0x02, 2);
		case 0x11: return 0x12 + tzxRead(file, pos + 0x0f, 3);
		case 0x12: return 0x04;
		case 0x13: return 0x01 + tzxRead(file, pos, 1) * 2;
		case 0x14: return 0x0a + tzxRead(file, pos + 0x07, 3);
		case 0x15: return 0x08 + tzxRead(file, pos + 0x05, 3);
		case 0x20: case 0x23: case 0x24: return 0x02;
		case 0x21: case 0x30: return 0x01 + tzxRead(file, pos, 1);
		case 0x22: case 0x25: case 0x27: return 0;
		case 0x26: return 0x02 + tzxRead(file, pos, 2) * 2;
		case 0x28: case 0x32: return 0x02 + tzxRead(file, pos, 2);
		case 0x31: return 0x02 + tzxRead(file, pos + 0x01, 1);
		case 0x33: return 0x01 + tzxRead(file, pos, 1) * 3;
		case 0x34: return 0x08;
		case 0x35: return 0x14 + tzxRead(file, pos + 0x10, 4);
		case 0x40: return 0x04 + tzxRead(file, pos + 0x01, 3);
		case 0x5a: return 0x09;
		// fixed, whatever their length field says: some images give #2A a 4 there
		case 0x2a: return 0x04;
		case 0x2b: return 0x05;
	}
	return 0x04 + tzxRead(file, pos, 4);	// 18, 19 and anything newer
}

typedef struct {
	int id;
	long pos;		// where its body starts
	int taken;		// a jump taken once already
} tzxBlockPos;

// Blocks read, jumps and calls included, before the file counts as looping for ever
#define TZX_MAX_STEPS	0x100000

// The blocks are read in the order the tape plays them: #23 jumps, #24/#25 loop
// and #26/#27 call - all relative to the block they stand in. A jump taken a
// second time is a tape that loops for ever, and the reading ends there.
int loadTZX(Computer* comp, const char* name, int drv) {
	Tape* tape = comp->tape;
	FILE* file = fopen(name, "rb");
	if (!file) return ERR_CANT_OPEN;

	tzxHead hd;
	fread((char*)&hd, sizeof(tzxHead), 1, file);
	if ((strncmp(hd.sign, "ZXTape!",7) != 0) || (hd.eot != 0x1a)) {
		fclose(file);
		return ERR_TZX_SIGN;
	}
	fseek(file, 0, SEEK_END);
	long fsize = ftell(file);
	long pos = sizeof(tzxHead);
	tzxBlockPos* blk = NULL;
	int cnt = 0;
	while (pos < fsize) {
		if ((cnt & 0xff) == 0)
			blk = (tzxBlockPos*)realloc(blk, (cnt + 0x100) * sizeof(tzxBlockPos));
		blk[cnt].id = (int)tzxRead(file, pos, 1);
		blk[cnt].pos = pos + 1;
		blk[cnt].taken = 0;
		pos = blk[cnt].pos + tzxBodySize(file, blk[cnt].pos, blk[cnt].id);
		cnt++;
	}

	tapEject(tape);
	tape->isData = 1;

	int pc = 0;
	int steps = 0;
	int loopPc = -1;
	int loopCount = 0;
	int callPc = -1;		// the #26 being run
	int callIdx = 0;
	int callCnt = 0;
	int i, off;
	while ((pc >= 0) && (pc < cnt) && (steps++ < TZX_MAX_STEPS)) {
		fseek(file, blk[pc].pos, SEEK_SET);
		switch (blk[pc].id) {
			case 0x23:
				off = (short)fgetw(file);
				if ((off == 0) || blk[pc].taken) {
					pc = cnt;
				} else {
					blk[pc].taken = 1;
					pc += off;
				}
				break;
			case 0x24:
				loopCount = fgetw(file);
				loopPc = pc + 1;
				pc++;
				break;
			case 0x25:
				if ((loopPc >= 0) && (loopCount > 1)) {
					loopCount--;
					pc = loopPc;
				} else {
					loopPc = -1;
					pc++;
				}
				break;
			case 0x26:
				callCnt = fgetw(file);
				if (callCnt > 0) {
					callPc = pc;
					callIdx = 0;
					pc += (short)fgetw(file);
				} else {
					pc++;
				}
				break;
			case 0x27:
				if (callPc < 0) {
					pc++;
					break;
				}
				callIdx++;
				if (callIdx < callCnt) {
					fseek(file, blk[callPc].pos + 2 + callIdx * 2, SEEK_SET);
					pc = callPc + (short)fgetw(file);
				} else {
					pc = callPc + 1;
					callPc = -1;
				}
				break;
			default:
				i = 0;
				while ((tzxBlockTab[i].id != 0xff) && (tzxBlockTab[i].id != blk[pc].id))
					i++;
				if (tzxBlockTab[i].callback)
					tzxBlockTab[i].callback(file, tape);
				pc++;
				break;
		}
	}
	free(blk);
	if (tape->tmpBlock.sigCount > 0)
		tzxAddBlock(tape);
	blkClear(&tape->tmpBlock);
	tap_set_text(tape, NULL);
	tape_set_path(tape, name);
	fclose(file);
	return ERR_OK;
}

#include <stdlib.h>
#include "xlog.h"
#include <stdio.h>
#include <string.h>

#include "tape.h"

// NEW STUFF

Tape* tape_create(cbirq cb, void* p) {
	Tape* tap = (Tape*)malloc(sizeof(Tape));
	memset(tap,0x00,sizeof(Tape));
	tap->isData = 1;
	tap->path = NULL;
	tap->blkData = NULL;
	tap->tmpBlock.data = NULL;
	tap->xirq = cb;
	tap->xptr = p;
	tap->volPlay = 0x80;
	tape_set_tick_ns(tap, TAPTICKNS);
	tap->speed = 100;
	tap->autorew = 1;
	blkClear(&tap->tmpBlock);
	return tap;
}

void tape_destroy(Tape* tap) {
	if (tap->path) free(tap->path);
	tapEject(tap);
	if (tap->tmpBlock.data)
		free(tap->tmpBlock.data);
	free(tap);
}

// The machine's own tick period: tzx/tap times are T states, and this is what
// one of them lasts. A fixed 284ns tick (3.52MHz) made every pulse .6% short
// of what a 3.5MHz loader measures, which is enough for a loader that checks
// its pilot against a fixed length to reject the tape outright.
// Kept as its reciprocal: tapSync runs once per opcode, and a divide by a
// variable there is an idiv the compiler cannot fold away.
void tape_set_tick_ns(Tape* tap, double ns) {
	if (!tap || (ns <= 0)) return;
	tap->ticksPerNsFixed = llround((double)(1LL << TAPE_RATE_BITS) / ns);
}

void tape_set_path(Tape* tap, const char* path) {
	if (path != NULL) {
		tap->path = realloc(tap->path, strlen(path) + 1);
		strcpy(tap->path, path);
	} else {
		free(tap->path);
		tap->path = NULL;
	}
}

// signal edge detector

#define EDGE_FIX	8			// fraction bits of the running averages
#define EDGE_FLOOR	(150 << EDGE_FIX)	// a swing smaller than this is silence, not a signal

// the averages follow the tape over ~12 ms, an order above the shortest pulse
void tape_edge_reset(TapeEdge* det, int rate) {
	int n = rate / 80;
	det->shift = 1;
	while ((1 << (det->shift + 1)) <= n)
		det->shift++;
	det->lev = 0;
	det->first = 1;
	det->dc = 0;
	det->env = 0;
}

int tape_edge_step(TapeEdge* det, int amp) {
	int val = amp << EDGE_FIX;
	int dev, hyst;
	if (det->first) {				// start on the signal, not on zero
		det->first = 0;
		det->dc = val;
	}
	det->dc += (val - det->dc) >> det->shift;
	dev = val - det->dc;
	det->env += (((dev < 0) ? -dev : dev) - det->env) >> (det->shift + 1);
	hyst = det->env >> 2;				// the trip point follows the level of the recording
	if (hyst < EDGE_FLOOR) hyst = EDGE_FLOOR;
	if (det->lev ? (dev < -hyst) : (dev > hyst)) {
		det->lev = !det->lev;
		return 1;
	}
	return 0;
}

// blocks

void blkClear(TapeBlock *blk) {
	if (blk->data) {
		free(blk->data);
		blk->data = NULL;
	}
	blk->breakPoint = 0;
	blk->stopMark = 0;
	blk->isHeader = 0;
	blk->hasBytes = 0;
	blk->sigCount = 0;
	blk->dataPos = -1;
	blk->text[0] = 0;
}

// the label blocks read in from here on will carry. NULL clears it

void tap_set_text(Tape* tap, const char* txt) {
	if (txt == NULL) {
		tap->blkText[0] = 0;
	} else {
		strncpy(tap->blkText, txt, TAPE_TEXT_LEN - 1);
		tap->blkText[TAPE_TEXT_LEN - 1] = 0;
	}
}

// add signal (1 level change)
void blkAddPulse(TapeBlock* blk, int len, int vol) {
	if ((blk->sigCount & 0xffff) == 0) {
		blk->data = realloc(blk->data,(blk->sigCount + 0x10000) * sizeof(TapeSignal));	// allocate mem for next 0x10000 signals
	}
	if (vol < 0)
		vol = blk->vol ? 0xb0 : 0x50;
	blk->data[blk->sigCount].size = len;
	blk->data[blk->sigCount].vol = vol & 0xff;
	blk->vol = (vol & 0x80) ? 0 : 1;
	blk->sigCount++;
}

// add pause. duration in mks
// A pause is silence on the tape, not a level held: the centre puts no dc step in
// the mix. Bit 7 is the ear bit and still flips, so a loader sees no change.
void blkAddPause(TapeBlock* blk, int len) {
	if (len < 1) return;
	blkAddPulse(blk, len, blk->vol ? 0x80 : 0x7f);
}

// add wave (2 pulses)
void blkAddWave(TapeBlock* blk, int len) {
	if (len < 1) return;
	blkAddPulse(blk,len, -1);
	blkAddPulse(blk,len, -1);
}

// add byte. b0len/b1len = duration of 0/1 bits. When 0, it takes from block signals data
void blkAddByte(TapeBlock* blk, unsigned char data, int b0len, int b1len) {
	if (b0len == 0) b0len = blk->len0;
	if (b1len == 0) b1len = blk->len1;
	for (int i = 0; i < 8; i++) {
		blkAddWave(blk, (data & 0x80) ? b1len : b0len);
		data <<= 1;
	}
}

// current time in block
// NOTE: not full time of block
int tapGetBlockTime(Tape* tape, int blk, int pos) {
	long long totsz = 0;
	int i;
	if (pos > tape->blkData[blk].sigCount)
		pos = tape->blkData[blk].sigCount;
	else if (pos < 0)
		pos = tape->blkData[blk].sigCount;
	for (i = 0; i < tape->blkData[blk].sigCount; i++)
		totsz += tape->blkData[blk].data[i].size;
	return (totsz / TAPTPS);			// mks -> sec
}

int tapGetBlockSize(TapeBlock* block) {
	int res = 0;
	if (block->dataPos >= 0)			// < 0: pure signal, no bytes to count
		res = ((block->sigCount - block->dataPos) >> 4) - 2;
	if (res < 0) res = 0;
	return res;
}

unsigned char tapGetBlockByte(TapeBlock* block, int bytePos) {
	unsigned char res = 0x00;
	int i;
	int sigPos = block->dataPos + (bytePos << 4);
	for (i = 0; i < 8; i++) {
		res <<= 1;
		if (sigPos < (int)(block->sigCount - 1)) {
			if ((block->data[sigPos].size == block->len1) && (block->data[sigPos + 1].size == block->len1)) {
				res |= 1;
			}
			sigPos += 2;
		}
	}
	return res;
}

int tapGetBlockData(Tape* tape, int blockNum, unsigned char* dst,int maxsize) {
	TapeBlock* block = &tape->blkData[blockNum];
	int pos = block->dataPos;
	int bytePos = 0;
	do {
		dst[bytePos] = tapGetBlockByte(block,bytePos);
		bytePos++;
		pos += 16;
	} while ((pos < (block->sigCount - 1)) && (bytePos < maxsize));
	return bytePos;
}

// a standard header is 19 bytes: flag, type, 10 chars of name, the length of the
// data block that follows, and two parameters whose meaning depends on the type

int tapGetBlockHeader(TapeBlock* block, TapeBlockInfo* inf) {
	int i;
	if (!block->isHeader) return 0;
	if (tapGetBlockSize(block) != 17) return 0;
	inf->htype = tapGetBlockByte(block, 1);
	for (i = 0; i < 10; i++)
		inf->name[i] = tapGetBlockByte(block, i + 2);
	i = 10;
	while ((i > 0) && (inf->name[i - 1] == ' '))		// the name is padded with spaces
		i--;
	inf->name[i] = 0;
	inf->dlen = tapGetBlockByte(block, 12) | (tapGetBlockByte(block, 13) << 8);
	inf->par1 = tapGetBlockByte(block, 14) | (tapGetBlockByte(block, 15) << 8);
	return 1;
}

TapeBlockInfo tapGetBlockInfo(Tape* tap, int blk) {
	TapeBlock* block = &tap->blkData[blk];
	TapeBlockInfo inf;
	memset(&inf, 0x00, sizeof(TapeBlockInfo));
	inf.htype = TAPE_HT_NONE;
	inf.type = TAPE_DATA;
	if (tapGetBlockHeader(block, &inf)) inf.type = TAPE_HEAD;
	strcpy(inf.text, block->text);
	inf.hasBytes = block->hasBytes;
	inf.size = tapGetBlockSize(block);
	inf.time = block->time;
	inf.breakPoint = block->breakPoint;
	inf.stopMark = block->stopMark;
	return inf;
}

int tapGetBlocksInfo(Tape* tap, TapeBlockInfo* dst) {
	int cnt = 0;
	int i;
	for (i=0; i < (int)tap->blkCount; i++) {
		dst[cnt] = tapGetBlockInfo(tap,i);
		cnt++;
	}
	return cnt;
}

void tapNormSignals(TapeBlock* block) {
	int low,hi;
	int i;
	for (i = 0; i < (int)block->sigCount; i++) {
		low = block->data[i].size - 3;
		hi = block->data[i].size + 3;
		if ((block->plen > low) && (block->plen < hi)) block->data[i].size = block->plen;
		if ((block->s1len > low) && (block->s1len < hi)) block->data[i].size = block->s1len;
		if ((block->s2len > low) && (block->s2len < hi)) block->data[i].size = block->s2len;
		if ((block->len0 > low) && (block->len0 < hi)) block->data[i].size = block->len0;
		if ((block->len1 > low) && (block->len1 < hi)) block->data[i].size = block->len1;
	}
}

void tapSwapBlocks(Tape* tap, int b1, int b2) {
	if ((b1 < tap->blkCount) && (b2 < tap->blkCount)) {
		TapeBlock tmp = tap->blkData[b1];
		tap->blkData[b1] = tap->blkData[b2];
		tap->blkData[b2] = tmp;
	}
}

void tapDelBlock(Tape* tap, int blk) {
	if (blk < tap->blkCount) {
		int idx = blk;
		if (tap->blkData[idx].data) {
			free(tap->blkData[idx].data);
			tap->blkData[idx].data = NULL;
		}
		while (idx < tap->blkCount - 1) {
			tap->blkData[idx] = tap->blkData[idx+1];
			idx++;
		}
		tap->blkCount--;
	}
}

// tape

// FIXME: non-zx must skip this part (do only tapAddBlock + blkClear + wait=1)
void tapStoreBlock(Tape* tap) {
	unsigned int i,j;
	int same;
	int diff;
	int siglens[11];
	for (i = 0; i < 11; i++) siglens[i]=0;
	int cnt = 0;
	TapeBlock* tblk = &tap->tmpBlock;
	if (tblk->sigCount < 1) return;
	if (!tblk->data) return;
	for (i = 0; i < tblk->sigCount; i++) {
		same = 0;
		for (j = 0; j < cnt; j++) {
			if (siglens[j] > 0) {
//				printf("%i %i %p %i\n",i,j,tblk->data,tblk->data->size);
				diff = (tblk->data[i].size - siglens[j]) * 100 / siglens[j];
//				printf("%i\n",diff);
				if ((diff > -5) && (diff < 5)) {
					same = 1;
				}
			} else {
				same = 1;
			}
		}
		if ((same == 0) && (cnt < 10)) {
			siglens[cnt] = tblk->data[i].size;
			cnt++;
		}
	}
//	tblk->data[tblk->sigCount-1].size = 1e6;		// last signal is 1 sec (pause)
//	tblk->data[tblk->sigCount-1].vol = tblk->vol ? 0x60 : 0xa0;

	// if there's only 00 or FF (1 bit not presented)
	if (cnt == 5) {
		siglens[5] = siglens[4];
		siglens[4] = siglens[3];
		siglens[3] = (siglens[3] > 350) ? SIGN0LEN : SIGN1LEN;
		cnt++;
	}
	if (xlog_on(XLG_TAPE, XLL_DEBUG)) {	// one record, one line
		char lens[128];
		int pos = 0;
		for (i = 0; (i < cnt) && (pos < (int)sizeof(lens) - 12); i++)
			pos += snprintf(lens + pos, sizeof(lens) - pos, " %i", siglens[i]);
		xlog_put(XLG_TAPE, XLL_DEBUG, "signals: %i:%s", cnt, lens);
	}

	tblk->breakPoint = 0;
	tblk->stopMark = 0;
	tblk->hasBytes = 0;
	tblk->isHeader = 0;
	if (cnt == 6) {
		tblk->plen = siglens[0];
		tblk->s1len = siglens[1];
		tblk->s2len = siglens[2];
		if (siglens[4] > siglens[3]) {
			tblk->len0 = siglens[3];
			tblk->len1 = siglens[4];
		} else {
			tblk->len0 = siglens[4];
			tblk->len1 = siglens[3];
		}
		tblk->hasBytes = 1;
	} else {
		tblk->plen = PILOTLEN;
		tblk->s1len = SYNC1LEN;
		tblk->s2len = SYNC2LEN;
		tblk->len0 = SIGN0LEN;
		tblk->len1 = SIGN1LEN;
	}
	tblk->dataPos = -1;
#if 1
	tapNormSignals(tblk);
	i = 1;
	while ((i < tblk->sigCount) && (tblk->data[i].size != tblk->s2len))
		i++;
	if (i < tblk->sigCount)
		tblk->dataPos = i + 1;
	if (tblk->dataPos != -1) {
		if (tapGetBlockByte(tblk,0) == 0) {
			tblk->isHeader = 1;
		}
	}
#endif
	tap_add_block(tap,tap->tmpBlock);
	blkClear(tblk);
	tap->wait = 1;
}

void tapEject(Tape* tap) {
	int i;
	tap->isData = 1;
	tap->userStop = 0;
	tap->block = 0;
	tap->pos = 0;
	tap_set_text(tap, NULL);
	tape_set_path(tap, NULL);
	if (tap->blkData) {
		for (i = 0; i < tap->blkCount; i++) {
			if (tap->blkData[i].data) {
				free(tap->blkData[i].data);
				tap->blkData[i].data = NULL;
			}
		}
		free(tap->blkData);
	}
	tap->blkCount = 0;
	tap->blkData = NULL;
}

void tapStop(Tape* tap) {
	if (tap->on) {
		xlog(XLG_TAPE, XLL_INFO, "stop, block %i of %i", tap->block, tap->blkCount);
		tap->on = 0;
		if (tap->rec)
			tapStoreBlock(tap);
		tap->volPlay = (tap->volPlay & 0x80) ? 0x7f : 0x81;
		//tap->volPlay = 0x80;
		// tap->pos = 0;
	}
	tap->detectReads = 0;
}

// Stop, as a person pressing the button: it beats the automatics. The rom trap
// and the loader detector are both still looking, so without this the tape is
// playing again a few opcodes after the button comes up - and on a tape that
// has run out there is nothing else to break the loop. Play, a rewind or
// another tape hands control back.
void tapUserStop(Tape* tap) {
	tapStop(tap);
	tap->userStop = 1;
}

int tapPlay(Tape* tap) {
	if (tap->userStop) return tap->on;
	if ((tap->block < tap->blkCount) && !tap->on) {
		xlog(XLG_TAPE, XLL_INFO, "play, block %i of %i", tap->block, tap->blkCount);
		tap->rec = 0;
		tap->on = 1;
		tap->blkData[tap->block].vol = 0;
		tap->tail = 0;
		tap->sigLen = TAPTPS / 2;	// .5 sec
		// tap->volPlay = (tap->volPlay & 0x80) ? 0x7f : 0x81;
	}
	tap->detectReads = 0;
	return tap->on;
}

// Play, as a person pressing the button: a tape sitting at its end starts over,
// else play would do nothing until it is rewound by hand. Only for that - the
// loader detector below must not come through here, or a stray pattern during
// a game would replay a tape nobody asked for.
int tapUserPlay(Tape* tap) {
	tap->userStop = 0;
	if (tap->autorew && !tap->on && (tap->block >= tap->blkCount))
		tapRewind(tap, 0);
	return tapPlay(tap);
}

// Detect loaders that bypass the ROM (custom in-game loaders): a tight loop reading
// port 0xFE a fixed number of T-states apart, with B changing by exactly 1 each time,
// is a strong sign of a bit-timing loop, so treat it as "a loader started" and play the
// tape. No matching auto-stop: the same pattern is also produced by ordinary DJNZ-timed
// delay/keyboard-wait loops, and once gameplay is scanning the keyboard port 0xFE never
// goes quiet either - so neither an irregular-read count nor an idle timeout can tell
// "loading is over" from "unrelated code is also hitting this port". TZX #20 stop
// markers and the manual Brk/Stop controls cover stopping instead.
void tapDetectLoader(Tape* tap, int tick, int regB) {
	if (!tap->detectOn || tap->on) {
		tap->detectReads = 0;
		return;
	}
	int tickDiff = tick - tap->detectLastTick;
	int bDiff = (regB - tap->detectLastB) & 0xff;
	tap->detectLastTick = tick;
	tap->detectLastB = regB & 0xff;
	if ((tickDiff <= 500) && ((bDiff == 1) || (bDiff == 0xff))) {
		tap->detectReads++;
		if (tap->detectReads >= 10)
			tapPlay(tap);
	} else {
		tap->detectReads = 0;
	}
}

void tapRec(Tape* tap) {
	xlog(XLG_TAPE, XLL_INFO, "record");
	tap->userStop = 0;
	tap->on = 1;
	tap->rec = 1;
	tap->wait = 1;
	tap->levRec = 1;
	tap->oldRec = 1;
	blkClear(&tap->tmpBlock);
}

void tapRewind(Tape* tap, int blk) {
	tap->userStop = 0;
	if (blk < tap->blkCount) {
		tap->block = blk;
		tap->pos = 0;
	} else {
		tapStop(tap);
	}
}

// nothing more to play once this block is done: it is the last one, or the one
// after it is marked to stop on
static int tap_stops_after(Tape* tap) {
	return ((tap->block + 1) >= tap->blkCount) || tap->blkData[tap->block + 1].breakPoint;
}

void tapSync(Tape* tap, int ns) {
	tap->tickAcc += (long long)ns * tap->speed * tap->ticksPerNsFixed / 100;
	int mks = (int)(tap->tickAcc >> TAPE_RATE_BITS);
	int sig;
	tap->tickAcc &= (1LL << TAPE_RATE_BITS) - 1;
	if (tap->on) {
		if (tap->rec) {
			if (tap->wait) {
				if (tap->oldRec != tap->levRec) {
					tap->oldRec = tap->levRec;
					tap->wait = 0;
					blkAddPulse(&tap->tmpBlock,0,-1);
				}
			} else {
				if (tap->oldRec != tap->levRec) {
					tap->oldRec = tap->levRec;
					blkAddPulse(&tap->tmpBlock,mks,-1);
				} else if (tap->tmpBlock.sigCount > 0) {
					tap->tmpBlock.data[tap->tmpBlock.sigCount - 1].size += mks;
					if (tap->tmpBlock.data[tap->tmpBlock.sigCount - 1].size > TAPE_PAUSE_TICKS) {
						tap->tmpBlock.sigCount--;
						tapStoreBlock(tap);
					}
				}
			}
		} else if (tap->blkCount > 0) {
			tap->sigLen -= mks;
			while ((tap->sigLen < 1) && tap->on) {
				if (tap->pos >= (int)tap->blkData[tap->block].sigCount) {
					if (tap_stops_after(tap) && !tap->tail) {
						// A pulse is a level held, so the one the last pulse ends
						// on is a level change of its own - and a loader reading
						// the last byte of a block is waiting for exactly that.
						// Stopping on the pulse swallowed it, so run the tape out
						// at the new level the way it would past the recording.
						tap->tail = 1;
						tap->volPlay = (tap->volPlay & 0x80) ? 0x7f : 0x80;	// silence, the way a pause is written
						tap->sigLen += TAPE_TAIL_TICKS;
					} else {
						tap->blkChange = 1;
						tap->block++;
						tap->pos = 0;
						if (tap->tail) {
							tap->tail = 0;
							tapStop(tap);
						}
						tap->xirq(IRQ_TAP_BLK, tap->xptr);
					}
				} else {
					sig = tap->volPlay;
					tap->sigLen += tap->blkData[tap->block].data[tap->pos].size;
					tap->volPlay = tap->blkData[tap->block].data[tap->pos].vol;
					tap->pos++;
					if (tap->xen && ((sig ^ tap->volPlay) & 0x80)) {	// signal changed
						tap->xirq(tap->volPlay & 0x80 ? IRQ_TAP_1 : IRQ_TAP_0, tap->xptr);
					}
				}
			}
		} else {
			tap->sigLen -= mks;
			while (tap->sigLen < 1) {
				tap->volPlay = 0x7f;
				// tap->volPlay = (tap->volPlay & 0x80) ? 0x7f : 0x81;
				tap->sigLen += TAPTPS / 2; // 5e5;	// .5 sec
			}
		}
	} else {
		// tape stoped
		tap->sigLen -= mks;
		while (tap->sigLen < 1) {
			tap->volPlay = 0x81;
			// tap->volPlay = (tap->volPlay & 0x80) ? 0x7f : 0x81;
			tap->sigLen += TAPTPS / 2; // 5e5;	// .5 sec
		}
	}
}

void tapNextBlock(Tape* tap) {
	tap->tail = 0;
	tap->block++;
	tap->pos = 0;
	tap->blkChange = 1;
	if (tap->block < tap->blkCount) {
		tap->blkData[tap->block].vol = 0;
		tap->volPlay = 0x7f;
	} else {
		// past the last block: stay there. Only the Play button winds a tape
		// back - which is what "Rewind at end" says it does - because winding
		// back here let autoplay start it over for ever.
		tapStop(tap);
	}
	tap->xirq(IRQ_TAP_BLK, tap->xptr);
}

// add file to tape

TapeBlock makeTapeBlock(unsigned char* ptr, int ln, int hd) {
	TapeBlock nblk;
	int i;
	unsigned char tmp;
	unsigned char crc;
	nblk.plen = PILOTLEN;
	nblk.s1len = SYNC1LEN;
	nblk.s2len = SYNC2LEN;
	nblk.len0 = SIGN0LEN;
	nblk.len1 = SIGN1LEN;
	nblk.breakPoint = 0;
	nblk.hasBytes = 1;
	nblk.isHeader = 0;
	nblk.sigCount = 0;
	nblk.data = NULL;
	nblk.vol = 0;
	if (hd) {
		nblk.pdur = 8063;
		nblk.isHeader = 1;
		crc = 0x00;
	} else {
		nblk.pdur = 3223;
		crc = 0xff;
	}
	for (i = 0; i < nblk.pdur; i++)
		blkAddPulse(&nblk,nblk.plen,-1);
	if (nblk.s1len != 0)
		blkAddPulse(&nblk,nblk.s1len,-1);
	if (nblk.s2len != 0)
		blkAddPulse(&nblk,nblk.s2len,-1);
	nblk.dataPos = nblk.sigCount;
	blkAddByte(&nblk,crc,0,0);
	for (i = 0; i < ln; i++) {
		tmp = *ptr;
		crc ^= tmp;
		blkAddByte(&nblk,tmp,0,0);
		ptr++;
	}
	blkAddByte(&nblk,crc,0,0);
	return nblk;
}

// tapeAddFile(tape, filename, type(0,3 = basic,code), start, lenght, autostart, pointer to data, is header)
void tapAddFile(Tape* tap, const char* nm, int tp, unsigned short st, unsigned short ln, unsigned short as, unsigned char* ptr, int hdr) {
	TapeBlock block;
	if (hdr) {
		unsigned char hdbuf[19];
		hdbuf[0] = tp & 0xff;						// type (0:basic, 3:code)
		memset(hdbuf + 1,' ',10);
		memcpy(hdbuf + 1, nm, (strlen(nm) < 10) ? strlen(nm) : 10);
		if (tp == 0) {
			hdbuf[11] = st & 0xff; hdbuf[12] = ((st & 0xff00) >> 8);
			hdbuf[13] = as & 0xff; hdbuf[14] = ((as & 0xff00) >> 8);
			hdbuf[15] = ln & 0xff; hdbuf[16] = ((ln & 0xff00) >> 8);
		} else {
			hdbuf[11] = ln & 0xff; hdbuf[12] = ((ln & 0xff00) >> 8);
			hdbuf[13] = st & 0xff; hdbuf[14] = ((st & 0xff00) >> 8);
			hdbuf[15] = as & 0xff; hdbuf[16] = ((as & 0xff00) >> 8);
		}
		block = makeTapeBlock(hdbuf,17,1);
		tap_add_block(tap,block);
		blkClear(&block);
	}
	block = makeTapeBlock(ptr,ln,0);
	tap_add_block(tap,block);
	blkClear(&block);
}

void tap_add_block(Tape* tap, TapeBlock block) {
	if (block.sigCount == 0) return;
	TapeBlock blk = block;
	strcpy(blk.text, tap->blkText);
	blk.data = malloc(blk.sigCount * sizeof(TapeSignal));
	memcpy(blk.data, block.data, blk.sigCount * sizeof(TapeSignal));

	long long total = 0;
	for (int i = 0; i < blk.sigCount; i++)
		total += blk.data[i].size;		// total time (ticks)
	blk.time = (int)(total / TAPTPS);		// seconds

	tap->blkCount++;
	tap->blkData = (TapeBlock*)realloc(tap->blkData,tap->blkCount * sizeof(TapeBlock));
	tap->blkData[tap->blkCount - 1] = blk;

	tap->newBlock = 1;
}

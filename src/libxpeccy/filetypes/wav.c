#include <stdlib.h>
#include <string.h>
#include "filetypes.h"

// A wav is a recording of a tape, not an image of one: the samples are turned
// into level changes, the level changes into pulses, and a run of pulses that
// looks like a standard ZX block is decoded back into its bytes. What does not
// decode is kept as the pulses themselves.

#define WAV_END_PAUSE	TAPTPS			// pause put after the last block
#define WAV_MIN_PAUSE	(TAPTPS / 10)		// shortest pause a block is given
#define WAV_FRAMES	4096
#define WAV_PULSE_STEP	0x10000
#define WAV_PILOT_MIN	64			// alike pulses in a row to call it a pilot tone
#define WAV_PILOT_LO	(PILOTLEN * 3 / 5)	// a wide speed error either side of the pilot
#define WAV_PILOT_HI	(PILOTLEN * 8 / 5)
#define WAV_MIN_PULSES	32			// fewer signal pulses than this is noise, not a block
#define WAV_SIG_MAX	6000			// longer than any zx pulse: tape noise, not a signal
#define WAV_DATA_MAX	0x20000
#define WAV_EXP_SMP	4			// samples the shortest pulse must get on export

typedef struct {
	int format;		// 1 = pcm, 3 = float
	int chans;
	int rate;
	int bits;
	long pos;		// where the data chunk starts
	unsigned int size;	// its size in bytes
} wavFile;

// walk the riff chunks. The fmt chunk is not always 16 bytes and data does not
// always follow it, so neither can be read as a fixed header.
static int wav_head(FILE* file, wavFile* wf) {
	char id[4];
	unsigned int csize;
	long next;
	int got = 0;
	if ((fread(id, 1, 4, file) != 4) || strncmp(id, "RIFF", 4)) return ERR_WAV_HEAD;
	fgeti(file);						// riff size: the chunks are walked instead
	if ((fread(id, 1, 4, file) != 4) || strncmp(id, "WAVE", 4)) return ERR_WAV_HEAD;
	while (fread(id, 1, 4, file) == 4) {
		csize = (unsigned int)fgeti(file);
		next = ftell(file) + csize + (csize & 1);
		if (!strncmp(id, "fmt ", 4)) {
			if (csize < 16) return ERR_WAV_HEAD;
			wf->format = fgetw(file);
			wf->chans = fgetw(file);
			wf->rate = fgeti(file);
			fseek(file, 6, SEEK_CUR);			// byte rate, block align
			wf->bits = fgetw(file);
			if ((wf->format == 0xfffe) && (csize >= 40)) {	// extensible: the real format heads the guid
				fseek(file, 8, SEEK_CUR);
				wf->format = fgetw(file);
			}
			got = 1;
		} else if (!strncmp(id, "data", 4)) {
			if (!got) return ERR_WAV_HEAD;
			wf->pos = ftell(file);
			wf->size = csize;
			return ERR_OK;
		}
		fseek(file, next, SEEK_SET);
	}
	return ERR_WAV_HEAD;
}

// one sample, brought to the 16 bit signed range
static int wav_sample(const unsigned char* p, int bits, int format) {
	int val;
	float flt;
	switch (bits) {
		case 8:
			return ((int)p[0] - 128) << 8;		// 8 bit wav is the only unsigned one
		case 16:
			return (short)(p[0] | (p[1] << 8));
		case 24:
			val = p[0] | (p[1] << 8) | (p[2] << 16);
			if (val & 0x800000) val -= 0x1000000;
			return val >> 8;
		case 32:
			val = (int)((unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24));
			if (format == 3) {
				memcpy(&flt, &val, 4);
				return (int)(flt * 32767);
			}
			return val / 0x10000;
	}
	return 0;
}

// pulses -> the bytes of the next standard block, 0 when there is none left.
// Everything is measured against the pilot pulse this recording actually has,
// so a tape running off speed decodes the same. *bstart/*bend get the pulses
// the block takes.
static int wav_find_block(int* plen, int cnt, int from, unsigned char* buf, int* bstart, int* bend) {
	int i = from;
	int j, k, pend, dur;
	int pilot, lo, hi, thr, top;
	long long sum;
	int bits, byte, len, end;
	unsigned char crc;
	while (i + WAV_PILOT_MIN <= cnt) {
		// a run of pulses alike: the pilot tone
		if ((plen[i] < WAV_PILOT_LO) || (plen[i] > WAV_PILOT_HI)) {
			i++;
			continue;
		}
		for (j = 1; j < WAV_PILOT_MIN; j++) {
			if (abs(plen[i + j] - plen[i]) * 4 > plen[i]) break;	// +-25%
		}
		if (j < WAV_PILOT_MIN) {
			i += j;
			continue;
		}
		// how far it runs, and what one of its pulses measures
		sum = 0;
		j = i;
		while ((j < cnt) && (abs(plen[j] - plen[i]) * 4 <= plen[i])) {
			sum += plen[j];
			j++;
		}
		pilot = (int)(sum / (j - i));
		pend = j;						// where to look on from if this comes to nothing
		// the two sync pulses: 667T and 735T against the pilot's 2168T
		lo = pilot * 18 / 100;
		hi = pilot * 55 / 100;
		if ((j + 2 > cnt) || (plen[j] < lo) || (plen[j] > hi) || (plen[j + 1] < lo) || (plen[j + 1] > hi)) {
			i = pend;
			continue;
		}
		j += 2;
		// data: one bit is two pulses, 855T for a 0 and 1710T for a 1
		thr = pilot * 59 / 100;					// halfway between them
		top = pilot * 95 / 100;					// longer than this is not a bit any more
		bits = 0;
		byte = 0;
		len = 0;
		end = j;
		while ((j < cnt) && (plen[j] <= top) && (len < WAV_DATA_MAX)) {
			dur = plen[j++];
			// the last pulse of a block has no partner: what bounds it is the
			// silence that follows, and that is not a pulse of its own
			if ((j < cnt) && (plen[j] <= top))
				dur = (dur + plen[j++]) / 2;
			byte = (byte << 1) | ((dur > thr) ? 1 : 0);
			if (++bits == 8) {
				buf[len++] = byte & 0xff;
				bits = 0;
				byte = 0;
				end = j;				// the block ends on a whole byte
			}
		}
		crc = 0;
		for (k = 0; k < len; k++)
			crc ^= buf[k];
		// the last byte is the xor of the rest: the one check that this really
		// was a block and not a loader that happens to look like one
		if ((len >= 3) && !crc) {
			*bstart = i;
			*bend = end;
			return len;
		}
		i = pend;
	}
	return 0;
}

// pulses that decoded into nothing are kept as they are, cut at the pauses so
// the tape map still has a row per thing on the tape
static void wav_add_custom(Tape* tap, int* plen, int from, int to) {
	TapeBlock blk;
	int i = from;
	int j, sig;
	while (i < to) {
		while ((i < to) && (plen[i] > TAPE_PAUSE_TICKS)) i++;	// silence
		j = i;
		sig = 0;
		while ((j < to) && (plen[j] <= TAPE_PAUSE_TICKS)) {
			if (plen[j] <= WAV_SIG_MAX) sig++;
			j++;
		}
		if (sig >= WAV_MIN_PULSES) {
			memset(&blk, 0, sizeof(TapeBlock));	// blkClear leaves the signal lengths alone
			blkClear(&blk);
			blk.vol = 1;
			while (i < j)
				blkAddPulse(&blk, plen[i++], -1);
			blkAddPause(&blk, (j < to) ? plen[j] : WAV_END_PAUSE);
			tap_add_block(tap, blk);
			blkClear(&blk);
			tap->isData = 0;
		}
		i = j;
	}
}

static void wav_make_tape(Tape* tap, int* plen, int cnt) {
	TapeBlock blk;
	unsigned char* buf = (unsigned char*)malloc(WAV_DATA_MAX);
	int pos = 0;
	int bstart, bend, len, pause;
	while (pos < cnt) {
		len = wav_find_block(plen, cnt, pos, buf, &bstart, &bend);
		if (len < 1) {
			wav_add_custom(tap, plen, pos, cnt);
			break;
		}
		wav_add_custom(tap, plen, pos, bstart);
		// the gap the recording has after the block, tail pulse aside
		pause = (bend < cnt) ? plen[bend] : 0;
		if ((bend + 1 < cnt) && (plen[bend + 1] > pause)) pause = plen[bend + 1];
		if (pause < WAV_MIN_PAUSE) pause = WAV_MIN_PAUSE;
		blk = tapDataToBlock((char*)buf, len, NULL);	// rebuilt at standard timings, so it can be saved as tap
		blkAddPause(&blk, pause);
		tap_add_block(tap, blk);
		blkClear(&blk);
		pos = bend;
	}
	free(buf);
}

int loadWAV(Computer* comp, const char* name, int drv) {
	Tape* tap = comp->tape;
	FILE* file = fopen(name, "rb");
	if (!file) return ERR_CANT_OPEN;
	wavFile wf;
	memset(&wf, 0, sizeof(wavFile));
	int err = wav_head(file, &wf);
	if (err == ERR_OK) {
		int ok = ((wf.format == 1) && !(wf.bits & 7) && (wf.bits >= 8) && (wf.bits <= 32))
			|| ((wf.format == 3) && (wf.bits == 32));
		if (!ok || (wf.chans < 1) || (wf.chans > 8) || (wf.rate < 4000) || (wf.rate > 192000))
			err = ERR_WAV_FORMAT;
	}
	if (err != ERR_OK) {
		fclose(file);
		return err;
	}

	int ssz = wf.bits >> 3;					// bytes per sample
	int fsz = ssz * wf.chans;				// bytes per frame
	long long frames = wf.size / fsz;
	long long nsPerSec = (long long)wf.rate * TAPTICKNS;
	unsigned char* rbuf = (unsigned char*)malloc(WAV_FRAMES * fsz);
	int pmax = WAV_PULSE_STEP;
	int* plen = (int*)malloc(pmax * sizeof(int));
	int pcnt = 0;
	TapeEdge det;
	long long pos = 0;					// samples read
	long long tick;
	long long last = 0;					// tick of the last level change
	int i, c, want, got, amp;

	tapEject(tap);
	tap->isData = 1;
	tape_edge_reset(&det, wf.rate);
	fseek(file, wf.pos, SEEK_SET);
	while (frames > 0) {
		want = (frames > WAV_FRAMES) ? WAV_FRAMES : (int)frames;
		got = fread(rbuf, fsz, want, file);
		if (got < 1) break;
		frames -= got;
		for (i = 0; i < got; i++) {
			amp = 0;
			for (c = 0; c < wf.chans; c++)
				amp += wav_sample(rbuf + i * fsz + c * ssz, wf.bits, wf.format);
			amp /= wf.chans;
			pos++;
			if (!tape_edge_step(&det, amp)) continue;
			// the tick a sample sits at is counted from its number, not added
			// up per sample: a sample is not a whole number of ticks
			tick = pos * 1000000000LL / nsPerSec;
			if (pcnt >= pmax) {
				pmax *= 2;
				plen = (int*)realloc(plen, pmax * sizeof(int));
			}
			plen[pcnt++] = (int)(tick - last);
			last = tick;
		}
	}
	// the whole recording is read first: blocks on a real tape follow each
	// other with gaps far shorter than a pause, so the pulses cannot be cut
	// into blocks as they come
	wav_make_tape(tap, plen, pcnt);

	free(plen);
	free(rbuf);
	fclose(file);
	tape_set_path(tap, name);
	return ERR_OK;
}

// --- export ---

// The wav is a recording of the tape, so every pulse becomes a run of samples.
// The run boundaries are counted from the tape position in ticks, not from the
// length of the pulse, so nothing is rounded twice and the tape keeps its
// length: the average of the rounded runs comes out exact.

static const int wav_rates[] = {44100, 48000, 96000, 192000, 0};

// A pulse length in a tape image is a count of T states at the standard
// 3.5 MHz, whatever clock the machine now playing it runs at - a 128K would
// make the same image come out 1.3 per cent shorter. The export counts in
// those, so one image always gives one file.
#define WAV_TPS	3500000.0

wavExport wav_export_default(void) {
	wavExport o;
	o.rate = 0;		// pick one from the tape
	o.bits = 16;
	o.level = 90;
	o.lead = 500;
	o.tail = 1000;
	return o;
}

// a pause is marked by a volume at the middle (see blkAddPause); the level it
// still carries is bit 7, the same as a pulse
static int wav_pause(int vol) { return (vol == 0x80) || (vol == 0x7f); }
static int wav_lev(int vol) { return (vol & 0x80) ? 1 : -1; }

// the rate the shortest pulse on the tape needs to keep its shape. A standard
// block asks for nothing above 44100, a tzx direct recording for much more.
static int wav_pick_rate(Tape* tap) {
	unsigned int least = 0;
	int i, j;
	for (i = 0; i < tap->blkCount; i++) {
		TapeBlock* blk = &tap->blkData[i];
		for (j = 0; j < blk->sigCount; j++) {
			if (wav_pause(blk->data[j].vol)) continue;
			if (!least || (blk->data[j].size < least))
				least = blk->data[j].size;
		}
	}
	if (!least) return wav_rates[0];
	double need = WAV_EXP_SMP * WAV_TPS / least;
	for (i = 0; wav_rates[i]; i++) {
		if (wav_rates[i] >= need) return wav_rates[i];
	}
	return wav_rates[i-1];
}

static void wav_put(FILE* file, int lev, int amp, int bits, int count) {
	int i;
	if (bits == 8) {
		int v = 0x80 + lev * amp;
		for (i = 0; i < count; i++) fputc(v & 0xff, file);
	} else {
		int v = lev * amp;
		for (i = 0; i < count; i++) {
			fputc(v & 0xff, file);
			fputc((v >> 8) & 0xff, file);
		}
	}
}

// what Auto would choose for the tape in the machine, so the gui can say so
int wav_export_rate(Computer* comp) {
	if (!comp || !comp->tape || (comp->tape->blkCount < 1)) return wav_rates[0];
	return wav_pick_rate(comp->tape);
}

int saveWAVopt(Computer* comp, const char* name, wavExport* opt) {
	Tape* tap = comp->tape;
	wavExport o;
	if (tap->blkCount < 1) return ERR_TAP_EMPTY;
	o = opt ? *opt : wav_export_default();
	if ((o.bits != 8) && (o.bits != 16)) o.bits = 16;
	if (o.level < 1) o.level = 1;
	if (o.level > 100) o.level = 100;
	if (o.lead < 0) o.lead = 0;
	if (o.tail < 0) o.tail = 0;
	if (o.rate < 1) o.rate = wav_pick_rate(tap);
	FILE* file = fopen(name, "wb");
	if (!file) return ERR_CANT_OPEN;

	wavHead hd;
	memcpy(hd.chunkId, "RIFF", 4);
	memcpy(hd.format, "WAVE", 4);
	memcpy(hd.subchunk1Id, "fmt ", 4);
	hd.subchunk1Size = 16;
	hd.audioFormat = 1;
	hd.numChannels = 1;
	hd.sampleRate = o.rate;
	hd.bitsPerSample = o.bits;
	hd.blockAlign = o.bits / 8;
	hd.byteRate = o.rate * hd.blockAlign;
	memcpy(hd.subchunk2Id, "data", 4);
	// neither size is known until the samples are written: both are filled
	// in below
	hd.chunkSize = 0;
	hd.subchunk2Size = 0;
	fwrite((char*)&hd, sizeof(wavHead), 1, file);

	int amp = ((o.bits == 8) ? 127 : 32767) * o.level / 100;
	double spt = o.rate / WAV_TPS;		// samples in one tape tick
	long long done = 0;			// samples written so far
	int i, j, n;

	n = (int)((long long)o.rate * o.lead / 1000);
	wav_put(file, 0, amp, o.bits, n);
	done += n;
	double acc = (double)done;
	for (i = 0; i < tap->blkCount; i++) {
		TapeBlock* blk = &tap->blkData[i];
		for (j = 0; j < blk->sigCount; j++) {
			acc += blk->data[j].size * spt;
			n = (int)(acc - done + 0.5);
			if (n < 1) n = 1;		// a pulse shorter than a sample still has to be one
			if (wav_pause(blk->data[j].vol)) {
				// a pause is silence, but it carries the level change that
				// closes the pulse before it: written as silence alone that
				// edge would be a move to the middle, which a loader is not
				// bound to see. It is played out first, then the silence.
				int edge = o.rate / 1000;
				if (edge > n) edge = n;
				wav_put(file, wav_lev(blk->data[j].vol), amp, o.bits, edge);
				wav_put(file, 0, amp, o.bits, n - edge);
			} else {
				wav_put(file, wav_lev(blk->data[j].vol), amp, o.bits, n);
			}
			done += n;
		}
		acc = (double)done;
	}
	n = (int)((long long)o.rate * o.tail / 1000);
	wav_put(file, 0, amp, o.bits, n);
	done += n;

	long long sz = done * hd.blockAlign;
	fseek(file, 4, SEEK_SET);			// riff size: everything after it
	fputi((int)(sz + sizeof(wavHead) - 8), file);
	fseek(file, sizeof(wavHead) - 4, SEEK_SET);	// data size
	fputi((int)sz, file);
	fclose(file);
	return ERR_OK;
}

int saveWAV(Computer* comp, const char* name, int drv) {
	return saveWAVopt(comp, name, NULL);
}

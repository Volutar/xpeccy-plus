
#include <stdio.h>
#include "../xlog.h"
#include <zlib.h>

#include "filetypes.h"

#pragma pack (push, 1)

typedef struct {
	char sign[4];
	unsigned char major;
	unsigned char minor;
	int flags;
} rzxHead;

typedef struct {
	int flag;
	char ext[4];
	int usl;
} rzxSnap;

typedef struct {
	int fCount;
	char byte09;
	int tStart;
	int flags;
} rzxFrm;

#pragma pack (pop)

// new

static char* msgRzxStop = " RZX playback end ";
static unsigned rzx_serial = 0;		// tells one playback from the next, see rzx_playing

void rzx_reread(Computer* comp) {
	FILE* file = comp->rzx.file;
	if (!comp->rzx.play || !file) return;
	if (comp->rzx.frm.size > 0) {
		x_fseek(file, comp->rzx.dataAt, SEEK_SET);
		fread(comp->rzx.frm.data, comp->rzx.frm.size, 1, file);
	}
	x_fseek(file, comp->rzx.next, SEEK_SET);
}

// A frame: its fetches, its IN count, then the bytes - none for a repeat of the
// last frame's (0xffff). Where it stands in the file is kept for rzx_reread().
static void rzx_frame_in(Computer* comp) {
	FILE* file = comp->rzx.file;
	comp->rzx.frm.fetches = fgetw(file);
	int size = fgetw(file);
	if (size != 0xffff) {
		comp->rzx.frm.size = size;
		comp->rzx.dataAt = x_ftell(file);
		if (size > 0)
			fread(comp->rzx.frm.data, size, 1, file);
	}
	comp->rzx.frm.pos = 0;
	comp->rzx.next = x_ftell(file);
}

void rzxGetFrame(Computer* comp) {
	int type;
	int len;
	int work;
	size_t pos;
	if (!comp->rzx.file) {
		rzxStop(comp);
	} else {
		if (comp->rzx.fCount > 0) {
			rzx_frame_in(comp);
		} else {
			work = 1;
			while (work) {
				type = fgetc(comp->rzx.file) & 0xff;
				len = fgeti(comp->rzx.file);
				pos = ftell(comp->rzx.file);
				switch (type) {
					case 0x80:					// IN block
						comp->rzx.fCount = fgeti(comp->rzx.file);		// +0 frame count
						fgeti(comp->rzx.file);					// +4 start Tstate
						rzx_frame_in(comp);					// +8.. frames
						work = 0;
						break;
					case 0x30:					// TODO: snapshot
						type = fgetc(comp->rzx.file);
						comp->rzx.noint = (type & 0x80) ? 1 : 0;
						switch(type & 0x3f) {
							case 0x00:
								loadSNA_f(comp, comp->rzx.file, len - 1);
								fseek(comp->rzx.file, pos + len, SEEK_SET);
								break;
							case 0x01:
								if (loadZ80_f(comp, comp->rzx.file) == ERR_OK) {	// bad loading?
									fseek(comp->rzx.file, pos + len, SEEK_SET);
								} else {
									rzxStop(comp);
									work = 0;
								}
								break;
							case 0x02: {			// what Fuse records with
								unsigned char* snap = (len > 1) ? (unsigned char*)malloc(len - 1) : NULL;
								int ok = snap && (fread(snap, len - 1, 1, comp->rzx.file) == 1)
									&& (loadSZX_buf(comp, snap, len - 1) == ERR_OK);
								free(snap);
								if (ok) {
									fseek(comp->rzx.file, pos + len, SEEK_SET);
								} else {
									rzxStop(comp);
									work = 0;
								}
								break;
							}
							default:
								xlog(XLG_FILE, XLL_WARN, "unknown snapshot type");
								rzxStop(comp);
								work = 0;
								// fseek(comp->rzx.file, len - 1, SEEK_CUR);
								break;
						}
						break;
					case 0xff:					// EOF
						rzxStop(comp);
						comp->msg = msgRzxStop;
						work = 0;
						break;
					default:
						fseek(comp->rzx.file, len, SEEK_CUR);	// skip (len) bytes
						break;

				}
			}
		}
	}
}

int inflateToFile(char* buf, int len, FILE* file) {
	int err = ERR_OK;
	z_stream strm;
	char* obuf = malloc(0x4000);
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.next_in = (unsigned char*)buf;
	strm.avail_in = len;
	if (inflateInit(&strm) != Z_OK) {
		err = ERR_RZX_UNPACK;
	} else {
		do {
			strm.next_out = (unsigned char*)obuf;
			strm.avail_out = 0x4000;
			err = inflate(&strm, Z_NO_FLUSH);
			if ((err == Z_OK) || (err == Z_STREAM_END)) {
				fwrite(obuf, 0x4000 - strm.avail_out, 1, file);
			}
		} while (err == Z_OK);
		inflateEnd(&strm);
		err = (err == Z_STREAM_END) ? ERR_OK : ERR_RZX_UNPACK;
	}
	free(obuf);
	return err;
}

int rzxGetSnapType(char* ext) {
	int res = 0xff;
	if (!strncmp(ext, "sna", 3) || !strncmp(ext, "SNA", 3)) {
		res = 0;
	} else if (!strncmp(ext, "z80", 3) || !strncmp(ext, "Z80", 3)) {
		res = 1;
	} else if (!strncmp(ext, "szx", 3) || !strncmp(ext, "SZX", 3)) {
		res = 2;
	}
	return res;
}

// Enough of a deflated snapshot to read its header, and no more.
static int rzx_head_unpack(FILE* file, int insize, unsigned char* dst, int dstsize) {
	z_stream zs;
	unsigned char in[1024];
	int rd = (insize > (int)sizeof(in)) ? (int)sizeof(in) : insize;
	if (rd <= 0) return 0;
	rd = fread(in, 1, rd, file);
	if (rd <= 0) return 0;
	memset(&zs, 0, sizeof(z_stream));
	if (inflateInit(&zs) != Z_OK) return 0;
	zs.next_in = in;
	zs.avail_in = rd;
	zs.next_out = dst;
	zs.avail_out = dstsize;
	inflate(&zs, Z_NO_FLUSH);		// a truncated stream is what we asked for
	rd = dstsize - zs.avail_out;
	inflateEnd(&zs);
	return rd;
}

static FILE* rzx_open_beside(const char*, const char*);

static int rzx_snap_hardware(int type, int usl, const unsigned char* head, int n) {
	if (n <= 0) return SNAP_HW_UNKNOWN;
	switch (type) {
		case 0: return sna_hardware_of(usl);		// the size says it
		case 1: return z80_hardware_of(head, n);
		case 2: return szx_hardware_of(head, n);
	}
	return SNAP_HW_UNKNOWN;
}

static rzxBlock* rzx_info_add(rzxInfo* inf) {
	rzxBlock* blk = (rzxBlock*)realloc(inf->blk, (inf->count + 1) * sizeof(rzxBlock));
	if (!blk) return NULL;
	inf->blk = blk;
	blk += inf->count++;
	memset(blk, 0, sizeof(rzxBlock));
	return blk;
}

// The block list of a recording, read without unpacking anything but the
// head of each snapshot; with first, up to the first snapshot only. A block
// whose length runs past the end of the file ends the walk, and what is left
// is counted as junk.
int rzx_info(const char* name, rzxInfo* inf, int first) {
	memset(inf, 0, sizeof(rzxInfo));
	FILE* file = fopen(name, "rb");
	if (!file) return ERR_CANT_OPEN;
	long long size = (long long)fgetSize(file);
	rzxHead hd;
	if ((fread(&hd, sizeof(rzxHead), 1, file) != 1) || strncmp(hd.sign, "RZX!", 4)) {
		fclose(file);
		return ERR_RZX_SIGN;
	}
	inf->major = hd.major;
	inf->minor = hd.minor;
	inf->flags = swap32(hd.flags);
	unsigned char head[0x400];
	rzxSnap shd;
	FILE* ext;
	int n;
	long long at;
	while ((at = x_ftell(file)) < size) {
		int id = fgetc(file);
		unsigned int len = (unsigned int)fgeti(file);
		if (feof(file) || (len < 5) || (at + len > size)) {
			inf->junk = (int)(size - at);
			break;
		}
		rzxBlock* blk = rzx_info_add(inf);
		if (!blk) break;
		blk->id = id;
		blk->size = len;
		blk->frame = inf->frames;
		switch (id) {
			case 0x10:
				if (len >= 29) {
					fread(blk->text, 20, 1, file);
					blk->text[20] = 0;
					blk->major = fgetw(file);
					blk->minor = fgetw(file);
					n = (int)len - 29;
					if (n >= (int)sizeof(blk->custom)) n = sizeof(blk->custom) - 1;
					if (n > 0) fread(blk->custom, n, 1, file);
					blk->custom[n > 0 ? n : 0] = 0;
				}
				break;
			case 0x30:
				if ((len < 17) || (fread(&shd, sizeof(rzxSnap), 1, file) != 1)) break;
				blk->flags = swap32(shd.flag);
				blk->usl = swap32(shd.usl);
				memcpy(blk->ext, shd.ext, 4);
				blk->ext[4] = 0;
				n = 0;
				if (blk->flags & 1) {			// b0: a file of its own, by name
					fgeti(file);			// checksum
					n = (len > 21) ? (int)len - 21 : 0;
					if (n >= (int)sizeof(blk->text)) n = sizeof(blk->text) - 1;
					fread(blk->text, n, 1, file);
					blk->text[n] = 0;
					n = 0;
					ext = rzx_open_beside(name, blk->text);
					if (ext) {
						n = (int)fread(head, 1, sizeof(head), ext);
						if (!blk->usl) blk->usl = (int)fgetSize(ext);
						fclose(ext);
					}
				} else if (blk->flags & 2) {
					n = rzx_head_unpack(file, len - 17, head, sizeof(head));
				} else {
					n = (int)fread(head, 1, sizeof(head), file);
				}
				blk->hw = rzx_snap_hardware(rzxGetSnapType(blk->ext), blk->usl, head, n);
				inf->snaps++;
				break;
			case 0x80:
				if (len < 18) break;
				blk->frames = fgeti(file);
				fgetc(file);
				blk->tstart = fgeti(file);
				blk->flags = fgeti(file);
				inf->frames += blk->frames;
				break;
		}
		if (first && (id == 0x30)) break;
		x_fseek(file, at + len, SEEK_SET);
	}
	fclose(file);
	return ERR_OK;
}

void rzx_info_free(rzxInfo* inf) {
	free(inf->blk);
	inf->blk = NULL;
	inf->count = 0;
}

// What the recording was made on. It is the hardware of the snapshot playback
// starts from, and unlike a snapshot on its own it has to be that machine
// exactly - see snapHwIs().
int rzxGetHardware(const char* name) {
	rzxInfo inf;
	int res = SNAP_HW_UNKNOWN;
	if ((rzx_info(name, &inf, 1) == ERR_OK) && inf.snaps)
		res = inf.blk[inf.count - 1].hw;	// playback starts from the first one
	rzx_info_free(&inf);
	return res;
}

// The snapshot a recording names instead of carrying: as written, else beside
// the recording, the way the two are kept together.
static FILE* rzx_open_beside(const char* rzx, const char* snap) {
	FILE* file = fopen(snap, "rb");
	if (file) return file;
	const char* base = snap;
	const char* p;
	for (p = snap; *p; p++)
		if ((*p == '/') || (*p == '\\') || (*p == ':')) base = p + 1;
	size_t dir = 0;
	for (p = rzx; *p; p++)
		if ((*p == '/') || (*p == '\\')) dir = (size_t)(p - rzx) + 1;
	char* path = (char*)malloc(dir + strlen(base) + 1);
	if (!path) return NULL;
	memcpy(path, rzx, dir);
	strcpy(path + dir, base);
	file = fopen(path, "rb");
	free(path);
	return file;
}

int loadRZX(Computer* comp, const char* name, int drv) {
	int err = ERR_OK;
	if (comp->rzx.file) rzxStop(comp);
	comp->rzx.play = 0;
	comp->rzx.fTotal = 0;
	FILE* file = fopen(name, "rb");
	int type;
	int len;
	rzxSnap shd;
	rzxFrm fhd;
	FILE* sfile;
	long long recpos = 0;
	long long recend;
	char* buf = NULL;
	char* obuf = malloc(0x4000);
	if (!file) {
		err = ERR_CANT_OPEN;
	} else {
		rzxHead hd;
		fread(&hd, sizeof(rzxHead), 1, file);
		hd.flags = swap32(hd.flags);
		if (strncmp(hd.sign,"RZX!",4)) {
			err = ERR_RZX_SIGN;
		} else {
			xlog(XLG_FILE, XLL_DEBUG, "RZX ver %i.%i",hd.major,hd.minor);
			comp->rzx.file = fopen_tmp();
			rzx_playing = comp->rzx.file ? ++rzx_serial : 0;
			if (!comp->rzx.file) {
				err = ERR_CANT_OPEN;
			} else {
				err = ERR_OK;
				while (!feof(file) && (err == ERR_OK)) {
					type = fgetc(file) & 0xff;
					len = fgeti(file);
					if (feof(file)) break;
					switch (type) {
						case 0x30:
							fread(&shd, sizeof(rzxSnap), 1, file);
							shd.flag = swap32(shd.flag);
							shd.usl = swap32(shd.usl);
							if (shd.flag & 1) {		// external
								fgeti(file);	// checksum
								buf = realloc(buf, len - 20);
								memset(buf, 0x00, len - 20);
								fread(buf, len - 21, 1, file);
								sfile = rzx_open_beside(name, buf);
								if (sfile) {
									len = fgetSize(sfile);
									fputc(0x30, comp->rzx.file);
									fputi(len + 1, comp->rzx.file);
									fputc(rzxGetSnapType(shd.ext) | ((shd.flag & RZX_SNAP_NOINT) ? 0x80 : 0) | ((shd.flag & RZX_SNAP_MARK) ? 0x40 : 0), comp->rzx.file);
									while (len > 0) {
										fread(obuf, 0x4000, 1, sfile);
										fwrite(obuf, (len > 0x4000) ? 0x4000 : len, 1, comp->rzx.file);
										len -= 0x4000;
									}
									fclose(sfile);
								} else {
									err = ERR_CANT_OPEN;
								}
							} else if (shd.flag & 2) {	// compressed
								fputc(0x30, comp->rzx.file);
								fputi(shd.usl + 1, comp->rzx.file);
								fputc(rzxGetSnapType(shd.ext) | ((shd.flag & RZX_SNAP_NOINT) ? 0x80 : 0) | ((shd.flag & RZX_SNAP_MARK) ? 0x40 : 0), comp->rzx.file);
								buf = realloc(buf, len - 17);
								fread(buf, len - 17, 1, file);
								err = inflateToFile(buf, len - 17, comp->rzx.file);
							} else {			// not compressed
								buf = realloc(buf, shd.usl);
								fread(buf, shd.usl, 1, file);
								fputc(0x30, comp->rzx.file);
								fputi(shd.usl + 1, comp->rzx.file);
								fputc(rzxGetSnapType(shd.ext) | ((shd.flag & RZX_SNAP_NOINT) ? 0x80 : 0) | ((shd.flag & RZX_SNAP_MARK) ? 0x40 : 0), comp->rzx.file);
								fwrite(buf, shd.usl, 1, comp->rzx.file);
							}
							break;
						case 0x80:
							//fread(&fhd, sizeof(rzxFrm), 1, file);
							//fhd.fCount = swap32(fhd.fCount);
							//fhd.tStart = swap32(fhd.tStart);
							//fhd.flags = swap32(fhd.flags);
							fhd.fCount = fgeti(file);	// +0 frames in block
							fhd.byte09 = fgetc(file);	// +4 skip 1 byte
							fhd.tStart = fgeti(file);	// +5 T state @ start
							fhd.flags = fgeti(file);	// +9 flags
							comp->rzx.fTotal += fhd.fCount;
							recpos = x_ftell(comp->rzx.file);
							fputc(0x80, comp->rzx.file);
							fputi(0, comp->rzx.file);		// the length, once it is known
							fputi(fhd.fCount, comp->rzx.file);
							fputi(fhd.tStart, comp->rzx.file);
							// fputw(fhd.tStart, comp->rzx.file);
							if (fhd.flags & 1) {			// crypted
								err = ERR_RZX_CRYPT;
							} else if (fhd.flags & 2) {		// packed
								buf = realloc(buf, len - 18);
								fread(buf, len - 18, 1, file);
								err = inflateToFile(buf, len - 18, comp->rzx.file);
							} else {				// raw
								buf = realloc(buf, len - 18);
								fread(buf, len - 18, 1, file);
								fwrite(buf, len - 18, 1, comp->rzx.file);
							}
							// a seek steps over the block by it
							recend = x_ftell(comp->rzx.file);
							x_fseek(comp->rzx.file, recpos + 1, SEEK_SET);
							fputi((int)(recend - recpos - 5), comp->rzx.file);
							x_fseek(comp->rzx.file, recend, SEEK_SET);
							break;
						default:
							fseek(file, len - 5, SEEK_CUR);
							break;
					}
				}
				if (err == ERR_OK) {
					fputc(0xff, comp->rzx.file);
					rewind(comp->rzx.file);
					comp->rzx.start = 1;
					comp->rzx.play = 0;
				} else {
					rzxStop(comp);
				}
			}
		}
		fclose(file);
	}
	free(buf);
	free(obuf);
	return err;
}

// Plays from the snapshot nearest before the frame, or from where playback
// stands if that is nearer. A snapshot is loaded just as playback reaching it
// would load it, the INT of one after input included. Returns the frame it
// stands at, which the caller runs on from; -1 when nothing plays.
int rzx_seek(Computer* comp, int frame) {
	FILE* file = comp->rzx.file;
	if (!comp->rzx.play || !file) return -1;
	long long here = x_ftell(file);
	long long best = -1;
	long long pos;
	int bestFrame = 0;
	int bestFirst = 0;
	int done = 0;
	int input = 0;
	int type, len;
	rewind(file);
	while (1) {
		pos = x_ftell(file);
		type = fgetc(file);
		if ((type == EOF) || (type == 0xff)) break;
		len = fgeti(file);
		if (feof(file)) break;
		if (type == 0x30) {
			if (done > frame) break;
			best = pos;
			bestFrame = done;
			bestFirst = !input;
		} else if (type == 0x80) {
			done += fgeti(file);
			input = 1;
		}
		x_fseek(file, pos + 5 + len, SEEK_SET);
	}
	if ((best < 0) || ((frame >= comp->rzx.fCurrent) && (comp->rzx.fCurrent >= bestFrame))) {
		x_fseek(file, here, SEEK_SET);
		return comp->rzx.fCurrent;
	}
	x_fseek(file, best, SEEK_SET);
	if (bestFirst) {			// as playback starts
		comp->rzx.fCount = 0;
		comp->rzx.fCurrent = 0;
		rzxGetFrame(comp);
		comp->rzx.noint = 0;
	} else {				// as the frame before it ends
		comp->rzx.fCount = 1;
		comp->rzx.fCurrent = bestFrame - 1;
		comp->rzx.frm.fetches = 0;
		vid_unlazy(comp->vid);
		comp->hw->irq(comp, IRQ_RZX_INT);
	}
	return comp->rzx.play ? comp->rzx.fCurrent : -1;
}


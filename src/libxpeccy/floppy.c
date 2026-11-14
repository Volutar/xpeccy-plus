#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "floppy.h"

static int interleave = 8;		// 1,9,2,10...: the order TR-DOS formats in

Floppy* flpCreate(int id, cbflpirq cb, void* p) {
	Floppy* flp = (Floppy*)malloc(sizeof(Floppy));
	memset(flp,0x00,sizeof(Floppy));
	flp->data = (flpTrack*)calloc(256, sizeof(flpTrack));
	flp->id = id;
	flp->fitted = 1;
	flp->trk80 = 1;
	flp->doubleSide = 1;
	flp->trk = 0;
	flp->pos = 0;
	flp->path = NULL;
	flp->insert = 0;
	flp->door = 0;
	flp->dwait = 0;
	flp->xirq = cb;
	flp->xptr = p;
	flp_set_hd(flp, 0);
	return flp;
}

void flpDestroy(Floppy* flp) {
	for (int i = 0; i < 256; i++)
		flp_track_written(flp, i);
	free(flp->path);
	free(flp->data);
	free(flp);
}

void flp_track_written(Floppy* flp, int tr) {
	flpTrack* t = &flp->data[tr & 0xff];
	for (int i = 0; i < t->nweak; i++)
		free(t->weak[i].data);
	free(t->weak);
	t->weak = NULL;
	t->nweak = 0;
	free(t->raw);
	t->raw = NULL;
	t->rawlen = 0;
}

void flp_keep_raw(Floppy* flp, int tr, const unsigned char* data, int len) {
	flpTrack* t = &flp->data[tr & 0xff];
	free(t->raw);
	t->raw = (len > 0) ? (unsigned char*)malloc(len) : NULL;
	t->rawlen = t->raw ? len : 0;
	if (t->raw) memcpy(t->raw, data, len);
}

void flp_add_weak(Floppy* flp, int tr, int pos, int len, int copies, const unsigned char* data, int made) {
	flpTrack* t = &flp->data[tr & 0xff];
	if ((len < 1) || (copies < 2)) return;
	flpWeak* lst = (flpWeak*)realloc(t->weak, (t->nweak + 1) * sizeof(flpWeak));
	if (!lst) return;
	t->weak = lst;
	unsigned char* buf = (unsigned char*)malloc((size_t)len * copies);
	if (!buf) return;
	memcpy(buf, data, (size_t)len * copies);
	lst[t->nweak].pos = pos;
	lst[t->nweak].len = len;
	lst[t->nweak].copies = copies;
	lst[t->nweak].made = made;
	lst[t->nweak].data = buf;
	t->nweak++;
}

// the read of a weak sector this revolution gives, or the track's own byte
static unsigned char flp_byte(Floppy* flp, flpTrack* t) {
	for (int i = 0; i < t->nweak; i++) {
		flpWeak* w = &t->weak[i];
		int off = flp->pos - w->pos;
		if ((off >= 0) && (off < w->len))
			return w->data[(flp->revs % w->copies) * w->len + off];
	}
	return t->byte[flp->pos];
}

void flp_set_path(Floppy* flp, const char* path) {
	if (path) {
		flp->path = realloc(flp->path, strlen(path) + 1);
		strcpy(flp->path, path);
	} else {
		free(flp->path);
		flp->path = NULL;
	}
}

void flp_set_hd(Floppy* flp, int hd) {
	if (flp) flp->trklen = hd ? TRKLEN_HD : TRKLEN_DD;
}

void flp_set_interleave(int iv) {
	if ((iv > 0) && (iv < 9)) {
		interleave = iv;
	}
}

int flp_get_interleave() {
	return interleave;
}

void flpWr(Floppy* flp, int hd, unsigned char val) {
	flp->wr = 1;
	if (x_runahead) return;			// this frame is going to be rolled back
	hd &= 1;
	if (hd & !flp->doubleSide) return;	// saving on HD1 for SS Floppy
	if (flp->insert && flp->door) {
		flp->changed = 1;
		x_media_writes++;
		flpTrack* t = &flp->data[(flp->trk << 1) | hd];
		if (t->nweak || t->raw) flp_track_written(flp, (flp->trk << 1) | hd);
		t->byte[flp->pos] = val;
	}
}

unsigned char flpRd(Floppy* flp, int hd) {
	flp->rd = 1;
	hd &= 1;
	if (!(flp->insert && flp->door)) return 0x00;
	if (hd && !flp->doubleSide) return 0x00;
	flpTrack* t = &flp->data[(flp->trk << 1) | hd];
	return t->nweak ? flp_byte(flp, t) : t->byte[flp->pos];
}

// TODO: move disk spinning here (if motor is on)
void flp_sync(Floppy* flp, int ns) {
	if (flp->dwait > 0) {
		flp->dwait -= ns;
		if (flp->dwait < 0) {
			flp->dwait = 0;
			if (flp->door != flp->insert) {
				flp->xirq(IRQ_FDD_RDY, flp->xptr);
				flp->door = flp->insert;
			}
		}
	}
}

// cylinders of an image past any drive's 86 (esxDOS-style mounts, up to 128), else 0
int flp_wide(Floppy* flp) {
	if (flp->cyls <= 87) return 0;
	return (flp->cyls > 128) ? 128 : flp->cyls;
}

// A drive stops at 86 (43 on a 40-track one); a wide image takes it to its last
int flp_last_cyl(Floppy* flp) {
	int wide = flp_wide(flp);
	return wide ? (wide - 1) : (flp->trk80 ? 86 : 43);
}

void flpStep(Floppy* flp, int dir) {
	switch (dir) {
		case FLP_FORWARD:
			if (flp->trk < flp_last_cyl(flp)) flp->trk++;
			break;
		case FLP_BACK:
			if (flp->trk > 0) flp->trk--;
			break;
	}
}

int flpNext(Floppy* flp, int fdcSide) {
	int res = 0;
	int rtrk = (flp->trk << 1);
	if (flp->doubleSide && fdcSide) rtrk++;
	if (flp->insert && flp->door) {
		flp->pos++;
		if (flp->pos >= flp->trklen) {
			flp->pos = 0;
			flp->revs++;
			res = 1;
		}
		flp->index = (flp->pos < 4) ? 1 : 0;		// ~90ms index pulse
		flp->field = flp->data[rtrk].field[flp->pos] & 0x0f;
	} else {
		flp->field = 0;
	}
	return res;
}

unsigned short getCrc(unsigned char* ptr, int len) {
	int crc = 0xffff;
	unsigned char val;
	int i;
	while (len--) {
		val = *ptr;
		crc ^= val << 8;
		for (i = 0; i < 8 ; i++) {
			if ((crc *= 2) & 0x10000) crc ^= 0x1021;
			crc &= 0xffff;		// only these 16 bits go on: no overflow over a long run
		}
		ptr++;
	}
	return  (crc & 0xffff);
}

static unsigned char trk_mark[4] = {0xc1,0xc1,0xc1,0xfc};
static unsigned char hd_mark[4] = {0xa1,0xa1,0xa1,0xfe};
static unsigned char dat_mark[4] = {0xa1,0xa1,0xa1,0xfb};

unsigned char* flp_format_sec(int tr, int hd, int sc, int n, int slen, int spc, char* data, unsigned char* ptr) {
	unsigned short crc;
	memset(ptr, 0x4e, 10); ptr += 10;
	memset(ptr, 0x00, 12); ptr += 12;
	unsigned char* stp = ptr;
	memcpy(ptr, hd_mark, 4); ptr += 4;
	*(ptr++) = tr & 0xff;
	*(ptr++) = hd & 1;
	*(ptr++) = sc & 0xff;
	*(ptr++) = n & 0xff;
	crc = getCrc(stp, ptr - stp);
	*(ptr++) = (crc >> 8) & 0xff;
	*(ptr++) = crc & 0xff;
	memset(ptr, 0x4e, 28); ptr += 28;	// 22
	memset(ptr, 0x00, 12); ptr += 12;
	stp = ptr;
	memcpy(ptr, dat_mark, 4); ptr += 4;
	if (data) {
		memcpy(ptr, data, slen);
		ptr += slen;
//		data += slen;
	} else {
		memset(ptr, 0x00, slen);
		ptr += slen;
	}
	crc = getCrc(stp, ptr - stp);
	*(ptr++) = (crc >> 8) & 0xff;
	*(ptr++) = crc & 0xff;
	ptr += spc;
	return ptr;
}

int flp_format_trk_buf(int trk, int spt, int slen, int trklen, char* data, unsigned char* buf) {
	int res = 1;
	int t = 128;
	int n = 0;		// 0:128, 1:256, 2:512, 3:1024, 4:2048
	int spc;
	int tr;
	int sc;
	int hd;
	unsigned char* ptr;
	char* dptr;
	while (t < slen) {
		t <<= 1;
		n++;
	}
	if ((n > 4) || (trk > 255) || (spt < 1)) {
		res = 0;
	} else {
		ptr = buf;
		slen = t;
		spc = (trklen - 60) / spt;	// minus: last gap(var) + 1st gap(12) + 1st sync(12) + track mark(4)
		spc -= (74 + slen);		// calc gap 3 size - minus: gap(10) + sync(12) + header mark(4) + crc(2) + gap(22 std, 28 for bk) + sync(12) + data mark(4) + sector size(slen) + crc(2)
		spc &= ~1;			// make it even (for BK disk system)
		if (spc < 10) {
			res = 0;
		} else {
			hd = trk & 1;
			tr = trk >> 1;
			memset(ptr, 0x4e, trklen);
			ptr += 12;				// 1st gap
			memset(ptr, 0x00, 12); ptr += 12;	// 1st sync
			memcpy(ptr, trk_mark, 4); ptr += 4;	// track gap
			for(t = 0; t < interleave; t++) {
				dptr = data + slen * t;		// start of 1st sector in group
				for (sc = t + 1; sc <= spt; sc += interleave) {		// sector numeration starts from 1
					ptr = flp_format_sec(tr, hd, sc, n, slen, spc, dptr, ptr);
					dptr += slen * interleave;
				}
			}
		}
	}
	return res;
}

int flp_format_trk(Floppy* flp, int trk, int spt, int slen, char* data) {
	int res = 1;
	if (trk > 255) {
		res = 0;
	} else {
		res = flp_format_trk_buf(trk, spt, slen, flp->trklen, data, flp->data[trk].byte);
		if (res) flpFillFields(flp, trk, 0);
	}
	return res;
}

void flpPrev(Floppy* flp, int fdcSide) {
	int rtrk = (flp->trk << 1);
	if (flp->doubleSide && fdcSide) rtrk++;
	if (flp->insert && flp->door) {
		if (flp->pos > 0) {
			flp->pos--;
		} else {
			flp->pos = flp->trklen - 1;
		}
		flp->field = flp->data[rtrk].field[flp->pos] & 0x0f;
	} else {
		flp->field = 0;
	}
}

void flpClearTrack(Floppy* flp,int tr) {
	flp_track_written(flp, tr);
	memset(flp->data[tr].byte, 0, TRKLEN_HD);
	memset(flp->data[tr].field, 0, TRKLEN_HD);
	memset(flp->data[tr].map, 0, sizeof(flp->data[tr].map));	// no sector found there either
}

void flpClearDisk(Floppy* flp) {
	int i;
	for (i = 0; i < 256; i++) flpClearTrack(flp,i);	// all: a disk of 128 cylinders was there
	flp->cyls = 0;
}

void flpFillFields(Floppy* flp,int tr, int flag) {
	int i, bcnt = 0;
	unsigned char fld = 0;
	unsigned char* cpos = flp->data[tr].byte;
	unsigned char* bpos = cpos;
	unsigned short crc;
	unsigned char scn = 0;		// sector number (1+)
	unsigned char sct = 1;		// sector size code
	if (tr > 255) return;
	flp_track_written(flp, tr);	// the track is laid out anew
	for (i = 0; i < 256; i++) {
		flp->data[tr].map[i] = 0;
	}
	for (i = 0; i < flp->trklen; i++) {
		flp->data[tr].field[i] = fld;
		fld &= 0x0f;		// reset flags
		if (flag & 1) {
			if ((fld == 0) || (fld == 0x0f)) {
					if ((*bpos) == 0xf5) *bpos = 0xa1;
					if ((*bpos) == 0xf6) *bpos = 0xc2;
			}
		}
		if (bcnt > 0) {
			bcnt--;
			if (bcnt==0) {
				if (fld < 4) {
					if (flag & 1) {
						cpos -= 3;		// including a1,a1,a1
						crc = getCrc(cpos, bpos - cpos + 1);
						*(bpos + 1) = ((crc & 0xff00) >> 8);
						*(bpos + 2) = (crc & 0xff);
					}
					fld = 4;
					bcnt = 2;
				} else {
					fld = 0;
				}
			}
		} else {					// TODO: find A1 here, then skip until !A1, this byte will be marker
			if (flp->data[tr].byte[i] == 0xa1) {
				if (fld == 0) {
					flp->data[tr].field[i] = 0x80;
				}
				fld = 0x0f;
			} else {
				if (fld == 0x0f) {
					switch (flp->data[tr].byte[i] & 0xfe) {
						case 0xfe:			// fe/ff : IDAM, head
							cpos = bpos;
							fld = 0x01;
							bcnt = 4;
							scn = flp->data[tr].byte[i+3];
							sct = flp->data[tr].byte[i+4];
							break;
						// case 0xfc:			// fc/fd : IAM, index marker
						//	break
						case 0xfa:			// fa/fb : DAM, data
							cpos = bpos;
							fld = 0x02;
							bcnt = (128 << (sct & 3));
							if (scn > 0) {
								flp->data[tr].map[scn] = i + 1;
								scn = 0;
							}
							break;
						case 0xf8:			// f8/f9 : DDAM, deleted data
							cpos = bpos;
							fld = 0x03;
							bcnt = (128 << (sct & 3));
							if (scn > 0) {
								flp->data[tr].map[scn] = i + 1;
								scn = 0;
							}
							break;
						default:
							fld = 0;
							break;

					}
				}
			}
		}
		bpos++;
	}
}

// if path is NULL, insert new unformatted disk
// for uPD765: RDY state changed -> interrupt
void flp_insert(Floppy* flp, const char* path) {
	flp->xirq(IRQ_FDD_RDY, flp->xptr);	// rdy 1->0
	flp->insert = 1;
	flp->door = 0;
	flp->dwait = 5e8;	// door will be closing after .5 sec
	flp->changed = 0;
	if (!path) flpClearDisk(flp);
	flp_set_path(flp, path);
}

// A disk read into a Floppy of its own goes into the drive, as flp_insert puts one
// in: its tracks and name by pointer, the drive's head, motor and sides as they
// are. What the drive held goes back in disk, to be freed away from the emulation.
void flp_take_disk(Floppy* flp, Floppy* disk) {
	flpTrack* data = flp->data;
	char* path = flp->path;
	flp->data = disk->data;
	flp->path = disk->path;
	disk->data = data;
	disk->path = path;
	int cyls = flp->cyls;
	flp->cyls = disk->cyls;
	disk->cyls = cyls;
	flp->trklen = disk->trklen;
	flp->protect = disk->protect;
	flp->xirq(IRQ_FDD_RDY, flp->xptr);	// rdy 1->0
	flp->insert = 1;
	flp->door = 0;
	flp->dwait = 5e8;	// door will be closing after .5 sec
	flp->changed = disk->changed;
}

// A drive that is not there has no sensor either: a restore steps out until
// the controller gives up, which is how software finds a drive missing.
int flp_trk0(Floppy* flp) {
	return flp->fitted && (flp->trk == 0);
}

void flp_eject(Floppy* flp) {
	flp_set_path(flp, NULL);
	flp->cyls = 0;
	flp->xirq(IRQ_FDD_RDY, flp->xptr);	// rdy 1->0
	flp->door = 0;
	flp->insert = 0;
	flp->dwait = 5e8;
}

void flpGetTrack(Floppy* flp,int tr,unsigned char* dst) {
	memcpy(dst,flp->data[tr].byte,flp->trklen);
}

void flpGetTrackFields(Floppy* flp,int tr,unsigned char* dst) {
	memcpy(dst,flp->data[tr].field,flp->trklen);
}

void flpPutTrack(Floppy* flp,int tr,unsigned char* src,int len) {
	flpClearTrack(flp,tr);
	memcpy(flp->data[tr].byte,src,len);
	flpFillFields(flp,tr,0);
}

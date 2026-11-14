#include "filetypes.h"
#include "../xlog.h"

#include <string.h>

void putint(unsigned char* ptr, unsigned int val) {
	*ptr++ = val & 0xff;
	*ptr++ = (val & 0xff00) >> 8;
	*ptr++ = (val & 0xff0000) >> 16;
	*ptr++ = (val & 0xff000000) >> 24;
}

unsigned int freadLen(FILE* file, int n) {
	unsigned int len = 0;
	for (int i = 0; i < n; i++) {
		len |= ((unsigned int)(fgetc(file) & 0xff) << (i << 3));
	}
	return len;
}

// crc32 for UDI, taken from Unreal 0.32.7
int uCrc32(int crc, unsigned char *buf, unsigned len) {
	while (len--) {
		crc ^= -1 ^ *buf++;
		for(int k = 8; k--; ) {
			int temp = -(crc & 1); crc >>= 1, crc ^= 0xEDB88320 & temp;
		}
		crc ^= -1;
	}
	return crc;
}

void loadUDITrack(Floppy* flp, FILE* file, unsigned char tr, int sd) {
	int rt = (tr << 1) + (sd ? 1 : 0);
	unsigned char type = fgetc(file);
	unsigned int len;
	if (type != 0x00) {
		xlog(XLG_FILE, XLL_WARN, "TRK %i: unknown format %.2X",rt,type);
		len = freadLen(file,4);					// field len
		fseek(file, len, SEEK_CUR);				// skip unknown field
	} else {
		unsigned char* trackBuf = (unsigned char*)malloc(flp->trklen);
		len = freadLen(file,2);					// track size
		memset(trackBuf, 0x4e, flp->trklen);
		// a file cut short leaves the rest of the track as gap
		if (len > flp->trklen) {
			if (fread((char*)trackBuf, flp->trklen, 1, file) != 1)		// read only TRACKLEN bytes
				xlog(XLG_FILE, XLL_WARN, "TRK %i: cut short", rt);
			fseek(file, len - flp->trklen, SEEK_CUR);		// and skip others
			flpPutTrack(flp, rt, trackBuf, flp->trklen);
		} else {
			if (fread((char*)trackBuf, len, 1, file) != 1)
				xlog(XLG_FILE, XLL_WARN, "TRK %i: cut short", rt);
			flpPutTrack(flp, rt, trackBuf, len);
		}
		len = (len >> 3) + (((len & 7) == 0) ? 0 : 1);		// skip bit field
		fseek(file, len, SEEK_CUR);
		free(trackBuf);
	}
}

void getUDIBitField(Floppy* flp, unsigned char tr, unsigned char* buf) {
	int i;
	int msk=0x01;
	//unsigned char fieldBuf[TRKLEN_DD];
	//unsigned char trackBuf[TRKLEN_DD];
	unsigned char* fieldBuf = (unsigned char*)malloc(flp->trklen);
	unsigned char* trackBuf = (unsigned char*)malloc(flp->trklen);
	flpGetTrack(flp,tr,trackBuf);
	flpGetTrackFields(flp,tr,fieldBuf);
	*buf = 0x00;
	for (i = 0; i < flp->trklen; i++) {
		if (msk == 0x100) {
			msk = 0x01;
			*(++buf)=0x00;
		}
		if ((fieldBuf[i] == 0) && (trackBuf[i] == 0xa1)) *buf |= msk;
		msk <<= 1;
	}
	buf++;
	free(trackBuf);
	free(fieldBuf);
}

int udi_read(Floppy* flp, const char* name) {
	FILE* file = fopen(name, "rb");
	if (!file) return ERR_CANT_OPEN;
	int err = ERR_OK;
	char buf[16];
	unsigned char tmp;
	int dbSide;

	if (fread(buf, 16, 1, file) != 1) {
		err = ERR_UDI_SIGN;
	} else if (strncmp((char*)buf, "UDI!", 4) != 0) {
		err = ERR_UDI_SIGN;
	} else if (buf[8] != 0x00) {
		err = ERR_UDI_SIGN;
	} else {
		tmp = buf[9];			// max track;
		if (tmp > 127) tmp = 127;	// two sides of 128 fill the drive's 256 tracks
		dbSide = buf[10] ? 1 : 0 ;	// if double side
		for (int i = 0; i < tmp + 1; i++) {
			loadUDITrack(flp, file, i, 0);
			if (dbSide) loadUDITrack(flp, file, i, 1);
		}
		flp->cyls = tmp + 1;
		flp_insert(flp, name);
	}
	fclose(file);
	return err;
}

int loadUDI(Computer* comp, const char* name, int drv) {
	return udi_read(comp->dif->flp[drv & 3], name);
}

// The disk as a UDI file, in memory: every track as it is, so a protected disk
// comes back as it was. NULL if there is no room; free() it.
unsigned char* udi_build(Floppy* flp, size_t* len) {
	const char sign[] = "UDI!";
	int bits = (flp->trklen + 7) >> 3;	// the sync mark bit field: 782 for a DD track
	// the drive's 80 or 40 cylinders, as always, or all of an image past 86
	int cyls = flp_wide(flp);
	if (!cyls) cyls = flp->trk80 ? 80 : 40;
	// header, two tracks a cylinder of type + length + bytes + bits, crc; an HD
	// track is near three times a DD one, so the buffer is sized for this floppy
	unsigned char* img = (unsigned char*)malloc(16 + cyls * 2 * (3 + flp->trklen + bits) + 4);
	unsigned char* dptr = img;
	unsigned char* bptr;
	int i,j;
	if (!img) return NULL;

	memcpy(dptr,sign,4);
	bptr = img + 4;
	dptr += 8;
	*(dptr++) = 0x00;			// version
	*(dptr++) = cyls - 1;			// maximum track number
	*(dptr++) = flp->doubleSide ? 1 : 0;		// double side (due to floppy property)
	*(dptr++) = 0x00;
	*(dptr++) = 0x00;
	*(dptr++) = 0x00;
	*(dptr++) = 0x00;
	*(dptr++) = 0x00;
	for (i = 0; i < cyls * 2; i++) {
		*(dptr++) = 0x00;		// MFM
		*(dptr++) = (flp->trklen & 0xff);	// track len
		*(dptr++) = ((flp->trklen & 0xff00) >> 8);
		flpGetTrack(flp,i,dptr); // memcpy((char*)dptr,(char*)flp->data[i].byte,TRACKLEN);	// track image
		dptr += flp->trklen;
		getUDIBitField(flp,i,dptr);
		dptr += bits;
		if (!flp->doubleSide) i++;		// if single-side skip
	}
	i = dptr - img;
	putint(bptr, i);
	j = -1;
	j = uCrc32(j, img, i);
	xlog(XLG_FILE, XLL_DEBUG, "crc = %X",j);
	putint(dptr, j);
	dptr += 4;
	*len = dptr - img;
	return img;
}

int saveUDI(Computer* comp, const char* name, int drv) {
	Floppy* flp = comp->dif->flp[drv & 3];
	size_t len = 0;
	unsigned char* img = udi_build(flp, &len);
	if (!img) return ERR_CANT_OPEN;
	FILE* file = fopen(name, "wb");
	if (!file) {
		free(img);
		return ERR_CANT_OPEN;
	}
	fwrite((char*)img, len, 1, file);
	fclose(file);
	free(img);

	flp_set_path(flp, name);
	flp->changed = 0;
	return ERR_OK;
}

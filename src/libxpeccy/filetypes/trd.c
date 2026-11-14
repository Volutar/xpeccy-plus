#include "filetypes.h"

#define TRD_MAX 0x100000

int trd_read(Floppy* flp, const char* name) {
	FILE* file = fopen(name, "rb");
	if (!file) return ERR_CANT_OPEN;
	int err = ERR_OK;
	size_t len = fgetSize(file);
	// 1 MB is 128 cylinders: TR-DOS keeps a side-track in a byte
	if (((len & 0xff) != 0) || (len == 0) || (len > TRD_MAX)) {
		err = ERR_TRD_LEN;
	} else {
//		diskFormat(flp);
		int trk = 0;
		char buf[0x1000];
		do {
			// a last track cut short is padded with zeros. A read of nothing
			// (feof comes only after it) leaves buf as it was, as it always did
			size_t got = fread(buf, 1, 0x1000, file);
			if ((got > 0) && (got < 0x1000)) memset(buf + got, 0, 0x1000 - got);
			flp_format_trk(flp, trk, 16, 256, buf);
			trk++;
		} while  (!feof(file));
		memset(buf, 0, 0x1000);
		while (trk < 168) {
			flp_format_trk(flp, trk, 16, 256, buf);
			trk++;
		}
		// a disk read into the drive itself: nothing of a wider one left past it
		while (trk < 256)
			flpClearTrack(flp, trk++);

		flp->cyls = (int)((len + 0x1fff) / 0x2000);
		flp_insert(flp, name);
	}
	fclose(file);
	return err;
}

int loadTRD(Computer* comp, const char* name, int drv) {
	return trd_read(comp->dif->flp[drv & 3], name);
}

int trd_write(Floppy* flp, const char* name) {
	unsigned char syssec[0x100];
	diskGetSectorData(flp, 0, 9, syssec, 256);
	int disksize;
	if (syssec[0xe7] == 0x10)
		disksize = syssec[0xe1] + syssec[0xe2]*16 + syssec[0xe5] + syssec[0xe6]*256;
	else
		disksize = 0;
	if (disksize < 2560) disksize = 2560;		// 80*2*16
	// an image past 86 cylinders whole, whatever its system sector says (another
	// DOS's, or a full disk's first free track wrapped to 0)
	if (disksize < flp_wide(flp) * 32) disksize = flp_wide(flp) * 32;
	if (disksize > TRD_MAX / 0x100) disksize = TRD_MAX / 0x100;	// what loadTRD takes back
	int tracks = (disksize + 15) / 16;

	// whole tracks are read, so the buffer holds tracks, not disksize. too big
	// for the stack: 128 cylinders is a megabyte
	unsigned char* img = malloc(tracks * 16 * 0x100);
	unsigned char* dptr = img;
	for (int i = 0; i < tracks; i++) {
		for (int j = 1; j < 17; j++) {
			if (!diskGetSectorData(flp, i, j, dptr, 256)) {
				free(img);
				return ERR_TRD_SNF;
			}
			dptr += 256;
		}
	}

	FILE* file = fopen(name, "wb");
	if (!file) {
		free(img);
		return ERR_CANT_OPEN;
	}
	fwrite((char*)img, disksize*0x100, 1, file);
	fclose(file);
	free(img);

	flp_set_path(flp, name);
	flp->changed = 0;
	return ERR_OK;
}

int saveTRD(Computer* comp, const char* name, int drv) {
	return trd_write(comp->dif->flp[drv & 3], name);
}

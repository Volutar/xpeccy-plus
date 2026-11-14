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
		// what the file lacks reads as zeros
		char* img = calloc(TRD_MAX, 1);
		if (fread(img, 1, len, file) != len) err = ERR_CANT_OPEN;
		// the disk is as big as the file or its system sector says, in whole cylinders
		int secs = (int)(len >> 8);
		unsigned char* sys = (unsigned char*)img + 8 * 0x100;
		int cap = sys[0xe1] + sys[0xe2] * 16 + sys[0xe5] + sys[0xe6] * 256;
		if ((sys[0xe7] == 0x10) && (cap > secs)) secs = (cap > TRD_MAX / 0x100) ? TRD_MAX / 0x100 : cap;
		int cyls = (secs + 31) / 32;
		int trk;
		for (trk = 0; (trk < cyls * 2) || (trk < 168); trk++)
			flp_format_trk(flp, trk, 16, 256, img + trk * 0x1000);
		while (trk < 256)		// read into the drive itself: nothing of a wider disk left
			flpClearTrack(flp, trk++);
		free(img);
		flp->cyls = cyls;
		if (err == ERR_OK) flp_insert(flp, name);
	}
	fclose(file);
	return err;
}

int loadTRD(Computer* comp, const char* name, int drv) {
	return trd_read(comp->dif->flp[drv & 3], name);
}

int trd_write(Floppy* flp, const char* name) {
	unsigned char syssec[0x100];
	if (!diskGetSectorData(flp, 0, 9, syssec, 256)) memset(syssec, 0, sizeof(syssec));
	int disksize;
	if (syssec[0xe7] == 0x10)
		disksize = syssec[0xe1] + syssec[0xe2]*16 + syssec[0xe5] + syssec[0xe6]*256;
	else
		disksize = 0;
	if (disksize < 2560) disksize = 2560;		// 80*2*16
	// a disk past 86 cylinders whole, whatever its system sector says
	if (disksize < flp_wide(flp) * 32) disksize = flp_wide(flp) * 32;
	if (disksize > TRD_MAX / 0x100) disksize = TRD_MAX / 0x100;	// what loadTRD takes back
	int tracks = (disksize + 15) / 16;

	// whole tracks, up to a megabyte: on the heap
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

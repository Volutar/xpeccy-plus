#include "filetypes.h"

#pragma pack (push, 1)

typedef struct {
	char sign[3];
	char wp;
	unsigned short cyls;
	unsigned short heads;
	unsigned short dInfo;
	unsigned short dData;
	unsigned short xSize;
} fdiHead;

typedef struct {
	unsigned int dData;
	unsigned short res;		// reserved
	unsigned char secCount;
} fdiTHead;

typedef struct {
	unsigned char cyl;
	unsigned char head;
	unsigned char sec;
	unsigned char len;
	unsigned char flag;
	unsigned short dData;
} fdiSHead;

#pragma pack (pop)

// 1 if any sector has a CRC flag (bits 0-5): a writer that sets none kept no CRCs
static int fdi_has_crcs(FILE* file, int tracks) {
	long at = ftell(file);
	fdiTHead thd;
	fdiSHead shd;
	int res = 0;
	for (int t = 0; (t < tracks) && !res; t++) {
		if (fread(&thd, sizeof(thd), 1, file) != 1) break;
		for (int s = 0; (s < thd.secCount) && !res; s++) {
			if (fread(&shd, sizeof(shd), 1, file) != 1) break;
			res = (shd.flag & 0x3f) ? 1 : 0;
		}
	}
	fseek(file, at, SEEK_SET);
	return res;
}

int fdi_read(Floppy* flp, const char* name) {
	FILE* file = fopen(name, "rb");
	if (!file) return ERR_CANT_OPEN;

	int err = ERR_OK;
	fdiHead hd;
	fdiTHead thd;
	fdiSHead shd;
	Sector trkImg[256];
	size_t pos;
//	int i;
//	for (i = 0; i < 256; i++) trkImg[i].data = NULL;
	if (fread((char*)&hd, sizeof(fdiHead), 1, file) != 1) memset(&hd, 0, sizeof(hd));	// no sign then
#ifdef WORDS_BIG_ENDIAN
	hd.cyls = swap16(hd.cyls);
	hd.heads = swap16(hd.heads);
	hd.dInfo = swap16(hd.dInfo);
	hd.dData = swap16(hd.dData);
	hd.xSize = swap16(hd.xSize);
#endif
	if (strncmp((char*)hd.sign, "FDI", 3) != 0) {
		err = ERR_FDI_SIGN;
	} else if ((hd.heads < 1) || (hd.heads > 2)) {
		err = ERR_FDI_HEAD;
	} else {
		fseek(file, hd.xSize, SEEK_CUR);
		int trk, head, sec;
		int cut = 0;		// the file ended: the tracks before stay
		int crcs = fdi_has_crcs(file, hd.cyls * hd.heads);
		for (trk = 0; (trk < hd.cyls) && !cut; trk++) {
			for (head = 0; (head < hd.heads) && !cut; head++) {
				if (fread((char*)&thd, sizeof(fdiTHead), 1, file) != 1) {
					cut = 1;
					break;
				}
#ifdef WORDS_BIG_ENDIAN
				thd.dData = swap32(thd.dData);
#endif
				for (sec = 0; sec < thd.secCount; sec++) {
					if (fread((char*)&shd, sizeof(fdiSHead), 1, file) != 1) {
						cut = 1;
						break;
					}
#ifdef WORDS_BIG_ENDIAN
					shd.dData = swap16(shd.dData);
#endif
					trkImg[sec].trk = shd.cyl;
					trkImg[sec].head = shd.head;
					trkImg[sec].sec = shd.sec;
					trkImg[sec].sz = shd.len;
					trkImg[sec].type = (shd.flag & 0x80) ? 0xf8 : 0xfb;
					trkImg[sec].flags = 0;
					trkImg[sec].crc = -1;
					if (shd.flag & 0x40) {			// an ID with no data field
						trkImg[sec].flags = SEC_NO_DATA;
						continue;
					}
					if (crcs && !(shd.flag & (1 << (shd.len & 3))))	// no good CRC at the length read
						trkImg[sec].flags = SEC_BAD_CRC;
					shd.len = shd.len & 3;
					pos = ftell(file);
					fseek(file, hd.dData + thd.dData + shd.dData, SEEK_SET);
					// data past the end of the file reads as zeros
					size_t want = 128 << (shd.len & 3);
					size_t got = fread((char*)trkImg[sec].data, 1, want, file);
					if (got < want) memset(trkImg[sec].data + got, 0, want - got);
					fseek(file, pos, SEEK_SET);
				}
				diskFormImageTrack(flp, (trk << 1) + head, trkImg, sec);
			}
		}
		flp->protect = hd.wp ? 1 : 0;
		flp->cyls = hd.cyls;
		flp_insert(flp, name);
	}
	//for (i = 0; i < 256; i++) if (trkImg[i].data) free(trkImg[i].data);
	fclose(file);
	return err;
}

int loadFDI(Computer* comp, const char* name, int drv) {
	return fdi_read(comp->dif->flp[drv & 3], name);
}

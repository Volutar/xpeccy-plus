#include "filetypes.h"

// 160trk x 18sec x 512bytes hd fdd image (1,44MB)

int ima_read(Floppy* flp, const char* path) {
	int res = ERR_OK;
	int trk;
	char buf[512 * 18];
	FILE* file = fopen(path, "rb");
	if (file) {
		for (trk = 0; trk < 160; trk++) {
			// an image shorter than 1.44M: the tracks it has not are zeros
			size_t got = fread(buf, 1, sizeof(buf), file);
			if (got < sizeof(buf)) memset(buf + got, 0, sizeof(buf) - got);
			flp_format_trk(flp, trk, 18, 512, buf);
		}
		fclose(file);
		flp->cyls = 0;			// 80, no wider than a drive
		flp_insert(flp, path);
	} else {
		res = ERR_CANT_OPEN;
	}
	return res;
}

int load_ima(Computer* comp, const char* path, int drv) {
	return ima_read(comp->dif->flp[drv & 3], path);
}

#include <stdio.h>
#include "unpackers/lh1/public/lha_decoder.h"

#include "filetypes.h"

#pragma pack(push, 1)

typedef struct {
	char sign[2];	// td | TD
	unsigned char vol;
	unsigned char chk;
	unsigned char ver;	// 0x15
	unsigned char dens;	// 0x00
	unsigned char typ;
	unsigned char flag;	// b7:comment
	unsigned char dos;
	unsigned char sides;
	unsigned short crc;
} td0Head;

typedef struct {
	unsigned short crc;
	unsigned short len;
	unsigned char yr,mon,day,hr,min,sec;
} td0RemHead;

typedef struct {
	unsigned char nsec;
	unsigned char trk;
	unsigned char head;
	unsigned char crc;
} td0TrkHead;

typedef struct {
	unsigned char trk;
	unsigned char head;
	unsigned char sec;
	unsigned char secz;
	unsigned char ctrl;
	unsigned char crc;
} td0SecHead;

#pragma pack(pop)

// both return how many bytes they gave: fewer than asked when the file ends
size_t fGetData(void* buf, size_t len, void* data) {
	if (feof((FILE*)data)) return 0;
	if (buf == NULL) {
		fseek((FILE*)data,len,SEEK_CUR);
		return len;
	}
	return fread(buf, 1, len, (FILE*)data);
}

size_t lhaDepack(void* buf, size_t len, void* data) {
	size_t res;
	if (buf == NULL) {
		unsigned char* tbuf = (unsigned char*)malloc(len);
		res = lha_decoder_read((LHADecoder*)data, tbuf, len);
		free(tbuf);
	} else {
		res = lha_decoder_read((LHADecoder*)data, buf, len);
	}
	return res;
}

// The sizes and counts come from the file: a cut or broken one stops the
// reading (the tracks before it stay), and nothing is written past a
// sector's buffer.
#define TD0_GET(dst, len) if (getBytes((dst), (len), dptr) != (size_t)(len)) {bad = 1; break;}

void doTD0(Floppy* flp, size_t(*getBytes)(void*, size_t,void*), void* dptr, int haveRem) {
	td0RemHead rhd;
	td0TrkHead thd;
	td0SecHead shd;
	Sector sec[256];
	int idx;
	int i;
	int icnt;
	unsigned short dlen;
	unsigned char btype;
	unsigned short cnt;
	unsigned char ch1,ch2;
	unsigned char buf[8192];
	unsigned char* ptr;
	unsigned char* end;
	size_t n;
	int bad = 0;
	if (haveRem) {		// skip comment
		if (getBytes(&rhd, sizeof(td0RemHead), dptr) != sizeof(td0RemHead)) return;
#ifdef WORDS_BIG_ENDIAN
		rhd.len = swap16(rhd.len);
#endif
		getBytes(NULL, rhd.len, dptr);
	}
	while (!bad) {
		idx = 0;
		TD0_GET(&thd, sizeof(td0TrkHead));
		if (thd.nsec == 0xff) break;
		for (i = 0; i < thd.nsec; i++) {
			TD0_GET(&shd, sizeof(td0SecHead));
			sec[idx].trk = shd.trk;
			sec[idx].head = shd.head;
			sec[idx].sec = shd.sec;
			sec[idx].sz = shd.secz;
			sec[idx].type = 0xfb;
			sec[idx].crc = -1;
			memset(sec[idx].data, 0x00, 4096);
			if (shd.ctrl & 0x10) {				// empty sector
				idx++;
				continue;
			}
			TD0_GET(&ch1, 1);
			TD0_GET(&ch2, 1);
			dlen = ch1 | (ch2 << 8);		// data size
			if ((idx > thd.nsec) || (shd.secz & 0xf8)) {
				getBytes(NULL, dlen, dptr);	// skip data
				continue;
			}
			if (dlen < 1) {				// not even the type byte
				bad = 1;
				break;
			}
			TD0_GET(&btype, 1);
			ptr = sec[idx].data;
			end = ptr + sizeof(sec[idx].data);
			switch (btype) {
				case 0:
					n = dlen - 1;
					if (n > sizeof(sec[idx].data)) {
						TD0_GET(ptr, sizeof(sec[idx].data));
						getBytes(NULL, n - sizeof(sec[idx].data), dptr);
					} else {
						TD0_GET(ptr, n);
					}
					idx++;
					break;
				case 1:
					TD0_GET(&ch1, 1);
					TD0_GET(&ch2, 1);
					cnt = ch1 | (ch2 << 8);
					TD0_GET(&ch1, 1);
					TD0_GET(&ch2, 1);
					while ((cnt > 0) && (end - ptr >= 2)) {
						*(ptr++) = ch1;
						*(ptr++) = ch2;
						cnt--;
					}
					idx++;
					break;
				case 2:
					while (!bad && (dlen > 1)) {
						TD0_GET(&ch1, 1);
						TD0_GET(&ch2, 1);
						dlen -= 2;
						if (ch1 == 0) {
							if (ch2 > end - ptr) {bad = 1; break;}
							TD0_GET(ptr, ch2);
							dlen -= ch2;
							ptr += ch2;
						} else {
							if (ch1 > 13) {bad = 1; break;}		// a pattern longer than buf
							icnt = (1 << ch1);
							TD0_GET(buf, icnt);
							dlen -= icnt;
							while ((ch2 > 0) && (end - ptr >= icnt)) {
								memcpy(ptr, buf, icnt);
								ptr += icnt;
								ch2--;
							}
						}
					}
					idx++;
					break;
			}
			if (bad) break;
		}
		if (bad) break;
		diskFormTrack(flp, (thd.trk << 1) + thd.head, sec, idx);
		if (thd.trk >= flp->cyls) flp->cyls = thd.trk + 1;
	}
}

#undef TD0_GET

int td0_read(Floppy* flp, const char* name) {
	FILE* file = fopen(name, "rb");
	if (!file) return ERR_CANT_OPEN;

	int err = ERR_OK;
	td0Head hd;
	if (fread(&hd, sizeof(td0Head), 1, file) != 1) {
		err = ERR_TD0_SIGN;
	} else if (strncmp(hd.sign, "td", 2) && strncmp(hd.sign, "TD", 2)) {
		err = ERR_TD0_SIGN;
	} else if ((hd.dens != 0) || (hd.sides > 2)) {
		err = ERR_TD0_TYPE;
	} else if (hd.ver < 0x14) {
		err = ERR_TD0_VERSION;
	} else {
		flp->cyls = 0;			// doTD0 widens it track by track
		if (strncmp(hd.sign,"TD",2)) {	// 1 on td (packed)
			LHADecoderType* decType;
			LHADecoder* dec = NULL;
			decType = lha_decoder_for_name("-lh1-");
			dec = lha_decoder_new(decType, fGetData, file, -1);
			doTD0(flp, lhaDepack, dec, hd.flag & 0x80);
			lha_decoder_free(dec);
		} else {
			doTD0(flp, fGetData, file, hd.flag & 0x80);
		}
		flp_insert(flp, name);
	}
	fclose(file);
	return err;
}

int loadTD0(Computer* comp, const char* name, int drv) {
	return td0_read(comp->dif->flp[drv & 3], name);
}

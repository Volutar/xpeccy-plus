#include "filetypes.h"
#include "../xlog.h"
#include "unpackers/unpackers.h"
#include "../cpu/Z80/z80.h"

#include <stdio.h>
#include <string.h>

#pragma pack (push, 1)

typedef struct {
	char astr[32];
	char sign[12];
	unsigned char fmt;	// 2c : format
	unsigned char day;	// 2d : day
	unsigned char month;	// 2e : month
	unsigned char year;	// 2f : year
	unsigned char pcl,pch;	// 30 : pc
	unsigned char spl,sph;	// 32 : sp
	unsigned char page3;	// 34 : page
	unsigned char flag35;	// 35 : flag35
	unsigned char pal,pah;	// 36 : pager address
	unsigned char ral,rah;	// 38 : resident address
	unsigned char bcl,bch;	// 3A : block count
	unsigned char sec;	// 3C : sec
	unsigned char min;	// 3D : min
	unsigned char hour;	// 3E : hour
	char res3F[17];		// 3F : reserved
	char creator[32];
	char res70[144];	// 70 : reserved
} spgHead;

#pragma pack (pop)

int loadSPG(Computer* comp, const char* name, int drv) {
	FILE* file = fopen(name,"rb");
	if (!file) return ERR_CANT_OPEN;

	spgHead hd;
	unsigned char blkInfo[768];
	unsigned char inbuf[0x4000];
	unsigned char outbuf[0xffff];
	if ((fread((char*)&hd, sizeof(spgHead), 1, file) != 1) || memcmp(hd.sign, "SpectrumProg", 12)) {
		fclose(file);
		return ERR_SPG_SIGN;
	}
	if (hd.fmt != 0x10) {			// 0.x has another layout altogether
		fclose(file);
		return ERR_SPG_VERSION;
	}
	fread((char*)blkInfo, 768, 1, file);

	// printf("spg ver %i.%i\n",(hd.fmt & 0xf0) >> 4, hd.fmt & 0x0f);

	int blkCount = hd.bcl | (hd.bch << 8);
	int idx = 0;
	int sze;
	unsigned char pg;
	int addr;
	for (int i = 0; i < blkCount; i++) {
		addr = (blkInfo[idx] & 0x1f) << 9;		// addr in page (00:0000 .. 1F:3E00)
		sze = ((blkInfo[idx+1] & 0x1f) + 1) << 9;	// block size (00:512 .. 1f:16K
		pg = blkInfo[idx+2];

		fread((char*)inbuf, sze, 1, file);
		int dst = (pg << 14) + addr;
		unsigned char* src = inbuf;
		switch (blkInfo[idx + 1] & 0xc0) {
			case 0x00:
				break;
			case 0x40:
				sze = demegalz(inbuf, outbuf);		// it works?
				src = outbuf;
				break;
			case 0x80:
				sze = dehrust(inbuf, outbuf);		// no 'last 6 bytes'
				src = outbuf;
				break;
			default:
				xlog(XLG_FILE, XLL_WARN, "(%.2X,%.4X,%.4X) unknown compression",pg,addr,sze);
				sze = 0;
				break;
		}
		if (dst + sze > comp->mem->ramMask + 1)		// a block runs off the end of ram
			sze = comp->mem->ramMask + 1 - dst;
		if (sze > 0)
			memcpy(comp->mem->ramData + dst, src, sze);

		if (blkInfo[idx] & 0x80) break;			// last block flag
		idx += 3;
	}
	compReset(comp, RES_DEFAULT);
	comp->cpu->regPC = (hd.pch << 8) | hd.pcl;
	comp->cpu->regSP = (hd.sph << 8) | hd.spl;
	// the speed the program wants is the machine's own turbo, as if written
	// to SYSCONF: the user's multiplier is not the file's business
	tsOut20AF(comp, 0x20af, hd.flag35 & 0x03);
	comp->cpu->flgIFF1 = (hd.flag35 & 0x04) ? 1 : 0;	// int enabled/disabled
	comp->cpu->inten = Z80_NMI | (comp->cpu->flgIFF1 ? Z80_INT : 0);
	comp->cpu->regIM = 1;				// im 1
	comp->cpu->regI = 0x3f;				// i = 3F
	comp->cpu->regIY = 0x5c3a;			// what the 48K rom's IM 1 handler wants
	comp->cpu->regHLa = 0x2758;
	comp->tsconf.p21af = 0x01;			// window 0 mapped by DOS/ROM128: basic 48
	comp->flgDOS = 0;
	comp->flgROM = 1;
	comp->flgCPM = 0;
	comp->p7FFD = 0x10;
	comp->pEFF7 = 0x00;
	comp->prt2 = 0x00;
	comp->hw->mapMem(comp);
	memSetBank(comp->mem,0x40,MEM_RAM,5,MEM_16K,NULL,NULL,NULL);
	memSetBank(comp->mem,0x80,MEM_RAM,2,MEM_16K,NULL,NULL,NULL);
	memSetBank(comp->mem,0xc0,MEM_RAM,hd.page3,MEM_16K,NULL,NULL,NULL);
	comp->vid->tsconf.tconfig = 0;
	comp->vid->nogfx = 0;
	tsReset(comp->ts);
	fclose(file);
	return ERR_OK;
}

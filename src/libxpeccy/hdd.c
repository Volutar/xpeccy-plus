#include <stdlib.h>
#include "xlog.h"
#include <stdio.h>
#include <string.h>

#include "hdd.h"
#include "filetypes/filetypes.h"

/* ABOUNT INTRQ:
On PIO transfers, INTRQ is asserted at the beginning of each data block to be
transferred. A data block is typically a single sector, except when declared
otherwise by use of the Set Multiple command. An exception occurs on Format
Track, Write Sector(s), Write Buffer and Write Long commands - INTRQ shall not
be asserted at the beginning of the first data block to be transferred.
On DMA transfers, INTRQ is asserted only once, after the command has
completed.
*/

// overall

void copyStringToBuffer(unsigned char* dst, const char* src, int len) {
	while (len > 1) {
		*(dst+1) = *(src);
		*(dst) = *(src+1);
		dst += 2;
		src += 2;
		len -= 2;
	}
}

// ATA device

ATADev* ataCreate(int tp, cbirq cb, void* p, int id) {
	ATADev* ata = (ATADev*)malloc(sizeof(ATADev));
	memset(ata,0x00,sizeof(ATADev));
	ata->type = tp;
	ata->hasLBA = 1;		// every IDE disk since the mid-90s; off is for an old CHS-only one
	ata->pass.cyls = 1024;
	ata->pass.hds = 16;
	ata->pass.vol = 1;
	ata->pass.bps = 512 * ata->pass.vol;
	ata->pass.spt = 255;
	ata->pass.bpt = ata->pass.bps * ata->pass.spt;
	ata->pass.type = 1;
	ata->xirq = cb;
	ata->xptr = p;
	ata->xid = id;
	memset(ata->pass.serial,' ',20);
	memcpy(ata->pass.serial,"IDDQD",strlen("IDDQD"));
	memset(ata->pass.mcver,' ',8);
	memset(ata->pass.model,' ',40);
	memcpy(ata->pass.model,"Xpeccy HDD image",strlen("Xpeccy HDD image"));
	ata->image = NULL;
	ata->file = NULL;
	ata->reg.state = 0;
	return ata;
}

void ataDestroy(ATADev* ata) {
	free(ata);
}

void ataReset(ATADev* ata) {
	ata->reg.state = HDF_DRDY | HDF_DSC;
	ata->reg.err = 0x01;		// err contains 01 after reset (it means no errors detected)
	ata->reg.count = 0x01;
	ata->reg.sec = 0x01;
	ata->reg.cyl = 0x0000;
	ata->reg.head = 0x00;
	ata->buf.mode = HDB_IDLE;
	ata->buf.pos = 0;
	ata->idle = 0;
	ata->sleep = 0;
	ata->standby = 0;
}

void ataClearBuf(ATADev* dev) {
	int i;
	for (i = 0; i < HDD_BUFSIZE; i++) {
		dev->buf.data[i] = 0x00;
	}
}

void ataRefresh(ATADev* dev) {
//	if (dev->hasLBA) {
//		dev->pass.spt = 64;
//		dev->pass.hds = 16;
//	} else {
//		dev->maxlba = dev->pass.cyls * dev->pass.hds * dev->pass.spt;
//	}
//	dev->pass.bpt = dev->pass.bps * dev->pass.spt;
}

void ataSetSector(ATADev* dev, int nr) {
	dev->lba = nr;
	if (dev->hasLBA && (dev->reg.head & HDF_LBA)) {
		dev->reg.sec = nr & 0xff;
		dev->reg.cyl = (nr >> 8) & 0xffff;
		dev->reg.head &= 0xf0;
		dev->reg.head |= ((nr >> 24) & 0x0f);
	} else {
		if (nr < dev->maxlba) {
			dev->reg.cyl = nr / (dev->pass.hds * dev->pass.spt);
			int tmp = nr % (dev->pass.hds * dev->pass.spt);
			dev->reg.head = tmp / dev->pass.spt;
			dev->reg.sec = tmp % dev->pass.spt + 1;
		}
	}
}

void ataNextSector(ATADev* dev) {
	if (dev->lba < (dev->maxlba - 1)) {
		dev->lba++;
	}
	ataSetSector(dev,dev->lba);
}

void ataSetLBA(ATADev* dev) {
	if (dev->hasLBA && (dev->reg.head & HDF_LBA)) {
		dev->lba = dev->reg.sec | (dev->reg.cyl << 8) | ((dev->reg.head & 0x0f) << 24);		// LBA28
	} else {
		dev->lba = ((dev->reg.cyl * dev->pass.hds + (dev->reg.head & 0x0f)) * dev->pass.spt) + dev->reg.sec - 1;
	}
}

void ataReadSector(ATADev* dev) {
	long nps;
	ataSetLBA(dev);
	if (dev->lba >= dev->maxlba) {					// sector not found
		dev->reg.state |= HDF_ERR;
		dev->reg.err |= (HDF_ABRT | HDF_IDNF);
	} else {
		if (dev->vfat) {
			vfat_read(dev->vfat, dev->lba, dev->buf.data);
		} else if (dev->file) {
			nps = dev->lba * dev->pass.bps + dev->offset;
			fseek(dev->file,nps,SEEK_SET);			// if filesize < nps, there will be 0xFF in buf
			fread((char*)dev->buf.data,dev->pass.bps,1,dev->file);
		} else {
			ataClearBuf(dev);
		}
	}
}

void ataWriteSector(ATADev* dev) {
	if (x_runahead) return;			// this frame is going to be rolled back
	ataSetLBA(dev);
	if (dev->lba >= dev->maxlba) {			// sector not found
		dev->reg.state |= HDF_ERR;
		dev->reg.err |= (HDF_ABRT | HDF_IDNF);
	} else if (!dev->vfat) {			// a folder is served read only
		if (dev->file) {
			long pos = dev->lba * dev->pass.bps + dev->offset;
			fseek(dev->file, pos, SEEK_SET);
			x_media_writes++;
			fwrite((char*)dev->buf.data, dev->pass.bps, 1, dev->file);
		}
	}
}

void ataAbort(ATADev* dev) {
	dev->reg.state |= HDF_ERR;
	dev->reg.err |= HDF_ABRT;
}

static int ata_present(ATADev* dev) {
	return (dev->type == IDE_ATA) && (dev->image != NULL);
}

// identify data: a word, and a pair of words low first
static void ata_put_word(ATADev* dev, int w, int val) {
	dev->buf.data[w * 2] = val & 0xff;
	dev->buf.data[w * 2 + 1] = (val >> 8) & 0xff;
}

static void ata_put_long(ATADev* dev, int w, int val) {
	ata_put_word(dev, w, val & 0xffff);
	ata_put_word(dev, w + 1, (val >> 16) & 0xffff);
}

void ataExec(ATADev* dev, unsigned char cm) {
	dev->reg.state = HDF_DRDY | HDF_DSC;
	dev->reg.err = 0x00;
	switch (dev->type) {
	case IDE_ATA:
	// printf("ATA exec %.2X (CHS=%X:%X:%X, count=%X)\n",cm,dev->reg.cyl,dev->reg.head & 0x0f, dev->reg.sec, dev->reg.count);
		dev->dma = 0;
		switch (cm) {
			case 0x00:			// NOP
				break;
			case 0x08:			// device reset
				break;
			case 0x20:			// read sectors (w/retry)
			case 0x21:			// read sectors (w/o retry)
				ataReadSector(dev);
				dev->buf.pos = 0;
				dev->buf.mode = HDB_READ;
				dev->reg.state |= HDF_DRQ;
				if (dev->inten)
					dev->xirq(dev->xid, dev->xptr);
					//dev->intrq = 1;		// non-dma transfer: INT on each sector in buffer
				break;
			case 0x22:			// read long (w/retry) TODO: read sector & ECC
			case 0x23:			// read long (w/o retry)
				ataAbort(dev);
				break;
			case 0x30:			// write sectors (w/retry)
			case 0x31:			// write sectors (w/o retry)
			case 0x3c:			// write verify
				dev->buf.pos = 0;
				dev->buf.mode = HDB_WRITE;
				dev->reg.state |= HDF_DRQ;
				break;
			case 0x32:			// write long (w/retry)
			case 0x33:			// write long (w/o retry)
				ataAbort(dev);
				break;
			case 0x40:			// verify sectors (w/retry)	TODO: is it just read sectors until error or count==0 w/o send buffer to host?
			case 0x41:			// verify sectors (w/o retry)
				do {
					ataReadSector(dev);
					dev->reg.count--;
					if (dev->reg.count)
						ataNextSector(dev);
				} while (dev->reg.count != 0);
				if (dev->inten)
					dev->xirq(dev->xid, dev->xptr);
					//dev->intrq = 1;		// generate int after all sectors verified
				break;
			case 0x50:			// format track; TODO: cyl = track; count = spt; drq=1; wait for buffer write, ignore(?) buffer; format track;
				break;
			case 0x90:			// execute drive diagnostic; FIXME: both drives must do this
				dev->reg.err = 0x01;
				break;
			case 0x91:			// initialize drive parameters; pass.spt = reg.count; pass.heads = (reg.head & 15) + 1;
				dev->pass.spt = dev->reg.count;
				dev->pass.hds = (dev->reg.head & 0x0f) + 1;
				break;
			case 0x94:			// standby immediate; TODO: if reg.count!=0 in idle/standby commands, HDD power off
			case 0xe0:
				dev->standby = 1;
				break;
			case 0x95:			// idle immediate
			case 0xe1:
				dev->idle = 1;
				break;
			case 0x96:			// standby
			case 0xe2:
				dev->standby = 1;
				break;
			case 0x97:			// idle
			case 0xe3:
				dev->idle = 1;
				break;
			case 0x98:			// check power mode
			case 0xe5:			// if drive is in, set sector count register to 0x00, if idle - to 0xff
				dev->reg.count = dev->idle ? 0xff : 0x00;
				break;
			case 0x99:			// sleep
			case 0xe6:
				dev->sleep = 1;
				break;
			case 0x9a:			// vendor unique
			case 0xc0:
			case 0xc1:
			case 0xc2:
			case 0xc3:
				ataAbort(dev);
				break;
			case 0xc4:			// read multiple; NOTE: doesn't support for 1-sector buffer
				ataAbort(dev);
				break;
			case 0xc5:			// write multiple; NOTE: doesn't support for 1-sector buffer
				ataAbort(dev);
				break;
			case 0xc6:			// set multiple mode; NOTE: only 1-sector reading supported
				ataAbort(dev);
				break;
			case 0xc8:			// read DMA (w/retry)
			case 0xc9:			// read DMA (w/o retry)
				ataReadSector(dev);		// put sector in buffer
				dev->buf.pos = 0;
				dev->buf.mode = HDB_READ;
				dev->reg.state |= HDF_DRQ;
				dev->dma = 1;
				// ataAbort(dev);
				break;
			case 0xca:			// write DMA (w/retry)
			case 0xcb:			// write DMA (w/o retry)
				dev->buf.pos = 0;
				dev->buf.mode = HDB_WRITE;
				dev->reg.state |= HDF_DRQ;
				dev->dma = 1;
				// ataAbort(dev);
				break;
			case 0xdb:			// acknowledge media chge
				dev->reg.state |= HDF_ERR;	// NOTE: HDD isn't removable, return abort error
				dev->reg.err |= HDF_ABRT;
				break;
			case 0xdc:			// boot - post-boot; TODO: do nothing?
				break;
			case 0xdd:			// boot - pre-boot; TODO: do nothing?
				break;
			case 0xde:			// door lock
				break;
			case 0xdf:			// door unlock
				break;
			case 0xe4:			// read buffer
				dev->buf.pos = 0;
				dev->buf.mode = HDB_READ;
				dev->reg.state |= HDF_DRQ;
				break;
			case 0xe8:
				dev->buf.pos = 0;	// write buffer
				dev->buf.mode = HDB_WRITE;
				dev->reg.state |= HDF_DRQ;
				break;
			case 0xe9:			// write same
				ataAbort(dev);
				break;
			case 0xec:			// identify drive
				ataClearBuf(dev);
				ata_put_word(dev, 0, 0x0004);		// main word
				ata_put_word(dev, 1, dev->pass.cyls);
				ata_put_word(dev, 3, dev->pass.hds);
				ata_put_word(dev, 4, dev->pass.bpt);
				ata_put_word(dev, 5, dev->pass.bps);
				ata_put_word(dev, 6, dev->pass.spt);
				copyStringToBuffer(&dev->buf.data[20],dev->pass.serial,20);	// serial (20 bytes)
				ata_put_word(dev, 20, dev->pass.type);	// buffer type
				ata_put_word(dev, 21, dev->pass.vol);	// buffer size
				copyStringToBuffer(&dev->buf.data[46],dev->pass.mcver,8);	// microcode version (8 bytes)
				copyStringToBuffer(&dev->buf.data[54],dev->pass.model,10);	// model (40 bytes)
				dev->buf.data[99] = (dev->hasDMA ? 0x01 : 0x00) | (dev->hasLBA ? 0x02 : 0x00);	// lba/dma support
				// words 54..58: the geometry in use, which 0x91 sets; 60..61: LBA sectors
				ata_put_word(dev, 53, 0x0001);
				ata_put_word(dev, 54, dev->pass.cyls);
				ata_put_word(dev, 55, dev->pass.hds);
				ata_put_word(dev, 56, dev->pass.spt);
				ata_put_long(dev, 57, dev->pass.cyls * dev->pass.hds * dev->pass.spt);
				if (dev->hasLBA)
					ata_put_long(dev, 60, dev->maxlba);
				dev->buf.pos = 0;
				dev->buf.mode = HDB_READ;
				dev->reg.state |= HDF_DRQ;
				//printf("request hdd info\n");
				break;
			case 0xef:			// set features
				break;
			default:
				switch (cm & 0xf0) {
					case 0x10:			// 0x1x: recalibrate
						dev->reg.cyl = 0x0000;
						break;
					case 0x70:			// seek; TODO: if cylinder/head is out of range - must be an error?
						break;
					case 0x80:			// vendor unique
					case 0xf0:
						ataAbort(dev);
						break;
					default:
						ataAbort(dev);
						xlog(XLG_DISK, XLL_WARN, "HDD exec: command %.2X isn't emulated",cm);
						break;
				}
				break;
		}
		break;
	}
}

unsigned short ataRd(ATADev* dev,int prt) {
	unsigned short res = 0xffff;
	if (!ata_present(dev)) return res;
	switch (prt) {
		case HDD_DATA:
			if ((dev->buf.mode == HDB_READ) && (dev->reg.state & HDF_DRQ)) {
				res = dev->buf.data[dev->buf.pos] | (dev->buf.data[dev->buf.pos + 1] << 8);	// low-hi
				dev->buf.pos += 2;
				if (dev->buf.pos >= HDD_BUFSIZE) {
					dev->buf.pos = 0;
					if ((dev->reg.com & 0xf0) == 0x20) {
						dev->reg.count--;
						if (dev->reg.count == 0) {
							dev->buf.mode = HDB_IDLE;
							dev->reg.state &= ~HDF_DRQ;
						} else {
							ataNextSector(dev);
							ataReadSector(dev);
							if (!dev->dma && dev->inten)
								dev->xirq(dev->xid, dev->xptr);
								//dev->intrq = 1;
						}
					} else {
						dev->buf.mode = HDB_IDLE;
						dev->reg.state &= ~HDF_DRQ;
					}
				}
			}
			break;
		case HDD_COUNT:
			res = dev->reg.count;
			break;
		case HDD_SECTOR:
			res = dev->reg.sec;
			break;
		case HDD_CYL_LOW:
			res = dev->reg.cyl & 0xff;
			break;
		case HDD_CYL_HI:
			res = ((dev->reg.cyl & 0xff00) >> 8);
			break;
		case HDD_HEAD:
			res = dev->reg.head;
			break;
		case HDD_ERROR:
			res = dev->reg.err;
			break;
		case HDD_STATE:
			res = dev->reg.state;
			dev->intrq = 0;		// host reading the status register = INT cleared
			break;
		case HDD_ASTATE:
			res = dev->reg.state;
			// this register doesn't reset interrupt line
			break;
		default:
			xlogh(XLG_DISK, XLL_DEBUG, "HDD in: port %.3X isn't emulated",prt);
//			throw(0);
	}
	return res;
}

void ataWr(ATADev* dev, int prt, unsigned short val) {
	if (!ata_present(dev)) return;
	switch (prt) {
		case HDD_DATA:
			if ((dev->buf.mode == HDB_WRITE) && (dev->reg.state & HDF_DRQ)) {
				dev->buf.data[dev->buf.pos++] = (val & 0xff);			// low
				dev->buf.data[dev->buf.pos++] = ((val & 0xff00) >> 8);		// hi
				if (dev->buf.pos >= HDD_BUFSIZE) {
					dev->buf.pos = 0;
					if ((dev->reg.com & 0xf0) == 0x30) {
						ataWriteSector(dev);
						if (!dev->dma && dev->inten)			// sector is writed, INT
							dev->xirq(dev->xid, dev->xptr);
							//dev->intrq = 1;
						dev->reg.count--;
						if (dev->reg.count == 0) {
							dev->buf.mode = HDB_IDLE;
							dev->reg.state &= ~HDF_DRQ;
						} else {
							ataNextSector(dev);
						}
					} else {
						dev->buf.mode = HDB_IDLE;
						dev->reg.state &= ~HDF_DRQ;
					}
				}
			}
			break;
		case HDD_COUNT:
			dev->reg.count = val & 0xff;
			break;
		case HDD_SECTOR:
			dev->reg.sec = val & 0xff;
			break;
		case HDD_CYL_LOW:
			dev->reg.cyl &= 0xff00;
			dev->reg.cyl |= (val & 0xff);
			break;
		case HDD_CYL_HI:
			dev->reg.cyl &= 0x00ff;
			dev->reg.cyl |= ((val & 0xff) << 8);
			break;
		case HDD_HEAD:
			dev->reg.head = val & 0xff;
			break;
		case HDD_COM:
			dev->reg.com = val & 0xff;
			ataExec(dev,dev->reg.com);
			if (dev->inten)
				dev->xirq(dev->xid, dev->xptr);
				//dev->intrq = 1;		// host writing the command register = INT
			break;
		case HDD_CTRL:
			dev->inten = (val & 2) ? 0 : 1;
			if (val & 0x04) {
				ataReset(dev);
				if (dev->inten)
					dev->xirq(dev->xid, dev->xptr);
					//dev->intrq = 1;	// INT on soft reset
			}
			break;
		case HDD_FEAT:
			break;
		default:
			xlogh(XLG_DISK, XLL_DEBUG, "HDD out: port %.3X isn't emulated",prt);
//			throw(0);
	}
}

// IDE interface

IDE* ideCreate(int tp, cbirq cb, void* p) {
	IDE* ide = (IDE*)malloc(sizeof(IDE));
	ide->master = ataCreate(IDE_NONE, cb, p, IRQ_HDD_PRI);
	ide->slave = ataCreate(IDE_NONE, cb, p, IRQ_HDD_PRI);
	ide->curDev = ide->master;
	ide->smuc.fdd = 0xc0;
	ide->smuc.sys = 0x00;
	ide->smuc.nv = nvCreate();
	//ide->type = tp;
	ide_set_type(ide, tp);
	return ide;
}

void ideDestroy(IDE* ide) {
	ideCloseFiles(ide);
	ataDestroy(ide->master);
	ataDestroy(ide->slave);
	free(ide);
}

// TODO: check extension

// cylinders of a 16 x 63 geometry over the whole volume, as far as CHS reaches
static int ata_chs_cyls(int maxlba) {
	int cyls = maxlba / (16 * 63);
	return (cyls > 16383) ? 16383 : cyls;
}

void ata_load_raw(ATADev* dev) {
	fseek(dev->file, 0, SEEK_END);
	long fsz = ftell(dev->file);
	dev->maxlba = fsz / 512;
	dev->pass.bps = 512;
	dev->pass.hds = 16;
	dev->pass.spt = 63;
	dev->pass.cyls = ata_chs_cyls(dev->maxlba);
	rewind(dev->file);
	dev->offset = 0;
}

void ata_load_hdi(ATADev* dev) {
	fseek(dev->file, 8, SEEK_SET);
	dev->offset = fgeti(dev->file);		// header size = data offset
	fgeti(dev->file);			// data size
	dev->pass.bps = fgeti(dev->file);	// sector size
	dev->pass.spt = fgeti(dev->file);	// sectors/track
	dev->pass.hds = fgeti(dev->file);	// heads
	dev->pass.cyls = fgeti(dev->file);	// cylinders
	dev->maxlba = dev->pass.spt * dev->pass.hds * dev->pass.cyls;
	fseek(dev->file, dev->offset, SEEK_SET);
}

typedef struct {
	const char* ext;
	void(*load)(ATADev*);
} HDDImageDsc;

HDDImageDsc hdd_file_tab[] = {
	{"hdi", ata_load_hdi},
	{NULL, ata_load_raw}
};

static ATADev* ide_get_dev(IDE* ide, int wut) {
	if (wut == IDE_MASTER) return ide->master;
	if (wut == IDE_SLAVE) return ide->slave;
	return NULL;
}

// release whatever the device holds: an image file or a folder volume
static void ataCloseFile(ATADev* dev) {
	if (dev->file) fclose(dev->file);
	dev->file = NULL;
	if (dev->vfat) vfat_free(dev->vfat);
	dev->vfat = NULL;
}

// a folder serves the HDD boot sector written for this interface's ports
static void ide_vfat_port(IDE* ide) {
	int port = (ide->type == IDE_ATM) ? VF_PORT_ATM : VF_PORT_NEMO;
	if (ide->master->vfat) ide->master->vfat->hddport = port;
	if (ide->slave->vfat) ide->slave->vfat->hddport = port;
}

// mount a synthetic volume built from a host folder. The device takes ownership
// of the volume; the folder path is kept in ->image, as with an image file.
void ideSetFolder(IDE* ide, int wut, const char* name, vFat* vf) {
	ATADev* dev = ide_get_dev(ide, wut);
	if (dev == NULL) return;
	ataCloseFile(dev);
	free(dev->image);
	dev->image = NULL;
	if (!vf) return;
	dev->image = (char*)malloc(strlen(name) + 1);
	strcpy(dev->image, name);
	dev->vfat = vf;
	dev->offset = 0;
	dev->pass.bps = 512;
	dev->pass.hds = 16;
	dev->pass.spt = 63;
	dev->maxlba = vf->volume;
	dev->pass.cyls = ata_chs_cyls(dev->maxlba);
	ide_vfat_port(ide);
}

void ideSetImage(IDE *ide, int wut, const char *name) {
	ATADev* dev = ide_get_dev(ide, wut);
	if (dev == NULL) return;
	ataCloseFile(dev);
	if (strlen(name) == 0) {
		free(dev->image);
		dev->image = NULL;
		dev->file = NULL;
	} else {
		dev->image = realloc(dev->image,strlen(name) + 1);
		strcpy(dev->image,name);
		dev->file = fopen(dev->image,"rb+");
		if (dev->file) {
			const char* ptr = strrchr(dev->image, '.');
			if (ptr) {
				ptr++;
				HDDImageDsc* itm = hdd_file_tab;
				while((itm->ext != NULL) && strcmp(itm->ext, ptr))
					itm++;
				itm->load(dev);
			} else {
				ata_load_raw(dev);
			}
		} else {
			free(dev->image);
			dev->image = NULL;
		}
	}
}

ATAPassport ideGetPassport(IDE* ide, int iface) {
	ATAPassport res;
	switch(iface) {
		case IDE_MASTER:
			res = ide->master->pass;
			break;
		case IDE_SLAVE:
			res = ide->slave->pass;
			break;
	}
	return res;
}

// IDE controller

// SMUC: dos, a0=0,a1=a5=a7=a11=a12=1	xxx1 1xxx 1x1x xx10

// dummy

int ide_dum_rd(IDE* ide, int port, int* val, int dos) {return 0;}
int ide_dum_wr(IDE* ide, int port, int val, int dos) {return 0;}

int ide_ata_rd(IDE* ide, int adr, int hi) {
	int res;
	if (hi) {
		res = (ide->bus >> 8) & 0xff;
	} else if ((ide->curDev == ide->slave) && !ata_present(ide->slave) && ata_present(ide->master)
			&& ((adr == HDD_STATE) || (adr == HDD_ASTATE))) {
		// ATA: device 0 answers for an absent device 1 with a status of 00
		ide->bus = 0x0000;
		res = 0x00;
	} else {
		ide->bus = ataRd(ide->curDev, adr);
		res = ide->bus & 0xff;
	}
	return res;
}

// a write to the head register picks master or slave
static void ide_select(IDE* ide, int adr, int val) {
	if (adr == HDD_HEAD)
		ide->curDev = (val & HDF_DRV) ? ide->slave : ide->master;
}

void ide_ata_wr(IDE* ide, int adr, int hi, int val) {
	if (hi) {
		ide->bus &= 0xff;
		ide->bus |= (val << 8);
	} else {
		ide->bus &= 0xff00;
		ide->bus |= (val & 0xff);
		ide_select(ide, adr, val);
		ataWr(ide->curDev, adr, ide->bus);
	}
}

// common

int ide_common_rd(IDE* ide, ataAddr adr) {
	return ide_ata_rd(ide, adr.port, adr.high);
}

void ide_common_wr(IDE* ide, ataAddr adr, int val) {
	ide_ata_wr(ide, adr.port, adr.high, val);
}

// atm2

ataAddr ide_atm_decode(int port, int dosen, int wr) {
	ataAddr res;
	res.iorq = (((port & 0x001f) == 0x000f) && dosen) ? 1 : 0;
	res.hdd = 1;
	res.high = ((port & 0x1ff) == 0x10f) ? 1 : 0;
	res.port = (port & 0xe0) >> 5;
	return res;
}

// nemo evo

ataAddr ide_nemoevo_decode(int port, int dosen, int wr) {
	ataAddr res;
	res.iorq = (((port & 0xff) == 0xc8) || ((port & 0xff) == 0x11) || ((port & 0x1f) == 0x10)) ? 1 : 0;
	res.hdd = 1;
	res.high = ((port & 0xff) == 0x11) ? 1 : 0;
	// #C8 is CS1: alternate status / device control (zports.v ide_cs1_n)
	res.port = ((port & 0xff) == 0xc8) ? HDD_ASTATE : (port & 0xe0) >> 5;
	return res;
}

int ide_nemoevo_rd(IDE* ide, ataAddr adr) {
	if (adr.port == HDD_DATA) {
		if (adr.high) {
			ide->hiTrig = 0;				// 11 : high, next 10 is low
		} else {
			if (ide->hiTrig) adr.high = 1;			// 10 : high byte
			ide->hiTrig ^= 1;				// switch trigger
		}
	} else {
		ide->hiTrig = 0;		// non-data ports : next 10 is low
	}
	return ide_ata_rd(ide, adr.port, adr.high);
}

void ide_nemoevo_wr(IDE* ide, ataAddr adr, int val) {
	if (adr.hdd) {
		if (adr.port == HDD_DATA) {
			if (adr.high) {
				ide->bus &= 0x00ff;
				ide->bus |= (val << 8);
				ide->hiTrig = 2;		// 11 : high, next 10 is low+wr
			} else {
				if (ide->hiTrig == 0) {		// 10 : low
					ide->bus &= 0xff00;
					ide->bus |= (val & 0xff);
					ide->hiTrig = 1;
				} else if (ide->hiTrig == 1) {	// 10 : high + wr
					ide->bus &= 0x00ff;
					ide->bus |= ((val & 0xff) << 8);
					ataWr(ide->curDev, 0, ide->bus);
					ide->hiTrig = 0;
				} else if (ide->hiTrig == 2) {	// 10 : low + wr (after 11)
					ide->bus &= 0xff00;
					ide->bus |= val;
					ataWr(ide->curDev, 0, ide->bus);
					ide->hiTrig = 0;
				}
			}
		} else {
			ide->hiTrig = 0;		// non-data ports : next 10 is low
			ide_select(ide, adr.port, val);
			ataWr(ide->curDev, adr.port, val);
		}
	}
}

// nemo

ataAddr ide_nemo_decode(int port, int dosen, int wr) {
	ataAddr res;
	res.iorq = (dosen || (port & 6)) ? 0 : 1;
	res.hdd = 1;
	res.high = ((port & 0xe1) == 0x01) ? 1 : 0;
	res.port = (port & 0xe0) >> 5;
	return res;
}

// nemo a8

ataAddr ide_nemoa8_decode(int port, int dosen, int wr) {
	ataAddr res;
	res.iorq = (dosen || (port & 6)) ? 0 : 1;
	res.hdd = 1;
	res.high = ((port & 0x1e0) == 0x100) ? 1 : 0;
	res.port = (port & 0xe0) >> 5;
	return res;
}

// smuc

// All in TR-DOS space: 1x?11xxx 101xx?10. A15, A13 and A2 pick the register,
// A8-A10 the drive's one; the rest is not decoded.
#define SMUC_VER	0x0000		// #5FBA
#define SMUC_REV	0x0004		// #5FBE
#define SMUC_VFDD	0x2000		// #7FBA
#define SMUC_PIC	0x2004		// #7xBE, an i8259 socket
#define SMUC_RTC	0x8000		// #DFBA
#define SMUC_IDEHI	0x8004		// #D8BE
#define SMUC_SYS	0xa000		// #FFBA
#define SMUC_IDE	0xa004		// #F8BE..#FFBE

ataAddr ide_smuc_decode(int port, int dosen, int wr) {
	ataAddr res;
	res.iorq = (((port & 0x18e3) == 0x18a2) && dosen) ? 1 : 0;
	res.high = 0;
	res.hdd = 0;
	res.port = port & 0xa004;
	if (res.port == SMUC_IDE) {
		res.hdd = 1;
		res.port = (port & 0x700) >> 8;
	} else if (res.port == SMUC_IDEHI) {
		res.hdd = 1;
		res.high = 1;
		res.port = HDD_DATA;
	}
	return res;
}

// SYS bit 7 puts the drive's CS1 block where CS0 is: only its register 6,
// alternate status / device control, is there
static int smuc_ide_reg(IDE* ide, ataAddr adr) {
	if (adr.high || !(ide->smuc.sys & 0x80)) return adr.port;
	return (adr.port == 6) ? HDD_ASTATE : -1;
}

// Version, revision, the empty PIC socket, the virtual FDD's spare bits and the
// IDE reset are UnrealSpeccy's, which ProfROM works with (MAME differs); no SMUC
// schematic has been found to check them
int ide_smuc_rd(IDE* ide, ataAddr adr) {
	int res = 0xff;
	if (adr.hdd) {
		int reg = smuc_ide_reg(ide, adr);
		if (reg >= 0) res = ide_ata_rd(ide, reg, adr.high);
	} else {
		switch (adr.port) {
			case SMUC_VER: res = 0x3f; break;
			case SMUC_REV: res = 0x57; break;
			case SMUC_SYS: res = nvRd(ide->smuc.nv) ? 0xff : 0xbf; break;	// b6: SDA
			case SMUC_VFDD: res = ide->smuc.fdd | 0x37; break;
			case SMUC_PIC: res = 0x57; break;
			case SMUC_RTC: res = cmos_rd(ide->smuc.cmos, CMOS_DATA); break;
		}
	}
	return res;
}

void ide_smuc_wr(IDE* ide, ataAddr adr, int val) {
	if (adr.hdd) {
		int reg = smuc_ide_reg(ide, adr);
		if (reg >= 0) ide_ata_wr(ide, reg, adr.high, val);
	} else {
		switch (adr.port) {
			case SMUC_SYS:
				if (val & 1) ideReset(ide);
				ide->smuc.sys = val;
				nvWr(ide->smuc.nv, val & 0x10, val & 0x40);		// sda,scl
				break;
			case SMUC_VFDD:
				ide->smuc.fdd = val & 0xc8;
				break;
			case SMUC_RTC:
				cmos_wr(ide->smuc.cmos, (ide->smuc.sys & 0x80) ? CMOS_DATA : CMOS_ADR, val);
				break;
		}
	}
}

// profi

ataAddr ide_profi_decode(int port, int dosen, int wr) {
	ataAddr res;
	if (wr) port ^= 0x20;	// wr: eb -> cb; wr 0eb<->0cb; now rd/wr 0EB is data high
	res.iorq = (((port & 0x00ff) == 0x00cb) || ((port & 0x7ff) == 0xeb)) ? 1 : 0;
	res.port = (port & 0x700) >> 8;
	if (port == 0x06ab) {		// no drive register
		res.hdd = 0;
		res.iorq = 1;
		res.port = 0xff;
	} else {
		res.hdd = 1;
	}
	res.high = ((port & 0x7ff) == 0xeb) ? 1 : 0;
	return res;
}

// others

ataAddr ideDecoder(IDE* ide, int port, int dosen, int wr) {
	ataAddr res;
	res.port = 0xff;
	res.iorq = 0;
	res.hdd = 0;
	res.high = 0;
	if (ide->core) {
		if (ide->core->decode) {
			res = ide->core->decode(port, dosen, wr);
		}
	}
	return res;
}

int ideIn(IDE* ide, int port, int* val, int dosen) {
	ataAddr adr = ideDecoder(ide, port, dosen, 0);
	if (!adr.iorq) return 0;
	int res = 0xff;
	if (ide->core) {
		if (ide->core->read)
			res = ide->core->read(ide, adr);
	}
	*val = res;
	return 1;
}

// nedoos: write to 11:10 (nemo mode)
int ideOut(IDE* ide, int port, int val,int dosen) {
	ataAddr adr = ideDecoder(ide,port,dosen,1);
	if (!adr.iorq) return 0;
	if (ide->core) {
		if (ide->core->write)
			ide->core->write(ide, adr, val);
	}
	return 1;
}

void ideReset(IDE* ide) {
	ataReset(ide->master);
	ataReset(ide->slave);
	ide->curDev = ide->master;
}

IDECore ide_core_tab[] = {
	{IDE_NEMO,	ide_nemo_decode,	ide_common_rd,	ide_common_wr},
	{IDE_NEMOA8,	ide_nemoa8_decode,	ide_common_rd,	ide_common_wr},
	{IDE_SMUC,	ide_smuc_decode,	ide_smuc_rd,	ide_smuc_wr},
	{IDE_ATM,	ide_atm_decode,		ide_common_rd,	ide_common_wr},
	{IDE_NEMO_EVO,	ide_nemoevo_decode,	ide_nemoevo_rd,	ide_nemoevo_wr},
	{IDE_PROFI,	ide_profi_decode,	ide_common_rd,	ide_common_wr},
	{IDE_NONE,	NULL,			NULL,		NULL}
};

IDECore* find_ide_core(int id) {
	IDECore* itm = ide_core_tab;
	while ((itm->id != id) && (itm->id != IDE_NONE))
		itm++;
	return itm;
}

void ide_set_type(IDE* ide, int id) {
	IDECore* core = find_ide_core(id);
	if (core) {
		ide->type = id;
		ide->core = core;
	} else {
		ide->type = IDE_NONE;
		ide->core = NULL;
	}
	ide_vfat_port(ide);
}

// reopening is ide_mount()'s job: only it knows a path may be a folder
void ideCloseFiles(IDE* ide) {
	ataCloseFile(ide->master);
	ataCloseFile(ide->slave);
}

#include <stdio.h>
#include <string.h>

#include "fdc.h"

int fdcFlag = 0;

// dummy (none)

void dumReset(DiskIF* dif) {}
int dumIn(DiskIF* dif, int port, int* res,int dos) {return 0;}
int dumOut(DiskIF* dif, int port, int val,int dos) {return 0;}
void dumSync(DiskIF* dif, int ns) {}

// overall

void fdcSync(FDC* fdc, int ns) {
	if (fdc->plan == NULL) return;	// fdc does nothing
	fdc->wait -= ns;
	fdc->tns += ns;
	while ((fdc->wait < 0) && (fdc->plan != NULL)) {
		if (fdc->plan[fdc->pos] != NULL) {
			fdc->plan[fdc->pos](fdc);
		} else {
			fdc->plan = NULL;
		}
	}
}

void dhwSync(DiskIF* dif, int ns) {
	fdcSync(dif->fdc, ns);
}

void dhw_irq(int id, void* p) {
	DiskIF* dif = p; // (DiskIF*)p;
	if (dif->hw->irq && (dif->fdc->dma || dif->inten)) {
		dif->hw->irq(dif, id);
	}
}

void fdc_set_hd(FDC* fdc, int hd) {
	fdc->hd = !!hd;
	fdc->bytedelay = hd ? 16000 : 32000;
	flp_set_hd(fdc->flop[0], hd);
	flp_set_hd(fdc->flop[1], hd);
	flp_set_hd(fdc->flop[2], hd);
	flp_set_hd(fdc->flop[3], hd);
}

// BDI (VG93)

void vgReset(FDC*);
unsigned char vgRead(FDC*, int);
void vgWrite(FDC*, int, unsigned char);
void vgSetMR(FDC*, int);

int bdiGetPort(int port) {
	int res = 0;
	if ((port & 0x9f) == 0x9f) {			// 1xxxxx11 : bdi system port
		res = BDI_SYS;
	} else {
		switch (port & 0xff) {			// 0xxxxx11 : vg93 registers
			case 0x1f: res = FDC_COM; break;	// 000xxx11
			case 0x3f: res = FDC_TRK; break;	// 001xxx11
			case 0x5f: res = FDC_SEC; break;	// 010xxx11
			case 0x7f: res = FDC_DATA; break;	// 011xxx11
		}
	}
	return res;
}

int bdiIn(DiskIF* dif, int port, int* res, int dos) {
	if (!dos) return 0;
	port = bdiGetPort(port);
//	printf("in BDI port %.2X\n",port);
	if (port == 0) {
		return 0;
	} else if (port == BDI_SYS) {
		*res = (dif->fdc->irq ? 0x80 : 0x00) | (dif->fdc->drq ? 0x40 : 0x00);
	} else {
		*res = vgRead(dif->fdc, port);
	}
	return 1;
}

int bdiOut(DiskIF* dif, int port, int val, int dos) {
	if (!dos) return 0;
	port = bdiGetPort(port);
	if (port == 0) {
		return 0;
	} else if (port == BDI_SYS) {
		dif->fdc->flp = dif->fdc->flop[val & 3];	// select floppy
		vgSetMR(dif->fdc,(val & 0x04) ? 1 : 0);		// master reset
		dif->fdc->block = (val & 0x08) ? 1 : 0;
		dif->fdc->side = (val & 0x10) ? 0 : 1;		// side
		dif->fdc->mfm = (val & 0x40) ? 1 : 0;
	} else {
		vgWrite(dif->fdc, port, val);
	}
	return 1;
}

void bdiReset(DiskIF* dif) {
	vgReset(dif->fdc);
	bdiOut(dif, 0xff, 0x00, 1);
}

void bdiSync(DiskIF* dif, int ns) {
	fdcSync(dif->fdc, ns);
}

// +3DOS (uPD765)

void uReset(FDC*);
unsigned char uRead(FDC*, int);
void uWrite(FDC*, int, unsigned char);
void uTCount(FDC*);

int pdosGetPort(int p) {
	int port = -1;
	if ((p & 0xf002) == 0x2000) port = 0;		// A0 input of upd765
	if ((p & 0xf002) == 0x3000) port = 1;		// 0:status(r), 1:data(rw)
	return port;
}

int pdosIn(DiskIF* dif, int port, int* res, int dos) {
	port = pdosGetPort(port);
	if (port < 0) return 0;
//	printf("in %.4X\n",port);
	*res = uRead(dif->fdc, port);
	return 1;
}

int pdosOut(DiskIF* dif, int port, int val, int dos) {
	port = pdosGetPort(port);
	if (port < 0) return 0;
	uWrite(dif->fdc, port, val);
	return 1;
}

void pdosReset(DiskIF* dif) {
//	dif->inten = 0;
//	dif->lirq = 0;
	uReset(dif->fdc);
}

// TODO: interrupt on flp ready signal changed (on a disk change)
void pdosSync(DiskIF* dif, int ns) {
	dhwSync(dif, ns);
/*
	if (dif->fdc->flp->dwait > 0) {
		dif->fdc->flp->dwait -= ns;
		if (dif->fdc->flp->dwait < 0) {
			dif->fdc->flp->dwait = 0;
			if (dif->fdc->flp->door != dif->fdc->flp->insert) {
				dhw_irq(IRQ_FDD_RDY, dif);
				dif->fdc->flp->door = dif->fdc->flp->insert;
//				printf("insert:%i\tdoor:%i\n",dif->fdc->flp->insert,dif->fdc->flp->door);
			}
		}
	}
*/
}


// common

static DiskHW dhwTab[] = {
	{DIF_NONE,dumReset,dumIn,dumOut,dumSync,NULL,NULL},
	{DIF_BDI,bdiReset,bdiIn,bdiOut,dhwSync,NULL,NULL},
	{DIF_P3DOS,pdosReset,pdosIn,pdosOut,pdosSync,NULL,NULL},		// upd765 (+3dos)
	{DIF_END,NULL,NULL,NULL,NULL,NULL,NULL}
};

DiskHW* findDHW(int id) {
	DiskHW* itm = dhwTab;
	while ((itm->id != id) && (itm->id != DIF_END))
		itm++;
	return (itm->id == DIF_END) ? NULL : itm;
}

void difSetHW(DiskIF* dif, int type) {
	dif->hw = findDHW(type);
	if (!dif->hw)
		dif->hw = findDHW(DIF_NONE);
	dif->type = dif->hw->id;
	dif->fdc->upd = (dif->hw->id == DIF_P3DOS) ? 1 : 0;	// difference between upd765 & i8272
}

FDC* fdc_create(cbirq cb, void* p) {
	FDC* fdc = malloc(sizeof(FDC));
	memset(fdc, 0x00, sizeof(FDC));
	fdc->wait = -1;
	fdc->plan = NULL;
	fdc->xptr = p;
	fdc->xirq = cb;
	fdc->debug = 0;
	fdc_set_hd(fdc, 0);
	return fdc;
}

void fdc_destroy(FDC* fdc) {
	fdc->flp = NULL;
	free(fdc);
}

void fdc_set_irqn(FDC* fdc, int in, int irn, int iwn) {
	fdc->irqn = in;
	fdc->irqrn = irn;
	fdc->irqwn = iwn;
}

void fdc_set_flps(FDC* fdc, Floppy* fa, Floppy* fb, Floppy* fc, Floppy* fd) {
	fdc->flop[0] = fa;
	fdc->flop[1] = fb;
	fdc->flop[2] = fc;
	fdc->flop[3] = fd;
}

void dif_align_flps(DiskIF* dif, FDC* fdc, int n0, int n1, int n2, int n3) {
	fdc->flop[0] = dif->flp[n0 & 3];
	fdc->flop[1] = dif->flp[n1 & 3];
	fdc->flop[2] = dif->flp[n2 & 3];
	fdc->flop[3] = dif->flp[n3 & 3];
	fdc->flp = fdc->flop[0];
}

DiskIF* difCreate(int type, cbirq cb, void* p) {
	DiskIF* dif = (DiskIF*)malloc(sizeof(DiskIF));
	dif->fdc = fdc_create(cb, p);
	fdc_set_irqn(dif->fdc, IRQ_FDC, IRQ_FDC_RD, IRQ_FDC_WR);
	for (int i = 0; i < 4; i++) {
		dif->flp[i] = flpCreate(i, dhw_irq, dif);
		dif->fdc->flop[i] = dif->flp[i];
	}
	dif->fdc->flp = dif->fdc->flop[0];
	difSetHW(dif, type);
	return dif;
}

void difDestroy(DiskIF* dif) {
	flpDestroy(dif->flp[0]);
	flpDestroy(dif->flp[1]);
	flpDestroy(dif->flp[2]);
	flpDestroy(dif->flp[3]);
	fdc_destroy(dif->fdc);
	free(dif);
}

void difReset(DiskIF* dif) {
	dif->inten = 1;
	dif->hw->reset(dif);
}

void difSync(DiskIF* dif, int ns) {
	dif->hw->sync(dif, ns);
	flp_sync(dif->fdc->flp, ns);
}

int difOut(DiskIF* dif, int port, int val, int dos) {
	return dif->hw->out(dif,port,val,dos);
}

int difIn(DiskIF* dif, int port, int* res, int dos) {
	return dif->hw->in(dif,port,res,dos);
}

void difTerminal(DiskIF* dif) {
	if (dif->hw->term)
		dif->hw->term(dif);
}

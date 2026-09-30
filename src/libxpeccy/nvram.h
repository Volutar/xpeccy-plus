#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 24C16: a 2K I2C EEPROM, the NVRAM on SMUC
#define NV_SIZE	0x800

enum {
	NV_IDLE	= 0,
	NV_RCV_CMD,
	NV_RCV_ADR,
	NV_RCV_DATA,
	NV_SEND_DATA,
	NV_RD_ACK
};

typedef struct {
	int state;
	int adr;
	unsigned char datain;
	unsigned char dataout;
	int bitsin;
	int bitsout;
	unsigned sda:1;		// the lines as the host last left them
	unsigned scl:1;
	unsigned out:1;		// what the chip puts on SDA
	unsigned outz:1;	// ...or SDA released, and the host's own level reads back
	unsigned char mem[NV_SIZE];
} nvRam;

nvRam* nvCreate();
void nvDestroy(nvRam*);

void nvWr(nvRam*,int,int);	// sda,scl
int nvRd(nvRam*);

#ifdef __cplusplus
}
#endif

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "defines.h"

#define TRKLEN_DD	6250
#define TRKLEN_HD	17700
// disk type
#define DISK_TYPE_TRD	1

// error
enum {
	ERR_MANYFILES = 1,
	ERR_NOSPACE,
	ERR_SHIT,
	ERR_SIZE
};

// step direction
enum {
	FLP_BACK = 0,
	FLP_FORWARD
};

typedef struct {
	unsigned char mark;	// f8/fb
	unsigned char size;	// code 0..3
	unsigned short pos;	// position in track
} xSecPos;

typedef void(*cbflpirq)(int, void*);

// A weak sector: bits on the disk that read differently each time. An image
// keeps a few reads of it; one is served per revolution, in turn.
typedef struct {
	int pos;		// its first data byte on the track
	int len;		// bytes in one read
	int copies;
	int made;		// the reads after the first are made up: the image kept one
	unsigned char* data;	// copies x len
} flpWeak;

typedef struct {
	unsigned char byte[TRKLEN_HD];
	unsigned char field[TRKLEN_HD];
	int map[256];		// position of sector n = 1+
	// What an image file said of the track beyond its bytes, dropped once the
	// track is written: its weak sectors, and its block as the file held it,
	// to be saved back the same
	flpWeak* weak;
	int nweak;
	unsigned char* raw;
	int rawlen;
} flpTrack;

typedef struct {
	unsigned trk80:1;	// fdd is 80T
	unsigned doubleSide:1;	// fdd is DS
	unsigned motor:1;	// fdd motor is on
	unsigned virt:1;	// fdd is virtual
	unsigned fitted:1;	// a drive is on the cable at all
	unsigned insert:1;	// disk inserted
	unsigned door:1;	// door closed (auto close some time after insert)
	unsigned protect:1;	// disk is write protected
	unsigned changed:1;	// disk is changed
	unsigned index:1;	// disk index impulse

	int dwait;		// delay between disk inserting and door closing
	unsigned rd:1;
	unsigned wr:1;

	cbflpirq xirq;		// send signal to fdc
	void* xptr;

	int id;
	unsigned char trk;
	unsigned char field;
	int pos;
	int revs;		// revolutions seen: which read of a weak sector is under the head
	int trklen;		// 12500 HD, 6250 DD
	char* path;
	int cyls;		// cylinders the inserted image holds, 0 when blank or unknown
	// 256 tracks in an allocation of their own, past what xstate keeps of the
	// drive: a disk read elsewhere goes in by this pointer
	flpTrack* data;
} Floppy;

Floppy* flpCreate(int, cbflpirq, void*);
void flpDestroy(Floppy*);

void flp_set_path(Floppy*, const char*);
void flp_set_hd(Floppy*, int);
void flp_sync(Floppy*, int);

void flp_insert(Floppy*, const char*);
void flp_eject(Floppy*);
void flp_take_disk(Floppy* flp, Floppy* disk);
int flp_trk0(Floppy*);		// the head is at track 0, as the drive's sensor says

unsigned char flpRd(Floppy*, int);
void flpWr(Floppy*, int, unsigned char);
int flpNext(Floppy*,int);		// return 1 if index strobe
void flpPrev(Floppy*,int);
void flpStep(Floppy*,int);
int flp_last_cyl(Floppy*);		// the last cylinder the head reaches
int flp_wide(Floppy*);			// cylinders of an image past 86, else 0

int flp_format_trk(Floppy* flp, int trk, int spt, int slen, char* data);
void flp_set_interleave(int);
int flp_get_interleave();

void flpFillFields(Floppy*,int,int);

void flpPutTrack(Floppy*,int,unsigned char*,int);
void flpGetTrack(Floppy*,int,unsigned char*);
void flpGetTrackFields(Floppy*,int,unsigned char*);

void flpClearTrack(Floppy*,int);
void flpClearDisk(Floppy*);
void flp_track_written(Floppy*, int);	// what the image said beyond the track's bytes is gone
void flp_add_weak(Floppy*, int tr, int pos, int len, int copies, const unsigned char* data, int made);
void flp_keep_raw(Floppy*, int tr, const unsigned char* data, int len);

unsigned short getCrc(unsigned char*, int);

// the bytes a size code means to the uPD765: past 7 its counter stops at 16K
static inline int flp_seclen(int n) {
	return (n > 7) ? 0x4000 : (0x80 << n);
}

#ifdef __cplusplus
}
#endif

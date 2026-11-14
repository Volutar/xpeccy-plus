#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "../spectrum.h"


#if defined(__linux) || defined(__APPLE__) || defined(__BSD)
	#define ENVHOME "HOME"
	#define SLASH "/"
	#define SLSH '/'
#elif defined(_WIN32)
	#define ENVHOME "HOMEPATH"
	#define SLASH "\\"
	#define SLSH '\\'
#endif

enum {
	ERR_OK = 0,
	ERR_CANCEL,
	ERR_CANT_OPEN,		// can't open file

	ERR_RZX_SIGN,		// rzx signature error
	ERR_RZX_CRYPT,		// rzx is crypted
	ERR_RZX_UNPACK,		// rzx unpacking error
	ERR_RZX_REC,		// rzx recording: the machine cannot be saved, or no memory

	ERR_Z80_HW,		// Z80 hw mode not supported

	ERR_TAP_DATA,		// can't save tap because of not-standart blocks
	ERR_TAP_EMPTY,		// 0 blocks at tape

	ERR_TZX_SIGN,		// tzx signature error
	ERR_TZX_UNKNOWN,	// tzx unsupported block

	ERR_WAV_HEAD,		// wrong wave header
	ERR_WAV_FORMAT,		// unsupported wav format

	ERR_TRD_LEN,		// incorrect trd lenght
	ERR_TRD_SIGN,		// not trd image
	ERR_TRD_SNF,		// can't save trd: wrong disk structure
	ERR_NOTRD,		// this is not trd disk

	ERR_HOB_CANT,		// can't create hobeta @ disk

	ERR_UDI_SIGN,		// udi signature errror

	ERR_FDI_SIGN,		// fdi signature error
	ERR_FDI_HEAD,		// wrong fdi heads count

	ERR_SCL_SIGN,		// scl signature error
	ERR_SCL_MANY,		// too many files in scl

	ERR_DSK_SIGN,		// dsk signature error

	ERR_RAW_LONG,		// raw file too long

	ERR_TD0_SIGN,		// td0 signature error
	ERR_TD0_TYPE,		// unsupported td0 type
	ERR_TD0_VERSION,	// unsupported version ( <20)
	ERR_NO_DRIVE,		// the machine has no such drive

	ERR_SPG_SIGN,		// not an spg
	ERR_SPG_VERSION,	// an spg of a version not supported (0.x)

	ERR_SZX_SIGN,		// not a zx-state file
	ERR_SZX_HW,		// a machine this emulator does not have
	ERR_SZX_DATA,		// no cpu or no memory in it

	ERR_CANT_SAVE,		// the format has no writer
	ERR_FILE_TYPE,		// the extension is none the machine knows
	ERR_SNAP_SHORT,		// a snapshot shorter than its own header says it is
	ERR_NEX_HW,		// a .nex on a machine that is not a ZX Spectrum Next
	ERR_NEX_CARD,		// a .nex with no folder in the SD card slot to boot NextZXOS from
	ERR_NEX_NAME,		// a .nex whose name cannot be typed
	ERR_SNAP_NEXT,		// a snapshot of the Next in .sna or .z80: only .szx keeps its state
	ERR_DSK_WIDE		// a disk of more cylinders than a .dsk is written with
};

// spg

#ifdef __cplusplus
extern "C" {
#endif

// wav header

#pragma pack (push, 1)

typedef struct {
	char chunkId[4];		// "RIFF"
	unsigned int chunkSize;
	char format[4];			// "WAVE"
	char subchunk1Id[4];		// "fmt "
	unsigned int subchunk1Size;	// 16
	unsigned short audioFormat;	// 1 = PCM
	unsigned short numChannels;
	unsigned int sampleRate;
	unsigned int byteRate;		// sampleRate * numChannels * bitsPerSample/8
	unsigned short blockAlign;	// numChannels * bitsPerSample/8
	unsigned short bitsPerSample;
	char subchunk2Id[4];		// "data"
	unsigned int subchunk2Size;
} wavHead;

#pragma pack(pop)

// what the tape export writes
typedef struct {
	int rate;	// samples per second, 0 = pick one from the shortest pulse
	int bits;	// 8 or 16
	int level;	// per cent of full scale
	int lead;	// ms of silence before the tape
	int tail;	// ms of silence after it
} wavExport;

// disk specific operations

typedef struct {
	unsigned char name[8];
	unsigned char ext;
	unsigned char lst,hst;
	unsigned char llen,hlen;
	unsigned char slen;
	unsigned char sec;
	unsigned char trk;
} TRFile;

void diskClear(Floppy*);
void trd_format(Floppy*);
void diskFormTrack(Floppy*,int,Sector*,int);
int diskFormFits(Floppy*, Sector*, int);	// 1 if diskFormTrack lays all of them down
void diskFormTRDTrack(Floppy*,int,unsigned char*);

int diskGetType(Floppy*);

unsigned char* diskGetSectorDataPtr(Floppy*,unsigned char,unsigned char);
int diskGetSectorData(Floppy*,unsigned char,unsigned char,unsigned char*,int);
int diskGetSectorsData(Floppy*,unsigned char,unsigned char,unsigned char*,int);
int diskPutSectorData(Floppy*,unsigned char,unsigned char,unsigned char*,int);

int diskCreateDescriptor(Floppy*,TRFile*);
int diskCreateFile(Floppy*, TRFile, unsigned char*, int);
int diskGetTRCatalog(Floppy*,TRFile*);
TRFile diskGetCatalogEntry(Floppy*, int);
TRFile diskMakeDescriptor(const char*, char, int, int);
int trd_file_len(TRFile*, unsigned char*, int*);

// +3DOS: a CP/M 2.2 file system, laid out by the spec in the boot sector or by
// the Amstrad CPC system and data formats, which +3DOS reads as well

#define P3_MAXBLK	1024

enum {
	P3_PLUS3 = 0,		// +3 or PCW single sided, the spec's format 0
	P3_CPC_SYS,
	P3_CPC_DATA,
	P3_PCW_DS
};

enum {
	P3_SIDES_ONE = 0,
	P3_SIDES_ALT,		// logical track 2n+h is cylinder n, head h
	P3_SIDES_SUCC		// side 0 first, then side 1
};

typedef struct {
	int format;		// P3_* above
	int sides;		// P3_SIDES_*
	int tracks;		// per side
	int spt;
	int first;		// the id of the first sector on a track
	int seclen;
	int reserved;		// tracks before the directory
	int blklen;
	int dirblk;		// blocks of directory, from block 0
	int blocks;		// in the data area
} p3Disk;

typedef struct {
	unsigned char user;
	char name[9];		// attribute bits stripped, spaces kept
	char ext[4];
	int records;		// of 128 bytes
	int nblk;
	int used;		// blocks taken, holes not counted
	unsigned short blk[P3_MAXBLK];
	int hdr;		// has a valid PLUS3DOS header
	unsigned char type;	// from it: 0 program, 1 numbers, 2 chars, 3 bytes
	unsigned short len;
	unsigned short param1;
	unsigned short param2;
} p3File;

int p3_disk(Floppy*, p3Disk*);
int p3_boots(Floppy*);		// the +3's Loader boots it: sector 1 of track 0 sums to 3
int p3_catalog(Floppy*, p3Disk*, p3File*, int);
int p3_read(Floppy*, p3Disk*, p3File*, unsigned char*, int);
int p3_dir_raw(Floppy*, p3Disk*, unsigned char*, int);
int p3_free_blocks(p3Disk*, p3File*, int);
int p3_block_track(p3Disk*, int, int*, int*);
int p3_logical(p3Disk*, int, int);
int p3_save_raw(Floppy*, p3Disk*, p3File*, const char*);
int p3_save_hobeta(Floppy*, p3Disk*, p3File*, const char*);

// common

int fgeti(FILE*);
int fgett(FILE*);
int fgetw(FILE*);
void fputi(int, FILE*);
void fputw(unsigned short, FILE*);

size_t fgetSize(FILE*);
FILE* fopen_tmp(void);
unsigned int freadLen(FILE*,int);
void fputwLE(FILE*, unsigned short);

void putint(unsigned char*, unsigned int);
void cutSpaces(char*);
void loadBoot(Computer*, const char*, int);
void boot_read(Floppy*, const char*);

unsigned short swap16(unsigned short);
unsigned int swap32(unsigned int);

// rzx

int loadRZX(Computer*, const char*, int);
int rzxGetHardware(const char*);
void rzxGetFrame(Computer*);
void rzx_frame_end(Computer*);		// the frame played ends: the next one in
void rzx_play_step(Computer*, int t);	// after an exec, a Next recording: its own frame ends

// one block of a recording as the file has it
typedef struct {
	int id;			// 0x10 creator, 0x20/0x21 signature, 0x30 snapshot, 0x80 input
	int size;		// bytes in the file, the 5 of the block's own head included
	int frame;		// frames recorded before it
	int flags;		// snapshot: b0 external, b1 packed; input: b0 encrypted, b1 packed
	int frames;		// input: frames in the block
	int tstart;		// input: T of the frame it starts in
	int usl;		// snapshot: bytes unpacked
	int hw;			// snapshot: SNAP_HW_*
	int major;		// creator: program version
	int minor;
	char ext[5];		// snapshot: its format, as the file names it
	char text[256];		// creator: program name; external snapshot: its file name
	char custom[256];	// creator: what else it says, as text
} rzxBlock;

typedef struct {
	int major;		// format version
	int minor;
	int flags;		// b0: signed
	int frames;		// in every input block
	int snaps;		// snapshot blocks, the first one included
	int junk;		// bytes after the last whole block
	int count;
	rzxBlock* blk;
} rzxInfo;

int rzx_info(const char*, rzxInfo*, int first);
void rzx_info_free(rzxInfo*);
rzxBlock* rzx_info_add(rzxInfo*);
int rzx_snap_hardware(int type, int usl, const unsigned char* head, int n);	// SNAP_HW_*
int rzx_seek(Computer*, int frame);

// A snapshot block's flags: b0 external, b1 packed, and two more. b30 is ours:
// taken between two INTs with interrupts on, so the playback raises none after
// it. b31 is a bookmark, a point to roll back to - Spectaculator's mark for its
// rollback points, which finalizing drops.
#define RZX_SNAP_NOINT	(1u << 30)
#define RZX_SNAP_MARK	(1u << 31)
int rzxGetSnapType(char*);

// recording (rzxrec.c)
int rzx_rec_start(Computer*);
int rzx_rec_take_over(Computer*);	// a recording being played goes on as this one
void rzx_rec_bookmark(void);		// one at the next frame's end
int rzx_rec_rollback(Computer*);	// to the last bookmark: frames back, -1 none
int rzx_rec_info(Computer*, rzxInfo*, const char* name, int major, int minor, const char* custom);	// the blocks so far, as rzx_info()
void rzx_rec_pre(Computer*);		// before an exec, when a join or a bookmark is wanted
extern int rzx_rec_marking;
void rzx_rec_stop(Computer*);
void rzx_rec_in(Computer*, int);
void rzx_rec_step(Computer*, int t);
void rzx_rec_touch(void);		// the machine changed from outside: a join before the next opcode
extern int rzx_rec_touched;
int rzx_rec_frames(Computer*);
int rzx_rec_joins(Computer*);
// what has been recorded, copied out to be written while the machine runs on
typedef struct rzxRecImage rzxRecImage;
rzxRecImage* rzx_rec_take(Computer*);
int rzx_rec_image_frames(rzxRecImage*);
int rzx_rec_image_write(rzxRecImage*, const char* path, const char* name, int major, int minor, const char* custom, int finalize);
rzxRecImage* rzx_rec_image_read(const char* path, int* err);
void rzx_rec_image_free(rzxRecImage*);

// memory (snapshot)

// what a snapshot was taken on, read from the file without loading it
enum {
	SNAP_HW_UNKNOWN = 0,
	SNAP_HW_48K,
	SNAP_HW_128K,
	SNAP_HW_PLUS2,
	SNAP_HW_PLUS2A,
	SNAP_HW_PLUS3,
	SNAP_HW_PENTAGON,
	SNAP_HW_SCORPION,
	SNAP_HW_PENT512,
	SNAP_HW_PENT1024,
	// a machine only our own files name: this core (HW_*) and nothing else
	SNAP_HW_CORE = 0x100
};

// or'ed into what a file's probe says of a 128K snapshot: SNAP_EDITOR, the 128
// editor is paged in, so a machine needs that editor; SNAP_IN_ROM, it was running
// the ROM, so it goes on only on the model it names
#define SNAP_EDITOR	0x10000
#define SNAP_IN_ROM	0x20000
#define SNAP_UNSURE	0x80000		// tied to its ROM, but which model's the file does not tell
#define SNAP_HW_OF(snap)	((snap) & ~(SNAP_EDITOR | SNAP_IN_ROM | SNAP_UNSURE))

int snapHwRuns(int snap, int hwid, int model);	// model: the machine's snapModel
int snapHwIs(int snap, int hwid);
int snap_rom_flags(int pc, int p7ffd);		// SNAP_EDITOR, SNAP_IN_ROM of a 128K snapshot
void snap_plus3_basic48(Computer* comp, int snap, int p7ffd);	// 1FFD for a file that has none

int loadSZX(Computer*, const char*, int);
int loadSZX_buf(Computer*, const unsigned char*, size_t);
int szxCanSave(Computer*);
int szxGetHardware(const char*);
int szx_hardware_of(const unsigned char*, size_t);
// the machine id a file of ours names, empty for any other file
void szx_machine_of(const char* name, char* id, size_t idsize);
// what the file's creator block is signed with
void szx_set_creator(const char* name, int major, int minor);
// what a saved file links to: the tape and the four drives (NULL: nothing)
void szx_set_links(const char* tape, const char* const* disks);

int z80_hardware_of(const unsigned char*, int);
int loadZ80(Computer*,const char*, int);
int loadZ80_f(Computer*, FILE*);
int saveZ80(Computer*, const char*, int);
int z80GetHardware(const char*);

int loadSNA(Computer*,const char*, int);
int saveSNA(Computer*, const char*, int);
int loadSNA_f(Computer*, FILE*, size_t);
int snaGetHardware(const char*);
int sna_editor_of(const char* name, const unsigned char* const* eds, int count);
int sna_plus3_drive(const char* name);
int sna_hardware_of(size_t);

int loadSPG(Computer*,const char*, int);

int load_ima(Computer*, const char*, int);
int ima_read(Floppy*, const char*);

// tape

int loadTAP(Computer*,const char*, int);
int tap_read(Tape*, const char*);
int saveTAP(Computer*,const char*, int);
TapeBlock tapDataToBlock(char*,int,int*);

int loadTZX(Computer*,const char*, int);
int tzx_read(Tape*, const char*);

int loadWAV(Computer*, const char*, int);
int wav_read(Tape*, const char*);
int saveWAV(Computer*, const char*, int);
int saveWAVopt(Computer*, const char*, wavExport*);
wavExport wav_export_default(void);
const int* wav_export_rates(void);	// 0 terminated
const int* wav_export_rates(void);	// 0 terminated
int wav_export_rate(Computer*);

// disk

int loadRaw(Computer*,const char*, int);
int saveRawFile(Floppy*,int,const char*);

int loadHobeta(Computer*,const char*,int);
int hobeta_read(Floppy*, const char*);
int saveHobetaFile(Floppy*,int,const char*);
int saveHobeta(TRFile,char*,const char*);

int loadSCL(Computer*,const char*,int);
int scl_read(Floppy*, const char*);
int saveSCL(Computer*,const char*,int);

int loadTRD(Computer*,const char*,int);
int trd_read(Floppy*, const char*);
int trd_write(Floppy*, const char*);
int saveTRD(Computer*,const char*,int);

int loadUDI(Computer*,const char*,int);
int udi_read(Floppy*, const char*);
int saveUDI(Computer*,const char*,int);
// a disk as a UDI file in memory (malloc), every track as it is
unsigned char* udi_build(Floppy*, size_t* len);

int loadFDI(Computer*,const char*,int);
int fdi_read(Floppy*, const char*);

int loadDSK(Computer*,const char*,int);
int dsk_read(Floppy*, const char*);
int saveDSK(Computer*,const char*,int);
unsigned char* dsk_build(Floppy*, size_t* len);	// the .dsk file in memory, malloc'd

int loadTD0(Computer*,const char*,int);
int td0_read(Floppy*, const char*);

// cartridge

int loadSlot(Computer*,const char*, int);

#ifdef __cplusplus
}
#endif

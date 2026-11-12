#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

// FAT32 volume synthesized from a host directory tree.
// The caller builds the tree with vfat_add (a scanner lives in xcore, so this
// file stays free of platform code), calls vfat_build, then asks for sectors.
// A volume made writable takes the guest's writes and carries them over to the
// host folder (vfat_wr.c).

#define VF_SECSIZE	512
#define VF_MAXNAME	255
#define VF_MAXDENTS	65536		// FAT's limit on the entries of one directory
#define VF_MAXFILE	0xfffffffeLL	// and on the size of a file
#define VF_FULL		-2		// vfat_add: the directory has no room for the entry
#define VF_TMPMARK	".xpeccy-sync~"	// names of the host side's temporary files: a scan skips them

// IDE port sets the HDD boot sector comes in
enum {
	VF_PORT_NEMO = 0,
	VF_PORT_ATM,
	VF_HDDBOOTS
};

// What the host folder is reached through; every path is UTF-8. Each call but open
// returns 0 when done.
typedef struct {
	void* ctx;
	FILE* (*open)(void* ctx, const char* path, const char* mode);
	int (*mkdir)(void* ctx, const char* path);
	int (*rename)(void* ctx, const char* from, const char* to);	// never over an existing target
	int (*replace)(void* ctx, const char* from, const char* to);	// from takes to's place, to's old content gone for good
	int (*remove)(void* ctx, const char* path, int isdir);		// to the trash or for good, as the user set
	int (*truncate)(void* ctx, const char* path, long long size);
	int (*kind)(void* ctx, const char* path);			// 0 nothing there, 1 a file, 2 a folder
	int (*empty)(void* ctx, const char* path);			// 1 when a folder holds nothing
	void (*settime)(void* ctx, const char* path, unsigned int mtime);	// unix time, local
	int (*stat)(void* ctx, const char* path, long long* size, long long* mtime);	// unix time; 0 when it is there
	int (*exact)(void* ctx, const char* path);			// 1 when it is there spelled just so, case and all
} vfHost;

typedef struct {
	unsigned isdir:1;
	unsigned lfn:1;			// name doesn't fit 8.3, needs long name entries
	unsigned dead:1;		// gone from the volume and the folder
	unsigned pinned:1;		// from elsewhere (vfat_pin): the host side leaves it and all in it be
	unsigned cut:1;			// a pinned folder holding only the program's file of its folder, or nothing
	char* name;			// name shown to the guest, UTF-8
	char* hname;			// name in the host folder, UTF-8 (NULL for the root)
	char* hroot;			// a pinned folder's own host path, UTF-8
	char sname[12];			// generated 8.3 name, space padded, no dot
	unsigned int shash;		// hash of sname and parent, the key in vFat.stab
	int parent;			// owning directory, -1 for root
	int child;			// first child, -1 if none
	int last;			// last child, so appending doesn't walk the list
	int next;			// next sibling, -1 if none
	unsigned int size;		// file size in bytes
	unsigned int clust;		// first cluster, 0 if the node takes none
	unsigned int nclust;		// clusters occupied
	unsigned short date;		// last write, FAT format
	unsigned short time;
	unsigned int hmtime;		// the host file's own time and size as the scan or the last sync left them
	unsigned int hsize;
	char* rname;			// where its content went when it could not be put in place
	int ecnt;			// directory entries this node takes in its parent
	int dents;			// entries this directory holds so far, end marker included
} vfNode;

typedef struct vfWr vfWr;

typedef struct {
	vfNode* node;
	int nodes;
	int cap;
	int* stab;			// open addressing table of nodes by shash, -1 = free
	unsigned int scap;		// its size, a power of two
	int* order;			// nodes holding clusters, sorted by first cluster
	int ordcnt;

	unsigned char spc;		// sectors per cluster
	unsigned int volume;		// whole volume, in sectors
	unsigned int partStart;		// first sector of the partition
	unsigned int partSize;		// partition size, in sectors
	unsigned int reserved;		// reserved sectors before the FATs
	unsigned int fatSize;		// one FAT copy, in sectors
	unsigned int dataStart;		// first data sector, relative to partition
	unsigned int clusters;		// data clusters in the partition
	unsigned int used;		// clusters handed out
	unsigned int serial;
	unsigned char hddboot[VF_HDDBOOTS][VF_SECSIZE];	// IS-DOS boot sectors for LBA 2 and 3, per port set
	int hddboots;			// how many of them were found
	int hddport;			// which one to serve: VF_PORT_*, -1 for none

	char* root;			// the host folder, UTF-8
	vfHost host;			// how it is reached; open NULL: plain fopen
	int cnode;			// cached open file
	FILE* cfile;

	int writable;			// the guest's writes are taken
	int hold;			// and kept in memory: the folder waits for the user's word before it changes
	const char* frozen;		// non-NULL: the folder takes no more of them, they stay in memory; why (vfat_read_only)
	vfWr* wr;			// what they did, from the first one on
} vFat;

vFat* vfat_create(void);
// writes left to the host folder are carried over first
void vfat_free(vFat*);
// the host folder, and how to reach it (NULL: stdio); before vfat_build
void vfat_set_root(vFat*, const char* root, const vfHost* host);

// a folder of the tree that is somewhere else on the host: its files are read from there,
// and what the guest does in it stays in the volume
void vfat_pin(vFat*, int idx, const char* hostpath);
// a pinned folder of the root by its name as put on: its node, -1 if none
int vfat_pin_node(vFat*, const char* name);
// the entries a pinned folder holds, down its tree
int vfat_pin_entries(vFat*, int pin);
// the program's file in a pinned folder that holds only it (cut): its node, -1 if none
int vfat_pin_file(vFat*, int pin);
// name: UTF-8, also the host name; returns node index, VF_FULL if the parent
// directory has no room left, or -1 on failure
int vfat_add(vFat*, int parent, const char* name, unsigned int size, unsigned int mtime, int isdir);
// lay the tree out on a volume of at least minsec sectors; 0 on failure
int vfat_build(vFat*, unsigned int minsec);
// fill one 512-byte sector; 0 if the LBA is outside the volume
int vfat_read(vFat*, unsigned int lba, unsigned char* dst);

// take one 512-byte sector from the guest; 0 if refused (read only, outside)
int vfat_write(vFat*, unsigned int lba, const unsigned char* src);
// once a frame: the folder follows the guest's writes after a quiet spell; 1 when it did
int vfat_tick(vFat*);
// carry the writes over now; force: even mid-operation (unmount), skipping what is unfinished
int vfat_sync(vFat*, int force);
// changes the folder has not taken (a failed host operation, an unfinished guest one)
int vfat_pending(vFat*);
// 1 once the folder stopped following the guest (a format, a mass delete): the volume lives on in memory
int vfat_detached(vFat*);
// 1 the first time a sync finds the guest changed something in a pinned folder
int vfat_pin_news(vFat*);
// carry over what the guest wrote, unless the volume is on hold or no longer follows:
// before the folder is read again or let go
void vfat_flush(vFat*);
// what the user is to hear of, once each: 1 the folder stopped following (*why says why),
// 2 host operations failed (*errors, *why names the first few); 0 nothing new
int vfat_news(vFat*, const char** why, int* errors);
// a name of ours an unfinished sync left in the folder's top level, made visible again;
// returns the name it has now (malloc'd), NULL if it stays as it is
char* vfat_tmp_restore(vFat*, const char* name);
// 1 while the guest has written to a volume on hold
int vfat_waiting(vFat*);
// the user's yes on a volume on hold: the folder follows (a no is vfat_read_only)
void vfat_release(vFat*);
// read-only from now on, the volume in use: the folder stays as it is, and what the guest
// writes stays in memory, so its DOS goes on as it was. why: for the log
void vfat_read_only(vFat*, const char* why);

// what vfat_refresh did, or why it did nothing
enum {
	VFR_OK = 0,
	VFR_WRITES,		// the guest's writes have not reached the folder yet (on hold)
	VFR_STUCK,		// a sync could not carry some of them (vfat_stuck says what)
	VFR_DETACHED,		// the folder no longer follows the machine
	VFR_FULL,		// the folder's changes do not fit on the volume
	VFR_PART,		// done, but some of the changes could not be put in (out->left)
	VFR_MEMORY		// no memory: if it ran out midway, the volume no longer follows the folder
};
typedef struct {
	int added, removed, changed;	// entries, what is in a folder counted too
	int left;			// changes left out
} vfRefresh;
// VFR_OK, or why a refresh is refused now: VFR_WRITES, VFR_STUCK, VFR_DETACHED
int vfat_refresh_ready(vFat*);
// the host names the last sync could not do, for the user ("" if none)
const char* vfat_stuck(vFat*);
// the folder changed on the host: what fresh (the tree scanned again, maxdepth deep) differs
// by goes into the volume; a refusal and VFR_FULL change nothing
int vfat_refresh(vFat*, const vFat* fresh, int maxdepth, vfRefresh* out);

#ifdef __cplusplus
}
#endif

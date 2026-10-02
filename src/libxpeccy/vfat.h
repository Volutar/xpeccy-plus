#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

// Read-only FAT32 volume synthesized from a host directory tree.
// The caller builds the tree with vfat_add (a scanner lives in xcore, so this
// file stays free of platform code), calls vfat_build, then asks for sectors.

#define VF_SECSIZE	512
#define VF_MAXNAME	255

// IDE port sets the HDD boot sector comes in
enum {
	VF_PORT_NEMO = 0,
	VF_PORT_ATM,
	VF_HDDBOOTS
};

typedef struct {
	unsigned isdir:1;
	unsigned lfn:1;			// name doesn't fit 8.3, needs long name entries
	char* name;			// name shown to the guest, UTF-8
	char* host;			// host path (files only), in the local 8-bit encoding
	char sname[12];			// generated 8.3 name, space padded, no dot
	unsigned int shash;		// hash of sname: siblings are compared by it first
	int parent;			// owning directory, -1 for root
	int child;			// first child, -1 if none
	int last;			// last child, so appending doesn't walk the list
	int next;			// next sibling, -1 if none
	unsigned int size;		// file size in bytes
	unsigned int clust;		// first cluster, 0 if the node takes none
	unsigned int nclust;		// clusters occupied
	unsigned short date;		// last write, FAT format
	unsigned short time;
	int ecnt;			// directory entries this node takes in its parent
} vfNode;

typedef struct {
	vfNode* node;
	int nodes;
	int cap;
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

	int cnode;			// cached open file
	FILE* cfile;
} vFat;

vFat* vfat_create(void);
void vfat_free(vFat*);

// name: UTF-8; host: local 8-bit path (files only); returns node index or -1
int vfat_add(vFat*, int parent, const char* name, const char* host, unsigned int size, unsigned int mtime, int isdir);
// lay the tree out on a volume of at least minsec sectors; 0 on failure
int vfat_build(vFat*, unsigned int minsec);
// fill one 512-byte sector; 0 if the LBA is outside the volume
int vfat_read(vFat*, unsigned int lba, unsigned char* dst);

#ifdef __cplusplus
}
#endif

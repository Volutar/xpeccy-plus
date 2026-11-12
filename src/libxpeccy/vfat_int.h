#pragma once

// What vfat.c (the volume as built) and vfat_wr.c (the guest's writes) share.

#include "vfat.h"

#define VF_EOC		0x0fffffff

static inline void vf_wr16(unsigned char* p, unsigned int v) {
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
}

static inline void vf_wr32(unsigned char* p, unsigned int v) {
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

static inline unsigned int vf_rd16(const unsigned char* p) {
	return p[0] | (p[1] << 8);
}

static inline unsigned int vf_rd32(const unsigned char* p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}

// the sectors of the volume as built
void vf_gen_fat(vFat*, unsigned int sec, unsigned char* dst);
void vf_gen_dir(vFat*, int idx, unsigned int sec, unsigned char* dst);
void vf_gen_file(vFat*, int idx, unsigned int sec, unsigned char* dst);
void vf_gen_reserved(vFat*, unsigned int lba, unsigned char* dst);	// MBR, gap, boot, FSInfo
int vf_clust_node(vFat*, unsigned int clu);

// directory entries as the build makes them
int vf_sname_make(const char* name, char* dst, int* ulen);	// 1 when a long name is needed
unsigned char vf_sname_sum(const char* sname);
// sname made unique: taken says whether a name is in use among its siblings
void vf_sname_uniq(char* sname, const char* name, int (*taken)(void* ctx, const char* sname), void* ctx);
void vf_fat_stamp(unsigned int mtime, unsigned short* date, unsigned short* tim);
void vf_entry(unsigned char* dst, const char* sname, int attr, unsigned int clust, unsigned int size, unsigned short date, unsigned short tim);
void vf_lfn(unsigned char* dst, unsigned short* name, int len, int part, int last, unsigned char sum);

// a node's host path into buf (UTF-8); 0 if it does not fit
int vf_path(vFat*, int idx, char* buf, int max);
FILE* vf_open(vFat*, const char* path, const char* mode);
void vf_close_cached(vFat*);
char* vf_strdup(const char*);
int vf_utf16(const char* src, unsigned short* dst, int max);

// vfat_wr.c
int vfw_read(vFat*, unsigned int lba, unsigned char* dst);
void vfw_free(vFat*);

#include "filetypes.h"
#include "szx.h"
#include "../xlog.h"

// What a zx-state file has no block for, kept in the creator block's own data,
// which the spec leaves to the program that wrote it: a tag, then records shaped
// like the file's blocks - a 4-byte id, a 32-bit length and the bytes. A record
// this build does not know is skipped, and a field missing from one keeps the
// value the standard blocks gave it, so files of older and newer builds load.

static const char szx_ext_tag[8] = {'X','p','e','c','c','y','+',0};

const unsigned char* szx_ext_find(const unsigned char* data, size_t len, size_t* extlen) {
	if ((len < sizeof(szx_ext_tag)) || memcmp(data, szx_ext_tag, sizeof(szx_ext_tag))) return NULL;
	if (extlen) *extlen = len - sizeof(szx_ext_tag);
	return data + sizeof(szx_ext_tag);
}

// libspectrum before 0.5.0 put F before A in the Z80R block where the spec has
// A first; it signs its files with "libspectrum: x.y.z" in the creator data
int szx_libspectrum_swap(const unsigned char* data, size_t len) {
	static const char sig[] = "libspectrum: ";
	size_t n = sizeof(sig) - 1;
	size_t i;
	int v1, v2, v3;
	char ver[16];
	for (i = 0; i + n <= len; i++) {
		if (memcmp(data + i, sig, n)) continue;
		size_t k = 0;
		while ((k < sizeof(ver) - 1) && (i + n + k < len) && data[i + n + k]) {
			ver[k] = data[i + n + k];
			k++;
		}
		ver[k] = 0;
		if (sscanf(ver, "%d.%d.%d", &v1, &v2, &v3) != 3) return 0;
		return (v1 == 0) && ((v2 < 5) || ((v2 == 5) && (v3 == 0)));
	}
	return 0;
}

int szx_ext_hardware(const unsigned char* file, size_t len) {
	return SNAP_HW_UNKNOWN;
}

void szx_ext_load(Computer* comp, const unsigned char* ext, size_t len) {
}

int szx_ext_tick(Computer* comp, int tick) {
	return tick;
}

int szx_ext_can_save(Computer* comp) {
	return 0;
}

void szx_ext_save(szxBuf* b, Computer* comp) {
	sb_put(b, szx_ext_tag, sizeof(szx_ext_tag));
}

// --- General Sound ---

void szx_rd_gs(Computer* comp, const unsigned char* p, size_t n) {
	xlog(XLG_FILE, XLL_INFO, "szx: General Sound state not taken yet");
}

void szx_rd_gsrp(Computer* comp, const unsigned char* p, size_t n) {
}

void szx_wr_gs(szxBuf* b, szxBuf* d, Computer* comp) {
}

void szx_machine_of(const char* name, char* id, size_t idsize) {
	if (idsize) id[0] = 0;
}

#include "filetypes.h"

// A ZX screen file: the 6912 bytes SAVE "" SCREEN$ writes from #4000 (bitmap, then
// attributes), the 6144 of the bitmap alone, or 6976 - the 6912 and the 64 ULA+
// palette entries (GGGRRRBB) after them. A Hobeta file of type C and 6912 bytes is
// the same screen behind its 17-byte header.
// It goes into the screen the machine shows, as a poke would: nothing else changes.

#define SCR_PIX		6144
#define SCR_FULL	6912
#define SCR_PAL		64
#define SCR_HOB_HEAD	17

// the screen the machine draws is the ZX one in its memory: the modes the SCR
// screenshot takes, and the Next's ULA layer
static int scr_zx_shown(Video* vid) {
	switch (vid->vmode) {
		case VID_NORMAL:
		case VID_ULA_SCR:
		case VID_ROBIK:
		case VID_TSL_NORMAL:
		case VID_NEXT:
			return 1;
	}
	return 0;
}

// a Hobeta file of a screen: the header says type C, 6912 bytes, and 27 whole
// sectors follow it
int scr_hobeta_head(const unsigned char* head, size_t size) {
	if (size != SCR_HOB_HEAD + SCR_FULL) return 0;
	return (head[8] == 'C') && ((head[11] | (head[12] << 8)) == SCR_FULL);
}

int loadSCR(Computer* comp, const char* name, int drv) {
	FILE* file = fopen(name, "rb");
	if (!file) return ERR_CANT_OPEN;
	unsigned char buf[SCR_HOB_HEAD + SCR_FULL + SCR_PAL];
	size_t size = fread(buf, 1, sizeof(buf), file);
	int more = (fgetc(file) != EOF);
	fclose(file);
	unsigned char* data = buf;
	if (!more && scr_hobeta_head(buf, size)) {
		data += SCR_HOB_HEAD;
		size = SCR_FULL;
	} else if (more || ((size != SCR_FULL) && (size != SCR_PIX) && (size != SCR_FULL + SCR_PAL))) {
		return ERR_SCR_SIZE;
	}
	Video* vid = comp->vid;
	if (!scr_zx_shown(vid)) return ERR_SCR_MODE;
	Memory* mem = comp->mem;
	int i;
	for (i = 0; i < SCR_FULL; i++)		// a bitmap alone: black on white, as CLS leaves it
		mem->ramData[MADR(vid->vidPage, i) & mem->ramMask] = (i < (int)size) ? data[i] : 0x38;
	// ULA+ through its ports, as a program sets it: the palette group, then the mode -
	// on for a screen that brings its palette, off for a plain one, whose attributes
	// mean the plain colors
	int pal = (size == SCR_FULL + SCR_PAL);
	if (vid->ula->enabled && (comp->hw->id != HW_ZXNEXT)) {
		for (i = 0; pal && (i < SCR_PAL); i++) {
			ula_wr(vid->ula, 0xbf3b, i);
			ula_wr(vid->ula, 0xff3b, data[SCR_FULL + i]);
		}
		if (pal || vid->ula->active) {
			ula_wr(vid->ula, 0xbf3b, 0x40);
			ula_wr(vid->ula, 0xff3b, pal);
		}
	} else if (pal) {
		comp->msg = " the screen without its ULA+ palette ";
	}
	return ERR_OK;
}

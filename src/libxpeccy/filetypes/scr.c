#include "filetypes.h"

// SAVE "" SCREEN$ (SCR_SIZE), the bitmap alone (6144), or SCR_SIZE + the ULA+ palette (6976)

#define SCR_PIX	6144
#define SCR_PAL	64

// a Hobeta screen: type C, SCR_SIZE bytes, 27 whole sectors after the header
int scr_hobeta_head(const unsigned char* head, size_t size) {
	if (size != HOB_HEAD_SIZE + SCR_SIZE) return 0;
	return (head[8] == 'C') && ((head[11] | (head[12] << 8)) == SCR_SIZE);
}

int scr_read(const char* name, unsigned char* buf, int* pal) {
	FILE* file = fopen(name, "rb");
	if (!file) return ERR_CANT_OPEN;
	size_t got = fread(buf, 1, SCR_BUF, file);
	int more = (fgetc(file) != EOF);
	fclose(file);
	if (!more && scr_hobeta_head(buf, got)) {
		memmove(buf, buf + HOB_HEAD_SIZE, SCR_SIZE);
		got = SCR_SIZE;
	} else if (more || ((got != SCR_SIZE) && (got != SCR_PIX) && (got != SCR_SIZE + SCR_PAL))) {
		return ERR_SCR_SIZE;
	}
	if (got == SCR_PIX) memset(buf + SCR_PIX, 0x38, SCR_SIZE - SCR_PIX);	// black on white
	*pal = (got == SCR_SIZE + SCR_PAL);
	return ERR_OK;
}

// an outer dot's color index: plain ink or paper (BRIGHT dropped), or the ULA+ one
static int scr_edge_dot(Video* vid, const unsigned char* scr, int x, int y) {
	int ink = (scr[zx_pix_adr_xy(x, y)] >> vid_scr_bit(x)) & 1;
	int at = scr[zx_atr_adr_xy(x, y)];
	if (!vid->ula->active) return ink ? (at & 7) : ((at >> 3) & 7);
	return ((at & 0xc0) >> 2) | (ink ? (at & 7) : (((at >> 3) & 7) | 8));
}

// The border the outer dots have most, ties to the lower; under ULA+ each of the 64 live
// colors counts as the border entry (8..15) nearest to it
static int scr_border_of(Video* vid, const unsigned char* scr) {
	int near[64];
	int cnt[8] = {0};
	int i, b, x, y;
	for (i = 0; i < 64; i++) {
		near[i] = i & 7;
		if (!vid->ula->active) continue;
		xColor c = vid_get_col(vid, i);
		int dmin = -1;
		for (b = 0; b < 8; b++) {
			xColor e = vid_get_col(vid, 8 | b);
			int d = (c.r - e.r) * (c.r - e.r) + (c.g - e.g) * (c.g - e.g) + (c.b - e.b) * (c.b - e.b);
			if ((dmin < 0) || (d < dmin)) {
				dmin = d;
				near[i] = b;
			}
		}
	}
	for (x = 0; x < 256; x++) {
		cnt[near[scr_edge_dot(vid, scr, x, 0)]]++;
		cnt[near[scr_edge_dot(vid, scr, x, 191)]]++;
	}
	for (y = 1; y < 191; y++) {
		cnt[near[scr_edge_dot(vid, scr, 0, y)]]++;
		cnt[near[scr_edge_dot(vid, scr, 255, y)]]++;
	}
	for (i = 0, b = 0; i < 8; i++)
		if (cnt[i] > cnt[b]) b = i;
	return b;
}

int scr_put(Computer* comp, const unsigned char* data, int pal, int view) {
	Video* vid = comp->vid;
	Memory* mem = comp->mem;
	int i;
	if (!vid_zx_shown(vid)) return ERR_SCR_MODE;
	vid_outside_write(vid);		// a still picture goes, the rest drawn first
	for (i = 0; i < SCR_SIZE; i++)
		mem->ramData[MADR(vid->vidPage, i) & mem->ramMask] = data[i];
	// ULA+: the palette and the mode on, or the mode off for a plain screen
	ulaPlus* ula = vid->ula;
	if (view && pal && (comp->hw->id != HW_ZXNEXT)) ula->enabled = 1;
	if (ula->enabled && (comp->hw->id != HW_ZXNEXT)) {
		if (pal) memcpy(ula->pal, data + SCR_SIZE, SCR_PAL);
		ula->active = pal;
		zxSetUlaPalete(comp);
		ula->palchan = 0;
	} else if (pal && !view) {
		comp->msg = " the screen without its ULA+ palette ";
	}
	// the border as a cpu OUT sets it, the beeper and MIC as they are
	comp_out_outside(comp, 0xfe, (comp->beep->lev ? 0x10 : 0) | (comp->tape->levRec ? 0x08 : 0)
		| scr_border_of(vid, data));
	if (view) return ERR_OK;
	rzx_rec_touch();		// a poke, as a cheat's
	x_input_bump();
	return ERR_OK;
}

int loadSCR(Computer* comp, const char* name, int drv) {
	unsigned char buf[SCR_BUF];
	int pal;
	int err = scr_read(name, buf, &pal);
	return (err == ERR_OK) ? scr_put(comp, buf, pal, 0) : err;
}

#include "filetypes.h"

// SAVE "" SCREEN$ (6912), the bitmap alone (6144), or 6912 + the ULA+ palette (6976)

#define SCR_PIX		6144
#define SCR_FULL	6912
#define SCR_PAL		64
#define SCR_HOB_HEAD	17
#define SCR_BUF		(SCR_HOB_HEAD + SCR_FULL + SCR_PAL)

// the SCR screenshot's modes, and the Next's ULA layer
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

// a Hobeta screen: type C, 6912 bytes, 27 whole sectors after the header
int scr_hobeta_head(const unsigned char* head, size_t size) {
	if (size != SCR_HOB_HEAD + SCR_FULL) return 0;
	return (head[8] == 'C') && ((head[11] | (head[12] << 8)) == SCR_FULL);
}

// the file's screen into buf: *size 6912, 6144 or 6976
static int scr_read(const char* name, unsigned char* buf, size_t* size) {
	FILE* file = fopen(name, "rb");
	if (!file) return ERR_CANT_OPEN;
	size_t got = fread(buf, 1, SCR_BUF, file);
	int more = (fgetc(file) != EOF);
	fclose(file);
	if (!more && scr_hobeta_head(buf, got)) {
		memmove(buf, buf + SCR_HOB_HEAD, SCR_FULL);
		got = SCR_FULL;
	} else if (more || ((got != SCR_FULL) && (got != SCR_PIX) && (got != SCR_FULL + SCR_PAL))) {
		return ERR_SCR_SIZE;
	}
	*size = got;
	return ERR_OK;
}

// a ULA+ entry GGGRRRBB as 3-bit r, g, b: the blue's low bit is either of its two
static void scr_ulap_rgb(unsigned char v, int* c) {
	c[0] = (v >> 2) & 7;
	c[1] = (v >> 5) & 7;
	c[2] = ((v & 3) << 1) | ((v & 3) ? 1 : 0);
}

// The border nearest the picture's outer dots: the color most of them have (BRIGHT
// dropped, under ULA+ the nearest of the eight border entries), ties to the lower
static int scr_border_of(const unsigned char* scr, const ulaPlus* ula) {
	int cnt[8] = {0};
	int brd[8][3];
	int i, b, x, y;
	for (b = 0; b < 8; b++) scr_ulap_rgb(ula->pal[8 | b], brd[b]);
	for (i = 0; i < 2 * 256 + 2 * 190; i++) {
		if (i < 512) {
			x = i & 255;
			y = (i < 256) ? 0 : 191;
		} else {
			x = (i < 702) ? 0 : 255;
			y = 1 + ((i - 512) % 190);
		}
		unsigned char px = scr[((y & 0xc0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2) | (x >> 3)];
		unsigned char at = scr[0x1800 + ((y >> 3) << 5) + (x >> 3)];
		int ink = (px >> (7 - (x & 7))) & 1;
		if (!ula->active) {
			cnt[ink ? (at & 7) : ((at >> 3) & 7)]++;
			continue;
		}
		int idx = ((at & 0xc0) >> 2) | (ink ? (at & 7) : (((at >> 3) & 7) | 8));
		int c[3], best = 0, dmin = 1 << 30;
		scr_ulap_rgb(ula->pal[idx], c);
		for (b = 0; b < 8; b++) {
			int d = (c[0] - brd[b][0]) * (c[0] - brd[b][0]) + (c[1] - brd[b][1]) * (c[1] - brd[b][1])
				+ (c[2] - brd[b][2]) * (c[2] - brd[b][2]);
			if (d < dmin) {
				dmin = d;
				best = b;
			}
		}
		cnt[best]++;
	}
	for (i = 0, b = 0; i < 8; i++)
		if (cnt[i] > cnt[b]) b = i;
	return b;
}

static int scr_put(Computer* comp, const unsigned char* data, size_t size) {
	Video* vid = comp->vid;
	if (!scr_zx_shown(vid)) return ERR_SCR_MODE;
	Memory* mem = comp->mem;
	int i;
	for (i = 0; i < SCR_FULL; i++)		// the bitmap alone: black on white
		mem->ramData[MADR(vid->vidPage, i) & mem->ramMask] = (i < (int)size) ? data[i] : 0x38;
	// ULA+ through its ports: the palette and the mode on, or the mode off for a plain screen
	int pal = (size == SCR_FULL + SCR_PAL);
	ulaPlus* ula = vid->ula;
	if (ula->enabled && (comp->hw->id != HW_ZXNEXT)) {
		unsigned char reg = ula->reg;		// what the program picked stays picked
		for (i = 0; pal && (i < SCR_PAL); i++) {
			ula_wr(ula, 0xbf3b, i);
			ula_wr(ula, 0xff3b, data[SCR_FULL + i]);
		}
		if (pal || ula->active) {
			ula_wr(ula, 0xbf3b, 0x40);
			ula_wr(ula, 0xff3b, pal);
		}
		ula->reg = reg;
		if (ula->palchan) {		// as after an OUT: the colors at once
			zxSetUlaPalete(comp);
			ula->palchan = 0;
		}
	} else if (pal) {
		comp->msg = " the screen without its ULA+ palette ";
	}
	// the border through port FE, as a program sets it, the beeper and MIC as they are
	unsigned char scr[SCR_FULL];
	for (i = 0; i < SCR_FULL; i++)
		scr[i] = mem->ramData[MADR(vid->vidPage, i) & mem->ramMask];
	comp->hw->out(comp, 0xfe, (comp->beep->lev ? 0x10 : 0) | (comp->tape->levRec ? 0x08 : 0)
		| scr_border_of(scr, ula));
	rzx_rec_touch();		// a poke, as a cheat's
	x_input_bump();
	return ERR_OK;
}

int loadSCR(Computer* comp, const char* name, int drv) {
	unsigned char buf[SCR_BUF];
	size_t size;
	int err = scr_read(name, buf, &size);
	return (err == ERR_OK) ? scr_put(comp, buf, size) : err;
}

// A screen held while the ROM starts up, which clears the screen: in once the ROM has
// scanned the whole keyboard and the time autostart waits after that
#define SCR_SETTLE	150
#define SCR_GIVEUP	1500

static struct {
	unsigned char data[SCR_BUF];
	size_t size;
	int on;
	int wait;		// -1: until the full KEY-SCAN
	int life;
} scr_held;

int scr_hold(Computer* comp, const char* name) {
	int err = scr_read(name, scr_held.data, &scr_held.size);
	if (err != ERR_OK) return err;
	scr_held.on = 1;
	scr_held.wait = -1;
	scr_held.life = SCR_GIVEUP;
	comp->keyb->scanmask = 0;
	return ERR_OK;
}

void scr_hold_cancel(void) {
	scr_held.on = 0;
}

void scr_hold_frame(Computer* comp) {
	if (!scr_held.on) return;
	if (--scr_held.life > 0) {
		if (scr_held.wait < 0) {
			if (comp->keyb->scanmask != 0xff) return;
			scr_held.wait = SCR_SETTLE;
		}
		if (scr_held.wait-- > 0) return;
	}
	scr_held.on = 0;
	if (scr_put(comp, scr_held.data, scr_held.size) == ERR_SCR_MODE)
		comp->msg = " not the ZX screen: no SCR ";
}

#include <cmath>
#include <string.h>

#include "xcore.h"
#include "vscalers.h"

#include <QApplication>
#include <QScreen>
#include <QDebug>

int drawX = 0;
int drawY = 0;
int drawW = 0;
int drawH = 0;
int drawZoom = 0;

const char* brdModeTab[] = {
	"no",		// VID_BRD_NONE
	"tiny",
	"small",
	"medium",
	"full",
	"overscan",	// VID_BRD_OVERSCAN
	NULL
};

const char* brd_mode_name(int mode) {
	if ((mode < VID_BRD_NONE) || (mode > VID_BRD_OVERSCAN))
		mode = VID_BRD_FULL;
	return brdModeTab[mode];
}

int brd_mode_id(const char* nam) {
	for (int i = 0; brdModeTab[i]; i++) {
		if (!strcmp(brdModeTab[i], nam))
			return i;
	}
	return VID_BRD_FULL;
}

// pre-2026.4 configs kept the border as a percentage of the machine's own
int brd_mode_pcnt(int pcnt) {
	if (pcnt < 5) return VID_BRD_NONE;
	if (pcnt < 25) return VID_BRD_TINY;
	if (pcnt < 45) return VID_BRD_SMALL;
	if (pcnt < 70) return VID_BRD_MEDIUM;
	if (pcnt < 100) return VID_BRD_FULL;
	return VID_BRD_OVERSCAN;
}

static const char* sfillTab[] = {"black", "edges", "mirror", NULL};

const char* sfill_name(int mode) {
	if ((mode < SFILL_BLACK) || (mode > SFILL_MIRROR)) mode = SFILL_BLACK;
	return sfillTab[mode];
}

int sfill_id(const char* nam) {
	for (int i = 0; sfillTab[i]; i++) {
		if (!strcmp(sfillTab[i], nam))
			return i;
	}
	return SFILL_BLACK;
}

const int sfillItems[SFILL_ITEMS] = {
	SFILL_BORDER | SFILL_BLACK, SFILL_BORDER | SFILL_EDGES, SFILL_BORDER | SFILL_MIRROR,
	SFILL_BLACK, SFILL_EDGES, SFILL_MIRROR
};

int sfill_item() {
	return conf.vid.sideFill | (conf.vid.fillBorder ? SFILL_BORDER : 0);
}

void sfill_set(int item) {
	conf.vid.sideFill = item & ~SFILL_BORDER;
	conf.vid.fillBorder = (item & SFILL_BORDER) ? 1 : 0;
}

// the picture area of a window that is not the zoom's size, empty when it is
static QSize fitArea;
// the monitor the window is on, for fullscreen
static QSize fullscreenSize;

void vid_set_fit(const QSize& area) {
	fitArea = area;
	vid_upd_scale();
}

QSize vid_room(int fullscreen) {
	if (!fullscreen) return fitArea;
	// Before the main window is shown, configuration has no monitor yet.
	if (!fullscreenSize.isValid()) {
#if QT_VERSION >= QT_VERSION_CHECK(5,0,0)
		fullscreenSize = QGuiApplication::primaryScreen()->size();
#else
		fullscreenSize = QApplication::desktop()->screenGeometry().size();
#endif
	}
	return fullscreenSize;
}

// Where the frame lands inside the window. In a window that is a whole number
// of pixels per dot, so a dot is never split. Fullscreen, and a window of
// another size - maximized, snapped, dragged - fit the picture to the room
// there is instead, keeping its shape: at a screen's resolution, and through
// a shader, a whole zoom is not worth the black bars it leaves. 'ignore aspect
// ratio' stretches it to fill.
vidFit vid_fit(Video* vid, int mode, const QSize& room, int zoom, int keepRatio, int fillBorder, double xscale) {
	vidFit f;
	f.crop = vid_crop_size(vid, mode);		// the frame the border size asks for
	f.widen = 0;
	f.wideW = 0;
	f.wideH = 0;
	if (room.isEmpty()) {
		f.kx = xscale * zoom;
		f.ky = zoom;
		f.w = (int)(f.crop.x * f.kx);
		f.h = f.crop.y * zoom;
		f.roomW = f.w;
		f.roomH = f.h;
		f.zoom = zoom;
	} else {
		f.roomW = room.width();
		f.roomH = room.height();
		if (keepRatio) {
			// The height sets the scale in a room wider than the picture, the
			// width in a taller one. With the border first, the free room shows
			// more border, as far as the raster goes: the frame takes a dot
			// more than fits and is cut at the room's edge, so the edge follows
			// the room smoothly instead of jumping a dot at a time.
			double picw = f.crop.x * xscale;
			double fit;
			if (f.roomW * (double)f.crop.y >= f.roomH * picw) {
				fit = f.roomH / (double)f.crop.y;
				f.wideW = (int)std::ceil(f.roomW / (fit * xscale) - 1e-6);
			} else {
				fit = f.roomW / picw;
				f.wideH = (int)std::ceil(f.roomH / fit - 1e-6);
			}
			if (fillBorder) {
				f.widen = 1;
				f.crop = vid_widen_size(vid, mode, f.wideW, f.wideH);
			}
			f.kx = fit * xscale;
			f.ky = fit;
			f.w = (int)(f.crop.x * f.kx + 0.5);
			f.h = (int)(f.crop.y * fit + 0.5);
			f.zoom = (fit == 1.0) ? 1 : 0;
		} else {
			f.w = f.roomW;
			f.h = f.roomH;
			f.kx = f.w / (double)f.crop.x;
			f.ky = f.h / (double)f.crop.y;
			f.zoom = 0;				// stretched: no whole zoom
		}
	}
	f.x = (f.roomW - f.w) / 2;
	f.y = (f.roomH - f.h) / 2;
	return f;
}

void vid_upd_scale(const QSize& screenSize) {
	if (!screenSize.isEmpty()) fullscreenSize = screenSize;
	Video* vid = conf.zx->vid;
	vidFit f = vid_fit(vid, vid->brdmode, vid_room(conf.vid.fullScreen), conf.vid.scale,
		conf.vid.keepRatio, conf.vid.fillBorder, conf.zx->hw->xscale);	// pixel aspect, 1.0 on every zx
	vid_upd_crop(vid);
	if (f.widen) vid_widen_crop(vid, f.wideW, f.wideH);
	drawX = f.x;
	drawY = f.y;
	drawW = f.w;
	drawH = f.h;
	drawZoom = f.zoom;
}

void vid_set_zoom(int zoom) {
	if (zoom < 1) return;
	if (zoom > 6) return;
	conf.vid.scale = zoom;
	vid_upd_scale();
}

void vid_set_fullscreen(int f) {
	conf.vid.fullScreen = f ? 1 : 0;
	vid_upd_scale();
}

void vid_set_border_mode(int mode) {
	Computer* comp = conf.zx;
	conf.vid.border = mode;
	vid_set_border(comp->vid, mode);
	vid_upd_scale();
}

void vid_set_ratio(int f) {
	conf.vid.keepRatio = f ? 1 : 0;
	vid_upd_scale();
}


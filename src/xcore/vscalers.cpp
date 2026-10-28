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

void vid_set_fit(const QSize& area) {
	fitArea = area;
	vid_upd_scale();
}

// Where the frame lands inside the window. In a window that is a whole number
// of pixels per dot, so a dot is never split. Fullscreen, and a window of
// another size - maximized, snapped, dragged - fit the picture to the room
// there is instead, keeping its shape: at a screen's resolution, and through
// a shader, a whole zoom is not worth the black bars it leaves. 'ignore aspect
// ratio' stretches it to fill.
void vid_upd_scale(const QSize& screenSize) {
	static QSize fullscreenSize;
	if (!screenSize.isEmpty()) fullscreenSize = screenSize;
	Video* vid = conf.zx->vid;
	double xscale = conf.zx->hw->xscale;		// pixel aspect, 1.0 on every zx
	int dwid;
	int dhei;
	vid_upd_crop(vid);					// the frame the border size asks for
	if (conf.vid.fullScreen || !fitArea.isEmpty()) {
		// Before the main window is shown, configuration has no monitor yet.
		if (conf.vid.fullScreen && !fullscreenSize.isValid()) {
#if QT_VERSION >= QT_VERSION_CHECK(5,0,0)
			fullscreenSize = QGuiApplication::primaryScreen()->size();
#else
			fullscreenSize = QApplication::desktop()->screenGeometry().size();
#endif
		}
		const QSize& area = conf.vid.fullScreen ? fullscreenSize : fitArea;
		dwid = area.width();
		dhei = area.height();
		if (conf.vid.keepRatio) {
			// The height sets the scale in a room wider than the picture, the
			// width in a taller one. With the border first, the free room shows
			// more border, as far as the raster goes: the frame takes a dot
			// more than fits and is cut at the room's edge, so the edge follows
			// the room smoothly instead of jumping a dot at a time.
			double picw = vid->vsze.x * xscale;
			double fit;
			if (dwid * (double)vid->vsze.y >= dhei * picw) {
				fit = dhei / (double)vid->vsze.y;
				if (conf.vid.fillBorder)
					vid_widen_crop(vid, (int)std::ceil(dwid / (fit * xscale) - 1e-6), 0);
			} else {
				fit = dwid / picw;
				if (conf.vid.fillBorder)
					vid_widen_crop(vid, 0, (int)std::ceil(dhei / fit - 1e-6));
			}
			picw = vid->vsze.x * xscale;
			drawW = (int)(picw * fit + 0.5);
			drawH = (int)(vid->vsze.y * fit + 0.5);
			drawZoom = (fit == 1.0) ? 1 : 0;
		} else {
			drawW = dwid;
			drawH = dhei;
			drawZoom = 0;				// stretched: no whole zoom
		}
	} else {
		drawW = (int)(vid->vsze.x * xscale * conf.vid.scale);
		drawH = vid->vsze.y * conf.vid.scale;
		dwid = drawW;
		dhei = drawH;
		drawZoom = conf.vid.scale;
	}
	drawX = (dwid - drawW) / 2;
	drawY = (dhei - drawH) / 2;
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


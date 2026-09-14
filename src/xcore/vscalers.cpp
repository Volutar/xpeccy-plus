// #include <math.h>
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

// Where the frame lands inside the window. In a window that is a whole number
// of pixels per dot, so a dot is never split. Fullscreen fits the picture to
// the screen instead, keeping its shape: at a screen's resolution, and through
// a shader, a whole zoom is not worth the black bars it leaves. 'ignore aspect
// ratio' stretches it to fill.
void vid_upd_scale() {
	QSize scrsz;
	Video* vid = conf.zx->vid;
	double xscale = conf.zx->hw->xscale;		// pixel aspect, 1.0 on every zx
	int dwid;
	int dhei;
	vid_upd_crop(vid);					// the frame the border size asks for
	if (conf.vid.fullScreen) {
#if QT_VERSION >= QT_VERSION_CHECK(5,14,0)
		scrsz = QApplication::screens().first()->size();
#else
		scrsz = QApplication::desktop()->screenGeometry().size();
#endif
		dwid = scrsz.width();
		dhei = scrsz.height();
		if (conf.vid.keepRatio) {
			// The height sets the scale on a screen wider than the picture.
			// Rather than leave black beside it, show more border there, as
			// far as the raster goes.
			vid_widen_crop(vid, (int)(dwid * vid->vsze.y / (dhei * xscale)));
			double picw = vid->vsze.x * xscale;
			double fit = dwid / picw;
			if (dhei / (double)vid->vsze.y < fit)
				fit = dhei / (double)vid->vsze.y;
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


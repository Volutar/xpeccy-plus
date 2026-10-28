#pragma once

#include <QSize>

#include "../libxpeccy/video/vidcommon.h"

// One name per VID_BRD_* mode, in enum order, closed with a NULL
extern const char* brdModeTab[];

const char* brd_mode_name(int);
int brd_mode_id(const char*);
int brd_mode_pcnt(int);

// What fills the room beside a fitted picture (conf.vid.sideFill)
enum {
	SFILL_BLACK = 0,
	SFILL_EDGES,		// the edge rows and columns carried outwards, blurred and dimmed
	SFILL_MIRROR		// the picture reflected at its edge, blurred and dimmed
};
const char* sfill_name(int);
int sfill_id(const char*);
// The Side Fill list, in the menu and in Options: the border widened into the
// room first and the fill past it, or the fill from the border size's edge. An
// item is a fill, | SFILL_BORDER for the border first (conf.vid.fillBorder).
#define SFILL_BORDER	0x10
#define SFILL_ITEMS	6
extern const int sfillItems[SFILL_ITEMS];
int sfill_item();		// the one conf has
void sfill_set(int item);
// the ranges of conf.vid.fill*, and what they start at
#define SFILL_BLUR_MIN	1
#define SFILL_BLUR_MAX	16
#define SFILL_DIM_MIN	10
#define SFILL_EDGE_MAX	32
#define SFILL_BLUR_DEF	2
#define SFILL_DIM_DEF	75
#define SFILL_FADE_DEF	50
#define SFILL_EDGE_DEF	12
#define SFILL_STRETCH_DEF	50

// Where the frame goes inside the window, in window pixels. The picture is
// always drawn at a whole multiple of the dot, so this is the window itself in
// windowed mode and a centered rectangle with black around it in fullscreen
// and in a window of another size (maximized, say).
extern int drawX;
extern int drawY;
extern int drawW;
extern int drawH;
// window pixels per dot, or 0 when the picture is stretched to the screen
extern int drawZoom;

#define VID_ZOOM_MAX	6

void vid_set_zoom(int);
void vid_set_fullscreen(int);
void vid_set_ratio(int);
void vid_set_border_mode(int);
// the picture area of a window that is not the zoom's size, fitted as fullscreen
// is; empty: the window is the zoom's size
void vid_set_fit(const QSize& area);
// Snap to steps (conf.vid.snap): the zoom when a window's picture area is a step,
// a whole zoom of the frame at a border between the one asked for and the
// widest the fit shows, else 0; the step nearest to an area, no larger than
// room (empty: any), invalid when there is none; the window at a zoom.
int vid_step_of(const QSize& area);
QSize vid_step_near(const QSize& area, const QSize& room);
// each axis on its own: the step width and height nearest to the area's, over
// every zoom; 0 on an axis with none
QSize vid_step_axes(const QSize& area, const QSize& room);
QSize vid_zoom_size(int zoom);
// The GUI supplies the current monitor size in logical pixels; setters reuse it.
void vid_upd_scale(const QSize& screenSize = QSize());

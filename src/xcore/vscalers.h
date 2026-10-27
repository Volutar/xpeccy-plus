#pragma once

#include <QSize>

#include "../libxpeccy/video/vidcommon.h"

// One name per VID_BRD_* mode, in enum order, closed with a NULL
extern const char* brdModeTab[];

const char* brd_mode_name(int);
int brd_mode_id(const char*);
int brd_mode_pcnt(int);

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

void vid_set_zoom(int);
void vid_set_fullscreen(int);
void vid_set_ratio(int);
void vid_set_border_mode(int);
// the picture area of a window that is not the zoom's size, fitted as fullscreen
// is; empty: the window is the zoom's size
void vid_set_fit(const QSize& area);
// The GUI supplies the current monitor size in logical pixels; setters reuse it.
void vid_upd_scale(const QSize& screenSize = QSize());

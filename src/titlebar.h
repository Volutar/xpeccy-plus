#pragma once

class QWidget;

// Colours the native titlebar of a top-level window (background + text) to
// match the current style sheet, while leaving Windows itself in charge of
// the caption controls (minimize/maximize/close) and their placement.
//
// Windows 11 only (via DWM); a no-op elsewhere - Linux window managers and
// macOS don't expose a portable way to recolour a titlebar they draw
// themselves. Safe to call on any widget at any time: no-op if the widget
// isn't a top-level window, and the DWM call itself is ignored by Windows
// versions that don't support it.
void applyTitleBarStyle(QWidget* w);

// Windows holds a click on a title bar for up to half a second while it decides
// whether it is a drag, and the gui thread dispatches nothing meanwhile. Ended
// here for every window of the application; a no-op elsewhere.
void installTitleBarClickFix();

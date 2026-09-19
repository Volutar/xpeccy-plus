#include "titlebar.h"

#include <QWidget>
#include <QColor>
#include <QByteArray>

#include "xcore/xcore.h"

#if defined(_WIN32)

#include <windows.h>
#include <dwmapi.h>

// Not in every mingw/older SDK dwmapi.h yet - the enum values are stable
// across SDKs, only the names are missing, so define them ourselves.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
// tells DWM to go back to drawing the titlebar itself
#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFFu
#endif

static COLORREF toColorRef(const QColor& c) {
	return RGB(c.red(), c.green(), c.blue());
}

// DWMWA_USE_IMMERSIVE_DARK_MODE has no "leave it as it was" value - once a dark
// style sets it true, going back to "System" has to set it false again itself,
// or the caption stays dark forever after. So "System" needs to know what the
// desktop's own app mode actually is right now, the same place Explorer/Qt
// read it from: AppsUseLightTheme (0 = dark apps, 1 = light apps, missing key
// = pre-dark-mode Windows, light).
static bool systemAppsUseLightTheme() {
	HKEY key;
	DWORD value = 1;
	if (RegOpenKeyExW(HKEY_CURRENT_USER,
			L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
			0, KEY_READ, &key) == ERROR_SUCCESS) {
		DWORD size = sizeof(value);
		RegQueryValueExW(key, L"AppsUseLightTheme", NULL, NULL, reinterpret_cast<LPBYTE>(&value), &size);
		RegCloseKey(key);
	}
	return value != 0;
}

void applyTitleBarStyle(QWidget* w) {
	if (!w || !w->isWindow()) return;
	HWND hwnd = reinterpret_cast<HWND>(w->winId());

	// dbg.header.bg/txt: the same accent already used for the debugger's panel
	// headers, reused here rather than adding a titlebar-only palette entry.
	// It always has a value (built-in default, then whatever a style's .pal
	// brings) - so the "System"/no-style case is gated explicitly, not by
	// colour validity.
	bool active = !conf.style.empty();
	QColor bg = active ? conf.pal.value("dbg.header.bg") : QColor();
	QColor txt = active ? conf.pal.value("dbg.header.txt") : QColor();

	COLORREF capColor = bg.isValid() ? toColorRef(bg) : DWMWA_COLOR_DEFAULT;
	COLORREF txtColor = txt.isValid() ? toColorRef(txt) : DWMWA_COLOR_DEFAULT;
	// pre-Win11 fallback, and what DWMWA_COLOR_DEFAULT above actually renders
	// as: always set explicitly, never left alone, so leaving a dark style
	// puts this back rather than leaving the caption stuck dark.
	BOOL dark = bg.isValid() ? (bg.lightness() < 128) : !systemAppsUseLightTheme();

	// What this window was last given. The handle is part of it: Qt throws the
	// native window away and builds a new one when a widget's flags change, and
	// the attributes go with it.
	QByteArray state(reinterpret_cast<const char*>(&hwnd), sizeof(hwnd));
	state.append(reinterpret_cast<const char*>(&capColor), sizeof(capColor));
	state.append(reinterpret_cast<const char*>(&txtColor), sizeof(txtColor));
	state.append(dark ? '1' : '0');
	if (w->property("xTitleBar").toByteArray() == state) return;
	w->setProperty("xTitleBar", state);

	DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &capColor, sizeof(capColor));
	DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &txtColor, sizeof(txtColor));
	DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
	// Windows 10 has no caption colour at all - only the dark flag lands there,
	// and it reaches the screen when the window's activation next changes, not
	// when it is set: without this the new colour arrived only once the dialog
	// that changed it was closed. So say the window's own activation state to
	// it twice, ending on the state it is really in. A frame change
	// (SWP_FRAMECHANGED) is not enough there; Windows 11 repaints by itself and
	// does not mind this either way.
	if (IsWindowVisible(hwnd)) {
		BOOL act = (GetActiveWindow() == hwnd);
		SendMessageW(hwnd, WM_NCACTIVATE, !act, 0);
		SendMessageW(hwnd, WM_NCACTIVATE, act, 0);
	}
}

#else

// X11/Wayland window managers draw and own the titlebar themselves with no
// portable colour hint an application can send; macOS would need a native
// Cocoa call (NSWindow) that nobody here can build-test. Left as a no-op on
// both rather than guessing.
void applyTitleBarStyle(QWidget*) {
}

#endif

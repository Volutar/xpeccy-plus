#include "menustyle.h"

#include <QStyle>

#if defined(_WIN32)

#include <QProxyStyle>
#include <QStyleOptionMenuItem>
#include <QPainter>
#include <QImage>
#include <QWidget>
#include <QScreen>
#include <QGuiApplication>

#include <windows.h>
#include <uxtheme.h>
#include <vssym32.h>

// The menu theme as windowsvista opens it: Qt 6 for the primary screen's DPI,
// Qt 5 for the window
static HTHEME open_menu_theme(HWND hwnd) {
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
	typedef HTHEME (WINAPI *ForDpi)(HWND, LPCWSTR, UINT);
	static ForDpi forDpi = reinterpret_cast<ForDpi>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), "OpenThemeDataForDpi")));
	QScreen* scr = QGuiApplication::primaryScreen();
	if (forDpi && scr)
		return forDpi(hwnd, L"MENU", qRound(scr->logicalDotsPerInchX() * scr->devicePixelRatio()));
#endif
	return OpenThemeData(hwnd, L"MENU");
}

// The theme's tick or dot (MENU_POPUPCHECK) at its own size in device pixels,
// on a transparent ground, and its margins; null when the theme gives none
static QImage menu_glyph(HWND hwnd, int state, QMargins* margins) {
	QImage img;
	HTHEME th = open_menu_theme(hwnd);
	if (!th) return img;
	SIZE sz = {0, 0};
	MARGINS mg = {0, 0, 0, 0};
	// measured in state 0, as windowsvista measures it
	if (SUCCEEDED(GetThemePartSize(th, nullptr, MENU_POPUPCHECK, 0, nullptr, TS_TRUE, &sz)) && (sz.cx > 0) && (sz.cy > 0)) {
		GetThemeMargins(th, nullptr, MENU_POPUPCHECK, 0, TMT_CONTENTMARGINS, nullptr, &mg);
		BITMAPINFO bmi;
		ZeroMemory(&bmi, sizeof(bmi));
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = sz.cx;
		bmi.bmiHeader.biHeight = -sz.cy;	// top-down
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		void* bits = nullptr;
		HDC dc = CreateCompatibleDC(nullptr);
		HBITMAP bmp = dc ? CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
		if (bmp && bits) {
			HGDIOBJ old = SelectObject(dc, bmp);
			memset(bits, 0, sz.cx * sz.cy * 4);
			RECT rc = {0, 0, sz.cx, sz.cy};
			if (SUCCEEDED(DrawThemeBackground(th, dc, MENU_POPUPCHECK, state, &rc, nullptr))) {
				GdiFlush();
				img = QImage(static_cast<const uchar*>(bits), sz.cx, sz.cy, QImage::Format_ARGB32_Premultiplied).copy();
			}
			SelectObject(dc, old);
		}
		if (bmp) DeleteObject(bmp);
		if (dc) DeleteDC(dc);
	}
	CloseThemeData(th);
	// a part drawn without alpha leaves it all zero: nothing usable
	bool seen = false;
	for (int y = 0; !seen && (y < img.height()); y++) {
		const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
		for (int x = 0; x < img.width(); x++) {
			if (qAlpha(row[x])) {
				seen = true;
				break;
			}
		}
	}
	if (!seen) return QImage();
	*margins = QMargins(mg.cxLeftWidth, mg.cyTopHeight, mg.cxRightWidth, mg.cyBottomHeight);
	return img;
}

class xMenuCheckStyle : public QProxyStyle {
	public:
		explicit xMenuCheckStyle(QStyle* base) : QProxyStyle(base) {}
		void drawControl(ControlElement el, const QStyleOption* opt, QPainter* p, const QWidget* w) const override;
};

// A checked item with no icon: the base draws it unchecked, then the glyph goes
// where windowsvista's CE_MenuItem puts it. One with an icon keeps the square,
// its only sign of the state.
void xMenuCheckStyle::drawControl(ControlElement el, const QStyleOption* opt, QPainter* p, const QWidget* w) const {
	const QStyleOptionMenuItem* mi = (el == CE_MenuItem) ? qstyleoption_cast<const QStyleOptionMenuItem*>(opt) : nullptr;
	if (!mi || (mi->menuItemType == QStyleOptionMenuItem::Separator) || (mi->checkType == QStyleOptionMenuItem::NotCheckable)
			|| !mi->checked || !mi->icon.isNull() || !IsThemeActive() || !IsAppThemed()) {
		QProxyStyle::drawControl(el, opt, p, w);
		return;
	}
	const bool dis = !(mi->state & State_Enabled);
	const bool dot = (mi->checkType == QStyleOptionMenuItem::Exclusive);
	const int state = dis ? (dot ? MC_BULLETDISABLED : MC_CHECKMARKDISABLED) : (dot ? MC_BULLETNORMAL : MC_CHECKMARKNORMAL);
	const QWidget* win = w ? w->window() : nullptr;
	QMargins mg;
	QImage glyph = menu_glyph((win && win->internalWinId()) ? reinterpret_cast<HWND>(win->internalWinId()) : nullptr, state, &mg);
	if (glyph.isNull()) {
		QProxyStyle::drawControl(el, opt, p, w);
		return;
	}
	QStyleOptionMenuItem plain(*mi);
	plain.checked = false;
	QProxyStyle::drawControl(el, &plain, p, w);
	// windowsvista's geometry: a check column of the glyph and its margins (or the
	// icons, if wider) less a 3 px gutter, the square centred in it, the glyph in that
	const qreal dpr = w ? w->devicePixelRatioF() : p->device()->devicePixelRatioF();
	const qreal f = 1.0 / dpr;
	const QSizeF size = QSizeF(glyph.size()) * f;
	const int gutter = qRound(3 * f);
	int col = qMax(mi->maxIconWidth, qRound(gutter + size.width() + (mg.left() + mg.right()) * f));
	const QRect& r = mi->rect;
	if (mi->direction == Qt::LeftToRight) col += r.x();
	QRect check = visualRect(mi->direction, r, QRect(r.x(), r.y(), col - (gutter + r.x()), r.height()));
	QRect back(0, 0, qRound(size.width() + (mg.left() + mg.right()) * f), qRound(size.height() + (mg.top() + mg.bottom()) * f));
	back.moveCenter(check.center());
	QRect at(QPoint(0, 0), size.toSize());
	at.moveCenter(back.center());
	glyph.setDevicePixelRatio(dpr);
	p->drawImage(at.topLeft(), glyph);
}

QStyle* menuCheckStyle(QStyle* base) {
	return base ? new xMenuCheckStyle(base) : base;
}

#else

QStyle* menuCheckStyle(QStyle* base) {
	return base;
}

#endif

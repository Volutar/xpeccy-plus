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
#include <QHash>

#include <windows.h>
#include <uxtheme.h>
#include <vssym32.h>

static int screen_dpi(const QScreen* scr) {
	return scr ? qRound(scr->logicalDotsPerInchX() * scr->devicePixelRatio()) : 96;
}

// as windowsvista opens it: Qt 6 at the primary screen's DPI, Qt 5 for the window
static HTHEME open_menu_theme(HWND hwnd) {
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
	typedef HTHEME (WINAPI *ForDpi)(HWND, LPCWSTR, UINT);
	static ForDpi forDpi = reinterpret_cast<ForDpi>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), "OpenThemeDataForDpi")));
	if (forDpi && QGuiApplication::primaryScreen())
		return forDpi(hwnd, L"MENU", screen_dpi(QGuiApplication::primaryScreen()));
#endif
	return OpenThemeData(hwnd, L"MENU");
}

struct xGlyph {
	QImage img;		// device pixels, transparent ground; null: the theme has none
	QMargins margins;
};

// MENU_POPUPCHECK in a state, sized in state 0 as windowsvista sizes it
static xGlyph render_glyph(HWND hwnd, int state) {
	xGlyph g;
	HTHEME th = open_menu_theme(hwnd);
	if (!th) return g;
	SIZE sz = {0, 0};
	MARGINS mg = {0, 0, 0, 0};
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
				g.img = QImage(static_cast<const uchar*>(bits), sz.cx, sz.cy, QImage::Format_ARGB32_Premultiplied).copy();
			}
			SelectObject(dc, old);
		}
		if (bmp) DeleteObject(bmp);
		if (dc) DeleteDC(dc);
	}
	CloseThemeData(th);
	// a part drawn without alpha leaves it all zero
	bool seen = false;
	for (int y = 0; !seen && (y < g.img.height()); y++) {
		const QRgb* row = reinterpret_cast<const QRgb*>(g.img.constScanLine(y));
		for (int x = 0; !seen && (x < g.img.width()); x++)
			seen = qAlpha(row[x]);
	}
	if (!seen) g.img = QImage();
	g.margins = QMargins(mg.cxLeftWidth, mg.cyTopHeight, mg.cxRightWidth, mg.cyBottomHeight);
	return g;
}

static const xGlyph& menu_glyph(const QWidget* w, int state) {
	static QHash<qint64, xGlyph> cache;
	const QWidget* win = w ? w->window() : nullptr;
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
	const int dpi = screen_dpi(QGuiApplication::primaryScreen());
#else
	const int dpi = screen_dpi(win ? win->screen() : nullptr);
#endif
	const qint64 key = (qint64(dpi) << 16) | state;
	auto it = cache.find(key);
	if (it == cache.end())
		it = cache.insert(key, render_glyph((win && win->internalWinId()) ? reinterpret_cast<HWND>(win->internalWinId()) : nullptr, state));
	return it.value();
}

// windowsvista's QWindowsStylePrivate::nativeMetricScaleFactor
static qreal metric_factor(const QWidget* w, const QPainter* p) {
	qreal f = 1.0 / (w ? w->devicePixelRatioF() : p->device()->devicePixelRatioF());
	const QScreen* prim = QGuiApplication::primaryScreen();
	const QScreen* scr = w ? w->screen() : nullptr;
	if (scr && prim && (scr != prim) && (screen_dpi(scr) != screen_dpi(prim)))
		f *= qreal(screen_dpi(scr)) / screen_dpi(prim);
	return f;
}

class xMenuCheckStyle : public QProxyStyle {
	public:
		explicit xMenuCheckStyle(QStyle* base) : QProxyStyle(base) {}
		void drawControl(ControlElement el, const QStyleOption* opt, QPainter* p, const QWidget* w) const override;
};

// with an icon the square stays: it is the item's only sign of the state
void xMenuCheckStyle::drawControl(ControlElement el, const QStyleOption* opt, QPainter* p, const QWidget* w) const {
	const QStyleOptionMenuItem* mi = (el == CE_MenuItem) ? qstyleoption_cast<const QStyleOptionMenuItem*>(opt) : nullptr;
	if (!mi || (mi->menuItemType == QStyleOptionMenuItem::Separator) || (mi->checkType == QStyleOptionMenuItem::NotCheckable)
			|| !mi->checked || !mi->icon.isNull() || !IsThemeActive() || !IsAppThemed()) {
		QProxyStyle::drawControl(el, opt, p, w);
		return;
	}
	const bool dis = !(mi->state & State_Enabled);
	const bool dot = (mi->checkType == QStyleOptionMenuItem::Exclusive);
	const xGlyph& glyph = menu_glyph(w, dis ? (dot ? MC_BULLETDISABLED : MC_CHECKMARKDISABLED) : (dot ? MC_BULLETNORMAL : MC_CHECKMARKNORMAL));
	if (glyph.img.isNull()) {
		QProxyStyle::drawControl(el, opt, p, w);
		return;
	}
	QStyleOptionMenuItem plain(*mi);
	plain.checked = false;
	QProxyStyle::drawControl(el, &plain, p, w);
	// windowsvista's CE_MenuItem geometry, rounding included
	const qreal f = metric_factor(w, p);
	const QSizeF size = QSizeF(glyph.img.size()) * f;
	const QMargins& mg = glyph.margins;
	const int gutter = qRound(3 * f);
	int col = qMax(mi->maxIconWidth, qRound(gutter + size.width() + (mg.left() + mg.right()) * f));
	const QRect& r = mi->rect;
	if (mi->direction == Qt::LeftToRight) col += r.x();
	QRect check = visualRect(mi->direction, r, QRect(r.x(), r.y(), col - (gutter + r.x()), r.height()));
	QRect back(0, 0, qRound(size.width() + (mg.left() + mg.right()) * f), qRound(size.height() + (mg.top() + mg.bottom()) * f));
	back.moveCenter(check.center());
	QRect at(QPoint(0, 0), size.toSize());
	at.moveCenter(back.center());
	p->drawImage(QRectF(at.topLeft(), size), glyph.img);
}

QStyle* menuCheckStyle(QStyle* base) {
	return base ? new xMenuCheckStyle(base) : base;
}

#else

QStyle* menuCheckStyle(QStyle* base) {
	return base;
}

#endif

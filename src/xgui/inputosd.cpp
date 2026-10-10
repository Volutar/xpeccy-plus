// the input overlay

#include <QDateTime>
#include <QPainterPath>
#include <string.h>
#include <math.h>

#include "inputosd.h"
#include "../xcore/xcore.h"

// a tap of one frame is held lit this long, then fades out over the next
#define OSD_HOLD_MS	90
#define OSD_FADE_MS	160

// all in key pitches
#define KBD_W		10.75
#define BLK_H		4.0
#define JOY_CROSS	3.0
#define JOY_GAP		0.4
#define JOY_BTN_EXT	3.2		// F1..F3 rising to the right, F4 above them
#define JOY_BTN		1.2
#define MOU_W		2.6
#define BLK_GAP		0.6
#define FAINT		0.45		// a device the machine lacks or the program is not reading

// Nothing is shaded but a key that is down: a key up is its outline and its
// label, so the picture under the overlay stays as it is
static const QColor colLine(255, 255, 255);
static const QColor colText(0xea, 0xea, 0xea);
static const QColor colHalo(0, 0, 0, 110);		// keeps the white readable on a light picture
static const QColor colLit(0xff, 0xfa, 0x57);
#define LIT_ALPHA	0.7

xInputOsd::xInputOsd() {
	memset(litAt, 0, sizeof(litAt));
	wheelDir = 0;
	wheelWas = 0;
	moveX = 0;
	moveY = 0;
	mxWas = 0;
	myWas = 0;
	mouseSeen = 0;
	now = 0;
	memset(&drawnFor, 0, sizeof(drawnFor));
	memset(&drawnLit, 0, sizeof(drawnLit));
}

double xInputOsd::glow(qint64 at) {
	if (at == 0) return 0.0;
	qint64 dt = now - at;
	if (dt <= OSD_HOLD_MS) return 1.0;
	dt -= OSD_HOLD_MS;
	return (dt >= OSD_FADE_MS) ? 0.0 : 1.0 - double(dt) / OSD_FADE_MS;
}

// light with a dark rim, the way the label of every key is drawn
static void drawGlyph(QPainter& pnt, const QPainterPath& path, double u) {
	pnt.setBrush(Qt::NoBrush);
	pnt.setPen(QPen(colHalo, qMax(1.5, u * 0.09), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	pnt.drawPath(path);
	pnt.setPen(Qt::NoPen);
	pnt.setBrush(colText);
	pnt.drawPath(path);
}

static void drawLabel(QPainter& pnt, const QRectF& rc, const char* txt, double u) {
	double px = u * 0.42;
	if (px < 5.0) return;
	QFont fnt = pnt.font();
	fnt.setBold(true);
	fnt.setPixelSize(qRound(px));
	QPainterPath path;
	path.addText(0, 0, fnt, txt);
	QRectF br = path.boundingRect();
	double sc = 1.0;
	if (br.width() > rc.width() * 0.82)
		sc = rc.width() * 0.82 / br.width();
	if (px * sc < 5.0) return;
	QTransform tr;
	tr.translate(rc.center().x(), rc.center().y());
	tr.scale(sc, sc);
	tr.translate(-br.center().x(), -br.center().y());
	drawGlyph(pnt, tr.map(path), u);
}

// a white outline with a dark rim outside it
static void drawOutline(QPainter& pnt, const QPainterPath& shape, double u) {
	double lw = qMax(1.0, u * 0.045);
	pnt.setBrush(Qt::NoBrush);
	pnt.setPen(QPen(colHalo, lw * 3));
	pnt.drawPath(shape);
	pnt.setPen(QPen(colLine, lw));
	pnt.drawPath(shape);
}

static void fillLit(QPainter& pnt, const QPainterPath& shape, double lit) {
	if (lit <= 0.0) return;
	QColor c = colLit;
	c.setAlphaF(LIT_ALPHA * lit);
	pnt.setPen(Qt::NoPen);
	pnt.setBrush(c);
	pnt.drawPath(shape);
}

// a key or a button: its outline and label with no lit state given, its fill with one
static void drawCap(QPainter& pnt, const QPainterPath& shape, const QRectF& rc, const char* txt, double u, const float* lit) {
	if (lit) {
		fillLit(pnt, shape, *lit);
	} else {
		drawOutline(pnt, shape, u);
		if (txt) drawLabel(pnt, rc, txt, u);
	}
}

extern "C" keyScan keyTab[];		// keyboard.c: the ZX keys, row by row as on the keyboard

static const double rowOff[4] = {0.0, 0.5, 0.75, 0.0};
static const char* keyLabel[4][10] = {
	{"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
	{"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"},
	{"A", "S", "D", "F", "G", "H", "J", "K", "L", "ENTER"},
	{"CAPS", "Z", "X", "C", "V", "B", "N", "M", "SYM", "SPACE"}
};

// a key's rectangle, x and y in pitches from the block's corner
static QRectF keyRect(QPointF org, double x, double y, double w, double u) {
	double gap = u * 0.08;
	return QRectF(org.x() + x * u + gap, org.y() + y * u + gap, w * u - gap * 2, u - gap * 2);
}

// a triangle in the middle of rc pointing dx,dy
static QPainterPath arrow(const QRectF& rc, int dx, int dy, double sz) {
	QPointF c = rc.center();
	QPainterPath path;
	path.moveTo(c.x() + dx * sz, c.y() + dy * sz);
	path.lineTo(c.x() - dx * sz * 0.6 + dy * sz, c.y() - dy * sz * 0.6 + dx * sz);
	path.lineTo(c.x() - dx * sz * 0.6 - dy * sz, c.y() - dy * sz * 0.6 - dx * sz);
	path.closeSubpath();
	return path;
}

// when each thing was last down, and which way the wheel and the mouse went
void xInputOsd::stamp(const InState& st) {
	// keyTab is the keyboard's own order, 10 keys a row
	for (int i = 0; i < 40; i++) {
		if (st.keys[keyTab[i].row] & keyTab[i].mask)
			litAt[LIT_KEY + i] = now;
	}
	for (int i = 0; i < 8; i++) {
		if (st.joy & (1 << i))
			litAt[LIT_JOY + i] = now;
	}
	for (int i = 0; i < 3; i++) {
		if (st.mbtn & (1 << i))		// IVM_LEFT, IVM_RIGHT, IVM_MIDDLE
			litAt[LIT_BTN + i] = now;
	}
	// the wheel is a 4-bit counter and the position a byte each way: what moved
	// since the last frame drawn. The Y port counts up the screen
	if (mouseSeen) {
		int dw = (st.mwheel - wheelWas) & 0x0f;
		if (dw) {
			wheelDir = (dw < 8) ? 1 : -1;
			litAt[LIT_WHEEL] = now;
		}
		int dx = (signed char)(st.mx - mxWas);
		int dy = -(signed char)(st.my - myWas);
		if (dx || dy) {
			double len = sqrt(double(dx * dx + dy * dy));
			moveX = dx / len;
			moveY = dy / len;
			litAt[LIT_MOVE] = now;
		}
	}
	mouseSeen = 1;
	wheelWas = st.mwheel;
	mxWas = st.mx;
	myWas = st.my;
}

void xInputOsd::paintKeys(QPainter& pnt, QPointF org, double u, const Lit* lit) {
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 10; c++) {
			double x = rowOff[r] + c;
			double w = 1.0;
			if (r == 3) {			// the bottom row: wide CAPS and SPACE
				if (c == 0) {
					w = 1.25;
				} else {
					x = 0.25 + c;
					if (c == 9) w = 1.5;
				}
			}
			QRectF rc = keyRect(org, x, r, w, u);
			QPainterPath shape;
			shape.addRoundedRect(rc, u * 0.14, u * 0.14);
			drawCap(pnt, shape, rc, keyLabel[r][c], u, lit ? &lit->lit[LIT_KEY + r * 10 + c] : nullptr);
		}
	}
}

// The cross is four keys of the keyboard's size on its bottom three rows. The
// buttons are laid out as on a Sega pad: F1..F3 in a row rising to the right,
// F4 above them where Start is.
void xInputOsd::paintJoy(QPainter& pnt, QPointF org, double u, const Lit* lit, bool ext) {
	// R L D U: kempston bits 0..3
	static const struct {int dx; int dy; double x; double y;} arm[4] = {
		{1, 0, 2, 2}, {-1, 0, 0, 2}, {0, 1, 1, 3}, {0, -1, 1, 1}
	};
	for (int i = 0; i < 4; i++) {
		QRectF rc = keyRect(org, arm[i].x, arm[i].y, 1.0, u);
		QPainterPath shape;
		shape.addRoundedRect(rc, u * 0.14, u * 0.14);
		drawCap(pnt, shape, rc, nullptr, u, lit ? &lit->lit[LIT_JOY + i] : nullptr);
		if (!lit) drawGlyph(pnt, arrow(rc, arm[i].dx, arm[i].dy, u * 0.2), u);
	}
	double bx = org.x() + (JOY_CROSS + JOY_GAP) * u;
	static const char* lab[4] = {"F1", "F2", "F3", "F4"};
	// centres, in pitches from the buttons' corner, and diameters
	static const double pos[4][3] = {{0.5, 3.15, 0.95}, {1.6, 2.85, 0.95}, {2.7, 2.55, 0.95}, {0.9, 1.45, 0.75}};
	static const double one[3] = {JOY_BTN / 2, 2.5, JOY_BTN};
	for (int i = 0; i < (ext ? 4 : 1); i++) {
		const double* p = ext ? pos[i] : one;
		double d = p[2] * u;
		QRectF rc(bx + p[0] * u - d / 2, org.y() + p[1] * u - d / 2, d, d);
		QPainterPath shape;
		shape.addEllipse(rc);
		drawCap(pnt, shape, rc, ext ? lab[i] : "FIRE", u, lit ? &lit->lit[LIT_JOY + 4 + i] : nullptr);
	}
}

void xInputOsd::paintMouse(QPainter& pnt, QPointF org, double u, const Lit* lit) {
	QRectF body(org.x() + u * 0.08, org.y() + u * 0.08, MOU_W * u - u * 0.16, BLK_H * u - u * 0.16);
	QPainterPath bp;
	bp.addRoundedRect(body, u * 1.1, u * 1.1);
	double slot = u * 0.5;
	double top = u * 1.7;
	QRectF whl(body.center().x() - slot * 0.42, body.top() + u * 0.3, slot * 0.84, top - u * 0.6);
	QPainterPath wp;
	wp.addRoundedRect(whl, whl.width() / 2, whl.width() / 2);
	QPointF mc(body.center().x(), body.top() + u * 2.8);
	double ring = u * 0.62;
	if (!lit) {
		QPainterPath lines = bp;
		lines.moveTo(body.left(), body.top() + top);
		lines.lineTo(body.right(), body.top() + top);
		drawOutline(pnt, lines, u);
		drawOutline(pnt, wp, u);
		QPainterPath rp;
		rp.addEllipse(mc, ring, ring);
		drawOutline(pnt, rp, u);
		return;
	}
	// the two buttons are the body's top, either side of the wheel
	for (int i = 0; i < 2; i++) {
		QRectF half = (i == 0) ? QRectF(body.left(), body.top(), body.width() / 2 - slot / 2, top)
			: QRectF(body.center().x() + slot / 2, body.top(), body.width() / 2 - slot / 2, top);
		pnt.save();
		pnt.setClipRect(half);
		fillLit(pnt, bp, lit->lit[LIT_BTN + i]);
		pnt.restore();
	}
	fillLit(pnt, wp, lit->lit[LIT_BTN + 2]);		// the middle button is the wheel
	QRectF tip = (lit->wheelDir < 0) ? QRectF(whl.left(), whl.top() - u * 0.32, whl.width(), u * 0.25)
		: QRectF(whl.left(), whl.bottom() + u * 0.07, whl.width(), u * 0.25);
	fillLit(pnt, arrow(tip, 0, lit->wheelDir, u * 0.22), lit->lit[LIT_WHEEL]);
	// which way it moves, a dot off the middle of the ring
	QPainterPath dot;
	dot.addEllipse(QPointF(mc.x() + lit->moveX * ring * 0.62, mc.y() + lit->moveY * ring * 0.62), u * 0.2, u * 0.2);
	fillLit(pnt, dot, lit->lit[LIT_MOVE]);
}

// the blocks side by side: their lines with no lit state given, their fills with one
void xInputOsd::paintBlocks(QPainter& pnt, const Layout& lay, const Lit* lit) {
	double u = lay.u;
	QPointF at(lay.dx, lay.dy);
	pnt.setRenderHint(QPainter::Antialiasing, true);
	if (lay.keys) {
		paintKeys(pnt, at, u, lit);
		at.rx() += (KBD_W + BLK_GAP) * u;
	}
	if (lay.joy) {
		pnt.setOpacity(lay.joyLive ? 1.0 : FAINT);
		paintJoy(pnt, at, u, lit, lay.ext);
		at.rx() += (JOY_CROSS + JOY_GAP + (lay.ext ? JOY_BTN_EXT : JOY_BTN) + BLK_GAP) * u;
	}
	if (lay.mouse) {
		pnt.setOpacity(lay.mouseLive ? 1.0 : FAINT);
		paintMouse(pnt, at, u, lit);
	}
	pnt.setOpacity(1.0);
}

void xInputOsd::paint(QPainter& pnt, const QRect& pic, const InState& st, qreal dpr) {
	now = QDateTime::currentMSecsSinceEpoch();
	Layout lay;
	memset(&lay, 0, sizeof(lay));		// compared whole, padding included
	lay.keys = conf.iosd.keys;
	lay.joy = conf.iosd.joy;
	lay.mouse = conf.iosd.mouse;
	if (!lay.keys && !lay.joy && !lay.mouse) return;
	lay.ext = st.ext;
	lay.joyLive = st.joyLive;
	lay.mouseLive = st.mouseLive;
	lay.dpr = dpr;
	double wid = 0;
	int blocks = 0;
	if (lay.keys) {wid += KBD_W; blocks++;}
	if (lay.joy) {wid += JOY_CROSS + JOY_GAP + (lay.ext ? JOY_BTN_EXT : JOY_BTN); blocks++;}
	if (lay.mouse) {wid += MOU_W; blocks++;}
	wid += BLK_GAP * (blocks - 1);
	// the size is the keyboard's share of the picture, and the whole strip has to fit
	double u = pic.width() * conf.iosd.size / 100.0 / KBD_W;
	double room = pic.width() * 0.96;
	if (wid * u > room) u = room / wid;
	double margin = pic.height() * 0.025;
	double x, y;
	switch (conf.iosd.pos) {
		case IOSD_POS_BOTTOM_LEFT: case IOSD_POS_TOP_LEFT: x = pic.left() + margin; break;
		case IOSD_POS_BOTTOM_RIGHT: case IOSD_POS_TOP_RIGHT: x = pic.right() + 1 - margin - wid * u; break;
		default: x = pic.left() + (pic.width() - wid * u) / 2; break;
	}
	if (conf.iosd.pos >= IOSD_POS_TOP_LEFT) {
		y = pic.top() + margin;
	} else {
		y = pic.bottom() + 1 - margin - BLK_H * u;
	}
	double pad = u * 0.2;
	double x0 = floor(x - pad);
	double y0 = floor(y - pad);
	lay.u = u;
	lay.dx = x - x0;
	lay.dy = y - y0;
	lay.w = ceil((x + wid * u + pad - x0) * dpr);
	lay.h = ceil((y + BLK_H * u + pad - y0) * dpr);
	// Drawn into images: on the GL window QPainter goes through the OpenGL
	// engine, which ignores antialiasing on a surface with no multisampling
	bool relayout = baseImg.isNull() || memcmp(&lay, &drawnFor, sizeof(lay));
	if (relayout) {
		drawnFor = lay;
		baseImg = QImage(lay.w, lay.h, QImage::Format_ARGB32_Premultiplied);
		baseImg.setDevicePixelRatio(dpr);
		baseImg.fill(Qt::transparent);
		QPainter bp(&baseImg);
		paintBlocks(bp, lay, nullptr);
		osdImg = QImage(lay.w, lay.h, QImage::Format_ARGB32_Premultiplied);
		osdImg.setDevicePixelRatio(dpr);
	}
	stamp(st);
	Lit lit;
	memset(&lit, 0, sizeof(lit));
	for (int i = 0; i < LIT_COUNT; i++)
		lit.lit[i] = glow(litAt[i]);
	lit.wheelDir = wheelDir;
	lit.moveX = moveX;
	lit.moveY = moveY;
	// an image left as it was keeps its texture on the GL side too
	if (relayout || memcmp(&lit, &drawnLit, sizeof(lit))) {
		drawnLit = lit;
		osdImg.fill(Qt::transparent);
		QPainter ip(&osdImg);
		paintBlocks(ip, lay, &lit);
		ip.drawImage(0, 0, baseImg);
	}
	// the opacity goes on the whole of it, so an outline over its rim does not show both
	pnt.save();
	pnt.setOpacity(conf.iosd.opacity / 100.0);
	pnt.drawImage(QPointF(x0, y0), osdImg);
	pnt.restore();
}

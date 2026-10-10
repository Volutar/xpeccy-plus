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

// Nothing is shaded but a key that is down: a key up is its outline and its
// label, so the picture under the overlay stays as it is
static const QColor colLine(255, 255, 255);
static const QColor colText(0xea, 0xea, 0xea);
static const QColor colHalo(0, 0, 0, 110);		// keeps the white readable on a light picture
static const QColor colLit(0xff, 0xfa, 0x57);
#define LIT_ALPHA	0.7

xInputOsd::xInputOsd() {
	memset(keyAt, 0, sizeof(keyAt));
	memset(joyAt, 0, sizeof(joyAt));
	memset(btnAt, 0, sizeof(btnAt));
	wheelAt = 0;
	wheelDir = 0;
	wheelWas = 0;
	moveAt = 0;
	moveX = 0;
	moveY = 0;
	mxWas = 0;
	myWas = 0;
	mouseSeen = 0;
	now = 0;
}

double xInputOsd::glow(qint64 at) {
	if (at == 0) return 0.0;
	qint64 dt = now - at;
	if (dt <= OSD_HOLD_MS) return 1.0;
	dt -= OSD_HOLD_MS;
	return (dt >= OSD_FADE_MS) ? 0.0 : 1.0 - double(dt) / OSD_FADE_MS;
}

static QColor litFill(double lit) {
	QColor c = colLit;
	c.setAlphaF(LIT_ALPHA * lit);
	return c;
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

static void drawShape(QPainter& pnt, const QPainterPath& shape, double lit, double u) {
	if (lit > 0.0) {
		pnt.setPen(Qt::NoPen);
		pnt.setBrush(litFill(lit));
		pnt.drawPath(shape);
	}
	drawOutline(pnt, shape, u);
}

void xInputOsd::drawKey(QPainter& pnt, const QRectF& rc, const char* txt, double lit, double u) {
	QPainterPath shape;
	shape.addRoundedRect(rc, u * 0.14, u * 0.14);
	drawShape(pnt, shape, lit, u);
	if (txt) drawLabel(pnt, rc, txt, u);
}

void xInputOsd::drawButton(QPainter& pnt, const QRectF& rc, const char* txt, double lit, double u) {
	QPainterPath shape;
	shape.addEllipse(rc);
	drawShape(pnt, shape, lit, u);
	if (txt) drawLabel(pnt, rc, txt, u);
}

// Each key is the half-row it answers on (address line A8+n) and its bit. The
// left five of a row go outwards from bit 0, the right five inwards.
static const int rowLeft[4] = {3, 2, 1, 0};
static const int rowRight[4] = {4, 5, 6, 7};
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

void xInputOsd::paintKeys(QPainter& pnt, QPointF org, double u, const InState& st) {
	for (int r = 0; r < 4; r++) {
		for (int c = 0; c < 10; c++) {
			int line = (c < 5) ? rowLeft[r] : rowRight[r];
			int bit = (c < 5) ? c : 9 - c;
			if (st.keys[line] & (1 << bit))
				keyAt[line][bit] = now;
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
			drawKey(pnt, keyRect(org, x, r, w, u), keyLabel[r][c], glow(keyAt[line][bit]), u);
		}
	}
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

// The cross is four keys of the keyboard's size on its bottom three rows. The
// buttons are laid out as on a Sega pad: F1..F3 in a row rising to the right,
// F4 above them where Start is.
void xInputOsd::paintJoy(QPainter& pnt, QPointF org, double u, const InState& st, bool ext) {
	for (int i = 0; i < 8; i++) {
		if (st.joy & (1 << i))
			joyAt[i] = now;
	}
	// R L D U: kempston bits 0..3
	struct {int bit; int dx; int dy; double x; double y;} arm[4] = {
		{0, 1, 0, 2, 2}, {1, -1, 0, 0, 2}, {2, 0, 1, 1, 3}, {3, 0, -1, 1, 1}
	};
	for (int i = 0; i < 4; i++) {
		QRectF rc = keyRect(org, arm[i].x, arm[i].y, 1.0, u);
		drawKey(pnt, rc, nullptr, glow(joyAt[arm[i].bit]), u);
		drawGlyph(pnt, arrow(rc, arm[i].dx, arm[i].dy, u * 0.2), u);
	}
	double bx = org.x() + (JOY_CROSS + JOY_GAP) * u;
	if (ext) {
		static const char* lab[4] = {"F1", "F2", "F3", "F4"};
		// centres, in pitches from the buttons' corner, and diameters
		static const double pos[4][3] = {{0.5, 3.15, 0.95}, {1.6, 2.85, 0.95}, {2.7, 2.55, 0.95}, {0.9, 1.45, 0.75}};
		for (int i = 0; i < 4; i++) {
			double d = pos[i][2] * u;
			QRectF rc(bx + pos[i][0] * u - d / 2, org.y() + pos[i][1] * u - d / 2, d, d);
			drawButton(pnt, rc, lab[i], glow(joyAt[4 + i]), u);
		}
	} else {
		double d = u * JOY_BTN;
		QRectF rc(bx, org.y() + 2.5 * u - d / 2, d, d);
		drawButton(pnt, rc, "FIRE", glow(joyAt[4]), u);
	}
}

void xInputOsd::paintMouse(QPainter& pnt, QPointF org, double u, const InState& st) {
	static const int btnBit[3] = {IVM_LEFT, IVM_RIGHT, IVM_MIDDLE};
	for (int i = 0; i < 3; i++) {
		if (st.mbtn & btnBit[i])
			btnAt[i] = now;
	}
	// the wheel is a 4-bit counter and the position a byte each way: what moved
	// since the last frame drawn. The Y port counts up the screen
	if (mouseSeen) {
		int dw = (st.mwheel - wheelWas) & 0x0f;
		if (dw) {
			wheelDir = (dw < 8) ? 1 : -1;
			wheelAt = now;
		}
		int dx = (signed char)(st.mx - mxWas);
		int dy = -(signed char)(st.my - myWas);
		if (dx || dy) {
			double len = sqrt(double(dx * dx + dy * dy));
			moveX = dx / len;
			moveY = dy / len;
			moveAt = now;
		}
	}
	mouseSeen = 1;
	wheelWas = st.mwheel;
	mxWas = st.mx;
	myWas = st.my;

	QRectF body(org.x() + u * 0.08, org.y() + u * 0.08, MOU_W * u - u * 0.16, BLK_H * u - u * 0.16);
	QPainterPath bp;
	bp.addRoundedRect(body, u * 1.1, u * 1.1);
	// the two buttons are cut out of the body's top
	double slot = u * 0.5;
	double top = u * 1.7;
	pnt.setPen(Qt::NoPen);
	for (int i = 0; i < 2; i++) {
		double lit = glow(btnAt[i]);
		if (lit <= 0.0) continue;
		QRectF half = (i == 0) ? QRectF(body.left(), body.top(), body.width() / 2 - slot / 2, top)
			: QRectF(body.center().x() + slot / 2, body.top(), body.width() / 2 - slot / 2, top);
		QPainterPath hp;
		hp.addRect(half);
		pnt.setBrush(litFill(lit));
		pnt.drawPath(bp.intersected(hp));
	}
	QPainterPath lines = bp;
	lines.moveTo(body.left(), body.top() + top);
	lines.lineTo(body.right(), body.top() + top);
	drawOutline(pnt, lines, u);
	// the wheel, the middle button under it
	QRectF whl(body.center().x() - slot * 0.42, body.top() + u * 0.3, slot * 0.84, top - u * 0.6);
	QPainterPath wp;
	wp.addRoundedRect(whl, whl.width() / 2, whl.width() / 2);
	drawShape(pnt, wp, glow(btnAt[2]), u);
	double wlit = glow(wheelAt);
	if (wlit > 0.0) {
		QRectF tip = (wheelDir < 0) ? QRectF(whl.left(), whl.top() - u * 0.32, whl.width(), u * 0.25)
			: QRectF(whl.left(), whl.bottom() + u * 0.07, whl.width(), u * 0.25);
		pnt.setPen(Qt::NoPen);
		pnt.setBrush(litFill(wlit));
		pnt.drawPath(arrow(tip, 0, wheelDir, u * 0.22));
	}
	// which way it moves, a dot off the middle of a ring
	QPointF mc(body.center().x(), body.top() + u * 2.8);
	double ring = u * 0.62;
	QPainterPath rp;
	rp.addEllipse(mc, ring, ring);
	drawOutline(pnt, rp, u);
	double mlv = glow(moveAt);
	if (mlv > 0.0) {
		QPainterPath dot;
		dot.addEllipse(QPointF(mc.x() + moveX * ring * 0.62, mc.y() + moveY * ring * 0.62), u * 0.2, u * 0.2);
		pnt.setPen(Qt::NoPen);
		pnt.setBrush(litFill(mlv));
		pnt.drawPath(dot);
	}
}

void xInputOsd::paint(QPainter& pnt, const QRect& pic, const InState& st, bool reads) {
	Q_UNUSED(reads);
	now = QDateTime::currentMSecsSinceEpoch();
	bool keys = conf.iosd.keys;
	bool joy = conf.iosd.joy;
	bool mou = conf.iosd.mouse;
	if (!keys && !joy && !mou) return;
	bool ext = conf.zx->joy->extbuttons;
	double joyW = JOY_CROSS + JOY_GAP + (ext ? JOY_BTN_EXT : JOY_BTN);
	double wid = 0;
	int blocks = 0;
	if (keys) {wid += KBD_W; blocks++;}
	if (joy) {wid += joyW; blocks++;}
	if (mou) {wid += MOU_W; blocks++;}
	wid += BLK_GAP * (blocks - 1);
	double hig = BLK_H;
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
		y = pic.bottom() + 1 - margin - hig * u;
	}
	double op = conf.iosd.opacity / 100.0;
	pnt.save();
	pnt.setRenderHint(QPainter::Antialiasing, true);
	pnt.setOpacity(op);
	QPointF at(x, y);
	if (keys) {
		paintKeys(pnt, at, u, st);
		at.rx() += (KBD_W + BLK_GAP) * u;
	}
	// a device the machine lacks, or the program is not reading, is drawn faint
	if (joy) {
		pnt.setOpacity(st.joyLive ? op : op * 0.45);
		paintJoy(pnt, at, u, st, ext);
		at.rx() += (joyW + BLK_GAP) * u;
	}
	if (mou) {
		pnt.setOpacity(st.mouseLive ? op : op * 0.45);
		paintMouse(pnt, at, u, st);
	}
	pnt.restore();
}

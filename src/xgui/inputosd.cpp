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
#define JOY_CROSS	4.0
#define JOY_GAP		0.3
#define JOY_BTN_EXT	1.9
#define JOY_BTN		1.4
#define MOU_W		2.6
#define BLK_GAP		0.6
#define PAD		0.3

static const QColor colKey(40, 40, 44);
static const QColor colEdge(130, 130, 140);
static const QColor colText(210, 210, 210);
static const QColor colLit(0, 200, 255);
static const QColor colLitText(0, 0, 0);

static QColor mix(const QColor& a, const QColor& b, double f) {
	return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * f,
		a.greenF() + (b.greenF() - a.greenF()) * f,
		a.blueF() + (b.blueF() - a.blueF()) * f);
}

// a key up is mostly glass, so the picture shows through it; one down is solid
#define KEY_GLASS	0.4

static QColor keyFill(double lit, const QColor& up = colKey) {
	QColor c = mix(up, colLit, lit);
	c.setAlphaF(KEY_GLASS + (1.0 - KEY_GLASS) * lit);
	return c;
}

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

static void fitText(QPainter& pnt, const QRectF& rc, const char* txt, double px) {
	if (px < 5.0) return;
	QFont fnt = pnt.font();
	fnt.setBold(true);
	fnt.setPixelSize(qRound(px));
	QFontMetricsF fm(fnt);
	double w = fm.horizontalAdvance(txt);
	if (w > rc.width() * 0.86) {
		px *= rc.width() * 0.86 / w;
		if (px < 5.0) return;
		fnt.setPixelSize(qRound(px));
	}
	pnt.setFont(fnt);
	pnt.drawText(rc, Qt::AlignCenter, txt);
}

void xInputOsd::drawKey(QPainter& pnt, const QRectF& rc, const char* txt, double lit, double u) {
	double base = pnt.opacity();
	pnt.setOpacity(base + (1.0 - base) * lit);
	pnt.setPen(QPen(mix(colEdge, colLit.lighter(130), lit), qMax(1.0, u * 0.05)));
	pnt.setBrush(keyFill(lit));
	pnt.drawRoundedRect(rc, u * 0.14, u * 0.14);
	if (txt) {
		pnt.setPen(mix(colText, colLitText, lit));
		fitText(pnt, rc, txt, u * 0.36);
	}
	pnt.setOpacity(base);
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

void xInputOsd::paintKeys(QPainter& pnt, QPointF org, double u, const InState& st) {
	double gap = u * 0.08;
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
			QRectF rc(org.x() + x * u + gap, org.y() + r * u + gap, w * u - gap * 2, u - gap * 2);
			drawKey(pnt, rc, keyLabel[r][c], glow(keyAt[line][bit]), u);
		}
	}
}

// a triangle in the middle of rc pointing dx,dy
static void arrow(QPainter& pnt, const QRectF& rc, int dx, int dy, double sz, const QColor& col) {
	QPointF c = rc.center();
	QPointF tip(c.x() + dx * sz, c.y() + dy * sz);
	QPointF b1(c.x() - dx * sz * 0.6 + dy * sz, c.y() - dy * sz * 0.6 + dx * sz);
	QPointF b2(c.x() - dx * sz * 0.6 - dy * sz, c.y() - dy * sz * 0.6 - dx * sz);
	QPointF pts[3] = {tip, b1, b2};
	pnt.setPen(Qt::NoPen);
	pnt.setBrush(col);
	pnt.drawPolygon(pts, 3);
}

void xInputOsd::paintJoy(QPainter& pnt, QPointF org, double u, const InState& st, bool ext) {
	for (int i = 0; i < 8; i++) {
		if (st.joy & (1 << i))
			joyAt[i] = now;
	}
	QPointF c(org.x() + JOY_CROSS * u / 2, org.y() + BLK_H * u / 2);
	double aw = u * 1.15;		// arm width
	double in = u * 0.6;		// the hub
	double out = u * 1.95;
	// R L D U: kempston bits 0..3
	struct {int bit; int dx; int dy;} arm[4] = {{0, 1, 0}, {1, -1, 0}, {2, 0, 1}, {3, 0, -1}};
	for (int i = 0; i < 4; i++) {
		QRectF rc;
		if (arm[i].dx) {
			double x0 = (arm[i].dx > 0) ? c.x() + in : c.x() - out;
			rc = QRectF(x0, c.y() - aw / 2, out - in, aw);
		} else {
			double y0 = (arm[i].dy > 0) ? c.y() + in : c.y() - out;
			rc = QRectF(c.x() - aw / 2, y0, aw, out - in);
		}
		double lit = glow(joyAt[arm[i].bit]);
		drawKey(pnt, rc, nullptr, lit, u);
		double base = pnt.opacity();
		pnt.setOpacity(base + (1.0 - base) * lit);
		arrow(pnt, rc, arm[i].dx, arm[i].dy, u * 0.28, mix(colText, colLitText, lit));
		pnt.setOpacity(base);
	}
	pnt.setPen(QPen(colEdge, qMax(1.0, u * 0.05)));
	pnt.setBrush(keyFill(0.0));
	pnt.drawEllipse(c, u * 0.42, u * 0.42);
	// the fire buttons, beside the cross
	double bx = org.x() + (JOY_CROSS + JOY_GAP) * u;
	if (ext) {
		static const char* lab[4] = {"F", "2", "3", "4"};
		double d = u * 0.85;
		double y0 = c.y() - d - u * 0.1;
		for (int i = 0; i < 4; i++) {
			QRectF rc(bx + (i & 1) * (d + u * 0.2), y0 + (i >> 1) * (d + u * 0.2), d, d);
			double lit = glow(joyAt[4 + i]);
			double base = pnt.opacity();
			pnt.setOpacity(base + (1.0 - base) * lit);
			pnt.setPen(QPen(mix(colEdge, colLit.lighter(130), lit), qMax(1.0, u * 0.05)));
			pnt.setBrush(keyFill(lit));
			pnt.drawEllipse(rc);
			pnt.setPen(mix(colText, colLitText, lit));
			fitText(pnt, rc, lab[i], u * 0.36);
			pnt.setOpacity(base);
		}
	} else {
		double d = u * JOY_BTN;
		QRectF rc(bx, c.y() - d / 2, d, d);
		double lit = glow(joyAt[4]);
		double base = pnt.opacity();
		pnt.setOpacity(base + (1.0 - base) * lit);
		pnt.setPen(QPen(mix(colEdge, colLit.lighter(130), lit), qMax(1.0, u * 0.05)));
		pnt.setBrush(keyFill(lit));
		pnt.drawEllipse(rc);
		pnt.setPen(mix(colText, colLitText, lit));
		fitText(pnt, rc, "FIRE", u * 0.36);
		pnt.setOpacity(base);
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

	double lw = qMax(1.0, u * 0.05);
	QRectF body(org.x(), org.y(), MOU_W * u, BLK_H * u);
	QPainterPath bp;
	bp.addRoundedRect(body, u * 1.1, u * 1.1);
	pnt.setPen(Qt::NoPen);
	pnt.setBrush(keyFill(0.0));
	pnt.drawPath(bp);
	// the two buttons, cut out of the body's top
	double slot = u * 0.5;
	double top = u * 1.7;
	for (int i = 0; i < 2; i++) {
		QRectF half = (i == 0) ? QRectF(body.left(), body.top(), body.width() / 2 - slot / 2, top)
			: QRectF(body.center().x() + slot / 2, body.top(), body.width() / 2 - slot / 2, top);
		QPainterPath hp;
		hp.addRect(half);
		double lit = glow(btnAt[i]);
		if (lit <= 0.0) continue;		// the body under it is the button up
		double base = pnt.opacity();
		pnt.setOpacity(base + (1.0 - base) * lit);
		pnt.setBrush(keyFill(lit));
		pnt.drawPath(bp.intersected(hp));
		pnt.setOpacity(base);
	}
	pnt.setPen(QPen(colEdge, lw));
	pnt.setBrush(Qt::NoBrush);
	pnt.drawPath(bp);
	pnt.drawLine(QPointF(body.left(), body.top() + top), QPointF(body.right(), body.top() + top));
	// the wheel, the middle button under it
	QRectF whl(body.center().x() - slot * 0.42, body.top() + u * 0.3, slot * 0.84, top - u * 0.6);
	double mlit = glow(btnAt[2]);
	double base = pnt.opacity();
	pnt.setOpacity(base + (1.0 - base) * mlit);
	pnt.setPen(QPen(mix(colEdge, colLit.lighter(130), mlit), lw));
	pnt.setBrush(keyFill(mlit, colKey.lighter(150)));
	pnt.drawRoundedRect(whl, whl.width() / 2, whl.width() / 2);
	pnt.setOpacity(base);
	double wlit = glow(wheelAt);
	if (wlit > 0.0) {
		pnt.setOpacity(base + (1.0 - base) * wlit);
		QColor col = mix(colText, colLit, wlit);
		double sz = u * 0.22;
		QRectF tip = (wheelDir < 0) ? QRectF(whl.left(), whl.top() - u * 0.32, whl.width(), u * 0.25)
			: QRectF(whl.left(), whl.bottom() + u * 0.07, whl.width(), u * 0.25);
		arrow(pnt, tip, 0, wheelDir, sz, col);
		pnt.setOpacity(base);
	}
	// which way it moves, a dot off the middle of a ring
	QPointF mc(body.center().x(), body.top() + u * 2.85);
	double ring = u * 0.62;
	double mlv = glow(moveAt);
	pnt.setPen(QPen(colEdge, lw));
	pnt.setBrush(Qt::NoBrush);
	pnt.drawEllipse(mc, ring, ring);
	if (mlv > 0.0) {
		pnt.setOpacity(base + (1.0 - base) * mlv);
		pnt.setPen(Qt::NoPen);
		pnt.setBrush(colLit);
		pnt.drawEllipse(QPointF(mc.x() + moveX * ring * 0.62, mc.y() + moveY * ring * 0.62), u * 0.2, u * 0.2);
		pnt.setOpacity(base);
	}
}

void xInputOsd::paint(QPainter& pnt, const QRect& pic, const InState& st, bool reads) {
	now = QDateTime::currentMSecsSinceEpoch();
	bool keys = conf.iosd.keys;
	bool joy = conf.iosd.joy;
	bool mou = conf.iosd.mouse;
	if (!keys && !joy && !mou) return;
	bool ext = conf.zx->joy->extbuttons;
	double wid = 0;
	int blocks = 0;
	if (keys) {wid += KBD_W; blocks++;}
	if (joy) {wid += JOY_CROSS + JOY_GAP + (ext ? JOY_BTN_EXT : JOY_BTN); blocks++;}
	if (mou) {wid += MOU_W; blocks++;}
	wid += BLK_GAP * (blocks - 1) + PAD * 2;
	double hig = BLK_H + PAD * 2;
	// the size is the keyboard's share of the picture, and the whole strip has to fit
	double u = pic.width() * conf.iosd.size / 100.0 / KBD_W;
	double room = pic.width() * 0.96;
	if (wid * u > room) u = room / wid;
	double margin = pic.height() * 0.02;
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
	pnt.setRenderHint(QPainter::TextAntialiasing, true);
	// a dark ground under the whole strip, so the keys read over any picture
	pnt.setOpacity(op * 0.3);
	pnt.setPen(Qt::NoPen);
	pnt.setBrush(Qt::black);
	pnt.drawRoundedRect(QRectF(x, y, wid * u, hig * u), u * 0.35, u * 0.35);
	pnt.setOpacity(op);
	QPointF at(x + PAD * u, y + PAD * u);
	if (keys) {
		paintKeys(pnt, at, u, st);
		at.rx() += (KBD_W + BLK_GAP) * u;
	}
	// a device the machine lacks, or the program is not reading, is drawn faint
	if (joy) {
		pnt.setOpacity(st.joyLive ? op : op * 0.45);
		paintJoy(pnt, at, u, st, ext);
		at.rx() += (JOY_CROSS + JOY_GAP + (ext ? JOY_BTN_EXT : JOY_BTN) + BLK_GAP) * u;
	}
	if (mou) {
		pnt.setOpacity(st.mouseLive ? op : op * 0.45);
		paintMouse(pnt, at, u, st);
	}
	pnt.restore();
}

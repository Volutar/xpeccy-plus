// The on-screen keyboard's picture, drawn from its layout file.
//
// The layout is Volutar's (res/vkbd/zx-*.json), in the format his layout_svg.py
// turns into an SVG, and it is drawn by the same rules: a key is anchored at its
// centre, a label sits at anchor + (dx, dy), "sn" snaps either to the pixel grid,
// "tl" squeezes a text run to a width and "th" stretches it to a height, the
// contour around a key is cut out of the ground so a pressed key's light can
// show through it. What the SVG cannot say is done here at the size the window
// has: key edges and text baselines land on whole pixels of that size, and the
// text is hinted (FreeType, vertical-only) and may be subpixel.

#include "vkbdskin.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRegion>

#include <cmath>
#include <cstring>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_ADVANCES_H

// ---------------------------------------------------------------- helpers

static int fr(double v) {
	return (int)std::floor(v + 0.5);
}

static double snapTo(double v, const QString& mode) {
	if (mode == "px") return fr(v);
	if (mode == "half") return fr(v * 2) / 2.0;
	return v;
}

static double num(const QJsonObject& o, const char* key, double def) {
	QJsonValue v = o.value(key);
	return v.isDouble() ? v.toDouble() : def;
}

// a field under its name or under the one older layouts used
static QJsonValue field(const QJsonObject& o, const char* key, const char* old) {
	return o.contains(key) ? o.value(key) : o.value(old);
}

static QPointF point(const QJsonValue& v) {
	QJsonArray a = v.toArray();
	return QPointF(a.at(0).toDouble(), a.at(1).toDouble());
}

static QVector<QPointF> points(const QJsonValue& v) {
	QVector<QPointF> res;
	foreach(QJsonValue p, v.toArray())
		res.append(point(p));
	return res;
}

static bool isRect(const QJsonArray& a) {
	return (a.size() == 4) && a.at(0).isDouble() && a.at(1).isDouble() && a.at(2).isDouble() && a.at(3).isDouble();
}

static QPainterPath rrect(double x, double y, double w, double h, double r) {
	QPainterPath p;
	r = std::max(0.0, std::min(r, std::min(w / 2.0, h / 2.0)));
	p.addRoundedRect(QRectF(x, y, w, h), r, r);
	return p;
}

// A Spectrum key by its name in a layout's "zx", as keyEntry::zxKey has it; 0 for
// a name that is none
static unsigned char zxKeyNamed(QString name) {
	name = name.trimmed().toUpper();
	if (name == "CS") return 'C';
	if (name == "SS") return 'S';
	if (name == "ENTER") return 'E';
	if (name == "SPACE") return ' ';
	if (name.size() != 1) return 0;
	char c = name[0].toLatin1();
	if ((c >= '0') && (c <= '9')) return c;
	if ((c >= 'A') && (c <= 'Z')) return c - 'A' + 'a';
	return 0;
}

// ---------------------------------------------------------------- outlines

// A polygon of right angles without the points that do not turn it: a point
// repeated, or one in the middle of a straight edge
static QVector<QPointF> cleanPoly(QVector<QPointF> p) {
	bool again = true;
	while (again && (p.size() > 2)) {
		again = false;
		for (int i = 0; i < p.size(); i++) {
			const QPointF& a = p[(i + p.size() - 1) % p.size()];
			const QPointF& v = p[i];
			const QPointF& b = p[(i + 1) % p.size()];
			bool same = (a == v);
			bool straight = ((a.x() == v.x()) && (v.x() == b.x())) || ((a.y() == v.y()) && (v.y() == b.y()));
			if (same || straight) {
				p.remove(i);
				again = true;
				break;
			}
		}
	}
	return p;
}

// whether every edge of a cleaned polygon is upright or level
static bool isRectilinear(const QVector<QPointF>& p) {
	if (p.size() < 4) return false;
	for (int i = 0; i < p.size(); i++) {
		const QPointF& a = p[i];
		const QPointF& b = p[(i + 1) % p.size()];
		if ((a.x() != b.x()) && (a.y() != b.y())) return false;
	}
	return true;
}

// twice the signed area: its sign is the way round the points go
static double polyArea(const QVector<QPointF>& p) {
	double a = 0;
	for (int i = 0; i < p.size(); i++) {
		const QPointF& u = p[i];
		const QPointF& v = p[(i + 1) % p.size()];
		a += u.x() * v.y() - v.x() * u.y();
	}
	return a;
}

static int sgn(double v) {
	return (v > 0) - (v < 0);
}

// A polygon of right angles pushed out by l, t, r, b across its edges that face
// left, up, right and down (in, for negative values): every edge moves along its
// own outward normal, and every vertex is where its two edges meet again
static QVector<QPointF> offsetPoly4(const QVector<QPointF>& p, double l, double t, double r, double b) {
	int n = p.size();
	int way = (polyArea(p) > 0) ? 1 : -1;
	QVector<double> at(n);		// edge i, p[i] to p[i + 1]: its x if upright, its y if level
	QVector<bool> upright(n);
	for (int i = 0; i < n; i++) {
		QPointF d = p[(i + 1) % n] - p[i];
		upright[i] = (d.x() == 0);
		if (upright[i]) {
			int out = way * sgn(d.y());		// 1: it faces right
			at[i] = p[i].x() + out * ((out > 0) ? r : l);
		} else {
			int out = -way * sgn(d.x());		// 1: it faces down
			at[i] = p[i].y() + out * ((out > 0) ? b : t);
		}
	}
	QVector<QPointF> res(n);
	for (int i = 0; i < n; i++) {
		int e = (i + n - 1) % n;	// the edge coming in
		res[i] = upright[i] ? QPointF(at[i], at[e]) : QPointF(at[e], at[i]);
	}
	return res;
}

// the same by dx across the upright edges and dy across the level ones
static QVector<QPointF> offsetPoly(const QVector<QPointF>& p, double dx, double dy) {
	return offsetPoly4(p, dx, dy, dx, dy);
}

// which way edge i of a polygon of right angles faces: 0 left, 1 up, 2 right, 3 down
static int edgeFaces(const QVector<QPointF>& p, int i) {
	int way = (polyArea(p) > 0) ? 1 : -1;
	QPointF d = p[(i + 1) % p.size()] - p[i];
	if (d.x() == 0) return (way * sgn(d.y()) > 0) ? 2 : 0;
	return (-way * sgn(d.x()) > 0) ? 3 : 1;
}

// Each vertex of a polygon rounded with its own radius, cut down to half the
// shorter edge beside it; the arcs are quarter circles as addRoundedRect draws them
static QPainterPath roundPoly(const QVector<QPointF>& p, const QVector<double>& r) {
	const double k = 0.5522847498;		// a quarter circle's control arm, per radius
	QPainterPath path;
	int n = p.size();
	for (int i = 0; i < n; i++) {
		QPointF a = p[(i + n - 1) % n];
		QPointF v = p[i];
		QPointF b = p[(i + 1) % n];
		double li = std::hypot(v.x() - a.x(), v.y() - a.y());
		double lo = std::hypot(b.x() - v.x(), b.y() - v.y());
		double rr = std::max(0.0, std::min(r[i], std::min(li, lo) / 2.0));
		QPointF ui = (li > 0) ? (v - a) / li : QPointF();
		QPointF uo = (lo > 0) ? (b - v) / lo : QPointF();
		QPointF p1 = v - ui * rr;
		QPointF p2 = v + uo * rr;
		if (i == 0) {
			path.moveTo(p1);
		} else {
			path.lineTo(p1);
		}
		if (rr > 0) path.cubicTo(p1 + ui * (k * rr), p2 - uo * (k * rr), p2);
	}
	path.closeSubpath();
	return path;
}

// key shapes of layout_svg.py: corner radius, how the body is filled, how far it
// is inset from the key box, and whether it has a contour
enum {FILL_NONE = 0, FILL_CAP, FILL_WHITE};
struct Preset {
	double corner;
	int fill;
	double inset;
	bool contour;
};

static const Preset* preset(const QString& name) {
	static const Preset key = {3, FILL_CAP, 1, true};
	static const Preset plate = {0, FILL_WHITE, 0, false};
	static const Preset ghost = {0, FILL_NONE, 0, false};
	if (name.isEmpty() || (name == "key")) return &key;
	if (name == "plate") return &plate;
	if (name == "ghost") return &ghost;
	return nullptr;
}

// the rainbow of layout_svg.py: colour and x of each band at the top edge, unless
// the layout has bands of its own
static const struct {
	const char* col;
	double x;
} rainbow[] = {{"#e21430", 569}, {"#e9b71d", 579}, {"#75c150", 589}, {"#379eee", 599}};

// ---------------------------------------------------------------- the font

// One face for the whole program, from the resource: the keyboard never uses a
// system font, so it looks the same everywhere
static FT_Library ftlib = nullptr;
static FT_Face ftface = nullptr;
static QByteArray ftdata;

static FT_Face keysFace() {
	static bool tried = false;
	if (tried) return ftface;
	tried = true;
	QFile file(":/res/vkbd/keys-font.ttf");
	if (!file.open(QIODevice::ReadOnly)) return nullptr;
	ftdata = file.readAll();		// FreeType reads it from here for good
	if (FT_Init_FreeType(&ftlib)) return nullptr;
	if (FT_New_Memory_Face(ftlib, (const FT_Byte*)ftdata.constData(), ftdata.size(), 0, &ftface))
		ftface = nullptr;
	return ftface;
}

// ---------------------------------------------------------------- loading

// A number from 0 to 1 that a seed and a key's place always give the same
static double unitHash(unsigned seed, int row, int col, int what) {
	unsigned x = seed * 0x9e3779b1u ^ (unsigned)row * 0x85ebca77u ^ (unsigned)col * 0xc2b2ae3du ^ (unsigned)what * 0x27d4eb2fu;
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x / 4294967296.0;
}

// A layout without its comments: // to the end of the line, /* to */, outside the
// strings. They become spaces, the line breaks stay, so an error's place is the same.
static QByteArray uncommented(QByteArray t) {
	bool str = false;
	for (int i = 0; i < t.size(); i++) {
		char c = t[i];
		if (str) {
			if (c == '\\') i++;
			else if (c == '"') str = false;
		} else if (c == '"') {
			str = true;
		} else if ((c == '/') && (i + 1 < t.size()) && (t[i + 1] == '/')) {
			while ((i < t.size()) && (t[i] != '\n') && (t[i] != '\r'))
				t[i++] = ' ';
		} else if ((c == '/') && (i + 1 < t.size()) && (t[i + 1] == '*')) {
			int e = t.indexOf("*/", i + 2);
			e = (e < 0) ? t.size() : e + 2;
			for (; i < e; i++) {
				if ((t[i] != '\n') && (t[i] != '\r')) t[i] = ' ';
			}
			i--;
		}
	}
	return t;
}

bool VkSkin::load(const QByteArray& json, QString* err, BaseFile base) {
	QJsonParseError perr;
	QJsonDocument doc = QJsonDocument::fromJson(uncommented(json), &perr);
	if (!doc.isObject()) {
		if (err) *err = perr.errorString();
		return false;
	}
	QJsonObject cfg = doc.object();
	// another layout under this one: its fields, this one's over them
	for (int depth = 0; cfg.contains("base"); depth++) {
		QString name = cfg.value("base").toString();
		QJsonObject under = QJsonDocument::fromJson(uncommented(base ? base(name) : QByteArray()), &perr).object();
		if ((depth > 4) || under.isEmpty()) {
			if (err) *err = QString("no layout to build on: %0").arg(name);
			return false;
		}
		cfg.remove("base");
		foreach(QString k, cfg.keys())
			under[k] = cfg.value(k);
		cfg = under;
	}
	QJsonObject canv = cfg.value("canvas").toObject();
	cw = field(canv, "w", "width").toDouble(530);
	ch = field(canv, "h", "height").toDouble(248);
	bg = QColor(field(canv, "bg", "background").toString("#000000"));
	// under the contours: the ground of the picture unless the layout says
	// otherwise - a light keyboard needs a darker one, or its contours vanish
	grnd = canv.contains("ground") ? QColor(canv.value("ground").toString()) : bg;
	if (!grnd.isValid()) grnd = bg;
	// a pressed key lit in cyan: through the contour round it, or on the key
	// itself by its pad's rim - the 128's gaps are too narrow to carry a light
	litCol = QColor(canv.value("lit").toString());
	if (!litCol.isValid()) litCol = QColor(0, 200, 255);
	padLit = (canv.value("light").toString() == "pad");
	litW = num(canv, "light_w", 1.8);
	anchor = cfg.value("anchor").toString();
	sn = cfg.value("sn").toString("off");
	// the named colors first: a gradient may use them
	colors.clear();
	QJsonObject cols = cfg.value("colors").toObject();
	foreach(QString k, cols.keys())
		colors[k] = QColor(cols.value(k).toString());
	grad = Grad();
	readGrad(QJsonDocument::fromJson("{\"stops\":[[0,\"#4e5868\"],[0.3,\"#2f384a\"],[1,\"#44545e\"]]}").object(), grad);
	readGrad(cfg.value("gradient"), grad);
	QJsonObject cnt = cfg.value("contour").toObject();
	inset = num(cnt, "inset", 1);
	outLR = num(cnt, "out_lr", 0);
	outTB = num(cnt, "out_tb", 1);
	corner = num(cnt, "corner", 3);
	QJsonObject rb = cfg.value("rainbow").toObject();
	rbW = field(rb, "w", "width").toDouble(11);
	rbSlope = num(rb, "slope", -0.45);
	// the bands run from y0 to y1, each one's x given at y0: by default the four
	// of the 48K, over the whole height
	rbY0 = num(rb, "y0", 0);
	rbY1 = num(rb, "y1", ch);
	bands.clear();
	if (rb.contains("bands")) {
		foreach(QJsonValue bv, rb.value("bands").toArray()) {
			QJsonArray ba = bv.toArray();
			bands.append({color(ba.at(0).toString()), ba.at(1).toDouble()});
		}
	} else {
		for (const auto& band : rainbow)
			bands.append({QColor(band.col), band.x});
	}
	bars.clear();
	foreach(QJsonValue bv, cfg.value("bars").toArray()) {
		QJsonObject bo = bv.toObject();
		Bar bar;
		bar.r = QRectF(num(bo, "x", 0), num(bo, "y", 0), num(bo, "w", 0), num(bo, "h", 0));
		bar.corner = num(bo, "corner", 0);
		bar.fill = color(bo.value("fill").toString());
		if (bo.contains("gradient")) readGrad(bo.value("gradient"), bar.grad);
		bars.append(bar);
	}
	dome = Dome();
	if (cfg.value("dome").isObject())
		readDome(cfg.value("dome").toObject(), dome);
	side = Side();
	if (cfg.contains("side")) readSide(cfg.value("side"), side);
	styles.clear();
	QJsonObject sts = cfg.value("styles").toObject();
	foreach(QString k, sts.keys())
		styles[k] = sts.value(k).toObject();
	// some keys turned and moved a little, the same ones every time
	QJsonObject wob = cfg.value("wobble").toObject();
	double wobTilt = num(wob, "tilt", 0);
	double wobShift = num(wob, "shift", 0);
	double wobShare = num(wob, "share", 1);
	unsigned wobSeed = (unsigned)num(wob, "seed", 1);
	icons.clear();
	QJsonObject ics = cfg.value("icons").toObject();
	foreach(QString k, ics.keys()) {
		QJsonObject g = ics.value(k).toObject();
		Icon ic;
		ic.sw = num(g, "sw", 0);
		if (g.contains("line")) {
			QJsonArray ln = g.value("line").toArray();
			ic.isLine = true;
			ic.p1 = point(ln.at(0));
			ic.p2 = point(ln.at(1));
		} else if (g.contains("box") || g.contains("cut")) {
			QJsonArray b = g.value("box").toArray();
			ic.isBox = true;
			ic.box = (b.size() == 4) ? QRectF(b.at(0).toDouble(), b.at(1).toDouble(), b.at(2).toDouble(), b.at(3).toDouble()) : QRectF(0, 0, 8, 8);
			foreach(QJsonValue c, g.value("cut").toArray()) {
				QJsonArray ca = c.toArray();
				if (isRect(ca)) {
					ic.cuts.append(QRectF(ca.at(0).toDouble(), ca.at(1).toDouble(), ca.at(2).toDouble(), ca.at(3).toDouble()));
				} else {
					ic.cutPolys.append(points(c));
				}
			}
		} else if (!g.value("polys").toArray().isEmpty()) {
			foreach(QJsonValue p, g.value("polys").toArray())
				ic.polys.append(points(p));
		} else {
			ic.polys.append(points(g.value("pts")));
		}
		icons[k] = ic;
	}
	rows.clear();
	foreach(QJsonValue rv, cfg.value("rows").toArray()) {
		QJsonObject ro = rv.toObject();
		Row row;
		row.y = num(ro, "y", 0);
		row.h = num(ro, "h", 0);
		foreach(QJsonValue kv, ro.value("keys").toArray()) {
			QJsonObject ko = kv.toObject();
			Key key;
			QString who = QString("key %0 of row %1").arg(row.keys.size()).arg(rows.size());
			auto fail = [&](const QString& why) {
				if (err) *err = who + ": " + why;
				rows.clear();
				return false;
			};
			key.x = num(ko, "x", 0);
			key.w = num(ko, "w", 0);
			key.y = num(ko, "y", row.y);		// a key of its own height, off its row
			key.h = num(ko, "h", row.h);
			key.ax = num(ko, "anchor_x", 0);
			key.ay = num(ko, "anchor_y", 0);
			key.shape = ko.value("shape").toString();
			key.sn = ko.value("sn").toString();
			if (!preset(key.shape))
				return fail(QString("unknown key shape: %0").arg(key.shape));
			// what it presses: a key's name, a few of them at once, or [] for none
			if (ko.contains("zx")) {
				QJsonValue zv = ko.value("zx");
				QJsonArray za = zv.isArray() ? zv.toArray() : QJsonArray({zv});
				key.hasZx = true;
				foreach(QJsonValue z, za) {
					unsigned char c = zxKeyNamed(z.toString());
					if (!c) return fail(QString("no Spectrum key is called \"%0\"").arg(z.toString()));
					key.zx.append((char)c);
				}
			}
			// a polygon of right angles instead of the box; the box is its bounds
			if (ko.contains("outline")) {
				key.outline = cleanPoly(points(ko.value("outline")));
				if (!isRectilinear(key.outline))
					return fail("its outline is not a polygon of right angles");
				QRectF br = QPolygonF(key.outline).boundingRect();
				key.x = br.x();
				key.y = br.y();
				key.w = br.width();
				key.h = br.height();
			}
			// the cap: the layout's, its style's over it, its own over both
			key.grad = grad;
			key.dome = dome;
			key.side = side;
			QString why;
			if (ko.contains("style")) {
				QString st = ko.value("style").toString();
				if (!styles.contains(st))
					return fail(QString("no style is called \"%0\"").arg(st));
				if (!readCap(styles[st], key, why))
					return fail(QString("style %0: %1").arg(st, why));
			}
			if (!readCap(ko, key, why))
				return fail(why);
			// caps of the same key: the key's cap, a style's over it, their own over both
			foreach(QJsonValue pv, ko.value("parts").toArray()) {
				QJsonObject po = pv.toObject();
				QString pw = QString("part %0: ").arg(key.parts.size());
				Key part = key;
				part.outline.clear();
				part.x = num(po, "x", key.x);
				part.w = num(po, "w", key.w);
				part.y = num(po, "y", key.y);
				part.h = num(po, "h", key.h);
				if (po.contains("outline")) {
					part.outline = cleanPoly(points(po.value("outline")));
					if (!isRectilinear(part.outline))
						return fail(pw + "its outline is not a polygon of right angles");
					QRectF br = QPolygonF(part.outline).boundingRect();
					part.x = br.x();
					part.y = br.y();
					part.w = br.width();
					part.h = br.height();
				}
				if (po.contains("style")) {
					QString st = po.value("style").toString();
					if (!styles.contains(st))
						return fail(pw + QString("no style is called \"%0\"").arg(st));
					if (!readCap(styles[st], part, why))
						return fail(pw + QString("style %0: %1").arg(st, why));
				}
				if (!readCap(po, part, why))
					return fail(pw + why);
				part.over = po.value("over").toBool();
				part.unlit = (po.value("light") == QJsonValue(false));
				key.parts.push_back(part);
			}
			// turned and moved: by the wobble, or by its own values
			double dx = 0, dy = 0;
			if ((preset(key.shape)->fill == FILL_CAP) && (unitHash(wobSeed, rows.size(), row.keys.size(), 0) < wobShare)) {
				key.tilt = (unitHash(wobSeed, rows.size(), row.keys.size(), 1) * 2 - 1) * wobTilt;
				dx = (unitHash(wobSeed, rows.size(), row.keys.size(), 2) * 2 - 1) * wobShift;
				dy = (unitHash(wobSeed, rows.size(), row.keys.size(), 3) * 2 - 1) * wobShift;
			}
			key.tilt = num(ko, "tilt", key.tilt);
			if (ko.value("shift").isArray()) {
				QPointF sh = point(ko.value("shift"));
				dx = sh.x();
				dy = sh.y();
			}
			if ((dx != 0) || (dy != 0)) {
				key.x += dx;
				key.y += dy;
				for (QPointF& v : key.outline)
					v += QPointF(dx, dy);
				for (Key& part : key.parts) {
					part.x += dx;
					part.y += dy;
					for (QPointF& v : part.outline)
						v += QPointF(dx, dy);
				}
			}
			foreach(QJsonValue lv, ko.value("labels").toArray()) {
				QJsonObject lo = lv.toObject();
				Label lab;
				lab.icon = field(lo, "i", "icon").toString();
				lab.text = lo.value("t").toVariant().toString();
				QString col = field(lo, "col", "color").toString("white");
				lab.col = colors.contains(col) ? colors[col] : QColor(col);
				if (!lab.col.isValid()) lab.col = Qt::white;
				lab.sz = field(lo, "sz", "size").toDouble(9) + num(lo, "sy", 0);
				lab.dx = num(lo, "dx", 0);
				lab.dy = num(lo, "dy", 0);
				lab.hasTl = lo.contains("tl");
				lab.tl = num(lo, "tl", 0) + num(lo, "sx", 0);
				lab.th = num(lo, "th", 0);
				// eb: thicker by that many canvas pixels, both ways or [x, y]
				QJsonValue eb = lo.value("eb");
				lab.ebx = eb.isArray() ? eb.toArray().at(0).toDouble() : eb.toDouble(0);
				lab.eby = eb.isArray() ? eb.toArray().at(1).toDouble() : eb.toDouble(0);
				lab.anchor = lo.value("a").toString();
				lab.sn = lo.value("sn").toString();
				if (!lab.icon.isEmpty() && !icons.contains(lab.icon)) {
					if (err) *err = QString("unknown icon id: %0").arg(lab.icon);
					rows.clear();
					return false;
				}
				key.labels.append(lab);
			}
			row.keys.append(key);
		}
		rows.append(row);
	}
	if (rows.isEmpty() || (cw <= 0) || (ch <= 0)) {
		if (err) *err = "no keys";
		rows.clear();
		return false;
	}
	if (!makeKeys(err)) {
		rows.clear();
		keytab.clear();
		return false;
	}
	return true;
}

QColor VkSkin::color(const QString& name) const {
	return colors.contains(name) ? colors[name] : QColor(name);
}

// a gradient: its stops alone, or {"stops", "h", "from"} over what it had
void VkSkin::readGrad(const QJsonValue& v, Grad& g) const {
	QJsonObject o = v.toObject();
	QJsonArray stp = v.isArray() ? v.toArray() : o.value("stops").toArray();
	if (!stp.isEmpty()) {
		g.stops.clear();
		foreach(QJsonValue sv, stp)
			g.stops.append(qMakePair(sv.toArray().at(0).toDouble(), color(sv.toArray().at(1).toString())));
	}
	g.h = num(o, "h", g.h);
	g.angle = num(o, "angle", g.angle);
	if (o.contains("from"))
		g.fromBottom = (o.value("from").toString() == "bottom");
}

// one shape of a dome, over what it had
void VkSkin::readDomePart(const QJsonObject& o, DomePart& d) {
	auto four = [](const QJsonValue& v, double* to) {
		QJsonArray a = v.toArray();
		for (int i = 0; i < 4; i++)
			to[i] = v.isDouble() ? v.toDouble() : a.at(i).toDouble();
	};
	if (o.contains("inset")) four(o.value("inset"), d.inset);
	// an outline of its own: its corners one radius, or one each
	if (o.contains("outline")) {
		d.outline = points(o.value("outline"));
		d.radii.clear();
	}
	if (o.contains("corner") && !d.outline.isEmpty()) {
		QJsonValue cv = o.value("corner");
		QJsonArray ca = cv.toArray();
		d.radii.clear();
		for (int i = 0; i < d.outline.size(); i++)
			d.radii.append(cv.isDouble() ? cv.toDouble() : ca.at(i).toDouble());
	} else if (o.contains("corner")) {
		four(o.value("corner"), d.corner);
	}
	while (d.radii.size() < d.outline.size())
		d.radii.append(0);
	d.cut = num(o, "cut", d.cut);
}

// a dome, over what it had: the layout's for a key; "more" lists shapes joined to it
void VkSkin::readDome(const QJsonObject& o, Dome& d) const {
	d.on = true;
	readDomePart(o, d.shape);
	if (o.contains("more")) {
		d.more.clear();
		foreach(QJsonValue v, o.value("more").toArray()) {
			DomePart p;
			readDomePart(v.toObject(), p);
			d.more.append(p);
		}
	}
	if (o.contains("gradient")) readGrad(o.value("gradient"), d.grad);
	if (o.contains("fill")) d.fill = color(o.value("fill").toString());
	// the edge: a color, or a gradient (lit on one side, dark on the other)
	if (o.contains("edge")) {
		QJsonValue ev = o.value("edge");
		if (ev.isString()) {
			d.edge = color(ev.toString());
			d.edgeGrad = Grad();
		} else {
			d.edge = QColor();
			readGrad(ev, d.edgeGrad);
		}
	}
	d.ew = num(o, "ew", d.ew);
}

// the sides, over what they had: "w", "shade" and "fill" one value for all four or
// [left, top, right, bottom]; false for none
void VkSkin::readSide(const QJsonValue& v, Side& sd) const {
	if (!v.isObject()) {
		sd.on = false;
		return;
	}
	QJsonObject o = v.toObject();
	auto four = [](const QJsonValue& f, int i) {
		return f.isArray() ? f.toArray().at(i) : f;
	};
	for (int i = 0; i < 4; i++) {
		if (o.contains("w")) sd.w[i] = four(o.value("w"), i).toDouble();
		if (o.contains("shade")) sd.shade[i] = std::max(-1.0, std::min(1.0, four(o.value("shade"), i).toDouble()));
		if (o.contains("fill")) sd.fill[i] = color(four(o.value("fill"), i).toString());
	}
	sd.on = true;
}

// What a key or a style says about its cap, over what it has: its corners (one
// radius, or [tl, tr, br, bl]), its gradient, its dome or none ("dome": false), its sides
bool VkSkin::readCap(const QJsonObject& o, Key& key, QString& why) const {
	if (o.contains("corner")) {
		QJsonValue cv = o.value("corner");
		QJsonArray ca = cv.toArray();
		if (cv.isDouble()) {
			for (int i = 0; i < 4; i++)
				key.corner[i] = cv.toDouble();
		} else if (isRect(ca)) {
			for (int i = 0; i < 4; i++)
				key.corner[i] = ca.at(i).toDouble();
		} else {
			why = "its corner is a number or four of them";
			return false;
		}
		key.hasCorner = true;
	}
	if (o.contains("gradient")) readGrad(o.value("gradient"), key.grad);
	if (o.contains("dome")) {
		if (o.value("dome").isObject()) {
			readDome(o.value("dome").toObject(), key.dome);
		} else {
			key.dome.on = false;
		}
	}
	if (o.contains("side")) readSide(o.value("side"), key.side);
	return true;
}

// a turned key: about its middle, given in the same units as what is turned
QTransform VkSkin::tiltAt(const Key& key, const QPointF& mid) {
	QTransform tr;
	if (key.tilt == 0) return tr;
	tr.translate(mid.x(), mid.y());
	tr.rotate(key.tilt);
	tr.translate(-mid.x(), -mid.y());
	return tr;
}

// the same in the picture's pixels, about the middle of its box at that size
QTransform VkSkin::tiltPx(const Key& key, double s) const {
	return tiltAt(key, QRectF(keyBox(key, s)).center());
}

// Which Spectrum key each one is comes from its "zx", or failing that from its
// place: the rows are the keyboard's, left to right. A key placed so whose first
// label says otherwise is a sign the layout was rearranged, and it is said so.
static const char* zxRows[] = {"1234567890", "qwertyuiop", "asdfghjklE", "CzxcvbnmS "};

unsigned char VkSkin::zxKeyAt(int row, int pos) {
	return ((row >= 0) && (row < 4) && (pos >= 0) && (pos < 10)) ? zxRows[row][pos] : 0;
}

// How far a label reaches up and down, in canvas units, when its anchor point is
// at height y: the ink of its glyphs (or its icon's shape), unhinted
bool VkSkin::labelSpan(const Label& lab, double y, double& top, double& bot) const {
	if (!lab.icon.isEmpty()) {
		const Icon& ic = icons[lab.icon];
		QRectF br;
		if (ic.isLine) {
			double hw = (ic.sw > 0 ? ic.sw : 1) / 2.0;
			br = QRectF(ic.p1, ic.p2).normalized().adjusted(-hw, -hw, hw, hw);
		} else if (ic.isBox) {
			br = ic.box;
		} else {
			foreach(const QVector<QPointF>& c, ic.polys)
				br |= QPolygonF(c).boundingRect().adjusted(-ic.sw / 2, -ic.sw / 2, ic.sw / 2, ic.sw / 2);
		}
		if (br.isNull()) return false;
		top = y + br.top();
		bot = y + br.bottom();
		return true;
	}
	FT_Face face = keysFace();
	if (!face || lab.text.isEmpty()) return false;
	FT_Pos ymin = 0, ymax = 0;
	bool any = false;
	foreach(uint cp, lab.text.toUcs4()) {
		if (FT_Load_Glyph(face, FT_Get_Char_Index(face, cp), FT_LOAD_NO_SCALE)) continue;
		if (face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) continue;
		if (!face->glyph->outline.n_points) continue;		// a space
		FT_BBox cb;
		FT_Outline_Get_CBox(&face->glyph->outline, &cb);
		ymin = any ? std::min(ymin, cb.yMin) : cb.yMin;
		ymax = any ? std::max(ymax, cb.yMax) : cb.yMax;
		any = true;
	}
	if (!any) return false;
	double k = ((lab.th > 0) ? lab.th : lab.sz) / face->units_per_EM;	// "th" stretches it to that
	top = y - ymax * k;
	bot = y - ymin * k;
	return true;
}

bool VkSkin::makeKeys(QString* err) {
	static const char* zxNames[] = {"ENTER", "CAPS", "SYMBOL", "BREAK"};
	static const char zxNamed[] = "ECS ";
	keytab.clear();
	// a key with parts: a click and the light take them all, turned with it;
	// hp is what a click takes of the key itself, vk.fill not turned yet
	auto withParts = [&](VkKey& vk, const Key& key, QPainterPath hp, const QTransform& tr) {
		for (const Key& part : key.parts) {
			QPainterPath ph, pf;
			if (part.outline.isEmpty()) {
				QRectF pb(part.x, part.y, part.w, part.h);
				ph.addRect(pb);
				pf.addRect(pb.adjusted(-outLR - 0.5, -outTB - 0.5, outLR + 0.5, outTB + 0.5));
			} else {
				ph.addPolygon(QPolygonF(part.outline));
				ph.closeSubpath();
				pf.addPolygon(QPolygonF(offsetPoly(part.outline, outLR + 0.5, outTB + 0.5)));
				pf.closeSubpath();
			}
			hp = hp.united(ph);
			if (!part.unlit) vk.fill = vk.fill.united(pf);
		}
		vk.hitPoly = tr.map(hp.simplified()).toFillPolygon();
		vk.hit = vk.hitPoly.boundingRect();
	};
	for (int r = 0; r < rows.size(); r++) {
		const Row& row = rows[r];
		double top = (r == 0) ? 0 : (rows[r - 1].y + rows[r - 1].h + row.y) / 2.0;
		double bot = (r == rows.size() - 1) ? ch : (row.y + row.h + rows[r + 1].y) / 2.0;
		for (int i = 0; i < row.keys.size(); i++) {
			const Key& key = row.keys[i];
			VkKey vk;
			vk.row = r;
			vk.col = i;
			if (key.hasZx) {
				vk.zx = key.zx;
			} else if (zxKeyAt(r, i)) {
				vk.zx = QByteArray(1, (char)zxKeyAt(r, i));
			} else {
				if (err) *err = QString("key %0 of row %1 is off the 48K's rows and has no \"zx\"").arg(i).arg(r);
				return false;
			}
			vk.ch = (vk.zx.size() == 1) ? (unsigned char)vk.zx[0] : 0;
			vk.box = QRectF(key.x, key.y, key.w, key.h);
			QTransform tr = tiltAt(key, vk.box.center());
			if (!key.outline.isEmpty()) {
				// the outline itself, as a box key takes its box: where keys touch,
				// the contour around it is half the neighbour's
				vk.hitPoly = tr.map(QPolygonF(key.outline));
				vk.hit = vk.hitPoly.boundingRect();
				vk.fill.addPolygon(QPolygonF(offsetPoly(key.outline, outLR + 0.5, outTB + 0.5)));
				vk.fill.closeSubpath();
				if (!key.parts.empty()) {
					QPainterPath hp;
					hp.addPolygon(QPolygonF(key.outline));
					hp.closeSubpath();
					withParts(vk, key, hp, tr);
				}
				vk.fill = tr.map(vk.fill);
				keytab.append(vk);
				continue;
			}
			vk.fill.addRect(vk.box.adjusted(-outLR - 0.5, -outTB - 0.5, outLR + 0.5, outTB + 0.5));
			// As wide as the key, and as tall as the key with its words over and
			// under it, but never past halfway to the next row: the gaps between
			// the keys and the rows are left as misses, which is where the window's
			// menu opens (the right button on a key holds the key down)
			double ht = key.y;
			double hb = key.y + key.h;
			double cy = key.y + key.h / 2.0 + key.ay;
			foreach(const Label& lab, key.labels) {
				double lt, lb;
				if (!labelSpan(lab, cy + lab.dy, lt, lb)) continue;
				ht = std::min(ht, lt);
				hb = std::max(hb, lb);
			}
			ht = std::max(ht, std::min(top, key.y));
			hb = std::min(hb, std::max(bot, key.y + key.h));
			vk.hit = QRectF(key.x, ht, key.w, hb - ht);
			if (!key.parts.empty()) {
				QPainterPath hp;
				hp.addRect(vk.hit);
				withParts(vk, key, hp, tr);
				vk.fill = tr.map(vk.fill);
			} else if (key.tilt != 0) {
				// turned: its words with it
				vk.hitPoly = tr.map(QPolygonF(vk.hit));
				vk.hit = vk.hitPoly.boundingRect();
				vk.fill = tr.map(vk.fill);
			}
			foreach(const Label& lab, key.labels) {
				if (key.hasZx) break;		// the layout says what it is
				if (lab.text.isEmpty()) continue;
				const char* nm = strchr(zxNamed, vk.ch);
				QString want = nm ? zxNames[nm - zxNamed] : QString(QChar(vk.ch)).toUpper();
				if (!lab.text.startsWith(want))
					qWarning("vkbd: key %i of row %i is '%c' by its place, its label says '%s'",
						 i, r, vk.ch, lab.text.toUtf8().constData());
				break;
			}
			keytab.append(vk);
		}
	}
	return true;
}

// ---------------------------------------------------------------- drawing

// a label's coverage, one byte per channel (R, G, B) - all three the same for gray
struct Mask {
	int x = 0, y = 0, w = 0, h = 0;
	std::vector<uchar> c;
};

// put a label down in its colour. A subpixel triad needs an opaque pixel under
// it, so over the contour (or anything not opaque) the three are averaged
static void composite(QImage& img, const Mask& m, const QColor& col) {
	int cr = col.red();
	int cg = col.green();
	int cb = col.blue();
	for (int y = 0; y < m.h; y++) {
		int dy = m.y + y;
		if ((dy < 0) || (dy >= img.height())) continue;
		QRgb* line = (QRgb*)img.scanLine(dy);
		const uchar* src = m.c.data() + y * m.w * 3;
		for (int x = 0; x < m.w; x++, src += 3) {
			int dx = m.x + x;
			if ((dx < 0) || (dx >= img.width())) continue;
			int kr = src[0];
			int kg = src[1];
			int kb = src[2];
			if (!(kr | kg | kb)) continue;
			QRgb d = line[dx];
			int a = qAlpha(d);
			int km = (kr + kg + kb + 1) / 3;
			if (a < 255) kr = kg = kb = km;
			line[dx] = qRgba((cr * kr + qRed(d) * (255 - kr) + 127) / 255,
					 (cg * kg + qGreen(d) * (255 - kg) + 127) / 255,
					 (cb * kb + qBlue(d) * (255 - kb) + 127) / 255,
					 (255 * km + a * (255 - km) + 127) / 255);
		}
	}
}

// an icon is drawn as a shape into a mask of its own, then laid down like text
static Mask iconMask(const QPainterPath& path, double sw, bool stroke, bool flatCap) {
	Mask m;
	QPen pen(Qt::black, sw, Qt::SolidLine, flatCap ? Qt::FlatCap : Qt::SquareCap, Qt::MiterJoin);
	pen.setMiterLimit(4);		// SVG's default
	QPainterPathStroker sk(pen);
	QRectF br = stroke ? sk.createStroke(path).boundingRect() : path.boundingRect();
	QRect box = br.toAlignedRect().adjusted(-1, -1, 1, 1);
	if (box.isEmpty()) return m;
	QImage msk(box.size(), QImage::Format_Alpha8);
	msk.fill(0);
	QPainter p(&msk);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.translate(-box.topLeft());
	if (stroke) {
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
	} else {
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::black);
	}
	p.drawPath(path);
	p.end();
	m.x = box.x();
	m.y = box.y();
	m.w = box.width();
	m.h = box.height();
	m.c.resize(m.w * m.h * 3);
	for (int y = 0; y < m.h; y++) {
		const uchar* s = msk.constScanLine(y);
		uchar* d = m.c.data() + y * m.w * 3;
		for (int x = 0; x < m.w; x++, d += 3)
			d[0] = d[1] = d[2] = s[x];
	}
	return m;
}

// a width in the picture's pixels: whole, and at least one if it is not none
static int pxWidth(double v, double s) {
	return (v == 0) ? 0 : std::max(1, fr(v * s));
}

// a key box in the picture's pixels, its edges on whole ones
static QRect keyBoxAt(double y, double h, double x, double w, double s) {
	int x0 = fr(x * s);
	int y0 = fr(y * s);
	return QRect(x0, y0, fr((x + w) * s) - x0, fr((y + h) * s) - y0);
}

// A gradient from top to bottom of a box, in the picture's pixels; one of a
// length of its own runs that far down from the top or up from the bottom, and
// its end colors go on past it - a key two rows tall keeps a one-row shading.
// One at an angle runs across the box that way, through its middle.
QLinearGradient VkSkin::gradAt(const Grad& g, const QRectF& box, double s) {
	QLinearGradient gr;
	if (g.angle != 0) {
		double a = g.angle * 3.14159265358979323846 / 180.0;
		QPointF v(std::sin(a), std::cos(a));
		double len = std::fabs(box.width() * v.x()) + std::fabs(box.height() * v.y());
		gr = QLinearGradient(box.center() - v * (len / 2), box.center() + v * (len / 2));
	} else {
		double top = box.top();
		double bot = box.bottom();
		if (g.h > 0) {
			if (g.fromBottom) {
				top = bot - g.h * s;
			} else {
				bot = top + g.h * s;
			}
		}
		gr = QLinearGradient(0, top, 0, bot);
	}
	for (const auto& st : g.stops)
		gr.setColorAt(st.first, st.second);
	return gr;
}

// an outline's vertices on whole pixels of that size, as a box's edges are
static QVector<QPointF> snapPoly(const QVector<QPointF>& p, double s) {
	QVector<QPointF> res;
	foreach(const QPointF& v, p)
		res.append(QPointF(fr(v.x() * s), fr(v.y() * s)));
	return cleanPoly(res);
}

// a key in the picture's pixels: its box, or its outline's bounds
QRect VkSkin::keyBox(const Key& key, double s) const {
	if (key.outline.isEmpty())
		return keyBoxAt(key.y, key.h, key.x, key.w, s);
	return QPolygonF(snapPoly(key.outline, s)).boundingRect().toRect();
}

// A key's shape in the picture's pixels - its outline, or its box when its corners
// are its own - pushed out by dx, dy whole pixels (in, if negative). A corner that
// juts out is rounded with its radius at that scale plus grow pixels, one that
// cuts in with that radius less grow: the edge keeps its distance all round.
// The radius is the key's own, or base; a corner cutting in takes the smallest.
QPainterPath VkSkin::keyPath(const Key& key, double s, int dx, int dy, double base, double grow) const {
	QVector<QPointF> pts;
	if (key.outline.isEmpty()) {
		QRect kb = keyBox(key, s);
		double x1 = kb.x() + kb.width();
		double y1 = kb.y() + kb.height();
		pts << QPointF(kb.x(), kb.y()) << QPointF(x1, kb.y()) << QPointF(x1, y1) << QPointF(kb.x(), y1);
	} else {
		pts = snapPoly(key.outline, s);
	}
	if (!isRectilinear(pts)) {		// squashed to nothing at this size
		QPainterPath none;
		return none;
	}
	int n = pts.size();
	int way = sgn(polyArea(pts));
	double least = key.hasCorner ? std::min(std::min(key.corner[0], key.corner[1]), std::min(key.corner[2], key.corner[3])) : base;
	QVector<double> rad(n);
	for (int i = 0; i < n; i++) {
		QPointF ui = pts[i] - pts[(i + n - 1) % n];
		QPointF uo = pts[(i + 1) % n] - pts[i];
		bool juts = (sgn(ui.x() * uo.y() - ui.y() * uo.x()) == way);
		double c = least;
		if (juts && key.hasCorner) {
			// which corner it is: where the key lies from it
			double bx = sgn(uo.x()) - sgn(ui.x());
			double by = sgn(uo.y()) - sgn(ui.y());
			c = key.corner[(by > 0) ? ((bx > 0) ? 0 : 1) : ((bx < 0) ? 2 : 3)];
		}
		rad[i] = juts ? (c * s + grow) : std::max(0.0, c * s - grow);
	}
	return roundPoly(offsetPoly(pts, dx, dy), rad);
}

// one cap's contour in the picture's pixels, not turned
QPainterPath VkSkin::contourOf(const Key& key, double s) const {
	int olr = pxWidth(outLR, s);
	int otb = pxWidth(outTB, s);
	if (!key.outline.isEmpty() || key.hasCorner)
		return keyPath(key, s, olr, otb, corner, std::max(outTB, outLR) * s);
	QRect kb = keyBox(key, s);
	return rrect(kb.x() - olr, kb.y() - otb, kb.width() + 2 * olr, kb.height() + 2 * otb, (corner + std::max(outTB, outLR)) * s);
}

// a key's contour as render() cuts it out, in the picture's pixels: its own and
// its parts', each apart - QPainterPath's union drops slivers where edges meet
QVector<QPainterPath> VkSkin::contoursAt(const Key& key, double s) const {
	QVector<QPainterPath> res;
	res.append(contourOf(key, s));
	for (const Key& part : key.parts)
		res.append(contourOf(part, s));
	if (key.tilt != 0) {
		QTransform tr = tiltPx(key, s);
		for (QPainterPath& path : res)
			path = tr.map(path);
	}
	return res;
}

// One shape of a dome in the picture's pixels: its box or outline, corners
// rounded, everything above its cut taken away
QPainterPath VkSkin::domePart(const Key& key, const DomePart& d, double s) const {
	QVector<QPointF> pts;
	QVector<double> rad;
	if (d.outline.isEmpty()) {
		double x0 = fr((key.x + d.inset[0]) * s);
		double y0 = fr((key.y + d.inset[1]) * s);
		double x1 = fr((key.x + key.w - d.inset[2]) * s);
		double y1 = fr((key.y + key.h - d.inset[3]) * s);
		pts << QPointF(x0, y0) << QPointF(x1, y0) << QPointF(x1, y1) << QPointF(x0, y1);
		for (int i = 0; i < 4; i++)
			rad.append(d.corner[i] * s);
	} else {
		for (int i = 0; i < d.outline.size(); i++) {
			pts.append(QPointF(fr((key.x + d.outline[i].x()) * s), fr((key.y + d.outline[i].y()) * s)));
			rad.append(d.radii[i] * s);
		}
	}
	QRectF br = QPolygonF(pts).boundingRect();
	if ((br.width() <= 0) || (br.height() <= 0)) return QPainterPath();
	QPainterPath path = roundPoly(pts, rad);
	if (d.cut >= 0) {
		QPainterPath below;
		double cy = fr((key.y + d.cut) * s);
		below.addRect(QRectF(br.left() - 1, cy, br.width() + 2, br.bottom() - cy + 1));
		path = path.intersected(below);
	}
	return path;
}

// The contour's outer edge as render() cuts it, and a pixel more all round: a
// contour light's excess is under the opaque ground, so no rounding can leave a
// dark seam; a pad light's picture differs from the plain one only on the caps,
// so the clip's edge never shows. A box key takes a rectangle; an outline key
// takes its own shape, as a rectangle round it would reach the keys in its
// corners. Where keys stand so close that the excess reaches into a neighbour's
// contour, that is cut away.
QPainterPath VkSkin::fillAt(int n, double s) const {
	QPainterPath path;
	if ((n < 0) || (n >= keytab.size())) return path;
	const Key& key = rows[keytab[n].row].keys[keytab[n].col];
	int olr = pxWidth(outLR, s) + 1;
	int otb = pxWidth(outTB, s) + 1;
	auto own = [&](const Key& k) {
		if (!k.outline.isEmpty())
			return keyPath(k, s, olr, otb, corner, std::max(outTB, outLR) * s + 1);
		QPainterPath r;
		r.addRect(QRectF(keyBox(k, s).adjusted(-olr, -otb, olr, otb)));
		return r;
	};
	// The joining and cutting are done on whole pixels: QPainterPath's own boolean
	// ops lose the lot now and then where curved edges nearly meet, and the light
	// went out at some window sizes. The window fills it without antialiasing anyway.
	path = own(key);
	if (key.tilt != 0) path = tiltPx(key, s).map(path);
	QRegion reg;
	bool cut = false;
	for (const Key& part : key.parts) {
		if (part.unlit) continue;
		QPainterPath pp = own(part);
		if (key.tilt != 0) pp = tiltPx(key, s).map(pp);
		if (!cut) reg = QRegion(path.toFillPolygon().toPolygon());
		cut = true;
		reg += QRegion(pp.toFillPolygon().toPolygon());
	}
	if (cut) {
		path = QPainterPath();
		path.addRegion(reg);
	}
	for (int i = 0; i < keytab.size(); i++) {
		const Key& other = rows[keytab[i].row].keys[keytab[i].col];
		if ((i == n) || !preset(other.shape)->contour) continue;
		foreach(const QPainterPath& them, contoursAt(other, s)) {
			if (!path.intersects(them)) continue;
			if (!cut) reg = QRegion(path.toFillPolygon().toPolygon());
			cut = true;
			reg -= QRegion(them.toFillPolygon().toPolygon());
		}
	}
	if (!cut) return path;
	QPainterPath out;
	out.addRegion(reg);
	return out;
}

QImage VkSkin::render(double s, bool lcd, bool lit) const {
	int W = std::max(1, fr(cw * s));
	int H = std::max(1, fr(ch * s));
	QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	if (isNull()) return img;
	auto px = [s](double v) {return pxWidth(v, s);};
	// a key drawn by keyPath(): an outline, or corners of its own
	auto shaped = [](const Key& key) {return !key.outline.isEmpty() || key.hasCorner;};

	QPainter p(&img);
	p.fillRect(img.rect(), bg);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(Qt::NoPen);
	foreach(const Band& band, bands) {
		QPolygonF poly;
		double run = rbSlope * (rbY1 - rbY0);
		poly << QPointF(band.x * s, rbY0 * s) << QPointF((band.x + rbW) * s, rbY0 * s)
		     << QPointF((band.x + run + rbW) * s, rbY1 * s) << QPointF((band.x + run) * s, rbY1 * s);
		p.setBrush(band.col);
		p.drawPolygon(poly);
	}
	foreach(const Bar& bar, bars) {
		QRectF r(bar.r.x() * s, bar.r.y() * s, bar.r.width() * s, bar.r.height() * s);
		if (bar.grad.stops.isEmpty()) {
			p.setBrush(bar.fill.isValid() ? bar.fill : grnd);
		} else {
			p.setBrush(gradAt(bar.grad, r, s));
		}
		p.drawPath(rrect(r.x(), r.y(), r.width(), r.height(), bar.corner * s));
	}
	// the contours, cut out of the ground and the rainbow alike
	p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
	p.setBrush(Qt::black);
	foreach(const Row& row, rows) {
		foreach(const Key& key, row.keys) {
			if (preset(key.shape)->contour) {
				foreach(const QPainterPath& path, contoursAt(key, s))
					p.drawPath(path);
			}
		}
	}
	// the caps over them: the edge pixel is the cap's colour with what is left of
	// its alpha, so a pressed key shows through exactly where the contour is
	p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	// one cap: its body, its sides, its dome
	auto cap = [&](const Key& key) {
		// lit, unless a part that stays as it is
		bool klit = lit && !key.unlit;
		const Preset* pr = preset(key.shape);
		QRect kb = keyBox(key, s);
		int ins = px(pr->inset);
		QRectF body(kb.x() + ins, kb.y() + ins, kb.width() - 2 * ins, kb.height() - 2 * ins);
		QPainterPath path;
		if (shaped(key)) {
			path = keyPath(key, s, -ins, -ins, pr->corner, -pr->inset * s);
			body = path.boundingRect();
		} else {
			path = rrect(body.x(), body.y(), body.width(), body.height(), std::max(0.0, pr->corner - pr->inset) * s);
		}
		if (pr->fill == FILL_CAP) {
			p.setBrush(gradAt(key.grad, body, s));
		} else {
			p.setBrush(Qt::white);
		}
		p.drawPath(path);
		// the sides: a band inside the body's edge, a trapezoid along each edge
		const Side& sd = key.side;
		if (sd.on && (pr->fill == FILL_CAP)) {
			QVector<QPointF> out;
			if (key.outline.isEmpty()) {
				out << body.topLeft() << body.topRight() << body.bottomRight() << body.bottomLeft();
			} else {
				out = offsetPoly(snapPoly(key.outline, s), -ins, -ins);
			}
			if (isRectilinear(out)) {
				QVector<QPointF> in = offsetPoly4(out, -fr(sd.w[0] * s), -fr(sd.w[1] * s), -fr(sd.w[2] * s), -fr(sd.w[3] * s));
				p.save();
				p.setClipPath(path);
				for (int i = 0; i < out.size(); i++) {
					int j = (i + 1) % out.size();
					int f = edgeFaces(out, i);
					QColor c = sd.fill[f];
					if (!c.isValid() && (sd.shade[f] != 0))
						c = (sd.shade[f] > 0) ? QColor(0, 0, 0, fr(sd.shade[f] * 255)) : QColor(255, 255, 255, fr(-sd.shade[f] * 255));
					if (!c.isValid() || !c.alpha()) continue;
					QPolygonF q;
					q << out[i] << out[j] << in[j] << in[i];
					p.setBrush(c);
					p.drawPolygon(q);
				}
				p.restore();
			}
		}
		// the dome: its shapes, each cut flat at its top, joined, never off the cap
		const Dome& dm = key.dome;
		if (!dm.on || (pr->fill != FILL_CAP) || (!dm.fill.isValid() && dm.grad.stops.isEmpty())) {
			// lit, a cap with no pad lights inside its own edge
			if (klit && pr->contour) {
				p.save();
				p.setClipPath(path);
				p.setBrush(Qt::NoBrush);
				p.setPen(QPen(litCol, 2 * litW * s));
				p.drawPath(path);
				p.restore();
			}
			return;
		}
		QPainterPath joined = domePart(key, dm.shape, s);
		foreach(const DomePart& part, dm.more)
			joined = joined.united(domePart(key, part, s));
		QPainterPath dp = joined.intersected(path);
		if (dp.isEmpty()) return;
		if (dm.fill.isValid()) {
			p.setBrush(dm.fill);
		} else {
			p.setBrush(gradAt(dm.grad, joined.boundingRect(), s));
		}
		p.drawPath(dp);
		// its edge: inside its rim and its cuts, where these are on the cap;
		// lit, the light in its place
		QColor ec = klit ? litCol : dm.edge;
		double ew = klit ? litW : dm.ew;
		bool shaded = !klit &&!ec.isValid() && !dm.edgeGrad.stops.isEmpty();
		if ((ec.isValid() || shaded) && (ew > 0)) {
			p.save();
			p.setClipPath(dp);
			p.setBrush(Qt::NoBrush);
			if (shaded) {
				p.setPen(QPen(QBrush(gradAt(dm.edgeGrad, joined.boundingRect(), s)), 2 * ew * s));
			} else {
				p.setPen(QPen(ec, 2 * ew * s));
			}
			p.drawPath(joined);
			p.restore();
		}
	};
	// a key's cap and its parts' under it and over it
	auto caps = [&](const Key& key) {
		for (const Key& part : key.parts)
			if (!part.over) cap(part);
		cap(key);
		for (const Key& part : key.parts)
			if (part.over) cap(part);
	};
	foreach(const Row& row, rows) {
		foreach(const Key& key, row.keys) {
			if (preset(key.shape)->fill == FILL_NONE) continue;
			if (key.tilt == 0) {
				caps(key);
				continue;
			}
			// a turned one: drawn as it is, the picture turned under it
			p.save();
			p.setTransform(tiltPx(key, s));
			caps(key);
			p.restore();
		}
	}
	p.end();

	FT_Face face = keysFace();
	foreach(const Row& row, rows) {
		foreach(const Key& key, row.keys) {
			// a turned key's words and icons drawn turned, each glyph by FreeType's own
			// transform - a turned bitmap of them came out soft
			bool klcd = lcd;
			bool turned = (key.tilt != 0);
			QTransform kt = turned ? tiltPx(key, s) : QTransform();
			auto put = [&](const Mask& m, const QColor& col) {composite(img, m, col);};
			auto place = [&](const QPainterPath& pp) {return turned ? kt.map(pp) : pp;};
			// the same turn for FreeType, its y up
			FT_Matrix ftm = {(FT_Fixed)std::lround(kt.m11() * 65536), (FT_Fixed)std::lround(-kt.m21() * 65536),
			                 (FT_Fixed)std::lround(-kt.m12() * 65536), (FT_Fixed)std::lround(kt.m22() * 65536)};
			QRect kb = keyBox(key, s);
			QString kmode = key.sn.isEmpty() ? sn : key.sn;
			double rx = snapTo((kb.left() + kb.left() + kb.width()) / 2.0 + key.ax * s, kmode);
			double ry = snapTo((kb.top() + kb.top() + kb.height()) / 2.0 + key.ay * s, kmode);
			foreach(const Label& lab, key.labels) {
				QString mode = lab.sn.isEmpty() ? kmode : lab.sn;
				double x = snapTo(rx + lab.dx * s, mode);
				double y = snapTo(ry + lab.dy * s, mode);
				if (!lab.icon.isEmpty()) {
					const Icon& ic = icons[lab.icon];
					QTransform tr;
					tr.translate(x, y);
					tr.scale(s, s);
					QPainterPath path;
					path.setFillRule(Qt::OddEvenFill);
					if (ic.isLine) {
						path.moveTo(ic.p1);
						path.lineTo(ic.p2);
						put(iconMask(place(tr.map(path)), (ic.sw > 0 ? ic.sw : 1) * s, true, true), lab.col);
					} else if (ic.isBox) {
						path.addRect(ic.box);
						foreach(QRectF c, ic.cuts)
							path.addRect(c);
						foreach(const QVector<QPointF>& c, ic.cutPolys)
							path.addPolygon(QPolygonF(c)), path.closeSubpath();
						put(iconMask(place(tr.map(path)), 0, false, false), lab.col);
					} else {
						foreach(const QVector<QPointF>& c, ic.polys) {
							QPainterPath pp;
							pp.setFillRule(Qt::OddEvenFill);
							pp.addPolygon(QPolygonF(c));
							pp.closeSubpath();
							put(iconMask(place(tr.map(pp)), ic.sw * s, ic.sw > 0, false), lab.col);
						}
					}
					continue;
				}
				if (!face || lab.text.isEmpty()) continue;
				// the run's glyphs and their places in font units, kerned
				QVector<uint> cps = lab.text.toUcs4();
				QVector<FT_UInt> gid;
				QVector<long> pos;
				long pen = 0;
				for (int i = 0; i < cps.size(); i++)
					gid.append(FT_Get_Char_Index(face, cps[i]));
				for (int i = 0; i < gid.size(); i++) {
					pos.append(pen);
					FT_Fixed adv = 0;
					FT_Get_Advance(face, gid[i], FT_LOAD_NO_SCALE, &adv);
					pen += adv;
					if (i + 1 < gid.size()) {
						FT_Vector kv;
						if (!FT_Get_Kerning(face, gid[i], gid[i + 1], FT_KERNING_UNSCALED, &kv))
							pen += kv.x;
					}
				}
				double upem = face->units_per_EM;
				double natural = pen * lab.sz / upem;
				double kx = (lab.hasTl && (lab.tl >= 4) && (natural > 0)) ? lab.tl / natural : 1.0;
				double ky = (lab.th > 0) ? lab.th / lab.sz : 1.0;
				QString anc = !lab.anchor.isEmpty() ? lab.anchor : (!anchor.isEmpty() ? anchor : QString("middle"));
				if (anc == "middle") x -= natural * kx * s / 2.0;
				else if (anc == "end") x -= natural * kx * s;
				int base = fr(y);		// the baseline on a whole pixel, always
				double ppx = lab.sz * s * kx;	// pixels per em, each way
				double ppy = lab.sz * s * ky;
				FT_Set_Char_Size(face, (FT_F26Dot6)std::lround(ppx * 64), (FT_F26Dot6)std::lround(ppy * 64), 72, 72);
				// every glyph rendered at its own subpixel x, then laid into one mask
				struct Glyph {int x, y, w, h; std::vector<uchar> c;};
				QVector<Glyph> gl;
				for (int i = 0; i < gid.size(); i++) {
					double gx = x + pos[i] * ppx / upem;
					double gy = base;
					if (turned) {
						QPointF q = kt.map(QPointF(gx, gy));
						gx = q.x();
						gy = q.y();
					}
					int ix = (int)std::floor(gx);
					int iy = (int)std::floor(gy);
					FT_Vector delta = {(FT_Pos)std::lround((gx - ix) * 64), (FT_Pos)std::lround(-(gy - iy) * 64)};
					FT_Set_Transform(face, turned ? &ftm : nullptr, &delta);
					// the font's own hinting on both paths; LIGHT would hand the glyph to the auto-hinter
					if (FT_Load_Glyph(face, gid[i], (klcd ? FT_LOAD_TARGET_LCD : FT_LOAD_TARGET_NORMAL) | FT_LOAD_NO_AUTOHINT)) continue;
					if ((lab.ebx != 0) || (lab.eby != 0))
						FT_Outline_EmboldenXY(&face->glyph->outline, std::lround(lab.ebx * s * 64), std::lround(lab.eby * s * 64));
					if (FT_Render_Glyph(face->glyph, klcd ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL)) continue;
					const FT_Bitmap& bm = face->glyph->bitmap;
					if (!bm.width || !bm.rows) continue;
					Glyph g;
					g.w = klcd ? bm.width / 3 : bm.width;
					g.h = bm.rows;
					g.x = ix + face->glyph->bitmap_left;
					g.y = iy - face->glyph->bitmap_top;
					g.c.resize(g.w * g.h * 3);
					for (int yy = 0; yy < g.h; yy++) {
						const uchar* s = bm.buffer + yy * bm.pitch;
						uchar* d = g.c.data() + yy * g.w * 3;
						for (int xx = 0; xx < g.w; xx++, d += 3) {
							if (klcd) {
								d[0] = s[xx * 3];
								d[1] = s[xx * 3 + 1];
								d[2] = s[xx * 3 + 2];
							} else {
								d[0] = d[1] = d[2] = s[xx];
							}
						}
					}
					gl.append(g);
				}
				FT_Set_Transform(face, nullptr, nullptr);
				if (gl.isEmpty()) continue;
				QRect box(gl[0].x, gl[0].y, gl[0].w, gl[0].h);
				foreach(const Glyph& g, gl)
					box |= QRect(g.x, g.y, g.w, g.h);
				Mask m;
				m.x = box.x();
				m.y = box.y();
				m.w = box.width();
				m.h = box.height();
				m.c.assign(m.w * m.h * 3, 0);
				foreach(const Glyph& g, gl) {
					for (int yy = 0; yy < g.h; yy++) {
						const uchar* s = g.c.data() + yy * g.w * 3;
						uchar* d = m.c.data() + ((g.y - m.y + yy) * m.w + (g.x - m.x)) * 3;
						for (int k = 0; k < g.w * 3; k++)	// overlapping glyphs: one over the other
							d[k] = 255 - (255 - d[k]) * (255 - s[k]) / 255;
					}
				}
				put(m, lab.col);
			}
		}
	}
	return img;
}

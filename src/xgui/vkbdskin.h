#pragma once

// The on-screen keyboard's picture, drawn from a layout file (res/vkbd/zx-*.json,
// the format of Volutar's layout_svg.py) at any size, sharp at every one of them.

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QJsonValue>
#include <QLinearGradient>
#include <QMap>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTransform>
#include <QVector>

#include <functional>
#include <vector>

struct VkKey {
	QByteArray zx;		// the Spectrum keys it presses, as keyEntry::zxKey takes them; none for lettering
	unsigned char ch;	// the one key it is, 0 for a combination: what lights it from the matrix
	QRectF box;		// the key as drawn (an outline's bounds), in canvas units
	QPainterPath fill;	// where its light goes on a stretched picture, in canvas units
	QRectF hit;		// what a click on it takes: the key and its words
	QPolygonF hitPoly;	// an outline or turned key's contour: a click takes this, not hit
	int row, col;		// where it is in the layout
};

class VkSkin {
	public:
		// a layout with "base" is another one with its own fields over it: base
		// gives that one's file by its name, empty for none
		typedef std::function<QByteArray(const QString&)> BaseFile;
		bool load(const QByteArray& json, QString* err = nullptr, BaseFile base = nullptr);
		bool isNull() const {return rows.isEmpty();}
		QSizeF canvas() const {return QSizeF(cw, ch);}
		// what the window puts under the picture, so what the contours show
		QColor ground() const {return grnd;}
		// a pressed key's light: its color, and where it is - through the contour
		// round the key, under the picture, or by its pad's rim, the lit picture
		// over the key ("light": "pad")
		QColor litColor() const {return litCol;}
		bool padLight() const {return padLit;}
		const QVector<VkKey>& keys() const {return keytab;}
		// where key n's light goes in a picture of that scale, in its pixels:
		// the key and its contour as the picture has them, never a neighbour
		QPainterPath fillAt(int n, double scale) const;
		// the Spectrum key at that place of the keyboard's matrix: half-row 0-3
		// from the top, key 0-9 from the left; 0 for none
		static unsigned char zxKeyAt(int row, int pos);
		// the picture at that many pixels per canvas unit, premultiplied ARGB;
		// lcd: subpixel (RGB) text on its opaque parts; lit: a pad light's, every
		// key lit, its pad's rim in the lit color, or a ring inside the cap's edge
		// on a key with no pad - the window draws it over a pressed key alone
		QImage render(double scale, bool lcd, bool lit = false) const;
	private:
		struct Label {
			QString text;		// a text label...
			QString icon;		// ...or an icon
			QColor col;
			double sz, dx, dy, tl, th, ebx, eby;
			bool hasTl;
			QString anchor;		// "", start, middle, end
			QString sn;		// "", off, px, half
		};
		struct Grad {
			QVector<QPair<double, QColor>> stops;
			double h = 0;			// its length, 0 for the whole body
			bool fromBottom = false;	// that length up from the bottom, not down from the top
			double angle = 0;		// degrees off straight down, clockwise: 45 runs to the bottom right
		};
		// one shape of a dome: a box inside the key's, or a polygon of its own
		struct DomePart {
			double inset[4] = {0, 0, 0, 0};	// left, top, right, bottom from the key's box
			double corner[4] = {0, 0, 0, 0};	// tl, tr, br, bl
			double cut = -1;		// cut flat that far below the key's top; < 0 none
			QVector<QPointF> outline;	// a polygon from the key's top left instead of the box...
			QVector<double> radii;		// ...and the radius at each of its corners
		};
		// a shape laid on the cap, inside it: the 128's round pad with a flat top; the
		// Enter's is two, its pad and a channel up its narrow part
		struct Dome {
			bool on = false;
			DomePart shape;
			QVector<DomePart> more;		// shapes joined to it
			Grad grad;			// its own gradient...
			QColor fill;			// ...or one color
			QColor edge;			// a line inside its rim, ew wide...
			Grad edgeGrad;			// ...or shaded along a gradient of its own
			double ew = 0;
		};
		// the cap's sloped sides seen from above: a band inside its edge, w wide on
		// each side, cut into four by the corners' diagonals, each darker or lighter
		// than the cap, or of a color of its own
		struct Side {
			bool on = false;
			double w[4] = {0, 0, 0, 0};	// left, top, right, bottom
			double shade[4] = {0, 0, 0, 0};	// > 0 darker, < 0 lighter, 1 at most
			QColor fill[4];			// over the shade when given
		};
		struct Key {
			double x, y, w, h, ax, ay;	// an outline key: its bounds
			QString shape, sn;
			QByteArray zx;			// the keys it presses...
			bool hasZx = false;		// ...or none given: its place says
			QVector<QPointF> outline;	// a polygon of right angles instead of the box
			double corner[4];		// tl, tr, br, bl...
			bool hasCorner = false;		// ...or the preset's
			Grad grad;			// the layout's, or its own
			Dome dome;			// the layout's, or its own, or none
			Side side;			// the layout's, its style's or its own
			double tilt = 0;		// turned that many degrees clockwise about its middle
			QVector<Label> labels;
			// caps of the same key, under it or over it: a flat step, a strip glued on
			std::vector<Key> parts;
			bool over = false;		// a part: drawn over the key's own cap
			bool unlit = false;		// a part: drawn the same when the key is lit
		};
		struct Band {
			QColor col;
			double x;		// at the rainbow's top
		};
		struct Row {
			double y, h;
			QVector<Key> keys;
		};
		// a strip drawn on the ground, not a key: the case's steps, the Next's colors
		struct Bar {
			QRectF r;
			double corner = 0;
			Grad grad;		// a gradient...
			QColor fill;		// ...or one color
		};
		struct Icon {
			QVector<QVector<QPointF>> polys;	// filled even-odd, or stroked with sw
			QVector<QRectF> cuts;			// holes in box
			QVector<QVector<QPointF>> cutPolys;
			QRectF box;
			bool isBox = false;
			bool isLine = false;
			QPointF p1, p2;
			double sw = 0;
		};
		double cw = 0, ch = 0;
		QColor bg, grnd, litCol;
		bool padLit = false;
		double litW = 1.8;	// the pad light's width
		QString anchor, sn;
		Grad grad;		// the caps'
		Dome dome;		// on every cap, unless a key says otherwise
		Side side;		// on every cap, unless a key says otherwise
		QMap<QString, QJsonObject> styles;	// a key's cap fields by a name
		double inset = 1, outLR = 0, outTB = 1, corner = 3;
		double rbW = 11, rbSlope = -0.45, rbY0 = 0, rbY1 = 0;
		QVector<Band> bands;
		QVector<Bar> bars;
		QMap<QString, QColor> colors;
		QMap<QString, Icon> icons;
		QVector<Row> rows;
		QVector<VkKey> keytab;
		bool makeKeys(QString* err);
		bool labelSpan(const Label& lab, double y, double& top, double& bot) const;
		QColor color(const QString& name) const;
		void readGrad(const QJsonValue& v, Grad& g) const;
		static QLinearGradient gradAt(const Grad& g, const QRectF& box, double s);
		void readDome(const QJsonObject& o, Dome& d) const;
		static void readDomePart(const QJsonObject& o, DomePart& p);
		void readSide(const QJsonValue& v, Side& sd) const;
		bool readCap(const QJsonObject& o, Key& key, QString& why) const;
		static QTransform tiltAt(const Key& key, const QPointF& mid);
		QTransform tiltPx(const Key& key, double s) const;
		QPainterPath domePart(const Key& key, const DomePart& p, double s) const;
		QRect keyBox(const Key& key, double s) const;
		QPainterPath keyPath(const Key& key, double s, int dx, int dy, double base, double grow) const;
		QPainterPath contourOf(const Key& key, double s) const;
		QVector<QPainterPath> contoursAt(const Key& key, double s) const;
};

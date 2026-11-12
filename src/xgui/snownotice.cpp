#include "snownotice.h"

#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include <QTimer>
#include <QResizeEvent>
#include <QScreen>
#include <QDateTime>
#include <QRandomGenerator>
#include <QtMath>

#define SNOW_SHADE	60		// alpha of the shade, of 255: the game still plain under it
#define SNOW_FADE_IN	0.3		// s
#define SNOW_PX_PER_FLAKE	1700	// picture area, logical px^2
#define SNOW_MIN	80
#define SNOW_MAX	1400
#define SNOW_HZ_MAX	60		// a tick redraws the whole window in software: no need for more
#define SNOW_BLINK_MS	500		// as the recording sign

// per layer, far to near: size, fall speed (px/s), how much wind it takes,
// how bright, its share of the flakes
static const struct {
	float size0, size1, vy0, vy1, drift, alpha, share;
} snowLayer[3] = {
	{2.2f, 3.4f, 20.0f, 38.0f, 0.45f, 0.50f, 0.45f},
	{3.6f, 5.2f, 38.0f, 62.0f, 0.70f, 0.75f, 0.35f},
	{6.0f, 9.0f, 65.0f, 110.0f, 1.00f, 0.85f, 0.20f},
};

static float rnd(float a, float b) {
	return a + (b - a) * (float)QRandomGenerator::global()->generateDouble();
}

xSnowFall::xSnowFall(QWidget* parent) : QWidget(parent) {
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setAttribute(Qt::WA_NoSystemBackground);
	setFocusPolicy(Qt::NoFocus);
	spriteDpr = 0;
	tid = 0;
	lastNs = 0;
	t = 0;
	sign[0] = QImage(":/images/osd-snow.png");
	sign[1] = QImage(":/images/osd-snow-off.png");		// the frame alone
	for (QImage& img : sign) img.setDevicePixelRatio(2);
	hide();
	parent->installEventFilter(this);
}

// The tick follows the screen, up to 60 Hz, not the machine: the snow moves
// at the host's pace, whatever the emulation does meanwhile.
void xSnowFall::start() {
	setGeometry(parentWidget()->rect());
	makeSprites();
	seed();
	t = 0;
	clock.start();
	lastNs = 0;
	show();
	raise();
	if (!tid) {
		QScreen* scr = screen();
		double hz = scr ? scr->refreshRate() : 60.0;
		if (hz < 30) hz = 60;
		if (hz > SNOW_HZ_MAX) hz = SNOW_HZ_MAX;
		tid = startTimer(qMax(4, (int)(1000.0 / hz)), Qt::PreciseTimer);
	}
}

// at once, all of it: the picture is the game's again
void xSnowFall::stop() {
	if (tid) killTimer(tid);
	tid = 0;
	hide();
	flakes.clear();
	flakes.shrink_to_fit();
}

bool xSnowFall::eventFilter(QObject* obj, QEvent* ev) {
	if ((obj == parentWidget()) && (ev->type() == QEvent::Resize) && isVisible()) {
		QSize old = size();
		setGeometry(parentWidget()->rect());
		if (old != size()) {
			// the same density over the new area: flakes added at the top or dropped
			seed();
		}
	}
	return false;
}

void xSnowFall::spawn(Flake& f, bool top) {
	float wid = width();
	float hgt = height();
	f.x = rnd(-20, wid + 20);
	f.y = top ? rnd(-30, -4) : rnd(-10, hgt);
	float r = rnd(0, 1);
	f.layer = (r < snowLayer[0].share) ? 0 : (r < snowLayer[0].share + snowLayer[1].share) ? 1 : 2;
	f.vy = rnd(snowLayer[f.layer].vy0, snowLayer[f.layer].vy1);
	f.amp = rnd(5, 18) * (0.6f + 0.2f * f.layer);
	f.freq = rnd(0.2f, 0.5f);		// a sway of 2 to 5 s
	f.phase = rnd(0, 2 * M_PI);
	f.drift = snowLayer[f.layer].drift * rnd(0.8f, 1.2f);
	f.kind = QRandomGenerator::global()->bounded((int)sprites[f.layer].size());
	f.rot = rnd(0, 60);
	f.spin = rnd(-40, 40);		// degrees a second, crystals only
}

void xSnowFall::seed() {
	size_t want = (size_t)qBound(SNOW_MIN, (width() * height()) / SNOW_PX_PER_FLAKE, SNOW_MAX);
	size_t had = flakes.size();
	flakes.resize(want);
	for (size_t i = had; i < want; i++)
		spawn(flakes[i], had > 0);	// the first fall is already under way
}

// Soft round flakes for every layer, and for the nearest one also a few small
// six-armed crystals. Made at the screen's pixel ratio, once per ratio.
void xSnowFall::makeSprites() {
	qreal dpr = devicePixelRatioF();
	if (dpr == spriteDpr) return;
	spriteDpr = dpr;
	for (int l = 0; l < 3; l++) {
		sprites[l].clear();
		for (int k = 0; k < 4; k++) {
			float sz = snowLayer[l].size0 + (snowLayer[l].size1 - snowLayer[l].size0) * k / 3.0f;
			bool crystal = (l == 2) && (k >= 2);
			if (crystal) sz *= 1.5f;
			int px = (int)ceil((sz + 2) * dpr);
			QPixmap pm(px, px);
			pm.fill(Qt::transparent);
			pm.setDevicePixelRatio(dpr);
			QPainter p(&pm);
			p.setRenderHint(QPainter::Antialiasing);
			QPointF c((sz + 2) / 2, (sz + 2) / 2);
			int a = (int)(255 * snowLayer[l].alpha);
			if (crystal) {
				// a faint glow under the arms
				QRadialGradient g(c, sz / 2);
				g.setColorAt(0.0, QColor(220, 235, 255, a / 3));
				g.setColorAt(1.0, QColor(220, 235, 255, 0));
				p.setPen(Qt::NoPen);
				p.setBrush(g);
				p.drawEllipse(c, sz / 2, sz / 2);
				p.setPen(QPen(QColor(240, 248, 255, a), 1.2, Qt::SolidLine, Qt::RoundCap));
				float r = sz / 2 - 0.5f;
				for (int i = 0; i < 6; i++) {
					double ang = i * M_PI / 3;
					QPointF tip = c + QPointF(r * cos(ang), r * sin(ang));
					p.drawLine(c, tip);
					// a pair of branches halfway out
					QPointF mid = c + QPointF(r * 0.55 * cos(ang), r * 0.55 * sin(ang));
					for (int s = -1; s <= 1; s += 2) {
						double b = ang + s * M_PI / 4;
						p.drawLine(mid, mid + QPointF(r * 0.35 * cos(b), r * 0.35 * sin(b)));
					}
				}
			} else {
				// the near ones out of focus: a wider, softer edge
				QRadialGradient g(c, sz / 2);
				g.setColorAt(0.0, QColor(255, 255, 255, a));
				g.setColorAt((l == 2) ? 0.35 : 0.5, QColor(245, 250, 255, (l == 2) ? a * 3 / 5 : a * 4 / 5));
				g.setColorAt(1.0, QColor(240, 248, 255, 0));
				p.setPen(Qt::NoPen);
				p.setBrush(g);
				p.drawEllipse(c, sz / 2, sz / 2);
			}
			p.end();
			sprites[l].push_back(pm);
		}
	}
}

// One wind for all: a slow breathing of two low sines, and now and then a
// stronger spell that leans the whole fall over.
double xSnowFall::wind() const {
	double w = 14 * sin(0.31 * t) + 8 * sin(0.83 * t + 1.3);
	double gust = sin(0.13 * t + 0.7);
	if (gust > 0) w += 55 * pow(gust, 6);
	return w;
}

void xSnowFall::step(double dt) {
	t += dt;
	double w = wind();
	float wid = width();
	float hgt = height();
	for (Flake& f : flakes) {
		// a near flake catches more of the gust, and the gust lifts it a little
		f.x += (float)(w * f.drift * dt);
		f.rot += (float)(f.spin * dt);
		f.y += (float)(f.vy * dt * (1.0 - 0.12 * f.drift * fabs(w) / 70.0));
		if (f.y > hgt + 12) {
			spawn(f, true);
		} else if (f.x > wid + 30) {
			f.x -= wid + 50;
		} else if (f.x < -30) {
			f.x += wid + 50;
		}
	}
}

void xSnowFall::timerEvent(QTimerEvent* ev) {
	if (ev->timerId() != tid) {
		QWidget::timerEvent(ev);
		return;
	}
	qint64 now = clock.nsecsElapsed();
	double dt = (now - lastNs) / 1e9;
	lastNs = now;
	if (dt > 0.1) dt = 0.1;		// a host that stalled does not throw the snow about
	step(dt);
	update();
}

void xSnowFall::paintEvent(QPaintEvent*) {
	if (devicePixelRatioF() != spriteDpr) makeSprites();	// moved to a screen of another scale
	QPainter p(this);
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);		// turned crystals
	double in = qMin(1.0, clock.nsecsElapsed() / 1e9 / SNOW_FADE_IN);
	p.fillRect(rect(), QColor(0, 0, 0, (int)(SNOW_SHADE * in)));
	p.setOpacity(in);
	for (const Flake& f : flakes) {
		const QPixmap& pm = sprites[f.layer][f.kind];
		float sway = f.amp * (float)sin(2 * M_PI * f.freq * t + f.phase);
		QSizeF sz = QSizeF(pm.size()) / pm.devicePixelRatio();
		if ((f.layer == 2) && (f.kind >= 2)) {		// a crystal turns as it falls
			p.save();
			p.translate(f.x + sway, f.y);
			p.rotate(f.rot);
			p.drawPixmap(QPointF(-sz.width() / 2, -sz.height() / 2), pm);
			p.restore();
		} else {
			p.drawPixmap(QPointF(f.x + sway - sz.width() / 2, f.y - sz.height() / 2), pm);
		}
	}
	p.setOpacity(1.0);
	if (signAt) drawSign(p, signAt());
}

// as the recording sign blinks
void xSnowFall::drawSign(QPainter& p, QPoint at) {
	int on = (QDateTime::currentMSecsSinceEpoch() / SNOW_BLINK_MS) & 1;
	p.save();
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);		// the 2x picture
	p.drawImage(at, sign[on ? 0 : 1]);
	p.restore();
}

#define QUIET_GAP	12		// px at least between the check box and the first button

// The check box placed by hand on the buttons' row, at the text's left edge; its own row if it would touch a button.
class xSnowBox : public QMessageBox {
	public:
		xSnowBox(QWidget* parent) : QMessageBox(parent) {}
		void setQuiet(QCheckBox* box, QAbstractButton* beside) {
			quiet = box;
			level = beside;
			text = findChild<QLabel*>("qt_msgbox_label");
			QWidget* bar = level->parentWidget();
			if (!text || !bar || !isAncestorOf(bar)) {
				setCheckBox(quiet);		// not the box known: its own row, as Qt does it
				quiet = nullptr;
				return;
			}
			for (QObject* o : {(QObject*)level, (QObject*)bar, (QObject*)text})
				o->installEventFilter(this);
			placeQuiet();
		}
	protected:
		bool eventFilter(QObject* obj, QEvent* ev) override {
			QEvent::Type t = ev->type();
			if ((t == QEvent::Move) || (t == QEvent::Resize) || (t == QEvent::Show)) placeQuiet();
			return QMessageBox::eventFilter(obj, ev);
		}
	private:
		QCheckBox* quiet = nullptr;
		QAbstractButton* level = nullptr;
		QLabel* text = nullptr;
		bool judging = false;
		void placeQuiet() {
			if (!quiet) return;
			QSize sz = quiet->sizeHint();
			int x = text->mapTo(this, QPoint(0, 0)).x();
			int y = level->mapTo(this, QPoint(0, 0)).y() + (level->height() - sz.height()) / 2;
			quiet->setGeometry(x, y, sz.width(), sz.height());
			quiet->raise();
			// judged once the layout settles (showing resizes the box after its children
			// moved), and out of the filter, as setCheckBox() lays the box out anew
			if (isVisible() && !judging) {
				judging = true;
				QTimer::singleShot(0, this, [this]() {judging = false; judgeQuiet();});
			}
		}
		void judgeQuiet() {
			if (!quiet) return;
			int right = quiet->geometry().right() + QUIET_GAP;
			for (QAbstractButton* bt : level->parentWidget()->findChildren<QAbstractButton*>(QString(), Qt::FindDirectChildrenOnly)) {
				if (bt->isHidden() || (right < bt->mapTo(this, QPoint(0, 0)).x())) continue;
				QCheckBox* cb = quiet;
				quiet = nullptr;
				setCheckBox(cb);
				return;
			}
		}
};

QMessageBox* snow_notice_box(QWidget* parent, const QUrl& more, std::function<void()> off, std::function<void()> never) {
	xSnowBox* box = new xSnowBox(parent);
	box->setAttribute(Qt::WA_DeleteOnClose);
#if QT_VERSION >= QT_VERSION_CHECK(6,6,0)
	box->setOption(QMessageBox::Option::DontUseNativeDialog);	// a native one (macOS) would drop the check box
#endif
	box->setModal(false);
	box->setIcon(QMessageBox::Information);
	box->setWindowTitle("ULA snow");
	box->setTextFormat(Qt::RichText);
	box->setTextInteractionFlags(Qt::TextBrowserInteraction);	// the link opens the manual
	// plain words: read by people of every language, in English
	box->setText(QString("This game causes <b>ULA snow</b>: random dots and lines on the screen.<br><br>"
		"Real ZX Spectrum 48K, 128K and +2 computers show it too. It is a problem in the game "
		"itself, so you may want to tell its author.<br><br>"
		"To turn it off, choose a machine without snow, or turn off ULA snow in:<br>"
		"<b>Options &gt; Machine &gt; Advanced settings</b><br><br>"
		"<a href=\"%0\">More about ULA snow</a>").arg(more.toString()));
	// one role for both, so they stand in the order given on every platform
	QPushButton* btOff = box->addButton("Turn Off for Now", QMessageBox::AcceptRole);
	QPushButton* btGo = box->addButton("Continue", QMessageBox::AcceptRole);
	QIcon flake;
	flake.addFile(":/images/snowflake-22.png", QSize(22, 22));
	flake.addFile(":/images/snowflake-44.png", QSize(44, 44));
	btGo->setIcon(flake);
	btGo->setIconSize(QSize(22, 22));		// the OSD sign's flake: at 16 px its branches run together
	btGo->ensurePolished();
	btOff->ensurePolished();
	btOff->setMinimumHeight(btGo->sizeHint().height());		// the icon makes it taller: both one height
	box->setDefaultButton(btGo);
	box->setEscapeButton(btGo);
	// the notice only, not the snow: unticked by default
	QCheckBox* quiet = new QCheckBox("Don't show this again", box);
	quiet->setToolTip("Options > Appearance can turn it back on");
	box->setQuiet(quiet, btOff);
	QObject::connect(box, &QMessageBox::buttonClicked, box, [btOff, off](QAbstractButton* bt) {
		if (bt == btOff) off();
	});
	// both buttons, Esc and the title bar's close all finish it
	QObject::connect(box, &QDialog::finished, box, [quiet, never]() {
		if (quiet->isChecked()) never();
	});
	return box;
}

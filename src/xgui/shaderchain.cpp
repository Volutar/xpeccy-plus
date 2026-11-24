#include "shaderchain.h"

#include "../libxpeccy/xlog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QOpenGLContext>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstring>

// names a header of GL ES 3.0 or of desktop GL 3.3 may lack
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812D
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif
#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT 0x8370
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#endif
#ifndef GL_PROGRAM_BINARY_LENGTH
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#endif
#ifndef GL_NUM_PROGRAM_BINARY_FORMATS
#define GL_NUM_PROGRAM_BINARY_FORMATS 0x87FE
#endif
#ifndef GL_COMPLETION_STATUS_KHR
#define GL_COMPLETION_STATUS_KHR 0x91B1
#endif
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif

namespace {

QElapsedTimer& chainClock() {
	static QElapsedTimer t = []{QElapsedTimer e; e.start(); return e;}();
	return t;
}

double msSince(qint64 ns) {
	return (chainClock().nsecsElapsed() - ns) / 1e6;
}

// Interleaved quad: position (x,y) + texcoord (u,v), a triangle strip. Positions are
// in [0,1]: MVP puts them into the target, row 0 of it being the picture's top.
const GLfloat kQuad[] = {
	1.0f, 0.0f,  1.0f, 0.0f,
	0.0f, 0.0f,  0.0f, 0.0f,
	1.0f, 1.0f,  1.0f, 1.0f,
	0.0f, 1.0f,  0.0f, 1.0f,
};

// ortho(0, 1, 1, 0, -1, 1), column by column: y turned over, the window's rows going up
const GLfloat kMvpWindow[16] = {
	2.0f, 0.0f, 0.0f, 0.0f,
	0.0f, -2.0f, 0.0f, 0.0f,
	0.0f, 0.0f, -1.0f, 0.0f,
	-1.0f, 1.0f, -0.0f, 1.0f,
};

// ortho(0, 1, 0, 1, -1, 1): a texture's row 0 stays the picture's top
const GLfloat kMvpTarget[16] = {
	2.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 2.0f, 0.0f, 0.0f,
	0.0f, 0.0f, -1.0f, 0.0f,
	-1.0f, -1.0f, -0.0f, 1.0f,
};

bool contextIsES() {
	QOpenGLContext* ctx = QOpenGLContext::currentContext();
	return ctx && ctx->isOpenGLES();
}

const char* versionPrefix(bool es) {
	return es
		? "#version 300 es\nprecision highp float;\n"
		: "#version 330\n";
}

const char* defaultVertexBody() {
	return
		"layout(location=0) in vec2 a_pos;\n"
		"layout(location=1) in vec2 a_uv;\n"
		"out vec2 v_uv;\n"
		"void main() {\n"
		"    v_uv = a_uv;\n"
		"    gl_Position = vec4(a_pos.x * 2.0 - 1.0, 1.0 - a_pos.y * 2.0, 0.0, 1.0);\n"
		"}\n";
}

// No shader chosen. At a whole zoom each texel is a block of whole pixels and is
// taken as it is. At any other scale (u_smooth) the texel's middle stays flat and
// only the one pixel where two texels meet is mixed of both, so the dots keep one
// width and the picture stays sharp. The mix is done here, from the NEAREST texture.
const char* defaultFragmentBody() {
	return
		"in vec2 v_uv;\n"
		"out vec4 fragColor;\n"
		"uniform sampler2D u_tex;\n"
		"uniform int u_smooth;\n"
		"uniform vec2 rubyTextureSize;\n"
		"uniform vec2 rubyOutputSize;\n"
		"void main() {\n"
		"    if (u_smooth == 0) {\n"
		"        fragColor = texture(u_tex, v_uv);\n"
		"        return;\n"
		"    }\n"
		"    vec2 scale = rubyOutputSize / rubyTextureSize;\n"
		"    vec2 texel = v_uv * rubyTextureSize;\n"
		"    vec2 cell = floor(texel);\n"
		"    vec2 off = texel - cell - 0.5;\n"
		"    vec2 flat_half = max(0.5 - 0.5 / scale, vec2(0.0));\n"
		"    vec2 p = cell + (off - clamp(off, -flat_half, flat_half)) * scale;\n"
		"    ivec2 i0 = ivec2(floor(p));\n"
		"    vec2 w = p - floor(p);\n"
		"    ivec2 hi = ivec2(rubyTextureSize) - 1;\n"
		"    vec4 a = texelFetch(u_tex, clamp(i0, ivec2(0), hi), 0);\n"
		"    vec4 b = texelFetch(u_tex, clamp(i0 + ivec2(1, 0), ivec2(0), hi), 0);\n"
		"    vec4 c = texelFetch(u_tex, clamp(i0 + ivec2(0, 1), ivec2(0), hi), 0);\n"
		"    vec4 d = texelFetch(u_tex, clamp(i0 + ivec2(1, 1), ivec2(0), hi), 0);\n"
		// mixed as light, gamma 2.2: a mix of the stored values is darker than
		// either side's light, and the seams would show as a moire
		"    const vec3 g = vec3(2.2);\n"
		"    vec3 m = mix(mix(pow(a.rgb, g), pow(b.rgb, g), w.x), mix(pow(c.rgb, g), pow(d.rgb, g), w.x), w.y);\n"
		"    fragColor = vec4(pow(m, 1.0 / g), 1.0);\n"
		"}\n";
}

// -----------------------------------------------------------------------------
// Legacy GLSL 110 shim.
//
// Rewrites fixed-function-era shader source onto the core-compatible VBO
// attribute and uniform layout. The transformation is entirely data-driven
// by the tables below - to add a new legacy identifier, append a row to the
// relevant table; to add a new preamble attribute or uniform, edit the stage
// preamble helper.
// -----------------------------------------------------------------------------

struct Rewrite {
	QRegularExpression pattern;
	QString replacement;
};

// Build a word-boundary-bounded regex for an identifier. Lookaround is used
// instead of \b so tokens ending in punctuation (e.g. gl_TexCoord[0]) can
// still be exact-matched as a future extension.
QRegularExpression wordBoundedRegex(const char* identifier) {
	return QRegularExpression(
		QStringLiteral(R"((?<!\w))")
		+ QRegularExpression::escape(QLatin1String(identifier))
		+ QStringLiteral(R"((?!\w))"));
}

Rewrite rewrite(const char* identifier, const char* replacement) {
	return { wordBoundedRegex(identifier), QString::fromLatin1(replacement) };
}

QString applyRewrites(QString s, const std::vector<Rewrite>& rewrites) {
	for (const auto& r : rewrites) s.replace(r.pattern, r.replacement);
	return s;
}

// Signals whose presence in the source triggers the shim pipeline.
// Absence of all of them lets a modern shader pass through untouched.
const std::vector<QRegularExpression>& legacySignalPatterns() {
	static const std::vector<QRegularExpression> r{
		wordBoundedRegex("gl_Vertex"),
		wordBoundedRegex("gl_MultiTexCoord0"),
		wordBoundedRegex("gl_ModelViewProjectionMatrix"),
		wordBoundedRegex("gl_FragColor"),
		wordBoundedRegex("varying"),
	};
	return r;
}

bool sourceNeedsLegacyShim(const QString& src) {
	const auto& patterns = legacySignalPatterns();
	return std::any_of(patterns.begin(), patterns.end(),
		[&src](const QRegularExpression& re) { return src.contains(re); });
}

const QRegularExpression& versionDirective() {
	static const QRegularExpression re(
		QStringLiteral(R"(^[ \t]*#[ \t]*version\b[^\n]*\n?)"),
		QRegularExpression::MultilineOption);
	return re;
}

// Rewrites applied to both vertex and fragment stages. half: a name GLSL 1.10 left
// free and 3.30 keeps for itself - NVIDIA lets it pass, Mesa does not.
const std::vector<Rewrite>& sharedRewrites() {
	static const std::vector<Rewrite> r{
		rewrite("texture2D", "texture"),
		rewrite("half", "xpq_half_"),
	};
	return r;
}

// ...and on OpenGL ES: sample is free in 3.30, kept in ES 3.00
const std::vector<Rewrite>& esRewrites() {
	static const std::vector<Rewrite> r{
		rewrite("sample", "xpq_sample_"),
	};
	return r;
}

const std::vector<Rewrite>& vertexRewrites() {
	static const std::vector<Rewrite> r{
		rewrite("varying",                      "out"),
		rewrite("gl_Vertex",                    "vec4(a_pos_.x, a_pos_.y, 0.0, 1.0)"),
		rewrite("gl_MultiTexCoord0",            "vec4(a_uv_.x,  a_uv_.y,  0.0, 0.0)"),
		rewrite("gl_ModelViewProjectionMatrix", "u_mvp_"),
	};
	return r;
}

const std::vector<Rewrite>& fragmentRewrites() {
	static const std::vector<Rewrite> r{
		rewrite("varying",      "in"),
		rewrite("gl_FragColor", "frag_out_"),
	};
	return r;
}

QString vertexShimPreamble(bool es) {
	return QString::fromLatin1(versionPrefix(es))
	     + QStringLiteral(
		     "layout(location=0) in vec2 a_pos_;\n"
		     "layout(location=1) in vec2 a_uv_;\n"
		     "uniform mat4 u_mvp_;\n");
}

QString fragmentShimPreamble(bool es) {
	return QString::fromLatin1(versionPrefix(es))
	     + QStringLiteral("out vec4 frag_out_;\n");
}

// One section of an old file as the driver gets it. Its #version line gives way to
// the shim's preamble (shim) or stays; a #line keeps the driver's line numbers the
// file's own either way. line: the file line of the section's first line.
QString legacyStage(QString src, int line, bool shim, const std::vector<Rewrite>& stageRewrites, const QString& preamble, bool es) {
	const QRegularExpressionMatch m = versionDirective().match(src);
	if (m.hasMatch()) {
		const int at = line + src.left(m.capturedStart()).count('\n') + 1;	// the line after it
		const QString mark = QString("#line %1\n").arg(at);
		if (shim) {
			src.replace(m.capturedStart(), m.capturedLength(), mark);
		} else {
			QString ver = m.captured();
			if (!ver.endsWith('\n')) ver.append('\n');
			src.replace(m.capturedStart(), m.capturedLength(), ver + mark);
		}
	} else if (shim) {
		src.prepend(QString("#line %1\n").arg(line));
	}
	if (!shim) return src;
	src = applyRewrites(std::move(src), sharedRewrites());
	if (es) src = applyRewrites(std::move(src), esRewrites());
	src = applyRewrites(std::move(src), stageRewrites);
	return preamble + src;
}

// main() renamed and wrapped: u_place_ moves gl_Position from the picture's clip space into the
// viewport's, so a mask from gl_Position keeps its pitch; unset it collapses all, drawLast sets it
QString placeVertexShader(QString src) {
	static const QRegularExpression re(QStringLiteral(R"((?<!\w)main(?=\s*\())"));
	if (!src.contains(re)) return src;
	src.replace(re, QStringLiteral("xpq_main_"));
	return src + QStringLiteral(
		"\nuniform vec4 u_place_;\n"
		"void main() {\n"
		"    xpq_main_();\n"
		"    gl_Position.xy = gl_Position.xy * u_place_.xy + u_place_.zw * gl_Position.w;\n"
		"}\n");
}

// ------------------------------------------------------------ preset parsing

const QRegularExpression& passHeader() {
	static const QRegularExpression re(QStringLiteral(R"(^pass(?:\s+([A-Za-z_]\w*))?\s*:\s*(?:#.*)?$)"));
	return re;
}

const QRegularExpression& paramPragma() {
	static const QRegularExpression re(QStringLiteral(
		R"re(^#\s*pragma\s+parameter\s+([A-Za-z_]\w*)\s+"([^"]*)"\s+(\S+)\s+(\S+)\s+(\S+)(?:\s+(\S+))?\s*$)re"));
	return re;
}

int wrapByName(const QString& s) {
	if (s == "border") return XSHD_WRAP_BORDER;
	if (s == "clamp") return XSHD_WRAP_CLAMP;
	if (s == "repeat") return XSHD_WRAP_REPEAT;
	if (s == "mirror") return XSHD_WRAP_MIRROR;
	return -1;
}

int scaleByName(const QString& s) {
	if (s == "source") return XSHD_SCALE_SOURCE;
	if (s == "viewport") return XSHD_SCALE_VIEWPORT;
	if (s == "absolute") return XSHD_SCALE_ABSOLUTE;
	return -1;
}

// a pass's own lines, between its header and its first stage; false: not understood
bool passSetting(xShdPass& p, const QStringList& t) {
	const QString& k = t[0];
	bool ok = true;
	if ((k == "scale") && (t.size() >= 3)) {
		const int type = scaleByName(t[1]);
		if (type < 0) return false;
		const float a = t[2].toFloat(&ok);
		if (!ok) return false;
		const float b = (t.size() > 3) ? t[3].toFloat(&ok) : a;
		if (!ok || ((type == XSHD_SCALE_ABSOLUTE) && (t.size() < 4))) return false;
		p.scaleType[0] = p.scaleType[1] = type;
		p.scale[0] = a;
		p.scale[1] = b;
		p.hasScale = true;
	} else if (((k == "scale_x") || (k == "scale_y")) && (t.size() == 3)) {
		const int type = scaleByName(t[1]);
		const float a = t[2].toFloat(&ok);
		if ((type < 0) || !ok) return false;
		const int ax = (k == "scale_x") ? 0 : 1;
		p.scaleType[ax] = type;
		p.scale[ax] = a;
		p.hasScale = true;
	} else if ((k == "filter") && (t.size() == 2) && ((t[1] == "linear") || (t[1] == "nearest"))) {
		p.linear = (t[1] == "linear");
	} else if ((k == "wrap") && (t.size() == 2) && (wrapByName(t[1]) >= 0)) {
		p.wrap = wrapByName(t[1]);
	} else if ((k == "format") && (t.size() == 2)) {
		if (t[1] == "rgba8") p.format = XSHD_FMT_RGBA8;
		else if (t[1] == "srgb8") p.format = XSHD_FMT_SRGB8;
		else if (t[1] == "rgba16f") p.format = XSHD_FMT_RGBA16F;
		else return false;
		p.hasFormat = true;
	} else if ((k == "mipmap") && (t.size() == 1)) {
		p.mipmap = true;
	} else if ((k == "when") && (t.size() == 2)) {
		p.when = t[1];
	} else {
		return false;
	}
	return true;
}

// texture NAME = file [linear|nearest] [clamp|border|repeat|mirror] [mipmap]
bool textureLine(const QString& lin, xShdTexture& tx) {
	static const QRegularExpression re(QStringLiteral(R"(^texture\s+([A-Za-z_]\w*)\s*=\s*("[^"]+"|\S+)(.*)$)"));
	const QRegularExpressionMatch m = re.match(lin);
	if (!m.hasMatch()) return false;
	tx.name = m.captured(1);
	tx.file = m.captured(2);
	if (tx.file.startsWith('"')) tx.file = tx.file.mid(1, tx.file.size() - 2);
	const QStringList opts = m.captured(3).section('#', 0, 0).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
	for (const QString& o : opts) {
		if (o == "linear") tx.linear = true;
		else if (o == "nearest") tx.linear = false;
		else if (o == "mipmap") tx.mipmap = true;
		else if (wrapByName(o) >= 0) tx.wrap = wrapByName(o);
		else return false;
	}
	return true;
}

// The lines that say how the sheet of knobs shows the parameters, before the first
// pass: name "..", group "..", tip NAME = text, list NAME = a, b, c, more NAME ..,
// preset "..": NAME value, ... What names a parameter is tied to it once all are read.
struct HeadNotes {
	QString group;				// the one the next parameters stand in
	QString groupSwitch;			// ...its switch, if it has one
	float groupOn = 0;
	struct Line {int n; QString kind; QString name; QString text;};
	QVector<Line> lines;
};

bool headLine(const QString& lin, int n, xShdPreset& out, HeadNotes& head) {
	static const QRegularExpression reName(QStringLiteral(R"re(^name\s+"([^"]*)"\s*$)re"));
	static const QRegularExpression reGroup(QStringLiteral(
		R"re(^group\s+"([^"]*)"(?:\s+when\s+([A-Za-z_]\w*)(?:\s+(\S+))?)?\s*$)re"));
	static const QRegularExpression reText(QStringLiteral(R"re(^(tip|list)\s+([A-Za-z_]\w*)\s*=\s*(.+)$)re"));
	static const QRegularExpression reMore(QStringLiteral(R"re(^more((?:\s+[A-Za-z_]\w*)+)\s*$)re"));
	static const QRegularExpression reSet(QStringLiteral(R"re(^preset\s+"([^"]+)"\s*:\s*(.*)$)re"));
	QRegularExpressionMatch m = reName.match(lin);
	if (m.hasMatch()) {
		out.title = m.captured(1);
		return true;
	}
	m = reGroup.match(lin);
	if (m.hasMatch()) {
		head.group = m.captured(1);
		head.groupSwitch = m.captured(2);
		head.groupOn = m.captured(3).toFloat();	// 0: none given, see headApply
		return true;
	}
	m = reText.match(lin);
	if (m.hasMatch()) {
		head.lines.append({n, m.captured(1), m.captured(2), m.captured(3).trimmed()});
		return true;
	}
	m = reMore.match(lin);
	if (m.hasMatch()) {
		for (const QString& nam : m.captured(1).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts))
			head.lines.append({n, "more", nam, QString()});
		return true;
	}
	m = reSet.match(lin);
	if (m.hasMatch()) {
		xShdSet set;
		set.name = m.captured(1);
		for (const QString& item : m.captured(2).split(',', Qt::SkipEmptyParts)) {
			const QStringList t = item.trimmed().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
			bool ok = (t.size() == 2);
			const float val = ok ? t[1].toFloat(&ok) : 0.0f;
			if (ok) set.values.append({t[0], val});
			else out.notes << QString("%1:%2: preset \"%3\": not understood: %4").arg(out.file).arg(n + 1).arg(set.name, item.trimmed());
		}
		out.sets << set;
		return true;
	}
	return false;
}

void headApply(xShdPreset& out, const HeadNotes& head) {
	auto find = [&out](const QString& name) -> xShdParam* {
		for (xShdParam& p : out.params)
			if (p.name == name) return &p;
		return nullptr;
	};
	for (const HeadNotes::Line& l : head.lines) {
		xShdParam* p = find(l.name);
		if (!p) {
			out.notes << QString("%1:%2: %3 %4: no such parameter").arg(out.file).arg(l.n + 1).arg(l.kind, l.name);
		} else if (l.kind == "tip") {
			p->tip = l.text;
		} else if (l.kind == "more") {
			p->more = true;
		} else {
			p->names.clear();
			for (const QString& s : l.text.split(','))
				p->names << s.trimmed();
		}
	}
	for (xShdParam& p : out.params) {
		if (p.groupSwitch.isEmpty()) continue;
		const xShdParam* sw = find(p.groupSwitch);
		if (!sw) {
			out.notes << QString("%1: group \"%2\" when %3: no such parameter").arg(out.file, p.group, p.groupSwitch);
			p.groupSwitch.clear();
			continue;
		}
		// switched on with nothing to go back to: the value the line gives, inside the
		// parameter's range; none given - its default, or its most if that is 0
		p.groupOn = std::min(std::max(p.groupOn, sw->min), sw->max);
		if (p.groupOn == 0.0f) p.groupOn = (sw->def != 0.0f) ? sw->def : sw->max;
	}
	for (xShdSet& set : out.sets) {
		for (int i = set.values.size() - 1; i >= 0; i--) {
			if (find(set.values[i].name)) continue;
			out.notes << QString("%1: preset \"%2\": %3: no such parameter").arg(out.file, set.name, set.values[i].name);
			set.values.remove(i);
		}
	}
}

} // anonymous namespace

QByteArray xshd_compose(const char* body) {
	return QByteArray(versionPrefix(contextIsES())) + body;
}

bool xshd_parse(const QString& path, xShdPreset& out, QString* err) {
	out = xShdPreset();
	out.path = path;
	out.file = QFileInfo(path).fileName();
	QFile file(path);
	if (!file.open(QFile::ReadOnly)) {
		*err = QString("%1: cannot be opened").arg(out.file);
		return false;
	}
	QStringList lines;
	while (!file.atEnd())
		lines << QString::fromUtf8(file.readLine()).trimmed();
	file.close();
	for (const QString& lin : lines) {
		if (passHeader().match(lin).hasMatch()) {
			out.legacy = false;
			break;
		}
	}
	if (out.legacy) {
		// as ever: what stands before the first section is dropped
		xShdPass p;
		int mode = 0;
		for (int n = 0; n < lines.size(); n++) {
			const QString& lin = lines[n];
			if (lin.startsWith("vertex:")) {
				mode = 1;
				if (!p.vtxLine) p.vtxLine = n + 2;
			} else if (lin.startsWith("fragment:")) {
				mode = 2;
				if (!p.frgLine) p.frgLine = n + 2;
			} else if (mode == 1) {
				p.vtx.append(lin).append('\n');
			} else if (mode == 2) {
				p.frg.append(lin).append('\n');
			}
		}
		out.passes << p;
		return true;
	}
	enum {TOP, COMMON, SETTINGS, VERTEX, FRAGMENT};
	int mode = TOP;
	HeadNotes head;
	auto note = [&out](int n, const QString& text) {
		out.notes << QString("%1:%2: %3").arg(out.file).arg(n + 1).arg(text);
	};
	for (int n = 0; n < lines.size(); n++) {
		const QString& lin = lines[n];
		const QRegularExpressionMatch pm = paramPragma().match(lin);
		if (pm.hasMatch()) {
			xShdParam prm;
			prm.name = pm.captured(1);
			prm.text = pm.captured(2);
			prm.def = pm.captured(3).toFloat();
			prm.min = pm.captured(4).toFloat();
			prm.max = pm.captured(5).toFloat();
			prm.step = pm.captured(6).toFloat();
			prm.value = prm.def;
			if (mode == TOP) {
				prm.group = head.group;
				prm.groupSwitch = head.groupSwitch;
				prm.groupOn = head.groupOn;
			}
			bool have = false;
			for (const xShdParam& o : out.params) have = have || (o.name == prm.name);
			if (!have) out.params << prm;
			// in a stage it stays a line of GLSL, which skips a pragma it does not know
			if ((mode == TOP) || (mode == SETTINGS)) continue;
		}
		const QRegularExpressionMatch hm = passHeader().match(lin);
		if (hm.hasMatch()) {
			xShdPass p;
			p.name = hm.captured(1);
			p.line = n + 1;
			out.passes << p;
			mode = SETTINGS;
			continue;
		}
		if (lin.startsWith("common:")) {
			mode = COMMON;
			if (!out.commonLine) out.commonLine = n + 2;
			continue;
		}
		if (lin.startsWith("vertex:") || lin.startsWith("fragment:")) {
			if (out.passes.isEmpty()) {
				note(n, "a stage before the first pass");
				mode = TOP;
				continue;
			}
			xShdPass& p = out.passes.last();
			if (lin.startsWith("vertex:")) {
				mode = VERTEX;
				if (!p.vtxLine) p.vtxLine = n + 2;
			} else {
				mode = FRAGMENT;
				if (!p.frgLine) p.frgLine = n + 2;
			}
			continue;
		}
		switch (mode) {
			case TOP: {
				if (lin.startsWith("texture ") || lin.startsWith("texture\t")) {
					xShdTexture tx;
					tx.line = n + 1;
					if (textureLine(lin, tx)) out.textures << tx;
					else note(n, "a texture line not understood");
				} else {
					headLine(lin, n, out, head);
				}
				break;		// anything else before the passes is free text
			}
			case COMMON:
				out.common.append(lin).append('\n');
				break;
			case SETTINGS: {
				const QString body = lin.section('#', 0, 0).trimmed();
				if (body.isEmpty()) break;
				if (!passSetting(out.passes.last(), body.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)))
					note(n, QString("not understood: %1").arg(body));
				break;
			}
			case VERTEX:
				out.passes.last().vtx.append(lin).append('\n');
				break;
			case FRAGMENT:
				out.passes.last().frg.append(lin).append('\n');
				break;
		}
	}
	if (out.passes.size() > XSHD_PASS_MAX) {
		*err = QString("%1: more than %2 passes").arg(out.file).arg(XSHD_PASS_MAX);
		return false;
	}
	headApply(out, head);
	for (int i = 0; i < out.passes.size(); i++) {
		xShdPass& p = out.passes[i];
		if (p.frg.trimmed().isEmpty()) {
			*err = QString("%1:%2: the pass has no fragment section").arg(out.file).arg(p.line);
			return false;
		}
		if (!p.when.isEmpty()) {
			bool have = false;
			for (const xShdParam& o : out.params) have = have || (o.name == p.when);
			if (!have) {
				out.notes << QString("%1:%2: when %3: no such parameter").arg(out.file).arg(p.line).arg(p.when);
				p.when.clear();
			}
		}
		if ((i == out.passes.size() - 1) && (p.hasScale || p.hasFormat))
			out.notes << QString("%1:%2: the last pass draws into the window: its scale and format are not used")
				.arg(out.file).arg(p.line);
	}
	return true;
}

// ---------------------------------------------------------------- a program

static const char kBinMagic[4] = {'X', 'P', 'B', '1'};

void xGlProgram::set(QOpenGLExtraFunctions* f, const xGlCaps* c, const QByteArray& vtx, const QByteArray& frg) {
	destroy();
	gl = f;
	caps = c;
	src[0] = vtx;
	src[1] = frg;
	cached = false;
	ms = 0;
	vtxLog.clear();
	frgLog.clear();
	linkLog.clear();
	t0 = chainClock().nsecsElapsed();
	QCryptographicHash h(QCryptographicHash::Sha1);
	h.addData(caps->driver);
	h.addData(QByteArray(1, '\0'));
	h.addData(vtx);
	h.addData(QByteArray(1, '\0'));
	h.addData(frg);
	key = h.result().toHex();
	st = Queued;
	if (loadBinary()) {
		st = Ready;
		cached = true;
		ms = msSince(t0);
	}
}

// the program as the driver saved it, if it still takes it: a new driver, another
// GPU or a changed source is another key, and a binary it refuses is thrown out
bool xGlProgram::loadBinary() {
	if (!caps->binary || caps->cacheDir.isEmpty()) return false;
	QFile f(caps->cacheDir + "/" + QString::fromLatin1(key) + ".bin");
	if (!f.open(QFile::ReadOnly)) return false;
	const QByteArray data = f.readAll();
	f.close();
	quint32 fmt = 0;
	quint32 len = 0;
	if ((data.size() < 12) || memcmp(data.constData(), kBinMagic, 4)) return false;
	memcpy(&fmt, data.constData() + 4, 4);
	memcpy(&len, data.constData() + 8, 4);
	if ((len == 0) || ((qint64)len != data.size() - 12)) return false;
	prog = gl->glCreateProgram();
	gl->glProgramBinary(prog, fmt, data.constData() + 12, (GLsizei)len);
	GLint ok = 0;
	gl->glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	while (gl->glGetError() != GL_NO_ERROR) {}		// a refusal may come as an error too
	if (!ok) {
		gl->glDeleteProgram(prog);
		prog = 0;
		f.remove();
		return false;
	}
	return true;
}

void xGlProgram::saveBinary() {
	if (!caps->binary || caps->cacheDir.isEmpty()) return;
	GLint len = 0;
	gl->glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
	if (len <= 0) return;
	QByteArray data(12 + len, '\0');
	GLsizei got = 0;
	GLenum fmt = 0;
	while (gl->glGetError() != GL_NO_ERROR) {}		// an error left by someone else is not this call's
	gl->glGetProgramBinary(prog, len, &got, &fmt, data.data() + 12);
	if ((gl->glGetError() != GL_NO_ERROR) || (got <= 0)) return;
	const quint32 f32 = fmt;
	const quint32 l32 = (quint32)got;
	memcpy(data.data(), kBinMagic, 4);
	memcpy(data.data() + 4, &f32, 4);
	memcpy(data.data() + 8, &l32, 4);
	data.truncate(12 + got);
	QDir().mkpath(caps->cacheDir);
	QSaveFile out(caps->cacheDir + "/" + QString::fromLatin1(key) + ".bin");
	if (!out.open(QFile::WriteOnly)) return;
	out.write(data);
	out.commit();
}

void xGlProgram::build(bool wait) {
	if (st != Queued) return;
	prog = gl->glCreateProgram();
	static const GLenum type[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
	for (int i = 0; i < 2; i++) {
		shd[i] = gl->glCreateShader(type[i]);
		const char* text = src[i].constData();
		const GLint len = src[i].size();
		gl->glShaderSource(shd[i], 1, &text, &len);
		gl->glCompileShader(shd[i]);
		gl->glAttachShader(prog, shd[i]);
	}
	// asked before the link it is to hold for
	if (caps->binary) gl->glProgramParameteri(prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
	gl->glLinkProgram(prog);
	st = Building;
	if (wait || !caps->parallel) finish();
}

xGlProgram::State xGlProgram::poll() {
	if (st != Building) return st;
	GLint done = 0;
	gl->glGetProgramiv(prog, GL_COMPLETION_STATUS_KHR, &done);
	if (done) finish();
	return st;
}

static QString shaderLog(QOpenGLExtraFunctions* gl, GLuint id, bool program) {
	GLint len = 0;
	if (program) gl->glGetProgramiv(id, GL_INFO_LOG_LENGTH, &len);
	else gl->glGetShaderiv(id, GL_INFO_LOG_LENGTH, &len);
	if (len <= 1) return QString();
	QByteArray buf(len, '\0');
	GLsizei got = 0;
	if (program) gl->glGetProgramInfoLog(id, len, &got, buf.data());
	else gl->glGetShaderInfoLog(id, len, &got, buf.data());
	return QString::fromLocal8Bit(buf.constData(), got).trimmed();
}

void xGlProgram::dropShaders() {
	for (int i = 0; i < 2; i++) {
		if (!shd[i]) continue;
		if (prog) gl->glDetachShader(prog, shd[i]);
		gl->glDeleteShader(shd[i]);
		shd[i] = 0;
	}
}

// the link is over: the program is there, or the driver's words are
void xGlProgram::finish() {
	GLint ok = 0;
	gl->glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (ok) {
		st = Ready;
		saveBinary();
		dropShaders();
	} else {
		GLint cs = 0;
		gl->glGetShaderiv(shd[0], GL_COMPILE_STATUS, &cs);
		if (!cs) vtxLog = shaderLog(gl, shd[0], false);
		gl->glGetShaderiv(shd[1], GL_COMPILE_STATUS, &cs);
		if (!cs) frgLog = shaderLog(gl, shd[1], false);
		if (vtxLog.isEmpty() && frgLog.isEmpty()) linkLog = shaderLog(gl, prog, true);
		if (vtxLog.isEmpty() && frgLog.isEmpty() && linkLog.isEmpty()) linkLog = "the program did not link";
		dropShaders();
		gl->glDeleteProgram(prog);
		prog = 0;
		st = Failed;
	}
	ms = msSince(t0);
}

GLint xGlProgram::loc(const char* name) {
	const QByteArray k(name);
	auto it = locs.constFind(k);
	if (it != locs.constEnd()) return it.value();
	const GLint l = prog ? gl->glGetUniformLocation(prog, name) : -1;
	locs.insert(k, l);
	return l;
}

void xGlProgram::forget() {
	prog = 0;
	shd[0] = shd[1] = 0;
	locs.clear();
	st = Empty;
}

void xGlProgram::destroy() {
	if (gl && QOpenGLContext::currentContext()) {
		dropShaders();
		if (prog) gl->glDeleteProgram(prog);
	}
	prog = 0;
	shd[0] = shd[1] = 0;
	locs.clear();
	st = Empty;
}

// ---------------------------------------------------------------- the chain

namespace {

enum {
	// samplers
	K_SOURCE = 0, K_ORIGINAL, K_HISTORY, K_PASSOUT, K_FEEDBACK, K_TEXTURE,
	K_SAMPLERS,
	// their sizes, in the same order
	K_SOURCE_SIZE = K_SAMPLERS, K_ORIGINAL_SIZE, K_HISTORY_SIZE, K_PASSOUT_SIZE, K_FEEDBACK_SIZE, K_TEXTURE_SIZE,
	K_OUTPUT_SIZE, K_FINAL_SIZE,
	K_FRAMECOUNT, K_DIRECTION, K_SUBFRAME, K_SUBFRAMES,
	K_MVP,
	K_RUBY_INPUT, K_RUBY_TEXTURE, K_RUBY_OUTPUT, K_SMOOTH, K_PLACE, K_WIDE,
	K_XP_PICTURE, K_XP_WINDOW, K_XP_RASTER, K_XP_CARRIER, K_XP_FRAME,
	K_PARAM
};

struct FixedName {
	const char* name;
	int kind;
};

const FixedName kFixed[] = {
	{"Source", K_SOURCE}, {"rubyTexture", K_SOURCE}, {"u_tex", K_SOURCE},
	{"SourceSize", K_SOURCE_SIZE},
	{"Original", K_ORIGINAL}, {"OriginalSize", K_ORIGINAL_SIZE},
	{"OutputSize", K_OUTPUT_SIZE}, {"FinalViewportSize", K_FINAL_SIZE},
	{"FrameCount", K_FRAMECOUNT}, {"FrameDirection", K_DIRECTION},
	{"CurrentSubFrame", K_SUBFRAME}, {"TotalSubFrames", K_SUBFRAMES},
	{"MVP", K_MVP}, {"u_mvp_", K_MVP},
	{"rubyInputSize", K_RUBY_INPUT}, {"rubyTextureSize", K_RUBY_TEXTURE}, {"rubyOutputSize", K_RUBY_OUTPUT},
	{"u_smooth", K_SMOOTH}, {"u_place_", K_PLACE}, {"xpWide", K_WIDE},
	{"xpPicture", K_XP_PICTURE}, {"xpWindow", K_XP_WINDOW}, {"xpRaster", K_XP_RASTER},
	{"xpCarrier", K_XP_CARRIER}, {"xpFrame", K_XP_FRAME},
};

// "PassOutput3" or "PassOutput3Size" under the prefix "PassOutput": the number, and whether a size
bool numbered(const QString& name, const char* prefix, int* index, bool* size) {
	const QLatin1String pre(prefix);
	if (!name.startsWith(pre)) return false;
	QString rest = name.mid(pre.size());
	*size = rest.endsWith(QLatin1String("Size"));
	if (*size) rest.chop(4);
	bool ok = false;
	*index = rest.toInt(&ok);
	return ok && (*index >= 0);
}

// the GLSL type a name of that kind must have to be fed; 0: any
GLenum kindType(int kind) {
	if (kind < K_SAMPLERS) return GL_SAMPLER_2D;
	switch (kind) {
		case K_SOURCE_SIZE: case K_ORIGINAL_SIZE: case K_HISTORY_SIZE: case K_PASSOUT_SIZE:
		case K_FEEDBACK_SIZE: case K_TEXTURE_SIZE: case K_OUTPUT_SIZE: case K_FINAL_SIZE:
		case K_PLACE: case K_XP_PICTURE: case K_XP_RASTER: case K_XP_CARRIER:
			return GL_FLOAT_VEC4;
		case K_RUBY_INPUT: case K_RUBY_TEXTURE: case K_RUBY_OUTPUT: case K_XP_WINDOW:
			return GL_FLOAT_VEC2;
		case K_FRAMECOUNT: case K_SUBFRAME: case K_SUBFRAMES:
			return GL_UNSIGNED_INT;
		case K_DIRECTION: case K_SMOOTH: case K_XP_FRAME:
			return GL_INT;
		case K_MVP:
			return GL_FLOAT_MAT4;
		case K_WIDE: case K_PARAM:
			return GL_FLOAT;
	}
	return 0;
}

void size4(QOpenGLExtraFunctions* gl, GLint loc, const QSize& s) {
	const float w = float(std::max(s.width(), 1));
	const float h = float(std::max(s.height(), 1));
	gl->glUniform4f(loc, w, h, 1.0f / w, 1.0f / h);
}

const char* formatName(GLenum fmt) {
	return (fmt == GL_RGBA16F) ? "rgba16f" : (fmt == GL_SRGB8_ALPHA8) ? "srgb8" : "rgba8";
}

} // anonymous namespace

bool xShaderChain::init(const QString& cacheDir) {
	// Qt makes the widget's context again when its window moves to another GPU: what
	// the old one held went with it, and its names may be the new one's by now
	if (vao) {
		for (Pass& p : passes) {
			p.prg.forget();
			p.tgt[0] = p.tgt[1] = Target();
		}
		passes.clear();
		textures.clear();
		preset = xShdPreset();
		none.forget();
		for (Seg& s : segs) s = Seg();
		black = vao = vbo = 0;
		bad = live = false;
	}
	caps = xGlCaps();
	initializeOpenGLFunctions();
	QOpenGLContext* ctx = QOpenGLContext::currentContext();
	const QSurfaceFormat f = ctx->format();
	caps.es = ctx->isOpenGLES();
	caps.version = f.majorVersion() * 10 + f.minorVersion();
	for (GLenum what : {GL_VENDOR, GL_RENDERER, GL_VERSION}) {
		const char* s = (const char*)glGetString(what);
		caps.driver.append(s ? s : "?").append('\n');
	}
	const bool binApi = caps.es ? (caps.version >= 30)
		: ((caps.version >= 41) || ctx->hasExtension("GL_ARB_get_program_binary"));
	GLint formats = 0;
	if (binApi) glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
	caps.binary = binApi && (formats > 0);
	caps.parallel = ctx->hasExtension("GL_KHR_parallel_shader_compile") || ctx->hasExtension("GL_ARB_parallel_shader_compile");
	caps.timer = caps.es ? ctx->hasExtension("GL_EXT_disjoint_timer_query")
		: ((caps.version >= 33) || ctx->hasExtension("GL_ARB_timer_query"));
	caps.floatTarget = !caps.es || ctx->hasExtension("GL_EXT_color_buffer_float") || ctx->hasExtension("GL_EXT_color_buffer_half_float");
	caps.border = !caps.es || (caps.version >= 32) || ctx->hasExtension("GL_EXT_texture_border_clamp")
		|| ctx->hasExtension("GL_OES_texture_border_clamp");
	caps.cacheDir = caps.binary ? cacheDir : QString();
	if (!caps.cacheDir.isEmpty()) {
		// the oldest go when there are too many: every edit of a shader leaves one behind
		QDir dir(caps.cacheDir);
		const QFileInfoList old = dir.entryInfoList(QStringList() << "*.bin", QDir::Files, QDir::Time | QDir::Reversed);
		for (int i = 0; i < old.size() - 200; i++)
			QFile::remove(old[i].absoluteFilePath());
	}
	xlog(XLG_GL, XLL_INFO, "programs: cache %s (%d binary formats), parallel compile %s, timer %s, float targets %s",
		caps.cacheDir.isEmpty() ? "none" : caps.cacheDir.toLocal8Bit().constData(), (int)formats,
		caps.parallel ? "yes" : "no", caps.timer ? "yes" : "no", caps.floatTarget ? "yes" : "no");
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void*>(0));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void*>(2 * sizeof(GLfloat)));
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	glGenTextures(1, &black);
	glBindTexture(GL_TEXTURE_2D, black);
	const unsigned char zero[4] = {0, 0, 0, 0};
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, 0);
	memset(samplers, 0, sizeof(samplers));
	builtin(none, placeVertexShader(QString::fromLatin1(xshd_compose(defaultVertexBody()))).toLatin1(),
		xshd_compose(defaultFragmentBody()));
	if (none.ready()) makeBinds(none, noneBinds);
	return none.ready();
}

void xShaderChain::builtin(xGlProgram& p, const QByteArray& vtx, const QByteArray& frg) {
	p.set(this, &caps, vtx, frg);
	if (!p.ready()) p.build(true);
	if (!p.ready())
		xlog(XLG_GL, XLL_ERROR, "a built-in program failed: %s %s %s", p.vtxLog.toLocal8Bit().constData(),
			p.frgLog.toLocal8Bit().constData(), p.linkLog.toLocal8Bit().constData());
}

void xShaderChain::clear() {
	for (Pass& p : passes) {
		p.prg.destroy();
		for (Target& t : p.tgt) {
			if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
			if (t.tex) glDeleteTextures(1, &t.tex);
		}
	}
	passes.clear();
	for (Tex& t : textures)
		if (t.tex && (t.tex != black)) glDeleteTextures(1, &t.tex);
	textures.clear();
	// the passes' times are another chain's from here on
	for (int i = 0; i < XSHD_PASS_MAX; i++) {
		segs[i].sum = 0;
		segs[i].n = 0;
		segs[i].used = false;
	}
	preset = xShdPreset();
	bad = false;
	live = false;
	told = false;
	errText.clear();
	histDepth = 0;
	lastTex = 0;
	useChain = false;
}

void xShaderChain::destroy() {
	clear();
	none.destroy();
	for (auto& a : samplers)
		for (auto& b : a)
			for (GLuint& s : b)
				if (s) {
					glDeleteSamplers(1, &s);
					s = 0;
				}
	for (Seg& s : segs) {
		if (s.q[0]) glDeleteQueries(2, s.q);
		s = Seg();
	}
	if (black) glDeleteTextures(1, &black);
	if (vbo) glDeleteBuffers(1, &vbo);
	if (vao) glDeleteVertexArrays(1, &vao);
	black = vbo = vao = 0;
}

float xShaderChain::paramValue(const QString& name) const {
	for (const xShdParam& p : preset.params)
		if (p.name == name) return p.value;
	return 1.0f;
}

bool xShaderChain::passOn(int i) const {
	const QString& w = preset.passes[i].when;
	return w.isEmpty() || (paramValue(w) != 0.0f);
}

void xShaderChain::refreshOn() {
	for (int i = 0; i < (int)passes.size(); i++)
		passes[i].on = passOn(i);
}

bool xShaderChain::setParam(const QString& name, float value) {
	for (xShdParam& p : preset.params) {
		if (p.name != name) continue;
		p.value = std::min(std::max(value, p.min), p.max);
		refreshOn();		// a pass that came on is built by the next steps
		return true;
	}
	return false;
}

// what stands in front of every stage of a new-format pass: the version, the names
// the engine feeds, the parameters. Names nobody uses cost nothing.
QByteArray xShaderChain::preamble(int i, bool vertex) {
	QString s = caps.es
		? "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
		: "#version 330\n";
	auto tex = [&s](const QString& name) {
		s += QString("uniform sampler2D %1;\nuniform vec4 %1Size;\n").arg(name);
	};
	s += "uniform mat4 MVP;\n";
	tex("Source");
	tex("Original");
	for (int n = 0; n <= XSHD_HISTORY_MAX; n++)
		tex(QString("OriginalHistory%1").arg(n));
	const int n = preset.passes.size();
	for (int k = 0; k < n - 1; k++) {
		const QString& alias = preset.passes[k].name;
		if (k < i) {				// causal: only the passes before this one
			tex(QString("PassOutput%1").arg(k));
			if (!alias.isEmpty()) tex(alias);
		}
		tex(QString("PassFeedback%1").arg(k));
		if (!alias.isEmpty()) tex(alias + "Feedback");
	}
	for (const xShdTexture& t : preset.textures)
		tex(t.name);
	s += "uniform vec4 OutputSize;\nuniform vec4 FinalViewportSize;\n"
		"uniform uint FrameCount;\nuniform int FrameDirection;\n"
		"uniform uint CurrentSubFrame;\nuniform uint TotalSubFrames;\n"
		"uniform vec4 xpPicture;\nuniform vec2 xpWindow;\nuniform vec4 xpRaster;\n"
		"uniform vec4 xpCarrier;\nuniform int xpFrame;\nuniform float xpWide;\n";
	for (const xShdParam& p : preset.params)
		s += QString("uniform float %1;\n").arg(p.name);
	if (vertex)
		s += "layout(location=0) in vec4 Position;\nlayout(location=1) in vec2 TexCoord;\n";
	return s.toLatin1();
}

void xShaderChain::sources(int i, QByteArray& vtx, QByteArray& frg) {
	const xShdPass& sp = preset.passes[i];
	const bool last = (i == preset.passes.size() - 1);
	QString v;
	QString f;
	if (preset.legacy) {
		const bool shim = sourceNeedsLegacyShim(sp.vtx) || sourceNeedsLegacyShim(sp.frg);
		v = legacyStage(sp.vtx, sp.vtxLine, shim, vertexRewrites(), vertexShimPreamble(caps.es), caps.es);
		f = legacyStage(sp.frg, sp.frgLine, shim, fragmentRewrites(), fragmentShimPreamble(caps.es), caps.es);
	} else {
		QString common;
		if (!preset.common.isEmpty())
			common = QString("#line %1\n").arg(preset.commonLine) + preset.common;
		v = QString::fromLatin1(preamble(i, true)) + common;
		if (sp.vtx.trimmed().isEmpty()) {
			v += "out vec2 vTexCoord;\n"
				"void main() {\n"
				"    gl_Position = MVP * Position;\n"
				"    vTexCoord = TexCoord;\n"
				"}\n";
		} else {
			v += QString("#line %1\n").arg(sp.vtxLine) + sp.vtx;
		}
		f = QString::fromLatin1(preamble(i, false)) + common + QString("#line %1\n").arg(sp.frgLine) + sp.frg;
	}
	if (last) v = placeVertexShader(std::move(v));
	vtx = v.toUtf8();
	frg = f.toUtf8();
}

void xShaderChain::startPass(int i) {
	Pass& p = passes[i];
	QByteArray vtx;
	QByteArray frg;
	sources(i, vtx, frg);
	p.prg.set(this, &caps, vtx, frg);
	p.bound = false;
	if (!p.prg.ready() && caps.parallel) p.prg.build(false);	// the driver's threads take it from here
	if (p.prg.ready()) {
		makeBinds(p.prg, p.binds);
		p.bound = true;
	}
}

void xShaderChain::load(const QString& path, const xShdValues& values) {
	clear();
	loadAt = chainClock().nsecsElapsed();
	if (path.isEmpty()) return;
	QString err;
	if (!xshd_parse(path, preset, &err)) {
		const QString name = preset.file;
		preset = xShdPreset();
		preset.file = name;
		preset.path = path;
		bad = true;
		errText = err;
		xlog(XLG_GL, XLL_ERROR, "shader %s", err.toLocal8Bit().constData());
		return;
	}
	for (const QString& n : preset.notes)
		xlog(XLG_GL, XLL_WARN, "shader %s", n.toLocal8Bit().constData());
	// before anything is built: a pass's `when` reads them
	for (const xShdValue& v : values) {
		for (xShdParam& p : preset.params)
			if (p.name == v.name) p.value = std::min(std::max(v.value, p.min), p.max);
	}
	passes.resize(preset.passes.size());
	const QString dir = QFileInfo(path).absolutePath();
	for (const xShdTexture& t : preset.textures) {
		Tex tx;
		QImage img(dir + "/" + t.file);
		if (img.isNull()) {
			const QString n = QString("%1:%2: texture %3 cannot be read").arg(preset.file).arg(t.line).arg(t.file);
			preset.notes << n;
			xlog(XLG_GL, XLL_WARN, "shader %s", n.toLocal8Bit().constData());
			tx.tex = black;
			tx.size = QSize(1, 1);
		} else {
			img = img.convertToFormat(QImage::Format_RGBA8888);
			glGenTextures(1, &tx.tex);
			glBindTexture(GL_TEXTURE_2D, tx.tex);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			if (t.mipmap) glGenerateMipmap(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, 0);
			tx.size = img.size();
		}
		tx.sampler = sampler(t.linear, t.wrap, t.mipmap && (tx.tex != black));
		textures.push_back(tx);
	}
	refreshOn();
	for (int i = 0; i < (int)passes.size(); i++)
		if (passes[i].on) startPass(i);
	checkLive();
}

void xShaderChain::step() {
	// the log's GL group at DEBUG asks for the times as the control port does
	if (xlog_on(XLG_GL, XLL_DEBUG) && caps.timer) {
		if (!timed) timing(true);
		if (++logTick >= 250) {
			logTick = 0;
			xlog(XLG_GL, XLL_DEBUG, "gpu time: %s", timeReport().trimmed().replace("\r\n", "; ").toLocal8Bit().constData());
		}
	}
	collectTimes();
	if (!loaded() || bad) return;
	bool built = false;
	for (int i = 0; i < (int)passes.size(); i++) {
		Pass& p = passes[i];
		if (!p.on) continue;
		if (p.prg.state() == xGlProgram::Empty) startPass(i);
		if (p.prg.state() == xGlProgram::Building) p.prg.poll();
		// no threads of the driver's: one program a paint, the picture goes on meanwhile
		if ((p.prg.state() == xGlProgram::Queued) && !built) {
			p.prg.build(true);
			built = true;
		}
		if (p.prg.ready() && !p.bound) {
			makeBinds(p.prg, p.binds);
			p.bound = true;
		}
		if (p.prg.state() == xGlProgram::Failed) {
			fail(i);
			return;
		}
	}
	checkLive();
}

void xShaderChain::checkLive() {
	if (bad || !loaded()) return;
	if (!live) {
		for (const Pass& p : passes)
			if (p.on && !(p.prg.ready() && p.bound)) return;
		live = true;
	}
	if (told) return;
	told = true;
	int fromCache = 0;
	int compiled = 0;
	double ms = 0;
	for (const Pass& p : passes) {
		if (!p.prg.ready()) continue;
		if (p.prg.cached) fromCache++; else compiled++;
		ms += p.prg.ms;
	}
	xlog(XLG_GL, XLL_INFO, "shader %s: %d pass(es), %d from the cache, %d compiled (%.1f ms), ready %.1f ms after the load",
		preset.file.toLocal8Bit().constData(), (int)passes.size(), fromCache, compiled, ms, msSince(loadAt));
}

QString xShaderChain::where(int i, bool vertex) const {
	const QString stage = vertex ? "vertex" : "fragment";
	if (preset.legacy) return stage;
	const QString& name = preset.passes[i].name;
	return name.isEmpty() ? QString("pass %1, %2").arg(i).arg(stage) : QString("pass %1 \"%2\", %3").arg(i).arg(name, stage);
}

// The driver's log, a line an error: "file:line: text (pass, stage)" where its line
// number can be read (#line made it the file's), as it came otherwise.
void xShaderChain::logErrors(int i, const QString& log, bool vertex) {
	static const QRegularExpression forms[] = {
		QRegularExpression(QStringLiteral(R"(^\d+\((\d+)\)\s*:\s*(.*)$)")),		// NVIDIA: 0(12) : error C0000: ...
		QRegularExpression(QStringLiteral(R"(^\d+:(\d+)\(\d+\):\s*(.*)$)")),		// Mesa, Intel: 0:12(3): error: ...
		QRegularExpression(QStringLiteral(R"(^(?:ERROR|WARNING):\s*\d+:(\d+):\s*(.*)$)")),	// AMD, Apple, ANGLE
	};
	for (const QString& raw : log.split('\n', Qt::SkipEmptyParts)) {
		const QString lin = raw.trimmed();
		if (lin.isEmpty()) continue;
		QString out;
		for (const QRegularExpression& re : forms) {
			const QRegularExpressionMatch m = re.match(lin);
			if (!m.hasMatch()) continue;
			// "error C1503: " and "error: " say nothing the line's level does not
			static const QRegularExpression lead(QStringLiteral(R"(^error(?:\s+\w+)?:\s*)"));
			QString text = m.captured(2);
			text.remove(lead);
			out = QString("%1:%2: %3 (%4)").arg(preset.file, m.captured(1), text, where(i, vertex));
			break;
		}
		if (out.isEmpty()) out = QString("%1: %2 (%3)").arg(preset.file, lin, where(i, vertex));
		xlog(XLG_GL, XLL_ERROR, "shader %s", out.toLocal8Bit().constData());
		if (!errText.isEmpty()) errText += '\n';
		errText += out;
	}
}

void xShaderChain::fail(int i) {
	Pass& p = passes[i];
	bad = true;
	live = false;
	logErrors(i, p.prg.vtxLog, true);
	logErrors(i, p.prg.frgLog, false);
	if (!p.prg.linkLog.isEmpty()) {
		const QString out = QString("%1: %2 (%3)").arg(preset.file, p.prg.linkLog.simplified(),
			preset.legacy ? QString("link") : QString("pass %1, link").arg(i));
		xlog(XLG_GL, XLL_ERROR, "shader %s", out.toLocal8Bit().constData());
		if (!errText.isEmpty()) errText += '\n';
		errText += out;
	}
}

// Every active uniform of a ready program, sorted out once by its name: what feeds
// it, and for a sampler the unit it reads. What a pass does not name is not there,
// so it is neither set nor kept: a history frame, a feedback copy exist by use.
void xShaderChain::makeBinds(xGlProgram& p, std::vector<Bind>& binds) {
	binds.clear();
	GLint count = 0;
	glGetProgramiv(p.id(), GL_ACTIVE_UNIFORMS, &count);
	int unit = 0;
	glUseProgram(p.id());
	for (int u = 0; u < count; u++) {
		char raw[256];
		GLsizei len = 0;
		GLint size = 0;
		GLenum type = 0;
		glGetActiveUniform(p.id(), u, sizeof(raw), &len, &size, &type, raw);
		QString name = QString::fromLatin1(raw, len);
		if (name.endsWith("[0]")) name.chop(3);
		Bind b = {-1, 0, -1, 0, false};
		for (const FixedName& fx : kFixed) {
			if (name == QLatin1String(fx.name)) {
				b.kind = fx.kind;
				break;
			}
		}
		bool isSize = false;
		int idx = 0;
		if (b.kind < 0) {
			if (numbered(name, "OriginalHistory", &idx, &isSize) && (idx <= XSHD_HISTORY_MAX)) {
				b.kind = isSize ? K_HISTORY_SIZE : K_HISTORY;
				b.index = idx;
			} else if (numbered(name, "PassOutput", &idx, &isSize)) {
				b.kind = isSize ? K_PASSOUT_SIZE : K_PASSOUT;
				b.index = idx;
			} else if (numbered(name, "PassFeedback", &idx, &isSize)) {
				b.kind = isSize ? K_FEEDBACK_SIZE : K_FEEDBACK;
				b.index = idx;
			}
		}
		for (int k = 0; (b.kind < 0) && (k < preset.passes.size()); k++) {
			const QString& alias = preset.passes[k].name;
			if (alias.isEmpty()) continue;
			if (name == alias) b.kind = K_PASSOUT;
			else if (name == alias + "Size") b.kind = K_PASSOUT_SIZE;
			else if (name == alias + "Feedback") b.kind = K_FEEDBACK;
			else if (name == alias + "FeedbackSize") b.kind = K_FEEDBACK_SIZE;
			b.index = k;
		}
		for (int k = 0; (b.kind < 0) && (k < preset.textures.size()); k++) {
			if (name == preset.textures[k].name) b.kind = K_TEXTURE;
			else if (name == preset.textures[k].name + "Size") b.kind = K_TEXTURE_SIZE;
			b.index = k;
		}
		for (int k = 0; (b.kind < 0) && (k < preset.params.size()); k++) {
			if (name == preset.params[k].name) b.kind = K_PARAM;
			b.index = k;
		}
		// an old file may call its one texture anything: main drew with the frame on
		// unit 0, where a sampler nobody set reads
		if ((b.kind < 0) && (type == GL_SAMPLER_2D) && preset.legacy) b.kind = K_SOURCE;
		if (b.kind < 0) continue;
		// a name of ours declared as another type is the shader's own: left alone
		b.asInt = (b.kind == K_FRAMECOUNT) && (type == GL_INT);
		if (!b.asInt && (type != kindType(b.kind))) continue;
		b.loc = glGetUniformLocation(p.id(), raw);
		if (b.loc < 0) continue;
		if (b.kind < K_SAMPLERS) {
			b.unit = unit++;
			glUniform1i(b.loc, b.unit);
		}
		if (((b.kind == K_FEEDBACK) || (b.kind == K_FEEDBACK_SIZE)) && (b.index < (int)passes.size() - 1))
			passes[b.index].feedback = true;
		if ((b.kind == K_HISTORY) || (b.kind == K_HISTORY_SIZE))
			histDepth = std::max(histDepth, b.index);
		binds.push_back(b);
	}
	glUseProgram(0);
}

GLuint xShaderChain::sampler(bool linear, int wrap, bool mip) {
	GLuint& s = samplers[linear ? 1 : 0][wrap][mip ? 1 : 0];
	if (s) return s;
	glGenSamplers(1, &s);
	const GLint mag = linear ? GL_LINEAR : GL_NEAREST;
	const GLint min = mip ? (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST) : mag;
	GLint w = GL_CLAMP_TO_EDGE;
	switch (wrap) {
		case XSHD_WRAP_BORDER: w = caps.border ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE; break;
		case XSHD_WRAP_REPEAT: w = GL_REPEAT; break;
		case XSHD_WRAP_MIRROR: w = GL_MIRRORED_REPEAT; break;
	}
	glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag);
	glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, min);
	glSamplerParameteri(s, GL_TEXTURE_WRAP_S, w);
	glSamplerParameteri(s, GL_TEXTURE_WRAP_T, w);
	return s;
}

// A pass's texture to draw into, made or made again when its size or format
// changes, and cleared then: a feedback not drawn yet reads as transparent black.
bool xShaderChain::target(Pass& p, int idx, const QSize& size, GLenum fmt) {
	Target& t = p.tgt[idx];
	if (t.tex && (t.size == size) && (t.fmt == fmt)) return true;
	if (!t.tex) glGenTextures(1, &t.tex);
	if (!t.fbo) glGenFramebuffers(1, &t.fbo);
	glBindTexture(GL_TEXTURE_2D, t.tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	for (int attempt = 0; attempt < 2; attempt++) {
		const GLenum type = (fmt == GL_RGBA16F) ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
		glTexImage2D(GL_TEXTURE_2D, 0, fmt, size.width(), size.height(), 0, GL_RGBA, type, nullptr);
		glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
			t.size = size;
			t.fmt = fmt;
			glViewport(0, 0, size.width(), size.height());
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT);
			return true;
		}
		if (fmt == GL_RGBA8) break;
		xlog(XLG_GL, XLL_WARN, "shader %s: a %s target cannot be drawn into here, rgba8 instead",
			preset.file.toLocal8Bit().constData(), formatName(fmt));
		p.fmt = fmt = GL_RGBA8;
	}
	t.size = QSize();
	return false;
}

void xShaderChain::unbind(const std::vector<Bind>& binds) {
	for (const Bind& b : binds) {
		if (b.kind >= K_SAMPLERS) continue;
		glBindSampler(b.unit, 0);
		if (b.unit) {
			glActiveTexture(GL_TEXTURE0 + b.unit);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
	}
	glActiveTexture(GL_TEXTURE0);
}

// The uniforms of a program for one draw. legacyTex: the textures are read with their
// own filter and wrap, as an old file and the built-in program always read the frame;
// otherwise through the pass's sampler.
void xShaderChain::apply(const std::vector<Bind>& binds, int pass, const xChainFrame& frm,
		GLuint srcTex, const QSize& srcSize, const QSize& outSize, const float* mvp,
		const float place[4], bool wide, bool legacyTex) {
	const xShdPass* sp = (pass >= 0) ? &preset.passes[pass] : nullptr;
	const bool linear = sp && sp->linear;
	const int wrap = (sp && !wide) ? sp->wrap : XSHD_WRAP_CLAMP;
	const xFrameNums& m = frm.cur.nums;
	for (const Bind& b : binds) {
		GLuint tex = 0;
		QSize size;
		GLuint smp = 0;
		bool isTex = true;
		switch (b.kind) {
			case K_SOURCE: case K_SOURCE_SIZE:
				tex = srcTex;
				size = srcSize;
				if (!legacyTex) smp = sampler(linear, wrap, sp && sp->mipmap);
				break;
			case K_ORIGINAL: case K_ORIGINAL_SIZE:
				tex = frm.cur.tex;
				size = frm.cur.size;
				if (!legacyTex) smp = sampler(linear, wrap, false);
				break;
			case K_HISTORY: case K_HISTORY_SIZE: {
				const xFrameTex& h = b.index ? frm.hist[b.index] : frm.cur;
				tex = h.tex ? h.tex : black;
				size = h.tex ? h.size : frm.cur.size;
				if (!legacyTex) smp = sampler(linear, wrap, false);
				break;
			}
			case K_PASSOUT: case K_PASSOUT_SIZE: {
				const bool ok = (b.index < (int)passes.size()) && (pass > b.index) && passes[b.index].outTex;
				tex = ok ? passes[b.index].outTex : black;
				size = ok ? passes[b.index].outSize : srcSize;
				if (!legacyTex) smp = sampler(linear, wrap, false);
				break;
			}
			case K_FEEDBACK: case K_FEEDBACK_SIZE: {
				const Pass* q = (b.index < (int)passes.size()) ? &passes[b.index] : nullptr;
				// a pass later in the chain has not drawn yet this paint: its last output
				// is still the current one
				const Target* t = (q && q->feedback) ? &q->tgt[q->flipped ? 1 - q->cur : q->cur] : nullptr;
				tex = (t && t->tex) ? t->tex : black;
				size = (t && t->tex) ? t->size : srcSize;
				if (!legacyTex) smp = sampler(linear, wrap, false);
				break;
			}
			case K_TEXTURE: case K_TEXTURE_SIZE:
				tex = textures[b.index].tex;
				size = textures[b.index].size;
				smp = textures[b.index].sampler;
				break;
			default:
				isTex = false;
				break;
		}
		if (isTex) {
			if (b.kind < K_SAMPLERS) {
				glActiveTexture(GL_TEXTURE0 + b.unit);
				glBindTexture(GL_TEXTURE_2D, tex);
				glBindSampler(b.unit, smp);
			} else {
				size4(this, b.loc, size);
			}
			continue;
		}
		switch (b.kind) {
			case K_OUTPUT_SIZE: size4(this, b.loc, outSize); break;
			case K_FINAL_SIZE: size4(this, b.loc, frm.pic.size()); break;
			case K_FRAMECOUNT:
				if (b.asInt) glUniform1i(b.loc, int(frameCount & 0x7fffffff));
				else glUniform1ui(b.loc, frameCount);
				break;
			case K_DIRECTION: glUniform1i(b.loc, frm.direction); break;
			case K_SUBFRAME: glUniform1ui(b.loc, 1); break;
			case K_SUBFRAMES: glUniform1ui(b.loc, 1); break;
			case K_MVP: glUniformMatrix4fv(b.loc, 1, GL_FALSE, mvp); break;
			case K_RUBY_INPUT:
			case K_RUBY_TEXTURE: glUniform2f(b.loc, GLfloat(srcSize.width()), GLfloat(srcSize.height())); break;
			case K_RUBY_OUTPUT: glUniform2f(b.loc, GLfloat(outSize.width()), GLfloat(outSize.height())); break;
			case K_SMOOTH: {
				// the built-in one: smooth when a dot is not a whole number of pixels either way
				double ppx = outSize.width() / double(std::max(srcSize.width(), 1));
				double ppy = outSize.height() / double(std::max(srcSize.height(), 1));
				if ((srcTex == frm.cur.tex) && !frm.dots.isEmpty()) {		// two texels a dot across
					ppx = outSize.width() / double(frm.dots.width());
					ppy = outSize.height() / double(frm.dots.height());
				}
				const bool whole = (std::fabs(ppx - std::floor(ppx + 0.5)) < 0.001) && (std::fabs(ppy - std::floor(ppy + 0.5)) < 0.001);
				glUniform1i(b.loc, whole ? 0 : 1);
				break;
			}
			case K_PLACE: glUniform4f(b.loc, place[0], place[1], place[2], place[3]); break;
			// the bundled shaders leave their corners and vignette off then, or the bars get a seam
			case K_WIDE: glUniform1f(b.loc, wide ? 1.0f : 0.0f); break;
			case K_XP_PICTURE:
				glUniform4f(b.loc, GLfloat(frm.pic.x()), GLfloat(frm.pic.y()), GLfloat(frm.pic.width()), GLfloat(frm.pic.height()));
				break;
			case K_XP_WINDOW: glUniform2f(b.loc, GLfloat(frm.view.width()), GLfloat(frm.view.height())); break;
			case K_XP_RASTER: glUniform4f(b.loc, GLfloat(m.fullX), GLfloat(m.fullY), GLfloat(m.cutX), GLfloat(m.cutY)); break;
			case K_XP_CARRIER: {
				// a free carrier's phase at the frame's first dot, from the frame's number: in
				// doubles here, a float in the shader would lose it within minutes
				double phase = 0.0;
				if (!m.locked) {
					const double perFrame = m.carrier * m.fullX * m.fullY;
					const double turn = (perFrame - std::floor(perFrame)) * m.frame;
					phase = turn - std::floor(turn);
				}
				// the V switch turns every line and does not start again with a frame
				const int vsense = int(((long long)m.frame * m.fullY) & 1);
				glUniform4f(b.loc, GLfloat(m.carrier), m.locked ? 1.0f : 0.0f, GLfloat(phase), GLfloat(vsense));
				break;
			}
			case K_XP_FRAME: glUniform1i(b.loc, m.frame); break;
			case K_PARAM: glUniform1f(b.loc, preset.params[b.index].value); break;
		}
	}
}

// The passes before the last, each into its own texture. A pass that is off, or not
// built yet, hands its Source on, so the chain with a stage off is the chain without it.
void xShaderChain::run(const xChainFrame& frm, bool use) {
	useChain = use && live && frm.cur.tex;
	lastTex = frm.cur.tex;
	lastSize = frm.cur.size;
	for (Pass& p : passes) {
		p.outTex = 0;
		p.ran = false;
		p.flipped = false;
	}
	if (!useChain) return;
	frameCount++;
	const int n = (int)passes.size();
	if (n < 2) return;
	GLint wasFbo = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &wasFbo);
	GLint wasView[4];
	glGetIntegerv(GL_VIEWPORT, wasView);
	const GLboolean wasScissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	static const float placeNone[4] = {1.0f, 1.0f, 0.0f, 0.0f};
	GLuint src = frm.cur.tex;
	QSize srcSize = frm.cur.size;
	bool srgbOn = false;
	for (int i = 0; i < n - 1; i++) {
		Pass& p = passes[i];
		const xShdPass& sp = preset.passes[i];
		p.outTex = src;
		p.outSize = srcSize;
		if (!p.on) {
			// nothing is kept for a pass that is off: its textures go too
			for (Target& t : p.tgt) {
				if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
				if (t.tex) glDeleteTextures(1, &t.tex);
				t = Target();
			}
			continue;
		}
		if (!p.prg.ready() || !p.bound) continue;
		QSize size;
		for (int ax = 0; ax < 2; ax++) {
			const int from = ax ? srcSize.height() : srcSize.width();
			const int view = ax ? frm.pic.height() : frm.pic.width();
			double v = sp.scale[ax];
			if (sp.scaleType[ax] == XSHD_SCALE_SOURCE) v *= from;
			else if (sp.scaleType[ax] == XSHD_SCALE_VIEWPORT) v *= view;
			const int px = std::min(std::max(int(v + 0.5), 1), 16384);
			if (ax) size.setHeight(px); else size.setWidth(px);
		}
		if (!p.fmt) {
			p.fmt = (sp.format == XSHD_FMT_SRGB8) ? GL_SRGB8_ALPHA8 : (sp.format == XSHD_FMT_RGBA16F) ? GL_RGBA16F : GL_RGBA8;
			if ((p.fmt == GL_RGBA16F) && !caps.floatTarget) {
				xlog(XLG_GL, XLL_WARN, "shader %s: no float targets here, pass %d draws into rgba8",
					preset.file.toLocal8Bit().constData(), i);
				p.fmt = GL_RGBA8;
			}
		}
		// last paint's output stays in the other texture for those who read it back
		const bool hadBoth = p.feedback && p.tgt[0].tex && p.tgt[1].tex;
		const int next = hadBoth ? 1 - p.cur : p.cur;
		if (!target(p, next, size, p.fmt)) continue;
		if (p.feedback && !target(p, 1 - next, size, p.fmt)) continue;
		p.cur = next;
		p.flipped = true;
		segBegin(i);
		if (sp.mipmap) {
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, src);
			glGenerateMipmap(GL_TEXTURE_2D);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, p.tgt[p.cur].fbo);
		glViewport(0, 0, size.width(), size.height());
		// desktop GL encodes into an sRGB target only when told to; ES always does
		const bool srgb = (p.fmt == GL_SRGB8_ALPHA8) && !caps.es;
		if (srgb != srgbOn) {
			if (srgb) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
			srgbOn = srgb;
		}
		glUseProgram(p.prg.id());
		apply(p.binds, i, frm, src, srcSize, size, kMvpTarget, placeNone, false, false);
		glBindVertexArray(vao);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindVertexArray(0);
		unbind(p.binds);
		segEnd();
		p.ran = true;
		p.outTex = p.tgt[p.cur].tex;
		p.outSize = size;
		src = p.outTex;
		srcSize = size;
	}
	if (srgbOn) glDisable(GL_FRAMEBUFFER_SRGB);
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, wasFbo);
	glViewport(wasView[0], wasView[1], wasView[2], wasView[3]);
	if (wasScissor) glEnable(GL_SCISSOR_TEST);
	lastTex = src;
	lastSize = srcSize;
}

void xShaderChain::drawLast(const xChainFrame& frm, const float place[4], bool wide, const std::function<void()>& quad) {
	const int n = (int)passes.size();
	Pass* lp = (useChain && n) ? &passes[n - 1] : nullptr;
	const bool chainLast = lp && lp->on && lp->prg.ready() && lp->bound;
	xGlProgram& p = chainLast ? lp->prg : none;
	const std::vector<Bind>& binds = chainLast ? lp->binds : noneBinds;
	if (!p.ready()) return;
	const GLuint src = useChain ? lastTex : frm.cur.tex;
	const QSize srcSize = useChain ? lastSize : frm.cur.size;
	// an old file and the built-in program read the texture as it is set up itself
	const bool legacyTex = !chainLast || preset.legacy;
	segBegin(SEG_PICTURE);
	if (chainLast && !preset.legacy && preset.passes[n - 1].mipmap) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src);
		glGenerateMipmap(GL_TEXTURE_2D);
	}
	glUseProgram(p.id());
	apply(binds, chainLast ? n - 1 : -1, frm, src, srcSize, frm.pic.size(), kMvpWindow, place, wide, legacyTex);
	int srcUnit = 0;
	for (const Bind& b : binds)
		if (b.kind == K_SOURCE) srcUnit = b.unit;
	glActiveTexture(GL_TEXTURE0 + srcUnit);
	// past the edge the edge dot; put back after, ES 3.0 has no CLAMP_TO_BORDER to set
	GLint wrap[2] = {GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE};
	const bool rewrap = wide && legacyTex;
	if (rewrap) {
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap[0]);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrap[1]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	quad();
	if (rewrap) {
		glActiveTexture(GL_TEXTURE0 + srcUnit);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[0]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[1]);
	}
	unbind(binds);
	glUseProgram(0);
	segEnd();
}

// ---------------------------------------------------------------- GPU time

void xShaderChain::timing(bool on) {
	timed = on && caps.timer;
	if (!timed) {
		for (Seg& s : segs) {
			s.sum = 0;
			s.n = 0;
			s.used = false;
			s.asked[0] = s.asked[1] = false;
		}
	}
}

// one query at a time: the segments follow each other, none inside another
void xShaderChain::segBegin(int seg) {
	if (!timed || (segOpen >= 0)) return;
	Seg& s = segs[seg];
	const int k = timeFrame & 1;
	if (s.asked[k]) return;			// drawn twice in a paint: the first one counts
	if (!s.q[0]) glGenQueries(2, s.q);
	glBeginQuery(GL_TIME_ELAPSED, s.q[k]);
	s.asked[k] = true;
	s.used = true;
	segOpen = seg;
}

void xShaderChain::segEnd() {
	if (segOpen < 0) return;
	glEndQuery(GL_TIME_ELAPSED);
	segOpen = -1;
}

// at a paint's start: what the paint before the last one asked is there by now
void xShaderChain::collectTimes() {
	if (!timed) return;
	timeFrame++;
	const int k = timeFrame & 1;
	for (Seg& s : segs) {
		if (!s.asked[k]) continue;
		GLuint ready = 0;
		glGetQueryObjectuiv(s.q[k], GL_QUERY_RESULT_AVAILABLE, &ready);
		if (ready) {
			GLuint ns = 0;
			glGetQueryObjectuiv(s.q[k], GL_QUERY_RESULT, &ns);
			if (s.n >= 60) {		// a running mean of about 60 paints
				s.sum -= s.sum / s.n;
				s.n--;
			}
			s.sum += ns;
			s.n++;
		}
		s.asked[k] = false;
	}
}

QString xShaderChain::timeReport() {
	if (!caps.timer) return "no timer queries here";
	if (!timed) return "gpu-time is off";
	QString out;
	double total = 0;
	for (int i = 0; i < SEG_COUNT; i++) {
		const Seg& s = segs[i];
		if (!s.used || !s.n) continue;
		const double us = s.sum / s.n / 1000.0;
		QString name = (i == SEG_PICTURE) ? "picture" : (i == SEG_FILL) ? "fill" : QString("pass %1").arg(i);
		if ((i < preset.passes.size()) && !preset.passes[i].name.isEmpty()) name += " " + preset.passes[i].name;
		// a pass that is off is not drawn: nothing is asked and its mean is stale
		if ((i < (int)passes.size()) && !passes[i].ran && (i < (int)passes.size() - 1)) continue;
		out += QString("%1: %2 us\r\n").arg(name).arg(us, 0, 'f', 1);
		total += us;
	}
	out += QString("total: %1 us\r\n").arg(total, 0, 'f', 1);
	return out;
}

// what the look costs: its passes and the picture's draw, the side fill left out
double xShaderChain::timeTotal() {
	if (!caps.timer || !timed) return -1;
	double total = 0;
	bool any = false;
	for (int i = 0; i <= SEG_PICTURE; i++) {
		const Seg& s = segs[i];
		if (!s.used || !s.n) continue;
		if ((i < (int)passes.size()) && !passes[i].ran && (i < (int)passes.size() - 1)) continue;
		total += s.sum / s.n / 1000.0;
		any = true;
	}
	return any ? total : -1;
}

QString xShaderChain::info() {
	QString out;
	if (!loaded() && !bad) return "no shader\r\n";
	out += QString("shader %1: %2\r\n").arg(preset.file,
		bad ? "failed" : live ? "ready" : "building");
	for (int i = 0; i < (int)passes.size(); i++) {
		const Pass& p = passes[i];
		const xShdPass& sp = preset.passes[i];
		const bool last = (i == (int)passes.size() - 1);
		const char* st = !p.on ? "off" : p.prg.ready() ? (p.prg.cached ? "cached" : "compiled")
			: (p.prg.state() == xGlProgram::Failed) ? "failed" : "building";
		QString line = QString("pass %1 %2: %3").arg(i).arg(sp.name.isEmpty() ? QString("-") : sp.name, QString(st));
		if (p.on && p.prg.ready()) line += QString(" %1 ms").arg(p.prg.ms, 0, 'f', 1);
		if (last) {
			line += ", into the window";
		} else if (p.tgt[p.cur].tex) {
			line += QString(", into %1x%2 %3").arg(p.tgt[p.cur].size.width()).arg(p.tgt[p.cur].size.height())
				.arg(QString::fromLatin1(formatName(p.tgt[p.cur].fmt)));
			if (p.feedback) line += " feedback";
		}
		if (!sp.when.isEmpty()) line += QString(" when %1").arg(sp.when);
		out += line + "\r\n";
	}
	if (histDepth) out += QString("history: %1\r\n").arg(histDepth);
	for (const xShdParam& p : preset.params)
		out += QString("param %1 = %2\r\n").arg(p.name).arg(p.value);
	for (const QString& n : preset.notes)
		out += QString("note %1\r\n").arg(n);
	if (!errText.isEmpty()) out += QString("error %1\r\n").arg(QString(errText).replace('\n', "\r\nerror "));
	return out;
}

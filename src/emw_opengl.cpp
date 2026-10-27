#include "emulwin.h"
#include "xcore/vscalers.h"
#include "xcore/log.h"

#include <QRegularExpression>
#if defined(USEOPENGL) && !BLOCKGL && !ISLEGACYGL
#include <QOffscreenSurface>
#include <QOpenGLContext>
#endif
#include <algorithm>
#include <vector>

#if defined(USEOPENGL) && !BLOCKGL

namespace {

// Interleaved quad: position (x,y) + texcoord (u,v), triangle strip order
// matching the legacy glBegin sequence (RT, LT, RB, LB). Positions are in
// [0,1]² and the vertex shader maps them to NDC with Y flipped so that the
// texture's top-left sample lands at the window's top-left.
constexpr GLfloat kQuadVertices[] = {
	1.0f, 0.0f,  1.0f, 0.0f,
	0.0f, 0.0f,  0.0f, 0.0f,
	1.0f, 1.0f,  1.0f, 1.0f,
	0.0f, 1.0f,  0.0f, 1.0f,
};

bool currentContextIsGLES() {
	QOpenGLContext* ctx = QOpenGLContext::currentContext();
	return ctx && ctx->isOpenGLES();
}

constexpr const char* glslVersionPrefix(bool es) {
	return es
		? "#version 300 es\nprecision highp float;\n"
		: "#version 330\n";
}

const char* defaultVertexShaderBody() {
	return
		"layout(location=0) in vec2 a_pos;\n"
		"layout(location=1) in vec2 a_uv;\n"
		"out vec2 v_uv;\n"
		"void main() {\n"
		"    v_uv = a_uv;\n"
		"    gl_Position = vec4(a_pos.x * 2.0 - 1.0, 1.0 - a_pos.y * 2.0, 0.0, 1.0);\n"
		"}\n";
}

const char* defaultFragmentShaderBody() {
	return
		"in vec2 v_uv;\n"
		"out vec4 fragColor;\n"
		"uniform sampler2D u_tex;\n"
		"void main() {\n"
		"    fragColor = texture(u_tex, v_uv);\n"
		"}\n";
}

QString composeShader(const char* body) {
	return QString::fromLatin1(glslVersionPrefix(currentContextIsGLES()))
	     + QString::fromLatin1(body);
}

// -----------------------------------------------------------------------------
// Side fill: the room beside a fitted picture. The frame is shrunk to a few
// dozen texels a side, blurred there, and drawn over the whole view with the
// small texture's own wrap mode (clamp or mirror) carrying it past the edge.
// The picture is drawn over the middle afterwards.
// -----------------------------------------------------------------------------

// every pass covers its whole target; the fragment's position is all it needs
const char* fillVertexShaderBody() {
	return
		"layout(location=0) in vec2 a_pos;\n"
		"void main() {\n"
		"    gl_Position = vec4(a_pos * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
}

// one texel = the average of a block of frame texels (the frame is NEAREST)
const char* fillDownShaderBody() {
	return
		"uniform sampler2D u_src;\n"
		"uniform ivec2 u_blk;\n"
		"out vec4 fragColor;\n"
		"void main() {\n"
		"    ivec2 sz = textureSize(u_src, 0);\n"
		"    ivec2 org = ivec2(gl_FragCoord.xy) * u_blk;\n"
		"    vec3 sum = vec3(0.0);\n"
		"    float n = 0.0;\n"
		"    for (int y = 0; y < u_blk.y; y++) {\n"
		"        for (int x = 0; x < u_blk.x; x++) {\n"
		"            ivec2 p = org + ivec2(x, y);\n"
		"            if (p.x < sz.x && p.y < sz.y) {\n"
		"                sum += texelFetch(u_src, p, 0).rgb;\n"
		"                n += 1.0;\n"
		"            }\n"
		"        }\n"
		"    }\n"
		"    fragColor = vec4(sum / max(n, 1.0), 1.0);\n"
		"}\n";
}

// 9-tap gaussian, sigma 2 texels, along u_dir (one texel in uv)
const char* fillBlurShaderBody() {
	return
		"uniform sampler2D u_src;\n"
		"uniform vec2 u_dir;\n"
		"out vec4 fragColor;\n"
		"const float w[5] = float[5](0.2042, 0.1802, 0.1238, 0.0663, 0.0276);\n"
		"void main() {\n"
		"    vec2 uv = gl_FragCoord.xy / vec2(textureSize(u_src, 0));\n"
		"    vec3 c = texture(u_src, uv).rgb * w[0];\n"
		"    for (int i = 1; i < 5; i++) {\n"
		"        c += texture(u_src, uv + u_dir * float(i)).rgb * w[i];\n"
		"        c += texture(u_src, uv - u_dir * float(i)).rgb * w[i];\n"
		"    }\n"
		"    fragColor = vec4(c, 1.0);\n"
		"}\n";
}

// u_pic: the picture in framebuffer pixels from the bottom left, as drawPicture's viewport;
// u_ramp: framebuffer pixels from the picture's edge, at full brightness, to the full dimming
const char* fillDrawShaderBody() {
	return
		"uniform sampler2D u_src;\n"
		"uniform vec4 u_pic;\n"
		"uniform vec2 u_view;\n"
		"uniform float u_dim;\n"
		"uniform float u_fade;\n"
		"uniform float u_ramp;\n"
		"out vec4 fragColor;\n"
		"void main() {\n"
		"    vec2 p = (gl_FragCoord.xy - u_pic.xy) / u_pic.zw;\n"
		"    vec3 c = texture(u_src, vec2(p.x, 1.0 - p.y)).rgb;\n"
		"    vec2 lo = max(u_pic.xy, vec2(1.0));\n"
		"    vec2 hi = max(u_view - u_pic.xy - u_pic.zw, vec2(1.0));\n"
		"    vec2 out_lo = (u_pic.xy - gl_FragCoord.xy) / lo;\n"
		"    vec2 out_hi = (gl_FragCoord.xy - u_pic.xy - u_pic.zw) / hi;\n"
		"    vec2 d = max(max(out_lo, out_hi), vec2(0.0));\n"
		"    float f = clamp(max(d.x, d.y), 0.0, 1.0);\n"
		"    vec2 px = max(max(u_pic.xy - gl_FragCoord.xy, gl_FragCoord.xy - u_pic.xy - u_pic.zw), vec2(0.0));\n"
		"    float k = mix(1.0, u_dim, smoothstep(0.0, max(u_ramp, 1.0), max(px.x, px.y)));\n"
		"    fragColor = vec4(c * k * (1.0 - u_fade * f), 1.0);\n"
		"}\n";
}

bool linkFill(QOpenGLShaderProgram& prg, const char* frag) {
	prg.removeAllShaders();
	if (prg.addShaderFromSourceCode(QOpenGLShader::Vertex, composeShader(fillVertexShaderBody()))
	    && prg.addShaderFromSourceCode(QOpenGLShader::Fragment, composeShader(frag))
	    && prg.link())
		return true;
	xlog(XLG_GL, XLL_WARN, "side fill: %s", prg.log().toLocal8Bit().data());
	prg.removeAllShaders();
	return false;
}

// -----------------------------------------------------------------------------
// Legacy GLSL 110 shim.
//
// Rewrites fixed-function-era shader source onto the core-compatible VBO
// attribute and uniform layout. The transformation is entirely data-driven
// by the tables below — to add a new legacy identifier, append a row to the
// relevant table; to add a new preamble attribute or uniform, edit the stage
// preamble helper. No change to loadShader or the shim pipeline is needed.
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

// Pure: apply a list of rewrites via left fold.
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

// Pure: strip any existing `#version` directive so the shim can supply its own.
QString stripVersionDirective(QString s) {
	static const QRegularExpression re(
		QStringLiteral(R"(^[ \t]*#[ \t]*version\b[^\n]*\n?)"),
		QRegularExpression::MultilineOption);
	s.replace(re, QString{});
	return s;
}

// Rewrites applied to both vertex and fragment stages.
const std::vector<Rewrite>& sharedRewrites() {
	static const std::vector<Rewrite> r{
		rewrite("texture2D", "texture"),
	};
	return r;
}

// Vertex-only rewrites.
const std::vector<Rewrite>& vertexRewrites() {
	static const std::vector<Rewrite> r{
		rewrite("varying",                      "out"),
		rewrite("gl_Vertex",                    "vec4(a_pos_.x, a_pos_.y, 0.0, 1.0)"),
		rewrite("gl_MultiTexCoord0",            "vec4(a_uv_.x,  a_uv_.y,  0.0, 0.0)"),
		rewrite("gl_ModelViewProjectionMatrix", "u_mvp_"),
	};
	return r;
}

// Fragment-only rewrites.
const std::vector<Rewrite>& fragmentRewrites() {
	static const std::vector<Rewrite> r{
		rewrite("varying",      "in"),
		rewrite("gl_FragColor", "frag_out_"),
	};
	return r;
}

QString vertexShimPreamble() {
	return QString::fromLatin1(glslVersionPrefix(currentContextIsGLES()))
	     + QStringLiteral(
		     "layout(location=0) in vec2 a_pos_;\n"
		     "layout(location=1) in vec2 a_uv_;\n"
		     "uniform mat4 u_mvp_;\n");
}

QString fragmentShimPreamble() {
	return QString::fromLatin1(glslVersionPrefix(currentContextIsGLES()))
	     + QStringLiteral("out vec4 frag_out_;\n");
}

// Pipeline: strip version → shared rewrites → stage rewrites → prepend preamble.
QString shimLegacyShader(QString src,
                         const std::vector<Rewrite>& stageRewrites,
                         const QString& preamble) {
	src = stripVersionDirective(std::move(src));
	src = applyRewrites(std::move(src), sharedRewrites());
	src = applyRewrites(std::move(src), stageRewrites);
	return preamble + src;
}

} // anonymous namespace

// A context of the default format, made current, and at least what the renderer
// needs: GLSL 330 / 300 es and a VAO. Qt can have no GL at all - its blacklist
// turns the desktop GL of some GPUs off (Arrandale's HD Graphics among them),
// and the fallbacks it would take instead are not shipped.
bool gl_probe(QString* why) {
#if ISLEGACYGL
	(void)why;
	return true;
#else
	QOffscreenSurface surf;
	surf.create();
	QOpenGLContext ctx;
	if (!ctx.create()) {
		*why = "no OpenGL context";
		return false;
	}
	if (!ctx.makeCurrent(&surf)) {
		*why = "the OpenGL context cannot be made current";
		return false;
	}
	const QSurfaceFormat f = ctx.format();
	const int ver = f.majorVersion() * 10 + f.minorVersion();
	const bool es = ctx.isOpenGLES();
	const bool ok = es ? (ver >= 30) : (ver >= 33);
	if (!ok) {
		const char* ren = (const char*)ctx.functions()->glGetString(GL_RENDERER);
		*why = QString("OpenGL %1%2.%3 on %4, %5 needed").arg(es ? "ES " : "")
			.arg(f.majorVersion()).arg(f.minorVersion())
			.arg(ren ? ren : "?").arg(es ? "ES 3.0" : "3.3");
	}
	ctx.doneCurrent();
	return ok;
#endif
}

xGLView::xGLView(MainWin* p) : mw(p) {
	setParent(p);
	setFocusPolicy(Qt::NoFocus);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setGeometry(p->rect());
}

void xGLView::initializeGL() { mw->initializeGL(); }
void xGLView::resizeGL(int w, int h) { mw->resizeGL(w, h); }
void xGLView::paintEvent(QPaintEvent*) { mw->paintGLView(); }

void MainWin::initializeGL() {
	qDebug() << __FUNCTION__;
#if !ISLEGACYGL
	initializeOpenGLFunctions();
	// what the driver actually gave us, not what was asked for: the shaders are
	// written against #version 330, and a context older than that explains a
	// black window better than any guess
	conf.vid.shd_support = QOpenGLShader::hasOpenGLShaders(QOpenGLShader::Vertex) && QOpenGLShader::hasOpenGLShaders(QOpenGLShader::Fragment);
	// the menus were built before this was known, the shader list with only "none" in it
	QTimer::singleShot(0, this, &MainWin::fillUserMenu);
	if (QOpenGLContext* ctx = QOpenGLContext::currentContext()) {
		const QSurfaceFormat f = ctx->format();
		const char* prof =
			(f.profile() == QSurfaceFormat::CoreProfile) ? "core" :
			(f.profile() == QSurfaceFormat::CompatibilityProfile) ? "compatibility" : "no";
		xlog(XLG_GL, XLL_INFO, "context %d.%d %s profile, %s | vendor %s | renderer %s | version %s | glsl %s | shaders %s",
			f.majorVersion(), f.minorVersion(), prof,
			ctx->isOpenGLES() ? "ES" : "desktop",
			(const char*)glGetString(GL_VENDOR),
			(const char*)glGetString(GL_RENDERER),
			(const char*)glGetString(GL_VERSION),
			(const char*)glGetString(GL_SHADING_LANGUAGE_VERSION),
			conf.vid.shd_support ? "yes" : "no");
	}
	curtex = 0;
	// nothing has been rendered yet, see paintEvent
	curtxid = 0;
	qDebug() << "vtx_shd";
	vtx_shd = new QOpenGLShader(QOpenGLShader::Vertex);
	qDebug() << "frg_shd";
	frg_shd = new QOpenGLShader(QOpenGLShader::Fragment);
#else
//	QGLFormat frmt;
//	frmt.setDoubleBuffer(false);
//	cont = new QGLContext(frmt);
//	setContext(cont);
//	setAutoBufferSwap(true);
//	makeCurrent();
//	curtex = 0;
//	conf.vid.shd_support = QGLShader::hasOpenGLShaders(QGLShader::Vertex) && QGLShader::hasOpenGLShaders(QGLShader::Fragment);
//	qDebug() << "vtx_shd";
//	vtx_shd = new QGLShader(QGLShader::Vertex, cont);
//	qDebug() << "frg_shd";
//	frg_shd = new QGLShader(QGLShader::Fragment, cont);
#endif
	glGenTextures(4, texids);
	glEnable(GL_MULTISAMPLE);
	for (int i = 0; i < 4; i++) {
		glBindTexture(GL_TEXTURE_2D, texids[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	}

#if !ISLEGACYGL
	vao.create();
	vao.bind();
	vbo.create();
	vbo.bind();
	vbo.setUsagePattern(QOpenGLBuffer::StaticDraw);
	vbo.allocate(kQuadVertices, sizeof(kQuadVertices));
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
	                      reinterpret_cast<void *>(0));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
	                      reinterpret_cast<void *>(2 * sizeof(GLfloat)));
	vbo.release();
	vao.release();
	initFill();
#endif

	loadShader();
	if (!conf.vid.shd_support) xlog(XLG_GL, XLL_WARN, "shaders are not supported");
	qDebug() << "end:" << __FUNCTION__;
}

void MainWin::cleanupGL() {
#if !ISLEGACYGL
	if (!glw || (!vao.isCreated() && !vbo.isCreated())) return;
	glw->makeCurrent();
	if (vao.isCreated()) vao.destroy();
	if (vbo.isCreated()) vbo.destroy();
	prg.removeAllShaders();
	glDeleteTextures(4, texids);
	fillDown.removeAllShaders();
	fillBlur.removeAllShaders();
	fillDraw.removeAllShaders();
	glDeleteFramebuffers(2, fillFbo);
	glDeleteTextures(2, fillTex);
	glw->doneCurrent();
#endif
}

#if !ISLEGACYGL
// The side fill's programs and its two small render targets. If any of it
// fails the fill is not drawn - black, as with no fill.
void MainWin::initFill() {
	glGenTextures(2, fillTex);
	glGenFramebuffers(2, fillFbo);
	fillSize = QSize(1, 1);
	bool ok = true;
	for (int i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, fillTex[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glBindFramebuffer(GL_FRAMEBUFFER, fillFbo[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fillTex[i], 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) ok = false;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, glw->defaultFramebufferObject());
	glBindTexture(GL_TEXTURE_2D, 0);
	if (!ok) xlog(XLG_GL, XLL_WARN, "side fill: framebuffer incomplete");
	if (!ok || !linkFill(fillDown, fillDownShaderBody()) || !linkFill(fillBlur, fillBlurShaderBody())
	    || !linkFill(fillDraw, fillDrawShaderBody()))
		fillDraw.removeAllShaders();		// fillWanted() looks at this one
}

// a fill is chosen, there is a frame, and the picture leaves room beside it
bool MainWin::fillWanted() {
	if ((conf.vid.sideFill == SFILL_BLACK) || !curtxid || !fillDraw.isLinked()) return false;
	return (drawX > 0) || (drawY > 0) || (drawX + drawW < width()) || (drawY + drawH < height());
}

// The current frame shrunk into fillTex[0] and blurred there, across and down.
// The wrap mode is set before the blur, so the blur at the edge already sees
// what is drawn past it.
void MainWin::prepareFill() {
	Video* vid = conf.zx->vid;
	const int blk = qBound(SFILL_BLUR_MIN, conf.vid.fillBlur, SFILL_BLUR_MAX);	// the blur is 2 texels: 2 * blk dots
	QSize sz((vid->vsze.x + blk - 1) / blk, (vid->vsze.y + blk - 1) / blk);
	const GLint wrap = (conf.vid.sideFill == SFILL_MIRROR) ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
	for (int i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, fillTex[i]);
		if (sz != fillSize)
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sz.width(), sz.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
	}
	fillSize = sz;
	glDisable(GL_BLEND);
	glViewport(0, 0, sz.width(), sz.height());
	glActiveTexture(GL_TEXTURE0);
	vao.bind();
	// two frame texels per dot across
	glBindFramebuffer(GL_FRAMEBUFFER, fillFbo[0]);
	fillDown.bind();
	fillDown.setUniformValue("u_src", 0);
	glUniform2i(fillDown.uniformLocation("u_blk"), blk * 2, blk);
	glBindTexture(GL_TEXTURE_2D, curtxid);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	fillBlur.bind();
	fillBlur.setUniformValue("u_src", 0);
	glBindFramebuffer(GL_FRAMEBUFFER, fillFbo[1]);
	fillBlur.setUniformValue("u_dir", GLfloat(1.0 / sz.width()), GLfloat(0));
	glBindTexture(GL_TEXTURE_2D, fillTex[0]);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindFramebuffer(GL_FRAMEBUFFER, fillFbo[0]);
	fillBlur.setUniformValue("u_dir", GLfloat(0), GLfloat(1.0 / sz.height()));
	glBindTexture(GL_TEXTURE_2D, fillTex[1]);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	fillBlur.release();
	vao.release();
	glBindFramebuffer(GL_FRAMEBUFFER, glw->defaultFramebufferObject());
}

// fillTex[0] over the whole view, dimmed; the picture then covers the middle
void MainWin::drawFill() {
	const qreal r = widgetDpr(this);
	const int vw = int(width() * r + 0.5);
	const int vh = int(height() * r + 0.5);
	glViewport(0, 0, vw, vh);
	fillDraw.bind();
	fillDraw.setUniformValue("u_src", 0);
	fillDraw.setUniformValue("u_pic", GLfloat(int(drawX * r + 0.5)), GLfloat(int(drawY * r + 0.5)),
		GLfloat(int(drawW * r + 0.5)), GLfloat(int(drawH * r + 0.5)));
	fillDraw.setUniformValue("u_view", GLfloat(vw), GLfloat(vh));
	fillDraw.setUniformValue("u_dim", GLfloat(conf.vid.fillDim / 100.0));
	fillDraw.setUniformValue("u_fade", GLfloat(conf.vid.fillFade / 100.0));
	// a dot is drawW / vsze.x pixels across, and as many down
	fillDraw.setUniformValue("u_ramp", GLfloat(conf.vid.fillEdge * drawW * r / conf.zx->vid->vsze.x));
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, fillTex[0]);
	vao.bind();
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	vao.release();
	fillDraw.release();
}
#endif

void MainWin::resizeGL(int w, int h) {
	const qreal r = widgetDpr(this);
	const int vw = int(w * r + 0.5);
	const int vh = int(h * r + 0.5);
	glViewport(0, 0, vw, vh);
	paintOwed = 1;
	qDebug() << "resizeGL logical" << w << h << "dpr" << r << "viewport" << vw << vh;
}

#endif

// The shader filters a picture that has been scaled up; at 1:1 there is nothing
// for it to work on, so the chosen one is ignored. It stays in the config, and
// comes back as soon as the size does.
std::string MainWin::wantedShader() {
	if (drawZoom == 1) return std::string();
	return conf.vid.shader;
}

#if defined(USEOPENGL) && !BLOCKGL
// Holds the widget's context current while it is in scope, and only takes it
// when it is not current already: initializeGL() and paintEvent() are handed
// it by Qt, and releasing it under them would leave Qt drawing into nothing.
namespace {
	struct glHold {
		xGLView* win;
		bool taken;
		glHold(xGLView* w) : win(w) {
#if !ISLEGACYGL
			taken = (QOpenGLContext::currentContext() != w->context());
#else
			taken = (QGLContext::currentContext() != w->context());
#endif
			if (taken) win->makeCurrent();
		}
		~glHold() { if (taken) win->doneCurrent(); }
	};
}
#endif

// Compiling and linking needs the context current on this thread, and the
// options dialog, the shader menu and the reload hotkey all get here from
// outside a paint event. Calling GL with no current context is undefined, and
// the Intel Gen9.5 driver crashes on it from its shader compiler thread a
// couple of seconds later.
void MainWin::loadShader() {
#if defined(USEOPENGL) && !BLOCKGL
	if (!glw || !conf.vid.shd_support) return;
	glHold hold(glw);
	shdLoaded = wantedShader();

	QString vtx;
	QString frg;
	bool user_shader = false;

	if (!shdLoaded.empty()) {
		QFile file(xres_path("shaders", QString::fromLocal8Bit(shdLoaded.c_str())));
		if (file.open(QFile::ReadOnly)) {
			int mode = 0;
			QString lin;
			while (!file.atEnd()) {
				lin = file.readLine().trimmed().append("\n");
				if (lin.startsWith("vertex:")) {
					mode = 1;
				} else if (lin.startsWith("fragment:")) {
					mode = 2;
				} else {
					switch (mode) {
						case 1: vtx.append(lin); break;
						case 2: frg.append(lin); break;
					}
				}
			}
			file.close();
			user_shader = true;
		} else {
			shitHappens("Can't open shader file");
		}
	}

	if (!user_shader) {
		vtx = composeShader(defaultVertexShaderBody());
		frg = composeShader(defaultFragmentShaderBody());
	} else if (sourceNeedsLegacyShim(vtx) || sourceNeedsLegacyShim(frg)) {
		vtx = shimLegacyShader(std::move(vtx), vertexRewrites(),   vertexShimPreamble());
		frg = shimLegacyShader(std::move(frg), fragmentRewrites(), fragmentShimPreamble());
	}

	prg.removeAllShaders();
	const bool vtx_ok = vtx_shd->compileSourceCode(vtx);
	if (!vtx_ok) qDebug() << "vertex shader:" << vtx_shd->log();
	const bool frg_ok = frg_shd->compileSourceCode(frg);
	if (!frg_ok) qDebug() << "fragment shader:" << frg_shd->log();

	if (vtx_ok && frg_ok) {
		prg.addShader(vtx_shd);
		prg.addShader(frg_shd);
		if (prg.link()) {
			if (user_shader) setMessage(" shader compiled ");
			return;
		}
		qDebug() << "program link:" << prg.log();
	}

	if (user_shader) {
		setMessage(" shader error ");
		conf.vid.shader.clear();
		loadShader();			// the hold above keeps the context
	} else {
		xlog(XLG_GL, XLL_ERROR, "the default shader failed to compile or link");
	}
#endif
}

void MainWin::shdSelected(QAction* act) {
	conf.vid.shader = act->data().toString().toLocal8Bit().data();
	loadShader();
}

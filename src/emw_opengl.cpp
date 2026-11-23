#include "emulwin.h"
#include "xcore/vscalers.h"
#include "xcore/log.h"

#include <QFileInfo>
#include <QOffscreenSurface>
#include <QStandardPaths>
#include <QOpenGLContext>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

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

// one texel = the average of a block of source texels: read one by one from the
// frame (NEAREST, no mipmaps), or one sample of the mipmap level that size
// (u_lod, the shown picture - a block there is a thousand pixels and more).
// u_blk: source texels per texel here, fractional for the shown picture;
// u_flip: the source's rows go bottom up, as a framebuffer's do
const char* fillDownShaderBody() {
	return
		"uniform sampler2D u_src;\n"
		"uniform vec2 u_blk;\n"
		"uniform int u_flip;\n"
		"uniform float u_lod;\n"
		"out vec4 fragColor;\n"
		"void main() {\n"
		"    ivec2 sz = textureSize(u_src, 0);\n"
		"    if (u_lod >= 0.0) {\n"
		"        vec2 uv = (floor(gl_FragCoord.xy) + 0.5) * u_blk / vec2(sz);\n"
		"        if (u_flip != 0) uv.y = 1.0 - uv.y;\n"
		"        fragColor = vec4(textureLod(u_src, uv, u_lod).rgb, 1.0);\n"
		"        return;\n"
		"    }\n"
		"    ivec2 org = ivec2(floor(gl_FragCoord.xy) * u_blk);\n"
		"    ivec2 end = min(ivec2(ceil((floor(gl_FragCoord.xy) + 1.0) * u_blk)), sz);\n"
		"    vec3 sum = vec3(0.0);\n"
		"    float n = 0.0;\n"
		"    for (int y = org.y; y < end.y; y++) {\n"
		"        for (int x = org.x; x < end.x; x++) {\n"
		"            sum += texelFetch(u_src, ivec2(x, (u_flip != 0) ? sz.y - 1 - y : y), 0).rgb;\n"
		"            n += 1.0;\n"
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
// u_ramp: framebuffer pixels from the picture's edge, at full brightness, to the full dimming;
// u_stretch: k of ln(1 + k*d) / k, d picture sizes out from the edge - the reflection drawn
// out like pulled rubber: as it is by the picture, 1 + k*d times wider further out, and its
// far part off the screen;
// u_cover: Letterbox's cover in fb px (z = 0: off), run into over u_ramp; u_paper: its uv;
// u_only: the dimming alone, to multiply what is drawn there already (sfill_sharp)
const char* fillDrawShaderBody() {
	return
		"uniform sampler2D u_src;\n"
		"uniform vec4 u_pic;\n"
		"uniform vec2 u_view;\n"
		"uniform float u_dim;\n"
		"uniform float u_fade;\n"
		"uniform float u_ramp;\n"
		"uniform float u_stretch;\n"
		"uniform vec4 u_cover;\n"
		"uniform vec4 u_paper;\n"
		"uniform int u_only;\n"
		"out vec4 fragColor;\n"
		"void main() {\n"
		"    vec2 room_lo = max(u_pic.xy, vec2(1.0));\n"
		"    vec2 room_hi = max(u_view - u_pic.xy - u_pic.zw, vec2(1.0));\n"
		"    vec2 out_lo = (u_pic.xy - gl_FragCoord.xy) / room_lo;\n"
		"    vec2 out_hi = (gl_FragCoord.xy - u_pic.xy - u_pic.zw) / room_hi;\n"
		"    vec2 d = max(max(out_lo, out_hi), vec2(0.0));\n"
		"    float f = clamp(max(d.x, d.y), 0.0, 1.0);\n"
		"    vec2 px = max(max(u_pic.xy - gl_FragCoord.xy, gl_FragCoord.xy - u_pic.xy - u_pic.zw), vec2(0.0));\n"
		"    float t = smoothstep(0.0, max(u_ramp, 1.0), max(px.x, px.y));\n"
		"    float k = mix(1.0, u_dim, t);\n"
		"    if (u_only != 0) {\n"
		"        fragColor = vec4(vec3(k * (1.0 - u_fade * f)), 1.0);\n"
		"        return;\n"
		"    }\n"
		"    vec2 p = (gl_FragCoord.xy - u_pic.xy) / u_pic.zw;\n"
		"    if (u_stretch > 0.0) {\n"
		"        vec2 lo = max(-p, vec2(0.0));\n"
		"        vec2 hi = max(p - 1.0, vec2(0.0));\n"
		"        p += lo - log(1.0 + u_stretch * lo) / u_stretch;\n"
		"        p -= hi - log(1.0 + u_stretch * hi) / u_stretch;\n"
		"    }\n"
		"    vec3 c = texture(u_src, vec2(p.x, 1.0 - p.y)).rgb;\n"
		"    if (u_cover.z > 0.0) {\n"
		"        vec2 q = (gl_FragCoord.xy - u_cover.xy) / u_cover.zw;\n"
		"        vec3 b = texture(u_src, u_paper.xy + vec2(q.x, 1.0 - q.y) * u_paper.zw).rgb;\n"
		"        c = mix(c, b, t);\n"
		"    }\n"
		"    fragColor = vec4(c * k * (1.0 - u_fade * f), 1.0);\n"
		"}\n";
}

} // anonymous namespace

// The first context made loads the driver: gl_probe_begin() has it made on a thread of
// its own while main() goes on, gl_probe() takes it up. main() must make no window of
// Qt's meanwhile. Qt 6 on Windows only, where Qt's static context is behind a lock.
#if defined(_WIN32) && (QT_VERSION >= QT_VERSION_CHECK(6,0,0))
static std::thread* probeThread = nullptr;
static QOpenGLContext* probeCtx = nullptr;	// the thread's, read after its join

void gl_probe_begin() {
	if (probeThread) return;
	QThread* gui = QThread::currentThread();
	probeThread = new std::thread([gui]() {
		QOpenGLContext* ctx = new QOpenGLContext;
		ctx->create();
		// made current and deleted on the GUI thread
		if (ctx->shareGroup()) ctx->shareGroup()->moveToThread(gui);
		ctx->moveToThread(gui);
		probeCtx = ctx;
	});
}

// the context the thread made (valid or not), nullptr when none was begun
static QOpenGLContext* probe_take() {
	if (!probeThread) return nullptr;
	probeThread->join();
	delete probeThread;
	probeThread = nullptr;
	QOpenGLContext* ctx = probeCtx;
	probeCtx = nullptr;
	return ctx;
}
#else
void gl_probe_begin() {}
static QOpenGLContext* probe_take() {return nullptr;}
#endif

void gl_probe_drop() {
	delete probe_take();
}

// A context of the default format, made current, and at least what the renderer
// needs: GLSL 330 / 300 es and a VAO. Qt can have no GL at all - its blacklist
// turns the desktop GL of some GPUs off (Arrandale's HD Graphics among them),
// and the fallbacks it would take instead are not shipped.
bool gl_probe(QString* why) {
	std::unique_ptr<QOpenGLContext> made(probe_take());
	if (made && !made->isValid()) made.reset();	// not made there: tried here, as ever
	QOffscreenSurface surf;
	surf.create();
	QOpenGLContext own;
	QOpenGLContext& ctx = made ? *made : own;
	if (!made && !own.create()) {
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
	ringSize = 4;
	ringCount = 0;
	// nothing has been rendered yet, see paintEvent
	curtxid = 0;
	glGenTextures(XP_RING_MAX, texids);
	glEnable(GL_MULTISAMPLE);
	for (int i = 0; i < XP_RING_MAX; i++) {
		ringInfo[i] = xFrameTex();
		ringSeq[i] = -1;
		glBindTexture(GL_TEXTURE_2D, texids[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	}

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
	// The programs' binaries: beside the config on Windows, where the program keeps to
	// its own folder, in the system's cache elsewhere
#ifdef _WIN32
	const QString cache = QString::fromLocal8Bit(conf.path.confDir.c_str()) + "/shader-cache";
#else
	const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/xpeccy-plus/shaders";
#endif
	// a context made again: the fill's programs were the old one's
	fillDown.forget();
	fillBlur.forget();
	fillDraw.forget();
	if (!chain.init(cache)) xlog(XLG_GL, XLL_ERROR, "the default shader failed to compile or link");
	initFill();
	glReady = true;

	loadShader(false);
	if (!conf.vid.shd_support) xlog(XLG_GL, XLL_WARN, "shaders are not supported");
	qDebug() << "end:" << __FUNCTION__;
}

void MainWin::cleanupGL() {
	if (!glw || (!vao.isCreated() && !vbo.isCreated())) return;
	glw->makeCurrent();
	if (vao.isCreated()) vao.destroy();
	if (vbo.isCreated()) vbo.destroy();
	if (vaoWide.isCreated()) vaoWide.destroy();
	if (vboWide.isCreated()) vboWide.destroy();
	chain.destroy();
	glDeleteTextures(XP_RING_MAX, texids);
	fillDown.destroy();
	fillBlur.destroy();
	fillDraw.destroy();
	glReady = false;
	glDeleteFramebuffers(2, fillFbo);
	glDeleteTextures(2, fillTex);
	glDeleteTextures(1, &fillPic);
	glw->doneCurrent();
}

// The side fill's two small render targets. Its programs are built when a fill is
// first drawn (fillPrograms). If any of it fails the fill is not drawn - black, as
// with no fill.
void MainWin::initFill() {
	vaoWide.create();
	vaoWide.bind();
	vboWide.create();
	vboWide.bind();
	vboWide.setUsagePattern(QOpenGLBuffer::DynamicDraw);
	vboWide.allocate(kQuadVertices, sizeof(kQuadVertices));		// made for the picture on its first draw
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void *>(0));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void *>(2 * sizeof(GLfloat)));
	vboWide.release();
	vaoWide.release();
	wideFor = QRect();
	glGenTextures(2, fillTex);
	glGenFramebuffers(2, fillFbo);
	fillSize = QSize(1, 1);
	glGenTextures(1, &fillPic);
	glBindTexture(GL_TEXTURE_2D, fillPic);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);	// with mipmaps when they are made
	fillPicSize = QSize();
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
	fillFboOk = ok;
	fillState = 0;
}

// the three programs, from the cache or compiled, the first time a fill is to be drawn
bool MainWin::fillPrograms() {
	if (fillState) return fillState > 0;
	fillState = -1;
	if (!fillFboOk) return false;
	const QByteArray vtx = xshd_compose(fillVertexShaderBody());
	chain.builtin(fillDown, vtx, xshd_compose(fillDownShaderBody()));
	chain.builtin(fillBlur, vtx, xshd_compose(fillBlurShaderBody()));
	chain.builtin(fillDraw, vtx, xshd_compose(fillDrawShaderBody()));
	if (fillDown.ready() && fillBlur.ready() && fillDraw.ready()) fillState = 1;
	return fillState > 0;
}

// a fill is chosen, there is a frame, and the picture leaves room beside it
bool MainWin::fillWanted() {
	if ((conf.vid.sideFill == SFILL_BLACK) || !curtxid || (fillState < 0)) return false;
	bool room;
	if (conf.vid.intScale) {
		// placed in device pixels: a gap of one would round away in logical ones
		const QRect pic = picRect();
		const QSize dev = devSize();
		room = (pic.x() > 0) || (pic.y() > 0) || (pic.right() + 1 < dev.width()) || (pic.bottom() + 1 < dev.height());
	} else {
		room = (drawX > 0) || (drawY > 0) || (drawX + drawW < width()) || (drawY + drawH < height());
	}
	return room && fillPrograms();
}

// The picture shrunk into fillTex[0] and blurred there, across and down. Mirror
// reflects the picture as it is shown - through the shader, a curved screen and
// its black corners too - so it is read back from the framebuffer, drawn already;
// Stretched edges carries the frame's own edge out, which a curved screen leaves
// black; Letterbox reads the frame as Edges does. The wrap mode is set before
// the blur, so the blur at the edge already sees what is drawn past it.
void MainWin::prepareFill() {
	Video* vid = conf.zx->vid;
	const bool mirror = (conf.vid.sideFill == SFILL_MIRROR);
	const int blk = qBound(SFILL_BLUR_LOW, conf.vid.fillBlur, SFILL_BLUR_MAX);	// the blur is 2 texels: 2 * blk dots
	QSize sz((vid->vsze.x + blk - 1) / blk, (vid->vsze.y + blk - 1) / blk);
	GLuint src = curtxid;
	QSize srcSize(vid->vsze.x * 2, vid->vsze.y);		// two frame texels per dot across
	if (mirror) {
		const QRect pic = picRect();
		srcSize = pic.size();
		glBindTexture(GL_TEXTURE_2D, fillPic);
		if (srcSize != fillPicSize) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, srcSize.width(), srcSize.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			fillPicSize = srcSize;
		}
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pic.x(), pic.y(), srcSize.width(), srcSize.height());
		src = fillPic;
	}
	const GLfloat bx = GLfloat(srcSize.width()) / sz.width();
	const GLfloat by = GLfloat(srcSize.height()) / sz.height();
	// A strong blur leaves few texels here, each a long loop over the picture's
	// pixels: then the mipmaps are cheaper (measured: 0.08 ms against 0.20 at
	// Blur 8 on an RTX 4070), and dearer at Blur 2, where the loops are short.
	const bool useMip = mirror && (sz.width() * sz.height() < 8000);
	if (mirror) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, useMip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
		if (useMip) glGenerateMipmap(GL_TEXTURE_2D);
	}
	const GLint wrap = mirror ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
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
	glBindFramebuffer(GL_FRAMEBUFFER, fillFbo[0]);
	glUseProgram(fillDown.id());
	glUniform1i(fillDown.loc("u_src"), 0);
	glUniform2f(fillDown.loc("u_blk"), bx, by);
	glUniform1i(fillDown.loc("u_flip"), mirror ? 1 : 0);
	glUniform1f(fillDown.loc("u_lod"), GLfloat(useMip ? std::log2(std::max(bx, by)) : -1.0));
	glBindTexture(GL_TEXTURE_2D, src);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUseProgram(fillBlur.id());
	glUniform1i(fillBlur.loc("u_src"), 0);
	glBindFramebuffer(GL_FRAMEBUFFER, fillFbo[1]);
	glUniform2f(fillBlur.loc("u_dir"), GLfloat(1.0 / sz.width()), GLfloat(0));
	glBindTexture(GL_TEXTURE_2D, fillTex[0]);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindFramebuffer(GL_FRAMEBUFFER, fillFbo[0]);
	glUniform2f(fillBlur.loc("u_dir"), GLfloat(0), GLfloat(1.0 / sz.height()));
	glBindTexture(GL_TEXTURE_2D, fillTex[1]);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUseProgram(0);
	vao.release();
	glBindFramebuffer(GL_FRAMEBUFFER, glw->defaultFramebufferObject());
}

// fillTex[0], dimmed, into the room beside the picture only: the bars at the sides,
// corners and all, and the strips above and below it. clip: the framebuffer
// rectangle the caller has scissored to, which the bars are cut to as well.
// dimOnly: over the picture drawn on past its edges (sfill_sharp), Brightness and Fade multiplied in
void MainWin::drawFill(const QRect* clip, bool dimOnly) {
	if (dimOnly && (conf.vid.fillDim >= 100) && (conf.vid.fillFade <= 0)) return;
	const QSize dev = devSize();
	const int vw = dev.width();
	const int vh = dev.height();
	// the picture as drawPicture's viewport has it, from the bottom left
	const QRect pic = picRect();
	const int px = pic.x();
	const int py = pic.y();
	const int pw = pic.width();
	const int ph = pic.height();
	const QRect bars[4] = {
		QRect(0, 0, px, vh),
		QRect(px + pw, 0, vw - px - pw, vh),
		QRect(px, 0, pw, py),
		QRect(px, py + ph, pw, vh - py - ph)
	};
	glViewport(0, 0, vw, vh);
	glUseProgram(fillDraw.id());
	glUniform1i(fillDraw.loc("u_src"), 0);
	glUniform4f(fillDraw.loc("u_pic"), GLfloat(px), GLfloat(py), GLfloat(pw), GLfloat(ph));
	glUniform2f(fillDraw.loc("u_view"), GLfloat(vw), GLfloat(vh));
	glUniform1f(fillDraw.loc("u_dim"), GLfloat(conf.vid.fillDim / 100.0));
	glUniform1f(fillDraw.loc("u_fade"), GLfloat(conf.vid.fillFade / 100.0));
	// a dot is pw / vsze.x pixels across, and as many down
	glUniform1f(fillDraw.loc("u_ramp"), GLfloat(conf.vid.fillEdge * pw / (double)conf.zx->vid->vsze.x));
	// 100 %: k = 10, the far edge of a bar a third of the picture wide drawn out 4 times
	const bool mirror = (conf.vid.sideFill == SFILL_MIRROR);
	glUniform1f(fillDraw.loc("u_stretch"), GLfloat(mirror ? conf.vid.fillStretch * 0.1 : 0.0));
	// Letterbox: the screen area as big as it takes to cover the view, centred on it
	GLfloat cover[4] = {0, 0, 0, 0};
	GLfloat paper[4] = {0, 0, 1, 1};
	const Video* vid = conf.zx->vid;
	if ((conf.vid.sideFill == SFILL_LETTERBOX) && (vid->vsze.x > 0) && (vid->vsze.y > 0) && (pw > 0) && (ph > 0)) {
		QRect scr = vid_paper(vid) & QRect(0, 0, vid->vsze.x, vid->vsze.y);
		if (scr.isEmpty()) scr = QRect(0, 0, vid->vsze.x, vid->vsze.y);
		paper[0] = GLfloat(scr.x()) / vid->vsze.x;
		paper[1] = GLfloat(scr.y()) / vid->vsze.y;
		paper[2] = GLfloat(scr.width()) / vid->vsze.x;
		paper[3] = GLfloat(scr.height()) / vid->vsze.y;
		const double sw = paper[2] * pw;
		const double sh = paper[3] * ph;
		const double k = std::max(vw / sw, vh / sh);
		cover[2] = GLfloat(sw * k);
		cover[3] = GLfloat(sh * k);
		cover[0] = GLfloat((vw - sw * k) / 2);
		cover[1] = GLfloat((vh - sh * k) / 2);
	}
	glUniform4f(fillDraw.loc("u_cover"), cover[0], cover[1], cover[2], cover[3]);
	glUniform4f(fillDraw.loc("u_paper"), paper[0], paper[1], paper[2], paper[3]);
	glUniform1i(fillDraw.loc("u_only"), dimOnly ? 1 : 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, fillTex[0]);
	vao.bind();
	if (dimOnly) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_ZERO, GL_SRC_COLOR);
	}
	glEnable(GL_SCISSOR_TEST);
	for (const QRect& bar : bars) {
		const QRect cut = clip ? (bar & *clip) : bar;
		if (cut.isEmpty()) continue;
		glScissor(cut.x(), cut.y(), cut.width(), cut.height());
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	if (dimOnly) glDisable(GL_BLEND);
	if (clip) {
		glScissor(clip->x(), clip->y(), clip->width(), clip->height());
	} else {
		glDisable(GL_SCISSOR_TEST);
	}
	vao.release();
	glUseProgram(0);
}

void MainWin::resizeGL(int w, int h) {
	const qreal r = widgetDpr(this);
	const int vw = int(w * r + 0.5);
	const int vh = int(h * r + 0.5);
	glViewport(0, 0, vw, vh);
	paintOwed = 1;
	qDebug() << "resizeGL logical" << w << h << "dpr" << r << "viewport" << vw << vh;
}

// The shader filters a picture that has been scaled up; at 1:1 there is nothing
// for it to work on, so the chosen one is not drawn through. Its chain stays
// loaded, and draws again as soon as the size does.
bool MainWin::shaderUsed() {
	return (drawZoom != 1) && !conf.vid.shader.empty();
}

// Holds the widget's context current while it is in scope, and only takes it
// when it is not current already: initializeGL() and paintEvent() are handed
// it by Qt, and releasing it under them would leave Qt drawing into nothing.
namespace {
	struct glHold {
		xGLView* win;
		bool taken;
		glHold(xGLView* w) : win(w) {
			taken = (QOpenGLContext::currentContext() != w->context());
			if (taken) win->makeCurrent();
		}
		~glHold() { if (taken) win->doneCurrent(); }
	};
}

// Loading programs needs the context current on this thread, and the options
// dialog, the shader menu and the reload hotkey all get here from outside a paint
// event. Calling GL with no current context is undefined, and the Intel Gen9.5
// driver crashes on it from its shader compiler thread a couple of seconds later.
// A program found in the cache is there on return; what has to be compiled is
// built over the next paints, the picture drawn by the built-in program meanwhile
// (paintGLView). A shader that fails stays chosen: fixed, the reload key takes it.
void MainWin::loadShader(bool tell) {
	if (!glw || !conf.vid.shd_support || !glReady) return;
	glHold hold(glw);
	shdLoaded = conf.vid.shader;
	shdSettled = false;
	shdTell = tell;
	chain.load(xres_path("shaders", QString::fromLocal8Bit(shdLoaded.c_str())));
	shaderSay();
}

void MainWin::shdSelected(QAction* act) {
	conf.vid.shader = act->data().toString().toLocal8Bit().data();
	loadShader(false);
}

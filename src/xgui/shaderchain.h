#pragma once

// The shader engine of the GL picture: a preset file read into passes, each pass a
// program kept as a binary between runs, the passes before the last drawn into
// textures of their own and the last one into the window, where the one shader
// was always drawn. The names a pass sees are slang's (Source, Original,
// OriginalHistoryN, PassOutputN, PassFeedbackN, ...Size, FrameCount), so a shader
// written for RetroArch ports by hand.

#include "../xcore/framenums.h"

#include <QByteArray>
#include <QHash>
#include <QOpenGLExtraFunctions>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <vector>

#define XSHD_HISTORY_MAX	8	// frames back a pass may ask for
#define XSHD_PASS_MAX		32

// ---------------------------------------------------------------- the preset

enum {
	XSHD_SCALE_SOURCE = 0,		// of the pass's Source
	XSHD_SCALE_VIEWPORT,		// of the picture in the window
	XSHD_SCALE_ABSOLUTE		// texels
};

enum {
	XSHD_WRAP_BORDER = 0,		// transparent black past the edge; slang's default
	XSHD_WRAP_CLAMP,
	XSHD_WRAP_REPEAT,
	XSHD_WRAP_MIRROR,
	XSHD_WRAP_COUNT
};

enum {
	XSHD_FMT_RGBA8 = 0,
	XSHD_FMT_SRGB8,
	XSHD_FMT_RGBA16F
};

struct xShdParam {
	QString name;
	QString text;
	float def = 0;
	float min = 0;
	float max = 1;
	float step = 0;
	float value = 0;
	// how the sheet of knobs shows it
	QString group;			// the frame it stands in; empty: none named
	QString groupSwitch;		// the frame has a switch: this parameter, off at 0...
	float groupOn = 0;		// ...and this when switched on with no value of its own to go back to
	QString tip;
	QStringList names;		// a list: what min, min + step... are called
	bool more = false;		// only under All settings
};

// a named set of values: a `preset` line of the file
struct xShdValue {
	QString name;
	float value = 0;
};
typedef QVector<xShdValue> xShdValues;

struct xShdSet {
	QString name;
	xShdValues values;
};

struct xShdTexture {
	QString name;
	QString file;			// beside the preset
	bool linear = true;
	bool mipmap = false;
	int wrap = XSHD_WRAP_REPEAT;
	int line = 0;
};

struct xShdPass {
	QString name;			// its alias, may be empty
	int line = 0;
	int scaleType[2] = {XSHD_SCALE_SOURCE, XSHD_SCALE_SOURCE};
	float scale[2] = {1.0f, 1.0f};
	bool hasScale = false;
	bool linear = false;
	int wrap = XSHD_WRAP_BORDER;
	int format = XSHD_FMT_RGBA8;
	bool hasFormat = false;
	bool mipmap = false;		// its Source gets mipmaps first
	QString when;			// a parameter: the pass is not run while it is 0
	QString vtx;			// empty: the default one
	QString frg;
	int vtxLine = 0;		// the file line of the stage's first line
	int frgLine = 0;
};

struct xShdPreset {
	QString path;
	QString file;			// its name, for messages
	bool legacy = true;		// no pass line: one vertex and one fragment section, as ever
	QString title;			// its `name` line: what the lists show
	bool raw = false;		// its `frames raw` line: frames are wanted before antiflicker mixes them
	QString rawWhen;		// ...while this parameter is not 0; empty: always
	QVector<xShdSet> sets;		// its `preset` lines
	QVector<xShdParam> params;
	QVector<xShdTexture> textures;
	QString common;
	int commonLine = 0;
	QVector<xShdPass> passes;
	QStringList notes;		// "file:line: text" of lines not understood
};

// false: the file cannot be read or holds no pass at all (err says which)
bool xshd_parse(const QString& path, xShdPreset& out, QString* err);

// a stage of the window's own programs: the context's #version line in front of body
QByteArray xshd_compose(const char* body);

// ---------------------------------------------------------------- a program

// What the context can do, asked once.
struct xGlCaps {
	bool es = false;
	int version = 0;		// 33, 41, 30 (es)...
	bool binary = false;		// programs can be saved and loaded
	bool parallel = false;		// KHR_parallel_shader_compile
	bool timer = false;		// GL_TIME_ELAPSED queries
	bool floatTarget = false;	// RGBA16F can be drawn into
	bool border = false;		// CLAMP_TO_BORDER
	QByteArray driver;		// vendor, renderer, version: a part of every cache key
	QString cacheDir;		// empty: no cache
};

class xGlProgram {
	public:
		enum State {Empty, Queued, Building, Ready, Failed};
		// the sources as they go to the driver. A binary of them in the cache makes
		// the program Ready here; otherwise it is Queued
		void set(QOpenGLExtraFunctions* f, const xGlCaps* caps, const QByteArray& vtx, const QByteArray& frg);
		// Queued: compiled and linked. wait: done on return; else the driver is left
		// to it where it can (caps->parallel) and poll() says when
		void build(bool wait);
		State poll();
		State state() const {return st;}
		bool ready() const {return st == Ready;}
		GLuint id() const {return prog;}
		GLint loc(const char* name);		// a uniform's location, kept
		void destroy();
		void forget();			// its context is gone: nothing to delete
		bool cached = false;		// came from the cache
		double ms = 0;			// from set() to Ready
		QString vtxLog;			// the driver's words, per stage and of the link
		QString frgLog;
		QString linkLog;
	private:
		QOpenGLExtraFunctions* gl = nullptr;
		const xGlCaps* caps = nullptr;
		State st = Empty;
		GLuint prog = 0;
		GLuint shd[2] = {0, 0};
		QByteArray src[2];
		QByteArray key;
		qint64 t0 = 0;
		QHash<QByteArray, GLint> locs;
		bool loadBinary();
		void saveBinary();
		void finish();
		void dropShaders();
};

// ---------------------------------------------------------------- the chain

// One frame texture of the window's ring.
struct xFrameTex {
	GLuint tex = 0;
	QSize size;
	xFrameNums nums;
};

// What a paint knows: the frame shown, the ones before it, where the picture goes.
struct xChainFrame {
	xFrameTex cur;
	xFrameTex hist[XSHD_HISTORY_MAX + 1];	// [n]: n frames back, tex 0 = none; [0] unused
	QRect pic;			// the picture in the window, device pixels from the bottom left
	QSize view;			// the window
	QSize dots;			// the shown part in the machine's dots
	int direction = 1;		// -1 while rewinding
};

class xShaderChain : protected QOpenGLExtraFunctions {
	public:
		enum {SEG_PICTURE = XSHD_PASS_MAX, SEG_FILL, SEG_COUNT};
		xGlCaps caps;
		// with the context current; false: the built-in program did not build
		// cacheDir: where the programs' binaries are kept, empty for no cache
		bool init(const QString& cacheDir);
		void destroy();
		// A preset by its path, empty for none. Programs found in the cache are ready
		// on return, the others are built over the next paints (step). values: the
		// parameters that do not stand at the file's defaults
		void load(const QString& path, const xShdValues& values = xShdValues());
		// once a paint, before run(): the programs being built looked at
		void step();
		QString file() const {return preset.file;}
		bool loaded() const {return !preset.passes.isEmpty();}
		bool failed() const {return bad;}
		bool active() const {return live;}		// every pass that is on is ready
		bool pending() const {return loaded() && !bad && !live;}
		QString error() const {return errText;}		// the last load's errors, one a line
		int historyDepth() const {return histDepth;}
		// the passes before the last one; use: the chain draws this paint
		void run(const xChainFrame& frm, bool use);
		// The last pass, or the built-in program, into the viewport the caller has
		// set. place: u_place_; wide: the quad runs past the picture (sfill_sharp).
		// quad draws the caller's quad
		void drawLast(const xChainFrame& frm, const float place[4], bool wide, const std::function<void()>& quad);
		// a program of the window's own (the side fill), through the cache
		void builtin(xGlProgram& p, const QByteArray& vtx, const QByteArray& frg);
		// parameters by name
		bool setParam(const QString& name, float value);
		// the chain wants its frames unmixed now (`frames raw`, its parameter not 0)
		bool rawFrames() const;
		const QVector<xShdParam>& params() const {return preset.params;}
		// GPU time: asked for, the passes and the window's own segments are timed
		void timing(bool on);
		bool timingOn() const {return timed;}
		void segBegin(int seg);
		void segEnd();
		QString timeReport();
		double timeTotal();		// the passes and the picture, us; below 0: not timed
		QString info();
		QStringList notes() const {return preset.notes;}
	private:
		struct Bind {			// one active uniform of a program and what feeds it
			int kind;
			int index;
			GLint loc;
			int unit;		// samplers
			bool asInt;		// FrameCount declared int, as older shaders have it
		};
		struct Target {
			GLuint tex = 0;
			GLuint fbo = 0;
			QSize size;
			GLenum fmt = 0;
		};
		struct Pass {
			xGlProgram prg;
			std::vector<Bind> binds;
			bool bound = false;	// binds made (the program is ready)
			bool on = true;		// its `when` parameter is not 0
			bool feedback = false;	// another pass reads its last output
			Target tgt[2];
			int cur = 0;		// the target drawn into last
			GLenum fmt = 0;
			// this paint
			GLuint outTex = 0;
			QSize outSize;
			bool ran = false;
			bool flipped = false;	// cur is this paint's already: last paint's output is the other
		};
		struct Tex {
			GLuint tex = 0;
			QSize size;
			GLuint sampler = 0;
		};
		struct Seg {
			GLuint q[2] = {0, 0};
			bool asked[2] = {false, false};
			double sum = 0;
			int n = 0;
			bool used = false;
		};
		xShdPreset preset;
		std::vector<Pass> passes;
		std::vector<Tex> textures;
		xGlProgram none;		// no shader: the built-in program
		std::vector<Bind> noneBinds;
		bool bad = false;
		bool live = false;
		bool told = false;
		QString errText;
		int histDepth = 0;
		unsigned frameCount = 0;
		GLuint samplers[2][XSHD_WRAP_COUNT][2];
		GLuint black = 0;		// 1 x 1, transparent: a frame or a feedback not there yet
		GLuint vao = 0;
		GLuint vbo = 0;
		qint64 loadAt = 0;
		// timers
		bool timed = false;
		int timeFrame = 0;
		int segOpen = -1;
		int logTick = 0;
		Seg segs[SEG_COUNT];
		// this paint
		GLuint lastTex = 0;		// the last pass's Source
		QSize lastSize;
		bool useChain = false;

		void clear();
		void sources(int i, QByteArray& vtx, QByteArray& frg);
		QByteArray preamble(int i, bool vertex);
		void startPass(int i);
		void makeBinds(xGlProgram& p, std::vector<Bind>& binds);
		void apply(const std::vector<Bind>& binds, int pass, const xChainFrame& frm,
			GLuint srcTex, const QSize& srcSize, const QSize& outSize, const float* mvp,
			const float place[4], bool wide, bool legacyTex);
		void unbind(const std::vector<Bind>& binds);
		bool target(Pass& p, int idx, const QSize& size, GLenum fmt);
		GLuint sampler(bool linear, int wrap, bool mip);
		void fail(int i);
		QString where(int i, bool vertex) const;
		void logErrors(int i, const QString& log, bool vertex);
		bool passOn(int i) const;
		void refreshOn();
		void checkLive();
		float paramValue(const QString& name) const;
		void collectTimes();
};

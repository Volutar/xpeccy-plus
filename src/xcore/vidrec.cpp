// Video recording through FFmpeg.
//
// FFmpeg runs as a program of its own. The emulation thread hands each picture
// that was shown to a writer thread, which pipes it to the encoder as raw RGB;
// the sound goes to a wav beside it, and on stop a second FFmpeg run puts the
// two together. One live input keeps it to a plain pipe on every system.
//
// The video is a record of what was shown, one picture after another. A frame
// at normal speed is one frame of video. Slow motion and fast forward are
// counted by their speed, fast loading and rewind by the host's clock, since
// they show pictures at a pace of their own; a picture fast loading never drew
// is not in it at all. The sound is fitted to that: whatever was sent to the
// output between two pictures is stretched or squeezed to the time they stand
// for, which at normal speed leaves it untouched.

#include <string.h>

#include <atomic>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <vector>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QProcess>
#include <QHash>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QWaitCondition>

#include "vidrec.h"
#include "xcore.h"
#include "pacing.h"
#include "sound.h"
#include "../libxpeccy/filetypes/filetypes.h"
#include "rewind.h"
#include "../libxpeccy/video/video.h"
#include "../libxpeccy/cpu/Z80/z80.h"
#include "../libxpeccy/xlog.h"

#define VR_QUEUE_MAX	(256 << 20)	// bytes of pictures the encoder may fall behind by
#define VR_PUSH_WAIT	3000		// ms the machine waits for it before a picture is dropped
#define VR_GAP_MAX	25.0		// frames: a pause between two pictures is not time shown
#define VR_TAIL		2048		// bytes of FFmpeg's complaints kept for the log
#define VR_FRAME_NS	20000000.0	// a frame at 50 fps

struct vrJob {
	QByteArray pic;		// RGB0 at the canvas size; empty: the window as last painted
	int copies;		// frames of video it stands for
	double smp;		// samples of sound those frames last
	std::vector<sndPair> snd;
};

class vrWriter : public QThread {
	public:
		QString prog;
		vrecCmd cmd;
	protected:
		void run();
};

static QMutex vrMutex;
static QWaitCondition vrNotEmpty;
static QWaitCondition vrNotFull;
static std::deque<vrJob*> vrQueue;
static qint64 vrQueued = 0;
static bool vrStopping = false;
static QString vrMsg;
static QImage vrScreen;

static std::atomic<int> vrState(VREC_IDLE);
static std::atomic<int> vrOn(0);	// pictures are wanted
static std::atomic<int> vrGen(0);	// one per recording
static vrWriter* vrThread = NULL;

// set before vrOn, read by the emulation thread
static int vrSrc = VREC_SRC_PICTURE;
static int vrW = 0;		// the canvas, in pixels: two to a dot for the picture
static int vrH = 0;

// the emulation thread's own
static int emGen = 0;
static std::vector<sndPair> emSnd;
static double emSlots = 0;
static long long emHost = 0;
static int emSwaps = 0;

static void vr_say(const QString& str) {
	QMutexLocker lock(&vrMutex);
	vrMsg = str;
}

QString vrec_message() {
	QMutexLocker lock(&vrMutex);
	QString res = vrMsg;
	vrMsg.clear();
	return res;
}

int vrec_state() {
	return vrState.load();
}

// FFMPEG

#ifdef _WIN32
	#define FFMPEG_EXE	"ffmpeg.exe"
#else
	#define FFMPEG_EXE	"ffmpeg"
#endif

QString vrec_ffmpeg() {
	if (conf.rec.ffmpeg.empty()) return vrec_ffmpeg_auto();
	QString path = QString::fromLocal8Bit(conf.rec.ffmpeg.c_str());
	return QFileInfo(path).isFile() ? path : QString();
}

// where a copy fetched for the user goes
QString vrec_ffmpeg_dir() {
	return QString::fromLocal8Bit(conf.path.confDir.c_str()) + "/ffmpeg/";
}

// a program on PATH, and on macOS in Homebrew, which a program started from
// the Finder has no PATH to
QString vrec_find_tool(const QString& name) {
	QString path = QStandardPaths::findExecutable(name);
#ifdef __APPLE__
	if (path.isEmpty()) path = QStandardPaths::findExecutable(name, QStringList() << "/opt/homebrew/bin" << "/usr/local/bin");
#endif
	return path;
}

// the copy fetched for the user, else PATH
QString vrec_ffmpeg_auto() {
	QString dir = vrec_ffmpeg_dir();
	foreach(QString sub, QStringList() << "" << "bin/") {
		if (QFileInfo(dir + sub + FFMPEG_EXE).isFile())
			return QDir::cleanPath(dir + sub + FFMPEG_EXE);
	}
	return vrec_find_tool("ffmpeg");
}

// "ffmpeg version 8.0.1-full_build-www.gyan.dev" or "... n9.0.2-14-gebafaee10a":
// a release keeps its number, anything else is shown whole
QString vrec_ffmpeg_release(const QString& line) {
	QString ver = line.section(' ', 2, 2);
	if ((ver.size() > 1) && (ver.at(0) == 'n') && ver.at(1).isDigit()) ver.remove(0, 1);
	if (!ver.isEmpty() && ver.at(0).isDigit()) ver = ver.section('-', 0, 0);
	return ver;
}

// asked once per program: Options opens with it
QString vrec_ffmpeg_version(const QString& path) {
	static QHash<QString, QString> known;
	if (path.isEmpty()) return QString();
	if (known.contains(path)) return known[path];
	QProcess prc;
	prc.start(path, QStringList() << "-hide_banner" << "-version");
	QString line;
	if (prc.waitForFinished(5000)) {
		line = QString::fromLocal8Bit(prc.readAllStandardOutput()).section('\n', 0, 0).trimmed();
		if (!line.startsWith("ffmpeg")) line.clear();
	} else {
		prc.kill();
		prc.waitForFinished(1000);
	}
	known[path] = line;
	return line;
}

QString vrec_dir() {
	return conf.rec.dir.empty() ? vrec_dir_auto() : QString::fromLocal8Bit(conf.rec.dir.c_str());
}

QString vrec_dir_auto() {
	QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	return dir.isEmpty() ? QDir::homePath() : dir;
}

static QString vr_ext(const xRecord& rec) {
	return ((rec.container == VREC_MKV) || (rec.codec == VREC_FFV1)) ? "mkv" : "mp4";
}

// what the user typed, split as a shell would
static QStringList vr_split(const QString& str) {
	QStringList res;
	QString cur;
	QChar quote;
	bool any = false;
	foreach(QChar ch, str) {
		if (!quote.isNull()) {
			if (ch == quote) quote = QChar();
			else cur.append(ch);
		} else if ((ch == '"') || (ch == '\'')) {
			quote = ch;
			any = true;
		} else if (ch.isSpace()) {
			if (any || !cur.isEmpty()) res.append(cur);
			cur.clear();
			any = false;
		} else {
			cur.append(ch);
		}
	}
	if (any || !cur.isEmpty()) res.append(cur);
	return res;
}

// the user's options for a part of the command, or the ones made for it
static QStringList vr_or(const std::string& over, const QStringList& made) {
	QStringList res = vr_split(QString::fromLocal8Bit(over.c_str()));
	return res.isEmpty() ? made : res;
}

static QString vr_even(int v) {
	return QString::number(v & ~1);
}

const char* vrecPresets[] = {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", NULL};

// the speed as an index into vrecPresets, veryfast when it is none of them
static int vr_preset(const xRecord& rec) {
	for (int i = 0; vrecPresets[i]; i++)
		if (rec.preset == vrecPresets[i]) return i;
	return 2;
}

// What each VREC_* codec is. Some take 4:2:0 alone: AMF, QSV and AV1. Those that
// a build may have without the machine running them are tried before they are
// offered (see vrec_codec_works).
enum {VF_X264 = 0, VF_X265, VF_FFV1, VF_NVENC, VF_AMF, VF_QSV, VF_SVT};

static const struct {
	const char* ff;		// FFmpeg's name for it
	int family;
	bool hevc;
	bool av1;
	bool yuv420;
	bool tried;
} vrCodec[] = {
	{"libx264", VF_X264, false, false, false, false},	// VREC_H264
	{"libx265", VF_X265, true, false, false, false},	// VREC_H265
	{"ffv1", VF_FFV1, false, false, false, false},		// VREC_FFV1
	{"h264_nvenc", VF_NVENC, false, false, false, true},
	{"hevc_nvenc", VF_NVENC, true, false, false, true},
	{"h264_amf", VF_AMF, false, false, true, true},
	{"hevc_amf", VF_AMF, true, false, true, true},
	{"h264_qsv", VF_QSV, false, false, true, true},
	{"hevc_qsv", VF_QSV, true, false, true, true},
	{"libsvtav1", VF_SVT, false, true, true, true},		// VREC_AV1: not in every build
	{"av1_nvenc", VF_NVENC, false, true, true, true},
	{"av1_amf", VF_AMF, false, true, true, true},
	{"av1_qsv", VF_QSV, false, true, true, true},
};
static_assert(sizeof(vrCodec) / sizeof(vrCodec[0]) == VREC_AV1_QSV + 1, "a codec without its row");

static int vr_codec(int codec) {
	return ((codec < 0) || (codec >= (int)(sizeof(vrCodec) / sizeof(vrCodec[0])))) ? VREC_H264 : codec;
}

bool vrec_420_only(int codec, int chroma) {
	codec = vr_codec(codec);
	return vrCodec[codec].yuv420 || ((chroma == VREC_CH_420) && (vrCodec[codec].family != VF_FFV1));
}

// 4:2:0 at an odd scale puts two dots' colours in one: it gets the even
// scales alone, and an odd one goes up to the next
int vrec_scale_for(int codec, int chroma, int n) {
	n = qBound(1, n, VREC_SCALE_MAX);
	if (vrec_420_only(codec, chroma) && (n & 1)) n = qMin(n + 1, VREC_SCALE_MAX);
	return n;
}

// 4:2:0 plays everywhere, and 4:4:4 is kept for where 4:2:0 spoils the picture:
// it shares one colour between 2x2 pixels, which at an odd scale is two dots,
// and it halves a shader's grid, which is mostly colour (29 dB at any crf)
int vrec_chroma(const xRecord& rec) {
	int codec = vr_codec(rec.codec);
	if (vrCodec[codec].family == VF_FFV1) return VREC_RGB;
	if (vrec_420_only(codec, rec.chroma)) return VREC_420;
	if (rec.chroma == VREC_CH_444) return VREC_444;
	if (rec.source == VREC_SRC_SCREEN) return VREC_444;
	return (qBound(1, rec.scale, VREC_SCALE_MAX) & 1) ? VREC_444 : VREC_420;
}

// Auto is AAC in MP4, which the system players and the editors take, and Opus
// in MKV, where it is at home
int vrec_acodec(const xRecord& rec) {
	if (rec.acodec != VREC_AUDIO_AUTO) return rec.acodec;
	return (vr_ext(rec) == "mkv") ? VREC_OPUS : VREC_AAC;
}

// what the encoder is handed: FFV1 keeps the RGB, the cards want nv12
static const char* vr_pix_fmt(const xRecord& rec) {
	int family = vrCodec[vr_codec(rec.codec)].family;
	switch (vrec_chroma(rec)) {
		case VREC_RGB: return "bgr0";
		case VREC_444: return "yuv444p";
	}
	return ((family == VF_X264) || (family == VF_X265) || (family == VF_SVT)) ? "yuv420p" : "nv12";
}

// The encoder and its quality and speed. The cards take a constant quantizer as
// -crf is, measured on NVENC and AMF: rate controls that aim at a quality stop
// getting better below about 18. The speed goes onto each one's own presets in
// x264's order. AV1 counts its quantizer 0..255, and 2.8 times the setting
// matches x264's picture at 18 (NVENC AV1, measured).
static QStringList vr_encoder(const xRecord& rec) {
	int codec = vr_codec(rec.codec);
	int pre = vr_preset(rec);
	QString q = QString::number(vrCodec[codec].av1 ? qMin(255, (int)lround(rec.crf * 2.8)) : rec.crf);
	QString preset = QString::fromStdString(rec.preset);
	QStringList res;
	res << "-c:v" << vrCodec[codec].ff;
	switch (vrCodec[codec].family) {
		case VF_X264:
			res << "-preset" << preset << "-crf" << q;
			break;
		case VF_X265:
			res << "-preset" << preset << "-crf" << q << "-x265-params" << "log-level=error";
			break;
		case VF_NVENC:
			res << "-preset" << QString("p%0").arg(pre + 1) << "-rc" << "constqp" << "-qp" << q;
			if (vrec_chroma(rec) == VREC_444)
				res << "-profile:v" << (vrCodec[codec].hevc ? "rext" : "high444p");
			break;
		case VF_AMF:
			res << "-quality" << ((pre < 3) ? "speed" : (pre < 5) ? "balanced" : "quality");
			res << "-rc" << "cqp" << "-qp_i" << q << "-qp_p" << q;
			if (codec == VREC_H264_AMF) res << "-qp_b" << q;	// the others have no B frames here
			break;
		case VF_QSV:
			res << "-preset" << vrecPresets[qMax(pre, 2)] << "-global_quality" << q;	// its presets start at veryfast
			break;
		case VF_SVT:
			// SVT's fast presets blur pixel art whatever the crf (34 dB at p10,
			// 42 at p8): veryfast is p8, and the others step from there
			res << "-preset" << QString::number(qBound(4, 10 - pre, 10)) << "-crf" << QString::number(qMin(63, rec.crf));
			break;
	}
	return res;
}

// One frame through the encoder: a build lists the card encoders whether or
// not there is a card to run them on. What the build has not got at all is
// known from one list of its encoders. Asked once per program; the first use
// of a card is slow.
bool vrec_codec_works(const QString& prog, int codec) {
	static QHash<QString, QString> lists;
	static QHash<QString, bool> known;
	codec = vr_codec(codec);
	if (!vrCodec[codec].tried) return true;
	if (prog.isEmpty()) return false;
	QString key = QString("%0|%1").arg(prog).arg(codec);
	if (known.contains(key)) return known[key];
	if (!lists.contains(prog)) {
		QProcess prc;
		prc.start(prog, QStringList() << "-hide_banner" << "-encoders");
		prc.waitForFinished(5000);
		lists[prog] = QString::fromLocal8Bit(prc.readAllStandardOutput());
	}
	bool ok = false;
	if (lists[prog].contains(QString(" %0 ").arg(vrCodec[codec].ff))) {
		xRecord rec = conf.rec;
		rec.codec = codec;
		rec.chroma = VREC_CH_420;	// the verdict is the codec's, not the settings'
		QStringList args;
		args << "-hide_banner" << "-v" << "error" << "-f" << "lavfi" << "-i" << "color=c=black:s=256x256:d=0.1";
		args << "-frames:v" << "1" << "-pix_fmt" << vr_pix_fmt(rec) << vr_encoder(rec) << "-f" << "null" << "-";
		QProcess prc;
		prc.start(prog, args);
		ok = prc.waitForFinished(20000) && (prc.exitStatus() == QProcess::NormalExit) && (prc.exitCode() == 0);
		if (prc.state() != QProcess::NotRunning) {
			prc.kill();
			prc.waitForFinished(1000);
		}
	}
	xlog(XLG_VIDEO, XLL_INFO, "%s: %s", vrCodec[codec].ff, ok ? "works" : "not here");
	known[key] = ok;
	return ok;
}

// what keeps FFmpeg quiet, left out of the command Options shows
static void vr_quiet(QStringList& args, bool quiet) {
	if (quiet) args << "-hide_banner" << "-nostats" << "-loglevel" << "error" << "-y";
}

// Both FFmpeg runs. w, h: the pictures handed over, two pixels to a dot for
// the emulated one; ns: a machine frame; path: the file, without extension.
vrecCmd vrec_command(const xRecord& rec, int w, int h, int ns, int rate, const QString& path, bool quiet) {
	vrecCmd cmd;
	QString ext = vr_ext(rec);
	cmd.out = path + "." + ext;
	cmd.tmpVideo = path + ".part." + ext;
	cmd.tmpAudio = path + ".part.wav";
	cmd.wavRate = rate;

	// the encoder: raw pictures in, video alone out
	QString fps = (rec.fps == VREC_FPS_50) ? QString("50") : QString("1000000000/%0").arg(ns);
	int ow = w;
	int oh = h;
	if (rec.source == VREC_SRC_PICTURE) {
		int n = vrec_scale_for(rec.codec, rec.chroma, rec.scale);
		ow = w / 2 * n;
		oh = h * n;
	}
	bool lossless = (rec.codec == VREC_FFV1);
	// neighbour, so a dot stays a block of whole pixels, and the colours
	// converted the way an HD player reads them back
	QString vf = QString("scale=%0:%1:flags=neighbor").arg(vr_even(ow)).arg(vr_even(oh));
	if (!lossless) vf += ":out_color_matrix=bt709";
	vf += QString(",format=") + vr_pix_fmt(rec);
	if (rec.fps60) vf += ",framerate=fps=60";
	// what the settings make of the picture; an override stands in for all of it
	QStringList& ev = cmd.video;
	ev << "-vf" << vf;
	ev << vr_encoder(rec);
	if (!lossless)
		ev << "-color_primaries" << "bt709" << "-color_trc" << "bt709" << "-colorspace" << "bt709";
	// the input and the file are the recorder's own whatever the user says
	QStringList& ea = cmd.enc;
	vr_quiet(ea, quiet);
	ea << "-f" << "rawvideo" << "-pix_fmt" << "rgb0" << "-s" << QString("%0x%1").arg(w).arg(h);
	ea << "-framerate" << fps << "-i" << "-";
	ea << vr_or(rec.videoOver, ev);
	ea << vr_split(QString::fromLocal8Bit(rec.extra.c_str()));
	ea << "-an" << cmd.tmpVideo;

	// the second run: the sound put beside it
	QStringList& ma = cmd.mux;
	vr_quiet(ma, quiet);
	ma << "-i" << cmd.tmpVideo << "-i" << cmd.tmpAudio;
	ma << "-map" << "0:v:0" << "-map" << "1:a:0" << "-c:v" << "copy";
	QStringList& ms = cmd.sound;
	if (rec.fps == VREC_FPS_50) {
		// a machine frame lasts 20 ms of video: the sound follows it. Its
		// pitch goes with the wav's header, so an override keeps that
		double ratio = ns / VR_FRAME_NS;
		if (rec.keepPitch) {
			ms << "-af" << QString("atempo=%0").arg(ratio, 0, 'f', 6);
		} else {
			cmd.wavRate = (int)lround(rate * ratio);
			ms << "-ar" << "48000";
		}
	}
	if (lossless) {
		ms << "-c:a" << "flac";
	} else {
		bool opus = (vrec_acodec(rec) == VREC_OPUS);
		ms << "-c:a" << (opus ? "libopus" : "aac") << "-b:a" << QString("%0k").arg(rec.abitrate);
		// FFmpeg before 4.3 calls opus in mp4 experimental and refuses it without this
		if (opus && (ext == "mp4")) ms << "-strict" << "-2";
	}
	ma << vr_or(rec.soundOver, ms);
	if (ext == "mp4") {
		if (vrCodec[vr_codec(rec.codec)].hevc)
			ma << "-tag:v" << "hvc1";	// or Apple's players refuse it
		ma << "-movflags" << "+faststart";
	}
	ma << cmd.out;
	return cmd;
}

// one argument as it would be typed, so vr_split() reads it back
static QString vr_quote(const QString& arg) {
	return (arg.contains(' ') || arg.isEmpty()) ? "\"" + arg + "\"" : arg;
}

// as it would be typed, without the program's name
QString vrec_args_line(const QStringList& args) {
	QStringList res;
	foreach(const QString& arg, args) res << vr_quote(arg);
	return res.join(' ');
}

// as it would be typed; lines: a line to each part - the input, the filters,
// the codec, the colour tags, the output
QString vrec_command_line(const QStringList& args, bool lines) {
	static const QStringList parts = QStringList() << "-vf" << "-c:v" << "-color_primaries" << "-an" << "-map" << "-movflags";
	QString res("ffmpeg");
	QString opt;
	foreach(const QString& arg, args) {
		// -map -map on one line, but every input on its own
		bool brk = lines && !opt.isEmpty() && (parts.contains(arg) || (arg == "-i")) && ((arg != opt) || (arg == "-i"));
		bool chain = lines && (opt == "-vf");
		if (arg.startsWith('-')) opt = arg;
		QString txt = vr_quote(arg);
		if (chain) txt.replace(",", ",\n      ");		// a filter to a line
		res += (brk ? "\n  " : " ") + txt;
	}
	return res;
}

// The file name, extension aside: %d the date, %t the time, %image the image
// in use (else the machine), %machine the machine. One pass, so a key that
// turns up in an image's name is left as it is.
QString vrec_file_name(const QString& tpl, const QString& image, const QDateTime& when) {
	QString src = tpl.trimmed().isEmpty() ? QString(VREC_NAME_DEF) : tpl.trimmed();
	static const QRegularExpression key("%(image|machine|d|t)");
	QString res;
	int pos = 0;
	QRegularExpressionMatchIterator it = key.globalMatch(src);
	while (it.hasNext()) {
		QRegularExpressionMatch m = it.next();
		res += src.mid(pos, m.capturedStart() - pos);
		QString k = m.captured(1);
		if (k == "d") res += when.toString("yyyyMMdd");
		else if (k == "t") res += when.toString("HHmmss");
		else if (k == "image") res += image;
		else res += QString::fromStdString(conf.macId);
		pos = m.capturedEnd();
	}
	res += src.mid(pos);
	static const QRegularExpression bad("[<>:\"/\\\\|?*\\x00-\\x1f]");	// what no file system takes
	res.replace(bad, "_");
	res = res.trimmed();
	return res.isEmpty() ? when.toString("yyyyMMdd_HHmmss") : res;
}

// AUTO

std::atomic<int> vrecWatch(0);
static std::atomic<int> vaStartSeen(0);	// the emulation met the start condition
static std::atomic<int> vaStopSeen(0);	// ...or the stop one
static int vaResets = -1;		// the user's resets seen so far

// One end, start or stop: on, and at what. The command line's say for this
// run lies over the settings (-1: none), and the two are resolved once into
// what the emulation reads at every opcode.
typedef struct {
	int on;
	int op;		// VREC_AT_*
	int adr;
} vaEnd;

enum {VA_START = 0, VA_STOP};
static vaEnd vaCli[2] = {{-1, -1, 0}, {-1, -1, 0}};
static vaEnd vaEff[2];

static void va_resolve() {
	vaEff[VA_START] = {conf.rec.autoStart, conf.rec.autoStartOp, conf.rec.autoStartAdr};
	vaEff[VA_STOP] = {conf.rec.autoStop, conf.rec.autoStopOp, conf.rec.autoStopAdr};
	for (int i = 0; i < 2; i++) {
		if (vaCli[i].on >= 0) vaEff[i].on = vaCli[i].on;
		if (vaCli[i].op >= 0) {
			vaEff[i].op = vaCli[i].op;
			vaEff[i].adr = vaCli[i].adr;
		}
	}
}

static const char* vaOps[] = {"", "==", ">=", "<="};	// in VREC_AT_* order

bool vrec_auto_parse(const QString& str, int* op, int* adr) {
	QString txt = str.trimmed();
	if ((txt.compare("ram", Qt::CaseInsensitive) == 0) || (txt.compare("reset", Qt::CaseInsensitive) == 0)) {
		*op = VREC_AT_OWN;
		return true;
	}
	int o = VREC_AT_EQ;
	for (int i = VREC_AT_EQ; i <= VREC_AT_LE; i++) {
		if (txt.startsWith(vaOps[i])) {
			o = i;
			txt = txt.mid(2);
			break;
		}
	}
	bool ok;
	int a = unreal_num(txt, &ok);
	if (!ok || (a < 0) || (a > 0xffff)) return false;
	*op = o;
	*adr = a;
	return true;
}

// own: what VREC_AT_OWN is called at this end, "ram" or "reset"
QString vrec_auto_text(int op, int adr, const char* own) {
	if ((op <= VREC_AT_OWN) || (op > VREC_AT_LE)) return own;
	return QString("%0#%1").arg(vaOps[op], gethexword(adr));
}

void vrec_auto_cli(int on) {
	vaCli[VA_START].on = on;
	vaCli[VA_STOP].on = on;
}

static bool va_cli_at(int end, const QString& str) {
	if (!vrec_auto_parse(str, &vaCli[end].op, &vaCli[end].adr)) return false;
	vaCli[end].on = 1;
	return true;
}

bool vrec_auto_cli_start(const QString& str) {
	return va_cli_at(VA_START, str);
}

bool vrec_auto_cli_stop(const QString& str) {
	return va_cli_at(VA_STOP, str);
}

// The ROMs run code of their own in ram: 128K BASIC its paging routines in the
// printer buffer, TR-DOS the RET it keeps at #5CC2. A program starts at PROG
// (#5CCB) or above, so the system area is not counted.
#define VA_SYS_FROM	0x5b00
#define VA_SYS_TO	0x5ccb

static bool va_pc_is(const vaEnd& end, Computer* comp, int pc) {
	switch (end.op) {
		case VREC_AT_EQ: return pc == end.adr;
		case VREC_AT_GE: return pc >= end.adr;
		case VREC_AT_LE: return pc <= end.adr;
	}
	if ((pc >= VA_SYS_FROM) && (pc < VA_SYS_TO)) return false;
	return mem_get_page(comp->mem, pc)->type == MEM_RAM;
}

void vrec_auto_pc(Computer* comp, int watch) {
	int pc = comp->cpu->regPC;
	if ((watch & VREC_WATCH_START) && va_pc_is(vaEff[VA_START], comp, pc)) {
		vrecWatch.fetch_and(~VREC_WATCH_START);
		vaStartSeen.store(1);
		xlog(XLG_VIDEO, XLL_INFO, "auto recording: start at #%.4X", pc);
	}
	if ((watch & VREC_WATCH_STOP) && va_pc_is(vaEff[VA_STOP], comp, pc)) {
		vrecWatch.fetch_and(~VREC_WATCH_STOP);
		vaStopSeen.store(1);
		xlog(XLG_VIDEO, XLL_INFO, "auto recording: stop at #%.4X", pc);
	}
}

// the start is armed while nothing is being recorded, a recording going on is left alone
static void va_arm(bool now) {
	if (vaEff[VA_START].on && now) {
		vrecWatch.fetch_or(VREC_WATCH_START);
	} else {
		vrecWatch.fetch_and(~VREC_WATCH_START);
	}
}

void vrec_auto_apply() {
	va_resolve();
	vaResets = xUserResets.load();
	vaStartSeen.store(0);
	va_arm(vrState.load() == VREC_IDLE);
}

// a recording has begun: the pc is watched for its end, if that is how it ends
static void va_began() {
	vaStopSeen.store(0);
	if (vaEff[VA_STOP].on && (vaEff[VA_STOP].op != VREC_AT_OWN))
		vrecWatch.fetch_or(VREC_WATCH_STOP);
}

int vrec_auto_tick() {
	int resets = xUserResets.load();
	if (resets != vaResets) {
		vaResets = resets;
		vaStartSeen.store(0);
		// a reset ends the recording when that is its stop, and arms the start
		// again unless something is left recording
		bool running = (vrState.load() == VREC_RUN);
		bool stop = running && vaEff[VA_STOP].on && (vaEff[VA_STOP].op == VREC_AT_OWN);
		va_arm(stop || !running);
		if (stop) return VREC_AUTO_STOP;
	}
	if (vaStopSeen.load()) {
		vaStopSeen.store(0);
		if (vrState.load() == VREC_RUN) return VREC_AUTO_STOP;
	}
	// the last one is still being written after a reset: this one waits for it
	if (vaStartSeen.load() && (vrState.load() == VREC_IDLE)) {
		vaStartSeen.store(0);
		return VREC_AUTO_START;
	}
	return VREC_AUTO_NONE;
}

// The settings changed. A start switched on arms at the next reset, not now:
// a game already running is in ram, and would start a recording at Apply.
void vrec_auto_settings() {
	va_resolve();
	if (!vaEff[VA_START].on) va_arm(false);
}

void vrec_manual() {
	vaStartSeen.store(0);
	vrecWatch.fetch_and(~VREC_WATCH_START);
}

// START / STOP

bool vrec_start(Computer* comp, const QString& base, int scrW, int scrH, QString* err, const QString& file) {
	if (vrState.load() != VREC_IDLE) {
		*err = "the last video is still being written";
		return false;
	}
	if (vrThread) {		// done with, but not yet let go of
		vrThread->wait();
		delete vrThread;
		vrThread = NULL;
	}
	QString prog = vrec_ffmpeg();
	if (prog.isEmpty()) {
		*err = "FFmpeg not found, see Options";
		return false;
	}
	// a file named on the command line: its own folder, its container by the extension
	QString dir = file.isEmpty() ? vrec_dir() : QFileInfo(file).absolutePath();
	if (!QDir().mkpath(dir)) {
		*err = "cannot make the folder for videos";
		return false;
	}
	Video* vid = comp->vid;
	xRecord rec = conf.rec;
	rec.source = (scrW > 0) ? VREC_SRC_SCREEN : VREC_SRC_PICTURE;
	int w = (rec.source == VREC_SRC_SCREEN) ? (scrW & ~1) : vid->vsze.x * 2;
	int h = (rec.source == VREC_SRC_SCREEN) ? (scrH & ~1) : vid->vsze.y;
	if ((w < 2) || (h < 2)) {
		*err = "no picture to record";
		return false;
	}
	QString name = vrec_file_name(QString::fromLocal8Bit(conf.rec.name.c_str()), base, QDateTime::currentDateTime());
	if (!file.isEmpty()) {
		QString ext = QFileInfo(file).suffix().toLower();
		if (ext == "mkv") rec.container = VREC_MKV;
		if (ext == "mp4") rec.container = VREC_MP4;
		name = QFileInfo(file).completeBaseName();
	}
	vrecCmd cmd = vrec_command(rec, w, h, vid->nsPerFrame, conf.snd.rate, QDir(dir).filePath(name));
	vrWriter* wr = new vrWriter;
	wr->prog = prog;
	wr->cmd = cmd;

	{
		QMutexLocker lock(&vrMutex);
		vrStopping = false;
		vrMsg.clear();
		vrScreen = QImage();
	}
	vrSrc = rec.source;
	vrW = w;
	vrH = h;
	vrThread = wr;
	vrState.store(VREC_RUN);
	vrGen.fetch_add(1);
	vrOn.store(1, std::memory_order_release);
	va_began();
	xlog(XLG_VIDEO, XLL_INFO, "recording to %s", cmd.out.toLocal8Bit().data());
	xlog(XLG_VIDEO, XLL_DEBUG, "%s", vrec_command_line(cmd.enc).toLocal8Bit().data());
	wr->start();
	return true;
}

void vrec_stop() {
	if (vrState.load() != VREC_RUN) return;
	vrOn.store(0);
	vrecWatch.fetch_and(~VREC_WATCH_STOP);
	vrState.store(VREC_FINISH);
	QMutexLocker lock(&vrMutex);
	vrStopping = true;
	vrNotEmpty.wakeAll();
	vrNotFull.wakeAll();
}

void vrec_wait() {
	vrec_stop();
	if (vrThread) {
		vrThread->wait();
		delete vrThread;
		vrThread = NULL;
	}
}

// THE WINDOW

bool vrec_wants_screen() {
	return vrOn.load() && (vrSrc == VREC_SRC_SCREEN);
}

void vrec_screen(const QImage& img) {
	QMutexLocker lock(&vrMutex);
	vrScreen = img;
}

// THE EMULATION THREAD

static void em_sync() {
	int gen = vrGen.load();
	if (gen == emGen) return;
	emGen = gen;
	emSnd.clear();
	emSlots = 0;
	emHost = paceClockNs();
	emSwaps = bufSwaps;
}

void vrec_sample(sndPair lev) {
	if (!vrOn.load(std::memory_order_acquire)) return;
	em_sync();
	emSnd.push_back(lev);
}

// the shown part of the raster, centred on the canvas: fullscreen can widen it
static void grab_picture(Video* vid, QByteArray& out) {
	out.resize(vrW * vrH * 4);
	int sw = vid->vsze.x * 2;
	int sh = vid->vsze.y;
	if ((sw != vrW) || (sh != vrH)) out.fill(0);
	int cw = qMin(sw, vrW);
	int ch = qMin(sh, vrH);
	int sx = ((sw - cw) / 2) & ~1;
	int dx = ((vrW - cw) / 2) & ~1;
	const unsigned char* src = bufimg + (vid->lcut.y + (sh - ch) / 2) * bytesPerLine + (vid->lcut.x * 2 + sx) * 4;
	unsigned char* dst = (unsigned char*)out.data() + (((vrH - ch) / 2) * vrW + dx) * 4;
	for (int y = 0; y < ch; y++) {
		memcpy(dst, src, cw * 4);
		src += bytesPerLine;
		dst += vrW * 4;
	}
}

static void vr_push(vrJob* job) {
	qint64 sz = (qint64)vrW * vrH * 4;
	QMutexLocker lock(&vrMutex);
	// the encoder falls behind: the machine slows down rather than lose pictures
	while ((vrQueued > VR_QUEUE_MAX) && !vrStopping) {
		if (!vrNotFull.wait(&vrMutex, VR_PUSH_WAIT)) {
			xlog(XLG_VIDEO, XLL_WARN, "the encoder is not keeping up, a picture is dropped");
			delete job;
			return;
		}
	}
	if (vrStopping) {
		delete job;
		return;
	}
	vrQueue.push_back(job);
	vrQueued += sz;
	vrNotEmpty.wakeOne();
}

void vrec_frame(Computer* comp) {
	if (!vrOn.load(std::memory_order_acquire)) return;
	em_sync();
	if (bufSwaps == emSwaps) return;	// nothing new drawn
	emSwaps = bufSwaps;
	Video* vid = comp->vid;
	long long now = paceClockNs();
	double span;
	if (conf.emu.fast || rewind_active()) {
		span = double(now - emHost) / vid->nsPerFrame;
		if (span > VR_GAP_MAX) span = VR_GAP_MAX;
	} else {
		span = 1.0 / conf.emu.speed;
	}
	emHost = now;
	emSlots += span;
	int copies = (int)(emSlots + 1e-6);
	if (copies < 1) return;		// its sound goes with the next one
	emSlots -= copies;
	vrJob* job = new vrJob;
	job->copies = copies;
	job->smp = copies * (double)conf.snd.rate * vid->nsPerFrame / 1e9;
	job->snd.swap(emSnd);
	emSnd.reserve(job->snd.size());		// the next frame takes about as many
	if (vrSrc == VREC_SRC_PICTURE)
		grab_picture(vid, job->pic);
	vr_push(job);
}

// THE WRITER


static void wav_put(std::vector<short>& buf, sndPair lev) {
	buf.push_back((short)qBound(-32768, lev.left, 32767));
	buf.push_back((short)qBound(-32768, lev.right, 32767));
}

// the window as last painted, at the canvas size and the right way up
static QByteArray screen_frame(const QImage& img) {
	QByteArray res(vrW * vrH * 4, 0);
	if (img.isNull()) return res;
	QImage pic = img;
	if ((pic.width() != vrW) || (pic.height() != vrH))
		pic = pic.scaled(vrW, vrH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	pic = pic.convertToFormat(QImage::Format_RGBA8888);
	for (int y = 0; y < vrH; y++)
		memcpy(res.data() + y * vrW * 4, pic.constScanLine(vrH - 1 - y), vrW * 4);
	return res;
}

// the writer gives up: nothing more is taken, and the loop drains what is queued
static void vr_fail(const QString& msg) {
	vr_say(msg);
	vrOn.store(0);
	vrecWatch.fetch_and(~VREC_WATCH_STOP);
	QMutexLocker lock(&vrMutex);
	vrStopping = true;
	vrState.store(VREC_FINISH);
	vrNotFull.wakeAll();
}

void vrWriter::run() {
	QByteArray tail;
	QProcess enc;
	enc.setStandardOutputFile(QProcess::nullDevice());
	enc.start(prog, cmd.enc);
	bool ok = enc.waitForStarted(10000);
	if (!ok) vr_fail(" video: FFmpeg did not start ");
	FILE* wav = ok ? fopen(cmd.tmpAudio.toLocal8Bit().data(), "wb") : NULL;
	if (ok && !wav) {
		ok = false;
		vr_fail(" video: cannot write to its folder ");
	}
	if (wav) {
		wavHead hd = wav_prepare(cmd.wavRate, 2);
		fwrite(&hd, sizeof(wavHead), 1, wav);
	}
	unsigned int wavBytes = 0;
	long long frames = 0;
	long long written = 0;
	double expect = 0;
	std::vector<short> abuf;
	QByteArray screenPic;		// black until the window is first painted
	qint64 screenKey = -1;
	for (;;) {
		vrJob* job;
		QImage screen;
		{
			QMutexLocker lock(&vrMutex);
			while (vrQueue.empty() && !vrStopping)
				vrNotEmpty.wait(&vrMutex);
			if (vrQueue.empty()) break;
			job = vrQueue.front();
			vrQueue.pop_front();
			vrQueued -= (qint64)vrW * vrH * 4;
			vrNotFull.wakeAll();
			if (job->pic.isEmpty()) screen = vrScreen;
		}
		// converted out of the lock: the window waits for it to paint the next
		if (!screen.isNull() && (screen.cacheKey() != screenKey)) {
			screenKey = screen.cacheKey();
			screenPic = screen_frame(screen);
		}
		if (ok) {
			// the sound, fitted to the time the pictures stand for
			expect += job->smp;
			long long want = llround(expect) - written;
			if (want < 0) want = 0;
			long long n = (long long)job->snd.size();
			abuf.clear();
			if ((n > 0) && (std::llabs(n - want) <= 2 + want / 100)) {
				for (long long i = 0; i < n; i++) wav_put(abuf, job->snd[i]);
			} else if (n > 0) {
				for (long long i = 0; i < want; i++) {
					double pos = (want > 1) ? (double)i * (n - 1) / (want - 1) : 0;
					long long a = (long long)pos;
					long long b = qMin(a + 1, n - 1);
					double f = pos - a;
					sndPair lev;
					lev.left = (int)(job->snd[a].left * (1 - f) + job->snd[b].left * f);
					lev.right = (int)(job->snd[a].right * (1 - f) + job->snd[b].right * f);
					wav_put(abuf, lev);
				}
			} else {
				abuf.assign(want * 2, 0);
			}
			written += abuf.size() / 2;
			if (!abuf.empty()) {
				fwrite(abuf.data(), 2, abuf.size(), wav);
				wavBytes += abuf.size() * 2;
			}
			// the picture, as many times as it lasts
			if (screenPic.isEmpty()) screenPic = screen_frame(QImage());
			const QByteArray& pic = job->pic.isEmpty() ? screenPic : job->pic;
			for (int i = 0; ok && (i < job->copies); i++) {
				enc.write(pic);
				while (ok && (enc.bytesToWrite() > 0)) {
					if (!enc.waitForBytesWritten(10000)) ok = false;
				}
				frames++;
			}
			tail = (tail + enc.readAllStandardError()).right(VR_TAIL);
			if (!ok || (enc.state() != QProcess::Running)) {
				ok = false;
				vr_fail(" video: FFmpeg stopped, see the log ");
			}
		}
		delete job;
	}
	enc.closeWriteChannel();
	if (!enc.waitForFinished(-1)) enc.kill();
	tail = (tail + enc.readAllStandardError()).right(VR_TAIL);
	if (wav) {
		fseek(wav, 4, SEEK_SET);
		fputi(wavBytes + sizeof(wavHead) - 8, wav);
		fseek(wav, sizeof(wavHead) - 4, SEEK_SET);
		fputi(wavBytes, wav);
		fclose(wav);
	}
	bool encOk = ok && (enc.exitStatus() == QProcess::NormalExit) && (enc.exitCode() == 0);
	if (ok && !encOk)
		vr_say(" video: FFmpeg failed, see the log ");
	if (!tail.trimmed().isEmpty())
		xlog(XLG_VIDEO, encOk ? XLL_INFO : XLL_ERROR, "ffmpeg: %s", tail.trimmed().data());
	if (encOk && (frames == 0)) {
		vr_say(" video: nothing recorded ");
		encOk = false;
	}
	if (encOk) {
		QProcess mux;
		mux.setStandardOutputFile(QProcess::nullDevice());
		xlog(XLG_VIDEO, XLL_DEBUG, "ffmpeg %s", cmd.mux.join(' ').toLocal8Bit().data());
		mux.start(prog, cmd.mux);
		bool done = mux.waitForStarted(10000) && mux.waitForFinished(-1)
			&& (mux.exitStatus() == QProcess::NormalExit) && (mux.exitCode() == 0);
		QByteArray err = mux.readAllStandardError().trimmed();
		if (!err.isEmpty())
			xlog(XLG_VIDEO, done ? XLL_INFO : XLL_ERROR, "ffmpeg: %s", err.right(VR_TAIL).data());
		if (done) {
			QFile::remove(cmd.tmpVideo);
			QFile::remove(cmd.tmpAudio);
			vr_say(QString(" video saved: %0 ").arg(QFileInfo(cmd.out).fileName()));
			xlog(XLG_VIDEO, XLL_INFO, "%lli frames written to %s", frames, cmd.out.toLocal8Bit().data());
		} else {
			// the two halves are kept: the picture alone is worth something
			vr_say(" video: putting the sound in failed, see the log ");
		}
	} else {
		QFile::remove(cmd.tmpVideo);
		QFile::remove(cmd.tmpAudio);
	}
	{
		QMutexLocker lock(&vrMutex);
		while (!vrQueue.empty()) {
			delete vrQueue.front();
			vrQueue.pop_front();
		}
		vrQueued = 0;
		vrScreen = QImage();
	}
	vrState.store(VREC_IDLE);
}

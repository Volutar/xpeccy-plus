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

// where a copy fetched for the user goes, else PATH
QString vrec_ffmpeg_auto() {
	QString dir = QString::fromLocal8Bit(conf.path.confDir.c_str()) + "/ffmpeg/";
	foreach(QString sub, QStringList() << "" << "bin/") {
		if (QFileInfo(dir + sub + FFMPEG_EXE).isFile())
			return QDir::cleanPath(dir + sub + FFMPEG_EXE);
	}
	return QStandardPaths::findExecutable("ffmpeg");
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

static QString vr_even(int v) {
	return QString::number(v & ~1);
}

const char* vrecPresets[] = {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", NULL};

// NVENC: its presets p1..p7 in the order of x264's, and a constant quantizer -
// its -cq rate control ignores the setting below about 18 and the picture stops
// getting better, -qp gets better with it as -crf does
static QStringList vr_nvenc(const xRecord& rec) {
	int pre = 3;
	for (int i = 0; vrecPresets[i]; i++)
		if (rec.preset == vrecPresets[i]) pre = i + 1;
	bool hevc = (rec.codec == VREC_H265_NVENC);
	return QStringList() << "-c:v" << (hevc ? "hevc_nvenc" : "h264_nvenc") << "-preset" << QString("p%0").arg(pre)
		<< "-rc" << "constqp" << "-qp" << QString::number(rec.crf)
		<< "-profile:v" << (hevc ? "rext" : "high444p");	// 4:4:4
}

// One frame through the encoder: a build lists NVENC whether or not there is
// a card to run it on. Asked once per program; the first use of the card is slow.
bool vrec_codec_works(const QString& prog, int codec) {
	static QHash<QString, bool> known;
	if ((codec != VREC_H264_NVENC) && (codec != VREC_H265_NVENC)) return true;
	if (prog.isEmpty()) return false;
	QString key = QString("%0|%1").arg(prog).arg(codec);
	if (known.contains(key)) return known[key];
	xRecord rec = conf.rec;
	rec.codec = codec;
	QStringList args;
	args << "-hide_banner" << "-v" << "error" << "-f" << "lavfi" << "-i" << "color=c=black:s=256x256:d=0.1";
	args << "-frames:v" << "1" << "-pix_fmt" << "yuv444p" << vr_nvenc(rec) << "-f" << "null" << "-";
	QProcess prc;
	prc.start(prog, args);
	bool ok = prc.waitForFinished(20000) && (prc.exitStatus() == QProcess::NormalExit) && (prc.exitCode() == 0);
	if (prc.state() != QProcess::NotRunning) {
		prc.kill();
		prc.waitForFinished(1000);
	}
	xlog(XLG_VIDEO, XLL_INFO, "%s: %s", codec == VREC_H265_NVENC ? "hevc_nvenc" : "h264_nvenc", ok ? "works" : "not here");
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
		int n = qBound(1, rec.scale, VREC_SCALE_MAX);
		ow = w / 2 * n;
		oh = h * n;
	}
	bool lossless = (rec.codec == VREC_FFV1);
	// neighbour, so a dot stays a block of whole pixels, and the colours
	// converted the way an HD player reads them back
	QString vf = QString("scale=%0:%1:flags=neighbor").arg(vr_even(ow)).arg(vr_even(oh));
	if (lossless) {
		vf += ",format=bgr0";
	} else {
		vf += ":out_color_matrix=bt709";
		// 4:4:4: 4:2:0 shares one colour between 2x2 pixels, which at an odd
		// scale is two dots, and halves a shader's grid, which is mostly colour
		vf += ",format=yuv444p";
	}
	if (rec.fps60) vf += ",framerate=fps=60";
	QStringList& ea = cmd.enc;
	vr_quiet(ea, quiet);
	ea << "-f" << "rawvideo" << "-pix_fmt" << "rgb0" << "-s" << QString("%0x%1").arg(w).arg(h);
	ea << "-framerate" << fps << "-i" << "-";
	ea << "-vf" << vf;
	switch (rec.codec) {
		case VREC_FFV1:
			ea << "-c:v" << "ffv1";
			break;
		case VREC_H265:
			ea << "-c:v" << "libx265" << "-preset" << QString::fromStdString(rec.preset);
			ea << "-crf" << QString::number(rec.crf) << "-x265-params" << "log-level=error";
			break;
		case VREC_H264_NVENC:
		case VREC_H265_NVENC:
			ea << vr_nvenc(rec);
			break;
		default:
			ea << "-c:v" << "libx264" << "-preset" << QString::fromStdString(rec.preset);
			ea << "-crf" << QString::number(rec.crf);
			break;
	}
	if (!lossless)
		ea << "-color_primaries" << "bt709" << "-color_trc" << "bt709" << "-colorspace" << "bt709";
	ea << vr_split(QString::fromLocal8Bit(rec.extra.c_str()));
	ea << "-an" << cmd.tmpVideo;

	// the second run: the sound put beside it
	QStringList& ma = cmd.mux;
	vr_quiet(ma, quiet);
	ma << "-i" << cmd.tmpVideo << "-i" << cmd.tmpAudio;
	ma << "-map" << "0:v:0" << "-map" << "1:a:0" << "-c:v" << "copy";
	if (rec.fps == VREC_FPS_50) {
		// a machine frame lasts 20 ms of video: the sound follows it
		double ratio = ns / VR_FRAME_NS;
		if (rec.keepPitch) {
			ma << "-af" << QString("atempo=%0").arg(ratio, 0, 'f', 6);
		} else {
			cmd.wavRate = (int)lround(rate * ratio);
			ma << "-ar" << "48000";
		}
	}
	if (lossless) {
		ma << "-c:a" << "flac";
	} else {
		ma << "-c:a" << "aac" << "-b:a" << QString("%0k").arg(rec.abitrate);
	}
	if (ext == "mp4") {
		if ((rec.codec == VREC_H265) || (rec.codec == VREC_H265_NVENC))
			ma << "-tag:v" << "hvc1";	// or Apple's players refuse it
		ma << "-movflags" << "+faststart";
	}
	ma << cmd.out;
	return cmd;
}

// as it would be typed; lines: a line to each part - the input, the filters,
// the codec, the colour tags, the output
QString vrec_command_line(const QStringList& args, bool lines) {
	static const QStringList parts = QStringList() << "-vf" << "-c:v" << "-color_primaries" << "-an" << "-map" << "-movflags";
	QString res("ffmpeg");
	QString opt;
	foreach(QString arg, args) {
		// -map -map on one line, but every input on its own
		bool brk = lines && !opt.isEmpty() && (parts.contains(arg) || (arg == "-i")) && ((arg != opt) || (arg == "-i"));
		bool chain = lines && (opt == "-vf");
		if (arg.startsWith('-')) opt = arg;
		if (arg.contains(' ') || arg.isEmpty()) arg = "\"" + arg + "\"";
		if (chain) arg.replace(",", ",\n      ");		// a filter to a line
		res += (brk ? "\n  " : " ") + arg;
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

// START / STOP

bool vrec_start(Computer* comp, const QString& base, int scrW, int scrH, QString* err) {
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
	QString dir = vrec_dir();
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
	vrecCmd cmd = vrec_command(rec, w, h, vid->nsPerFrame, conf.snd.rate, QDir(dir).filePath(vrec_file_name(QString::fromLocal8Bit(conf.rec.name.c_str()), base, QDateTime::currentDateTime())));
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
	xlog(XLG_VIDEO, XLL_INFO, "recording to %s", cmd.out.toLocal8Bit().data());
	xlog(XLG_VIDEO, XLL_DEBUG, "%s", vrec_command_line(cmd.enc).toLocal8Bit().data());
	wr->start();
	return true;
}

void vrec_stop() {
	if (vrState.load() != VREC_RUN) return;
	vrOn.store(0);
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

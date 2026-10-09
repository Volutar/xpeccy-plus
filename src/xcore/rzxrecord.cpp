#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>

#include <atomic>
#include <thread>

#include "xcore.h"
#include "rzxrecord.h"
#include "vidrec.h"
#include "pacing.h"
#include "../filer.h"
#include "../libxpeccy/filetypes/filetypes.h"
#include "../libxpeccy/filetypes/szx.h"
#include "version.h"

#define RR_SAVE_NS	30000000000LL	// the file is brought up to date this often

static QString rr_path;
static int rr_rollbacks = 0;
static int rr_slowFrames = 0;
static int rr_lastFrames = 0;		// to see a rollback: the log got shorter
static long long rr_savedAt = 0;
static QString rr_msg;
static QMutex rr_msgLock;
static std::thread rr_writer;		// the autosave, written while the machine runs on
static std::atomic<bool> rr_writing(false);

static void rr_say(const QString& msg) {
	QMutexLocker lock(&rr_msgLock);
	rr_msg = msg;
}

QString rzxr_message() {
	QMutexLocker lock(&rr_msgLock);
	QString res = rr_msg;
	rr_msg.clear();
	return res;
}

bool rzxr_on() {
	return rzx_recording != 0;
}

QString rzxr_path() {
	return rr_path;
}

int rzxr_rollbacks() {
	return rr_rollbacks;
}

double rzxr_slow_secs() {
	return conf.zx ? rr_slowFrames / comp_fps(conf.zx) : 0;
}

// what the creator block says besides the name, in lines other players skip
static QByteArray rr_custom() {
	QString res = QString("%0 %1\nrollbacks: %2\nslow motion: %3 s\n")
		.arg(XPRODUCT, XVERSION).arg(rr_rollbacks).arg(rzxr_slow_secs(), 0, 'f', 1);
	return res.toUtf8();
}

// Written to a file of its own and then put in place, so an autosave cut short
// leaves the last one whole.
static int rr_write(rzxRecImage* img, const QString& path, const QByteArray& custom) {
	QString part = path + ".part";
	QStringList ver = QString(XVERSION_BASE).split('.');
	int err = rzx_rec_image_write(img, part.toLocal8Bit().constData(), XPRODUCT,
		ver.value(0).toInt(), ver.value(1).toInt(), custom.constData());
	rzx_rec_image_free(img);
	if (err == ERR_OK) {
		QFile::remove(path);
		if (!QFile::rename(part, path)) err = ERR_CANT_OPEN;
	} else {
		QFile::remove(part);
	}
	return err;
}

static void rr_join_writer() {
	if (rr_writer.joinable()) rr_writer.join();
}

int rzxr_start(Computer* comp) {
	if (rzxr_on()) return ERR_OK;
	rr_join_writer();
	if (comp->rzx.play)			// taken over: the recording goes on from here
		rzxStop(comp);
	szx_set_machine(conf.macId.c_str());
	int err = rzx_rec_start(comp);
	if (err != ERR_OK) return err;
	QDir().mkpath(vrec_dir());
	QString name = vrec_file_name(QString::fromStdString(conf.rec.name), media_image_name(), QDateTime::currentDateTime());
	rr_path = QDir(vrec_dir()).filePath(name + ".rzx");
	rr_rollbacks = 0;
	rr_slowFrames = 0;
	rr_lastFrames = 0;
	rr_savedAt = paceClockNs();
	xlog(XLG_FILE, XLL_INFO, "rzx recording into %s", rr_path.toLocal8Bit().constData());
	return ERR_OK;
}

QString rzxr_stop(Computer* comp) {
	if (!rzxr_on()) return QString();
	rr_join_writer();
	rzxRecImage* img = rzx_rec_take(comp);
	int frames = rzx_rec_image_frames(img);
	rzx_rec_stop(comp);
	int err = img ? rr_write(img, rr_path, rr_custom()) : ERR_RZX_REC;
	xlog(XLG_FILE, (err == ERR_OK) ? XLL_INFO : XLL_WARN, "rzx recording stopped: %i frames, %i rollbacks, %s",
		frames, rr_rollbacks, (err == ERR_OK) ? "written" : "not written");
	rr_say((err == ERR_OK) ? QString(" RZX saved: %0 ").arg(QFileInfo(rr_path).fileName()) : QString(" RZX not saved "));
	return (err == ERR_OK) ? rr_path : QString();
}

void rzxr_frame(Computer* comp) {
	if (!comp->rzx.rec.on) return;
	int frames = rzx_rec_frames(comp);
	if (frames < rr_lastFrames) rr_rollbacks++;	// a rewind or a quick load took it back
	rr_lastFrames = frames;
	if ((conf.emu.tmode == XTM_SLOW) || (conf.emu.speed < 1.0))
		rr_slowFrames++;
}

void rzxr_tick(Computer* comp) {
	if (!rzxr_on() || (paceClockNs() - rr_savedAt < RR_SAVE_NS)) return;
	if (rr_writing) return;			// the last one is still being written
	rr_join_writer();
	rr_savedAt = paceClockNs();
	emu_lock();
	rzxRecImage* img = rzx_rec_take(comp);
	emu_unlock();
	if (!img) return;
	QString path = rr_path;
	QByteArray custom = rr_custom();
	rr_writing = true;
	rr_writer = std::thread([img, path, custom]() {
		if (rr_write(img, path, custom) != ERR_OK)
			rr_say(" RZX autosave failed ");
		rr_writing = false;
	});
}

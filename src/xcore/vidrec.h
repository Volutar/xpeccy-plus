#pragma once

#include <QString>
#include <QImage>
#include <QStringList>
#include <QDateTime>

#include "../libxpeccy/spectrum.h"
#include "../libxpeccy/sound/sndcommon.h"
#include "xcore.h"

// what goes into the video
enum {
	VREC_SRC_PICTURE = 0,	// the emulated frame, no shader, scaled by whole pixels
	VREC_SRC_SCREEN		// the window as shown: shader, indicators and all
};

// what its frame rate says
enum {
	VREC_FPS_MACHINE = 0,	// the machine's own, 48.83 on a Pentagon
	VREC_FPS_50		// every frame is 20 ms, whatever the machine
};

enum {VREC_MP4 = 0, VREC_MKV};
enum {VREC_H264 = 0, VREC_H265, VREC_FFV1, VREC_H264_NVENC, VREC_H265_NVENC};

extern const char* vrecPresets[];		// the speeds, x264's names, fastest first
enum {VREC_IDLE = 0, VREC_RUN, VREC_FINISH};

#define VREC_SCALE_MAX	8
#define VREC_NAME_DEF	"%d_%t_%image"

QString vrec_ffmpeg();				// the program that will be run, empty if none
QString vrec_ffmpeg_auto();			// ...when the settings name none
QString vrec_ffmpeg_version(const QString&);	// its first line of -version, empty if it does not run
QString vrec_dir();				// where the videos go
QString vrec_dir_auto();			// ...when the settings name none
bool vrec_codec_works(const QString& prog, int codec);	// the build has it, and this machine the hardware

// the two FFmpeg runs a recording makes
typedef struct {
	QStringList enc;	// pictures to video
	QStringList mux;	// video and sound to the file
	QString out;
	QString tmpVideo;
	QString tmpAudio;
	int wavRate;		// what the wav's header says
} vrecCmd;

vrecCmd vrec_command(const xRecord&, int w, int h, int ns, int rate, const QString& path, bool quiet = true);
QString vrec_command_line(const QStringList&, bool lines = false);
QString vrec_file_name(const QString& tpl, const QString& image, const QDateTime& when);

bool vrec_start(Computer*, const QString& base, int scrW, int scrH, QString* err);
void vrec_stop();
void vrec_wait();			// until the last one is written, for the exit
int vrec_state();
QString vrec_message();			// what the window should say, once

bool vrec_wants_screen();
void vrec_screen(const QImage&);	// the window as painted, bottom row first

// emulation thread
void vrec_frame(Computer*);		// a picture has been made
void vrec_sample(sndPair);		// a sample has gone to the output

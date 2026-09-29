#pragma once

#include <QString>
#include <QImage>
#include <QStringList>
#include <QDateTime>
#include <atomic>

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
enum {VREC_AAC = 0, VREC_OPUS, VREC_AUDIO_AUTO};
enum {VREC_CH_AUTO = 0, VREC_CH_420, VREC_CH_444};
enum {VREC_H264 = 0, VREC_H265, VREC_FFV1, VREC_H264_NVENC, VREC_H265_NVENC, VREC_H264_AMF, VREC_H265_AMF, VREC_H264_QSV, VREC_H265_QSV,
	VREC_AV1, VREC_AV1_NVENC, VREC_AV1_AMF, VREC_AV1_QSV};

extern const char* vrecPresets[];		// the speeds, x264's names, fastest first
enum {VREC_IDLE = 0, VREC_RUN, VREC_FINISH};

#define VREC_SCALE_MAX	8
#define VREC_NAME_DEF	"%d_%t_%image"

QString vrec_ffmpeg();				// the program that will be run, empty if none
QString vrec_ffmpeg_auto();			// ...when the settings name none
QString vrec_ffmpeg_version(const QString&);	// its first line of -version, empty if it does not run
QString vrec_dir();				// where the videos go
QString vrec_dir_auto();			// ...when the settings name none
bool vrec_codec_works(const QString& prog, int codec);	// the build has it, and this machine what it runs on
bool vrec_420_only(int codec, int chroma);	// AMF, QSV, AV1 or 4:2:0 asked for: an odd scale is off
int vrec_scale_for(int codec, int chroma, int n);	// ...and goes up to the next
int vrec_acodec(const xRecord&);		// VREC_AAC or VREC_OPUS, Auto resolved
enum {VREC_420 = 0, VREC_444, VREC_RGB};
int vrec_chroma(const xRecord&);		// what the picture is written as

// the two FFmpeg runs a recording makes
typedef struct {
	QStringList enc;	// pictures to video
	QStringList mux;	// video and sound to the file
	QStringList video;	// the picture's options as the settings make them, override or not
	QStringList sound;	// ...and the sound's
	QString out;
	QString tmpVideo;
	QString tmpAudio;
	int wavRate;		// what the wav's header says
} vrecCmd;

vrecCmd vrec_command(const xRecord&, int w, int h, int ns, int rate, const QString& path, bool quiet = true);
QString vrec_command_line(const QStringList&, bool lines = false);
QString vrec_args_line(const QStringList&);
QString vrec_file_name(const QString& tpl, const QString& image, const QDateTime& when);

bool vrec_start(Computer*, const QString& base, int scrW, int scrH, QString* err, const QString& file = QString());
void vrec_stop();
void vrec_wait();			// until the last one is written, for the exit
int vrec_state();
QString vrec_message();			// what the window should say, once

bool vrec_wants_screen();
void vrec_screen(const QImage&);	// the window as painted, bottom row first

// emulation thread
void vrec_frame(Computer*);		// a picture has been made
void vrec_sample(sndPair);		// a sample has gone to the output

// Auto recording: the start is armed once per reset and fires on the pc (code
// in ram, or against an address); the stop is a reset or the pc too, and ends
// any recording. The emulation watches, the window starts and stops.
enum {VREC_AUTO_NONE = 0, VREC_AUTO_START, VREC_AUTO_STOP};
enum {VREC_AT_OWN = 0, VREC_AT_EQ, VREC_AT_GE, VREC_AT_LE};	// own: code in ram to start, a reset to stop
enum {VREC_WATCH_START = 1, VREC_WATCH_STOP = 2};
extern std::atomic<int> vrecWatch;		// what the emulation looks at the pc for
bool vrec_auto_parse(const QString&, int* op, int* adr);	// "ram"/"reset", "#6000", "==#6000", ">=0x6000"...
QString vrec_auto_text(int op, int adr, const char* own);
void vrec_auto_pc(Computer*, int watch);	// emulation thread, while watching: at every opcode
void vrec_auto_cli(int);		// --video-auto 1, --no-video-auto 0: both ends, for this run
bool vrec_auto_cli_start(const QString&);	// --video-autostart AT
bool vrec_auto_cli_stop(const QString&);	// --video-autostop AT
void vrec_auto_apply();			// at the start: the start armed at once
void vrec_auto_settings();		// the settings changed
int vrec_auto_tick();			// the window, now and then: what to do
void vrec_manual();			// started or stopped by hand: the start disarmed until a reset

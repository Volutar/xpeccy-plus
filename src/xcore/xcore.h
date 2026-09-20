#pragma once

#include <atomic>
#include <string>
#include <vector>
#include <map>

#if defined(__linux) || defined(__BSD)
#include <linux/limits.h>
#endif

#include <SDL_joystick.h>

#include <QKeySequence>
#include <QString>
#include <QPoint>
#include <QColor>
#include <QFont>
#include <QSize>
#include <QMap>

#include "../libxpeccy/spectrum.h"
#include "../libxpeccy/xlog.h"
#include "../libxpeccy/filetypes/filetypes.h"
#include "gamepad.h"
#include "xexpr.h"

#ifndef USEMUTEX
#define USEMUTEX 0
#endif

#define NEW_SMP_METHOD 1
// init: smpNeed = 0
// each sdl_sound_callback smpNeed += (samples needed = bytes/4)
// each sample in buffer: smpNeed--
// if (smpNeed == 0) conf.snd.fill = 0 (end of emulation cycle)
// after emulation cycle: wait for smpNeed!=0 (instead of sleepy)

#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
	#define yDelta angleDelta().y()
	#define xEventX position().x()
	#define xEventY position().y()
	#define xGlobalX globalPosition().x()
	#define xGlobalY globalPosition().y()
#elif QT_VERSION >= QT_VERSION_CHECK(5,0,0)
	#define yDelta angleDelta().y()
	#define xEventX x()
	#define xEventY y()
	#define xGlobalX globalX()
	#define xGlobalY globalY()
#else
	#define yDelta delta()
	#define xEventX x()
	#define xEventY y()
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5,14,0)
	#define	X_SkipEmptyParts Qt::SkipEmptyParts
	#define X_KeepEmptyParts Qt::KeepEmptyParts
	#include <QScreen>
	#define SCREENSIZE screen()->size()
#else
	#define X_SkipEmptyParts QString::SkipEmptyParts
	#define X_KeepEmptyParts QString::KeepEmptyParts
	#include <QDesktopWidget>
	#define SCREENSIZE QApplication::desktop()->screenGeometry().size()
#endif

// The screen a widget is on, in widget context. QCursor::pos and setPos go by
// the primary screen when not given one, which is the wrong scale factor on a
// second monitor. Not defined before Qt5: there is no QScreen at all
#if QT_VERSION >= QT_VERSION_CHECK(5,14,0)
	#define WIDGET_SCREEN screen()
#elif QT_VERSION >= QT_VERSION_CHECK(5,0,0)
	#define WIDGET_SCREEN (windowHandle() ? windowHandle()->screen() : QGuiApplication::primaryScreen())
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
	#include <QtCore5Compat>
	#include <QSurfaceFormat>
	#define X_BackgroundRole Qt::BackgroundRole
	#define X_MidButton Qt::MiddleButton
	typedef QSurfaceFormat QGLFormat;
	typedef QOpenGLContext QGLContext;
#else
	#include <QRegExp>
	#define X_BackgroundRole Qt::BackgroundColorRole
	#define X_MidButton Qt::MidButton
#endif

// common

std::string getTimeString(int);
std::string int2str(int);
std::string float2str(float);
int getRanged(const char*, int, int);
void setFlagBit(bool, int*, int);
bool str2bool(std::string);
std::vector<std::string> splitstr(std::string,const char*);
std::pair<std::string,std::string> splitline(std::string, char = '=');
void copyFile(const char*, const char*);

// a file that ships inside the binary, replaceable by one of the user's own
QString xres_dir(const char*);
QString xres_root(const char*);
QString xres_path(const char*, const QString&);
QStringList xres_list(const char*, const QStringList&);

int toPower(int);
int toLimits(int, int, int);

// the speed scale: 1/8 1/4 1/2 x1 x2 x4 x8, x1 in the middle
#define XSPD_CENTER	3
#define XSPD_MAX	6
// ...and the ceiling on base x turbo x overclock, whoever asks for it
#define XSPD_CLOCK_MAX	16
double xspeed_mult(int);
void xspeed_set(int);
int xspeed_get(void);
int xspeed_max(void);
QString xspeed_name(int, bool ascii = false);
double xspeed_clock(void);
double xcpu_frq_parse(const QString&, double);
int tape_running(Tape*);
void tape_apply_options(Tape*);
double absd(double);

QString getbinbyte(unsigned char);
QString gethexshift(char);
QString getdecshift(char);
QString formbufword(int);	// hex, no padding - what the gethex* below are built on
QString gethexbyte(int);
QString gethexword(int);
QString getPortString(int, int);
bool parsePort(const QString&, int*, int*);
QString gethex6(int);
QString gethexint(int);

// text in the zx spectrum's own character set, for showing only
QString zx_text(const unsigned char*, int);

typedef struct {
	int err;
	int value;
	const char* ptr;
} xResult;

xResult xEval(const char*, int = 0);

typedef struct {
	unsigned b:1;
	int i;
	double d;
	const char* s;
} xArg;

// pause reasons
#define	PR_MENU		1
#define	PR_FILE		(1<<1)
#define	PR_OPTS		(1<<2)
#define	PR_DEBUG	(1<<3)
#define	PR_QUIT		(1<<4)
#define	PR_PAUSE	(1<<5)
#define	PR_EXTRA	(1<<6)
#define PR_RZX		(1<<7)
#define	PR_EXIT		(1<<8)

// labels

typedef struct {
	QString name;
	QMap<QString, xAdr> list;
} xLabelSet;

xLabelSet* newLabelSet(QString);
int delLabelSet(QString);
xLabelSet* setLabelSet(QString);

void add_label(xAdr, QString, xLabelSet* = nullptr);
void del_label(QString);
QString find_label(xAdr);
xAdr find_label(QString);
void clear_labels();
void clear_all_labels();

int loadLabels(const char*);
int saveLabels(const char*);

// comments

void add_comment(xAdr, QString);
void del_comment(xAdr);
QString find_comment(xAdr);
void clear_comments();

// brk points

#define DELBREAKS 0		// delete breakpoint on FRW=000

#define BRKF_SYSTEM 1

enum {
	BRK_ACT_DBG = 1,
	BRK_ACT_SCR,
	BRK_ACT_COUNT,
};

typedef struct {
	unsigned off:1;
	unsigned fetch:1;
	unsigned read:1;
	unsigned write:1;
	unsigned temp:1;
	unsigned last:1;	// BRK_COND: condition was true on previous check
	unsigned fired:1;	// BRK_COND: it fired on this instruction
	unsigned onchg:1;	// BRK_COND: fire when it becomes true, not while it is
	int type;
	int adr;	// start adr (mem), port(io)
	int eadr;	// end adr
	int mask;	// io: if (port & mask == adr & mask)
	int hits;	// times the breakpoint was triggered (condition not asked yet)
	int count;	// times it actually fired (condition passed)
	int action;	// what to do
	std::string cond;	// condition text, empty = unconditional
	xExpr script;		// compiled condition
} xBrkPoint;

void brkSet(int, int, int, int);
void brkXor(int, int, int, int, int);
void brkAdd(xBrkPoint, int = 0);
// void brkInstall(xBrkPoint*, int);
void brkDelete(xBrkPoint);
void brkInstallAll();
void brk_clear_tmp(Computer*);
xBrkPoint* brk_find(int, int);
void brk_set_cond(xBrkPoint*, const char*);
int brk_cond_true(xBrkPoint*, Computer*);
int brk_check_cond(Computer*);
int brk_cond_count();
int brk_load_list(const char*);
int brk_save_list(const char*);

// hold the emulation thread while the machine is rebuilt (defined in ethread.cpp)
void emu_lock();
void emu_unlock();

// the running machine

// switch to a machine by id, keeping what the user changed on the one we leave
bool xm_set(std::string);
bool xm_set_layout(std::string);
int xm_set_hardware(std::string);

// what the user changed on the machine that is running: into its own file, and
// back out of it again
void xm_save_over();
void xm_reset_over();		// drop it and take the machine as it ships

// a config written before schema 3 kept the same settings in [MACHINE.<id>]
// blocks; they are read into a staging map and written out as files once
void xm_over_add(const std::string&, const std::string&, const std::string&);
void xm_over_migrate();
void xm_drop_running();		// the config is being read from scratch

void xm_defer(const std::string&, const std::string&);
bool xm_migrate(const std::string&, const std::string&);
std::string xm_id_for_name(const std::string&);
void xm_finish_load();
void xm_save_nvram();
void xm_save_media(FILE*);
std::string getDiskString(Floppy*);

QStringList getWatchPorts(Computer*, int = 1);
void setWatchPorts(Computer*, QStringList);

//screenshot format
#define	SCR_BMP		1
#define	SCR_PNG		2
#define	SCR_JPG		3
#define	SCR_SCR		4
#define	SCR_HOB		5
#define	SCR_DISK	6

#define	PAL_DEFAULT_NAME	"Xpeccy+"	// the built-in palette, as the lists name it

void conf_init(char*, char* confdir = NULL);
QList<QColor> loadColors(std::string);
int saveColors(std::string, QList<QColor>);
void loadPalette();
std::string palette_name(const std::string&);	// what a config's palette name means now
void dbgPaletteDefaults();			// debugger colours: back to the built-in ones
const char* dbgPaletteDefault(const char*);
bool loadStylePalette(const std::string&);	// debugger colours shipped with a style sheet
void loadConfig();
void saveConfig();
bool reloadConfig();

// the whole configuration as one text file, and back to how it ships
bool xconf_export(const QString&);
bool xconf_import(const QString&);
bool xconf_reset();

extern std::map<std::string, int> shotFormat;

// keymap

enum {
	XCUT_SIZEX1 = 0x10000,
	XCUT_SIZEX2,
	XCUT_SIZEX3,
	XCUT_SIZEX4,
	XCUT_SIZEX5,
	XCUT_SIZEX6,
	XCUT_FULLSCR,
	XCUT_RATIO,
	XCUT_SCRSHOT,
	XCUT_COMBOSHOT,
	XCUT_RES_DOS,
	XCUT_KEYBOARD,
	XCUT_FAST,
	XCUT_NOFLICK,
	XCUT_MOUSE,
	XCUT_GRABKBD,
	XCUT_PAUSE,
	XCUT_DEBUG,
	XCUT_MENU,
	XCUT_OPTIONS,
	XCUT_SAVE,
	XCUT_LOAD,
	XCUT_TAPLAY,
	XCUT_TAPREC,
	XCUT_TAPWIN,
	XCUT_RZXWIN,
	XCUT_SCRWIN,
	XCUT_SNDWIN,
	XCUT_FASTSAVE,
	XCUT_NMI,
	XCUT_RESET,
	XCUT_TURBO,
	XCUT_SPEED_UP,
	XCUT_SPEED_DOWN,
//	XCUT_TVLINES,
	XCUT_WAV_OUT,
	XCUT_RELOAD_SHD,
	XCUT_RELOAD,
	XCUT_FAVORITE,

	XCUT_STEPIN,
	XCUT_STEPOVER,
	XCUT_STEPOUT,
	XCUT_FASTSTEP,
	XCUT_TMPBRK,
	XCUT_TRACE,
	XCUT_OPEN_DUMP,
	XCUT_SAVE_DUMP,
	XCUT_OPEN_XMAP,
	XCUT_SAVE_XMAP,
	XCUT_FINDER,
	XCUT_LABELS,
	XCUT_LABLIST,
	XCUT_DBG_RELOAD,

	XCUT_TOPC,
	XCUT_SETPC,
	XCUT_SETBRK,
	XCUT_JUMPTO,
	XCUT_RETFROM,
	XCUT_GOTOADR,

	XCUT_DUMP_GOTOADR,
	XCUT_DUMP_REG_PC,
	XCUT_DUMP_REG_SP,
	XCUT_DUMP_REG_BC,
	XCUT_DUMP_REG_DE,
	XCUT_DUMP_REG_HL,
	XCUT_DUMP_REG_IX,
	XCUT_DUMP_REG_IY,
};

enum {
	SCG_ALL = -1,
	SCG_MAIN = (1 << 0),
	SCG_DEBUGA = (1 << 1),
	SCG_DISASM = (1 << 2),
	SCG_DUMP = (1 << 3)
};

void loadKeys();
void setKey(const char*, const char*);
keyEntry getKeyEntry(int);
int getKeyIdByName(const char*);
const char* getKeyNameById(int);
int qKey2id(int, Qt::KeyboardModifiers = Qt::NoModifier);
int key2qid(int);

typedef struct {
	int grp;
	int id;
	const char* name;
	const char* text;
	QKeySequence seq;
	QKeySequence def;
} xShortcut;

void shortcut_init();
xShortcut* find_shortcut_id(int);
xShortcut* find_shortcut_name(const char*);
void set_shortcut_id(int, QKeySequence);
void set_shortcut_name(const char*, QKeySequence);
xShortcut* shortcut_tab();
int shortcut_check(int, QKeySequence);
int shortcut_match(int, int, QKeySequence);
Qt::KeyboardModifiers xNativeMods(Qt::KeyboardModifiers);

// bookmarks

typedef struct {
	std::string name;
	std::string path;
} xBookmark;

void addBookmark(std::string,std::string);
int findBookmark(const QString&);		// -1 when the file is not in the list
void setBookmark(int,std::string,std::string);
void delBookmark(int);
void swapBookmarks(int,int);

// romsets

typedef struct {
	std::string name;
	int foffset;
	int fsize;
	int roffset;
} xRomFile;

typedef struct {
	std::string name;
	std::string gsFile;		// general sound
	std::string fntFile;		// charset
	QList<xRomFile> roms;
} xRomset;

// the romset table an old config file carries; read once, never written
xRomset* findRomset(std::string);
bool addRomset(xRomset);

// where a rom file is: under the rom directory, unless it names its own path
std::string xm_rom_path(const std::string&);

// what the machine loads, and putting a changed set back as the user's own
void xm_set_roms(const xRomset&, bool poweron = false);
void xm_rom_set_file(xRomset&, int, const std::string&);

// machines

// What a machine is: read-only, from the binary's own resources, with a file of
// the same id in machines/ of the config directory patching it - and a file
// with an id of its own being a machine of its own. See docs/machine-format.md.

typedef struct {
	std::string id;
	std::string name;
	std::string family;
	std::string parent;		// the machine it inherits, if any
	std::string hw;			// HardWare.name
	std::string cpu;		// cpuCore.name
	int memory;			// KB
	std::string ramCold;		// power-on ram pattern, hex groups with *N runs; empty = memory left alone
	int ramNoise;			// bytes in a thousand that come up wrong in it
	int cpufrq;			// Hz
	std::string turboSteps;		// turbo steps this board has, "1" = none
	int resbank;			// RES_*
	int earback;			// EAR_*
	unsigned contio:1;
	unsigned contmem:1;
	unsigned scrpwait:1;
	std::string geometry;		// layout name
	int contPattern;
	unsigned early:1;
	unsigned brd4t:1;
	unsigned snow:1;		// the ULA snow effect
	unsigned snowcrash:1;		// ...and ram that cannot take it
	int floatbus;			// FBUS_*: what a port nothing answers reads back
	int psgCount;
	int psgType;			// SND_*
	double psgFrq;			// MHz, 0 = the chip type's own clock
	int psgStereo;			// AY_*
	int soundrive;			// SDRV_*
	int disk;			// DIF_*
	int ide;			// IDE_*
	unsigned mouse:1;
	unsigned mouseWheel:1;
	unsigned joyButtons:1;
	int scantab;			// KBD_*, 0 = the keyboard core's own type
	unsigned gs:1;			// General Sound
	unsigned saa:1;
	unsigned ulaplus:1;
	unsigned ddpal:1;		// Profi's dd palette
	int romBanks;			// 16K rom banks the core can page
	xRomset roms;			// the files it ships with
} xMachine;

void xm_load_all();
// machines of the user's own: making one out of the running machine, and
// dropping it again
bool xm_save_as(const std::string&, const std::string&);
std::string xm_free_id(const std::string&);
bool xm_name_free(const std::string&);
bool xm_delete(const std::string&);
bool xm_ships(const std::string&);	// it comes with the emulator
bool xm_is_users(const std::string&);	// the user made it
bool xm_is_changed(const std::string&);	// it carries something of theirs
QString xm_list_name(const xMachine&);	// its name, marked when it does
QStringList xm_user_files();
bool xm_is_user_file(const QString&);
const QList<xMachine>& xm_list();
std::string xm_turbo_str(Computer*);
void xm_turbo_set(Computer*, std::string);
int xm_turbo_index(Computer*);
const xMachine* xm_find(std::string);		// as the user has it
const xMachine* xm_stock(std::string);		// as it ships
const xMachine* xm_find_by_core(std::string);
int xm_ram_size(std::string, int* mask = NULL);

// layouts

typedef struct {
	std::string name;
	vLayout lay;
} xLayout;

// what a machine gets when the layout it names is not there
#define	LAY_DEFAULT	"ULA.48"

bool addLayout(std::string, vLayout);
const xLayout* layout_shipped(const std::string&);
bool addLayoutString(const std::string&);
std::string layoutString(const xLayout&);
void rmLayout(std::string);
xLayout* findLayout(std::string);
void layouts_load_all();
void layouts_save();

// xmap

void load_xmap(QString);
void save_xmap(QString);

// config

#define	YESNO(cnd) ((cnd) ? "yes" : "no")

struct xConfig {
	// the machine, one per process, and the workspace around it
	Computer* zx;
	std::string macId;		// machine definition id
	std::string layName;		// screen layout
	xRomset roms;			// what it loads: its own, that variant, your files
	std::string palette;		// colour palette file
	std::string kmapName;		// keyboard layout
	std::string jmapNameA;		// gamepad maps
	std::string jmapNameB;
	std::string lastDir;
	struct {
		std::vector<xBrkPoint> list;
		std::vector<xBrkPoint> list_sys;
		std::map<int, std::map<int, xBrkPoint*> > map;	// [memtype][addr] = pointer
	} brk;
	QMap<int, QMap<int, QString> > commap;	// comments: [memtype][addr] = string
	QMap<int, QMap<int, QString> > labmap;	// [memtype][addr] = name
	QList<xLabelSet*> labsets;
	xLabelSet* curlabset;		// curlabset->list = labels

	unsigned running:1;
	unsigned storePaths:1;		// store tape/disk paths
	unsigned boot:1;		// add boot to trdos floppies
	unsigned autorun:1;		// reset and start media opened from the gui
	unsigned confexit:1;		// confirm on exit
	int xpos;			// window position
	int ypos;
	QList<xRomset> rsList;
	QList<xLayout> layList;
	QList<xBookmark> bookmarkList;
	QMap<QString, QColor> pal;
	QString labpath;
	std::string style;
	unsigned short port;		// port to listen
	struct {
		unsigned fast:1;
		int pause;
		// frames the emulation runs ahead of the timeline it keeps, to hide
		// the machine's own reaction time. 0 = off (see ethread.cpp)
		int runahead;
		// slow motion: how fast emulated time runs against the host's, 1.0 =
		// normal. It is the host's pace and not the machine's clock, so the
		// frame is the same length in T and the frame rate is what drops
		double speed;
	} emu;
	struct {
		unsigned fullScreen:1;	// use fullscreen
		unsigned keepRatio:1;	// keep ratio in fullscreen (add black borders)
		unsigned lowLatency:1;	// show each frame at once (low input lag), else buffer for smooth motion
		int border;		// VID_BRD_* : how much border to show
		int scale;		// x1..x4
		int fcount;		// frames counter (for fps showing) (= fcnt ???)
		long long fctime;	// time of that frame, ns (see paceClockNs)
		double curfps;		// last measured fps
		std::string shader;
		int shd_support;
	} vid;
	struct {
		unsigned enabled:1;
		unsigned wavout:1;	// recording to wav, at the output rate
		unsigned fill:1;	// 1 while snd buffer not filled, 0 at end of snd buffer
		// samples the emulation still owes. Filled by the pacer's timer thread
		// (pacing.cpp), drained by the emulation thread, so it has to be atomic -
		// as a plain int a decrement landing inside an addition wiped it out.
		std::atomic<int> need;
		int rate;
		int rateauto;		// take the rate from the device instead (see sound.cpp)
		int chans;
		int latency;		// ms of sound kept in the ring buffer (see sound.h)
		int latauto;		// let the emulator find that number by itself
		int filter;		// filter down to the output rate, else just average
		sndVolume vol;
		FILE* wavfile;
	} snd;
	struct {
		unsigned autostart:1;
		unsigned fast:1;
		unsigned rewind:1;	// play starts a tape played to its end over
	} tape;
//	struct {
//		xGamepad* gpad;
//		xGamepad* gpadb;
//	} joy;
	xGamepadController* gpctrl;
	struct {
		unsigned noLeds:1;
		unsigned noBorder:1;
		int count;
		int interval;
		std::string format;
		std::string dir;
	} scrShot;
	struct {
		unsigned dock:1;	// glued under the emulator window
		int width;		// window width; the height follows the picture
	} keywin;
	struct {
		unsigned mouse:1;
		unsigned joy:1;
		unsigned keys:1;
		unsigned tape:1;
		unsigned disk:1;
		unsigned message:1;
		unsigned fps:1;
		unsigned halt:1;
		unsigned clock:1;	// cpu clock, shown only when it is not the machine's own
	} led;
	struct {
		unsigned enabled:1;	// write the log file
		unsigned console:1;	// mirror the log to stdout
		int level;		// the level a group follows unless set apart
		signed char grp[XLG_COUNT];	// XLOG_FOLLOW, or a level of its own
		std::string dir;	// holds logs/; empty = beside the binary
	} log;
	struct {
		std::string confDir;
		std::string confFile;
		std::string romDir;
		std::string prfDir;	// old profiles, only read once by the migration
		std::string nvDir;	// nvram/, what the machines keep
		std::string palDir;
		std::string plgDir;	// so/dll/dynlib (experimental, works only for CPU)
		std::string font;
		std::string boot;
	} path;
	struct {
		unsigned labels:1;
		unsigned segment:1;
		unsigned dimadr:1;	// address column drawn dimmed
		unsigned dimops:1;	// opcode bytes column drawn dimmed
		unsigned synhl:1;	// syntax colors in the command column
		unsigned blocksep:1;	// empty line after RET/JP/JR
		unsigned romwr:1;
		unsigned showmmap:1;	// blocks of the misc panel
		unsigned showports:1;
		unsigned showsig:1;
		unsigned showray:1;
		unsigned showfrm:1;
		unsigned regsplit:1;	// light a changed byte on its own, not the whole register
		unsigned scrnopix:1;	// screen panel: attributes alone
		unsigned scrnoatr:1;	// screen panel: pixels alone
		unsigned scrnoflash:1;	// screen panel: hold the flash attribute still
		unsigned scrgrid:1;	// screen panel: dim every other cell
		unsigned scrdetach:1;	// screen panel: shown in a window of its own
		unsigned snddetach:1;	// sound panel: shown in a window of its own
		unsigned sndfm:1;	// sound panel: the FM page of the chip, not the PSG one
		unsigned sndfit:1;	// sound panel: the scope centres and scales itself
		unsigned sndlog:1;	// sound panel: the scope draws on a log curve
		QFont font;
		int dbsize;
		int dwsize;
		int dmsize;
		int stackofs;		// first offset the stack panel shows, even, see DBG_STACK_OFS
		int scrzoom;		// screen panel: pixels per dot, or XSCR_FIT
		int scrmode;		// screen panel: which screen, XSCR_*
		int scrpage;		// screen panel: page for XSCR_CUSTOM
		int scrofs;		// screen panel: offset for XSCR_CUSTOM
		int reglayout;		// register panel: columns, or DBG_REGS_AUTO
		int sndchip;		// sound panel: which chip it shows, 0..3
		QPoint pos;
		QSize siz;
		QPoint scrpos;		// screen window, -1 for wherever it lands
		QSize scrsiz;
		QPoint sndpos;		// sound window, the same way
		QSize sndsiz;
	} dbg;
};

// constants in the disasm listing (labels go bold instead of coloured)

#define DBG_PAL_CONST	"dbg.asm.const.txt"

// what a register field that changed is drawn in

#define DBG_PAL_CHG_BG	"dbg.changed.bg"
#define DBG_PAL_CHG_TXT	"dbg.changed.txt"

// how far either way the stack panel may start from SP

#define DBG_STACK_OFS	16

// register panel layout: the value is the column count, 0 = pick it by width

#define DBG_REGS_AUTO	0
#define DBG_REGS_1COL	1
#define DBG_REGS_2COL	2
#define DBG_REGS_WIDE	4	// 4 register columns, flags beside them

// screen panel: which screen it shows

enum {
	XSCR_AUTO = 0,		// whichever the machine is showing
	XSCR_MAIN,		// page 5
	XSCR_SHADOW,		// page 7
	XSCR_BOTH,		// both at once
	XSCR_CUSTOM		// any page at any offset
};

#define XSCR_FIT	0	// zoom value for "use the whole panel"
#define XSCR_ZOOMMAX	4

// fast mode makes frames far quicker than a screen has anything new to say

#define XSCR_FAST_EVERY	10

#define XSCR_PAGE_MAIN		5
#define XSCR_PAGE_SHADOW	7

extern xConfig conf;

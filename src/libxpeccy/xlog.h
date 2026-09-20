// Event log, shared by the core and the gui.
//
// A disabled log costs one compare: xlog() looks at a global per-group level
// before it touches anything else. Groups that would fire inside the emulation
// loop itself use xlogh(), which is not compiled in at all unless XLOG_HOT is
// defined (cmake -DXLOGHOT=1).

#ifndef X_XLOG_H
#define X_XLOG_H

#ifdef __cplusplus
extern "C" {
#endif

// levels, low to high. XLL_OFF means the group says nothing at all
enum {
	XLL_OFF = 0,
	XLL_ERROR,
	XLL_WARN,
	XLL_INFO,
	XLL_DEBUG,
	XLL_TRACE,
	XLL_COUNT
};

enum {
	XLG_APP = 0,	// start, stop, command line
	XLG_CONF,	// config, profiles, romsets, palettes
	XLG_CORE,	// spectrum.c and the rest of the core
	XLG_CPU,
	XLG_VIDEO,
	XLG_SOUND,
	XLG_TAPE,
	XLG_DISK,
	XLG_INPUT,
	XLG_HW,		// per-machine hardware
	XLG_FILE,	// file formats
	XLG_GUI,
	XLG_GL,
	XLG_NET,
	XLG_BRK,	// breakpoints asked to log what they caught
	XLG_COUNT
};

extern unsigned char xlog_lev[XLG_COUNT];

#define xlog_on(grp, lev)	((unsigned)(lev) <= (unsigned)xlog_lev[grp])

#if defined(__GNUC__)
void xlog_put(int grp, int lev, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
#else
void xlog_put(int grp, int lev, const char* fmt, ...);
#endif

#define xlog(grp, lev, ...) \
	do { if (xlog_on(grp, lev)) xlog_put(grp, lev, __VA_ARGS__); } while (0)

// Hot paths: per port access, per dot, per sample. A line there costs a couple
// of microseconds - formatting, a lock and a flush - so turning the group up
// would stall the very machine being debugged. Not built unless asked for.
#ifdef XLOG_HOT
#define xlogh(grp, lev, ...)	xlog(grp, lev, __VA_ARGS__)
#else
#define xlogh(grp, lev, ...)	((void)0)
#endif

// set up / take down. xlog_init is called before anything else in main
void xlog_init(void);
void xlog_done(void);

// the host monotonic clock (paceClockNs) and the emulated one, handed in so
// the core keeps no platform or app code of its own
void xlog_set_clock(long long (*fn)(void));
void xlog_set_emutime(void (*fn)(int* frame, int* tick));

int  xlog_open(const char* path);	// 1 on success
// the lines held from before the file existed: into it, after the header, or
// dropped when no file is coming
void xlog_flush_early(void);
void xlog_close(void);
void xlog_set_console(int on);
int  xlog_is_open(void);
const char* xlog_path(void);

// a header line: written as it is, no columns
void xlog_raw(const char* text);

// names, for the config file, the command line and the options page
const char* xlog_group_name(int grp);
const char* xlog_level_name(int lev);
int xlog_group_id(const char* name);	// -1 when unknown
int xlog_level_id(const char* name);	// -1 when unknown

#ifdef __cplusplus
}
#endif

#endif


#include <stdio.h>
#include "xlog.h"
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define xstricmp	_stricmp
static CRITICAL_SECTION xlog_cs;
static int xlog_cs_ready = 0;
#define XLOCK()		do { if (xlog_cs_ready) EnterCriticalSection(&xlog_cs); } while (0)
#define XUNLOCK()	do { if (xlog_cs_ready) LeaveCriticalSection(&xlog_cs); } while (0)
#else
#include <pthread.h>
#include <strings.h>
#include <sys/time.h>
#define xstricmp	strcasecmp
static pthread_mutex_t xlog_mtx = PTHREAD_MUTEX_INITIALIZER;
#define XLOCK()		pthread_mutex_lock(&xlog_mtx)
#define XUNLOCK()	pthread_mutex_unlock(&xlog_mtx)
#endif

// Until the config is read nobody knows whether a log file is wanted, so the
// early lines are kept here until xlog_flush_early() either pours them into the
// file that opened or drops them because none did.
#define PREBUF_MAX	512
#define LINE_MAX	1024

unsigned char xlog_lev[XLG_COUNT];

static const char* grp_name[XLG_COUNT] = {
	"APP", "CONF", "CORE", "CPU", "VIDEO", "SOUND", "TAPE",
	"DISK", "INPUT", "HW", "FILE", "GUI", "GL", "NET", "BRK"
};

static const char* lev_name[XLL_COUNT] = {
	"OFF", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"
};

static FILE* xlog_file = NULL;
static char xlog_file_path[1024] = "";
static int xlog_console = 0;

static char* prebuf[PREBUF_MAX];
static int prebuf_cnt = 0;
static int prebuf_lost = 0;
static int prebuf_open = 1;		// still collecting

static long long (*clock_fn)(void) = NULL;
static void (*emutime_fn)(int*, int*) = NULL;
static long long start_ns = 0;

// ---------------------------------------------------------------- host time

static void wall_now(int* h, int* m, int* s, int* ms) {
#ifdef _WIN32
	SYSTEMTIME st;
	GetLocalTime(&st);
	*h = st.wHour;
	*m = st.wMinute;
	*s = st.wSecond;
	*ms = st.wMilliseconds;
#else
	struct timeval tv;
	struct tm tmv;
	time_t sec;
	gettimeofday(&tv, NULL);
	sec = (time_t)tv.tv_sec;
	localtime_r(&sec, &tmv);
	*h = tmv.tm_hour;
	*m = tmv.tm_min;
	*s = tmv.tm_sec;
	*ms = (int)(tv.tv_usec / 1000);
#endif
}

// Only used before pacingInit() hands over the app's own clock.
// QueryPerformanceCounter rather than GetTickCount64: the latter wants a
// Vista-or-later SDK, which the 32-bit MinGW toolchain does not default to.
static long long host_ns(void) {
	if (clock_fn) return clock_fn();
#ifdef _WIN32
	LARGE_INTEGER frq, cnt;
	if (!QueryPerformanceFrequency(&frq) || (frq.QuadPart == 0)) return 0;
	QueryPerformanceCounter(&cnt);
	// split so the multiply cannot overflow on a long uptime
	return (cnt.QuadPart / frq.QuadPart) * 1000000000LL
		+ ((cnt.QuadPart % frq.QuadPart) * 1000000000LL) / frq.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

// milliseconds since xlog_init. Uses the app's own monotonic clock once it has
// handed one over, so the log and the pacer never disagree about time.
static long long elapsed_ms(void) {
	return (host_ns() - start_ns) / 1000000LL;
}

// ------------------------------------------------------------------- sinks

// called with the lock held
static void sink_line(const char* line) {
	if (xlog_file) {
		fputs(line, xlog_file);
		// every line hits the disk: a log that stops one line short of the
		// crash it was opened for is worth nothing
		fflush(xlog_file);
	}
	if (xlog_console) {
		fputs(line, stdout);
		fflush(stdout);
	}
	if (prebuf_open && !xlog_file) {
		if (prebuf_cnt < PREBUF_MAX) {
			size_t len = strlen(line) + 1;
			char* cp = (char*)malloc(len);
			if (cp) {
				memcpy(cp, line, len);
				prebuf[prebuf_cnt++] = cp;
			}
		} else {
			prebuf_lost++;
		}
	}
}

static void prebuf_drop(void) {
	int i;
	for (i = 0; i < prebuf_cnt; i++)
		free(prebuf[i]);
	prebuf_cnt = 0;
	prebuf_lost = 0;
	prebuf_open = 0;
}

// ------------------------------------------------------------------ output

void xlog_put(int grp, int lev, const char* fmt, ...) {
	char line[LINE_MAX];
	char ftxt[16];
	char ttxt[16];
	va_list args;
	long long el = elapsed_ms();
	int h, m, s, ms;
	int frame = -1;
	int tick = -1;
	int len;

	if ((unsigned)grp >= XLG_COUNT) grp = XLG_CORE;
	if ((unsigned)lev >= XLL_COUNT) lev = XLL_INFO;

	wall_now(&h, &m, &s, &ms);
	if (emutime_fn) emutime_fn(&frame, &tick);
	if (frame < 0) {
		strcpy(ftxt, "F--------");
	} else {
		snprintf(ftxt, sizeof(ftxt), "F%08d", frame);
	}
	if (tick < 0) {
		strcpy(ttxt, "T------");
	} else {
		snprintf(ttxt, sizeof(ttxt), "T%06d", tick);
	}

	// The columns first, then the message straight after them: one buffer and
	// one copy. Integer milliseconds, not %f - the float conversion alone was
	// costing more than the rest of the line put together.
	len = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d +%05lld.%03lld %s %s %-5s %-5s ",
		h, m, s, ms, el / 1000, el % 1000, ftxt, ttxt,
		lev_name[lev], grp_name[grp]);
	if (len < 0) return;
	if ((size_t)len > sizeof(line) - 2) len = (int)sizeof(line) - 2;

	va_start(args, fmt);
	len += vsnprintf(line + len, sizeof(line) - len - 1, fmt, args);
	va_end(args);
	if ((size_t)len > sizeof(line) - 2) len = (int)sizeof(line) - 2;

	// the callers are old printf's: most of them end in a newline, the column
	// layout wants exactly one and puts it there itself
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		len--;
	line[len] = '\n';
	line[len + 1] = 0;

	XLOCK();
	sink_line(line);
	XUNLOCK();
}

void xlog_raw(const char* text) {
	char line[LINE_MAX + 128];
	snprintf(line, sizeof(line), "%s\n", text);
	XLOCK();
	sink_line(line);
	XUNLOCK();
}

// ------------------------------------------------------------- set up/down

void xlog_init(void) {
	int i;
#ifdef _WIN32
	InitializeCriticalSection(&xlog_cs);
	xlog_cs_ready = 1;
#endif
	for (i = 0; i < XLG_COUNT; i++)
		xlog_lev[i] = XLL_INFO;		// captured, not shown, until the config says
	prebuf_cnt = 0;
	prebuf_lost = 0;
	prebuf_open = 1;
	xlog_console = 1;			// where the old printf's went
	start_ns = host_ns();
}

void xlog_done(void) {
	XLOCK();
	if (xlog_file) {
		fflush(xlog_file);
		fclose(xlog_file);
		xlog_file = NULL;
	}
	xlog_file_path[0] = 0;
	prebuf_drop();
	XUNLOCK();
#ifdef _WIN32
	if (xlog_cs_ready) {
		xlog_cs_ready = 0;
		DeleteCriticalSection(&xlog_cs);
	}
#endif
}

void xlog_set_clock(long long (*fn)(void)) {
	long long was = host_ns();
	clock_fn = fn;
	// keep the elapsed column running across the swap
	if (fn) start_ns = fn() - (was - start_ns);
}

void xlog_set_emutime(void (*fn)(int*, int*)) {
	emutime_fn = fn;
}

int xlog_open(const char* path) {
	FILE* f;
	if (!path || !*path) return 0;
	f = fopen(path, "wb");
	if (!f) return 0;
	XLOCK();
	if (xlog_file) fclose(xlog_file);
	xlog_file = f;
	snprintf(xlog_file_path, sizeof(xlog_file_path), "%s", path);
	XUNLOCK();
	return 1;
}

// The header goes in first, so the lines held from before the file existed are
// poured in after it, not above it.
void xlog_flush_early(void) {
	int i;
	XLOCK();
	if (xlog_file) {
		for (i = 0; i < prebuf_cnt; i++) {
			fputs(prebuf[i], xlog_file);
			free(prebuf[i]);
		}
		if (prebuf_lost > 0)
			fprintf(xlog_file, "# %d early lines lost: the buffer holds %d\n", prebuf_lost, PREBUF_MAX);
		fflush(xlog_file);
		prebuf_cnt = 0;
		prebuf_lost = 0;
	} else {
		prebuf_drop();
	}
	prebuf_open = 0;
	XUNLOCK();
}

void xlog_close(void) {
	XLOCK();
	if (xlog_file) {
		fflush(xlog_file);
		fclose(xlog_file);
		xlog_file = NULL;
	}
	xlog_file_path[0] = 0;
	XUNLOCK();
}

void xlog_set_console(int on) {
	xlog_console = on ? 1 : 0;
}

int xlog_is_open(void) {
	return xlog_file ? 1 : 0;
}

const char* xlog_path(void) {
	return xlog_file_path;
}

// -------------------------------------------------------------------- names

const char* xlog_group_name(int grp) {
	if ((unsigned)grp >= XLG_COUNT) return "?";
	return grp_name[grp];
}

const char* xlog_level_name(int lev) {
	if ((unsigned)lev >= XLL_COUNT) return "?";
	return lev_name[lev];
}

int xlog_group_id(const char* name) {
	int i;
	if (!name) return -1;
	for (i = 0; i < XLG_COUNT; i++)
		if (!xstricmp(name, grp_name[i])) return i;
	return -1;
}

int xlog_level_id(const char* name) {
	int i;
	if (!name) return -1;
	for (i = 0; i < XLL_COUNT; i++)
		if (!xstricmp(name, lev_name[i])) return i;
	return -1;
}

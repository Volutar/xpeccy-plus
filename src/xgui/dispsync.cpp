// QueryDisplayConfig is Windows 7's; a 32-bit MinGW defaults below it (Qt 5 needs Windows 7)
#if defined(_WIN32) && (!defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0601))
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "dispsync.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

#include "xcore/pacing.h"
#include "xcore/xcore.h"

#if defined(__WIN32)
#include <windows.h>
#include <dxgi.h>
#endif

// how long the vblanks are counted before the measured rate is believed, ns
#define DS_SETTLE_NS	5000000000LL
// the window the measured rate is taken over, ns: long enough for 0.01 %
#define DS_WINDOW_NS	10000000000LL
// the measured rate wins over the system's when they are this far apart
#define DS_TRUST_PPM	2000

static std::atomic<double> sysRate{0};	// what the system says, Hz
static std::atomic<bool> adaptive{false};
static QString screenName;
static bool probed = false;		// the log has had a first answer

// the measurement, on the vblank thread; read under the lock
static std::mutex measGuard;
static long long measFirst = -1;	// the window's start
static long long measLast = -1;		// the vblank before
static long long measCount = 0;		// vblank periods in the window
static double measHz = 0;		// last settled figure
static bool measTold = false;		// the log has the settled figure

static std::atomic<dsyncTick> tickCb{nullptr};
static bool swapWait = false;

void dsync_swap_wait(bool on) {
	swapWait = on;
}

bool dsync_swaps_wait() {
	return swapWait;
}

static void meas_reset() {
	std::lock_guard<std::mutex> lock(measGuard);
	measFirst = -1;
	measLast = -1;
	measCount = 0;
	measHz = 0;
	measTold = false;
}

// One vblank seen. A late wake-up skips a vblank or two: the gap is counted as
// the periods it spans, by the rate expected, so the mean is not dragged down.
// A wait that returns again inside the same period is no vblank: false.
static bool meas_vblank(long long ns) {
	double tell = 0;
	{
	std::lock_guard<std::mutex> lock(measGuard);
	if (measLast < 0) {
		measFirst = measLast = ns;
		return true;
	}
	long long dt = ns - measLast;
	double est = (measHz > 0) ? measHz : sysRate.load();
	long long n = 1;
	if (est > 0) {
		n = llround(dt * est / 1e9);
		if (n < 1) return false;
	}
	measLast = ns;
	measCount += n;
	long long span = ns - measFirst;
	// both ends are vblanks, so the figure is exact to the clock. A full window
	// is kept and a new one started from here: the rate follows a slow drift,
	// and nothing in the middle of a window is ever guessed at
	if (span >= DS_SETTLE_NS)
		measHz = measCount * 1e9 / span;
	if (span >= DS_WINDOW_NS) {
		if (!measTold) tell = measHz;
		measTold = true;
		measFirst = ns;
		measCount = 0;
	}
	}
	if (tell > 0)
		xlog(XLG_VIDEO, XLL_INFO, "vsync: measured %.4f Hz over the vblanks, system says %.4f", tell, sysRate.load());
	return true;
}

// The frame-time log (video at debug): how many vblanks each frame drawn stayed
// up for. Locked, every frame stays n; a Next at 58.15 Hz on a 60 Hz screen
// shows two now and then - the jerk. Two frames inside one vblank: one of them
// was never seen.
#define FT_LOG_NS	10000000000LL
#define FT_RUNS		8
static std::atomic<int> ftDrawn{0};
static std::atomic<bool> ftPending{false};
static long long ftSince = -1;
static int ftRun = 0;			// vblanks the frame up now has stayed
static int ftHist[FT_RUNS];		// frames by how long they stayed, the last: that or longer
static int ftLost = 0;
static int ftVbl = 0;

void dsync_frame_drawn() {
	ftDrawn.fetch_add(1, std::memory_order_relaxed);
	ftPending = true;
}

// The tick's hold. A frame made at the vblank is ready in a couple of
// milliseconds and then waits most of the period for the next vblank, its input
// read that much too early. So the tick is held back until the frame it makes
// reaches the window late in the period, still short of the next vblank: it is
// shown at the same vblank, with its input read some 5 ms later at 100 Hz.
// Filmed at 240 fps (Windows 11, 100 Hz, VRR off), frames held and not held
// both stayed 2 vblanks each. The slowest frame of 2 s is the one aimed, so a
// slow frame still makes the vblank.
#define HOLD_AT		0.85		// the slowest frame of 2 s lands here, of a period
#define HOLD_WIN_NS	1000000000LL
#define PH_BINS		10
static std::atomic<long long> holdNs{0};
static std::atomic<long long> cbAt{-1};		// the tick last ran its callback
static long long workMax = 0;			// the tick to the frame in the window, the slowest
static long long workMaxPrev = 0;		// of this second, of the one before
static long long workSince = -1;
static std::atomic<long long> phVbl{-1};
static std::atomic<int> phHist[PH_BINS];	// for the log: when frames reach the window

void dsync_frame_presented() {
	if (!ftPending.exchange(false)) return;
	long long now = paceClockNs();
	double hz = sysRate;
	if (hz <= 0) return;
	long long v = phVbl;
	if (v >= 0) {
		int b = (int)((now - v) * hz / 1e9 * PH_BINS);
		phHist[std::max(0, std::min(b, PH_BINS - 1))].fetch_add(1, std::memory_order_relaxed);
	}
	// unlocked, the tick makes no frame: nothing to hold for
	long long c = cbAt;
	if (!pacing_locked() || (c < 0)) {
		holdNs = 0;
		workMax = workMaxPrev = 0;
		workSince = -1;
		return;
	}
	workMax = std::max(workMax, now - c);
	if (workSince < 0) workSince = now;
	if (now - workSince < HOLD_WIN_NS) return;
	// a frame that took past the next tick reads short here, so the slowest of
	// two seconds stands for them
	long long slow = std::max(workMax, workMaxPrev);
	workMaxPrev = workMax;
	workMax = 0;
	workSince = now;
	long long at = (long long)(HOLD_AT * 1e9 / hz);
	holdNs = std::max(0LL, at - slow);
}

static void ft_vblank(long long ns) {
	if (!xlog_on(XLG_VIDEO, XLL_DEBUG)) return;
	int k = ftDrawn.exchange(0);
	if (ftSince < 0) {
		ftSince = ns;
		ftRun = 0;
		ftLost = ftVbl = 0;
		memset(ftHist, 0, sizeof(ftHist));
	}
	ftVbl++;
	if (k > 0) {
		if (ftRun > 0) ftHist[std::min(ftRun, FT_RUNS) - 1]++;
		ftLost += k - 1;
		ftRun = 1;
	} else if (ftRun > 0) {
		ftRun++;
	}
	if (ns - ftSince >= FT_LOG_NS) {
		char buf[160];
		int pos = 0;
		for (int i = 0; i < FT_RUNS; i++)
			if (ftHist[i]) pos += snprintf(buf + pos, sizeof(buf) - pos, " %s%i:%i", (i == FT_RUNS - 1) ? ">=" : "", i + 1, ftHist[i]);
		xlog(XLG_VIDEO, XLL_DEBUG, "vsync: frames by vblanks shown over %i vblanks:%s, unseen %i",
			ftVbl, pos ? buf : " none", ftLost);
		pos = 0;
		for (int i = 0; i < PH_BINS; i++)
			pos += snprintf(buf + pos, sizeof(buf) - pos, " %i", phHist[i].exchange(0));
		xlog(XLG_VIDEO, XLL_DEBUG, "vsync: frames into the window by tenths of a period after the vblank:%s; the tick held %.2f ms",
			buf, holdNs / 1e6);
		ftSince = -1;
	}
}

static bool ds_vblank(long long ns) {
	if (!meas_vblank(ns)) return false;
	phVbl = ns;
	ft_vblank(ns);
	return true;
}

static void ds_call() {
	dsyncTick cb = tickCb;
	long long ns = paceClockNs();
	cbAt = ns;
	if (cb) cb(ns);
}

#if !defined(__WIN32)
static void ds_tick(long long ns) {
	if (ds_vblank(ns)) ds_call();
}
#endif

double dsync_rate() {
	return sysRate;
}

double dsync_measured() {
	std::lock_guard<std::mutex> lock(measGuard);
	return measHz;
}

double dsync_lock_rate() {
	double m = dsync_measured();
	double s = sysRate;
	if (m <= 0) return s;
	if (s <= 0) return m;
	return (std::fabs(m / s - 1.0) * 1e6 > DS_TRUST_PPM) ? m : s;
}

bool dsync_adaptive() {
	return adaptive;
}

// == Windows ==
#if defined(__WIN32)

static std::atomic<HMONITOR> monitor{nullptr};
static std::thread tickThread;
static std::atomic<bool> tickRun{false};

// The mode's own refresh, as a ratio: QueryDisplayConfig gives the signal the
// monitor gets, 60000/1001 and not 60. The path is matched to the window's
// monitor by the GDI name both of them carry.
static double win_rate(HMONITOR mon) {
	MONITORINFOEXW mi;
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(mon, &mi)) return 0;
	UINT32 np = 0, nm = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return 0;
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return 0;
	for (UINT32 i = 0; i < np; i++) {
		const DISPLAYCONFIG_PATH_INFO& p = paths[i];
		DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
		src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		src.header.size = sizeof(src);
		src.header.adapterId = p.sourceInfo.adapterId;
		src.header.id = p.sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
		if (wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;
		DISPLAYCONFIG_RATIONAL r = p.targetInfo.refreshRate;
		UINT32 mi2 = p.targetInfo.modeInfoIdx;
		if ((mi2 != DISPLAYCONFIG_PATH_MODE_IDX_INVALID) && (mi2 < nm)
				&& (modes[mi2].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)) {
			DISPLAYCONFIG_RATIONAL v = modes[mi2].targetMode.targetVideoSignalInfo.vSyncFreq;
			if (v.Numerator && v.Denominator) r = v;
		}
		if (r.Numerator && r.Denominator) return (double)r.Numerator / r.Denominator;
	}
	return 0;
}

// the DXGI output showing that monitor, on whichever adapter drives it
static IDXGIOutput* win_output(IDXGIFactory1* fac, HMONITOR mon) {
	IDXGIAdapter1* ad = nullptr;
	for (UINT a = 0; fac->EnumAdapters1(a, &ad) != DXGI_ERROR_NOT_FOUND; a++) {
		IDXGIOutput* out = nullptr;
		for (UINT o = 0; ad->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; o++) {
			DXGI_OUTPUT_DESC d;
			if (SUCCEEDED(out->GetDesc(&d)) && (d.Monitor == mon)) {
				ad->Release();
				return out;
			}
			out->Release();
		}
		ad->Release();
	}
	return nullptr;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// the tick's hold, to well under a millisecond where Windows has the timer for it
static void win_hold(HANDLE tmr, long long until) {
	long long left = until - paceClockNs();
	if (left <= 0) return;
	LARGE_INTEGER due;
	due.QuadPart = -(left / 100);		// relative, in 100 ns
	if (tmr && SetWaitableTimer(tmr, &due, 0, nullptr, nullptr, FALSE))
		WaitForSingleObject(tmr, (DWORD)(left / 1000000) + 50);
	else
		Sleep((DWORD)(left / 1000000));
}

// WaitForVBlank needs no device and no swap chain: it is the output's own
// vblank, whatever the compositor does with the window.
static void win_tick_loop() {
	IDXGIFactory1* fac = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) {
		xlog(XLG_VIDEO, XLL_WARN, "vsync: no DXGI factory");
		tickRun = false;
		return;
	}
	HANDLE tmr = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (!tmr) tmr = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
	HMONITOR have = nullptr;
	IDXGIOutput* out = nullptr;
	int quick = 0;
	long long last = -1;
	bool failTold = false;
	xlog(XLG_VIDEO, XLL_INFO, "vsync: vblank tick on");
	while (tickRun) {
		HMONITOR want = monitor;
		if (want != have) {
			if (out) out->Release();
			out = want ? win_output(fac, want) : nullptr;
			have = want;
			if (!out) xlog(XLG_VIDEO, XLL_WARN, "vsync: no DXGI output for the window's monitor");
		}
		if (!out || FAILED(out->WaitForVBlank())) {
			if (out && !failTold) xlog(XLG_VIDEO, XLL_WARN, "vsync: the output gives no vblank");
			failTold = true;
			Sleep(10);	// a monitor switched off or gone: no tick until it is back
			last = -1;
			continue;
		}
		long long ns = paceClockNs();
		// a display asleep can answer at once: do not spin on it
		quick = ((last >= 0) && (ns - last < 2000000)) ? quick + 1 : 0;
		last = ns;
		if (quick > 4) {
			Sleep(1);
			continue;
		}
		if (!ds_vblank(ns)) continue;
		long long h = holdNs;
		if (h > 0) win_hold(tmr, ns + h);
		ds_call();
	}
	if (tmr) CloseHandle(tmr);
	if (out) out->Release();
	fac->Release();
}

void dsync_probe(QWindow* win) {
	HMONITOR mon = win ? MonitorFromWindow((HWND)win->winId(), MONITOR_DEFAULTTONEAREST) : nullptr;
	double r = mon ? win_rate(mon) : 0;
	QString name = (win && win->screen()) ? win->screen()->name() : QString();
	bool moved = !probed || (mon != monitor.load()) || (r != sysRate);
	sysRate = r;
	monitor = mon;
	screenName = name;
	if (moved) {
		probed = true;
		meas_reset();
		xlog(XLG_VIDEO, XLL_INFO, "vsync: screen '%s' at %.4f Hz (system)", name.toUtf8().constData(), r);
	}
}

bool dsync_start(dsyncTick cb) {
	tickCb = cb;
	if (tickRun) return true;
	meas_reset();
	tickRun = true;
	tickThread = std::thread(win_tick_loop);
	return true;
}

void dsync_stop() {
	tickCb = nullptr;
	if (!tickRun && !tickThread.joinable()) return;
	tickRun = false;
	if (tickThread.joinable()) tickThread.join();
}

bool dsync_running() {
	return tickRun;
}

void dsync_swapped() {}

// == macOS: the CoreVideo display link, in dispsync_mac.mm ==
#elif defined(__APPLE__)

bool dsmac_probe(void* view, double* hz, bool* vrr, unsigned* display);
bool dsmac_start(unsigned display, void (*cb)(long long));
void dsmac_display(unsigned display);
void dsmac_stop();

static unsigned displayId = 0;
static std::atomic<bool> tickRun{false};

static void mac_tick(long long) {
	ds_tick(paceClockNs());
}

void dsync_probe(QWindow* win) {
	double r = 0;
	bool vrr = false;
	unsigned id = 0;
	if (win) dsmac_probe((void*)win->winId(), &r, &vrr, &id);
	QString name = (win && win->screen()) ? win->screen()->name() : QString();
	bool moved = !probed || (id != displayId) || (r != sysRate);
	sysRate = r;
	adaptive = vrr;
	if (id != displayId) {
		displayId = id;
		if (tickRun) dsmac_display(id);
	}
	screenName = name;
	if (moved) {
		probed = true;
		meas_reset();
		xlog(XLG_VIDEO, XLL_INFO, "vsync: screen '%s' at %.4f Hz (system)%s", name.toUtf8().constData(), r,
			vrr ? ", adaptive" : "");
	}
}

bool dsync_start(dsyncTick cb) {
	tickCb = cb;
	if (tickRun) return true;
	meas_reset();
	tickRun = dsmac_start(displayId, mac_tick);
	return tickRun;
}

void dsync_stop() {
	tickCb = nullptr;
	if (!tickRun) return;
	dsmac_stop();
	tickRun = false;
}

bool dsync_running() {
	return tickRun;
}

void dsync_swapped() {}

// == Linux and the rest: Qt's rate, the GL view's swaps as the tick ==
#else

static std::atomic<bool> tickRun{false};

void dsync_probe(QWindow* win) {
	QScreen* scr = win ? win->screen() : QGuiApplication::primaryScreen();
	double r = scr ? scr->refreshRate() : 0;
	QString name = scr ? scr->name() : QString();
	bool moved = !probed || (name != screenName) || (r != sysRate);
	sysRate = r;
	screenName = name;
	if (moved) {
		probed = true;
		meas_reset();
		xlog(XLG_VIDEO, XLL_INFO, "vsync: screen '%s' at %.4f Hz (system)", name.toUtf8().constData(), r);
	}
}

bool dsync_start(dsyncTick cb) {
	if (!swapWait) return false;	// swaps that do not wait are no tick
	tickCb = cb;
	if (!tickRun) meas_reset();
	tickRun = true;
	return true;
}

void dsync_stop() {
	tickCb = nullptr;
	tickRun = false;
}

bool dsync_running() {
	return tickRun;
}

void dsync_swapped() {
	if (tickRun) ds_tick(paceClockNs());
}

#endif

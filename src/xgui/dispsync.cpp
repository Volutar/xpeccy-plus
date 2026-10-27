#include "dispsync.h"

#include <algorithm>
#include <atomic>
#include <cmath>
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

static double sysRate = 0;		// what the system says, Hz
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

static dsyncTick tickCb = nullptr;

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
	double est = (measHz > 0) ? measHz : sysRate;
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
		xlog(XLG_VIDEO, XLL_INFO, "vsync: measured %.4f Hz over the vblanks, system says %.4f", tell, sysRate);
	return true;
}

static void ds_tick(long long ns) {
	if (!meas_vblank(ns)) return;
	dsyncTick cb = tickCb;
	if (cb) cb(ns);
}

double dsync_rate() {
	return sysRate;
}

double dsync_measured() {
	std::lock_guard<std::mutex> lock(measGuard);
	return measHz;
}

double dsync_lock_rate() {
	double m = dsync_measured();
	if (m <= 0) return sysRate;
	if (sysRate <= 0) return m;
	return (std::fabs(m / sysRate - 1.0) * 1e6 > DS_TRUST_PPM) ? m : sysRate;
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

// WaitForVBlank needs no device and no swap chain: it is the output's own
// vblank, whatever the compositor does with the window.
static void win_tick_loop() {
	IDXGIFactory1* fac = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) {
		xlog(XLG_VIDEO, XLL_WARN, "vsync: no DXGI factory");
		tickRun = false;
		return;
	}
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
		ds_tick(ns);
	}
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

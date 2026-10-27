// Cocoa side of the display sync (dispsync.cpp): the display a view is on, its
// nominal refresh, whether it adapts its rate, and a CoreVideo display link
// ticking on its vblanks.

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>

#include <atomic>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"	// CVDisplayLink: macOS 15 says use NSView's

static CVDisplayLinkRef link = NULL;
static std::atomic<void (*)(long long)> linkCb{nullptr};

bool dsmac_probe(void* v, double* hz, bool* vrr, unsigned* display) {
	*hz = 0;
	*vrr = false;
	*display = 0;
	NSView* view = (NSView*)v;
	NSScreen* scr = (view && [view window]) ? [[view window] screen] : [NSScreen mainScreen];
	if (scr == nil) return false;
	NSNumber* num = [[scr deviceDescription] objectForKey:@"NSScreenNumber"];
	CGDirectDisplayID id = num ? [num unsignedIntValue] : CGMainDisplayID();
	*display = id;
	CVDisplayLinkRef dl = NULL;
	if (CVDisplayLinkCreateWithCGDisplay(id, &dl) == kCVReturnSuccess) {
		CVTime t = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(dl);
		if (!(t.flags & kCVTimeIsIndefinite) && (t.timeValue > 0))
			*hz = (double)t.timeScale / (double)t.timeValue;
		CVDisplayLinkRelease(dl);
	}
	if (@available(macOS 12.0, *)) {
		// ProMotion and adaptive sync: the shortest and the longest frame differ
		*vrr = ([scr minimumRefreshInterval] != [scr maximumRefreshInterval]);
	}
	return *hz > 0;
}

static CVReturn link_cb(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*, void*) {
	void (*cb)(long long) = linkCb;
	if (cb) cb(0);	// the caller stamps it on its own clock
	return kCVReturnSuccess;
}

bool dsmac_start(unsigned display, void (*cb)(long long)) {
	linkCb = cb;
	if (link) return true;
	if (display) {
		if (CVDisplayLinkCreateWithCGDisplay(display, &link) != kCVReturnSuccess) link = NULL;
	} else if (CVDisplayLinkCreateWithActiveCGDisplays(&link) != kCVReturnSuccess) {
		link = NULL;
	}
	if (!link) return false;
	CVDisplayLinkSetOutputCallback(link, link_cb, NULL);
	if (CVDisplayLinkStart(link) != kCVReturnSuccess) {
		CVDisplayLinkRelease(link);
		link = NULL;
		return false;
	}
	return true;
}

void dsmac_display(unsigned display) {
	if (link && display) CVDisplayLinkSetCurrentCGDisplay(link, display);
}

void dsmac_stop() {
	linkCb = nullptr;
	if (!link) return;
	CVDisplayLinkStop(link);
	CVDisplayLinkRelease(link);
	link = NULL;
}

#pragma clang diagnostic pop

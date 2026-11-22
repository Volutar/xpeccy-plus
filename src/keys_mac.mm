// Cocoa side of the host keyboard: Shift, Ctrl and Option told left from right.
//
// Qt finds a modifier's change by the bit the two keys share (qnsview_keys.mm,
// flagsChanged:), so its event has no side, and with one Shift held the other's
// press and release send nothing at all. Every flags change carries the
// device's own bits too, one per key; a local monitor reads them and hands the
// six keys' state on (mac_mods_changed in emulwin.cpp). The event goes on to Qt
// as it was: its modifiers stay right for the hotkeys.

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include "keys_mac.h"

// IOKit's IOLLEvent.h, not pulled in by AppKit
#ifndef NX_DEVICELCTLKEYMASK
#define NX_DEVICELCTLKEYMASK	0x00000001
#endif
#ifndef NX_DEVICELSHIFTKEYMASK
#define NX_DEVICELSHIFTKEYMASK	0x00000002
#endif
#ifndef NX_DEVICERSHIFTKEYMASK
#define NX_DEVICERSHIFTKEYMASK	0x00000004
#endif
#ifndef NX_DEVICELALTKEYMASK
#define NX_DEVICELALTKEYMASK	0x00000020
#endif
#ifndef NX_DEVICERALTKEYMASK
#define NX_DEVICERALTKEYMASK	0x00000040
#endif
#ifndef NX_DEVICERCTLKEYMASK
#define NX_DEVICERCTLKEYMASK	0x00002000
#endif

static const struct {
	unsigned long dev;
	unsigned bit;
} modTab[] = {
	{NX_DEVICELSHIFTKEYMASK, MACMOD_LSHIFT}, {NX_DEVICERSHIFTKEYMASK, MACMOD_RSHIFT},
	{NX_DEVICELCTLKEYMASK, MACMOD_LCTRL}, {NX_DEVICERCTLKEYMASK, MACMOD_RCTRL},
	{NX_DEVICELALTKEYMASK, MACMOD_LALT}, {NX_DEVICERALTKEYMASK, MACMOD_RALT}
};

static unsigned dev_held(unsigned long flags) {
	unsigned held = 0;
	for (const auto& m : modTab)
		if (flags & m.dev) held |= m.bit;
	return held;
}

static void (*modsCb)(unsigned, unsigned long) = NULL;
static id modsMonitor = nil;

void mac_mods_watch(void (*cb)(unsigned, unsigned long)) {
	modsCb = cb;
	if (modsMonitor != nil) return;
	modsMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskFlagsChanged handler:^NSEvent*(NSEvent* ev) {
		unsigned long flags = (unsigned long)[ev modifierFlags];
		if (modsCb) modsCb(dev_held(flags), flags);
		return ev;
	}];
#if !__has_feature(objc_arc)
	[modsMonitor retain];		// kept for the program's life
#endif
}

// The keys as the system holds them now. A menu's or a window drag's own event
// loop takes the flags changes the monitor would see, so what it last said may
// be old. A pair whose shared bit is off is up; one whose side the system does
// not say (no device bits here) stays as it was, so a guess never presses one.
unsigned mac_mods_live(unsigned last, unsigned long* flagsOut) {
	static const struct {
		unsigned long any;
		unsigned pair;
	} pairTab[] = {
		{kCGEventFlagMaskShift, MACMOD_LSHIFT | MACMOD_RSHIFT},
		{kCGEventFlagMaskControl, MACMOD_LCTRL | MACMOD_RCTRL},
		{kCGEventFlagMaskAlternate, MACMOD_LALT | MACMOD_RALT}
	};
	unsigned long flags = (unsigned long)CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
	unsigned dev = dev_held(flags);
	unsigned held = 0;
	for (const auto& p : pairTab) {
		if (!(flags & p.any)) continue;
		held |= (dev & p.pair) ? (dev & p.pair) : (last & p.pair);
	}
	if (flagsOut) *flagsOut = flags;
	return held;
}

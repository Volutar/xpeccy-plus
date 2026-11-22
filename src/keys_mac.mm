// Cocoa side of the host keyboard: Shift, Ctrl and Option told left from right.
//
// Qt finds a modifier's change by the bit the two keys share (qnsview_keys.mm,
// flagsChanged:), so its event has no side, and with one Shift held the other's
// press and release send nothing at all. Every flags change carries the
// device's own bits too, one per key; a local monitor reads them and hands the
// six keys' state on (mac_mods_changed in emulwin.cpp). The event goes on to Qt
// as it was: its modifiers stay right for the hotkeys.

#import <AppKit/AppKit.h>

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

static void (*modsCb)(unsigned, unsigned long) = NULL;
static id modsMonitor = nil;

void mac_mods_watch(void (*cb)(unsigned, unsigned long)) {
	modsCb = cb;
	if (modsMonitor != nil) return;
	modsMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskFlagsChanged handler:^NSEvent*(NSEvent* ev) {
		unsigned long flags = (unsigned long)[ev modifierFlags];
		unsigned held = 0;
		for (const auto& m : modTab)
			if (flags & m.dev) held |= m.bit;
		if (modsCb) modsCb(held, flags);
		return ev;
	}];
#if !__has_feature(objc_arc)
	[modsMonitor retain];		// kept for the program's life
#endif
}

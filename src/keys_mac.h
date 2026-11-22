#pragma once

// macOS: Shift, Ctrl and Option told left from right (keys_mac.mm)

#include <QKeyEvent>

#define MACMOD_LSHIFT	(1 << 0)
#define MACMOD_RSHIFT	(1 << 1)
#define MACMOD_LCTRL	(1 << 2)	// the physical Ctrl, which Qt calls Meta
#define MACMOD_RCTRL	(1 << 3)
#define MACMOD_LALT	(1 << 4)	// Option
#define MACMOD_RALT	(1 << 5)

// the nativeScanCode of a modifier key event of our own: nativeVirtualKey is its XKEY_*
#define MACMOD_EVENT	0x584d4f44

// cb gets the MACMOD_* keys held and the raw flags, on every change of them
void mac_mods_watch(void (*cb)(unsigned, unsigned long));
// the MACMOD_* keys held as the system says now; last: for a pair it gives no side of
unsigned mac_mods_live(unsigned last, unsigned long* flagsOut);
// emulwin.cpp: the machine given the modifiers held (key: ahead of a key press), and let go of them
void mac_mods_sync(bool key);
void mac_mods_let_go();

// Qt's own event of a Shift, Ctrl or Option: no side, and not every one comes;
// the machine and the pads take these keys from our own events instead
inline bool mac_mod_key(const QKeyEvent* ev) {
	if (ev->nativeScanCode() == MACMOD_EVENT) return false;
	int key = ev->key();
	return (key == Qt::Key_Shift) || (key == Qt::Key_Meta) || (key == Qt::Key_Alt);
}
